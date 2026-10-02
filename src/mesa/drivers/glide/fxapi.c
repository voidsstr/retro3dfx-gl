/*
 * Mesa 3-D graphics library
 * Version:  4.0
 *
 * Copyright (C) 1999-2001  Brian Paul   All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * BRIAN PAUL BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
 * AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/* Authors:
 *    David Bucciarelli
 *    Brian Paul
 *    Daryll Strauss
 *    Keith Whitwell
 *    Daniel Borca
 *    Hiroshi Morii
 */


/* fxapi.c - public interface to FX/Mesa functions (fxmesa.h) */


#ifdef HAVE_CONFIG_H
#include "conf.h"
#endif

#if defined(FX)
#include <math.h>
#include "fxdrv.h"
#include "fxrlog.h"   /* [retro3dfx] C:\retrogl.log context-creation tracer */

#include "drivers/common/driverfuncs.h"

#ifndef TDFX_DEBUG
int TDFX_DEBUG = (0
/*		  | VERBOSE_VARRAY */
/*		  | VERBOSE_TEXTURE */
/*		  | VERBOSE_IMMEDIATE */
/*		  | VERBOSE_PIPELINE */
/*		  | VERBOSE_DRIVER */
/*		  | VERBOSE_STATE */
/*		  | VERBOSE_API */
/*		  | VERBOSE_DISPLAY_LIST */
/*		  | VERBOSE_LIGHTING */
/*		  | VERBOSE_PRIMS */
/*		  | VERBOSE_VERTS */
   );
#endif

static fxMesaContext fxMesaCurrentCtx = NULL;

/* [retro3dfx] windowed-render request: wglCreateContext's windowed branch calls
 * fxMesaRequestWindowed(w,h) just before fxMesaCreateContext; the create path
 * consumes and clears it, and falls back to fullscreen grSstWinOpen if the
 * windowed surface path is unavailable. */
static int fxWinReq = 0, fxWinReqW = 0, fxWinReqH = 0;
void fxMesaRequestWindowed(int w, int h) { fxWinReq = 1; fxWinReqW = w; fxWinReqH = h; }

/*
 * Status of 3Dfx hardware initialization
 */

static int glbGlideInitialized = 0;
static int glb3DfxPresent = 0;
static int glbTotNumCtx = 0;

static GrHwConfiguration glbHWConfig;
static int glbCurrentBoard = 0;


/* [retro3dfx 0.1.62] Set by the process-exit handler. fxCloseHardware keeps
 * Glide initialised across a context destroy (vid_restart safety, 0.1.31), but
 * at PROCESS EXIT that left grGlideShutdown uncalled, so the board was never
 * released through Glide - the process teardown was left to reclaim it.
 * Found while chasing an intermittent dead board mapping in AmigaMerlin's
 * grGlideInit on the Voodoo 5 6000 (.124, 2026-09-24). An interleaved A/B on a
 * fresh boot (10 launches each) did NOT reproduce that fault with either
 * build, so this is hygiene - release what we acquired - not a proven cure. */
static int glbProcessExiting = 0;

#if defined(__WIN32__)
static int
cleangraphics(void)
{
   glbProcessExiting = 1;
   fxProfStop();   /* [retro3dfx] 0.1.67: writes RETROGL_PROF, if armed */
   if (fxMesaCurrentCtx) {
      glbTotNumCtx = 1;
      fxMesaDestroyContext(fxMesaCurrentCtx);   /* -> fxCloseHardware */
   }
   /* The game may already have deleted its context (wglDeleteContext before
    * exit), in which case nothing above reached fxCloseHardware. */
   if (glbGlideInitialized) {
      rgl_log("cleangraphics: process exit -> grGlideShutdown()");
      grGlideShutdown();
      rgl_log("cleangraphics: grGlideShutdown returned");
      glbGlideInitialized = 0;
   }
   return 0;
}
#elif defined(__linux__)
static void
cleangraphics(void)
{
   glbTotNumCtx = 1;
   fxMesaDestroyContext(fxMesaCurrentCtx);
}

static void
cleangraphics_handler(int s)
{
   fprintf(stderr, "fxmesa: ERROR: received a not handled signal %d\n", s);

   cleangraphics();
/*    abort(); */
   exit(1);
}
#endif


/* [retro3dfx] 0.1.67: Glide's default error callback reports a FATAL error
 * with MessageBox(NULL, ...) + exit(1). Behind a game's fullscreen window
 * nobody can see or dismiss that box, and the game waits forever - the
 * intermittent Quake III "hang in grGlideInit" on the V5 6000 (.124,
 * 2026-09-25) was exactly that: ntsd put the thread in USER32!MessageBoxA
 * called from glide3x!_grErrorDefaultCallback, the text in minihwc's
 * errorString. Log it and RETURN instead: Glide then skips the board,
 * FX_grSstQueryHardware finds none, and the context fails cleanly - a
 * failure the game can report and a runner can retry. Our h5 Glide keeps a
 * callback installed before grGlideInit (fork, gpci.c); a Glide that resets
 * it (AmigaMerlin's) simply behaves as before. */
static int glbGlideErrors = 0;
static int glbGlideFatal = 0;

static void   /* cdecl: GrErrorCallbackFnc_t has no FX_CALL */
fxGlideErrorCallback(const char *string, FxBool fatal)
{
   if (fatal)
      glbGlideFatal++;
   if (glbGlideErrors++ < 32)
      rgl_log("GLIDE %s ERROR: %s", fatal ? "FATAL" : "non-fatal",
              string ? string : "(null)");
}

/*
 * Query 3Dfx hardware presence/kind
 */
static GLboolean GLAPIENTRY fxQueryHardware (void)
{
 if (TDFX_DEBUG & VERBOSE_DRIVER) {
    fprintf(stderr, "fxQueryHardware()\n");
 }

 if (!glbGlideInitialized) {
#if defined(__WIN32__)
    /* [retro3dfx] performance defaults: don't stall in grBufferSwap waiting
     * on vsync, and allow a 2-deep swap queue. Measured on Voodoo3/P3-845
     * (Q3 1.32 timedemo four, 16bpp): 640x480 +7%, 1024x768 +32% vs Glide's
     * wait-on-vidsync defaults. Set through BOTH the CRT env (_putenv - seen
     * by a glide3x sharing msvcrt) and the Win32 env (seen by
     * GetEnvironmentVariable readers), only when the user hasn't set them -
     * an explicit user environment always wins. */
    {
       static const char *fx_perf_defaults[][2] = {
          { "FX_GLIDE_SWAPINTERVAL",       "0" },
          { "SST_SWAP_EN_WAIT_ON_VIDSYNC", "0" },
          { "FX_GLIDE_SWAPPENDINGCOUNT",   "2" },
       };
       int i;
       for (i = 0; i < 3; i++) {
          if (!getenv(fx_perf_defaults[i][0])) {
             char buf[64];
             _snprintf(buf, sizeof(buf), "%s=%s",
                       fx_perf_defaults[i][0], fx_perf_defaults[i][1]);
             buf[sizeof(buf) - 1] = '\0';
             _putenv(buf);
             SetEnvironmentVariableA(fx_perf_defaults[i][0],
                                     fx_perf_defaults[i][1]);
          }
       }
    }
#endif
    rgl_log("fxQueryHardware: FIRST init -> calling grGlideInit() ...");
    grErrorSetCallback(fxGlideErrorCallback);
    grGlideInit();
    rgl_log("fxQueryHardware: grGlideInit() RETURNED (%d fatal Glide error(s)); "
            "calling FX_grSstQueryHardware() ...", glbGlideFatal);
    glb3DfxPresent = FX_grSstQueryHardware(&glbHWConfig);
    rgl_log("fxQueryHardware: FX_grSstQueryHardware -> present=%d num_sst=%d type0=%d",
            (int)glb3DfxPresent, (int)glbHWConfig.num_sst,
            glbHWConfig.num_sst > 0 ? (int)glbHWConfig.SSTs[0].type : -1);

    glbGlideInitialized = 1;

#if defined(__WIN32__)
    _onexit((_onexit_t) cleangraphics);
#elif defined(__linux__)
    /* Only register handler if environment variable is not defined. */
    if (!getenv("MESA_FX_NO_SIGNALS")) {
       atexit(cleangraphics);
    }
#endif
 }

 return glb3DfxPresent;
}


/*
 * Select the Voodoo board to use when creating
 * a new context.
 */
GLint GLAPIENTRY fxMesaSelectCurrentBoard (int n)
{
   fxQueryHardware();

   if ((n < 0) || (n >= glbHWConfig.num_sst))
      return -1;

   return glbHWConfig.SSTs[glbCurrentBoard = n].type;
}


fxMesaContext GLAPIENTRY fxMesaGetCurrentContext (void)
{
 return fxMesaCurrentCtx;
}


void GLAPIENTRY fxGetScreenGeometry (GLint *w, GLint *h)
{
 GLint width = 0;
 GLint height = 0;
 
 if (fxMesaCurrentCtx != NULL) {
    width = fxMesaCurrentCtx->screen_width;
    height = fxMesaCurrentCtx->screen_height;
 }

 if (w != NULL) {
    *w = width;
 }
 if (h != NULL) {
    *h = height;
 }
}


/*
 * The 3Dfx Global Palette extension for GLQuake.
 * More a trick than a real extesion, use the shared global
 * palette extension. 
 */
extern void GLAPIENTRY gl3DfxSetPaletteEXT(GLuint * pal);	/* silence warning */
void GLAPIENTRY
gl3DfxSetPaletteEXT(GLuint * pal)
{
   fxMesaContext fxMesa = fxMesaCurrentCtx;

   if (TDFX_DEBUG & VERBOSE_DRIVER) {
      int i;

      fprintf(stderr, "gl3DfxSetPaletteEXT(...)\n");

      for (i = 0; i < 256; i++) {
	 fprintf(stderr, "\t%x\n", pal[i]);
      }
   }

   if (fxMesa) {
      fxMesa->haveGlobalPaletteTexture = 1;

      grTexDownloadTable(GR_TEXTABLE_PALETTE, (GuTexPalette *) pal);
   }
}


/* [retro3dfx] glide resolution enum -> pixel dims. Moved to file scope (was a
 * function-local static in fxBestResolution) so the C:\retrogl.log tracer can
 * map a GR_RESOLUTION_* enum back to its WxH at the grSstWinOpen call site.
 * The table is indexed by the enum value itself (GR_RESOLUTION_320x200==0,
 * ..._640x480==7, ..._1024x768==0xC, etc.), so row index == enum. Behavior of
 * fxBestResolution is unchanged. */
static const int fxResolutions[][3] = {
        { GR_RESOLUTION_320x200,    320,  200 },
        { GR_RESOLUTION_320x240,    320,  240 },
        { GR_RESOLUTION_400x256,    400,  256 },
        { GR_RESOLUTION_512x384,    512,  384 },
        { GR_RESOLUTION_640x200,    640,  200 },
        { GR_RESOLUTION_640x350,    640,  350 },
        { GR_RESOLUTION_640x400,    640,  400 },
        { GR_RESOLUTION_640x480,    640,  480 },
        { GR_RESOLUTION_800x600,    800,  600 },
        { GR_RESOLUTION_960x720,    960,  720 },
        { GR_RESOLUTION_856x480,    856,  480 },
        { GR_RESOLUTION_512x256,    512,  256 },
        { GR_RESOLUTION_1024x768,  1024,  768 },
        { GR_RESOLUTION_1280x1024, 1280, 1024 },
        { GR_RESOLUTION_1600x1200, 1600, 1200 },
        { GR_RESOLUTION_400x300,    400,  300 },
        { GR_RESOLUTION_1152x864,  1152,  864 },
        { GR_RESOLUTION_1280x960,  1280,  960 },
        { GR_RESOLUTION_1600x1024, 1600, 1024 },
        { GR_RESOLUTION_1792x1344, 1792, 1344 },
        { GR_RESOLUTION_1856x1392, 1856, 1392 },
        { GR_RESOLUTION_1920x1440, 1920, 1440 },
        { GR_RESOLUTION_2048x1536, 2048, 1536 },
        { GR_RESOLUTION_2048x2048, 2048, 2048 }
};

/* [retro3dfx] map a GR_RESOLUTION_* enum to its pixel WxH for the trace log. */
static void rgl_res_dims(int res, int *w, int *h)
{
   if (res >= 0 && res < (int)(sizeof(fxResolutions) / sizeof(fxResolutions[0]))) {
      *w = fxResolutions[res][1];
      *h = fxResolutions[res][2];
   } else {
      *w = -1;
      *h = -1;
   }
}

static GrScreenResolution_t fxBestResolution (int width, int height)
{
 int i, size;
 int lastvalidres = GR_RESOLUTION_640x480;
 int min = 2048 * 2048; /* max is GR_RESOLUTION_2048x2048 */
 GrResolution resTemplate = {
              GR_QUERY_ANY,
              GR_QUERY_ANY,
              2 /*GR_QUERY_ANY */,
              GR_QUERY_ANY
 };
 GrResolution *presSupported;

 if (!fxQueryHardware()) {
     rgl_log("fxBestResolution: HW query FAILED for %dx%d -> default GR_RESOLUTION_640x480", width, height);
     return lastvalidres;
 }

 size = grQueryResolutions(&resTemplate, NULL);
 presSupported = malloc(size);

 size /= sizeof(GrResolution);
 grQueryResolutions(&resTemplate, presSupported);

 for (i = 0; i < size; i++) {
     int r = presSupported[i].resolution;
     if ((width <= fxResolutions[r][1]) && (height <= fxResolutions[r][2])) {
        if (min > (fxResolutions[r][1] * fxResolutions[r][2])) {
           min = fxResolutions[r][1] * fxResolutions[r][2];
           lastvalidres = r;
        }
     }
 }

 free(presSupported);

 rgl_log("fxBestResolution: request %dx%d -> res enum %d (%dx%d), %d supported modes queried",
         width, height, lastvalidres, fxResolutions[lastvalidres][1],
         fxResolutions[lastvalidres][2], size);
 return fxResolutions[lastvalidres][0];
}


/* [retro3dfx 0.1.64] Fullscreen refresh. Re-implements the 0.1.34 fix, which
 * was lost from every source (README §15.4) while its test survived
 * (tests/native/test_fx_best_refresh.c, which this mirrors exactly).
 * Glide programs the video timing itself, so GDI/-freq cannot override it; the
 * old code passed GR_REFRESH_60Hz to every fullscreen game - visible flicker on
 * a CRT. Choose: env override (FX_GLIDE_REFRESH_RATE / SSTV2_REFRESH_RATE /
 * MESA_FX_REFRESH, in Hz) else the highest rate the display driver enumerates
 * for WxH, snapped DOWN to a GR_REFRESH_* timing Glide has; below 60 or no
 * answer -> 60. */
static GrScreenRefresh_t
fxSnapRefresh(int hz)
{
   static const struct { int hz; GrScreenRefresh_t ref; } tbl[] = {
      {120, GR_REFRESH_120Hz}, {100, GR_REFRESH_100Hz}, {90, GR_REFRESH_90Hz},
      {85, GR_REFRESH_85Hz},   {80, GR_REFRESH_80Hz},   {75, GR_REFRESH_75Hz},
      {72, GR_REFRESH_72Hz},   {70, GR_REFRESH_70Hz},   {60, GR_REFRESH_60Hz},
   };
   unsigned i;
   for (i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++)
      if (tbl[i].hz <= hz)
         return tbl[i].ref;
   return GR_REFRESH_60Hz;
}

static GrScreenRefresh_t
fxBestRefresh(int width, int height)
{
   int hz = 0;
   const char *src = "none";
   const char *env = getenv("FX_GLIDE_REFRESH_RATE");
   if (!env) env = getenv("SSTV2_REFRESH_RATE");
   if (!env) env = getenv("MESA_FX_REFRESH");
   if (env) {
      hz = atoi(env);
      src = "env";
   }
#if defined(__WIN32__)
   else {
      DEVMODEA dm;
      DWORD i;
      memset(&dm, 0, sizeof(dm));
      dm.dmSize = sizeof(dm);
      for (i = 0; EnumDisplaySettingsA(NULL, i, &dm); i++) {
         /* 0 and 1 Hz are the driver's "default" sentinels, never a rate */
         if ((int)dm.dmPelsWidth == width && (int)dm.dmPelsHeight == height &&
             dm.dmDisplayFrequency > 1 && (int)dm.dmDisplayFrequency > hz)
            hz = (int)dm.dmDisplayFrequency;
      }
      src = "EnumDisplaySettings";
   }
#endif
   rgl_log("fxBestRefresh: %dx%d max %d Hz (%s) -> GR_REFRESH enum %d",
           width, height, hz, src, (int)fxSnapRefresh(hz));
   return fxSnapRefresh(hz);
}


fxMesaContext GLAPIENTRY
fxMesaCreateBestContext(GLuint win, GLint width, GLint height,
			const GLint attribList[])
{
 /* fxMesaCreateContext() handles fxQueryHardware() error returns */
 int res = fxBestResolution(width, height);
 GrScreenRefresh_t ref = fxBestRefresh(width, height);
 rgl_log("fxMesaCreateBestContext: win=%lu %dx%d -> res enum %d, ref enum %d",
         (unsigned long)win, width, height, res, (int)ref);
 return fxMesaCreateContext(win, res, ref, attribList);
}


/* [retro3dfx 0.1.82] how many colour buffers to open the board with: two,
 * unless RETROGL_COLOR_BUFFERS=3 asks for three. 0.1.81 opened three
 * whenever the swaps waited for the retrace, to keep an 85 Hz CRT from
 * dropping straight to 42.5 fps when a frame misses a refresh - and it bought
 * nothing on the V5 6000: Quake III demo four, 1280x960x32, vsync on, 55.9
 * fps with two buffers and 56.0 with three (vsync off 65.0 / 64.7,
 * 2026-10-02). The swap evidently still holds the command stream until the
 * retrace, so a third buffer never gets drawn into early. Kept as an
 * explicit switch for that investigation; the default does not spend the
 * memory. cb = getenv("RETROGL_COLOR_BUFFERS"); si is the swap interval,
 * no longer consulted. */
static int
rgl_color_buffers_for(const char *cb, const char *si)
{
   (void)si;
   if (cb && (cb[0] == '2' || cb[0] == '3') && cb[1] == '\0')
      return cb[0] - '0';
   return 2;
}


/*
 * Create a new FX/Mesa context and return a handle to it.
 */
fxMesaContext GLAPIENTRY
fxMesaCreateContext(GLuint win,
		    GrScreenResolution_t res,
		    GrScreenRefresh_t ref, const GLint attribList[])
{
 fxMesaContext fxMesa = NULL;
 GLcontext *ctx = NULL, *shareCtx = NULL;
 struct dd_function_table functions;

 int i;
 const char *str;
 int sliaa, numSLI, samplesPerChip;
 struct SstCard_St *voodoo;
 struct tdfx_glide *Glide;

 GLboolean aux;
 GLboolean doubleBuffer;
 GLuint colDepth;
 GLuint depthSize, alphaSize, stencilSize, accumSize;
 GLuint redBits, greenBits, blueBits, alphaBits;
 GrPixelFormat_t pixFmt;
   
 if (TDFX_DEBUG & VERBOSE_DRIVER) {
    fprintf(stderr, "fxMesaCreateContext(...)\n");
 }

 {
    int rw = -1, rh = -1;
    rgl_res_dims((int)res, &rw, &rh);
    rgl_log("fxMesaCreateContext: ENTER win=%lu res=%d (%dx%d) ref=%d",
            (unsigned long)win, (int)res, rw, rh, (int)ref);
 }

 /* Okay, first process the user flags */
 aux = GL_FALSE;
 doubleBuffer = GL_FALSE;
 colDepth = 16;
 depthSize = alphaSize = stencilSize = accumSize = 0;

 i = 0;
 while (attribList[i] != FXMESA_NONE) {
       switch (attribList[i]) {
              case FXMESA_COLORDEPTH:
	           colDepth = attribList[++i];
	           break;
              case FXMESA_DOUBLEBUFFER:
	           doubleBuffer = GL_TRUE;
	           break;
              case FXMESA_ALPHA_SIZE:
	           if ((alphaSize = attribList[++i])) {
	              aux = GL_TRUE;
                   }
	           break;
              case FXMESA_DEPTH_SIZE:
	           if ((depthSize = attribList[++i])) {
	              aux = GL_TRUE;
                   }
	           break;
              case FXMESA_STENCIL_SIZE:
	           stencilSize = attribList[++i];
	           break;
              case FXMESA_ACCUM_SIZE:
	           accumSize = attribList[++i];
	           break;
              /* XXX ugly hack here for sharing display lists */
              case FXMESA_SHARE_CONTEXT:
                   shareCtx = (GLcontext *)attribList[++i];
	           break;
              default:
                   rgl_log("fxMesaCreateContext: BAIL -> NULL (bad attrib %d in attribList)", attribList[i]);
                   fprintf(stderr, "fxMesaCreateContext: ERROR: wrong parameter (%d) passed\n", attribList[i]);
	           return NULL;
       }
       i++;
 }

 if (!fxQueryHardware()) {
    rgl_log("fxMesaCreateContext: no Voodoo hardware (fxQueryHardware failed) -> errorhandler");
    str = "no Voodoo hardware!";
    goto errorhandler;
 }

 grSstSelect(glbCurrentBoard);
 /*grEnable(GR_OPENGL_MODE_EXT);*/ /* [koolsmoky] */
 voodoo = &glbHWConfig.SSTs[glbCurrentBoard];

 fxMesa = (fxMesaContext)CALLOC_STRUCT(tfxMesaContext);
 if (!fxMesa) {
    str = "private context";
    goto errorhandler;
 }

 if (getenv("MESA_FX_INFO")) {
    fxMesa->verbose = GL_TRUE;
 }

 fxMesa->type = voodoo->type;
 fxMesa->HavePalExt = voodoo->HavePalExt && !getenv("MESA_FX_IGNORE_PALEXT");
 fxMesa->HavePixExt = voodoo->HavePixExt && !getenv("MESA_FX_IGNORE_PIXEXT");
 fxMesa->HaveTexFmt = voodoo->HaveTexFmt && !getenv("MESA_FX_IGNORE_TEXFMT");
 fxMesa->HaveCmbExt = voodoo->HaveCmbExt && !getenv("MESA_FX_IGNORE_CMBEXT");
 fxMesa->HaveMirExt = voodoo->HaveMirExt && !getenv("MESA_FX_IGNORE_MIREXT");
 fxMesa->HaveTexUma = voodoo->HaveTexUma && !getenv("MESA_FX_IGNORE_TEXUMA");
 fxMesa->Glide = glbHWConfig.Glide;
 Glide = &fxMesa->Glide;
 fxMesa->HaveTexus2 = Glide->txImgQuantize &&
                      Glide->txMipQuantize &&
                      Glide->txPalToNcc && !getenv("MESA_FX_IGNORE_TEXUS2");

 /* Determine if we need vertex swapping, RGB order and SLI/AA */
 sliaa = 0;
 switch (fxMesa->type) {
        case GR_SSTTYPE_VOODOO:
        case GR_SSTTYPE_SST96:
        case GR_SSTTYPE_Banshee:
             fxMesa->bgrOrder = GL_TRUE;
             fxMesa->snapVertices = (getenv("MESA_FX_NOSNAP") == NULL);
             break;
        case GR_SSTTYPE_Voodoo2:
             fxMesa->bgrOrder = GL_TRUE;
             fxMesa->snapVertices = GL_FALSE;
             break;
        case GR_SSTTYPE_Voodoo4:
        case GR_SSTTYPE_Voodoo5:
             /* number of SLI units and AA Samples per chip */
             if ((str = Glide->grGetRegistryOrEnvironmentStringExt("SSTH3_SLI_AA_CONFIGURATION")) != NULL) {
                sliaa = atoi(str);
             }
        case GR_SSTTYPE_Voodoo3:
        default:
             fxMesa->bgrOrder = GL_FALSE;
             fxMesa->snapVertices = GL_FALSE;
             break;
 }
 /* XXX todo - Add the old SLI/AA settings for Napalm. */
 switch(voodoo->numChips) {
 case 4: /* 4 chips */
   switch(sliaa) {
   case 8: /* 8 Sample AA */
     numSLI         = 1;
     samplesPerChip = 2;
     break;
   case 7: /* 4 Sample AA */
     numSLI         = 1;
     samplesPerChip = 1;
     break;
   case 6: /* 2 Sample AA */
     numSLI         = 2;
     samplesPerChip = 1;
     break;
   default:
     numSLI         = 4;
     samplesPerChip = 1;
   }
   break;
 case 2: /* 2 chips */
   switch(sliaa) {
   case 4: /* 4 Sample AA */
     numSLI         = 1;
     samplesPerChip = 2;
     break;
   case 3: /* 2 Sample AA */
     numSLI         = 1;
     samplesPerChip = 1;
     break;
   default:
     numSLI         = 2;
     samplesPerChip = 1;
   }
   break;
 default: /* 1 chip */
   switch(sliaa) {
   case 1: /* 2 Sample AA */
     numSLI         = 1;
     samplesPerChip = 2;
     break;
   default:
     numSLI         = 1;
     samplesPerChip = 1;
   }
 }

 fxMesa->fsaa = samplesPerChip * voodoo->numChips / numSLI; /* 1:noFSAA, 2:2xFSAA, 4:4xFSAA, 8:8xFSAA */

 switch (fxMesa->colDepth = colDepth) {
   case 15:
     redBits   = 5;
     greenBits = 5;
     blueBits  = 5;
     alphaBits = depthSize ? 1 : 8;
     switch(fxMesa->fsaa) {
       case 8:
         pixFmt = GR_PIXFMT_AA_8_ARGB_1555;
         break;
       case 4:
         pixFmt = GR_PIXFMT_AA_4_ARGB_1555;
         break;
       case 2:
         pixFmt = GR_PIXFMT_AA_2_ARGB_1555;
         break;
       default:
         pixFmt = GR_PIXFMT_ARGB_1555;
     }
     break;
   case 16:
     redBits   = 5;
     greenBits = 6;
     blueBits  = 5;
     alphaBits = depthSize ? 0 : 8;
     switch(fxMesa->fsaa) {
       case 8:
         pixFmt = GR_PIXFMT_AA_8_RGB_565;
         break;
       case 4:
         pixFmt = GR_PIXFMT_AA_4_RGB_565;
         break;
       case 2:
         pixFmt = GR_PIXFMT_AA_2_RGB_565;
         break;
       default:
         pixFmt = GR_PIXFMT_RGB_565;
     }
     break;
   case 24:
     fxMesa->colDepth = 32;
   case 32:
     redBits   = 8;
     greenBits = 8;
     blueBits  = 8;
     alphaBits = 8;
     switch(fxMesa->fsaa) {
       case 8:
         pixFmt = GR_PIXFMT_AA_8_ARGB_8888;
         break;
       case 4:
         pixFmt = GR_PIXFMT_AA_4_ARGB_8888;
         break;
       case 2:
         pixFmt = GR_PIXFMT_AA_2_ARGB_8888;
         break;
       default:
         pixFmt = GR_PIXFMT_ARGB_8888;
     }
     break;
   default:
     rgl_log("fxMesaCreateContext: BAIL -> errorhandler (unsupported colDepth=%d)", colDepth);
     str = "pixelFormat";
     goto errorhandler;
 }
 rgl_log("fxMesaCreateContext: colDepth=%d fsaa=%d -> pixFmt=%d (RGB_565=%d ARGB_1555=%d ARGB_8888=%d)",
         colDepth, (int)fxMesa->fsaa, (int)pixFmt,
         (int)GR_PIXFMT_RGB_565, (int)GR_PIXFMT_ARGB_1555, (int)GR_PIXFMT_ARGB_8888);

 /* Tips:
  * 1. we don't bother setting/checking AUX for stencil, because we'll decide
  *    later whether we have HW stencil, based on depth buffer (thus AUX is
  *    properly set)
  * 2. when both DEPTH and ALPHA are enabled, depth should win. However, it is
  *    not clear whether 15bpp and 32bpp require AUX alpha buffer. Furthermore,
  *    alpha buffering is required only if destination alpha is used in alpha
  *    blending; alpha blending modes that do not use destination alpha can be
  *    used w/o alpha buffer.
  * 3. `alphaBits' is what we can provide
  *    `alphaSize' is what app requests
  *    if we cannot provide enough bits for alpha buffer, we should fallback to
  *    SW alpha. However, setting `alphaBits' to `alphaSize' might confuse some
  *    of the span functions...
  */

 fxMesa->haveHwAlpha = GL_FALSE;
 if (alphaSize && (alphaSize <= alphaBits)) {
    alphaSize = alphaBits;
    fxMesa->haveHwAlpha = GL_TRUE;
 }

 fxMesa->haveHwStencil = (fxMesa->HavePixExt && stencilSize && depthSize == 24);

 fxMesa->haveZBuffer = depthSize > 0;
 fxMesa->haveDoubleBuffer = doubleBuffer;
 fxMesa->haveGlobalPaletteTexture = GL_FALSE;
 fxMesa->board = glbCurrentBoard;

 fxMesa->haveTwoTMUs = (voodoo->nTexelfx > 1);

 if ((str = Glide->grGetRegistryOrEnvironmentStringExt("FX_GLIDE_NUM_TMU"))) {
    if (atoi(str) <= 1) {
       fxMesa->haveTwoTMUs = GL_FALSE;
    }
 }

 if ((str = Glide->grGetRegistryOrEnvironmentStringExt("FX_GLIDE_SWAPPENDINGCOUNT"))) {
    fxMesa->maxPendingSwapBuffers = atoi(str);
    if (fxMesa->maxPendingSwapBuffers > 6) {
       fxMesa->maxPendingSwapBuffers = 6;
    } else if (fxMesa->maxPendingSwapBuffers < 0) {
       fxMesa->maxPendingSwapBuffers = 0;
    }
 } else {
    fxMesa->maxPendingSwapBuffers = 2;
 }

 /* [retro3dfx] read the swap interval from the PROCESS env with our own CRT,
  * not via Glide's grGetRegistryOrEnvironmentStringExt: when the user set
  * nothing, that callback returns Glide's own vsync-on default, so the
  * else-branch (interval 0) never ran - measured cost up to 32% at
  * fillrate-bound resolutions (Q3 1024x768: 38.7 vs 51.3 fps, Voodoo3).
  * An explicit user environment value still wins. */
 if ((str = getenv("FX_GLIDE_SWAPINTERVAL"))) {
    fxMesa->swapInterval = atoi(str);
 } else {
    fxMesa->swapInterval = 0;
 }

 if (TDFX_DEBUG & VERBOSE_DRIVER) {
    fprintf(stderr, "[retro3dfx] pre grSstWinOpen win=%lx res=%d ref=%d HavePixExt=%d pixFmt=%d aux=%d "
                    "vis=%d fg=%d\n",
            (unsigned long)win, (int)res, (int)ref, (int)fxMesa->HavePixExt, (int)pixFmt, (int)aux,
            (int)IsWindowVisible((HWND)(UINT_PTR)win),
            (int)(GetForegroundWindow() == (HWND)(UINT_PTR)win)); fflush(stderr);
 }
 {
    int rw = -1, rh = -1;
    rgl_res_dims((int)res, &rw, &rh);
    rgl_log("fxMesaCreateContext: about to open board: win=%lu res=%d (%dx%d) ref=%d "
            "HavePixExt=%d pixFmt=%d aux=%d fxWinReq=%d",
            (unsigned long)win, (int)res, rw, rh, (int)ref,
            (int)fxMesa->HavePixExt, (int)pixFmt, (int)(aux ? 1 : 0), fxWinReq);
 }
#if defined(__WIN32__)
 /* [retro3dfx] windowed render path first, if the caller asked for it. Falls
  * through to fullscreen grSstWinOpen if unavailable (fxWinOpen returns 0). */
 if (fxWinReq) {
    int wReqW = fxWinReqW, wReqH = fxWinReqH;
    fxWinReq = 0;
    rgl_log("grSstWinOpen[windowed]: -> fxWinOpen(win=%lu, %dx%d, aux=%d)",
            (unsigned long)win, wReqW, wReqH, (int)(aux ? 1 : 0));
    BEGIN_BOARD_LOCK();
    fxMesa->glideContext = fxWinOpen(fxMesa, (FxU32)win, wReqW, wReqH, aux ? 1 : 0);
    END_BOARD_LOCK();
    rgl_log("grSstWinOpen[windowed]: fxWinOpen returned glideContext=%lu (0 == failed, falls back to fullscreen)",
            (unsigned long)fxMesa->glideContext);
    if (!fxMesa->glideContext && (TDFX_DEBUG & VERBOSE_DRIVER))
       fprintf(stderr, "[retro3dfx] windowed open failed; falling back to fullscreen\n");
 }
#endif
 if (!fxMesa->glideContext) {
    BEGIN_BOARD_LOCK();
    int ncol = rgl_color_buffers_for(getenv("RETROGL_COLOR_BUFFERS"),
                                     getenv("FX_GLIDE_SWAPINTERVAL"));
    if (fxMesa->HavePixExt) {
       int rw = -1, rh = -1;
       rgl_res_dims((int)res, &rw, &rh);
       rgl_log("grSstWinOpenExt: win=%lu res=%d (%dx%d) ref=%d colorformat=%d(ABGR) "
               "origin=%d(LOWER_LEFT) pixFmt=%d nColBuffers=%d nAuxBuffers=%d  [Napalm PIXEXT path]",
               (unsigned long)win, (int)res, rw, rh, (int)ref,
               (int)GR_COLORFORMAT_ABGR, (int)GR_ORIGIN_LOWER_LEFT, (int)pixFmt, ncol,
               (int)(aux ? 1 : 0));
       fxMesa->glideContext = Glide->grSstWinOpenExt((FxU32)win, res, ref,
                                                     GR_COLORFORMAT_ABGR, GR_ORIGIN_LOWER_LEFT,
                                                     pixFmt,
                                                     ncol, aux);
       rgl_log("grSstWinOpenExt: returned glideContext=%lu (0 == FAILED)",
               (unsigned long)fxMesa->glideContext);
    } else if (pixFmt == GR_PIXFMT_RGB_565) {
       int rw = -1, rh = -1;
       rgl_res_dims((int)res, &rw, &rh);
       /* THE key line: fullscreen Voodoo3 board open. These are the EXACT args
        * passed to glide3x's grSstWinOpen. */
       rgl_log("grSstWinOpen: win=%lu res=%d (%dx%d) ref=%d colorformat=%d(ABGR) "
               "origin=%d(LOWER_LEFT) nColBuffers=%d nAuxBuffers=%d  [Voodoo3 RGB_565 fullscreen path]",
               (unsigned long)win, (int)res, rw, rh, (int)ref,
               (int)GR_COLORFORMAT_ABGR, (int)GR_ORIGIN_LOWER_LEFT, ncol, (int)(aux ? 1 : 0));
       fxMesa->glideContext = grSstWinOpen((FxU32)win, res, ref,
                                           GR_COLORFORMAT_ABGR, GR_ORIGIN_LOWER_LEFT,
                                           ncol, aux);
       rgl_log("grSstWinOpen: returned glideContext=%lu (0 == FAILED)",
               (unsigned long)fxMesa->glideContext);
    } else {
       rgl_log("fxMesaCreateContext: NO board-open path taken (HavePixExt=0 and "
               "pixFmt=%d != GR_PIXFMT_RGB_565=%d) -> glideContext forced to 0",
               (int)pixFmt, (int)GR_PIXFMT_RGB_565);
       fxMesa->glideContext = 0;
    }
    END_BOARD_LOCK();
 }
 if (TDFX_DEBUG & VERBOSE_DRIVER) {
    fprintf(stderr, "[retro3dfx] post board-open ctx=%p windowed=%d\n",
            (void*)fxMesa->glideContext, (int)fxMesa->windowed); fflush(stderr);
 }
 rgl_log("fxMesaCreateContext: board-open complete: glideContext=%lu windowed=%d (0 == open FAILED)",
         (unsigned long)fxMesa->glideContext, (int)fxMesa->windowed);
 if (!fxMesa->glideContext) {
    rgl_log("fxMesaCreateContext: BAIL -> errorhandler 'grSstWinOpen' (board open returned 0)");
    str = "grSstWinOpen";
    goto errorhandler;
 }
 fxSetupShadowReset();   /* [retro3dfx] fresh Glide context = fresh registers */

 /* [retro3dfx] Quality defaults the stock init never sets (both free on this
  * present-bound card — see retro3dfx/REVIEW-FINDINGS.md):
  *  - Gamma: nothing else loads a gamma ramp, so 16-bit output looks dark. Load
  *    a pow(1/g) ramp (default g=1.3, env FX_GAMMA, "1.0"/"0" = identity/off).
  *    The DAC ramp is global; fxMesaDestroyContext restores identity.
  *  - Dither: Glide resets to GR_DITHER_2x2 (GSST.C), and Mesa only sets 4x4 on a
  *    live glEnable(GL_DITHER) transition, so games that never toggle it band at
  *    2x2. Force 4x4 once here (env FX_DITHER=0 keeps the 2x2 default). */
 {
    const char *gs = getenv("FX_GAMMA");
    double g = gs ? atof(gs) : 1.3;
    if (g >= 0.1 && g != 1.0) {
       FxU32 rr[256], gg[256], bb[256];
       int n = FX_grGetInteger(GR_GAMMA_TABLE_ENTRIES);
       int inc, i, idx;
       if (n <= 0 || n > 256) n = 256;
       inc = 256 / n;
       for (i = 0, idx = 0; i < n; i++, idx += inc) {
          int v = (int)(pow((double)idx / 255.0, 1.0 / g) * 255.0 + 0.5);
          if (v > 255) v = 255; if (v < 0) v = 0;
          rr[i] = gg[i] = bb[i] = (FxU32)v;
       }
       BEGIN_BOARD_LOCK();
       grLoadGammaTable(n, rr, gg, bb);
       END_BOARD_LOCK();
       fxMesa->haveDefaultGamma = GL_TRUE;
#if defined(__WIN32__)
       fxWglNoteGamma(n, rr, gg, bb);   /* 0.1.77: what Get3DFX must report */
#endif
    }
    /* [retro3dfx 0.1.80] QUIT-TRACE: the steps after the board open - the
     * window in which 8x AA froze .124 (2026-09-30) */
    rgl_sync("ctx-create: gamma loaded -> dither, renderer string, Mesa context");
    if (!getenv("FX_DITHER") || atoi(getenv("FX_DITHER")) != 0) {
       BEGIN_BOARD_LOCK();
       grDitherMode(GR_DITHER_4x4);
       END_BOARD_LOCK();
    }
 }

   /* screen */
#if defined(__WIN32__)
   if (fxMesa->windowed) {
      /* [retro3dfx] the render target is our offscreen surface, sized to the
       * window client rect, not the whole board scanout. */
      fxMesa->screen_width = fxMesa->winW;
      fxMesa->screen_height = fxMesa->winH;
   } else
#endif
   {
      fxMesa->screen_width = FX_grSstScreenWidth();
      fxMesa->screen_height = FX_grSstScreenHeight();
   }

   /* window inside screen */
   fxMesa->width = fxMesa->screen_width;
   fxMesa->height = fxMesa->screen_height;

   /* scissor inside window */
   fxMesa->clipMinX = 0;
   fxMesa->clipMaxX = fxMesa->width;
   fxMesa->clipMinY = 0;
   fxMesa->clipMaxY = fxMesa->height;

   if (fxMesa->verbose) {
      FxI32 tmuRam, fbRam;

      /* Not that it matters, but tmuRam and fbRam change after grSstWinOpen. */
      tmuRam = voodoo->tmuConfig[GR_TMU0].tmuRam;
      fbRam  = voodoo->fbRam;
      BEGIN_BOARD_LOCK();
      grGet(GR_MEMORY_TMU, 4, &tmuRam);
      grGet(GR_MEMORY_FB, 4, &fbRam);
      END_BOARD_LOCK();

      fprintf(stderr, "Voodoo Using Glide %s\n", grGetString(GR_VERSION));
      fprintf(stderr, "Voodoo Board: %d/%d, %s, %d GPU\n",
                      fxMesa->board + 1,
                      glbHWConfig.num_sst,
                      grGetString(GR_HARDWARE),
                      voodoo->numChips);
      fprintf(stderr, "Voodoo Memory: FB = %ld, TM = %d x %ld\n",
                      fbRam,
                      voodoo->nTexelfx,
                      tmuRam);
      fprintf(stderr, "Voodoo Screen: %dx%d:%d %s, %svertex snapping\n",
	              fxMesa->screen_width,
                      fxMesa->screen_height,
                      colDepth,
                      fxMesa->bgrOrder ? "BGR" : "RGB",
                      fxMesa->snapVertices ? "" : "no ");
   }

  sprintf(fxMesa->rendererString, "Mesa %s v0.62 %s%s [voodoo-cleanroom 0.1.82]",
          grGetString(GR_RENDERER),
          grGetString(GR_HARDWARE),
          ((fxMesa->type < GR_SSTTYPE_Voodoo4) && (voodoo->numChips > 1)) ? " SLI" : "");

   fxMesa->glVis = _mesa_create_visual(GL_TRUE,		/* RGB mode */
				       doubleBuffer,
				       GL_FALSE,	/* stereo */
				       redBits,		/* RGBA.R bits */
				       greenBits,	/* RGBA.G bits */
				       blueBits,	/* RGBA.B bits */
				       alphaSize,	/* RGBA.A bits */
				       0,		/* index bits */
				       depthSize,	/* depth_size */
				       stencilSize,	/* stencil_size */
				       accumSize,
				       accumSize,
				       accumSize,
				       alphaSize ? accumSize : 0,
                                       1);
   if (!fxMesa->glVis) {
      rgl_log("fxMesaCreateContext: BAIL -> errorhandler (_mesa_create_visual failed)");
      str = "_mesa_create_visual";
      goto errorhandler;
   }

   _mesa_init_driver_functions(&functions);
   ctx = fxMesa->glCtx = _mesa_create_context(fxMesa->glVis, shareCtx,
					      &functions, (void *) fxMesa);
   if (!ctx) {
      rgl_log("fxMesaCreateContext: BAIL -> errorhandler (_mesa_create_context failed)");
      str = "_mesa_create_context";
      goto errorhandler;
   }


   rgl_sync("ctx-create: Mesa context made -> fxDDInitFxMesaContext");
   if (!fxDDInitFxMesaContext(fxMesa)) {
      rgl_log("fxMesaCreateContext: BAIL -> errorhandler (fxDDInitFxMesaContext failed)");
      str = "fxDDInitFxMesaContext";
      goto errorhandler;
   }


   fxMesa->glBuffer = _mesa_create_framebuffer(fxMesa->glVis,
					       GL_FALSE,	/* no software depth */
					       stencilSize && !fxMesa->haveHwStencil,
					       fxMesa->glVis->accumRedBits > 0,
					       alphaSize && !fxMesa->haveHwAlpha);
   if (!fxMesa->glBuffer) {
      rgl_log("fxMesaCreateContext: BAIL -> errorhandler (_mesa_create_framebuffer failed)");
      str = "_mesa_create_framebuffer";
      goto errorhandler;
   }

   glbTotNumCtx++;

   /* install signal handlers */
#if defined(__linux__)
   /* Only install if environment var. is not set. */
   if (!getenv("MESA_FX_NO_SIGNALS")) {
      signal(SIGINT, cleangraphics_handler);
      signal(SIGHUP, cleangraphics_handler);
      signal(SIGPIPE, cleangraphics_handler);
      signal(SIGFPE, cleangraphics_handler);
      signal(SIGBUS, cleangraphics_handler);
      signal(SIGILL, cleangraphics_handler);
      signal(SIGSEGV, cleangraphics_handler);
      signal(SIGTERM, cleangraphics_handler);
   }
#endif

   rgl_sync("ctx-create: SUCCESS (%dx%d colDepth %d)", fxMesa->screen_width, fxMesa->screen_height,
            colDepth);
   rgl_log("fxMesaCreateContext: SUCCESS glideContext=%lu screen=%dx%d colDepth=%d windowed=%d",
           (unsigned long)fxMesa->glideContext, fxMesa->screen_width, fxMesa->screen_height,
           colDepth, (int)fxMesa->windowed);
   return fxMesa;

errorhandler:
 if (fxMesa) {
    if (fxMesa->glideContext) {
       grSstWinClose(fxMesa->glideContext);
       fxMesa->glideContext = 0;
    }

    if (fxMesa->state) {
       FREE(fxMesa->state);
    }
    if (fxMesa->fogTable) {
       FREE(fxMesa->fogTable);
    }
    if (fxMesa->glBuffer) {
       _mesa_destroy_framebuffer(fxMesa->glBuffer);
    }
    if (fxMesa->glVis) {
       _mesa_destroy_visual(fxMesa->glVis);
    }
    if (fxMesa->glCtx) {
       _mesa_destroy_context(fxMesa->glCtx);
    }
    FREE(fxMesa);
 }

 rgl_log("fxMesaCreateContext: RETURN NULL (errorhandler): %s", str);
 fprintf(stderr, "fxMesaCreateContext: ERROR: %s\n", str);
 return NULL;
}


/*
 * Function to set the new window size in the context (mainly for the Voodoo Rush)
 */
void GLAPIENTRY
fxMesaUpdateScreenSize(fxMesaContext fxMesa)
{
   fxMesa->width = FX_grSstScreenWidth();
   fxMesa->height = FX_grSstScreenHeight();
}


/*
 * Destroy the given FX/Mesa context.
 */
void GLAPIENTRY
fxMesaDestroyContext(fxMesaContext fxMesa)
{
   if (TDFX_DEBUG & VERBOSE_DRIVER) {
      fprintf(stderr, "fxMesaDestroyContext(...)\n");
   }

   if (!fxMesa)
      return;

   /* [retro3dfx 0.1.79] QUIT-TRACE (fxrlog.h) */
   rgl_sync("fxMesaDestroyContext: enter at swap %lu", rgl_swaps);

   if (fxMesa->verbose) {
      fprintf(stderr, "Misc Stats:\n");
      fprintf(stderr, "  # swap buffer: %u\n", fxMesa->stats.swapBuffer);

      if (!fxMesa->stats.swapBuffer)
	 fxMesa->stats.swapBuffer = 1;

      fprintf(stderr, "Textures Stats:\n");
      fprintf(stderr, "  Free texture memory on TMU0: %d\n",
	      fxMesa->freeTexMem[FX_TMU0]);
      if (fxMesa->haveTwoTMUs)
	 fprintf(stderr, "  Free texture memory on TMU1: %d\n",
		 fxMesa->freeTexMem[FX_TMU1]);
      fprintf(stderr, "  # request to TMM to upload a texture objects: %u\n",
	      fxMesa->stats.reqTexUpload);
      fprintf(stderr,
	      "  # request to TMM to upload a texture objects per swapbuffer: %.2f\n",
	      fxMesa->stats.reqTexUpload / (float) fxMesa->stats.swapBuffer);
      fprintf(stderr, "  # texture objects uploaded: %u\n",
	      fxMesa->stats.texUpload);
      fprintf(stderr, "  # texture objects uploaded per swapbuffer: %.2f\n",
	      fxMesa->stats.texUpload / (float) fxMesa->stats.swapBuffer);
      fprintf(stderr, "  # MBs uploaded to texture memory: %.2f\n",
	      fxMesa->stats.memTexUpload / (float) (1 << 20));
      fprintf(stderr,
	      "  # MBs uploaded to texture memory per swapbuffer: %.2f\n",
	      (fxMesa->stats.memTexUpload /
	       (float) fxMesa->stats.swapBuffer) / (float) (1 << 20));
   }

   glbTotNumCtx--;

   if (!glbTotNumCtx && getenv("MESA_FX_INFO")) {
      GrSstPerfStats_t st;

      FX_grSstPerfStats(&st);

      fprintf(stderr, "Pixels Stats:\n");
      fprintf(stderr, "  # pixels processed (minus buffer clears): %u\n",
              (unsigned) st.pixelsIn);
      fprintf(stderr, "  # pixels not drawn due to chroma key test failure: %u\n",
              (unsigned) st.chromaFail);
      fprintf(stderr, "  # pixels not drawn due to depth test failure: %u\n",
              (unsigned) st.zFuncFail);
      fprintf(stderr,
              "  # pixels not drawn due to alpha test failure: %u\n",
              (unsigned) st.aFuncFail);
      fprintf(stderr, "  # pixels drawn (including buffer clears and LFB writes): %u\n",
              (unsigned) st.pixelsOut);
   }

   /* [retro3dfx] restore an identity DAC gamma ramp if we loaded a non-identity
    * one at context create — the ramp is global, so leaving it would brighten
    * the desktop after the game exits. */
   if (fxMesa->haveDefaultGamma) {
      FxU32 rr[256], gg[256], bb[256];
      int n = FX_grGetInteger(GR_GAMMA_TABLE_ENTRIES);
      int inc, i, idx;
      if (n <= 0 || n > 256) n = 256;
      inc = 256 / n;
      for (i = 0, idx = 0; i < n; i++, idx += inc)
         rr[i] = gg[i] = bb[i] = (FxU32)idx;
      rgl_sync("fxMesaDestroyContext: identity gamma grLoadGammaTable(%d) ->", n);
      BEGIN_BOARD_LOCK();
      grLoadGammaTable(n, rr, gg, bb);
      END_BOARD_LOCK();
      rgl_sync("fxMesaDestroyContext: identity gamma returned");
#if defined(__WIN32__)
      fxWglNoteGamma(n, rr, gg, bb);    /* 0.1.77: identity is back in the DAC */
#endif
   }

   /* close the hardware first,
    * so we can debug atexit problems (memory leaks, etc).
    */
   /* [retro3dfx] 0.1.67 teardown breadcrumbs. Quake II never reaches the end
    * of its own quit on the all-ours V5 6000 stack (the runner force-kills it
    * 10 s later), and a force-kill is what leaves the display driver's stale
    * per-PID slot behind; a log line per step names the step that stops. */
   rgl_log("fxMesaDestroyContext: closing board (windowed=%d)", (int) fxMesa->windowed);
   rgl_sync("fxMesaDestroyContext: closing board (windowed=%d) -> grSstWinClose",
            (int) fxMesa->windowed);
#if defined(__WIN32__)
   if (fxMesa->windowed)
      fxWinClose(fxMesa);   /* grSurfaceReleaseContext + release DDraw surfaces */
   else
#endif
      grSstWinClose(fxMesa->glideContext);
   rgl_log("fxMesaDestroyContext: board closed; fxCloseHardware");
   rgl_sync("fxMesaDestroyContext: board closed -> fxCloseHardware");
   fxCloseHardware();

   fxDDDestroyFxMesaContext(fxMesa); /* must be before _mesa_destroy_context */
   _mesa_destroy_visual(fxMesa->glVis);
   _mesa_destroy_context(fxMesa->glCtx);
   _mesa_destroy_framebuffer(fxMesa->glBuffer);
   fxTMClose(fxMesa); /* must be after _mesa_destroy_context */

   FREE(fxMesa);

   if (fxMesa == fxMesaCurrentCtx)
      fxMesaCurrentCtx = NULL;
   rgl_log("fxMesaDestroyContext: done");
}


/*
 * Make the specified FX/Mesa context the current one.
 */
void GLAPIENTRY
fxMesaMakeCurrent(fxMesaContext fxMesa)
{
   if (!fxMesa) {
      _mesa_make_current(NULL, NULL);
      fxMesaCurrentCtx = NULL;

      if (TDFX_DEBUG & VERBOSE_DRIVER) {
	 fprintf(stderr, "fxMesaMakeCurrent(NULL)\n");
      }

      return;
   }

   /* if this context is already the current one, we can return early */
   if (fxMesaCurrentCtx == fxMesa
       && fxMesaCurrentCtx->glCtx == _mesa_get_current_context()) {
      if (TDFX_DEBUG & VERBOSE_DRIVER) {
	 fprintf(stderr, "fxMesaMakeCurrent(NOP)\n");
      }

      return;
   }

   if (TDFX_DEBUG & VERBOSE_DRIVER) {
      fprintf(stderr, "fxMesaMakeCurrent(...)\n");
   }

   if (fxMesaCurrentCtx)
      grGlideGetState((GrState *) fxMesaCurrentCtx->state);

   fxMesaCurrentCtx = fxMesa;

   grSstSelect(fxMesa->board);
   rgl_sync("fxMesaMakeCurrent: grGlideSetState -> (T-buffer mask, state to hw)");
   grGlideSetState((GrState *) fxMesa->state);
   rgl_sync("fxMesaMakeCurrent: grGlideSetState returned");
   fxSetupShadowReset();   /* [retro3dfx] SetState rewrote the real registers */

   _mesa_make_current(fxMesa->glCtx, fxMesa->glBuffer);

   fxSetupDDPointers(fxMesa->glCtx);

   /* The first time we call MakeCurrent we set the initial viewport size */
   if (fxMesa->glCtx->Viewport.Width == 0)
      _mesa_set_viewport(fxMesa->glCtx, 0, 0, fxMesa->width, fxMesa->height);
}


/*
 * Swap front/back buffers for current context if double buffered.
 */
void GLAPIENTRY
fxMesaSwapBuffers(void)
{
   /* [retro3dfx] FX_PROFILE=1: per-frame cost report, every 100 frames. */
   {
      extern unsigned long fxp_setup_calls, fxp_dbl_calls, fxp_single_calls;
      extern unsigned long fxp_texcomb_issued, fxp_texcomb_skipped, fxp_texsource_issued;
      extern unsigned long long fxp_setup_cycles;
      extern unsigned long fxp_pipeline_runs, fxp_verts;
      extern unsigned long long fxp_swap_cycles;
      extern unsigned long fxp_fixup, fxp_choose, fxp_begins, fxp_vsize;
      extern unsigned long long fxp_imm_cycles;
      extern unsigned long long fxp_pipeline_cycles;
      extern int fxp_enabled;
      static unsigned long frames = 0;
      if (fxp_enabled < 0) fxp_enabled = getenv("FX_PROFILE") ? 1 : 0;
      if (fxp_enabled > 0 && ++frames % 100 == 0) {
         rgl_log("PROF f=%lu setup/f=%lu dbl/f=%lu pipe/f=%lu verts/f=%lu "
                 "kcyc_pipe/f=%lu kcyc_setup/f=%lu KCYC_SWAP/f=%lu FIXUP/f=%lu BEGINS/f=%lu VSZ=%lu KCYC_IMM/f=%lu",
                 frames, fxp_setup_calls/100, fxp_dbl_calls/100,
                 fxp_pipeline_runs/100, fxp_verts/100,
                 (unsigned long)(fxp_pipeline_cycles/100/1000),
                 (unsigned long)(fxp_setup_cycles/100/1000),
                 (unsigned long)(fxp_swap_cycles/100/1000),
                 fxp_fixup/100, fxp_begins/100, fxp_vsize,
                 (unsigned long)(fxp_imm_cycles/100/1000));
         fxp_setup_calls = fxp_dbl_calls = fxp_single_calls = 0;
         fxp_texcomb_issued = fxp_texcomb_skipped = fxp_texsource_issued = 0;
         fxp_setup_cycles = 0;
         fxp_pipeline_runs = fxp_verts = 0; fxp_pipeline_cycles = 0;
         fxp_swap_cycles = 0;
         fxp_fixup = fxp_choose = fxp_begins = 0; fxp_imm_cycles = 0;
      }
   }
   if (TDFX_DEBUG & VERBOSE_DRIVER) {
      fprintf(stderr, "fxMesaSwapBuffers()\n");
   }

   if (fxMesaCurrentCtx) {
      _mesa_notifySwapBuffers(fxMesaCurrentCtx->glCtx);

      if (fxMesaCurrentCtx->haveDoubleBuffer) {

#if defined(__WIN32__)
	 if (fxMesaCurrentCtx->windowed) {
	    /* [retro3dfx] present the offscreen surface to the window (no page
	     * flip in windowed mode). grBufferSwap is completed first inside the
	     * board so the render is finished before we Blt it out. */
	    {
	       /* [retro3dfx] FX_PROFILE: everything Mesa does is identical between
	        * single- and multi-textured frames (TNL 5.39 vs 5.55ms, same vertex
	        * count, 0 texture downloads), yet the frame is 14ms longer. If that
	        * time is the hardware, it shows up as a blocking swap. */
	       extern int fxp_enabled; extern unsigned long long fxp_swap_cycles;
	       unsigned long _l0,_h0,_l1,_h1;
	       if (fxp_enabled > 0) __asm__ __volatile__("rdtsc":"=a"(_l0),"=d"(_h0));
	       grBufferSwap(0);
	       if (fxp_enabled > 0) {
	          __asm__ __volatile__("rdtsc":"=a"(_l1),"=d"(_h1));
	          fxp_swap_cycles += (((unsigned long long)_h1<<32)|_l1)
	                           - (((unsigned long long)_h0<<32)|_l0);
	       }
	    }
	    fxWinSwap(fxMesaCurrentCtx);
	 } else
#endif
	 grBufferSwap(fxMesaCurrentCtx->swapInterval);

#if 0
	 /*
	  * Don't allow swap buffer commands to build up!
	  */
	 while (FX_grGetInteger(GR_PENDING_BUFFERSWAPS) >
		fxMesaCurrentCtx->maxPendingSwapBuffers)
	    /* The driver is able to sleep when waiting for the completation
	       of multiple swapbuffer operations instead of wasting
	       CPU time (NOTE: you must uncomment the following line in the
	       in order to enable this option) */
	    /* usleep(10000); */
	    ;
#endif

	 fxMesaCurrentCtx->stats.swapBuffer++;
      }
   }
}


/*
 * Shutdown Glide library
 */
void GLAPIENTRY
fxCloseHardware(void)
{
   if (glbGlideInitialized) {
      if (glbTotNumCtx == 0) {
	 /* [retro3dfx] Keep Glide INITIALIZED across a context destroy by default.
	  * A game's in-engine resolution change (idTech `vid_restart`) destroys the
	  * GL context and immediately recreates it at the new mode. The stock path
	  * did grSstWinClose -> grGlideShutdown -> grGlideInit -> grSstWinOpen(newRes)
	  * — a full Glide teardown+reinit with a hardware mode-change mid-flight,
	  * which WEDGES the Voodoo3 board and hangs the whole box (see
	  * DEBUGGING-NOTES 2026-07-20). Leaving Glide initialized turns the switch
	  * into the far less disruptive grSstWinClose -> grSstWinOpen(newRes). The
	  * board is still released by grSstWinClose, and the process teardown (atexit
	  * cleangraphics / OS) reclaims Glide on final exit. FX_GLIDE_SHUTDOWN=1
	  * restores the old shutdown-on-last-context behaviour for A/B testing. */
	 if (glbProcessExiting || getenv("FX_GLIDE_SHUTDOWN")) {
	    rgl_log("fxCloseHardware: grGlideShutdown()");
	    rgl_sync("fxCloseHardware: grGlideShutdown ->");
	    grGlideShutdown();
	    rgl_sync("fxCloseHardware: grGlideShutdown returned");
	    rgl_log("fxCloseHardware: grGlideShutdown returned");
	    glbGlideInitialized = 0;
	 }
      }
   }
}


#else


/*
 * Need this to provide at least one external definition.
 */
extern int gl_fx_dummy_function_api(void);
int
gl_fx_dummy_function_api(void)
{
   return 0;
}

#endif /* FX */

/*
 * [retro3dfx] Windowed-Glide rendering for the MesaFX ICD.
 *
 * Some engines (idTech GoldSrc / Half-Life / Counter-Strike 1.6) drive
 * fullscreen OpenGL by calling ChangeDisplaySettings(CDS_FULLSCREEN) themselves
 * and rendering GL into a desktop-owned window. Our normal path takes the Voodoo
 * board via grSstWinOpen, which does DirectDraw SetCooperativeLevel(EXCLUSIVE |
 * FULLSCREEN) + SetDisplayMode -- that collides with the engine's own mode
 * ownership and the process dies right after GL init, before any frame.
 *
 * The Voodoo3 (Avenger) is a 2D+3D part and can render Glide into an offscreen
 * video-memory surface with the desktop left alone (DDSCL_NORMAL), then Blt that
 * surface to the window -- exactly what 3dfx's own GoldSrc MiniGL does. Glide3
 * exposes this via the grSurface* extension API (grSurfaceCreateContextExt /
 * grSurfaceSetRenderingSurfaceExt / grSurfaceSetAuxSurfaceExt), verified present
 * in the fleet's retail glide3x.dll. This module wires that path.
 *
 * Reference: H5/GLIDE3/CONFORM/UTILITY/{win32_ddsurf,WIN32}.cpp (the exact DDraw
 * surface recipe) and H5/GLIDE3/SRC/GSFC.C (the windowed surface implementation).
 *
 * Everything here is resolved at runtime (grGetProcAddress for the Glide procs,
 * LoadLibrary for DirectDrawCreate) so the build needs no extra import libs, and
 * it degrades safely: if any piece is unavailable, fxWinOpen returns 0 and the
 * caller falls back to the normal fullscreen grSstWinOpen path.
 */

#ifdef HAVE_CONFIG_H
#include "conf.h"
#endif

#if defined(FX) && defined(__WIN32__)

#include <windows.h>
#include <ddraw.h>
#include <stdarg.h>
#include "fxdrv.h"

/* GR_SURFACECONTEXT_WINDOWED (GSFC.H) */
#define FXWIN_CTX_WINDOWED 0

/* [retro3dfx] direct-file diag that bypasses stderr/freopen (which may not land
 * for a GUI process). Enabled by FX_WINDOWED_LOG=<path>; off by default. */
static void fxWinLog(const char *fmt, ...)
{
   const char *path = getenv("FX_WINDOWED_LOG");
   FILE *f;
   va_list ap;
   if (!path) return;
   f = fopen(path, "a");
   if (!f) return;
   va_start(ap, fmt);
   vfprintf(f, fmt, ap);
   va_end(ap);
   fclose(f);
}

typedef GrContext_t (FX_CALL *pfnSurfCreateCtx)(int type);
typedef void        (FX_CALL *pfnSurfReleaseCtx)(GrContext_t ctx);
typedef void        (FX_CALL *pfnSurfSetSurface)(void *sfc);

static pfnSurfCreateCtx   p_grSurfaceCreateContext  = NULL;
static pfnSurfReleaseCtx  p_grSurfaceReleaseContext = NULL;
static pfnSurfSetSurface  p_grSurfaceSetRendering   = NULL;
static pfnSurfSetSurface  p_grSurfaceSetAux         = NULL;
static int fxWinProcsResolved = 0;   /* 0=unknown, 1=ok, -1=unavailable */

typedef HRESULT (WINAPI *pfnDirectDrawCreate)(GUID FAR *, LPDIRECTDRAW FAR *, IUnknown FAR *);
static pfnDirectDrawCreate p_DirectDrawCreate = NULL;

static int
fxWinResolveProcs(void)
{
   if (fxWinProcsResolved) return fxWinProcsResolved == 1;

   p_grSurfaceCreateContext  = (pfnSurfCreateCtx)  grGetProcAddress("grSurfaceCreateContextExt");
   p_grSurfaceReleaseContext = (pfnSurfReleaseCtx) grGetProcAddress("grSurfaceReleaseContextExt");
   p_grSurfaceSetRendering   = (pfnSurfSetSurface) grGetProcAddress("grSurfaceSetRenderingSurfaceExt");
   p_grSurfaceSetAux         = (pfnSurfSetSurface) grGetProcAddress("grSurfaceSetAuxSurfaceExt");

   if (!p_DirectDrawCreate) {
      HMODULE dd = LoadLibraryA("ddraw.dll");
      if (dd)
         p_DirectDrawCreate = (pfnDirectDrawCreate) GetProcAddress(dd, "DirectDrawCreate");
   }

   fxWinProcsResolved =
      (p_grSurfaceCreateContext && p_grSurfaceSetRendering && p_DirectDrawCreate) ? 1 : -1;
   if (fxWinProcsResolved != 1 && (TDFX_DEBUG & VERBOSE_DRIVER))
      fprintf(stderr, "[retro3dfx] windowed unavailable: surfCtx=%p setRender=%p ddCreate=%p\n",
              (void*)p_grSurfaceCreateContext, (void*)p_grSurfaceSetRendering, (void*)p_DirectDrawCreate);
   return fxWinProcsResolved == 1;
}

static LPDIRECTDRAWSURFACE
fxWinMakeOffscreen(LPDIRECTDRAW dd, int w, int h)
{
   DDSURFACEDESC desc;
   LPDIRECTDRAWSURFACE s = NULL;
   memset(&desc, 0, sizeof(desc));
   desc.dwSize  = sizeof(DDSURFACEDESC);
   desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
   desc.dwWidth = w;
   desc.dwHeight = h;
   desc.ddsCaps.dwCaps = DDSCAPS_VIDEOMEMORY | DDSCAPS_3DDEVICE;
   desc.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
   desc.ddpfPixelFormat.dwFlags = DDPF_RGB;
   desc.ddpfPixelFormat.dwRGBBitCount = 16;
   desc.ddpfPixelFormat.dwRBitMask = 0x0000f800UL;   /* 565 */
   desc.ddpfPixelFormat.dwGBitMask = 0x000007e0UL;
   desc.ddpfPixelFormat.dwBBitMask = 0x0000001fUL;
   if (IDirectDraw_CreateSurface(dd, &desc, &s, NULL) != DD_OK)
      return NULL;
   return s;
}

static LPDIRECTDRAWSURFACE
fxWinMakeZBuffer(LPDIRECTDRAW dd, int w, int h)
{
   DDSURFACEDESC desc;
   LPDIRECTDRAWSURFACE s = NULL;
   memset(&desc, 0, sizeof(desc));
   desc.dwSize  = sizeof(DDSURFACEDESC);
   desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_ZBUFFERBITDEPTH;
   desc.dwWidth = w;
   desc.dwHeight = h;
   desc.dwZBufferBitDepth = 16;
   desc.ddsCaps.dwCaps = DDSCAPS_VIDEOMEMORY | DDSCAPS_ZBUFFER;
   if (IDirectDraw_CreateSurface(dd, &desc, &s, NULL) != DD_OK)
      return NULL;
   return s;
}

/*
 * Open a windowed Glide rendering context into hWnd's client area.
 * Returns the GrContext_t on success (non-zero), or 0 on any failure (caller
 * then falls back to grSstWinOpen). On success, fills fxMesa->win* + windowed.
 */
GrContext_t
fxWinOpen(fxMesaContext fxMesa, FxU32 hWnd, int w, int h, int wantAux)
{
   LPDIRECTDRAW dd = NULL;
   LPDIRECTDRAWSURFACE front = NULL, back = NULL, aux = NULL;
   LPDIRECTDRAWCLIPPER clip = NULL;
   DDSURFACEDESC pdesc;
   GrContext_t gctx = 0;

   fxWinLog("fxWinOpen hwnd=%lx %dx%d aux=%d\n", (unsigned long)hWnd, w, h, wantAux);
   if (w <= 0 || h <= 0) return 0;
   if (!fxWinResolveProcs()) { fxWinLog("  resolveProcs FAILED\n"); return 0; }
   fxWinLog("  procs ok (surfCtx=%p setRender=%p ddCreate=%p)\n",
            (void*)p_grSurfaceCreateContext, (void*)p_grSurfaceSetRendering, (void*)p_DirectDrawCreate);

   if (p_DirectDrawCreate(NULL, &dd, NULL) != DD_OK || !dd) { fxWinLog("  DirectDrawCreate FAILED\n"); return 0; }
   fxWinLog("  DirectDrawCreate ok dd=%p\n", (void*)dd);

   /* cooperative (NOT exclusive) -- leaves the engine's desktop mode alone */
   if (IDirectDraw_SetCooperativeLevel(dd, (HWND)(UINT_PTR)hWnd, DDSCL_NORMAL) != DD_OK) { fxWinLog("  SetCoopLevel FAILED\n"); goto fail; }
   fxWinLog("  SetCoopLevel(NORMAL) ok\n");

   /* primary surface + a clipper bound to the window (so Blt clips to it) */
   memset(&pdesc, 0, sizeof(pdesc));
   pdesc.dwSize = sizeof(DDSURFACEDESC);
   pdesc.dwFlags = DDSD_CAPS;
   pdesc.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
   if (IDirectDraw_CreateSurface(dd, &pdesc, &front, NULL) != DD_OK || !front) { fxWinLog("  CreateSurface(PRIMARY) FAILED\n"); goto fail; }
   fxWinLog("  primary ok front=%p\n", (void*)front);
   if (IDirectDraw_CreateClipper(dd, 0, &clip, NULL) == DD_OK && clip) {
      IDirectDrawClipper_SetHWnd(clip, 0, (HWND)(UINT_PTR)hWnd);
      IDirectDrawSurface_SetClipper(front, clip);
   }

   /* offscreen video-memory back buffer that Glide rasterizes into. Create ALL
    * DDraw surfaces (back + Z) BEFORE engaging the Glide context: creating a
    * DDraw surface AFTER grSurfaceSetRendering faults (Glide has taken the board
    * memory), so allocate everything up front. */
   back = fxWinMakeOffscreen(dd, w, h);
   if (!back) { fxWinLog("  CreateSurface(BACK vidmem/3ddevice 565) FAILED\n"); goto fail; }
   fxWinLog("  back ok back=%p\n", (void*)back);

   if (wantAux && !getenv("FX_WINDOWED_NOAUX")) {
      /* aux (depth) is a plain 16-bit surface, same format as the color back
       * buffer -- that's what grSurfaceSetAux expects (win32_ddsurf.cpp), NOT a
       * DDSCAPS_ZBUFFER surface (which crashes grSurfaceSetAux). */
      aux = getenv("FX_WINDOWED_ZAUX") ? fxWinMakeZBuffer(dd, w, h)
                                       : fxWinMakeOffscreen(dd, w, h);
      fxWinLog("  aux surface -> %p\n", (void*)aux);
   }

   /* create the windowed Glide context and point it at our surfaces */
   gctx = p_grSurfaceCreateContext(FXWIN_CTX_WINDOWED);
   fxWinLog("  grSurfaceCreateContext -> %lx\n", (unsigned long)gctx);
   if (!gctx) goto fail;
   grSelectContext(gctx);
   fxWinLog("  grSelectContext ok\n");
   p_grSurfaceSetRendering((void*)back);
   fxWinLog("  grSurfaceSetRendering(back) ok\n");
   if (aux && p_grSurfaceSetAux) {
      p_grSurfaceSetAux((void*)aux);
      fxWinLog("  grSurfaceSetAux(aux) ok\n");
   }
   fxWinLog("  fxWinOpen SUCCESS ctx=%lx\n", (unsigned long)gctx);

   fxMesa->windowed = GL_TRUE;
   fxMesa->winDDObj  = (void*)dd;
   fxMesa->winFront  = (void*)front;
   fxMesa->winBack   = (void*)back;
   fxMesa->winAux    = (void*)aux;
   fxMesa->winClip   = (void*)clip;
   fxMesa->winHwnd   = hWnd;
   fxMesa->winW = w;
   fxMesa->winH = h;
   if (TDFX_DEBUG & VERBOSE_DRIVER)
      fprintf(stderr, "[retro3dfx] windowed ctx=%lx %dx%d aux=%p\n",
              (unsigned long)gctx, w, h, (void*)aux);
   return gctx;

fail:
   if (aux)   IDirectDrawSurface_Release(aux);
   if (back)  IDirectDrawSurface_Release(back);
   if (clip)  IDirectDrawClipper_Release(clip);
   if (front) IDirectDrawSurface_Release(front);
   if (dd)    IDirectDraw_Release(dd);
   return 0;
}

/* Present: Blt the back buffer to the window's client rect on the primary. */
void
fxWinSwap(fxMesaContext fxMesa)
{
   LPDIRECTDRAWSURFACE front = (LPDIRECTDRAWSURFACE)fxMesa->winFront;
   LPDIRECTDRAWSURFACE back  = (LPDIRECTDRAWSURFACE)fxMesa->winBack;
   RECT src, dst;
   POINT p;
   HWND hWnd = (HWND)(UINT_PTR)fxMesa->winHwnd;

   if (!front || !back) return;

   src.left = 0; src.top = 0; src.right = fxMesa->winW; src.bottom = fxMesa->winH;
   p.x = 0; p.y = 0;
   ClientToScreen(hWnd, &p);
   dst.left = p.x; dst.top = p.y;
   dst.right = p.x + fxMesa->winW; dst.bottom = p.y + fxMesa->winH;

   IDirectDrawSurface_Blt(front, &dst, back, &src, DDBLT_WAIT, NULL);
}

void
fxWinClose(fxMesaContext fxMesa)
{
   if (!fxMesa->windowed) return;
   if (p_grSurfaceReleaseContext && fxMesa->glideContext)
      p_grSurfaceReleaseContext(fxMesa->glideContext);
   if (fxMesa->winAux)   IDirectDrawSurface_Release((LPDIRECTDRAWSURFACE)fxMesa->winAux);
   if (fxMesa->winBack)  IDirectDrawSurface_Release((LPDIRECTDRAWSURFACE)fxMesa->winBack);
   if (fxMesa->winClip)  IDirectDrawClipper_Release((LPDIRECTDRAWCLIPPER)fxMesa->winClip);
   if (fxMesa->winFront) IDirectDrawSurface_Release((LPDIRECTDRAWSURFACE)fxMesa->winFront);
   if (fxMesa->winDDObj) IDirectDraw_Release((LPDIRECTDRAW)fxMesa->winDDObj);
   fxMesa->winDDObj = fxMesa->winFront = fxMesa->winBack = NULL;
   fxMesa->winAux = fxMesa->winClip = NULL;
   fxMesa->windowed = GL_FALSE;
}

#else
extern int fxwindow_c_dummy;
int fxwindow_c_dummy = 0;
#endif

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
 *    Keith Whitwell
 *    Hiroshi Morii
 *    Daniel Borca
 */

/* fxwgl.c - Microsoft wgl functions emulation for
 *           3Dfx VooDoo/Mesa interface
 */


#ifdef _WIN32

#ifdef __cplusplus
extern "C"
{
#endif

#include <windows.h>
#define GL_GLEXT_PROTOTYPES
#include "GL/gl.h"
#include "GL/glext.h"

#ifdef __cplusplus
}
#endif

#include "GL/fxmesa.h"
#include "glheader.h"
#include "glapi.h"
#include "imports.h"
#include "fxdrv.h"
#include "fxrlog.h"   /* [retro3dfx] C:\retrogl.log context-creation tracer */

#define MAX_MESA_ATTRS  20

#if (_MSC_VER >= 1200)
#pragma warning( push )
#pragma warning( disable : 4273 )
#endif

struct __pixelformat__
{
   PIXELFORMATDESCRIPTOR pfd;
   GLint mesaAttr[MAX_MESA_ATTRS];
};

/* [retro3dfx] 0.1.77: the ramp WGL_3DFX_gamma_control reports. Until 0.1.76
 * this was zero-filled until the game's first Set, while the DAC really held
 * our FX_GAMMA ramp (fxapi.c) or Glide's identity. id Tech 3 saves what Get
 * returns as the "original" gamma (WG_CheckHardwareGamma) and loads it back in
 * GLimp_Shutdown (WG_RestoreGamma), so every vid_restart and every quit loaded
 * an ALL-ZERO CLUT: black until the next Set, or until our own identity
 * restore at context destroy (none when FX_GAMMA=1.0). Soldier of Fortune II
 * MP does exactly this: sof2mp.exe 0x4de050 saves, 0x4de330 restores. Now the
 * table tracks every ramp this ICD loads: identity before the first (what
 * Glide's board open leaves unless SSTH3_*GAMMA is set), then
 * fxWglNoteGamma() and wglSetDeviceGammaRamp3DFX. */
static GLushort gammaTable[3*256];
static GLboolean gammaTableKnown = GL_FALSE;

/* An n-entry 8-bit Glide ramp (what grLoadGammaTable takes) as the 3 x 256
 * 16-bit WGL ramp: entry i of 256 comes from Glide entry i*n/256, the inverse
 * of wglSetDeviceGammaRamp3DFX's index = i * (256/n), widened v -> v*0x101. */
void
fxWglNoteGamma(int n, const FxU32 *r, const FxU32 *g, const FxU32 *b)
{
 int i, k;
 if (n <= 0 || n > 256 || !r || !g || !b)
    return;
 for (i = 0; i < 256; i++) {
     k = i * n / 256;
     gammaTable[i]       = (GLushort)((r[k] & 0xff) * 0x101);
     gammaTable[256 + i] = (GLushort)((g[k] & 0xff) * 0x101);
     gammaTable[512 + i] = (GLushort)((b[k] & 0xff) * 0x101);
 }
 gammaTableKnown = GL_TRUE;
}

static struct __pixelformat__ pix[] = {
   /* 16bit RGB565 single buffer with depth */
   {
    {sizeof(PIXELFORMATDESCRIPTOR), 1,
     PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL,
     PFD_TYPE_RGBA,
     16,
     5, 0, 6, 5, 5, 11, 0, 0,
     0, 0, 0, 0, 0,
     16,
     0,
     0,
     PFD_MAIN_PLANE,
     0, 0, 0, 0}
    ,
    {FXMESA_COLORDEPTH, 16,
     FXMESA_ALPHA_SIZE, 0,
     FXMESA_DEPTH_SIZE, 16,
     FXMESA_STENCIL_SIZE, 0,
     FXMESA_ACCUM_SIZE, 0,
     FXMESA_NONE}
   }
   ,
   /* 16bit RGB565 double buffer with depth */
   {
    {sizeof(PIXELFORMATDESCRIPTOR), 1,
     PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL |
     PFD_DOUBLEBUFFER | PFD_SWAP_COPY,
     PFD_TYPE_RGBA,
     16,
     5, 0, 6, 5, 5, 11, 0, 0,
     0, 0, 0, 0, 0,
     16,
     0,
     0,
     PFD_MAIN_PLANE,
     0, 0, 0, 0}
    ,
    {FXMESA_COLORDEPTH, 16,
     FXMESA_DOUBLEBUFFER,
     FXMESA_ALPHA_SIZE, 0,
     FXMESA_DEPTH_SIZE, 16,
     FXMESA_STENCIL_SIZE, 0,
     FXMESA_ACCUM_SIZE, 0,
     FXMESA_NONE}
   }
   ,
   /* 16bit ARGB1555 single buffer with depth */
   {
    {sizeof(PIXELFORMATDESCRIPTOR), 1,
     PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL,
     PFD_TYPE_RGBA,
     16,
     5, 0, 5, 5, 5, 10, 1, 15,
     0, 0, 0, 0, 0,
     16,
     0,
     0,
     PFD_MAIN_PLANE,
     0, 0, 0, 0}
    ,
    {FXMESA_COLORDEPTH, 15,
     FXMESA_ALPHA_SIZE, 1,
     FXMESA_DEPTH_SIZE, 16,
     FXMESA_STENCIL_SIZE, 0,
     FXMESA_ACCUM_SIZE, 0,
     FXMESA_NONE}
   }
   ,
   /* 16bit ARGB1555 double buffer with depth */
   {
    {sizeof(PIXELFORMATDESCRIPTOR), 1,
     PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL |
     PFD_DOUBLEBUFFER | PFD_SWAP_COPY,
     PFD_TYPE_RGBA,
     16,
     5, 0, 5, 5, 5, 10, 1, 15,
     0, 0, 0, 0, 0,
     16,
     0,
     0,
     PFD_MAIN_PLANE,
     0, 0, 0, 0}
    ,
    {FXMESA_COLORDEPTH, 15,
     FXMESA_DOUBLEBUFFER,
     FXMESA_ALPHA_SIZE, 1,
     FXMESA_DEPTH_SIZE, 16,
     FXMESA_STENCIL_SIZE, 0,
     FXMESA_ACCUM_SIZE, 0,
     FXMESA_NONE}
   }
   ,
   /* 32bit ARGB8888 single buffer with depth */
   {
    {sizeof(PIXELFORMATDESCRIPTOR), 1,
     PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL,
     PFD_TYPE_RGBA,
     32,
     8, 0, 8, 8, 8, 16, 8, 24,
     0, 0, 0, 0, 0,
     24,
     8,
     0,
     PFD_MAIN_PLANE,
     0, 0, 0, 0}
    ,
    {FXMESA_COLORDEPTH, 32,
     FXMESA_ALPHA_SIZE, 8,
     FXMESA_DEPTH_SIZE, 24,
     FXMESA_STENCIL_SIZE, 8,
     FXMESA_ACCUM_SIZE, 0,
     FXMESA_NONE}
   }
   ,
   /* 32bit ARGB8888 double buffer with depth */
   {
    {sizeof(PIXELFORMATDESCRIPTOR), 1,
     PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL |
     PFD_DOUBLEBUFFER | PFD_SWAP_COPY,
     PFD_TYPE_RGBA,
     32,
     8, 0, 8, 8, 8, 16, 8, 24,
     0, 0, 0, 0, 0,
     24,
     8,
     0,
     PFD_MAIN_PLANE,
     0, 0, 0, 0}
    ,
    {FXMESA_COLORDEPTH, 32,
     FXMESA_DOUBLEBUFFER,
     FXMESA_ALPHA_SIZE, 8,
     FXMESA_DEPTH_SIZE, 24,
     FXMESA_STENCIL_SIZE, 8,
     FXMESA_ACCUM_SIZE, 0,
     FXMESA_NONE}
   }
};

static fxMesaContext ctx = NULL;
static WNDPROC hWNDOldProc;
static int curPFD = 0;
static HDC hDC;
static HWND hWND;


/* [retro3dfx] One-time "the game loaded THIS ICD" banner, written to
 * C:\retrogl.log the first time GoldSrc (or any GL app) calls any of our
 * entry points. Proves the game bound to our opengl32 and not the Microsoft
 * software GL wrapper, and records which glide3x.dll the loader resolved our
 * grFoo imports against (the usual .124 failure is an underscore-decoration
 * mismatch that makes LoadLibrary fall back to software GL entirely). */
static void rgl_first_entry(const char *fn)
{
   static int done = 0;
   char path[MAX_PATH];
   HMODULE hg;
   if (done)
      return;
   done = 1;
   rgl_log("==================================================================");
   rgl_log("retrogl ICD attached: first entry via %s (pid=%lu)",
           fn, (unsigned long)GetCurrentProcessId());
   if (GetModuleFileNameA(NULL, path, sizeof(path)))
      rgl_log("  host process : %s", path);
   /* which glide3x.dll got loaded (our grFoo imports bind to it) */
   hg = GetModuleHandleA("glide3x.dll");
   if (!hg)
      hg = GetModuleHandleA("glide3x");
   if (hg && GetModuleFileNameA(hg, path, sizeof(path)))
      rgl_log("  glide3x.dll  : %s (module=%p)", path, (void *)hg);
   else
      rgl_log("  glide3x.dll  : NOT LOADED (GetModuleHandle failed)");
}

/* [retro3dfx] DllMain fires the instant ANY app LoadLibrary's us — before the
 * app calls a single GL entry point. This distinguishes "GoldSrc never loaded
 * our MiniGL" (no line) from "loaded it but failed before calling GL" (line
 * present, but no rgl_first_entry banner). Critical for the CS/GoldSrc path,
 * which resolves exports via kernel32 GetProcAddress (invisible to our per-call
 * tracer). */
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID reserved)
{
   (void) hInst;
   if (reason == DLL_PROCESS_ATTACH) {
      char path[MAX_PATH];
      if (!GetModuleFileNameA(NULL, path, sizeof(path)))
         path[0] = '\0';
      rgl_log("### DllMain PROCESS_ATTACH: retrogl LOADED by '%s' (pid=%lu) ###",
              path, (unsigned long) GetCurrentProcessId());
   } else if (reason == DLL_PROCESS_DETACH) {
      /* [retro3dfx] 0.1.67: reserved != NULL means the process is exiting;
       * NULL means FreeLibrary (an engine's renderer restart). Its ABSENCE
       * after a run means the process was terminated, not exited. */
      rgl_log("### DllMain PROCESS_DETACH: retrogl %s (pid=%lu) ###",
              reserved ? "process exit" : "FreeLibrary",
              (unsigned long) GetCurrentProcessId());
   }
   return TRUE;
}


static int env_check (const char *var, int val)
{
 const char *env = getenv(var);
 return (env && (env[0] == val));
}

static LRESULT WINAPI
__wglMonitor(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
   LRESULT  ret;		/* Now gives the resized window at the end to hWNDOldProc */

   if (ctx && hwnd == hWND) {
     switch (message) {
       case WM_PAINT:
       case WM_MOVE:
	   break;
       case WM_DISPLAYCHANGE:
       case WM_SIZE:
	   break;
       case WM_ACTIVATE:
	   break;
       case WM_SHOWWINDOW:
	   break;
       case WM_SYSKEYDOWN:
       case WM_SYSCHAR:
	   break;
     }
   }

   /* Finaly call the hWNDOldProc, which handles the resize witch the
      now changed window sizes */
   ret = CallWindowProc(hWNDOldProc, hwnd, message, wParam, lParam);

   return (ret);
}

static void wgl_error (long error)
{
#define WGL_INVALID_PIXELFORMAT ERROR_INVALID_PIXEL_FORMAT
 SetLastError(0xC0000000 /* error severity */
             |0x00070000 /* error facility (who we are) */
             |error);
}

GLAPI BOOL GLAPIENTRY
wglCopyContext(HGLRC hglrcSrc, HGLRC hglrcDst, UINT mask)
{
   return (FALSE);
}

GLAPI HGLRC GLAPIENTRY
wglCreateContext(HDC hdc)
{
   HWND hWnd;
   WNDPROC oldProc;
   int error;

   rgl_first_entry("wglCreateContext");
   rgl_log("wglCreateContext(hdc=%p) enter [curPFD=%d]", (void *)hdc, curPFD);

   if (ctx) {
      rgl_log("wglCreateContext: BAIL -> NULL (a context already exists)");
      SetLastError(0);
      return (NULL);
   }

   if (!(hWnd = WindowFromDC(hdc))) {
      rgl_log("wglCreateContext: BAIL -> NULL (WindowFromDC(hdc) == NULL, no window for DC)");
      SetLastError(0);
      return (NULL);
   }

   if (curPFD == 0) {
      rgl_log("wglCreateContext: BAIL -> NULL (curPFD==0: SetPixelFormat never took)");
      wgl_error(WGL_INVALID_PIXELFORMAT);
      return (NULL);
   }

   if ((oldProc = (WNDPROC) GetWindowLong(hWnd, GWL_WNDPROC)) != __wglMonitor) {
      hWNDOldProc = oldProc;
      SetWindowLong(hWnd, GWL_WNDPROC, (LONG) __wglMonitor);
   }

   /* always log when debugging, or if user demands */
   if (TDFX_DEBUG || env_check("MESA_FX_INFO", 'r')) {
      /* [retro3dfx] Write to a guaranteed-writable temp path, not the process
       * CWD: under Program Files on XP the CWD is read-only for a limited user,
       * and a failed freopen() CLOSES stderr per the C standard — every later
       * fprintf(stderr,...) then writes to a dead stream. Build %TEMP%\MESA.LOG
       * and only redirect if it actually opened. See REVIEW-FINDINGS.md B1. */
      char logpath[MAX_PATH];
      DWORD n = GetTempPathA(sizeof(logpath), logpath);
      if (n == 0 || n >= sizeof(logpath) - 12) {
         logpath[0] = '\0';
      }
      strcat(logpath, "MESA.LOG");
      if (!freopen(logpath, "w", stderr)) {
         /* fall back to the game dir; if that also fails, leave stderr intact */
         freopen("MESA.LOG", "w", stderr);
      }
   }

   {
     RECT cliRect;
     MSG pumpMsg;
     int pumpI;
     ShowWindow(hWnd, SW_SHOWNORMAL);
     SetForegroundWindow(hWnd);
     SetActiveWindow(hWnd);
     Sleep(100); /* a hack for win95 */
     /* [retro3dfx] Drain the window's pending messages before Glide takes the
      * board. A freshly-shown window (idTech2 ref_gl) still has its activation
      * messages queued; grSstWinOpen's DirectDraw SetCooperativeLevel(EXCLUSIVE
      * | FULLSCREEN) blocks until the window is really foreground/active, so if
      * the game thread enters grSstWinOpen without pumping, it deadlocks. Q3's
      * window is already settled, which is why it never hit this. Pump so the
      * WM_ACTIVATE/WM_SETFOCUS get dispatched and the window is truly active. */
     /* [retro3dfx 0.1.65] The inner loop was unbounded, and WM_PAINT is not a
      * queued message: Windows synthesises it for as long as the window has an
      * update region. Loaded as the SYSTEM ICD, Microsoft's opengl32 subclasses
      * the window too, and under SDL (ioquake3) the region never cleared - the
      * thread sat in this loop forever inside wglCreateContext (ntsd on .124,
      * 2026-09-24: DispatchMessageA(WM_PAINT) -> __wglMonitor -> opengl32 hook
      * -> SDL WndProc -> EndPaint, every time). The pump exists only to let the
      * ACTIVATION messages through (idTech2 ref_gl deadlock, see above), so:
      * validate WM_PAINT instead of dispatching it (the game repaints every
      * frame anyway), cap the total, and never swallow a WM_QUIT. */
     /* [retro3dfx 0.1.76] ...and the WM_PAINT branch was itself unbounded: a
      * paint that ValidateRect does not end (Unreal Tournament's OpenGLDrv
      * viewport on .124, 2026-09-28) spun this loop 87,500,081 times - 195 s
      * of a 220 s startup, and on another window it could be forever. So a
      * validated paint counts against the pump's budget too: after
      * RGL_PUMP_PAINTS of them, stop pumping (the activation messages this
      * exists for arrive first, and the game repaints every frame anyway). */
     {
#define RGL_PUMP_PAINTS 64
        int dispatched = 0, painted = 0, quit = 0;
        for (pumpI = 0; pumpI < 40 && dispatched < 256 && painted < RGL_PUMP_PAINTS && !quit;
             pumpI++) {
           while (dispatched < 256 && painted < RGL_PUMP_PAINTS &&
                  PeekMessage(&pumpMsg, NULL, 0, 0, PM_REMOVE)) {
              if (pumpMsg.message == WM_QUIT) {
                 PostQuitMessage((int) pumpMsg.wParam);   /* hand it back */
                 quit = 1;
                 break;
              }
              if (pumpMsg.message == WM_PAINT) {
                 ValidateRect(pumpMsg.hwnd, NULL);
                 painted++;
                 continue;
              }
              TranslateMessage(&pumpMsg);
              DispatchMessage(&pumpMsg);
              dispatched++;
           }
           Sleep(5);
        }
        rgl_log("wglCreateContext: activation pump dispatched=%d paints-validated=%d quit=%d",
                dispatched, painted, quit);
     }
        GetClientRect(hWnd, &cliRect);
        rgl_log("wglCreateContext: hWnd=%p GetClientRect=%ldx%ld curPFD=%d "
                "pfd.cColorBits=%d mesaColDepth=%d (this WxH feeds the resolution snapper)",
                (void *)hWnd, (long)cliRect.right, (long)cliRect.bottom, curPFD,
                (int)pix[curPFD - 1].pfd.cColorBits, (int)pix[curPFD - 1].mesaAttr[1]);
        /* [retro3dfx] Opt-in windowed-Glide rendering (FX_WINDOWED=1, or the
         * legacy MESA_GLX_FX=window). Needed by desktop-fullscreen engines
         * (GoldSrc/Half-Life/CS) whose own ChangeDisplaySettings collides with
         * grSstWinOpen's exclusive mode. Request it here; fxMesaCreateContext
         * uses the DDraw offscreen+Blt path and silently falls back to the
         * fullscreen path if the surface API isn't available, so Q2/Q3/UT (which
         * don't set the env) are unaffected. */
        if (env_check("FX_WINDOWED", '1') || env_check("MESA_GLX_FX", 'w')) {
           rgl_log("wglCreateContext: FX_WINDOWED/MESA_GLX_FX set -> requesting windowed Glide %ldx%ld",
                   (long)cliRect.right, (long)cliRect.bottom);
           fxMesaRequestWindowed(cliRect.right, cliRect.bottom);
        }
        if (TDFX_DEBUG & VERBOSE_DRIVER)
           fprintf(stderr, "[retro3dfx] pre create hWnd=%p cliRect=%ldx%ld style=%lx attr0=%d\n",
                   (void*)hWnd, (long)cliRect.right, (long)cliRect.bottom,
                   (unsigned long)GetWindowLong(hWnd, GWL_STYLE), (int)pix[curPFD-1].mesaAttr[0]);
        rgl_log("wglCreateContext: -> fxMesaCreateBestContext(win=%p, %ldx%ld, mesaAttr[colDepth=%d])",
                (void *)hWnd, (long)cliRect.right, (long)cliRect.bottom,
                (int)pix[curPFD - 1].mesaAttr[1]);
        error = !(ctx = fxMesaCreateBestContext((GLuint) hWnd, cliRect.right, cliRect.bottom, pix[curPFD - 1].mesaAttr));
        rgl_log("wglCreateContext: fxMesaCreateBestContext returned ctx=%p (error=%d)", (void *)ctx, error);
        if (TDFX_DEBUG & VERBOSE_DRIVER)
           fprintf(stderr, "[retro3dfx] post create ctx=%p\n", (void*)ctx);
   }

   if (error) {
      rgl_log("wglCreateContext: BAIL -> NULL (context creation failed; app falls back to software GL)");
      SetLastError(0);
      return (NULL);
   }

   hDC = hdc;
   hWND = hWnd;

   /* Required by the OpenGL Optimizer 1.1 (is it a Optimizer bug ?) */
   wglMakeCurrent(hdc, (HGLRC) 1);

   rgl_log("wglCreateContext: SUCCESS -> HGLRC 1 (ctx=%p)", (void *)ctx);
   return ((HGLRC) 1);
}

GLAPI HGLRC GLAPIENTRY
wglCreateLayerContext(HDC hdc, int iLayerPlane)
{
   SetLastError(0);
   return (NULL);
}

GLAPI BOOL GLAPIENTRY
wglDeleteContext(HGLRC hglrc)
{
   if (ctx && hglrc == (HGLRC) 1) {
      rgl_log("wglDeleteContext: enter");

      fxMesaDestroyContext(ctx);

      SetWindowLong(WindowFromDC(hDC), GWL_WNDPROC, (LONG) hWNDOldProc);

      ctx = NULL;
      hDC = 0;
      rgl_log("wglDeleteContext: done");
      return (TRUE);
   }

   SetLastError(0);

   return (FALSE);
}

GLAPI HGLRC GLAPIENTRY
wglGetCurrentContext(VOID)
{
   if (ctx)
      return ((HGLRC) 1);

   SetLastError(0);
   return (NULL);
}

GLAPI HDC GLAPIENTRY
wglGetCurrentDC(VOID)
{
   if (ctx)
      return (hDC);

   SetLastError(0);
   return (NULL);
}

GLAPI BOOL GLAPIENTRY
wglSwapIntervalEXT (int interval)
{
 if (ctx == NULL) {
    return FALSE;
 }
 if (interval < 0) {
    interval = 0;
 } else if (interval > 3) {
    interval = 3;
 }
 ctx->swapInterval = interval;
 return TRUE;
}

GLAPI int GLAPIENTRY
wglGetSwapIntervalEXT (void)
{
 return (ctx == NULL) ? -1 : ctx->swapInterval;
}

GLAPI BOOL GLAPIENTRY
wglGetDeviceGammaRamp3DFX (HDC hdc, LPVOID arrays)
{
 /* gammaTable should be per-context */
 if (!gammaTableKnown) {
    /* no ramp loaded by us or the game yet: report identity, never zeros */
    GLint i;
    for (i = 0; i < 256; i++)
       gammaTable[i] = gammaTable[256 + i] = gammaTable[512 + i] = (GLushort)(i * 0x101);
    gammaTableKnown = GL_TRUE;
 }
 memcpy(arrays, gammaTable, 3*256*sizeof(GLushort));
 return TRUE;
}

GLAPI BOOL GLAPIENTRY
wglSetDeviceGammaRamp3DFX (HDC hdc, LPVOID arrays)
{
 GLint i, tableSize, inc, index;
 GLushort *red, *green, *blue;
 FxU32 gammaTableR[256], gammaTableG[256], gammaTableB[256];

 /* gammaTable should be per-context */
 memcpy(gammaTable, arrays, 3*256*sizeof(GLushort));
 gammaTableKnown = GL_TRUE;

 tableSize = FX_grGetInteger(GR_GAMMA_TABLE_ENTRIES);
 inc = 256 / tableSize;
 red = (GLushort *)arrays;
 green = (GLushort *)arrays + 256;
 blue = (GLushort *)arrays + 512;
 for (i = 0, index = 0; i < tableSize; i++, index += inc) {
     gammaTableR[i] = red[index] >> 8;
     gammaTableG[i] = green[index] >> 8;
     gammaTableB[i] = blue[index] >> 8;
 }

 grLoadGammaTable(tableSize, gammaTableR, gammaTableG, gammaTableB);

 return TRUE;
}

typedef void *HPBUFFERARB;

/* WGL_ARB_pixel_format */
GLAPI BOOL GLAPIENTRY
wglGetPixelFormatAttribivARB (HDC hdc,
			      int iPixelFormat,
			      int iLayerPlane,
			      UINT nAttributes,
			      const int *piAttributes,
			      int *piValues)
{
  SetLastError(0);
  return(FALSE);
}

GLAPI BOOL GLAPIENTRY
wglGetPixelFormatAttribfvARB (HDC hdc,
			      int iPixelFormat,
			      int iLayerPlane,
			      UINT nAttributes,
			      const int *piAttributes,
			      FLOAT *pfValues)
{
  SetLastError(0);
  return(FALSE);
}

GLAPI BOOL GLAPIENTRY
wglChoosePixelFormatARB (HDC hdc,
			 const int *piAttribIList,
			 const FLOAT *pfAttribFList,
			 UINT nMaxFormats,
			 int *piFormats,
			 UINT *nNumFormats)
{
  SetLastError(0);
  return(FALSE);
}

/* WGL_ARB_render_texture */
GLAPI BOOL GLAPIENTRY
wglBindTexImageARB (HPBUFFERARB hPbuffer, int iBuffer)
{
  SetLastError(0);
  return(FALSE);
}

GLAPI BOOL GLAPIENTRY
wglReleaseTexImageARB (HPBUFFERARB hPbuffer, int iBuffer)
{
  SetLastError(0);
  return(FALSE);
}

GLAPI BOOL GLAPIENTRY
wglSetPbufferAttribARB (HPBUFFERARB hPbuffer,
			const int *piAttribList)
{
  SetLastError(0);
  return(FALSE);
}

/* WGL_ARB_pbuffer */
GLAPI HPBUFFERARB GLAPIENTRY
wglCreatePbufferARB (HDC hDC,
		     int iPixelFormat,
		     int iWidth,
		     int iHeight,
		     const int *piAttribList)
{
  SetLastError(0);
  return NULL;
}

GLAPI HDC GLAPIENTRY
wglGetPbufferDCARB (HPBUFFERARB hPbuffer)
{
  SetLastError(0);
  return NULL;
}

GLAPI int GLAPIENTRY
wglReleasePbufferDCARB (HPBUFFERARB hPbuffer, HDC hDC)
{
  SetLastError(0);
  return -1;
}

GLAPI BOOL GLAPIENTRY
wglDestroyPbufferARB (HPBUFFERARB hPbuffer)
{
  SetLastError(0);
  return(FALSE);
}

GLAPI BOOL GLAPIENTRY
wglQueryPbufferARB (HPBUFFERARB hPbuffer,
		    int iAttribute,
		    int *piValue)
{
  SetLastError(0);
  return(FALSE);
}

GLAPI const char * GLAPIENTRY
wglGetExtensionsStringEXT (void)
{
 return "WGL_3DFX_gamma_control "
        "WGL_EXT_swap_control "
        "WGL_EXT_extensions_string WGL_ARB_extensions_string"
        /*WGL_ARB_pixel_format WGL_ARB_render_texture WGL_ARB_pbuffer*/;
}

GLAPI const char * GLAPIENTRY
wglGetExtensionsStringARB (HDC hdc)
{
 return wglGetExtensionsStringEXT();
}

/* ---- GL_SGIS_multitexture shim -------------------------------------------
 * Quake II resolves glSelectTextureSGIS / glMTexCoord2fSGIS through
 * wglGetProcAddress once it sees GL_SGIS_multitexture in GL_EXTENSIONS.
 * SGIS_multitexture is a strict subset of ARB_multitexture; the only
 * difference that matters is the texture-unit enum base, so translate it.
 *   GL_TEXTURE0_SGIS = 0x835E, GL_TEXTURE0_ARB = 0x84C0
 */
#define GL_TEXTURE0_SGIS 0x835E

static void APIENTRY fx_glSelectTextureSGIS(GLenum target)
{
   /* SGIS_multitexture has ONE selector: it switches the unit for immediate-mode
    * state AND for array pointers. ARB split that into glActiveTexture (server)
    * and glClientActiveTexture (client), so a faithful shim must set BOTH.
    * Quake II enables GL_EXT_compiled_vertex_array and issues glTexCoordPointer
    * for unit 1 after selecting it -- with only the server unit switched those
    * pointers land on unit 0 and the multitexture path degenerates. */
   GLenum unit = (GLenum)(GL_TEXTURE0_ARB + (target - GL_TEXTURE0_SGIS));
   glActiveTextureARB(unit);
   /* _mesa_ClientActiveTextureARB has NO early-out and does an unconditional
    * FLUSH_VERTICES(_NEW_ARRAY) -- unlike _mesa_ActiveTextureARB, which returns
    * early when the unit is unchanged. An app that selects a unit per surface
    * therefore eats a vertex flush + full array revalidation per surface even
    * when it draws in immediate mode and has no arrays bound.
    * FX_SGIS_NO_CLIENTTEX=1 skips it, to attribute that cost.
    *
    * [retro3dfx] 0.1.72: read that variable ONCE. It was read on every call,
    * and Quake II selects a unit twice per surface: XP msvcrt's getenv is
    * locale-aware (MultiByteToWideChar, CompareStringA, GetVersionExW per
    * call), and the ICD profiler put the env/locale functions at ~27 % of a
    * single-pass Quake II frame on the V5 6000 - the largest cost left once
    * 0.1.71 fixed the lightmap re-downloads. */
   static int noClientTex = -1;
   if (noClientTex < 0)
      noClientTex = getenv("FX_SGIS_NO_CLIENTTEX") != NULL;
   if (!noClientTex) {
      glClientActiveTextureARB(unit);
   }
}

static void APIENTRY fx_glMTexCoord2fSGIS(GLenum target, GLfloat s, GLfloat t)
{
   glMultiTexCoord2fARB((GLenum)(GL_TEXTURE0_ARB + (target - GL_TEXTURE0_SGIS)), s, t);
}

static void APIENTRY fx_glMTexCoord2fvSGIS(GLenum target, const GLfloat *v)
{
   glMultiTexCoord2fvARB((GLenum)(GL_TEXTURE0_ARB + (target - GL_TEXTURE0_SGIS)), v);
}

static struct {
       const char *name;
       PROC func;
} wgl_ext[] = {
       /* GL_SGIS_multitexture (Quake II and other 1997-era GL apps) */
       {"glSelectTextureSGIS",          (PROC)fx_glSelectTextureSGIS},
       {"glMTexCoord2fSGIS",            (PROC)fx_glMTexCoord2fSGIS},
       {"glMTexCoord2fvSGIS",           (PROC)fx_glMTexCoord2fvSGIS},
       {"wglGetExtensionsStringARB",    (PROC)wglGetExtensionsStringARB},
       {"wglGetExtensionsStringEXT",    (PROC)wglGetExtensionsStringEXT},
       {"wglSwapIntervalEXT",           (PROC)wglSwapIntervalEXT},
       {"wglGetSwapIntervalEXT",        (PROC)wglGetSwapIntervalEXT},
       {"wglGetDeviceGammaRamp3DFX",    (PROC)wglGetDeviceGammaRamp3DFX},
       {"wglSetDeviceGammaRamp3DFX",    (PROC)wglSetDeviceGammaRamp3DFX},
       /* WGL_ARB_pixel_format */
       {"wglGetPixelFormatAttribivARB", (PROC)wglGetPixelFormatAttribivARB},
       {"wglGetPixelFormatAttribfvARB", (PROC)wglGetPixelFormatAttribfvARB},
       {"wglChoosePixelFormatARB",      (PROC)wglChoosePixelFormatARB},
       /* WGL_ARB_render_texture */
       {"wglBindTexImageARB",           (PROC)wglBindTexImageARB},
       {"wglReleaseTexImageARB",        (PROC)wglReleaseTexImageARB},
       {"wglSetPbufferAttribARB",       (PROC)wglSetPbufferAttribARB},
       /* WGL_ARB_pbuffer */
       {"wglCreatePbufferARB",          (PROC)wglCreatePbufferARB},
       {"wglGetPbufferDCARB",           (PROC)wglGetPbufferDCARB},
       {"wglReleasePbufferDCARB",       (PROC)wglReleasePbufferDCARB},
       {"wglDestroyPbufferARB",         (PROC)wglDestroyPbufferARB},
       {"wglQueryPbufferARB",           (PROC)wglQueryPbufferARB},
       {NULL, NULL}
};

GLAPI PROC GLAPIENTRY
wglGetProcAddress(LPCSTR lpszProc)
{
   int i;
   PROC p;
   rgl_first_entry("wglGetProcAddress");

   /* [retro3dfx] OUR table MUST be consulted BEFORE Mesa's glapi.
    *
    * _glapi_get_proc_address() SYNTHESIZES a dispatch stub for any unrecognised
    * "gl*" name rather than failing, so it answers for e.g. glSelectTextureSGIS
    * with a stub that is not wired to anything -- and the loop below, which
    * holds the real implementation, was never reached. Quake II then called the
    * synthesized stub the moment multitexture engaged. Found on .171
    * (Voodoo 2) 2026-08-29: the demo1 timedemo stopped completing at all.
    * Names we implement ourselves win; everything else still falls through to
    * glapi exactly as before. */
   for (i = 0; wgl_ext[i].name; i++) {
       if (!strcmp(lpszProc, wgl_ext[i].name)) {
          return wgl_ext[i].func;
       }
   }

   /* [retro3dfx] 0.1.78: a name glapi holds no dispatch slot for is a
    * function this ICD does not implement - answer NULL. Asked for it,
    * _glapi_get_proc_address() SYNTHESIZES a stub at dispatch offset ~0
    * ("If that never happens, and the user calls this function, he'll
    * segfault" - its own comment), and an application that trusts a
    * non-NULL pointer calls straight into it. WON Half-Life's hw.dll does
    * exactly that with glPNTrianglesiATI (ATI TruForm, enums 0x87F0-0x87F7),
    * which we do not advertise: Deathmatch Classic on .124 (V5 6000,
    * 2026-09-28) died with eip 0x010c01d9 - a heap stub - called from
    * hw.dll+0x76d3c. Every function we do implement has an offset (the
    * static table, or _glapi_add_entrypoint at context creation). */
   if (_glapi_get_proc_offset((const char *) lpszProc) < 0) {
      SetLastError(0);
      return (NULL);
   }

   p = (PROC) _glapi_get_proc_address((const char *) lpszProc);

   /* we can't BlendColor. work around buggy applications */
   if (p && strcmp(lpszProc, "glBlendColor") && strcmp(lpszProc, "glBlendColorEXT"))
      return p;

   SetLastError(0);
   return (NULL);
}

GLAPI PROC GLAPIENTRY
wglGetDefaultProcAddress(LPCSTR lpszProc)
{ 
   SetLastError(0);
   return (NULL);
}

GLAPI BOOL GLAPIENTRY
wglMakeCurrent(HDC hdc, HGLRC hglrc)
{
   if ((hdc == NULL) && (hglrc == NULL))
      return (TRUE);

   if (!ctx || hglrc != (HGLRC) 1 || WindowFromDC(hdc) != hWND) {
      SetLastError(0);
      return (FALSE);
   }

   hDC = hdc;

   fxMesaMakeCurrent(ctx);
   fxProfStart();   /* [retro3dfx] 0.1.67: no-op unless RETROGL_PROF is set */

   return (TRUE);
}

GLAPI BOOL GLAPIENTRY
wglShareLists(HGLRC hglrc1, HGLRC hglrc2)
{
   if (!ctx || hglrc1 != (HGLRC) 1 || hglrc1 != hglrc2) {
      SetLastError(0);
      return (FALSE);
   }

   return (TRUE);
}

static BOOL
wglUseFontBitmaps_FX(HDC fontDevice, DWORD firstChar, DWORD numChars,
		     DWORD listBase)
{
   TEXTMETRIC metric;
   BITMAPINFO *dibInfo;
   HDC bitDevice;
   COLORREF tempColor;
   int i;

   GetTextMetrics(fontDevice, &metric);

   dibInfo = (BITMAPINFO *) calloc(sizeof(BITMAPINFO) + sizeof(RGBQUAD), 1);
   dibInfo->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
   dibInfo->bmiHeader.biPlanes = 1;
   dibInfo->bmiHeader.biBitCount = 1;
   dibInfo->bmiHeader.biCompression = BI_RGB;

   bitDevice = CreateCompatibleDC(fontDevice);

   /* Swap fore and back colors so the bitmap has the right polarity */
   tempColor = GetBkColor(bitDevice);
   SetBkColor(bitDevice, GetTextColor(bitDevice));
   SetTextColor(bitDevice, tempColor);

   /* Place chars based on base line */
   SetTextAlign(bitDevice, TA_BASELINE);

   for (i = 0; i < (int)numChars; i++) {
      SIZE size;
      char curChar;
      int charWidth, charHeight, bmapWidth, bmapHeight, numBytes, res;
      HBITMAP bitObject;
      HGDIOBJ origBmap;
      unsigned char *bmap;

      curChar = (char)(i + firstChar); /* [koolsmoky] explicit cast */

      /* Find how high/wide this character is */
      GetTextExtentPoint32(bitDevice, &curChar, 1, &size);
 
      /* Create the output bitmap */
      charWidth = size.cx;
      charHeight = size.cy;
      bmapWidth = ((charWidth + 31) / 32) * 32;	/* Round up to the next multiple of 32 bits */
      bmapHeight = charHeight;
      bitObject = CreateCompatibleBitmap(bitDevice, bmapWidth, bmapHeight);
      /*VERIFY(bitObject);*/

      /* Assign the output bitmap to the device */
      origBmap = SelectObject(bitDevice, bitObject);

      PatBlt(bitDevice, 0, 0, bmapWidth, bmapHeight, BLACKNESS);

      /* Use our source font on the device */
      SelectObject(bitDevice, GetCurrentObject(fontDevice, OBJ_FONT));

      /* Draw the character */
      TextOut(bitDevice, 0, metric.tmAscent, &curChar, 1);

      /* Unselect our bmap object */
      SelectObject(bitDevice, origBmap);

      /* Convert the display dependant representation to a 1 bit deep DIB */
      numBytes = (bmapWidth * bmapHeight) / 8;
      bmap = MALLOC(numBytes);
      dibInfo->bmiHeader.biWidth = bmapWidth;
      dibInfo->bmiHeader.biHeight = bmapHeight;
      res = GetDIBits(bitDevice, bitObject, 0, bmapHeight, bmap,
		      dibInfo, DIB_RGB_COLORS);

      /* Create the GL object */
      glNewList(i + listBase, GL_COMPILE);
      glBitmap(bmapWidth, bmapHeight, 0.0, metric.tmDescent,
	       charWidth, 0.0, bmap);
      glEndList();
      /* CheckGL(); */

      /* Destroy the bmap object */
      DeleteObject(bitObject);

      /* Deallocate the bitmap data */
      FREE(bmap);
   }

   /* Destroy the DC */
   DeleteDC(bitDevice);

   FREE(dibInfo);

   return TRUE;
}

GLAPI BOOL GLAPIENTRY
wglUseFontBitmapsW(HDC hdc, DWORD first, DWORD count, DWORD listBase)
{
   /* [retro3dfx] Delegate to the working ANSI implementation. Unicode-compiled
    * engines call the W export for console/HUD text; returning FALSE left their
    * display lists empty (missing text, or a crash when glCallLists hits unset
    * lists). The GDI glyph path is identical for both. See REVIEW-FINDINGS.md B1. */
   return wglUseFontBitmapsA(hdc, first, count, listBase);
}

GLAPI BOOL GLAPIENTRY
wglUseFontOutlinesA(HDC hdc, DWORD first, DWORD count,
		    DWORD listBase, FLOAT deviation,
		    FLOAT extrusion, int format, LPGLYPHMETRICSFLOAT lpgmf)
{
   SetLastError(0);
   return (FALSE);
}

GLAPI BOOL GLAPIENTRY
wglUseFontOutlinesW(HDC hdc, DWORD first, DWORD count,
		    DWORD listBase, FLOAT deviation,
		    FLOAT extrusion, int format, LPGLYPHMETRICSFLOAT lpgmf)
{
   SetLastError(0);
   return (FALSE);
}


GLAPI BOOL GLAPIENTRY
wglSwapLayerBuffers(HDC hdc, UINT fuPlanes)
{
   if (ctx && WindowFromDC(hdc) == hWND) {
      fxMesaSwapBuffers();

      return (TRUE);
   }

   SetLastError(0);
   return (FALSE);
}

static int pfd_tablen (void)
{
 /* we should take an envvar for `fxMesaSelectCurrentBoard' */
 /* [retro3dfx] Pre-Napalm (Voodoo3/Banshee) has no 32-bit framebuffer, so the
  * two 32-bit ARGB8888 entries (pix[4]/pix[5]) stay hidden — but it DOES support
  * the 16-bit ARGB1555 formats (pix[2]/pix[3]). Exposing them (4, not 2) lets
  * games that request an alpha buffer (SDL/GLFW default to 8 destination-alpha
  * bits; the matcher hard-rejects cAlphaBits>0 with no alpha PFD) actually create
  * a context instead of failing outright. See retro3dfx/REVIEW-FINDINGS.md A1. */
 return (fxMesaSelectCurrentBoard(0) < GR_SSTTYPE_Voodoo4)
        ? 4                               /* RGB565 + ARGB1555 (alpha) 16bit entries */
        : sizeof(pix) / sizeof(pix[0]);   /* full table */
}

GLAPI int GLAPIENTRY
wglChoosePixelFormat(HDC hdc, const PIXELFORMATDESCRIPTOR * ppfd)
{
   int i, best = -1, qt_valid_pix;
   PIXELFORMATDESCRIPTOR pfd = *ppfd;

   rgl_first_entry("wglChoosePixelFormat");
   rgl_log("wglChoosePixelFormat: REQUEST cColorBits=%d cDepthBits=%d cAlphaBits=%d "
           "cStencilBits=%d iPixelType=%d dwFlags=0x%lx",
           (int)ppfd->cColorBits, (int)ppfd->cDepthBits, (int)ppfd->cAlphaBits,
           (int)ppfd->cStencilBits, (int)ppfd->iPixelType, (unsigned long)ppfd->dwFlags);

   qt_valid_pix = pfd_tablen();

#if 1 || QUAKE2 || GORE
  /* QUAKE2: 24+32 */
  /* GORE  : 24+16 */
  if ((pfd.cColorBits == 24) || (pfd.cColorBits == 32)) {
     /* [retro3dfx] Remap a 24/32-bit request to what the card actually has:
      * 32-bit only on Napalm (Voodoo4/5), else 16-bit. This used to test
      * `qt_valid_pix > 2`, but once we expose the V3 alpha PFDs (tablen 2->4)
      * that heuristic wrongly reports "has 32-bit" on a Voodoo3 and remaps Q2's
      * request to a nonexistent 32-bit format. Test the board type directly. */
     pfd.cColorBits = (fxMesaSelectCurrentBoard(0) >= GR_SSTTYPE_Voodoo4) ? 32 : 16;
  }
  if (pfd.cColorBits == 32) {
     pfd.cDepthBits = 24;
  } else if (pfd.cColorBits == 16) {
     pfd.cDepthBits = 16;
  }
#endif

   rgl_log("wglChoosePixelFormat: after card remap cColorBits=%d cDepthBits=%d (tablen=%d formats)",
           (int)pfd.cColorBits, (int)pfd.cDepthBits, qt_valid_pix);

   if (pfd.nSize != sizeof(PIXELFORMATDESCRIPTOR) || pfd.nVersion != 1) {
      rgl_log("wglChoosePixelFormat: BAIL -> 0 (bad nSize=%d/nVersion=%d)",
              (int)pfd.nSize, (int)pfd.nVersion);
      SetLastError(0);
      return (0);
   }

   for (i = 0; i < qt_valid_pix; i++) {
      if (pfd.cColorBits > 0 && pix[i].pfd.cColorBits != pfd.cColorBits) 
		  continue;

      if ((pfd.dwFlags & PFD_DRAW_TO_WINDOW)
	  && !(pix[i].pfd.dwFlags & PFD_DRAW_TO_WINDOW)) continue;
      if ((pfd.dwFlags & PFD_DRAW_TO_BITMAP)
	  && !(pix[i].pfd.dwFlags & PFD_DRAW_TO_BITMAP)) continue;
      if ((pfd.dwFlags & PFD_SUPPORT_GDI)
	  && !(pix[i].pfd.dwFlags & PFD_SUPPORT_GDI)) continue;
      if ((pfd.dwFlags & PFD_SUPPORT_OPENGL)
	  && !(pix[i].pfd.dwFlags & PFD_SUPPORT_OPENGL)) continue;
      if (!(pfd.dwFlags & PFD_DOUBLEBUFFER_DONTCARE)
	  && ((pfd.dwFlags & PFD_DOUBLEBUFFER) !=
	      (pix[i].pfd.dwFlags & PFD_DOUBLEBUFFER))) continue;
#if 1 /* Doom3 fails here! */
      if (!(pfd.dwFlags & PFD_STEREO_DONTCARE)
	  && ((pfd.dwFlags & PFD_STEREO) !=
	      (pix[i].pfd.dwFlags & PFD_STEREO))) continue;
#endif

      if (pfd.cDepthBits > 0 && pix[i].pfd.cDepthBits == 0)
	 continue;		/* need depth buffer */

      if (pfd.cAlphaBits > 0 && pix[i].pfd.cAlphaBits == 0)
	 continue;		/* need alpha buffer */

#if 0 /* regression bug? */
      if (pfd.cStencilBits > 0 && pix[i].pfd.cStencilBits == 0)
	 continue;		/* need stencil buffer */
#endif

      if (pfd.iPixelType == pix[i].pfd.iPixelType) {
	 best = i + 1;
	 break;
      }
   }

   if (best == -1) {
      rgl_log("wglChoosePixelFormat: NO MATCH -> 0 (no pixelformat fits the request; "
              "full PFD dump follows in MESA.LOG)");
      FILE *err = fopen("MESA.LOG", "w");
      if (err != NULL) {
         fprintf(err, "wglChoosePixelFormat failed\n");
         fprintf(err, "\tnSize           = %d\n", ppfd->nSize);
         fprintf(err, "\tnVersion        = %d\n", ppfd->nVersion);
         fprintf(err, "\tdwFlags         = %lu\n", ppfd->dwFlags);
         fprintf(err, "\tiPixelType      = %d\n", ppfd->iPixelType);
         fprintf(err, "\tcColorBits      = %d\n", ppfd->cColorBits);
         fprintf(err, "\tcRedBits        = %d\n", ppfd->cRedBits);
         fprintf(err, "\tcRedShift       = %d\n", ppfd->cRedShift);
         fprintf(err, "\tcGreenBits      = %d\n", ppfd->cGreenBits);
         fprintf(err, "\tcGreenShift     = %d\n", ppfd->cGreenShift);
         fprintf(err, "\tcBlueBits       = %d\n", ppfd->cBlueBits);
         fprintf(err, "\tcBlueShift      = %d\n", ppfd->cBlueShift);
         fprintf(err, "\tcAlphaBits      = %d\n", ppfd->cAlphaBits);
         fprintf(err, "\tcAlphaShift     = %d\n", ppfd->cAlphaShift);
         fprintf(err, "\tcAccumBits      = %d\n", ppfd->cAccumBits);
         fprintf(err, "\tcAccumRedBits   = %d\n", ppfd->cAccumRedBits);
         fprintf(err, "\tcAccumGreenBits = %d\n", ppfd->cAccumGreenBits);
         fprintf(err, "\tcAccumBlueBits  = %d\n", ppfd->cAccumBlueBits);
         fprintf(err, "\tcAccumAlphaBits = %d\n", ppfd->cAccumAlphaBits);
         fprintf(err, "\tcDepthBits      = %d\n", ppfd->cDepthBits);
         fprintf(err, "\tcStencilBits    = %d\n", ppfd->cStencilBits);
         fprintf(err, "\tcAuxBuffers     = %d\n", ppfd->cAuxBuffers);
         fprintf(err, "\tiLayerType      = %d\n", ppfd->iLayerType);
         fprintf(err, "\tbReserved       = %d\n", ppfd->bReserved);
         fprintf(err, "\tdwLayerMask     = %lu\n", ppfd->dwLayerMask);
         fprintf(err, "\tdwVisibleMask   = %lu\n", ppfd->dwVisibleMask);
         fprintf(err, "\tdwDamageMask    = %lu\n", ppfd->dwDamageMask);
         fclose(err);
      }

      SetLastError(0);
      return (0);
   }

   rgl_log("wglChoosePixelFormat: SELECTED pixelformat #%d (1-based)", best);
   return (best);
}

GLAPI int GLAPIENTRY
ChoosePixelFormat(HDC hdc, const PIXELFORMATDESCRIPTOR * ppfd)
{
  
   return wglChoosePixelFormat(hdc, ppfd);
}

GLAPI int GLAPIENTRY
wglDescribePixelFormat(HDC hdc, int iPixelFormat, UINT nBytes,
		       LPPIXELFORMATDESCRIPTOR ppfd)
{
   int qt_valid_pix;

   rgl_first_entry("wglDescribePixelFormat");

   qt_valid_pix = pfd_tablen();

   if (iPixelFormat < 1 || iPixelFormat > qt_valid_pix ||
       ((nBytes != sizeof(PIXELFORMATDESCRIPTOR)) && (nBytes != 0))) {
      SetLastError(0);
      return (qt_valid_pix);
   }

   if (nBytes != 0)
      *ppfd = pix[iPixelFormat - 1].pfd;

   return (qt_valid_pix);
}

GLAPI int GLAPIENTRY
DescribePixelFormat(HDC hdc, int iPixelFormat, UINT nBytes,
		    LPPIXELFORMATDESCRIPTOR ppfd)
{
   return wglDescribePixelFormat(hdc, iPixelFormat, nBytes, ppfd);
}

GLAPI int GLAPIENTRY
wglGetPixelFormat(HDC hdc)
{
   if (curPFD == 0) {
      SetLastError(0);
      return (0);
   }

   return (curPFD);
}

GLAPI int GLAPIENTRY
GetPixelFormat(HDC hdc)
{
   return wglGetPixelFormat(hdc);
}

GLAPI BOOL GLAPIENTRY
wglSetPixelFormat(HDC hdc, int iPixelFormat, const PIXELFORMATDESCRIPTOR * ppfd)
{
   int qt_valid_pix;

   rgl_first_entry("wglSetPixelFormat");

   qt_valid_pix = pfd_tablen();

   rgl_log("wglSetPixelFormat: iPixelFormat=%d (valid range 1..%d, ppfd=%s)",
           iPixelFormat, qt_valid_pix, ppfd ? "supplied" : "NULL");

   if (iPixelFormat < 1 || iPixelFormat > qt_valid_pix) {
      if (ppfd == NULL) {
         PIXELFORMATDESCRIPTOR my_pfd;
         if (!wglDescribePixelFormat(hdc, iPixelFormat, sizeof(PIXELFORMATDESCRIPTOR), &my_pfd)) {
            rgl_log("wglSetPixelFormat: BAIL -> FALSE (iPixelFormat %d out of range, "
                    "DescribePixelFormat failed)", iPixelFormat);
            SetLastError(0);
            return (FALSE);
         }
      } else if (ppfd->nSize != sizeof(PIXELFORMATDESCRIPTOR)) {
         rgl_log("wglSetPixelFormat: BAIL -> FALSE (iPixelFormat %d out of range, bad ppfd->nSize=%d)",
                 iPixelFormat, (int)ppfd->nSize);
         SetLastError(0);
         return (FALSE);
      }
   }
   curPFD = iPixelFormat;

   rgl_log("wglSetPixelFormat: OK -> TRUE (curPFD=%d; this arms wglCreateContext)", curPFD);
   return (TRUE);
}

GLAPI BOOL GLAPIENTRY
wglSwapBuffers(HDC hdc)
{
   if (!ctx) {
      SetLastError(0);
      return (FALSE);
   }

   fxMesaSwapBuffers();
   fxProfFrame();

   return (TRUE);
}

GLAPI BOOL GLAPIENTRY
SetPixelFormat(HDC hdc, int iPixelFormat, const PIXELFORMATDESCRIPTOR * ppfd)
{
   return wglSetPixelFormat(hdc, iPixelFormat, ppfd);
}

GLAPI BOOL GLAPIENTRY
SwapBuffers(HDC hdc)
{
   return wglSwapBuffers(hdc);
}

static FIXED FixedFromDouble(double d)
{
   struct {
      FIXED f;
      long l;
   } pun;
   pun.l = (long)(d * 65536L);
   return pun.f;
}

/*
** This was yanked from windows/gdi/wgl.c
*/
GLAPI BOOL GLAPIENTRY
wglUseFontBitmapsA(HDC hdc, DWORD first, DWORD count, DWORD listBase)
{
  int i;
  GLuint font_list;
  DWORD size;
  GLYPHMETRICS gm;
  HANDLE hBits;
  LPSTR lpBits;
  MAT2 mat;
  int  success = TRUE;

  if (first<0)
     return FALSE;
  if (count<0)
     return FALSE;
  if (listBase<0)
     return FALSE;

  font_list = listBase;

  mat.eM11 = FixedFromDouble(1);
  mat.eM12 = FixedFromDouble(0);
  mat.eM21 = FixedFromDouble(0);
  mat.eM22 = FixedFromDouble(-1);

  memset(&gm,0,sizeof(gm));

  /*
  ** If we can't get the glyph outline, it may be because this is a fixed
  ** font.  Try processing it that way.
  */
  if( GetGlyphOutline(hdc, first, GGO_BITMAP, &gm, 0, NULL, &mat)
      == GDI_ERROR )
  {
    return wglUseFontBitmaps_FX( hdc, first, count, listBase );
  }

  /*
  ** Otherwise process all desired characters.
  */
  for (i = 0; i < count; i++)
  {
    DWORD err;

    glNewList( font_list+i, GL_COMPILE );

    /* allocate space for the bitmap/outline */
    size = GetGlyphOutline(hdc, first + i, GGO_BITMAP, &gm, 0, NULL, &mat);
    if (size == GDI_ERROR)
    {
      glEndList( );
      err = GetLastError();
      success = FALSE;
      continue;
    }

    hBits  = GlobalAlloc(GHND, size+1);
    lpBits = GlobalLock(hBits);

    err = GetGlyphOutline(hdc,                /* handle to device context */
                          first + i,          /* character to query */
                          GGO_BITMAP,         /* format of data to return */
                          &gm,                /* pointer to structure for metrics*/
                          size,               /* size of buffer for data */
                          lpBits,             /* pointer to buffer for data */
                          &mat                /* pointer to transformation */
                                              /* matrix structure */
                          );

    if (err == GDI_ERROR)
    {
      GlobalUnlock(hBits);
      GlobalFree(hBits);
      
      glEndList( );
      err = GetLastError();
      success = FALSE;
      continue;
    }

    glBitmap(gm.gmBlackBoxX,gm.gmBlackBoxY,
             -gm.gmptGlyphOrigin.x,
             gm.gmptGlyphOrigin.y,
             gm.gmCellIncX,gm.gmCellIncY,
             (const GLubyte * )lpBits);

    GlobalUnlock(hBits);
    GlobalFree(hBits);

    glEndList( );
  }

  return success;
}

GLAPI BOOL GLAPIENTRY
wglDescribeLayerPlane(HDC hdc, int iPixelFormat, int iLayerPlane,
                      UINT nBytes, LPLAYERPLANEDESCRIPTOR ppfd)
{
  SetLastError(0);
  return (FALSE);
}

GLAPI int GLAPIENTRY
wglGetLayerPaletteEntries(HDC hdc, int iLayerPlane, int iStart,
                          int cEntries, COLORREF *pcr)
{
  SetLastError(0);
  return (FALSE);
}

GLAPI BOOL GLAPIENTRY
wglRealizeLayerPalette(HDC hdc,int iLayerPlane,BOOL bRealize)
{
  SetLastError(0);
  return(FALSE);
}

GLAPI int GLAPIENTRY
wglSetLayerPaletteEntries(HDC hdc,int iLayerPlane, int iStart,
                          int cEntries, CONST COLORREF *pcr)
{
  SetLastError(0);
  return(FALSE);
}

#if (_MSC_VER >= 1200)
#pragma warning( pop )
#endif

#endif /* FX */

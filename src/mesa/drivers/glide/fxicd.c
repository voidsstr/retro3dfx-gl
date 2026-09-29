/*
 * [retro3dfx 0.1.63] Microsoft ICD front end for the MesaFX glide driver.
 *
 * Until now this DLL could only be reached by games that load an OpenGL
 * library BY NAME (Quake II gl_driver, Quake III/RtCW r_glDriver): it exports
 * the whole opengl32 surface itself, MiniGL-style. Everything that links the
 * system opengl32.dll - GoldSrc / Counter-Strike, UT99 OpenGLDrv, Serious Sam,
 * GLQuake-era titles that do not take a driver name - never saw it, because
 * opengl32 is a KnownDLL on XP and a game-local copy is ignored.
 *
 * Microsoft's opengl32 loads the display driver's registered ICD
 * (HKLM\...\OpenGLDrivers\<name>\Dll) and talks to it only through Drv*
 * entry points plus a 336-entry dispatch table handed back by DrvSetContext.
 * These map one-for-one onto the wgl* layer in fxwgl.c, which is a single-
 * context implementation (HGLRC == 1), so the same DLL now works both ways.
 * The table order is Microsoft's, from Mesa's own drivers/windows/icd.
 */

#include <windows.h>
#include "GL/gl.h"
#include "glapi.h"
#include "fxrlog.h"

typedef struct _icdTable {
   DWORD size;
   PROC  table[336];
} ICDTABLE, *PICDTABLE;

static ICDTABLE icdTable = { 336, {
#define ICD_ENTRY(func) (PROC) gl##func,
#include "../windows/icd/icdlist.h"
#undef ICD_ENTRY
} };

/* fxwgl.c */
extern HGLRC GLAPIENTRY wglCreateContext(HDC hdc);
extern BOOL  GLAPIENTRY wglDeleteContext(HGLRC hglrc);
extern BOOL  GLAPIENTRY wglMakeCurrent(HDC hdc, HGLRC hglrc);
extern BOOL  GLAPIENTRY wglShareLists(HGLRC a, HGLRC b);
extern BOOL  GLAPIENTRY wglSwapBuffers(HDC hdc);
extern PROC  GLAPIENTRY wglGetProcAddress(LPCSTR name);
extern int   GLAPIENTRY wglDescribePixelFormat(HDC hdc, int fmt, UINT n,
                                               LPPIXELFORMATDESCRIPTOR ppfd);
extern BOOL  GLAPIENTRY wglSetPixelFormat(HDC hdc, int fmt,
                                          const PIXELFORMATDESCRIPTOR *ppfd);

#define DRV __declspec(dllexport)

DRV BOOL APIENTRY DrvValidateVersion(DWORD version)
{
   rgl_log("ICD: DrvValidateVersion(0x%lx) -> TRUE (loaded as the system ICD)",
           (unsigned long) version);
   return TRUE;
}

DRV int APIENTRY DrvDescribePixelFormat(HDC hdc, int fmt, UINT n,
                                        LPPIXELFORMATDESCRIPTOR ppfd)
{
   return wglDescribePixelFormat(hdc, fmt, n, ppfd);
}

DRV BOOL APIENTRY DrvSetPixelFormat(HDC hdc, int fmt)
{
   BOOL ok = wglSetPixelFormat(hdc, fmt, NULL);
   rgl_log("ICD: DrvSetPixelFormat(%d) -> %d", fmt, (int) ok);
   return ok;
}

DRV HGLRC APIENTRY DrvCreateContext(HDC hdc)
{
   HGLRC rc = wglCreateContext(hdc);
   rgl_log("ICD: DrvCreateContext -> %p", (void *) rc);
   return rc;
}

DRV HGLRC APIENTRY DrvCreateLayerContext(HDC hdc, int layer)
{
   return layer == 0 ? wglCreateContext(hdc) : NULL;
}

DRV BOOL APIENTRY DrvDeleteContext(HGLRC rc)
{
   return wglDeleteContext(rc);
}

DRV PICDTABLE APIENTRY DrvSetContext(HDC hdc, HGLRC rc, void *callback)
{
   (void) callback;
   if (!hdc || !rc) {
      wglMakeCurrent(NULL, NULL);
      return NULL;
   }
   if (!wglMakeCurrent(hdc, rc)) {
      rgl_log("ICD: DrvSetContext(hdc=%p, rc=%p) FAILED", (void *) hdc, (void *) rc);
      return NULL;
   }
   return &icdTable;
}

DRV BOOL APIENTRY DrvReleaseContext(HGLRC rc)
{
   (void) rc;
   return wglMakeCurrent(NULL, NULL);
}

DRV BOOL APIENTRY DrvShareLists(HGLRC a, HGLRC b)
{
   return wglShareLists(a, b);
}

DRV BOOL APIENTRY DrvCopyContext(HGLRC src, HGLRC dst, UINT mask)
{
   (void) src; (void) dst; (void) mask;
   return FALSE;
}

DRV BOOL APIENTRY DrvSwapBuffers(HDC hdc)
{
   return wglSwapBuffers(hdc);
}

DRV BOOL APIENTRY DrvSwapLayerBuffers(HDC hdc, UINT planes)
{
   (void) planes;
   return wglSwapBuffers(hdc);
}

DRV PROC APIENTRY DrvGetProcAddress(LPCSTR name)
{
   return wglGetProcAddress(name);
}

DRV BOOL APIENTRY DrvDescribeLayerPlane(HDC hdc, int fmt, int layer, UINT n,
                                        LPLAYERPLANEDESCRIPTOR plpd)
{
   (void) hdc; (void) fmt; (void) layer; (void) n; (void) plpd;
   return FALSE;
}

DRV int APIENTRY DrvSetLayerPaletteEntries(HDC hdc, int layer, int start,
                                           int count, CONST COLORREF *pcr)
{
   (void) hdc; (void) layer; (void) start; (void) count; (void) pcr;
   return 0;
}

DRV int APIENTRY DrvGetLayerPaletteEntries(HDC hdc, int layer, int start,
                                           int count, COLORREF *pcr)
{
   (void) hdc; (void) layer; (void) start; (void) count; (void) pcr;
   return 0;
}

DRV BOOL APIENTRY DrvRealizeLayerPalette(HDC hdc, int layer, BOOL realize)
{
   (void) hdc; (void) layer; (void) realize;
   return FALSE;
}

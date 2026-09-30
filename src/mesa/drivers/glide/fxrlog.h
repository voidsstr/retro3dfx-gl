/* fxrlog.h - [retro3dfx] GL context-creation trace logger.
 *
 * The MesaFX OpenGL ICD (3dfxgl.dll / opengl32_retail.dll) is a plain user-mode
 * DLL, so we can just fopen/fprintf a log file. This helper appends ONE flushed
 * line per step to C:\retrogl.log so that a `hl.exe -gl` launch (or any GL app)
 * leaves a step-by-step record of exactly where OpenGL context creation fails.
 *
 * Open+append+flush+close per line is deliberate: the interesting failure modes
 * (grSstWinOpen wedging the board, a hard fault inside Glide) can kill the
 * process mid-context-creation, and a per-line fclose guarantees every step we
 * already reached is on disk.
 *
 * LOGGING ONLY - this never changes rendering behavior. Shared by fxwgl.c (the
 * WGL entry points) and fxapi.c (fxMesaCreateContext / the grSstWinOpen call).
 */

#ifndef FXRLOG_H
#define FXRLOG_H

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <windows.h>
#include <io.h>

#define RGL_LOGFILE "C:\\retrogl.log"

static void __attribute__((unused))
rgl_log(const char *fmt, ...)
{
   va_list ap;
   FILE *f = fopen(RGL_LOGFILE, "a");
   if (!f)
      return;
   va_start(ap, fmt);
   vfprintf(f, fmt, ap);
   va_end(ap);
   fputc('\n', f);
   fflush(f);
   fclose(f);
}

/* [retro3dfx 0.1.79] QUIT-TRACE: disk-flushed lines for the V5 6000 AA
 * freeze at a game's quit (2026-09-30: Quake II at SSTH3_SLI_AA_CONFIGURATION=6
 * froze .124 between its last frame and its shutdown gamma reset, a window the
 * Glide trace does not cover). rgl_log above reaches only the OS cache, which a
 * frozen PC never writes back; rgl_sync() pushes each line through
 * FlushFileBuffers, like Glide's hwcTrace.
 *
 * RETROGL_SYNCTRACE=<n> (process environment only, read once) turns it on:
 *   1 = the rare events (palette downloads, colour clears, texture deletes,
 *       gamma ramps, context teardown, ICD context calls) + a line every 300
 *       swaps; after a colour clear or a global palette download once 300
 *       swaps have passed ("armed"), every swap, Flush and Finish too.
 *   2 = every swap from the start as well.
 * The lines go to FX_GLIDE_TRACE_FILE when that is set - the same file Glide's
 * trace appends to, so one file holds both in the order they happened - else
 * to C:\retrogl.log. Off (the default) it costs one test per event. */
extern unsigned long rgl_swaps;   /* wglSwapBuffers calls since the DLL loaded */
extern int rgl_armed;             /* 1: log every swap/Flush/Finish from now on */

static int __attribute__((unused))
rgl_sync_level(void)
{
   static int level = -1;
   if (level < 0) {
      const char *e = getenv("RETROGL_SYNCTRACE");
      level = (e && *e) ? atoi(e) : 0;
      if (level < 0)
         level = 0;
   }
   return level;
}

static void __attribute__((unused))
rgl_sync(const char *fmt, ...)
{
   va_list ap;
   const char *path;
   FILE *f;
   if (!rgl_sync_level())
      return;
   path = getenv("FX_GLIDE_TRACE_FILE");
   f = fopen((path && *path) ? path : RGL_LOGFILE, "a");
   if (!f)
      return;
   fprintf(f, "%lu ICD ", (unsigned long) GetTickCount());
   va_start(ap, fmt);
   vfprintf(f, fmt, ap);
   va_end(ap);
   fputc('\n', f);
   fflush(f);
   FlushFileBuffers((HANDLE) _get_osfhandle(_fileno(f)));
   fclose(f);
}

/* a late colour clear or palette download: arm the per-swap log */
#define RGL_ARM_AFTER 300UL
static void __attribute__((unused))
rgl_maybe_arm(const char *why)
{
   if (rgl_sync_level() && !rgl_armed && rgl_swaps >= RGL_ARM_AFTER) {
      rgl_armed = 1;
      rgl_sync("ARMED at swap %lu by %s: every swap/Flush/Finish is logged from here",
               rgl_swaps, why);
   }
}

#endif /* FXRLOG_H */

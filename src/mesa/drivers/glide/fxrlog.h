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

#endif /* FXRLOG_H */

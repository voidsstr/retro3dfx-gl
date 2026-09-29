/* fxprof.c - [retro3dfx] 0.1.67: an opt-in sampling profiler for the render
 * thread, because XP ships none and the Voodoo 5 6000 is CPU-bound.
 *
 * WHY: with four VSA-100s Quake II runs at the same ~215 fps at 640x480 and at
 * 320x240 on one chip - the card is waiting on the CPU. Which part of the
 * CPU (our Mesa T&L, the fx vertex emit, Glide's triangle setup, the game
 * itself) cannot be read off a frame rate, and guessing at it is how an
 * optimisation pass spends a day on the wrong function.
 *
 * HOW: RETROGL_PROF=<output path> in the game's environment. At the first
 * wglMakeCurrent the calling thread - the one that renders - becomes the
 * target, and a TIME_CRITICAL sampler thread suspends it every ~1 ms, reads
 * its EIP and resumes it. A sample counts only while wglSwapBuffers has run
 * in the last 100 ms, so level loading and menus stay out of the profile. At
 * process exit (cleangraphics, the atexit handler) the histogram is written
 * as "eip count allocation-base module-path" lines; our DLLs keep their
 * symbols, so scripts/benchmarks/icdprof.py names the functions on the host.
 *
 * SAFETY: while the target is suspended the sampler calls ONLY kernel entry
 * points (SuspendThread, GetThreadContext, ResumeThread, Sleep) and writes a
 * static table - no heap, no CRT, no loader lock - so it can never wait on a
 * lock the suspended thread holds. winmm's timeBeginPeriod is resolved with
 * LoadLibrary so the ICD gains no static import. Unset, none of this runs:
 * fxProfStart returns on the getenv and fxProfFrame is one store.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "fxrlog.h"

#define PROF_SLOTS   65536u        /* power of two; open addressing */
#define PROF_PROBES  64

static DWORD prof_eip[PROF_SLOTS];
static DWORD prof_cnt[PROF_SLOTS];
static volatile LONG prof_run;
static volatile DWORD prof_lastswap;
static DWORD prof_samples, prof_idle, prof_dropped;
static HANDLE prof_target, prof_thread;
static char prof_path[MAX_PATH];

static void
prof_add(DWORD eip)
{
   DWORD h = (eip * 2654435761u) >> 16, i;
   for (i = 0; i < PROF_PROBES; i++) {
      DWORD k = (h + i) & (PROF_SLOTS - 1);
      if (prof_eip[k] == eip) { prof_cnt[k]++; return; }
      if (prof_eip[k] == 0)   { prof_eip[k] = eip; prof_cnt[k] = 1; return; }
   }
   prof_dropped++;
}

static DWORD WINAPI
prof_main(LPVOID unused)
{
   CONTEXT c;
   (void) unused;
   while (prof_run) {
      Sleep(1);
      if (GetTickCount() - prof_lastswap > 100) { prof_idle++; continue; }
      if (SuspendThread(prof_target) == (DWORD) -1)
         break;                       /* the thread is gone */
      c.ContextFlags = CONTEXT_CONTROL;
      if (GetThreadContext(prof_target, &c)) {
         prof_add(c.Eip);
         prof_samples++;
      }
      ResumeThread(prof_target);
   }
   return 0;
}

void
fxProfStart(void)
{
   const char *p;
   DWORD tid;
   if (prof_thread || !(p = getenv("RETROGL_PROF")) || !*p)
      return;
   lstrcpynA(prof_path, p, sizeof(prof_path));
   if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                        &prof_target, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                        THREAD_QUERY_INFORMATION, FALSE, 0)) {
      rgl_log("fxprof: DuplicateHandle failed (%lu) - no profile", GetLastError());
      return;
   }
   {  /* 1 ms Sleep needs the 1 ms timer; most id engines already set it */
      HMODULE w = LoadLibraryA("winmm.dll");
      FARPROC f = w ? GetProcAddress(w, "timeBeginPeriod") : NULL;
      if (f) ((UINT (WINAPI *)(UINT)) f)(1);
   }
   prof_lastswap = GetTickCount() - 1000;
   prof_run = 1;
   prof_thread = CreateThread(NULL, 0, prof_main, NULL, 0, &tid);
   if (!prof_thread) {
      prof_run = 0;
      rgl_log("fxprof: CreateThread failed (%lu) - no profile", GetLastError());
      return;
   }
   SetThreadPriority(prof_thread, THREAD_PRIORITY_TIME_CRITICAL);
   rgl_log("fxprof: sampling render thread %lu -> %s",
           (unsigned long) GetCurrentThreadId(), prof_path);
}

void
fxProfFrame(void)
{
   prof_lastswap = GetTickCount();
}

void
fxProfStop(void)
{
   FILE *f;
   DWORD k, n = 0;
   if (!prof_thread)
      return;
   prof_run = 0;
   WaitForSingleObject(prof_thread, 500);
   CloseHandle(prof_thread);
   prof_thread = NULL;
   f = fopen(prof_path, "w");
   if (!f) {
      rgl_log("fxprof: cannot write %s", prof_path);
      return;
   }
   fprintf(f, "# retrogl fxprof v1 samples=%lu idle=%lu dropped=%lu\n",
           (unsigned long) prof_samples, (unsigned long) prof_idle,
           (unsigned long) prof_dropped);
   for (k = 0; k < PROF_SLOTS; k++) {
      MEMORY_BASIC_INFORMATION mbi;
      char mod[MAX_PATH];
      if (!prof_eip[k])
         continue;
      mod[0] = '\0';
      if (VirtualQuery((LPCVOID) prof_eip[k], &mbi, sizeof(mbi)) && mbi.AllocationBase)
         GetModuleFileNameA((HMODULE) mbi.AllocationBase, mod, sizeof(mod));
      else
         mbi.AllocationBase = NULL;
      fprintf(f, "%08lx %lu %08lx %s\n", (unsigned long) prof_eip[k],
              (unsigned long) prof_cnt[k], (unsigned long) mbi.AllocationBase,
              mod[0] ? mod : "?");
      n++;
   }
   fclose(f);
   rgl_log("fxprof: wrote %lu addresses, %lu samples -> %s",
           (unsigned long) n, (unsigned long) prof_samples, prof_path);
}

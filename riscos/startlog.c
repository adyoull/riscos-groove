/* Groove - a start-up log, so a start that fails on a real machine says
   where. Written to <Wimp$ScrapDir>.GrooveLog (on Linux, /tmp/GrooveLog),
   a line per step, flushed as it goes; a crash is caught, logged and shown
   in an error box rather than ending the task silently. */
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <kernel.h>
#include <swis.h>
#include "app.h"
#ifdef __riscos__
#include <unixlib/local.h>
#endif

static FILE *logf;
static const char *stage = "starting";

static void open_log (void)
{
#ifdef __riscos__
  const char *dir = getenv ("Wimp$ScrapDir");
  char ro[300], ux[400];
  if (!dir || !*dir)
    return;
  snprintf (ro, sizeof ro, "%s.GrooveLog", dir);
  /* UnixLib's fopen takes Unix-style names: convert the RISC OS one. */
  if (__unixify_std (ro, ux, sizeof ux, __RISCOSIFY_FILETYPE_NOTSPECIFIED))
    logf = fopen (ux, "w");
#else
  logf = fopen ("/tmp/GrooveLog", "w");
#endif
}

void startlog (const char *fmt, ...)
{
  va_list ap;
  if (!logf)
    return;
  va_start (ap, fmt);
  vfprintf (logf, fmt, ap);
  va_end (ap);
  fputc ('\n', logf);
  fflush (logf);
}

void startlog_stage (const char *what)
{
  stage = what;
  startlog ("%s", what);
}

static void crashed (int sig)
{
  _kernel_oserror e;
  const char *name = sig == SIGSEGV ? "an address exception or abort (SIGSEGV)"
                   : sig == SIGILL ? "an illegal instruction (SIGILL)"
                   : sig == SIGBUS ? "an alignment fault (SIGBUS)"
                   : sig == SIGFPE ? "a floating point exception (SIGFPE)"
                   : "an abort (SIGABRT)";
  startlog ("CRASH: signal %d, %s, while %s", sig, name, stage);
  e.errnum = 0;
  snprintf (e.errmess, sizeof e.errmess, "Groove stopped: %s while %s. "
            "Please send <Wimp$ScrapDir>.GrooveLog.", name, stage);
  _swix (Wimp_ReportError, _INR (0, 2), &e, 1, APP_NAME);
  _exit (1);
}

void startlog_init (void)
{
  open_log ();
  startlog ("Groove " APP_VERSION " (built " __DATE__ " " __TIME__ ")");
#ifdef __riscos__
  /* (Not on Linux, where AddressSanitizer reports crashes better.) */
  signal (SIGSEGV, crashed);
  signal (SIGILL, crashed);
  signal (SIGBUS, crashed);
  signal (SIGFPE, crashed);
  signal (SIGABRT, crashed);
#else
  (void) crashed;
#endif
}

/* A step failed with this error: log it and say so in a box. */
void startlog_fail (const char *step, const _kernel_oserror *e)
{
  char msg[252];
  startlog ("FAILED: %s: %s (error &%X)", step, e ? e->errmess : "no error block", e ? e->errnum : 0);
  snprintf (msg, sizeof msg, "Groove couldn't start: %s failed: %.180s", step, e ? e->errmess : "(no reason given)");
  app_error (msg);
}

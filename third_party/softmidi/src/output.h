/*
 * midisynth - the interface between output.c and the output backends.
 * Copyright (c) 2026 Andrew Youll. MIT licence, see LICENSE.
 *
 * A backend plays the synth's sound through one RISC OS sound system.
 * output.c tries them in order (SharedSoundBuffer, then DigitalRenderer)
 * and keeps the one that opened in ms->out. To add a backend: write
 * output_xxx.c with a struct ms_backend, and list it in output.c.
 */
#ifndef MIDISYNTH_OUTPUT_H
#define MIDISYNTH_OUTPUT_H

#include <stddef.h>
#include "midisynth_internal.h"

struct ms_backend;

/* Each backend's own state starts with this, so output.c can find the
   backend's functions. */
struct ms_output
{
    const struct ms_backend *backend;
};

struct ms_backend
{
    const char *name;              /* as midisynth_output_name returns it */
    /* Start playing. Returns the backend's state, or NULL with the reason
       in err. May call ms_set_rate. Called without the synth's lock. */
    struct ms_output *(*open)(midisynth *ms, const char *name, char *err, size_t errlen);
    /* Render and queue more sound if the queue is low. */
    void (*poll)(struct ms_output *out, midisynth *ms);
    /* Stop, and free the state. */
    void (*close)(struct ms_output *out);
};

extern const struct ms_backend ms_backend_ssb;   /* output_ssb.c */
extern const struct ms_backend ms_backend_dr;    /* output_dr.c */

/* The backends are built for RISC OS, and for the tests with fake SWIs
   (MIDISYNTH_FAKE_SWI, see tests/fake_swi.c). */
#if defined(__riscos__) || defined(MIDISYNTH_FAKE_SWI)
#define MS_HAVE_OUTPUT 1
#else
#define MS_HAVE_OUTPUT 0
#endif

#endif

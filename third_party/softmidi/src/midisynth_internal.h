/*
 * midisynth - internal definitions shared by the library's source files.
 * Copyright (c) 2026 Andrew Youll. MIT licence, see LICENSE.
 *
 * Files:
 *   midisynth.c    the synth: SoundFont, songs, MIDI events, rendering
 *   output.c       midisynth_output_*: picks a backend and calls it
 *   output_ssb.c   backend: SharedSoundBuffer (RISC OS)
 *   output_dr.c    backend: DigitalRenderer (RISC OS), the fallback
 *   output.h       the interface between output.c and the backends
 *   riscos_swi.h   <kernel.h> for the backends (or the tests' fake one)
 */
#ifndef MIDISYNTH_INTERNAL_H
#define MIDISYNTH_INTERNAL_H

#include <pthread.h>
#include "midisynth.h"

#define MS_ERRLEN 320

struct tsf;
struct tml_message;
struct ms_output;

struct midisynth
{
    pthread_mutex_t lock;          /* held by every public call */
    struct tsf *sf;
    int rate;                      /* output sample rate, Hz */
    struct tml_message *song;      /* whole song (freed on unload) */
    struct tml_message *next;      /* next event to play */
    double msec;                   /* song position */
    int playing, loop;
    float volume;                  /* 0-1, as set by midisynth_set_volume */
    struct ms_output *out;         /* sound output (output.c), NULL if none */
    char errbuf[MS_ERRLEN];        /* midisynth_last_error */
};

/* Record why a call failed: "msg" or "msg: extra". ms may be NULL (create).
   Doesn't take the lock. */
void ms_error(midisynth *ms, const char *msg, const char *extra);

/* getenv(name), or getenv(old_name) if that's unset or empty (old_name may
   be NULL). For system variables renamed with the app in 0.5.0. */
const char *ms_getenv(const char *name, const char *old_name);

/* Change the output sample rate (8000-96000), e.g. to the rate
   DigitalRenderer chose. Takes the lock. Returns 1, or 0 if out of range. */
int ms_set_rate(midisynth *ms, int rate);

#endif

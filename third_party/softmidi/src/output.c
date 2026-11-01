/*
 * midisynth - midisynth_output_*: play the synth's sound on its own.
 * Copyright (c) 2026 Andrew Youll. MIT licence, see LICENSE.
 *
 * Tries each backend in turn and keeps the first that opens. The backends
 * are in output_ssb.c and output_dr.c; see output.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "output.h"

#if MS_HAVE_OUTPUT

/* In order of preference. SharedSoundBuffer mixes with other programs'
   sound; DigitalRenderer can only be used by one program at a time. */
static const struct ms_backend *const ms_backends[] = {
    &ms_backend_ssb,
    &ms_backend_dr,
};
#define MS_NBACKENDS (sizeof(ms_backends) / sizeof(ms_backends[0]))

int
midisynth_output_open(midisynth *ms, const char *name)
{
    /* SoftMIDI$Output (MIDISynth$Output before 0.5.0) names the only
       backend to try (for testing, or to choose DigitalRenderer on purpose) */
    const char *only = ms_getenv("SoftMIDI$Output", "MIDISynth$Output");
    char why[MS_ERRLEN] = "", err[MS_ERRLEN];
    size_t i;

    if (ms->out)
        return 1;
    if (name == NULL || *name == 0)
        name = "SoftMIDI";
    for (i = 0; i < MS_NBACKENDS; i++) {
        const struct ms_backend *b = ms_backends[i];
        struct ms_output *out;
        if (only && *only && strcasecmp(only, b->name) != 0)
            continue;
        err[0] = 0;
        out = b->open(ms, name, err, sizeof(err));
        if (out) {
            out->backend = b;
            ms->out = out;
            return 1;
        }
        /* collect the reasons: "SharedSoundBuffer: ...; DigitalRenderer: ..." */
        snprintf(why + strlen(why), sizeof(why) - strlen(why), "%s%s: %s",
                 why[0] ? "; " : "", b->name, err);
    }
    ms_error(ms, "No sound output", why[0] ? why : "SoftMIDI$Output names no known output");
    return 0;
}

void
midisynth_output_poll(midisynth *ms)
{
    if (ms->out)
        ms->out->backend->poll(ms->out, ms);
}

void
midisynth_output_close(midisynth *ms)
{
    if (ms->out) {
        struct ms_output *out = ms->out;
        ms->out = NULL;
        out->backend->close(out);
    }
}

const char *
midisynth_output_name(midisynth *ms)
{
    return ms->out ? ms->out->backend->name : NULL;
}

#else /* no sound output on this system: render into your own buffer */

int
midisynth_output_open(midisynth *ms, const char *name)
{
    (void)name;
    ms_error(ms, "No sound output", "not supported on this system");
    return 0;
}

void midisynth_output_poll(midisynth *ms) { (void)ms; }
void midisynth_output_close(midisynth *ms) { (void)ms; }
const char *midisynth_output_name(midisynth *ms) { (void)ms; return NULL; }

#endif

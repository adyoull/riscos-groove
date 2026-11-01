/*
 * midisynth - output backend: DigitalRenderer, the fallback.
 * Copyright (c) 2026 Andrew Youll. MIT licence, see LICENSE.
 *
 * DigitalRenderer (Andreas Dehmel) plays a stream of sample buffers. It is
 * used when SharedSoundBuffer isn't available. Unlike SharedSoundBuffer it
 * has one user at a time, so this backend refuses to start if another
 * program is already using it (UnixLib's /dev/dsp takes it over; we
 * don't). The SWIs are called directly: UnixLib's /dev/dsp would busy-wait.
 * SWI numbers and usage follow GCCSDK's DRender.h and UnixLib's dsp.c.
 */
#include <stdlib.h>
#include <stdio.h>
#include "output.h"

#if MS_HAVE_OUTPUT
#include "riscos_swi.h"

#define DR_CHUNK 0x4F700
#define XDigitalRenderer_Deactivate         (MS_XSWI | (DR_CHUNK + 1))
#define XDigitalRenderer_ReadState          (MS_XSWI | (DR_CHUNK + 5))
#define XDigitalRenderer_NumBuffers         (MS_XSWI | (DR_CHUNK + 9))
#define XDigitalRenderer_Stream16BitSamples (MS_XSWI | (DR_CHUNK + 11))
#define XDigitalRenderer_StreamStatistics   (MS_XSWI | (DR_CHUNK + 12))
#define XDigitalRenderer_StreamFlags        (MS_XSWI | (DR_CHUNK + 13))
#define XDigitalRenderer_Activate16         (MS_XSWI | (DR_CHUNK + 15))
#define XDigitalRenderer_GetFrequency       (MS_XSWI | (DR_CHUNK + 16))
#define XDigitalRenderer_SampleFormat       (MS_XSWI | (DR_CHUNK + 18))

#define DRState_Active        (1 << 0)
#define DRStream_OverrunNull  (1 << 0)     /* play silence if we fall behind */
#define DRActivate_Restore    (1 << 0)     /* put back the old sound handler after */
#define DRFormat_S16LR        3            /* 16-bit, left then right */

#define BUF_FRAMES   512           /* frames per DigitalRenderer buffer */
#define QUEUE_MS     100           /* sound to keep queued ahead */
#define MIN_BUFFERS  4
#define POLL_BUFFERS 32            /* most buffers rendered in one poll */

struct dr_output
{
    struct ms_output base;         /* must be first */
    int buffers;                   /* how many buffers to keep queued */
    int16_t buf[BUF_FRAMES * 2];
};

/* A DigitalRenderer SWI with R0 and R1 in; R0 out goes in *result
   (if not NULL). Returns the error, or NULL. */
static _kernel_oserror *
dr_swi(int swi, int r0, int r1, int *result)
{
    _kernel_swi_regs r;
    _kernel_oserror *e;
    r.r[0] = r0;
    r.r[1] = r1;
    e = _kernel_swi(swi, &r, &r);
    if (e == NULL && result)
        *result = r.r[0];
    return e;
}

static void
dr_close(struct ms_output *out)
{
    dr_swi(XDigitalRenderer_Deactivate, 0, 0, NULL);
    dr_swi(XDigitalRenderer_NumBuffers, 0, 0, NULL);    /* stop streaming */
    free(out);
}

static struct ms_output *
dr_open(midisynth *ms, const char *name, char *err, size_t errlen)
{
    struct dr_output *o;
    _kernel_swi_regs r;
    _kernel_oserror *e;
    int state, rate;

    (void)name;                    /* DigitalRenderer has no stream names */
    if ((e = dr_swi(XDigitalRenderer_ReadState, 0, 0, &state)) != NULL) {
        snprintf(err, errlen, "%s", e->errmess);   /* usually "SWI not known" */
        return NULL;
    }
    if (state & DRState_Active) {
        snprintf(err, errlen, "in use by another program");
        return NULL;
    }
    o = (struct dr_output *) calloc(1, sizeof(*o));
    if (o == NULL) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    o->buffers = (ms->rate * QUEUE_MS / 1000 + BUF_FRAMES - 1) / BUF_FRAMES;
    if (o->buffers < MIN_BUFFERS)
        o->buffers = MIN_BUFFERS;
    /* As UnixLib does: the buffers and flags first, then activate */
    dr_swi(XDigitalRenderer_NumBuffers, o->buffers, 0, NULL);
    dr_swi(XDigitalRenderer_StreamFlags, DRStream_OverrunNull, 0, NULL);  /* flags = (old AND 0) EOR OverrunNull */
    r.r[0] = 2;                                  /* channels */
    r.r[1] = BUF_FRAMES;                         /* frames per buffer */
    r.r[2] = ms->rate;
    r.r[3] = DRActivate_Restore;
    if ((e = _kernel_swi(XDigitalRenderer_Activate16, &r, &r)) != NULL) {
        snprintf(err, errlen, "%s", e->errmess);
        dr_swi(XDigitalRenderer_NumBuffers, 0, 0, NULL);
        free(o);
        return NULL;
    }
    /* DigitalRenderer may not manage the rate we asked for */
    if (dr_swi(XDigitalRenderer_GetFrequency, 0, 0, &rate) == NULL
        && rate > 0 && rate != ms->rate && !ms_set_rate(ms, rate)) {
        snprintf(err, errlen, "plays at %d Hz, which the synth can't use", rate);
        dr_close(&o->base);
        return NULL;
    }
    dr_swi(XDigitalRenderer_SampleFormat, DRFormat_S16LR, 0, NULL);
    return &o->base;
}

static void
dr_poll(struct ms_output *out, midisynth *ms)
{
    struct dr_output *o = (struct dr_output *) out;
    int n, queued;

    for (n = 0; n < POLL_BUFFERS; n++) {
        if (dr_swi(XDigitalRenderer_StreamStatistics, 0, 0, &queued) != NULL
            || queued >= o->buffers)
            break;
        midisynth_render(ms, o->buf, BUF_FRAMES, 0);
        /* the count is in samples: left and right count separately */
        if (dr_swi(XDigitalRenderer_Stream16BitSamples, MS_PTR(o->buf), BUF_FRAMES * 2, NULL) != NULL)
            break;
    }
}

const struct ms_backend ms_backend_dr = {
    "DigitalRenderer", dr_open, dr_poll, dr_close
};

#endif /* MS_HAVE_OUTPUT */

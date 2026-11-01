/*
 * midisynth - output backend: SharedSoundBuffer.
 * Copyright (c) 2026 Andrew Youll. MIT licence, see LICENSE.
 *
 * SharedSoundBuffer 0.07 and StreamManager 0.03 are by John Duffell. They
 * queue blocks of sound and play them through SharedSound, mixed with
 * other programs' sound. Their documentation (in ssb.zip, 2004) is out of
 * date in places; where it differs, this follows RDPClient's c/Sound,
 * which is known to work. See docs/DESIGN.md.
 */
#include <stdlib.h>
#include <stdio.h>
#include "output.h"

#if MS_HAVE_OUTPUT
#include "riscos_swi.h"

#define XSharedSoundBuffer_OpenStream         (MS_XSWI | 0x55FC0)
#define XSharedSoundBuffer_CloseStream        (MS_XSWI | 0x55FC1)
#define XSharedSoundBuffer_Volume             (MS_XSWI | 0x55FC4)
#define XSharedSoundBuffer_SampleRate         (MS_XSWI | 0x55FC5)
#define XSharedSoundBuffer_Pause              (MS_XSWI | 0x55FC9)
#define XSharedSoundBuffer_ReturnStreamHandle (MS_XSWI | 0x55FCE)
#define XStreamManager_AddBlock               (MS_XSWI | 0x57282)
#define XStreamManager_SetBuffer              (MS_XSWI | 0x57287)
#define XStreamManager_BufferStats            (MS_XSWI | 0x57288)

#define SSB_OPEN_BLOCKSIZE  (1u << 1)      /* OpenStream R0: R2 holds the usual block size */
#define SSB_PAUSE_RESUME    (1u << 0)      /* Pause R1: set = play, clear = pause */
#define SSB_VOLUME_FULL     0xFFFFFFFFu    /* Volume R1: &LLLLRRRR, left and right 0-&FFFF */
#define SSB_RATE_UNIT       1024           /* SampleRate R1 is in 1/1024 Hz */

#define FRAME_BYTES    4           /* one frame: 16-bit left + 16-bit right */
#define BLOCK_FRAMES   1024        /* frames per block given to StreamManager */
#define BLOCK_BYTES    (BLOCK_FRAMES * FRAME_BYTES)
#define QUEUE_MS       100         /* sound to keep queued ahead */
#define BUFFER_MAX     4           /* StreamManager may hold up to 4 x that */
#define POLL_BLOCKS    16          /* most blocks rendered in one poll */

struct ssb_output
{
    struct ms_output base;         /* must be first */
    int ssb;                       /* SharedSoundBuffer stream handle */
    int stream;                    /* the StreamManager stream under it */
    int started;                   /* playing yet? (starts paused) */
    int target;                    /* bytes to keep queued (QUEUE_MS worth) */
    int16_t block[BLOCK_FRAMES * 2];
};

/* Bytes queued but not yet played, or -1 on error. BufferStats returns
   R0 = bytes added so far, R1 = bytes played (the 2004 documentation says
   R0 = unplayed bytes; RDPClient uses R0 - R1). */
static int
ssb_queued(struct ssb_output *o)
{
    _kernel_swi_regs r;
    r.r[0] = o->stream;
    if (_kernel_swi(XStreamManager_BufferStats, &r, &r) != NULL)
        return -1;
    return r.r[0] - r.r[1];
}

static void
ssb_pause(struct ssb_output *o, int play)
{
    _kernel_swi_regs r;
    r.r[0] = o->ssb;
    r.r[1] = play ? SSB_PAUSE_RESUME : 0;
    _kernel_swi(XSharedSoundBuffer_Pause, &r, &r);
}

static void
ssb_close(struct ms_output *out)
{
    struct ssb_output *o = (struct ssb_output *) out;
    _kernel_swi_regs r;
    r.r[0] = o->ssb;
    _kernel_swi(XSharedSoundBuffer_CloseStream, &r, &r);
    free(o);
}

static struct ms_output *
ssb_open(midisynth *ms, const char *name, char *err, size_t errlen)
{
    struct ssb_output *o;
    _kernel_swi_regs r;
    _kernel_oserror *e;

    o = (struct ssb_output *) calloc(1, sizeof(*o));
    if (o == NULL) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    r.r[0] = SSB_OPEN_BLOCKSIZE;
    r.r[1] = MS_PTR(name);
    r.r[2] = BLOCK_BYTES;
    if ((e = _kernel_swi(XSharedSoundBuffer_OpenStream, &r, &r)) != NULL) {
        snprintf(err, errlen, "%s", e->errmess);   /* usually "SWI not known" */
        free(o);
        return NULL;
    }
    o->ssb = r.r[0];
    r.r[0] = o->ssb;
    if ((e = _kernel_swi(XSharedSoundBuffer_ReturnStreamHandle, &r, &r)) != NULL) {
        snprintf(err, errlen, "%s", e->errmess);
        ssb_close(&o->base);
        return NULL;
    }
    o->stream = r.r[0];
    /* Keep about QUEUE_MS queued (at least two blocks): plenty for a Wimp
       program that is only polled now and then. StreamManager may hold a
       few times that, so a burst of polls can't overfill it. */
    o->target = ms->rate * FRAME_BYTES * QUEUE_MS / 1000;
    if (o->target < BLOCK_BYTES * 2)
        o->target = BLOCK_BYTES * 2;
    /* The next three calls only fail if the stream is broken, which
       AddBlock will then report; so their errors aren't checked. */
    r.r[0] = o->stream;
    r.r[1] = o->target * BUFFER_MAX;
    _kernel_swi(XStreamManager_SetBuffer, &r, &r);
    r.r[0] = o->ssb;
    r.r[1] = ms->rate * SSB_RATE_UNIT;
    _kernel_swi(XSharedSoundBuffer_SampleRate, &r, &r);
    r.r[0] = o->ssb;
    r.r[1] = (int)SSB_VOLUME_FULL;     /* the synth applies its own volume */
    _kernel_swi(XSharedSoundBuffer_Volume, &r, &r);
    ssb_pause(o, 0);                   /* paused until two blocks are queued */
    return &o->base;
}

static void
ssb_poll(struct ms_output *out, midisynth *ms)
{
    struct ssb_output *o = (struct ssb_output *) out;
    _kernel_swi_regs r;
    int queued, n;

    for (n = 0; n < POLL_BLOCKS; n++) {
        queued = ssb_queued(o);
        if (queued < 0 || queued >= o->target)
            break;
        midisynth_render(ms, o->block, BLOCK_FRAMES, 0);
        r.r[0] = o->stream;
        r.r[1] = MS_PTR(o->block);
        r.r[2] = BLOCK_BYTES;
        if (_kernel_swi(XStreamManager_AddBlock, &r, &r) != NULL)   /* copies the data */
            break;
    }
    if (!o->started && ssb_queued(o) >= BLOCK_BYTES * 2) {
        ssb_pause(o, 1);
        o->started = 1;
    }
}

const struct ms_backend ms_backend_ssb = {
    "SharedSoundBuffer", ssb_open, ssb_poll, ssb_close
};

#endif /* MS_HAVE_OUTPUT */

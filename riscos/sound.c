/* Groove - sound output through SharedSound.

   The engine runs in the Wimp task; the sound system wants data at
   interrupt time, when another task may be paged in. So the two meet in
   the RMA:

     task (null events)                       interrupt (SharedSound)
     gr_render_words() --> ring buffer in RMA --> handler.s, also in RMA
                           fill offset ->         <- play offset

   One writer per offset, each a single aligned word store, so no locks
   are needed. The ring holds RING_FRAMES of 16-bit stereo at 44.1 kHz;
   we keep TARGET frames queued: enough to ride out a busy desktop (a
   window being dragged, a Filer copy), small enough that a key press is
   heard promptly. SharedSound's fill routines resample to the output rate.

   The real fix for latency and for single-tasking moments is to move the
   engine into a module (see the design doc); this file is the first step
   and its interface wouldn't change. */
#include <stdint.h>
#include <string.h>
#include <kernel.h>
#include <swis.h>
#include "app.h"

#define RING_FRAMES  8192          /* 186 ms: the most we could ever queue */
#define TARGET       3072          /* 70 ms queued: the latency we aim for */

typedef struct
{
  uint32_t *start, *end;
  volatile uint32_t play;         /* byte offsets */
  volatile uint32_t fill;
  uint32_t acc;
  volatile uint32_t underruns;
} ring_block;

extern const char groove_handler[], groove_handler_end[];

static ring_block *ring;           /* in the RMA: block, then code, then buffer */
static int handler = -1;

static void *rma_claim (int size)
{
  void *p = NULL;
  if (_swix (OS_Module, _IN (0) | _IN (3) | _OUT (2), 6, size, &p))
    return NULL;
  return p;
}

static void rma_free (void *p)
{
  if (p)
    _swix (OS_Module, _IN (0) | _IN (2), 7, p);
}

int sound_init (void)
{
  int code = (int) (groove_handler_end - groove_handler);
  int size = sizeof (ring_block) + ((code + 15) & ~15) + RING_FRAMES * 4;
  char *base, *code_copy;
  _kernel_oserror *e;

  base = rma_claim (size);
  if (!base)
    {
      app_error ("Groove couldn't claim memory for its sound buffer.");
      return -1;
    }
  memset (base, 0, (size_t) size);
  /* RMA blocks are word-aligned, the block is a whole number of words and
     the code is padded to 16 bytes, so every part below is word-aligned. */
  ring = (void *) base;
  code_copy = base + sizeof (ring_block);
  ring->start = (void *) (code_copy + ((code + 15) & ~15));
  ring->end = ring->start + RING_FRAMES;
  memcpy (code_copy, groove_handler, (size_t) code);
  /* New code in memory: make sure the instruction cache sees it. */
  _swix (OS_SynchroniseCodeAreas, _INR (0, 2), 1, code_copy, code_copy + code - 4);

  /* R2 bit 0: use the type in R4; type 0: an immediate handler (called
     from the sound interrupt itself - ours only copies words). */
  e = _swix (SharedSound_InstallHandler, _INR (0, 4) | _OUT (0),
             code_copy, ring, 1, APP_NAME, 0, &handler);
  if (e)
    {
      handler = -1;
      rma_free (base);
      ring = NULL;
      app_error ("Groove needs the SharedSound module, and couldn't use it. "
                 "Check that !Boot loads it (it comes with RISC OS 5).");
      return -1;
    }
  /* Tell SharedSound our data is 44.1 kHz; it works out the step. */
  _swix (SharedSound_SampleRate, _INR (0, 1), handler, GR_RATE * 1024);
  startlog ("sound: handler %d, code copied to &%X (%d bytes), ring &%X", handler,
            (unsigned) (uintptr_t) code_copy, code, (unsigned) (uintptr_t) ring->start);
  return 0;
}

/* For the start-up log: is the handler being called? */
unsigned sound_played (void) { return ring ? ring->play : 0; }

int sound_latency (void)
{
  uint32_t size, used;
  if (!ring)
    return 0;
  size = RING_FRAMES * 4;
  used = (ring->fill + size - ring->play) % size;
  return (int) (used / 4);
}

unsigned sound_underruns (void)
{
  return ring ? ring->underruns : 0;
}

/* Top the ring up to TARGET frames, rendering straight into it (at most two
   runs: to the end of the buffer, then from its start). */
void sound_fill (gr_engine *e)
{
  uint32_t size, fill;
  int want, n;
  if (!ring)
    return;
  /* With nothing to play, let the ring drain and the handler fill silence:
     no rendering at all while the desktop is idle. */
  if (!gr_busy (e))
    return;
  want = TARGET - sound_latency ();
  if (want <= 0)
    return;
  size = RING_FRAMES * 4;
  fill = ring->fill;
  while (want > 0)
    {
      n = (int) ((size - fill) / 4);              /* room before the end */
      if (n > want)
        n = want;
      gr_render_words (e, ring->start + fill / 4, n);
      fill = (fill + (uint32_t) n * 4) % size;
      want -= n;
    }
  ring->fill = fill;                               /* one store: now visible */
}

void sound_close (void)
{
  if (handler >= 0)
    _swix (SharedSound_RemoveHandler, _IN (0), handler);
  handler = -1;
  /* The handler is gone, so nothing reads the RMA block any more. */
  if (ring)
    rma_free (ring);
  ring = NULL;
}

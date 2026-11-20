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

/* How hard rendering is working: over each half second of sound made,
   the time it took. Only if it's more than LOAD_MAX% of the half second
   and the sound waiting to be played has run low (it really can't keep
   up: the desktop would crawl and the sound break up) are the voices
   least missed let go (gr_gm_shed), and only a few. (0.4.2-test2 to
   test7 let 40% go at 60%, which on a Pi cut notes in a big score.) */
#define LOAD_WINDOW (GR_RATE / 2)
#define LOAD_MAX    85
#define LOW_WATER   (TARGET / 4)    /* frames waiting to play: running low */
#define FILL_MAX_CS 4               /* the longest one fill renders for */
#define CHUNK       512             /* frames rendered at a time */
static int win_frames, win_cs;
/* Over a play, for the log: how hard rendering worked on average and at
   worst (per half second), and the most SoundFont voices sounding. */
static int was_playing, play_frames, play_cs, play_peak, play_voices, play_stolen;

static int now_cs (void)
{
  int t = 0;
  _swix (OS_ReadMonotonicTime, _OUT (0), &t);
  return t;
}

/* Top the ring up to TARGET frames, rendering straight into it, a chunk at
   a time, stopping early if it's taking too long (the ring runs short
   rather than the desktop freezing). */
void sound_fill (gr_engine *e)
{
  uint32_t size, fill;
  int want, n, t0, t;
  if (!ring)
    return;
  if (was_playing && !gr_playing (e) && play_frames > 0)
    {
      startlog ("sound: rendering took %d%% of the time playing (at worst %d%%), %d SoundFont voices at most, %d notes cut short, %u underruns so far",
                play_cs * 100 / (play_frames / (GR_RATE / 100)), play_peak, play_voices, gr_gm_stolen () - play_stolen, sound_underruns ());
      play_stolen = gr_gm_stolen ();
      play_frames = play_cs = play_peak = play_voices = 0;
    }
  was_playing = gr_playing (e);
  /* With nothing to play, let the ring drain and the handler fill silence:
     no rendering at all while the desktop is idle. */
  if (!gr_busy (e))
    return;
  want = TARGET - sound_latency ();
  if (want <= 0)
    return;
  size = RING_FRAMES * 4;
  fill = ring->fill;
  t0 = t = now_cs ();
  while (want > 0 && t - t0 < FILL_MAX_CS)
    {
      n = (int) ((size - fill) / 4);              /* room before the end */
      if (n > want)
        n = want;
      if (n > CHUNK)
        n = CHUNK;
      gr_render_words (e, ring->start + fill / 4, n);
      fill = (fill + (uint32_t) n * 4) % size;
      ring->fill = fill;                           /* one store: now visible */
      want -= n;
      win_frames += n;
      t = now_cs ();
    }
  win_cs += t - t0;
  if (gr_gm_voices () > play_voices)
    play_voices = gr_gm_voices ();
  if (win_frames >= LOAD_WINDOW)
    {
      if (was_playing)
        {
          int pct = win_cs * GR_RATE / win_frames;
          play_frames += win_frames;
          play_cs += win_cs;
          if (pct > play_peak)
            play_peak = pct;
        }
      /* (rendering took win_cs to make win_frames of sound) */
      if (win_cs * GR_RATE > LOAD_MAX * win_frames && sound_latency () < LOW_WATER)
        {
          int before = gr_gm_voices ();
          gr_gm_shed (80);
          startlog ("sound: rendering %d%% of the time with %d SoundFont voices: down to %d",
                    win_cs * GR_RATE / win_frames, before, gr_gm_voices ());
        }
      win_frames = win_cs = 0;
    }
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

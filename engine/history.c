/* Groove - Undo and Redo.

   The history is a list of snapshots of the song, each a compact copy of
   what is in use (song settings, tracks, regions, patterns with only their
   notes, clips without their audio). The front end calls gr_history_note
   after the user has done something (not in the middle of a drag); if the
   song differs from the current snapshot, a new one is added. Undo and
   Redo copy a snapshot back into the song.

   A clip's audio isn't copied: gr_song_gc keeps it when its last region
   goes, until the slot is reused, and each clip carries an id, so a
   snapshot can tell whether its recording is still there. */
#include <stdlib.h>
#include <string.h>
#include "groove.h"

typedef struct
{
  unsigned char *data;
  size_t size;
} snap;

struct gr_history
{
  snap *s;
  int n, cap;                   /* snapshots held */
  int cur;                      /* the one the song matches */
  int saved;                    /* the one that was saved or loaded (-1: none) */
  int max_levels;
  size_t max_bytes, bytes;
};

/* ---- snapshots ---------------------------------------------------------- */

typedef struct { unsigned char *p; size_t n, cap; int fail; } buf;

static void put (buf *b, const void *d, size_t n)
{
  if (b->fail)
    return;
  if (b->n + n > b->cap)
    {
      size_t cap = b->cap ? b->cap * 2 : 4096;
      unsigned char *p;
      while (cap < b->n + n)
        cap *= 2;
      if (!(p = realloc (b->p, cap)))
        {
          b->fail = 1;
          return;
        }
      b->p = p;
      b->cap = cap;
    }
  memcpy (b->p + b->n, d, n);
  b->n += n;
}

static void put_int (buf *b, int v) { put (b, &v, sizeof v); }

typedef struct
{
  int tempo, key, bars, loop_on, loop_start, loop_end, metronome, ntracks;
  float master;
  int nmeters;
  gr_meter meters[GR_MAX_METERS];
} song_head;

static int take (const gr_song *s, snap *out)
{
  buf b = { NULL, 0, 0, 0 };
  song_head h;
  int i;
  memset (&h, 0, sizeof h);
  h.tempo = s->tempo; h.key = s->key; h.bars = s->bars; h.loop_on = s->loop_on;
  h.loop_start = s->loop_start; h.loop_end = s->loop_end; h.metronome = s->metronome;
  h.ntracks = s->ntracks; h.master = s->master;
  h.nmeters = s->nmeters;
  memcpy (h.meters, s->meters, sizeof h.meters);
  put (&b, &h, sizeof h);
  for (i = 0; i < s->ntracks; i++)
    {
      gr_track t = s->tracks[i];
      t.meter = 0.0f;           /* (changes as it plays: not part of the song) */
      t.armed = 0;              /* (arming isn't an edit) */
      put (&b, &t, sizeof t);
    }
  for (i = 0; i < GR_MAX_REGIONS; i++)
    if (s->regions[i].used)
      {
        put_int (&b, i);
        put (&b, &s->regions[i], sizeof s->regions[i]);
      }
  put_int (&b, -1);
  for (i = 0; i < GR_MAX_PATTERNS; i++)
    if (s->patterns[i].used)
      {
        const gr_pattern *p = &s->patterns[i];
        put_int (&b, i);
        put (&b, p->name, sizeof p->name);
        put_int (&b, p->len);
        put_int (&b, p->nnotes);
        put (&b, p->notes, (size_t) p->nnotes * sizeof p->notes[0]);
      }
  put_int (&b, -1);
  for (i = 0; i < GR_MAX_CLIPS; i++)
    if (s->clips[i].used)
      {
        const gr_clip *c = &s->clips[i];
        put_int (&b, i);
        put (&b, c->name, sizeof c->name);
        put_int (&b, (int) c->id);
        put_int (&b, c->frames);
        put_int (&b, c->rate);
      }
  put_int (&b, -1);
  if (b.fail)
    {
      free (b.p);
      return -1;
    }
  out->data = b.p;
  out->size = b.n;
  return 0;
}

typedef struct { const unsigned char *p, *end; } rd;

static int get (rd *r, void *d, size_t n)
{
  if ((size_t) (r->end - r->p) < n)
    return -1;
  memcpy (d, r->p, n);
  r->p += n;
  return 0;
}

static int get_int (rd *r) { int v = -1; get (r, &v, sizeof v); return v; }

/* Put snapshot sn back into the song. Audio data stays where it is. */
static void give (const snap *sn, gr_song *s)
{
  rd r = { sn->data, sn->data + sn->size };
  song_head h;
  int i, armed[GR_MAX_TRACKS], keep_armed = s->ntracks;
  for (i = 0; i < GR_MAX_TRACKS; i++)
    armed[i] = i < s->ntracks ? s->tracks[i].armed : 0;
  get (&r, &h, sizeof h);
  s->tempo = h.tempo; s->key = h.key; s->bars = h.bars; s->loop_on = h.loop_on;
  s->loop_start = h.loop_start; s->loop_end = h.loop_end; s->metronome = h.metronome;
  s->master = h.master;
  s->nmeters = h.nmeters;
  memcpy (s->meters, h.meters, sizeof s->meters);
  s->ntracks = h.ntracks;
  for (i = 0; i < h.ntracks && i < GR_MAX_TRACKS; i++)
    {
      get (&r, &s->tracks[i], sizeof s->tracks[i]);
      s->tracks[i].armed = i < keep_armed ? armed[i] : 0;
    }
  for (i = 0; i < GR_MAX_REGIONS; i++)
    s->regions[i].used = 0;
  while ((i = get_int (&r)) >= 0 && i < GR_MAX_REGIONS)
    get (&r, &s->regions[i], sizeof s->regions[i]);
  for (i = 0; i < GR_MAX_PATTERNS; i++)
    s->patterns[i].used = 0;
  while ((i = get_int (&r)) >= 0 && i < GR_MAX_PATTERNS)
    {
      gr_pattern *p = &s->patterns[i];
      p->used = 1;
      get (&r, p->name, sizeof p->name);
      p->len = get_int (&r);
      p->nnotes = get_int (&r);
      if (p->nnotes < 0 || p->nnotes > GR_MAX_NOTES)
        p->nnotes = 0;
      get (&r, p->notes, (size_t) p->nnotes * sizeof p->notes[0]);
    }
  for (i = 0; i < GR_MAX_CLIPS; i++)
    s->clips[i].used = 0;
  while ((i = get_int (&r)) >= 0 && i < GR_MAX_CLIPS)
    {
      gr_clip *c = &s->clips[i];
      char name[GR_NAME];
      unsigned id;
      int frames, rate;
      get (&r, name, sizeof name);
      id = (unsigned) get_int (&r);
      frames = get_int (&r);
      rate = get_int (&r);
      /* Only if that recording is still in its slot. */
      if (c->id == id && c->data)
        {
          memcpy (c->name, name, sizeof name);
          c->used = 1;
          c->frames = frames < c->cap ? frames : c->cap;
          c->rate = rate;
        }
    }
  /* A region whose recording has gone can't come back. */
  for (i = 0; i < GR_MAX_REGIONS; i++)
    if (s->regions[i].used && s->regions[i].clip >= 0
        && (s->regions[i].clip >= GR_MAX_CLIPS || !s->clips[s->regions[i].clip].used))
      s->regions[i].used = 0;
}

/* ---- the history ---------------------------------------------------------- */

gr_history *gr_history_new (int max_levels, size_t max_bytes)
{
  gr_history *h = calloc (1, sizeof *h);
  if (!h)
    return NULL;
  h->max_levels = max_levels < 2 ? 2 : max_levels;
  h->max_bytes = max_bytes;
  h->saved = -1;
  h->cur = -1;
  return h;
}

static void drop (gr_history *h, int from)      /* snapshots from..n-1 */
{
  int i;
  for (i = from; i < h->n; i++)
    {
      h->bytes -= h->s[i].size;
      free (h->s[i].data);
    }
  h->n = from;
  if (h->saved >= h->n)
    h->saved = -1;
}

static void drop_oldest (gr_history *h)
{
  h->bytes -= h->s[0].size;
  free (h->s[0].data);
  memmove (h->s, h->s + 1, (size_t) (h->n - 1) * sizeof h->s[0]);
  h->n--;
  h->cur--;
  h->saved = h->saved > 0 ? h->saved - 1 : -1;
}

void gr_history_free (gr_history *h)
{
  if (!h)
    return;
  drop (h, 0);
  free (h->s);
  free (h);
}

static int push (gr_history *h, snap sn)
{
  if (h->n == h->cap)
    {
      int cap = h->cap ? h->cap * 2 : 32;
      snap *s = realloc (h->s, (size_t) cap * sizeof *s);
      if (!s)
        {
          free (sn.data);
          return -1;
        }
      h->s = s;
      h->cap = cap;
    }
  h->s[h->n++] = sn;
  h->bytes += sn.size;
  h->cur = h->n - 1;
  while (h->n > 1 && (h->n > h->max_levels || (h->max_bytes && h->bytes > h->max_bytes)))
    drop_oldest (h);
  return 0;
}

void gr_history_reset (gr_history *h, const gr_song *s)
{
  snap sn;
  drop (h, 0);
  h->cur = h->saved = -1;
  if (take (s, &sn) == 0 && push (h, sn) == 0)
    h->saved = 0;
}

int gr_history_note (gr_history *h, const gr_song *s)
{
  snap sn;
  if (take (s, &sn))
    return 0;
  if (h->cur >= 0 && h->s[h->cur].size == sn.size && !memcmp (h->s[h->cur].data, sn.data, sn.size))
    {
      free (sn.data);           /* nothing changed */
      return 0;
    }
  drop (h, h->cur + 1);         /* a new change: the redo states go */
  return push (h, sn) == 0;
}

int gr_history_can_undo (const gr_history *h) { return h->cur > 0; }
int gr_history_can_redo (const gr_history *h) { return h->cur >= 0 && h->cur < h->n - 1; }

int gr_history_undo (gr_history *h, gr_song *s)
{
  if (!gr_history_can_undo (h))
    return 0;
  give (&h->s[--h->cur], s);
  return 1;
}

int gr_history_redo (gr_history *h, gr_song *s)
{
  if (!gr_history_can_redo (h))
    return 0;
  give (&h->s[++h->cur], s);
  return 1;
}

void gr_history_saved (gr_history *h) { h->saved = h->cur; }
int gr_history_is_saved (const gr_history *h) { return h->cur >= 0 && h->cur == h->saved; }

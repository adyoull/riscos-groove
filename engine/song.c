/* Groove - the song model: tracks, patterns, regions and audio clips. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "groove.h"

static void copy_name (char *dst, const char *src)
{
  snprintf (dst, GR_NAME, "%s", src ? src : "");
}

void gr_song_init (gr_song *s)
{
  memset (s, 0, sizeof *s);
  s->tempo = 110;
  s->key = 0;
  s->bars = 16;
  s->loop_on = 0;
  s->loop_start = 0;
  s->loop_end = 4 * GR_BAR;
  s->metronome = 0;
  s->master = 0.8f;
  s->nmeters = 1;
  s->meters[0].bar = 0;
  s->meters[0].num = 4;
  s->meters[0].den = 4;
}

/* ---- time signatures ---------------------------------------------------- */

static int meter_bar_len (const gr_meter *m) { return m->num * (4 * GR_PPQ) / m->den; }

const gr_meter *gr_meter_at (const gr_song *s, int bar)
{
  static const gr_meter four = { 0, 4, 4 };
  int i;
  if (s->nmeters < 1)
    return &four;
  for (i = s->nmeters - 1; i > 0 && s->meters[i].bar > bar; i--)
    ;
  return &s->meters[i];
}

int gr_bar_len (const gr_song *s, int bar) { return meter_bar_len (gr_meter_at (s, bar < 0 ? 0 : bar)); }
int gr_beat_len (const gr_song *s, int bar) { return 4 * GR_PPQ / gr_meter_at (s, bar < 0 ? 0 : bar)->den; }

int gr_bar_tick (const gr_song *s, int bar)
{
  int i, tick = 0, at = 0;
  if (bar <= 0)
    return 0;
  for (i = 0; i < s->nmeters; i++)
    {
      int until = i + 1 < s->nmeters ? s->meters[i + 1].bar : bar;
      if (until > bar)
        until = bar;
      tick += (until - at) * meter_bar_len (&s->meters[i]);
      at = until;
      if (at >= bar)
        return tick;
    }
  return tick + (bar - at) * meter_bar_len (gr_meter_at (s, bar));
}

int gr_tick_bar (const gr_song *s, int tick)
{
  int i, at = 0, t0 = 0;
  if (tick <= 0)
    return 0;
  for (i = 0; i < s->nmeters; i++)
    {
      int len = meter_bar_len (&s->meters[i]);
      if (i + 1 < s->nmeters)
        {
          int t1 = t0 + (s->meters[i + 1].bar - at) * len;
          if (tick < t1)
            return at + (tick - t0) / len;
          t0 = t1;
          at = s->meters[i + 1].bar;
        }
      else
        return at + (tick - t0) / len;
    }
  return 0;
}

int gr_bar_snap (const gr_song *s, int tick)
{
  int b = gr_tick_bar (s, tick), t0 = gr_bar_tick (s, b), t1 = gr_bar_tick (s, b + 1);
  return tick - t0 < t1 - tick ? t0 : t1;
}

int gr_set_meter (gr_song *s, int bar, int num, int den)
{
  int i, j;
  if (bar < 0 || num < 1 || num > 32 || den < 1 || den > 64 || (den & (den - 1)))
    return -1;
  if (s->nmeters < 1)
    {
      s->nmeters = 1;
      s->meters[0].bar = 0; s->meters[0].num = 4; s->meters[0].den = 4;
    }
  for (i = 0; i < s->nmeters && s->meters[i].bar < bar; i++)
    ;
  if (i == s->nmeters || s->meters[i].bar != bar)
    {
      if (s->nmeters == GR_MAX_METERS)
        return -1;
      memmove (&s->meters[i + 1], &s->meters[i], (size_t) (s->nmeters - i) * sizeof s->meters[0]);
      s->nmeters++;
      s->meters[i].bar = bar;
    }
  s->meters[i].num = num;
  s->meters[i].den = den;
  /* A change to the meter already in force isn't a change. */
  for (i = j = 1; i < s->nmeters; i++)
    if (s->meters[i].num != s->meters[j - 1].num || s->meters[i].den != s->meters[j - 1].den)
      s->meters[j++] = s->meters[i];
  s->nmeters = j;
  return 0;
}

void gr_song_cover (gr_song *s, int end_tick)
{
  if (end_tick > 0 && gr_tick_bar (s, end_tick - 1) + 1 > s->bars)
    s->bars = gr_tick_bar (s, end_tick - 1) + 1;
}

void gr_song_free (gr_song *s)
{
  int i;
  for (i = 0; i < GR_MAX_CLIPS; i++)
    {
      free (s->clips[i].data);
      s->clips[i].data = NULL;
      s->clips[i].used = 0;
    }
}

int gr_track_add (gr_song *s, gr_track_type t, const char *name, int patch)
{
  gr_track *tr;
  if (s->ntracks >= GR_MAX_TRACKS)
    return -1;
  tr = &s->tracks[s->ntracks];
  memset (tr, 0, sizeof *tr);
  copy_name (tr->name, name);
  tr->type = t;
  tr->patch = patch;
  tr->gm = -1;
  tr->vol = 0.8f;
  tr->pan = 0.0f;
  tr->reverb = t == TRK_DRUMS ? 0.08f : 0.18f;
  return s->ntracks++;
}

/* Move a track from one place in the list to another; its regions go with it. */
void gr_track_move (gr_song *s, int from, int to)
{
  gr_track t;
  int i;
  if (from < 0 || from >= s->ntracks || to < 0 || to >= s->ntracks || from == to)
    return;
  t = s->tracks[from];
  if (from < to)
    memmove (&s->tracks[from], &s->tracks[from + 1], (size_t) (to - from) * sizeof t);
  else
    memmove (&s->tracks[to + 1], &s->tracks[to], (size_t) (from - to) * sizeof t);
  s->tracks[to] = t;
  for (i = 0; i < GR_MAX_REGIONS; i++)
    if (s->regions[i].used)
      {
        int r = s->regions[i].track;
        if (r == from)
          r = to;
        else if (from < to && r > from && r <= to)
          r--;
        else if (from > to && r >= to && r < from)
          r++;
        s->regions[i].track = r;
      }
}

void gr_track_delete (gr_song *s, int track)
{
  int i;
  if (track < 0 || track >= s->ntracks)
    return;
  for (i = 0; i < GR_MAX_REGIONS; i++)
    if (s->regions[i].used)
      {
        if (s->regions[i].track == track)
          s->regions[i].used = 0;
        else if (s->regions[i].track > track)
          s->regions[i].track--;
      }
  memmove (&s->tracks[track], &s->tracks[track + 1],
           (size_t) (s->ntracks - track - 1) * sizeof s->tracks[0]);
  s->ntracks--;
  gr_song_gc (s);
}

int gr_pattern_new (gr_song *s, const char *name, int len)
{
  int i;
  for (i = 0; i < GR_MAX_PATTERNS; i++)
    if (!s->patterns[i].used)
      {
        gr_pattern *p = &s->patterns[i];
        memset (p, 0, sizeof *p);
        p->used = 1;
        p->len = len > 0 ? len : GR_BAR;
        copy_name (p->name, name);
        return i;
      }
  return -1;
}

/* Insert keeping the notes sorted by tick, so the sequencer can stop
   scanning early. Returns the index, or -1 if full. */
int gr_pattern_add_note (gr_pattern *p, int tick, int len, int pitch, int vel)
{
  int i;
  if (p->nnotes >= GR_MAX_NOTES || tick < 0 || tick >= p->len)
    return -1;
  if (pitch < 0) pitch = 0;
  if (pitch > 127) pitch = 127;
  if (vel < 1) vel = 1;
  if (vel > 127) vel = 127;
  if (len < 1) len = 1;
  for (i = p->nnotes; i > 0 && p->notes[i - 1].tick > tick; i--)
    p->notes[i] = p->notes[i - 1];
  p->notes[i].tick = (int16_t) tick;
  p->notes[i].len = (int16_t) len;
  p->notes[i].pitch = (uint8_t) pitch;
  p->notes[i].vel = (uint8_t) vel;
  p->nnotes++;
  return i;
}

int gr_pattern_find_note (const gr_pattern *p, int tick, int pitch)
{
  int i;
  for (i = 0; i < p->nnotes; i++)
    if (p->notes[i].pitch == pitch && tick >= p->notes[i].tick
        && tick < p->notes[i].tick + (p->notes[i].len > 0 ? p->notes[i].len : 1))
      return i;
  return -1;
}

void gr_pattern_delete_note (gr_pattern *p, int index)
{
  if (index < 0 || index >= p->nnotes)
    return;
  memmove (&p->notes[index], &p->notes[index + 1],
           (size_t) (p->nnotes - index - 1) * sizeof p->notes[0]);
  p->nnotes--;
}

int gr_region_add (gr_song *s, int track, int start, int len, int pattern)
{
  int i;
  if (track < 0 || track >= s->ntracks || len <= 0)
    return -1;
  if (start < 0)
    start = 0;
  for (i = 0; i < GR_MAX_REGIONS; i++)
    if (!s->regions[i].used)
      {
        gr_region *r = &s->regions[i];
        memset (r, 0, sizeof *r);
        r->used = 1;
        r->track = track;
        r->start = start;
        r->len = len;
        r->pattern = pattern;
        r->clip = -1;
        gr_song_cover (s, start + len);
        return i;
      }
  return -1;
}

void gr_region_delete (gr_song *s, int region)
{
  if (region >= 0 && region < GR_MAX_REGIONS)
    s->regions[region].used = 0;
  gr_song_gc (s);
}

int gr_region_at (const gr_song *s, int track, int tick)
{
  int i;
  for (i = GR_MAX_REGIONS - 1; i >= 0; i--)   /* newest wins where they overlap */
    {
      const gr_region *r = &s->regions[i];
      if (r->used && r->track == track && tick >= r->start && tick < r->start + r->len)
        return i;
    }
  return -1;
}

/* Regions dropped from the same loop share nothing (gr_loop_place makes a
   new pattern each time), but a copied region shares its pattern. Before an
   edit, give the region its own copy so the edit doesn't surprise the user
   elsewhere in the song. */
int gr_pattern_unshare (gr_song *s, int region)
{
  int i, users = 0, p, np;
  gr_region *r = &s->regions[region];
  p = r->pattern;
  if (p < 0)
    return p;
  for (i = 0; i < GR_MAX_REGIONS; i++)
    if (s->regions[i].used && s->regions[i].pattern == p && s->regions[i].clip < 0)
      users++;
  if (users <= 1)
    return p;
  np = gr_pattern_new (s, s->patterns[p].name, s->patterns[p].len);
  if (np < 0)
    return p;
  memcpy (&s->patterns[np], &s->patterns[p], sizeof s->patterns[p]);
  r->pattern = np;
  return np;
}

/* Split a region at song tick `tick` (strictly inside it). The right part
   keeps sounding exactly as before: for notes, it gets the pattern rotated
   to start where the split falls (or shares it, if the split falls on a
   repeat); for audio, it starts that much further into the recording.
   Returns the new (right-hand) region, or -1. */
int gr_region_split (gr_song *s, int region, int tick)
{
  gr_region *r, *nr;
  int n, off;
  if (region < 0 || region >= GR_MAX_REGIONS || !s->regions[region].used)
    return -1;
  r = &s->regions[region];
  if (tick <= r->start || tick >= r->start + r->len)
    return -1;
  n = gr_region_add (s, r->track, tick, r->start + r->len - tick, r->pattern);
  if (n < 0)
    return -1;
  r = &s->regions[region];
  nr = &s->regions[n];
  off = tick - r->start;
  if (r->clip >= 0)
    {
      double spt = GR_RATE * 60.0 / ((double) s->tempo * GR_PPQ);
      nr->clip = r->clip;
      nr->clip_offset = r->clip_offset
                        + (int) (off * spt * s->clips[r->clip].rate / GR_RATE + 0.5);
      nr->pattern = -1;
    }
  else if (r->pattern >= 0 && off % s->patterns[r->pattern].len)
    {
      const gr_pattern *src = &s->patterns[r->pattern];
      int len = src->len, rot = off % len, k;
      int np = gr_pattern_new (s, src->name, len);
      if (np < 0)
        {
          nr->used = 0;
          return -1;
        }
      src = &s->patterns[r->pattern];
      for (k = 0; k < src->nnotes; k++)
        {
          const gr_note *x = &src->notes[k];
          gr_pattern_add_note (&s->patterns[np], (x->tick - rot + len) % len, x->len, x->pitch, x->vel);
        }
      nr->pattern = np;
    }
  r->len = off;
  return n;
}

/* A copy of region `region` at `tick` on `track` (which must be of the same
   kind), with a pattern of its own. Returns the new region, or -1. */
int gr_region_copy (gr_song *s, const gr_region *src_r, const gr_pattern *src_p, int track, int tick)
{
  int n, np = -1;
  if (track < 0 || track >= s->ntracks)
    return -1;
  if (src_r->clip < 0)
    {
      if (!src_p || (np = gr_pattern_new (s, src_p->name, src_p->len)) < 0)
        return -1;
      memcpy (&s->patterns[np], src_p, sizeof *src_p);
      s->patterns[np].used = 1;
    }
  n = gr_region_add (s, track, tick, src_r->len, np);
  if (n < 0)
    {
      if (np >= 0)
        s->patterns[np].used = 0;
      return -1;
    }
  if (src_r->clip >= 0)
    {
      s->regions[n].clip = src_r->clip;
      s->regions[n].clip_offset = src_r->clip_offset;
      s->clips[src_r->clip].used = 1;     /* (its audio may have been kept only for Undo) */
    }
  return n;
}

int gr_clip_new (gr_song *s, const char *name, int rate)
{
  static unsigned next_id = 1;
  int i, pass;
  /* A slot never used first; then one whose audio is only kept for Undo. */
  for (pass = 0; pass < 2; pass++)
    for (i = 0; i < GR_MAX_CLIPS; i++)
      if (!s->clips[i].used && (pass || !s->clips[i].data))
      {
        gr_clip *c = &s->clips[i];
        free (c->data);
        memset (c, 0, sizeof *c);
        c->used = 1;
        c->id = next_id++;
        c->rate = rate;
        copy_name (c->name, name);
        return i;
      }
  return -1;
}

int gr_clip_append (gr_clip *c, const int16_t *stereo, int frames)
{
  if (c->frames + frames > c->cap)
    {
      int cap = c->cap ? c->cap * 2 : GR_RATE * 4;
      int16_t *d;
      while (cap < c->frames + frames)
        cap *= 2;
      d = realloc (c->data, (size_t) cap * 2 * sizeof (int16_t));
      if (!d)
        return -1;
      c->data = d;
      c->cap = cap;
    }
  memcpy (c->data + 2 * c->frames, stereo, (size_t) frames * 2 * sizeof (int16_t));
  c->frames += frames;
  return 0;
}

void gr_song_gc (gr_song *s)
{
  int i, j;
  for (i = 0; i < GR_MAX_PATTERNS; i++)
    if (s->patterns[i].used)
      {
        for (j = 0; j < GR_MAX_REGIONS; j++)
          if (s->regions[j].used && s->regions[j].clip < 0 && s->regions[j].pattern == i)
            break;
        if (j == GR_MAX_REGIONS)
          s->patterns[i].used = 0;
      }
  for (i = 0; i < GR_MAX_CLIPS; i++)
    if (s->clips[i].used)
      {
        for (j = 0; j < GR_MAX_REGIONS; j++)
          if (s->regions[j].used && s->regions[j].clip == i)
            break;
        if (j == GR_MAX_REGIONS)
          s->clips[i].used = 0;   /* the audio stays, for Undo, until the slot is reused */
      }
}

int gr_song_end (const gr_song *s)
{
  int i, end = 0;
  for (i = 0; i < GR_MAX_REGIONS; i++)
    if (s->regions[i].used && s->regions[i].start + s->regions[i].len > end)
      end = s->regions[i].start + s->regions[i].len;
  return end;
}

/* ---- names ---------------------------------------------------------- */
static const char *const patch_names[PATCH_COUNT] =
  { "Electric Piano", "Synth Bass", "Warm Pad", "Square Lead",
    "Plucked Guitar", "Drawbar Organ", "Glass Bells" };
static const char *const kit_names[KIT_COUNT] = { "Studio Kit", "Electro Kit" };
static const char *const drum_names[GR_DRUM_SOUNDS] =
  { "Kick", "Snare", "Hi-hat", "Open hat", "Clap", "Low tom", "High tom", "Cowbell" };
static const char *const notes[12] =
  { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

const char *gr_patch_name (int p) { return p >= 0 && p < PATCH_COUNT ? patch_names[p] : "?"; }
const char *gr_kit_name (int k) { return k >= 0 && k < KIT_COUNT ? kit_names[k] : "?"; }
const char *gr_drum_name (int d) { return d >= 0 && d < GR_DRUM_SOUNDS ? drum_names[d] : "?"; }
const char *gr_key_name (int k) { return notes[((k % 12) + 12) % 12]; }

const char *gr_note_name (int pitch, char buf[8])
{
  pitch &= 127;
  snprintf (buf, 8, "%s%d", notes[pitch % 12], pitch / 12 - 1);
  return buf;
}

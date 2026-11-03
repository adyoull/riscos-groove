/* Groove - Standard MIDI Files: import into the song, export the song.

   Import keeps the file's own timing (its ticks scaled to Groove's 96 per
   beat) rather than playing it through the synth and listening, so what
   arrives is notes you can edit: a track per MIDI channel, cut into
   4-bar regions, with SoundFont sounds from the file's
   program changes and Groove sounds as the fallback. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "groove.h"
#include "place.h"


/* ---- reading ------------------------------------------------------------------ */

typedef gr_pnote mnote;

typedef struct
{
  mnote *notes;
  int n, cap;
  int program;                  /* first program change, or -1 */
  int open_start[128], open_vel[128];   /* start tick + 1 of a held key, 0 = none */
} mchan;

typedef struct
{
  const unsigned char *p, *end;
} rd;

static int vlq (rd *r, unsigned long *v)
{
  int i;
  *v = 0;
  for (i = 0; i < 4 && r->p < r->end; i++)
    {
      unsigned c = *r->p++;
      *v = (*v << 7) | (c & 0x7F);
      if (!(c & 0x80))
        return 0;
    }
  return -1;
}

static unsigned long be32 (const unsigned char *p)
{ return ((unsigned long) p[0] << 24) | ((unsigned long) p[1] << 16) | (p[2] << 8) | p[3]; }

static int add_note (mchan *c, int start, int end, int key, int vel)
{
  if (c->n == c->cap)
    {
      int cap = c->cap ? c->cap * 2 : 256;
      mnote *m = realloc (c->notes, (size_t) cap * sizeof *m);
      if (!m)
        return -1;
      c->notes = m;
      c->cap = cap;
    }
  c->notes[c->n].start = start;
  c->notes[c->n].end = end > start ? end : start + 1;
  c->notes[c->n].key = key;
  c->notes[c->n].vel = vel;
  c->n++;
  return 0;
}

static void key_off (mchan *c, int key, long tick)
{
  if (c->open_start[key])
    {
      add_note (c, c->open_start[key] - 1, (int) tick, key, c->open_vel[key]);
      c->open_start[key] = 0;
    }
}

/* Time signatures found (file ticks), for an import into an empty song. */
static struct { long tick; int num, den; } ts[GR_MAX_METERS];
static int nts;

/* One MTrk chunk. Times stay in the file's ticks. */
static int read_track (rd *r, mchan *ch, long *tempo, long *last)
{
  long tick = 0;
  int status = 0;
  while (r->p < r->end)
    {
      unsigned long dt, len;
      int st, a, b;
      if (vlq (r, &dt))
        return -1;
      tick += (long) dt;
      if (r->p >= r->end)
        return -1;
      st = *r->p;
      if (st & 0x80)
        r->p++;
      else if (status)
        st = status;            /* running status */
      else
        return -1;
      if (st == 0xFF)           /* meta event */
        {
          int type;
          if (r->p >= r->end)
            return -1;
          type = *r->p++;
          if (vlq (r, &len) || len > (unsigned long) (r->end - r->p))
            return -1;
          if (type == 0x51 && len == 3 && *tempo == 0)
            *tempo = ((long) r->p[0] << 16) | (r->p[1] << 8) | r->p[2];
          if (type == 0x58 && len >= 2 && nts < GR_MAX_METERS && r->p[1] <= 6)
            {
              ts[nts].tick = tick;
              ts[nts].num = r->p[0];
              ts[nts].den = 1 << r->p[1];
              nts++;
            }
          r->p += len;
          if (type == 0x2F)
            break;
          continue;
        }
      if (st == 0xF0 || st == 0xF7)   /* SysEx: skipped */
        {
          if (vlq (r, &len) || len > (unsigned long) (r->end - r->p))
            return -1;
          r->p += len;
          status = 0;
          continue;
        }
      status = st;
      a = r->p < r->end ? *r->p++ & 127 : 0;
      b = 0;
      if ((st & 0xF0) != 0xC0 && (st & 0xF0) != 0xD0)
        b = r->p < r->end ? *r->p++ & 127 : 0;
      {
        mchan *c = &ch[st & 15];
        switch (st & 0xF0)
          {
          case 0x90:
            if (b > 0)
              {
                key_off (c, a, tick);        /* a key struck again */
                c->open_start[a] = (int) tick + 1;
                c->open_vel[a] = b;
                break;
              }
            /* velocity 0 is a note-off */
            /* fall through */
          case 0x80:
            key_off (c, a, tick);
            break;
          case 0xC0:
            if (c->program < 0)
              c->program = a;
            break;
          }
      }
      if (tick > *last)
        *last = tick;
    }
  return 0;
}

/* Groove's own sound nearest a GM program, for when there's no SoundFont. */
static int nearest_patch (int program)
{
  static const int by_family[16] = {
    PATCH_EPIANO, PATCH_BELL, PATCH_ORGAN, PATCH_PLUCK, PATCH_BASS, PATCH_PAD,
    PATCH_PAD, PATCH_LEAD, PATCH_LEAD, PATCH_LEAD, PATCH_LEAD, PATCH_PAD,
    PATCH_PAD, PATCH_PLUCK, PATCH_BELL, PATCH_PAD };
  return by_family[(program & 127) / 8];
}

/* A GM drum key's row in the step grid. */
int gr_drum_row_for_key (int key)
{
  switch (key)
    {
    case 35: case 36: return DR_KICK;
    case 37: case 38: case 40: return DR_SNARE;
    case 39: return DR_CLAP;
    case 42: case 44: return DR_CHAT;
    case 46: return DR_OHAT;
    case 41: case 43: case 45: case 47: return DR_LTOM;
    case 48: case 50: return DR_HTOM;
    default: return DR_PERC;  /* cymbals, cowbell, the rest of the kit */
    }
}

int gr_midi_import (gr_song *s, const char *path, int from, const char **why)
{
  static char msg[96];
  FILE *f = fopen (path, "rb");
  unsigned char *data = NULL;
  long size;
  mchan *ch = NULL;
  int division, ntrk, i, c, made = 0, empty = gr_song_end (s) == 0, full_at = -1, dropped = 0;
  long tempo = 0, last = 0;
  rd r;
  *why = NULL;
  if (!f)
    {
      *why = "Groove couldn't open that file";
      return -1;
    }
  fseek (f, 0, SEEK_END);
  size = ftell (f);
  fseek (f, 0, SEEK_SET);
  if (size < 14 || size > 8 * 1024 * 1024 || !(data = malloc ((size_t) size))
      || fread (data, 1, (size_t) size, f) != (size_t) size)
    {
      fclose (f);
      free (data);
      *why = "That isn't a MIDI file Groove can read";
      return -1;
    }
  fclose (f);
  if (memcmp (data, "MThd", 4) || be32 (data + 4) < 6)
    {
      free (data);
      *why = "That isn't a MIDI file";
      return -1;
    }
  ntrk = (data[10] << 8) | data[11];
  division = (data[12] << 8) | data[13];
  if (division & 0x8000 || division == 0)
    {
      free (data);
      *why = "That MIDI file counts time in SMPTE frames, which Groove can't read";
      return -1;
    }
  ch = calloc (16, sizeof *ch);
  if (!ch)
    {
      free (data);
      *why = "Not enough memory";
      return -1;
    }
  for (c = 0; c < 16; c++)
    ch[c].program = -1;
  nts = 0;
  r.p = data + 8 + be32 (data + 4);
  r.end = data + size;
  for (i = 0; i < ntrk && r.end - r.p >= 8; i++)
    {
      unsigned long len = be32 (r.p + 4);
      rd t;
      int is_trk = !memcmp (r.p, "MTrk", 4);
      r.p += 8;
      if (len > (unsigned long) (r.end - r.p))
        len = (unsigned long) (r.end - r.p);
      t.p = r.p;
      t.end = r.p + len;
      if (is_trk)
        {
          read_track (&t, ch, &tempo, &last);   /* (a damaged track keeps what it had) */
          for (c = 0; c < 16; c++)
            {
              int k;
              for (k = 0; k < 128; k++)
                key_off (&ch[c], k, last);
            }
        }
      r.p += len;
    }
  free (data);

  if (empty && nts)
    {
      /* The file's time signatures, in order, each from the bar it falls
         in (or the next, if it isn't on a bar line). */
      int a, z;
      for (a = 1; a < nts; a++)
        for (z = a; z > 0 && ts[z].tick < ts[z - 1].tick; z--)
          {
            long tt = ts[z].tick; int nn = ts[z].num, dd = ts[z].den;
            ts[z] = ts[z - 1];
            ts[z - 1].tick = tt; ts[z - 1].num = nn; ts[z - 1].den = dd;
          }
      for (a = 0; a < nts; a++)
        {
          int tk = from + (int) ((long long) ts[a].tick * GR_PPQ / division);
          int bar = gr_tick_bar (s, tk);
          if (gr_bar_tick (s, bar) != tk)
            bar++;
          gr_set_meter (s, bar, ts[a].num, ts[a].den);
        }
    }
  if (empty && tempo > 0)
    {
      long bpm = (60000000L + tempo / 2) / tempo;
      s->tempo = bpm < 40 ? 40 : bpm > 240 ? 240 : (int) bpm;
    }
  /* Into a song with no music yet, the parts replace its empty tracks
     (a new song's Drums and Electric Piano), so a 16-part file fits. */
  if (empty)
    {
      int any = 0;
      for (c = 0; c < 16; c++)
        any |= ch[c].n > 0;
      while (any && s->ntracks)
        gr_track_delete (s, s->ntracks - 1);
    }
  for (c = 0; c < 16 && full_at < 0; c++)
    {
      mchan *m = &ch[c];
      int t, k, drums = c == 9, prog = m->program < 0 ? 0 : m->program;
      char name[GR_NAME];
      if (!m->n)
        continue;
      snprintf (name, sizeof name, "%s", drums ? "Drums" : gr_gm_program_name (prog));
      t = gr_track_add (s, drums ? TRK_DRUMS : TRK_INST, name, drums ? KIT_STUDIO : nearest_patch (prog));
      if (t < 0)
        {
          snprintf (msg, sizeof msg, "Groove has room for %d tracks, so some parts weren't imported",
                    GR_MAX_TRACKS);
          *why = msg;
          break;
        }
      s->tracks[t].gm = drums ? 0 : prog;
      if (drums && m->program >= 0)
        s->tracks[t].gm = m->program;
      /* The file's ticks to Groove's. */
      for (k = 0; k < m->n; k++)
        {
          m->notes[k].start = (int) ((long long) m->notes[k].start * GR_PPQ / division);
          m->notes[k].end = (int) ((long long) m->notes[k].end * GR_PPQ / division);
          if (drums)
            m->notes[k].key = gr_drum_row_for_key (m->notes[k].key);
        }
      made++;
      if (gr_place_notes (s, t, m->notes, m->n, from, &full_at, &dropped))
        {
          snprintf (msg, sizeof msg, "The song is full: the MIDI file was imported up to bar %d",
                    gr_tick_bar (s, full_at) + 1);
          *why = msg;
        }
    }
  if (!*why && dropped)
    {
      snprintf (msg, sizeof msg, "%d notes didn't fit and were left out", dropped);
      *why = msg;
    }
  if (!made && !*why)
    *why = "That MIDI file has no notes";
  for (c = 0; c < 16; c++)
    free (ch[c].notes);
  free (ch);
  return made ? made : -1;
}

/* ---- writing ------------------------------------------------------------------ */

typedef struct { int tick, order; unsigned char b[3]; } mev;

typedef struct
{
  unsigned char *d;
  size_t n, cap;
} buf;

static int put (buf *b, const void *p, size_t n)
{
  if (b->n + n > b->cap)
    {
      size_t cap = b->cap ? b->cap * 2 : 4096;
      unsigned char *d;
      while (cap < b->n + n)
        cap *= 2;
      if (!(d = realloc (b->d, cap)))
        return -1;
      b->d = d;
      b->cap = cap;
    }
  memcpy (b->d + b->n, p, n);
  b->n += n;
  return 0;
}

static int put_vlq (buf *b, unsigned long v)
{
  unsigned char t[5];
  int n = 0, i;
  t[n++] = v & 0x7F;
  while ((v >>= 7))
    t[n++] = 0x80 | (v & 0x7F);
  for (i = n - 1; i >= 0; i--)
    if (put (b, &t[i], 1))
      return -1;
  return 0;
}

static int cmp_ev (const void *a, const void *b)
{
  const mev *x = a, *y = b;
  if (x->tick != y->tick)
    return x->tick < y->tick ? -1 : 1;
  return x->order - y->order;      /* note-offs before note-ons at the same tick */
}

/* Groove's own sounds as GM programs, for other programs to play. */
int gr_patch_program (int patch)
{
  static const int gm[PATCH_COUNT] = { 4, 38, 89, 80, 24, 16, 14 };
  return patch >= 0 && patch < PATCH_COUNT ? gm[patch] : 0;
}

static int write_chunk (FILE *f, const buf *b)
{
  unsigned char h[8] = { 'M', 'T', 'r', 'k',
                         (unsigned char) (b->n >> 24), (unsigned char) (b->n >> 16),
                         (unsigned char) (b->n >> 8), (unsigned char) b->n };
  return fwrite (h, 1, 8, f) != 8 || fwrite (b->d, 1, b->n, f) != b->n ? -1 : 0;
}

int gr_midi_export (const gr_song *s, const char *path)
{
  FILE *f;
  buf b = { 0, 0, 0 };
  mev *ev = NULL;
  int t, ntracks = 1, rc = 0, chan_of[GR_MAX_TRACKS];
  unsigned long us = 60000000UL / (unsigned long) s->tempo;
  unsigned char h[14] = { 'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 0, 0, GR_PPQ };
  for (t = 0; t < s->ntracks; t++)
    if (s->tracks[t].type != TRK_AUDIO)
      ntracks++;
  h[11] = (unsigned char) ntracks;
  if (!(f = fopen (path, "wb")))
    return -1;
  fwrite (h, 1, 14, f);
  {
    /* The tempo track: tempo, the time signatures where they change, end. */
    const unsigned char tr[] = { 0, 0xFF, 0x51, 3, (unsigned char) (us >> 16), (unsigned char) (us >> 8),
                                 (unsigned char) us };
    int m, now = 0;
    b.n = 0;
    rc |= put (&b, tr, sizeof tr);
    for (m = 0; m < s->nmeters || m == 0; m++)
      {
        const gr_meter *mt = s->nmeters ? &s->meters[m] : gr_meter_at (s, 0);
        int at = gr_bar_tick (s, mt->bar), dd = 0;
        unsigned char ev[7];
        while ((1 << dd) < mt->den)
          dd++;
        ev[0] = 0xFF; ev[1] = 0x58; ev[2] = 4;
        ev[3] = (unsigned char) mt->num; ev[4] = (unsigned char) dd;
        ev[5] = (unsigned char) (96 / mt->den > 0 ? 96 / mt->den : 1);   /* MIDI clocks a beat */
        ev[6] = 8;
        rc |= put_vlq (&b, (unsigned long) (at - now));
        rc |= put (&b, ev, 7);
        now = at;
      }
    {
      const unsigned char end[] = { 0, 0xFF, 0x2F, 0 };
      rc |= put (&b, end, sizeof end);
    }
    rc |= write_chunk (f, &b);
  }
  /* MIDI channels: drums on 10 (9 here), each instrument track the next
     free one of the other 15. (Only a song of 16 instrument tracks runs
     out: its last shares channel 16.) */
  {
    int next = 0;
    for (t = 0; t < s->ntracks; t++)
      if (s->tracks[t].type == TRK_INST)
        {
          if (next == 9)
            next++;
          chan_of[t] = next < 16 ? next++ : 15;
        }
  }
  for (t = 0; t < s->ntracks && !rc; t++)
    {
      const gr_track *tr = &s->tracks[t];
      int drums = tr->type == TRK_DRUMS, chn = drums ? 9 : chan_of[t], nev = 0, cap = 0, i, now = 0, len;
      unsigned char hd[8];
      if (tr->type == TRK_AUDIO)
        continue;
      for (i = 0; i < GR_MAX_REGIONS; i++)
        {
          const gr_region *r = &s->regions[i];
          const gr_pattern *p;
          int rep;
          if (!r->used || r->track != t || r->pattern < 0 || r->clip >= 0)
            continue;
          p = &s->patterns[r->pattern];
          if (p->len <= 0)
            continue;
          for (rep = 0; rep * p->len < r->len; rep++)
            {
              int k;
              for (k = 0; k < p->nnotes; k++)
                {
                  const gr_note *n = &p->notes[k];
                  int on = r->start + rep * p->len + n->tick, off;
                  if (on >= r->start + r->len)
                    continue;
                  off = on + (drums ? GR_STEP / 2 : n->len);
                  if (off > r->start + r->len)
                    off = r->start + r->len;
                  if (nev + 2 > cap)
                    {
                      mev *m;
                      cap = cap ? cap * 2 : 1024;
                      if (!(m = realloc (ev, (size_t) cap * sizeof *m)))
                        {
                          rc = -1;
                          goto done;
                        }
                      ev = m;
                    }
                  ev[nev].tick = on;
                  ev[nev].order = 1;
                  ev[nev].b[0] = (unsigned char) (0x90 | chn);
                  ev[nev].b[1] = (unsigned char) (drums ? gr_gm_drum_note (n->pitch % GR_DRUM_SOUNDS) : n->pitch & 127);
                  ev[nev].b[2] = n->vel ? n->vel : 1;
                  ev[nev + 1] = ev[nev];
                  ev[nev + 1].tick = off;
                  ev[nev + 1].order = 0;
                  ev[nev + 1].b[0] = (unsigned char) (0x80 | chn);
                  ev[nev + 1].b[2] = 64;
                  nev += 2;
                }
            }
        }
      qsort (ev, (size_t) nev, sizeof *ev, cmp_ev);
      b.n = 0;
      /* Name, sound, volume, pan. */
      len = (int) strlen (tr->name);
      hd[0] = 0; hd[1] = 0xFF; hd[2] = 0x03; hd[3] = (unsigned char) len;
      rc |= put (&b, hd, 4);
      rc |= put (&b, tr->name, (size_t) len);
      hd[0] = 0; hd[1] = (unsigned char) (0xC0 | chn);
      hd[2] = (unsigned char) (tr->gm >= 0 ? tr->gm : drums ? (tr->patch == KIT_ELECTRO ? GM_KIT_808 : 0)
                               : gr_patch_program (tr->patch));
      rc |= put (&b, hd, 3);
      hd[0] = 0; hd[1] = (unsigned char) (0xB0 | chn); hd[2] = 7;
      hd[3] = (unsigned char) (tr->vol * 127.0f + 0.5f > 127 ? 127 : tr->vol * 127.0f + 0.5f);
      rc |= put (&b, hd, 4);
      hd[0] = 0; hd[1] = (unsigned char) (0xB0 | chn); hd[2] = 10;
      hd[3] = (unsigned char) ((tr->pan + 1.0f) * 63.5f > 127 ? 127 : (tr->pan + 1.0f) * 63.5f);
      rc |= put (&b, hd, 4);
      for (i = 0; i < nev; i++)
        {
          rc |= put_vlq (&b, (unsigned long) (ev[i].tick - now));
          rc |= put (&b, ev[i].b, 3);
          now = ev[i].tick;
        }
      hd[0] = 0; hd[1] = 0xFF; hd[2] = 0x2F; hd[3] = 0;
      rc |= put (&b, hd, 4);
      if (!rc)
        rc = write_chunk (f, &b);
    }
done:
  free (ev);
  free (b.d);
  if (fclose (f))
    rc = -1;
  return rc ? -1 : 0;
}

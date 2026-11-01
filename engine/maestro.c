/* Groove - importing Maestro music files (RISC OS file type &AF1).

   The format: "Maestro\n", a version byte (1 or 2), then blocks, each a
   type byte and its data (version 1 files have them in a fixed order):
     1  the music: a "gate" array and eight note queues, one per channel;
     2  the staves: how many (0-3 meaning 1-4) and a percussion flag;
     3  each channel's voice (pairs of channel, voice number);
     4  each channel's volume (0-7);
     5  each channel's stereo position (0-6);
     6  the tempo (an index into Maestro's table).
   Integers are BBC BASIC's: &40 and four bytes, most significant first.

   A gate is a byte saying which channels' next note or rest sounds at
   this point, or 0 and an attribute byte: a time signature, key
   signature, clef, slur, octave shift or bar line. A note is two bytes:
   its stave line (0 = a rest), a tie, then its accidental, dots and type
   (breve to hemidemisemiquaver).

   This follows what Maestro itself plays (its !RunImage, read for the
   purpose), not just what the format can say:
   - pitch: the stave line plus the clef's offset, then the accidental if
     there is one, else one earlier in the bar on that same line of that
     stave, else the key signature. The percussion stave has no clef or key;
   - timing: each stave keeps its own time; a gate sounds at the latest
     time of the staves its channels are on, and moves each of those
     staves on by its shortest note or rest there. Every bar starts on
     time, one time signature's length after the last, whatever its
     notes add up to. A time signature sets the length of the bar it's in;
   - a tied note sounds once, for all the tied notes' lengths together;
   - each channel's volume and stereo position; the tempo in crotchets a
     minute; every note at full length. Slurs and octave-shift marks
     don't change what Maestro plays, so they don't here either. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "groove.h"
#include "place.h"

#define NCH 8

/* The voices a RISC OS 5 machine has to begin with, by voice number. */
static const char *const default_voices[] = {
  "WaveSynth-Beep", "StringLib-Soft", "StringLib-Pluck", "StringLib-Steel",
  "StringLib-Hard", "Percussion-Soft", "Percussion-Medium", "Percussion-Snare",
  "Percussion-Noise"
};

static const int tempo_table[15] = { 40, 50, 60, 65, 70, 80, 90, 100, 115, 130, 145, 160, 175, 190, 210 };

typedef struct
{
  const unsigned char *gates;
  int ngates;
  const unsigned char *q[NCH];
  int qlen[NCH];                /* bytes */
  int staves, perc;             /* music staves (1-4), percussion stave (0/1) */
  int voice[NCH], vol[NCH], stereo[NCH], tempo;
  int have_music;
} mfile;

static int basic_int (const unsigned char *d, int n, int *p, int *v)
{
  if (*p + 5 > n || d[*p] != 0x40)
    return -1;
  *v = (d[*p + 1] << 24) | (d[*p + 2] << 16) | (d[*p + 3] << 8) | d[*p + 4];
  *p += 5;
  return 0;
}

static int read_music (const unsigned char *d, int n, int *p, mfile *m)
{
  int c;
  if (basic_int (d, n, p, &m->ngates) || m->ngates < 0)
    return -1;
  for (c = 0; c < NCH; c++)
    if (basic_int (d, n, p, &m->qlen[c]) || m->qlen[c] < 0)
      return -1;
  if (m->ngates > n - *p)
    return -1;
  m->gates = d + *p;
  *p += m->ngates;
  for (c = 0; c < NCH; c++)
    {
      if (m->qlen[c] > n - *p)
        return -1;
      m->q[c] = d + *p;
      *p += m->qlen[c];
    }
  m->have_music = 1;
  return 0;
}

static int parse (const unsigned char *d, int n, mfile *m)
{
  int p = 9, c;
  memset (m, 0, sizeof *m);
  m->staves = 1;
  for (c = 0; c < NCH; c++)
    {
      m->voice[c] = 1;
      m->vol[c] = 6;            /* (Maestro's own defaults) */
      m->stereo[c] = 3;
    }
  m->tempo = 6;
  if (n < 9 || memcmp (d, "Maestro\n", 8))
    return -1;
  if (d[8] == 1)
    {
      /* Version 1: tempo, voices, staves, music, in that order. */
      if (p + 1 + 16 + 2 > n)
        return -1;
      m->tempo = d[p++];
      for (c = 0; c < NCH; c++, p += 2)
        m->voice[c] = d[p + 1];
      m->staves = d[p++] + 1;
      m->perc = d[p++];
      return read_music (d, n, &p, m);
    }
  if (d[8] != 2)
    return -1;
  while (p < n)
    {
      int type = d[p++];
      switch (type)
        {
        case 1:
          if (read_music (d, n, &p, m))
            return -1;
          break;
        case 2:
          if (p + 2 > n) return -1;
          m->staves = d[p] + 1;
          m->perc = d[p + 1];
          p += 2;
          break;
        case 3:
          if (p + 16 > n) return -1;
          for (c = 0; c < NCH; c++)
            m->voice[c] = d[p + 2 * c + 1];
          p += 16;
          break;
        case 4:
          if (p + 8 > n) return -1;
          for (c = 0; c < NCH; c++)
            m->vol[c] = d[p + c] > 7 ? 7 : d[p + c];
          p += 8;
          break;
        case 5:
          if (p + 8 > n) return -1;
          for (c = 0; c < NCH; c++)
            m->stereo[c] = d[p + c] > 6 ? 6 : d[p + c];
          p += 8;
          break;
        case 6:
          if (p + 1 > n) return -1;
          m->tempo = d[p++];
          break;
        default:
          p = n;                /* blocks Maestro doesn't know: it stops reading too */
          break;
        }
    }
  if (m->staves > 4)
    m->staves = 4;
  if (m->tempo > 14)
    m->tempo = 14;
  return m->have_music ? 0 : -1;
}

/* Maestro's channel to stave mapping (block 2). */
static int stave_of (const mfile *m, int c)
{
  static const int three[NCH] = { 0, 1, 1, 1, 1, 2, 2, 2 };
  if (m->perc && c == NCH - 1)
    return m->staves;           /* the percussion stave, after the music staves */
  if (m->staves == 3)
    return three[c];
  return m->staves * c / NCH;
}

/* A note or rest's length in Maestro's units (a crotchet is 128): type
   0-7 is breve to hemidemisemiquaver; each dot adds half as much again. */
static int units (int b1)
{
  static const int dotted[4] = { 8, 12, 14, 15 };
  int type = b1 >> 5, dots = (b1 >> 3) & 3;
  return (128 >> type) * dotted[dots];
}

/* Stave line to MIDI note: line + clef offset is a step of the C major
   scale, 21 being middle C. */
static int scale_note (int step)
{
  static const int semis[7] = { 0, 2, 4, 5, 7, 9, 11 };
  if (step < 0)
    step = 0;
  return 24 + 12 * (step / 7) + semis[step % 7];
}

static const int clef_offset[4] = { 11, 5, 3, -1 };   /* treble, alto, tenor, bass */
static const int acc_semis[8] = { 0, 0, 1, -1, 2, -2, 1, -1 };   /* none, natural, #, b, x, bb, natural#, naturalb */

/* ---- voices to sounds ---------------------------------------------------- */

static int has (const char *name, const char *what)
{
  size_t i, n = strlen (what);
  for (i = 0; name[i]; i++)
    {
      size_t k;
      for (k = 0; k < n && name[i + k]; k++)
        {
          char a = name[i + k], b = what[k];
          if (a >= 'A' && a <= 'Z') a += 32;
          if (b >= 'A' && b <= 'Z') b += 32;
          if (a != b) break;
        }
      if (k == n)
        return 1;
    }
  return 0;
}

static int is_percussion (const char *voice) { return has (voice, "Percussion") || has (voice, "Drum"); }

/* A GM program for a voice, by its name. */
static int voice_program (const char *v)
{
  if (has (v, "StringLib-Soft")) return 24;     /* nylon guitar */
  if (has (v, "StringLib-Pluck")) return 46;    /* harp */
  if (has (v, "StringLib-Steel")) return 25;    /* steel guitar */
  if (has (v, "StringLib-Hard")) return 6;      /* harpsichord */
  if (has (v, "Beep") || has (v, "WaveSynth")) return 80;   /* square lead */
  if (has (v, "Piano")) return 0;
  if (has (v, "Organ")) return 19;
  if (has (v, "Bass")) return 33;
  if (has (v, "Violin") || has (v, "String")) return 48;
  if (has (v, "Flute")) return 73;
  if (has (v, "Clarinet")) return 71;
  if (has (v, "Oboe")) return 68;
  if (has (v, "Trumpet") || has (v, "Brass")) return 56;
  if (has (v, "Choir") || has (v, "Voice")) return 52;
  if (has (v, "Guitar")) return 24;
  if (has (v, "Bell")) return 14;
  return 0;
}

static int voice_patch (int program)
{
  static const int by_family[16] = {
    PATCH_EPIANO, PATCH_BELL, PATCH_ORGAN, PATCH_PLUCK, PATCH_BASS, PATCH_PAD,
    PATCH_PAD, PATCH_LEAD, PATCH_LEAD, PATCH_LEAD, PATCH_LEAD, PATCH_PAD,
    PATCH_PAD, PATCH_PLUCK, PATCH_BELL, PATCH_PAD };
  if (program == 6)
    return PATCH_PLUCK;         /* (a harpsichord is plucked) */
  return by_family[(program & 127) / 8];
}

/* A percussion voice's note to a drum row: Groove's rows are Kick, Snare,
   Hi-hat, Open hat, Clap, Low tom, High tom, Cowbell. */
static int drum_row (const char *v, int key, int len)
{
  if (has (v, "Snare")) return 1;
  if (has (v, "Noise")) return len >= GR_PPQ ? 3 : 2;
  if (has (v, "Medium")) return key < 60 ? 5 : 6;
  return key < 60 ? 0 : 5;      /* Percussion-Soft and the rest */
}

/* ---- the import ------------------------------------------------------------ */

typedef struct
{
  gr_pnote *n;
  int count, cap;
} notes;

static int add (notes *l, int start, int end, int key)
{
  if (l->count == l->cap)
    {
      int cap = l->cap ? l->cap * 2 : 256;
      gr_pnote *p = realloc (l->n, (size_t) cap * sizeof *p);
      if (!p)
        return -1;
      l->n = p;
      l->cap = cap;
    }
  l->n[l->count].start = start;
  l->n[l->count].end = end;
  l->n[l->count].key = key;
  l->n[l->count].vel = 100;
  l->count++;
  return 0;
}

/* Maestro's units (128 a crotchet) to Groove's ticks (96 a crotchet). */
static int ticks (long long u) { return (int) ((u * GR_PPQ * 2 + 128) / 256); }

int gr_maestro_import (gr_song *s, const char *path, int from,
                       const char *const *voices, int nvoices, const char **why)
{
  static char msg[96];
  FILE *f;
  unsigned char *d;
  long size;
  mfile m;
  notes out[NCH];
  int c, made = 0, empty = gr_song_end (s) == 0, full_at = -1, dropped = 0;
  int qpos[NCH], tied[NCH], clef[5], key[7], acc[5][32];
  long long bar_start = 0, next[5], first = -1;
  int bar_len, bar_has_notes = 0, bar = 0, first_bar = -1, meter_num = 4, meter_type = 3;
  int bar_num[1024], bar_type[1024], nbar = 0;
  long long cap;
  const char *vname[NCH];
  int i;

  *why = NULL;
  if (!voices || nvoices <= 0)
    {
      voices = default_voices;
      nvoices = (int) (sizeof default_voices / sizeof default_voices[0]);
    }
  if (!(f = fopen (path, "rb")))
    {
      *why = "The Maestro file couldn't be opened";
      return -1;
    }
  fseek (f, 0, SEEK_END);
  size = ftell (f);
  fseek (f, 0, SEEK_SET);
  if (size < 9 || size > 4 * 1024 * 1024 || !(d = malloc ((size_t) size)))
    {
      fclose (f);
      *why = size < 9 ? "That isn't a Maestro file" : "Not enough memory";
      return -1;
    }
  if (fread (d, 1, (size_t) size, f) != (size_t) size)
    size = 0;
  fclose (f);
  if (parse (d, (int) size, &m))
    {
      free (d);
      *why = "That isn't a Maestro file Groove can read";
      return -1;
    }

  /* Each channel's voice, as Maestro picks it: the number, wrapped to the
     voices there are (none or past the end is voice 1). */
  for (c = 0; c < NCH; c++)
    {
      int v = m.voice[c] % (nvoices + 1);
      if (v == 0)
        v = 1;
      vname[c] = voices[v - 1];
    }

  memset (out, 0, sizeof out);
  memset (qpos, 0, sizeof qpos);
  memset (tied, 0, sizeof tied);
  memset (clef, 0, sizeof clef);        /* treble on every stave until a clef says otherwise */
  memset (key, 0, sizeof key);
  memset (acc, 0, sizeof acc);
  memset (next, 0, sizeof next);
  bar_len = meter_num * (1024 >> meter_type);
  /* The longest a note sounds in Maestro: 254 twentieths of a second. */
  cap = (long long) 254 * tempo_table[m.tempo] * 128 / (20 * 60);

  for (i = 0; i < m.ngates; i++)
    {
      int g = m.gates[i];
      if (g == 0)
        {
          int a = i + 1 < m.ngates ? m.gates[++i] : 0;
          if (a & 1)                    /* time signature: this bar on */
            {
              meter_num = ((a >> 1) & 15) + 1;
              meter_type = (a >> 5) & 7;
              bar_len = meter_num * (1024 >> meter_type);
            }
          else if (a & 2)               /* key signature */
            {
              static const int sharps[7] = { 3, 0, 4, 1, 5, 2, 6 };   /* F C G D A E B */
              static const int flats[7] = { 6, 2, 5, 1, 4, 0, 3 };    /* B E A D G C F */
              int flat = (a >> 2) & 1, count = (a >> 3) & 7, k;
              memset (key, 0, sizeof key);
              for (k = 0; k < count; k++)
                key[flat ? flats[k] : sharps[k]] = flat ? 3 : 2;
              if (empty && first < 0)
                s->key = flat ? (5 * count) % 12 : (7 * count) % 12;
            }
          else if (a & 4)               /* clef */
            clef[(a >> 6) & 3] = (a >> 3) & 3;
          else if (a & 32)              /* bar line */
            {
              if (bar_has_notes || first >= 0)
                {
                  if (nbar < 1024)
                    {
                      bar_num[nbar] = meter_num;
                      bar_type[nbar] = meter_type;
                      nbar++;
                    }
                  bar_start += bar_len;
                  bar++;
                }
              bar_has_notes = 0;
              memset (next, 0, sizeof next);
              memset (acc, 0, sizeof acc);
            }
          /* (slurs and octave-shift marks: Maestro plays them as written) */
          continue;
        }
      /* A note gate: at the latest time of its channels' staves. */
      {
        long long q = 0, shortest[5];
        int st;
        for (c = 0; c < NCH; c++)
          if ((g >> c & 1) && next[stave_of (&m, c)] > q)
            q = next[stave_of (&m, c)];
        for (st = 0; st < 5; st++)
          shortest[st] = -1;
        if (first < 0)
          {
            first = bar_start;
            first_bar = bar;
          }
        bar_has_notes = 1;
        for (c = 0; c < NCH; c++)
          {
            int b0, b1, line, u;
            if (!(g >> c & 1) || qpos[c] + 2 > m.qlen[c])
              continue;
            b0 = m.q[c][qpos[c]];
            b1 = m.q[c][qpos[c] + 1];
            st = stave_of (&m, c);
            line = b0 >> 3;
            u = units (b1);
            if (line && !tied[c])
              {
                int pitch, a = 0;
                long long len = u;
                if (st < m.staves)
                  {
                    if (b1 & 7)
                      acc[st][line] = b1 & 7;
                    a = acc[st][line];
                    if (a)
                      pitch = scale_note (line + clef_offset[clef[st]]) + acc_semis[a];
                    else
                      {
                        int step = line + clef_offset[clef[st]];
                        pitch = scale_note (step) + acc_semis[key[(step < 0 ? 0 : step) % 7]];
                      }
                  }
                else
                  pitch = scale_note (line);     /* the percussion stave */
                if (b0 & 4)
                  {
                    /* Tied: on through the notes after it until one isn't. */
                    int k = qpos[c] + 2;
                    tied[c] = 1;
                    while (k + 2 <= m.qlen[c])
                      {
                        len += units (m.q[c][k + 1]);
                        if (!(m.q[c][k] & 4))
                          break;
                        k += 2;
                      }
                  }
                if (len > cap)
                  len = cap;
                if (pitch >= 0 && pitch < 128)
                  {
                    long long at = bar_start + q;
                    if (add (&out[c], ticks (at), ticks (at + len), pitch))
                      break;
                  }
              }
            else if (tied[c] && !(b0 & 4))
              tied[c] = 0;            /* the last of the tied notes */
            if (shortest[st] < 0 || u < shortest[st])
              shortest[st] = u;
            qpos[c] += 2;
          }
        for (st = 0; st < 5; st++)
          if (shortest[st] >= 0)
            next[st] = q + shortest[st];
      }
    }
  if (bar_has_notes && nbar < 1024)
    {
      bar_num[nbar] = meter_num;
      bar_type[nbar] = meter_type;
      nbar++;
    }

  /* The song: its tempo and time signatures if it was empty. */
  if (empty)
    {
      int b0 = gr_tick_bar (s, from);
      s->tempo = tempo_table[m.tempo];
      for (i = 0; i < nbar; i++)
        {
          int num = bar_num[i], den;
          /* Maestro's beat types: 0 breve ... 7 hemidemisemiquaver. */
          if (bar_type[i] == 0)
            num *= 2, den = 1;
          else
            den = 1 << (bar_type[i] - 1);
          gr_set_meter (s, b0 + i, num, den);
        }
    }

  /* Into a song with no music yet, the channels replace its empty tracks
     (a new song's Drums and Electric Piano). */
  if (empty)
    {
      int any = 0;
      for (c = 0; c < NCH; c++)
        any |= out[c].count > 0;
      while (any && s->ntracks)
        gr_track_delete (s, s->ntracks - 1);
    }
  /* A track per channel with notes. */
  for (c = 0; c < NCH; c++)
    {
      int t, k, drums;
      char name[GR_NAME];
      if (!out[c].count)
        continue;
      drums = is_percussion (vname[c]);
      snprintf (name, sizeof name, "%s %d", vname[c], c + 1);
      t = gr_track_add (s, drums ? TRK_DRUMS : TRK_INST, name,
                        drums ? KIT_STUDIO : voice_patch (voice_program (vname[c])));
      if (t < 0)
        {
          if (!*why)
            {
              snprintf (msg, sizeof msg, made ? "There wasn't room for all the channels: a song has %d tracks"
                                              : "There's no room for another track in this song", GR_MAX_TRACKS);
              *why = msg;
            }
          break;
        }
      s->tracks[t].gm = drums ? GM_KIT_STANDARD : voice_program (vname[c]);
      /* Maestro's volume is RISC OS's log scale, 16 steps a halving,
         (v + 1) * 15 - 1 of 127; 7 is the loudest. */
      s->tracks[t].vol = 0.8f * (float) pow (2.0, -((7 - m.vol[c]) * 15) / 16.0);
      s->tracks[t].pan = (float) m.stereo[c] / 3.0f - 1.0f;
      if (drums)
        for (k = 0; k < out[c].count; k++)
          out[c].n[k].key = drum_row (vname[c], out[c].n[k].key, out[c].n[k].end - out[c].n[k].start);
      /* From the first bar with music in it. */
      for (k = 0; k < out[c].count; k++)
        {
          out[c].n[k].start -= ticks (first);
          out[c].n[k].end -= ticks (first);
        }
      made++;
      if (gr_place_notes (s, t, out[c].n, out[c].count, from, &full_at, &dropped))
        {
          snprintf (msg, sizeof msg, "The song is full: the Maestro file was imported up to bar %d",
                    gr_tick_bar (s, full_at) + 1);
          *why = msg;
          break;
        }
    }
  (void) first_bar;
  if (!*why && dropped)
    {
      snprintf (msg, sizeof msg, "%d notes didn't fit and were left out", dropped);
      *why = msg;
    }
  for (c = 0; c < NCH; c++)
    free (out[c].n);
  free (d);
  if (!made && !*why)
    *why = "There are no notes in that Maestro file";
  return made ? made : -1;
}

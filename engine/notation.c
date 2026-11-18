/* Groove - the song as written music, for the Score window and MusicXML.

   A piano roll has no note values, so they're made here:
   - Each beat's notes are put on a grid: straight (down to 32nd notes) or,
     where they fit it better, triplets (a beat of triplet 8ths or 16ths),
     each voice by its own notes.
   - Each voice's music is cut where any of its notes starts or ends, so
     every piece is a chord (the notes sounding) or a rest; a note that
     goes on is tied into the next piece.
   - A piece is written as the longest note values that start where such
     a value may in the bar (a half note on beat 1 or 3 of 4/4, and so on)
     and don't cross a beat unless they start on one; the rest is tied.
   - An instrument that plays both well above and well below middle C gets
     two staves, split at middle C, so a held bass note isn't cut up by
     the tune above it; a note held while others start goes to a second
     voice, and so does a straight rhythm sharing a beat with triplets.
   - The key signature is worked out from the notes (major or minor), and
     changes where a long passage moves to another key and the new
     signature saves a good many accidentals; notes are spelled in the
     key of their bar.
   - Each staff's clef can change for a passage that sits high or low
     (a cello to the tenor clef), and a passage far above or below the
     staff goes under an 8va or 8vb line, whichever needs fewest ledger
     lines.
   - With GR_NT_REPEATS, music played again straight after is written
     once between repeat signs, with first and second endings where
     only its end differs.
   - Drums go on a percussion staff, where MuseScore's drum set puts them:
     hands (cymbals, snare, toms) stems up, feet (kick, pedal hi-hat) stems
     down.
   - How the notes are played is marked too: dynamics (pp to ff) from how
     hard they're played, a bar at a time; accents on notes much harder
     than those round them; staccato for a note let go well before the
     next (written to the next, with a dot); slurs over notes played
     legato, each overlapping the next. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "notation.h"

#define DIV GR_PPQ               /* ticks a quarter note */

typedef struct { int on, off, pitch, vel, flags, release; signed char step, alter; } xn;   /* release: when it really
                                                                     stops, from off */
#define XF_LEGATO 1               /* overlapped the next note (played legato) */
#define XF_STACC  2               /* let go early: written longer, staccato */

typedef struct
{
  xn *n;
  int count, cap;
} xlist;

static int add (xlist *l, int on, int off, int pitch, int vel)
{
  if (l->count == l->cap)
    {
      int cap = l->cap ? l->cap * 2 : 256;
      xn *a = realloc (l->n, (size_t) cap * sizeof *a);
      if (!a)
        return -1;
      l->n = a;
      l->cap = cap;
    }
  l->n[l->count].on = on;
  l->n[l->count].off = off;
  l->n[l->count].pitch = pitch;
  l->n[l->count].vel = vel;
  l->n[l->count].flags = 0;
  l->n[l->count].release = 0;
  l->n[l->count].step = -1;
  l->count++;
  return 0;
}

static int add_x (xlist *l, const xn *n)
{
  if (add (l, n->on, n->off, n->pitch, n->vel))
    return -1;
  l->n[l->count - 1].flags = n->flags;
  l->n[l->count - 1].release = n->release;
  l->n[l->count - 1].step = n->step;
  l->n[l->count - 1].alter = n->alter;
  return 0;
}

static int cmp_on (const void *a, const void *b)
{
  const xn *x = a, *y = b;
  if (x->on != y->on)
    return x->on < y->on ? -1 : 1;
  return x->pitch - y->pitch;
}

/* Every note a track plays, in song ticks (patterns looped in their
   regions, as they play). */
static int track_notes (const gr_song *s, int t, xlist *l)
{
  int i;
  for (i = 0; i < GR_MAX_REGIONS; i++)
    {
      const gr_region *r = &s->regions[i];
      const gr_pattern *p;
      int rep, k;
      if (!r->used || r->track != t || r->pattern < 0 || r->clip >= 0)
        continue;
      p = &s->patterns[r->pattern];
      if (p->len <= 0)
        continue;
      for (rep = 0; rep * p->len < r->len; rep++)
        for (k = 0; k < p->nnotes; k++)
          {
            const gr_note *n = &p->notes[k];
            int on = r->start + rep * p->len + n->tick, off = on + (n->len > 0 ? n->len : 1);
            if (on >= r->start + r->len)
              continue;
            if (off > r->start + r->len)
              off = r->start + r->len;
            if (add (l, on, off, n->pitch, n->vel ? n->vel : 1))
              return -1;
          }
    }
  if (l->count > 1)
    qsort (l->n, (size_t) l->count, sizeof *l->n, cmp_on);
  return 0;
}

/* The sustain pedal (controller 64) as the part's pedal marks: where it
   goes down and comes up, to the nearest 32nd note, up at the end if it's
   still down then. */
static int track_pedal (const gr_song *s, int t, const gr_nbar *bars, int nbars, gr_npart *p)
{
  xlist l = { NULL, 0, 0 };
  int i, k, down = 0, cap = 0, end = bars[nbars - 1].start + bars[nbars - 1].len;
  for (i = 0; i < GR_MAX_REGIONS; i++)
    {
      const gr_region *r = &s->regions[i];
      const gr_pattern *pt;
      int rep;
      if (!r->used || r->track != t || r->pattern < 0 || r->clip >= 0)
        continue;
      pt = &s->patterns[r->pattern];
      if (pt->len <= 0)
        continue;
      for (rep = 0; rep * pt->len < r->len; rep++)
        for (k = 0; k < pt->nctls; k++)
          if (pt->ctls[k].type == 64)
            {
              int at = r->start + rep * pt->len + pt->ctls[k].tick;
              if (at >= r->start + r->len)
                continue;
              /* (on: the tick; pitch: down or up, so an up sorts first) */
              if (add (&l, at, at, pt->ctls[k].value >= 64, 0))
                {
                  free (l.n);
                  return -1;
                }
            }
    }
  if (l.count > 1)
    qsort (l.n, (size_t) l.count, sizeof *l.n, cmp_on);
  for (k = 0; k < l.count + 1; k++)
    {
      int at = k < l.count ? (l.n[k].on + 6) / 12 * 12 : end, want = k < l.count ? l.n[k].pitch : 0, b;
      if (want == down || at > end)
        continue;
      if (at == end && want)
        continue;
      if (p->npedal == cap)
        {
          gr_npedal *a;
          cap = cap ? cap * 2 : 32;
          if (!(a = realloc (p->pedal, (size_t) cap * sizeof *a)))
            {
              free (l.n);
              return -1;
            }
          p->pedal = a;
        }
      for (b = 0; b + 1 < nbars && bars[b + 1].start <= at; b++)
        ;
      p->pedal[p->npedal].bar = b;
      p->pedal[p->npedal].start = at - bars[b].start;
      p->pedal[p->npedal].down = want;
      p->npedal++;
      down = want;
    }
  free (l.n);
  return 0;
}

/* ---- bars and the beat grid ------------------------------------------------ */

typedef gr_nbar xbar;

static int is_compound (int num, int den)
{
  return den >= 8 && num % 3 == 0 && num > 3;
}

/* Is the beat window starting at w (DIV ticks) better as triplets? From
   the note starts and ends that fall in it. */
/* How a beat (a window: one beat, or two for crotchet triplets) is
   written: straight, or as tuplets. Each bar has a kind for each beat. */
enum
{
  TW_NONE,                      /* straight, on a 32nd grid */
  TW_TRIP,                      /* triplets in a beat (and sextuplets): a grid of 16 */
  TW_QUINT,                     /* five in a beat: at 0, 19, 38, 58, 77 */
  TW_CTRIP,                     /* crotchet triplets, three over this beat and the next */
  TW_CONT,                      /* (the second beat of one) */
  TW_DUP                        /* duplets: two in a dotted beat (6/8...), at 0 and 72 */
};

static const int q5[6] = { 0, 19, 38, 58, 77, 96 };

static int near_q5 (int d)       /* the nearest quintuplet place to d (0..96) */
{
  int j, best = 0;
  for (j = 1; j < 6; j++)
    if (abs (d - q5[j]) < abs (d - q5[best]))
      best = j;
  return best;
}

/* Every start and end of a note strictly inside w0..w0+len, as offsets:
   n of them, into d[] (up to max). */
static int inside (const xlist *x, int w0, int len, int *d, int max)
{
  int k, n = 0;
  for (k = 0; k < x->count && n < max; k++)
    {
      int t2[2], j;
      t2[0] = x->n[k].on - w0;
      t2[1] = x->n[k].off - w0;
      for (j = 0; j < 2 && n < max; j++)
        if (t2[j] > 0 && t2[j] < len)
          d[n++] = t2[j];
    }
  return n;
}

/* A simple beat: straight, triplets or quintuplets, whichever grid its
   notes' starts and ends sit closest to. */
static int beat_kind (const xlist *x, int w0)
{
  static int d[4096];
  int n = inside (x, w0, DIV, d, 4096), i;
  long e12 = 0, e16 = 0, e5 = 0;
  if (!n)
    return TW_NONE;
  for (i = 0; i < n; i++)
    {
      int r = d[i] % 12;
      e12 += r < 6 ? r : 12 - r;
      r = d[i] % 16;
      e16 += r < 8 ? r : 16 - r;
      e5 += abs (d[i] - q5[near_q5 (d[i])]);
    }
  if (e5 < e12 && e5 < e16)
    return TW_QUINT;
  return e16 < e12 ? TW_TRIP : TW_NONE;
}

/* Two beats from w0 that are crotchet triplets: everything inside them
   starts or ends a third or two thirds of the way through (and
   something does). */
static int crotchet_triplets (const xlist *x, int w0)
{
  static int d[4096];
  int n = inside (x, w0, 2 * DIV, d, 4096), i;
  if (!n)
    return 0;
  for (i = 0; i < n; i++)
    if (abs (d[i] - 64) > 6 && abs (d[i] - 128) > 6)
      return 0;
  return 1;
}

/* A dotted beat (144 ticks) that's two duplets: everything inside it at
   its middle. */
static int duplets (const xlist *x, int w0)
{
  static int d[4096];
  int n = inside (x, w0, 144, d, 4096), i;
  if (!n)
    return 0;
  for (i = 0; i < n; i++)
    if (abs (d[i] - 72) > 6)
      return 0;
  return 1;
}

static int snap (int t, int grid, int w0)
{
  int d = t - w0;
  return w0 + (d + grid / 2) / grid * grid;
}

/* ---- note values ------------------------------------------------------------ */

typedef struct { int ticks, type, dots; } value;

static const value straight[] = {
  { 384, NV_WHOLE, 0 }, { 288, NV_HALF, 1 }, { 192, NV_HALF, 0 }, { 144, NV_QUARTER, 1 },
  { 96, NV_QUARTER, 0 }, { 72, NV_EIGHTH, 1 }, { 48, NV_EIGHTH, 0 }, { 36, NV_16TH, 1 },
  { 24, NV_16TH, 0 }, { 18, NV_32ND, 1 }, { 12, NV_32ND, 0 }, { 6, NV_64TH, 0 }, { 3, NV_128TH, 0 } };

/* In a triplet beat: the written value, three in the time of two. */
static const value triplet[] = {
  { 64, NV_QUARTER, 0 }, { 32, NV_EIGHTH, 0 }, { 16, NV_16TH, 0 }, { 8, NV_32ND, 0 } };

const char *gr_nvalue_name (int v)
{
  static const char *const n[] = { "whole", "half", "quarter", "eighth", "16th", "32nd", "64th", "128th" };
  return v >= 0 && v <= NV_128TH ? n[v] : "quarter";
}

int gr_nvalue_ticks (int v)
{
  return v >= 0 && v <= NV_128TH ? 384 >> v : 96;
}

/* The longest value from a (ticks into the bar) that fits before b. */
static const value *next_value (const xbar *bar, int a, int b, int in_triplet, int win)
{
  int i, room = b - a;
  if (in_triplet)
    {
      for (i = 0; i < 4; i++)
        if (triplet[i].ticks <= room && (a - win) % (i == 0 ? 32 : triplet[i].ticks) == 0)
          return &triplet[i];
      return &triplet[3];
    }
  for (i = 0; i < (int) (sizeof straight / sizeof straight[0]); i++)
    {
      const value *v = &straight[i];
      int align = v->dots ? v->ticks * 2 / 3 : v->ticks;
      if (bar->compound && v->dots)
        align = v->ticks;
      if (v->ticks > room || a % align)
        continue;
      /* Off the beat, a value mustn't run over the next beat. */
      if (a % bar->beat && (a / bar->beat + 1) * bar->beat < a + v->ticks)
        continue;
      /* In a bar of four beats, only a value from the downbeat runs over
         the middle (a crotchet then a minim, not a dotted crotchet...). */
      if (bar->len == 4 * bar->beat && a > 0 && a < bar->len / 2 && a + v->ticks > bar->len / 2)
        continue;
      return v;
    }
  return &straight[sizeof straight / sizeof straight[0] - 1];
}

/* ---- spelling --------------------------------------------------------------- */

static const char *const sharp_step = "CCDDEFFGGAAB";
static const int sharp_alter[12] = { 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0 };
static const char *const flat_step = "CDDEEFGGAABB";
static const int flat_alter[12] = { 0, -1, 0, -1, 0, 0, -1, 0, -1, 0, -1, 0 };

/* How well a key (k: 0-11 the major keys on C..B, 12-23 the minor ones)
   fits how long each pitch class sounds (h): the correlation with
   Krumhansl and Kessler's key profiles. */
static double key_fit (const double *h, int k)
{
  static const double maj[12] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
  static const double min[12] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };
  const double *pr = k < 12 ? maj : min;
  double mx = 0, my = 0, sxy = 0, sxx = 0, syy = 0;
  int j;
  for (j = 0; j < 12; j++)
    {
      mx += h[(j + k) % 12];
      my += pr[j];
    }
  mx /= 12;
  my /= 12;
  for (j = 0; j < 12; j++)
    {
      double x = h[(j + k) % 12] - mx, y = pr[j] - my;
      sxy += x * y;
      sxx += x * x;
      syy += y * y;
    }
  return sxx > 0 && syy > 0 ? sxy / sqrt (sxx * syy) : 0;
}

static const int fifths_of_major[12] = { 0, -5, 2, -3, 4, -1, 6, 1, -4, 3, -2, 5 };


/* The key of the whole piece (0-23, as key_fit), from all its notes. */
static int find_key (const xlist *ls, int nls, int song_key)
{
  double h[12] = { 0 }, best = -1e30, total = 0;
  int i, k, key = song_key % 12;
  for (i = 0; i < nls; i++)
    for (k = 0; k < ls[i].count; k++)
      {
        h[ls[i].n[k].pitch % 12] += ls[i].n[k].off - ls[i].n[k].on;
        total += ls[i].n[k].off - ls[i].n[k].on;
      }
  if (total > 0)
    for (k = 0; k < 24; k++)
      {
        double r = key_fit (h, k);
        /* (ties go to the song's own key, then to major) */
        if (r > best + 1e-9 || (r > best - 1e-9 && k == song_key % 12))
          {
            best = r;
            key = k;
          }
      }
  return key;
}

/* The key in each bar (kf: its fifths, km: minor). A key signature is
   there to save accidentals, so it changes only where that saves a
   good many of them for a long passage: each bar costs, in each of the
   twelve key signatures, a note for each pitch outside the key it
   plays (counted once a bar, in each part), and a change of key
   signature costs as much as several of those, more for a key further
   round the circle of fifths. The signatures that cost least over the
   piece are chosen (the piece's own key liked a little more), then
   whether each passage is major or minor by how its notes fit each. */
#define KEY_CHANGE  6.0             /* (accidentals) */
#define KEY_FAR     2.0             /* more for each fifth further */
#define KEY_AWAY    1.0             /* a bar not in the piece's key (or the one it starts in) */
static int find_keys (const xlist *ls, int nls, const xbar *bars, int nbars, int song_key, int *kf, int *km)
{
  double (*h)[12] = calloc ((size_t) nbars, sizeof *h);
  double (*v)[12] = calloc ((size_t) nbars, sizeof *v);
  unsigned short (*pcs)[1] = calloc ((size_t) nbars * (size_t) (nls + 1), sizeof (unsigned short));
  int home = find_key (ls, nls, song_key), hs, os, i, k, b, j, cur;
  if (!h || !v || !pcs)
    {
      free (h);
      free (v);
      free (pcs);
      return -1;
    }
  /* the signature, as the major key with it (0 C, 1 C#/Db... by pitch class of the tonic) */
  hs = home >= 12 ? (home - 12 + 3) % 12 : home;
  for (i = 0; i < nls; i++)
    for (k = 0, b = 0; k < ls[i].count; k++)
      {
        const xn *n = &ls[i].n[k];
        while (b > 0 && bars[b].start > n->on)
          b--;
        while (b + 1 < nbars && bars[b + 1].start <= n->on)
          b++;
        pcs[b * (nls + 1) + i][0] |= (unsigned short) (1 << (n->pitch % 12));
        for (j = b; j < nbars && bars[j].start < n->off; j++)
          {
            int a = n->on > bars[j].start ? n->on : bars[j].start;
            int e = n->off < bars[j].start + bars[j].len ? n->off : bars[j].start + bars[j].len;
            if (e > a)
              h[j][n->pitch % 12] += e - a;
          }
      }
  /* the key it starts in: its first eight bars with notes */
  {
    double w[12] = { 0 }, best = -1e30;
    int n = 0, opening = home;
    for (b = 0; b < nbars && n < 8; b++)
      {
        double tot = 0;
        for (j = 0; j < 12; j++)
          w[j] += h[b][j], tot += h[b][j];
        n += tot > 0;
      }
    for (k = 0; k < 24; k++)
      if (key_fit (w, k) > best + 1e-9 || (key_fit (w, k) > best - 1e-9 && k == home))
        best = key_fit (w, k), opening = k;
    os = opening >= 12 ? (opening - 12 + 3) % 12 : opening;
  }
  /* each bar's accidentals in each signature */
  for (b = 0; b < nbars; b++)
    for (k = 0; k < 12; k++)
      {
        static const int scale = (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 11);
        int in_key = ((scale << k) | (scale >> (12 - k))) & 0xFFF;
        double c = k == hs || k == os ? 0 : KEY_AWAY;
        for (i = 0; i < nls; i++)
          {
            int out = pcs[b * (nls + 1) + i][0] & ~in_key;
            while (out)
              {
                c += out & 1;
                out >>= 1;
              }
          }
        v[b][k] = c;
      }
  /* from the end back: the least each bar on can cost in each */
  for (b = nbars - 2; b >= 0; b--)
    for (k = 0; k < 12; k++)
      {
        double best = v[b + 1][k];
        for (j = 0; j < 12; j++)
          {
            int d = abs (fifths_of_major[j] - fifths_of_major[k]);
            double c = v[b + 1][j] + KEY_CHANGE + KEY_FAR * (d > 6 ? 12 - d : d);
            if (j != k && c < best)
              best = c;
          }
        v[b][k] += best;
      }
  /* then forwards, keeping each as long as that's as good */
  cur = os;
  for (k = 0; k < 12; k++)
    if (v[0][k] < v[0][cur] - 1e-9)
      cur = k;
  for (b = 0; b < nbars; b++)
    {
      if (b > 0)
        {
          double best = v[b][cur];
          int to = cur;
          for (j = 0; j < 12; j++)
            {
              int d = abs (fifths_of_major[j] - fifths_of_major[cur]);
              double c = v[b][j] + KEY_CHANGE + KEY_FAR * (d > 6 ? 12 - d : d);
              if (j != cur && c < best - 1e-9)
                best = c, to = j;
            }
          cur = to;
        }
      kf[b] = fifths_of_major[cur];
    }
  /* each passage in one signature: major, or its relative minor */
  for (b = 0; b < nbars;)
    {
      double w[12] = { 0 };
      int e = b, tonic = ((kf[b] * 7) % 12 + 12) % 12, minor;
      while (e < nbars && kf[e] == kf[b])
        {
          for (j = 0; j < 12; j++)
            w[j] += h[e][j];
          e++;
        }
      if (tonic == hs && (b == 0 || e == nbars))
        minor = home >= 12;
      else
        minor = key_fit (w, (tonic + 9) % 12 + 12) > key_fit (w, tonic);
      for (; b < e; b++)
        km[b] = minor;
    }
  free (h);
  free (v);
  free (pcs);
  return 0;
}

/* ---- drums ------------------------------------------------------------------ */

/* Where MuseScore's drum set writes a key: staff line (0 = the top line,
   counting down in steps) and notehead. */
static void drum_place (int key, int *line, int *head)
{
  static const struct { unsigned char key; signed char line; unsigned char head; } kit[] = {
    { 35, 8, NH_NORMAL }, { 36, 7, NH_NORMAL }, { 37, 3, NH_SLASHED }, { 38, 3, NH_NORMAL }, { 39, 3, NH_X },
    { 40, 3, NH_SLASH }, { 41, 6, NH_NORMAL }, { 42, -1, NH_X }, { 43, 5, NH_NORMAL }, { 44, 9, NH_X },
    { 45, 4, NH_NORMAL }, { 46, -1, NH_CIRCLE_X }, { 47, 2, NH_NORMAL }, { 48, 1, NH_NORMAL }, { 49, -2, NH_X },
    { 50, 0, NH_NORMAL }, { 51, 0, NH_X }, { 52, -3, NH_NORMAL }, { 53, 0, NH_DIAMOND }, { 54, 1, NH_DIAMOND },
    { 55, -4, NH_X }, { 56, 1, NH_TRIANGLE_DOWN }, { 57, -3, NH_X }, { 59, 2, NH_X } };
  int i;
  for (i = 0; i < (int) (sizeof kit / sizeof kit[0]); i++)
    if (kit[i].key == key)
      {
        *line = kit[i].line;
        *head = kit[i].head;
        return;
      }
  *line = key < 45 ? 6 : key < 60 ? 1 : -2;   /* the rest: low, middle, high */
  *head = NH_TRIANGLE_UP;
}

/* Notes held while others start (a bass under a tune, a long melody note
   over moving chords) go to a second voice, so they're written whole
   rather than cut up and tied. A note that only just overlaps the next
   (played legato) is ended where the next starts. */
static int split_held (xlist *l, xlist *held)
{
  int k, j, wn = 0;
  for (k = 0; k < l->count; k++)
    for (j = k + 1; j < l->count; j++)
      if (l->n[j].on > l->n[k].on)
        {
          if (l->n[j].on < l->n[k].off && l->n[k].off - l->n[j].on < DIV / 4)
            {
              l->n[k].release = l->n[k].off - l->n[j].on;
              l->n[k].off = l->n[j].on;
              l->n[k].flags |= XF_LEGATO;
            }
          break;
        }
  for (k = 0; k < l->count; k++)
    {
      int is_held = 0;
      if (l->n[k].off - l->n[k].on >= DIV)
        for (j = k + 1; j < l->count && l->n[j].on < l->n[k].off; j++)
          if (l->n[j].on > l->n[k].on && l->n[k].off - l->n[j].on >= DIV / 2)
            {
              is_held = 1;
              break;
            }
      if (is_held)
        {
          if (add_x (held, &l->n[k]))
            return -1;
        }
      else
        l->n[wn++] = l->n[k];
    }
  l->count = wn;
  return 0;
}

/* Where a beat holds both exact triplets and an exact straight rhythm
   (Beethoven's dotted tune over his triplets), the straight notes move
   to the second voice, when it's free then, so each voice can be written
   in its own rhythm. */
static int on_grid (int t, int w0, int g)
{
  int d = t - w0;
  return d > 0 && d < DIV && d % g == 0;
}

static int move_cross_rhythms (xlist *l, xlist *held)
{
  int k, j, wn = 0;
  for (k = 0; k < l->count; k++)
    {
      xn *n = &l->n[k];
      int w0, w1, tri = 0, move = 0;
      /* this note's straight-only point, and its beat */
      int pt = on_grid (n->off, n->off / DIV * DIV, 12) && !on_grid (n->off, n->off / DIV * DIV, 16) ? n->off
        : on_grid (n->on, n->on / DIV * DIV, 12) && !on_grid (n->on, n->on / DIV * DIV, 16) ? n->on : -1;
      if (pt >= 0)
        {
          w0 = pt / DIV * DIV;
          w1 = w0 + DIV;
          for (j = 0; j < l->count && !tri; j++)
            tri = (on_grid (l->n[j].on, w0, 16) && !on_grid (l->n[j].on, w0, 12))
                  || (on_grid (l->n[j].off, w0, 16) && !on_grid (l->n[j].off, w0, 12));
          if (tri)
            {
              move = 1;
              for (j = 0; j < held->count; j++)
                if (held->n[j].on < n->off && held->n[j].off > n->on)
                  move = 0;
              (void) w1;
            }
        }
      if (move)
        {
          if (add_x (held, n))
            return -1;
        }
      else
        l->n[wn++] = l->n[k];
    }
  l->count = wn;
  if (held->count > 1)
    qsort (held->n, (size_t) held->count, sizeof *held->n, cmp_on);
  return 0;
}

static int cmp_int (const void *a, const void *b)
{
  return *(const int *) a - *(const int *) b;
}

/* Snaps one voice's note starts and ends to its beat grid (straight or
   triplets, each beat, as suits this voice's notes), recording which beats
   are triplets in tp (16 a bar). */
static void quantise (const gr_song *s, xlist *x, const xbar *bars, int nbars, unsigned char *tp)
{
  int b, k, j, wn;
  for (b = 0; b < nbars; b++)
    {
      int beat = bars[b].beat;
      if (!bars[b].compound && bars[b].den <= 4 && beat == DIV)
        {
          for (k = 0; k < 16 && (k + 1) * DIV <= bars[b].len; k++)
            tp[b * 16 + k] = (unsigned char) beat_kind (x, bars[b].start + k * DIV);
          /* crotchet triplets: over beats 1-2 or 3-4 */
          for (k = 0; k + 1 < 16 && (k + 2) * DIV <= bars[b].len; k += 2)
            if (crotchet_triplets (x, bars[b].start + k * DIV))
              {
                tp[b * 16 + k] = TW_CTRIP;
                tp[b * 16 + k + 1] = TW_CONT;
              }
        }
      else if (bars[b].compound && beat == 144)
        for (k = 0; k < 16 && (k + 1) * beat <= bars[b].len; k++)
          if (duplets (x, bars[b].start + k * beat))
            tp[b * 16 + k] = TW_DUP;
    }
  for (k = 0; k < x->count; k++)
    {
      int *tt[2], q;
      tt[0] = &x->n[k].on;
      tt[1] = &x->n[k].off;
      for (q = 0; q < 2; q++)
        {
          int v = *tt[q], bb = gr_tick_bar (s, v < 0 ? 0 : v), win, kind, w0;
          if (bb >= nbars)
            bb = nbars - 1;
          win = (v - bars[bb].start) / bars[bb].beat;
          kind = win < 16 ? tp[bb * 16 + win] : TW_NONE;
          w0 = bars[bb].start + win * bars[bb].beat;
          switch (kind)
            {
            case TW_TRIP:  *tt[q] = snap (v, 16, w0); break;
            case TW_QUINT: *tt[q] = w0 + q5[near_q5 (v - w0)]; break;
            case TW_CTRIP: *tt[q] = snap (v, 64, w0); break;
            case TW_CONT:  *tt[q] = snap (v, 64, w0 - DIV); break;
            case TW_DUP:   *tt[q] = snap (v, 72, w0); break;
            default:       *tt[q] = snap (v, 12, w0); break;
            }
        }
      if (x->n[k].off <= x->n[k].on)
        x->n[k].off = x->n[k].on + 12;
    }
  /* A key struck again before it's let go: the first ends there. */
  qsort (x->n, (size_t) x->count, sizeof *x->n, cmp_on);
  for (k = 0; k < x->count; k++)
    for (j = k + 1; j < x->count && x->n[j].on < x->n[k].off; j++)
      if (x->n[j].pitch == x->n[k].pitch)
        {
          x->n[k].off = x->n[j].on;
          break;
        }
  for (k = wn = 0; k < x->count; k++)
    if (x->n[k].off > x->n[k].on)
      x->n[wn++] = x->n[k];
  x->count = wn;
}

/* Notes let go well before the next one starts (by a 16th or more, and
   sounding at most half the time to it) are written lasting until it,
   at most a beat and not over one, with a staccato dot; release keeps how
   much earlier they really stop. A chord only if all its notes are. Not
   a note that hands over to another in the part (in its other voice or on
   the other staff, as in a broken chord): ons, nons, sorted, are where
   all the part's notes start. */
static int starts_at (const int *ons, int nons, int t)
{
  int lo = 0, hi = nons;
  while (lo < hi)
    {
      int mid = (lo + hi) / 2;
      if (ons[mid] < t)
        lo = mid + 1;
      else
        hi = mid;
    }
  return lo < nons && ons[lo] == t;
}

static void staccato (const gr_song *s, xlist *x, const xbar *bars, int nbars, const int *ons, int nons)
{
  int k = 0, sounding = 0;
  while (k < x->count)
    {
      int j, g = k, on = x->n[k].on, next = -1, bb, beat, slot, ok = 1, into;
      while (g < x->count && x->n[g].on == on)
        g++;
      for (j = g; j < x->count; j++)
        if (x->n[j].on > on)
          {
            next = x->n[j].on;
            break;
          }
      bb = gr_tick_bar (s, on);
      if (bb >= nbars)
        bb = nbars - 1;
      beat = bars[bb].beat;
      into = (on - bars[bb].start) % beat;
      slot = beat - into;                           /* to the next beat */
      if (next >= 0 && next - on < slot)
        slot = next - on;
      if (bars[bb].start + bars[bb].len - on < slot)
        slot = bars[bb].start + bars[bb].len - on;
      /* only a plain value: an 8th, a quarter, a dotted 8th... */
      if (slot != 24 && slot != 36 && slot != 48 && slot != 72 && slot != 96 && slot != 144)
        ok = 0;
      for (j = k; j < g && ok; j++)
        {
          int d = x->n[j].off - x->n[j].on;
          if (d * 2 > slot || slot - d < 24 || x->n[j].off > on + slot || starts_at (ons, nons, x->n[j].off))
            ok = 0;
        }
      /* nothing else in the voice still sounding then */
      if (sounding > on)
        ok = 0;
      if (ok && next < 0 && slot < 48)
        ok = 0;
      for (j = k; j < g && ok; j++)
        {
          x->n[j].release = x->n[j].off - (on + slot);
          x->n[j].off = on + slot;
          x->n[j].flags |= XF_STACC;
        }
      for (j = k; j < g; j++)
        if (x->n[j].off > sounding)
          sounding = x->n[j].off;
      k = g;
    }
}

/* A drum hit is written lasting until the next in its voice, as one
   note value (drums aren't tied): the longest that fits, starts where it
   may and doesn't cross a beat unless it starts on one. */
static void drum_lengths (const gr_song *s, xlist *x, const xbar *bars, int nbars)
{
  static const int lens[] = { 96, 72, 48, 36, 24, 18, 12, 6 };
  int k, j;
  for (k = 0; k < x->count; k++)
    {
      int on = x->n[k].on, room = DIV, bb = gr_tick_bar (s, on), a, beat, i, len = 12;
      for (j = k + 1; j < x->count; j++)
        if (x->n[j].on > on)
          {
            if (x->n[j].on - on < room)
              room = x->n[j].on - on;
            break;
          }
      if (bb >= nbars)
        bb = nbars - 1;
      a = on - bars[bb].start;
      beat = bars[bb].beat;
      if (bars[bb].len - a < room)
        room = bars[bb].len - a;
      for (i = 0; i < (int) (sizeof lens / sizeof lens[0]); i++)
        {
          int l = lens[i], align = (i & 1) && l != 6 ? l * 2 / 3 : l;
          if (l > room || a % align)
            continue;
          if (a % beat && (a / beat + 1) * beat < a + l)
            continue;
          len = l;
          break;
        }
      x->n[k].off = on + len;
    }
}

/* ---- the events ---------------------------------------------------------------- */

static int new_event (gr_notation *nt)
{
  if (nt->nev == nt->evcap)
    {
      int cap = nt->evcap ? nt->evcap * 2 : 1024;
      gr_nevent *a = realloc (nt->ev, (size_t) cap * sizeof *a);
      if (!a)
        return -1;
      nt->ev = a;
      nt->evcap = cap;
    }
  memset (&nt->ev[nt->nev], 0, sizeof nt->ev[0]);
  nt->ev[nt->nev].head = nt->nhd;
  return nt->nev++;
}

static gr_nhead *new_head (gr_notation *nt)
{
  if (nt->nhd == nt->hdcap)
    {
      int cap = nt->hdcap ? nt->hdcap * 2 : 1024;
      gr_nhead *a = realloc (nt->hd, (size_t) cap * sizeof *a);
      if (!a)
        return NULL;
      nt->hd = a;
      nt->hdcap = cap;
    }
  memset (&nt->hd[nt->nhd], 0, sizeof nt->hd[0]);
  return &nt->hd[nt->nhd++];
}

static void spell (gr_nhead *h, int pitch, int fifths, int drums)
{
  if (drums)
    {
      int line, head, pos;
      drum_place (pitch, &line, &head);
      pos = 10 - line;                        /* steps up from C4 */
      h->step = (signed char) (((pos % 7) + 7) % 7);
      h->octave = (signed char) (4 + (pos >= 0 ? pos / 7 : (pos - 6) / 7));
      h->alter = 0;
      h->head = (unsigned char) head;
    }
  else
    {
      int pc = pitch % 12;
      const char *steps = fifths >= 0 ? sharp_step : flat_step;
      h->step = (signed char) (strchr ("CDEFGAB", steps[pc]) - "CDEFGAB");
      h->alter = (signed char) (fifths >= 0 ? sharp_alter[pc] : flat_alter[pc]);
      h->octave = (signed char) (pitch / 12 - 1);
      h->head = NH_NORMAL;
    }
}

/* ---- spelling ------------------------------------------------------------------ */

/* How each note in a voice is spelled. The key's own notes as the key
   spells them; a note from outside it as a sharp if the tune goes up a
   semitone from it (G#-A) and a flat if it goes down (Ab-G), or else
   by the note it came from (up from G: G#; down from A: Ab), or else
   as music usually spells that step of the key: b2, b3, #4, b6, b7 in a
   major key; b2, #4, and the raised 3rd, 6th and 7th in a minor one. */
static void spell_list (xlist *l, const int *kf, const int *km, const xbar *bars, int nbars)
{
  static const int major_steps[7] = { 0, 2, 4, 5, 7, 9, 11 }, natural[7] = { 0, 2, 4, 5, 7, 9, 11 };
  int i, k, b = 0, fifths = 99, minor = 0;
  signed char dstep[12], dalter[12];
  int diatonic[12], home = 0;
  for (k = 0; k < l->count; k++)
    {
      xn *n = &l->n[k];
      int pc = n->pitch % 12, sharp = -1, j, d;
      /* the key of the bar it starts in */
      while (b + 1 < nbars && bars[b + 1].start <= n->on)
        b++;
      while (b > 0 && bars[b].start > n->on)
        b--;
      if (kf[b] != fifths || km[b] != minor)
        {
          /* the major key's tonic: its letter and pitch class */
          int tonic_letter, tonic_pc;
          fifths = kf[b];
          minor = km[b];
          tonic_letter = ((fifths * 4) % 7 + 7) % 7;
          tonic_pc = ((fifths * 7) % 12 + 12) % 12;
          home = minor ? (tonic_pc + 9) % 12 : tonic_pc;
          memset (diatonic, 0, sizeof diatonic);
          for (i = 0; i < 7; i++)
            {
              int letter = (tonic_letter + i) % 7, p2 = (tonic_pc + major_steps[i]) % 12, a = p2 - natural[letter];
              if (a > 6) a -= 12;
              if (a < -6) a += 12;
              dstep[p2] = (signed char) letter;
              dalter[p2] = (signed char) a;
              diatonic[p2] = 1;
            }
        }
      if (diatonic[pc])
        {
          n->step = dstep[pc];
          n->alter = dalter[pc];
          continue;
        }
      /* where the tune goes next: the nearest note of the next chord */
      for (j = k + 1; j < l->count && l->n[j].on == n->on; j++)
        ;
      if (j < l->count)
        {
          int best = 99, on = l->n[j].on;
          for (; j < l->count && l->n[j].on == on; j++)
            if (abs (l->n[j].pitch - n->pitch) < abs (best))
              best = l->n[j].pitch - n->pitch;
          if (best == 1)
            sharp = 1;
          else if (best == -1)
            sharp = 0;
        }
      /* where it came from */
      if (sharp < 0)
        {
          for (j = k - 1; j >= 0 && l->n[j].on == n->on; j--)
            ;
          if (j >= 0)
            {
              int best = 99, on = l->n[j].on;
              for (; j >= 0 && l->n[j].on == on; j--)
                if (abs (n->pitch - l->n[j].pitch) < abs (best))
                  best = n->pitch - l->n[j].pitch;
              if (best == 1)
                sharp = 1;
              else if (best == -1)
                sharp = 0;
            }
        }
      /* the usual spelling of that step of the key */
      if (sharp < 0)
        {
          d = (pc - home + 12) % 12;
          sharp = minor ? (d == 4 || d == 6 || d == 9 || d == 11) : d == 6;
        }
      /* raise the key's note below, or lower the one above */
      if (sharp)
        {
          int below = (pc + 11) % 12;
          n->step = dstep[below];
          n->alter = (signed char) (dalter[below] + 1);
        }
      else
        {
          int above = (pc + 1) % 12;
          n->step = dstep[above];
          n->alter = (signed char) (dalter[above] - 1);
        }
    }
}

/* One voice's music for one bar, as events. trip[k]: beat window k could
   be triplets. hidden: a second voice, whose gaps are spaces. */
static int build_bar (gr_notation *nt, const xlist *l, const xbar *bar, const unsigned char *trip, int ntrip,
                      int hidden, int drums)
{
  int *bd, nb = 0, cap = 8, i, k, end = bar->start + bar->len, any = 0, beat = bar->beat, e_first;
  unsigned char mine[16];
  /* A window is written as tuplets only if this voice has a note
     starting or ending inside it: notes on the beat stay plain. */
  memset (mine, 0, sizeof mine);
  for (k = 0; k < ntrip; k++)
    if (trip[k] && trip[k] != TW_CONT)
      {
        int len = trip[k] == TW_CTRIP ? 2 * DIV : beat, q;
        for (q = 0; q < l->count; q++)
          {
            int a = l->n[q].on - bar->start - k * beat, b = l->n[q].off - bar->start - k * beat;
            if ((a > 0 && a < len) || (b > 0 && b < len))
              {
                mine[k] = trip[k];
                if (trip[k] == TW_CTRIP && k + 1 < 16)
                  mine[k + 1] = TW_CONT;
                break;
              }
          }
      }
  trip = mine;
  for (k = 0; k < l->count; k++)
    if (l->n[k].on < end && l->n[k].off > bar->start)
      {
        any = 1;
        break;
      }
  if (!any)
    {
      int e;
      if (hidden)
        return 0;
      if ((e = new_event (nt)) < 0)
        return -1;
      nt->ev[e].kind = NE_BAR_REST;
      nt->ev[e].dur = bar->len;
      nt->ev[e].value = NV_WHOLE;
      return 0;
    }
  e_first = nt->nev;
  /* Where pieces start and end: the bar's ends, every note's start and
     end in it, and the ends of each tuplet window. */
  for (k = 0; k < l->count; k++)
    if (l->n[k].on < end && l->n[k].off > bar->start)
      cap += 2;
  cap += 2 * ntrip;
  bd = malloc ((size_t) cap * sizeof *bd);
  if (!bd)
    return -1;
  bd[nb++] = 0;
  bd[nb++] = bar->len;
  for (k = 0; k < l->count; k++)
    if (l->n[k].on < end && l->n[k].off > bar->start)
      {
        if (l->n[k].on > bar->start)
          bd[nb++] = l->n[k].on - bar->start;
        if (l->n[k].off < end)
          bd[nb++] = l->n[k].off - bar->start;
      }
  for (k = 0; k < ntrip; k++)
    if (trip[k] && trip[k] != TW_CONT)
      {
        int len = trip[k] == TW_CTRIP ? 2 * DIV : beat;
        bd[nb++] = k * beat;
        bd[nb++] = k * beat + len < bar->len ? k * beat + len : bar->len;
      }
  qsort (bd, (size_t) nb, sizeof *bd, cmp_int);
  for (i = 0; i + 1 < nb; i++)
    {
      int a = bd[i], b = bd[i + 1];
      int win = a / beat, kind = win < ntrip ? trip[win] : TW_NONE, w0, wlen;
      if (b <= a)
        continue;
      if (kind == TW_CONT)
        win--, kind = TW_CTRIP;
      w0 = win * beat;
      wlen = kind == TW_CTRIP ? 2 * DIV : beat;
      while (a < b)
        {
          value tv;
          const value *v = &tv;
          int d, tup = 0, t0, t1, e;
          gr_nevent *ev;
          switch (kind)
            {
            case TW_TRIP:
              v = next_value (bar, a, b, 1, w0);
              break;
            case TW_CTRIP:
              /* crotchets (or a minim) three in the time of two */
              tv.dots = 0;
              tv.ticks = b - a >= 128 && (a - w0) % 64 == 0 ? 128 : 64;
              tv.type = tv.ticks == 128 ? NV_HALF : NV_QUARTER;
              break;
            case TW_QUINT:
              {
                /* fifths of a beat: 4 a crotchet, 3 a dotted quaver... */
                int j0 = near_q5 (a - w0), j1 = near_q5 (b - w0), u = j1 - j0;
                if (u > 4) u = 4;
                if (u < 1) u = 1;
                tv.ticks = q5[j0 + u] - q5[j0];
                tv.type = u == 4 ? NV_QUARTER : u >= 2 ? NV_EIGHTH : NV_16TH;
                tv.dots = u == 3;
              }
              break;
            case TW_DUP:
              tv.ticks = 72;
              tv.type = NV_EIGHTH;
              tv.dots = 0;
              break;
            default:
              v = next_value (bar, a, b, 0, w0);
              kind = TW_NONE;
              break;
            }
          d = v->ticks;
          if (d > b - a)
            d = b - a;
          t0 = bar->start + a;
          t1 = t0 + d;
          if (kind != TW_NONE)
            {
              tup = 1;
              if (a == w0)
                tup |= 2;
              if (a + d >= w0 + wlen || a + d >= bar->len)
                tup |= 4;
            }
          if ((e = new_event (nt)) < 0)
            {
              free (bd);
              return -1;
            }
          ev = &nt->ev[e];
          ev->start = a;
          ev->dur = d;
          ev->value = (unsigned char) v->type;
          ev->dots = (unsigned char) v->dots;
          ev->tuplet = (unsigned char) tup;
          if (tup)
            {
              ev->tup_n = kind == TW_QUINT ? 5 : kind == TW_DUP ? 2 : 3;
              ev->tup_m = kind == TW_QUINT ? 4 : kind == TW_DUP ? 3 : 2;
              ev->tup_len = (unsigned short) wlen;
            }
          ev->kind = NE_REST;
          for (k = 0; k < l->count; k++)
            {
              const xn *n = &l->n[k];
              if (n->on <= t0 && n->off >= t1)
                {
                  gr_nhead *h = new_head (nt);
                  if (!h)
                    {
                      free (bd);
                      return -1;
                    }
                  ev = &nt->ev[e];
                  h->pitch = (unsigned char) n->pitch;
                  h->vel = (unsigned char) n->vel;
                  spell (h, n->pitch, nt->fifths, drums);
                  if (!drums && n->step >= 0)
                    {
                      /* (as spell_list spelled it: B#3 is written in octave 3) */
                      static const int natural[7] = { 0, 2, 4, 5, 7, 9, 11 };
                      h->step = n->step;
                      h->alter = n->alter;
                      h->octave = (signed char) ((n->pitch - n->alter - natural[(int) n->step]) / 12 - 1);
                    }
                  h->tie_stop = n->on < t0 && !drums;
                  h->tie_start = n->off > t1 && !drums;
                  h->legato = (n->flags & XF_LEGATO) != 0;
                  if (n->off == t1)
                    h->release = (short) n->release;
                  if ((n->flags & XF_STACC) && n->off == t1)
                    ev->artic |= NA_STACCATO;
                  ev->kind = NE_NOTE;
                  ev->nheads++;
                }
            }
          if (ev->kind == NE_REST && hidden)
            ev->kind = NE_SPACE;
          a += d;
        }
    }
  free (bd);
  /* A beat of triplets in semiquavers: a sextuplet, "6" (six in the time
     of four). */
  for (k = e_first; k < nt->nev; k++)
    if ((nt->ev[k].tuplet & 2) && nt->ev[k].tup_n == 3 && nt->ev[k].tup_len == DIV)
      {
        int j, sixteenths = 0;
        for (j = k; j < nt->nev; j++)
          {
            sixteenths |= nt->ev[j].value == NV_16TH;
            if (nt->ev[j].tuplet & 4)
              break;
          }
        if (sixteenths)
          for (j = k; j < nt->nev; j++)
            {
              nt->ev[j].tup_n = 6;
              nt->ev[j].tup_m = 4;
              if (nt->ev[j].tuplet & 4)
                break;
            }
      }
  return 0;
}

static int clef_for (const xlist *l, int staves, int staff, int drums)
{
  long sum = 0;
  int k;
  if (drums)
    return NC_PERC;
  if (staves > 1)
    return staff ? NC_F : NC_G;
  for (k = 0; k < l->count; k++)
    sum += l->n[k].pitch;
  return l->count && sum / l->count < 55 ? NC_F : NC_G;
}

/* ---- how it's played ------------------------------------------------------------ */

const char *gr_ndyn_name (int d)
{
  static const char *const n[] = { "", "ppp", "pp", "p", "mp", "mf", "f", "ff", "fff" };
  return d >= 0 && d <= ND_FFF ? n[d] : "";
}

/* The velocity where each dynamic starts (pp from 25 ... fff from 120),
   about midway between those MuseScore plays them at. */
static const int dyn_from[ND_FFF + 1] = { 0, 0, 25, 41, 57, 72, 88, 104, 120 };

static int dyn_of (int vel)
{
  int d = ND_PPP;
  while (d < ND_FFF && vel >= dyn_from[d + 1])
    d++;
  return d;
}

/* A note that starts here (rather than one tied from before): its
   loudest head's velocity, or -1. */
static int starts (const gr_notation *nt, const gr_nevent *e)
{
  int h, v = -1;
  if (e->kind != NE_NOTE)
    return -1;
  for (h = 0; h < e->nheads; h++)
    if (!nt->hd[e->head + h].tie_stop && nt->hd[e->head + h].vel > v)
      v = nt->hd[e->head + h].vel;
  return v;
}

static int cmp_int_down (const void *a, const void *b)
{
  return *(const int *) b - *(const int *) a;
}

static int median (int *v, int n)
{
  qsort (v, (size_t) n, sizeof *v, cmp_int_down);
  return n ? v[n / 2] : 0;
}

static int legato_start (const gr_notation *nt, const gr_nevent *e)
{
  int h;
  for (h = 0; h < e->nheads; h++)
    if (!nt->hd[e->head + h].tie_stop && nt->hd[e->head + h].legato)
      return 1;
  return 0;
}

static void end_slur (gr_notation *nt, int open, int last)
{
  if (last != open)
    nt->ev[last].slur |= NS_STOP;
  else
    nt->ev[open].slur &= (unsigned char) ~NS_START;
}

typedef struct { int t, v, ev; } onset;

static int cmp_onset (const void *a, const void *b)
{
  const onset *x = a, *y = b;
  if (x->t != y->t)
    return x->t < y->t ? -1 : 1;
  return x->ev - y->ev;
}

/* Hairpins: a run of notes each played as hard or harder than the one
   before (crescendo), or as soft or softer (diminuendo), a little at a
   time (a jump of more than 20 is a new dynamic or an accent, not a
   hairpin), over at least two beats and four notes, changing by 20 or
   more (a groove's accents, rising into the next bar, aren't one). in[] (for the part's
   events from p0): 1 starts one, 2 inside one, 4 ends one. */
static unsigned char *hairpins (gr_notation *nt, const gr_npart *p, int p0)
{
  int n = 0, b, z, k, i, j, dir;
  onset *o;
  unsigned char *in = calloc ((size_t) (nt->nev - p0 + 1), 1);
  if (!in)
    return NULL;
  o = malloc ((size_t) (nt->nev - p0 + 1) * sizeof *o);
  if (!o)
    return in;
  for (b = 0; b < nt->nbars; b++)
    for (z = 0; z < p->staves * 2; z++)
      {
        const gr_nrange *r = &p->voice[b * GR_NVOICES + z];
        for (k = 0; k < r->n; k++)
          {
            int v = starts (nt, &nt->ev[r->first + k]);
            if (v < 0)
              continue;
            o[n].t = nt->bars[b].start + nt->ev[r->first + k].start;
            o[n].v = v;
            o[n].ev = r->first + k;
            n++;
          }
      }
  qsort (o, (size_t) n, sizeof *o, cmp_onset);
  /* one per moment: the loudest, on the first voice it's in */
  for (i = j = 0; i < n; i++)
    if (j && o[j - 1].t == o[i].t)
      {
        if (o[i].v > o[j - 1].v)
          o[j - 1].v = o[i].v;
      }
    else
      o[j++] = o[i];
  n = j;
  i = 0;
  dir = 0;
  for (k = 1; k <= n; k++)
    {
      int d = k < n ? o[k].v - o[k - 1].v : 0;
      if (k < n && (dir == 0 || d * dir >= -2) && d <= 20 && d >= -20)
        {
          if (!dir && d)
            dir = d > 0 ? 1 : -1;
          continue;
        }
      /* the run i .. k-1, without its level ends */
      {
        int a = i, e = k - 1, steps = 0, q;
        while (a < e && o[a + 1].v == o[a].v)
          a++;
        while (e > a && o[e - 1].v == o[e].v)
          e--;
        for (q = a; q < e; q++)
          if ((o[q + 1].v - o[q].v) * dir > 0)
            steps++;
        if (dir && e - a >= 3 && steps >= 3 && (o[e].v - o[a].v) * dir >= 20 && o[e].t - o[a].t >= 2 * DIV)
          {
            nt->ev[o[a].ev].wedge |= dir > 0 ? NW_CRESC : NW_DIM;
            nt->ev[o[e].ev].wedge |= NW_STOP;
            for (q = a; q <= e; q++)
              in[o[q].ev - p0] |= 2;
            in[o[a].ev - p0] |= 1;
            in[o[e].ev - p0] |= 4;
          }
      }
      /* the next run starts where this one turned */
      i = k - 1;
      dir = k < n && d ? (d > 0 ? 1 : -1) : 0;
    }
  free (o);
  return in;
}

/* Dynamics, accents and slurs for a part, from its notes' velocities and
   how they join. */
static void mark_playing (gr_notation *nt, gr_npart *p)
{
  static int vs[1024];
  int b, z, k, cur = ND_NONE, p0 = p->voice[0].first;
  unsigned char *in = hairpins (nt, p, p0), none = 0;
#define IN(i) (in ? in[(i) - p0] : none)
  /* Dynamics: each bar's middle velocity, marked where it moves into
     another dynamic (by more than a little, so it doesn't flicker), at
     the bar's first note. */
  for (b = 0; b < nt->nbars; b++)
    {
      int n = 0, m, d, first = -1, first_t = 1 << 30;
      for (z = 0; z < p->staves * 2; z++)
        {
          const gr_nrange *r = &p->voice[b * GR_NVOICES + z];
          for (k = 0; k < r->n; k++)
            {
              const gr_nevent *e = &nt->ev[r->first + k];
              int v = starts (nt, e);
              if (v < 0)
                continue;
              if (n < 1024)
                vs[n++] = v;
              if (e->start < first_t)
                {
                  first_t = e->start;
                  first = r->first + k;
                }
            }
        }
      if (!n)
        continue;
      m = median (vs, n);
      d = dyn_of (m);
      if (cur != ND_NONE && ((d > cur && m < dyn_from[cur + 1] + 4) || (d < cur && m >= dyn_from[cur] - 4)))
        d = cur;
      /* (where a hairpin starts, the level it starts from; not while
         one's getting there) */
      if (IN (first) & 1)
        d = dyn_of (starts (nt, &nt->ev[first]));
      if (d != cur && (IN (first) & 3) != 2)
        {
          nt->ev[first].dyn = (unsigned char) d;
          cur = d;
        }
      /* where a hairpin ends: the dynamic it got to */
      for (z = 0; z < p->staves * 2; z++)
        {
          const gr_nrange *r = &p->voice[b * GR_NVOICES + z];
          for (k = 0; k < r->n; k++)
            if (nt->ev[r->first + k].wedge & NW_STOP)
              {
                d = dyn_of (starts (nt, &nt->ev[r->first + k]));
                if (d != cur)
                  {
                    nt->ev[r->first + k].dyn = (unsigned char) d;
                    cur = d;
                  }
              }
        }
    }
  /* Accents: a note at least 20 harder than the middle of its voice's
     bar, where only a few are. */
  for (b = 0; b < nt->nbars; b++)
    for (z = 0; z < p->staves * 2; z++)
      {
        const gr_nrange *r = &p->voice[b * GR_NVOICES + z];
        int n = 0, m, hard = 0;
        for (k = 0; k < r->n; k++)
          {
            int v = starts (nt, &nt->ev[r->first + k]);
            if (v >= 0 && n < 1024)
              vs[n++] = v;
          }
        if (n < 3)
          continue;
        m = median (vs, n);
        for (k = 0; k < r->n; k++)
          if (starts (nt, &nt->ev[r->first + k]) >= m + 20)
            hard++;
        if (hard * 3 > n)
          continue;
        for (k = 0; k < r->n; k++)
          {
            int v = starts (nt, &nt->ev[r->first + k]);
            if (v >= m + 20 && v >= 64 && !IN (r->first + k))
              nt->ev[r->first + k].artic |= NA_ACCENT;
          }
      }
  free (in);
#undef IN
  /* Slurs: from a note played legato into the next, to the first note
     after a run of them that isn't. A rest ends one early. */
  if (p->drums)
    return;
  for (z = 0; z < p->staves * 2; z++)
    {
      int open = -1, last = -1;
      for (b = 0; b < nt->nbars; b++)
        {
          const gr_nrange *r = &p->voice[b * GR_NVOICES + z];
          for (k = 0; k < r->n; k++)
            {
              int i = r->first + k;
              gr_nevent *e = &nt->ev[i];
              if (e->kind != NE_NOTE)
                {
                  if (open >= 0)
                    end_slur (nt, open, last);
                  open = -1;
                  continue;
                }
              if (starts (nt, e) < 0)
                continue;
              if (open < 0 && legato_start (nt, e))
                {
                  open = i;
                  e->slur |= NS_START;
                }
              else if (open >= 0 && !legato_start (nt, e))
                {
                  e->slur |= NS_STOP;
                  open = -1;
                }
              last = i;
            }
        }
      if (open >= 0)
        end_slur (nt, open, last);
    }
}

void gr_notation_free (gr_notation *nt)
{
  int p;
  if (!nt)
    return;
  for (p = 0; p < nt->nparts; p++)
    {
      free (nt->parts[p].voice);
      free (nt->parts[p].pedal);
      free (nt->parts[p].key);
      free (nt->parts[p].clefs);
      free (nt->parts[p].ottava);
      free (nt->parts[p].ott_start);
      free (nt->parts[p].ott_end);
    }
  free (nt->parts);
  free (nt->bars);
  free (nt->bar_of);
  free (nt->ev);
  free (nt->hd);
  free (nt);
}

/* ---- instruments ------------------------------------------------------------- */

/* How an instrument (its General MIDI program) is written: transpose,
   semitones written above sounding (key: a transposing instrument, not
   just an octave); clef (-1: by its range; tenor: a bass clef that
   goes to tenor clef when the part sits high); keyboard: two staves
   when it's wide. */
typedef struct { int transpose, key, clef, tenor, keyboard; } instr;

static instr instrument (const gr_track *tr)
{
  int g = gr_track_program (tr) & 127;
  instr r = { 0, 0, -1, 0, 0 };
  if (g <= 23 || (g >= 46 && g <= 55) || (g >= 88 && g <= 103))
    r.keyboard = 1;                           /* pianos, organs, harp, strings and choirs, pads */
  if (g == 9)
    r.transpose = -24;                        /* glockenspiel */
  else if (g == 13 || g == 72)
    r.transpose = -12;                        /* xylophone, piccolo */
  else if (g >= 24 && g <= 31)
    r.transpose = 12, r.clef = NC_G8;         /* guitars */
  else if ((g >= 32 && g <= 39) || g == 43)
    r.transpose = 12, r.clef = NC_F;          /* bass guitars, double bass */
  else if (g == 41)
    r.clef = NC_C3;                           /* viola */
  else if (g == 42 || g == 57 || g == 70)
    r.clef = NC_F, r.tenor = 1;               /* cello, trombone, bassoon */
  else if (g == 58)
    r.clef = NC_F;                            /* tuba */
  else if (g == 56 || g == 59 || g == 64 || g == 71)
    r.transpose = 2, r.key = 1;               /* Bb: trumpets, soprano sax, clarinet */
  else if (g == 65)
    r.transpose = 9, r.key = 1;               /* Eb alto sax */
  else if (g == 66)
    r.transpose = 14, r.key = 1;              /* Bb tenor sax */
  else if (g == 67)
    r.transpose = 21, r.key = 1;              /* Eb baritone sax */
  else if (g == 60 || g == 69)
    r.transpose = 7, r.key = 1;               /* F: horn, cor anglais */
  return r;
}

/* A key moved by some semitones: its fifths (within six sharps or flats). */
static int key_moved (int fifths, int semis)
{
  int d = (((semis % 12) + 12) % 12) * 7 % 12;
  fifths += d > 6 ? d - 12 : d;
  while (fifths > 6)
    fifths -= 12;
  while (fifths < -6)
    fifths += 12;
  return fifths;
}

/* ---- repeats ----------------------------------------------------------------- */

/* The dynamic each part's playing at the start of each bar (dl[part *
   nbars + bar]), so bars can be compared by how loud they're played,
   not by where the marks happen to be. */
static int *dyn_levels (const gr_notation *nt)
{
  int *dl = malloc ((size_t) (nt->nparts * nt->nbars + 1) * sizeof *dl), p, b, z, k;
  if (!dl)
    return NULL;
  for (p = 0; p < nt->nparts; p++)
    {
      const gr_npart *pp = &nt->parts[p];
      int level = 0;
      for (b = 0; b < nt->nbars; b++)
        {
          int at = -1, d = level;
          dl[p * nt->nbars + b] = level;
          for (z = 0; z < GR_NVOICES; z++)
            for (k = 0; k < pp->voice[b * GR_NVOICES + z].n; k++)
              {
                const gr_nevent *e = &nt->ev[pp->voice[b * GR_NVOICES + z].first + k];
                if (e->dyn && e->start >= at)
                  at = e->start, d = e->dyn;
              }
          level = d;
        }
    }
  return dl;
}

/* Are bars a and b the same music in every part (to the note, how hard
   and how long each is played, and every mark)? */
static int same_bars (const gr_notation *nt, const int *dl, int a, int b)
{
  const gr_nbar *x = &nt->bars[a], *y = &nt->bars[b];
  int p, z, k, j;
  if (x->len != y->len || x->num != y->num || x->den != y->den || x->fifths != y->fifths || x->minor != y->minor)
    return 0;
  for (p = 0; p < nt->nparts; p++)
    {
      const gr_npart *pp = &nt->parts[p];
      int da = dl[p * nt->nbars + a], db = dl[p * nt->nbars + b], na = 0, nb = 0;
      if (pp->key[a] != pp->key[b])
        return 0;
      for (z = 0; z < 2; z++)
        if (pp->clefs[a * 2 + z] != pp->clefs[b * 2 + z] || pp->ottava[a * 2 + z] != pp->ottava[b * 2 + z]
            || pp->ott_start[a * 2 + z] != pp->ott_start[b * 2 + z] || pp->ott_end[a * 2 + z] != pp->ott_end[b * 2 + z])
          return 0;
      for (k = 0; k < pp->npedal; k++)
        na += pp->pedal[k].bar == a, nb += pp->pedal[k].bar == b;
      if (na != nb)
        return 0;
      for (k = j = 0; na && k < pp->npedal; k++)
        if (pp->pedal[k].bar == a)
          {
            while (pp->pedal[j].bar != b)
              j++;
            if (pp->pedal[k].start != pp->pedal[j].start || pp->pedal[k].down != pp->pedal[j].down)
              return 0;
            j++;
          }
      for (z = 0; z < GR_NVOICES; z++)
        {
          const gr_nrange *ra = &pp->voice[a * GR_NVOICES + z], *rb = &pp->voice[b * GR_NVOICES + z];
          if (ra->n != rb->n)
            return 0;
          for (k = 0; k < ra->n; k++)
            {
              const gr_nevent *e = &nt->ev[ra->first + k], *f = &nt->ev[rb->first + k];
              if (e->start != f->start || e->dur != f->dur || e->kind != f->kind || e->value != f->value
                  || e->dots != f->dots || e->tuplet != f->tuplet || e->tup_n != f->tup_n || e->tup_m != f->tup_m
                  || e->tup_len != f->tup_len || e->nheads != f->nheads || e->artic != f->artic
                  || e->slur != f->slur || e->wedge != f->wedge)
                return 0;
              /* (as loud: the same mark, or none where it's already that loud) */
              if (e->dyn)
                da = e->dyn;
              if (f->dyn)
                db = f->dyn;
              if (e->kind == NE_NOTE && da != db)
                return 0;
              for (j = 0; e->kind == NE_NOTE && j < e->nheads; j++)
                {
                  const gr_nhead *g = &nt->hd[e->head + j], *h = &nt->hd[f->head + j];
                  if (g->pitch != h->pitch || g->vel != h->vel || g->step != h->step || g->alter != h->alter
                      || g->octave != h->octave || g->head != h->head || g->tie_start != h->tie_start
                      || g->tie_stop != h->tie_stop || g->legato != h->legato || g->release != h->release)
                    return 0;
                }
            }
        }
    }
  return 1;
}

/* Where the music can be cut between bars for a repeat (cut[b]: before
   bar b): nothing held over it (a tie, a slur, a hairpin or the pedal). */
static unsigned char *clean_cuts (const gr_notation *nt)
{
  unsigned char *cut = malloc ((size_t) nt->nbars + 1);
  int p, b, z, k;
  if (!cut)
    return NULL;
  memset (cut, 1, (size_t) nt->nbars + 1);
  for (p = 0; p < nt->nparts; p++)
    {
      const gr_npart *pp = &nt->parts[p];
      int slur[GR_NVOICES] = { 0 }, wedge = 0, ped = 0, pk = 0;
      for (b = 0; b < nt->nbars; b++)
        {
          int any_slur = 0;
          for (z = 0; z < GR_NVOICES; z++)
            any_slur |= slur[z];
          if (any_slur || wedge || ped)
            cut[b] = 0;
          while (pk < pp->npedal && pp->pedal[pk].bar < b)
            pk++;
          if (ped && pk < pp->npedal && pp->pedal[pk].bar == b && pp->pedal[pk].start == 0 && !pp->pedal[pk].down)
            cut[b] = !(any_slur || wedge);
          for (z = 0; z < GR_NVOICES; z++)
            for (k = 0; k < pp->voice[b * GR_NVOICES + z].n; k++)
              {
                const gr_nevent *e = &nt->ev[pp->voice[b * GR_NVOICES + z].first + k];
                int h;
                if (e->slur & NS_STOP)
                  slur[z] = 0;
                if (e->slur & NS_START)
                  slur[z] = 1;
                if (e->wedge & NW_STOP)
                  wedge = 0;
                if (e->wedge & (NW_CRESC | NW_DIM))
                  wedge = 1;
                for (h = 0; e->kind == NE_NOTE && h < e->nheads; h++)
                  if (nt->hd[e->head + h].tie_stop && e->start == 0)
                    cut[b] = 0;
              }
          for (k = 0; k < pp->npedal; k++)
            if (pp->pedal[k].bar == b)
              ped = pp->pedal[k].down;
        }
      {
        int any_slur = 0;
        for (z = 0; z < GR_NVOICES; z++)
          any_slur |= slur[z];
        if (any_slur || wedge || ped)
          cut[nt->nbars] = 0;
      }
    }
  return cut;
}

/* Does bar b have a note in it, in any part? */
static int has_notes (const gr_notation *nt, int b)
{
  int p, z, k;
  for (p = 0; p < nt->nparts; p++)
    for (z = 0; z < GR_NVOICES; z++)
      for (k = 0; k < nt->parts[p].voice[b * GR_NVOICES + z].n; k++)
        if (nt->ev[nt->parts[p].voice[b * GR_NVOICES + z].first + k].kind == NE_NOTE)
          return 1;
  return 0;
}

#define REP_LONGEST 16              /* bars in a repeated passage, at most */
#define END_LONGEST 4               /* in an ending */

/* Music played again straight after, written once between repeat marks:
   a passage of bars played two or more times over (a one-bar riff only
   if three or more), or played twice with a different last bar or few
   (first and second endings). Going from the start, the way of writing
   it that saves the most bars is taken. Then the bars played again are
   taken out (bar_of says where each of the song's bars is written). */
static int find_repeats (gr_notation *nt)
{
  int n = nt->nbars, i, j, k, p, z, *keep, *dl, nkeep = 0;
  unsigned char *cut;
  dl = dyn_levels (nt);
  cut = clean_cuts (nt);
  keep = malloc ((size_t) n * sizeof *keep);
  if (!dl || !cut || !keep)
    {
      free (dl);
      free (cut);
      free (keep);
      return -1;
    }
  for (i = 0; i < n; i++)
    keep[i] = i;                    /* (a bar written as another: -1 - that one) */
  for (i = 0; i < n;)
    {
      int best = 0, bl = 0, bk = 0, bc = 0, be = 0, l, c, e;
      if (!cut[i] || !has_notes (nt, i))
        {
          i++;
          continue;
        }
      /* |: L bars :| played k times */
      for (l = 1; l <= REP_LONGEST && i + 2 * l <= n; l++)
        {
          int times = 1, ok = 1;
          while (i + (times + 1) * l <= n && cut[i + times * l] && cut[i + (times + 1) * l])
            {
              for (j = 0; j < l && ok; j++)
                ok = same_bars (nt, dl, i + j, i + times * l + j);
              if (!ok)
                break;
              times++;
            }
          if (times >= 2 && (l >= 2 || times >= 3) && cut[i + l] && l * (times - 1) > best)
            best = l * (times - 1), bl = l, bk = times, bc = 0;
        }
      /* |: C bars [1. E bars :| [2. E bars */
      for (c = 2; c <= REP_LONGEST; c++)
        for (e = 1; e <= END_LONGEST && i + 2 * c + 2 * e <= n; e++)
          {
            int ok = cut[i + c] && cut[i + c + e] && cut[i + 2 * c + e] && cut[i + 2 * c + 2 * e], diff = 0;
            for (j = 0; j < c && ok; j++)
              ok = same_bars (nt, dl, i + j, i + c + e + j);
            for (j = 0; j < e && ok; j++)
              diff |= !same_bars (nt, dl, i + c + j, i + 2 * c + e + j);
            if (ok && diff && !same_bars (nt, dl, i + c, i + 2 * c + e) && c > best)
              best = c, bl = c, bk = 2, bc = 1, be = e;
          }
      if (!best)
        {
          i++;
          continue;
        }
      nt->bars[i].rep |= NR_START;
      if (!bc)
        {
          nt->bars[i + bl - 1].rep |= NR_END;
          nt->bars[i + bl - 1].times = (unsigned char) bk;
          for (k = 1; k < bk; k++)
            for (j = 0; j < bl; j++)
              keep[i + k * bl + j] = -1 - (i + j);
          i += bl * bk;
        }
      else
        {
          int e1 = i + bl, e2 = i + 2 * bl + be;
          for (j = 0; j < be; j++)
            {
              nt->bars[e1 + j].ending = 1;
              nt->bars[e2 + j].ending = 2;
            }
          nt->bars[e1].ending_start = nt->bars[e2].ending_start = 1;
          nt->bars[e1 + be - 1].ending_stop = 1;
          nt->bars[e2 + be - 1].ending_stop = 2;
          nt->bars[e1 + be - 1].rep |= NR_END;
          nt->bars[e1 + be - 1].times = 2;
          for (j = 0; j < bl; j++)
            keep[i + bl + be + j] = -1 - (i + j);
          i = e2 + be;
        }
    }
  /* take out the bars played again */
  {
    int *newno = malloc ((size_t) n * sizeof *newno);
    if (!newno)
      {
        free (dl);
        free (cut);
        free (keep);
        return -1;
      }
    for (i = 0; i < n; i++)
      newno[i] = keep[i] >= 0 ? nkeep++ : -1;
    for (i = 0; i < n; i++)
      nt->bar_of[i] = keep[i] >= 0 ? newno[i] : newno[-1 - keep[i]];
    for (i = 0; i < n; i++)
      if (keep[i] >= 0)
        nt->bars[newno[i]] = nt->bars[i];
    for (p = 0; p < nt->nparts; p++)
      {
        gr_npart *pp = &nt->parts[p];
        for (i = 0; i < n; i++)
          if (keep[i] >= 0)
            {
              pp->key[newno[i]] = pp->key[i];
              for (z = 0; z < 2; z++)
                {
                  pp->clefs[newno[i] * 2 + z] = pp->clefs[i * 2 + z];
                  pp->ottava[newno[i] * 2 + z] = pp->ottava[i * 2 + z];
                  pp->ott_start[newno[i] * 2 + z] = pp->ott_start[i * 2 + z];
                  pp->ott_end[newno[i] * 2 + z] = pp->ott_end[i * 2 + z];
                }
              for (z = 0; z < GR_NVOICES; z++)
                pp->voice[newno[i] * GR_NVOICES + z] = pp->voice[i * GR_NVOICES + z];
            }
        for (i = j = 0; i < pp->npedal; i++)
          if (keep[pp->pedal[i].bar] >= 0)
            {
              pp->pedal[j] = pp->pedal[i];
              pp->pedal[j++].bar = newno[pp->pedal[i].bar];
            }
        pp->npedal = j;
      }
    nt->nbars = nkeep;
    free (newno);
  }
  free (dl);
  free (cut);
  free (keep);
  return 0;
}

/* ---- clefs and 8va lines -------------------------------------------------------- */

/* A clef's bottom line, in diatonic steps from C0 (a guitar's treble-8
   is written as treble: its notes are already an octave up). */
static int clef_bottom (int clef)
{
  return clef == NC_F ? 2 * 7 + 4 : clef == NC_C3 ? 3 * 7 + 3 : clef == NC_C4 ? 3 * 7 + 1 : 4 * 7 + 2;
}

/* How hard a note is to read at a place on the staff (0: the bottom
   line), in quarters: nothing within a ledger line, a little at two,
   then more and more for each line beyond. */
static int ledger_cost (int pos)
{
  int l = pos >= 10 ? (pos - 8) / 2 : pos <= -2 ? -pos / 2 : 0;
  return l <= 1 ? 0 : l == 2 ? 1 : 4 * (l - 2) * (l - 2);
}

#define CLEF_CHANGE   14            /* (quarters, as ledger_cost) */
#define OTTAVA_CHANGE 24
#define CLEF_AWAY     2             /* a bar not in the part's own clef */
#define OTTAVA_NOTE   2             /* a note under an 8va or 8vb line */
#define OTTAVA_IDLE   6             /* one under it that didn't need it */

/* The clef (and any 8va or 8vb line) for each bar of each staff of a
   part: whichever makes its notes need the fewest ledger lines, with a
   change of clef or a line costing as much as a good many notes, so
   they change only for a passage, at a bar line. Which clefs a part may
   use goes by its instrument: a cello bass, tenor or treble; a viola
   alto or treble; a piano's upper staff treble (or 8va), its lower bass
   (or 8vb); a guitar always treble-8. */
/* A staff's notes, for choose_clefs: when each starts and ends, and its
   place (diatonic steps from C0), in time order. */
typedef struct { int on, off, dia; } cnote;

static int cmp_cnote (const void *a, const void *b)
{
  const cnote *x = a, *y = b;
  return x->on != y->on ? (x->on < y->on ? -1 : 1) : x->dia - y->dia;
}

/* The first and last notes (fi, li) in n[0..cnt) that need an 8va line
   in clef cl: two or more ledger lines without it. -1 if none. */
static void needing (const cnote *n, int cnt, int cl, int *fi, int *li)
{
  int i;
  *fi = *li = -1;
  for (i = 0; i < cnt; i++)
    if (ledger_cost (n[i].dia - clef_bottom (cl)) > 0)
      {
        if (*fi < 0)
          *fi = i;
        *li = i;
      }
}

static int choose_clefs (gr_npart *p, const xlist *v, const xbar *bars, int nbars, const gr_track *tr)
{
  static const int natural[7] = { 0, 2, 4, 5, 7, 9, 11 };
  int z, b, i, j, k;
  cnote *cn = NULL;
  int *first = NULL;               /* [bar]: its first note in cn (cn sorted by bar, then time) */
  p->clefs = calloc ((size_t) nbars * 2, 1);
  p->ottava = calloc ((size_t) nbars * 2, 1);
  p->ott_start = calloc ((size_t) nbars * 2, sizeof *p->ott_start);
  p->ott_end = calloc ((size_t) nbars * 2, sizeof *p->ott_end);
  first = malloc ((size_t) (nbars + 1) * sizeof *first);
  if (!p->clefs || !p->ottava || !p->ott_start || !p->ott_end || !first)
    {
      free (first);
      return -1;
    }
  for (z = 0; z < p->staves; z++)
    {
      int cl[6], ot[6], ns = 0, home = 0, s0, cur, cnt = 0;
      long (*cost)[6];
      for (b = 0; b < nbars; b++)
        p->clefs[b * 2 + z] = (unsigned char) p->clef[z];
      if (p->drums)
        continue;
      memset (ot, 0, sizeof ot);
      if (p->staves > 1)
        {
          cl[0] = cl[1] = cl[2] = p->clef[z];
          ot[1] = z ? -1 : 1;
          ot[2] = z ? -2 : 2;
          ns = 3;
        }
      else
        {
          instr in = instrument (tr);
          if (in.clef == NC_G8)
            continue;
          if (in.tenor)
            cl[0] = NC_F, cl[1] = NC_C4, cl[2] = NC_G, ns = 3;
          else if (in.clef == NC_C3)
            cl[0] = NC_C3, cl[1] = NC_G, ns = 2;
          else if (in.clef == NC_F)
            cl[0] = cl[1] = NC_F, ot[1] = -1, ns = 2;
          else
            {
              cl[0] = cl[2] = cl[4] = NC_G, cl[1] = cl[3] = cl[5] = NC_F;
              ot[2] = 1, ot[3] = -1, ot[4] = 2, ot[5] = -2;
              ns = 6;
            }
          for (i = 0; i < ns; i++)
            if (cl[i] == p->clef[z] && !ot[i])
              home = i;
        }
      /* the staff's notes, bar by bar, in time order */
      free (cn);
      cn = malloc ((size_t) (v[z * 2].count + v[z * 2 + 1].count + 1) * sizeof *cn);
      if (!cn)
        {
          free (first);
          return -1;
        }
      for (i = 0; i < 2; i++)
        for (k = 0; k < v[z * 2 + i].count; k++)
          {
            const xn *n = &v[z * 2 + i].n[k];
            cn[cnt].on = n->on;
            cn[cnt].off = n->off;
            if (n->step >= 0)
              cn[cnt].dia = ((n->pitch - n->alter - natural[(int) n->step]) / 12 - 1) * 7 + n->step;
            else
              cn[cnt].dia = (n->pitch / 12 - 1) * 7 + (int) (strchr ("CDEFGAB", sharp_step[n->pitch % 12]) - "CDEFGAB");
            cnt++;
          }
      qsort (cn, (size_t) cnt, sizeof *cn, cmp_cnote);
      for (b = 0, i = 0; b < nbars; b++)
        {
          while (i < cnt && cn[i].on < bars[b].start)
            i++;
          first[b] = i;
        }
      first[nbars] = cnt;
      if (!(cost = calloc ((size_t) nbars, sizeof *cost)))
        {
          free (cn);
          free (first);
          return -1;
        }
      /* each bar's notes, in each; under a line, only from the first note
         that needs it to the last (the line can start and end in the bar) */
      for (b = 0; b < nbars; b++)
        for (j = 0; j < ns; j++)
          {
            int fi, li, a = first[b], e = first[b + 1];
            if (ot[j])
              {
                needing (cn + a, e - a, cl[j], &fi, &li);
                if (fi < 0)
                  fi = 0, li = e - a - 1;
              }
            else
              fi = 0, li = -1;
            for (i = a; i < e; i++)
              {
                int under = i - a >= fi && i - a <= li;
                cost[b][j] += ledger_cost (cn[i].dia - clef_bottom (cl[j]) - (under ? 7 * ot[j] : 0))
                  + (!under ? 0 : ledger_cost (cn[i].dia - clef_bottom (cl[j])) ? OTTAVA_NOTE + (abs (ot[j]) > 1)
                     : OTTAVA_IDLE);
              }
          }
      for (b = 0; b < nbars; b++)
        for (j = 0; j < ns; j++)
          if (j != home)
            cost[b][j] += CLEF_AWAY;
      /* from the end back, the least each bar on can cost in each; then
         forwards, staying as long as that's as good */
      for (b = nbars - 2; b >= 0; b--)
        for (j = 0; j < ns; j++)
          {
            long best = cost[b + 1][j];
            for (k = 0; k < ns; k++)
              {
                long c = cost[b + 1][k] + (cl[k] != cl[j] ? CLEF_CHANGE : 0) + (ot[k] != ot[j] ? OTTAVA_CHANGE : 0);
                if (k != j && c < best)
                  best = c;
              }
            cost[b][j] += best;
          }
      s0 = home;
      for (j = 0; j < ns; j++)
        if (cost[0][j] < cost[0][s0])
          s0 = j;
      cur = s0;
      for (b = 0; b < nbars; b++)
        {
          if (b > 0)
            {
              long best = cost[b][cur];
              int to = cur;
              for (k = 0; k < ns; k++)
                {
                  long c = cost[b][k] + (cl[k] != cl[cur] ? CLEF_CHANGE : 0) + (ot[k] != ot[cur] ? OTTAVA_CHANGE : 0);
                  if (k != cur && c < best)
                    best = c, to = k;
                }
              cur = to;
            }
          p->clefs[b * 2 + z] = (unsigned char) cl[cur];
          p->ottava[b * 2 + z] = (signed char) ot[cur];
        }
      p->clef[z] = cl[s0];
      /* a line starts at the first note that needs it in its first bar,
         and ends after the last in its last bar */
      for (b = 0; b < nbars; b++)
        {
          int o = p->ottava[b * 2 + z], fi, li, a = first[b], e = first[b + 1];
          if (!o)
            continue;
          needing (cn + a, e - a, p->clefs[b * 2 + z], &fi, &li);
          if (fi < 0)
            continue;
          if (b == 0 || p->ottava[(b - 1) * 2 + z] != o)
            p->ott_start[b * 2 + z] = cn[a + fi].on - bars[b].start;
          if (b == nbars - 1 || p->ottava[(b + 1) * 2 + z] != o)
            {
              /* (to where the next note after it starts, if one does in the bar) */
              for (i = a + li + 1; i < e && cn[i].on == cn[a + li].on; i++)
                ;
              p->ott_end[b * 2 + z] = i < e ? cn[i].on - bars[b].start : 0;
            }
        }
      free (cost);
    }
  free (cn);
  free (first);
  return 0;
}

gr_notation *gr_notation_make (const gr_song *s, int track, int min_bars)
{
  return gr_notation_make_ex (s, track, min_bars, 0);
}

gr_notation *gr_notation_make_ex (const gr_song *s, int track, int min_bars, int flags)
{
  static xlist v[GR_MAX_TRACKS][GR_NVOICES];
  static unsigned char *tp[GR_MAX_TRACKS][GR_NVOICES];
  static int tr_shift[GR_MAX_TRACKS];
  int *kf = NULL, *km = NULL, *wf = NULL;
  int nst[GR_MAX_TRACKS], t, k, z, last = 0, nbars, b, ok = 0;
  gr_notation *nt = calloc (1, sizeof *nt);
  xbar *bars = NULL;
  if (!nt)
    return NULL;
  memset (v, 0, sizeof v);
  memset (tp, 0, sizeof tp);
  memset (nst, 0, sizeof nst);

  /* Each drum or instrument track's notes, on one staff or two. (All of
     them, for the key, even when only one is written.) */
  for (t = 0; t < s->ntracks; t++)
    {
      xlist all = { NULL, 0, 0 };
      int lo = 127, hi = 0;
      const gr_track *tr = &s->tracks[t];
      if (tr->type == TRK_AUDIO)
        continue;
      if (track_notes (s, t, &all))
        {
          free (all.n);
          goto fail;
        }
      if (!all.count)
        {
          free (all.n);
          if (t == track)
            nst[t] = 1;
          continue;
        }
      nst[t] = 1;
      for (k = 0; k < all.count; k++)
        if (all.n[k].off > last)
          last = all.n[k].off;
      if (tr->type == TRK_DRUMS)
        {
          /* Hands (voice 1, stems up) and feet (voice 2, stems down: the
             bass drums and the pedal hi-hat). */
          for (k = 0; k < all.count; k++)
            {
              int p = all.n[k].pitch, feet = p == 35 || p == 36 || p == 44;
              if (add_x (&v[t][feet], &all.n[k]))
                {
                  free (all.n);
                  goto fail;
                }
            }
          free (all.n);
          continue;
        }
      for (k = 0; k < all.count; k++)
        {
          if (all.n[k].pitch < lo) lo = all.n[k].pitch;
          if (all.n[k].pitch > hi) hi = all.n[k].pitch;
        }
      if (lo < 55 && hi > 64 && instrument (tr).keyboard)
        {
          for (k = 0; k < all.count; k++)
            if (add (&v[t][all.n[k].pitch >= 60 ? 0 : 2], all.n[k].on, all.n[k].off,
                     all.n[k].pitch, all.n[k].vel))
              {
                free (all.n);
                goto fail;
              }
          free (all.n);
          nst[t] = 2;
        }
      else
        v[t][0] = all;
      /* Held notes to a second voice on each staff. */
      for (z = 0; z < nst[t]; z++)
        if (split_held (&v[t][z * 2], &v[t][z * 2 + 1])
            || move_cross_rhythms (&v[t][z * 2], &v[t][z * 2 + 1]))
          goto fail;
    }
  for (t = 0; t < s->ntracks; t++)
    if (nst[t] && (track < 0 || t == track))
      nt->nparts++;
  if (!nt->nparts)
    goto fail;
  nbars = last > 0 ? gr_tick_bar (s, last - 1) + 1 : 1;
  if (nbars < min_bars)
    nbars = min_bars;
  bars = calloc ((size_t) nbars, sizeof *bars);
  if (!bars)
    goto fail;
  for (b = 0; b < nbars; b++)
    {
      const gr_meter *m = gr_meter_at (s, b);
      bars[b].start = gr_bar_tick (s, b);
      bars[b].len = gr_bar_tick (s, b + 1) - bars[b].start;
      bars[b].num = m->num;
      bars[b].den = m->den;
      bars[b].compound = is_compound (m->num, m->den);
      bars[b].beat = bars[b].compound ? DIV * 4 / m->den * 3 : DIV * 4 / m->den;
    }
  nt->bars = bars;
  nt->nbars = nbars;
  for (t = 0; t < s->ntracks; t++)
    for (z = 0; z < GR_NVOICES; z++)
      if (v[t][z].count)
        {
          if (!(tp[t][z] = calloc ((size_t) nbars * 16, 1)))
            goto fail;
          quantise (s, &v[t][z], bars, nbars, tp[t][z]);
        }
  /* then the staccato notes and drum hits' lengths, by where all the
     part's notes start */
  for (t = 0; t < s->ntracks; t++)
    {
      int nons = 0, *ons;
      for (z = 0; z < GR_NVOICES; z++)
        nons += v[t][z].count;
      if (!nons)
        continue;
      if (!(ons = malloc ((size_t) nons * sizeof *ons)))
        goto fail;
      nons = 0;
      for (z = 0; z < GR_NVOICES; z++)
        for (k = 0; k < v[t][z].count; k++)
          ons[nons++] = v[t][z].n[k].on;
      qsort (ons, (size_t) nons, sizeof *ons, cmp_int);
      for (z = 0; z < GR_NVOICES; z++)
        if (v[t][z].count)
          {
            if (s->tracks[t].type == TRK_INST)
              staccato (s, &v[t][z], bars, nbars, ons, nons);
            else
              drum_lengths (s, &v[t][z], bars, nbars);
          }
      free (ons);
    }
  {
    static xlist ls[GR_MAX_TRACKS * GR_NVOICES];
    int nls = 0;
    for (t = 0; t < s->ntracks; t++)
      if (s->tracks[t].type == TRK_INST)
        for (z = 0; z < GR_NVOICES; z++)
          ls[nls++] = v[t][z];
    if (!(kf = malloc ((size_t) nbars * sizeof *kf)) || !(km = malloc ((size_t) nbars * sizeof *km))
        || !(wf = malloc ((size_t) nbars * sizeof *wf)) || find_keys (ls, nls, bars, nbars, s->key, kf, km))
      goto fail;
    for (b = 0; b < nbars; b++)
      {
        bars[b].fifths = kf[b];
        bars[b].minor = km[b];
        bars[b].src = b;
      }
    nt->fifths = kf[0];
    nt->minor = km[0];
    /* each part as it's written (a Bb clarinet a tone up, in two more
       sharps), then spelled in its key */
    for (t = 0; t < s->ntracks; t++)
      {
        tr_shift[t] = 0;
        if (s->tracks[t].type != TRK_INST)
          continue;
        {
          instr in = instrument (&s->tracks[t]);
          tr_shift[t] = in.key && (flags & GR_NT_CONCERT) ? in.transpose - in.transpose % 12 : in.transpose;
        }
        for (b = 0; b < nbars; b++)
          wf[b] = key_moved (kf[b], tr_shift[t]);
        for (z = 0; z < GR_NVOICES; z++)
          {
            for (k = 0; k < v[t][z].count; k++)
              {
                int pch = v[t][z].n[k].pitch + tr_shift[t];
                v[t][z].n[k].pitch = pch < 0 ? 0 : pch > 127 ? 127 : pch;
              }
            spell_list (&v[t][z], wf, km, bars, nbars);
          }
      }
  }

  nt->parts = calloc ((size_t) nt->nparts, sizeof *nt->parts);
  if (!nt->parts)
    goto fail;
  nt->nparts = 0;
  for (t = 0; t < s->ntracks; t++)
    {
      gr_npart *p;
      if (!nst[t] || (track >= 0 && t != track))
        continue;
      p = &nt->parts[nt->nparts++];
      p->track = t;
      snprintf (p->name, sizeof p->name, "%s", s->tracks[t].name);
      p->drums = s->tracks[t].type == TRK_DRUMS;
      p->staves = nst[t];
      for (z = 0; z < p->staves; z++)
        p->clef[z] = clef_for (&v[t][z * 2], p->staves, z, p->drums);
      p->transpose = tr_shift[t];
      if (!(p->key = malloc ((size_t) nbars)))
        goto fail;
      for (b = 0; b < nbars; b++)
        p->key[b] = (signed char) (p->drums ? 0 : key_moved (kf[b], tr_shift[t]));
      p->fifths = p->key[0];
      if (!p->drums && p->staves == 1)
        {
          instr in = instrument (&s->tracks[t]);
          if (in.clef >= 0)
            p->clef[0] = in.clef;
          /* (high for a cello: mostly above middle C's A below) */
          if (in.tenor && v[t][0].count)
            {
              long sum = 0;
              for (k = 0; k < v[t][0].count; k++)
                sum += v[t][0].n[k].pitch;
              if (sum / v[t][0].count >= 58)
                p->clef[0] = NC_C4;
            }
        }
      if (choose_clefs (p, v[t], bars, nbars, &s->tracks[t]))
        goto fail;
      p->voice = calloc ((size_t) nbars * GR_NVOICES, sizeof *p->voice);
      if (!p->voice)
        goto fail;
      if (!p->drums && track_pedal (s, t, bars, nbars, p))
        goto fail;
      for (b = 0; b < nbars; b++)
        for (z = 0; z < p->staves * 2; z++)
          {
            static const unsigned char none[16];
            gr_nrange *r = &p->voice[b * GR_NVOICES + z];
            int ntr = 16;
            /* (a drum part's feet show their rests) */
            int hidden = (z & 1) && !(p->drums && v[t][z].count);
            r->first = nt->nev;
            if (build_bar (nt, &v[t][z], &bars[b], tp[t][z] ? &tp[t][z][b * 16] : none, ntr, hidden, p->drums))
              goto fail;
            r->n = nt->nev - r->first;
            /* a bar with no drums at all: one bar's rest, not one a voice */
            if (z == 1 && r->n == 1 && nt->ev[r->first].kind == NE_BAR_REST && p->voice[b * GR_NVOICES].n == 1
                && nt->ev[p->voice[b * GR_NVOICES].first].kind == NE_BAR_REST)
              {
                nt->nev--;
                r->n = 0;
              }
          }
      mark_playing (nt, p);
    }
  nt->nsong = nbars;
  if (!(nt->bar_of = malloc ((size_t) nbars * sizeof *nt->bar_of)))
    goto fail;
  for (b = 0; b < nbars; b++)
    nt->bar_of[b] = b;
  if ((flags & GR_NT_REPEATS) && find_repeats (nt))
    goto fail;
  ok = 1;
fail:
  free (kf);
  free (km);
  free (wf);
  for (t = 0; t < s->ntracks; t++)
    for (z = 0; z < GR_NVOICES; z++)
      {
        free (v[t][z].n);
        free (tp[t][z]);
      }
  if (!ok)
    {
      if (!nt->bars)
        free (bars);
      gr_notation_free (nt);
      return NULL;
    }
  return nt;
}

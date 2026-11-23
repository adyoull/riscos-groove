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

typedef struct { int on, off, pitch, vel, flags, release, wr, spell, tie1; signed char step, alter; } xn;   /* wr, spell,
                                                                                    tie1: gr_note's */   /* release: when it really
                                                                     stops, from off */
#define XF_LEGATO 1               /* overlapped the next note (played legato) */
#define XF_STACC  2               /* let go early: written longer, staccato */
#define XF_TIEOUT 4               /* tied at its end to a note in another voice */
#define XF_TIEIN  8               /* tied at its start from one in another voice */
#define XF_CUE    16              /* a score's cue note (shown small, not played) */
#define XF_XDOWN  32              /* written on the staff below its voice's (a score's cross-staff chord) */
#define XF_XUP    64              /* ...or above */
#define XF_MOVED  128             /* a score's chord moved to this staff, in a voice of this one's */

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
  l->n[l->count].wr = 0;
  l->n[l->count].spell = 0;
  l->n[l->count].tie1 = 0;
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
  l->n[l->count - 1].wr = n->wr;
  l->n[l->count - 1].spell = n->spell;
  l->n[l->count - 1].tie1 = n->tie1;
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
            if ((n->wr & GR_WR) && (n->spell & GR_SP_HIDDEN))
              continue;                    /* (a score's note it doesn't show) */
            if (off > r->start + r->len)
              off = r->start + r->len;
            if (add (l, on, off, n->pitch, n->vel ? n->vel : 1))
              return -1;
            l->n[l->count - 1].spell = n->spell;
            if (n->tie1 > 0 && on + n->tie1 < off)
              l->n[l->count - 1].tie1 = n->tie1;
            if (n->tie1 == GR_TIE_OUT || n->tie1 == GR_TIE_BOTH)
              l->n[l->count - 1].flags |= XF_TIEOUT;
            if (n->tie1 == GR_TIE_IN || n->tie1 == GR_TIE_BOTH)
              l->n[l->count - 1].flags |= XF_TIEIN;
            if (n->wr & GR_WR)
              {
                xn *x = &l->n[l->count - 1];
                x->wr = n->wr;
                /* staccato: written twice as long as it plays */
                if ((n->wr & GR_WR_HALF) && off == on + n->len)
                  {
                    x->off = on + 2 * n->len;
                    x->release = -n->len;
                    x->flags |= XF_STACC;
                  }
              }
          }
    }
  if (l->count > 1)
    qsort (l->n, (size_t) l->count, sizeof *l->n, cmp_on);
  return 0;
}

/* The marks written in a track's patterns (from an imported score), in
   song ticks, in order. */
typedef struct { int on, len, kind, value, staff, pitch; } xmark;

/* A score's tuplet in a voice (GR_MK_TUPLET): on to on + len, n notes in
   the time of m, each written nv. */
typedef struct { int on, len, n, m, nv, show; } xtup;

/* The score's tuplets in voice rz (staff * GR_VPS + voice) that overlap
   from..to, in order, none inside another: up to max, how many. */
static int score_tuplets (const xmark *rm, int nrm, int rz, int from, int to, xtup *w, int max)
{
  int k, n = 0;
  for (k = 0; k < nrm && n < max; k++)
    if (rm[k].kind == GR_MK_TUPLET && rm[k].len > 0 && !(rm[k].value & 0x20)
        && (rm[k].staff & 1) == rz / GR_VPS && (rm[k].value & 3) == rz % GR_VPS
        && rm[k].on < to && rm[k].on + rm[k].len > from && rm[k].pitch > 0 && (rm[k].staff >> 1) > 0
        && (!n || rm[k].on >= w[n - 1].on + w[n - 1].len))
      {
        w[n].on = rm[k].on;
        w[n].len = rm[k].len;
        w[n].n = rm[k].pitch;
        w[n].m = rm[k].staff >> 1;
        w[n].nv = (rm[k].value >> 2) & 7;
        w[n].show = (rm[k].value & 0x40 ? 8 : 0) | (rm[k].value & 0x80 ? 16 : 0);
        n++;
      }
  return n;
}


static int cmp_xmark (const void *a, const void *b)
{
  const xmark *x = a, *y = b;
  return x->on != y->on ? (x->on < y->on ? -1 : 1) : x->kind - y->kind;
}

static int track_marks (const gr_song *s, int t, xmark **out)
{
  xmark *m = NULL;
  int n = 0, cap = 0, i, k, rep;
  for (i = 0; i < GR_MAX_REGIONS; i++)
    {
      const gr_region *r = &s->regions[i];
      const gr_pattern *pt;
      if (!r->used || r->track != t || r->pattern < 0 || r->clip >= 0)
        continue;
      pt = &s->patterns[r->pattern];
      if (pt->len <= 0)
        continue;
      for (rep = 0; rep * pt->len < r->len; rep++)
        for (k = 0; k < pt->nmarks; k++)
          {
            int at = r->start + rep * pt->len + pt->marks[k].tick;
            if (at >= r->start + r->len)
              continue;
            if (n == cap)
              {
                xmark *a = realloc (m, (size_t) (cap = cap ? cap * 2 : 64) * sizeof *a);
                if (!a)
                  {
                    free (m);
                    return -1;
                  }
                m = a;
              }
            m[n].on = at;
            m[n].len = pt->marks[k].len;
            m[n].kind = pt->marks[k].kind;
            m[n].value = pt->marks[k].value;
            m[n].staff = pt->marks[k].staff;
            m[n].pitch = pt->marks[k].pitch;
            n++;
          }
    }
  if (n > 1)
    qsort (m, (size_t) n, sizeof *m, cmp_xmark);
  *out = m;
  return n;
}

static int has_mark (const xmark *m, int n, int kind, int on)
{
  int i;
  for (i = 0; i < n; i++)
    if (m[i].kind == kind && m[i].on >= on - 2 && m[i].on <= on + 2)
      return 1;
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
      p->pedal[p->npedal].hidden = 0;
      p->pedal[p->npedal].line = 0;
      p->pedal[p->npedal].above = 0;
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
  TW_DUP,                       /* duplets: two in a dotted beat (6/8...), at 0 and 72 */
  TW_TRIP2,                     /* triplets in each half of a beat (a score's semiquaver ones) */
  TW_TRIP6,                     /* a score's sextuplet in a beat: as TW_TRIP, written 6:4 */
  TW_SEXT,                      /* a score's six quavers over this beat and the next (6:4) */
  TW_SEXT2,                     /* (the second beat of one) */
  TW_TRIP4,                     /* triplets in each quarter of a beat (a score's demisemiquaver ones) */
  TW_EXPL                       /* (a score's own tuplet, written as it says: build_bar) */
};

/* The kinds over two beats, and their second beats. */
#define TW_TWO(k) ((k) == TW_CTRIP || (k) == TW_SEXT)
#define TW_SECOND(k) ((k) == TW_CONT || (k) == TW_SEXT2)

/* Beats of a bar (tp, 16) that a score's tuplet covers part of: written
   from it, not a grid (and a two-beat kind that loses half, neither). */
static void clear_tuplet_beats (unsigned char *tp, const xtup *w, int nw, int start, int beat, int len, int to)
{
  int k, i;
  for (i = 0; i < nw; i++)
    for (k = 0; k < 16 && k * beat < len; k++)
      if (w[i].on < start + (k + 1) * beat && w[i].on + w[i].len > start + k * beat)
        tp[k] = (unsigned char) to;
  for (k = 0; k < 16; k++)
    {
      if (TW_SECOND (tp[k]) && (k == 0 || !TW_TWO (tp[k - 1])))
        tp[k] = TW_NONE;
      if (TW_TWO (tp[k]) && (k == 15 || !TW_SECOND (tp[k + 1])))
        tp[k] = TW_NONE;
    }
}

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

/* A beat len ticks long from w0 whose notes start and end nearer a
   16-tick grid (triplets) than a 12-tick one. */
static int grid_triplets (const xlist *x, int w0, int len)
{
  static int d[4096];
  int n = inside (x, w0, len, d, 4096), i;
  long e12 = 0, e16 = 0;
  if (!n)
    return 0;
  for (i = 0; i < n; i++)
    {
      int r = d[i] % 12;
      e12 += r < 6 ? r : 12 - r;
      r = d[i] % 16;
      e16 += r < 8 ? r : 16 - r;
    }
  return e16 < e12;
}

/* A dotted beat (144 ticks) whose quavers are triplets of semiquavers:
   its notes' starts and ends nearer a 16-tick grid than a 12-tick one. */
static int compound_triplets (const xlist *x, int w0)
{
  static int d[4096];
  int n = inside (x, w0, 144, d, 4096), i;
  long e12 = 0, e16 = 0;
  if (!n)
    return 0;
  for (i = 0; i < n; i++)
    {
      int r = d[i] % 12;
      e12 += r < 6 ? r : 12 - r;
      r = d[i] % 16;
      e16 += r < 8 ? r : 16 - r;
    }
  return e16 < e12;
}

static int snap (int t, int grid, int w0)
{
  int d = t - w0;
  return w0 + (d + grid / 2) / grid * grid;
}

/* ---- note values ------------------------------------------------------------ */

typedef struct { int ticks, type, dots; } value;

#define NV_BREVE 8              /* (a value's type only: a breve, written NV_WHOLE with breve set) */
static const value straight[] = {
  { 1152, NV_BREVE, 1 }, { 768, NV_BREVE, 0 }, { 672, NV_WHOLE, 2 }, { 576, NV_WHOLE, 1 },
  { 384, NV_WHOLE, 0 }, { 336, NV_HALF, 2 }, { 288, NV_HALF, 1 }, { 192, NV_HALF, 0 }, { 168, NV_QUARTER, 2 },
  { 144, NV_QUARTER, 1 }, { 96, NV_QUARTER, 0 }, { 84, NV_EIGHTH, 2 }, { 72, NV_EIGHTH, 1 }, { 48, NV_EIGHTH, 0 },
  { 42, NV_16TH, 2 }, { 36, NV_16TH, 1 }, { 24, NV_16TH, 0 }, { 21, NV_32ND, 2 }, { 18, NV_32ND, 1 },
  { 12, NV_32ND, 0 }, { 9, NV_64TH, 1 }, { 6, NV_64TH, 0 }, { 3, NV_128TH, 0 } };

/* In a triplet beat: the written value, three in the time of two. */
static const value triplet[] = {
  { 64, NV_QUARTER, 0 }, { 32, NV_EIGHTH, 0 }, { 16, NV_16TH, 0 }, { 8, NV_32ND, 0 } };
/* (dotted, in a tuplet only as a score writes them) */
static const value triplet_dotted[] = { { 48, NV_EIGHTH, 1 }, { 24, NV_16TH, 1 }, { 12, NV_32ND, 1 } };

const char *gr_nvalue_name (int v)
{
  static const char *const n[] = { "whole", "half", "quarter", "eighth", "16th", "32nd", "64th", "128th" };
  return v >= 0 && v <= NV_128TH ? n[v] : "quarter";
}

int gr_nvalue_ticks (int v)
{
  return v >= 0 && v <= NV_128TH ? 384 >> v : 96;
}

/* The longest value from a (ticks into the bar) that fits before b.
   written: the voice is from a score, whose notes keep the values they
   were written with: 1, [a, b) is the whole of a note (as long as one
   value, it's written as that, wherever it starts); 2, it's a rest
   (not dotted, in simple time). */
static const value *next_value (const xbar *bar, int a, int b, int in_triplet, int win, int written)
{
  int i, room = b - a;
  if (in_triplet)
    {
      if (written == 1)
        for (i = 0; i < 3; i++)
          if (triplet_dotted[i].ticks == room)
            return &triplet_dotted[i];
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
      if (v->ticks > room)
        continue;
      if (written == 1 && v->ticks == room)
        return v;
      /* (double dots, and a dotted semibreve or a 64th, only as a score writes them) */
      if (v->dots == 2 || v->ticks == 576 || v->ticks == 9 || v->ticks == 1152)
        continue;
      if (written == 2 && v->dots && !bar->compound)
        continue;
      if (a % align)
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
static int tuplet_kind (int tup, int len, const xbar *bar, int beat)
{
  return tup == GR_TUP_5 && len == 0x40 && beat == DIV ? TW_QUINT
    : tup == GR_TUP_2 && bar->compound ? TW_DUP
    : tup == GR_TUP_6 && len == 0x40 && !bar->compound && beat == DIV ? TW_TRIP6
    : tup == GR_TUP_6 && len == 0x60 && !bar->compound && beat == DIV ? TW_SEXT
    : (tup == GR_TUP_3 || tup == GR_TUP_6) && len == 0x40 ? TW_TRIP
    : tup == GR_TUP_3 && len == 0x20 ? (bar->compound ? TW_TRIP : beat == DIV ? TW_TRIP2 : TW_TRIP)
    : tup == GR_TUP_3 && len == 0x60 && beat == DIV ? TW_CTRIP : -1;
}

/* (rm, nrm: the score's rests, those on staff z / GR_VPS in voice
   z % GR_VPS being this voice's) */
static void quantise (const gr_song *s, xlist *x, const xbar *bars, int nbars, unsigned char *tp,
                      const xmark *rm, int nrm, int z)
{
  int b, k, j, wn, nw = 0;
  /* (a score's voice: its notes say which beats are tuplets, below) */
  for (k = 0; k < x->count; k++)
    nw += (x->n[k].wr & GR_WR) != 0;
  for (b = 0; b < nbars && nw * 2 <= x->count; b++)
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
      else if (!bars[b].compound && (beat == DIV / 2 || beat == 2 * DIV))
        {
          /* quaver beats (3/8) or minim ones (2/2): triplets in a beat */
          for (k = 0; k < 16 && (k + 1) * beat <= bars[b].len; k++)
            if (grid_triplets (x, bars[b].start + k * beat, beat))
              tp[b * 16 + k] = TW_TRIP;
        }
      else if (bars[b].compound && beat == 144)
        for (k = 0; k < 16 && (k + 1) * beat <= bars[b].len; k++)
          {
            if (duplets (x, bars[b].start + k * beat))
              tp[b * 16 + k] = TW_DUP;
            else if (compound_triplets (x, bars[b].start + k * beat))
              tp[b * 16 + k] = TW_TRIP;     /* (each quaver of it a triplet of semiquavers) */
          }
    }
  /* A score's notes say which tuplets they're in: those beats are
     written so, and others straight. */
  {
    static unsigned char seen[16];
    int bb = -1;
    for (k = 0; k <= x->count; k++)
      {
        const xn *n = k < x->count ? &x->n[k] : NULL;
        int nb = n ? gr_tick_bar (s, n->on < 0 ? 0 : n->on) : -1, w, beat, kind, tup, len;
        if (nb >= nbars)
          nb = nbars - 1;
        if (nb != bb)
          {
            /* the bar before: its beats with score notes in them */
            if (bb >= 0)
              for (w = 0; w < 16; w++)
                if (seen[w] == 1 && !TW_SECOND (tp[bb * 16 + w]))
                  tp[bb * 16 + w] = TW_NONE;
            memset (seen, 0, sizeof seen);
            bb = nb;
          }
        if (!n || !(n->wr & GR_WR) || nb < 0)
          continue;
        beat = bars[nb].beat;
        w = (n->on - bars[nb].start) / beat;
        if (w < 0 || w >= 16)
          continue;
        tup = (n->spell & GR_SP_TUPLET) >> 5;
        len = n->wr & GR_WR_TUPLEN;
        if (!tup)
          {
            if (!seen[w])
              seen[w] = 1;
            continue;
          }
        seen[w] = 2;
        kind = tuplet_kind (tup, len, &bars[nb], beat);
        /* (demisemiquaver triplets: a quarter of a beat each three) */
        if (kind == TW_TRIP2 && (n->off - n->on < 16 || tp[nb * 16 + w] == TW_TRIP4))
          kind = TW_TRIP4;
        if (TW_TWO (kind))
          {
            /* (over this beat and the next, or the one before and this) */
            int w0 = TW_SECOND (tp[nb * 16 + w]) ? w - 1 : w;
            if (w0 + 1 < 16 && (w0 + 2) * beat <= bars[nb].len)
              {
                tp[nb * 16 + w0] = (unsigned char) kind;
                tp[nb * 16 + w0 + 1] = kind == TW_CTRIP ? TW_CONT : TW_SEXT2;
                seen[w0] = seen[w0 + 1] = 2;
              }
          }
        else if (kind >= 0 && !TW_SECOND (tp[nb * 16 + w]))
          tp[nb * 16 + w] = (unsigned char) kind;
      }
  }
  /* and the score's rests in tuplets, where no note says otherwise */
  for (k = 0; k < nrm; k++)
    if (rm[k].kind == GR_MK_REST && (rm[k].staff > 0) == (z / GR_VPS > 0) && (rm[k].value & 3) == z % GR_VPS
        && ((rm[k].value >> 2) & 7))
      {
        int nb = gr_tick_bar (s, rm[k].on < 0 ? 0 : rm[k].on), w, kind;
        if (nb >= nbars)
          nb = nbars - 1;
        w = (rm[k].on - bars[nb].start) / bars[nb].beat;
        if (w < 0 || w >= 16 || tp[nb * 16 + w] != TW_NONE)
          continue;
        kind = tuplet_kind ((rm[k].value >> 2) & 7, rm[k].value & 0x60, &bars[nb], bars[nb].beat);
        if (TW_TWO (kind))
          {
            if (w + 1 < 16 && (w + 2) * bars[nb].beat <= bars[nb].len && tp[nb * 16 + w + 1] == TW_NONE)
              {
                tp[nb * 16 + w] = (unsigned char) kind;
                tp[nb * 16 + w + 1] = kind == TW_CTRIP ? TW_CONT : TW_SEXT2;
              }
          }
        else if (kind >= 0)
          tp[nb * 16 + w] = (unsigned char) kind;
      }
  /* Beats a score's tuplet covers: its notes are where the score puts
     them, not on a grid */
  if (nrm > 0)
    {
      xtup *all = malloc ((size_t) nrm * sizeof *all);
      int nw = all ? score_tuplets (rm, nrm, z, -0x7fffffff, 0x7fffffff, all, nrm) : 0, i;
      for (i = 0; i < nw; i++)
        for (b = gr_tick_bar (s, all[i].on < 0 ? 0 : all[i].on); b >= 0 && b < nbars && bars[b].start < all[i].on + all[i].len; b++)
          clear_tuplet_beats (&tp[b * 16], &all[i], 1, bars[b].start, bars[b].beat, bars[b].len, TW_EXPL);
      free (all);
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
            case TW_TRIP2:
            case TW_TRIP4: *tt[q] = snap (v, (x->n[k].wr & GR_WR) ? 4 : kind == TW_TRIP4 ? 8 : 16, w0); break;
            case TW_TRIP:
            case TW_TRIP6:
            case TW_SEXT:  *tt[q] = snap (v, 16, w0); break;
            case TW_SEXT2: *tt[q] = snap (v, 16, w0 - DIV); break;
            case TW_QUINT: *tt[q] = w0 + q5[near_q5 (v - w0)]; break;
            case TW_CTRIP: *tt[q] = snap (v, 64, w0); break;
            case TW_CONT:  *tt[q] = snap (v, 64, w0 - DIV); break;
            case TW_DUP:   *tt[q] = snap (v, 72, w0); break;
            case TW_EXPL:  break;                 /* (a score's own tuplet: where it puts them) */
            default:       *tt[q] = snap (v, (x->n[k].wr & GR_WR) ? 3 : 12, w0); break;   /* (a score's 64ths, 128ths) */
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

/* (A part from a score with marks (nm >= 0): a note is staccato where the
   score says, and only there.) */
static void staccato (const gr_song *s, xlist *x, const xbar *bars, int nbars, const int *ons, int nons,
                      const xmark *mk, int nm)
{
  int k = 0, sounding = 0;
  while (k < x->count)
    {
      int j, g = k, on = x->n[k].on, next = -1, bb, beat, slot, ok = 1, into, marked = 0;
      while (g < x->count && x->n[g].on == on)
        g++;
      if (x->n[k].wr & GR_WR)
        {
          /* written in a score: already its written length, and its dot
             where the score has one (track_notes) */
          for (j = k; j < g; j++)
            {
              if (x->n[j].off > sounding)
                sounding = x->n[j].off;
            }
          k = g;
          continue;
        }
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
      if (nm >= 0)
        {
          marked = has_mark (mk, nm, GR_MK_STACCATO, on);
          if (!marked)
            ok = 0;
          for (j = k; j < g && ok; j++)
            if (x->n[j].off > on + slot)
              ok = 0;
        }
      else
        {
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
        }
      for (j = k; j < g && ok; j++)
        {
          x->n[j].release = x->n[j].off - (on + slot);
          x->n[j].off = on + slot;
          x->n[j].flags |= XF_STACC;
        }
      for (j = k; j < g && marked && !ok; j++)
        x->n[j].flags |= XF_STACC;        /* (the dot, on the note as long as it's played) */
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
  nt->ev[nt->nev].tup_at = 0xFFFF;
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

/* (the drum part being written: its score's kit, where it has one) */
static const unsigned char *cur_drumline;

static void spell (gr_nhead *h, int pitch, int fifths, int drums)
{
  if (drums)
    {
      int line, head, pos;
      drum_place (pitch, &line, &head);
      if (cur_drumline && pitch >= 0 && pitch < 128 && cur_drumline[pitch])
        line = cur_drumline[pitch] - 64;      /* (as the score's kit writes it) */
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
/* shift: the part is written that many semitones above how it sounds (a
   score's spelling, of how it sounds, is moved by the interval). */
static void spell_list (xlist *l, const int *kf, const int *km, const xbar *bars, int nbars, int shift)
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
      if (n->spell & GR_SP)
        {
          /* as the score spelled it: in fifths from C (tpc), moved by the
             interval it's written at, and back to a letter and accidental */
          static const signed char letter[12] = { 0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6 };
          static const signed char fifths_of[7] = { 0, 2, 4, -1, 1, 3, 5 };          /* C D E F G A B */
          static const signed char by_semis[12] = { 0, -5, 2, -3, 4, -1, 6, 1, -4, 3, -2, 5 };
          static const signed char of_fifths[7] = { 3, 0, 4, 1, 5, 2, 6 };            /* F C G D A E B */
          int alter = (n->spell & 7) - 2, cpc = ((n->pitch - shift) % 12 + 12) % 12;
          int nat = ((cpc - alter) % 12 + 12) % 12;
          if (alter >= -2 && alter <= 2 && letter[nat] >= 0)
            {
              int f = fifths_of[letter[nat]] + 7 * alter + by_semis[((shift % 12) + 12) % 12];
              int a2 = (int) floor ((f + 1) / 7.0), st = of_fifths[f + 1 - 7 * a2];
              if (a2 >= -2 && a2 <= 2)
                {
                  n->step = (signed char) st;
                  n->alter = (signed char) a2;
                  continue;
                }
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
/* (A score's rests in this voice, from its marks: rm, nrm of them, those
   of kind GR_MK_REST on staff rz / GR_VPS in voice rz % GR_VPS.) */
static int rest_mark (const xmark *rm, int nrm, int rz, int k)
{
  (void) nrm;
  return rm[k].kind == GR_MK_REST && (rm[k].staff > 0) == (rz / GR_VPS > 0) && (rm[k].value & 3) == rz % GR_VPS;
}

/* Is [a, b) (ticks) a rest the score writes, or inside one (2: exactly one)? */
static int score_rest (const xmark *rm, int nrm, int rz, int a, int b)
{
  int k, in = 0;
  for (k = 0; k < nrm && rm[k].on < b; k++)
    if (rest_mark (rm, nrm, rz, k) && rm[k].on <= a && rm[k].on + rm[k].len >= b)
      {
        if (rm[k].on == a && rm[k].on + rm[k].len == b)
          return 2;
        in = 1;
      }
  return in;
}

/* A score's tie at t inside a note of this voice (rz: staff * GR_VPS +
   voice) that sounds through it (GR_MK_TIE): the note's, else -1. */
static int tie_mark_at (const xlist *l, const xmark *rm, int nrm, int rz, int t)
{
  int k, q;
  for (k = 0; k < nrm; k++)
    if (rm[k].kind == GR_MK_TIE && rm[k].on == t && (rm[k].value & 3) == rz % GR_VPS && (rm[k].staff & 1) == rz / GR_VPS)
      for (q = 0; q < l->count; q++)
        if (l->n[q].pitch == rm[k].pitch && l->n[q].on < t && l->n[q].off > t)
          return q;
  return -1;
}

static int build_bar (gr_notation *nt, const xlist *l, const xbar *bar, const unsigned char *trip, int ntrip,
                      int hidden, int drums, const xmark *rm, int nrm, int rz)
{
  int *bd, nb = 0, cap = 8, i, k, end = bar->start + bar->len, any = 0, beat = bar->beat, e_first, written = 0;
  int score_rests = 0;
  unsigned char mine[16];
  for (k = 0; k < l->count && !written; k++)
    written = (l->n[k].wr & GR_WR) && l->n[k].on < end && l->n[k].off > bar->start;
  for (k = 0; k < nrm && written; k++)
    if (rest_mark (rm, nrm, rz, k) && rm[k].on < end && rm[k].on + rm[k].len > bar->start)
      score_rests++;
  /* A window is written as tuplets only if this voice has a note
     starting or ending inside it: notes on the beat stay plain. */
  memset (mine, 0, sizeof mine);
  for (k = 0; k < ntrip; k++)
    if (trip[k] && !TW_SECOND (trip[k]))
      {
        int len = TW_TWO (trip[k]) ? 2 * DIV : beat, q;
        for (q = 0; q < l->count + nrm; q++)
          {
            /* (its notes, and the score's rests in tuplets in it) */
            int on, off, a, b;
            if (q < l->count)
              on = l->n[q].on, off = l->n[q].off;
            else if (rest_mark (rm, nrm, rz, q - l->count) && ((rm[q - l->count].value >> 2) & 7))
              on = rm[q - l->count].on, off = on + rm[q - l->count].len;
            else
              continue;
            a = on - bar->start - k * beat;
            b = off - bar->start - k * beat;
            if ((a > 0 && a < len) || (b > 0 && b < len))
              {
                mine[k] = trip[k];
                if (TW_TWO (trip[k]) && k + 1 < 16)
                  mine[k + 1] = trip[k] == TW_CTRIP ? TW_CONT : TW_SEXT2;
                break;
              }
          }
      }
  trip = mine;
  /* The score's own tuplets in this voice: written as it writes them */
  xtup tw[16];
  int ntw = written ? score_tuplets (rm, nrm, rz, bar->start, end, tw, 16) : 0, wcur = -1, wcum = 0;
  if (ntw)
    clear_tuplet_beats (mine, tw, ntw, bar->start, beat, bar->len, TW_NONE);
  for (k = 0; k < l->count; k++)
    if (l->n[k].on < end && l->n[k].off > bar->start)
      {
        any = 1;
        break;
      }
  if (!any)
    {
      /* No notes: a bar's rest, or none in a second voice; or the score's
         own rests in this voice, where it has some (but not the bar's) */
      int nr = 0, whole = 0;
      for (k = 0; k < nrm && !drums; k++)
        if (rest_mark (rm, nrm, rz, k) && rm[k].on < end && rm[k].on + rm[k].len > bar->start)
          {
            nr++;
            whole |= rm[k].on <= bar->start && rm[k].on + rm[k].len >= end;
          }
      if (nr && !whole)
        {
          written = 1;
          score_rests = nr;
          any = 1;
        }
    }
  if (!any)
    {
      int e, nr = 0;
      for (k = 0; k < nrm && !drums; k++)
        nr += rest_mark (rm, nrm, rz, k) && rm[k].on < end && rm[k].on + rm[k].len > bar->start;
      if (hidden && !nr)
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
      cap += 3;
  cap += 8 * ntrip + 2 * score_rests + 2 * ntw;
  for (k = 0; k < nrm && written; k++)
    cap += rm[k].kind == GR_MK_TIE;
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
        /* (where a score tied two notes together) */
        if (l->n[k].tie1 > 0 && l->n[k].on + l->n[k].tie1 > bar->start && l->n[k].on + l->n[k].tie1 < end
            && l->n[k].on + l->n[k].tie1 < l->n[k].off)
          bd[nb++] = l->n[k].on + l->n[k].tie1 - bar->start;
      }
  /* (where it tied a long note's notes after the first, too) */
  for (k = 0; k < nrm && written; k++)
    if (rm[k].kind == GR_MK_TIE && rm[k].on > bar->start && rm[k].on < end && tie_mark_at (l, rm, nrm, rz, rm[k].on) >= 0)
      bd[nb++] = rm[k].on - bar->start;
  /* (and where the score's own rests start and end) */
  for (k = 0; k < nrm && score_rests; k++)
    if (rest_mark (rm, nrm, rz, k) && rm[k].on < end && rm[k].on + rm[k].len > bar->start)
      {
        if (rm[k].on > bar->start)
          bd[nb++] = rm[k].on - bar->start;
        if (rm[k].on + rm[k].len < end)
          bd[nb++] = rm[k].on + rm[k].len - bar->start;
      }
  for (k = 0; k < ntrip; k++)
    if (trip[k] && !TW_SECOND (trip[k]))
      {
        int len = TW_TWO (trip[k]) ? 2 * DIV : beat;
        bd[nb++] = k * beat;
        bd[nb++] = k * beat + len < bar->len ? k * beat + len : bar->len;
        if (trip[k] == TW_TRIP && bar->compound)
          {
            bd[nb++] = k * beat + beat / 3;
            bd[nb++] = k * beat + 2 * beat / 3;
          }
        if (trip[k] == TW_TRIP2 || trip[k] == TW_TRIP4)
          {
            /* (around each part of the beat that has the score's triplets) */
            int sub = trip[k] == TW_TRIP4 ? beat / 4 : beat / 2, j, q;
            for (j = 0; j < beat; j += sub)
              {
                int here = !written, a0 = bar->start + k * beat + j;
                for (q = 0; q < l->count && !here; q++)
                  here = (l->n[q].spell & GR_SP_TUPLET) && l->n[q].on >= a0 && l->n[q].on < a0 + sub;
                for (q = 0; q < nrm && !here; q++)
                  here = rest_mark (rm, nrm, rz, q) && ((rm[q].value >> 2) & 7) && rm[q].on >= a0 && rm[q].on < a0 + sub;
                if (here)
                  {
                    bd[nb++] = k * beat + j;
                    bd[nb++] = k * beat + j + sub;
                  }
              }
          }
      }
  for (k = 0; k < ntw; k++)
    {
      int wa = tw[k].on - bar->start, wb = wa + tw[k].len;
      bd[nb++] = wa > 0 ? wa : 0;
      bd[nb++] = wb < bar->len ? wb : bar->len;
    }
  qsort (bd, (size_t) nb, sizeof *bd, cmp_int);
  for (i = 0; i + 1 < nb; i++)
    {
      int ex = -1;
      int a = bd[i], b = bd[i + 1];
      int win = a / beat, kind = win < ntrip ? trip[win] : TW_NONE, w0, wlen;
      if (b <= a)
        continue;
      if (TW_SECOND (kind))
        win--, kind = kind == TW_CONT ? TW_CTRIP : TW_SEXT;
      w0 = win * beat;
      wlen = TW_TWO (kind) ? 2 * DIV : beat;
      if (kind == TW_TRIP && bar->compound)
        {
          /* a triplet a quaver */
          w0 += (a - w0) / (beat / 3) * (beat / 3);
          wlen = beat / 3;
        }
      if (kind == TW_TRIP2 || kind == TW_TRIP4)
        {
          /* a triplet each half (or quarter) beat, where the score has
             one: straight in the rest of the beat */
          int sub = kind == TW_TRIP4 ? beat / 4 : beat / 2, q, here = 0;
          w0 += (a - w0) / sub * sub;
          for (q = 0; q < l->count && !here; q++)
            here = (l->n[q].spell & GR_SP_TUPLET) && l->n[q].on >= bar->start + w0 && l->n[q].on < bar->start + w0 + sub;
          for (q = 0; q < nrm && !here; q++)
            here = rest_mark (rm, nrm, rz, q) && ((rm[q].value >> 2) & 7) && rm[q].on >= bar->start + w0
                   && rm[q].on < bar->start + w0 + sub;
          if (here || !written)
            {
              wlen = sub;
              kind = TW_TRIP;
            }
          else
            {
              w0 = win * beat;
              kind = TW_NONE;
            }
        }
      for (k = 0; k < ntw && ex < 0; k++)
        {
          int wa = tw[k].on - bar->start, wb = wa + tw[k].len;
          if (a >= (wa > 0 ? wa : 0) && b <= (wb < bar->len ? wb : bar->len))
            ex = k;
        }
      if (ex < 0 && kind == TW_EXPL)
        kind = TW_NONE;
      if (ex >= 0)
        {
          kind = TW_EXPL;
          w0 = tw[ex].on - bar->start;
          wlen = tw[ex].len;
          if (ex != wcur)
            {
              /* (where in it this bar starts, if it began in the bar before) */
              wcur = ex;
              wcum = w0 >= 0 ? 0 : (int) floor ((double) -w0 * tw[ex].n / tw[ex].m + 0.5);
            }
        }
      while (a < b)
        {
          value tv;
          const value *v = &tv;
          int d, tup = 0, t0, t1, e;
          gr_nevent *ev;
          int wmode = 0;
          if (written)
            {
              int q, sounding = 0, whole = 0;
              for (q = 0; q < l->count; q++)
                if (l->n[q].on <= bar->start + a && l->n[q].off > bar->start + a)
                  {
                    sounding = 1;
                    int A = bar->start + a, B = bar->start + b, tt = l->n[q].tie1 ? l->n[q].on + l->n[q].tie1 : -1;
                    /* (a barline is where a score ties a note on, too) */
                    int from = l->n[q].on >= bar->start ? l->n[q].on : bar->start;
                    int to = l->n[q].off <= end ? l->n[q].off : end;
                    whole |= (from == A && to == B) || (from == A && tt == B) || (tt == A && to == B);
                    /* (or between the score's other ties in it) */
                    if (!whole && nrm)
                      {
                        int ta = tie_mark_at (l, rm, nrm, rz, A) == q, tb = tie_mark_at (l, rm, nrm, rz, B) == q;
                        whole |= (ta || A == from || A == tt) && (tb || B == to || B == tt) && (ta || tb);
                      }
                  }
              wmode = whole ? 1 : sounding ? 0 : 2;
              /* a rest written as the score writes it */
              if (!sounding && score_rests && score_rest (rm, nrm, rz, bar->start + a, bar->start + b) == 2)
                wmode = 1;
            }
          switch (kind)
            {
            case TW_TRIP:
            case TW_TRIP6:
            case TW_SEXT:
              v = next_value (bar, a, b, 1, w0, wmode);
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
                if (j0 > 4) j0 = 4;
                if (u > 5 - j0) u = 5 - j0;
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
            case TW_EXPL:
              {
                /* its written value: the time it takes, n in the time of m */
                double want = (double) (b - a) * tw[ex].n / tw[ex].m, tol = 1.6 * tw[ex].n / tw[ex].m;
                int q2, ns = (int) (sizeof straight / sizeof straight[0]);
                tv = straight[ns - 1];
                for (q2 = 0; q2 < ns; q2++)
                  if (straight[q2].ticks <= want + tol && (straight[q2].dots < 2 || fabs (straight[q2].ticks - want) <= tol))
                    {
                      tv = straight[q2];
                      break;
                    }
              }
              break;
            default:
              v = next_value (bar, a, b, 0, w0, wmode);
              kind = TW_NONE;
              break;
            }
          d = v->ticks;
          if (kind == TW_EXPL)
            {
              double want = (double) (b - a) * tw[ex].n / tw[ex].m;
              d = fabs (v->ticks - want) <= 1.6 * tw[ex].n / tw[ex].m ? b - a
                : (int) floor ((double) v->ticks * tw[ex].m / tw[ex].n + 0.5);
              if (d < 1)
                d = 1;
            }
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
          ev->value = (unsigned char) (v->type == NV_BREVE ? NV_WHOLE : v->type);
          ev->breve = v->type == NV_BREVE;
          ev->dots = (unsigned char) v->dots;
          ev->tuplet = (unsigned char) tup;
          if (tup && kind == TW_EXPL)
            {
              ev->tuplet |= (unsigned char) tw[ex].show;
              ev->tup_n = (unsigned char) tw[ex].n;
              ev->tup_m = (unsigned char) tw[ex].m;
              ev->tup_len = (unsigned short) (wlen < 0xFFFF ? wlen : 0xFFFF);
              ev->tup_at = (unsigned short) (wcum < 0xFFFF ? wcum : 0xFFFE);
              wcum += v->ticks;
            }
          else if (tup)
            {
              ev->tup_n = kind == TW_QUINT ? 5 : kind == TW_DUP ? 2 : kind == TW_TRIP6 || kind == TW_SEXT ? 6 : 3;
              ev->tup_m = kind == TW_QUINT ? 4 : kind == TW_DUP ? 3 : kind == TW_TRIP6 || kind == TW_SEXT ? 4 : 2;
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
                  h->tie_stop = (n->on < t0 || ((n->flags & XF_TIEIN) && n->on == t0)) && !drums;
                  h->tie_start = (n->off > t1 || ((n->flags & XF_TIEOUT) && n->off == t1)) && !drums;
                  h->legato = (n->flags & XF_LEGATO) != 0;
                  if (n->off == t1)
                    h->release = (short) n->release;
                  if ((n->flags & XF_STACC) && n->off == t1)
                    ev->artic |= NA_STACCATO;
                  if ((n->flags & XF_CUE) && !ev->nheads)
                    ev->artic |= NA_CUE;
                  if (n->flags & (XF_XDOWN | XF_XUP))
                    ev->xstaff = n->flags & XF_XDOWN ? 1 : -1;
                  if (n->flags & XF_MOVED)
                    ev->moved = 1;
                  ev->kind = NE_NOTE;
                  ev->nheads++;
                }
            }
          /* A score's rests are where it has them (in a second voice too,
             and none where it hides one); else a second voice's gaps
             are spaces. */
          if (ev->kind == NE_REST && written)
            {
              if (!score_rest (rm, nrm, rz, bar->start + ev->start, bar->start + ev->start + ev->dur))
                ev->kind = NE_SPACE;
            }
          else if (ev->kind == NE_REST && hidden)
            ev->kind = NE_SPACE;
          a += d;
        }
    }
  free (bd);
  /* A beat of triplets in semiquavers: a sextuplet, "6" (six in the time
     of four), unless the score says (TW_TRIP6). */
  for (k = e_first; k < nt->nev && !written; k++)
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
    for (z = 0; z < p->staves * GR_VPS; z++)
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
      for (z = 0; z < p->staves * GR_VPS; z++)
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
      for (z = 0; z < p->staves * GR_VPS; z++)
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
    for (z = 0; z < p->staves * GR_VPS; z++)
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
  for (z = 0; z < p->staves * GR_VPS; z++)
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
      free (nt->parts[p].mclef);
      free (nt->parts[p].mclef_at);
      free (nt->parts[p].ottava);
      free (nt->parts[p].ott_start);
      free (nt->parts[p].ott_end);
    }
  free (nt->parts);
  free (nt->bars);
  free (nt->bar_of);
  free (nt->score_bar);
  free (nt->ev);
  free (nt->hd);
  free (nt->gr);
  free (nt->ly);
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
    r.clef = NC_G8;                           /* guitars (the clef writes them an octave up) */
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
  if (tr->written == 1)
    {
      /* as the score it came from wrote it */
      r.transpose = tr->wtranspose;
      r.key = tr->wtranspose % 12 != 0;
    }
  if (tr->written && tr->wclef[0])
    r.clef = tr->wclef[0] - 1, r.tenor = 0;
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
  /* (from a score: only bars it played from the same measure) */
  if (nt->score_bar && (nt->score_bar[a] || nt->score_bar[b]) && nt->score_bar[a] != nt->score_bar[b])
    return 0;
  if (x->len != y->len || x->num != y->num || x->den != y->den || x->fifths != y->fifths || x->minor != y->minor
      || x->tempo != y->tempo)
    return 0;
  for (p = 0; p < nt->nparts; p++)
    {
      const gr_npart *pp = &nt->parts[p];
      int da = dl[p * nt->nbars + a], db = dl[p * nt->nbars + b], na = 0, nb = 0;
      if (pp->key[a] != pp->key[b])
        return 0;
      for (z = 0; z < 2; z++)
        if (pp->clefs[a * 2 + z] != pp->clefs[b * 2 + z] || pp->ottava[a * 2 + z] != pp->ottava[b * 2 + z]
            || (pp->mclef && (pp->mclef[a * 2 + z] != pp->mclef[b * 2 + z] || pp->mclef_at[a * 2 + z] != pp->mclef_at[b * 2 + z]))
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
                  || e->tup_len != f->tup_len || e->tup_at != f->tup_at || e->breve != f->breve || e->nheads != f->nheads || e->artic != f->artic
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
static int cmp_lyric (const void *a, const void *b)
{
  const gr_nlyric *x = a, *y = b;
  if (x->ev != y->ev)
    return x->ev < y->ev ? -1 : 1;
  return x->verse - y->verse;
}

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
  /* the words under a bar played again: under the bar written (the
     second verse under the first), the same event in it */
  if (nt->nly > 0)
    {
      int *at = malloc ((size_t) nt->nev * sizeof *at), moved = 0;
      if (at)
        {
          for (k = 0; k < nt->nev; k++)
            at[k] = -1;
          for (p = 0; p < nt->nparts; p++)
            for (i = 0; i < n * GR_NVOICES; i++)
              {
                const gr_nrange *r = &nt->parts[p].voice[i];
                for (j = 0; j < r->n; j++)
                  at[r->first + j] = p * n * GR_NVOICES + i;
              }
          for (k = 0; k < nt->nly; k++)
            {
              int ev = nt->ly[k].ev, w = ev >= 0 && ev < nt->nev ? at[ev] : -1;
              int pp = w / (n * GR_NVOICES), bz = w % (n * GR_NVOICES), b2 = bz / GR_NVOICES, zz = bz % GR_NVOICES;
              if (w >= 0 && keep[b2] < 0)
                {
                  const gr_nrange *r = &nt->parts[pp].voice[bz];
                  const gr_nrange *r2 = &nt->parts[pp].voice[(-1 - keep[b2]) * GR_NVOICES + zz];
                  if (ev - r->first < r2->n)
                    {
                      nt->ly[k].ev = r2->first + (ev - r->first);
                      moved = 1;
                    }
                }
            }
          free (at);
          if (moved)
            qsort (nt->ly, (size_t) nt->nly, sizeof *nt->ly, cmp_lyric);
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
                  if (pp->mclef)
                    {
                      pp->mclef[newno[i] * 2 + z] = pp->mclef[i * 2 + z];
                      pp->mclef_at[newno[i] * 2 + z] = pp->mclef_at[i * 2 + z];
                    }
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
      for (i = cnt = 0; i < GR_VPS; i++)
        cnt += v[z * GR_VPS + i].count;
      cn = malloc ((size_t) (cnt + 1) * sizeof *cn);
      cnt = 0;
      if (!cn)
        {
          free (first);
          return -1;
        }
      for (i = 0; i < GR_VPS; i++)
        for (k = 0; k < v[z * GR_VPS + i].count; k++)
          {
            const xn *n = &v[z * GR_VPS + i].n[k];
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

/* ---- a score's own marks ------------------------------------------------- */

static int bar_at (const xbar *bars, int nbars, int t)
{
  int lo = 0, hi = nbars - 1;
  while (lo < hi)
    {
      int mid = (lo + hi + 1) / 2;
      if (bars[mid].start <= t)
        lo = mid;
      else
        hi = mid - 1;
    }
  return lo;
}

/* The score's 8va and 15ma lines, in place of those Groove would choose. */
/* A part from a score: the clefs it has, where it has them (a change
   inside a bar from the next), and no 8va lines but its own. */
static void score_clefs (gr_npart *p, const gr_track *tr, const xmark *m, int n, const xbar *bars, int nbars)
{
  int z, b, k;
  for (z = 0; z < p->staves && z < 2; z++)
    if (tr->wclef[z])
      p->clef[z] = tr->wclef[z] - 1;
  for (z = 0; z < 2; z++)
    for (b = 0; b < nbars; b++)
      {
        p->clefs[b * 2 + z] = (unsigned char) p->clef[z];
        p->ottava[b * 2 + z] = 0;
        p->ott_start[b * 2 + z] = p->ott_end[b * 2 + z] = 0;
      }
  for (k = 0; k < n; k++)
    if (m[k].kind == GR_MK_CLEF && m[k].staff < 2 && m[k].value >= 0 && m[k].value <= NC_C4)
      {
        int at = bar_at (bars, nbars, m[k].on), z = m[k].staff;
        if (m[k].on > bars[at].start)
          {
            /* inside a bar: from there (a small clef before the note) */
            if (!p->mclef)
              {
                p->mclef = calloc ((size_t) nbars * 2, 1);
                p->mclef_at = calloc ((size_t) nbars * 2, sizeof *p->mclef_at);
              }
            if (p->mclef && p->mclef_at && m[k].on < bars[at].start + bars[at].len)
              {
                p->mclef[at * 2 + z] = (unsigned char) (m[k].value + 1);
                p->mclef_at[at * 2 + z] = m[k].on - bars[at].start;
              }
            at++;
          }
        else if (p->mclef)
          p->mclef[at * 2 + z] = 0;
        for (b = at; b < nbars; b++)
          p->clefs[b * 2 + z] = (unsigned char) m[k].value;
      }
}

static void score_ottavas (gr_npart *p, const xmark *m, int n, const xbar *bars, int nbars)
{
  int i, b;
  memset (p->ottava, 0, (size_t) nbars * 2);
  memset (p->ott_start, 0, (size_t) nbars * 2 * sizeof *p->ott_start);
  memset (p->ott_end, 0, (size_t) nbars * 2 * sizeof *p->ott_end);
  for (i = 0; i < n; i++)
    if (m[i].kind == GR_MK_OTTAVA && m[i].len > 0 && m[i].value)
      {
        int z = m[i].staff < p->staves ? m[i].staff : p->staves - 1, end = m[i].on + m[i].len;
        int b0 = bar_at (bars, nbars, m[i].on), b1 = bar_at (bars, nbars, end - 1);
        for (b = b0; b <= b1; b++)
          p->ottava[b * 2 + z] = (signed char) m[i].value;
        p->ott_start[b0 * 2 + z] = m[i].on - bars[b0].start;
        p->ott_end[b1 * 2 + z] = end < bars[b1].start + bars[b1].len ? end - bars[b1].start : 0;
      }
}

/* A note on a staff (any voice) starting at song tick t or, after: the
   next one after it (within two bars); before: the last one at or before
   it. -1 if none. */
static int note_near (const gr_notation *nt, const gr_npart *p, int staff, int t, int after, int *when)
{
  int b = bar_at (nt->bars, nt->nbars, t), best = -1, bt = 0, b1, z, k;
  for (b1 = b; b1 >= 0 && b1 < nt->nbars && (after ? b1 <= b + 2 : b1 >= b - 2); b1 += after ? 1 : -1)
    {
      for (z = staff * GR_VPS; z < staff * GR_VPS + GR_VPS; z++)
        {
          const gr_nrange *r = &p->voice[b1 * GR_NVOICES + z];
          for (k = 0; k < r->n; k++)
            {
              const gr_nevent *e = &nt->ev[r->first + k];
              int at = nt->bars[b1].start + e->start;
              if (e->kind != NE_NOTE || starts (nt, e) < 0)
                continue;
              if (after ? (at >= t - 2 && (best < 0 || at < bt)) : (at <= t + 2 && (best < 0 || at > bt)))
                best = r->first + k, bt = at;
            }
        }
      if (best >= 0)
        break;
    }
  if (when)
    *when = bt;
  return best;
}

/* Dynamics, hairpins, accents, slurs and spread chords as the score
   wrote them (rather than from how the notes are played). */
/* The event (a chord or rest) starting at t on a staff, in either voice;
   -1 if none. */
static int event_at (const gr_notation *nt, const gr_npart *p, int staff, int t)
{
  int b = bar_at (nt->bars, nt->nbars, t), z, k;
  for (z = staff * GR_VPS; z < staff * GR_VPS + GR_VPS; z++)
    {
      const gr_nrange *r = &p->voice[b * GR_NVOICES + z];
      for (k = 0; k < r->n; k++)
        {
          const gr_nevent *e = &nt->ev[r->first + k];
          if ((e->kind == NE_NOTE || e->kind == NE_REST || e->kind == NE_BAR_REST)
              && abs (nt->bars[b].start + e->start - t) <= 2)
            return r->first + k;
        }
    }
  return -1;
}

/* The chord starting at t on a staff, in the voice given if there's one
   there, else in any; -1 if none. */
static int chord_in_voice (const gr_notation *nt, const gr_npart *p, int staff, int voice, int t)
{
  int b = bar_at (nt->bars, nt->nbars, t), z, k, pass;
  for (pass = 0; pass < 2; pass++)
    for (z = staff * GR_VPS; z < staff * GR_VPS + GR_VPS; z++)
      {
        const gr_nrange *r = &p->voice[b * GR_NVOICES + z];
        if (pass == 0 && z != staff * GR_VPS + voice)
          continue;
        for (k = 0; k < r->n; k++)
          {
            const gr_nevent *e = &nt->ev[r->first + k];
            if (e->kind == NE_NOTE && abs (nt->bars[b].start + e->start - t) <= 2)
              return r->first + k;
          }
      }
  return -1;
}

static void score_marks (gr_notation *nt, gr_npart *p, const xmark *m, int n, int written)
{
  int i;
  for (i = 0; i < n; i++)
    {
      int z = m[i].staff < p->staves ? m[i].staff : p->staves - 1, a, e, w;
      switch (m[i].kind)
        {
        case GR_MK_DYNAMIC:
          if ((a = note_near (nt, p, z, m[i].on, 1, &w)) >= 0 && m[i].value >= ND_PPP && m[i].value <= ND_FFF)
            {
              nt->ev[a].dyn = (unsigned char) m[i].value;
              if (m[i].pitch == 1)
                nt->ev[a].wedge |= NW_DYN_ABOVE;
            }
          break;
        case GR_MK_HAIRPIN:
          a = note_near (nt, p, z, m[i].on, 1, &w);
          e = note_near (nt, p, z, m[i].on + m[i].len - 1, 0, NULL);
          if (a >= 0 && e > a && !(nt->ev[a].wedge & (NW_CRESC | NW_DIM)) && !(nt->ev[e].wedge & NW_STOP))
            {
              nt->ev[a].wedge |= ((m[i].value & 3) == 2 ? NW_DIM : NW_CRESC) | (m[i].value & GR_MK_ABOVE ? NW_ABOVE : 0);
              nt->ev[e].wedge |= NW_STOP;
            }
          break;
        case GR_MK_SLUR:
          if (p->drums)
            break;
          a = note_near (nt, p, z, m[i].on, 1, &w);
          e = note_near (nt, p, z, m[i].on + m[i].len, 0, NULL);
          if (a >= 0 && e > a && !(nt->ev[a].slur & NS_START) && !(nt->ev[e].slur & NS_STOP))
            {
              nt->ev[a].slur |= NS_START;
              nt->ev[e].slur |= NS_STOP;
            }
          break;
        case GR_MK_BEAM:
          /* (a chord moved to this staff: written in its voice on the other,
             if it's there) */
          a = -1;
          if (p->staves == 2)
            {
              a = chord_in_voice (nt, p, !z, (m[i].value >> 3) & 3, m[i].on);
              if (a >= 0 && !nt->ev[a].xstaff)
                a = -1;
            }
          if (a >= 0 || (a = chord_in_voice (nt, p, z, (m[i].value >> 3) & 3, m[i].on)) >= 0)
            nt->ev[a].beam = (unsigned char) (m[i].value & 7);
          break;
        case GR_MK_STEM:
          /* (likewise) */
          a = -1;
          if (p->staves == 2)
            {
              a = chord_in_voice (nt, p, !z, (m[i].value >> 3) & 3, m[i].on);
              if (a >= 0 && !nt->ev[a].xstaff)
                a = -1;
            }
          if (a >= 0 || (a = chord_in_voice (nt, p, z, (m[i].value >> 3) & 3, m[i].on)) >= 0)
            nt->ev[a].stem = (unsigned char) (m[i].value & 3);
          break;
        case GR_MK_ACCENT:
        case GR_MK_TENUTO:
        case GR_MK_MARCATO:
          /* (on the chord it's written on, not the next) */
          if ((a = note_near (nt, p, z, m[i].on, 1, &w)) >= 0 && w <= m[i].on + 2)
            nt->ev[a].artic |= m[i].kind == GR_MK_ACCENT ? NA_ACCENT : m[i].kind == GR_MK_TENUTO ? NA_TENUTO : NA_MARCATO;
          break;
        case GR_MK_ORNAMENT:
          if ((a = note_near (nt, p, z, m[i].on, 1, &w)) >= 0 && w <= m[i].on + 2 && m[i].value >= 1 && m[i].value <= 3)
            nt->ev[a].orn = (unsigned char) m[i].value;
          break;
        case GR_MK_TREMOLO:
          if ((a = note_near (nt, p, z, m[i].on, 1, &w)) < 0 || w > m[i].on + 2)
            break;
          if (!(m[i].value & 0x10))
            nt->ev[a].trem = (unsigned char) (m[i].value & 7);
          else if ((e = note_near (nt, p, z, m[i].on + m[i].len / 2, 1, &w)) > a && w <= m[i].on + m[i].len / 2 + 2
                   && (m[i].pitch & 15) <= NV_128TH)
            {
              /* two chords, each written as long as the two together */
              nt->ev[a].trem = (unsigned char) (0x10 | (m[i].value & 7));
              nt->ev[e].trem = 0x20;
              nt->ev[a].value = nt->ev[e].value = (unsigned char) (m[i].pitch & 15);
              nt->ev[a].dots = nt->ev[e].dots = (unsigned char) (m[i].pitch >> 4 & 3);
            }
          break;
        case GR_MK_GLISS:
          if ((a = note_near (nt, p, z, m[i].on, 1, &w)) >= 0 && w <= m[i].on + 2)
            nt->ev[a].artic |= NA_GLISS;
          break;
        case GR_MK_FERMATA:
          /* (over a rest too) */
          if ((a = event_at (nt, p, z, m[i].on)) >= 0)
            nt->ev[a].artic |= NA_FERMATA | (m[i].value < 0 ? NA_FERM_BELOW : 0);
          break;
        case GR_MK_GRACE:
          if ((a = note_near (nt, p, z, m[i].on, 1, &w)) >= 0 && w <= m[i].on + 2 && nt->ev[a].ngrace < 16)
            {
              gr_ngrace *g;
              gr_nhead h;
              int b = bar_at (nt->bars, nt->nbars, m[i].on);
              if (nt->ngr == nt->grcap)
                {
                  int cap = nt->grcap ? nt->grcap * 2 : 32;
                  gr_ngrace *x = realloc (nt->gr, (size_t) cap * sizeof *x);
                  if (!x)
                    break;
                  nt->gr = x;
                  nt->grcap = cap;
                }
              g = &nt->gr[nt->ngr++];
              int gp = m[i].pitch + p->transpose < 0 ? 0 : m[i].pitch + p->transpose > 127 ? 127 : m[i].pitch + p->transpose;
              memset (&h, 0, sizeof h);
              spell (&h, gp, p->key[b], 0);
              g->ev = a;
              g->pitch = (unsigned char) gp;
              g->value = (unsigned char) (m[i].value & 7);
              g->slash = (m[i].value & 0x40) != 0;
              g->place = (unsigned char) (m[i].len < 0 ? 0 : m[i].len > 15 ? 15 : m[i].len);
              g->step = h.step;
              g->alter = h.alter;
              g->octave = h.octave;
              nt->ev[a].ngrace++;
            }
          break;
        case GR_MK_ARPEGGIO:
          /* (a piano's chord Groove writes on both staves is spread on both;
             one line through both if the score says) */
          {
            int av = abs (m[i].value), span = (av & GR_ARP_SPAN) && p->staves == 2;
            int fl = NA_ARPEGGIO | (m[i].value < 0 ? NA_ARP_DOWN : (av & 3) == 2 ? NA_ARP_UP : 0);
            int up = -1, dn = -1;
            for (z = 0; z < p->staves; z++)
              if ((a = note_near (nt, p, z, m[i].on, 1, &w)) >= 0 && w <= m[i].on + 2
                  && (z == (m[i].staff & 1) || span || !written))
                {
                  nt->ev[a].artic |= fl;
                  if (z == (m[i].staff & 1))
                    nt->ev[a].arp_ext = (unsigned char) (m[i].staff >> 1);
                  if (z == 0) up = a; else dn = a;
                }
            /* (one the score draws on down past the staff, to the other
               hand's chord: through that) */
            if (!span && p->staves == 2 && !(m[i].staff & 1) && (m[i].staff >> 1) >= 8 && up >= 0 && dn < 0
                && (a = note_near (nt, p, 1, m[i].on, 1, &w)) >= 0 && w <= m[i].on + 2)
              {
                dn = a;
                span = 1;
                nt->ev[dn].artic |= fl;
              }
            /* (through a staff on the track below: an instrument on three
               staves or more) */
            if ((abs (m[i].value) & GR_ARP_SPAN) && !(span && up >= 0 && dn >= 0))
              {
                int at = (m[i].staff & 1) && p->staves == 2 ? dn : up;
                if (at >= 0)
                  {
                    nt->ev[at].artic |= NA_ARP_SPAN;
                    nt->ev[at].arp_rows = (unsigned char) (m[i].pitch > 0 && m[i].pitch <= 3 ? m[i].pitch : 1);
                  }
              }
            if (span && up >= 0 && dn >= 0)
              {
                nt->ev[up].arp_ext = 0;
                nt->ev[up].artic |= NA_ARP_SPAN;
                nt->ev[up].arp_rows = (unsigned char) ((abs (m[i].value) & GR_ARP_SPAN) && m[i].pitch > 1 && m[i].pitch <= 3 ? m[i].pitch : 1);
                nt->ev[dn].artic |= NA_ARP_SPANNED;
              }
          }
          break;
        }
    }
}


int gr_notation_lyric (const gr_notation *n, int ev)
{
  int lo = 0, hi = n->nly;
  while (lo < hi)
    {
      int mid = (lo + hi) / 2;
      if (n->ly[mid].ev < ev)
        lo = mid + 1;
      else
        hi = mid;
    }
  return lo < n->nly && n->ly[lo].ev == ev ? lo : -1;
}

gr_notation *gr_notation_make (const gr_song *s, int track, int min_bars)
{
  return gr_notation_make_ex (s, track, min_bars, 0);
}

/* Track t is in the view of track (all, if track < 0): it, or a track
   it's joined to (one instrument on several). */
static int in_view (const gr_song *s, int t, int track)
{
  int a, b;
  if (track < 0)
    return 1;
  for (a = track; a > 0 && s->tracks[a].joined; a--)
    ;
  for (b = track; b + 1 < s->ntracks && s->tracks[b + 1].joined; b++)
    ;
  return t >= a && t <= b;
}

gr_notation *gr_notation_make_ex (const gr_song *s, int track, int min_bars, int flags)
{
  static xlist v[GR_MAX_TRACKS][GR_NVOICES];
  static unsigned char *tp[GR_MAX_TRACKS][GR_NVOICES];
  static int tr_shift[GR_MAX_TRACKS];
  static xmark *mk[GR_MAX_TRACKS];
  static int nmk[GR_MAX_TRACKS];
  int *kf = NULL, *km = NULL, *wf = NULL;
  int nst[GR_MAX_TRACKS], t, k, z, last = 0, last_on = 0, nbars, b, ok = 0;
  gr_notation *nt = calloc (1, sizeof *nt);
  xbar *bars = NULL;
  if (!nt)
    return NULL;
  memset (v, 0, sizeof v);
  memset (tp, 0, sizeof tp);
  memset (mk, 0, sizeof mk);
  memset (nmk, 0, sizeof nmk);
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
      if ((nmk[t] = track_marks (s, t, &mk[t])) < 0)
        {
          nmk[t] = 0;
          goto fail;
        }
      if (track_notes (s, t, &all))
        {
          free (all.n);
          goto fail;
        }
      /* a score's notes Groove doesn't play (a unison in another voice):
         written all the same */
      for (k = 0; k < nmk[t]; k++)
        if (mk[t][k].kind == GR_MK_NOTE && mk[t][k].len > 0)
          {
            if (add (&all, mk[t][k].on, mk[t][k].on + mk[t][k].len, mk[t][k].pitch, 64))
              {
                free (all.n);
                goto fail;
              }
            all.n[all.count - 1].wr = GR_WR | (mk[t][k].value & GR_WR_VOICE) | (mk[t][k].staff & 1 ? GR_WR_STAFF : 0)
                                      | (mk[t][k].staff & 2 ? GR_WR_CROSS : 0) | ((mk[t][k].staff >> 6) & 3) << 5;
            all.n[all.count - 1].spell = ((mk[t][k].value >> 2) & 0x1f) | ((mk[t][k].staff >> 3) & 7) << 5;
            if (mk[t][k].value & 0x80)
              all.n[all.count - 1].flags |= XF_CUE;
            if ((all.n[all.count - 1].spell & GR_SP_HIDDEN))
              {
                all.count--;               /* (a score's note it doesn't show) */
                continue;
              }
            /* (a unison: tied where the note it shares is) */
            {
              int j;
              for (j = 0; j < all.count - 1; j++)
                if (all.n[j].pitch == mk[t][k].pitch && all.n[j].on == mk[t][k].on
                    && all.n[j].off == mk[t][k].on + mk[t][k].len && all.n[j].tie1 > 0)
                  {
                    all.n[all.count - 1].tie1 = all.n[j].tie1;
                    break;
                  }
            }
            if (mk[t][k].staff & 4)
              {
                /* tied on from the note playing then, in another voice:
                   that one is written up to here, tied to this */
                int j, on = mk[t][k].on;
                for (j = all.count - 2; j >= 0; j--)
                  if (all.n[j].pitch == mk[t][k].pitch && all.n[j].on < on && all.n[j].off >= on
                      && !(all.n[j].flags & XF_TIEOUT))
                    {
                      all.n[j].off = on;
                      all.n[j].flags |= XF_TIEOUT;
                      all.n[all.count - 1].flags |= XF_TIEIN;
                      if (all.n[j].tie1 >= on - all.n[j].on)
                        all.n[j].tie1 = 0;
                      break;
                    }
              }
          }
      if (all.count > 1)
        qsort (all.n, (size_t) all.count, sizeof *all.n, cmp_on);
      if (!all.count)
        {
          free (all.n);
          if (in_view (s, t, track))
            nst[t] = 1;
          continue;
        }
      nst[t] = 1;
      for (k = 0; k < all.count; k++)
        {
          if (all.n[k].off > last)
            last = all.n[k].off;
          if (all.n[k].on >= last_on)
            last_on = all.n[k].on + 1;
        }
      if (tr->type == TRK_DRUMS)
        {
          /* Hands (voice 1, stems up) and feet (voice 2, stems down: the
             bass drums and the pedal hi-hat); a score's in the voices it
             writes them in. */
          int wn = 0;
          for (k = 0; k < all.count; k++)
            wn += (all.n[k].wr & GR_WR) != 0;
          for (k = 0; k < all.count; k++)
            {
              int p = all.n[k].pitch, feet = p == 35 || p == 36 || p == 44;
              if (wn * 2 > all.count)
                feet = (all.n[k].wr & GR_WR) ? all.n[k].wr & GR_WR_VOICE : 0;
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
      for (k = z = 0; k < all.count; k++)
        z += (all.n[k].wr & GR_WR) != 0;
      if (z * 2 > all.count)
        {
          /* From a score: each note on the staff and in the voice it was
             written in. Any without (added since) go by pitch, in the
             first voice. Notes moved to the other staff after the rest,
             in the first voice there unless it's playing then. */
          int two = 0;
          for (k = 0; k < all.count; k++)
            two |= (all.n[k].wr & (GR_WR | GR_WR_STAFF)) == (GR_WR | GR_WR_STAFF);
          int pass;
          for (pass = 0; pass < 2; pass++)
            for (k = 0; k < all.count; k++)
              {
                const xn *x = &all.n[k];
                int st = !two ? 0 : x->wr & GR_WR ? (x->wr & GR_WR_STAFF) != 0 : x->pitch < 60;
                int vo = (x->wr & GR_WR) ? x->wr & GR_WR_VOICE : 0, cross = (x->wr & GR_WR_CROSS) != 0, j;
                if (cross != pass)
                  continue;
                if (cross && two)
                  {
                    /* (written in its own voice on the staff it came from,
                       moved: beamed with that voice, if the voice is free then) */
                    const xlist *vs = &v[t][(!st) * GR_VPS + vo];
                    int busy = 0, near = 0;
                    for (j = 0; j < vs->count && !busy; j++)
                      {
                        busy = vs->n[j].on < x->off && vs->n[j].off > x->on
                               && !(vs->n[j].on == x->on && vs->n[j].off == x->off && (vs->n[j].flags & (XF_XDOWN | XF_XUP)));
                        /* (only where the voice has notes of its own on its staff
                           in the bar: beamed across; else written in a voice there) */
                        near |= !(vs->n[j].flags & (XF_XDOWN | XF_XUP)) && gr_tick_bar (s, vs->n[j].on) == gr_tick_bar (s, x->on);
                      }
                    if (!busy && near)
                      {
                        if (add_x (&v[t][(!st) * GR_VPS + vo], x))
                          {
                            free (all.n);
                            goto fail;
                          }
                        v[t][(!st) * GR_VPS + vo].n[v[t][(!st) * GR_VPS + vo].count - 1].flags |= st ? XF_XDOWN : XF_XUP;
                        continue;
                      }
                  }
                if (cross)
                  {
                    /* (in its own voice there if that's free then, else the
                       first that is, one with its stems the same way first
                       (2 for 0), else its own; not in another's chords) */
                    static const int order[GR_VPS][GR_VPS] = { { 0, 2, 1, 3 }, { 1, 3, 0, 2 }, { 2, 0, 3, 1 }, { 3, 1, 2, 0 } };
                    int c, want = vo, busy = 1;
                    for (c = 0; c < GR_VPS && busy; c++)
                      {
                        const xlist *vc;
                        vo = order[want & 3][c];
                        vc = &v[t][st * GR_VPS + vo];
                        busy = 0;
                        for (j = 0; j < vc->count && !busy; j++)
                          busy = vc->n[j].on < x->off && vc->n[j].off > x->on
                                 && !(vc->n[j].on == x->on && vc->n[j].off == x->off
                                      && (vc->n[j].flags & XF_MOVED));
                      }
                    if (busy)
                      vo = want;
                  }
                if (add_x (&v[t][st * GR_VPS + vo], x))
                  {
                    free (all.n);
                    goto fail;
                  }
                if (cross)
                  v[t][st * GR_VPS + vo].n[v[t][st * GR_VPS + vo].count - 1].flags |= XF_MOVED;
              }
          free (all.n);
          nst[t] = two ? 2 : 1;
          continue;
        }
      if (lo < 55 && hi > 64 && instrument (tr).keyboard)
        {
          for (k = 0; k < all.count; k++)
            if (add (&v[t][all.n[k].pitch >= 60 ? 0 : GR_VPS], all.n[k].on, all.n[k].off,
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
        if (split_held (&v[t][z * GR_VPS], &v[t][z * GR_VPS + 1])
            || move_cross_rhythms (&v[t][z * GR_VPS], &v[t][z * GR_VPS + 1]))
          goto fail;
    }
  for (t = 0; t < s->ntracks; t++)
    if (nst[t] && in_view (s, t, track))
      nt->nparts++;
  if (!nt->nparts)
    goto fail;
  nbars = last > 0 ? gr_tick_bar (s, last - 1) + 1 : 1;
  if (nbars < min_bars)
    nbars = min_bars;
  /* (a score's: no further than its last bar or note, though a note
     sounds on past it, or the song's regions run on) */
  if (s->nsrcbar > 0 && s->srcbar && nbars > s->nsrcbar)
    {
      int lb = s->nsrcbar;
      while (lb > 0 && !s->srcbar[lb - 1])
        lb--;
      if (last_on > 0 && gr_tick_bar (s, last_on - 1) + 1 > lb)
        lb = gr_tick_bar (s, last_on - 1) + 1;
      if (lb > 0 && nbars > lb)
        nbars = lb;
    }
  bars = calloc ((size_t) nbars, sizeof *bars);
  if (!bars)
    goto fail;
  for (b = 0; b < nbars; b++)
    {
      const gr_meter *m = gr_meter_at (s, b);
      bars[b].start = gr_bar_tick (s, b);
      bars[b].len = gr_bar_tick (s, b + 1) - bars[b].start;
      bars[b].num = m->num;
      bars[b].timesym = m->symbol;
      bars[b].den = m->den;
      bars[b].compound = is_compound (m->num, m->den);
      bars[b].beat = bars[b].compound ? DIV * 4 / m->den * 3 : DIV * 4 / m->den;
    }
  /* A score's pickup: the first bar written short, without the time
     before it (unless notes have been put there since). */
  if (s->pickup > 0 && s->pickup < bars[0].len)
    {
      int early = 0;
      for (t = 0; t < s->ntracks && !early; t++)
        for (z = 0; z < GR_NVOICES && !early; z++)
          for (k = 0; k < v[t][z].count && !early; k++)
            early = v[t][z].n[k].on < s->pickup;
      if (!early)
        {
          bars[0].start = s->pickup;
          bars[0].len -= s->pickup;
          bars[0].pickup = 1;
        }
    }
  /* tempo changes (the song's start has its own mark) */
  for (k = 0; k < s->ntempos; k++)
    if (s->tempos[k].tick > 0 && !s->tempos[k].hidden)
      {
        b = bar_at (bars, nbars, s->tempos[k].tick);
        if (s->tempos[k].tick < bars[b].start + bars[b].len)
          bars[b].tempo = (int) (s->tempos[k].bpm + 0.5);
      }
  nt->bars = bars;
  nt->nbars = nbars;
  nt->tempo_hidden = s->tempo_hidden;
  for (t = 0; t < s->ntracks; t++)
    for (z = 0; z < GR_NVOICES; z++)
      if (v[t][z].count)
        {
          if (!(tp[t][z] = calloc ((size_t) nbars * 16, 1)))
            goto fail;
          quantise (s, &v[t][z], bars, nbars, tp[t][z], mk[t], nmk[t], z);
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
              staccato (s, &v[t][z], bars, nbars, ons, nons, mk[t], nmk[t] > 0 ? nmk[t] : -1);
            else
              {
                /* (a score's drums: as long as it writes them) */
                int wn = 0;
                for (k = 0; k < v[t][z].count; k++)
                  wn += (v[t][z].n[k].wr & GR_WR) != 0;
                if (wn * 2 <= v[t][z].count)
                  drum_lengths (s, &v[t][z], bars, nbars);
              }
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
        || !(wf = malloc ((size_t) nbars * sizeof *wf)))
      goto fail;
    if (s->nkeysigs > 0)
      {
        /* the key signatures written in the score it came from */
        for (b = 0, k = 0; b < nbars; b++)
          {
            while (k + 1 < s->nkeysigs && s->keysigs[k + 1].bar <= b)
              k++;
            kf[b] = s->keysigs[k].bar <= b ? s->keysigs[k].fifths : s->keysigs[0].fifths;
            km[b] = s->keysigs[k].bar <= b ? s->keysigs[k].minor : s->keysigs[0].minor;
          }
      }
    else if (find_keys (ls, nls, bars, nbars, s->key, kf, km))
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
            spell_list (&v[t][z], wf, km, bars, nbars, tr_shift[t]);
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
      if (!nst[t] || !in_view (s, t, track))
        continue;
      p = &nt->parts[nt->nparts++];
      p->track = t;
      /* (an instrument's staves on from the part before) */
      p->joined = nt->nparts > 1 && nt->parts[nt->nparts - 2].track == t - 1 ? s->tracks[t].joined : GR_JOIN_NONE;
      snprintf (p->name, sizeof p->name, "%s", s->tracks[t].name);
      p->drums = s->tracks[t].type == TRK_DRUMS;
      p->lines = p->drums && s->tracks[t].wlines == 1 ? 1 : 5;
      p->staves = nst[t];
      for (z = 0; z < p->staves; z++)
        p->clef[z] = clef_for (&v[t][z * GR_VPS], p->staves, z, p->drums);
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
      if (s->tracks[t].written && !p->drums)
        score_clefs (p, &s->tracks[t], mk[t], nmk[t], bars, nbars);
      if (nmk[t] > 0 && !p->drums)
        score_ottavas (p, mk[t], nmk[t], bars, nbars);
      p->voice = calloc ((size_t) nbars * GR_NVOICES, sizeof *p->voice);
      if (!p->voice)
        goto fail;
      if (!p->drums && !(s->tracks[t].written && s->tracks[t].wpedal) && track_pedal (s, t, bars, nbars, p))
        goto fail;
      /* (the pedal lines the score hides: their down and up not shown) */
      for (k = 0; k < nmk[t]; k++)
        if (mk[t][k].kind == GR_MK_NOPEDAL)
          {
            int q;
            for (q = 0; q < p->npedal; q++)
              {
                int at = bars[p->pedal[q].bar].start + p->pedal[q].start;
                int here = p->pedal[q].down ? abs (at - mk[t][k].on) <= 12 : abs (at - (mk[t][k].on + mk[t][k].len)) <= 12;
                if (here && (mk[t][k].value & (p->pedal[q].down ? 1 : 2)))
                  p->pedal[q].hidden = 1;
                if (here && (mk[t][k].value & 4))
                  p->pedal[q].line = 1;
                if (here && (mk[t][k].value & 8))
                  p->pedal[q].above = 1;
              }
          }
      cur_drumline = p->drums ? s->tracks[t].drumline : NULL;
      for (b = 0; b < nbars; b++)
        for (z = 0; z < p->staves * GR_VPS; z++)
          {
            static const unsigned char none[16];
            gr_nrange *r = &p->voice[b * GR_NVOICES + z];
            int ntr = 16;
            /* (a drum part's feet show their rests) */
            int hidden = (z % GR_VPS) != 0 && !(p->drums && v[t][z].count);
            r->first = nt->nev;
            if (build_bar (nt, &v[t][z], &bars[b], tp[t][z] ? &tp[t][z][b * 16] : none, ntr, hidden, p->drums,
                           mk[t], nmk[t], z))
              goto fail;
            r->n = nt->nev - r->first;
            /* a bar with no drums at all: one bar's rest, not one a voice */
            if (z % GR_VPS == 1 && r->n == 1 && nt->ev[r->first].kind == NE_BAR_REST && p->voice[b * GR_NVOICES + z - 1].n == 1
                && nt->ev[p->voice[b * GR_NVOICES + z - 1].first].kind == NE_BAR_REST)
              {
                nt->nev--;
                r->n = 0;
              }
          }
      /* A score's bar with notes only in another voice and no rest of
         the first's: it hid that rest */
      if (nmk[t] > 0 && !p->drums)
        {
          int rests = 0;
          for (k = 0; k < nmk[t] && !rests; k++)
            rests = mk[t][k].kind == GR_MK_REST;
          for (b = 0; b < nbars; b++)
            for (z = 0; z < p->staves; z++)
              {
                gr_nrange *r0 = &p->voice[b * GR_NVOICES + z * GR_VPS];
                int others = 0, mine = 0, i, j, end = bars[b].start + bars[b].len;
                /* (or chords moved here from the other staff's voices) */
                for (i = 0; i < GR_VPS && p->staves == 2 && !others; i++)
                  {
                    const gr_nrange *ri = &p->voice[b * GR_NVOICES + (!z) * GR_VPS + i];
                    for (j = 0; j < ri->n && !others; j++)
                      others = nt->ev[ri->first + j].kind == NE_NOTE
                               && (nt->ev[ri->first + j].xstaff == (z ? 1 : -1) || nt->ev[ri->first + j].moved);
                  }
                if (!others && !rests)
                  continue;
                if (r0->n != 1 || nt->ev[r0->first].kind != NE_BAR_REST)
                  continue;
                for (k = 0; k < nmk[t] && !mine; k++)
                  mine = rest_mark (mk[t], nmk[t], z * GR_VPS, k) && mk[t][k].on < end && mk[t][k].on + mk[t][k].len > bars[b].start;
                for (i = 1; i < GR_VPS && !mine && !others; i++)
                  {
                    const gr_nrange *ri = &p->voice[b * GR_NVOICES + z * GR_VPS + i];
                    for (j = 0; j < ri->n && !others; j++)
                      others = nt->ev[ri->first + j].kind == NE_NOTE;
                  }
                if (others && !mine)
                  nt->ev[r0->first].kind = NE_SPACE;
              }
        }
      if (nmk[t] > 0)
        score_marks (nt, p, mk[t], nmk[t], s->tracks[t].written != 0);
      else
        mark_playing (nt, p);
    }
  /* the words: each syllable under its chord */
  if (s->nlyrics > 0 && (nt->ly = malloc ((size_t) s->nlyrics * sizeof *nt->ly)))
    {
      int i;
      for (i = 0; i < s->nlyrics; i++)
        {
          const gr_lyric *l = &s->lyrics[i];
          int pi, a, w;
          for (pi = 0; pi < nt->nparts && nt->parts[pi].track != l->track; pi++)
            ;
          if (pi == nt->nparts || nt->parts[pi].drums)
            continue;
          if (l->verse >= GR_TEXT_ABOVE)
            {
              /* (words not sung: at the chord or rest there, else the next chord) */
              a = event_at (nt, &nt->parts[pi], l->staff < nt->parts[pi].staves ? l->staff : 0, l->tick);
              if (a < 0)
                a = note_near (nt, &nt->parts[pi], l->staff < nt->parts[pi].staves ? l->staff : 0, l->tick, 1, &w);
              if (a < 0)
                continue;
            }
          else
            {
              a = note_near (nt, &nt->parts[pi], l->staff < nt->parts[pi].staves ? l->staff : 0, l->tick, 1, &w);
              if (a < 0 || w > l->tick + 2)
                continue;
            }
          nt->ly[nt->nly].ev = a;
          nt->ly[nt->nly].verse = l->verse;
          nt->ly[nt->nly].syllabic = l->syllabic;
          memcpy (nt->ly[nt->nly].text, l->text, sizeof l->text);
          nt->nly++;
        }
      qsort (nt->ly, (size_t) nt->nly, sizeof *nt->ly, cmp_lyric);
    }
  nt->nsong = nbars;
  if (!(nt->bar_of = malloc ((size_t) nbars * sizeof *nt->bar_of)))
    goto fail;
  for (b = 0; b < nbars; b++)
    nt->bar_of[b] = b;
  if (flags & GR_NT_REPEATS)
    {
      if (s->nsrcbar > 0 && (nt->score_bar = calloc ((size_t) nbars, sizeof *nt->score_bar)))
        for (b = 0; b < nbars && b < s->nsrcbar; b++)
          nt->score_bar[b] = s->srcbar[b];
      if (find_repeats (nt))
        goto fail;
      free (nt->score_bar);
      nt->score_bar = NULL;
    }
  ok = 1;
fail:
  free (kf);
  free (km);
  free (wf);
  for (t = 0; t < s->ntracks; t++)
    {
      for (z = 0; z < GR_NVOICES; z++)
        {
          free (v[t][z].n);
          free (tp[t][z]);
        }
      free (mk[t]);
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

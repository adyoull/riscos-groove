/* Groove - laying out written music: one part of a song's notation as
   systems of staves, as a list of things to draw (engrave.h).

   Engraving as a music copyist would, kept simple:
   - Notes are spaced by how long they last (a half note gets more room
     than an eighth, but not twice as much), with room for accidentals
     and dots, and bars are packed into systems that fill the width.
   - Stems go up or down by where the notes sit (up for the first voice
     and down for the second when a staff has two); eighths and shorter
     are beamed a beat at a time, with second and third beams for 16ths
     and 32nds, and flags when a note stands alone.
   - Accidentals follow the key signature and last to the end of the bar;
     notes a second apart in a chord sit either side of the stem.
   - Ties, dots, ledger lines, rests (placed for their voice), triplet
     numbers, clefs, key and time signatures, a brace for two staves,
     bar lines and bar numbers.
   - Staccato dots and accents by the notehead (or by the stem, when a
     staff has two voices), slurs over the notes, and dynamics in a line
     under the (upper) staff.
   - Each system is drawn, measured, and drawn again if it needs more
     room above, below or between its staves than first thought. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engrave.h"
#include "glyphs.h"

#define SP EG_SP
#define ARP_W (SP * 3 / 4)      /* a spread chord's wavy line, and room after it */
#define HSP (SP / 2)

#define STAFF_LINE   32
#define LEDGER       40
#define STEM         30
#define BARLINE      40
#define THICK_BAR    125
#define BEAM         125
#define BEAM_GAP     190          /* from one beam to the next */
#define STEM_LEN     (SP * 7 / 2)
#define GRAND_GAP    (SP * 10)     /* top line to top line, two staves */
#define MARGIN       (SP * 2)
#define MAXROWS      64            /* staves in a system */
#define MAXPARTS     GR_MAX_TRACKS

/* ---- the list --------------------------------------------------------------- */

static int put (gr_engraving *e, int kind, int glyph, int x0, int y0, int x1, int y1, int w)
{
  gr_eitem *it;
  if (e->n == e->cap)
    {
      int cap = e->cap ? e->cap * 2 : 2048;
      gr_eitem *a = realloc (e->it, (size_t) cap * sizeof *a);
      if (!a)
        return -1;
      e->it = a;
      e->cap = cap;
    }
  it = &e->it[e->n++];
  it->kind = (unsigned char) kind;
  it->glyph = (unsigned char) glyph;
  it->x0 = x0;
  it->y0 = y0;
  it->x1 = x1;
  it->y1 = y1;
  it->w = w;
  return 0;
}

static int glyph (gr_engraving *e, int g, int x, int y, int scale)
{
  return put (e, EK_GLYPH, g, x, y, 0, 0, scale);
}

static int text_st (gr_engraving *e, int x, int y, int size, const char *s, int bold);

static int text (gr_engraving *e, int x, int y, int size, const char *s)
{
  return text_st (e, x, y, size, s, 0);
}

/* (bold: EK_TEXT's glyph 1; italic, 2) */
static int text_st (gr_engraving *e, int x, int y, int size, const char *s, int bold)
{
  size_t l = strlen (s) + 1;
  if ((size_t) e->tlen + l > (size_t) e->tcap)
    {
      int cap = e->tcap ? e->tcap * 2 : 1024;
      char *a;
      while ((size_t) cap < (size_t) e->tlen + l)
        cap *= 2;
      if (!(a = realloc (e->text, (size_t) cap)))
        return -1;
      e->text = a;
      e->tcap = cap;
    }
  memcpy (e->text + e->tlen, s, l);
  if (put (e, EK_TEXT, (unsigned char) bold, x, y, 0, size, e->tlen))
    return -1;
  e->tlen += (int) l;
  return 0;
}

static int gw (int g)
{
  return gr_glyphs[g].adv;
}

void gr_engraving_free (gr_engraving *e)
{
  if (!e)
    return;
  free (e->it);
  free (e->text);
  free (e->sys_top);
  free (e->sys_bot);
  free (e);
}

int gr_engraving_bar_at (const gr_engraving *e, int x, int y)
{
  int i;
  for (i = 0; e && i < e->n; i++)
    if (e->it[i].kind == EK_BAR && x >= e->it[i].x0 && x < e->it[i].x1 && y >= e->it[i].y0 && y < e->it[i].y1)
      return e->it[i].w;
  return -1;
}

/* ---- positions -------------------------------------------------------------- */

/* A clef as drawn in a bar: NC_*, and whether the notes are under an
   8va line (written an octave lower than they're read) or an 8vb. */
#define CL_8VA  0x10
#define CL_8VB  0x20
#define CL_15MA 0x40
#define CL_15MB 0x80
#define CL_CLEF(c) ((c) & 15)

static int ott_bits (int o)
{
  return o == 1 ? CL_8VA : o == -1 ? CL_8VB : o == 2 ? CL_15MA : o == -2 ? CL_15MB : 0;
}

/* The clef in a bar (and the line the bar's under, if any). */
static int bclef (const gr_npart *p, int bar, int z)
{
  int c = p->clefs ? p->clefs[bar * 2 + z] : p->clef[z], o = p->ottava ? p->ottava[bar * 2 + z] : 0;
  return c | ott_bits (o);
}

/* The clef at the end of bar (after any change inside it). */
static int end_clef (const gr_npart *p, int bar, int z)
{
  if (p->mclef && p->mclef[bar * 2 + z])
    return (p->mclef[bar * 2 + z] - 1) | ott_bits (p->ottava ? p->ottava[bar * 2 + z] : 0);
  return bclef (p, bar, z);
}

/* The same for a note starting at tick t in the bar (a line can start or
   end part way through one; so can a clef). */
static int eclef (const gr_npart *p, int bar, int z, int t)
{
  int c = p->clefs ? p->clefs[bar * 2 + z] : p->clef[z], o = p->ottava ? p->ottava[bar * 2 + z] : 0;
  if (p->mclef && p->mclef[bar * 2 + z] && t >= p->mclef_at[bar * 2 + z])
    c = p->mclef[bar * 2 + z] - 1;
  if (o && p->ott_start && (t < p->ott_start[bar * 2 + z] || (p->ott_end[bar * 2 + z] && t >= p->ott_end[bar * 2 + z])))
    o = 0;
  return c | ott_bits (o);
}

static int bkey (const gr_npart *p, int bar)
{
  return p->drums ? 0 : p->key ? p->key[bar] : p->fifths;
}

/* A note's place on the staff: 0 is the bottom line, 1 the space above... */
static int staff_pos (const gr_nhead *h, int clef)
{
  int dia = h->octave * 7 + h->step + (clef & CL_8VA ? -7 : clef & CL_8VB ? 7 : clef & CL_15MA ? -14 : clef & CL_15MB ? 14 : 0);
  switch (CL_CLEF (clef))
    {
    case NC_F:  return dia - (2 * 7 + 4);    /* bottom line G2 */
    case NC_C3: return dia - (3 * 7 + 3);    /* F3 */
    case NC_C4: return dia - (3 * 7 + 1);    /* D3 */
    case NC_G8: return dia - (3 * 7 + 2);    /* E3 (a guitar's, a tenor's: shown an octave up) */
    default:    return dia - (4 * 7 + 2);    /* E4 */
    }
}

static int clef_glyph (int clef)
{
  clef = CL_CLEF (clef);
  return clef == NC_F ? GL_F_CLEF : clef == NC_PERC ? GL_PERC_CLEF : clef == NC_G8 ? GL_G_CLEF_8VB
    : clef == NC_C3 || clef == NC_C4 ? GL_C_CLEF : GL_G_CLEF;
}

/* Where a clef sits (half spaces up from the bottom line): on its line. */
static int clef_y (int clef)
{
  clef = CL_CLEF (clef);
  return clef == NC_F ? 6 : clef == NC_PERC || clef == NC_C3 ? 4 : clef == NC_C4 ? 6 : 2;
}

/* The k'th sharp (or flat) of a key signature's place on the staff. */
static int key_pos (int clef, int sharps, int k)
{
  clef = CL_CLEF (clef);
  static const int sharp_g[7] = { 8, 5, 9, 6, 3, 7, 4 }, flat_g[7] = { 4, 7, 3, 6, 2, 5, 1 };
  static const int sharp_tenor[7] = { 2, 6, 3, 7, 4, 8, 5 };
  if (clef == NC_C4)
    return sharps ? sharp_tenor[k] : flat_g[k] + 1;
  return (sharps ? sharp_g[k] : flat_g[k]) - (clef == NC_F ? 2 : clef == NC_C3 ? 1 : 0);
}

static int head_glyph (const gr_nhead *h, int value)
{
  switch (h->head)
    {
    case NH_X:
      return value == NV_WHOLE ? GL_HEAD_X_WHOLE : value == NV_HALF ? GL_HEAD_X_HALF : GL_HEAD_X;
    case NH_CIRCLE_X:
      return GL_HEAD_CIRCLE_X;
    case NH_DIAMOND:
      return GL_HEAD_DIAMOND;
    case NH_TRIANGLE_DOWN:
      return GL_HEAD_TRIANGLE_DOWN;
    case NH_TRIANGLE_UP:
      return GL_HEAD_TRIANGLE_UP;
    case NH_SLASHED:
    case NH_SLASH:
      return GL_HEAD_SLASHED;
    }
  return value == 8 ? GL_HEAD_BREVE : value == NV_WHOLE ? GL_HEAD_WHOLE : value == NV_HALF ? GL_HEAD_HALF : GL_HEAD_BLACK;
}

static const int rest_glyph[] = { GL_REST_WHOLE, GL_REST_HALF, GL_REST_QUARTER, GL_REST_8TH,
                                  GL_REST_16TH, GL_REST_32ND, GL_REST_64TH, GL_REST_128TH };
static const int flag_up[] = { 0, 0, 0, GL_FLAG_8TH_UP, GL_FLAG_16TH_UP, GL_FLAG_32ND_UP, GL_FLAG_64TH_UP, GL_FLAG_128TH_UP };
static const int flag_down[] = { 0, 0, 0, GL_FLAG_8TH_DOWN, GL_FLAG_16TH_DOWN, GL_FLAG_32ND_DOWN, GL_FLAG_64TH_DOWN,
                                 GL_FLAG_128TH_DOWN };

/* The key signature: steps sharpened (or flattened), in order. */
static const int sharp_order[7] = { 3, 0, 4, 1, 5, 2, 6 };
static const int flat_order[7] = { 6, 2, 5, 1, 4, 0, 3 };

/* ---- what each bar needs ---------------------------------------------------- */

typedef struct
{
  int n;                        /* onsets */
  int *t;                       /* their ticks */
  int *seg;                     /* room after each (natural) */
  int *pre;                     /* room before each (accidentals) */
  int natural;                  /* the bar's natural width */
} barfit;

#define NO_ACC 99

static int cmp_int (const void *a, const void *b)
{
  return *(const int *) a - *(const int *) b;
}

/* Room for a gap of d ticks between notes. */
static int room_for (int d)
{
  double q = d / (double) GR_PPQ;
  int r = (int) (SP * 3.4 * pow (q, 0.45));
  return r < SP * 17 / 10 ? SP * 17 / 10 : r;
}

/* Which notes in a bar show an accidental (acc[] per head), for one
   staff: each step's sharp or flat from the key, then the bar's own. */
static void accidentals (const gr_notation *nt, const gr_npart *p, int bar, int staff, signed char *acc)
{
  int key[7] = { 0 }, state[7][10], k, z, i, idx[512], n = 0, fifths = bkey (p, bar);
  if (p->drums)
    return;
  for (k = 0; k < 7 && k < (fifths >= 0 ? fifths : -fifths); k++)
    key[fifths >= 0 ? sharp_order[k] : flat_order[k]] = fifths >= 0 ? 1 : -1;
  for (k = 0; k < 7; k++)
    for (z = 0; z < 10; z++)
      state[k][z] = key[k];
  /* both voices' events, in time order */
  for (z = 0; z < GR_VPS; z++)
    {
      const gr_nrange *r = &p->voice[bar * GR_NVOICES + staff * GR_VPS + z];
      for (k = 0; k < r->n && n < 512; k++)
        idx[n++] = r->first + k;
    }
  for (i = 1; i < n; i++)
    for (k = i; k > 0 && nt->ev[idx[k]].start < nt->ev[idx[k - 1]].start; k--)
      {
        int x = idx[k];
        idx[k] = idx[k - 1];
        idx[k - 1] = x;
      }
  for (i = 0; i < n; i++)
    {
      const gr_nevent *e = &nt->ev[idx[i]];
      if (e->kind != NE_NOTE)
        continue;
      for (k = 0; k < e->nheads; k++)
        {
          const gr_nhead *h = &nt->hd[e->head + k];
          int oct = h->octave < 0 ? 0 : h->octave > 9 ? 9 : h->octave;
          if (h->tie_stop)
            acc[e->head + k] = NO_ACC;
          else if (state[(int) h->step][oct] != h->alter)
            acc[e->head + k] = h->alter;
          else
            acc[e->head + k] = NO_ACC;
          state[(int) h->step][oct] = h->alter;
        }
    }
}

#define GRACE_SC 60                 /* grace notes' size (per cent) */
#define LYR_SIZE (SP * 6 / 5)       /* the words' size */
#define GRACE_W (gw (GL_HEAD_BLACK) * GRACE_SC / 100 + SP / 3)
#define CUE_SC 75                 /* a cue note's size, in % */

/* The room grace notes before the chords at t need: the most of them (by
   their places) and their accidentals. */
static int grace_room (const gr_notation *nt, const gr_npart *const *ps, int nps, int b, int t)
{
  int pi, z, k, i, best = 0;
  for (pi = 0; pi < nps; pi++)
    for (z = 0; z < GR_NVOICES; z++)
      {
        const gr_nrange *r = &ps[pi]->voice[b * GR_NVOICES + z];
        for (k = 0; k < r->n; k++)
          {
            const gr_nevent *e = &nt->ev[r->first + k];
            int places = 0, accs = 0, w;
            if (e->start != t || !e->ngrace)
              continue;
            for (i = 0; i < nt->ngr; i++)
              if (nt->gr[i].ev == r->first + k)
                {
                  if (nt->gr[i].place + 1 > places)
                    places = nt->gr[i].place + 1;
                  accs += nt->gr[i].alter != 0;
                }
            w = places * GRACE_W + accs * gw (GL_SHARP) * GRACE_SC / 100 + SP / 2;
            if (w > best)
              best = w;
          }
      }
  return best;
}

/* A bar's onsets and the room they need, across all the parts (ps, nps)
   shown together, so they line up. */
static int fit_bar (const gr_notation *nt, const gr_npart *const *ps, int nps, int b, const signed char *acc, barfit *f)
{
  int z, k, cap = 2, i, j, pi;
  const gr_nbar *bar = &nt->bars[b];
  const gr_npart *p;
  for (pi = 0; pi < nps; pi++)
    for (z = 0; z < GR_NVOICES; z++)
      cap += ps[pi]->voice[b * GR_NVOICES + z].n;
  f->t = malloc ((size_t) cap * sizeof *f->t);
  f->seg = malloc ((size_t) cap * sizeof *f->seg);
  f->pre = malloc ((size_t) cap * sizeof *f->pre);
  if (!f->t || !f->seg || !f->pre)
    return -1;
  f->n = 0;
  for (pi = 0; pi < nps; pi++)
    for (z = 0; z < GR_NVOICES; z++)
      {
        const gr_nrange *r = &ps[pi]->voice[b * GR_NVOICES + z];
        for (k = 0; k < r->n; k++)
          f->t[f->n++] = nt->ev[r->first + k].start;
      }
  if (!f->n)
    f->t[f->n++] = 0;
  qsort (f->t, (size_t) f->n, sizeof *f->t, cmp_int);
  for (i = j = 0; i < f->n; i++)
    if (!j || f->t[i] != f->t[j - 1])
      f->t[j++] = f->t[i];
  f->n = j;
  f->natural = SP * 6 / 5;
  int *lwa = calloc ((size_t) f->n + 1, sizeof *lwa);   /* (each onset's widest words) */
  if (!lwa)
    return -1;
  for (i = 0; i < f->n; i++)
    {
      int next = i + 1 < f->n ? f->t[i + 1] : bar->len, need = gw (GL_HEAD_BLACK) + SP / 2, accs = 0, arp = 0, grace = 0;
      int lyr = 0;
      f->seg[i] = room_for (next - f->t[i]);
      f->pre[i] = 0;
      for (pi = 0; pi < nps; pi++)
      for (p = ps[pi], z = 0; z < GR_NVOICES; z++)
        {
          const gr_nrange *r = &p->voice[b * GR_NVOICES + z];
          for (k = 0; k < r->n; k++)
            {
              const gr_nevent *e = &nt->ev[r->first + k];
              int h, a = 0;
              if (e->start != f->t[i])
                continue;
              if (e->artic & NA_ARPEGGIO)
                arp = 1;
              if (e->ngrace > grace)
                grace = e->ngrace;
              /* room for its words (half each side of the note) */
              {
                int li = nt->nly ? gr_notation_lyric (nt, r->first + k) : -1;
                for (; li >= 0 && li < nt->nly && nt->ly[li].ev == r->first + k && nt->ly[li].verse < GR_TEXT_ABOVE; li++)
                  {
                    /* (and a hyphen after it, if the word goes on) */
                    int lw = (int) strlen (nt->ly[li].text) * LYR_SIZE * 68 / 100
                      + (nt->ly[li].syllabic == 1 || nt->ly[li].syllabic == 2 ? SP : 0);
                    if (lw > lyr)
                      lyr = lw;
                  }
              }
              if (e->kind == NE_BAR_REST)
                f->seg[i] = SP * 6;
              if (e->dots)
                need += SP / 3 + e->dots * SP / 2;
              for (h = 0; e->kind == NE_NOTE && h < e->nheads; h++)
                if (acc[e->head + h] != NO_ACC)
                  a++;
              if (a > accs)
                accs = a;
              /* a second in the chord: heads both sides of the stem */
              for (h = 1; e->kind == NE_NOTE && h < e->nheads; h++)
                if (abs (staff_pos (&nt->hd[e->head + h], bclef (p, b, z / GR_VPS))
                         - staff_pos (&nt->hd[e->head + h - 1], bclef (p, b, z / GR_VPS))) == 1)
                  need += gw (GL_HEAD_BLACK);
            }
        }
      if (f->seg[i] < need)
        f->seg[i] = need;
      lwa[i] = lyr;
      if (accs)
        f->pre[i] = (accs > 2 ? 2 : accs) * (gw (GL_SHARP) + SP / 8) + SP / 6;
      if (arp)
        f->pre[i] += ARP_W;               /* room for a spread chord's wavy line */
      if (grace)
        f->pre[i] += grace_room (nt, ps, nps, b, f->t[i]);
      /* a clef changing here (before the first onset from where it changes) */
      for (pi = 0; pi < nps; pi++)
        for (z = 0; z < 2 && ps[pi]->mclef; z++)
          if (ps[pi]->mclef[b * 2 + z] && f->t[i] >= ps[pi]->mclef_at[b * 2 + z]
              && (i == 0 || f->t[i - 1] < ps[pi]->mclef_at[b * 2 + z]))
            {
              f->pre[i] += gw (GL_G_CLEF_SMALL) + SP / 3;
              pi = nps;
              break;
            }
      f->natural += f->seg[i] + f->pre[i];
    }
  /* Room for the words: each centred under its note, half of its own
     and half of the next one's between two notes, and space between. */
  for (i = 0; i < f->n; i++)
    if (lwa[i] || (i + 1 < f->n && lwa[i + 1]))
      {
        int want = lwa[i] / 2 + (i + 1 < f->n ? lwa[i + 1] / 2 : lwa[i] / 2) + SP * 2 / 3;
        if (f->seg[i] < want)
          {
            f->natural += want - f->seg[i];
            f->seg[i] = want;
          }
      }
  free (lwa);
  return 0;
}

/* ---- drawing a voice -------------------------------------------------------- */

/* A system's staves are rows: each part's one or two staves, top down. */
typedef struct
{
  gr_engraving *e;
  const gr_notation *nt;
  const signed char *acc;
  int nrows;
  const gr_npart *rp[MAXROWS];  /* each row's part */
  int rs[MAXROWS];              /* which of its staves */
  int rpart[MAXROWS];           /* its part's place in the list */
  int g0[MAXROWS], g1[MAXROWS]; /* its instrument's first and last rows (a part on several
                                   tracks, joined: bar lines down through them all) */
  int b0[MAXROWS], b1[MAXROWS]; /* ...and its brace's */
  int staff_y[MAXROWS];         /* each row's bottom line */
  int sys, sys_x0, sys_x1;      /* the system, and where its music starts and ends */
  /* ties waiting for the note they end on: per row and key */
  struct { int on, x, y, sys, x_end, above; } tie[MAXROWS][128];
  /* slurs waiting for their last note: per row and voice */
  struct { int on, x, y, sys, x_end, above, peak; } slur[MAXROWS * GR_VPS];
  /* this system's dynamics: where, which, and under which row */
  int ndyn, dyn_x[256], dyn_row[256];
  unsigned char dyn_up[256];    /* (over the staff, as the score has it) */
  unsigned char dyn_g[256];
  /* a hairpin going on in each part, and this system's pieces of them */
  int hp_on[MAXPARTS], hp_x[MAXPARTS], hp_kind[MAXPARTS], hp_upk[MAXPARTS];
  int nhp, hp_x0[128], hp_x1[128], hp_row[128];
  unsigned char hp_cresc[128], hp_up[128];
  /* this system's words: where, under which row, which verse */
  int ntx, tx_x[256], tx_row[256];  /* words that aren't sung: where, under which row */
  unsigned char tx_above[256], tx_style[256];   /* (style: 1 a tempo's words, bold; 2 with its mark after) */
  int tx_bar[256];
  const char *tx_text[256];
  /* items drawn for one row on another (a score's chords moved to the
     other staff: measured with that one; their beams with neither) */
  int nxr, xr_from[256], xr_to[256], xr_row[256];
  /* this system's metronome marks (over the top staff) */
  int nmet, met_x[64], met_bar[64], met_bpm[64];
  int nly, ly_x[1024], ly_row[1024];
  unsigned char ly_verse[1024], ly_syl[1024];
  const char *ly_text[1024];
  /* a glissando waiting for the next note: per row and voice */
  struct { int on, x, y, sys; } gl[MAXROWS * GR_VPS];
  /* this system's pedal marks */
  int nped, ped_x[256], ped_row[256];
  unsigned char ped_g[256], ped_up[256];   /* (ped_up: over the staff) */
  /* pedal lines: one going on (from x, in each part's pedal row), and the
     system's pieces (from x0 to x1, hooked at the start and/or end) */
  int pl_on[MAXROWS], pl_x[MAXROWS], pl_hook0[MAXROWS], pl_onup[MAXROWS];
  int npl, pl_x0[64], pl_x1[64], pl_row[64];
  unsigned char pl_hooks[64], pl_up[64];
  int arp_lo;                   /* a spread chord's line down through the staff below: its
                                   bottom (y), while its marks are drawn; 0 */
} drawer;

static int pos_y (const drawer *d, int staff, int pos)
{
  return d->staff_y[staff] + pos * HSP;
}

static int end_tie (drawer *d, int staff, int pitch, int x1, int y1)
{
  int dy, rc = 0;
  if (!d->tie[staff][pitch].on)
    return 0;
  d->tie[staff][pitch].on = 0;
  dy = d->tie[staff][pitch].above ? SP / 3 : -SP / 3;
  if (d->tie[staff][pitch].sys == d->sys)
    return put (d->e, EK_TIE, 0, d->tie[staff][pitch].x, d->tie[staff][pitch].y + dy, x1, y1 + dy,
                d->tie[staff][pitch].above ? 1 : -1);
  /* across a system break (its first half drawn at the end of the last
     system, carry_over): from the start of this one */
  rc |= put (d->e, EK_TIE, 0, d->sys_x0 - SP, y1 + dy, x1, y1 + dy, d->tie[staff][pitch].above ? 1 : -1);
  return rc;
}

static int end_slur (drawer *d, int sv, int x1, int y1)
{
  int x0 = d->slur[sv].x, y0 = d->slur[sv].y, len = x1 - x0, h, clear;
  int above = d->slur[sv].above;
  d->slur[sv].on = 0;
  if (len < SP)
    return 0;
  h = len * 3 / 20;
  if (h > SP * 3 / 2) h = SP * 3 / 2;
  if (h < SP / 2) h = SP / 2;
  /* over (or under) the notes between */
  clear = above ? d->slur[sv].peak - (y0 > y1 ? y0 : y1) + SP / 3 : (y0 < y1 ? y0 : y1) - d->slur[sv].peak + SP / 3;
  if (clear > h)
    h = clear > SP * 4 ? SP * 4 : clear;
  return put (d->e, EK_SLUR, 0, x0, y0, x1, y1, above ? h : -h);
}

static int ledgers (drawer *d, int staff, int pos, int x0, int x1)
{
  int lp;
  for (lp = -2; lp >= pos; lp -= 2)
    if (put (d->e, EK_LINE, 0, x0, pos_y (d, staff, lp), x1, pos_y (d, staff, lp), LEDGER))
      return -1;
  for (lp = 10; lp <= pos; lp += 2)
    if (put (d->e, EK_LINE, 0, x0, pos_y (d, staff, lp), x1, pos_y (d, staff, lp), LEDGER))
      return -1;
  return 0;
}

typedef struct
{
  int ev;                       /* index in nt->ev */
  int x;                        /* where it is */
  int up;                       /* stem up */
  int group;                    /* beam group, or -1 */
  int lo, hi;                   /* lowest and highest head's y */
  int tip;                      /* stem end's y */
  int stem_x;                   /* stem's left edge */
  int xg;                       /* beamed with chords on the other staff */
} placed;

static int beams_of (int value)
{
  return value >= NV_EIGHTH ? value - NV_EIGHTH + 1 : 0;
}

static int end_slur (drawer *d, int sv, int x1, int y1);
static void extent (const gr_engraving *e, int from, int to, int *lo, int *hi);

/* At the end of a system: the first pieces of ties and slurs that go on
   into the next, drawn now, so they're measured with this system's staff
   (lo, hi: its lowest and highest points so far). */
static int carry_over (drawer *d, int staff, int *lo, int *hi)
{
  int k, from = d->e->n, v;
  for (k = 0; k < 128; k++)
    if (d->tie[staff][k].on && d->tie[staff][k].sys == d->sys)
      {
        int dy = d->tie[staff][k].above ? SP / 3 : -SP / 3;
        if (put (d->e, EK_TIE, 0, d->tie[staff][k].x, d->tie[staff][k].y + dy, d->tie[staff][k].x_end,
                 d->tie[staff][k].y + dy, d->tie[staff][k].above ? 1 : -1))
          return -1;
        d->tie[staff][k].sys = -1;
      }
  for (v = 0; v < GR_VPS; v++)
    {
      int sv = staff * GR_VPS + v;
      if (d->slur[sv].on && d->slur[sv].sys == d->sys)
        {
          if (end_slur (d, sv, d->slur[sv].x_end, d->slur[sv].y))
            return -1;
          d->slur[sv].on = 1;
          d->slur[sv].sys = -1;
        }
    }
  extent (d->e, from, d->e->n, lo, hi);
  return 0;
}

/* A piece of part pt's hairpin, under row (its upper staff). */
static void hairpin_piece (drawer *d, int pt, int row, int x0, int x1)
{
  if (d->nhp >= 128 || x1 - x0 < SP)
    return;
  d->hp_x0[d->nhp] = x0;
  d->hp_x1[d->nhp] = x1;
  d->hp_row[d->nhp] = row;
  d->hp_up[d->nhp] = (unsigned char) d->hp_upk[pt];
  d->hp_cresc[d->nhp++] = (unsigned char) d->hp_kind[pt];
}

/* A note's staccato dot, accent, slur and dynamic. Marks go by the
   notehead, away from the stem; with two voices on the staff, the first
   voice's go above and the second's below, by the stem. */
static int marks (drawer *d, int staff, int voice, int two_voices, const gr_nevent *e, const placed *pl, int hw)
{
  int above = two_voices ? !(voice & 1) : !pl->up, stem_side = above == pl->up && e->value != NV_WHOLE;
  int top = pos_y (d, staff, 8), bot = pos_y (d, staff, 0), cx = pl->x + hw / 2, y, sv = staff * GR_VPS + voice;
  /* the note's outside edge on the marks' side */
  int edge = above ? (stem_side ? pl->tip : pl->hi) : (stem_side ? pl->tip : pl->lo);
  if ((e->artic & NA_ARPEGGIO) && !(e->artic & NA_ARP_SPANNED))
    {
      /* a spread chord: the wavy line up its left, past the accidentals
         (down through the staff below's, if it spreads that too) */
      int lo = d->arp_lo ? d->arp_lo : pl->lo - e->arp_ext * HSP;
      int h = pl->hi - lo + SP * 3 / 2, sc = h * 100 / 1358, ax;
      int a, accs = 0, g0;
      for (a = 0; a < e->nheads; a++)
        if (d->acc[e->head + a] != NO_ACC)
          accs++;
      if (sc < 30)
        sc = 30;
      ax = pl->x - (accs > 2 ? 2 : accs) * (gw (GL_SHARP) + SP / 8) - ARP_W;
      g0 = d->e->n;
      if (sc <= 60)
        {
          if (glyph (d->e, GL_ARPEGGIATO, ax, lo - SP * 3 / 4, sc))
            return -1;
        }
      else
        {
          /* (a long one: the wavy line at its usual size, again and again
             up the chord: not stretched wider) */
          int seg = gr_glyphs[GL_ARPEGGIATO].y1 * 80 / 100 - gr_glyphs[GL_ARPEGGIATO].y0 * 80 / 100;
          int y0 = lo - SP * 3 / 4, y1 = y0 + h * 1358 / 1358, yy;
          if (seg < SP)
            seg = SP;
          for (yy = y0; yy < y1 - seg / 4; yy += seg * 9 / 10)
            if (glyph (d->e, GL_ARPEGGIATO, ax + (gr_glyphs[GL_ARPEGGIATO].x1 * sc / 200 - gr_glyphs[GL_ARPEGGIATO].x1 * 80 / 200), yy, 80))
              return -1;
        }
      /* the way it's spread, if the score says: an arrowhead at that end */
      if (e->artic & (NA_ARP_DOWN | NA_ARP_UP))
        {
          int g = e->artic & NA_ARP_DOWN ? GL_ARROWHEAD_DOWN : GL_ARROWHEAD_UP, as = 60;
          int cx = ax + gr_glyphs[GL_ARPEGGIATO].x1 * sc / 200, gwh = gr_glyphs[g].x1 * as / 100;
          int top = lo - SP * 3 / 4 + gr_glyphs[GL_ARPEGGIATO].y1 * sc / 100;
          if (glyph (d->e, g, cx - gwh / 2, e->artic & NA_ARP_DOWN ? lo - SP * 3 / 4 - gr_glyphs[g].y1 * as / 100 : top, as))
            return -1;
        }
      /* (one through two staves: measured with neither) */
      if ((d->arp_lo || e->arp_ext > 4) && d->nxr < 256)
        {
          d->xr_from[d->nxr] = g0;
          d->xr_to[d->nxr] = d->e->n;
          d->xr_row[d->nxr++] = -1;
        }
    }

  if (e->artic & ~(NA_ARPEGGIO | NA_ARP_DOWN | NA_ARP_UP | NA_ARP_SPAN | NA_ARP_SPANNED))
    {
      /* the dot: a space beyond the note (outside the stem), in a space */
      if (stem_side)
        y = above ? edge + SP * 2 / 3 : edge - SP * 2 / 3;
      else
        {
          int pos = (edge - d->staff_y[staff]) / HSP + (above ? 2 : -2);
          if (!(pos & 1) && pos >= 0 && pos <= 8)
            pos += above ? 1 : -1;
          y = pos_y (d, staff, pos);
        }
      if (e->artic & NA_STACCATO)
        {
          if (glyph (d->e, above ? GL_STACCATO_ABOVE : GL_STACCATO_BELOW, cx - gw (GL_STACCATO_ABOVE) / 2,
                     above ? y - 42 : y + 42, 100))
            return -1;
          y += above ? SP * 2 / 3 : -SP * 2 / 3;
        }
      if (e->artic & NA_TENUTO)
        {
          if (glyph (d->e, above ? GL_TENUTO_ABOVE : GL_TENUTO_BELOW, cx - gw (GL_TENUTO_ABOVE) / 2, y, 100))
            return -1;
          y += above ? SP * 2 / 3 : -SP * 2 / 3;
        }
      if (e->artic & NA_MARCATO)
        {
          if (above && y < top + SP / 2)
            y = top + SP / 2;
          if (!above && y > bot - SP / 2)
            y = bot - SP / 2;
          if (glyph (d->e, above ? GL_MARCATO_ABOVE : GL_MARCATO_BELOW, cx - gw (GL_MARCATO_ABOVE) / 2,
                     above ? y : y - SP / 2, 100))
            return -1;
          y += above ? SP : -SP;
        }
      if (e->artic & NA_ACCENT)
        {
          /* outside the staff */
          if (above && y < top + SP / 2)
            y = top + SP / 2;
          if (!above && y > bot - SP / 2)
            y = bot - SP / 2;
          if (glyph (d->e, above ? GL_ACCENT_ABOVE : GL_ACCENT_BELOW, cx - gw (GL_ACCENT_ABOVE) / 2,
                     above ? y - SP / 4 : y + SP / 4, 100))
            return -1;
        }
    }
  /* ornaments, then a pause, above the staff (a pause under it if the
     score says) */
  {
    int oy = (pl->up && e->value != NV_WHOLE ? pl->tip : pl->hi) + SP * 3 / 4;
    if (oy < top + SP)
      oy = top + SP;
    if (e->artic & (NA_ACCENT | NA_MARCATO | NA_STACCATO | NA_TENUTO))
      oy += SP;
    if (e->orn && voice == 0)
      {
        int og = e->orn == NO_TRILL ? GL_TRILL : e->orn == NO_MORDENT ? GL_MORDENT : GL_TURN;
        if (glyph (d->e, og, cx - gw (og) / 2, oy, 100))
          return -1;
        oy += SP * 3 / 2;
      }
    if (e->artic & NA_FERMATA)
      {
        if (e->artic & NA_FERM_BELOW)
          {
            int fy = (!pl->up && e->value != NV_WHOLE ? pl->tip : pl->lo) - SP;
            if (fy > bot - SP)
              fy = bot - SP;
            if (glyph (d->e, GL_FERMATA_BELOW, cx - gw (GL_FERMATA_BELOW) / 2, fy, 100))
              return -1;
          }
        else if (glyph (d->e, GL_FERMATA_ABOVE, cx - gw (GL_FERMATA_ABOVE) / 2, oy, 100))
          return -1;
      }
  }
  {
    static const int g[] = { 0, GL_DYN_PPP, GL_DYN_PP, GL_DYN_P, GL_DYN_MP, GL_DYN_MF, GL_DYN_F, GL_DYN_FF, GL_DYN_FFF };
    int dx = e->dyn ? cx - gw (g[e->dyn]) / 2 : cx, pt = d->rpart[staff], top_row = staff - d->rs[staff];
    /* a hairpin ends just before this note (or its dynamic) */
    if ((e->wedge & NW_STOP) && d->hp_on[pt])
      {
        hairpin_piece (d, pt, top_row, d->hp_x[pt], (e->dyn ? dx : cx) - SP / 3);
        d->hp_on[pt] = 0;
      }
    if (e->dyn && d->ndyn < 256)
      {
        d->dyn_g[d->ndyn] = (unsigned char) g[e->dyn];
        d->dyn_row[d->ndyn] = staff;      /* (under the staff it's for, or over) */
        d->dyn_up[d->ndyn] = (e->wedge & NW_DYN_ABOVE) != 0;
        d->dyn_x[d->ndyn++] = dx;
      }
    /* and one starts here (after its dynamic) */
    if (e->wedge & (NW_CRESC | NW_DIM))
      {
        d->hp_on[pt] = 1;
        d->hp_kind[pt] = (e->wedge & NW_CRESC) != 0;
        d->hp_upk[pt] = (e->wedge & NW_ABOVE) != 0;
        d->hp_x[pt] = e->dyn ? dx + gw (g[e->dyn]) + SP / 3 : cx - hw / 2;
      }
  }
  /* slurs: ended, carried over a system break, started, or just passed */
  if (d->slur[sv].on)
    {
      int ey = d->slur[sv].above ? (pl->up == 1 && e->value != NV_WHOLE && d->slur[sv].above == pl->up ? pl->tip + SP / 2 : pl->hi + SP * 2 / 3)
        : (!pl->up && e->value != NV_WHOLE && !d->slur[sv].above ? pl->tip - SP / 2 : pl->lo - SP * 2 / 3);
      int ex = d->slur[sv].above ? edge + SP / 2 : edge - SP / 2;
      if (d->slur[sv].sys != d->sys)
        {
          /* (carried over a system break: its first piece is drawn) */
          d->slur[sv].x = d->sys_x0 - SP / 2;
          d->slur[sv].y = ey;
          d->slur[sv].sys = d->sys;
          d->slur[sv].x_end = d->sys_x1;
          d->slur[sv].peak = ey;
        }
      if (d->slur[sv].above ? ex > d->slur[sv].peak : ex < d->slur[sv].peak)
        d->slur[sv].peak = ex;
      if ((e->slur & NS_STOP) && end_slur (d, sv, cx, ey))
        return -1;
    }
  if ((e->slur & NS_START) && !d->slur[sv].on)
    {
      int a = two_voices ? !(voice & 1) : !pl->up;
      int sy = a ? (stem_side ? pl->tip + SP / 2 : pl->hi + SP * 2 / 3) : (stem_side ? pl->tip - SP / 2 : pl->lo - SP * 2 / 3);
      d->slur[sv].on = 1;
      d->slur[sv].above = a;
      d->slur[sv].x = cx;
      d->slur[sv].y = sy;
      d->slur[sv].sys = d->sys;
      d->slur[sv].x_end = d->sys_x1;
      d->slur[sv].peak = sy;
    }
  return 0;
}

static int small_clef (int clef)
{
  clef = CL_CLEF (clef);
  return clef == NC_F ? GL_F_CLEF_SMALL : clef == NC_C3 || clef == NC_C4 ? GL_C_CLEF_SMALL : GL_G_CLEF_SMALL;
}

#define ECL(ev) eclef (p, bar, pstaff + (ev)->xstaff, (ev)->start)
#define EST(ev) (staff + (ev)->xstaff)          /* (the row a chord's written on: its own, or moved) */

/* The grace notes before chord ev (its heads at x): small, stems up, a
   flag (slashed for an acciaccatura), left of its accidentals. */
static int draw_graces (drawer *d, int staff, int bar, int pstaff, int ev, int x, const gr_nevent *e)
{
  const gr_notation *nt = d->nt;
  const gr_npart *p = d->rp[staff];
  int i, places = 0, accs = 0, k, x_end, fifths = bkey (p, bar), key[7] = { 0 };
  int hw = gw (GL_HEAD_BLACK) * GRACE_SC / 100, clef = eclef (p, bar, pstaff, e->start);
  for (k = 0; k < 7 && k < (fifths >= 0 ? fifths : -fifths); k++)
    key[fifths >= 0 ? sharp_order[k] : flat_order[k]] = fifths >= 0 ? 1 : -1;
  for (k = 0; k < e->nheads; k++)
    if (d->acc[e->head + k] != NO_ACC)
      accs++;
  x_end = x - (accs > 2 ? 2 : accs) * (gw (GL_SHARP) + SP / 8) - ((e->artic & NA_ARPEGGIO) ? ARP_W : 0) - SP / 4;
  for (i = 0; i < nt->ngr; i++)
    if (nt->gr[i].ev == ev && nt->gr[i].place + 1 > places)
      places = nt->gr[i].place + 1;
  for (i = 0; i < nt->ngr; i++)
    {
      const gr_ngrace *g = &nt->gr[i];
      gr_nhead h;
      int gx, pos, y, tip, hd;
      if (g->ev != ev)
        continue;
      memset (&h, 0, sizeof h);
      h.step = g->step;
      h.alter = g->alter;
      h.octave = g->octave;
      h.pitch = g->pitch;
      gx = x_end - (places - g->place) * GRACE_W;
      pos = staff_pos (&h, clef);
      y = pos_y (d, staff, pos);
      if (ledgers (d, staff, pos, gx - SP / 6, gx + hw + SP / 6))
        return -1;
      hd = g->value <= NV_HALF ? GL_HEAD_HALF : GL_HEAD_BLACK;
      if (glyph (d->e, hd, gx, y, GRACE_SC))
        return -1;
      if (g->alter != key[(int) g->step]
          && glyph (d->e, g->alter == 1 ? GL_SHARP : g->alter == -1 ? GL_FLAT : g->alter == 2 ? GL_DOUBLE_SHARP
                    : g->alter == -2 ? GL_DOUBLE_FLAT : GL_NATURAL,
                    gx - gw (GL_SHARP) * GRACE_SC / 100 - SP / 10, y, GRACE_SC))
        return -1;
      /* stem and flag, once a place (a grace chord shares them) */
      tip = y + STEM_LEN * GRACE_SC / 100;
      if (put (d->e, EK_LINE, 0, gx + hw - STEM / 2, y + SP / 8, gx + hw - STEM / 2, tip, STEM * 3 / 4))
        return -1;
      if (g->value >= NV_EIGHTH
          && glyph (d->e, flag_up[g->value], gx + hw - STEM, tip, GRACE_SC))
        return -1;
      if (g->slash && put (d->e, EK_LINE, 0, gx + hw - STEM / 2 - SP / 2, tip - SP * 6 / 5,
                           gx + hw - STEM / 2 + SP * 2 / 3, tip - SP / 4, STEM * 3 / 4))
        return -1;
    }
  return 0;
}

/* One voice of one row (staff) for one bar. Its notes go where the
   onset table (ot, ox: no of them) puts their times. */
static int draw_voice (drawer *d, int bar, int staff, int voice, const int *ot, const int *ox, int no,
                       int bar_x0, int bar_x1, int two_voices)
{
  const gr_notation *nt = d->nt;
  const gr_npart *p = d->rp[staff];
  int pstaff = d->rs[staff];
  const gr_nrange *r = &p->voice[bar * GR_NVOICES + pstaff * GR_VPS + voice];
  const gr_nbar *nb = &nt->bars[bar];
  int hw = gw (GL_HEAD_BLACK), k, i, ngroups = 0;
  int rest_shift = two_voices ? (voice & 1 ? -4 : 4) * (voice >= 2 ? 3 : 2) / 2 : 0;
  placed *pl;
  int tup_x0 = 0, tup_ext = 0, tup_up = 1, tup_open = 0, clef_done = 0, tup_group = -2, ev_from = 0;
  if (!r->n)
    return 0;
  pl = calloc ((size_t) r->n, sizeof *pl);
  if (!pl)
    return -1;
  /* Where each event goes, and which are beamed together. */
  for (k = 0; k < r->n; k++)
    {
      const gr_nevent *e = &nt->ev[r->first + k];
      int j;
      pl[k].ev = r->first + k;
      pl[k].x = bar_x0;
      for (j = 0; j < no; j++)
        if (ot[j] == e->start)
          pl[k].x = ox[j];
      pl[k].group = -1;
      if (e->kind == NE_NOTE)
        {
          pl[k].lo = pos_y (d, EST (e), staff_pos (&nt->hd[e->head], ECL (e)));
          pl[k].hi = pos_y (d, EST (e), staff_pos (&nt->hd[e->head + e->nheads - 1], ECL (e)));
          if (pl[k].lo > pl[k].hi)
            {
              int t = pl[k].lo;
              pl[k].lo = pl[k].hi;
              pl[k].hi = t;
            }
        }
    }
  for (k = 0; k < r->n; k++)
    {
      const gr_nevent *e = &nt->ev[pl[k].ev];
      const gr_nevent *f = k ? &nt->ev[pl[k - 1].ev] : NULL;
      /* (beamed a beat at a time; in 2/8 and 3/8 the bar, in 4/8 and up
         two quavers) */
      int beat = e->tuplet ? e->tup_len
        : nb->den >= 8 && !nb->compound ? (nb->num <= 3 ? nb->len : 2 * nb->beat) : nb->beat;
      int prev = f && f->kind == NE_NOTE && f->value >= NV_EIGHTH && pl[k - 1].group >= 0 && f->beam != GR_BEAM_END;
      if (e->kind != NE_NOTE || e->value < NV_EIGHTH || e->beam == GR_BEAM_NONE)
        continue;
      /* as the score beams it, where it says; else a beat at a time */
      if (e->beam == GR_BEAM_BEGIN)
        pl[k].group = ngroups++;
      else if ((e->beam == GR_BEAM_MID || e->beam == GR_BEAM_END) && prev)
        pl[k].group = pl[k - 1].group;
      else if (prev && e->tuplet && f->tuplet && e->tup_at != 0xFFFF && f->tup_at != 0xFFFF)
        {
          /* (a score's tuplet: beamed through, a new group where the next starts) */
          if (!(e->tuplet & 2))
            pl[k].group = pl[k - 1].group;
          else
            pl[k].group = ngroups++;
        }
      else if (prev && f->start / beat == e->start / beat && (f->tuplet != 0) == (e->tuplet != 0))
        pl[k].group = pl[k - 1].group;
      else if (prev && !e->tuplet && !f->tuplet && nb->num == 4 && nb->den == 4 && !nb->compound
               && f->start / (2 * beat) == e->start / (2 * beat) && f->start / beat + 1 == e->start / beat)
        {
          /* in 4/4, quavers beamed in fours (as MuseScore does): over the
             middle of each half bar, if there's nothing shorter in it */
          int j, half = e->start / (2 * beat) * 2 * beat, ok = 1;
          for (j = 0; j < r->n && ok; j++)
            {
              const gr_nevent *g = &nt->ev[r->first + j];
              if (g->start >= half && g->start < half + 2 * beat && g->kind == NE_NOTE)
                ok = g->value <= NV_EIGHTH && !g->tuplet;
              if (g->start >= half && g->start < half + 2 * beat && g->kind == NE_REST)
                ok = 0;
            }
          pl[k].group = ok ? pl[k - 1].group : ngroups++;
        }
      else
        pl[k].group = ngroups++;
    }
  /* Stem directions: by voice when there are two, else by where the
     notes sit (a beam group together). Drums: up. */
  for (k = 0; k < r->n; k++)
    {
      const gr_nevent *e = &nt->ev[pl[k].ev];
      if (e->kind != NE_NOTE)
        continue;
      if (two_voices)
        pl[k].up = !(voice & 1);
      else if (p->drums)
        pl[k].up = 1;
      else
        {
          long sum = 0, cnt = 0;
          int j, h;
          for (j = 0; j < r->n; j++)
            if (j == k || (pl[k].group >= 0 && pl[j].group == pl[k].group))
              {
                const gr_nevent *g = &nt->ev[pl[j].ev];
                for (h = 0; h < g->nheads; h++)
                  {
                    sum += staff_pos (&nt->hd[g->head + h], ECL (g));
                    cnt++;
                  }
              }
          pl[k].up = cnt && sum < 4 * cnt;
        }
      /* (a stem the score sets: its chord's, or another's in its beam) */
      {
        int j, sd = e->stem;
        for (j = 0; j < r->n && !sd; j++)
          if (pl[k].group >= 0 && pl[j].group == pl[k].group)
            sd = nt->ev[pl[j].ev].stem;
        if (sd)
          pl[k].up = sd == 1;
      }
      if (pl[k].up)
        {
          int mid = pos_y (d, EST (e), 4);
          pl[k].stem_x = pl[k].x + hw - STEM;
          pl[k].tip = pl[k].hi + STEM_LEN + (e->value > NV_16TH ? (e->value - NV_16TH) * SP / 2 : 0);
          if (pl[k].tip < mid)
            pl[k].tip = mid;
        }
      else
        {
          int mid = pos_y (d, EST (e), 4);
          pl[k].stem_x = pl[k].x;
          pl[k].tip = pl[k].lo - STEM_LEN - (e->value > NV_16TH ? (e->value - NV_16TH) * SP / 2 : 0);
          if (pl[k].tip > mid)
            pl[k].tip = mid;
        }
    }
  /* Beams: a straight line between the ends of the first and last stems,
     not too steep, moved so no stem is too short. */
  for (i = 0; i < ngroups; i++)
    {
      int first = -1, last = -1, up = 1, n = 0, xgroup = 0, bfrom;
      double slope, shift = 0;
      for (k = 0; k < r->n; k++)
        if (pl[k].group == i)
          {
            if (first < 0)
              first = k;
            last = k;
            n++;
          }
      if (n < 2)
        {
          for (k = 0; k < r->n; k++)
            if (pl[k].group == i)
              pl[k].group = -1;
          continue;
        }
      up = pl[first].up;
      for (k = first; k <= last; k++)
        pl[k].up = up;
      for (k = first; k <= last; k++)
        if (pl[k].group == i)
          {
            pl[k].stem_x = up ? pl[k].x + hw - STEM : pl[k].x;
            pl[k].tip = up ? pl[k].hi + STEM_LEN : pl[k].lo - STEM_LEN;
          }
      slope = pl[last].stem_x == pl[first].stem_x ? 0
        : (double) (pl[last].tip - pl[first].tip) / (pl[last].stem_x - pl[first].stem_x);
      if (slope > 0.25) slope = 0.25;
      if (slope < -0.25) slope = -0.25;
      /* (level, as MuseScore has it, where a note inside the group is as
         far out towards the beam as the further end: a zig-zag, a turn) */
      {
        int ends = up ? (pl[first].hi > pl[last].hi ? pl[first].hi : pl[last].hi)
                      : (pl[first].lo < pl[last].lo ? pl[first].lo : pl[last].lo);
        for (k = first + 1; k < last; k++)
          if (pl[k].group == i && (up ? pl[k].hi >= ends : pl[k].lo <= ends))
            slope = 0;
      }
      /* (and rising or falling a space at most, over the whole group) */
      if (pl[last].stem_x > pl[first].stem_x)
        {
          double most = (double) SP / (pl[last].stem_x - pl[first].stem_x);
          if (slope > most) slope = most;
          if (slope < -most) slope = -most;
        }
      for (k = first; k <= last; k++)
        if (pl[k].group == i)
          {
            const gr_nevent *e = &nt->ev[pl[k].ev];
            double y = pl[first].tip + slope * (pl[k].stem_x - pl[first].stem_x);
            int extra = (beams_of (e->value) - 1) * BEAM_GAP;
            if (up)
              {
                double need = pl[k].hi + SP * 5 / 2 + extra;
                if (need - y > shift)
                  shift = need - y;
              }
            else
              {
                double need = pl[k].lo - SP * 5 / 2 - extra;
                if (need - y < shift)
                  shift = need - y;
              }
          }
      {
        /* (from the first stem's end as it was: it moves too) */
        int base = pl[first].tip, bx = pl[first].stem_x;
        for (k = first; k <= last; k++)
          if (pl[k].group == i)
            pl[k].tip = (int) (base + shift + slope * (pl[k].stem_x - bx));
      }
      /* A group on two staves (a score's chords moved to the other): the
         beam level between them, the upper staff's stems down to it, the
         lower's up */
      {
        int top = 999, bot = -1;
        for (k = first; k <= last; k++)
          if (pl[k].group == i)
            {
              int row = EST (&nt->ev[pl[k].ev]);
              if (row < top) top = row;
              if (row > bot) bot = row;
            }
        if (bot > top || top != staff)
          xgroup = 1;                     /* (its beams measured with neither staff) */
        if (bot > top)
          {
            int mid = (d->staff_y[top] + d->staff_y[bot] + 4 * SP) / 2, lo_head = 1 << 30, hi_head = -(1 << 30);
            int lv, maxl = 1;
            for (k = first; k <= last; k++)
              if (pl[k].group == i)
                {
                  if ((lv = beams_of (nt->ev[pl[k].ev].value)) > maxl)
                    maxl = lv;
                  if (EST (&nt->ev[pl[k].ev]) == top && pl[k].lo < lo_head)
                    lo_head = pl[k].lo;
                  if (EST (&nt->ev[pl[k].ev]) == bot && pl[k].hi > hi_head)
                    hi_head = pl[k].hi;
                }
            /* (clear of the heads either side, the beams under it) */
            mid += (maxl - 1) * BEAM_GAP / 2;
            if (lo_head < (1 << 30) && mid > lo_head - SP * 3 / 2)
              mid = lo_head - SP * 3 / 2;
            if (hi_head > -(1 << 30) && mid - (maxl - 1) * BEAM_GAP < hi_head + SP * 3 / 2)
              mid = hi_head + SP * 3 / 2 + (maxl - 1) * BEAM_GAP;
            up = 1;
            slope = 0;
            for (k = first; k <= last; k++)
              if (pl[k].group == i)
                pl[k].xg = 1;
            for (k = first; k <= last; k++)
              if (pl[k].group == i)
                {
                  int lower = EST (&nt->ev[pl[k].ev]) == bot;
                  pl[k].up = lower;
                  pl[k].stem_x = lower ? pl[k].x + hw - STEM : pl[k].x;
                  pl[k].tip = mid;
                }
          }
      }
      /* the beams themselves */
      bfrom = d->e->n;
      {
        int level, maxlev = 1;
        for (k = first; k <= last; k++)
          if (pl[k].group == i && beams_of (nt->ev[pl[k].ev].value) > maxlev)
            maxlev = beams_of (nt->ev[pl[k].ev].value);
        for (level = 1; level <= maxlev; level++)
          {
            int off = (level - 1) * BEAM_GAP * (up ? -1 : 1);
            for (k = first; k <= last; k++)
              {
                int lv = pl[k].group == i ? beams_of (nt->ev[pl[k].ev].value) : 0;
                int nx = k < last ? k + 1 : -1, pv = k > first ? k - 1 : -1;
                int lvn = nx >= 0 && pl[nx].group == i ? beams_of (nt->ev[pl[nx].ev].value) : 0;
                int lvp = pv >= 0 && pl[pv].group == i ? beams_of (nt->ev[pl[pv].ev].value) : 0;
                int xa = pl[k].stem_x, ya = pl[k].tip + off;
                if (lv < level)
                  continue;
                if (nx >= 0 && lvn >= level)
                  {
                    if (put (d->e, EK_BEAM, 0, xa, ya, pl[nx].stem_x + STEM, pl[nx].tip + off, up ? BEAM : -BEAM))
                      goto fail;
                  }
                else if (pv < 0 || lvp < level)
                  {
                    /* a note on its own at this level: a short beam,
                       towards the next note (or the one before, at the end) */
                    int len = SP, dir = nx >= 0 ? 1 : -1, xb = dir > 0 ? xa + STEM + len : xa - len + STEM;
                    if (put (d->e, EK_BEAM, 0, dir > 0 ? xa : xb, dir > 0 ? ya : (int) (ya - slope * len),
                             dir > 0 ? xb : xa + STEM, dir > 0 ? (int) (ya + slope * len) : ya, up ? BEAM : -BEAM))
                      goto fail;
                  }
              }
          }
      }
      if (xgroup && d->nxr < 256)
        {
          d->xr_from[d->nxr] = bfrom;
          d->xr_to[d->nxr] = d->e->n;
          d->xr_row[d->nxr++] = -1;
        }
    }

  /* (a tremolo between a chord moved to the other staff and one not:
     measured with neither) */
  for (k = 0; k + 1 < r->n; k++)
    if ((nt->ev[pl[k].ev].trem & 0x10) && (nt->ev[pl[k].ev].xstaff || nt->ev[pl[k + 1].ev].xstaff))
      pl[k].xg = pl[k + 1].xg = 1;

  /* Now everything else, event by event. */
  for (k = 0; k < r->n; k++)
    {
      const gr_nevent *e = &nt->ev[pl[k].ev];
      int x = pl[k].x;
      /* (the chord before, moved to the other staff: measured with that;
         one beamed across the two, with neither) */
      if (k > 0 && (nt->ev[pl[k - 1].ev].xstaff || pl[k - 1].xg) && d->nxr < 256)
        {
          d->xr_from[d->nxr] = ev_from;
          d->xr_to[d->nxr] = d->e->n;
          d->xr_row[d->nxr++] = -1;
        }
      ev_from = d->e->n;
      /* words that aren't sung, over or under the staff, at a chord or a rest */
      if (nt->nly && e->kind != NE_SPACE)
        {
          int li = gr_notation_lyric (nt, pl[k].ev);
          for (; li >= 0 && li < nt->nly && nt->ly[li].ev == pl[k].ev; li++)
            if (nt->ly[li].verse >= GR_TEXT_ABOVE && d->ntx < 256)
              {
                d->tx_x[d->ntx] = e->kind == NE_BAR_REST ? bar_x0 + SP : x;
                d->tx_row[d->ntx] = staff;
                d->tx_above[d->ntx] = nt->ly[li].verse == GR_TEXT_BELOW ? 0 : nt->ly[li].verse == GR_TEXT_PEDAL ? 2 : 1;
                d->tx_style[d->ntx] = nt->ly[li].verse == GR_TEXT_TEMPO ? 1 : nt->ly[li].verse == GR_TEXT_TEMPO_MARK ? 2 : 0;
                d->tx_bar[d->ntx] = bar;
                d->tx_text[d->ntx++] = nt->ly[li].text;
              }
        }
      if (e->tuplet & 2)
        {
          int j;
          tup_open = 1;
          tup_x0 = x;
          tup_up = e->kind == NE_NOTE ? pl[k].up : !(voice & 1);
          /* (all beamed together, or a bracket over them) */
          tup_group = e->kind == NE_NOTE ? pl[k].group : -1;
          for (j = k; j < r->n && tup_group >= 0; j++)
            {
              const gr_nevent *g = &nt->ev[pl[j].ev];
              if (g->kind != NE_SPACE && pl[j].group != tup_group)
                tup_group = -1;
              if (g->tuplet & 4)
                break;
            }
          if (tup_group < 0 && !two_voices)
            tup_up = 1;                 /* (a bracket over notes not beamed: above) */
          tup_ext = tup_up ? pos_y (d, staff, 8) : pos_y (d, staff, 0);
        }
      if (e->kind == NE_SPACE)
        ;
      else if (e->kind == NE_BAR_REST)
        {
          int rx = (bar_x0 + bar_x1) / 2 - gw (GL_REST_WHOLE) / 2;
          if (glyph (d->e, GL_REST_WHOLE, rx, pos_y (d, staff, 6 + rest_shift), 100))
            goto fail;
          if ((e->artic & NA_FERMATA)
              && glyph (d->e, GL_FERMATA_ABOVE, rx + gw (GL_REST_WHOLE) / 2 - gw (GL_FERMATA_ABOVE) / 2,
                        pos_y (d, staff, 8) + SP, 100))
            goto fail;
        }
      else if (e->kind == NE_REST)
        {
          int g = e->breve ? GL_REST_BREVE : rest_glyph[e->value], pos = e->value == NV_WHOLE && !e->breve ? 6 : 4, dot, shift = rest_shift;
          int rx = x + (hw - gw (g)) / 2;
          if (two_voices)
            {
              /* (clear of the other voices' notes then: under them in a
                 lower voice, over them in an upper one) */
              int z2, j, h, lo = 1000, hi = -1000;
              for (z2 = 0; z2 < GR_VPS; z2++)
                {
                  const gr_nrange *o = &p->voice[bar * GR_NVOICES + pstaff * GR_VPS + z2];
                  if (z2 == voice)
                    continue;
                  for (j = 0; j < o->n; j++)
                    {
                      const gr_nevent *f = &nt->ev[o->first + j];
                      if (f->kind != NE_NOTE || f->start >= e->start + e->dur || f->start + f->dur <= e->start)
                        continue;
                      for (h = 0; h < f->nheads; h++)
                        {
                          int sp = staff_pos (&nt->hd[f->head + h], ECL (f));
                          if (sp < lo) lo = sp;
                          if (sp > hi) hi = sp;
                        }
                    }
                }
              if ((voice & 1) && lo < 1000 && pos + shift > lo - 5)
                shift = lo - 5 - pos;
              if (!(voice & 1) && hi > -1000 && pos + shift < hi + 5)
                shift = hi + 5 - pos;
              shift = shift & ~1;          /* (on a line or space as a rest sits) */
            }
          if (glyph (d->e, g, rx, pos_y (d, staff, pos + shift), 100))
            goto fail;
          for (dot = 0; dot < e->dots; dot++)
            if (glyph (d->e, GL_DOT, rx + gw (g) + SP / 4 + dot * SP / 2, pos_y (d, staff, 5 + shift), 100))
              goto fail;
          if ((e->artic & NA_FERMATA)
              && glyph (d->e, GL_FERMATA_ABOVE, rx + gw (g) / 2 - gw (GL_FERMATA_ABOVE) / 2, pos_y (d, staff, 8) + SP, 100))
            goto fail;
        }
      else
        {
          int up = pl[k].up, h, displaced_right = 0, last_pos = -1000, last_disp = 0;
          int acc_col_pos[3] = { 1000, 1000, 1000 };
          int gsv = staff * GR_VPS + voice;
          /* a clef changing in the bar: small, before the first note from there */
          if (voice == 0 && p->mclef && p->mclef[bar * 2 + pstaff] && e->start >= p->mclef_at[bar * 2 + pstaff]
              && !clef_done)
            {
              int c = p->mclef[bar * 2 + pstaff] - 1, accs = 0, a2;
              for (a2 = 0; a2 < e->nheads; a2++)
                accs += d->acc[e->head + a2] != NO_ACC;
              clef_done = 1;
              if (glyph (d->e, small_clef (c), x - (accs > 2 ? 2 : accs) * (gw (GL_SHARP) + SP / 8)
                         - gw (GL_G_CLEF_SMALL) - SP / 3, d->staff_y[staff] + clef_y (c) * HSP, 100))
                goto fail;
            }
          /* a glissando from the note before ends here */
          if (d->gl[gsv].on)
            {
              int ty = pos_y (d, EST (e), staff_pos (&nt->hd[e->head], ECL (e)));
              int x0 = d->gl[gsv].sys == d->sys ? d->gl[gsv].x : d->sys_x0 - SP;
              if (put (d->e, EK_LINE, 0, x0, d->gl[gsv].y, x - SP / 3, ty, LEDGER))
                goto fail;
              d->gl[gsv].on = 0;
            }
          if (e->ngrace && draw_graces (d, staff, bar, pstaff, pl[k].ev, x, e))
            goto fail;
          /* heads, from the stem's end of the chord: a second away from
             the last head that's on its normal side goes to the other side */
          for (h = 0; h < e->nheads; h++)
            {
              int hi = up ? h : e->nheads - 1 - h;
              const gr_nhead *nh = &nt->hd[e->head + hi];
              int pos = staff_pos (nh, ECL (e)), hx = x, disp = 0, g = head_glyph (nh, e->breve ? 8 : e->value);
              if (abs (pos - last_pos) == 1 && !last_disp)
                {
                  disp = 1;
                  int dw = e->breve ? gw (g) : hw;   /* (a breve's wider head: beside the other) */
                  hx = up ? x + dw - STEM / 2 : x - dw + STEM / 2;
                  if (up)
                    displaced_right = 1;
                }
              last_disp = disp;
              last_pos = pos;
              if (ledgers (d, EST (e), pos, hx - SP / 4, hx + hw + SP / 4))
                goto fail;
              /* (a cue note's head small, against its stem) */
              if (glyph (d->e, g, hx + ((e->artic & NA_CUE) && up ? hw - hw * CUE_SC / 100 : 0), pos_y (d, EST (e), pos),
                         e->artic & NA_CUE ? CUE_SC : 100))
                goto fail;
              if (nh->tie_stop && end_tie (d, EST (e), nh->pitch, hx - SP / 8, pos_y (d, EST (e), pos)))
                goto fail;
              if (nh->tie_start)
                {
                  d->tie[EST (e)][nh->pitch].on = 1;
                  d->tie[EST (e)][nh->pitch].x = hx + hw + SP / 8;
                  d->tie[EST (e)][nh->pitch].y = pos_y (d, EST (e), pos);
                  d->tie[EST (e)][nh->pitch].sys = d->sys;
                  d->tie[EST (e)][nh->pitch].x_end = d->sys_x1 + SP / 2;
                  d->tie[EST (e)][nh->pitch].above = e->nheads > 1 ? hi == e->nheads - 1 && hi > 0 ? 1 : 0 : !up;
                }
            }
          /* accidentals, top down, in columns so they don't collide */
          for (h = e->nheads - 1; h >= 0; h--)
            {
              const gr_nhead *nh = &nt->hd[e->head + h];
              int a = d->acc[e->head + h], pos = staff_pos (nh, ECL (e)), col, g;
              if (a == NO_ACC)
                continue;
              for (col = 0; col < 2 && acc_col_pos[col] - pos < 6; col++)
                ;
              acc_col_pos[col > 2 ? 2 : col] = pos;
              g = a == 1 ? GL_SHARP : a == -1 ? GL_FLAT : a == 2 ? GL_DOUBLE_SHARP : a == -2 ? GL_DOUBLE_FLAT : GL_NATURAL;
              if (glyph (d->e, g, x - (col + 1) * (gw (GL_SHARP) + SP / 8) - SP / 10, pos_y (d, EST (e), pos),
                         e->artic & NA_CUE ? CUE_SC : 100))
                goto fail;
            }
          /* dots, in the space beside each head */
          for (h = 0; h < e->nheads && e->dots; h++)
            {
              int pos = staff_pos (&nt->hd[e->head + h], ECL (e)), dot;
              int dpos = pos & 1 ? pos : pos + ((two_voices && (voice & 1)) ? -1 : 1);
              for (dot = 0; dot < e->dots; dot++)
                if (glyph (d->e, GL_DOT, x + hw + (displaced_right ? hw : 0) + SP / 3 + dot * SP / 2,
                           pos_y (d, EST (e), dpos), 100))
                  goto fail;
            }
          /* the stem, and a flag if it isn't beamed */
          if (e->value != NV_WHOLE)
            {
              int y0 = up ? pl[k].lo + SP / 6 : pl[k].hi - SP / 6;
              if (put (d->e, EK_LINE, 0, pl[k].stem_x + STEM / 2, y0, pl[k].stem_x + STEM / 2, pl[k].tip, STEM))
                goto fail;
              if (pl[k].group < 0 && e->value >= NV_EIGHTH
                  && glyph (d->e, up ? flag_up[e->value] : flag_down[e->value], pl[k].stem_x, pl[k].tip,
                            e->artic & NA_CUE ? CUE_SC : 100))
                goto fail;
            }
          /* a tremolo: strokes through the stem, or between this chord's
             stem and the one before */
          if ((e->trem & 7) && !(e->trem & 0x10))
            {
              int s, n = e->trem & 7, sx = e->value == NV_WHOLE ? x + hw / 2 : pl[k].stem_x + STEM / 2;
              int ymid = e->value == NV_WHOLE ? (up ? pl[k].hi + SP * 3 / 2 : pl[k].lo - SP * 3 / 2)
                : (up ? pl[k].tip - SP * 5 / 4 : pl[k].tip + SP * 5 / 4);
              for (s = 0; s < n; s++)
                {
                  int y = ymid + (n - 1) * BEAM_GAP / 2 - s * BEAM_GAP;
                  if (put (d->e, EK_BEAM, 0, sx - SP * 3 / 5, y - SP / 6, sx + SP * 3 / 5, y + SP / 6, BEAM))
                    goto fail;
                }
            }
          if ((e->trem & 0x20) && k > 0 && (nt->ev[pl[k - 1].ev].trem & 0x10))
            {
              const placed *a = &pl[k - 1];
              int s, n = nt->ev[a->ev].trem & 7, x0 = a->stem_x + STEM + SP / 3, x1 = pl[k].stem_x - SP / 3;
              /* (half way along the stems, sloping a little at most) */
              int y0 = (a->tip + (a->up ? a->hi : a->lo)) / 2, y1 = (pl[k].tip + (pl[k].up ? pl[k].hi : pl[k].lo)) / 2;
              int ym = (y0 + y1) / 2;
              if (y0 - ym > SP / 2) y0 = ym + SP / 2;
              if (ym - y0 > SP / 2) y0 = ym - SP / 2;
              y1 = 2 * ym - y0;
              y0 += (n - 1) * BEAM_GAP / 2;
              y1 += (n - 1) * BEAM_GAP / 2;
              if (e->value == NV_WHOLE)
                {
                  x0 = a->x + hw + SP / 3;
                  x1 = x - SP / 3;
                  y0 = (a->lo + a->hi) / 2 + SP;
                  y1 = (pl[k].lo + pl[k].hi) / 2 + SP;
                }
              for (s = 0; s < n && x1 > x0; s++)
                if (put (d->e, EK_BEAM, 0, x0, y0 - s * BEAM_GAP, x1, y1 - s * BEAM_GAP, BEAM))
                  goto fail;
            }
          /* (a spread through the chord on the staff below: down to its
             lowest note) */
          d->arp_lo = 0;
          if (e->artic & NA_ARP_SPAN)
            {
              /* (down through the staves below: its own part's, or a track
                 of the same instrument's; to the lowest chord there) */
              int k2, rows = e->arp_rows ? e->arp_rows : 1;
              for (k2 = 1; k2 <= rows && staff + k2 < d->nrows && d->g0[staff + k2] == d->g0[staff]; k2++)
                {
                  const gr_npart *q = d->rp[staff + k2];
                  int qs = d->rs[staff + k2], z2, q2;
                  for (z2 = 0; z2 < GR_VPS; z2++)
                    {
                      const gr_nrange *r2 = &q->voice[bar * GR_NVOICES + qs * GR_VPS + z2];
                      for (q2 = 0; q2 < r2->n; q2++)
                        {
                          const gr_nevent *g = &nt->ev[r2->first + q2];
                          if (g->start == e->start && g->nheads && g->kind == NE_NOTE)
                            {
                              int y2 = pos_y (d, staff + k2, staff_pos (&nt->hd[g->head], eclef (q, bar, qs, g->start)));
                              if (!d->arp_lo || y2 < d->arp_lo)
                                d->arp_lo = y2;
                            }
                        }
                    }
                }
            }
          if (marks (d, staff, voice, two_voices, e, &pl[k], hw))
            goto fail;
          d->arp_lo = 0;
          /* its words, for the system's lines of them */
          {
            int li = gr_notation_lyric (nt, pl[k].ev);
            for (; li >= 0 && li < nt->nly && nt->ly[li].ev == pl[k].ev && d->nly < 1024; li++)
              {
                if (nt->ly[li].verse >= GR_TEXT_ABOVE)
                  continue;                   /* (drawn above, chord or rest) */
                d->ly_x[d->nly] = x + hw / 2;
                d->ly_row[d->nly] = staff;
                d->ly_verse[d->nly] = nt->ly[li].verse;
                d->ly_syl[d->nly] = nt->ly[li].syllabic;
                d->ly_text[d->nly++] = nt->ly[li].text;
              }
          }
          if (e->artic & NA_GLISS)
            {
              int gsv2 = staff * GR_VPS + voice;
              d->gl[gsv2].on = 1;
              d->gl[gsv2].x = x + hw + (e->dots ? SP / 3 + e->dots * SP / 2 : 0) + SP / 4;
              d->gl[gsv2].y = pos_y (d, EST (e), staff_pos (&nt->hd[e->head], ECL (e)));
              d->gl[gsv2].sys = d->sys;
            }
          if (tup_open)
            {
              if (tup_up && pl[k].tip > tup_ext) tup_ext = pl[k].tip;
              if (tup_up && pl[k].hi > tup_ext) tup_ext = pl[k].hi;
              if (!tup_up && pl[k].tip < tup_ext) tup_ext = pl[k].tip;
              if (!tup_up && pl[k].lo < tup_ext) tup_ext = pl[k].lo;
            }
        }
      if ((e->tuplet & 4) && tup_open)
        {
          /* (its number: two digits for 10 and up) */
          int tg = GL_TUPLET_0 + (e->tup_n < 10 ? e->tup_n : e->tup_n / 10 % 10);
          int tg2 = e->tup_n >= 10 ? GL_TUPLET_0 + e->tup_n % 10 : -1;
          int nw = gw (tg) + (tg2 >= 0 ? gw (tg2) : 0), mx = (tup_x0 + x + hw) / 2 - nw / 2;
          int ny = tup_up ? tup_ext + SP / 2 : tup_ext - SP * 3 / 2;
          int shown = !(e->tuplet & 8);
          tup_open = 0;
          if (shown && (glyph (d->e, tg, mx, ny, 100) || (tg2 >= 0 && glyph (d->e, tg2, mx + gw (tg), ny, 100))))
            goto fail;
          if (!shown)
            mx = (tup_x0 + x + hw) / 2, nw = 0;   /* (a bracket with no gap) */
          if (tup_group < 0 && !(e->tuplet & 16))
            {
              /* a bracket, its number in a gap in the middle */
              int by = ny + (gr_glyphs[tg].y0 + gr_glyphs[tg].y1) / 2, hook = tup_up ? -SP / 2 : SP / 2;
              int xa = tup_x0, xb = x + hw;
              if (put (d->e, EK_LINE, 0, xa, by + hook, xa, by, STEM)
                  || put (d->e, EK_LINE, 0, xa, by, shown ? mx - SP / 4 : mx, by, STEM)
                  || put (d->e, EK_LINE, 0, shown ? mx + nw + SP / 4 : mx, by, xb, by, STEM)
                  || put (d->e, EK_LINE, 0, xb, by, xb, by + hook, STEM))
                goto fail;
            }
        }
    }
  if (r->n > 0 && (nt->ev[pl[r->n - 1].ev].xstaff || pl[r->n - 1].xg) && d->nxr < 256)
    {
      d->xr_from[d->nxr] = ev_from;
      d->xr_to[d->nxr] = d->e->n;
      d->xr_row[d->nxr++] = -1;
    }
  free (pl);
  return 0;
fail:
  free (pl);
  return -1;
}

/* The lowest and highest point of items from..to (bar boxes aside). */
static void extent (const gr_engraving *e, int from, int to, int *lo, int *hi);

/* A row's items from..to measured with it, but those drawn for it on
   another row (d->xr_*) with that one, and their beams with neither. */
static void extent_rows (const drawer *d, int from, int to, int row, int *lo, int *hi)
{
  int i, q, at = from;
  for (i = from; i <= to; i++)
    {
      for (q = 0; q < d->nxr; q++)
        if (i < to && i >= d->xr_from[q] && i < d->xr_to[q] && i >= from)
          break;
      if (i == to || q < d->nxr)
        {
          if (at < i)
            extent (d->e, at, i, &lo[row], &hi[row]);
          if (i < to)
            {
              int end = d->xr_to[q] < to ? d->xr_to[q] : to;
              if (d->xr_row[q] >= 0)
                extent (d->e, i, end, &lo[d->xr_row[q]], &hi[d->xr_row[q]]);
              i = end - 1;
              at = end;
            }
        }
    }
}

/* An item's box: its lowest and highest points (a, b) and its left and
   right (xa, xb); 0 if it has none (a bar's box). */
static int item_box (const gr_eitem *it, const char *text, int *a_, int *b_, int *xa, int *xb)
{
  int a = it->y0 < it->y1 ? it->y0 : it->y1, b = it->y0 > it->y1 ? it->y0 : it->y1;
  *xa = it->x0 < it->x1 ? it->x0 : it->x1;
  *xb = it->x0 > it->x1 ? it->x0 : it->x1;
  switch (it->kind)
    {
    case EK_GLYPH:
      a = it->y0 + gr_glyphs[it->glyph].y0 * it->w / 100;
      b = it->y0 + gr_glyphs[it->glyph].y1 * it->w / 100;
      *xa = it->x0 + gr_glyphs[it->glyph].x0 * it->w / 100;
      *xb = it->x0 + gr_glyphs[it->glyph].x1 * it->w / 100;
      break;
    case EK_LINE:
      a -= it->w / 2;
      b += it->w / 2;
      break;
    case EK_BEAM:
      if (it->w > 0)
        a -= it->w;
      else
        b -= it->w;
      break;
    case EK_TIE:
      if (it->w > 0)
        b += SP / 2;
      else
        a -= SP / 2;
      break;
    case EK_SLUR:
      if (it->w > 0)
        b += it->w;
      else
        a += it->w;
      break;
    case EK_TEXT:
      a = it->y0 - it->y1 / 4;
      b = it->y0 + it->y1 * 3 / 4;
      *xa = it->x0;
      *xb = it->x0 + (int) strlen (text + it->w) * it->y1 * 6 / 10;
      break;
    default:
      return 0;
    }
  *a_ = a;
  *b_ = b;
  return 1;
}

/* Which row each item was measured with (for the room between staves,
   place by place): set while a system is laid out. */
static int *ext_base;               /* (that system's ext_lo: a row's is ext_lo + row) */
static signed char *item_row;
static int item_row_cap;

static void extent (const gr_engraving *e, int from, int to, int *lo, int *hi)
{
  int i;
  if (ext_base && lo >= ext_base && lo < ext_base + MAXROWS && to > 0)
    {
      if (to > item_row_cap)
        {
          int cap = to * 2 + 256;
          signed char *r = realloc (item_row, (size_t) cap);
          if (r)
            {
              memset (r + item_row_cap, -1, (size_t) (cap - item_row_cap));
              item_row = r;
              item_row_cap = cap;
            }
        }
      if (to <= item_row_cap)
        for (i = from; i < to; i++)
          if (item_row[i] < 0)
            item_row[i] = (signed char) (lo - ext_base);
    }
  for (i = from; i < to; i++)
    {
      int a, b, xa, xb;
      if (!item_box (&e->it[i], e->text, &a, &b, &xa, &xb))
        continue;
      if (a < *lo)
        *lo = a;
      if (b > *hi)
        *hi = b;
    }
}

/* The highest point of what's measured with a row (items from..to)
   between xa and xb; lo if nothing is. */
static int row_top_in (const gr_engraving *e, int from, int to, int row, int xa, int xb, int lo)
{
  int i, top = lo;
  if (!item_row || to > item_row_cap)
    return lo;
  for (i = from; i < to; i++)
    {
      int a, b, ia, ib;
      if (item_row[i] == row && item_box (&e->it[i], e->text, &a, &b, &ia, &ib) && ib >= xa && ia <= xb && b > top)
        top = b;
    }
  return top;
}

/* The room two staves need between them (upper's bottom line to lower's
   top line), from what's under one and over the other at each place
   across: items from..to measured with them; -1 if nothing is. */
static int sky_gap (const gr_engraving *e, int from, int to, int up, int y_up, int y_lo_top, int clear)
{
  enum { NB = 512 };
  int below[NB], above[NB], i, x0 = 1 << 30, x1 = -(1 << 30), w, need = -1;
  if (!item_row || to > item_row_cap)
    return -1;
  for (i = from; i < to; i++)
    {
      int a, b, xa, xb;
      if ((item_row[i] == up || item_row[i] == up + 1) && item_box (&e->it[i], e->text, &a, &b, &xa, &xb))
        {
          if (xa < x0) x0 = xa;
          if (xb > x1) x1 = xb;
        }
    }
  if (x1 <= x0)
    return -1;
  w = (x1 - x0) / NB + 1;
  if (w < SP / 2)
    w = SP / 2;
  for (i = 0; i < NB; i++)
    below[i] = above[i] = 0;
  for (i = from; i < to; i++)
    {
      int a, b, xa, xb, k, k0, k1;
      if ((item_row[i] != up && item_row[i] != up + 1) || !item_box (&e->it[i], e->text, &a, &b, &xa, &xb))
        continue;
      /* (a little room either side) */
      k0 = (xa - x0 - SP / 4) / w;
      k1 = (xb - x0 + SP / 4) / w;
      if (k0 < 0) k0 = 0;
      if (k1 >= NB) k1 = NB - 1;
      for (k = k0; k <= k1; k++)
        if (item_row[i] == up)
          {
            if (y_up - a > below[k])
              below[k] = y_up - a;          /* (how far under the upper staff) */
          }
        else if (b - y_lo_top > above[k])
          above[k] = b - y_lo_top;          /* (how far over the lower) */
    }
  for (i = 0; i < NB; i++)
    if (below[i] + above[i] + clear > need)
      need = below[i] + above[i] + clear;
  /* (and the deepest under the one, the highest over the other, apart at
     the least, clear of each other) */
  return need;
}

/* ---- systems ---------------------------------------------------------------- */

/* A bar's width with its notes' room stretched by factor. */
static int bar_width (const barfit *f, double factor)
{
  int i, w = f->natural;
  for (i = 0; i < f->n; i++)
    w += (int) (f->seg[i] * factor) - f->seg[i];
  return w;
}

/* Is bar b's time signature written at its start: a change from the
   last one written? (A bar the score makes longer or shorter than its
   time signature has one of its own, not written, as MuseScore writes
   none: gr_meter.symbol 3.) */
static int time_shown (const gr_notation *nt, int b)
{
  int p;
  if (b <= 0 || nt->bars[b].timesym == 3)
    return 0;
  for (p = b - 1; p > 0 && nt->bars[p].timesym == 3; p--)
    ;
  return nt->bars[b].num != nt->bars[p].num || nt->bars[b].den != nt->bars[p].den
    || nt->bars[b].timesym != (nt->bars[p].timesym == 3 ? 0 : nt->bars[p].timesym);
}

static int time_width (int num, int den)
{
  int a = num >= 10 ? 2 : 1, b = den >= 10 ? 2 : 1;
  return (a > b ? a : b) * gw (GL_TIME_4) + SP;
}

static int draw_time (gr_engraving *e, int x, const int *staff_y, int staves, int num, int den, int sym)
{
  int z, w = time_width (num, den) - SP;
  for (z = 0; z < staves; z++)
    {
      char a[4], b[4];
      int i, xa, xb;
      if (sym)
        {
          /* C, or C cut through: common time, alla breve */
          int g = sym == 2 ? GL_TIME_CUT : GL_TIME_COMMON;
          if (glyph (e, g, x + (w - gw (g)) / 2, staff_y[z] + 2 * SP, 100))
            return -1;
          continue;
        }
      snprintf (a, sizeof a, "%d", num);
      snprintf (b, sizeof b, "%d", den);
      xa = x + (w - (int) strlen (a) * gw (GL_TIME_4)) / 2;
      xb = x + (w - (int) strlen (b) * gw (GL_TIME_4)) / 2;
      for (i = 0; a[i]; i++)
        if (glyph (e, GL_TIME_0 + a[i] - '0', xa + i * gw (GL_TIME_4), staff_y[z] + 3 * SP, 100))
          return -1;
      for (i = 0; b[i]; i++)
        if (glyph (e, GL_TIME_0 + b[i] - '0', xb + i * gw (GL_TIME_4), staff_y[z] + SP, 100))
          return -1;
    }
  return 0;
}


/* Is bar b a whole bar's rest in every part shown (and nothing else
   there: no pedal)? */
static int empty_bar (const gr_notation *nt, const gr_npart *const *ps, int nps, int b)
{
  int pi, z, k;
  for (pi = 0; pi < nps; pi++)
    {
      for (z = 0; z < GR_NVOICES; z++)
        {
          const gr_nrange *r = &ps[pi]->voice[b * GR_NVOICES + z];
          if (r->n > 1 || (r->n == 1 && nt->ev[r->first].kind != NE_BAR_REST))
            return 0;
        }
      for (k = 0; k < ps[pi]->npedal; k++)
        if (ps[pi]->pedal[k].bar == b)
          return 0;
    }
  return 1;
}

/* Words that aren't sung in a bar (they start a new multi-bar rest, to be shown). */
static int bar_words (const gr_notation *nt, const gr_npart *const *ps, int nps, int b)
{
  int pi, z, q;
  if (!nt->nly)
    return 0;
  for (pi = 0; pi < nps; pi++)
    for (z = 0; z < GR_NVOICES; z++)
      {
        const gr_nrange *r = &ps[pi]->voice[b * GR_NVOICES + z];
        for (q = 0; q < r->n; q++)
          {
            int li = gr_notation_lyric (nt, r->first + q);
            for (; li >= 0 && li < nt->nly && nt->ly[li].ev == r->first + q; li++)
              if (nt->ly[li].verse >= GR_TEXT_ABOVE)
                return 1;
          }
      }
  return 0;
}

/* A change of key signature from `from` to `to`: the naturals cancelling
   sharps or flats it no longer has (all of them going to C, or from
   sharps to flats; the last few going to fewer), then its own. */
static void key_change (int from, int to, int *nat, int *first_nat, int *acc)
{
  int af = abs (from), at = abs (to);
  *nat = *first_nat = *acc = 0;
  if (from == to)
    return;
  *acc = at;
  if (to == 0 || (from > 0) != (to > 0))
    *nat = af;
  else if (at < af)
    *nat = af - at, *first_nat = at;
}

static int key_change_width (int from, int to)
{
  int nat, fn, acc;
  key_change (from, to, &nat, &fn, &acc);
  return nat + acc ? (nat + acc) * (gw (GL_SHARP) + SP / 6) + SP / 2 : 0;
}


/* Room at the start of bar b (not the first) for what changes there: a
   clef (for any staff), a key, and a repeat starting. */
static int clef_change_width (const gr_npart *const *ps, int nps, int b)
{
  int pi, z, w = 0;
  for (pi = 0; pi < nps; pi++)
    for (z = 0; z < ps[pi]->staves; z++)
      if (b > 0 && CL_CLEF (bclef (ps[pi], b, z)) != CL_CLEF (end_clef (ps[pi], b - 1, z))
          && gw (small_clef (bclef (ps[pi], b, z))) + SP / 2 > w)
        w = gw (small_clef (bclef (ps[pi], b, z))) + SP / 2;
  return w;
}

static int keys_change_width (const gr_npart *const *ps, int nps, int b)
{
  int pi, w = 0;
  for (pi = 0; pi < nps; pi++)
    if (b > 0 && key_change_width (bkey (ps[pi], b - 1), bkey (ps[pi], b)) > w)
      w = key_change_width (bkey (ps[pi], b - 1), bkey (ps[pi], b));
  return w;
}

/* What changes at the start of bar b (a clef, a key), drawn at x on each
   row: in the bar, or at the end of the system before (a courtesy). */
static int draw_changes (const drawer *d, int b, int x)
{
  int row, k, cw = clef_change_width (d->rp, d->nrows, b);
  for (row = 0; cw && row < d->nrows; row++)
    {
      int c = bclef (d->rp[row], b, d->rs[row]);
      if (CL_CLEF (c) != CL_CLEF (end_clef (d->rp[row], b - 1, d->rs[row]))
          && glyph (d->e, small_clef (c), x + SP / 4, d->staff_y[row] + clef_y (c) * HSP, 100))
        return -1;
    }
  x += cw;
  for (row = 0; row < d->nrows; row++)
    {
      const gr_npart *pp = d->rp[row];
      int from = bkey (pp, b - 1), to = bkey (pp, b), nat, fn, acc, c = bclef (pp, b, d->rs[row]);
      int kx = x + SP / 4, step = gw (GL_SHARP) + SP / 6;
      key_change (from, to, &nat, &fn, &acc);
      for (k = fn; k < fn + nat && k < 7; k++, kx += step)
        if (glyph (d->e, GL_NATURAL, kx, d->staff_y[row] + key_pos (c, from > 0, k) * HSP, 100))
          return -1;
      for (k = 0; k < acc && k < 7; k++, kx += step)
        if (glyph (d->e, to > 0 ? GL_SHARP : GL_FLAT, kx, d->staff_y[row] + key_pos (c, to > 0, k) * HSP, 100))
          return -1;
    }
  return 0;
}

#define START_REPEAT_W (THICK_BAR + SP / 2 + BARLINE + SP / 3 + gw (GL_DOT) + SP / 2)

/* Words over the staff at x, baseline y: bold if asked (a tempo's); a
   metronome mark's notes in them (GR_CH_*) are drawn as notes by what
   draws the text (it knows how wide its letters are). */
static int tempo_text (gr_engraving *e, int x, int y, const char *s, int bold)
{
  return text_st (e, x, y, LYR_SIZE, s, bold);
}

/* A part's name's width, roughly (for the left margin). */
static int name_width (const char *s, int size)
{
  return (int) strlen (s) * size * 62 / 100;
}

/* A dashed line, level, from x0 to x1. */
static int dashed (gr_engraving *e, int x0, int x1, int y)
{
  int x;
  for (x = x0; x < x1; x += SP + SP / 2)
    if (put (e, EK_LINE, 0, x, y, x + SP < x1 ? x + SP : x1, y, STAFF_LINE))
      return -1;
  return 0;
}

/* Over (and under) a system of bars first..last-1 (bxs: where each
   bar's notes start; bxb: its end): the 8va and 8vb lines on each staff,
   the endings over the top one, and "Play 3 times" over a repeat's end. */
/* Where in bar b (laid out: f its onsets, ox where they went, or NULL)
   tick t is: the onset there or the next one, or dflt. */
static int tick_x (const barfit *f, const int *ox, int t, int dflt)
{
  int j;
  for (j = 0; ox && j < f->n; j++)
    if (f->t[j] >= t)
      return ox[j];
  return dflt;
}

static int lines_over (drawer *d, int first, int last, const int *bxs, const int *bxb, const barfit *fits,
                       int *const *oxs, int *lo, int *hi)
{
  gr_engraving *e = d->e;
  const gr_notation *nt = d->nt;
  int row, b, from;
  for (row = 0; row < d->nrows; row++)
    for (b = first; b < last;)
      {
        int o = d->rp[row]->ottava ? d->rp[row]->ottava[b * 2 + d->rs[row]] : 0, b1 = b, g, x0, x1, y, gwid;
        if (!o)
          {
            b++;
            continue;
          }
        while (b1 + 1 < last && d->rp[row]->ottava[(b1 + 1) * 2 + d->rs[row]] == o)
          b1++;
        g = o == 1 ? GL_OTTAVA_8VA : o == -1 ? GL_OTTAVA_8VB : o > 0 ? GL_QUINDICESIMA_A : GL_QUINDICESIMA_B;
        gwid = gw (g) * 8 / 10;
        {
          int z = d->rs[row], st = d->rp[row]->ott_start ? d->rp[row]->ott_start[b * 2 + z] : 0;
          int en = d->rp[row]->ott_end ? d->rp[row]->ott_end[b1 * 2 + z] : 0;
          /* (from its first note, if it starts part way through a bar; to
             just before the note after its last) */
          x0 = st ? tick_x (&fits[b], oxs[b], st, bxs[b]) - SP / 3 : bxs[b];
          x1 = en ? tick_x (&fits[b1], oxs[b1], en, bxb[b1]) - SP * 2 / 3 : bxb[b1] - SP / 2;
          if (x1 < x0 + gwid + SP)
            x1 = x0 + gwid + SP;
        }
        from = e->n;
        if (o > 0)
          {
            y = d->staff_y[row] + 6 * SP;
            if (hi[row] + SP / 2 > y)
              y = hi[row] + SP / 2;
            if (glyph (e, g, x0, y, 80) || dashed (e, x0 + gwid + SP / 3, x1, y + SP * 3 / 4)
                || put (e, EK_LINE, 0, x1, y + SP * 3 / 4, x1, y - SP / 4, STAFF_LINE))
              return -1;
          }
        else
          {
            y = d->staff_y[row] - 2 * SP - gr_glyphs[g].y1 * 8 / 10;
            if (lo[row] - SP / 2 - gr_glyphs[g].y1 * 8 / 10 < y)
              y = lo[row] - SP / 2 - gr_glyphs[g].y1 * 8 / 10;
            if (glyph (e, g, x0, y, 80) || dashed (e, x0 + gwid + SP / 3, x1, y + SP / 4)
                || put (e, EK_LINE, 0, x1, y + SP / 4, x1, y + SP * 5 / 4, STAFF_LINE))
              return -1;
          }
        extent (e, from, e->n, &lo[row], &hi[row]);
        b = b1 + 1;
      }
  /* endings, over the top staff */
  for (b = first; b < last;)
    {
      const gr_nbar *nb = &nt->bars[b];
      int b1 = b, y, x0, x1, top = d->staff_y[0] + 4 * SP;
      char num[8];
      if (!nb->ending)
        {
          b++;
          continue;
        }
      while (b1 + 1 < last && nt->bars[b1 + 1].ending == nb->ending && !nt->bars[b1 + 1].ending_start)
        b1++;
      x0 = bxs[b] - SP;
      x1 = bxb[b1] - SP / 2;
      y = top + SP * 3;
      if (hi[0] + SP > y)
        y = hi[0] + SP;
      from = e->n;
      if (put (e, EK_LINE, 0, x0, y, x1, y, STAFF_LINE))
        return -1;
      if (nb->ending_start)
        {
          snprintf (num, sizeof num, "%d.", nb->ending);
          if (put (e, EK_LINE, 0, x0, y, x0, y - SP * 3 / 2, STAFF_LINE) || text (e, x0 + SP / 2, y - SP * 5 / 4, SP * 6 / 5, num))
            return -1;
        }
      if (nt->bars[b1].ending_stop == 1 && put (e, EK_LINE, 0, x1, y, x1, y - SP * 3 / 2, STAFF_LINE))
        return -1;
      extent (e, from, e->n, &lo[0], &hi[0]);
      b = b1 + 1;
    }
  /* how many times */
  for (b = first; b < last; b++)
    if ((nt->bars[b].rep & NR_END) && nt->bars[b].times > 2)
      {
        char t[24];
        int y = d->staff_y[0] + 5 * SP, size = SP * 6 / 5;
        snprintf (t, sizeof t, "Play %d times", nt->bars[b].times);
        if (hi[0] + SP / 3 > y)
          y = hi[0] + SP / 3;
        from = e->n;
        if (text (e, bxb[b] - name_width (t, size), y, size, t))
          return -1;
        extent (e, from, e->n, &lo[0], &hi[0]);
      }
  return 0;
}

gr_engraving *gr_engrave (const gr_notation *nt, int part, int width, const char *title, int tempo)
{
  return gr_engrave_parts (nt, &part, 1, width, title, tempo, EG_MULTIRESTS);
}

gr_engraving *gr_engrave_parts (const gr_notation *nt, const int *parts, int nparts, int width, const char *title,
                                int tempo, int flags)
{
  gr_engraving *e = calloc (1, sizeof *e);
  const gr_npart *ps[MAXPARTS];
  signed char *acc = NULL;
  barfit *fits = NULL;
  int *chg = NULL, *run = NULL, *bxa = NULL, *bxb = NULL, *bxs = NULL, **oxs = NULL;
  drawer *d = NULL;
  drawer *saved = NULL;
  int b, z, k, nkey = 0, left0, left, right, avail, y, first_bar, sysno = 0, pass, n0 = 0, tl0 = 0, pi, row;
  int names = 0, clefw = 0, nrows = 0;
  if (!e || nparts < 1 || nparts > MAXPARTS)
    {
      free (e);
      return NULL;
    }
  for (pi = 0; pi < nparts; pi++)
    {
      if (parts[pi] < 0 || parts[pi] >= nt->nparts)
        {
          free (e);
          return NULL;
        }
      ps[pi] = &nt->parts[parts[pi]];
      nrows += ps[pi]->staves;
    }
  if (nrows > MAXROWS)
    {
      free (e);
      return NULL;
    }
  e->width = width;
  acc = malloc ((size_t) nt->nhd + 1);
  fits = calloc ((size_t) nt->nbars, sizeof *fits);
  run = calloc ((size_t) nt->nbars, sizeof *run);
  bxa = calloc ((size_t) nt->nbars, sizeof *bxa);
  bxb = calloc ((size_t) nt->nbars, sizeof *bxb);
  bxs = calloc ((size_t) nt->nbars, sizeof *bxs);
  oxs = calloc ((size_t) nt->nbars, sizeof *oxs);
  chg = calloc ((size_t) nt->nbars + 1, sizeof *chg);
  d = calloc (1, sizeof *d);
  saved = malloc (sizeof *saved);
  if (!acc || !fits || !run || !d || !saved || !bxa || !bxb || !bxs || !oxs || !chg)
    goto fail;
  memset (acc, NO_ACC, (size_t) nt->nhd + 1);
  for (pi = 0; pi < nparts; pi++)
    for (b = 0; b < nt->nbars; b++)
      for (z = 0; z < ps[pi]->staves; z++)
        accidentals (nt, ps[pi], b, z, acc);
  /* bars of rest one after another, in the same time: one multi-bar
     rest (run[b]: how many bars from b; 0 inside one) */
  for (b = 0; b < nt->nbars; b++)
    run[b] = 1;
  if (flags & EG_MULTIRESTS)
    for (b = 0; b < nt->nbars;)
      {
        int n = 0;
        while (b + n < nt->nbars && empty_bar (nt, ps, nparts, b + n)
               && (n == 0 || (nt->bars[b + n].num == nt->bars[b].num && nt->bars[b + n].den == nt->bars[b].den
                              && !(nt->bars[b + n].rep & NR_START) && !(nt->bars[b + n - 1].rep & NR_END)
                              && nt->bars[b + n].ending == nt->bars[b].ending && !nt->bars[b + n].ending_start
                              && !clef_change_width (ps, nparts, b + n) && !keys_change_width (ps, nparts, b + n)
                              && !nt->bars[b + n].tempo        /* (a new tempo starts a new one, to be shown) */
                              && !bar_words (nt, ps, nparts, b + n))))   /* (as do words) */
          n++;
        if (n >= 2)
          {
            run[b] = n;
            for (k = 1; k < n; k++)
              run[b + k] = 0;
            b += n;
          }
        else
          b++;
      }
  for (b = 0; b < nt->nbars; b++)
    {
      if (fit_bar (nt, ps, nparts, b, acc, &fits[b]))
        goto fail;
      if (run[b] == 0)
        {
          /* (inside a multi-bar rest: no room of its own) */
          fits[b].n = 0;
          fits[b].natural = 0;
          continue;
        }
      if (run[b] > 1)
        {
          fits[b].n = 1;
          fits[b].t[0] = 0;
          fits[b].pre[0] = 0;
          fits[b].seg[0] = SP * 8;
          fits[b].natural = SP * 6 / 5 + SP * 8;
        }
      if (time_shown (nt, b))
        fits[b].natural += time_width (nt->bars[b].num, nt->bars[b].den);
      chg[b] = clef_change_width (ps, nparts, b) + keys_change_width (ps, nparts, b);
      fits[b].natural += chg[b];
      if (b > 0 && (nt->bars[b].rep & NR_START))
        fits[b].natural += START_REPEAT_W;
      if (nt->bars[b].rep & NR_END)
        fits[b].natural += SP * 3 / 2;
    }
  d->e = e;
  d->nt = nt;
  d->acc = acc;
  d->nrows = nrows;
  for (pi = 0, row = 0; pi < nparts; pi++)
    {
      for (b = 0; b < nt->nbars; b++)
        {
          int n = abs (bkey (ps[pi], b));
          if (n > 7)
            n = 7;
          if (n > nkey)
            nkey = n;
        }
      for (z = 0; z < ps[pi]->staves; z++, row++)
        {
          d->rp[row] = ps[pi];
          d->rs[row] = z;
          d->rpart[row] = pi;
          for (b = 0; b < nt->nbars; b++)
            if (gw (clef_glyph (bclef (ps[pi], b, z))) > clefw)
              clefw = gw (clef_glyph (bclef (ps[pi], b, z)));
        }
    }
  /* each row's instrument (its part, and parts joined on to it) and brace */
  for (row = 0; row < nrows; row++)
    {
      int pj = d->rpart[row];
      d->g0[row] = d->b0[row] = row - d->rs[row];
      d->g1[row] = d->b1[row] = row - d->rs[row] + d->rp[row]->staves - 1;
      if (row > 0 && d->rs[row] == 0 && ps[pj]->joined && pj > 0 && ps[pj - 1] == ps[pj] - 1)
        {
          d->g0[row] = d->g0[row - 1];
          if (ps[pj]->joined == GR_JOIN_BRACE)
            d->b0[row] = d->b0[row - 1];
        }
      else if (d->rs[row] > 0)
        {
          d->g0[row] = d->g0[row - 1];
          d->b0[row] = d->b0[row - 1];
        }
    }
  for (row = nrows - 1; row >= 0; row--)
    if (row + 1 < nrows && d->g0[row + 1] == d->g0[row])
      {
        d->g1[row] = d->g1[row + 1];
        if (d->b0[row + 1] == d->b0[row])
          d->b1[row] = d->b1[row + 1];
      }
  /* the parts' names, down the left of the first system (of a score of
     more than one) */
  if (nparts > 1)
    for (pi = 0; pi < nparts; pi++)
      {
        const char *nm = ps[pi]->name;
        if (name_width (nm, SP * 3 / 2) > names)
          names = name_width (nm, SP * 3 / 2);
      }
  if (names > width / 4)
    names = width / 4;
  left = MARGIN + SP * 3 / 2;              /* (room for a brace) */
  left0 = left + (names ? names + SP * 3 : 0);
  right = MARGIN;
  y = -SP * 2;
  if (title && *title)
    {
      if (text (e, left, y - SP * 2, SP * 2, title))
        goto fail;
      y -= SP * 4;
    }

  for (first_bar = 0; first_bar < nt->nbars; sysno++)
    {
      int head_w, last_bar, used = 0, x, sys_y0 = y, lft = sysno ? left : left0;
      int gap[MAXROWS], top_ext = SP * 3, bot_ext = SP * 3, last = nrows - 1;
      double factor;
      long segs = 0, fixed = 0;
      int cour = 0;
      const gr_nbar *fb = &nt->bars[first_bar];
      int show_time = first_bar == 0;
      avail = width - lft - right;
      head_w = SP * 2 / 3 + clefw + SP * 2 / 3 + nkey * (gw (GL_SHARP) + SP / 6)
        + (show_time ? time_width (fb->num, fb->den) : 0) + SP / 3;
      /* bars that fit */
      for (last_bar = first_bar; last_bar < nt->nbars; last_bar++)
        {
          int w = fits[last_bar].natural - (last_bar == first_bar ? chg[first_bar] : 0);
          if (last_bar > first_bar && run[last_bar] && head_w + used + w + (last_bar + 1 < nt->nbars ? chg[last_bar + 1] : 0) > avail)
            break;
          used += w;
        }
      /* (with room at the end for a courtesy clef or key, if the next bar
         changes them) */
      while (last_bar < nt->nbars && last_bar - first_bar > 1 && head_w + used + chg[last_bar] > avail)
        {
          do
            used -= fits[--last_bar].natural;
          while (last_bar - first_bar > 1 && !run[last_bar]);
        }
      cour = last_bar < nt->nbars ? chg[last_bar] : 0;
      /* stretch them to fill the line (the last line less, if it's short) */
      for (b = first_bar; b < last_bar; b++)
        {
          int i;
          for (i = 0; i < fits[b].n; i++)
            {
              segs += fits[b].seg[i];
              fixed += fits[b].pre[i];
            }
          fixed += fits[b].natural - (b == first_bar ? chg[b] : 0);
          for (i = 0; i < fits[b].n; i++)
            fixed -= fits[b].seg[i] + fits[b].pre[i];
        }
      factor = segs ? (double) (avail - head_w - fixed - cour) / segs : 1;
      if (last_bar >= nt->nbars && factor > 1.3)
        factor = 1.3;
      if (factor < 0.5)
        factor = 0.5;
      /* room between the staves: a first guess, measured below */
      for (row = 0; row < nrows; row++)
        gap[row] = row != d->g0[row] ? SP * 6 : SP * 7;
      if (sysno == 0 && tempo > 0 && !nt->tempo_hidden)
        top_ext += SP * 2;
      for (pass = 0; pass < 2; pass++)
        {
          int ext_lo[MAXROWS], ext_hi[MAXROWS], need_top, need_bot, changed = 0;
          if (pass)
            {
              /* again, with the room it needs */
              e->n = n0;
              e->tlen = tl0;
              *d = *saved;
            }
          else
            {
              n0 = e->n;
              tl0 = e->tlen;
              *saved = *d;
            }
          d->ndyn = 0;
          d->nly = 0;
          d->ntx = 0;
          d->nmet = 0;
          d->nxr = 0;
          d->nhp = 0;
          d->nped = 0;
          d->npl = 0;
          for (row = 0; row < MAXROWS; row++)
            if (d->pl_on[row])
              d->pl_x[row] = lft + head_w, d->pl_hook0[row] = 0;   /* (carried on from the last system) */
          for (pi = 0; pi < nparts; pi++)
            if (d->hp_on[pi])
              d->hp_x[pi] = lft + head_w;    /* (a hairpin carried on from the last system) */
          for (row = 0; row < nrows; row++)
            {
              ext_lo[row] = 1 << 30;
              ext_hi[row] = -(1 << 30);
              d->staff_y[row] = row ? d->staff_y[row - 1] - gap[row] - SP * 4 : y - top_ext - SP * 4;
            }
          ext_base = ext_lo;
          if (item_row && n0 < item_row_cap)
            memset (item_row + n0, -1, (size_t) (item_row_cap - n0));
          d->sys = sysno;
          d->sys_x0 = lft + head_w;
          /* the staves */
          {
            int sx1;
            x = lft + head_w;
            for (b = first_bar; b < last_bar; b++)
              x += bar_width (&fits[b], factor) - (b == first_bar ? chg[b] : 0);
            sx1 = x + cour;
            d->sys_x1 = sx1;
            for (row = 0; row < nrows; row++)
              for (k = 0; k < 5; k++)
                if ((d->rp[row]->lines != 1 || k == 0)   /* (a drum's one line: where its notes are) */
                    && put (e, EK_LINE, 0, lft, d->staff_y[row] + k * SP, sx1, d->staff_y[row] + k * SP, STAFF_LINE))
                  goto fail;
            /* the line down the left of the system, and a brace for each
               part on two staves */
            if (nrows > 1 && put (e, EK_LINE, 0, lft, d->staff_y[last], lft, d->staff_y[0] + 4 * SP, BARLINE))
              goto fail;
            for (row = 0; row < nrows; row++)
              if (row == d->b0[row] && d->b1[row] > row)
                {
                  int top = d->staff_y[row] + 4 * SP, bot = d->staff_y[d->b1[row]];
                  if (glyph (e, GL_BRACE, lft - SP * 3 / 2, bot, (top - bot) * 100 / 1000))
                    goto fail;
                }
            /* the parts' names */
            if (names && sysno == 0)
              for (row = 0; row < nrows; row++)
                if (row == d->g0[row])
                  {
                    const gr_npart *pp = d->rp[row];
                    int mid = d->g1[row] > row ? (d->staff_y[row] + 4 * SP + d->staff_y[d->g1[row]]) / 2 : d->staff_y[row] + 2 * SP;
                    if (text (e, MARGIN, mid - SP / 2, SP * 3 / 2, pp->name))
                      goto fail;
                  }
            /* clef, key, time */
            for (row = 0; row < nrows; row++)
              {
                const gr_npart *pp = d->rp[row];
                int c = bclef (pp, first_bar, d->rs[row]), g = clef_glyph (c), nk, kf = bkey (pp, first_bar);
                int cy = clef_y (c), kx = lft + SP * 2 / 3 + clefw + SP * 2 / 3;
                if (glyph (e, g, lft + SP * 2 / 3, d->staff_y[row] + cy * HSP, 100))
                  goto fail;
                nk = abs (kf);
                for (k = 0; k < nk && k < 7; k++)
                  {
                    int pos = key_pos (c, kf > 0, k);
                    if (glyph (e, kf > 0 ? GL_SHARP : GL_FLAT, kx + k * (gw (GL_SHARP) + SP / 6),
                               d->staff_y[row] + pos * HSP, 100))
                      goto fail;
                  }
              }
            if (show_time && draw_time (e, lft + head_w - time_width (fb->num, fb->den), d->staff_y, nrows, fb->num, fb->den, fb->timesym == 3 ? 0 : fb->timesym))
              goto fail;
            /* the tempo, over the first system */
            if (sysno == 0 && tempo > 0 && !nt->tempo_hidden && d->nmet < 64)
              {
                /* (drawn with the words over the staff, below) */
                d->met_x[d->nmet] = lft + SP;
                d->met_bar[d->nmet] = first_bar;
                d->met_bpm[d->nmet++] = tempo;
              }
            /* the bar number, from the second system */
            if (sysno > 0)
              {
                char t[12];
                snprintf (t, sizeof t, "%d", first_bar + !nt->bars[0].pickup);
                if (text (e, lft, d->staff_y[0] + 4 * SP + SP, SP * 6 / 5, t))
                  goto fail;
              }
          }
          /* the bars */
          x = lft + head_w;
          for (b = first_bar; b < last_bar; b++)
            {
              const gr_nbar *nb = &nt->bars[b];
              int bx0 = x, i, *ox, bx1, top = d->staff_y[0] + 4 * SP, bot = d->staff_y[last], endb = b + run[b] - 1;
              if (!run[b])
                continue;
              ox = malloc ((size_t) (fits[b].n + 1) * sizeof *ox);
              if (!ox)
                goto fail;
              /* a change of clef or key (shown at the end of the system
                 before instead, if the bar starts one) */
              if (b > first_bar)
                {
                  if (draw_changes (d, b, x))
                    {
                      free (ox);
                      goto fail;
                    }
                  x += clef_change_width (ps, nparts, b) + keys_change_width (ps, nparts, b);
                }
              if (time_shown (nt, b))
                {
                  if (draw_time (e, x + SP / 2, d->staff_y, nrows, nb->num, nb->den, nb->timesym))
                    {
                      free (ox);
                      goto fail;
                    }
                  x += time_width (nb->num, nb->den);
                }
              /* a repeat starting: a thick line, a thin one and two dots
                 (after a repeat ending: just the thin line and dots) */
              if (b > 0 && (nb->rep & NR_START))
                {
                  int after = nt->bars[b - 1].rep & NR_END;
                  for (row = 0; row < nrows; row++)
                    if (row == d->g0[row])
                      {
                        int ptop = d->staff_y[row] + 4 * SP, pbot = d->staff_y[d->g1[row]], r2;
                        if ((!after && put (e, EK_LINE, 0, x + THICK_BAR / 2, pbot, x + THICK_BAR / 2, ptop, THICK_BAR))
                            || put (e, EK_LINE, 0, x + THICK_BAR + SP / 2, pbot, x + THICK_BAR + SP / 2, ptop, BARLINE))
                          {
                            free (ox);
                            goto fail;
                          }
                        for (r2 = row; r2 <= d->g1[row]; r2++)
                          if (glyph (e, GL_DOT, x + THICK_BAR + SP / 2 + BARLINE + SP / 3, d->staff_y[r2] + SP * 3 / 2, 100)
                              || glyph (e, GL_DOT, x + THICK_BAR + SP / 2 + BARLINE + SP / 3, d->staff_y[r2] + SP * 5 / 2, 100))
                            {
                              free (ox);
                              goto fail;
                            }
                      }
                  x += START_REPEAT_W;
                }
              x += SP * 6 / 5;
              for (k = 0; k < run[b]; k++)
                bxs[b + k] = x;
              for (i = 0; i < fits[b].n; i++)
                {
                  x += fits[b].pre[i];
                  ox[i] = x;
                  x += (int) (fits[b].seg[i] * factor);
                }
              bx1 = x;
              /* (keep where the onsets went, for lines over them) */
              free (oxs[b]);
              if ((oxs[b] = malloc ((size_t) (fits[b].n + 1) * sizeof **oxs)))
                memcpy (oxs[b], ox, (size_t) (fits[b].n + 1) * sizeof **oxs);
              for (k = 0; k < run[b]; k++)
                {
                  bxa[b + k] = bx0;
                  bxb[b + k] = bx1;
                }
              /* a change of tempo: its metronome mark over the bar (over a
                 multi-bar rest's number, if it starts one) */
              if (b > 0 && nb->tempo > 0 && d->nmet < 64)
                {
                  /* (drawn with the words over the staff, below) */
                  d->met_x[d->nmet] = bx0 + SP / 2;
                  d->met_bar[d->nmet] = b;
                  d->met_bpm[d->nmet++] = nb->tempo;
                }
              if (run[b] > 1)
                {
                  /* a multi-bar rest: a thick bar on the middle line,
                     how many bars over it; a box for each bar */
                  char num[12];
                  int n = run[b], j, xa = bx0 + SP * 3 / 2, xb = bx1 - SP;
                  snprintf (num, sizeof num, "%d", n);
                  for (j = 0; j < n; j++)
                    if (put (e, EK_BAR, 0, bx0 + (bx1 - bx0) * j / n, bot - SP * 2, bx0 + (bx1 - bx0) * (j + 1) / n,
                             top + SP * 2, b + j))
                      {
                        free (ox);
                        goto fail;
                      }
                  for (row = 0; row < nrows && nt->nly; row++)
                    {
                      /* (words at its first bar, over or under the rest) */
                      const gr_npart *pp = d->rp[row];
                      int zs = d->rs[row], vz, q;
                      for (vz = 0; vz < GR_VPS; vz++)
                        {
                          const gr_nrange *rr = &pp->voice[b * GR_NVOICES + zs * GR_VPS + vz];
                          for (q = 0; q < rr->n; q++)
                            {
                              int li = gr_notation_lyric (nt, rr->first + q);
                              for (; li >= 0 && li < nt->nly && nt->ly[li].ev == rr->first + q; li++)
                                if (nt->ly[li].verse >= GR_TEXT_ABOVE && d->ntx < 256)
                                  {
                                    d->tx_x[d->ntx] = bx0 + SP;
                                    d->tx_row[d->ntx] = row;
                                    d->tx_above[d->ntx] = nt->ly[li].verse == GR_TEXT_BELOW ? 0 : nt->ly[li].verse == GR_TEXT_PEDAL ? 2 : 1;
                                    d->tx_style[d->ntx] = nt->ly[li].verse == GR_TEXT_TEMPO ? 1
                                      : nt->ly[li].verse == GR_TEXT_TEMPO_MARK ? 2 : 0;
                                    d->tx_bar[d->ntx] = b;
                                    d->tx_text[d->ntx++] = nt->ly[li].text;
                                  }
                            }
                        }
                    }
                  for (row = 0; row < nrows; row++)
                    {
                      int mid = d->staff_y[row] + 2 * SP, from = e->n, nw = (int) strlen (num) * gw (GL_TIME_4);
                      if (put (e, EK_BEAM, 0, xa, mid + SP / 4, xb, mid + SP / 4, SP / 2)
                          || put (e, EK_LINE, 0, xa, mid - SP, xa, mid + SP, BARLINE)
                          || put (e, EK_LINE, 0, xb, mid - SP, xb, mid + SP, BARLINE))
                        {
                          free (ox);
                          goto fail;
                        }
                      for (j = 0; num[j]; j++)
                        if (glyph (e, GL_TIME_0 + num[j] - '0', (xa + xb - nw) / 2 + j * gw (GL_TIME_4),
                                   d->staff_y[row] + 5 * SP + SP / 2, 100))
                          {
                            free (ox);
                            goto fail;
                          }
                      extent (e, from, e->n, &ext_lo[row], &ext_hi[row]);
                    }
                }
              else
                {
                  if (put (e, EK_BAR, 0, bx0, bot - SP * 2, bx1, top + SP * 2, b))
                    {
                      free (ox);
                      goto fail;
                    }
                  for (row = 0; row < nrows; row++)
                    {
                      const gr_npart *pp = d->rp[row];
                      int zs = d->rs[row], two = 0, from = e->n, vz;
                      for (vz = 1; vz < GR_VPS; vz++)
                        two |= pp->voice[b * GR_NVOICES + zs * GR_VPS + vz].n > 0;
                      for (vz = 0; vz < GR_VPS; vz++)
                        if (draw_voice (d, b, row, vz, fits[b].t, ox, fits[b].n, bx0, bx1, two))
                          {
                            free (ox);
                            goto fail;
                          }
                      extent_rows (d, from, e->n, row, ext_lo, ext_hi);
                    }
                  /* the pedal: where in the bar, between the notes either side */
                  for (row = 0; row < nrows; row++)
                    {
                      const gr_npart *pp = d->rp[row];
                      if (d->rs[row] != pp->staves - 1)
                        continue;
                      for (i = 0; i < pp->npedal; i++)
                        if (pp->pedal[i].bar == b && (!pp->pedal[i].hidden || pp->pedal[i].line) && d->nped < 256)
                          {
                            int t = pp->pedal[i].start, j = 0, px, xa, xb, ta, tb;
                            while (j + 1 < fits[b].n && fits[b].t[j + 1] <= t)
                              j++;
                            ta = fits[b].t[j];
                            xa = ox[j];
                            tb = j + 1 < fits[b].n ? fits[b].t[j + 1] : nb->len;
                            xb = j + 1 < fits[b].n ? ox[j + 1] : bx1;
                            if (t < ta)
                              ta = 0, xa = bx0 + SP;
                            px = tb > ta ? xa + (int) ((long) (xb - xa) * (t - ta) / (tb - ta)) : xa;
                            /* (a line: on from here, or ended here) */
                            if (pp->pedal[i].line)
                              {
                                if (pp->pedal[i].down)
                                  {
                                    d->pl_on[row] = 1;
                                    d->pl_onup[row] = pp->pedal[i].above;
                                    d->pl_hook0[row] = pp->pedal[i].hidden;
                                    d->pl_x[row] = pp->pedal[i].hidden ? px - SP / 4 : px - SP / 4 + gw (GL_PED) * 8 / 10 + SP / 4;
                                  }
                                else if (d->pl_on[row] && d->npl < 64)
                                  {
                                    d->pl_x0[d->npl] = d->pl_x[row];
                                    d->pl_x1[d->npl] = px - SP / 2;
                                    d->pl_row[d->npl] = row;
                                    d->pl_up[d->npl] = (unsigned char) d->pl_onup[row];
                                    d->pl_hooks[d->npl++] = (unsigned char) (d->pl_hook0[row] | (pp->pedal[i].hidden ? 2 : 0));
                                    d->pl_on[row] = 0;
                                  }
                              }
                            if (pp->pedal[i].hidden)
                              continue;
                            if (pp->pedal[i].down)
                              d->ped_x[d->nped] = px - SP / 4;
                            else
                              {
                                d->ped_x[d->nped] = px - gw (GL_PED_UP) * 4 / 10;
                                /* (let straight down again: the star just before) */
                                if (i + 1 < pp->npedal && pp->pedal[i + 1].bar == b && pp->pedal[i + 1].start == t
                                    && !pp->pedal[i + 1].hidden)
                                  d->ped_x[d->nped] -= gw (GL_PED_UP) * 4 / 10 + SP / 4;
                              }
                            d->ped_row[d->nped] = row;
                            d->ped_up[d->nped] = (unsigned char) pp->pedal[i].above;
                            d->ped_g[d->nped++] = pp->pedal[i].down ? GL_PED : GL_PED_UP;
                          }
                    }
                }
              free (ox);
              /* bar lines: down through each part's staves */
              for (row = 0; row < nrows; row++)
                if (row == d->g0[row])
                  {
                    int ptop = d->staff_y[row] + 4 * SP, pbot = d->staff_y[d->g1[row]];
                    if (nt->bars[endb].rep & NR_END)
                      {
                        /* a repeat ending: two dots, a thin line and a thick one */
                        int r2, dx = x - THICK_BAR - SP / 2 - BARLINE / 2 - SP / 3 - gw (GL_DOT);
                        if (put (e, EK_LINE, 0, x - THICK_BAR - SP / 2, pbot, x - THICK_BAR - SP / 2, ptop, BARLINE)
                            || put (e, EK_LINE, 0, x - THICK_BAR / 2, pbot, x - THICK_BAR / 2, ptop, THICK_BAR))
                          goto fail;
                        for (r2 = row; r2 <= d->g1[row]; r2++)
                          if (glyph (e, GL_DOT, dx, d->staff_y[r2] + SP * 3 / 2, 100)
                              || glyph (e, GL_DOT, dx, d->staff_y[r2] + SP * 5 / 2, 100))
                            goto fail;
                      }
                    else if (endb == nt->nbars - 1)
                      {
                        if (put (e, EK_LINE, 0, x - THICK_BAR - SP / 2, pbot, x - THICK_BAR - SP / 2, ptop, BARLINE)
                            || put (e, EK_LINE, 0, x - THICK_BAR / 2, pbot, x - THICK_BAR / 2, ptop, THICK_BAR))
                          goto fail;
                      }
                    else if (endb + 1 < last_bar && (nt->bars[endb + 1].rep & NR_START))
                      ;     /* (the next bar's start repeat stands for it) */
                    else if (keys_change_width (ps, nparts, endb + 1))
                      {
                        /* (before a new key: a double bar line) */
                        if (put (e, EK_LINE, 0, x - BARLINE / 2 - SP / 2, pbot, x - BARLINE / 2 - SP / 2, ptop, BARLINE)
                            || put (e, EK_LINE, 0, x - BARLINE / 2, pbot, x - BARLINE / 2, ptop, BARLINE))
                          goto fail;
                      }
                    else if (put (e, EK_LINE, 0, x - BARLINE / 2, pbot, x - BARLINE / 2, ptop, BARLINE))
                      goto fail;
                  }
            }
          /* a courtesy clef or key at the end, for the next system's */
          if (cour && draw_changes (d, last_bar, x))
            goto fail;
          /* 8va and 8vb lines, then first and second endings, and how
             many times a repeat's played, over the top */
          if (lines_over (d, first_bar, last_bar, bxs, bxb, fits, oxs, ext_lo, ext_hi))
            goto fail;
          /* ties and slurs going on to the next system */
          for (row = 0; row < nrows; row++)
            if (carry_over (d, row, &ext_lo[row], &ext_hi[row]))
              goto fail;
          /* hairpins going on to the next system */
          for (row = 0; row < nrows; row++)
            if (d->rs[row] == 0 && d->hp_on[d->rpart[row]])
              hairpin_piece (d, d->rpart[row], row, d->hp_x[d->rpart[row]], d->sys_x1 - SP / 2);
          /* ...and pedal lines */
          for (row = 0; row < nrows; row++)
            if (d->pl_on[row] && d->npl < 64)
              {
                d->pl_x0[d->npl] = d->pl_x[row];
                d->pl_x1[d->npl] = d->sys_x1 - SP / 2;
                d->pl_row[d->npl] = row;
                d->pl_up[d->npl] = (unsigned char) d->pl_onup[row];
                d->pl_hooks[d->npl++] = (unsigned char) d->pl_hook0[row];
              }
          /* the words, a line a verse under each staff that has them,
             with hyphens between a word's syllables */
          for (row = 0; row < nrows; row++)
            {
              int i, j, from = e->n, line, any = 0;
              for (i = 0; i < d->nly; i++)
                any |= d->ly_row[i] == row;
              if (!any)
                continue;
              line = d->staff_y[row] - SP;
              if (ext_lo[row] - SP / 2 < line)
                line = ext_lo[row] - SP / 2;
              for (i = 0; i < d->nly; i++)
                {
                  int w, base, x0;
                  if (d->ly_row[i] != row)
                    continue;
                  w = name_width (d->ly_text[i], LYR_SIZE);
                  base = line - LYR_SIZE - d->ly_verse[i] * LYR_SIZE * 3 / 2;
                  x0 = d->ly_x[i] - w / 2;
                  if (text (e, x0, base, LYR_SIZE, d->ly_text[i]))
                    goto fail;
                  if (d->ly_syl[i] == 1 || d->ly_syl[i] == 2)
                    {
                      /* (to the next syllable of this verse, or the system's end) */
                      int xe = d->sys_x1;
                      for (j = i + 1; j < d->nly; j++)
                        if (d->ly_row[j] == row && d->ly_verse[j] == d->ly_verse[i])
                          {
                            xe = d->ly_x[j] - name_width (d->ly_text[j], LYR_SIZE) / 2;
                            break;
                          }
                      if (xe - (x0 + w) > SP / 2
                          && put (e, EK_LINE, 0, (x0 + w + xe) / 2 - SP / 4, base + LYR_SIZE / 3,
                                  (x0 + w + xe) / 2 + SP / 4, base + LYR_SIZE / 3, LEDGER))
                        goto fail;
                    }
                }
              extent (e, from, e->n, &ext_lo[row], &ext_hi[row]);
            }
          /* each part's dynamics and hairpins, in a line under its
             (upper) staff */
          for (row = 0; row < nrows; row++)
            {
              int i, top = 444, from = e->n, line, base, mid, h = SP * 3 / 5, th = SP * 4 / 25, any = 0;
              for (i = 0; i < d->ndyn; i++)
                any |= d->dyn_row[i] == row && !d->dyn_up[i];
              for (i = 0; i < d->nhp; i++)
                any |= d->hp_row[i] == row && !d->hp_up[i];
              for (i = 0; i < d->ntx; i++)
                any |= d->tx_row[i] == row && d->tx_above[i] == 0;
              if (!any)
                continue;
              line = d->staff_y[row] - SP;
              if (ext_lo[row] - SP / 2 < line)
                line = ext_lo[row] - SP / 2;
              base = line - top;
              for (i = 0; i < d->ndyn; i++)
                if (d->dyn_row[i] == row && !d->dyn_up[i] && glyph (e, d->dyn_g[i], d->dyn_x[i], base, 100))
                  goto fail;
              /* words under the staff ("sostenuto"): after a dynamic there */
              for (i = 0; i < d->ntx; i++)
                if (d->tx_row[i] == row && d->tx_above[i] == 0)
                  {
                    int j, tx = d->tx_x[i];
                    for (j = 0; j < d->ndyn; j++)
                      if (d->dyn_row[j] == row && !d->dyn_up[j] && abs (d->dyn_x[j] - tx) < SP * 2)
                        tx = d->dyn_x[j] + gw (d->dyn_g[j]) + SP / 3;
                    if (text (e, tx, base, LYR_SIZE, d->tx_text[i]))
                      goto fail;
                  }
              mid = base + SP * 3 / 4;
              for (i = 0; i < d->nhp; i++)
                if (d->hp_row[i] == row && !d->hp_up[i])
                  {
                    int x0 = d->hp_x0[i], x1 = d->hp_x1[i], open0 = d->hp_cresc[i] ? 0 : h, open1 = d->hp_cresc[i] ? h : 0;
                    if (put (e, EK_BEAM, 0, x0, mid + open0 + th / 2, x1, mid + open1 + th / 2, th)
                        || put (e, EK_BEAM, 0, x0, mid - open0 + th / 2, x1, mid - open1 + th / 2, th))
                      goto fail;
                  }
              extent (e, from, e->n, &ext_lo[row], &ext_hi[row]);
            }
          /* ...and those the score puts over the staff (a voice's), in a line
             over it */
          for (row = 0; row < nrows; row++)
            {
              int i, from = e->n, base, mid, h = SP * 3 / 5, th = SP * 4 / 25, any = 0, lo = 0;
              for (i = 0; i < d->ndyn; i++)
                any |= d->dyn_row[i] == row && d->dyn_up[i];
              for (i = 0; i < d->nhp; i++)
                any |= d->hp_row[i] == row && d->hp_up[i];
              if (!any)
                continue;
              /* (clear of the notes over the staff: the dynamics' lowest point on it) */
              for (i = 0; i < d->ndyn; i++)
                if (d->dyn_row[i] == row && d->dyn_up[i] && gr_glyphs[d->dyn_g[i]].y0 < lo)
                  lo = gr_glyphs[d->dyn_g[i]].y0;
              base = d->staff_y[row] + 5 * SP - lo;
              if (ext_hi[row] + SP / 2 - lo > base)
                base = ext_hi[row] + SP / 2 - lo;
              for (i = 0; i < d->ndyn; i++)
                if (d->dyn_row[i] == row && d->dyn_up[i] && glyph (e, d->dyn_g[i], d->dyn_x[i], base, 100))
                  goto fail;
              mid = base + SP / 2;
              for (i = 0; i < d->nhp; i++)
                if (d->hp_row[i] == row && d->hp_up[i])
                  {
                    int x0 = d->hp_x0[i], x1 = d->hp_x1[i], open0 = d->hp_cresc[i] ? 0 : h, open1 = d->hp_cresc[i] ? h : 0;
                    if (put (e, EK_BEAM, 0, x0, mid + open0 + th / 2, x1, mid + open1 + th / 2, th)
                        || put (e, EK_BEAM, 0, x0, mid - open0 + th / 2, x1, mid - open1 + th / 2, th))
                      goto fail;
                  }
              extent (e, from, e->n, &ext_lo[row], &ext_hi[row]);
            }
          /* the pedal, in a line under each part's lowest staff */
          for (row = 0; row < nrows; row++)
            {
              int i, from = e->n, line = d->staff_y[row] - SP * 3 / 2, any = 0;
              for (i = 0; i < d->nped; i++)
                any |= d->ped_row[i] == row && !d->ped_up[i];
              for (i = 0; i < d->npl; i++)
                any |= d->pl_row[i] == row && !d->pl_up[i];
              for (i = 0; i < d->ntx; i++)
                any |= d->tx_above[i] == 2 && d->tx_row[i] - d->rs[d->tx_row[i]] + d->rp[d->tx_row[i]]->staves - 1 == row;
              if (!any)
                continue;
              if (ext_lo[row] - SP * 2 / 3 < line)
                line = ext_lo[row] - SP * 2 / 3;
              /* (a pedal's words: from its chord, the "Ped." after them) */
              for (i = 0; i < d->ntx; i++)
                if (d->tx_above[i] == 2 && d->tx_row[i] - d->rs[d->tx_row[i]] + d->rp[d->tx_row[i]]->staves - 1 == row)
                  {
                    int j, x = d->tx_x[i] - SP / 4, w = name_width (d->tx_text[i], LYR_SIZE);
                    if (text_st (e, x, line - gr_glyphs[GL_PED].y1 * 8 / 10, LYR_SIZE, d->tx_text[i], 2))
                      goto fail;
                    for (j = 0; j < d->nped; j++)
                      if (d->ped_row[j] == row && !d->ped_up[j] && d->ped_g[j] == GL_PED && abs (d->ped_x[j] - x) < SP * 2)
                        d->ped_x[j] = x + w * 6 / 5 + SP / 2;
                  }
              for (i = 0; i < d->nped; i++)
                if (d->ped_row[i] == row && !d->ped_up[i]
                    && glyph (e, d->ped_g[i], d->ped_x[i], line - gr_glyphs[d->ped_g[i]].y1 * 8 / 10, 80))
                  goto fail;
              /* (the lines: along the bottom of the "Ped.", hooked up at the ends) */
              for (i = 0; i < d->npl; i++)
                if (d->pl_row[i] == row && !d->pl_up[i] && d->pl_x1[i] > d->pl_x0[i])
                  {
                    int ly = line - SP, th = SP / 8;
                    if (put (e, EK_BEAM, 0, d->pl_x0[i], ly + th / 2, d->pl_x1[i], ly + th / 2, th)
                        || ((d->pl_hooks[i] & 1) && put (e, EK_LINE, 0, d->pl_x0[i], ly, d->pl_x0[i], ly + SP, th))
                        || ((d->pl_hooks[i] & 2) && put (e, EK_LINE, 0, d->pl_x1[i], ly, d->pl_x1[i], ly + SP, th)))
                      goto fail;
                  }
              extent (e, from, e->n, &ext_lo[row], &ext_hi[row]);
            }
          /* ...and those the score puts over its staff (the hooks down) */
          for (row = 0; row < nrows; row++)
            {
              int i, from = e->n, line, any = 0;
              for (i = 0; i < d->nped; i++)
                any |= d->ped_row[i] == row && d->ped_up[i];
              for (i = 0; i < d->npl; i++)
                any |= d->pl_row[i] == row && d->pl_up[i];
              if (!any)
                continue;
              /* (clear of the notes under it, place by place) */
              line = d->staff_y[row] + 4 * SP + SP * 2;
              for (i = 0; i < d->nped; i++)
                if (d->ped_row[i] == row && d->ped_up[i])
                  {
                    int t = row_top_in (e, n0, from, row, d->ped_x[i], d->ped_x[i] + gw (d->ped_g[i]), line) + SP * 2;
                    if (t > line)
                      line = t;
                  }
              for (i = 0; i < d->npl; i++)
                if (d->pl_row[i] == row && d->pl_up[i])
                  {
                    int t = row_top_in (e, n0, from, row, d->pl_x0[i], d->pl_x1[i], line) + SP * 2;
                    if (t > line)
                      line = t;
                  }
              for (i = 0; i < d->nped; i++)
                if (d->ped_row[i] == row && d->ped_up[i]
                    && glyph (e, d->ped_g[i], d->ped_x[i], line - gr_glyphs[d->ped_g[i]].y1 * 8 / 10, 80))
                  goto fail;
              for (i = 0; i < d->npl; i++)
                if (d->pl_row[i] == row && d->pl_up[i] && d->pl_x1[i] > d->pl_x0[i])
                  {
                    int ly = line - SP, th = SP / 8;
                    if (put (e, EK_BEAM, 0, d->pl_x0[i], ly + th / 2, d->pl_x1[i], ly + th / 2, th)
                        || ((d->pl_hooks[i] & 1) && put (e, EK_LINE, 0, d->pl_x0[i], ly - SP, d->pl_x0[i], ly, th))
                        || ((d->pl_hooks[i] & 2) && put (e, EK_LINE, 0, d->pl_x1[i], ly - SP, d->pl_x1[i], ly, th)))
                      goto fail;
                  }
              extent (e, from, e->n, &ext_lo[row], &ext_hi[row]);
            }
          /* words over the staff ("dolce", "rit."): over all else there;
             a tempo's in bold, with its metronome mark after them if
             the score writes them together ("Andante. (\u2669 = 66)"); the
             other metronome marks over the top staff */
          for (row = 0; row < nrows; row++)
            {
              int i, j, from = e->n, base = d->staff_y[row] + 5 * SP, any = 0, nplaced = 0, placed[64][3];
              unsigned char used[64];
              memset (used, 0, sizeof used);
              for (i = 0; i < d->ntx; i++)
                any |= d->tx_row[i] == row && d->tx_above[i] == 1;
              if (!any && !(row == 0 && d->nmet))
                continue;
              if (ext_hi[row] + SP / 2 > base)
                base = ext_hi[row] + SP / 2;
              for (i = 0; i < d->ntx; i++)
                if (d->tx_row[i] == row && d->tx_above[i] == 1)
                  {
                    if (d->tx_style[i] == 2)
                      for (j = 0; j < d->nmet; j++)
                        if (d->met_bar[j] == d->tx_bar[i])
                          used[j] = 1;      /* (the mark the text writes: not again) */
                    {
                      /* (over words already there that it would run into) */
                      int y = base, x0 = d->tx_x[i], x1 = x0 + name_width (d->tx_text[i], LYR_SIZE) * (d->tx_style[i] ? 11 : 10) / 10;
                      int q, moved = 1;
                      while (moved)
                        {
                          moved = 0;
                          for (q = 0; q < nplaced; q++)
                            if (placed[q][2] == y && x0 < placed[q][1] + SP / 2 && placed[q][0] < x1 + SP / 2)
                              {
                                y += LYR_SIZE * 3 / 2;
                                moved = 1;
                              }
                        }
                      if (nplaced < 64)
                        {
                          placed[nplaced][0] = x0;
                          placed[nplaced][1] = x1;
                          placed[nplaced++][2] = y;
                        }
                      if (tempo_text (e, x0, y, d->tx_text[i], d->tx_style[i] != 0))
                        goto fail;
                    }
                  }
              if (row == 0)
                for (j = 0; j < d->nmet; j++)
                  if (!used[j])
                    {
                      /* (a mark on its own: over words at the same place) */
                      char t[16];
                      int y = base;
                      for (i = 0; i < d->ntx; i++)
                        if (d->tx_row[i] == 0 && d->tx_above[i] == 1 && d->tx_bar[i] == d->met_bar[j])
                          y = base + LYR_SIZE * 3 / 2;
                      snprintf (t, sizeof t, "%c = %d", GR_CH_NOTE4, d->met_bpm[j]);
                      if (tempo_text (e, d->met_x[j], y, t, 1))
                        goto fail;
                    }
              extent (e, from, e->n, &ext_lo[row], &ext_hi[row]);
            }
          /* the room it needed */
          need_top = ext_hi[0] - (d->staff_y[0] + 4 * SP) + SP * 3 / 4;
          if (need_top < SP * 3)
            need_top = SP * 3;
          if (sysno == 0 && tempo > 0 && !nt->tempo_hidden)
            need_top += SP * 2;
          if (need_top != top_ext)
            changed = 1;
          top_ext = need_top;
          for (row = 1; row < nrows; row++)
            {
              int lo = ext_lo[row - 1] < d->staff_y[row - 1] ? ext_lo[row - 1] : d->staff_y[row - 1];
              int hi = ext_hi[row] > d->staff_y[row] + 4 * SP ? ext_hi[row] : d->staff_y[row] + 4 * SP;
              int clear = d->rs[row] ? SP * 5 / 4 : SP * 2;
              int need = (d->staff_y[row - 1] - lo) + (hi - (d->staff_y[row] + 4 * SP)) + clear,
                min = d->rs[row] ? SP * 6 : SP * 7;
              /* (fitted place by place, as MuseScore does: what's under the
                 one staff only has to clear what's over the other there) */
              int sky = sky_gap (e, n0, e->n, row - 1, d->staff_y[row - 1], d->staff_y[row] + 4 * SP, clear);
              if (sky >= 0 && sky < need)
                need = sky;
              if (need < min)
                need = min;
              if (need != gap[row])
                changed = 1;
              gap[row] = need;
            }
          need_bot = d->staff_y[last] - ext_lo[last] + SP;
          if (need_bot < SP * 3)
            need_bot = SP * 3;
          if (need_bot != bot_ext)
            changed = 1;
          bot_ext = need_bot;
          if (!changed)
            break;
        }
      {
        int *a = realloc (e->sys_top, (size_t) (e->nsys + 1) * sizeof *a), *z2;
        if (!a)
          goto fail;
        e->sys_top = a;
        if (!(z2 = realloc (e->sys_bot, (size_t) (e->nsys + 1) * sizeof *z2)))
          goto fail;
        e->sys_bot = z2;
        e->sys_top[e->nsys] = sys_y0;
        e->sys_bot[e->nsys++] = d->staff_y[last] - bot_ext;
      }
      y = d->staff_y[last] - bot_ext;
      first_bar = last_bar;
    }
  e->height = -y + SP * 2;
  ext_base = NULL;
  free (acc);
  for (b = 0; b < nt->nbars; b++)
    {
      free (fits[b].t);
      free (fits[b].seg);
      free (fits[b].pre);
    }
  free (fits);
  free (run);
  free (bxa);
  free (bxb);
  free (bxs);
  if (oxs)
    for (b = 0; b < nt->nbars; b++)
      free (oxs[b]);
  free (oxs);
  free (chg);
  free (d);
  free (saved);
  return e;
fail:
  ext_base = NULL;
  free (acc);
  if (fits)
    for (b = 0; b < nt->nbars; b++)
      {
        free (fits[b].t);
        free (fits[b].seg);
        free (fits[b].pre);
      }
  free (fits);
  free (run);
  free (bxa);
  free (bxb);
  free (bxs);
  if (oxs)
    for (b = 0; b < nt->nbars; b++)
      free (oxs[b]);
  free (oxs);
  free (chg);
  free (d);
  free (saved);
  gr_engraving_free (e);
  return NULL;
}

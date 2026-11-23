/* Groove - written scores into the song: the part shared by the MuseScore
   and MusicXML importers. Repeats and voltas are played out, as MuseScore
   plays them; tied notes become one note; each part becomes a track, with
   its instrument as a SoundFont sound, cut into regions as MIDI import
   does. D.C. and D.S. jumps aren't followed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "groove.h"
#include "place.h"
#include "score.h"
#include "unzip.h"

void gs_init (gs_score *sc)
{
  memset (sc, 0, sizeof *sc);
}

void gs_free (gs_score *sc)
{
  int i;
  for (i = 0; i < sc->nparts; i++)
    {
      free (sc->parts[i].notes);
      free (sc->parts[i].pedals);
      free (sc->parts[i].marks);
      free (sc->parts[i].lyrics);
    }
  free (sc->tempos);
  free (sc->parts);
  free (sc->meas);
  memset (sc, 0, sizeof *sc);
}

/* A name from a score (UTF-8) in RISC OS's Latin-1: accented letters
   kept, flat and sharp signs as b and #, anything else as ?. */
static void to_latin1 (char *out, size_t n, const char *in)
{
  const unsigned char *p = (const unsigned char *) (in ? in : "");
  size_t o = 0;
  while (*p && o + 2 < n)
    {
      unsigned long c = *p;
      int len = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1, i;
      if (len > 1)
        {
          c &= 0x3Fu >> (len - 1);
          for (i = 1; i < len && (p[i] & 0xC0) == 0x80; i++)
            c = (c << 6) | (p[i] & 0x3F);
          if (i < len)
            len = 1, c = '?';
        }
      p += len;
      if (c == 0x266D)
        out[o++] = 'b';
      else if (c == 0x266F)
        out[o++] = '#';
      else if (c == 0x266E)
        ;
      else if (c == 0x2013 || c == 0x2014)
        out[o++] = '-';
      else if (c == 0x2018 || c == 0x2019)
        out[o++] = '\'';
      else if (c == 0x201C || c == 0x201D)
        out[o++] = '"';
      else if (c == 0x203F || c == 0x2040)
        out[o++] = '_';                /* (an elision: two syllables on one note) */
      else if ((c == 0x0153 || c == 0x0152) && o + 3 < n)
        out[o++] = c == 0x0153 ? 'o' : 'O', out[o++] = c == 0x0153 ? 'e' : 'E';
      else if (c >= 0x2000 && c <= 0x200A)
        out[o++] = ' ';
      else if (c == 0x2669 || c == 0xECA5 || c == 0xE1D5)
        out[o++] = (char) GR_CH_NOTE4;   /* (a metronome mark's notes: Unicode's, SMuFL's) */
      else if (c == 0x1D15E || c == 0xECA3 || c == 0xE1D3)
        out[o++] = (char) GR_CH_NOTE2;
      else if (c == 0x266A || c == 0xECA7 || c == 0xE1D7)
        out[o++] = (char) GR_CH_NOTE8;
      else if (c == 0x1D16D || c == 0xECB7 || c == 0xE1E7)
        out[o++] = (char) GR_CH_NOTEDOT;
      else if ((c >= 0x200B && c <= 0x200F) || c == 0x2060 || c == 0xFEFF || (c >= 0xE000 && c <= 0xF8FF))
        ;                              /* (marks that take no room; a music font's symbols) */
      else if (c == 0x2026 && o + 4 < n)
        out[o++] = '.', out[o++] = '.', out[o++] = '.';
      else if (c < 0x20)
        out[o++] = ' ';
      else
        out[o++] = (char) (c < 0x100 ? c : '?');
    }
  out[o] = 0;
}

gs_part *gs_add_part (gs_score *sc, const char *name)
{
  gs_part *p = realloc (sc->parts, (size_t) (sc->nparts + 1) * sizeof *p);
  if (!p)
    return NULL;
  sc->parts = p;
  p = &sc->parts[sc->nparts++];
  memset (p, 0, sizeof *p);
  to_latin1 (p->name, sizeof p->name, name);
  {
    /* (on one line, without spaces either end: "Voice\n" is "Voice") */
    char *c = p->name, *d = p->name;
    size_t n;
    while (*c == ' ' || *c == '\n' || *c == '\r' || *c == '\t')
      c++;
    for (; *c; c++)
      *d++ = *c == '\n' || *c == '\r' || *c == '\t' ? ' ' : *c;
    *d = 0;
    for (n = strlen (p->name); n > 0 && p->name[n - 1] == ' '; n--)
      p->name[n - 1] = 0;
  }
  p->program = p->bank_msb = p->bank_lsb = p->channel = -1;
  return p;
}

int gs_add_pedal (gs_part *p, int measure, double off, int down)
{
  if (p->npedals == p->pedcap)
    {
      int cap = p->pedcap ? p->pedcap * 2 : 64;
      gs_pedal *a = realloc (p->pedals, (size_t) cap * sizeof *a);
      if (!a)
        return -1;
      p->pedals = a;
      p->pedcap = cap;
    }
  p->pedals[p->npedals].measure = measure;
  p->pedals[p->npedals].off = off;
  p->pedals[p->npedals].down = down;
  p->npedals++;
  return 0;
}

int gs_add_mark (gs_part *p, int measure, double off, int emeasure, double eoff,
                 int kind, int value, int pitch, int staff)
{
  gs_mark *k;
  if (measure < 0)
    return 0;
  if (p->nmarks == p->markcap)
    {
      int cap = p->markcap ? p->markcap * 2 : 64;
      gs_mark *a = realloc (p->marks, (size_t) cap * sizeof *a);
      if (!a)
        return -1;
      p->marks = a;
      p->markcap = cap;
    }
  k = &p->marks[p->nmarks++];
  k->measure = measure;
  k->off = off;
  k->emeasure = emeasure;
  k->eoff = eoff;
  k->kind = kind;
  k->value = value;
  k->pitch = pitch;
  k->staff = staff;
  k->hi = pitch;
  k->st = p->cur_staff;
  return 0;
}

int gs_add_lyric (gs_part *p, int measure, double off, int staff, int verse, const char *syllabic, const char *text)
{
  gs_lyric *l;
  if (!text || !*text)
    return 0;
  if (p->nlyrics == p->lyriccap)
    {
      int cap = p->lyriccap ? p->lyriccap * 2 : 64;
      gs_lyric *a = realloc (p->lyrics, (size_t) cap * sizeof *a);
      if (!a)
        return -1;
      p->lyrics = a;
      p->lyriccap = cap;
    }
  l = &p->lyrics[p->nlyrics++];
  l->measure = measure;
  l->off = off;
  l->staff = staff;
  l->st = p->cur_staff;
  l->verse = verse;
  l->syllabic = !syllabic ? 0 : !strcmp (syllabic, "begin") ? 1 : !strcmp (syllabic, "middle") ? 2
    : !strcmp (syllabic, "end") ? 3 : 0;
  to_latin1 (l->text, sizeof l->text, text);
  return 0;
}

int gs_add_tempo (gs_score *sc, int measure, double off, double bpm)
{
  if (bpm <= 0 || measure < 0)
    return 0;
  if (sc->tempo == 0)
    sc->tempo = bpm;
  if (sc->ntempos == sc->tempocap)
    {
      int cap = sc->tempocap ? sc->tempocap * 2 : 16;
      gs_tempo *a = realloc (sc->tempos, (size_t) cap * sizeof *a);
      if (!a)
        return -1;
      sc->tempos = a;
      sc->tempocap = cap;
    }
  sc->tempos[sc->ntempos].measure = measure;
  sc->tempos[sc->ntempos].off = off;
  sc->tempos[sc->ntempos].bpm = bpm;
  sc->tempos[sc->ntempos].hidden = 0;
  sc->ntempos++;
  return 0;
}

static int cmp_pctl (const void *a, const void *b)
{
  const gr_pctl *x = a, *y = b;
  if (x->tick != y->tick)
    return x->tick < y->tick ? -1 : 1;
  return x->value - y->value;   /* (up before down at the same moment) */
}

int gs_add_note (gs_part *p, int measure, double off, double len, int pitch, int vel, int tie, int wr)
{
  gs_note *n;
  if (pitch < 0 || pitch > 127 || len <= 0)
    return 0;
  if (p->n == p->cap)
    {
      int cap = p->cap ? p->cap * 2 : 256;
      gs_note *a = realloc (p->notes, (size_t) cap * sizeof *a);
      if (!a)
        return -1;
      p->notes = a;
      p->cap = cap;
    }
  n = &p->notes[p->n++];
  n->st = p->cur_staff;
  n->measure = measure;
  n->off = off;
  n->len = len;
  n->pitch = pitch;
  n->vel = vel < 1 ? 1 : vel > 127 ? 127 : vel;
  n->tie = tie;
  n->wr = wr;
  return 0;
}

gs_measure *gs_measure_at (gs_score *sc, int i)
{
  if (i < 0)
    return NULL;
  if (i >= sc->meascap)
    {
      int cap = sc->meascap ? sc->meascap * 2 : 64;
      gs_measure *m;
      while (cap <= i)
        cap *= 2;
      m = realloc (sc->meas, (size_t) cap * sizeof *m);
      if (!m)
        return NULL;
      memset (m + sc->meascap, 0, (size_t) (cap - sc->meascap) * sizeof *m);
      sc->meas = m;
      sc->meascap = cap;
    }
  if (i >= sc->nmeas)
    {
      int k;
      for (k = sc->nmeas; k <= i; k++)
        if (!sc->meas[k].num)
          {
            /* A new measure carries on the time signature before it. */
            sc->meas[k].num = k ? sc->meas[k - 1].num : 4;
            sc->meas[k].den = k ? sc->meas[k - 1].den : 4;
            sc->meas[k].timesym = k ? sc->meas[k - 1].timesym : 0;
            sc->meas[k].len = sc->meas[k].num * 4.0 / sc->meas[k].den;
          }
      sc->nmeas = i + 1;
    }
  return &sc->meas[i];
}

void gs_measure_label (gs_measure *m, const char *label)
{
  if (m->nlabels < 6 && label && *label)
    snprintf (m->labels[m->nlabels++], sizeof m->labels[0], "%s", label);
}

static int has_label (const gs_measure *m, const char *label)
{
  int k;
  for (k = 0; k < m->nlabels; k++)
    if (!strcmp (m->labels[k], label))
      return 1;
  return 0;
}

/* "start": the start of the section measure from is in. */
static int find_label (const gs_score *sc, const char *label, int from)
{
  int i;
  if (!strcmp (label, "start"))
    {
      for (i = from; i > 0 && !sc->meas[i - 1].section_end; i--)
        ;
      return i;
    }
  for (i = 0; i < sc->nmeas; i++)
    if (has_label (&sc->meas[i], label))
      return i;
  return -1;
}

/* Is measure i in a volta group's last volta? (After a jump, with no
   repeats, only the last is played.) */
static int last_volta (const gs_score *sc, int i)
{
  unsigned e = sc->meas[i].endings;
  while (i + 1 < sc->nmeas && sc->meas[i + 1].endings == e && !sc->meas[i].end_repeat)
    i++;
  return i + 1 >= sc->nmeas || !sc->meas[i + 1].endings;
}

/* More endings further on in this repeat (before a new one, or a new
   section)? Then the passes go on through the bars between. */
static int endings_ahead (const gs_score *sc, int k)
{
  for (; k < sc->nmeas && !sc->meas[k].start_repeat; k++)
    {
      if (sc->meas[k].endings)
        return 1;
      if (sc->meas[k].section_end)
        break;
    }
  return 0;
}

/* The order the measures are played in, repeats, voltas and jumps (D.C.,
   D.S., al Fine, al Coda) played out. Returns how many (in *order, to
   free), or -1. */
static int play_order (const gs_score *sc, int **order)
{
  int cap = sc->nmeas * 2 + 16, n = 0, i = 0, start = 0, pass = 1, guard = 0;
  int in_jump = 0, norep = 0;
  const char *until = "", *cont = "";
  int *o = malloc ((size_t) cap * sizeof *o), *count = calloc ((size_t) sc->nmeas + 1, sizeof *count);
  char *jumped = calloc ((size_t) sc->nmeas + 1, 1);
  if (!o || !count || !jumped)
    {
      free (o);
      free (count);
      free (jumped);
      return -1;
    }
  while (i < sc->nmeas && guard++ < 100000)
    {
      const gs_measure *m = &sc->meas[i];
      if (i > 0 && sc->meas[i - 1].section_end && !in_jump)
        {
          pass = 1;                  /* a new section: repeats start again */
          start = i;
        }
      if (norep)
        {
          if (m->endings && !last_volta (sc, i))
            {
              i++;
              continue;
            }
        }
      else
        {
          if (m->start_repeat && pass == 1)
            start = i;
          if (m->endings && pass <= 32 && !(m->endings & (1u << (pass - 1))))
            {
              /* Not this time round: past this volta. On the last pass, the
                 volta that has no bit for it is skipped like the others. */
              i++;
              continue;
            }
        }
      if (n == cap)
        {
          int *a = realloc (o, (size_t) (cap *= 2) * sizeof *a);
          if (!a)
            break;
          o = a;
        }
      o[n++] = i;
      if (norep)
        ;
      else if (m->end_repeat > 0)
        {
          if (++count[i] < m->end_repeat)
            {
              pass++;
              i = start;
              continue;
            }
          count[i] = 0;
          /* Is this the end of a volta? Then the next pass number goes on
             (2nd time ending after the 1st), else the repeat is done. */
          if (!m->endings)
            {
              pass = 1;
              start = i + 1;
            }
        }
      else if (m->endings && pass > 1 && (i + 1 >= sc->nmeas || !sc->meas[i + 1].endings)
               && !endings_ahead (sc, i + 1))
        {
          pass = 1;                  /* the last ending is over */
          start = i + 1;
        }
      else if (m->endings && pass <= 32 && i + 1 < sc->nmeas && sc->meas[i + 1].endings
               && sc->meas[i + 1].endings != m->endings)
        {
          /* An ending with no repeat of its own, other endings straight
             after it: on at the next ending for this time round further
             on (as MuseScore plays "1." "2." "3. Pour finir", then the
             verse's own "1." and "2."), if there's one before a new
             repeat or section. */
          int j = i + 1, k;
          while (j < sc->nmeas && sc->meas[j].endings)
            j++;
          for (k = j; k < sc->nmeas && !sc->meas[k].start_repeat && !sc->meas[k - 1].section_end; k++)
            if (sc->meas[k].endings & (1u << (pass - 1)))
              break;
          if (k < sc->nmeas && (sc->meas[k].endings & (1u << (pass - 1)))
              && !sc->meas[k].start_repeat && !sc->meas[k - 1].section_end)
            {
              i = k;
              continue;
            }
        }
      if (in_jump && *until && has_label (m, until))
        {
          /* Fine, or To Coda: on at the Coda, or the end. */
          int j = *cont ? find_label (sc, cont, i) : -1;
          if (j < 0)
            {
              /* The Fine ends its section: on at the next, if any. */
              for (j = i; j < sc->nmeas && !sc->meas[j].section_end; j++)
                ;
              if (++j >= sc->nmeas)
                break;
            }
          in_jump = norep = 0;
          pass = 1;
          /* (on at a bar in an ending: it's played, as the ending's last time) */
          if (sc->meas[j].endings)
            while (pass < 32 && (sc->meas[j].endings >> pass))
              pass++;
          start = i = j;
          continue;
        }
      if (m->jump && !jumped[i])
        {
          int j = find_label (sc, m->jump_to, i);
          jumped[i] = 1;
          if (j >= 0)
            {
              in_jump = 1;
              norep = !m->jump_repeats;
              until = m->until;
              cont = m->cont;
              pass = 1;
              start = i = j;
              continue;
            }
        }
      i++;
    }
  free (count);
  free (jumped);
  *order = o;
  return n;
}

/* Is t (quarters) where a measure starts? mstart is in order. */
static int on_barline (const double *mstart, int n, double t)
{
  int lo = 0, hi = n - 1;
  while (lo <= hi)
    {
      int mid = (lo + hi) / 2;
      if (fabs (mstart[mid] - t) < 1.0 / 256)
        return 1;
      if (mstart[mid] < t)
        lo = mid + 1;
      else
        hi = mid - 1;
    }
  return 0;
}

static int cmp_pnote (const void *a, const void *b)
{
  const gr_pnote *x = a, *y = b;
  if (x->start != y->start)
    return x->start < y->start ? -1 : 1;
  return x->key - y->key;
}

typedef struct { double start, end, tie1; int pitch, vel, tie, wr, cont; } unote;   /* tie1: gr_note's, in quarters;
                                                                               cont: its last ucont, or -1 */
/* A tied note's continuation written in another voice (as a score can
   tie a note into another voice): played as the note it's tied from,
   written as a note of its own there (GR_MK_NOTE). */
typedef struct { double start, end; int pitch, wr; } ucont;

static int cmp_unote (const void *a, const void *b)
{
  const unote *x = a, *y = b;
  if (x->start != y->start)
    return x->start < y->start ? -1 : 1;
  return x->pitch - y->pitch;
}

/* A part's staves in groups of one or two: an organ's manuals, then its
   pedals; else each staff over the last two on its own, the last two
   together (a piano's extra staff over its hands). Group of each staff,
   and how many groups. */
static int staff_groups (const gs_part *p, int nst, int *grp)
{
  int z, g = 0;
  for (z = 0; z < nst; z++)
    grp[z] = p->organ ? (z < 2 ? 0 : z - 1) : (z < nst - 2 ? z : nst - 2);
  for (z = 0; z < nst; z++)
    if (grp[z] + 1 > g)
      g = grp[z] + 1;
  return g;
}

static int part_staves (const gs_part *p)
{
  int n = 0, k;
  for (k = 0; k < p->n; k++)
    if (p->notes[k].st + 1 > n)
      n = p->notes[k].st + 1;
  for (k = 0; k < p->nmarks; k++)
    if (p->marks[k].st + 1 > n)
      n = p->marks[k].st + 1;
  for (k = 0; k < GS_STAVES; k++)
    if (p->clef[k] && k + 1 > n)
      n = k + 1;
  return n > GS_STAVES ? GS_STAVES : n;
}

int gs_split_staves (gs_score *sc)
{
  int i;
  for (i = 0; i < sc->nparts; i++)
    {
      int nst = part_staves (&sc->parts[i]), grp[GS_STAVES], first[GS_STAVES + 1], ng, g, z, k;
      gs_part *a;
      if (nst < 3 || sc->parts[i].drums)
        continue;
      ng = staff_groups (&sc->parts[i], nst, grp);
      for (z = nst - 1; z >= 0; z--)
        first[grp[z]] = z;
      /* room for the new parts, after this one */
      if (!(a = realloc (sc->parts, (size_t) (sc->nparts + ng - 1) * sizeof *a)))
        return -1;
      sc->parts = a;
      memmove (&a[i + ng], &a[i + 1], (size_t) (sc->nparts - i - 1) * sizeof *a);
      sc->nparts += ng - 1;
      for (g = 1; g < ng; g++)
        {
          gs_part *q = &a[i + g];
          *q = a[i];
          q->notes = NULL, q->n = q->cap = 0;
          q->pedals = NULL, q->npedals = q->pedcap = 0;
          q->marks = NULL, q->nmarks = q->markcap = 0;
          q->lyrics = NULL, q->nlyrics = q->lyriccap = 0;
          q->joined = a[i].organ && g == ng - 1 ? GR_JOIN_BARS : GR_JOIN_BRACE;
          /* (each plays the pedal: shown under the last only) */
          for (k = 0; k < a[i].npedals; k++)
            if (gs_add_pedal (q, a[i].pedals[k].measure, a[i].pedals[k].off, a[i].pedals[k].down))
              return -1;
          if (g < ng - 1)
            q->pedal_shown = 0, q->pedal_hidden = q->npedals;
        }
      if (ng > 1)
        a[i].pedal_shown = 0, a[i].pedal_hidden = a[i].npedals;
      /* each group's clefs */
      {
        int all[GS_STAVES];
        memcpy (all, a[i].clef, sizeof all);
        for (g = 0; g < ng; g++)
          {
            int c[GS_STAVES] = { 0 };
            for (z = 0; z < nst; z++)
              if (grp[z] == g)
                c[z - first[g]] = all[z];
            memcpy (a[i + g].clef, c, sizeof c);
          }
      }
      /* the notes, marks and words to their group, on its staff there */
      {
        gs_part *p0 = &a[i];
        int nn = 0, nm = 0, nl = 0;
        for (k = 0; k < p0->n; k++)
          {
            gs_note x = p0->notes[k];
            int st = x.st < 0 ? 0 : x.st >= nst ? nst - 1 : x.st, gg = grp[st], l = st - first[gg];
            int two = gg + 1 < ng ? first[gg + 1] - first[gg] > 1 : nst - first[gg] > 1;
            x.st = l;
            x.wr = (x.wr & ~(GR_WR_STAFF | (two ? 0 : GR_WR_CROSS))) | (x.wr & GR_WR && l > 0 ? GR_WR_STAFF : 0);
            if (gg == 0)
              p0->notes[nn++] = x;
            else
              {
                gs_part *q = &a[i + gg];
                if (q->n == q->cap)
                  {
                    int cap = q->cap ? q->cap * 2 : 256;
                    gs_note *b = realloc (q->notes, (size_t) cap * sizeof *b);
                    if (!b)
                      return -1;
                    q->notes = b, q->cap = cap;
                  }
                q->notes[q->n++] = x;
              }
          }
        p0->n = nn;
        for (k = 0; k < p0->nmarks; k++)
          {
            gs_mark x = p0->marks[k];
            int st = x.kind == GR_MK_NOPEDAL ? nst - 1 : x.st < 0 ? 0 : x.st >= nst ? nst - 1 : x.st, gg = grp[st], l = st - first[gg];
            int two = gg + 1 < ng ? first[gg + 1] - first[gg] > 1 : nst - first[gg] > 1;
            x.st = l;
            if (x.kind == GR_MK_NOTE)
              x.staff = (x.staff & ~(two ? 1 : 3)) | (l > 0);
            else if (x.kind == GR_MK_ARPEGGIO || x.kind == GR_MK_TUPLET)
              x.staff = (x.staff & ~1) | (l > 0);   /* (its line's length, its ratio, above) */
            else if (x.staff >= 0)
              x.staff = l;
            if (gg == 0)
              p0->marks[nm++] = x;
            else
              {
                gs_part *q = &a[i + gg];
                if (q->nmarks == q->markcap)
                  {
                    int cap = q->markcap ? q->markcap * 2 : 64;
                    gs_mark *b = realloc (q->marks, (size_t) cap * sizeof *b);
                    if (!b)
                      return -1;
                    q->marks = b, q->markcap = cap;
                  }
                q->marks[q->nmarks++] = x;
              }
          }
        p0->nmarks = nm;
        for (k = 0; k < p0->nlyrics; k++)
          {
            gs_lyric x = p0->lyrics[k];
            int st = x.st < 0 ? 0 : x.st >= nst ? nst - 1 : x.st, gg = grp[st];
            x.st = x.staff = st - first[gg];
            if (gg == 0)
              p0->lyrics[nl++] = x;
            else
              {
                gs_part *q = &a[i + gg];
                if (q->nlyrics == q->lyriccap)
                  {
                    int cap = q->lyriccap ? q->lyriccap * 2 : 64;
                    gs_lyric *b = realloc (q->lyrics, (size_t) cap * sizeof *b);
                    if (!b)
                      return -1;
                    q->lyrics = b, q->lyriccap = cap;
                  }
                q->lyrics[q->nlyrics++] = x;
              }
          }
        p0->nlyrics = nl;
      }
      i += ng - 1;
    }
  return 0;
}

int gs_place (gr_song *s, gs_score *sc, int from, const char *kind, const char **why)
{
  int made_from = -2, joined = 0, member;
  static char msg[100];
  int *order = NULL, norder, i, made = 0, full_at = -1, dropped = 0, empty = gr_song_end (s) == 0;
  double *mstart;
  *why = NULL;
  if (!sc->nmeas || !sc->nparts)
    {
      snprintf (msg, sizeof msg, "That %s has no music in it", kind);
      *why = msg;
      return -1;
    }
  if (gs_split_staves (sc))
    {
      *why = "Not enough memory";
      return -1;
    }
  norder = play_order (sc, &order);
  if (norder <= 0 || !(mstart = malloc ((size_t) norder * sizeof *mstart)))
    {
      free (order);
      *why = "Not enough memory";
      return -1;
    }
  /* A short first measure (a pickup) is written at the end of a whole
     bar, as it's printed; every other measure starts a bar of its own. */
  mstart[0] = 0;
  {
    const gs_measure *m0 = &sc->meas[order[0]];
    double full = m0->den > 0 ? m0->num * 4.0 / m0->den : 4;
    if (m0->len > 0 && m0->len < full - 1e-9)
      mstart[0] = full - m0->len;
  }
  if (empty && from == 0)
    s->pickup = (int) floor (mstart[0] * GR_PPQ + 0.5);
  for (i = 1; i < norder; i++)
    mstart[i] = mstart[i - 1] + sc->meas[order[i - 1]].len;

  if (empty)
    {
      int any = 0;
      for (i = 0; i < sc->nparts; i++)
        any |= sc->parts[i].n > 0;
      empty = any;
    }
  if (empty)
    {
      /* The score's time signatures, at the bars where they change. A
         measure after the first that's shorter or longer than its time
         signature (a bar of 2/4 in 3/4, where the music needs it) gets
         one of its own. */
      int bar = 0, pn = 0, pd = 0, pirreg = 0;
      for (i = 0; i < norder; i++)
        {
          const gs_measure *m = &sc->meas[order[i]];
          int num = m->num, den = m->den, irreg = 0;
          if ((i > 0 || m->len > num * 4.0 / den) && den > 0 && fabs (m->len - num * 4.0 / den) > 1e-6 && m->len > 0)
            {
              irreg = 1;
              /* its length as a time signature: in its own beats if it can be */
              for (den = m->den; den <= 64; den *= 2)
                if (fabs (m->len * den / 4 - floor (m->len * den / 4 + 0.5)) < 1e-6)
                  break;
              num = (int) floor (m->len * den / 4 + 0.5);
              if (den > 64 || num < 1)
                num = m->num, den = m->den;
            }
          bar = gr_tick_bar (s, from + (int) floor ((i ? mstart[i] : 0) * GR_PPQ + 0.5));
          if (num != pn || den != pd || (m->timesym && i > 0 && sc->meas[order[i - 1]].timesym != m->timesym)
              || irreg != pirreg)
            {
              gr_set_meter (s, bar, num, den);
              gr_set_meter_symbol (s, bar, irreg ? 3 : m->timesym);
              pn = num;
              pd = den;
              pirreg = irreg;
            }
        }
      if (sc->tempo > 0)
        s->tempo = sc->tempo < 40 ? 40 : sc->tempo > 240 ? 240 : (int) floor (sc->tempo + 0.5);
      /* (whether the score writes a metronome mark for it) */
      s->tempo_hidden = sc->ntempos == 0 || sc->tempos[0].hidden;   /* (none written: none shown) */
      /* Its tempo changes, played out with the repeats. */
      s->ntempos = 0;
      for (i = 0; i < norder; i++)
        {
          int k;
          for (k = 0; k < sc->ntempos; k++)
            if (sc->tempos[k].measure == order[i])
              {
                int tk = from + (int) floor ((mstart[i] + sc->tempos[k].off) * GR_PPQ + 0.5);
                if (fabs (gr_tempo_at (s, tk) - sc->tempos[k].bpm) > 0.01)
                  {
                    int q;
                    gr_tempo_add (s, tk, sc->tempos[k].bpm);
                    for (q = 0; q < s->ntempos; q++)
                      if (s->tempos[q].tick == tk)
                        s->tempos[q].hidden = sc->tempos[k].hidden;
                  }
              }
        }
      /* Its key signatures (a score with none is in C). */
      s->nkeysigs = 0;
      {
        int f = 0, mi = 0, set = 0;
        for (i = 0; i < norder; i++)
          {
            const gs_measure *m = &sc->meas[order[i]];
            int bar = gr_tick_bar (s, from + (int) floor (mstart[i] * GR_PPQ + 0.5));
            if (m->has_key)
              f = m->fifths, mi = m->minor;
            if (!set || m->has_key)
              {
                if (!set || f != s->keysigs[s->nkeysigs - 1].fifths || mi != s->keysigs[s->nkeysigs - 1].minor)
                  gr_set_keysig (s, bar, f, mi);
                set = 1;
              }
          }
        if (s->nkeysigs)
          s->key = ((s->keysigs[0].fifths * 7 % 12 + 12) % 12 + (s->keysigs[0].minor ? 9 : 0)) % 12;
      }
      while (s->ntracks)
        gr_track_delete (s, s->ntracks - 1);
      s->nlyrics = 0;
      s->nsrcbar = 0;
    }
  /* Which measure each bar is played from (so the Score window writes
     the score's repeats, and only those, as repeats). */
  for (i = 0; i < norder; i++)
    if (gr_set_srcbar (s, gr_tick_bar (s, from + (int) floor (mstart[i] * GR_PPQ + 0.5)), order[i]))
      break;

  /* A long score in few regions, so it all fits: bars to a region by how
     many bars and parts it has and how many regions are free. */
  {
    int np = 0, nfree = 0, npfree = 0, bars, j;
    for (i = 0; i < sc->nparts; i++)
      np += sc->parts[i].n > 0;
    for (j = 0; j < GR_MAX_REGIONS; j++)
      nfree += !s->regions[j].used;
    for (j = 0; j < GR_MAX_PATTERNS; j++)
      npfree += !s->patterns[j].used;
    if (npfree < nfree)
      nfree = npfree;
    bars = gr_tick_bar (s, from + (int) floor ((mstart[norder - 1] + sc->meas[order[norder - 1]].len) * GR_PPQ + 0.5))
           - gr_tick_bar (s, from) + 1;
    if (nfree > 16 && np > 0)
      {
        /* (room for busy bars that need regions of their own) */
        long need = ((long) bars * np * 5 / 4 + nfree - 1) / nfree;
        gr_place_chunk ((int) ((need + 3) / 4 * 4));
      }
  }
  for (i = 0; i < sc->nparts && full_at < 0; i++)
    {
      gs_part *p = &sc->parts[i];
      unote *u = NULL;
      ucont *uc = NULL, *ts = NULL;   /* ts: ties after a long note's first, inside a bar */
      gr_pnote *pn = NULL;
      int nu = 0, nuc = 0, nts = 0, k, o, t, prog, last[128];
      /* (an instrument's staff with nothing on it: kept, on a track of its own,
         if it's joined to the rest of the instrument's) */
      member = p->joined || (i + 1 < sc->nparts && sc->parts[i + 1].joined);
      if (!p->n && !member)
        continue;
      /* (it goes on from the part before it, if that made a track) */
      joined = p->joined && i == made_from + 1 ? p->joined : GR_JOIN_NONE;
      /* Played out: each measure's notes, every time it's played. */
      for (o = 0; o < norder; o++)
        for (k = 0; k < p->n; k++)
          if (p->notes[k].measure == order[o])
            nu++;
      if ((!nu && !member) || !(u = malloc ((size_t) (nu + 1) * sizeof *u)) || !(pn = malloc ((size_t) (nu + 1) * sizeof *pn))
          || !(uc = malloc ((size_t) (nu + 1) * sizeof *uc)) || !(ts = malloc ((size_t) (nu + 1) * sizeof *ts)))
        {
          free (u);
          free (pn);
          free (uc);
          free (ts);
          if (nu)
            {
              *why = "Not enough memory";
              break;
            }
          continue;
        }
      nu = 0;
      for (o = 0; o < norder; o++)
        for (k = 0; k < p->n; k++)
          if (p->notes[k].measure == order[o])
            {
              u[nu].start = mstart[o] + p->notes[k].off;
              u[nu].end = u[nu].start + p->notes[k].len;
              u[nu].pitch = p->notes[k].pitch;
              u[nu].vel = p->notes[k].vel;
              u[nu].tie = p->notes[k].tie;
              u[nu].wr = p->notes[k].wr;   /* (a drum's too: as the score writes it) */
              u[nu].tie1 = 0;
              u[nu].cont = -1;
              nu++;
            }
      qsort (u, (size_t) nu, sizeof *u, cmp_unote);
      /* Tied notes: a note tied from the one before it (on the same key,
         ending where it starts) lengthens that one instead. */
      for (k = 0; k < 128; k++)
        last[k] = -1;
      {
        int w = 0;
        for (k = 0; k < nu; k++)
          {
            int l = last[u[k].pitch];
            /* (a score's note is tied from one in its own voice, on its own
               staff, moved there or not: look back for it if the last on the key is another's) */
            if (l >= 0 && (u[k].wr & GR_WR)
                && (u[l].wr & (GR_WR_VOICE | GR_WR_STAFF | GR_WR_CROSS)) != (u[k].wr & (GR_WR_VOICE | GR_WR_STAFF | GR_WR_CROSS)))
              {
                int j;
                for (j = w - 1; j >= 0 && j >= w - 64; j--)
                  if (u[j].pitch == u[k].pitch
                      && (u[j].wr & (GR_WR_VOICE | GR_WR_STAFF | GR_WR_CROSS)) == (u[k].wr & (GR_WR_VOICE | GR_WR_STAFF | GR_WR_CROSS))
                      && fabs ((u[j].wr & GR_WR_HALF ? 2 * u[j].end - u[j].start : u[j].end) - u[k].start) < 1.0 / 64)
                    break;
                if (j >= 0 && j >= w - 64)
                  l = j;
              }
            /* (where it ends as written: a staccato note plays for half that) */
            double wend = l < 0 ? 0 : u[l].wr & GR_WR_HALF ? 2 * u[l].end - u[l].start : u[l].end;
            /* A tie on to a later note, not the next (as from the notes of
               an arpeggio to the chord they ring into): both written
               tied, each played */
            if (l >= 0 && (u[l].tie & GS_TIE_FWD) && (u[k].tie & GS_TIE_BACK) && (u[k].wr & GR_WR)
                && wend < u[k].start - 1.0 / 64 && u[k].start - wend <= 4 && (u[l].tie1 == 0 || u[l].tie1 == GR_TIE_IN))
              {
                u[l].tie1 = u[l].tie1 == GR_TIE_IN ? GR_TIE_BOTH : GR_TIE_OUT;
                u[l].tie &= ~GS_TIE_FWD;
                u[k].tie1 = GR_TIE_IN;
                u[k].tie &= ~GS_TIE_BACK;
              }
            if (l >= 0 && (u[l].tie & GS_TIE_FWD || u[k].tie & GS_TIE_BACK)
                && fabs (wend - u[k].start) < 1.0 / 64)
              {
                /* (where the first is written to end; a tie at a barline
                   needs no note of it, so the next one inside a bar) */
                if (u[l].tie1 == 0 && !on_barline (mstart, norder, wend))
                  u[l].tie1 = wend - u[l].start;
                else if (u[l].tie1 > 0 && !on_barline (mstart, norder, wend) && u[l].cont < 0 && (u[l].wr & GR_WR) && nts < nu)
                  {
                    /* (a third note tied on, or more: where, as a mark) */
                    ts[nts].start = wend;
                    ts[nts].end = wend;
                    ts[nts].pitch = u[l].pitch;
                    ts[nts++].wr = u[l].wr;
                  }
                /* (tied on into another voice: written there from here) */
                {
                  int vmask = GR_WR_VOICE | GR_WR_STAFF | GR_WR_CROSS;
                  int cw = u[l].cont >= 0 ? uc[u[l].cont].wr : u[l].wr;
                  if ((u[l].wr & GR_WR) && (u[k].wr & GR_WR) && (cw & vmask) != (u[k].wr & vmask))
                    {
                      uc[nuc].start = u[k].start;
                      uc[nuc].end = u[k].end;
                      uc[nuc].pitch = u[k].pitch;
                      uc[nuc].wr = u[k].wr;
                      u[l].cont = nuc++;
                    }
                  else if (u[l].cont >= 0)
                    uc[u[l].cont].end = u[k].end;
                }
                u[l].end = u[k].end;
                u[l].tie = u[k].tie;
                u[l].wr &= ~GR_WR_HALF;   /* (no longer half its whole length) */
                continue;
              }
            u[w] = u[k];
            last[u[w].pitch] = w;
            w++;
          }
        nu = w;
      }
      for (k = 0; k < nu; k++)
        {
          pn[k].start = (int) floor (u[k].start * GR_PPQ + 0.5);
          pn[k].end = (int) floor (u[k].end * GR_PPQ + 0.5);
          if (pn[k].end <= pn[k].start)
            pn[k].end = pn[k].start + 1;
          pn[k].key = u[k].pitch;
          pn[k].vel = u[k].vel;
          pn[k].wr = u[k].wr;
          pn[k].tie1 = (int) floor ((u[k].start + u[k].tie1) * GR_PPQ + 0.5) - pn[k].start;
          if (u[k].tie1 <= 0 || pn[k].tie1 <= 0 || pn[k].tie1 >= pn[k].end - pn[k].start)
            pn[k].tie1 = 0;
          if (u[k].tie1 < 0)
            pn[k].tie1 = (int) u[k].tie1;      /* (GR_TIE_OUT, GR_TIE_IN: to or from a later or earlier note) */
        }
      free (u);
      qsort (pn, (size_t) nu, sizeof *pn, cmp_pnote);
      prog = p->program < 0 ? 0 : p->program & 127;
      t = gr_track_add (s, p->drums ? TRK_DRUMS : TRK_INST,
                        p->name[0] ? p->name : p->drums ? "Drums" : gr_gm_program_name (prog),
                        p->drums ? KIT_STUDIO : gs_nearest_patch (prog));
      if (t < 0)
        {
          snprintf (msg, sizeof msg, "Groove has room for %d tracks, so some parts weren't imported",
                    GR_MAX_TRACKS);
          *why = msg;
          free (pn);
          free (uc);
          free (ts);
          break;
        }
      s->tracks[t].gm = p->drums ? 0 : prog;
      if (!p->drums)
        {
          /* how the score writes it (else Groove goes by its sound) */
          s->tracks[t].written = 1;
          s->tracks[t].wtranspose = p->has_transpose ? p->transpose : 0;
          s->tracks[t].wclef[0] = p->clef[0];
          s->tracks[t].wclef[1] = p->clef[1];   /* (a third staff's: on a part of its own, joined) */
          s->tracks[t].wpedal = p->pedal_hidden > 0 && p->pedal_shown == 0;
          if (!p->has_transpose)
            s->tracks[t].written = 2;     /* (its transposition's not known) */
        }
      s->tracks[t].joined = joined;
      memcpy (s->tracks[t].drumline, p->drumline, sizeof p->drumline);
      s->tracks[t].wlines = p->drums && p->lines == 1 ? 1 : 0;
      made_from = i;
      s->tracks[t].channel = p->channel;
      s->tracks[t].bank_msb = p->bank_msb;
      s->tracks[t].bank_lsb = p->bank_lsb;
      made++;
      /* (and the notes it writes but doesn't play, where it plays none: a
         region for them to be written in) */
      {
        int na = 0;
        gr_pnote *pa;
        for (o = 0; o < norder; o++)
          for (k = 0; k < p->nmarks; k++)
            na += p->marks[k].measure == order[o] && p->marks[k].kind == GR_MK_NOTE;
        if (na && (pa = realloc (pn, (size_t) (nu + na) * sizeof *pn)))
          {
            pn = pa;
            for (o = 0; o < norder; o++)
              for (k = 0; k < p->nmarks; k++)
                if (p->marks[k].measure == order[o] && p->marks[k].kind == GR_MK_NOTE)
                  {
                    gr_pnote *x = &pn[nu++];
                    memset (x, 0, sizeof *x);
                    x->start = (int) floor ((mstart[o] + p->marks[k].off) * GR_PPQ + 0.5);
                    x->end = x->start + 1;
                    x->key = -1;
                  }
            qsort (pn, (size_t) nu, sizeof *pn, cmp_pnote);
          }
      }
      if (gr_place_notes (s, t, pn, nu, from, &full_at, &dropped))
        {
          snprintf (msg, sizeof msg, "The song is full: the %s was imported up to bar %d",
                    kind, gr_tick_bar (s, full_at) + 1);
          *why = msg;
        }
      free (pn);
      /* The words, played out like the notes (each verse every time). */
      for (o = 0; o < norder && full_at < 0; o++)
        for (k = 0; k < p->nlyrics; k++)
          if (p->lyrics[k].measure == order[o])
            gr_lyric_add (s, t, from + (int) floor ((mstart[o] + p->lyrics[k].off) * GR_PPQ + 0.5),
                          p->lyrics[k].staff, p->lyrics[k].verse, p->lyrics[k].syllabic, p->lyrics[k].text);
      /* The sustain pedal, played out like the notes, as controller 64. */
      if (p->npedals && full_at < 0)
        {
          gr_pctl *c;
          int nc = 0;
          for (o = 0; o < norder; o++)
            for (k = 0; k < p->npedals; k++)
              if (p->pedals[k].measure == order[o])
                nc++;
          if (nc && (c = malloc ((size_t) nc * sizeof *c)))
            {
              nc = 0;
              for (o = 0; o < norder; o++)
                for (k = 0; k < p->npedals; k++)
                  if (p->pedals[k].measure == order[o])
                    {
                      c[nc].tick = (int) floor ((mstart[o] + p->pedals[k].off) * GR_PPQ + 0.5);
                      c[nc].type = 64;
                      c[nc].value = p->pedals[k].down ? 127 : 0;
                      nc++;
                    }
              qsort (c, (size_t) nc, sizeof *c, cmp_pctl);
              /* let up and straight down again: up a 64th note before
                 (a pattern holds one value a tick) */
              for (k = 0; k + 1 < nc; k++)
                if (c[k].tick == c[k + 1].tick && !c[k].value && c[k + 1].value
                    && c[k].tick >= GR_PPQ / 16 && (k == 0 || c[k - 1].tick < c[k].tick - GR_PPQ / 16))
                  c[k].tick -= GR_PPQ / 16;
              gr_place_ctls (s, t, c, nc, from, &full_at, &dropped);
              free (c);
            }
        }
      /* The written marks, played out too: one that spans ends where its
         end is next played. */
      if ((p->nmarks || nuc || nts) && full_at < 0)
        {
          gr_pmark *pm;
          int nm = nuc + nts;
          for (o = 0; o < norder; o++)
            for (k = 0; k < p->nmarks; k++)
              if (p->marks[k].measure == order[o])
                nm++;
          if (nm && (pm = malloc ((size_t) nm * sizeof *pm)))
            {
              nm = 0;
              for (o = 0; o < norder; o++)
                for (k = 0; k < p->nmarks; k++)
                  {
                    const gs_mark *g = &p->marks[k];
                    int o2 = o;
                    if (g->measure != order[o])
                      continue;
                    if (g->emeasure >= 0)
                      {
                        while (o2 < norder && order[o2] != g->emeasure)
                          o2++;
                        if (o2 == norder)
                          continue;
                      }
                    pm[nm].tick = (int) floor ((mstart[o] + g->off) * GR_PPQ + 0.5);
                    pm[nm].len = g->emeasure < 0 ? 0
                      : (int) floor ((mstart[o2] + g->eoff) * GR_PPQ + 0.5) - pm[nm].tick;
                    if (pm[nm].len < 0)
                      continue;
                    pm[nm].kind = g->kind;
                    pm[nm].value = g->value;
                    pm[nm].pitch = g->pitch;
                    if (g->kind == GR_MK_ARPEGGIO || g->kind == GR_MK_GRACE)
                      pm[nm].len = g->hi;      /* (its top note, for a chord spread; a grace note's place) */
                    pm[nm].staff = g->staff;
                    nm++;
                  }
              /* notes tied on into another voice: written there */
              for (k = 0; k < nuc; k++)
                {
                  pm[nm].tick = (int) floor (uc[k].start * GR_PPQ + 0.5);
                  pm[nm].len = (int) floor (uc[k].end * GR_PPQ + 0.5) - pm[nm].tick;
                  pm[nm].kind = GR_MK_NOTE;
                  pm[nm].value = (uc[k].wr & GR_WR_VOICE) | ((uc[k].wr >> 8) & 0x1f) << 2;
                  pm[nm].pitch = uc[k].pitch;
                  pm[nm].staff = gr_mknote_staff (uc[k].wr, 1);
                  if (pm[nm].len > 0)
                    nm++;
                }
              /* a long note's ties after its first, inside a bar */
              for (k = 0; k < nts; k++)
                {
                  pm[nm].tick = (int) floor (ts[k].start * GR_PPQ + 0.5);
                  pm[nm].len = 0;
                  pm[nm].kind = GR_MK_TIE;
                  pm[nm].value = ts[k].wr & GR_WR_VOICE;
                  pm[nm].pitch = ts[k].pitch;
                  pm[nm].staff = (ts[k].wr & GR_WR_STAFF) != 0;
                  nm++;
                }
              {
                int lost = 0;     /* (a pattern full of marks: they're left out, not reported) */
                gr_place_marks (s, t, pm, nm, from, &lost);
              }
              free (pm);
            }
        }
      free (uc);
      free (ts);
    }
  free (order);
  free (mstart);
  if (!*why && dropped)
    {
      snprintf (msg, sizeof msg, "%d notes didn't fit and were left out", dropped);
      *why = msg;
    }
  if (!made && !*why)
    {
      snprintf (msg, sizeof msg, "That %s has no notes", kind);
      *why = msg;
    }
  gr_place_chunk (0);
  return made ? made : -1;
}

/* A written note value in quarter notes, with its dots. */
double gs_note_value (const char *type, int dots)
{
  static const struct { const char *n; double q; } v[] = {
    { "long", 16 }, { "breve", 8 }, { "whole", 4 }, { "half", 2 }, { "quarter", 1 },
    { "eighth", 0.5 }, { "16th", 0.25 }, { "32nd", 0.125 }, { "64th", 0.0625 },
    { "128th", 0.03125 }, { "256th", 0.015625 }, { "512th", 0.0078125 }, { "1024th", 0.00390625 } };
  int i;
  for (i = 0; i < (int) (sizeof v / sizeof v[0]); i++)
    if (!strcmp (type, v[i].n))
      {
        double q = v[i].q, add = q / 2;
        while (dots-- > 0)
          {
            q += add;
            add /= 2;
          }
        return q;
      }
  return 0;
}

int gs_gm_by_name (const char *name)
{
  int i;
  if (!name || !*name)
    return -1;
  for (i = 0; i < 128; i++)
    {
      const char *g = gr_gm_program_name (i), *a = name;
      while (*a && *g && (*a | 32) == (*g | 32))
        a++, g++;
      if (!*a && !*g)
        return i;
    }
  return -1;
}

/* ---- files -------------------------------------------------------------------- */

static unsigned char *read_file (const char *path, size_t *n)
{
  FILE *f = fopen (path, "rb");
  unsigned char *d = NULL;
  long size;
  if (!f)
    return NULL;
  fseek (f, 0, SEEK_END);
  size = ftell (f);
  fseek (f, 0, SEEK_SET);
  if (size >= 0 && size <= 32L * 1024 * 1024 && (d = malloc ((size_t) size + 1))
      && fread (d, 1, (size_t) size, f) == (size_t) size)
    {
      fclose (f);
      *n = (size_t) size;
      return d;
    }
  fclose (f);
  free (d);
  return NULL;
}

/* A UTF-16 file (it starts with its byte-order mark) as UTF-8, in a new
   buffer with room for a 0; d is freed. Anything else is left as it is. */
static unsigned char *from_utf16 (unsigned char *d, size_t *n)
{
  int be;
  size_t i, o = 0;
  unsigned char *u;
  size_t skip = 2;
  if (*n >= 2 && d[0] == '<' && d[1] == 0)
    be = 0, skip = 0;                    /* no byte-order mark */
  else if (*n >= 2 && d[0] == 0 && d[1] == '<')
    be = 1, skip = 0;
  else if (*n < 2 || !((d[0] == 0xFF && d[1] == 0xFE) || (d[0] == 0xFE && d[1] == 0xFF)))
    return d;
  else
    be = d[0] == 0xFE;
  u = malloc (*n / 2 * 3 + 4);
  if (!u)
    {
      free (d);
      return NULL;
    }
  for (i = skip; i + 1 < *n; i += 2)
    {
      unsigned long c = be ? (unsigned) (d[i] << 8 | d[i + 1]) : (unsigned) (d[i + 1] << 8 | d[i]);
      if (c >= 0xD800 && c < 0xDC00 && i + 3 < *n)
        {
          unsigned long lo = be ? (unsigned) (d[i + 2] << 8 | d[i + 3]) : (unsigned) (d[i + 3] << 8 | d[i + 2]);
          if (lo >= 0xDC00 && lo < 0xE000)
            {
              c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
              i += 2;
            }
        }
      if (c < 0x80)
        u[o++] = (unsigned char) c;
      else if (c < 0x800)
        {
          u[o++] = (unsigned char) (0xC0 | (c >> 6));
          u[o++] = (unsigned char) (0x80 | (c & 0x3F));
        }
      else if (c < 0x10000)
        {
          u[o++] = (unsigned char) (0xE0 | (c >> 12));
          u[o++] = (unsigned char) (0x80 | ((c >> 6) & 0x3F));
          u[o++] = (unsigned char) (0x80 | (c & 0x3F));
        }
      else
        {
          u[o++] = (unsigned char) (0xF0 | (c >> 18));
          u[o++] = (unsigned char) (0x80 | ((c >> 12) & 0x3F));
          u[o++] = (unsigned char) (0x80 | ((c >> 6) & 0x3F));
          u[o++] = (unsigned char) (0x80 | (c & 0x3F));
        }
    }
  free (d);
  *n = o;
  return u;
}

static int ends_with (const char *s, const char *tail)
{
  size_t a = strlen (s), b = strlen (tail);
  size_t i;
  if (a < b)
    return 0;
  for (i = 0; i < b; i++)
    {
      char c = s[a - b + i];
      if (c >= 'A' && c <= 'Z')
        c = (char) (c - 'A' + 'a');
      if (c != tail[i])
        return 0;
    }
  return 1;
}

/* Of an XML file's start: which kind of score (MSCX or MUSICXML), or NONE. */
static int xml_kind (const unsigned char *d, size_t n)
{
  size_t i, lim = n < 4096 ? n : 4096;
  for (i = 0; i + 12 < lim; i++)
    if (d[i] == '<')
      {
        if (!memcmp (d + i + 1, "museScore", 9))
          return GR_SCORE_MSCX;
        if (!memcmp (d + i + 1, "score-partwise", 14) || !memcmp (d + i + 1, "score-timewise", 14))
          return GR_SCORE_MUSICXML;
      }
  return GR_SCORE_NONE;
}

/* The score inside a zip: MuseScore's main .mscx (as its container.xml
   says, or the first that isn't a part's), or MusicXML's root file. */
static int zip_score (const unsigned char *d, size_t n, gr_zip_entry *which)
{
  gr_zip_entry e[64];
  int k = gr_zip_names (d, n, e, 64), i, found = GR_SCORE_NONE;
  if (k <= 0)
    return GR_SCORE_NONE;
  for (i = 0; i < k; i++)
    if (!strcmp (e[i].name, "META-INF/container.xml"))
      {
        size_t len;
        char *c = (char *) gr_zip_read (d, n, &e[i], &len), *fp;
        if (c)
          {
            c[len] = 0;
            /* (MuseScore 4 lists its style, thumbnail and settings files
               too, the style first: take the first that's a score) */
            for (fp = strstr (c, "full-path=\""); fp && !found; fp = strstr (fp, "full-path=\""))
              {
                char *name = fp + 11, *q = strchr (name, '"');
                int j;
                if (!q)
                  break;
                *q = 0;
                fp = q + 1;
                if (!ends_with (name, ".mscx") && !ends_with (name, ".musicxml") && !ends_with (name, ".xml"))
                  continue;
                for (j = 0; j < k; j++)
                  if (!strcmp (e[j].name, name))
                    {
                      *which = e[j];
                      found = ends_with (e[j].name, ".mscx") ? GR_SCORE_MSCZ : GR_SCORE_MXL;
                    }
              }
            free (c);
          }
      }
  if (found)
    return found;
  for (i = 0; i < k; i++)
    if (ends_with (e[i].name, ".mscx") && strncmp (e[i].name, "Excerpts/", 9))
      {
        *which = e[i];
        return GR_SCORE_MSCZ;
      }
  for (i = 0; i < k; i++)
    if ((ends_with (e[i].name, ".musicxml") || ends_with (e[i].name, ".xml"))
        && strncmp (e[i].name, "META-INF/", 9))
      {
        *which = e[i];
        return GR_SCORE_MXL;
      }
  return GR_SCORE_NONE;
}

int gr_score_kind (const char *path)
{
  FILE *f = fopen (path, "rb");
  unsigned char head[4096];
  size_t n;
  int kind = GR_SCORE_NONE;
  if (!f)
    return GR_SCORE_NONE;
  n = fread (head, 1, sizeof head, f);
  fclose (f);
  if (n >= 4 && !memcmp (head, "PK\3\4", 4))
    {
      size_t all;
      unsigned char *d = read_file (path, &all);
      gr_zip_entry e;
      if (d)
        {
          kind = zip_score (d, all, &e);
          free (d);
        }
      return kind;
    }
  if (n >= 2 && ((head[0] == 0xFF && head[1] == 0xFE) || (head[0] == 0xFE && head[1] == 0xFF)
                 || (head[0] == '<' && head[1] == 0) || (head[0] == 0 && head[1] == '<')))
    {
      unsigned char *c = malloc (n + 1);
      if (!c)
        return GR_SCORE_NONE;
      memcpy (c, head, n);
      c = from_utf16 (c, &n);
      if (!c)
        return GR_SCORE_NONE;
      kind = xml_kind (c, n);
      free (c);
      return kind;
    }
  return xml_kind (head, n);
}

int gr_score_import (gr_song *s, const char *path, int tick, const char **why)
{
  size_t n;
  unsigned char *d = read_file (path, &n);
  int kind, r;
  gs_score sc;
  const char *what;
  *why = NULL;
  if (!d)
    {
      *why = "Groove couldn't open that file";
      return -1;
    }
  kind = GR_SCORE_NONE;
  if (n >= 4 && !memcmp (d, "PK\3\4", 4))
    {
      gr_zip_entry e;
      size_t len;
      unsigned char *x;
      kind = zip_score (d, n, &e);
      if (kind == GR_SCORE_NONE)
        {
          free (d);
          *why = "That zip file has no MuseScore or MusicXML score in it";
          return -1;
        }
      x = gr_zip_read (d, n, &e, &len);
      free (d);
      if (!x)
        {
          *why = "Groove couldn't unpack the score from that file";
          return -1;
        }
      d = realloc (x, len + 1);
      if (!d)
        {
          free (x);
          *why = "Not enough memory";
          return -1;
        }
      n = len;
    }
  if (!(d = from_utf16 (d, &n)))
    {
      *why = "Not enough memory";
      return -1;
    }
  if (kind == GR_SCORE_NONE)
    kind = xml_kind (d, n);
  gs_init (&sc);
  if (kind == GR_SCORE_MSCX || kind == GR_SCORE_MSCZ)
    {
      what = "MuseScore file";
      r = gs_read_mscx (&sc, (char *) d, n, why);
    }
  else if (kind == GR_SCORE_MUSICXML || kind == GR_SCORE_MXL)
    {
      what = "MusicXML file";
      r = gs_read_musicxml (&sc, (char *) d, n, why);
    }
  else
    {
      free (d);
      *why = "That isn't a MuseScore or MusicXML file";
      return -1;
    }
  free (d);
  if (r == 0)
    r = gs_place (s, &sc, tick, what, why);
  gs_free (&sc);
  return r;
}

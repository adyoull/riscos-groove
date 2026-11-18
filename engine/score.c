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
    }
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

static int cmp_pctl (const void *a, const void *b)
{
  const gr_pctl *x = a, *y = b;
  if (x->tick != y->tick)
    return x->tick < y->tick ? -1 : 1;
  return x->value - y->value;   /* (up before down at the same moment) */
}

int gs_add_note (gs_part *p, int measure, double off, double len, int pitch, int vel, int tie)
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
  n->measure = measure;
  n->off = off;
  n->len = len;
  n->pitch = pitch;
  n->vel = vel < 1 ? 1 : vel > 127 ? 127 : vel;
  n->tie = tie;
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
            sc->meas[k].len = sc->meas[k].num * 4.0 / sc->meas[k].den;
          }
      sc->nmeas = i + 1;
    }
  return &sc->meas[i];
}

/* The order the measures are played in, repeats and voltas played out.
   Returns how many (in *order, to free), or -1. */
static int play_order (const gs_score *sc, int **order)
{
  int cap = sc->nmeas * 2 + 16, n = 0, i = 0, start = 0, pass = 1, guard = 0;
  int *o = malloc ((size_t) cap * sizeof *o), *count = calloc ((size_t) sc->nmeas + 1, sizeof *count);
  if (!o || !count)
    {
      free (o);
      free (count);
      return -1;
    }
  while (i < sc->nmeas && guard++ < 100000)
    {
      const gs_measure *m = &sc->meas[i];
      if (m->start_repeat && pass == 1)
        start = i;
      if (m->endings && pass <= 32 && !(m->endings & (1u << (pass - 1))))
        {
          /* Not this time round: past this volta. On the last pass, the
             volta that has no bit for it is skipped like the others. */
          i++;
          continue;
        }
      if (n == cap)
        {
          int *a = realloc (o, (size_t) (cap *= 2) * sizeof *a);
          if (!a)
            break;
          o = a;
        }
      o[n++] = i;
      if (m->end_repeat > 0)
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
      else if (m->endings && pass > 1 && (i + 1 >= sc->nmeas || !sc->meas[i + 1].endings))
        {
          pass = 1;                  /* the last ending is over */
          start = i + 1;
        }
      i++;
    }
  free (count);
  *order = o;
  return n;
}

static int cmp_pnote (const void *a, const void *b)
{
  const gr_pnote *x = a, *y = b;
  if (x->start != y->start)
    return x->start < y->start ? -1 : 1;
  return x->key - y->key;
}

typedef struct { double start, end; int pitch, vel, tie; } unote;

static int cmp_unote (const void *a, const void *b)
{
  const unote *x = a, *y = b;
  if (x->start != y->start)
    return x->start < y->start ? -1 : 1;
  return x->pitch - y->pitch;
}

int gs_place (gr_song *s, gs_score *sc, int from, const char *kind, const char **why)
{
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
  norder = play_order (sc, &order);
  if (norder <= 0 || !(mstart = malloc ((size_t) norder * sizeof *mstart)))
    {
      free (order);
      *why = "Not enough memory";
      return -1;
    }
  mstart[0] = 0;
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
         short measure (a pickup) still takes a whole bar of Groove's. */
      int bar = 0, pn = 0, pd = 0;
      double at = 0;
      for (i = 0; i < norder; i++)
        {
          const gs_measure *m = &sc->meas[order[i]];
          int tk = from + (int) floor (at * GR_PPQ + 0.5);
          bar = gr_tick_bar (s, tk);
          if (m->num != pn || m->den != pd)
            {
              if (gr_bar_tick (s, bar) != tk)
                bar++;
              gr_set_meter (s, bar, m->num, m->den);
              pn = m->num;
              pd = m->den;
            }
          at += m->len;
        }
      if (sc->tempo > 0)
        s->tempo = sc->tempo < 40 ? 40 : sc->tempo > 240 ? 240 : (int) floor (sc->tempo + 0.5);
      while (s->ntracks)
        gr_track_delete (s, s->ntracks - 1);
    }

  for (i = 0; i < sc->nparts && full_at < 0; i++)
    {
      gs_part *p = &sc->parts[i];
      unote *u = NULL;
      gr_pnote *pn = NULL;
      int nu = 0, k, o, t, prog, last[128];
      if (!p->n)
        continue;
      /* Played out: each measure's notes, every time it's played. */
      for (o = 0; o < norder; o++)
        for (k = 0; k < p->n; k++)
          if (p->notes[k].measure == order[o])
            nu++;
      if (!nu || !(u = malloc ((size_t) nu * sizeof *u)) || !(pn = malloc ((size_t) nu * sizeof *pn)))
        {
          free (u);
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
            if (l >= 0 && (u[l].tie & GS_TIE_FWD || u[k].tie & GS_TIE_BACK)
                && fabs (u[l].end - u[k].start) < 1.0 / 64)
              {
                u[l].end = u[k].end;
                u[l].tie = u[k].tie;
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
          break;
        }
      s->tracks[t].gm = p->drums ? 0 : prog;
      s->tracks[t].channel = p->channel;
      s->tracks[t].bank_msb = p->bank_msb;
      s->tracks[t].bank_lsb = p->bank_lsb;
      made++;
      if (gr_place_notes (s, t, pn, nu, from, &full_at, &dropped))
        {
          snprintf (msg, sizeof msg, "The song is full: the %s was imported up to bar %d",
                    kind, gr_tick_bar (s, full_at) + 1);
          *why = msg;
        }
      free (pn);
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
            fp = strstr (c, "full-path=\"");
            if (fp)
              {
                char *q = strchr (fp + 11, '"');
                int j;
                if (q)
                  *q = 0;
                for (j = 0; j < k; j++)
                  if (!strcmp (e[j].name, fp + 11))
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

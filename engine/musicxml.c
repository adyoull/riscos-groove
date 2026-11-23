/* Groove - MusicXML: reading scores (partwise MusicXML 1.0 to 4.0, as
   MuseScore, Sibelius, Dorico and Finale write it), and writing the song
   as one (musicxml_out.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include "groove.h"
#include "score.h"
#include "notation.h"
#include "xml.h"

#define MAX_INSTR 64

typedef struct
{
  char id[64];
  int unpitched;                /* MIDI key, or -1 */
  int channel, program;         /* -1 if not given */
  char name[48];                /* midi-name */
} instr;

typedef struct
{
  char id[64];
  char name[GR_NAME];
  instr in[MAX_INSTR];
  int nin;
} part_info;

static instr *find_instr (part_info *pi, const char *id)
{
  int i;
  if (!id)
    return NULL;
  for (i = 0; i < pi->nin; i++)
    if (!strcmp (pi->in[i].id, id))
      return &pi->in[i];
  if (pi->nin < MAX_INSTR)
    {
      instr *in = &pi->in[pi->nin++];
      memset (in, 0, sizeof *in);
      snprintf (in->id, sizeof in->id, "%s", id);
      in->unpitched = in->channel = in->program = -1;
      return in;
    }
  return NULL;
}

static void read_part_list (gx_node *pl, part_info **out, int *n)
{
  gx_node *sp;
  for (sp = gx_child (pl, "score-part"); sp; sp = gx_next (sp, "score-part"))
    {
      part_info *a = realloc (*out, (size_t) (*n + 1) * sizeof *a), *pi;
      gx_node *mi;
      const char *nm;
      if (!a)
        return;
      *out = a;
      pi = &a[(*n)++];
      memset (pi, 0, sizeof *pi);
      snprintf (pi->id, sizeof pi->id, "%s", gx_attr (sp, "id") ? gx_attr (sp, "id") : "");
      nm = gx_text (sp, "part-name");
      if (!*nm || !strcmp (nm, "MusicXML Part"))
        nm = gx_text (sp, "part-abbreviation");
      snprintf (pi->name, sizeof pi->name, "%s", nm);
      for (mi = gx_child (sp, "midi-instrument"); mi; mi = gx_next (mi, "midi-instrument"))
        {
          instr *in = find_instr (pi, gx_attr (mi, "id") ? gx_attr (mi, "id") : "");
          if (!in)
            continue;
          snprintf (in->name, sizeof in->name, "%s", gx_text (mi, "midi-name"));
          if (gx_child (mi, "midi-channel"))
            in->channel = gx_int (mi, "midi-channel", 1) - 1;
          if (gx_child (mi, "midi-program"))
            in->program = (gx_int (mi, "midi-program", 1) - 1) & 127;
          /* (midi-bank isn't used: files give it for the program, and
             a SoundFont has no sound in most banks) */
          if (gx_child (mi, "midi-unpitched"))
            in->unpitched = gx_int (mi, "midi-unpitched", 1) - 1;
        }
    }
}

static int step_semitone (char c)
{
  switch (c)
    {
    case 'C': return 0;
    case 'D': return 2;
    case 'E': return 4;
    case 'F': return 5;
    case 'G': return 7;
    case 'A': return 9;
    case 'B': return 11;
    }
  return -100;
}

/* A drum written at a staff position, when the file doesn't say which
   MIDI key it is: MuseScore's standard drum kit (line 0 is the top line,
   F5, counting down in steps), matching the notehead where it can. */
static int drum_at (const char *step, int octave, const char *head)
{
  static const struct { signed char line; const char *head; unsigned char key; } kit[] = {
    { 8, "normal", 35 }, { 7, "normal", 36 }, { 3, "slashed1", 37 }, { 3, "normal", 38 },
    { 3, "slash", 40 }, { 6, "normal", 41 }, { -1, "cross", 42 }, { 5, "normal", 43 },
    { 9, "cross", 44 }, { 4, "normal", 45 }, { -1, "xcircle", 46 }, { 2, "normal", 47 },
    { 1, "normal", 48 }, { -2, "cross", 49 }, { 0, "normal", 50 }, { 0, "cross", 51 },
    { -3, "normal", 52 }, { 0, "diamond", 53 }, { 1, "diamond", 54 }, { -4, "cross", 55 },
    { 1, "triangle-down", 56 }, { -3, "cross", 57 }, { 2, "cross", 59 } };
  int pos = (octave - 4) * 7 + (int) (strchr ("CDEFGAB", *step ? *step : 'C') - "CDEFGAB");
  int line = 10 - pos, i, any = -1;          /* F5 (pos 10) is line 0 */
  const char *h = !head || !*head || !strcmp (head, "normal") ? "normal"
    : !strcmp (head, "x") || !strcmp (head, "cross") ? "cross"
    : !strcmp (head, "circle-x") ? "xcircle"
    : !strcmp (head, "inverted triangle") ? "triangle-down" : head;
  for (i = 0; i < (int) (sizeof kit / sizeof kit[0]); i++)
    if (kit[i].line == line)
      {
        if (!strcmp (kit[i].head, h))
          return kit[i].key;
        if (any < 0 || !strcmp (kit[i].head, "normal"))
          any = kit[i].key;
      }
  return any >= 0 ? any : 38;
}

/* Slurs, hairpins and 8va lines are written as a start and a stop, matched
   by kind and number: those started and not yet stopped. */
#define MAX_OPEN 32
typedef struct { int kind, number, staff, measure, value; double off; } ospan;

static int span (ospan *open, int *nopen, gs_part *gp, int kind, int value, const gx_node *x,
                 int m, double at, int staff, int is_start, int is_stop)
{
  int number = gx_attr (x, "number") ? atoi (gx_attr (x, "number")) : 1, i;
  if (is_start)
    {
      if (*nopen == MAX_OPEN)
        memmove (open, open + 1, --*nopen * sizeof *open);   /* (the oldest is forgotten) */
      open[*nopen].kind = kind;
      open[*nopen].number = number;
      open[*nopen].staff = staff;
      open[*nopen].measure = m;
      open[*nopen].off = at;
      open[*nopen].value = value;
      ++*nopen;
      return 0;
    }
  if (!is_stop)
    return 0;
  for (i = *nopen - 1; i >= 0; i--)
    if (open[i].kind == kind && open[i].number == number)
      {
        ospan o = open[i];
        memmove (open + i, open + i + 1, (size_t) (*nopen - i - 1) * sizeof *open);
        --*nopen;
        gp->cur_staff = o.staff < 0 ? 0 : o.staff;
        return gs_add_mark (gp, o.measure, o.off, m, at, kind, o.value, 0, o.staff);
      }
  return 0;
}

static int count_dots (const gx_node *e)
{
  const gx_node *d;
  int n = 0;
  for (d = gx_child (e, "dot"); d && n < 3; d = gx_next (d, "dot"))
    n++;
  return n;
}

/* A tuplet bracket started and not yet stopped, in a voice. */
typedef struct { int m, act, norm, nv, staff, show; double at; } tupopen;

/* A note value's name as NV_* (whole 0 .. 128th 7), or -1. */
static int nv_index (const char *t)
{
  static const char *const names[] = { "whole", "half", "quarter", "eighth", "16th", "32nd", "64th", "128th" };
  int i;
  for (i = 0; t && i < 8; i++)
    if (!strcmp (t, names[i]))
      return i;
  return -1;
}

/* A rest's tuplet, for its mark (gr_rest_tuplet). */
static int rest_tuplet (const gx_node *e, const gs_measure *gm)
{
  const gx_node *tmn = gx_child (e, "time-modification");
  const char *nty;
  double bq;
  if (!tmn)
    return 0;
  nty = gx_child (tmn, "normal-type") ? gx_text (tmn, "normal-type") : gx_text (e, "type");
  bq = gm->den > 0 ? (gm->num % 3 == 0 && gm->num > 3 && gm->den >= 8 ? 12.0 : 4.0) / gm->den : 1;
  return gr_rest_tuplet (gr_tuplet_bits (gx_int (tmn, "actual-notes", 0), gx_int (tmn, "normal-notes", 0),
                                         gx_int (tmn, "normal-notes", 0) * gs_note_value (nty, 0), bq));
}

/* A <time>'s beats ("3+2" adds up) and beat type: 0 if it has none. */
static int time_sig (const gx_node *t, int *num, int *den)
{
  const char *b;
  *num = 0;
  *den = t ? gx_int (t, "beat-type", 4) : 4;
  if (!t || !gx_child (t, "beats"))
    return 0;
  for (b = gx_text (t, "beats"); *b; )
    {
      char *end;
      long v = strtol (b, &end, 10);
      if (end == b)
        b++;
      else
        {
          *num += (int) v;
          b = end;
        }
    }
  return *num > 0 && *den > 0;
}

/* Each measure's longest time signature, of all the parts: where one
   part's own is shorter (a staff with its own time signature, as
   MuseScore writes it: a voice's 4/4 over the piano's 12/8), that part
   is stretched to fill the bar. */
typedef struct { double len; int num, den; } mlen;

static mlen *measure_lengths (gx_node *root, int *nm)
{
  gx_node *part, *meas, *e;
  mlen *a = NULL;
  int cap = 0;
  *nm = 0;
  for (part = gx_child (root, "part"); part; part = gx_next (part, "part"))
    {
      int m = 0, num = 0, den = 4;
      for (meas = gx_child (part, "measure"); meas; meas = gx_next (meas, "measure"), m++)
        {
          for (e = gx_child (meas, "attributes"); e; e = gx_next (e, "attributes"))
            {
              int n2, d2;
              if (time_sig (gx_child (e, "time"), &n2, &d2))
                num = n2, den = d2;
            }
          if (m >= cap)
            {
              int c2 = cap ? cap * 2 : 256, i;
              mlen *b = realloc (a, (size_t) c2 * sizeof *b);
              if (!b)
                {
                  free (a);
                  *nm = 0;
                  return NULL;
                }
              for (i = cap; i < c2; i++)
                b[i].len = 0, b[i].num = 0, b[i].den = 4;
              a = b;
              cap = c2;
            }
          if (num > 0 && num * 4.0 / den > a[m].len + 1e-6)
            a[m].len = num * 4.0 / den, a[m].num = num, a[m].den = den;
          if (m + 1 > *nm)
            *nm = m + 1;
        }
    }
  return a;
}

int gs_read_musicxml (gs_score *sc, char *xml, size_t n, const char **why)
{
  gx_doc *doc = gx_parse (xml, n);
  gx_node *root, *pl, *part;
  part_info *pinfo = NULL;
  int npinfo = 0, first = 1, nmlens = 0;
  mlen *mlens = NULL;
  if (!doc)
    {
      *why = "That MusicXML file is damaged";
      return -1;
    }
  root = gx_root (doc);
  if (!strcmp (root->name, "score-timewise"))
    {
      gx_free (doc);
      *why = "That's a time-wise MusicXML file, which Groove can't read: save it as part-wise MusicXML";
      return -1;
    }
  if (strcmp (root->name, "score-partwise"))
    {
      gx_free (doc);
      *why = "That isn't a MusicXML score";
      return -1;
    }
  pl = gx_child (root, "part-list");
  if (pl)
    read_part_list (pl, &pinfo, &npinfo);
  mlens = measure_lengths (root, &nmlens);
  for (part = gx_child (root, "part"); part; part = gx_next (part, "part"), first = 0)
    {
      double own = 0, stretch = 1;   /* (this part's own bar length, and how it's stretched) */
      part_info *pi = NULL, none;
      gs_part *gp;
      gx_node *meas;
      int i, m = 0, divisions = 1, transpose = 0, vel = 80, chan = -1;
      int ngrace = 0, gord_n = 0, gpitch[16], gval[16], gstaff[16], gord[16];   /* grace notes waiting */
      int vhome[16];
      tupopen topen[16][4];     /* (each voice's open tuplets) */
      int ntopen[16];
      unsigned volta = 0;
      ospan open[MAX_OPEN];
      int nopen = 0;
      const char *pid = gx_attr (part, "id");
      for (i = 0; i < npinfo && pid; i++)
        if (!strcmp (pinfo[i].id, pid))
          pi = &pinfo[i];
      if (!pi)
        {
          memset (&none, 0, sizeof none);
          pi = &none;
        }
      gp = gs_add_part (sc, pi->name);
      if (!gp)
        goto nomem;
      /* (an organ's three staves: manuals braced, pedals apart) */
      {
        char lo[GR_NAME];
        int q;
        for (q = 0; pi->name[q] && q < GR_NAME - 1; q++)
          lo[q] = (char) tolower ((unsigned char) pi->name[q]);
        lo[q] = 0;
        gp->organ = strstr (lo, "organ") != NULL;
      }
      if (!pi->nin || pi->in[0].program < 0)
        gp->program = gs_gm_by_name (pi->name);
      /* Its sound: the first instrument's program, bank and channel. */
      if (pi->nin)
        {
          instr *in = &pi->in[0];
          gp->program = in->program;
          if (gp->program < 0)
            gp->program = gs_gm_by_name (in->name);
          chan = in->channel;
          if (chan == 9)
            gp->drums = 1;
        }
      /* Each voice's own staff: where most of its notes are. A note
         of it on another staff is one the score moves there (a hand's
         chord written on the other hand's staff), kept in its voice
         rather than merged into that staff's own. */
      {
        int vc[16][4], v2, s2;
        memset (vc, 0, sizeof vc);
        for (meas = gx_child (part, "measure"); meas; meas = gx_next (meas, "measure"))
          {
            gx_node *ne;
            for (ne = gx_child (meas, "note"); ne; ne = gx_next (ne, "note"))
              if (gx_child (ne, "pitch") && !gx_child (ne, "grace"))
                {
                  int vv = gx_int (ne, "voice", 1) - 1, ss = gx_int (ne, "staff", 1) - 1;
                  if (vv >= 0 && vv < 16 && ss >= 0 && ss < 4)
                    vc[vv][ss]++;
                }
          }
        memset (ntopen, 0, sizeof ntopen);
        for (v2 = 0; v2 < 16; v2++)
          {
            vhome[v2] = -1;
            for (s2 = 0; s2 < 4; s2++)
              if (vc[v2][s2] && (vhome[v2] < 0 || vc[v2][s2] > vc[v2][vhome[v2]]))
                vhome[v2] = s2;
          }
      }
      for (meas = gx_child (part, "measure"); meas; meas = gx_next (meas, "measure"), m++)
        {
          gs_measure *gm = gs_measure_at (sc, m);
          gx_node *e;
          double pos = 0, maxpos = 0, last_start = 0, raw = 0, rawb[128], posb[128];
          int nb = 0;
          const char *imp = gx_attr (meas, "implicit");
          if (!gm)
            goto nomem;
          {
            gx_node *at;
            int n2, d2;
            for (at = gx_child (meas, "attributes"); at; at = gx_next (at, "attributes"))
              if (time_sig (gx_child (at, "time"), &n2, &d2))
                own = n2 * 4.0 / d2;
            stretch = own > 0 && m < nmlens && mlens[m].len > own + 1e-6 ? mlens[m].len / own : 1;
          }
          if (first && volta)
            gm->endings = volta;
          for (e = meas->child; e; e = e->next)
            {
              const char *nm = e->name;
              /* (the staff it's on, in a part on more than one) */
              gp->cur_staff = gx_child (e, "staff") && gx_int (e, "staff", 1) > 1 ? gx_int (e, "staff", 1) - 1 : 0;
              if (!strcmp (nm, "attributes"))
                {
                  gx_node *t = gx_child (e, "time"), *tr = gx_child (e, "transpose");
                  if (gx_child (e, "divisions"))
                    divisions = gx_int (e, "divisions", 1) > 0 ? gx_int (e, "divisions", 1) : 1;
                  if (gx_child (e, "staff-details") && gx_int (gx_child (e, "staff-details"), "staff-lines", 5) == 1)
                    gp->lines = 1;    /* (a drum's staff of one line) */
                  if (tr)
                    transpose = gx_int (tr, "chromatic", 0) + 12 * gx_int (tr, "octave-change", 0);
                  gp->has_transpose = 1;
                  gp->transpose = -transpose;
                  /* clefs: the first in the part, and changes as marks */
                  {
                    gx_node *c;
                    for (c = gx_child (e, "clef"); c; c = gx_next (c, "clef"))
                      {
                        const char *sg = gx_text (c, "sign");
                        int line = gx_int (c, "line", 0), oc = gx_int (c, "clef-octave-change", 0);
                        int z = gx_attr (c, "number") ? atoi (gx_attr (c, "number")) - 1 : 0, nc = -1;
                        if (!strcmp (sg, "G"))
                          nc = oc < 0 ? NC_G8 : NC_G;
                        else if (!strcmp (sg, "F"))
                          nc = NC_F;
                        else if (!strcmp (sg, "C"))
                          nc = line >= 4 ? NC_C4 : NC_C3;
                        else if (!strcmp (sg, "percussion"))
                          nc = NC_PERC;
                        if (nc < 0 || z < 0 || z >= GS_STAVES)
                          continue;
                        gp->cur_staff = z;
                        if (m == 0 && pos == 0)
                          gp->clef[z] = nc + 1;
                        else if (gs_add_mark (gp, m, pos, -1, 0, GR_MK_CLEF, nc, 0, z))
                          goto nomem;
                      }
                  }
                  /* the key signature, from the first part, as it sounds
                     (a B flat clarinet's two sharps are C major) */
                  if (first && gx_child (e, "key") && gx_child (gx_child (e, "key"), "fifths"))
                    {
                      gx_node *k = gx_child (e, "key");
                      int f = gx_int (k, "fifths", 0) + 7 * (transpose % 12);
                      while (f > 7) f -= 12;
                      while (f < -7) f += 12;
                      gm->has_key = 1;
                      gm->fifths = f;
                      gm->minor = !strcmp (gx_text (k, "mode"), "minor");
                    }
                  if (t && first && gx_child (t, "beats"))
                    {
                      /* "3+2" beats add up */
                      const char *b = gx_text (t, "beats");
                      int num = 0, den = gx_int (t, "beat-type", 4), j;
                      while (*b)
                        {
                          char *end;
                          long v = strtol (b, &end, 10);
                          if (end == b)
                            b++;
                          else
                            {
                              num += (int) v;
                              b = end;
                            }
                        }
                      if (num > 0 && den > 0 && m < nmlens && mlens[m].len > num * 4.0 / den + 1e-6)
                        {
                          /* (a part with its own, shorter: the score's is another part's) */
                          num = mlens[m].num;
                          den = mlens[m].den;
                        }
                      if (num > 0 && den > 0)
                        {
                          const char *sym = gx_attr (t, "symbol") ? gx_attr (t, "symbol") : "";
                          gm->num = num;
                          gm->den = den;
                          gm->timesym = !strcmp (sym, "common") ? 1 : !strcmp (sym, "cut") ? 2 : 0;
                          gm->len = num * 4.0 / den;
                          for (j = m + 1; j < sc->nmeas; j++)
                            {
                              sc->meas[j].num = num;
                              sc->meas[j].den = den;
                              sc->meas[j].timesym = gm->timesym;
                              sc->meas[j].len = gm->len;
                            }
                        }
                    }
                }
              else if (!strcmp (nm, "note"))
                {
                  double dur = gx_num (e, "duration", 0) / divisions, start, snd;
                  gx_node *p = gx_child (e, "pitch"), *up = gx_child (e, "unpitched"), *c;
                  int pitch = -1, tie = 0, v = vel;
                  if (gx_child (e, "grace") && gx_child (e, "pitch") && ngrace < 16)
                    {
                      /* written small before the next note (not played) */
                      gx_node *gp2 = gx_child (e, "pitch");
                      const char *ty = gx_text (e, "type");
                      int gv = !strcmp (ty, "16th") ? NV_16TH : !strcmp (ty, "32nd") ? NV_32ND
                        : !strcmp (ty, "quarter") ? NV_QUARTER : NV_EIGHTH;
                      gx_node *gr = gx_child (e, "grace");
                      if (gx_attr (gr, "slash") && !strcmp (gx_attr (gr, "slash"), "yes"))
                        gv |= 0x40;
                      if (!gx_child (e, "chord") && ngrace)
                        gord_n++;
                      gpitch[ngrace] = (gx_int (gp2, "octave", 4) + 1) * 12 + step_semitone (*gx_text (gp2, "step"))
                        + (int) floor (gx_num (gp2, "alter", 0) + 0.5) + transpose;
                      gval[ngrace] = gv;
                      gstaff[ngrace] = gx_int (e, "staff", 1) - 1;
                      gord[ngrace++] = gord_n;
                      continue;
                    }
                  if (gx_child (e, "grace"))
                    continue;
                  /* The written value, rather than the duration: some
                     programs round tuplets to too few divisions, or get
                     the divisions wrong, as MuseScore allows for. */
                  if (gx_child (e, "type"))
                    {
                      gx_node *tm = gx_child (e, "time-modification"), *d;
                      int dots = 0;
                      double w;
                      for (d = gx_child (e, "dot"); d; d = gx_next (d, "dot"))
                        dots++;
                      w = gs_note_value (gx_text (e, "type"), dots);
                      if (tm && gx_int (tm, "actual-notes", 0) > 0 && gx_int (tm, "normal-notes", 0) > 0)
                        w = w * gx_int (tm, "normal-notes", 1) / gx_int (tm, "actual-notes", 1);
                      /* (a whole bar's rest is written as a whole note) */
                      if (w > 0 && !(gx_child (e, "rest") && gx_attr (gx_child (e, "rest"), "measure")))
                        dur = w * stretch;
                      else if (stretch != 1)
                        dur = mlens[m].len;     /* (a whole bar's rest, of the bar) */
                    }
                  start = gx_child (e, "chord") ? last_start : pos;
                  if (p)
                    {
                      int st = step_semitone (*gx_text (p, "step"));
                      double alter = gx_num (p, "alter", 0);
                      pitch = (gx_int (p, "octave", 4) + 1) * 12 + st + (int) floor (alter + 0.5) + transpose;
                    }
                  else if (up)
                    {
                      instr *in = find_instr (pi, gx_attr (gx_child (e, "instrument"), "id"));
                      pitch = in && in->unpitched >= 0 ? in->unpitched
                        : pi->nin == 1 && pi->in[0].unpitched >= 0 ? pi->in[0].unpitched
                        : drum_at (gx_text (up, "display-step"), gx_int (up, "display-octave", 5), gx_text (e, "notehead"));
                      gp->drums = 1;
                      /* (where the score writes it: its line, as the kit has it) */
                      if (pitch >= 0 && pitch < 128 && !gp->drumline[pitch] && *gx_text (up, "display-step"))
                        {
                          const char *ds = strchr ("CDEFGAB", *gx_text (up, "display-step"));
                          int at = ds ? (gx_int (up, "display-octave", 4) - 4) * 7 + (int) (ds - "CDEFGAB") : 99;
                          if (10 - at > -60 && 10 - at < 60)
                            gp->drumline[pitch] = (unsigned char) (10 - at + 64);
                        }
                    }
                  for (c = gx_child (e, "tie"); c; c = gx_next (c, "tie"))
                    {
                      const char *type = gx_attr (c, "type");
                      if (type && !strcmp (type, "start"))
                        tie |= GS_TIE_FWD;
                      else if (type && !strcmp (type, "stop"))
                        tie |= GS_TIE_BACK;
                    }
                  if (gx_attr (e, "dynamics"))
                    v = (int) (atof (gx_attr (e, "dynamics")) * 0.9 + 0.5);
                  /* How long it sounds: a staccato note half its value (as
                     MuseScore plays it), unless release says exactly (it's
                     let go that much earlier or later). */
                  snd = dur;
                  if (gx_attr (e, "release") && atof (gx_attr (e, "release")) != 0)
                    snd = dur + atof (gx_attr (e, "release")) / divisions;
                  else
                    {
                      gx_node *no = gx_child (e, "notations"), *ar = no ? gx_child (no, "articulations") : NULL;
                      if (ar && (gx_child (ar, "staccato") || gx_child (ar, "staccatissimo") || gx_child (ar, "spiccato")))
                        snd = dur / 2;
                    }
                  if (snd <= 0)
                    snd = dur;
                  /* where it's written: MuseScore numbers voices 1-4 on
                     the first staff, 5-8 on the second */
                  {
                    int wst = gx_int (e, "staff", 1) - 1, wv = gx_int (e, "voice", 1) - 1;
                    gx_node *tmn = gx_child (e, "time-modification");
                    int tact = tmn ? gx_int (tmn, "actual-notes", 0) : 0, tnorm = tmn ? gx_int (tmn, "normal-notes", 0) : 0;
                    const char *nty = tmn && gx_child (tmn, "normal-type") ? gx_text (tmn, "normal-type") : gx_text (e, "type");
                    double bq = gm->den > 0 ? (gm->num % 3 == 0 && gm->num > 3 && gm->den >= 8 ? 12.0 : 4.0) / gm->den : 1;
                    int wr = GR_WR | ((wv < 0 ? 0 : wv) & GR_WR_VOICE) | (wst > 0 ? GR_WR_STAFF : 0)
                      | (snd < dur - 1e-6 && fabs (snd * 2 - dur) < 1e-6 ? GR_WR_HALF : 0)
                      | (wv >= 0 && wv < 16 && vhome[wv] >= 0 && vhome[wv] != wst ? GR_WR_CROSS : 0);
                    if (snd < dur - 1e-6 && !(wr & GR_WR_HALF))
                      wr = 0;                   /* (played some other length: worked out) */
                    else if (tmn)
                      wr |= gr_tuplet_bits (tact, tnorm, tnorm * gs_note_value (nty, 0), bq);
                    /* its accidental (as written: of no use if it's transposed) */
                    if (p && !transpose)
                      {
                        int al = (int) floor (gx_num (p, "alter", 0) + 0.5);
                        if (al >= -2 && al <= 2)
                          wr |= (GR_SP | (al + 2)) << 8;
                      }
                    if (wr && gx_attr (e, "print-object") && !strcmp (gx_attr (e, "print-object"), "no"))
                      wr |= GR_SP_HIDDEN << 8;      /* (played, not shown) */
                    if (pitch >= 0 && !gx_child (e, "rest") && gx_child (e, "cue"))
                      {
                        /* a cue note: written small, not played */
                        if (!gp->drums && gs_add_mark (gp, m, start, m, start + dur, GR_MK_NOTE,
                                                       (wr & GR_WR_VOICE) | ((wr >> 8) & 0x1f) << 2 | 0x80, pitch,
                                                       gr_mknote_staff (wr, 0)))
                          goto nomem;
                      }
                    else if (pitch >= 0 && !gx_child (e, "rest")
                             && gs_add_note (gp, m, start, snd, pitch, v, tie, wr))
                      goto nomem;
                  }
                  /* Its tuplet brackets: where one starts and stops, in its
                     voice, for the Score window to write it there. */
                  {
                    gx_node *no = gx_child (e, "notations"), *tu;
                    int wv = gx_int (e, "voice", 1) - 1, wst = gx_int (e, "staff", 1) - 1;
                    gx_node *tmn = gx_child (e, "time-modification");
                    if (wv < 0 || wv > 15)
                      wv = 0;
                    for (tu = no ? gx_child (no, "tuplet") : NULL; tu && !gx_child (e, "chord"); tu = gx_next (tu, "tuplet"))
                      {
                        const char *ty = gx_attr (tu, "type");
                        if (ty && !strcmp (ty, "start") && ntopen[wv] < 4)
                          {
                            tupopen *o = &topen[wv][ntopen[wv]++];
                            gx_node *ta = gx_child (tu, "tuplet-actual"), *tn = gx_child (tu, "tuplet-normal");
                            const char *nt2 = tn && gx_child (tn, "tuplet-type") ? gx_text (tn, "tuplet-type")
                              : tmn && gx_child (tmn, "normal-type") ? gx_text (tmn, "normal-type") : gx_text (e, "type");
                            o->m = m;
                            o->at = start;
                            o->act = ta && gx_child (ta, "tuplet-number") ? gx_int (ta, "tuplet-number", 0) : tmn ? gx_int (tmn, "actual-notes", 0) : 0;
                            o->norm = tn && gx_child (tn, "tuplet-number") ? gx_int (tn, "tuplet-number", 0) : tmn ? gx_int (tmn, "normal-notes", 0) : 0;
                            o->nv = nv_index (nt2);
                            o->staff = wst > 0;
                            o->show = (gx_attr (tu, "show-number") && !strcmp (gx_attr (tu, "show-number"), "none") ? 0x40 : 0)
                              | (gx_attr (tu, "bracket") && !strcmp (gx_attr (tu, "bracket"), "no") ? 0x80 : 0);
                          }
                        else if (ty && !strcmp (ty, "stop") && ntopen[wv] > 0)
                          {
                            tupopen *o = &topen[wv][--ntopen[wv]];
                            if (o->nv >= 0 && o->act > 0 && o->act < 256 && o->norm > 0 && o->norm < 128
                                && !gp->drums
                                && gs_add_mark (gp, o->m, o->at, m, start + dur, GR_MK_TUPLET,
                                                (wv & 3) | o->nv << 2 | (ntopen[wv] ? 0x20 : 0) | o->show, o->act,
                                                o->norm << 1 | o->staff))
                              goto nomem;
                          }
                      }
                  }
                  /* a rest it shows, in its voice */
                  if (gx_child (e, "rest") && !gx_child (e, "chord")
                      && !(gx_attr (e, "print-object") && !strcmp (gx_attr (e, "print-object"), "no"))
                      && gs_add_mark (gp, m, start, m, start + dur, GR_MK_REST,
                                      ((gx_int (e, "voice", 1) - 1) & 3) | rest_tuplet (e, gm), 0,
                                      gx_int (e, "staff", 1) > 1))
                    goto nomem;
                  /* its words (once a chord) */
                  if (!gx_child (e, "chord") && !gx_child (e, "rest"))
                    {
                      gx_node *ly;
                      for (ly = gx_child (e, "lyric"); ly; ly = gx_next (ly, "lyric"))
                        if (gs_add_lyric (gp, m, start, gx_int (e, "staff", 1) > 1,
                                          gx_attr (ly, "number") ? atoi (gx_attr (ly, "number")) - 1 : 0,
                                          gx_text (ly, "syllabic"), gx_text (ly, "text")))
                          goto nomem;
                    }
                  /* its stem, as the score has it (once a chord) */
                  if (!gx_child (e, "chord") && !gx_child (e, "rest") && !gx_child (e, "grace") && gx_child (e, "stem") && !gp->drums)
                    {
                      const char *sd = gx_text (e, "stem");
                      int dir = !strcmp (sd, "up") ? 1 : !strcmp (sd, "down") ? 2 : 0;
                      if (dir && gs_add_mark (gp, m, start, -1, 0, GR_MK_STEM, dir | ((gx_int (e, "voice", 1) - 1) & 3) << 3, 0,
                                              gx_int (e, "staff", 1) > 1))
                        goto nomem;
                    }
                  /* grace notes before it */
                  if (pitch >= 0 && !gx_child (e, "rest") && ngrace)
                    {
                      int q;
                      for (q = 0; q < ngrace; q++)
                        if (gs_add_mark (gp, m, start, -1, 0, GR_MK_GRACE, gval[q], gpitch[q], gstaff[q]))
                          goto nomem;
                        else
                          gp->marks[gp->nmarks - 1].hi = gord[q];
                      ngrace = gord_n = 0;
                    }
                  /* a pause over a note or rest */
                  {
                    gx_node *no2 = gx_child (e, "notations"), *fe = no2 ? gx_child (no2, "fermata") : NULL;
                    if (fe && !gx_child (e, "chord")
                        && gs_add_mark (gp, m, start, -1, 0, GR_MK_FERMATA,
                                        gx_attr (fe, "type") && !strcmp (gx_attr (fe, "type"), "inverted") ? -1 : 1, 0,
                                        gx_int (e, "staff", 1) - 1))
                      goto nomem;
                  }
                  /* Its written marks (once a chord: on its first note). */
                  if (pitch >= 0 && !gx_child (e, "rest"))
                    {
                      gx_node *no, *x;
                      int staff = gx_int (e, "staff", 1) - 1;
                      for (no = gx_child (e, "notations"); no; no = gx_next (no, "notations"))
                        {
                          for (x = gx_child (no, "slur"); x; x = gx_next (x, "slur"))
                            if (span (open, &nopen, gp, GR_MK_SLUR, 0, x, m, start, staff,
                                      gx_attr (x, "type") && !strcmp (gx_attr (x, "type"), "start"),
                                      gx_attr (x, "type") && !strcmp (gx_attr (x, "type"), "stop")))
                              goto nomem;
                          {
                            /* a spread chord: each of its notes says so; the
                               spread covers them (not the other hand's) */
                            gx_node *ag = gx_child (no, "arpeggiate");
                            gs_mark *lm = NULL;
                            int q;
                            for (q = gp->nmarks - 1; q >= 0 && q >= gp->nmarks - 8 && !lm; q--)
                              if (gp->marks[q].kind == GR_MK_ARPEGGIO && gp->marks[q].measure == m && gp->marks[q].off == start)
                                lm = &gp->marks[q];
                            int dir = !ag || !gx_attr (ag, "direction") ? 1 : !strcmp (gx_attr (ag, "direction"), "down") ? -1
                                      : !strcmp (gx_attr (ag, "direction"), "up") ? 2 : 1;
                            const char *dx = ag && gx_attr (ag, "default-x") ? gx_attr (ag, "default-x") : "";
                            /* (one through both staves: the same spread on the
                               staff below, where MuseScore writes it the same,
                               or with no placing at all) */
                            gs_mark *up = NULL;
                            if (ag && staff >= 1 && !gx_child (e, "chord"))
                              for (q = gp->nmarks - 1; q >= 0 && q >= gp->nmarks - 64 && !up; q--)
                                if (gp->marks[q].kind == GR_MK_ARPEGGIO && gp->marks[q].measure == m && gp->marks[q].off == start
                                    && gp->marks[q].staff + (abs (gp->marks[q].value) & GR_ARP_SPAN ? gp->marks[q].pitch : 0) == staff - 1
                                    && (gp->marks[q].value < 0) == (dir < 0)
                                    && gp->marks[q].emeasure == -1 && fabs (gp->marks[q].eoff - (*dx ? atof (dx) : 0)) < 0.5)
                                  up = &gp->marks[q];
                            if (up)
                              {
                                /* (pitch: the staves below it reaches) */
                                up->pitch = abs (up->value) & GR_ARP_SPAN ? up->pitch + 1 : 1;
                                up->value = up->value < 0 ? -1 - GR_ARP_SPAN : (abs (up->value) & 3) + GR_ARP_SPAN;
                                if (pitch > up->hi) up->hi = pitch;
                              }
                            else if (ag && gx_child (e, "chord") && lm)
                              {
                                if (pitch < lm->pitch) lm->pitch = pitch;
                                if (pitch > lm->hi) lm->hi = pitch;
                              }
                            else if (ag && gs_add_mark (gp, m, start, -1, 0, GR_MK_ARPEGGIO,
                                                        !gx_attr (ag, "direction") ? 1 : !strcmp (gx_attr (ag, "direction"), "down") ? -1
                                                        : !strcmp (gx_attr (ag, "direction"), "up") ? 2 : 1,
                                                        pitch, staff))
                              goto nomem;
                            else if (ag)
                              gp->marks[gp->nmarks - 1].eoff = *dx ? atof (dx) : 0;   /* (where it's placed, for the staff below's) */
                          }
                          if (!gx_child (e, "chord"))
                            {
                              gx_node *ar = gx_child (no, "articulations");
                              if (ar && (gx_child (ar, "staccato") || gx_child (ar, "staccatissimo") || gx_child (ar, "spiccato"))
                                  && gs_add_mark (gp, m, start, -1, 0, GR_MK_STACCATO, 0, 0, staff))
                                goto nomem;
                              if (ar && gx_child (ar, "accent")
                                  && gs_add_mark (gp, m, start, -1, 0, GR_MK_ACCENT, 0, 0, staff))
                                goto nomem;
                              if (ar && gx_child (ar, "strong-accent")
                                  && gs_add_mark (gp, m, start, -1, 0, GR_MK_MARCATO, 0, 0, staff))
                                goto nomem;
                              if (ar && gx_child (ar, "tenuto")
                                  && gs_add_mark (gp, m, start, -1, 0, GR_MK_TENUTO, 0, 0, staff))
                                goto nomem;
                              {
                                gx_node *orn = gx_child (no, "ornaments");
                                int ov = !orn ? 0 : gx_child (orn, "trill-mark") ? 1
                                  : gx_child (orn, "mordent") || gx_child (orn, "inverted-mordent") ? 2
                                  : gx_child (orn, "turn") || gx_child (orn, "inverted-turn") ? 3 : 0;
                                if (ov && gs_add_mark (gp, m, start, -1, 0, GR_MK_ORNAMENT, ov, 0, staff))
                                  goto nomem;
                                /* a tremolo: single (through the stem) or from here to the next */
                                if (orn && gx_child (orn, "tremolo"))
                                  {
                                    gx_node *tr = gx_child (orn, "tremolo");
                                    const char *ty = gx_attr (tr, "type") ? gx_attr (tr, "type") : "single";
                                    int n = gx_int (tr, NULL, 3), nv;
                                    static const char *const nvn[] = { "whole", "half", "quarter", "eighth", "16th", "32nd", "64th", "128th" };
                                    for (nv = 0; nv < 8 && strcmp (nvn[nv], gx_text (e, "type")); nv++)
                                      ;
                                    if (n < 1) n = 1;
                                    if (n > 4) n = 4;
                                    if (!strcmp (ty, "single")
                                        && gs_add_mark (gp, m, start, -1, 0, GR_MK_TREMOLO, n, 0, staff))
                                      goto nomem;
                                    if (!strcmp (ty, "start") && nv < 8
                                        && gs_add_mark (gp, m, start, m, start + 2 * dur, GR_MK_TREMOLO, 0x10 | n,
                                                        nv | count_dots (e) << 4, staff))
                                      goto nomem;
                                  }
                              }
                            }
                          /* a glissando (or slide) from this note */
                          {
                            gx_node *gl = gx_child (no, "glissando");
                            if (!gl)
                              gl = gx_child (no, "slide");
                            if (gl && gx_attr (gl, "type") && !strcmp (gx_attr (gl, "type"), "start")
                                && gs_add_mark (gp, m, start, -1, 0, GR_MK_GLISS, 0, pitch, staff))
                              goto nomem;
                            }
                        }
                    }
                  if (!gx_child (e, "chord"))
                    {
                      if (stretch != 1 && nb < 128)
                        rawb[nb] = raw, posb[nb++] = pos;
                      raw += gx_num (e, "duration", 0) / divisions;
                      last_start = pos;
                      pos += dur;
                      if (stretch != 1 && nb < 128)
                        rawb[nb] = raw, posb[nb++] = pos;
                    }
                }
              else if (!strcmp (nm, "backup") && stretch != 1)
                {
                  /* (a stretched part's: back to where its notes were, as their
                     durations say, else stretched too) */
                  int q;
                  raw -= gx_num (e, "duration", 0) / divisions;
                  pos = raw * stretch;
                  for (q = 0; q < nb; q++)
                    if (fabs (rawb[q] - raw) < 1e-6)
                      {
                        pos = posb[q];
                        break;
                      }
                }
              else if (!strcmp (nm, "forward") && stretch != 1)
                {
                  raw += gx_num (e, "duration", 0) / divisions;
                  pos += gx_num (e, "duration", 0) / divisions * stretch;
                }
              else if (!strcmp (nm, "backup"))
                pos -= gx_num (e, "duration", 0) / divisions;
              else if (!strcmp (nm, "forward"))
                pos += gx_num (e, "duration", 0) / divisions;
              else if (!strcmp (nm, "direction") || !strcmp (nm, "sound"))
                {
                  gx_node *dt;
                  int pedal_seen = 0;
                  double at = pos + gx_num (e, "offset", 0) / divisions;
                  for (dt = gx_child (e, "direction-type"); dt; dt = gx_next (dt, "direction-type"))
                    {
                      gx_node *pd = gx_child (dt, "pedal");
                      if (pd && gx_attr (pd, "type"))
                        {
                          /* the sustain pedal: down, up, or up and down again */
                          const char *type = gx_attr (pd, "type");
                          pedal_seen = 1;
                          if ((!strcmp (type, "stop") || !strcmp (type, "change")) && gs_add_pedal (gp, m, at, 0))
                            goto nomem;
                          if ((!strcmp (type, "start") || !strcmp (type, "change")) && gs_add_pedal (gp, m, at, 1))
                            goto nomem;
                        }
                      /* (an 8va or 8vb line, octave-shift, changes no pitch: the
                         notes under it are written at the pitch they sound) */
                      int staff = gx_int (e, "staff", 1) - 1;
                      gx_node *wd;
                      /* words over or under the staff ("dolce", "rit.") */
                      {
                        /* words over or under the staff ("dolce", "rit."); a
                           tempo's in bold, with its metronome mark after them
                           ("Andante. (\u2669 = 66)"), or the mark alone */
                        const gx_node *dt2, *met = NULL, *snd = gx_child (e, "sound"), *lastw = NULL;
                        int below = gx_attr (e, "placement") && !strcmp (gx_attr (e, "placement"), "below");
                        for (dt2 = gx_child (e, "direction-type"); dt2; dt2 = gx_next (dt2, "direction-type"))
                          {
                            const gx_node *w2;
                            if (!met && (met = gx_child (dt2, "metronome")) && gx_attr (met, "print-object")
                                && !strcmp (gx_attr (met, "print-object"), "no"))
                              met = NULL;
                            for (w2 = gx_child (dt2, "words"); w2; w2 = gx_next (w2, "words"))
                              if (w2->text && *w2->text)
                                lastw = w2;
                          }
                        if (met && (!gx_child (met, "beat-unit") || !gx_child (met, "per-minute")))
                          met = NULL;
                        for (wd = gx_child (dt, "words"); gp; wd = wd ? gx_next (wd, "words") : NULL)
                          {
                            int verse = below ? GR_TEXT_BELOW : GR_TEXT_ABOVE;
                            char wt[160] = "";
                            if (wd && !(wd->text && *wd->text))
                              continue;
                            if (!wd && !(met && !lastw && gx_child (dt, "metronome") == met))
                              break;        /* (the mark alone: once, at its own direction-type) */
                            if (wd)
                              snprintf (wt, sizeof wt, "%s", wd->text);
                            if (met && !below && wd == lastw)
                              {
                                static const char *const units[][2] = {
                                  { "quarter", "\xE2\x99\xA9" }, { "half", "\xF0\x9D\x85\x9E" }, { "eighth", "\xE2\x99\xAA" } };
                                int paren = gx_attr (met, "parentheses") && !strcmp (gx_attr (met, "parentheses"), "yes"), u, nd = 0;
                                const gx_node *bd;
                                char *z = wt + strlen (wt);
                                size_t o;
                                while (z > wt && (z[-1] == ' ' || z[-1] == '('))
                                  *--z = 0;
                                o = strlen (wt);
                                snprintf (wt + o, sizeof wt - o, "%s%s", o ? " " : "", paren ? "(" : "");
                                for (u = 0; u < 3; u++)
                                  if (!strcmp (gx_text (met, "beat-unit"), units[u][0]))
                                    break;
                                o = strlen (wt);
                                snprintf (wt + o, sizeof wt - o, "%s", units[u < 3 ? u : 0][1]);
                                for (bd = gx_child (met, "beat-unit-dot"); bd && nd < 2; bd = gx_next (bd, "beat-unit-dot"), nd++)
                                  {
                                    o = strlen (wt);
                                    snprintf (wt + o, sizeof wt - o, "\xF0\x9D\x85\xAD");
                                  }
                                o = strlen (wt);
                                snprintf (wt + o, sizeof wt - o, " = %s%s", gx_text (met, "per-minute"), paren ? ")" : "");
                                verse = GR_TEXT_TEMPO_MARK;
                              }
                            else if (!below && ((snd && gx_attr (snd, "tempo"))
                                                || (wd && gx_attr (wd, "font-weight") && !strcmp (gx_attr (wd, "font-weight"), "bold"))))
                              verse = GR_TEXT_TEMPO;
                            if (*wt && gs_add_lyric (gp, m, at, staff > 0, verse, NULL, wt))
                              goto nomem;
                            if (!wd)
                              break;
                          }
                      }
                      gx_node *x = gx_child (dt, "octave-shift");
                      if (x && gx_attr (x, "type"))
                        {
                          /* "down": the notes are written lower than they sound - 8va */
                          int sz = gx_attr (x, "size") ? atoi (gx_attr (x, "size")) : 8;
                          int v = (sz >= 15 ? 2 : 1) * (!strcmp (gx_attr (x, "type"), "up") ? -1 : 1);
                          if (span (open, &nopen, gp, GR_MK_OTTAVA, v, x, m, at, staff, strcmp (gx_attr (x, "type"), "stop") != 0,
                                    !strcmp (gx_attr (x, "type"), "stop")))
                            goto nomem;
                        }
                      x = gx_child (dt, "wedge");
                      if (x && gx_attr (x, "type"))
                        {
                          const char *t = gx_attr (x, "type");
                          if (span (open, &nopen, gp, GR_MK_HAIRPIN, (!strcmp (t, "diminuendo") ? 2 : 1)
                                    | (gx_attr (e, "placement") && !strcmp (gx_attr (e, "placement"), "above") ? GR_MK_ABOVE : 0), x, m, at, staff,
                                    !strcmp (t, "crescendo") || !strcmp (t, "diminuendo"), !strcmp (t, "stop")))
                            goto nomem;
                        }
                      gx_node *dy = gx_child (dt, "dynamics");
                      if (dy && dy->child)
                        {
                          static const char *nd[] = { "ppp", "pp", "p", "mp", "mf", "f", "ff", "fff" };
                          int j;
                          for (j = 0; j < 8; j++)
                            if (!strcmp (dy->child->name, nd[j])
                                && gs_add_mark (gp, m, at, -1, 0, GR_MK_DYNAMIC, j + 1,
                                                gx_attr (e, "placement") && !strcmp (gx_attr (e, "placement"), "above"), staff))
                              goto nomem;
                        }
                      if (dy && dy->child)
                        {
                          /* A dynamic mark, as MuseScore plays it. */
                          static const struct { const char *n; int v; } dyn[] = {
                            { "pppppp", 1 }, { "ppppp", 5 }, { "pppp", 10 }, { "ppp", 16 }, { "pp", 33 },
                            { "p", 49 }, { "mp", 64 }, { "mf", 80 }, { "f", 96 }, { "ff", 112 },
                            { "fff", 126 }, { "ffff", 127 }, { "fffff", 127 }, { "ffffff", 127 },
                            { "fp", 96 }, { "sf", 112 }, { "sfz", 112 }, { "sff", 126 }, { "sffz", 126 },
                            { "sfp", 112 }, { "sfpp", 112 }, { "rfz", 112 }, { "rf", 112 }, { "fz", 112 } };
                          int j;
                          for (j = 0; j < (int) (sizeof dyn / sizeof dyn[0]); j++)
                            if (!strcmp (dy->child->name, dyn[j].n))
                              vel = dyn[j].v;
                        }
                    }
                  gx_node *s = !strcmp (nm, "sound") ? e : gx_child (e, "sound");
                  if (s && !pedal_seen && gx_attr (s, "damper-pedal") && gp
                      && gs_add_pedal (gp, m, at, (!strcmp (gx_attr (s, "damper-pedal"), "yes") || atoi (gx_attr (s, "damper-pedal")) > 0)))
                    goto nomem;
                  if (s && gx_attr (s, "tempo") && (first || sc->tempo == 0))
                    {
                      /* (shown as a metronome mark only if the direction writes one) */
                      int shown = 0;
                      const gx_node *dt;
                      if (s != e)
                        for (dt = gx_child (e, "direction-type"); dt; dt = gx_next (dt, "direction-type"))
                          {
                            const gx_node *mt = gx_child (dt, "metronome");
                            if (mt && !(gx_attr (mt, "print-object") && !strcmp (gx_attr (mt, "print-object"), "no")))
                              shown = 1;
                          }
                      if (gs_add_tempo (sc, m, at, atof (gx_attr (s, "tempo"))))
                        goto nomem;
                      if (sc->ntempos > 0)
                        sc->tempos[sc->ntempos - 1].hidden = !shown;
                    }
                  if (s && first)
                    {
                      /* Jumps and their markers, by MusicXML's names:
                         S segno, C coda, T To Coda (put together below). */
                      gs_measure *jm = gs_measure_at (sc, m);
                      char lab[16];
                      if (!jm)
                        goto nomem;
                      if (gx_attr (s, "segno"))
                        snprintf (lab, sizeof lab, "S%s", gx_attr (s, "segno")), gs_measure_label (jm, lab);
                      if (gx_attr (s, "coda"))
                        snprintf (lab, sizeof lab, "C%s", gx_attr (s, "coda")), gs_measure_label (jm, lab);
                      if (gx_attr (s, "tocoda"))
                        snprintf (lab, sizeof lab, "T%s", gx_attr (s, "tocoda")), gs_measure_label (jm, lab);
                      if (gx_attr (s, "fine"))
                        gs_measure_label (jm, "fine");
                      if (gx_attr (s, "dacapo") && !strcmp (gx_attr (s, "dacapo"), "yes"))
                        jm->jump = 1, snprintf (jm->jump_to, sizeof jm->jump_to, "start");
                      if (gx_attr (s, "dalsegno"))
                        jm->jump = 1, snprintf (jm->jump_to, sizeof jm->jump_to, "S%.10s", gx_attr (s, "dalsegno"));
                    }
                  if (s && gx_attr (s, "dynamics"))
                    {
                      vel = (int) (atof (gx_attr (s, "dynamics")) * 0.9 + 0.5);
                      vel = vel < 1 ? 1 : vel > 127 ? 127 : vel;
                    }
                }
              else if (!strcmp (nm, "barline") && first)
                {
                  gx_node *rp = gx_child (e, "repeat"), *en = gx_child (e, "ending");
                  if (rp && gx_attr (rp, "direction"))
                    {
                      if (!strcmp (gx_attr (rp, "direction"), "forward"))
                        gm->start_repeat = 1;
                      else
                        {
                          int t = gx_attr (rp, "times") ? atoi (gx_attr (rp, "times")) : 2;
                          gm->end_repeat = t < 2 ? 2 : t > 16 ? 16 : t;
                        }
                    }
                  if (en && gx_attr (en, "type"))
                    {
                      const char *type = gx_attr (en, "type");
                      if (!strcmp (type, "start"))
                        {
                          const char *num = gx_attr (en, "number");
                          unsigned bits = 0;
                          while (num && *num)
                            {
                              char *end;
                              long v = strtol (num, &end, 10);
                              if (end == num)
                                num++;
                              else
                                {
                                  if (v >= 1 && v <= 32)
                                    bits |= 1u << (v - 1);
                                  num = end;
                                }
                            }
                          volta = bits ? bits : 1;
                          gm->endings = volta;
                        }
                      else
                        {
                          gm->endings = volta ? volta : gm->endings;
                          volta = 0;
                        }
                    }
                }
              if (pos > maxpos)
                maxpos = pos;
            }
          if (first)
            {
              if (imp && !strcmp (imp, "yes") && maxpos > 0)
                gm->len = maxpos;
              else if (maxpos > gm->len + 1e-6)
                gm->len = maxpos;
            }
        }
    }
  /* A jump plays until the Fine, if there is one, else to the To Coda
     and on at its Coda, else to the end. */
  {
    int i, k;
    const char *fine = NULL, *toc = NULL;
    for (i = 0; i < sc->nmeas; i++)
      for (k = 0; k < sc->meas[i].nlabels; k++)
        if (!strcmp (sc->meas[i].labels[k], "fine"))
          fine = "fine";
        else if (sc->meas[i].labels[k][0] == 'T' && !toc)
          toc = sc->meas[i].labels[k];
    for (i = 0; i < sc->nmeas; i++)
      if (sc->meas[i].jump)
        {
          gs_measure *jm = &sc->meas[i];
          snprintf (jm->until, sizeof jm->until, "%s", fine ? fine : toc ? toc : "end");
          if (!fine && toc)
            snprintf (jm->cont, sizeof jm->cont, "C%s", toc + 1);
        }
  }
  free (pinfo);
  free (mlens);
  gx_free (doc);
  return 0;
nomem:
  free (pinfo);
  free (mlens);
  gx_free (doc);
  *why = "Not enough memory";
  return -1;
}

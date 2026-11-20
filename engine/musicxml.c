/* Groove - MusicXML: reading scores (partwise MusicXML 1.0 to 4.0, as
   MuseScore, Sibelius, Dorico and Finale write it), and writing the song
   as one (musicxml_out.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "groove.h"
#include "score.h"
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
        return gs_add_mark (gp, o.measure, o.off, m, at, kind, o.value, 0, o.staff);
      }
  return 0;
}

int gs_read_musicxml (gs_score *sc, char *xml, size_t n, const char **why)
{
  gx_doc *doc = gx_parse (xml, n);
  gx_node *root, *pl, *part;
  part_info *pinfo = NULL;
  int npinfo = 0, first = 1;
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
  for (part = gx_child (root, "part"); part; part = gx_next (part, "part"), first = 0)
    {
      part_info *pi = NULL, none;
      gs_part *gp;
      gx_node *meas;
      int i, m = 0, divisions = 1, transpose = 0, vel = 80, chan = -1;
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
      for (meas = gx_child (part, "measure"); meas; meas = gx_next (meas, "measure"), m++)
        {
          gs_measure *gm = gs_measure_at (sc, m);
          gx_node *e;
          double pos = 0, maxpos = 0, last_start = 0;
          const char *imp = gx_attr (meas, "implicit");
          if (!gm)
            goto nomem;
          if (first && volta)
            gm->endings = volta;
          for (e = meas->child; e; e = e->next)
            {
              const char *nm = e->name;
              if (!strcmp (nm, "attributes"))
                {
                  gx_node *t = gx_child (e, "time"), *tr = gx_child (e, "transpose");
                  if (gx_child (e, "divisions"))
                    divisions = gx_int (e, "divisions", 1) > 0 ? gx_int (e, "divisions", 1) : 1;
                  if (tr)
                    transpose = gx_int (tr, "chromatic", 0) + 12 * gx_int (tr, "octave-change", 0);
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
                      if (num > 0 && den > 0)
                        {
                          gm->num = num;
                          gm->den = den;
                          gm->len = num * 4.0 / den;
                          for (j = m + 1; j < sc->nmeas; j++)
                            {
                              sc->meas[j].num = num;
                              sc->meas[j].den = den;
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
                  if (gx_child (e, "grace") || gx_child (e, "cue"))
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
                        dur = w;
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
                  if (pitch >= 0 && !gx_child (e, "rest")
                      && gs_add_note (gp, m, start, snd, pitch, v, tie))
                    goto nomem;
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
                            if (ag && gx_child (e, "chord") && lm)
                              {
                                if (pitch < lm->pitch) lm->pitch = pitch;
                                if (pitch > lm->hi) lm->hi = pitch;
                              }
                            else if (ag && gs_add_mark (gp, m, start, -1, 0, GR_MK_ARPEGGIO,
                                                        !gx_attr (ag, "direction") ? 1 : !strcmp (gx_attr (ag, "direction"), "down") ? -1
                                                        : !strcmp (gx_attr (ag, "direction"), "up") ? 2 : 1,
                                                        pitch, staff))
                              goto nomem;
                          }
                          if (!gx_child (e, "chord"))
                            {
                              gx_node *ar = gx_child (no, "articulations");
                              if (ar && (gx_child (ar, "staccato") || gx_child (ar, "staccatissimo") || gx_child (ar, "spiccato"))
                                  && gs_add_mark (gp, m, start, -1, 0, GR_MK_STACCATO, 0, 0, staff))
                                goto nomem;
                              if (ar && (gx_child (ar, "accent") || gx_child (ar, "strong-accent"))
                                  && gs_add_mark (gp, m, start, -1, 0, GR_MK_ACCENT, 0, 0, staff))
                                goto nomem;
                            }
                        }
                    }
                  if (!gx_child (e, "chord"))
                    {
                      last_start = pos;
                      pos += dur;
                    }
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
                          if (span (open, &nopen, gp, GR_MK_HAIRPIN, !strcmp (t, "diminuendo") ? 2 : 1, x, m, at, staff,
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
                                && gs_add_mark (gp, m, at, -1, 0, GR_MK_DYNAMIC, j + 1, 0, staff))
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
                  if (s && gx_attr (s, "tempo") && (first || sc->tempo == 0)
                      && gs_add_tempo (sc, m, at, atof (gx_attr (s, "tempo"))))
                    goto nomem;
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
  free (pinfo);
  gx_free (doc);
  return 0;
nomem:
  free (pinfo);
  gx_free (doc);
  *why = "Not enough memory";
  return -1;
}

/* Groove - reading MuseScore files (.mscx, the XML inside .mscz), from
   MuseScore 2 to 4 (and the 4.4+ layout, where ties are written on the
   note they end on).

   What's read: each part's instrument (its program and bank, or a drum
   set), its staves' notes with their written lengths (dots, tuplets,
   measure rests, pickups and the voices' position moves), ties, time
   signatures, the first tempo, dynamics as note velocities, and repeats
   with their voltas. Grace notes are left out (they take no time in the
   bar). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "groove.h"
#include "score.h"
#include "xml.h"

#define MAX_STAVES 64
#define MAX_TUPLETS 8

/* "a/b" as a number: a fraction of a whole note, in quarter notes. */
static double fraction_q (const char *t)
{
  long a = 0, b = 1;
  if (sscanf (t, " %ld/%ld", &a, &b) < 1 || b == 0)
    return 0;
  return a * 4.0 / b;
}

static int is_grace (const gx_node *chord)
{
  static const char *g[] = { "acciaccatura", "appoggiatura", "grace4", "grace8after", "grace16",
                             "grace16after", "grace32", "grace32after" };
  const gx_node *c;
  int i;
  for (c = chord->child; c; c = c->next)
    for (i = 0; i < (int) (sizeof g / sizeof g[0]); i++)
      if (!strcmp (c->name, g[i]))
        return 1;
  return 0;
}

/* How a note is tied, from the elements each MuseScore version writes. */
static int note_tie (const gx_node *note)
{
  const gx_node *c;
  int tie = 0;
  for (c = note->child; c; c = c->next)
    {
      if (!strcmp (c->name, "Tie"))
        /* MuseScore 4.4+: on the note it ends on, naming its start.
           MuseScore 2: <Tie id> on the note it starts from. */
        tie |= gx_child (c, "startElement") ? GS_TIE_BACK : GS_TIE_FWD;
      else if (!strcmp (c->name, "endSpanner"))
        tie |= GS_TIE_BACK;                      /* MuseScore 2 */
      else if (!strcmp (c->name, "Spanner"))
        {
          const char *type = gx_attr (c, "type");
          if (type && !strcmp (type, "Tie"))      /* MuseScore 3 to 4.3 */
            {
              if (gx_child (c, "next"))
                tie |= GS_TIE_FWD;
              if (gx_child (c, "prev"))
                tie |= GS_TIE_BACK;
            }
        }
    }
  return tie;
}

/* "1, 2" or "1-3" as a bit set (bit n = ending n+1). */
static unsigned endings_bits (const char *t)
{
  unsigned bits = 0;
  while (*t)
    {
      char *e;
      long a = strtol (t, &e, 10), b;
      if (e == t)
        {
          t++;
          continue;
        }
      b = a;
      t = e;
      while (*t == ' ')
        t++;
      if (*t == '-')
        {
          b = strtol (t + 1, &e, 10);
          t = e;
        }
      for (; a <= b && a <= 32; a++)
        if (a >= 1)
          bits |= 1u << (a - 1);
    }
  return bits;
}

typedef struct
{
  gs_score *sc;
  gs_part *part;
  int first_staff;              /* this staff sets the measures */
  int division;
  int version;                  /* MuseScore's file version x 100 (302, 410...) */
  int vel;                      /* the dynamic in force */
  int pending_volta_end;        /* measures left in a volta (first staff) */
  unsigned volta_bits;
  char volta_id[16];            /* MuseScore 2: the open volta, until its endSpanner */
  char pedal_id[16];            /* MuseScore 2: the pedal held down, until its endSpanner */
  int staff;                    /* which of its part's staves (0, 1) */
} reader;

/* A spanner starting here (it has <next>): where it ends, as MuseScore
   writes it - measures and fractions on from this measure and this
   point in it. 0 if this is its end (<prev>). */
static int span_end (const gx_node *sp, int m, double pos, int *em, double *eoff)
{
  const gx_node *nx = gx_child (sp, "next"), *loc = nx ? gx_child (nx, "location") : NULL;
  if (!nx)
    return 0;
  *em = m + (loc ? gx_int (loc, "measures", 0) : 0);
  *eoff = pos + (loc && gx_child (loc, "fractions") ? fraction_q (gx_text (loc, "fractions")) : 0);
  return 1;
}

/* A dynamic's name as notation.h's ND_PPP..ND_FFF (1..8), or 0. */
static int dyn_level (const char *t)
{
  static const char *n[] = { "ppp", "pp", "p", "mp", "mf", "f", "ff", "fff" };
  int i;
  for (i = 0; i < 8; i++)
    if (!strcmp (t, n[i]))
      return i + 1;
  return 0;
}

/* An 8va line's subtype: "8va", "8vb", "15ma", "15mb" (MuseScore 3 and
   4), or 0..3 in that order (older files). */
static int ottava_shift (const char *t)
{
  if (!strncmp (t, "8va", 3) || !strcmp (t, "0")) return 1;
  if (!strncmp (t, "8vb", 3) || !strcmp (t, "1")) return -1;
  if (!strncmp (t, "15ma", 4) || !strcmp (t, "2")) return 2;
  if (!strncmp (t, "15mb", 4) || !strcmp (t, "3")) return -2;
  return 0;
}

/* The written marks on a chord: slurs from it, a spread chord,
   staccato and accents. */
static int chord_marks (reader *r, const gx_node *chord, int m, double pos)
{
  const gx_node *c;
  for (c = chord->child; c; c = c->next)
    {
      if (!strcmp (c->name, "Spanner") && gx_attr (c, "type") && !strcmp (gx_attr (c, "type"), "Slur"))
        {
          int em;
          double eoff;
          if (span_end (c, m, pos, &em, &eoff)
              && gs_add_mark (r->part, m, pos, em, eoff, GR_MK_SLUR, 0, 0, r->staff))
            return -1;
        }
      else if (!strcmp (c->name, "Arpeggio"))
        {
          /* subtype: 0 plain, 1 up, 2 down, 3 a bracket (not spread), 4 and 5
             straight up and down */
          int t = gx_int (c, "subtype", 0), lo = 127, hi = 0, p;
          const gx_node *nt;
          /* (only this chord's notes are spread: not the other hand's) */
          for (nt = gx_child (chord, "Note"); nt; nt = gx_next (nt, "Note"))
            if ((p = gx_int (nt, "pitch", -1)) >= 0)
              {
                if (p < lo) lo = p;
                if (p > hi) hi = p;
              }
          if (t != 3 && hi >= lo)
            {
              if (gs_add_mark (r->part, m, pos, -1, 0, GR_MK_ARPEGGIO,
                               t == 2 || t == 5 ? -1 : t == 1 || t == 4 ? 2 : 1, lo, r->staff))
                return -1;
              r->part->marks[r->part->nmarks - 1].hi = hi;
            }
        }
      else if (!strcmp (c->name, "Articulation"))
        {
          const char *t = gx_child (c, "subtype") ? gx_text (c, "subtype") : gx_text (c, NULL);
          int kind = strstr (t, "taccat") ? GR_MK_STACCATO
            : strstr (t, "ccent") || strstr (t, "sforzato") || strstr (t, "arcato") ? GR_MK_ACCENT : 0;
          if (kind && gs_add_mark (r->part, m, pos, -1, 0, kind, 0, 0, r->staff))
            return -1;
        }
    }
  return 0;
}

/* MuseScore 2's voltas: <Volta id> where one starts, and <endSpanner id>
   where it has ended - before the measure's notes if it ended with the
   last measure, else after them. */
static void ms2_volta (reader *r, const gx_node *meas, gs_measure *gm)
{
  const gx_node *list[9], *e;
  int nl = 0, i, seen_notes = 0, closes = 0;
  list[nl++] = meas;
  for (e = gx_child (meas, "voice"); e && nl < 9; e = gx_next (e, "voice"))
    list[nl++] = e;
  for (i = 0; i < nl; i++)
    for (e = list[i]->child; e; e = e->next)
      {
        const char *id = gx_attr (e, "id");
        if (!strcmp (e->name, "Chord") || !strcmp (e->name, "Rest"))
          seen_notes = 1;
        else if (!strcmp (e->name, "endSpanner") && id && r->volta_id[0] && !strcmp (id, r->volta_id))
          closes = seen_notes ? 2 : 1;
        else if (!strcmp (e->name, "Volta") && id)
          {
            if (closes == 0 && r->volta_id[0])
              closes = 1;                  /* (one volta straight after another) */
            snprintf (r->volta_id, sizeof r->volta_id, "%s", id);
            r->volta_bits = endings_bits (gx_text (e, "endings"));
            if (!r->volta_bits)
              r->volta_bits = 1;
            gm->endings = r->volta_bits;
            return;
          }
      }
  if (closes == 1)
    r->volta_id[0] = 0;
  else if (r->volta_id[0])
    {
      gm->endings = r->volta_bits;
      if (closes == 2)
        r->volta_id[0] = 0;
    }
}

static int read_voice (reader *r, const gx_node *voice, int m, double mlen, double mstart)
{
  double pos = 0, factor[MAX_TUPLETS], f = 1;
  int ntup = 0, k;
  struct { char id[16]; double f; } ids[MAX_TUPLETS];
  int nids = 0;
  const gx_node *e;
  for (e = voice->child; e; e = e->next)
    {
      const char *n = e->name;
      if (!strcmp (n, "location"))
        {
          if (gx_child (e, "fractions"))
            pos += fraction_q (gx_text (e, "fractions"));
        }
      else if (!strcmp (n, "tick"))           /* MuseScore 2: absolute */
        pos = gx_num (e, NULL, 0) / (r->division ? r->division : 480) - mstart;
      else if (!strcmp (n, "Tuplet"))
        {
          double tf = 1;
          int norm = gx_int (e, "normalNotes", 0), act = gx_int (e, "actualNotes", 0);
          if (norm > 0 && act > 0)
            tf = (double) norm / act;
          if (gx_attr (e, "id"))
            {
              /* MuseScore 2: chords name their tuplet by id */
              if (nids < MAX_TUPLETS)
                {
                  snprintf (ids[nids].id, sizeof ids[nids].id, "%s", gx_attr (e, "id"));
                  ids[nids++].f = tf;
                }
            }
          else if (ntup < MAX_TUPLETS)
            {
              factor[ntup++] = tf;
              f *= tf;
            }
        }
      else if (!strcmp (n, "endTuplet"))
        {
          if (ntup > 0)
            f /= factor[--ntup];
        }
      else if (!strcmp (n, "Dynamic"))
        {
          int v = gx_int (e, "velocity", 0), d = dyn_level (gx_text (e, "subtype"));
          if (v > 0)
            r->vel = v > 127 ? 127 : v;
          if (d && gs_add_mark (r->part, m, pos, -1, 0, GR_MK_DYNAMIC, d, 0, r->staff))
            return -1;
        }
      else if (!strcmp (n, "Tempo"))
        {
          /* (in beats a second, in quarters; written on the first staff) */
          double t = gx_num (e, "tempo", 0);
          if (t > 0 && (r->first_staff || r->sc->tempo == 0) && gs_add_tempo (r->sc, m, pos, t * 60))
            return -1;
        }
      else if (!strcmp (n, "Spanner") && gx_attr (e, "type")
               && (!strcmp (gx_attr (e, "type"), "HairPin") || !strcmp (gx_attr (e, "type"), "Ottava")))
        {
          int em, hair = !strcmp (gx_attr (e, "type"), "HairPin"), v;
          double eoff;
          const gx_node *h = gx_child (e, hair ? "HairPin" : "Ottava");
          if (h && span_end (e, m, pos, &em, &eoff))
            {
              if (hair)
                {
                  /* subtype 0, 2: crescendo (a hairpin, or "cresc."); 1, 3: diminuendo */
                  v = gx_int (h, "subtype", 0) & 1 ? 2 : 1;
                  if (gs_add_mark (r->part, m, pos, em, eoff, GR_MK_HAIRPIN, v, 0, r->staff))
                    return -1;
                }
              else if ((v = ottava_shift (gx_text (h, "subtype"))) != 0
                       && gs_add_mark (r->part, m, pos, em, eoff, GR_MK_OTTAVA, v, 0, r->staff))
                return -1;
            }
        }
      else if (!strcmp (n, "Spanner") && gx_attr (e, "type") && !strcmp (gx_attr (e, "type"), "Pedal"))
        {
          /* MuseScore 3 and 4: the pedal line starts here (with next), or
             ends here (with prev) */
          if (gs_add_pedal (r->part, m, pos, !gx_child (e, "prev")))
            return -1;
        }
      else if (!strcmp (n, "Pedal") && gx_attr (e, "id"))
        {
          snprintf (r->pedal_id, sizeof r->pedal_id, "%s", gx_attr (e, "id"));   /* MuseScore 2 */
          if (gs_add_pedal (r->part, m, pos, 1))
            return -1;
        }
      else if (!strcmp (n, "endSpanner") && r->pedal_id[0] && gx_attr (e, "id")
               && !strcmp (gx_attr (e, "id"), r->pedal_id))
        {
          r->pedal_id[0] = 0;
          if (gs_add_pedal (r->part, m, pos, 0))
            return -1;
        }
      else if (!strcmp (n, "Spanner") && r->first_staff)
        {
          const char *type = gx_attr (e, "type");
          const gx_node *v = gx_child (e, "Volta"), *nx = gx_child (e, "next");
          if (type && !strcmp (type, "Volta") && v)
            {
              int span = 1;
              const gx_node *loc = nx ? gx_child (nx, "location") : NULL;
              if (loc && gx_child (loc, "measures"))
                span = gx_int (loc, "measures", 1);
              else if (loc && gx_child (loc, "fractions"))
                span = (int) (fraction_q (gx_text (loc, "fractions")) / (mlen > 0 ? mlen : 4) + 0.999);
              if (span < 1)
                span = 1;
              r->volta_bits = endings_bits (gx_text (v, "endings"));
              if (!r->volta_bits)
                r->volta_bits = 1;
              r->pending_volta_end = span;
            }
        }
      else if (!strcmp (n, "Chord") || !strcmp (n, "Rest"))
        {
          const char *dt = gx_text (e, "durationType");
          double len, cf = f;
          const gx_node *tn = gx_child (e, "Tuplet");
          if (tn && *gx_text (tn, NULL))
            for (k = 0; k < nids; k++)
              if (!strcmp (ids[k].id, gx_text (tn, NULL)))
                cf *= ids[k].f;
          if (gx_child (e, "duration"))
            len = fraction_q (gx_text (e, "duration"));
          else if (!strcmp (dt, "measure"))
            len = mlen;
          else
            len = gs_note_value (dt, gx_int (e, "dots", 0)) * cf;
          if (!strcmp (n, "Chord"))
            {
              const gx_node *note, *ar;
              double snd = len;
              if (is_grace (e))
                continue;
              if (chord_marks (r, e, m, pos))
                return -1;
              /* staccato: half its value, as MuseScore plays it */
              for (ar = gx_child (e, "Articulation"); ar; ar = gx_next (ar, "Articulation"))
                if (strstr (gx_text (ar, "subtype"), "taccat") || strstr (gx_text (ar, NULL), "taccat"))
                  snd = len / 2;
              for (note = gx_child (e, "Note"); note; note = gx_next (note, "Note"))
                {
                  int pitch = gx_int (note, "pitch", -1), vel = r->vel;
                  if (gx_child (note, "play") && !gx_int (note, "play", 1))
                    continue;                    /* (a cue note: shown, not played) */
                  /* A note's own velocity: MuseScore 3 marks one that
                     replaces the dynamic "user" (else it's an offset);
                     from MuseScore 4 it's always the velocity. */
                  if (gx_child (note, "velocity"))
                    {
                      const char *vt = gx_text (note, "veloType");
                      if (!strcmp (vt, "user") || (!*vt && r->version >= 400))
                        vel = gx_int (note, "velocity", vel);
                      else if (!strcmp (vt, "offset") || !*vt)
                        vel += vel * gx_int (note, "velocity", 0) / 100;
                      vel = vel < 1 ? 1 : vel > 127 ? 127 : vel;
                    }
                  if (gs_add_note (r->part, m, pos, snd, pitch, vel, note_tie (note)))
                    return -1;
                }
            }
          pos += len;
        }
    }
  return 0;
}

int gs_read_mscx (gs_score *sc, char *xml, size_t n, const char **why)
{
  gx_doc *doc = gx_parse (xml, n);
  gx_node *root, *score, *e;
  int staff_part[MAX_STAVES + 1], nstaff = 0, first = -1, k, nmeid = 0;
  const char **meid = NULL;     /* the first staff's measures' element ids */
  reader r;
  if (!doc)
    {
      *why = "That MuseScore file is damaged";
      return -1;
    }
  root = gx_root (doc);
  score = strcmp (root->name, "museScore") ? NULL : gx_child (root, "Score");
  if (!score)
    {
      gx_free (doc);
      *why = "That isn't a MuseScore file";
      return -1;
    }
  memset (&r, 0, sizeof r);
  r.sc = sc;
  r.division = gx_int (score, "Division", 480);
  if (gx_attr (root, "version"))
    r.version = (int) (atof (gx_attr (root, "version")) * 100 + 0.5);
  for (k = 0; k <= MAX_STAVES; k++)
    staff_part[k] = -1;
  /* The parts, and which staves are theirs. */
  for (e = gx_child (score, "Part"); e; e = gx_next (e, "Part"))
    {
      gx_node *ins = gx_child (e, "Instrument"), *st, *ch;
      const char *name = gx_text (e, "trackName");
      gs_part *p;
      if (!*name && ins)
        name = gx_text (ins, "trackName");
      if (!*name && ins)
        name = gx_text (ins, "longName");
      if (!*name && ins && gx_child (ins, "InstrumentLabel"))          /* MuseScore 4.4+ */
        name = gx_text (gx_child (ins, "InstrumentLabel"), "longName");
      p = gs_add_part (sc, name);
      if (!p)
        goto nomem;
      if (ins && gx_int (ins, "useDrumset", 0))
        p->drums = 1;
      for (st = gx_child (e, "Staff"); st; st = gx_next (st, "Staff"))
        {
          const gx_node *stt = gx_child (st, "StaffType");
          const char *id = gx_attr (st, "id");
          int sid = id ? atoi (id) : nstaff + 1;
          if (stt && gx_attr (stt, "group") && !strcmp (gx_attr (stt, "group"), "percussion"))
            p->drums = 1;
          if (sid >= 1 && sid <= MAX_STAVES)
            staff_part[sid] = sc->nparts - 1;
          nstaff++;
        }
      /* Its sound: its "normal" channel, or the first (brass have "open"
         and "mute", strings "arco", "pizzicato" and "tremolo"). */
      {
        const gx_node *use = NULL, *c;
        for (ch = ins ? gx_child (ins, "Channel") : NULL; ch; ch = gx_next (ch, "Channel"))
          {
            const char *cn = gx_attr (ch, "name");
            if (!use)
              use = ch;
            if (!cn || !strcmp (cn, "normal"))
              {
                use = ch;
                break;
              }
          }
        for (c = use ? use->child : NULL; c; c = c->next)
          if (!strcmp (c->name, "program") && gx_attr (c, "value"))
            p->program = atoi (gx_attr (c, "value")) & 127;
        /* (Its bank isn't used: it picks among MuseScore's own sounds,
           which a General MIDI SoundFont or instrument doesn't have.) */
      }
      if (p->drums && p->program < 0)
        p->program = 0;
    }
  /* The staves' music. The first staff sets the measures. */
  for (e = gx_child (score, "Staff"); e; e = gx_next (e, "Staff"))
    {
      const char *id = gx_attr (e, "id");
      int sid = id ? atoi (id) : 1, m = 0;
      double mstart = 0;
      gx_node *meas;
      if (sid < 1 || sid > MAX_STAVES || staff_part[sid] < 0)
        continue;
      if (first < 0)
        first = sid;
      r.part = &sc->parts[staff_part[sid]];
      r.first_staff = sid == first;
      for (k = sid - 1, r.staff = 0; k >= 1 && staff_part[k] == staff_part[sid]; k--)
        r.staff++;
      r.vel = 80;
      r.pending_volta_end = 0;
      for (meas = gx_child (e, "Measure"); meas; meas = gx_next (meas, "Measure"), m++)
        {
          gs_measure *gm = gs_measure_at (sc, m);
          gx_node *v, *ts = NULL;
          const char *len = gx_attr (meas, "len");
          int voices = 0;
          if (!gm)
            goto nomem;
          /* The time signature, from any voice. */
          for (v = gx_child (meas, "voice"); v && !ts; v = gx_next (v, "voice"))
            ts = gx_child (v, "TimeSig");
          if (!ts)
            ts = gx_child (meas, "TimeSig");
          if (r.first_staff)
            {
              const char **a = realloc (meid, (size_t) (m + 1) * sizeof *a);
              if (!a)
                goto nomem;
              meid = a;
              meid[m] = gx_text (meas, "eid");
              nmeid = m + 1;
              if (ts && gx_int (ts, "sigN", 0) > 0 && gx_int (ts, "sigD", 0) > 0)
                {
                  int j;
                  gm->num = gx_int (ts, "sigN", 4);
                  gm->den = gx_int (ts, "sigD", 4);
                  gm->len = gm->num * 4.0 / gm->den;
                  for (j = m + 1; j < sc->nmeas; j++)
                    {
                      sc->meas[j].num = gm->num;
                      sc->meas[j].den = gm->den;
                      sc->meas[j].len = gm->len;
                    }
                }
              if (len)
                gm->len = fraction_q (len);
              if (gx_child (meas, "startRepeat"))
                gm->start_repeat = 1;
              if (gx_child (meas, "endRepeat"))
                {
                  int t = gx_int (meas, "endRepeat", 2);
                  gm->end_repeat = t < 2 ? 2 : t > 16 ? 16 : t;
                }
              if (r.pending_volta_end > 0)
                {
                  gm->endings = r.volta_bits;
                  r.pending_volta_end--;
                }
            }
          for (v = gx_child (meas, "voice"); v; v = gx_next (v, "voice"))
            {
              voices++;
              if (read_voice (&r, v, m, sc->meas[m].len, mstart))
                goto nomem;
            }
          if (!voices && read_voice (&r, meas, m, sc->meas[m].len, mstart))   /* MuseScore 2 */
            goto nomem;
          if (r.first_staff)
            ms2_volta (&r, meas, &sc->meas[m]);
          /* A volta that starts in this measure covers it too. */
          if (r.first_staff && r.pending_volta_end > 0 && !sc->meas[m].endings)
            {
              sc->meas[m].endings = r.volta_bits;
              r.pending_volta_end--;
            }
          mstart += sc->meas[m].len;
        }
    }
  /* MuseScore 4.4+: voltas are in a list of their own, naming the
     measures they start and end in (the first staff's, by element id). */
  for (e = gx_child (gx_child (score, "SpannerMap"), "Volta"); e && nmeid; e = gx_next (e, "Volta"))
    {
      const char *a = gx_text (e, "startElement"), *b = gx_text (e, "endElement");
      int from = -1, to = -1, j;
      unsigned bits = endings_bits (gx_text (e, "endings"));
      for (j = 0; j < nmeid; j++)
        {
          if (!strcmp (meid[j], a))
            from = j;
          if (!strcmp (meid[j], b))
            to = j;
        }
      if (from >= 0 && to >= from)
        for (j = from; j <= to && j < sc->nmeas; j++)
          sc->meas[j].endings = bits ? bits : 1;
    }
  free (meid);
  gx_free (doc);
  if (first < 0)
    {
      *why = "That MuseScore file has no staves";
      return -1;
    }
  return 0;
nomem:
  free (meid);
  gx_free (doc);
  *why = "Not enough memory";
  return -1;
}

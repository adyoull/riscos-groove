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
#include "notation.h"
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

/* A clef's name (G, F, C3, G8vb, PERC...) as NC_* + 1, or 0. Groove
   has no clef an octave up or down but the guitar's, nor soprano or
   baritone C clefs: those are written as the nearest it has. */
static int clef_of (const char *t)
{
  if (!t || !*t) return 0;
  if (!strcmp (t, "G8vb") || !strcmp (t, "G8vb_o") || !strcmp (t, "G8vb_b") || !strcmp (t, "G8vb_p"))
    return NC_G8 + 1;
  if (*t == 'G') return NC_G + 1;
  if (*t == 'F') return NC_F + 1;
  if (!strcmp (t, "C3") || !strcmp (t, "C2") || !strcmp (t, "C1")) return NC_C3 + 1;
  if (!strcmp (t, "C4") || !strcmp (t, "C5")) return NC_C4 + 1;
  if (!strncmp (t, "PERC", 4)) return NC_PERC + 1;
  if (!strcmp (t, "TAB") || !strncmp (t, "TAB", 3)) return 0;
  /* (MuseScore 2 numbers them: 0 G, 1 G8va, 2 G15ma, 3 G8vb, 4 F...) */
  if (*t >= '0' && *t <= '9')
    {
      static const int old[] = { NC_G, NC_G, NC_G, NC_G8, NC_F, NC_F, NC_F, NC_F, NC_F, NC_F,
                                 NC_C3, NC_C3, NC_C3, NC_C4, NC_PERC };
      int k = atoi (t);
      return k >= 0 && k < (int) (sizeof old / sizeof old[0]) ? old[k] + 1 : 0;
    }
  return 0;
}

/* A <Clef>'s type, as written (transposing) where it says. */
static int clef_elem (const gx_node *c)
{
  const char *t = gx_text (c, "transposingClefType");
  if (!*t) t = gx_text (c, "concertClefType");
  if (!*t) t = gx_text (c, "subtype");
  return clef_of (t);
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
  double chord_len;             /* the chord being read: how long it takes (its tuplet's too) */
  int mrep;                     /* in a measure repeat: how many measures it repeats */
  double stretch;               /* a staff with a time signature of its own: its notes'
                                   lengths to the score's (MuseScore's stretch, upside down) */
  unsigned volta_bits;
  char volta_id[16];            /* MuseScore 2: the open volta, until its endSpanner */
  char pedal_id[16];            /* MuseScore 2: the pedal held down, until its endSpanner */
  struct { char id[16]; int kind, value, m; double pos; } ms2[8];   /* MuseScore 2: 8va lines and
                                                                     hairpins, until their endSpanner */
  int staff;                    /* which of its part's staves (0, 1) */
  int beam_stem;                /* a <Beam>'s stem direction (1 up, 2 down), for its first chord */
  int voice;                    /* the chord's voice (0-3), for chord_marks */
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

/* An element's text with its children's (a lyric's <b>, <i>, <font>...),
   as far as fits in out (n bytes). */
static void all_text (const gx_node *e, char *out, size_t n)
{
  const gx_node *c;
  int lead = e->text && *e->text;
  size_t o;
  for (c = e->child; c; c = c->next)
    if (c->tail == e->text)
      lead = 0;               /* (its text is a child's tail, not before them:
                                 "<b>Vite</b> (inquiet)" in that order) */
  o = strlen (out);
  if (lead && o + 1 < n)
    snprintf (out + o, n - o, "%s", e->text);
  for (c = e->child; c; c = c->next)
    {
      if (strcmp (c->name, "sym"))
        all_text (c, out, n);
      o = strlen (out);
      if (c->tail && *c->tail && o + 1 < n)
        snprintf (out + o, n - o, "%s", c->tail);
    }
}

/* A text's words and symbols in order, as UTF-8 (a metronome mark's
   note as Unicode's; MuseScore's "space" symbol and others left out). */
static void rich_text (const gx_node *e, char *out, size_t n)
{
  static const char *const syms[][2] = {
    { "metNoteQuarterUp", "\xE2\x99\xA9" }, { "unicodeNoteQuarterUp", "\xE2\x99\xA9" },
    { "metNoteHalfUp", "\xF0\x9D\x85\x9E" }, { "unicodeNoteHalfUp", "\xF0\x9D\x85\x9E" },
    { "metNote8thUp", "\xE2\x99\xAA" }, { "unicodeNote8thUp", "\xE2\x99\xAA" },
    { "metAugmentationDot", "\xF0\x9D\x85\xAD" }, { "unicodeAugmentationDot", "\xF0\x9D\x85\xAD" },
  };
  const gx_node *c;
  int lead = e->text && *e->text;
  size_t o, i;
  for (c = e->child; c; c = c->next)
    if (c->tail == e->text)
      lead = 0;               /* (its text is a child's tail, not before them) */
  o = strlen (out);
  if (lead && o + 1 < n)
    snprintf (out + o, n - o, "%s", e->text);
  for (c = e->child; c; c = c->next)
    {
      if (!strcmp (c->name, "sym"))
        {
          for (i = 0; i < sizeof syms / sizeof syms[0]; i++)
            if (c->text && !strcmp (c->text, syms[i][0]))
              {
                o = strlen (out);
                if (o + 1 < n)
                  snprintf (out + o, n - o, "%s", syms[i][1]);
              }
        }
      else
        rich_text (c, out, n);
      o = strlen (out);
      if (c->tail && *c->tail && o + 1 < n)
        snprintf (out + o, n - o, "%s", c->tail);
    }
}

/* A chord's written value (NV_*, notation.h), and how long it lasts in
   quarters (as it says, or as written). */
static int written_value (const gx_node *chord)
{
  static const char *const names[] = { "whole", "half", "quarter", "eighth", "16th", "32nd", "64th", "128th" };
  const char *t = gx_text (chord, "durationType");
  int i;
  for (i = 0; i < 8; i++)
    if (!strcmp (t, names[i]))
      return i | (gx_int (chord, "dots", 0) & 3) << 4;   /* (its dots above) */
  return NV_QUARTER;
}

static double len_of_chord (const gx_node *chord)
{
  if (gx_child (chord, "duration"))
    return fraction_q (gx_text (chord, "duration"));
  return gs_note_value (gx_text (chord, "durationType"), gx_int (chord, "dots", 0));
}

/* Whether a dynamic or hairpin goes over the staff: as it says, else
   over a voice's. */
static int place_above (const gx_node *e, int vocal)
{
  const char *pl = gx_child (e, "placement") ? gx_text (e, "placement") : "";
  return !strcmp (pl, "above") ? 1 : !strcmp (pl, "below") ? 0 : vocal;
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
          const gx_node *sl = gx_child (c, "Slur");
          if (sl && gx_child (sl, "visible") && !gx_int (sl, "visible", 1))
            continue;                    /* (a slur it doesn't show) */
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
              /* (one line through the staff below too: the other hand's notes
                 spread with these) */
              int span = r->voice + gx_int (c, "span", 1) > GR_VPS, v = t == 2 || t == 5 ? -1 : t == 1 || t == 4 ? 2 : 1;
              if (span)
                v = v < 0 ? -1 - GR_ARP_SPAN : v + GR_ARP_SPAN, lo = (r->voice + gx_int (c, "span", 1) - 1) / GR_VPS;
              /* (drawn longer, down, as the score has it: in half spaces) */
              double ul = gx_child (c, "userLen2") ? atof (gx_text (c, "userLen2")) : 0;
              int ext = ul > 0 ? (int) (ul * 2 + 0.5) : 0;
              if (ext > 127)
                ext = 127;
              if (gs_add_mark (r->part, m, pos, -1, 0, GR_MK_ARPEGGIO, v, lo, (r->staff > 0) | ext << 1))
                return -1;
              r->part->marks[r->part->nmarks - 1].hi = hi;
            }
        }
      else if (!strcmp (c->name, "Articulation") || !strcmp (c->name, "Ornament"))
        {
          const char *t = gx_child (c, "subtype") ? gx_text (c, "subtype") : gx_text (c, NULL);
          int kind = strstr (t, "taccat") ? GR_MK_STACCATO
            : strstr (t, "arcato") ? GR_MK_MARCATO
            : strstr (t, "ccent") || strstr (t, "sforzato") ? GR_MK_ACCENT
            : strstr (t, "enuto") ? GR_MK_TENUTO
            : strstr (t, "ermata") ? GR_MK_FERMATA
            : strstr (t, "rill") || strstr (t, "ordent") || strstr (t, "urn") ? GR_MK_ORNAMENT : 0;
          int v = kind == GR_MK_ORNAMENT ? (strstr (t, "rill") ? 1 : strstr (t, "ordent") ? 2 : 3)
            : kind == GR_MK_FERMATA && strstr (t, "elow") ? -1 : kind == GR_MK_FERMATA ? 1 : 0;
          if (kind && gs_add_mark (r->part, m, pos, -1, 0, kind, v, 0, r->staff))
            return -1;
        }
      else if (!strcmp (c->name, "Tremolo") || !strcmp (c->name, "TremoloSingleChord")
               || !strcmp (c->name, "TremoloTwoChord"))
        {
          /* r8..r64 through the stem; c8..c64 between this chord and the next */
          const char *t = gx_child (c, "subtype") ? gx_text (c, "subtype") : gx_text (c, NULL);
          int strokes = strstr (t, "64") ? 4 : strstr (t, "32") ? 3 : strstr (t, "16") ? 2 : strstr (t, "8") ? 1 : 0;
          double l = r->chord_len > 0 ? r->chord_len : len_of_chord (chord);
          if (strokes && *t == 'c')
            {
              if (gs_add_mark (r->part, m, pos, m, pos + 2 * l, GR_MK_TREMOLO, 0x10 | strokes,
                               written_value (chord), r->staff))
                return -1;
            }
          else if (strokes && gs_add_mark (r->part, m, pos, -1, 0, GR_MK_TREMOLO, strokes, 0, r->staff))
            return -1;
        }
      else if (!strcmp (c->name, "Lyrics"))
        {
          char t[64] = "";
          const gx_node *tx = gx_child (c, "text");
          if (tx)
            all_text (tx, t, sizeof t);
          if (gs_add_lyric (r->part, m, pos, r->staff, gx_int (c, "no", 0), gx_text (c, "syllabic"), t))
            return -1;
        }
      else if (!strcmp (c->name, "Note"))
        {
          /* a glissando from this note */
          const gx_node *sp;
          for (sp = gx_child (c, "Spanner"); sp; sp = gx_next (sp, "Spanner"))
            if (gx_attr (sp, "type") && !strcmp (gx_attr (sp, "type"), "Glissando") && gx_child (sp, "next")
                && gs_add_mark (r->part, m, pos, -1, 0, GR_MK_GLISS, 0, gx_int (c, "pitch", 60), r->staff))
              return -1;
        }
    }
  return 0;
}

/* MuseScore 2's voltas: <Volta id> where one starts, and <endSpanner id>
   where it has ended - before the measure's notes if it ended with the
   last measure, else after them. */
/* A measure repeat (the sign for "play the measure, or the two or four,
   before again"): this staff's notes and marks from the measures it
   repeats, copied in. MuseScore 4 counts the measures of one
   (measureRepeatCount) and names how many in a MeasureRepeat in one of
   them; MuseScore 3's RepeatMeasure is one measure. */
static const gx_node *repeat_sign (const gx_node *meas)
{
  const gx_node *v, *x;
  for (v = gx_child (meas, "voice"); v; v = gx_next (v, "voice"))
    if ((x = gx_child (v, "MeasureRepeat")) || (x = gx_child (v, "RepeatMeasure")))
      return x;
  return NULL;
}

static int measure_repeat (reader *r, const gx_node *meas, int m)
{
  const gx_node *x = repeat_sign (meas), *nx;
  int c = gx_int (meas, "measureRepeatCount", 0), n = 0, k, from, nn, nm;
  if (!x && !c)
    {
      r->mrep = 0;
      return 0;
    }
  if (x)
    n = !strcmp (x->name, "RepeatMeasure") ? 1 : gx_int (x, "subtype", 1);
  else
    {
      /* (the sign is in a later measure of the same one) */
      int cc = c;
      for (nx = gx_next (meas, "Measure"); nx && gx_int (nx, "measureRepeatCount", 0) > cc; nx = gx_next (nx, "Measure"))
        {
          cc = gx_int (nx, "measureRepeatCount", 0);
          if ((x = repeat_sign (nx)))
            {
              n = gx_int (x, "subtype", 1);
              break;
            }
        }
    }
  if (n > 0)
    r->mrep = n;
  n = r->mrep;
  from = m - n;
  if (n <= 0 || from < 0)
    return 0;
  nn = r->part->n;
  for (k = 0; k < nn; k++)
    {
      gs_note g = r->part->notes[k];
      if (g.measure != from || g.st != r->staff)
        continue;
      if (gs_add_note (r->part, m, g.off, g.len, g.pitch, g.vel, g.tie, g.wr))
        return -1;
      r->part->notes[r->part->n - 1].st = g.st;
    }
  nm = r->part->nmarks;
  for (k = 0; k < nm; k++)
    {
      gs_mark g = r->part->marks[k];
      if (g.measure != from || (g.emeasure >= 0 && g.emeasure != from) || g.st != r->staff)
        continue;
      if (gs_add_mark (r->part, m, g.off, g.emeasure >= 0 ? m : -1, g.eoff, g.kind, g.value, g.pitch, g.staff))
        return -1;
      r->part->marks[r->part->nmarks - 1].hi = g.hi;
      r->part->marks[r->part->nmarks - 1].st = g.st;
    }
  return 0;
}

/* Jumps and markers (D.C., D.S., Fine, Coda), in the measure or in its
   voices (MuseScore 2). */
static void ms_jumps_in (const gx_node *n, gs_measure *gm)
{
  const gx_node *e;
  for (e = gx_child (n, "Marker"); e; e = gx_next (e, "Marker"))
    gs_measure_label (gm, gx_text (e, "label"));
  for (e = gx_child (n, "Jump"); e; e = gx_next (e, "Jump"))
    {
      const char *to = gx_text (e, "jumpTo"), *until = gx_text (e, "playUntil");
      gm->jump = 1;
      gm->jump_repeats = gx_int (e, "playRepeats", 0) != 0;
      snprintf (gm->jump_to, sizeof gm->jump_to, "%s", *to ? to : "start");
      snprintf (gm->until, sizeof gm->until, "%s", *until ? until : "end");
      snprintf (gm->cont, sizeof gm->cont, "%s", gx_text (e, "continueAt"));
    }
}

static void ms_jumps (const gx_node *meas, gs_measure *gm)
{
  const gx_node *v;
  ms_jumps_in (meas, gm);
  for (v = gx_child (meas, "voice"); v; v = gx_next (v, "voice"))
    ms_jumps_in (v, gm);
}

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

/* MuseScore 2: an 8va line or hairpin (id) ends at m, pos: its mark. -1
   if out of memory; 0, as well, if it wasn't one of them. */
static int ms2_end (reader *r, const char *id, int m, double pos)
{
  int q;
  for (q = 0; q < 8; q++)
    if (r->ms2[q].id[0] && !strcmp (r->ms2[q].id, id))
      {
        r->ms2[q].id[0] = 0;
        /* (an 8va line's end is its last note's start: to the measure's end will do) */
        return gs_add_mark (r->part, r->ms2[q].m, r->ms2[q].pos, m, pos, r->ms2[q].kind, r->ms2[q].value, 0, r->staff);
      }
  return 0;
}

/* A note value's name as NV_* (notation.h: whole 0 .. 128th 7), or -1. */
static int nv_index (const char *t)
{
  static const char *const names[] = { "whole", "half", "quarter", "eighth", "16th", "32nd", "64th", "128th" };
  int i;
  for (i = 0; i < 8; i++)
    if (!strcmp (t, names[i]))
      return i;
  return -1;
}

static int read_voice (reader *r, const gx_node *voice, int vno, int m, double mlen, double mstart)
{
  double pos = 0, factor[MAX_TUPLETS], f = r->stretch > 0 ? r->stretch : 1;
  int ntup = 0, k, ngrace = 0, nchords_g = 0, gpitch[16], gval[16], gord[16];
  int tbits[MAX_TUPLETS];       /* (each open tuplet's bits for its notes) */
  struct { char id[16]; double f; int bits; } ids[MAX_TUPLETS];
  const gs_measure *gm0 = m < r->sc->nmeas ? &r->sc->meas[m] : NULL;
  double beat_q = gm0 && gm0->den > 0 ? (gm0->num % 3 == 0 && gm0->num > 3 && gm0->den >= 8 ? 12.0 : 4.0) / gm0->den : 1;
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
          int bits = gr_tuplet_bits (act, norm, norm * gs_note_value (gx_text (e, "baseNote"), 0), beat_q);
          if (norm > 0 && act > 0)
            tf = (double) norm / act;
          if (gx_attr (e, "id"))
            {
              /* MuseScore 2: chords name their tuplet by id */
              if (nids < MAX_TUPLETS)
                {
                  snprintf (ids[nids].id, sizeof ids[nids].id, "%s", gx_attr (e, "id"));
                  ids[nids].bits = bits;
                  ids[nids++].f = tf;
                }
            }
          else if (ntup < MAX_TUPLETS)
            {
              /* where it is, for the Score window to write it there (its
                 notes in the time of norm of its base note) */
              int bn = nv_index (gx_text (e, "baseNote"));
              double tl = norm * gs_note_value (gx_text (e, "baseNote"), 0) * f;
              if (bn >= 0 && act > 0 && act < 256 && norm > 0 && norm < 128 && tl > 0
                  && gs_add_mark (r->part, m, pos, m, pos + tl, GR_MK_TUPLET,
                                  (vno & 3) | bn << 2 | (ntup ? 0x20 : 0)
                                  | (gx_int (e, "numberType", 0) == 2 || !gx_int (e, "visible", 1)
                                     || (gx_child (e, "Number") && !gx_int (gx_child (e, "Number"), "visible", 1)) ? 0x40 : 0)
                                  | (gx_int (e, "bracketType", 0) == 2 || !gx_int (e, "visible", 1) ? 0x80 : 0),
                                  act, norm << 1 | (r->staff > 0)))
                return -1;
              tbits[ntup] = bits;
              factor[ntup++] = tf;
              f *= tf;
            }
        }
      else if (!strcmp (n, "endTuplet"))
        {
          if (ntup > 0)
            f /= factor[--ntup];
        }
      else if (!strcmp (n, "Clef") && vno == 0)
        {
          int c = clef_elem (e);
          if (c && (pos > 0 || m > 0) && gs_add_mark (r->part, m, pos, -1, 0, GR_MK_CLEF, c - 1, 0, r->staff))
            return -1;
          if (c && pos == 0 && m == 0 && r->staff < GS_STAVES)
            r->part->clef[r->staff] = c;
        }
      else if (!strcmp (n, "KeySig") && vno == 0)
        {
          /* the key as it sounds (MuseScore 4), or as written (3 and
             older): each staff's says which it is, and the most win (a
             transposing part, or an empty staff in C, is outvoted) */
          gs_measure *gm = gs_measure_at (r->sc, m);
          const char *ck = gx_child (e, "concertKey") ? "concertKey" : gx_child (e, "accidental") ? "accidental" : NULL;
          int f = ck ? gx_int (e, ck, 0) : 0;
          if (!gm)
            return -1;
          if ((ck || !gx_child (e, "custom")) && f >= -7 && f <= 7)
            {
              if (!gm->has_key)
                gm->minor = !strcmp (gx_text (e, "mode"), "minor");
              gm->has_key = 1;
              gm->kvotes[f + 7]++;
            }
        }
      else if (!strcmp (n, "Fermata"))
        {
          /* (MuseScore 3 and 4: before the chord or rest it's over) */
          const char *t = gx_child (e, "subtype") ? gx_text (e, "subtype") : "";
          if (gs_add_mark (r->part, m, pos, -1, 0, GR_MK_FERMATA, strstr (t, "elow") ? -1 : 1, 0, r->staff))
            return -1;
        }
      else if (!strcmp (n, "Spanner") && gx_attr (e, "type") && !strcmp (gx_attr (e, "type"), "Trill")
               && gx_child (e, "next"))
        {
          if (gs_add_mark (r->part, m, pos, -1, 0, GR_MK_ORNAMENT, 1, 0, r->staff))
            return -1;
        }
      else if ((!strcmp (n, "StaffText") || !strcmp (n, "Expression") || (!strcmp (n, "SystemText") && r->first_staff))
               && (!gx_child (e, "visible") || gx_int (e, "visible", 1)))
        {
          /* words over or under the staff ("dolce", "sostenuto", "rit.") */
          char t[64] = "";
          const gx_node *tx = gx_child (e, "text");
          const char *pl = gx_text (e, "placement"), *st = gx_text (e, "style");
          const gx_node *ps = gx_child (e, "pos") ? gx_child (e, "pos") : gx_child (e, "offset");
          /* (where: as it says; else MuseScore 2's moved down under the staff; else by kind) */
          int below = *pl ? !strcmp (pl, "below")
            : ps && gx_attr (ps, "y") ? atof (gx_attr (ps, "y")) > 2
            : !strcmp (n, "Expression") || !strcmp (st, "Expression");
          char *c;
          if (tx)
            all_text (tx, t, sizeof t);
          for (c = t; *c; c++)
            if (*c == '\n' || *c == '\r' || *c == '\t')
              *c = ' ';
          {
            char *w = t;      /* (runs of spaces as one) */
            for (c = t; *c; c++)
              if (*c != ' ' || (w > t && w[-1] != ' '))
                *w++ = *c;
            *w = 0;
          }
          c = t;
          if (*c && gs_add_lyric (r->part, m, pos, r->staff, below ? GR_TEXT_BELOW : GR_TEXT_ABOVE, NULL, c))
            return -1;
        }
      else if (!strcmp (n, "Dynamic"))
        {
          int v = gx_int (e, "velocity", 0), d = dyn_level (gx_text (e, "subtype"));
          if (v > 0)
            r->vel = v > 127 ? 127 : v;
          if (!d && gx_child (e, "text"))
            {
              /* a dynamic written as words: its symbols ("p"), then the
                 rest of them under the staff ("p sostenuto") */
              const gx_node *tx = gx_child (e, "text"), *c;
              char dn[8] = "", words[64] = "";
              size_t k2 = 0;
              if (tx->text)
                snprintf (words, sizeof words, "%s", tx->text);
              for (c = tx->child; c; c = c->next)
                if (!strcmp (c->name, "sym") && c->text && k2 + 1 < sizeof dn)
                  {
                    const char *sy = c->text;
                    char ch = !strcmp (sy, "dynamicPiano") ? 'p' : !strcmp (sy, "dynamicForte") ? 'f'
                      : !strcmp (sy, "dynamicMezzo") ? 'm' : 0;
                    if (ch)
                      dn[k2++] = ch, dn[k2] = 0;
                  }
                else if (c->text && strlen (words) + strlen (c->text) + 1 < sizeof words)
                  strcat (words, c->text);
              d = dyn_level (dn);
              {
                char *w = words;
                while (*w == ' ')
                  w++;
                if (*w && strlen (w) > 1 && gs_add_lyric (r->part, m, pos, r->staff, GR_TEXT_BELOW, NULL, w))
                  return -1;
              }
            }
          if (d && (!gx_child (e, "visible") || gx_int (e, "visible", 1))
              && gs_add_mark (r->part, m, pos, -1, 0, GR_MK_DYNAMIC, d, place_above (e, r->part->vocal), r->staff))
            return -1;
        }
      else if (!strcmp (n, "Tempo"))
        {
          /* (in beats a second, in quarters; written on the first staff) */
          double t = gx_num (e, "tempo", 0);
          /* its text ("Andante.", "Andante. (\u2669 = 66)"), over the staff,
             in bold (a metronome mark alone too, as the score writes it) */
          if (r->first_staff && gx_child (e, "text") && (!gx_child (e, "visible") || gx_int (e, "visible", 1)))
            {
              char w[160] = "", *c, *o;
              int mark = 0;
              rich_text (gx_child (e, "text"), w, sizeof w);
              /* (on one line, single spaces, none at the ends) */
              for (c = o = w; *c; c++)
                {
                  char ch = *c == '\n' || *c == '\r' || *c == '\t' ? ' ' : *c;
                  if (ch == ' ' && (o == w || o[-1] == ' '))
                    continue;
                  *o++ = ch;
                }
              while (o > w && o[-1] == ' ')
                o--;
              *o = 0;
              /* (a note, in Unicode or the music font's own) */
              for (c = w; *c; c++)
                if ((unsigned char) c[0] == 0xE2 && (unsigned char) c[1] == 0x99 && ((unsigned char) c[2] == 0xA9 || (unsigned char) c[2] == 0xAA))
                  mark = 1;
                else if ((unsigned char) c[0] == 0xEE && ((unsigned char) c[1] == 0xB2 || (unsigned char) c[1] == 0x87))
                  mark = 1;
                else if ((unsigned char) c[0] == 0xF0 && (unsigned char) c[1] == 0x9D && (unsigned char) c[2] == 0x85)
                  mark = 1;
              if (!mark && strchr (w, '=') && strlen (w) < sizeof w - 4)
                {
                  /* ("= 72" with its note lost: the crotchet) */
                  memmove (w + 3, w, strlen (w) + 1);
                  memcpy (w, "\xE2\x99\xA9", 3);
                  mark = 1;
                }
              if (strlen (w) > 1 && gs_add_lyric (r->part, m, pos, 0, mark ? GR_TEXT_TEMPO_MARK : GR_TEXT_TEMPO, NULL, w))
                return -1;
            }
          if (t > 0 && (r->first_staff || r->sc->tempo == 0))
            {
              /* (a metronome mark written: "= 72" in its words, or a note symbol) */
              const gx_node *tx = gx_child (e, "text");
              char all[128] = "";
              int shown;
              if (tx)
                all_text (tx, all, sizeof all);
              shown = (!gx_child (e, "visible") || gx_int (e, "visible", 1))
                && (!tx || strchr (all, '=') || strstr (all, "metNote") != NULL);
              if (gs_add_tempo (r->sc, m, pos, t * 60))
                return -1;
              if (r->sc->ntempos > 0)
                r->sc->tempos[r->sc->ntempos - 1].hidden = !shown;
            }
        }
      else if (!strcmp (n, "Spanner") && gx_attr (e, "type")
               && (!strcmp (gx_attr (e, "type"), "HairPin") || !strcmp (gx_attr (e, "type"), "Ottava")))
        {
          int em, hair = !strcmp (gx_attr (e, "type"), "HairPin"), v;
          double eoff;
          const gx_node *h = gx_child (e, hair ? "HairPin" : "Ottava");
          if (h && span_end (e, m, pos, &em, &eoff) && (!gx_child (h, "visible") || gx_int (h, "visible", 1)))
            {
              if (hair)
                {
                  /* subtype 0, 2: crescendo (a hairpin, or "cresc."); 1, 3: diminuendo */
                  v = (gx_int (h, "subtype", 0) & 1 ? 2 : 1) | (place_above (h, r->part->vocal) ? GR_MK_ABOVE : 0);
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
          if (gx_child (e, "Pedal"))
            {
              const gx_node *pd = gx_child (e, "Pedal");
              int em, hide = 0;
              double eoff;
              if (gx_child (pd, "visible") && !gx_int (pd, "visible", 1))
                {
                  r->part->pedal_hidden++;
                  hide = 3;
                }
              else
                {
                  r->part->pedal_shown++;
                  /* (MuseScore's own way: a line with no text; else its text, its
                     star if it ends with one) */
                  int line = !gx_child (pd, "lineVisible") || gx_int (pd, "lineVisible", 1);
                  if (!gx_child (pd, "beginText") && !gx_child (pd, "lineVisible"))
                    hide = 4 | 3;               /* (MuseScore's own way: a line, hooked) */
                  else
                    {
                      if (!gx_child (pd, "endText") || !strstr (gx_text (pd, "endText"), "PedalUp"))
                        hide = 2;
                      if (!gx_child (pd, "beginText") || !strstr (gx_text (pd, "beginText"), "PedalPed"))
                        hide |= 1;
                      /* (its words: "Con" Ped., "simile"; "Ped." written out as
                         the sign) */
                      if (gx_child (pd, "beginText"))
                        {
                          const char *bt = gx_text (pd, "beginText");
                          char w[GR_LYRIC], *o = w;
                          int intag = 0;
                          for (; *bt && o < w + sizeof w - 1; bt++)
                            if (*bt == '<')
                              {
                                intag = 1;
                                if (!strncmp (bt, "<sym>", 5))
                                  {
                                    const char *z = strstr (bt, "</sym>");
                                    bt = z ? z + 5 : bt + strlen (bt) - 1;
                                    intag = 0;
                                  }
                              }
                            else if (*bt == '>')
                              intag = 0;
                            else if (!intag)
                              *o++ = *bt;
                          *o = 0;
                          while (o > w && o[-1] == ' ')
                            *--o = 0;
                          for (o = w; *o == ' '; o++)
                            ;
                          if (!strcmp (o, "Ped.") || !strcmp (o, "Ped"))
                            hide &= ~1;
                          else if (*o && gs_add_lyric (r->part, m, pos, r->staff > 0, GR_TEXT_PEDAL, NULL, o))
                            return -1;
                        }
                      if (line)
                        hide |= 4;
                    }
                }
              /* (over the staff, if the score puts it there) */
              if (hide != 3 && gx_child (pd, "placement") && !strcmp (gx_text (pd, "placement"), "above"))
                hide |= 8;
              /* (where, for the ones shown either side) */
              if (hide && span_end (e, m, pos, &em, &eoff)
                  && gs_add_mark (r->part, m, pos, em, eoff, GR_MK_NOPEDAL, hide, 0, 0))
                return -1;
            }
        }
      else if (!strcmp (n, "Pedal") && gx_attr (e, "id"))
        {
          snprintf (r->pedal_id, sizeof r->pedal_id, "%s", gx_attr (e, "id"));   /* MuseScore 2 */
          if (gs_add_pedal (r->part, m, pos, 1))
            return -1;
          if (gx_child (e, "visible") && !gx_int (e, "visible", 1))
            r->part->pedal_hidden++;
          else
            r->part->pedal_shown++;
        }
      else if ((!strcmp (n, "Ottava") || !strcmp (n, "HairPin")) && gx_attr (e, "id"))
        {
          /* MuseScore 2: from here to its endSpanner */
          int q, v = !strcmp (n, "Ottava") ? ottava_shift (gx_text (e, "subtype")) : gx_int (e, "subtype", 0) & 1 ? 2 : 1;
          for (q = 0; q < 8 && r->ms2[q].id[0]; q++)
            ;
          if (q < 8 && v)
            {
              snprintf (r->ms2[q].id, sizeof r->ms2[q].id, "%s", gx_attr (e, "id"));
              r->ms2[q].kind = !strcmp (n, "Ottava") ? GR_MK_OTTAVA : GR_MK_HAIRPIN;
              r->ms2[q].value = v;
              r->ms2[q].m = m;
              r->ms2[q].pos = pos;
            }
        }
      else if (!strcmp (n, "endSpanner") && gx_attr (e, "id") && ms2_end (r, gx_attr (e, "id"), m, pos))
        return -1;
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
                {
                  /* (and part of a bar more: "31 measures, 5/8" ends
                     in the 32nd, so covers it) */
                  span = gx_int (loc, "measures", 1);
                  if (gx_child (loc, "fractions") && fraction_q (gx_text (loc, "fractions")) > 0)
                    span++;
                }
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
      else if (!strcmp (n, "Beam"))
        {
          /* a beam's settings, before its first chord: its stems */
          const char *sd = gx_child (e, "StemDirection") ? gx_text (e, "StemDirection") : "";
          r->beam_stem = !strcmp (sd, "up") ? 1 : !strcmp (sd, "down") ? 2 : 0;
        }
      else if (!strcmp (n, "Chord") || !strcmp (n, "Rest"))
        {
          /* (MuseScore 2 says each chord's and rest's voice by its track:
             four a staff) */
          int cv = gx_child (e, "track") ? gx_int (e, "track", 0) & 3 : vno;
          const char *dt = gx_text (e, "durationType");
          double len, cf = f;
          const gx_node *tn = gx_child (e, "Tuplet");
          int tb = ntup > 0 ? tbits[ntup - 1] : 0;
          if (tn && *gx_text (tn, NULL))
            for (k = 0; k < nids; k++)
              if (!strcmp (ids[k].id, gx_text (tn, NULL)))
                {
                  cf *= ids[k].f;
                  tb = ids[k].bits;
                }
          if (gx_child (e, "duration"))
            len = fraction_q (gx_text (e, "duration")) * cf;   /* (in its tuplet, its staff's time too) */
          else if (!strcmp (dt, "measure"))
            len = mlen;
          else
            len = gs_note_value (dt, gx_int (e, "dots", 0)) * cf;
          if (!strcmp (n, "Chord") && !is_grace (e) && (gx_child (e, "StemDirection") || r->beam_stem))
            {
              /* its stem, as the score sets it (or its beam does) */
              const char *sd = gx_child (e, "StemDirection") ? gx_text (e, "StemDirection") : "";
              int dir = !strcmp (sd, "up") ? 1 : !strcmp (sd, "down") ? 2 : r->beam_stem;
              int mv = gx_int (e, "staffMove", 0) + gx_int (e, "move", 0);
              r->beam_stem = 0;
              r->part->cur_staff = r->staff + mv < 0 ? 0 : r->staff + mv;
              if (dir && gs_add_mark (r->part, m, pos, -1, 0, GR_MK_STEM, dir | (cv & 3) << 3, 0, r->staff + mv > 0))
                return -1;
              r->part->cur_staff = r->staff;
            }
          if (!strcmp (n, "Chord") && gx_child (e, "BeamMode") && !is_grace (e))
            {
              /* how the score beams it */
              const char *bm = gx_text (e, "BeamMode");
              int mode = !strcmp (bm, "begin") ? GR_BEAM_BEGIN : !strcmp (bm, "no") ? GR_BEAM_NONE
                : !strcmp (bm, "end") ? GR_BEAM_END : !strncmp (bm, "begin", 5) || !strcmp (bm, "mid") ? GR_BEAM_MID : 0;
              int mv = gx_int (e, "staffMove", 0) + gx_int (e, "move", 0);
              r->part->cur_staff = r->staff + mv < 0 ? 0 : r->staff + mv;
              if (mode && gs_add_mark (r->part, m, pos, -1, 0, GR_MK_BEAM, mode | (cv & 3) << 3, 0, r->staff + mv > 0))
                return -1;
              r->part->cur_staff = r->staff;
            }
          if (!strcmp (n, "Chord"))
            {
              const gx_node *note, *ar;
              double snd = len;
              if (is_grace (e))
                {
                  /* written small before the next chord (not played) */
                  const char *dt2 = gx_text (e, "durationType");
                  int gv = gx_child (e, "grace16") || !strcmp (dt2, "16th") ? NV_16TH
                    : gx_child (e, "grace32") || !strcmp (dt2, "32nd") ? NV_32ND
                    : gx_child (e, "grace4") || !strcmp (dt2, "quarter") ? NV_QUARTER : NV_EIGHTH;
                  if (gx_child (e, "acciaccatura"))
                    gv |= 0x40;
                  if (!gx_child (e, "grace8after") && !gx_child (e, "grace16after") && !gx_child (e, "grace32after"))
                    for (note = gx_child (e, "Note"); note; note = gx_next (note, "Note"))
                      if (ngrace < 16)
                        {
                          gpitch[ngrace] = gx_int (note, "pitch", 60);
                          gval[ngrace] = gv;
                          gord[ngrace++] = nchords_g;
                        }
                  nchords_g++;
                  continue;
                }
              for (k = 0; k < ngrace; k++)
                if (gs_add_mark (r->part, m, pos, -1, 0, GR_MK_GRACE, gval[k], gpitch[k], r->staff))
                  return -1;
                else
                  r->part->marks[r->part->nmarks - 1].hi = gord[k];   /* (its place among them) */
              ngrace = nchords_g = 0;
              r->voice = cv;
              r->chord_len = len;
              if (chord_marks (r, e, m, pos))
                return -1;
              /* staccato: half its value, as MuseScore plays it */
              for (ar = gx_child (e, "Articulation"); ar; ar = gx_next (ar, "Articulation"))
                if (strstr (gx_text (ar, "subtype"), "taccat") || strstr (gx_text (ar, NULL), "taccat"))
                  snd = len / 2;
              for (note = gx_child (e, "Note"); note; note = gx_next (note, "Note"))
                {
                  int pitch = gx_int (note, "pitch", -1), vel = r->vel;
                  int move = gx_int (e, "staffMove", 0) + gx_int (e, "move", 0);
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
                  /* its spelling: tpc counts fifths from C's 14 (F 13 ...
                                     B 19 natural, then the sharps; the flats below) */
                  int tpc = gx_child (note, "tpc") ? gx_int (note, "tpc", 14) : -99, sp = 0;
                  if (tpc >= -1 && tpc <= 33)
                    {
                      int alter = (tpc + 1) / 7 - 2;
                      sp = GR_SP | (alter + 2);
                    }
                  if (gx_child (note, "visible") && !gx_int (note, "visible", 1))
                    sp |= GR_SP_HIDDEN;             /* (played, not shown) */
                  r->part->cur_staff = r->staff + move < 0 ? 0 : r->staff + move;
                  if (gx_child (note, "play") && !gx_int (note, "play", 1))
                    {
                      /* a note shown but not played (a cue note, another
                         verse's): written all the same */
                      if (pitch >= 0 && pitch <= 127
                          && gs_add_mark (r->part, m, pos, m, pos + len, GR_MK_NOTE,
                                          (cv & 3) | (sp & 0x1f) << 2 | (gx_int (e, "small", 0) || gx_int (note, "small", 0) ? 0x80 : 0), pitch,
                                          gr_mknote_staff ((r->staff + move > 0 ? GR_WR_STAFF : 0) | (move ? GR_WR_CROSS : 0) | tb, 0)))
                        return -1;
                      continue;
                    }
                  if (gs_add_note (r->part, m, pos, snd, pitch, vel, note_tie (note),
                                   GR_WR | (cv & GR_WR_VOICE) | (move ? GR_WR_CROSS : 0)
                                   | (r->staff + move > 0 ? GR_WR_STAFF : 0)
                                   | (snd < len ? GR_WR_HALF : 0) | sp << 8 | tb))
                    return -1;
                }
              r->part->cur_staff = r->staff;
            }
          else if (!gx_child (e, "visible") || gx_int (e, "visible", 1))
            {
              /* a rest the score shows (in its voice) */
              int rst = r->staff + gx_int (e, "staffMove", 0);
              r->part->cur_staff = rst < 0 ? 0 : rst;
              if (gs_add_mark (r->part, m, pos, m, pos + len, GR_MK_REST, (cv & 3) | gr_rest_tuplet (tb), 0,
                               r->staff + gx_int (e, "staffMove", 0) > 0))
                return -1;
              r->part->cur_staff = r->staff;
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
      /* its name as the score prints it (longName), else its track's */
      const char *name = ins ? gx_text (ins, "longName") : "";
      gs_part *p;
      if (!*name)
        name = gx_text (e, "trackName");
      if (!*name && ins)
        name = gx_text (ins, "trackName");
      if (!*name && ins && gx_child (ins, "InstrumentLabel"))          /* MuseScore 4.4+ */
        name = gx_text (gx_child (ins, "InstrumentLabel"), "longName");
      p = gs_add_part (sc, name);
      if (!p)
        goto nomem;
      /* (an organ's three staves: manuals braced, pedals apart) */
      if (ins && (strstr (gx_text (ins, "instrumentId"), "organ") || strstr (gx_attr (ins, "id") ? gx_attr (ins, "id") : "", "organ")))
        p->organ = 1;
      /* (a voice's: dynamics over the staff, as MuseScore puts them) */
      if (ins && gx_attr (ins, "id"))
        {
          static const char *const voices[] = { "voice", "women", "men", "soprano", "mezzo-soprano", "alto", "contralto",
                                                "countertenor", "tenor", "baritone", "bass", "boy-soprano", "choir" };
          size_t q;
          for (q = 0; q < sizeof voices / sizeof voices[0]; q++)
            if (!strcmp (gx_attr (ins, "id"), voices[q]))
              p->vocal = 1;
        }
      if (ins && gx_int (ins, "useDrumset", 0))
        p->drums = 1;
      /* its kit: where each drum is written */
      if (ins && gx_child (ins, "Drum"))
        {
          gx_node *dr;
          for (dr = gx_child (ins, "Drum"); dr; dr = gx_next (dr, "Drum"))
            {
              int k = gx_attr (dr, "pitch") ? atoi (gx_attr (dr, "pitch")) : -1, ln = gx_int (dr, "line", -100);
              if (k >= 0 && k < 128 && ln > -60 && ln < 60)
                p->drumline[k] = (unsigned char) (ln + 64);
            }
        }
      /* how it's written: transposeChromatic is how far it sounds from
         that (a B flat clarinet's -2), and its clefs */
      p->has_transpose = 1;
      p->transpose = ins ? -gx_int (ins, "transposeChromatic", 0) : 0;
      /* (a score shown in concert pitch: written as it sounds, as MuseScore
         shows and exports it) */
      if (gx_child (score, "Style") && gx_int (gx_child (score, "Style"), "concertPitch", 0))
        p->transpose = 0;
      if (ins)
        {
          gx_node *c;
          for (c = ins->child; c; c = c->next)
            if (!strcmp (c->name, "clef") || !strcmp (c->name, "transposingClef")
                || (!strcmp (c->name, "concertClef") && !gx_child (ins, "transposingClef")))
              {
                int z = gx_attr (c, "staff") ? atoi (gx_attr (c, "staff")) - 1 : 0;
                if (z >= 0 && z < GS_STAVES && clef_of (gx_text (c, NULL)))
                  p->clef[z] = clef_of (gx_text (c, NULL));
              }
        }
      for (st = gx_child (e, "Staff"); st; st = gx_next (st, "Staff"))
        {
          const gx_node *stt = gx_child (st, "StaffType");
          const char *id = gx_attr (st, "id");
          int sid = id ? atoi (id) : nstaff + 1;
          if (stt && gx_attr (stt, "group") && !strcmp (gx_attr (stt, "group"), "percussion"))
            p->drums = 1;
          if (stt && gx_int (stt, "lines", 5) == 1)
            p->lines = 1;
          /* (MuseScore 3 and 4 may give a staff's clef here instead) */
          {
            int z = 0, k2, c = clef_of (gx_text (st, "defaultTransposingClef"));
            if (!c) c = clef_of (gx_text (st, "defaultClef"));
            if (!c) c = clef_of (gx_text (st, "defaultConcertClef"));
            for (k2 = sid - 1; k2 >= 1 && staff_part[k2] == sc->nparts - 1; k2--)
              z++;
            if (c && z < GS_STAVES && !p->clef[z])
              p->clef[z] = c;
          }
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
      /* (a drum on a staff of one line is on it: written where MusicXML
         has that line, a five-line staff's bottom one) */
      if (p->lines == 1)
        {
          int k;
          for (k = 0; k < 128; k++)
            if (p->drumline[k])
              p->drumline[k] = (unsigned char) (p->drumline[k] + 8);
        }
    }
  /* The staves' music. The first staff sets the measures. */
  for (k = 0; k < sc->nmeas; k++)
    memset (sc->meas[k].kvotes, 0, sizeof sc->meas[k].kvotes);
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
      r.part->cur_staff = r.staff;
      r.vel = 80;
      r.pending_volta_end = 0;
      r.mrep = 0;
      r.stretch = 1;
      for (meas = gx_child (e, "Measure"); meas; meas = gx_next (meas, "Measure"), m++)
        {
          /* MuseScore 3 keeps a multi-measure rest as a measure of its own
             (for the layout) before the measures it stands for. */
          if (gx_int (meas, "multiMeasureRest", 0) > 0)
            {
              m--;
              continue;
            }
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
          /* (a time signature of this staff's own, with the score's
             stretched to fit it: 3/8 over the score's 9/16) */
          if (ts)
            {
              int sn = gx_int (ts, "stretchN", 0), sd = gx_int (ts, "stretchD", 0);
              r.stretch = sn > 0 && sd > 0 ? (double) sd / sn : 1;
            }
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
                  int j, sn = gx_int (ts, "stretchN", 0), sd = gx_int (ts, "stretchD", 0);
                  gm->num = gx_int (ts, "sigN", 4);
                  gm->den = gx_int (ts, "sigD", 4);
                  gm->timesym = gx_int (ts, "subtype", 0) == 1 ? 1 : gx_int (ts, "subtype", 0) == 2 ? 2 : 0;
                  if (sn > 0 && sd > 0 && sn != sd)
                    {
                      /* (the score's own, which this staff's is stretched from) */
                      int g = 1, a = gm->num * sd, b = gm->den * sn, x, y;
                      for (x = a, y = b; y; )
                        {
                          int t = x % y;
                          x = y;
                          y = t;
                        }
                      g = x > 0 ? x : 1;
                      /* (as the score has it, 12/8, not 3/2: in lowest terms only
                         if it must be) */
                      while (g > 1 && !(g % 2))
                        g /= 2;                    /* (its odd part: 9/24 to 3/8) */
                      a /= g;
                      b /= g;
                      while (b > 64 && !(a % 2) && !(b % 2))
                        a /= 2, b /= 2;
                      if (b > 0 && b <= 64 && !(b & (b - 1)))
                        gm->num = a, gm->den = b;
                    }
                  gm->len = gm->num * 4.0 / gm->den;
                  for (j = m + 1; j < sc->nmeas; j++)
                    {
                      sc->meas[j].num = gm->num;
                      sc->meas[j].den = gm->den;
                      sc->meas[j].timesym = gm->timesym;
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
              if (read_voice (&r, v, voices++, m, sc->meas[m].len, mstart))
                goto nomem;
            }
          if (!voices && read_voice (&r, meas, 0, m, sc->meas[m].len, mstart))   /* MuseScore 2 */
            goto nomem;
          if (measure_repeat (&r, meas, m))
            goto nomem;
          if (r.first_staff)
            {
              ms2_volta (&r, meas, &sc->meas[m]);
              ms_jumps (meas, &sc->meas[m]);
              const gx_node *lb;
              for (lb = gx_child (meas, "LayoutBreak"); lb; lb = gx_next (lb, "LayoutBreak"))
                if (!strcmp (gx_text (lb, "subtype"), "section"))
                  sc->meas[m].section_end = 1;
            }
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
  /* MuseScore keeps a note under an 8va line at the pitch it would have
     without the line (as written): it sounds an octave (or two) away */
  for (k = 0; k < sc->nparts; k++)
      {
        gs_part *p = &sc->parts[k];
        int i, j;
        for (i = 0; i < p->nmarks; i++)
          {
            const gs_mark *o = &p->marks[i];
            if (o->kind != GR_MK_OTTAVA || o->emeasure < 0)
              continue;
            for (j = 0; j < p->n; j++)
              {
                gs_note *n = &p->notes[j];
                int st = (n->wr & GR_WR_STAFF) != 0;
                if (st != (o->staff > 0)
                    || n->measure < o->measure || (n->measure == o->measure && n->off < o->off - 1e-9)
                    || n->measure > o->emeasure || (n->measure == o->emeasure && n->off >= o->eoff - 1e-9))
                  continue;
                n->pitch += 12 * o->value;
                if (n->pitch < 0) n->pitch = 0;
                if (n->pitch > 127) n->pitch = 127;
              }
            /* (grace notes too) */
            for (j = 0; j < p->nmarks; j++)
              {
                gs_mark *g = &p->marks[j];
                if (g->kind != GR_MK_GRACE || (g->staff > 0) != (o->staff > 0)
                    || g->measure < o->measure || (g->measure == o->measure && g->off < o->off - 1e-9)
                    || g->measure > o->emeasure || (g->measure == o->emeasure && g->off >= o->eoff - 1e-9))
                  continue;
                g->pitch += 12 * o->value;
                g->pitch = g->pitch < 0 ? 0 : g->pitch > 127 ? 127 : g->pitch;
              }
          }
      }
  /* each measure's key signature: the one most staves have */
  for (k = 0; k < sc->nmeas; k++)
    if (sc->meas[k].has_key)
      {
        int j, best = 7;
        for (j = 0; j < 15; j++)
          if (sc->meas[k].kvotes[j] > sc->meas[k].kvotes[best])
            best = j;
        sc->meas[k].fifths = best - 7;
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

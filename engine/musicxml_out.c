/* Groove - writing the song as MusicXML, for MuseScore and other notation
   programs: the written music that notation.c makes from the piano roll
   (the same the Score window shows), one part per drum or instrument
   track with notes, with each note's velocity as its "dynamics". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "notation.h"


/* MusicXML divisions a crotchet: five times Groove's ticks, so a
   quintuplet's notes (a fifth of a beat) come out whole. */
#define XDIV (GR_PPQ * 5)

/* An event's length in divisions: a quintuplet note's from its written
   value (its ticks are rounded), anything else's from its ticks. */
static int xdur (const gr_nevent *e)
{
  if (e->tuplet && e->tup_n == 5)
    {
      int t = gr_nvalue_ticks (e->value);
      if (e->dots)
        t += t / 2;
      return t * 4;
    }
  return e->dur * 5;
}

typedef struct
{
  FILE *f;
  int err;
} out;

static void w (out *o, const char *fmt, ...) __attribute__ ((format (printf, 2, 3)));
#include <stdarg.h>
static void w (out *o, const char *fmt, ...)
{
  va_list a;
  va_start (a, fmt);
  if (vfprintf (o->f, fmt, a) < 0)
    o->err = 1;
  va_end (a);
}

static void w_text (out *o, const char *s)
{
  for (; *s; s++)
    switch (*s)
      {
      case '<': w (o, "&lt;"); break;
      case '>': w (o, "&gt;"); break;
      case '&': w (o, "&amp;"); break;
      case '"': w (o, "&quot;"); break;
      default:
        if ((unsigned char) *s >= 0x80)
          w (o, "&#%d;", (unsigned char) *s);   /* Latin-1, as RISC OS writes it */
        else
          fputc (*s, o->f);
      }
}

static void leaf_title (const char *path, char *title, size_t n)
{
  const char *p = path, *s;
#ifdef __riscos
  for (s = path; *s; s++)
    if (*s == '.' || *s == ':')
      p = s + 1;
  snprintf (title, n, "%s", p);
  if ((s = strchr (title, '/')))
    *(char *) s = 0;
#else
  for (s = path; *s; s++)
    if (*s == '/')
      p = s + 1;
  snprintf (title, n, "%s", p);
  if ((s = strrchr (title, '.')))
    *(char *) s = 0;
#endif
}


static const char *const head_name[] = { NULL, "x", "circle-x", "diamond", "inverted triangle", "triangle",
                                          "slashed1", "slash" };

/* A direction under the staff: a hairpin, or the pedal (offset: ticks on
   from here; sound: the pedal's sound, 1 down, 0 up, -1 none). */
static void w_direction (out *o, const char *what, int voice, int staff, int staves, int offset)
{
  w (o, "      <direction placement=\"below\">\n        <direction-type>%s</direction-type>\n", what);
  if (offset)
    w (o, "        <offset>%d</offset>\n", offset);
  if (voice)
    w (o, "        <voice>%d</voice>\n", voice);
  if (staves > 1)
    w (o, "        <staff>%d</staff>\n", staff);
  w (o, "      </direction>\n");
}

static void w_event (out *o, const gr_notation *nt, const gr_nevent *e, int voice, int staff, int staves,
                     int drums, const char *pid)
{
  int k, n = e->kind == NE_NOTE ? e->nheads : 1;
  if (e->kind == NE_SPACE)
    {
      w (o, "      <forward><duration>%d</duration><voice>%d</voice>", xdur (e), voice);
      if (staves > 1)
        w (o, "<staff>%d</staff>", staff);
      w (o, "</forward>\n");
      return;
    }
  if (e->kind == NE_BAR_REST)
    {
      w (o, "      <note>\n        <rest measure=\"yes\"/>\n        <duration>%d</duration>\n"
         "        <voice>%d</voice>\n", xdur (e), voice);
      if (staves > 1)
        w (o, "        <staff>%d</staff>\n", staff);
      w (o, "      </note>\n");
      return;
    }
  if (e->wedge & NW_STOP)
    w_direction (o, "<wedge type=\"stop\" number=\"1\"/>", voice, staff, staves, 0);
  if (e->dyn)
    {
      w (o, "      <direction placement=\"below\">\n        <direction-type><dynamics><%s/></dynamics></direction-type>\n"
         "        <voice>%d</voice>\n", gr_ndyn_name (e->dyn), voice);
      if (staves > 1)
        w (o, "        <staff>%d</staff>\n", staff);
      w (o, "      </direction>\n");
    }
  if (e->wedge & (NW_CRESC | NW_DIM))
    w_direction (o, e->wedge & NW_CRESC ? "<wedge type=\"crescendo\" number=\"1\"/>"
                 : "<wedge type=\"diminuendo\" number=\"1\"/>", voice, staff, staves, 0);
  for (k = 0; k < n; k++)
    {
      const gr_nhead *h = e->kind == NE_NOTE ? &nt->hd[e->head + k] : NULL;
      w (o, "      <note");
      if (h)
        w (o, " dynamics=\"%.2f\"", h->vel * 100.0 / 90);
      if (h && h->release)
        w (o, " release=\"%d\"", h->release * 5);
      w (o, ">\n");
      if (k)
        w (o, "        <chord/>\n");
      if (!h)
        w (o, "        <rest/>\n");
      else if (drums)
        w (o, "        <unpitched><display-step>%c</display-step><display-octave>%d</display-octave></unpitched>\n",
           "CDEFGAB"[h->step], h->octave);
      else
        {
          w (o, "        <pitch><step>%c</step>", "CDEFGAB"[h->step]);
          if (h->alter)
            w (o, "<alter>%d</alter>", h->alter);
          w (o, "<octave>%d</octave></pitch>\n", h->octave);
        }
      w (o, "        <duration>%d</duration>\n", xdur (e));
      if (h && h->tie_stop)
        w (o, "        <tie type=\"stop\"/>\n");
      if (h && h->tie_start)
        w (o, "        <tie type=\"start\"/>\n");
      if (h && drums)
        w (o, "        <instrument id=\"%s-I%d\"/>\n", pid, h->pitch + 1);
      w (o, "        <voice>%d</voice>\n", voice);
      w (o, "        <type>%s</type>\n", gr_nvalue_name (e->value));
      if (e->dots)
        w (o, "        <dot/>\n");
      if (e->tuplet)
        w (o, "        <time-modification><actual-notes>%d</actual-notes><normal-notes>%d</normal-notes></time-modification>\n",
           e->tup_n, e->tup_m);
      if (h && drums && h->head != NH_NORMAL)
        w (o, "        <notehead>%s</notehead>\n", head_name[h->head]);
      if (staves > 1)
        w (o, "        <staff>%d</staff>\n", staff);
      if ((h && (h->tie_stop || h->tie_start)) || (!k && ((e->tuplet & 6) || e->artic || e->slur))
          || (e->artic & NA_ARPEGGIO))
        {
          w (o, "        <notations>");
          if (!k && (e->slur & NS_STOP))
            w (o, "<slur type=\"stop\" number=\"1\"/>");
          if (!k && (e->slur & NS_START))
            w (o, "<slur type=\"start\" number=\"1\"/>");
          if (!k && (e->artic & (NA_ACCENT | NA_STACCATO)))
            w (o, "<articulations>%s%s</articulations>", e->artic & NA_ACCENT ? "<accent/>" : "",
               e->artic & NA_STACCATO ? "<staccato/>" : "");
          if (e->artic & NA_ARPEGGIO)
            w (o, e->artic & NA_ARP_DOWN ? "<arpeggiate direction=\"down\"/>" : e->artic & NA_ARP_UP ? "<arpeggiate direction=\"up\"/>" : "<arpeggiate/>");
          if (h && h->tie_stop)
            w (o, "<tied type=\"stop\"/>");
          if (h && h->tie_start)
            w (o, "<tied type=\"start\"/>");
          if (!k && (e->tuplet & 2))
            w (o, "<tuplet type=\"start\" bracket=\"yes\"/>");
          if (!k && (e->tuplet & 4))
            w (o, "<tuplet type=\"stop\"/>");
          w (o, "</notations>\n");
        }
      w (o, "      </note>\n");
    }
}

static void w_clef (out *o, int clef, int number)
{
  w (o, "        <clef");
  if (number)
    w (o, " number=\"%d\"", number);
  w (o, clef == NC_PERC ? "><sign>percussion</sign></clef>\n"
     : clef == NC_F ? "><sign>F</sign><line>4</line></clef>\n"
     : clef == NC_G8 ? "><sign>G</sign><line>2</line><clef-octave-change>-1</clef-octave-change></clef>\n"
     : clef == NC_C3 ? "><sign>C</sign><line>3</line></clef>\n"
     : clef == NC_C4 ? "><sign>C</sign><line>4</line></clef>\n" : "><sign>G</sign><line>2</line></clef>\n");
}

/* How a part written `t` semitones above how it sounds is played:
   MusicXML's transpose (sounding = written + chromatic + 12 octaves). */
static void w_transpose (out *o, int t)
{
  int shift = -t, oct = shift / 12, chrom = shift % 12, a = chrom < 0 ? -chrom : chrom;
  static const int diatonic[12] = { 0, 1, 1, 2, 2, 3, 3, 4, 5, 5, 6, 6 };
  if (!t)
    return;
  w (o, "        <transpose><diatonic>%d</diatonic><chromatic>%d</chromatic>", chrom < 0 ? -diatonic[a] : diatonic[a],
     chrom);
  if (oct)
    w (o, "<octave-change>%d</octave-change>", oct);
  w (o, "</transpose>\n");
}

/* An 8va (8vb, 15ma, 15mb) line on staff z starting in bar b, or ending. */
static int ott_starts (const gr_npart *pp, int b, int z)
{
  int ov = pp->ottava[b * 2 + z];
  return ov && (b == 0 || pp->ottava[(b - 1) * 2 + z] != ov);
}

static int ott_ends (const gr_npart *pp, int b, int nbars, int z)
{
  int ov = pp->ottava[b * 2 + z];
  return ov && (b == nbars - 1 || pp->ottava[(b + 1) * 2 + z] != ov);
}

/* Its start (its notes are at the pitch they sound: the line says
   they're written an octave or two nearer) or its stop. */
static void w_ottava (out *o, const gr_npart *pp, int b, int z, int start)
{
  int ov = pp->ottava[b * 2 + z], size = ov == 1 || ov == -1 ? 8 : 15;
  if (start)
    w (o, "      <direction placement=\"%s\">\n        <direction-type><octave-shift type=\"%s\" size=\"%d\"/></direction-type>\n",
       ov > 0 ? "above" : "below", ov > 0 ? "down" : "up", size);
  else
    w (o, "      <direction>\n        <direction-type><octave-shift type=\"stop\" size=\"%d\"/></direction-type>\n", size);
  if (pp->staves > 1)
    w (o, "        <staff>%d</staff>\n", z + 1);
  w (o, "      </direction>\n");
}

int gr_musicxml_export (const gr_song *s, const char *path)
{
  out o;
  gr_notation *nt = gr_notation_make_ex (s, -1, 1, GR_NT_REPEATS);
  int p, b, k, z, pnum[GR_MAX_TRACKS], t;
  char title[64];
  if (!nt)
    return -1;
  memset (pnum, 0, sizeof pnum);
  for (p = 0; p < nt->nparts; p++)
    pnum[nt->parts[p].track] = p + 1;
  if (!(o.f = fopen (path, "w")))
    {
      gr_notation_free (nt);
      return -1;
    }
  o.err = 0;
  leaf_title (path, title, sizeof title);
  w (&o, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\n"
     "<!DOCTYPE score-partwise PUBLIC \"-//Recordare//DTD MusicXML 4.0 Partwise//EN\" "
     "\"http://www.musicxml.org/dtds/partwise.dtd\">\n"
     "<score-partwise version=\"4.0\">\n  <work>\n    <work-title>");
  w_text (&o, title);
  w (&o, "</work-title>\n  </work>\n  <identification>\n    <encoding>\n"
     "      <software>Groove for RISC OS</software>\n"
     "      <supports element=\"accidental\" type=\"no\"/>\n"
     "      <supports element=\"beam\" type=\"no\"/>\n"
     "      <supports element=\"stem\" type=\"no\"/>\n"
     "    </encoding>\n  </identification>\n  <part-list>\n");
  for (t = 0; t < s->ntracks; t++)
    {
      const gr_track *tr = &s->tracks[t];
      if (!pnum[t])
        continue;
      w (&o, "    <score-part id=\"P%d\">\n      <part-name>", pnum[t]);
      w_text (&o, tr->name);
      w (&o, "</part-name>\n");
      if (tr->type == TRK_DRUMS)
        {
          int used[128] = { 0 };
          const gr_npart *pp = &nt->parts[pnum[t] - 1];
          for (b = 0; b < nt->nbars; b++)
            for (z = 0; z < 2; z++)
              {
                const gr_nrange *r = &pp->voice[b * GR_NVOICES + z];
                for (k = 0; k < r->n; k++)
                  {
                    const gr_nevent *e = &nt->ev[r->first + k];
                    int j;
                    if (e->kind == NE_NOTE)
                      for (j = 0; j < e->nheads; j++)
                        used[nt->hd[e->head + j].pitch & 127] = 1;
                  }
              }
          for (k = 0; k < 128; k++)
            if (used[k])
              {
                w (&o, "      <score-instrument id=\"P%d-I%d\">\n        <instrument-name>", pnum[t], k + 1);
                w_text (&o, gr_drum_name (k));
                w (&o, "</instrument-name>\n      </score-instrument>\n");
              }
          for (k = 0; k < 128; k++)
            if (used[k])
              w (&o, "      <midi-instrument id=\"P%d-I%d\">\n        <midi-channel>10</midi-channel>\n"
                 "        <midi-program>1</midi-program>\n        <midi-unpitched>%d</midi-unpitched>\n"
                 "      </midi-instrument>\n", pnum[t], k + 1, k + 1);
        }
      else
        {
          w (&o, "      <score-instrument id=\"P%d-I1\">\n        <instrument-name>", pnum[t]);
          w_text (&o, gr_gm_program_name (gr_track_program (tr) & 127));
          w (&o, "</instrument-name>\n      </score-instrument>\n"
             "      <midi-instrument id=\"P%d-I1\">\n        <midi-channel>%d</midi-channel>\n"
             "        <midi-program>%d</midi-program>\n      </midi-instrument>\n",
             pnum[t], gr_track_channel (s, t) + 1, (gr_track_program (tr) & 127) + 1);
        }
      w (&o, "    </score-part>\n");
    }
  w (&o, "  </part-list>\n");


  for (p = 0; p < nt->nparts; p++)
    {
      const gr_npart *pp = &nt->parts[p];
      char pid[16];
      snprintf (pid, sizeof pid, "P%d", p + 1);
      w (&o, "  <part id=\"%s\">\n", pid);
      for (b = 0; b < nt->nbars; b++)
        {
          const gr_nbar *bar = &nt->bars[b];
          int newtime = b == 0 || bar->num != nt->bars[b - 1].num || bar->den != nt->bars[b - 1].den, first = 1;
          int newkey = !pp->drums && (b == 0 || pp->key[b] != pp->key[b - 1] || bar->minor != nt->bars[b - 1].minor);
          int newclef = b == 0;
          for (z = 0; z < pp->staves; z++)
            newclef |= b > 0 && pp->clefs[b * 2 + z] != pp->clefs[(b - 1) * 2 + z];
          w (&o, "    <measure number=\"%d\">\n", b + 1);
          /* a repeat starting, or an ending */
          if ((bar->rep & NR_START) || bar->ending_start)
            {
              w (&o, "      <barline location=\"left\">");
              if (bar->rep & NR_START)
                w (&o, "<bar-style>heavy-light</bar-style>");
              if (bar->ending_start)
                w (&o, "<ending number=\"%d\" type=\"start\">%d.</ending>", bar->ending, bar->ending);
              if (bar->rep & NR_START)
                w (&o, "<repeat direction=\"forward\"/>");
              w (&o, "</barline>\n");
            }
          if (newtime || newkey || newclef)
            {
              w (&o, "      <attributes>\n");
              if (b == 0)
                w (&o, "        <divisions>%d</divisions>\n", XDIV);
              if (newkey)
                w (&o, "        <key><fifths>%d</fifths><mode>%s</mode></key>\n", pp->key[b],
                   bar->minor ? "minor" : "major");
              if (newtime)
                w (&o, "        <time><beats>%d</beats><beat-type>%d</beat-type></time>\n", bar->num, bar->den);
              if (b == 0 && pp->staves > 1)
                w (&o, "        <staves>2</staves>\n");
              for (z = 0; z < pp->staves; z++)
                if (b == 0 || pp->clefs[b * 2 + z] != pp->clefs[(b - 1) * 2 + z])
                  w_clef (&o, pp->clefs[b * 2 + z], pp->staves > 1 ? z + 1 : 0);
              if (b == 0)
                w_transpose (&o, pp->transpose);
              w (&o, "      </attributes>\n");
            }
          /* an 8va or 8vb line starting (its notes are at the pitch they
             sound: the line says they're written an octave nearer) */
          for (z = 0; z < pp->staves; z++)
            if (ott_starts (pp, b, z) && !pp->ott_start[b * 2 + z])
              w_ottava (&o, pp, b, z, 1);
          if (b == 0 && p == 0)
            w (&o, "      <direction placement=\"above\">\n        <direction-type>\n"
               "          <metronome><beat-unit>quarter</beat-unit><per-minute>%d</per-minute></metronome>\n"
               "        </direction-type>\n        <sound tempo=\"%d\"/>\n      </direction>\n",
               s->tempo, s->tempo);
          /* tempo changes in the bar, where they are (on the first part) */
          if (p == 0)
            {
              int k, t0 = gr_bar_tick (s, bar->src), t1 = gr_bar_tick (s, bar->src + 1);
              for (k = 0; k < s->ntempos; k++)
                if (s->tempos[k].tick >= t0 && s->tempos[k].tick < t1 && s->tempos[k].tick > 0)
                  {
                    char bpm[24];
                    snprintf (bpm, sizeof bpm, "%.2f", (double) s->tempos[k].bpm);
                    w (&o, "      <direction placement=\"above\">\n        <direction-type>\n"
                       "          <metronome><beat-unit>quarter</beat-unit><per-minute>%d</per-minute></metronome>\n"
                       "        </direction-type>\n", (int) (s->tempos[k].bpm + 0.5));
                    if (s->tempos[k].tick > t0)
                      w (&o, "        <offset sound=\"yes\">%d</offset>\n", (s->tempos[k].tick - t0) * 5);
                    w (&o, "        <sound tempo=\"%s\"/>\n      </direction>\n", bpm);
                  }
            }
          for (z = 0; z < pp->staves * 2; z++)
            {
              const gr_nrange *r = &pp->voice[b * GR_NVOICES + z];
              if (!r->n)
                continue;
              if (!first)
                w (&o, "      <backup><duration>%d</duration></backup>\n", bar->len * 5);
              first = 0;
              /* the pedal, under the lowest staff */
              if (z == (pp->staves - 1) * 2)
                for (k = 0; k < pp->npedal; k++)
                  if (pp->pedal[k].bar == b)
                    {
                      w (&o, "      <direction placement=\"below\">\n        <direction-type><pedal type=\"%s\" line=\"yes\" sign=\"yes\"/></direction-type>\n",
                         pp->pedal[k].down ? "start" : "stop");
                      if (pp->pedal[k].start)
                        w (&o, "        <offset sound=\"yes\">%d</offset>\n", pp->pedal[k].start * 5);
                      if (pp->staves > 1)
                        w (&o, "        <staff>%d</staff>\n", pp->staves);
                      w (&o, "        <sound damper-pedal=\"%s\"/>\n      </direction>\n", pp->pedal[k].down ? "yes" : "no");
                    }
              for (k = 0; k < r->n; k++)
                {
                  const gr_nevent *ev = &nt->ev[r->first + k];
                  /* (an 8va line starting or ending part way through the bar:
                     in the staff's first voice, where its notes start or end) */
                  int st = z / 2, mid_on = !(z & 1) && ott_starts (pp, b, st) && pp->ott_start[b * 2 + st];
                  int mid_off = !(z & 1) && ott_ends (pp, b, nt->nbars, st) && pp->ott_end[b * 2 + st];
                  if (mid_on && ev->start >= pp->ott_start[b * 2 + st] && (k == 0 || nt->ev[r->first + k - 1].start < pp->ott_start[b * 2 + st]))
                    w_ottava (&o, pp, b, st, 1);
                  w_event (&o, nt, ev, z + 1 + (z >= 2 ? 2 : 0), z / 2 + 1, pp->staves, pp->drums, pid);
                  if (mid_off && ev->start + ev->dur >= pp->ott_end[b * 2 + st]
                      && (k == 0 || nt->ev[r->first + k - 1].start + nt->ev[r->first + k - 1].dur < pp->ott_end[b * 2 + st]))
                    w_ottava (&o, pp, b, st, 0);
                }
            }
          for (z = 0; z < pp->staves; z++)
            if (ott_ends (pp, b, nt->nbars, z) && !pp->ott_end[b * 2 + z])
              w_ottava (&o, pp, b, z, 0);
          if ((bar->rep & NR_END) || bar->ending_stop)
            {
              w (&o, "      <barline location=\"right\">");
              if (bar->rep & NR_END)
                w (&o, "<bar-style>light-heavy</bar-style>");
              else if (b == nt->nbars - 1)
                w (&o, "<bar-style>light-heavy</bar-style>");
              if (bar->ending_stop)
                w (&o, "<ending number=\"%d\" type=\"%s\"/>", bar->ending, bar->ending_stop == 1 ? "stop" : "discontinue");
              if (bar->rep & NR_END)
                {
                  if (bar->times > 2)
                    w (&o, "<repeat direction=\"backward\" times=\"%d\"/>", bar->times);
                  else
                    w (&o, "<repeat direction=\"backward\"/>");
                }
              w (&o, "</barline>\n");
            }
          else if (b == nt->nbars - 1)
            w (&o, "      <barline location=\"right\"><bar-style>light-heavy</bar-style></barline>\n");
          w (&o, "    </measure>\n");
        }
      w (&o, "  </part>\n");
    }
  w (&o, "</score-partwise>\n");
  if (fclose (o.f))
    o.err = 1;
  gr_notation_free (nt);
  return o.err ? -1 : 0;
}

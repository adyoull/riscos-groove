/* Groove - the song model: tracks, patterns, regions and audio clips. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "groove.h"

static void copy_name (char *dst, const char *src)
{
  snprintf (dst, GR_NAME, "%s", src ? src : "");
}

void gr_song_init (gr_song *s)
{
  memset (s, 0, sizeof *s);
  s->tempo = 110;
  s->key = 0;
  s->bars = 16;
  s->loop_on = 0;
  s->loop_start = 0;
  s->loop_end = 4 * GR_BAR;
  s->metronome = 0;
  s->master = 0.8f;
  s->nmeters = 1;
  s->meters[0].bar = 0;
  s->meters[0].num = 4;
  s->meters[0].den = 4;
  s->nkeysigs = 0;
}

int gr_tuplet_bits (int act, int norm, double glen, double beat)
{
  int t, r;
  if (act <= 0 || norm <= 0 || act == norm)
    return 0;
  t = act == 3 && norm == 2 ? GR_TUP_3 : act == 5 && norm == 4 ? GR_TUP_5 : act == 6 && norm == 4 ? GR_TUP_6
    : act == 2 && norm == 3 ? GR_TUP_2 : act == 4 && norm == 3 ? GR_TUP_4 : act == 7 ? GR_TUP_7 : GR_TUP_OTHER;
  r = glen <= 0 || beat <= 0 ? 0 : glen < beat - 1e-6 ? 0x20 : glen < 2 * beat - 1e-6 ? 0x40 : 0x60;
  return r | t << 13;
}

int gr_set_keysig (gr_song *s, int bar, int fifths, int minor)
{
  int i;
  for (i = 0; i < s->nkeysigs && s->keysigs[i].bar < bar; i++)
    ;
  if (i == s->nkeysigs || s->keysigs[i].bar != bar)
    {
      if (s->nkeysigs == GR_MAX_KEYSIGS)
        return -1;
      memmove (&s->keysigs[i + 1], &s->keysigs[i], (size_t) (s->nkeysigs - i) * sizeof s->keysigs[0]);
      s->nkeysigs++;
    }
  s->keysigs[i].bar = bar;
  s->keysigs[i].fifths = fifths < -7 ? -7 : fifths > 7 ? 7 : fifths;
  s->keysigs[i].minor = minor != 0;
  return 0;
}

/* ---- tempo ---------------------------------------------------------------- */

double gr_tempo_at (const gr_song *s, int tick)
{
  int i;
  for (i = s->ntempos - 1; i >= 0; i--)
    if (s->tempos[i].tick <= tick)
      return s->tempos[i].bpm;
  return s->tempo > 0 ? s->tempo : 120;
}

/* Seconds a tick lasts at a tempo. */
static double tick_secs (double bpm) { return 60.0 / (bpm * GR_PPQ); }

double gr_tick_seconds (const gr_song *s, double tick)
{
  double t = 0.0, at = 0.0, bpm = s->tempo > 0 ? s->tempo : 120;
  int i;
  for (i = 0; i < s->ntempos && s->tempos[i].tick < tick; i++)
    {
      if (s->tempos[i].tick > at)
        {
          t += (s->tempos[i].tick - at) * tick_secs (bpm);
          at = s->tempos[i].tick;
        }
      bpm = s->tempos[i].bpm;
    }
  return t + (tick - at) * tick_secs (bpm);
}

double gr_seconds_tick (const gr_song *s, double secs)
{
  double t = 0.0, at = 0.0, bpm = s->tempo > 0 ? s->tempo : 120;
  int i;
  for (i = 0; i < s->ntempos; i++)
    {
      double d = (s->tempos[i].tick - at) * tick_secs (bpm);
      if (s->tempos[i].tick > at)
        {
          if (t + d > secs)
            break;
          t += d;
          at = s->tempos[i].tick;
        }
      bpm = s->tempos[i].bpm;
    }
  return at + (secs - t) / tick_secs (bpm);
}

int gr_tempo_add (gr_song *s, int tick, double bpm)
{
  int i;
  if (bpm < 10.0) bpm = 10.0;
  if (bpm > 400.0) bpm = 400.0;
  if (tick <= 0)
    {
      s->tempo = (int) (bpm + 0.5);
      return 0;
    }
  for (i = 0; i < s->ntempos && s->tempos[i].tick < tick; i++)
    ;
  if (i < s->ntempos && s->tempos[i].tick == tick)
    {
      s->tempos[i].bpm = (float) bpm;
      s->tempos[i].hidden = 0;
      return 0;
    }
  if (s->ntempos >= GR_MAX_TEMPOS)
    return -1;
  memmove (&s->tempos[i + 1], &s->tempos[i], (size_t) (s->ntempos - i) * sizeof s->tempos[0]);
  s->tempos[i].tick = tick;
  s->tempos[i].bpm = (float) bpm;
  s->tempos[i].hidden = 0;
  s->ntempos++;
  return 0;
}

void gr_tempo_base (gr_song *s, int bpm)
{
  int i;
  if (bpm < 40) bpm = 40;
  if (bpm > 240) bpm = 240;
  if (s->tempo > 0)
    for (i = 0; i < s->ntempos; i++)
      s->tempos[i].bpm = s->tempos[i].bpm * bpm / s->tempo;
  s->tempo = bpm;
  s->tempo_hidden = 0;
}

/* ---- time signatures ---------------------------------------------------- */

static int meter_bar_len (const gr_meter *m) { return m->num * (4 * GR_PPQ) / m->den; }

const gr_meter *gr_meter_at (const gr_song *s, int bar)
{
  static const gr_meter four = { 0, 4, 4, 0 };
  int i;
  if (s->nmeters < 1)
    return &four;
  for (i = s->nmeters - 1; i > 0 && s->meters[i].bar > bar; i--)
    ;
  return &s->meters[i];
}

int gr_bar_len (const gr_song *s, int bar) { return meter_bar_len (gr_meter_at (s, bar < 0 ? 0 : bar)); }
int gr_beat_len (const gr_song *s, int bar) { return 4 * GR_PPQ / gr_meter_at (s, bar < 0 ? 0 : bar)->den; }

int gr_bar_tick (const gr_song *s, int bar)
{
  int i, tick = 0, at = 0;
  if (bar <= 0)
    return 0;
  for (i = 0; i < s->nmeters; i++)
    {
      int until = i + 1 < s->nmeters ? s->meters[i + 1].bar : bar;
      if (until > bar)
        until = bar;
      tick += (until - at) * meter_bar_len (&s->meters[i]);
      at = until;
      if (at >= bar)
        return tick;
    }
  return tick + (bar - at) * meter_bar_len (gr_meter_at (s, bar));
}

int gr_tick_bar (const gr_song *s, int tick)
{
  int i, at = 0, t0 = 0;
  if (tick <= 0)
    return 0;
  for (i = 0; i < s->nmeters; i++)
    {
      int len = meter_bar_len (&s->meters[i]);
      if (i + 1 < s->nmeters)
        {
          int t1 = t0 + (s->meters[i + 1].bar - at) * len;
          if (tick < t1)
            return at + (tick - t0) / len;
          t0 = t1;
          at = s->meters[i + 1].bar;
        }
      else
        return at + (tick - t0) / len;
    }
  return 0;
}

int gr_bar_snap (const gr_song *s, int tick)
{
  int b = gr_tick_bar (s, tick), t0 = gr_bar_tick (s, b), t1 = gr_bar_tick (s, b + 1);
  return tick - t0 < t1 - tick ? t0 : t1;
}

int gr_set_meter (gr_song *s, int bar, int num, int den)
{
  int i, j;
  if (bar < 0 || num < 1 || num > 128 || den < 1 || den > 64 || (den & (den - 1)))
    return -1;
  if (s->nmeters < 1)
    {
      s->nmeters = 1;
      s->meters[0].bar = 0; s->meters[0].num = 4; s->meters[0].den = 4;
    }
  for (i = 0; i < s->nmeters && s->meters[i].bar < bar; i++)
    ;
  if (i == s->nmeters || s->meters[i].bar != bar)
    {
      if (s->nmeters == GR_MAX_METERS)
        return -1;
      memmove (&s->meters[i + 1], &s->meters[i], (size_t) (s->nmeters - i) * sizeof s->meters[0]);
      s->nmeters++;
      s->meters[i].bar = bar;
      s->meters[i].symbol = 0;
    }
  if (s->meters[i].num != num || s->meters[i].den != den)
    s->meters[i].symbol = 0;
  s->meters[i].num = num;
  s->meters[i].den = den;
  /* A change to the meter already in force isn't a change. */
  for (i = j = 1; i < s->nmeters; i++)
    if (s->meters[i].num != s->meters[j - 1].num || s->meters[i].den != s->meters[j - 1].den
        || s->meters[i].symbol != s->meters[j - 1].symbol)
      s->meters[j++] = s->meters[i];
  s->nmeters = j;
  return 0;
}

void gr_set_meter_symbol (gr_song *s, int bar, int symbol)
{
  int i;
  for (i = 0; i < s->nmeters; i++)
    if (s->meters[i].bar == bar)
      s->meters[i].symbol = symbol == 1 && s->meters[i].num == 4 && s->meters[i].den == 4 ? 1
        : symbol == 2 && s->meters[i].num == 2 && s->meters[i].den == 2 ? 2
        : symbol == 3 ? 3 : 0;
}

void gr_song_cover (gr_song *s, int end_tick)
{
  if (end_tick > 0 && gr_tick_bar (s, end_tick - 1) + 1 > s->bars)
    s->bars = gr_tick_bar (s, end_tick - 1) + 1;
}

int gr_lyric_add (gr_song *s, int track, int tick, int staff, int verse, int syllabic, const char *text)
{
  gr_lyric *l;
  if (s->nlyrics == s->lyriccap)
    {
      int cap = s->lyriccap ? s->lyriccap * 2 : 256;
      gr_lyric *a = realloc (s->lyrics, (size_t) cap * sizeof *a);
      if (!a)
        return -1;
      s->lyrics = a;
      s->lyriccap = cap;
    }
  l = &s->lyrics[s->nlyrics++];
  memset (l, 0, sizeof *l);
  l->track = track;
  l->tick = tick;
  l->staff = (unsigned char) (staff != 0);
  l->verse = (unsigned char) (verse == GR_TEXT_ABOVE || verse == GR_TEXT_BELOW || verse == GR_TEXT_TEMPO || verse == GR_TEXT_TEMPO_MARK || verse == GR_TEXT_PEDAL ? verse : verse < 0 ? 0 : verse > 9 ? 9 : verse);
  l->syllabic = (unsigned char) (syllabic & 3);
  snprintf (l->text, sizeof l->text, "%s", text);
  return 0;
}

int gr_set_srcbar (gr_song *s, int bar, int measure)
{
  if (bar < 0 || bar >= 65535)
    return 0;
  if (bar >= s->nsrcbar)
    {
      unsigned short *a = realloc (s->srcbar, (size_t) (bar + 1) * sizeof *a);
      if (!a)
        return -1;
      memset (a + s->nsrcbar, 0, (size_t) (bar + 1 - s->nsrcbar) * sizeof *a);
      s->srcbar = a;
      s->nsrcbar = bar + 1;
    }
  s->srcbar[bar] = (unsigned short) (measure < 0 || measure >= 65534 ? 0 : measure + 1);
  return 0;
}

void gr_song_free (gr_song *s)
{
  int i;
  free (s->lyrics);
  s->lyrics = NULL;
  s->nlyrics = s->lyriccap = 0;
  free (s->srcbar);
  s->srcbar = NULL;
  s->nsrcbar = 0;
  for (i = 0; i < GR_MAX_CLIPS; i++)
    {
      free (s->clips[i].data);
      s->clips[i].data = NULL;
      s->clips[i].used = 0;
    }
}

int gr_track_add (gr_song *s, gr_track_type t, const char *name, int patch)
{
  gr_track *tr;
  if (s->ntracks >= GR_MAX_TRACKS)
    return -1;
  tr = &s->tracks[s->ntracks];
  memset (tr, 0, sizeof *tr);
  copy_name (tr->name, name);
  tr->type = t;
  tr->patch = patch;
  tr->gm = -1;
  tr->channel = tr->bank_msb = tr->bank_lsb = tr->program = -1;
  tr->vol = 0.8f;
  tr->pan = 0.0f;
  tr->reverb = t == TRK_DRUMS ? 0.08f : 0.18f;
  return s->ntracks++;
}

/* Move a track from one place in the list to another; its regions go with it. */
void gr_track_move (gr_song *s, int from, int to)
{
  gr_track t;
  int i;
  if (from < 0 || from >= s->ntracks || to < 0 || to >= s->ntracks || from == to)
    return;
  /* (one moved from or into the middle of an instrument on several tracks
     leaves it on its own) */
  if (from + 1 < s->ntracks)
    s->tracks[from + 1].joined = GR_JOIN_NONE;
  s->tracks[from].joined = GR_JOIN_NONE;
  t = s->tracks[from];
  if (from < to)
    memmove (&s->tracks[from], &s->tracks[from + 1], (size_t) (to - from) * sizeof t);
  else
    memmove (&s->tracks[to + 1], &s->tracks[to], (size_t) (from - to) * sizeof t);
  s->tracks[to] = t;
  for (i = 0; i < GR_MAX_REGIONS; i++)
    if (s->regions[i].used)
      {
        int r = s->regions[i].track;
        if (r == from)
          r = to;
        else if (from < to && r > from && r <= to)
          r--;
        else if (from > to && r >= to && r < from)
          r++;
        s->regions[i].track = r;
      }
  if (to + 1 < s->ntracks)
    s->tracks[to + 1].joined = GR_JOIN_NONE;
  /* (its words go with it) */
  for (i = 0; i < s->nlyrics; i++)
    {
      int r = s->lyrics[i].track;
      if (r == from)
        r = to;
      else if (from < to && r > from && r <= to)
        r--;
      else if (from > to && r >= to && r < from)
        r++;
      s->lyrics[i].track = r;
    }
}

void gr_track_delete (gr_song *s, int track)
{
  int i;
  if (track < 0 || track >= s->ntracks)
    return;
  for (i = 0; i < GR_MAX_REGIONS; i++)
    if (s->regions[i].used)
      {
        if (s->regions[i].track == track)
          s->regions[i].used = 0;
        else if (s->regions[i].track > track)
          s->regions[i].track--;
      }
  memmove (&s->tracks[track], &s->tracks[track + 1],
           (size_t) (s->ntracks - track - 1) * sizeof s->tracks[0]);
  s->ntracks--;
  if (track < s->ntracks)
    s->tracks[track].joined = GR_JOIN_NONE;   /* (what it was joined to has gone) */
  /* (its words go with it) */
  {
    int w = 0;
    for (i = 0; i < s->nlyrics; i++)
      if (s->lyrics[i].track != track)
        {
          s->lyrics[w] = s->lyrics[i];
          if (s->lyrics[w].track > track)
            s->lyrics[w].track--;
          w++;
        }
    s->nlyrics = w;
  }
  gr_song_gc (s);
}

int gr_pattern_new (gr_song *s, const char *name, int len)
{
  int i;
  for (i = 0; i < GR_MAX_PATTERNS; i++)
    if (!s->patterns[i].used)
      {
        gr_pattern *p = &s->patterns[i];
        memset (p, 0, sizeof *p);
        p->used = 1;
        p->len = len > 0 ? len : GR_BAR;
        copy_name (p->name, name);
        return i;
      }
  return -1;
}

/* ---- controller events ---------------------------------------------------- */

int gr_ctl_min (int type) { return type == GR_CTL_BEND ? -8192 : 0; }
int gr_ctl_max (int type) { return type == GR_CTL_BEND ? 8191 : 127; }

int gr_ctl_default (int type)
{
  switch (type)
    {
    case 7: return 100;                  /* volume */
    case 8: case 10: return 64;          /* balance, pan: the middle */
    case 11: return 127;                 /* expression */
    case GR_CTL_BEND: return 0;
    default: return 0;
    }
}

const char *gr_ctl_name (int type)
{
  static char other[16];
  switch (type)
    {
    case 1: return "Modulation (CC 1)";
    case 2: return "Breath (CC 2)";
    case 4: return "Foot (CC 4)";
    case 5: return "Portamento time (CC 5)";
    case 7: return "Volume (CC 7)";
    case 8: return "Balance (CC 8)";
    case 10: return "Pan (CC 10)";
    case 11: return "Expression (CC 11)";
    case 64: return "Sustain (CC 64)";
    case 65: return "Portamento (CC 65)";
    case 66: return "Sostenuto (CC 66)";
    case 67: return "Soft pedal (CC 67)";
    case 71: return "Resonance (CC 71)";
    case 74: return "Brightness (CC 74)";
    case 91: return "Reverb (CC 91)";
    case 93: return "Chorus (CC 93)";
    case GR_CTL_PROGRAM: return "Program change";
    case GR_CTL_BEND: return "Pitch bend";
    case GR_CTL_PRESSURE: return "Channel pressure";
    }
  snprintf (other, sizeof other, "CC %d", type & 127);
  return other;
}

int gr_pattern_find_ctl (const gr_pattern *p, int tick, int type)
{
  int k;
  for (k = 0; k < p->nctls && p->ctls[k].tick <= tick; k++)
    if (p->ctls[k].tick == tick && p->ctls[k].type == type)
      return k;
  return -1;
}

int gr_pattern_set_ctl (gr_pattern *p, int tick, int type, int value)
{
  int i, k;
  if (tick < 0 || tick >= p->len || type < 0 || type >= GR_CTL_TYPES)
    return -1;
  if (value < gr_ctl_min (type)) value = gr_ctl_min (type);
  if (value > gr_ctl_max (type)) value = gr_ctl_max (type);
  k = gr_pattern_find_ctl (p, tick, type);
  if (k >= 0)
    {
      p->ctls[k].value = (int16_t) value;
      return k;
    }
  if (p->nctls >= GR_MAX_CTLS)
    return -1;
  for (i = p->nctls; i > 0 && p->ctls[i - 1].tick > tick; i--)
    p->ctls[i] = p->ctls[i - 1];
  p->ctls[i].tick = (int16_t) tick;
  p->ctls[i].type = (uint8_t) type;
  p->ctls[i].spare = 0;
  p->ctls[i].value = (int16_t) value;
  p->nctls++;
  return i;
}

int gr_pattern_add_mark (gr_pattern *p, const gr_mark *m)
{
  int i;
  if (m->tick < 0 || m->tick >= p->len || p->nmarks >= GR_MAX_MARKS)
    return -1;
  for (i = p->nmarks; i > 0 && p->marks[i - 1].tick > m->tick; i--)
    p->marks[i] = p->marks[i - 1];
  p->marks[i] = *m;
  p->nmarks++;
  return i;
}

const gr_mark *gr_arpeggio_mark (const gr_pattern *p, int tick)
{
  int lo = 0, hi = p->nmarks;
  while (lo < hi)
    {
      int mid = (lo + hi) / 2;
      if (p->marks[mid].tick < tick)
        lo = mid + 1;
      else
        hi = mid;
    }
  for (; lo < p->nmarks && p->marks[lo].tick == tick; lo++)
    if (p->marks[lo].kind == GR_MK_ARPEGGIO)
      return &p->marks[lo];
  return NULL;
}

int gr_arpeggio_at (const gr_pattern *p, int tick)
{
  const gr_mark *m = gr_arpeggio_mark (p, tick);
  return !m ? 0 : m->value < 0 ? -1 : 1;
}

int gr_arpeggio_has (const gr_mark *m, int pitch)
{
  return m && (m->len <= 0 || (pitch >= m->pitch && pitch <= m->len));
}

int gr_arpeggio_turn (const gr_pattern *p, int k, int *count)
{
  const gr_mark *m = gr_arpeggio_mark (p, p->notes[k].tick);
  int dir = !m ? 0 : m->value < 0 ? -1 : 1, i, turn = 0, n = 0;
  if (count)
    *count = 0;
  if (!dir || !gr_arpeggio_has (m, p->notes[k].pitch))
    return 0;
  for (i = 0; i < p->nnotes; i++)
    if (p->notes[i].tick == p->notes[k].tick && gr_arpeggio_has (m, p->notes[i].pitch))
      {
        n++;
        if (i != k && (dir > 0 ? p->notes[i].pitch < p->notes[k].pitch
                       || (p->notes[i].pitch == p->notes[k].pitch && i < k)
                       : p->notes[i].pitch > p->notes[k].pitch
                       || (p->notes[i].pitch == p->notes[k].pitch && i < k)))
          turn++;
      }
  if (count)
    *count = n;
  return n > 1 ? turn : 0;
}

void gr_pattern_delete_ctl (gr_pattern *p, int index)
{
  if (index < 0 || index >= p->nctls)
    return;
  memmove (&p->ctls[index], &p->ctls[index + 1], (size_t) (p->nctls - index - 1) * sizeof p->ctls[0]);
  p->nctls--;
}

int gr_ctl_value_at (const gr_pattern *p, int tick, int type, int dflt)
{
  int k, v = dflt;
  for (k = 0; k < p->nctls && p->ctls[k].tick <= tick; k++)
    if (p->ctls[k].type == type)
      v = p->ctls[k].value;
  return v;
}

/* Insert keeping the notes sorted by tick, so the sequencer can stop
   scanning early. Returns the index, or -1 if full. */
int gr_pattern_add_note (gr_pattern *p, int tick, int len, int pitch, int vel)
{
  int i;
  if (p->nnotes >= GR_MAX_NOTES || tick < 0 || tick >= p->len)
    return -1;
  if (pitch < 0) pitch = 0;
  if (pitch > 127) pitch = 127;
  if (vel < 1) vel = 1;
  if (vel > 127) vel = 127;
  if (len < 1) len = 1;
  for (i = p->nnotes; i > 0 && p->notes[i - 1].tick > tick; i--)
    p->notes[i] = p->notes[i - 1];
  p->notes[i].tick = (int16_t) tick;
  p->notes[i].len = (int16_t) len;
  p->notes[i].pitch = (uint8_t) pitch;
  p->notes[i].vel = (uint8_t) vel;
  p->notes[i].wr = 0;
  p->notes[i].spell = 0;
  p->notes[i].tie1 = 0;
  p->nnotes++;
  return i;
}

int gr_pattern_find_note (const gr_pattern *p, int tick, int pitch)
{
  int i;
  for (i = 0; i < p->nnotes; i++)
    if (p->notes[i].pitch == pitch && tick >= p->notes[i].tick
        && tick < p->notes[i].tick + (p->notes[i].len > 0 ? p->notes[i].len : 1))
      return i;
  return -1;
}

void gr_pattern_delete_note (gr_pattern *p, int index)
{
  if (index < 0 || index >= p->nnotes)
    return;
  memmove (&p->notes[index], &p->notes[index + 1],
           (size_t) (p->nnotes - index - 1) * sizeof p->notes[0]);
  p->nnotes--;
}

int gr_region_add (gr_song *s, int track, int start, int len, int pattern)
{
  int i;
  if (track < 0 || track >= s->ntracks || len <= 0)
    return -1;
  if (start < 0)
    start = 0;
  for (i = 0; i < GR_MAX_REGIONS; i++)
    if (!s->regions[i].used)
      {
        gr_region *r = &s->regions[i];
        memset (r, 0, sizeof *r);
        r->used = 1;
        r->track = track;
        r->start = start;
        r->len = len;
        r->pattern = pattern;
        r->clip = -1;
        gr_song_cover (s, start + len);
        return i;
      }
  return -1;
}

void gr_region_delete (gr_song *s, int region)
{
  if (region >= 0 && region < GR_MAX_REGIONS)
    s->regions[region].used = 0;
  gr_song_gc (s);
}

int gr_region_at (const gr_song *s, int track, int tick)
{
  int i;
  for (i = GR_MAX_REGIONS - 1; i >= 0; i--)   /* newest wins where they overlap */
    {
      const gr_region *r = &s->regions[i];
      if (r->used && r->track == track && tick >= r->start && tick < r->start + r->len)
        return i;
    }
  return -1;
}

/* Regions dropped from the same loop share nothing (gr_loop_place makes a
   new pattern each time), but a copied region shares its pattern. Before an
   edit, give the region its own copy so the edit doesn't surprise the user
   elsewhere in the song. */
int gr_pattern_unshare (gr_song *s, int region)
{
  int i, users = 0, p, np;
  gr_region *r = &s->regions[region];
  p = r->pattern;
  if (p < 0)
    return p;
  for (i = 0; i < GR_MAX_REGIONS; i++)
    if (s->regions[i].used && s->regions[i].pattern == p && s->regions[i].clip < 0)
      users++;
  if (users <= 1)
    return p;
  np = gr_pattern_new (s, s->patterns[p].name, s->patterns[p].len);
  if (np < 0)
    return p;
  memcpy (&s->patterns[np], &s->patterns[p], sizeof s->patterns[p]);
  r->pattern = np;
  return np;
}

/* Split a region at song tick `tick` (strictly inside it). The right part
   keeps sounding exactly as before: for notes, it gets the pattern rotated
   to start where the split falls (or shares it, if the split falls on a
   repeat); for audio, it starts that much further into the recording.
   Returns the new (right-hand) region, or -1. */
int gr_region_split (gr_song *s, int region, int tick)
{
  gr_region *r, *nr;
  int n, off;
  if (region < 0 || region >= GR_MAX_REGIONS || !s->regions[region].used)
    return -1;
  r = &s->regions[region];
  if (tick <= r->start || tick >= r->start + r->len)
    return -1;
  n = gr_region_add (s, r->track, tick, r->start + r->len - tick, r->pattern);
  if (n < 0)
    return -1;
  r = &s->regions[region];
  nr = &s->regions[n];
  off = tick - r->start;
  if (r->clip >= 0)
    {
      double secs = gr_tick_seconds (s, tick) - gr_tick_seconds (s, r->start);
      nr->clip = r->clip;
      nr->clip_offset = r->clip_offset + (int) (secs * s->clips[r->clip].rate + 0.5);
      nr->pattern = -1;
    }
  else if (r->pattern >= 0 && off % s->patterns[r->pattern].len)
    {
      const gr_pattern *src = &s->patterns[r->pattern];
      int len = src->len, rot = off % len, k;
      int np = gr_pattern_new (s, src->name, len);
      if (np < 0)
        {
          nr->used = 0;
          return -1;
        }
      src = &s->patterns[r->pattern];
      for (k = 0; k < src->nnotes; k++)
        {
          const gr_note *x = &src->notes[k];
          int at = gr_pattern_add_note (&s->patterns[np], (x->tick - rot + len) % len, x->len, x->pitch, x->vel);
          if (at >= 0)
            GR_NOTE_WRITTEN (&s->patterns[np].notes[at], x);
        }
      for (k = 0; k < src->nctls; k++)
        {
          const gr_ctl *c = &src->ctls[k];
          gr_pattern_set_ctl (&s->patterns[np], (c->tick - rot + len) % len, c->type, c->value);
        }
      for (k = 0; k < src->nmarks; k++)
        {
          gr_mark m = src->marks[k];
          m.tick = (int16_t) ((m.tick - rot + len) % len);
          gr_pattern_add_mark (&s->patterns[np], &m);
        }
      nr->pattern = np;
    }
  r->len = off;
  return n;
}

/* A copy of region `region` at `tick` on `track` (which must be of the same
   kind), with a pattern of its own. Returns the new region, or -1. */
int gr_region_copy (gr_song *s, const gr_region *src_r, const gr_pattern *src_p, int track, int tick)
{
  int n, np = -1;
  if (track < 0 || track >= s->ntracks)
    return -1;
  if (src_r->clip < 0)
    {
      if (!src_p || (np = gr_pattern_new (s, src_p->name, src_p->len)) < 0)
        return -1;
      memcpy (&s->patterns[np], src_p, sizeof *src_p);
      s->patterns[np].used = 1;
    }
  n = gr_region_add (s, track, tick, src_r->len, np);
  if (n < 0)
    {
      if (np >= 0)
        s->patterns[np].used = 0;
      return -1;
    }
  if (src_r->clip >= 0)
    {
      s->regions[n].clip = src_r->clip;
      s->regions[n].clip_offset = src_r->clip_offset;
      s->clips[src_r->clip].used = 1;     /* (its audio may have been kept only for Undo) */
    }
  return n;
}

int gr_clip_new (gr_song *s, const char *name, int rate)
{
  static unsigned next_id = 1;
  int i, pass;
  /* A slot never used first; then one whose audio is only kept for Undo. */
  for (pass = 0; pass < 2; pass++)
    for (i = 0; i < GR_MAX_CLIPS; i++)
      if (!s->clips[i].used && (pass || !s->clips[i].data))
      {
        gr_clip *c = &s->clips[i];
        free (c->data);
        memset (c, 0, sizeof *c);
        c->used = 1;
        c->id = next_id++;
        c->rate = rate;
        copy_name (c->name, name);
        return i;
      }
  return -1;
}

int gr_clip_append (gr_clip *c, const int16_t *stereo, int frames)
{
  if (c->frames + frames > c->cap)
    {
      int cap = c->cap ? c->cap * 2 : GR_RATE * 4;
      int16_t *d;
      while (cap < c->frames + frames)
        cap *= 2;
      d = realloc (c->data, (size_t) cap * 2 * sizeof (int16_t));
      if (!d)
        return -1;
      c->data = d;
      c->cap = cap;
    }
  memcpy (c->data + 2 * c->frames, stereo, (size_t) frames * 2 * sizeof (int16_t));
  c->frames += frames;
  return 0;
}

void gr_song_gc (gr_song *s)
{
  int i, j;
  for (i = 0; i < GR_MAX_PATTERNS; i++)
    if (s->patterns[i].used)
      {
        for (j = 0; j < GR_MAX_REGIONS; j++)
          if (s->regions[j].used && s->regions[j].clip < 0 && s->regions[j].pattern == i)
            break;
        if (j == GR_MAX_REGIONS)
          s->patterns[i].used = 0;
      }
  for (i = 0; i < GR_MAX_CLIPS; i++)
    if (s->clips[i].used)
      {
        for (j = 0; j < GR_MAX_REGIONS; j++)
          if (s->regions[j].used && s->regions[j].clip == i)
            break;
        if (j == GR_MAX_REGIONS)
          s->clips[i].used = 0;   /* the audio stays, for Undo, until the slot is reused */
      }
}

int gr_song_end (const gr_song *s)
{
  int i, end = 0;
  for (i = 0; i < GR_MAX_REGIONS; i++)
    if (s->regions[i].used && s->regions[i].start + s->regions[i].len > end)
      end = s->regions[i].start + s->regions[i].len;
  return end;
}

/* ---- names ---------------------------------------------------------- */
static const char *const patch_names[PATCH_COUNT] =
  { "Electric Piano", "Synth Bass", "Warm Pad", "Square Lead",
    "Plucked Guitar", "Drawbar Organ", "Glass Bells" };
static const char *const kit_names[KIT_COUNT] = { "Studio Kit", "Electro Kit" };
static const char *const drum_names[GR_DRUM_SOUNDS] =
  { "Kick", "Snare", "Hi-hat", "Open hat", "Clap", "Low tom", "High tom", "Cowbell" };
/* General MIDI's percussion map, keys 35..81. */
static const char *const gm_drums[47] =
  { "Acoustic Bass Drum", "Bass Drum 1", "Side Stick", "Acoustic Snare", "Hand Clap",
    "Electric Snare", "Low Floor Tom", "Closed Hi-Hat", "High Floor Tom", "Pedal Hi-Hat",
    "Low Tom", "Open Hi-Hat", "Low-Mid Tom", "Hi-Mid Tom", "Crash Cymbal 1", "High Tom",
    "Ride Cymbal 1", "Chinese Cymbal", "Ride Bell", "Tambourine", "Splash Cymbal",
    "Cowbell", "Crash Cymbal 2", "Vibraslap", "Ride Cymbal 2", "Hi Bongo", "Low Bongo",
    "Mute Hi Conga", "Open Hi Conga", "Low Conga", "High Timbale", "Low Timbale",
    "High Agogo", "Low Agogo", "Cabasa", "Maracas", "Short Whistle", "Long Whistle",
    "Short Guiro", "Long Guiro", "Claves", "Hi Wood Block", "Low Wood Block",
    "Mute Cuica", "Open Cuica", "Mute Triangle", "Open Triangle" };
static const char *const notes[12] =
  { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

const char *gr_patch_name (int p) { return p >= 0 && p < PATCH_COUNT ? patch_names[p] : "?"; }
const char *gr_kit_name (int k) { return k >= 0 && k < KIT_COUNT ? kit_names[k] : "?"; }
int gr_drum_kit_key (int row) { return gr_gm_drum_note (row); }

const char *gr_drum_name (int key)
{
  static char other[128][12];
  int v;
  if (key < 0 || key > 127)
    return "?";
  for (v = 0; v < GR_DRUM_SOUNDS; v++)
    if (gr_gm_drum_note (v) == key)
      return drum_names[v];
  if (key >= 35 && key <= 81)
    return gm_drums[key - 35];
  if (!other[key][0])
    {
      char n[8];
      snprintf (other[key], sizeof other[key], "%s %d", gr_note_name (key, n), key);
    }
  return other[key];
}
const char *gr_key_name (int k)
{
  /* (as keys are named: E flat, not D sharp) */
  static const char *const keys[12] = { "C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
  return keys[((k % 12) + 12) % 12];
}

const char *gr_note_name (int pitch, char buf[8])
{
  pitch &= 127;
  snprintf (buf, 8, "%s%d", notes[pitch % 12], pitch / 12 - 1);
  return buf;
}

/* Groove - putting imported notes into regions. See place.h. */
#include <string.h>
#include "place.h"

#define CHUNK_BARS 4
#define MAX_PATTERN_TICKS 32000 /* (a note's tick in its pattern is 16 bits) */

static int chunk_bars = CHUNK_BARS;

void gr_place_chunk (int bars)
{
  chunk_bars = bars < CHUNK_BARS ? CHUNK_BARS : bars;
}
#define MAX_STRETCH 16          /* bars a region may grow by to keep a note whole */

/* Does a note start in [a, b) and go on past b? */
static int crosses (const gr_pnote *m, int n, int a, int b)
{
  int k;
  for (k = 0; k < n; k++)
    if (m[k].start >= a && m[k].start < b && m[k].end > b)
      return 1;
  return 0;
}

static int count_in (const gr_pnote *m, int n, int a, int b)
{
  int k, c = 0;
  for (k = 0; k < n; k++)
    if (m[k].start >= a && m[k].start < b)
      c++;
  return c;
}

/* One region [a, b) (ticks from 'from'), if it has notes. */
static int region (gr_song *s, int t, const gr_pnote *m, int n, int from, int a, int b,
                   int *full_at, int *dropped)
{
  int drums = s->tracks[t].type == TRK_DRUMS, k, pat, reg, len = b - a, at;
  gr_pattern *p;
  if (!count_in (m, n, a, b))
    return 0;
  pat = gr_pattern_new (s, s->tracks[t].name, len);
  reg = pat < 0 ? -1 : gr_region_add (s, t, from + a, len, pat);
  if (reg < 0)
    {
      if (pat >= 0)
        s->patterns[pat].used = 0;
      *full_at = from + a;
      return -1;
    }
  p = &s->patterns[pat];
  for (k = 0; k < n; k++)
    if (m[k].start >= a && m[k].start < b && m[k].key >= 0)
      {
        int tick = m[k].start - a, nl = m[k].end - m[k].start;
        if (drums && !(m[k].wr & GR_WR))
          nl = GR_STEP;               /* (a score's keeps its written length, for the Score window) */
        if (tick + nl > len)
          nl = len - tick;
        if (nl < 1)
          nl = 1;
        if ((at = gr_pattern_find_note (p, tick, m[k].key)) >= 0
            && (!(m[k].wr & GR_WR) || p->notes[at].tick == tick))
          {
            /* the same key twice at once: played once. A score's unison in
               another voice is kept as a mark, to be written. */
            if ((m[k].wr & GR_WR) && (p->notes[at].wr & (GR_WR_VOICE | GR_WR_STAFF | GR_WR_CROSS))
                != (m[k].wr & (GR_WR_VOICE | GR_WR_STAFF | GR_WR_CROSS)))
              {
                gr_mark mk;
                memset (&mk, 0, sizeof mk);
                mk.tick = (int16_t) tick;
                mk.len = (int16_t) ((m[k].wr & GR_WR_HALF) ? 2 * nl : nl);
                mk.kind = GR_MK_NOTE;
                mk.value = (int8_t) ((m[k].wr & GR_WR_VOICE) | ((m[k].wr >> 8) & 0x1f) << 2);   /* voice, spelling */
                mk.pitch = (uint8_t) m[k].key;
                mk.staff = (uint8_t) gr_mknote_staff (m[k].wr, 0);
                gr_pattern_add_mark (p, &mk);
              }
            continue;
          }
        if ((at = gr_pattern_add_note (p, tick, nl, m[k].key, m[k].vel)) < 0)
          (*dropped)++;
        else
          {
            p->notes[at].wr = (uint8_t) (nl == m[k].end - m[k].start ? m[k].wr & 0xff : 0);
            p->notes[at].spell = (uint8_t) (m[k].wr >> 8);
            p->notes[at].tie1 = (int16_t) ((m[k].tie1 > 0 && m[k].tie1 < nl) || m[k].tie1 < 0 ? m[k].tie1 : 0);
          }
      }
  return 0;
}

int gr_place_notes (gr_song *s, int t, const gr_pnote *m, int n, int from, int *full_at, int *dropped)
{
  int k, last = 0, last_end = 0, bar0 = gr_tick_bar (s, from), b, song_end, written = 0;
  for (k = 0; k < n; k++)
    {
      written |= (m[k].wr & GR_WR) != 0;
      if (m[k].start > last)
        last = m[k].start;
      if (m[k].end > last_end)
        last_end = m[k].end;
    }
  /* (a score's last region ends with the bar the last note ends in, not
     four bars on) */
  song_end = gr_bar_tick (s, gr_tick_bar (s, from + last_end - 1) + 1) - from;
  for (b = bar0; gr_bar_tick (s, b) - from <= last;)
    {
      int a = gr_bar_tick (s, b) - from, e = b + chunk_bars, stretch = 0, z;
      /* Don't end the region in the middle of a note. */
      while (crosses (m, n, a, gr_bar_tick (s, e) - from) && stretch < MAX_STRETCH)
        e++, stretch++;
      z = gr_bar_tick (s, e) - from;
      if (written && z > song_end && song_end > a)
        {
          z = song_end;
          e = gr_tick_bar (s, from + z);
        }
      if (count_in (m, n, a, z) <= GR_MAX_NOTES && z - a <= MAX_PATTERN_TICKS)
        {
          if (region (s, t, m, n, from, a, z, full_at, dropped))
            return -1;
        }
      else
        {
          /* Busy (or long): regions of as many bars as fit, never ending
             inside a note. */
          int c = b;
          while (c < e)
            {
              int d = c + 1, ca = gr_bar_tick (s, c) - from;
              while (d < e && crosses (m, n, ca, gr_bar_tick (s, d) - from))
                d++;
              while (d < e)
                {
                  int d2 = d + 1, zz;
                  while (d2 < e && crosses (m, n, ca, gr_bar_tick (s, d2) - from))
                    d2++;
                  zz = gr_bar_tick (s, d2) - from;
                  if (zz - ca > MAX_PATTERN_TICKS || count_in (m, n, ca, zz) > GR_MAX_NOTES)
                    break;
                  d = d2;
                }
              if (region (s, t, m, n, from, ca, gr_bar_tick (s, d) - from, full_at, dropped))
                return -1;
              c = d;
            }
        }
      b = e;
    }
  return 0;
}

int gr_place_ctls (gr_song *s, int t, const gr_pctl *c, int n, int from, int *full_at, int *dropped)
{
  int k, made = -1;             /* the region made last for events over a rest */
  for (k = 0; k < n; k++)
    {
      int at = from + c[k].tick, i, r = -1;
      for (i = 0; i < GR_MAX_REGIONS; i++)
        {
          const gr_region *g = &s->regions[i];
          if (g->used && g->track == t && g->clip < 0 && g->pattern >= 0
              && at >= g->start && at < g->start + g->len)
            {
              r = i;
              break;
            }
        }
      if (r < 0)
        {
          /* No region here: a bar of its own, or the one made last
             grown to reach it when it's the next bar. */
          int bar = gr_tick_bar (s, at), b0 = gr_bar_tick (s, bar), bl = gr_bar_len (s, bar);
          if (made >= 0 && s->regions[made].start + s->regions[made].len == b0)
            {
              gr_region *g = &s->regions[made];
              g->len += bl;
              s->patterns[g->pattern].len = g->len;
              r = made;
            }
          else
            {
              int pat = gr_pattern_new (s, s->tracks[t].name, bl);
              r = pat < 0 ? -1 : gr_region_add (s, t, b0, bl, pat);
              if (r < 0)
                {
                  if (pat >= 0)
                    s->patterns[pat].used = 0;
                  *full_at = at;
                  return -1;
                }
              made = r;
            }
        }
      {
        const gr_region *g = &s->regions[r];
        gr_pattern *p = &s->patterns[g->pattern];
        if (gr_pattern_set_ctl (p, (at - g->start) % p->len, c[k].type, c[k].value) < 0)
          (*dropped)++;
      }
    }
  return 0;
}

void gr_place_marks (gr_song *s, int t, const gr_pmark *m, int n, int from, int *dropped)
{
  int k, i;
  for (k = 0; k < n; k++)
    {
      int at = from + m[k].tick;
      for (i = 0; i < GR_MAX_REGIONS; i++)
        {
          const gr_region *g = &s->regions[i];
          if (g->used && g->track == t && g->clip < 0 && g->pattern >= 0
              && at >= g->start && at < g->start + g->len)
            {
              gr_pattern *p = &s->patterns[g->pattern];
              gr_mark mk;
              mk.tick = (int16_t) ((at - g->start) % p->len);
              mk.len = (int16_t) (m[k].len < 0 ? 0 : m[k].len > 32767 ? 32767 : m[k].len);
              mk.kind = (uint8_t) m[k].kind;
              mk.value = (int8_t) m[k].value;
              mk.pitch = (uint8_t) m[k].pitch;
              mk.staff = (uint8_t) m[k].staff;
              if (gr_pattern_add_mark (p, &mk) < 0)
                (*dropped)++;
              break;
            }
        }
    }
}

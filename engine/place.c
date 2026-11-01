/* Groove - putting imported notes into regions. See place.h. */
#include <string.h>
#include "place.h"

#define CHUNK_BARS 4
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
  int drums = s->tracks[t].type == TRK_DRUMS, k, pat, reg, len = b - a;
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
    if (m[k].start >= a && m[k].start < b)
      {
        int tick = m[k].start - a, nl = m[k].end - m[k].start;
        if (drums)
          nl = GR_STEP;
        if (tick + nl > len)
          nl = len - tick;
        if (nl < 1)
          nl = 1;
        if (gr_pattern_find_note (p, tick, m[k].key) >= 0)
          continue;                     /* the same key twice at once */
        if (gr_pattern_add_note (p, tick, nl, m[k].key, m[k].vel) < 0)
          (*dropped)++;
      }
  return 0;
}

int gr_place_notes (gr_song *s, int t, const gr_pnote *m, int n, int from, int *full_at, int *dropped)
{
  int k, last = 0, bar0 = gr_tick_bar (s, from), b;
  for (k = 0; k < n; k++)
    if (m[k].start > last)
      last = m[k].start;
  for (b = bar0; gr_bar_tick (s, b) - from <= last;)
    {
      int a = gr_bar_tick (s, b) - from, e = b + CHUNK_BARS, stretch = 0, z;
      /* Don't end the region in the middle of a note. */
      while (crosses (m, n, a, gr_bar_tick (s, e) - from) && stretch < MAX_STRETCH)
        e++, stretch++;
      z = gr_bar_tick (s, e) - from;
      if (count_in (m, n, a, z) <= GR_MAX_NOTES)
        {
          if (region (s, t, m, n, from, a, z, full_at, dropped))
            return -1;
        }
      else
        {
          /* Busy: a region a bar (again never ending inside a note). */
          int c = b;
          while (c < e)
            {
              int d = c + 1, ca = gr_bar_tick (s, c) - from;
              while (d < e && crosses (m, n, ca, gr_bar_tick (s, d) - from))
                d++;
              if (region (s, t, m, n, from, ca, gr_bar_tick (s, d) - from, full_at, dropped))
                return -1;
              c = d;
            }
        }
      b = e;
    }
  return 0;
}

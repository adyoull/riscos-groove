/* Groove - layout and hit testing. See layout.h. */
#include <string.h>
#include "layout.h"

int lay_drum_h = ED_DRUM_MIN;
int lay_drum_rows = GR_DRUM_SOUNDS;
int lay_label_w = ED_LABEL_KEYS;
static int editor_h_fit = 0;    /* the height lay_fit_drum_rows last fitted */

/* ---- arranger ------------------------------------------------------------ */

int lay_tick_to_x (const lay_view *v, int tick)
{
  return HDR_W + (int) ((long long) (tick - v->hscroll) * v->bar_w / GR_BAR);
}

int lay_x_to_tick (const lay_view *v, int x)
{
  long long t = (long long) (x - HDR_W) * GR_BAR;
  /* Round towards minus infinity, so the left edge maps to hscroll. */
  t = t >= 0 ? t / v->bar_w : -((-t + v->bar_w - 1) / v->bar_w);
  return v->hscroll + (int) t;
}

int lay_snap (int tick, int grid)
{
  if (tick < 0)
    return 0;
  return ((tick + grid / 2) / grid) * grid;
}

int lay_track_top (int track)
{
  return LANES_TOP - track * TRACK_H;
}

int lay_work_height (int ntracks)
{
  /* The tracks, plus a row for "drop a loop here / add a track", and never
     less than a useful window's height: the Wimp won't open a window taller
     than its work area, so with two tracks the song window opened only
     242 pixels high. The space below the tracks is the drop zone. */
  int h = TOOL_H + RULER_H + (ntracks + 1) * TRACK_H;
  return h < 2400 ? 2400 : h;
}

/* Header layout (relative to the track's top-left):

     | Name of the track             |
     | [M] [S] [R]   =====o======    |   volume slider
     |                 L ---o--- R   |   pan                                */
lay_box lay_header_box (int what)
{
  lay_box b = { 0, 0, 0, 0 };
  switch (what)
    {
    case HIT_NAME:   b.x0 = 16;  b.x1 = HDR_W - 8;  b.y0 = -44;  b.y1 = -6;  break;
    case HIT_MUTE:   b.x0 = 16;  b.x1 = 60;  b.y0 = -96; b.y1 = -56; break;
    case HIT_SOLO:   b.x0 = 66;  b.x1 = 110; b.y0 = -96; b.y1 = -56; break;
    case HIT_ARM:    b.x0 = 116; b.x1 = 160; b.y0 = -96; b.y1 = -56; break;
    case HIT_VOLUME: b.x0 = 180; b.x1 = HDR_W - 20; b.y0 = -74; b.y1 = -54; break;
    case HIT_PAN:    b.x0 = 180; b.x1 = HDR_W - 20; b.y0 = -102; b.y1 = -82; break;
    }
  return b;
}

static int in_box (lay_box b, int x, int y)
{
  return x >= b.x0 && x < b.x1 && y >= b.y0 && y < b.y1;
}

lay_hit lay_arranger_hit (const lay_view *v, const gr_song *s, int x, int y)
{
  lay_hit h = { HIT_NONE, -1, -1, 0 };
  int track;
  if (y > -TOOL_H)
    return h;                   /* under the toolbar */
  h.tick = lay_x_to_tick (v, x);
  if (y > LANES_TOP)
    {
      if (x >= HDR_W)
        h.what = HIT_RULER;
      return h;
    }
  track = (LANES_TOP - y) / TRACK_H;
  if (track >= s->ntracks)
    {
      h.what = track == s->ntracks ? HIT_ADD_TRACK : HIT_NONE;
      return h;
    }
  h.track = track;
  if (x < HDR_W)
    {
      int ry = y - lay_track_top (track), what;
      for (what = HIT_NAME; what <= HIT_PAN; what++)
        if (in_box (lay_header_box (what), x, ry))
          {
            h.what = what;
            return h;
          }
      h.what = HIT_NAME;        /* anywhere else on the header selects it */
      return h;
    }
  h.region = gr_region_at (s, track, h.tick);
  if (h.region >= 0)
    {
      const gr_region *r = &s->regions[h.region];
      int x1 = lay_tick_to_x (v, r->start + r->len);
      h.what = x >= x1 - EDGE_W ? HIT_REGION_END : HIT_REGION;
    }
  else
    {
      /* Just past a region's end still grabs the end: it is a thin target. */
      int r = gr_region_at (s, track, lay_x_to_tick (v, x - EDGE_W / 2));
      if (r >= 0)
        {
          h.region = r;
          h.what = HIT_REGION_END;
        }
      else
        h.what = HIT_EMPTY_LANE;
    }
  return h;
}

/* ---- toolbar ------------------------------------------------------------- */

/* Left to right; widths in OS units. A zero width is a gap. */
static const struct { int id, w; } toolbar[] = {
  { TB_REWIND, 72 }, { TB_PLAY, 88 }, { TB_RECORD, 72 }, { TB_CYCLE, 72 }, { -1, 16 },
  { TB_LCD, 300 }, { -1, 16 },
  { TB_TEMPO_DOWN, 44 }, { TB_TEMPO, 120 }, { TB_TEMPO_UP, 44 }, { TB_KEY, 80 },
  { TB_METRONOME, 72 }, { -1, 16 },
  { TB_ZOOM_OUT, 52 }, { TB_ZOOM_IN, 52 }, { -1, 16 },
  { TB_LOOPS, 100 }, { TB_EDITOR, 88 }, { TB_KEYS, 88 }, { -1, 16 }, { TB_MASTER, 256 },
};

lay_box lay_toolbar_box (int button)
{
  lay_box b = { 0, -TOOL_H + 16, 0, -16 };
  int x = 16, i;
  for (i = 0; i < (int) (sizeof toolbar / sizeof toolbar[0]); i++)
    {
      if (toolbar[i].id == button)
        {
          b.x0 = x;
          b.x1 = x + toolbar[i].w - 8;
          return b;
        }
      x += toolbar[i].w;
    }
  b.y0 = b.y1 = 0;
  return b;
}

int lay_toolbar_hit (int x, int y)
{
  int i;
  for (i = 0; i < TB_COUNT; i++)
    if (in_box (lay_toolbar_box (i), x, y))
      return i;
  return -1;
}

/* ---- editor --------------------------------------------------------------- */

int lay_editor_height (gr_track_type t)
{
  /* (and room at the bottom for the controller lane pinned there) */
  if (t == TRK_DRUMS)
    return ED_HEAD_H + lay_drum_rows * ED_DRUM_H + ED_LANE_H;
  return ED_HEAD_H + (ED_HIGH - ED_LOW + 1) * ED_KEY_H + ED_LANE_H;
}

int lay_step_w (int width, int len)
{
  int steps = len / GR_STEP, w;
  if (steps < 1)
    steps = 1;
  w = (width - ED_LABEL_W - 8) / steps;
  return w < 8 ? 8 : w;
}

int lay_grid_hit (int width, int len, int x, int y, int *row, int *step)
{
  int w = lay_step_w (width, len), r, st;
  if (x < ED_LABEL_W || y > -ED_HEAD_H)
    return 0;
  r = (-ED_HEAD_H - y) / ED_DRUM_H;
  st = (x - ED_LABEL_W) / w;
  if (r < 0 || r >= lay_drum_rows || st < 0 || st >= len / GR_STEP)
    return 0;
  *row = r;
  *step = st;
  return 1;
}

int lay_roll_y (int pitch)
{
  return -ED_HEAD_H - (ED_HIGH - pitch) * ED_KEY_H;
}

int lay_roll_x (int width, int len, int tick)
{
  return ED_LABEL_W + (int) ((long long) tick * lay_step_w (width, len) / GR_STEP);
}

int lay_roll_hit (int width, int len, int x, int y, int *pitch, int *tick)
{
  int p, t;
  if (y > -ED_HEAD_H)
    return 0;
  p = ED_HIGH - (-ED_HEAD_H - y) / ED_KEY_H;
  if (p < ED_LOW || p > ED_HIGH)
    return 0;
  *pitch = p;
  if (x < ED_LABEL_W)
    {
      *tick = -1;               /* on the keyboard strip */
      return 1;
    }
  t = (int) ((long long) (x - ED_LABEL_W) * GR_STEP / lay_step_w (width, len));
  if (t >= len)
    return 0;
  *tick = t;
  return 1;
}

/* ---- loop browser --------------------------------------------------------- */

int lay_browser_row_y (int i)
{
  int k, y = 0;
  const char *cat = NULL;
  for (k = 0; k <= i && k < gr_loop_count (); k++)
    {
      const gr_loop_info *li = gr_loop_get (k);
      if (!cat || strcmp (cat, li->category))
        {
          y -= LB_HEAD_H;
          cat = li->category;
        }
      if (k < i)
        y -= LB_ROW_H;
    }
  return y;
}

int lay_browser_hit (int y)
{
  int i;
  for (i = 0; i < gr_loop_count (); i++)
    {
      int top = lay_browser_row_y (i);
      if (y <= top && y > top - LB_ROW_H)
        return i;
    }
  return -1;
}

int lay_browser_height (void)
{
  return -lay_browser_row_y (gr_loop_count () - 1) + LB_ROW_H + 8;
}

/* ---- workspace ------------------------------------------------------------ */

lay_box lay_workspace (int sw, int sh, int which)
{
  lay_box b = { 0, 0, 0, 0 };
  int x0 = WS_MARGIN, x1 = sw - WS_MARGIN;
  int y0 = WS_ICONBAR + WS_MARGIN, y1 = sh - WS_MARGIN;
  int loops_x0 = x1 - WS_SCROLL - LB_W;          /* the browser's column */
  int left_x1 = loops_x0 - WS_MARGIN - WS_SCROLL; /* the song and editor */
  int song_h, song_y0;
  if (left_x1 - x0 < 1200)                       /* a small screen: overlap */
    left_x1 = x1 - WS_SCROLL < x0 + 1200 ? x1 - WS_SCROLL : x0 + 1200;
  /* The song gets 55% of the height, title bar included. */
  song_h = (y1 - y0) * 55 / 100;
  song_y0 = y1 - song_h;
  switch (which)
    {
    case WS_SONG:
      b.x0 = x0; b.x1 = left_x1; b.y0 = song_y0; b.y1 = y1 - WS_TITLE;
      break;
    case WS_EDITOR:
      b.x0 = x0; b.x1 = left_x1; b.y0 = y0; b.y1 = song_y0 - WS_MARGIN - WS_TITLE;
      break;
    case WS_LOOPS:
      b.x0 = loops_x0; b.x1 = loops_x0 + LB_W; b.y0 = y0; b.y1 = y1 - WS_TITLE;
      if (b.y1 - b.y0 > lay_browser_height ())       /* no taller than the list */
        b.y0 = b.y1 - lay_browser_height ();
      break;
    case WS_KEYS:
      b.x0 = x0 + WS_MARGIN; b.x1 = b.x0 + WS_KEYS_W; b.y0 = y0 + WS_MARGIN; b.y1 = b.y0 + WS_KEYS_H;
      break;
    }
  return b;
}

/* Rows that fill the editor (a few rows big, more rows smaller); all 128
   keys get small rows and a scroll bar. editor_h 0: the last height. */
int lay_fit_drum_rows (int editor_h)
{
  int h;
  if (editor_h > 0)
    editor_h_fit = editor_h;
  else
    editor_h = editor_h_fit ? editor_h_fit : 8 * ED_DRUM_MIN + ED_HEAD_H + ED_LANE_H;
  if (lay_drum_rows > 24)
    return ED_DRUM_ALL_H;
  h = (editor_h - ED_HEAD_H - ED_LANE_H) / lay_drum_rows;
  h &= ~3;                                       /* whole pixels at any eig */
  return h < ED_DRUM_MIN ? ED_DRUM_MIN : h > 120 ? 120 : h;
}

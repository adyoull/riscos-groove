/* Groove - the song window: the arranger, with its toolbar pane.

   The window scrolls vertically like any other (Wimp scroll bar), but the
   timeline's horizontal position is ours (app.view.hscroll), changed by the
   horizontal scroll bar, the zoom buttons, Shift + the scroll wheel, and by
   following the playhead. That way the track headers on the left never
   scroll away. The Wimp's horizontal scroll bar is kept in step with it:
   the work area is as wide as the song at this zoom, its scroll offset is
   hscroll in OS units, and everything is drawn from the visible left edge,
   so when the Wimp moves the contents sideways the whole window is drawn
   again (arrange_open_request). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include <swis.h>
#include "app.h"
#include "gfx.h"

static char title[300] = APP_NAME;
static int scroll_px;           /* the Wimp's horizontal scroll offset (= hscroll) */

static int ticks_px (long long t) { return (int) (t * app.view.bar_w / GR_BAR); }

/* ---- creating and opening -------------------------------------------------- */

static int create (int *b, int flags, int work_w, int work_h, const char *t, int indirect)
{
  int handle;
  memset (b, 0, 22 * sizeof (int));
  b[0] = 100; b[1] = 200; b[2] = 1500; b[3] = 1000;
  b[6] = -1;
  b[7] = flags;
  ((unsigned char *) b)[32] = 7;  ((unsigned char *) b)[33] = 2;
  ((unsigned char *) b)[34] = 7;  ((unsigned char *) b)[35] = 255;   /* we paint it all */
  ((unsigned char *) b)[36] = 3;  ((unsigned char *) b)[37] = 1;
  ((unsigned char *) b)[38] = 12;
  b[10] = 0; b[11] = -work_h; b[12] = work_w; b[13] = 0;
  b[15] = 3 << 12;              /* work area: report clicks */
  b[16] = 1;
  /* Minimum size. None for a pane: the Wimp enforces the minimum even
     past the work area, so a 200-high minimum made the 104-high toolbar
     cover the ruler and the first track (seen on RISC OS 5.31-5). */
  if (!(flags & 0x20))
    {
      ((short *) b)[34] = 400;
      ((short *) b)[35] = 200;
    }
  if (indirect)
    {
      b[14] = 0x13D;            /* title: text, border, centred, filled, indirected */
      b[18] = (int) t; b[19] = -1; b[20] = (int) strlen (t) + 1;
      if (indirect > b[20]) b[20] = indirect;
    }
  else
    {
      b[14] = 0x3D;
      strncpy ((char *) &b[18], t, 12);
    }
  b[21] = 0;
  if ((app_create_error = _swix (Wimp_CreateWindow, _IN (1) | _OUT (0), b, &handle)) != NULL)
    return -1;
  return handle;
}

int arrange_create (void)
{
  int b[24];
  /* Movable, back, close, title, toggle size, vertical and horizontal
     scroll bars, adjust size, and Scroll_Request events (bit 8) so the
     wheel can move the timeline. */
  app.w_main = create (b, (int) 0xFF000102, 0x4000, lay_work_height (0), title, sizeof title);
  /* The toolbar: a pane (bit 5), no furniture. */
  app.w_tool = create (b, (int) 0x80000020, 0x4000, TOOL_H, "", 0);
  return app.w_main < 0 || app.w_tool < 0;
}

void arrange_title (void)
{
  const char *leaf = app.path[0] ? app.path : "<Untitled>";
  snprintf (title, sizeof title, "%s%s", leaf, app.unsaved ? " *" : "");
  if (app.main_open)
    {
      /* Redraw just the title bar: ForceRedraw with the window's outline. */
      int o[11];
      o[0] = app.w_main;
      _swix (Wimp_GetWindowOutline, _IN (1), o);
      _swix (Wimp_ForceRedraw, _INR (0, 4), -1, o[1], o[4] - 48, o[3], o[4]);
    }
}

void arrange_open (void)
{
  app_open_window (app.w_main, WS_SONG);
  app.main_open = 1;
}

/* The work area: as wide as the song (or the place we've scrolled to)
   at this zoom, and a quarter of a window more, so scrolled to the far
   right the end of the music is in view with a little room after it. */
void arrange_set_extent (void)
{
  int end = gr_bar_tick (&app.song, app.song.bars), w;
  if (gr_song_end (&app.song) > end)
    end = gr_song_end (&app.song);
  if (app.view.hscroll > end)
    end = app.view.hscroll;
  w = HDR_W + ticks_px (end) + (app.view.width > 640 ? app.view.width / 4 : 160);
  app_set_extent (app.w_main, w, lay_work_height (app.song.ntracks));
}

/* hscroll has changed: move the scroll bar to match, and draw it all. */
void arrange_sync_hscroll (void)
{
  int s[10];
  if (!app.main_open)
    return;
  arrange_set_extent ();
  s[0] = app.w_main;
  if (!_swix (Wimp_GetWindowState, _IN (1), s) && s[5] != ticks_px (app.view.hscroll))
    {
      s[5] = ticks_px (app.view.hscroll);
      scroll_px = s[5];             /* (so the open isn't taken as a drag of the bar) */
      arrange_open_request (s);
    }
  app_redraw_all (app.w_main);
}

/* Force a redraw of part of the window, in our coordinates (from the
   visible left edge). */
static void redraw_main (int x0, int y0, int x1, int y1)
{
  app_redraw_work (app.w_main, x0 + scroll_px, y0, x1 + scroll_px, y1);
}

/* Open the main window as asked, then the pane exactly over its top, then
   the main window again just behind the pane (the classic pane dance). */
void arrange_open_request (int *b)
{
  int s[10], p[8], moved = 0;
  if (b[5] != scroll_px)
    {
      /* The horizontal scroll bar was moved: that's our timeline position. */
      app.view.hscroll = (int) ((long long) (b[5] < 0 ? 0 : b[5]) * GR_BAR / app.view.bar_w);
      moved = 1;
    }
  app_check (_swix (Wimp_OpenWindow, _IN (1), b));
  s[0] = app.w_main;
  _swix (Wimp_GetWindowState, _IN (1), s);
  p[0] = app.w_tool;
  p[1] = s[1];
  p[3] = s[3];
  p[4] = s[4];
  p[2] = s[4] - TOOL_H;
  p[5] = 0; p[6] = 0;
  p[7] = s[7];                  /* where main was in the stack */
  _swix (Wimp_OpenWindow, _IN (1), p);
  s[7] = app.w_tool;
  _swix (Wimp_OpenWindow, _IN (1), s);
  scroll_px = s[5];
  if (app.view.width != s[3] - s[1])
    {
      app.view.width = s[3] - s[1];
      app_redraw_all (app.w_tool);
      arrange_set_extent ();
    }
  app.main_open = 1;
  if (moved)
    app_redraw_all (app.w_main);    /* (the Wimp moved the headers with the rest) */
}

void arrange_close (void)
{
  int b[1];
  gr_stop (app.eng);
  b[0] = app.w_tool;
  _swix (Wimp_CloseWindow, _IN (1), b);
  b[0] = app.w_main;
  _swix (Wimp_CloseWindow, _IN (1), b);
  app.main_open = 0;
}

void arrange_new_song (void)
{
  gr_stop (app.eng);
  gr_stop (app.eng);
  gr_song_free (&app.song);
  gr_song_init (&app.song);
  gr_engine_set_song (app.eng, &app.song);
  /* Start with something to play: a drummer and a piano, the piano
     selected, so musical typing makes a sound straight away. */
  gr_track_add (&app.song, TRK_DRUMS, "Drums", KIT_STUDIO);
  gr_track_add (&app.song, TRK_INST, gr_patch_name (PATCH_EPIANO), PATCH_EPIANO);
  app.sel_track = 1;
  app.sel_region = -1;
  app.view.hscroll = 0;
  app.path[0] = 0;
  arrange_set_extent ();
  editor_show (-1);
  app_changed ();
  if (app.hist)
    gr_history_reset (app.hist, &app.song);
  app.unsaved = 0;
  arrange_title ();
}

/* ---- the heard position ---------------------------------------------------- */

/* The engine runs ahead of the speaker by what is queued in the ring, so
   the playhead shows the engine position less that latency. */
static double heard_position (void)
{
  double spt = GR_RATE * 60.0 / ((double) app.song.tempo * GR_PPQ);
  double pos = gr_position (app.eng);
  if (gr_playing (app.eng))
    pos -= sound_latency () / spt;
  return pos < 0 ? 0 : pos;
}

/* ---- drawing the arranger -------------------------------------------------- */

/* VDU 24: set the graphics window (OS units, inclusive). Used to clip a
   region's name to the region; afterwards the Wimp's rectangle (exclusive
   top and right, as in the redraw block) is put back. */
static void graphics_window (int x0, int y0, int x1, int y1)
{
  char v[9];
  v[0] = 24;
  v[1] = (char) (x0 & 255); v[2] = (char) ((x0 >> 8) & 255);
  v[3] = (char) (y0 & 255); v[4] = (char) ((y0 >> 8) & 255);
  v[5] = (char) (x1 & 255); v[6] = (char) ((x1 >> 8) & 255);
  v[7] = (char) (y1 & 255); v[8] = (char) ((y1 >> 8) & 255);
  _swix (OS_WriteN, _INR (0, 1), v, 9);
}

static unsigned track_colour (const gr_track *t, int dark)
{
  return gfx_track_colour (t->type, t->patch, t->gm, dark);
}

static void button (int x0, int y0, int x1, int y1, const char *label, int on, unsigned on_col)
{
  gfx_colour (on ? on_col : C_BUTTON);
  gfx_round (x0, y0, x1, y1, 8);
  gfx_text ((x0 + x1) / 2, y0 + (y1 - y0) / 2 - 8, label, on ? 0x101010 : C_TEXT, on ? on_col : C_BUTTON, 1);
}

#define MASTER_X0 72            /* the master slider within its toolbar box */
#define MASTER_X1 20

static void slider (int x0, int y0, int x1, int y1, float frac, unsigned col)
{
  int k = x0 + (int) (frac * (x1 - x0));
  int mid = (y0 + y1) / 2;
  gfx_colour (0x1E1F22);
  gfx_round (x0, mid - 4, x1, mid + 4, 4);
  gfx_colour (col);
  gfx_round (x0, mid - 4, k, mid + 4, 4);
  gfx_colour (0xDADCE0);
  gfx_circle (k, mid, 10);
}

static void draw_header (int ox, int oy, const int *clip, int t)
{
  const gr_track *tr = &app.song.tracks[t];
  int top = oy + lay_track_top (t), x0 = ox, sel = t == app.sel_track;
  lay_box b;
  char label[40];
  if (!gfx_visible (clip, x0, top - TRACK_H, x0 + HDR_W, top))
    return;
  gfx_colour (sel ? C_HEADER_SEL : C_HEADER);
  gfx_rect (x0, top - TRACK_H, x0 + HDR_W, top);
  gfx_colour (track_colour (tr, 0));
  gfx_rect (x0, top - TRACK_H, x0 + 8, top);         /* the track's colour tab */
  gfx_colour (C_BG);
  gfx_rect (x0, top - TRACK_H, x0 + HDR_W, top - TRACK_H + (1 << app.yeig));

  b = lay_header_box (HIT_NAME);
  snprintf (label, sizeof label, "%s", tr->name);
  gfx_text (x0 + b.x0, top + b.y0 + 10, label, C_TEXT, sel ? C_HEADER_SEL : C_HEADER, 0);
  if (tr->type != TRK_AUDIO)
    gfx_text (x0 + b.x1, top + b.y0 + 10,
              tr->midi_out && midiout_on () ? "MIDI out" : tr->type == TRK_DRUMS ? gr_sound_name (tr) : "",
              C_TEXT_DIM, sel ? C_HEADER_SEL : C_HEADER, 2);

  b = lay_header_box (HIT_MUTE);
  button (x0 + b.x0, top + b.y0, x0 + b.x1, top + b.y1, "M", tr->mute, C_MUTE);
  b = lay_header_box (HIT_SOLO);
  button (x0 + b.x0, top + b.y0, x0 + b.x1, top + b.y1, "S", tr->solo, C_SOLO);
  b = lay_header_box (HIT_ARM);
  button (x0 + b.x0, top + b.y0, x0 + b.x1, top + b.y1, "R", tr->armed, C_RECORD);

  b = lay_header_box (HIT_VOLUME);
  slider (x0 + b.x0, top + b.y0, x0 + b.x1, top + b.y1, tr->vol, track_colour (tr, 0));
  b = lay_header_box (HIT_PAN);
  slider (x0 + b.x0, top + b.y0, x0 + b.x1, top + b.y1, (tr->pan + 1.0f) / 2.0f, 0x777C88);

  /* The level meter along the bottom edge. */
  {
    int w = (int) (tr->meter > 1.0f ? HDR_W - 24 : tr->meter * (HDR_W - 24));
    gfx_colour (0x1E1F22);
    gfx_rect (x0 + 16, top - TRACK_H + 4, x0 + HDR_W - 8, top - TRACK_H + 10);
    gfx_colour (tr->meter > 0.95f ? C_RECORD : 0x58D26E);
    gfx_rect (x0 + 16, top - TRACK_H + 4, x0 + 16 + w, top - TRACK_H + 10);
  }
}

/* What's inside a region, so you can see it at a glance: a waveform for
   audio, little notes for the rest, and a notch where a loop repeats.
   (x0..x1, y0..y1) is the area to draw in, ox the window's x origin. */
static void draw_region_content (int ox, int x0, int y0, int x1, int y1,
                                 const gr_region *r, const gr_track *tr)
{
  const lay_view *v = &app.view;
  int px = 1 << app.xeig;
  if (y1 - y0 < 8)
    return;
  if (r->clip >= 0)
    {
      const gr_clip *c = &app.song.clips[r->clip];
      double spt = GR_RATE * 60.0 / ((double) app.song.tempo * GR_PPQ);
      double frames_per_tick = spt * c->rate / GR_RATE;
      int x, mid = (y0 + y1) / 2, half = (y1 - y0) / 2 - 2;
      if (!c->used || !c->data)
        return;
      gfx_colour (C_AUDIO_DK);
      for (x = x0; x < x1; x += px)
        {
          int f0 = (int) ((lay_x_to_tick (v, x - ox) - r->start) * frames_per_tick) + r->clip_offset;
          int f1 = (int) ((lay_x_to_tick (v, x + px - ox) - r->start) * frames_per_tick) + r->clip_offset;
          int f, lo = 0, hi = 0, step;
          if (f0 < 0) f0 = 0;
          if (f1 > c->frames) f1 = c->frames;
          step = (f1 - f0) / 32 + 1;      /* sample a column: fast enough on a Pi */
          for (f = f0; f < f1; f += step)
            {
              int sv = c->data[2 * f];
              if (sv < lo) lo = sv;
              if (sv > hi) hi = sv;
            }
          gfx_rect (x, mid + lo * half / 32768, x + px, mid + hi * half / 32768 + px);
        }
      return;
    }
  if (r->pattern >= 0 && app.song.patterns[r->pattern].used)
    {
      const gr_pattern *p = &app.song.patterns[r->pattern];
      int lo = 127, hi = 0, k, rep, h = y1 - y0;
      if (p->nnotes == 0 || p->len <= 0)
        return;
      if (tr->type == TRK_DRUMS)
        { lo = 0; hi = GR_DRUM_SOUNDS - 1; }
      else
        for (k = 0; k < p->nnotes; k++)
          {
            if (p->notes[k].pitch < lo) lo = p->notes[k].pitch;
            if (p->notes[k].pitch > hi) hi = p->notes[k].pitch;
          }
      if (hi - lo < 12) { lo -= (12 - (hi - lo)) / 2; hi = lo + 12; }
      gfx_colour (track_colour (tr, 1));
      for (rep = 0; rep * p->len < r->len; rep++)
        {
          int base = r->start + rep * p->len;
          int bx = ox + lay_tick_to_x (v, base);
          if (bx >= x1)
            break;
          if (rep && bx > x0)
            gfx_rect (bx, y0, bx + px, y0 + 12);         /* the loop notch */
          if (ox + lay_tick_to_x (v, base + p->len) < x0)
            continue;                                   /* scrolled off the left */
          for (k = 0; k < p->nnotes; k++)
            {
              const gr_note *n = &p->notes[k];
              int t0 = base + n->tick, len = tr->type == TRK_DRUMS ? GR_STEP / 2 : n->len;
              int nx0, nx1, ny;
              if (t0 >= r->start + r->len)
                break;
              nx0 = ox + lay_tick_to_x (v, t0);
              nx1 = ox + lay_tick_to_x (v, t0 + len);
              if (nx1 < x0 || nx0 >= x1)
                continue;
              if (nx0 < x0) nx0 = x0;
              if (nx1 > x1) nx1 = x1;
              if (nx1 < nx0 + 4) nx1 = nx0 + 4;
              ny = y0 + 4 + (int) ((long) (n->pitch - lo) * (h - 12) / (hi - lo));
              gfx_rect (nx0, ny, nx1, ny + 4);
            }
        }
    }
}

static void draw_regions (int ox, int oy, const int *clip)
{
  const lay_view *v = &app.view;
  int i;
  for (i = 0; i < GR_MAX_REGIONS; i++)
    {
      const gr_region *r = &app.song.regions[i];
      const gr_track *tr;
      int x0, x1, top, y0, y1;
      if (!r->used || r->track >= app.song.ntracks)
        continue;
      tr = &app.song.tracks[r->track];
      x0 = ox + lay_tick_to_x (v, r->start);
      x1 = ox + lay_tick_to_x (v, r->start + r->len);
      if (x0 < ox + HDR_W) x0 = ox + HDR_W;
      if (x1 <= x0)
        continue;
      top = oy + lay_track_top (r->track);
      y1 = top - 6;
      y0 = top - TRACK_H + 6;
      if (!gfx_visible (clip, x0, y0, x1, y1))
        continue;
      if (i == app.sel_region)
        {
          gfx_colour (0xFFFFFF);
          gfx_round (x0 - 4, y0 - 4, x1 + 4, y1 + 4, 12);
        }
      gfx_colour (track_colour (tr, 0));
      gfx_round (x0, y0, x1, y1, 10);
      gfx_colour (track_colour (tr, 1));
      gfx_rect (x0 + 4, y1 - 34, x1 - 4, y1 - 4);       /* name band */
      {
        const char *name = r->clip >= 0 ? app.song.clips[r->clip].name
                         : r->pattern >= 0 ? app.song.patterns[r->pattern].name : "";
        if (x1 - x0 > 60)
          {
            /* Clip the name to the region. */
            int gx0 = x0 + 8 > clip[0] ? x0 + 8 : clip[0];
            int gy0 = y1 - 34 > clip[1] ? y1 - 34 : clip[1];
            int gx1 = x1 - 8 < clip[2] ? x1 - 8 : clip[2];
            int gy1 = y1 - 4 < clip[3] ? y1 - 4 : clip[3];
            if (gx1 > gx0 && gy1 > gy0)
              {
                graphics_window (gx0, gy0, gx1 - 1, gy1 - 1);
                gfx_text (x0 + 12, y1 - 26, name, 0xFFFFFF, track_colour (tr, 1), 0);
                graphics_window (clip[0], clip[1], clip[2] - 1, clip[3] - 1);
              }
          }
      }
      draw_region_content (ox, x0 + 4, y0 + 4, x1 - 4, y1 - 36, r, tr);
    }
}

static void paint_main (int ox, int oy, const int *clip)
{
  const lay_view *v = &app.view;
  int ruler_top = oy - TOOL_H, ruler_bot = ruler_top - RULER_H;
  int lanes_bot = oy + lay_track_top (app.song.ntracks);
  int bar, first_bar, last_bar, t, px = 1 << app.xeig;
  char buf[16];

  /* Background, lanes. */
  gfx_colour (C_BG);
  gfx_rect (clip[0], clip[1], clip[2], clip[3]);
  for (t = 0; t < app.song.ntracks; t++)
    {
      int top = oy + lay_track_top (t);
      gfx_colour (t & 1 ? C_LANE_ALT : C_LANE);
      gfx_rect (ox + HDR_W, top - TRACK_H, clip[2], top);
    }

  /* Grid lines and the ruler. */
  first_bar = gr_tick_bar (&app.song, lay_x_to_tick (v, clip[0] - ox)) - 1;
  last_bar = gr_tick_bar (&app.song, lay_x_to_tick (v, clip[2] - ox)) + 1;
  if (first_bar < 0) first_bar = 0;
  gfx_colour (C_RULER);
  gfx_rect (ox + HDR_W, ruler_bot, clip[2], ruler_top);
  if (app.song.loop_on)
    {
      gfx_colour (C_CYCLE);
      gfx_rect (ox + lay_tick_to_x (v, app.song.loop_start), ruler_bot + 6,
                ox + lay_tick_to_x (v, app.song.loop_end), ruler_bot + 18);
    }
  for (bar = first_bar; bar <= last_bar; bar++)
    {
      int t0 = gr_bar_tick (&app.song, bar), x = ox + lay_tick_to_x (v, t0), beat;
      int blen = gr_beat_len (&app.song, bar), beats = gr_meter_at (&app.song, bar)->num;
      const gr_meter *m = gr_meter_at (&app.song, bar);
      if (x < ox + HDR_W && lay_tick_to_x (v, gr_bar_tick (&app.song, bar + 1)) + ox < ox + HDR_W)
        continue;
      if (x >= ox + HDR_W)
        {
          gfx_colour (bar < app.song.bars ? C_GRID_BAR : C_GRID);
          gfx_rect (x, lanes_bot, x + px, ruler_top);
        }
      if ((long long) blen * v->bar_w / GR_BAR >= 22)       /* beats, if there's room */
        for (beat = 1; beat < beats; beat++)
          {
            int bx = ox + lay_tick_to_x (v, t0 + beat * blen);
            if (bx < ox + HDR_W)
              continue;
            gfx_colour (C_GRID);
            gfx_rect (bx, lanes_bot, bx + px, ruler_bot);
            gfx_colour (C_TEXT_DIM);
            gfx_rect (bx, ruler_bot, bx + px, ruler_bot + 10);
          }
      if (x >= ox + HDR_W && (v->bar_w >= 64 || bar % 4 == 0 || (bar && m->bar == bar)))
        {
          snprintf (buf, sizeof buf, "%d", bar + 1);
          gfx_text (x + 8, ruler_bot + 14, buf, C_TEXT, C_RULER, 0);
          /* Where the time signature changes (and at the start if it isn't 4/4). */
          if (m->bar == bar && (bar > 0 || m->num != 4 || m->den != 4))
            {
              char ts[12];
              snprintf (ts, sizeof ts, "%d/%d", m->num, m->den);
              gfx_text (x + 12 + gfx_text_width (buf), ruler_bot + 14, ts, C_CYCLE, C_RULER, 0);
            }
        }
    }
  /* The end of the song: everything after is dimmed. */
  {
    int ex = ox + lay_tick_to_x (v, gr_bar_tick (&app.song, app.song.bars));
    if (ex < clip[2])
      {
        gfx_colour (0x1F2023);
        gfx_rect (ex > ox + HDR_W ? ex : ox + HDR_W, lanes_bot, clip[2], ruler_bot);
      }
  }

  draw_regions (ox, oy, clip);

  /* Headers (drawn after the regions, so regions slide under them). */
  gfx_colour (C_HEADER);
  gfx_rect (ox, ruler_bot, ox + HDR_W, ruler_top);
  gfx_text (ox + 16, ruler_bot + 14, "Tracks", C_TEXT_DIM, C_HEADER, 0);
  for (t = 0; t < app.song.ntracks; t++)
    draw_header (ox, oy, clip, t);

  /* The "add a track" row. */
  if (app.song.ntracks < GR_MAX_TRACKS)
    {
      int top = lanes_bot;
      gfx_text (ox + 16, top - 30, "New track:", C_TEXT_DIM, C_BG, 0);
      button (ox + 12, top - 92, ox + 116, top - 46, "Drums", 0, 0);
      button (ox + 124, top - 92, ox + 236, top - 46, "Keys", 0, 0);
      button (ox + 244, top - 92, ox + HDR_W - 8, top - 46, "Audio", 0, 0);
      gfx_text (ox + HDR_W + 24, top - 46,
                app.song.ntracks ? "Drag loops here from the Loop browser (F7)"
                                 : "Add a track, or drag in a loop from the Loop browser (F7)",
                C_TEXT_DIM, C_BG, 0);
    }

  /* The playhead. */
  {
    int x = ox + lay_tick_to_x (v, (int) heard_position ());
    if (x >= ox + HDR_W)
      {
        gfx_colour (C_PLAYHEAD);
        gfx_rect (x, lanes_bot, x + px, ruler_bot);
        gfx_tri (x - 12, ruler_top - 4, x + 12, ruler_top - 4, x, ruler_bot + 4);
      }
  }
}

void arrange_redraw (int *b)
{
  int more;
  _swix (Wimp_RedrawWindow, _IN (1) | _OUT (0), b, &more);
  while (more)
    {
      paint_main (b[1], b[4] - b[6], &b[7]);     /* x from the visible left edge */
      _swix (Wimp_GetRectangle, _IN (1) | _OUT (0), b, &more);
    }
}

/* ---- the toolbar --------------------------------------------------------- */

static int snap_down (int tick);

static void lcd_text (char *buf, size_t n)
{
  double pos = heard_position ();
  int b = gr_tick_bar (&app.song, (int) pos);
  int beat = ((int) pos - gr_bar_tick (&app.song, b)) / gr_beat_len (&app.song, b) + 1;
  snprintf (buf, n, "%3d . %d", b + 1, beat);
}

static void paint_tool (int ox, int oy, const int *clip)
{
  int i, playing = gr_playing (app.eng), rec = gr_recording (app.eng);
  char buf[32];
  gfx_colour (0x34363B);
  gfx_rect (clip[0], clip[1], clip[2], clip[3]);
  gfx_colour (0x1E1F22);
  gfx_rect (ox, oy - TOOL_H, ox + 0x4000, oy - TOOL_H + (1 << app.yeig));
  for (i = 0; i < TB_COUNT; i++)
    {
      lay_box b = lay_toolbar_box (i);
      int x0 = ox + b.x0, y0 = oy + b.y0, x1 = ox + b.x1, y1 = oy + b.y1;
      int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
      if (!gfx_visible (clip, x0, y0, x1, y1))
        continue;
      switch (i)
        {
        case TB_LCD:
          gfx_colour (C_LCD);
          gfx_round (x0, y0, x1, y1, 10);
          lcd_text (buf, sizeof buf);
          gfx_text (x0 + 20, cy - 10, buf, C_LCD_TEXT, C_LCD, 0);
          if (gr_counting_in (app.eng))
            snprintf (buf, sizeof buf, "COUNT %d", gr_counting_in (app.eng));
          else
            snprintf (buf, sizeof buf, "%s", rec ? "REC" : playing ? "PLAY" : "");
          gfx_text (x1 - 20, cy - 10, buf, rec ? C_RECORD : C_LCD_TEXT, C_LCD, 2);
          if (playing && sound_underruns () != app.underruns_at_play)
            gfx_text (x1 - 100, cy - 10, "!", C_RECORD, C_LCD, 2);   /* the sound glitched */
          if (midiin_active ())
            {
              /* The MIDI light: a small dot, lit while notes come in. */
              gfx_colour (midiin_lit () ? C_LCD_TEXT : 0x2E4A38);
              gfx_circle (x0 + 10, y1 - 10, 4);
            }
          break;
        case TB_TEMPO:
          gfx_colour (C_LCD);
          gfx_round (x0, y0, x1, y1, 10);
          snprintf (buf, sizeof buf, "%d bpm", app.song.tempo);
          gfx_text (cx, cy - 10, buf, C_LCD_TEXT, C_LCD, 1);
          break;
        case TB_KEY:
          gfx_colour (C_LCD);
          gfx_round (x0, y0, x1, y1, 10);
          gfx_text (cx, cy - 10, gr_key_name (app.song.key), C_LCD_TEXT, C_LCD, 1);
          break;
        case TB_MASTER:           /* the master volume: "Vol" and a slider */
          gfx_colour (C_LCD);
          gfx_round (x0, y0, x1, y1, 10);
          gfx_text (x0 + 14, cy - 10, "Vol", C_LCD_TEXT, C_LCD, 0);
          slider (x0 + MASTER_X0, y0, x1 - MASTER_X1, y1, app.song.master, C_LCD_TEXT);
          break;
        default:
          {
            int on = (i == TB_PLAY && playing) || (i == TB_RECORD && rec)
                     || (i == TB_CYCLE && app.song.loop_on) || (i == TB_METRONOME && app.song.metronome)
                     || (i == TB_LOOPS && app.loops_open) || (i == TB_EDITOR && app.edit_open)
                     || (i == TB_KEYS && app.keys_open);
            unsigned col = i == TB_RECORD ? C_RECORD : i == TB_CYCLE ? C_CYCLE : C_BUTTON_ON;
            gfx_colour (on ? col : C_BUTTON);
            gfx_round (x0, y0, x1, y1, 10);
            gfx_colour (on && i != TB_PLAY ? 0x101010 : C_TEXT);
            switch (i)
              {
              case TB_REWIND:
                gfx_rect (cx - 18, cy - 14, cx - 14, cy + 14);
                gfx_tri (cx - 14, cy, cx + 2, cy + 14, cx + 2, cy - 14);
                gfx_tri (cx + 2, cy, cx + 18, cy + 14, cx + 18, cy - 14);
                break;
              case TB_PLAY:
                if (playing)
                  gfx_rect (cx - 12, cy - 12, cx + 12, cy + 12);
                else
                  gfx_tri (cx - 12, cy - 16, cx - 12, cy + 16, cx + 16, cy);
                break;
              case TB_RECORD:
                gfx_colour (on ? 0xFFFFFF : C_RECORD);
                gfx_circle (cx, cy, 14);
                break;
              case TB_CYCLE:
                gfx_frame (cx - 18, cy - 10, cx + 18, cy + 10);
                gfx_tri (cx + 10, cy + 18, cx + 10, cy + 2, cx + 22, cy + 10);
                break;
              case TB_METRONOME:
                gfx_tri (cx - 14, cy - 16, cx + 14, cy - 16, cx, cy + 18);
                gfx_colour (on ? 0xFFFFFF : C_BUTTON);
                gfx_line (cx, cy - 8, cx + 12, cy + 14);
                break;
              case TB_TEMPO_DOWN: case TB_ZOOM_OUT:
                gfx_rect (cx - 10, cy - 2, cx + 10, cy + 2);
                break;
              case TB_TEMPO_UP: case TB_ZOOM_IN:
                gfx_rect (cx - 10, cy - 2, cx + 10, cy + 2);
                gfx_rect (cx - 2, cy - 10, cx + 2, cy + 10);
                break;
              default:
                gfx_text (cx, cy - 10, i == TB_LOOPS ? "Loops" : i == TB_EDITOR ? "Edit" : "Keys",
                          on ? 0x101010 : C_TEXT, on ? col : C_BUTTON, 1);
                break;
              }
          }
        }
    }
  if (clip[2] > ox + lay_toolbar_box (TB_MASTER).x1 + 24)
    gfx_text (ox + lay_toolbar_box (TB_MASTER).x1 + 24, oy - TOOL_H / 2 - 10,
              "Space play/stop   R record   A-K play notes   Z/X octave",
              C_TEXT_DIM, 0x34363B, 0);
}

void arrange_tool_redraw (int *b)
{
  int more;
  _swix (Wimp_RedrawWindow, _IN (1) | _OUT (0), b, &more);
  while (more)
    {
      paint_tool (b[1] - b[5], b[4] - b[6], &b[7]);
      _swix (Wimp_GetRectangle, _IN (1) | _OUT (0), b, &more);
    }
}

static void redraw_tool_button (int i);
void arrange_midi_light (void)
{
  if (app.main_open)
    redraw_tool_button (TB_LCD);
}

static void redraw_tool_button (int i)
{
  lay_box b = lay_toolbar_box (i);
  app_redraw_work (app.w_tool, b.x0, b.y0, b.x1, b.y1);
}

/* ---- transport --------------------------------------------------------------- */

static void transport_changed (void)
{
  app_redraw_all (app.w_tool);
  if (app.main_open)
    arrange_sync_hscroll ();        /* (Rewind and Return go back to the start) */
}

static void play_stop (void)
{
  if (gr_playing (app.eng) || gr_counting_in (app.eng))
    gr_stop (app.eng);
  else
    {
      browser_stop_preview ();
      app.underruns_at_play = sound_underruns ();
      gr_play (app.eng);
    }
  transport_changed ();
}

static void record (void)
{
  int t, any = 0;
  if (gr_recording (app.eng))
    {
      gr_stop (app.eng);
      app_changed ();
      transport_changed ();
      return;
    }
  /* Nothing armed? Arm the selected track. */
  for (t = 0; t < app.song.ntracks; t++)
    any |= app.song.tracks[t].armed;
  if (!any && app.sel_track >= 0 && app.sel_track < app.song.ntracks)
    app.song.tracks[app.sel_track].armed = 1;
  if (gr_playing (app.eng))
    gr_stop (app.eng);          /* (a stop when stopped would rewind) */
  /* From the start of the bar we're in, so there's a moment to come in on. */
  gr_locate (app.eng, gr_bar_tick (&app.song, gr_tick_bar (&app.song, (int) gr_position (app.eng))));
  app.underruns_at_play = sound_underruns ();
  gr_set_count_in (app.eng, app.count_in ? 4 : 0);   /* a bar of 4/4 */
  gr_set_merge (app.eng, app.merge);
  gr_record (app.eng);
  transport_changed ();
}

static void zoom (int dir)
{
  /* Zoom about the playhead when it is in view, so
     what you are working on stays put; otherwise keep the left edge. */
  int anchor = (int) heard_position ();
  int x = lay_tick_to_x (&app.view, anchor);
  int w = dir > 0 ? app.view.bar_w * 2 : app.view.bar_w / 2;
  if (w < 40 || w > 2560)
    return;
  if (x < HDR_W || x >= app.view.width)
    {
      anchor = app.view.hscroll;
      x = HDR_W;
    }
  app.view.bar_w = w;
  app.view.hscroll = anchor - (int) ((long long) (x - HDR_W) * GR_BAR / w);
  if (app.view.hscroll < 0) app.view.hscroll = 0;
  arrange_sync_hscroll ();
}

static void drag_master (int x, int y, int done)
{
  int ox, oy;
  lay_box bx = lay_toolbar_box (TB_MASTER);
  float f;
  (void) y; (void) done;
  app_window_origin (app.w_tool, &ox, &oy);
  f = (float) (x - ox - bx.x0 - MASTER_X0) / (float) (bx.x1 - MASTER_X1 - bx.x0 - MASTER_X0);
  app.song.master = f < 0 ? 0.0f : f > 1 ? 1.0f : f;
  redraw_tool_button (TB_MASTER);
}

void arrange_tool_click (int *b)
{
  int ox, oy, i;
  app_window_origin (app.w_tool, &ox, &oy);
  if (b[2] == BUT_MENU)
    {
      app_main_menu (b[0], b[1]);
      return;
    }
  i = lay_toolbar_hit (b[0] - ox, b[1] - oy);
  switch (i)
    {
    case TB_REWIND:
      gr_locate (app.eng, 0);
      app.view.hscroll = 0;
      transport_changed ();
      break;
    case TB_PLAY: play_stop (); break;
    case TB_RECORD: record (); break;
    case TB_CYCLE:
      app.song.loop_on = !app.song.loop_on;
      if (app.song.loop_end <= app.song.loop_start)
        app.song.loop_end = gr_bar_tick (&app.song, gr_tick_bar (&app.song, app.song.loop_start) + 4);
      transport_changed ();
      break;
    case TB_TEMPO_DOWN: case TB_TEMPO_UP: case TB_TEMPO:
      {
        /* Select steps by 1, Adjust by 10 (the RISC OS way to go faster). */
        int step = b[2] == BUT_ADJUST ? 10 : 1;
        if (i == TB_TEMPO_DOWN) app.song.tempo -= step;
        else if (i == TB_TEMPO_UP) app.song.tempo += step;
        else app.song.tempo += b[2] == BUT_ADJUST ? -5 : 5;
        if (app.song.tempo < 40) app.song.tempo = 40;
        if (app.song.tempo > 240) app.song.tempo = 240;
        app.unsaved = 1;
        redraw_tool_button (TB_TEMPO);
        if (app.edit_open) app_redraw_all (app.w_edit);
        app_redraw_all (app.w_main);     /* audio regions change length on screen */
      }
      break;
    case TB_KEY:
      app.song.key = (app.song.key + (b[2] == BUT_ADJUST ? 11 : 1)) % 12;
      app.unsaved = 1;
      redraw_tool_button (TB_KEY);
      break;
    case TB_METRONOME:
      app.song.metronome = !app.song.metronome;
      redraw_tool_button (TB_METRONOME);
      break;
    case TB_ZOOM_OUT: zoom (-1); break;
    case TB_ZOOM_IN: zoom (1); break;
    case TB_LOOPS: browser_open (); break;
    case TB_EDITOR: editor_open (); break;
    case TB_KEYS: keys_open (); break;
    case TB_MASTER:
      drag_master (b[0], b[1], 0);
      app_start_drag (app.w_tool, drag_master);
      break;
    }
}

/* ---- split, copy and paste ------------------------------------------------- */


static gr_region clip_region;   /* the copied region, with its own copy of its notes */
static gr_pattern clip_pattern;
static gr_track_type clip_type;
static unsigned clip_audio_id;
static int clip_have;

int arrange_can_paste (void) { return clip_have; }

static int region_ok (int r)
{
  return r >= 0 && r < GR_MAX_REGIONS && app.song.regions[r].used;
}

void arrange_copy (void)
{
  const gr_region *r;
  if (!region_ok (app.sel_region))
    return;
  r = &app.song.regions[app.sel_region];
  clip_region = *r;
  clip_type = app.song.tracks[r->track].type;
  if (r->pattern >= 0 && r->clip < 0)
    clip_pattern = app.song.patterns[r->pattern];
  clip_audio_id = r->clip >= 0 ? app.song.clips[r->clip].id : 0;
  clip_have = 1;
}

/* At the playhead (on the bar, or the beat when zoomed in), on the
   selected track if it's the right kind, else the first one that is. */
void arrange_paste (void)
{
  int t = app.sel_track, tick, n;
  if (!clip_have)
    return;
  if (t < 0 || t >= app.song.ntracks || app.song.tracks[t].type != clip_type)
    for (t = 0; t < app.song.ntracks && app.song.tracks[t].type != clip_type; t++)
      ;
  if (t >= app.song.ntracks)
    {
      app_error (clip_type == TRK_DRUMS ? "There's no drum track to paste onto: add one first."
                 : clip_type == TRK_INST ? "There's no instrument track to paste onto: add one first."
                 : "There's no audio track to paste onto: add one first.");
      return;
    }
  if (clip_region.clip >= 0
      && (clip_region.clip >= GR_MAX_CLIPS || app.song.clips[clip_region.clip].id != clip_audio_id
          || !app.song.clips[clip_region.clip].data))
    {
      app_error ("The copied recording isn't in this song any more.");
      return;
    }
  tick = snap_down ((int) gr_position (app.eng));
  n = gr_region_copy (&app.song, &clip_region, &clip_pattern, t, tick);
  if (n < 0)
    {
      app_error ("There's no room for another region in this song.");
      return;
    }
  app.sel_region = n;
  app.sel_track = t;
  editor_show (n);
  app_changed ();
}

/* Split the selected region at the playhead (or the region under the
   playhead on the selected track, if none is selected there). */
void arrange_split (void)
{
  int r = app.sel_region, tick = (int) gr_position (app.eng), n;
  if (!region_ok (r) || tick <= app.song.regions[r].start
      || tick >= app.song.regions[r].start + app.song.regions[r].len)
    r = gr_region_at (&app.song, app.sel_track, tick);
  if (!region_ok (r))
    {
      app_error ("Put the playhead inside the region you want to split, and select it.");
      return;
    }
  n = gr_region_split (&app.song, r, tick);
  if (n < 0)
    return;
  app.sel_region = n;
  editor_show (n);
  app_changed ();
}

/* Keys that work in every Groove window (called from keys.c). */
int arrange_key (int code)
{
  switch (code)
    {
    case ' ': play_stop (); return 1;
    case 'r': case 'R': record (); return 1;
    case 13:                      /* Return: back to the start */
      gr_locate (app.eng, 0);
      app.view.hscroll = 0;
      transport_changed ();
      return 1;
    case 0x18C: case 0x18D:       /* cursor left/right: a bar at a time */
      {
        int b = gr_tick_bar (&app.song, (int) gr_position (app.eng)) + (code == 0x18D ? 1 : -1);
        int t = b < 0 ? -1 : gr_bar_tick (&app.song, b);
        gr_locate (app.eng, t < 0 ? 0 : t);
        transport_changed ();
      }
      return 1;
    case 0x03: arrange_copy (); return 1;          /* Ctrl-C */
    case 0x16: arrange_paste (); return 1;         /* Ctrl-V */
    case 0x14: arrange_split (); return 1;         /* Ctrl-T */
    case 0x7F: case 8:            /* Delete / Backspace: the selected region */
      if (app.sel_region >= 0)
        {
          gr_region_delete (&app.song, app.sel_region);
          app.sel_region = -1;
          editor_show (-1);
          app_changed ();
          return 1;
        }
      return 0;
    }
  return 0;
}

/* ---- clicks and drags in the arranger --------------------------------------- */

static int drag_region, drag_grab, drag_start_y;
static int last_click_time, last_click_region = -2;

/* Snapping follows the song's bars (and their beats when zoomed in),
   whatever the time signature. */
static int zoomed_in (void) { return app.view.bar_w >= 640; }

static int beat_snap (int tick)
{
  int b, t0, bl;
  if (tick < 0)
    tick = 0;
  b = gr_tick_bar (&app.song, tick);
  t0 = gr_bar_tick (&app.song, b);
  bl = gr_beat_len (&app.song, b);
  return t0 + (tick - t0 + bl / 2) / bl * bl;
}

static int snap (int tick)
{
  if (tick < 0)
    tick = 0;
  return zoomed_in () ? beat_snap (tick) : gr_bar_snap (&app.song, tick);
}

static int snap_down (int tick)          /* the bar (or beat) a tick is in */
{
  int b = gr_tick_bar (&app.song, tick < 0 ? 0 : tick), t0 = gr_bar_tick (&app.song, b);
  if (!zoomed_in ())
    return t0;
  return t0 + (tick - t0) / gr_beat_len (&app.song, b) * gr_beat_len (&app.song, b);
}

static int grid_at (int tick)            /* one bar (or beat) there */
{
  int b = gr_tick_bar (&app.song, tick < 0 ? 0 : tick);
  return zoomed_in () ? gr_beat_len (&app.song, b) : gr_bar_len (&app.song, b);
}

static void to_work (int x, int y, int *wx, int *wy)
{
  int ox, oy;
  app_window_origin (app.w_main, &ox, &oy);
  *wx = x - ox - scroll_px;     /* x from the visible left edge */
  *wy = y - oy;
}

static void drag_move (int x, int y, int done)
{
  gr_region *r = &app.song.regions[drag_region];
  int wx, wy, start, track;
  (void) done;
  to_work (x, y, &wx, &wy);
  start = snap (lay_x_to_tick (&app.view, wx) - drag_grab);
  track = (LANES_TOP - wy) / TRACK_H;
  if (wy > LANES_TOP || track < 0 || track >= app.song.ntracks
      || app.song.tracks[track].type != app.song.tracks[r->track].type)
    track = r->track;           /* only onto a track of the same kind */
  if (start != r->start || track != r->track)
    {
      r->start = start;
      r->track = track;
      app.sel_track = track;
      gr_song_cover (&app.song, r->start + r->len);
      app_changed ();
    }
}

static void drag_resize (int x, int y, int done)
{
  gr_region *r = &app.song.regions[drag_region];
  int wx, wy, len;
  (void) done;
  to_work (x, y, &wx, &wy);
  len = snap (lay_x_to_tick (&app.view, wx)) - r->start;
  if (len < 1)
    len = grid_at (r->start);
  /* Audio regions can't be longer than their recording. */
  if (r->clip >= 0)
    {
      double spt = GR_RATE * 60.0 / ((double) app.song.tempo * GR_PPQ);
      int max = (int) ((app.song.clips[r->clip].frames - r->clip_offset) / spt) + 1;
      if (len > max) len = max;
    }
  if (len != r->len)
    {
      r->len = len;
      gr_song_cover (&app.song, r->start + len);
      app_changed ();
    }
}

static void drag_locate (int x, int y, int done)
{
  int wx, wy;
  (void) done;
  to_work (x, y, &wx, &wy);
  gr_locate (app.eng, beat_snap (lay_x_to_tick (&app.view, wx)));
  transport_changed ();
}

static void drag_cycle (int x, int y, int done)
{
  int wx, wy, t;
  (void) done;
  to_work (x, y, &wx, &wy);
  t = gr_bar_snap (&app.song, lay_x_to_tick (&app.view, wx) < 0 ? 0 : lay_x_to_tick (&app.view, wx));
  if (t > drag_grab)
    {
      app.song.loop_start = drag_grab;
      app.song.loop_end = t;
    }
  else if (t < drag_grab)
    {
      app.song.loop_start = t;
      app.song.loop_end = drag_grab;
    }
  app.song.loop_on = 1;
  transport_changed ();
}

/* Dragging a track by its name: it moves up or down the list. */
static void drag_track (int x, int y, int done)
{
  int wx, wy, t;
  (void) done;
  to_work (x, y, &wx, &wy);
  t = (LANES_TOP - wy) / TRACK_H;
  if (wy > LANES_TOP) t = 0;
  if (t >= app.song.ntracks) t = app.song.ntracks - 1;
  if (t != drag_region && t >= 0)
    {
      if (gr_playing (app.eng))
        gr_locate (app.eng, (int) gr_position (app.eng));   /* notes off: tracks change channel */
      gr_track_move (&app.song, drag_region, t);
      drag_region = t;
      app.sel_track = t;
      app_changed ();
    }
}

static void drag_slider (int x, int y, int done)
{
  int wx, wy;
  lay_box b = lay_header_box (drag_start_y);
  gr_track *tr = &app.song.tracks[drag_region];
  float f;
  (void) done;
  to_work (x, y, &wx, &wy);
  f = (float) (wx - b.x0) / (float) (b.x1 - b.x0);
  if (f < 0) f = 0;
  if (f > 1) f = 1;
  if (drag_start_y == HIT_VOLUME)
    tr->vol = f;
  else
    tr->pan = f * 2.0f - 1.0f;
  if (tr->pan > -0.06f && tr->pan < 0.06f)
    tr->pan = 0.0f;             /* the centre is sticky */
  app.unsaved = 1;
  redraw_main (0, lay_track_top (drag_region) - TRACK_H, HDR_W, lay_track_top (drag_region));
}

void arrange_click (int *b)
{
  int wx, wy, now;
  lay_hit h;
  if (b[2] == BUT_MENU)
    {
      app_main_menu (b[0], b[1]);
      return;
    }
  to_work (b[0], b[1], &wx, &wy);
  h = lay_arranger_hit (&app.view, &app.song, wx, wy);
  _swix (OS_ReadMonotonicTime, _OUT (0), &now);

  if (h.track >= 0 && h.track != app.sel_track)
    {
      app.sel_track = h.track;
      keys_release_all ();
      redraw_main (0, LANES_TOP - GR_MAX_TRACKS * TRACK_H, HDR_W, LANES_TOP);
    }
  switch (h.what)
    {
    case HIT_RULER:
      if (b[2] == BUT_ADJUST)
        {
          drag_grab = gr_bar_snap (&app.song, h.tick < 0 ? 0 : h.tick);
          app_start_drag (app.w_main, drag_cycle);
        }
      else
        {
          drag_locate (b[0], b[1], 0);
          app_start_drag (app.w_main, drag_locate);
        }
      break;
    case HIT_NAME:
      if (b[2] == BUT_SELECT && !gr_recording (app.eng))
        {
          drag_region = h.track;
          app_start_drag (app.w_main, drag_track);
        }
      break;
    case HIT_MUTE: case HIT_SOLO: case HIT_ARM:
      {
        gr_track *tr = &app.song.tracks[h.track];
        int *f = h.what == HIT_MUTE ? &tr->mute : h.what == HIT_SOLO ? &tr->solo : &tr->armed;
        *f = !*f;
        app.unsaved = 1;
        redraw_main (0, lay_track_top (h.track) - TRACK_H, HDR_W, lay_track_top (h.track));
      }
      break;
    case HIT_VOLUME: case HIT_PAN:
      drag_region = h.track;
      drag_start_y = h.what;
      drag_slider (b[0], b[1], 0);
      app_start_drag (app.w_main, drag_slider);
      break;
    case HIT_REGION:
      if (h.region == last_click_region && now - last_click_time < 40)
        {
          editor_show (h.region);         /* double-click: edit it */
          editor_open ();
          last_click_region = -2;
          break;
        }
      last_click_region = h.region;
      last_click_time = now;
      app.sel_region = h.region;
      editor_show (h.region);
      app_redraw_all (app.w_main);
      if (b[2] == BUT_SELECT)
        {
          drag_region = h.region;
          drag_grab = h.tick - app.song.regions[h.region].start;
          app_start_drag (app.w_main, drag_move);
        }
      break;
    case HIT_REGION_END:
      app.sel_region = h.region;
      editor_show (h.region);
      drag_region = h.region;
      app_start_drag (app.w_main, drag_resize);
      app_redraw_all (app.w_main);
      break;
    case HIT_EMPTY_LANE:
      if (last_click_region == -1 && now - last_click_time < 40
          && app.song.tracks[h.track].type != TRK_AUDIO)
        {
          /* Double-click on an empty lane: a new empty bar to write in. */
          int bar = gr_tick_bar (&app.song, h.tick), start = gr_bar_tick (&app.song, bar);
          int p = gr_pattern_new (&app.song, "New region", gr_bar_len (&app.song, bar));
          int r = p >= 0 ? gr_region_add (&app.song, h.track, start, gr_bar_len (&app.song, bar), p) : -1;
          if (r >= 0)
            {
              app.sel_region = r;
              editor_show (r);
              editor_open ();
              app_changed ();
            }
          last_click_region = -2;
          break;
        }
      last_click_region = -1;
      last_click_time = now;
      if (app.sel_region >= 0)
        {
          app.sel_region = -1;
          app_redraw_all (app.w_main);
        }
      break;
    case HIT_ADD_TRACK:
      if (wx < HDR_W)
        {
          gr_track_type t = wx < 120 ? TRK_DRUMS : wx < 240 ? TRK_INST : TRK_AUDIO;
          int n = gr_track_add (&app.song, t, t == TRK_DRUMS ? "Drums" : t == TRK_INST
                                ? gr_patch_name (PATCH_EPIANO) : "Audio",
                                t == TRK_DRUMS ? KIT_STUDIO : PATCH_EPIANO);
          if (n >= 0)
            {
              app.sel_track = n;
              arrange_set_extent ();
              app_changed ();
            }
        }
      break;
    }
}

void arrange_scroll (int *b)
{
  int shift = 0;
  /* Shift held (OS_Byte 121, internal key 0 EOR &80)? Then the wheel moves
     along the timeline instead of up and down. */
  _swix (OS_Byte, _INR (0, 1) | _OUT (1), 121, 0x80, &shift);
  if (b[8] || shift)
    {
      int d = b[8] ? b[8] : -b[9];
      app.view.hscroll += d * GR_BAR * (d == 2 || d == -2 ? 4 : 1);
      if (app.view.hscroll < 0) app.view.hscroll = 0;
      arrange_sync_hscroll ();
      return;
    }
  {
    int h = b[4] - b[2] - TOOL_H;   /* the visible height below the toolbar */
    b[6] += b[9] == 2 || b[9] == -2 ? (b[9] > 0 ? h : -h) : b[9] * TRACK_H / 2;
    arrange_open_request (b);
  }
}

/* ---- dropping loops and files ------------------------------------------------ */

void arrange_drop_loop (int loop, int x, int y)
{
  int wx, wy, r;
  lay_hit h;
  to_work (x, y, &wx, &wy);
  h = lay_arranger_hit (&app.view, &app.song, wx > HDR_W ? wx : HDR_W, wy);
  r = gr_loop_place (&app.song, loop, h.track, h.tick < 0 ? 0 : h.tick);
  if (r < 0)
    {
      app_error ("There's no room for another track or region in this song.");
      return;
    }
  app.sel_region = r;
  app.sel_track = app.song.regions[r].track;
  arrange_set_extent ();
  editor_show (r);
  app_changed ();
}

void arrange_drop_file (const char *path, int type, int x, int y)
{
  int wx, wy, r;
  lay_hit h;
  to_work (x, y, &wx, &wy);
  h = lay_arranger_hit (&app.view, &app.song, wx > HDR_W ? wx : HDR_W, wy);
  if (type == FILETYPE_MIDI || type == FILETYPE_MAESTRO)
    {
      arrange_import (path, type, gr_bar_snap (&app.song, h.tick < 0 ? 0 : h.tick));
      return;
    }
  r = gr_wav_import (&app.song, path, h.track, h.tick < 0 ? 0 : h.tick);
  if (r < 0)
    {
      app_error ("Groove couldn't import that sound: it reads 8, 16 and 24-bit PCM WAV files.");
      return;
    }
  app.sel_region = r;
  app.sel_track = app.song.regions[r].track;
  arrange_set_extent ();
  app_changed ();
}

/* The computer's voices, by number, as Maestro reads them
   (Sound_InstallVoice): Maestro files name voices by number. */
#ifndef Sound_InstallVoice
#define Sound_InstallVoice 0x40183
#endif
static int read_voices (const char *names[], int max)
{
  int n = 0, slots = 0, i;
  if (_swix (Sound_InstallVoice, _INR (0, 1) | _OUT (1), 0, 0, &slots))
    return 0;
  for (i = 1; i < slots && n < max; i++)
    {
      const char *name = NULL;
      if (_swix (Sound_InstallVoice, _INR (0, 1) | _OUT (3), 2, i, &name) || !name)
        break;
      names[n++] = name;
    }
  return n;
}

/* A MIDI or Maestro file: new tracks from `tick` (a bar). */
void arrange_import (const char *path, int type, int tick)
{
  const char *why, *voices[32];
  int first = app.song.ntracks, n;
  _swix (Hourglass_On, 0);
  if (type == FILETYPE_MAESTRO)
    {
      int nv = read_voices (voices, 32);
      n = gr_maestro_import (&app.song, path, tick, nv ? voices : NULL, nv, &why);
    }
  else
    n = gr_midi_import (&app.song, path, tick, &why);
  _swix (Hourglass_Off, 0);
  if (n < 0)
    {
      app_error (why ? why : type == FILETYPE_MAESTRO ? "Groove couldn't import that Maestro file."
                 : "Groove couldn't import that MIDI file.");
      return;
    }
  app.sel_track = first;
  app.sel_region = -1;
  arrange_set_extent ();
  app_changed ();
  if (why)
    app_error (why);
  if (gr_song_uses_gm (&app.song))
    app_gm_ensure ();
}

/* ---- on null events ------------------------------------------------------------ */

void arrange_tick (void)
{
  static int last_x = -1, last_lcd = -1, metering;
  int x, t, lcd;
  if (!app.main_open)
    return;
  x = lay_tick_to_x (&app.view, (int) heard_position ());
  if (gr_playing (app.eng) && app.follow
      && (x > app.view.width - 64 || x < HDR_W))
    {
      /* Page along, keeping a bar of what has just played in view. */
      app.view.hscroll = ((int) heard_position () / GR_BAR - 1) * GR_BAR;
      if (app.view.hscroll < 0) app.view.hscroll = 0;
      arrange_sync_hscroll ();
      last_x = -1;
      return;
    }
  if (x != last_x)
    {
      int top = -TOOL_H, bot = lay_track_top (app.song.ntracks);
      if (last_x >= HDR_W)
        redraw_main (last_x - 14, bot, last_x + 14, top);
      if (x >= HDR_W)
        redraw_main (x - 14, bot, x + 14, top);
      last_x = x;
    }
  lcd = (int) heard_position () / GR_PPQ + 100000 * gr_counting_in (app.eng);
  if (lcd != last_lcd)
    {
      last_lcd = lcd;
      redraw_tool_button (TB_LCD);
      if (!gr_playing (app.eng))
        {
          redraw_tool_button (TB_PLAY);
          redraw_tool_button (TB_RECORD);
        }
    }
  /* Meters: redraw while any is moving, and once more when they fall. */
  for (t = 0, x = 0; t < app.song.ntracks; t++)
    x |= app.song.tracks[t].meter > 0.01f;
  if (x || metering)
    for (t = 0; t < app.song.ntracks; t++)
      redraw_main (0, lay_track_top (t) - TRACK_H, HDR_W, lay_track_top (t) - TRACK_H + 12);
  metering = x;
}

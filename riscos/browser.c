/* Groove - the loop browser. Click a loop to hear it (in the song's key and
   tempo); click again, or another, to stop or switch. Drag it into the song
   window to use it: onto a track of the same kind, or onto the empty space
   below the tracks to make a new track. */
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <swis.h>
#include "app.h"
#include "gfx.h"

static gr_pattern preview;      /* the loop being previewed: ours, not the song's */
static int previewing = -1, pressed = -1, press_x, press_y, wimp_dragging;

int browser_create (void)
{
  int b[24];
  memset (b, 0, sizeof b);
  b[0] = 1700; b[1] = 200; b[2] = 1700 + LB_W; b[3] = 1100;
  b[6] = -1;
  b[7] = (int) 0xBF000002;      /* movable, back, close, title, toggle size, vertical scroll, size */
  ((unsigned char *) b)[32] = 7;  ((unsigned char *) b)[33] = 2;
  ((unsigned char *) b)[34] = 7;  ((unsigned char *) b)[35] = 255;
  ((unsigned char *) b)[36] = 3;  ((unsigned char *) b)[37] = 1;
  ((unsigned char *) b)[38] = 12;
  b[10] = 0; b[11] = -lay_browser_height (); b[12] = LB_W; b[13] = 0;
  b[14] = 0x3D;
  b[15] = 3 << 12;
  b[16] = 1;
  ((short *) b)[34] = LB_W;  ((short *) b)[35] = 300;
  strcpy ((char *) &b[18], "Loops");
  b[21] = 0;
  return (app_create_error = _swix (Wimp_CreateWindow, _IN (1) | _OUT (0), b, &app.w_loops)) != NULL;
}

void browser_open (void)
{
  app_open_window (app.w_loops, WS_LOOPS);
  app.loops_open = 1;
  app_redraw_all (app.w_tool);
}

static unsigned cat_colour (const gr_loop_info *li, int dark)
{
  return gfx_track_colour (li->type, li->patch, -1, dark);   /* as the track it would make */
}

static void paint (int ox, int oy, const int *clip)
{
  int i;
  const char *cat = NULL;
  char buf[16];
  gfx_colour (C_BG);
  gfx_rect (clip[0], clip[1], clip[2], clip[3]);
  for (i = 0; i < gr_loop_count (); i++)
    {
      const gr_loop_info *li = gr_loop_get (i);
      int top = oy + lay_browser_row_y (i), bot = top - LB_ROW_H;
      if (!cat || strcmp (cat, li->category))
        {
          cat = li->category;
          if (gfx_visible (clip, ox, top, ox + LB_W, top + LB_HEAD_H))
            gfx_text (ox + 16, top + 12, cat, C_TEXT_DIM, C_BG, 0);
        }
      if (!gfx_visible (clip, ox, bot, ox + LB_W, top))
        continue;
      gfx_colour (i == previewing ? C_HEADER_SEL : (i & 1) ? C_LANE_ALT : C_LANE);
      gfx_round (ox + 8, bot + 3, ox + LB_W - 8, top - 3, 8);
      /* Play / stop symbol in the loop's colour. */
      gfx_colour (cat_colour (li, 0));
      gfx_circle (ox + 36, bot + LB_ROW_H / 2, 15);
      gfx_colour (0x1E1F22);
      if (i == previewing)
        gfx_rect (ox + 30, bot + LB_ROW_H / 2 - 6, ox + 42, bot + LB_ROW_H / 2 + 6);
      else
        gfx_tri (ox + 31, bot + LB_ROW_H / 2 - 8, ox + 31, bot + LB_ROW_H / 2 + 8, ox + 44, bot + LB_ROW_H / 2);
      gfx_text (ox + 64, bot + 16, li->name, C_TEXT, i == previewing ? C_HEADER_SEL : (i & 1) ? C_LANE_ALT : C_LANE, 0);
      snprintf (buf, sizeof buf, "%d bar%s", li->bars, li->bars == 1 ? "" : "s");
      gfx_text (ox + LB_W - 24, bot + 16, buf, C_TEXT_DIM, i == previewing ? C_HEADER_SEL : (i & 1) ? C_LANE_ALT : C_LANE, 2);
    }
}

void browser_redraw (int *b)
{
  int more;
  _swix (Wimp_RedrawWindow, _IN (1) | _OUT (0), b, &more);
  while (more)
    {
      paint (b[1] - b[5], b[4] - b[6], &b[7]);
      _swix (Wimp_GetRectangle, _IN (1) | _OUT (0), b, &more);
    }
}

void browser_stop_preview (void)
{
  if (previewing >= 0)
    {
      gr_preview_stop (app.eng);
      previewing = -1;
      if (app.loops_open)
        app_redraw_all (app.w_loops);
    }
}

static void toggle_preview (int i)
{
  const gr_loop_info *li = gr_loop_get (i);
  if (i == previewing)
    {
      browser_stop_preview ();
      return;
    }
  gr_preview_stop (app.eng);
  gr_loop_fill (i, app.song.key, &preview);
  /* Hear it with the song if the song is playing; on its own otherwise. */
  gr_preview_pattern (app.eng, li->type, li->patch, &preview);
  previewing = i;
  app_redraw_all (app.w_loops);
}

/* Between the click and either a release (a click: preview) or a move
   (a drag: hand over to a Wimp drag box, which can leave the window). */
static void watch_press (int x, int y, int done)
{
  if (done)
    {
      toggle_preview (pressed);
      pressed = -1;
      return;
    }
  if ((x - press_x) * (x - press_x) + (y - press_y) * (y - press_y) > 16 * 16)
    {
      int ox, oy, top, d[10];
      app_window_origin (app.w_loops, &ox, &oy);
      top = oy + lay_browser_row_y (pressed);
      d[0] = app.w_loops;
      d[1] = 5;                 /* drag a fixed-size box */
      d[2] = ox + 8; d[3] = top - LB_ROW_H + 3; d[4] = ox + LB_W - 8; d[5] = top - 3;
      d[6] = -0x8000; d[7] = -0x8000; d[8] = 0x7FFF; d[9] = 0x7FFF;   /* anywhere */
      if (!_swix (Wimp_DragBox, _IN (1), d))
        wimp_dragging = 1;
      app.drag = NULL;          /* the Wimp follows it from here */
    }
}

void browser_click (int *b)
{
  int ox, oy, i;
  if (b[2] == BUT_MENU)
    {
      app_main_menu (b[0], b[1]);
      return;
    }
  app_window_origin (app.w_loops, &ox, &oy);
  i = lay_browser_hit (b[1] - oy);
  if (i < 0)
    return;
  pressed = i;
  press_x = b[0];
  press_y = b[1];
  app_start_drag (app.w_loops, watch_press);
}

void browser_drag_done (void)
{
  int x, y, w;
  if (!wimp_dragging)
    return;
  wimp_dragging = 0;
  app_pointer (&x, &y, NULL, &w, NULL);
  if (pressed >= 0 && w == app.w_main)
    {
      arrange_drop_loop (pressed, x, y);
      browser_stop_preview ();
    }
  else if (pressed >= 0 && w == app.w_tool)
    app_error ("Drop loops onto the tracks, below the toolbar.");
  pressed = -1;
}

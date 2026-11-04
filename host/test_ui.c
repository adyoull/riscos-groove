/* Groove's Wimp front end on Linux, against the fake RISC OS (fake_ro.c).

   The script below plays a session the way a person would: open the app,
   drag loops into the song, preview a loop, play, play notes from the
   keyboard, edit a drum pattern and a piano roll, change tempo and key,
   record, use the menus, save by typing a path and by dragging to a "Filer
   window", export a WAV, load the song and a WAV back, scroll, and quit
   with unsaved changes. Every window that changes is redrawn (by the real
   drawing code) before the next event, as the Wimp would. It runs under
   AddressSanitizer; the CHECKs test the state after each step. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "stubs/swis.h"
#include "fake_ro.h"
#include "../riscos/app.h"
#include "../riscos/gfx.h"

int groove_main (void);

/* As if the song had just been saved: no unsaved changes. */
static void mark_saved (void)
{
  gr_history_reset (app.hist, &app.song);
  app.unsaved = 0;
}

static int midi_before, undo_regions, undo_region, undo_start;
static int ed_region, ed_tick0, ed_pitch0, ed_regions, ed_end, ren_regions;
static char ren_name0, ren_names[3][GR_NAME];
static gr_pattern ed_before;

/* After the paste, the notes start at tick 0 (the paste) plus one step (the drag). */
static int is_shifted (const gr_pattern *p, int k)
{
  int first = ed_before.notes[0].tick;
  return p->notes[k].tick == ed_before.notes[k].tick - first + GR_STEP
         && p->notes[k].pitch == ed_before.notes[k].pitch + 1;
}
static int failures, checks;
#define CHECK(c, what) do { checks++; if (!(c)) { failures++; printf ("FAIL step %d: %s\n", step, what); } } while (0)

static int step, wait_nulls, region_drums, region_keys, saved_tracks;

/* ---- event helpers ---------------------------------------------------------- */

static void point (int win, int wx, int wy, int buttons)
{
  fake_to_screen (win, wx, wy, &fake.px, &fake.py);
  fake.buttons = buttons;
}

static void click (int *b, int *reason, int win, int wx, int wy, int buttons)
{
  point (win, wx, wy, buttons);
  b[0] = fake.px; b[1] = fake.py; b[2] = buttons; b[3] = win; b[4] = -1;
  *reason = 6;
}

static void release (int *reason)        /* let go: a null event follows */
{
  fake.buttons = 0;
  *reason = 0;
}

static void key (int *b, int *reason, int win, int code)
{
  b[0] = win; b[1] = -1; b[6] = code;
  *reason = 8;
}

static void tool_click (int *b, int *reason, int button, int buttons)
{
  lay_box bx = lay_toolbar_box (button);
  click (b, reason, app.w_tool, (bx.x0 + bx.x1) / 2, (bx.y0 + bx.y1) / 2, buttons);
}

static int loop_index (const char *name)
{
  int i;
  for (i = 0; i < gr_loop_count (); i++)
    if (!strcmp (gr_loop_get (i)->name, name))
      return i;
  return -1;
}

static int region_named (int track, const char *name)
{
  int i;
  for (i = 0; i < GR_MAX_REGIONS; i++)
    if (app.song.regions[i].used && app.song.regions[i].track == track
        && app.song.regions[i].pattern >= 0
        && !strcmp (app.song.patterns[app.song.regions[i].pattern].name, name))
      return i;
  return -1;
}

static int count_regions (void)
{
  int i, n = 0;
  for (i = 0; i < GR_MAX_REGIONS; i++)
    n += app.song.regions[i].used;
  return n;
}

static void message (int *b, int *reason, int action, int window, const char *path, int type)
{
  memset (b, 0, 64 * sizeof (int));
  b[1] = 0x6000;                  /* another task */
  b[2] = 4242;
  b[4] = action;
  b[5] = window;
  b[10] = type;
  strcpy ((char *) &b[11], path);
  b[0] = (44 + (int) strlen (path) + 1 + 3) & ~3;
  *reason = 17;
}

static char *save_path_buffer (void)
{
  return (char *) fake_win (app.w_save)->icons[1][5];   /* the writable icon's text */
}

static int exists (const char *p)
{
  return access (p, R_OK) == 0;
}


/* ---- screenshots ------------------------------------------------------------------ */

/* Redraw every open window, bottom to top, logging the drawing commands
   (fake_ro.c) for host/shots.py to turn into a PNG. */
static void snapshot (const char *name)
{
  char path[128];
  int order, i, b[64];
  FILE *f;
  snprintf (path, sizeof path, "/tmp/groove_shots/%s.log", name);
  f = fopen (path, "w");
  if (!f)
    return;
  fake.log = f;
  for (order = 1; order <= fake.order; order++)
    for (i = 0; i < FAKE_WINDOWS; i++)
      {
        fake_window *w = &fake.w[i];
        int h = 0x1000 + i * 0x100;
        if (!w->used || !w->open || w->order != order)
          continue;
        fprintf (f, "W %d %d %d %d %d %u %s\n", w->vis[0], w->vis[1], w->vis[2], w->vis[3],
                 h == app.w_tool, (unsigned) w->flags, w->title ? w->title : "");
        memset (b, 0, sizeof b);
        b[0] = h;
        if (h == app.w_main) arrange_redraw (b);
        else if (h == app.w_tool) arrange_tool_redraw (b);
        else if (h == app.w_edit) editor_redraw (b);
        else if (h == app.w_loops) browser_redraw (b);
        else if (h == app.w_keys) keys_redraw (b);
        fprintf (f, "E\n");
      }
  fake.log = NULL;
  fclose (f);
}

static void place (int h, int x0, int y0, int x1, int y1, int scroll_y)
{
  int b[10];
  b[0] = h; b[1] = x0; b[2] = y0; b[3] = x1; b[4] = y1; b[5] = 0; b[6] = scroll_y; b[7] = -1;
  if (h == app.w_main) arrange_open_request (b);
  else if (h == app.w_edit) editor_open_request (b);
  else _swix (Wimp_OpenWindow, _IN (1), b);
}

/* A song worth looking at: drums, bass, chords, a hook and a pad. */
static void showcase_song (void)
{
  gr_song *s = &app.song;
  int r;
  gr_stop (app.eng); gr_stop (app.eng);
  gr_song_free (s);
  gr_song_init (s);
  gr_engine_set_song (app.eng, s);
  s->tempo = 96;
  r = gr_loop_place (s, loop_index ("Boom Bap"), -1, 0);            s->regions[r].len = 7 * GR_BAR;
  gr_loop_place (s, loop_index ("Tom Fill"), 0, 7 * GR_BAR);
  r = gr_loop_place (s, loop_index ("Boom Bap"), 0, 8 * GR_BAR);    s->regions[r].len = 8 * GR_BAR;
  r = gr_loop_place (s, loop_index ("Root Pump"), -1, 0);           s->regions[r].len = 16 * GR_BAR;
  r = gr_loop_place (s, loop_index ("Pop Chords"), -1, 0);          s->regions[r].len = 16 * GR_BAR;
  region_keys = r;
  r = gr_loop_place (s, loop_index ("Hook Melody"), -1, 4 * GR_BAR); s->regions[r].len = 4 * GR_BAR;
  r = gr_loop_place (s, loop_index ("Hook Melody"), 3, 12 * GR_BAR); s->regions[r].len = 4 * GR_BAR;
  r = gr_loop_place (s, loop_index ("Minor Pads"), -1, 8 * GR_BAR); s->regions[r].len = 8 * GR_BAR;
  s->tracks[3].mute = 0;
  s->tracks[2].solo = 0;
  s->tracks[1].armed = 0;
  s->bars = 18;
  s->loop_on = 1; s->loop_start = 4 * GR_BAR; s->loop_end = 8 * GR_BAR;
  region_drums = gr_region_at (s, 0, 0);
  snprintf (app.path, sizeof app.path, "SDFS::RISCOSpi.$.Music.Groove.Sunday");
  app.unsaved = 1;
  app.view.hscroll = 0;
  app.view.bar_w = 160;
  app_set_extent (app.w_main, 0x4000, lay_work_height (s->ntracks));
  arrange_title ();
}


/* ---- the workspace ------------------------------------------------------------- */

static int overlap (lay_box a, lay_box b)
{
  return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}

/* The outline of a window's visible area with its title bar and scroll bar. */
static lay_box outline (lay_box v)
{
  lay_box o = { v.x0, v.y0, v.x1 + WS_SCROLL, v.y1 + WS_TITLE };
  return o;
}

static void test_workspace (void)
{
  static const int sizes[][2] = { { 1280, 1024 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 } };
  int i, w;
  for (i = 0; i < 4; i++)
    {
      int sw = sizes[i][0] * 2, sh = sizes[i][1] * 2, ok = 1;
      lay_box s = lay_workspace (sw, sh, WS_SONG), e = lay_workspace (sw, sh, WS_EDITOR);
      lay_box l = lay_workspace (sw, sh, WS_LOOPS);
      long long used;
      char what[96];
      for (w = WS_SONG; w <= WS_KEYS; w++)
        {
          lay_box o = outline (lay_workspace (sw, sh, w));
          if (o.x0 < 0 || o.x1 > sw || o.y0 < WS_ICONBAR || o.y1 > sh || o.x1 <= o.x0 || o.y1 <= o.y0)
            ok = 0;
        }
      snprintf (what, sizeof what, "%dx%d: every window on screen, clear of the icon bar", sizes[i][0], sizes[i][1]);
      CHECK (ok, what);
      snprintf (what, sizeof what, "%dx%d: song, editor and loops don't overlap", sizes[i][0], sizes[i][1]);
      CHECK (!overlap (outline (s), outline (e)) && (sizes[i][0] < 1600 || (!overlap (outline (s), outline (l))
             && !overlap (outline (e), outline (l)))), what);
      used = (long long) (outline (s).x1 - outline (s).x0) * (outline (s).y1 - outline (s).y0)
           + (long long) (outline (e).x1 - outline (e).x0) * (outline (e).y1 - outline (e).y0)
           + (long long) (outline (l).x1 - outline (l).x0) * (outline (l).y1 - outline (l).y0);
      snprintf (what, sizeof what, "%dx%d: the windows use most of the screen (%lld%%)", sizes[i][0], sizes[i][1],
                used * 100 / ((long long) sw * (sh - WS_ICONBAR)));
      CHECK (used * 100 / ((long long) sw * (sh - WS_ICONBAR)) >= 80, what);
      printf ("  %s\n", what);
    }
}

/* ---- the script ---------------------------------------------------------------- */

int fake_script (int *b, int *reason)
{
  if (wait_nulls > 0)
    {
      wait_nulls--;
      *reason = 0;
      return 1;
    }
  switch (step++)
    {
    case 0:                       /* Select on the icon bar icon opens the song */
      CHECK (app.main_open && fake_win (app.w_main)->open && app.loops_open,
             "Groove opens straight into the song window and the loops, without a click");
      CHECK (fake.caret == app.w_main, "with the input focus, so keys work at once");
      b[0] = 1000; b[1] = 20; b[2] = BUT_SELECT; b[3] = -2; b[4] = 0;
      *reason = 6;
      return 1;
    case 1:
      CHECK (app.main_open && fake_win (app.w_main)->open && fake_win (app.w_tool)->open,
             "the song window and its toolbar open");
      CHECK (fake_win (app.w_tool)->vis[3] == fake_win (app.w_main)->vis[3]
             && fake_win (app.w_tool)->vis[1] == fake_win (app.w_main)->vis[3] - TOOL_H,
             "the toolbar pane sits over the top of the song window");
      CHECK (app.song.ntracks == 2, "a new song has a drum and a piano track");
      tool_click (b, reason, TB_LOOPS, BUT_SELECT);
      return 1;
    case 2:
      CHECK (app.loops_open, "the Loops button opens the loop browser");
      click (b, reason, app.w_loops, 100, lay_browser_row_y (loop_index ("Four on the Floor")) - 20, BUT_SELECT);
      return 1;
    case 3:                       /* move with the button down: a drag starts */
      fake.px += 40;
      *reason = 0;
      return 1;
    case 4:
      CHECK (fake.dragging == 1, "dragging a loop starts a Wimp drag box");
      point (app.w_main, HDR_W + 2 * app.view.bar_w + 30, lay_track_top (0) - 50, 0);
      fake.dragging = 0;
      *reason = 7;                /* User_Drag_Box */
      return 1;
    case 5:
      region_drums = region_named (0, "Four on the Floor");
      CHECK (region_drums >= 0, "the drum loop lands on the drum track");
      CHECK (region_drums >= 0 && app.song.regions[region_drums].start == 2 * GR_BAR,
             "at the bar it was dropped on");
      click (b, reason, app.w_loops, 100, lay_browser_row_y (loop_index ("Pop Chords")) - 20, BUT_SELECT);
      return 1;
    case 6:
      fake.px += 40;
      *reason = 0;
      return 1;
    case 7:                       /* drop below the tracks: a new track */
      point (app.w_main, HDR_W + 10, lay_track_top (app.song.ntracks) - 50, 0);
      fake.dragging = 0;
      *reason = 7;
      return 1;
    case 8:
      CHECK (app.song.ntracks == 3 && app.song.tracks[2].type == TRK_INST
             && app.song.tracks[2].patch == PATCH_EPIANO, "dropped below the tracks: a new piano track");
      region_keys = region_named (2, "Pop Chords");
      CHECK (region_keys >= 0 && app.song.regions[region_keys].start == 0, "with the chords at bar 1");
      click (b, reason, app.w_loops, 100, lay_browser_row_y (1) - 20, BUT_SELECT);
      return 1;
    case 9:
      release (reason);
      return 1;
    case 10:
      CHECK (gr_previewing (app.eng), "clicking a loop previews it");
      wait_nulls = 5;
      *reason = 0;
      return 1;
    case 11:
      click (b, reason, app.w_loops, 100, lay_browser_row_y (1) - 20, BUT_SELECT);
      return 1;
    case 12:
      release (reason);
      return 1;
    case 13:
      CHECK (!gr_previewing (app.eng), "clicking it again stops the preview");
      tool_click (b, reason, TB_PLAY, BUT_SELECT);
      return 1;
    case 14:
      CHECK (gr_playing (app.eng), "Play plays");
      if (fake.ring)
        ((unsigned *) fake.ring)[5] = 0;
      wait_nulls = 60;
      *reason = 0;
      return 1;
    case 15:
      CHECK (gr_position (app.eng) > GR_PPQ, "time moves on while playing");
      CHECK (fake.frames_played > 30000, "the sound ring is being filled and played");
      CHECK (((unsigned *) fake.ring)[5] <= 2, "no underruns at a steady 2 cs poll");
      fake.key_down[(256 - 66) & 0xFF] = 1;   /* A down */
      key (b, reason, app.w_main, 'a');
      return 1;
    case 16:
      CHECK (keys_held (), "A plays a note");
      /* D and G go down too, but RISC OS sends no key-press events past
         two held keys: the keyboard scan has to find them. */
      fake.key_down[(256 - 51) & 0xFF] = fake.key_down[(256 - 84) & 0xFF] = 1;
      wait_nulls = 5;
      *reason = 0;
      return 1;
    case 17:
      CHECK (keys_held (), "it keeps sounding while A is held");
      CHECK (keys_held_count () == 3, "a three-note chord: keys held down with no key-press event play too");
      fake.key_down[(256 - 66) & 0xFF] = fake.key_down[(256 - 51) & 0xFF] = fake.key_down[(256 - 84) & 0xFF] = 0;
      wait_nulls = 2;
      *reason = 0;
      return 1;
    case 18:
      CHECK (!keys_held (), "and stops when A comes up");
      key (b, reason, app.w_main, ' ');
      return 1;
    case 19:
      CHECK (!gr_playing (app.eng), "Space stops");
      click (b, reason, app.w_main, lay_tick_to_x (&app.view, 2 * GR_BAR + GR_PPQ), lay_track_top (0) - 60, BUT_SELECT);
      return 1;
    case 20:
      release (reason);
      return 1;
    case 21:
      CHECK (app.sel_region == region_drums, "clicking a region selects it");
      click (b, reason, app.w_main, lay_tick_to_x (&app.view, 2 * GR_BAR + GR_PPQ), lay_track_top (0) - 60, BUT_SELECT);
      return 1;
    case 22:
      release (reason);
      return 1;
    case 23:
      CHECK (app.edit_open, "double-clicking it opens the editor");
      {
        lay_box sv = lay_workspace (3840, 2160, WS_SONG), ev = lay_workspace (3840, 2160, WS_EDITOR);
        fake_window *ms = fake_win (app.w_main), *ed = fake_win (app.w_edit);
        CHECK (ms->vis[0] == sv.x0 && ms->vis[2] == sv.x1 && ms->vis[3] == sv.y1 && ms->vis[1] == sv.y0,
               "the song window opens at its workspace place, full width");
        /* The drum grid's rows are whole pixels, so it may stop a few
           units short of the bottom. */
        CHECK (ed->vis[0] == ev.x0 && ed->vis[3] == ev.y1 && ed->vis[1] - ev.y0 >= 0 && ed->vis[1] - ev.y0 < 40,
               "the editor opens below it, filling the rest of the height");
        CHECK (app.edit_width == ed->vis[2] - ed->vis[0], "the step grid is laid out for the editor's real width");
      }
      {
        int sw = lay_step_w (app.edit_width, GR_BAR);
        click (b, reason, app.w_edit, ED_LABEL_W + 1 * sw + sw / 2, -ED_HEAD_H - 20, BUT_SELECT);
      }
      return 1;
    case 24:                      /* paint along the row */
      {
        int sw = lay_step_w (app.edit_width, GR_BAR);
        point (app.w_edit, ED_LABEL_W + 3 * sw + sw / 2, -ED_HEAD_H - 20, BUT_SELECT);
      }
      *reason = 0;
      return 1;
    case 25:
      release (reason);
      return 1;
    case 26:
      {
        gr_pattern *p = &app.song.patterns[app.song.regions[region_drums].pattern];
        CHECK (gr_pattern_find_note (p, 1 * GR_STEP, DR_KICK) >= 0
               && gr_pattern_find_note (p, 3 * GR_STEP, DR_KICK) >= 0,
               "clicking and dragging along the kick row adds kicks");
        CHECK (app.unsaved, "and marks the song changed");
      }
      {
        int sw = lay_step_w (app.edit_width, GR_BAR);
        click (b, reason, app.w_edit, ED_LABEL_W + 1 * sw + sw / 2, -ED_HEAD_H - 20, BUT_ADJUST);
      }
      return 1;
    case 27:
      {
        gr_pattern *p = &app.song.patterns[app.song.regions[region_drums].pattern];
        int k = gr_pattern_find_note (p, 1 * GR_STEP, DR_KICK);
        CHECK (k >= 0 && p->notes[k].vel == 127, "Adjust accents a step");
      }
      click (b, reason, app.w_main, lay_tick_to_x (&app.view, GR_PPQ), lay_track_top (2) - 60, BUT_SELECT);
      return 1;
    case 28:
      release (reason);
      return 1;
    case 29:                      /* draw a note in the piano roll and drag it long */
      CHECK (app.sel_region == region_keys, "the chords region is selected");
      {
        gr_pattern *p = &app.song.patterns[app.song.regions[region_keys].pattern];
        click (b, reason, app.w_edit, lay_roll_x (app.edit_width, p->len, 2 * GR_STEP) + 4,
               lay_roll_y (70) - 5, BUT_SELECT);
      }
      return 1;
    case 30:
      {
        gr_pattern *p = &app.song.patterns[app.song.regions[region_keys].pattern];
        point (app.w_edit, lay_roll_x (app.edit_width, p->len, 5 * GR_STEP) + 4, lay_roll_y (70) - 5, BUT_SELECT);
      }
      *reason = 0;
      return 1;
    case 31:
      release (reason);
      return 1;
    case 32:
      {
        gr_pattern *p = &app.song.patterns[app.song.regions[region_keys].pattern];
        int k = gr_pattern_find_note (p, 2 * GR_STEP, 70);
        CHECK (k >= 0 && p->notes[k].len == 4 * GR_STEP, "a note drawn and dragged to four steps long");
        click (b, reason, app.w_edit, lay_roll_x (app.edit_width, p->len, 3 * GR_STEP), lay_roll_y (70) - 5, BUT_ADJUST);
      }
      return 1;
    case 33:
      {
        gr_pattern *p = &app.song.patterns[app.song.regions[region_keys].pattern];
        CHECK (gr_pattern_find_note (p, 2 * GR_STEP, 70) < 0, "Adjust on a note deletes it");
      }
      tool_click (b, reason, TB_TEMPO_UP, BUT_SELECT);
      return 1;
    case 34:
      CHECK (app.song.tempo == 111, "tempo + goes up by 1");
      tool_click (b, reason, TB_TEMPO_UP, BUT_ADJUST);
      return 1;
    case 35:
      CHECK (app.song.tempo == 121, "and by 10 with Adjust");
      tool_click (b, reason, TB_ZOOM_IN, BUT_SELECT);
      return 1;
    case 36:
      CHECK (app.view.bar_w == 320, "zoom in doubles the bar width");
      CHECK (app.view.hscroll <= gr_position (app.eng)
             && lay_tick_to_x (&app.view, (int) gr_position (app.eng)) < app.view.width,
             "zoom keeps the playhead in view");
      tool_click (b, reason, TB_ZOOM_OUT, BUT_SELECT);
      return 1;
    case 37:
      tool_click (b, reason, TB_CYCLE, BUT_SELECT);
      return 1;
    case 38:
      CHECK (app.song.loop_on, "the cycle button turns cycling on");
      tool_click (b, reason, TB_CYCLE, BUT_SELECT);
      return 1;
    case 39:                      /* select the piano track, record a note */
      click (b, reason, app.w_main, 40, lay_track_top (1) - 20, BUT_SELECT);
      return 1;
    case 40:
      release (reason);           /* (let go of the track's name) */
      return 1;
    case 41:
      CHECK (app.sel_track == 1, "clicking a track header selects the track");
      CHECK (app.count_in, "(Count-in is on as standard)");
      app.count_in = 0;           /* these recording tests time the take without it */
      gr_locate (app.eng, 8 * GR_BAR);
      tool_click (b, reason, TB_RECORD, BUT_SELECT);
      return 1;
    case 42:
      CHECK (gr_recording (app.eng) && app.song.tracks[1].armed, "Record arms the selected track and records");
      wait_nulls = 10;
      *reason = 0;
      return 1;
    case 43:
      fake.key_down[(256 - 82) & 0xFF] = 1;   /* S down: D */
      key (b, reason, app.w_main, 's');
      return 1;
    case 44:
      wait_nulls = 8;
      *reason = 0;
      return 1;
    case 45:
      fake.key_down[(256 - 82) & 0xFF] = 0;
      wait_nulls = 4;
      *reason = 0;
      return 1;
    case 46:
      tool_click (b, reason, TB_RECORD, BUT_SELECT);
      return 1;
    case 47:
      {
        int r = gr_region_at (&app.song, 1, 8 * GR_BAR);
        CHECK (!gr_recording (app.eng), "Record again stops");
        CHECK (r >= 0 && app.song.patterns[app.song.regions[r].pattern].nnotes == 1
               && app.song.patterns[app.song.regions[r].pattern].notes[0].pitch == 62,
               "the recorded D is in a new region at bar 9");
      }
      click (b, reason, app.w_main, 600, lay_track_top (0) - 50, BUT_MENU);
      return 1;
    case 48:
      CHECK (fake.menu != -1 && fake.menus > 0, "Menu opens the main menu");
      b[0] = 1; b[1] = 2; b[2] = -1;   /* Track > New audio track */
      *reason = 9;
      return 1;
    case 49:
      CHECK (app.song.ntracks == 4 && app.song.tracks[3].type == TRK_AUDIO, "Track > New audio track");
      app.sel_region = region_keys;
      b[0] = 2; b[1] = 1; b[2] = -1;   /* Region > Duplicate */
      *reason = 9;
      return 1;
    case 50:
      CHECK (count_regions () == 4 && app.song.regions[app.sel_region].start == 4 * GR_BAR,
             "Region > Duplicate puts a copy straight after it");
      b[0] = 0; b[1] = 4; b[2] = 2; b[3] = -1;   /* Song > Key > D */
      *reason = 9;
      return 1;
    case 51:
      CHECK (app.song.key == 2, "Song > Key > D");
      key (b, reason, app.w_main, 0x13);          /* Ctrl-S */
      return 1;
    case 52:
      CHECK (fake.menu == app.w_save, "Ctrl-S opens the save box");
      strcpy (save_path_buffer (), "/tmp/groove_ui.song");
      key (b, reason, app.w_save, 13);
      return 1;
    case 53:
      CHECK (exists ("/tmp/groove_ui.song") && !app.unsaved && fake.filetype_set == FILETYPE_SONG,
             "typing a path and Return saves the song");
      saved_tracks = app.song.ntracks;
      key (b, reason, app.w_main, 0x13);
      return 1;
    case 54:                      /* now drag the file icon to a "Filer window" */
      {
        fake_window *w = fake_win (app.w_save);
        click (b, reason, app.w_save, (w->icons[0][0] + w->icons[0][2]) / 2,
               (w->icons[0][1] + w->icons[0][3]) / 2, BUT_SELECT);
        b[4] = 0;
      }
      return 1;
    case 55:
      fake.px += 40;
      *reason = 0;
      return 1;
    case 56:
      CHECK (fake.dragging, "dragging the file icon starts a drag");
      fake.filer_window = 1;
      fake.px = 3500; fake.py = 500; fake.buttons = 0;
      fake.dragging = 0;
      *reason = 7;
      return 1;
    case 57:
      CHECK (fake.msg[4] == MSG_DATASAVE && fake.msg[10] == FILETYPE_SONG && fake.msg_to == FAKE_FILER
             /* RISC OS leaf: after the last '.', so "song" from this Linux path */
             && strcmp ((char *) &fake.msg[11], "song") == 0,
             "dropping it sends DataSave to the window under the pointer");
      {
        int ref = fake.msg[2];
        message (b, reason, MSG_DATASAVEACK, FAKE_FILER, "/tmp/groove_ui_drag.song", FILETYPE_SONG);
        b[3] = ref;
        b[9] = 100;
      }
      return 1;
    case 58:
      CHECK (exists ("/tmp/groove_ui_drag.song"), "on DataSaveAck the song is saved where the Filer said");
      CHECK (fake.msg[4] == MSG_DATALOAD, "and DataLoad tells the Filer it's there");
      b[0] = 0; b[1] = 2; b[2] = -1;   /* Song > Export as WAV */
      *reason = 9;
      return 1;
    case 59:
      CHECK (fake.menu == app.w_save, "Song > Export opens the save box");
      strcpy (save_path_buffer (), "/tmp/groove_ui.wav");
      key (b, reason, app.w_save, 13);
      return 1;
    case 60:
      CHECK (exists ("/tmp/groove_ui.wav") && fake.filetype_set == FILETYPE_WAVE, "the mix is exported as a WAV");
      app.song.tempo = 90;           /* a change, then load the saved song over it */
      mark_saved ();
      message (b, reason, MSG_DATALOAD, -2, "/tmp/groove_ui.song", FILETYPE_SONG);
      return 1;
    case 61:
      CHECK (app.song.tempo == 121 && app.song.ntracks == saved_tracks && app.song.key == 2,
             "dropping the song on the icon bar loads it");
      CHECK (fake.msg[4] == MSG_DATALOADACK, "and acknowledges the DataLoad");
      {
        int x, y;
        fake_to_screen (app.w_main, HDR_W + 20, lay_track_top (3) - 50, &x, &y);
        message (b, reason, MSG_DATALOAD, app.w_main, "/tmp/groove_ui.wav", FILETYPE_WAVE);
        b[7] = x; b[8] = y;
      }
      return 1;
    case 62:
      {
        int r = gr_region_at (&app.song, 3, 0);
        CHECK (r >= 0 && app.song.regions[r].clip >= 0, "a WAV dropped on the audio track imports there");
      }
      tool_click (b, reason, TB_PLAY, BUT_SELECT);
      return 1;
    case 63:
      wait_nulls = 30;
      *reason = 0;
      return 1;
    case 64:
      tool_click (b, reason, TB_PLAY, BUT_SELECT);
      return 1;
    case 65:                      /* the wheel, then Shift-wheel style sideways */
      {
        fake_window *w = fake_win (app.w_main);
        b[0] = app.w_main;
        memcpy (&b[1], w->vis, 4 * sizeof (int));
        b[5] = w->scroll[0]; b[6] = w->scroll[1]; b[7] = -1;
        b[8] = 1; b[9] = 0;
        *reason = 10;
      }
      return 1;
    case 66:
      CHECK (app.view.hscroll == GR_BAR, "a sideways scroll moves the timeline a bar");
      message (b, reason, MSG_MODECHANGE, 0, "", 0);
      return 1;
    case 67:
      key (b, reason, app.w_main, 0x189);    /* F9: musical typing */
      return 1;
    case 68:
      CHECK (app.keys_open, "F9 opens the keyboard window");
      CHECK (fake_win (app.w_keys)->vis[2] - fake_win (app.w_keys)->vis[0] == WS_KEYS_W
             && fake_win (app.w_keys)->vis[3] - fake_win (app.w_keys)->vis[1] == WS_KEYS_H,
             "the keyboard window opens at its full size");
      click (b, reason, app.w_keys, 16 + 40, -64 - 260, BUT_SELECT);
      return 1;
    case 69:
      CHECK (keys_held (), "clicking a key plays it");
      release (reason);
      return 1;
    case 70:
      CHECK (!keys_held (), "until the button is let go");
      b[0] = app.w_edit;
      *reason = 3;                /* close the editor */
      return 1;
    case 71:
      CHECK (!app.edit_open && !fake_win (app.w_edit)->open, "the editor closes");
      app.unsaved = 1;
      fake.error_answer = 2;      /* "Cancel" in the "quit anyway?" box */
      message (b, reason, MSG_PREQUIT, 0, "", 0);
      return 1;
    case 72:
      CHECK (fake.msg[4] == MSG_PREQUIT && fake.msg[3] == 4242,
             "unsaved changes: PreQuit is acknowledged, so the shutdown stops");
      fake.error_answer = 0;
      wait_nulls = 3;
      *reason = 0;
      return 1;

    case 73:                      /* the editor's header stays at the top when scrolled */
      app.sel_track = 2;
      app.sel_region = region_keys;
      editor_open ();
      editor_show (region_keys);    /* scrolls the piano roll to the chords */
      {
        fake_window *w = fake_win (app.w_edit);
        int scroll = w->scroll[1];
        CHECK (scroll < -ED_HEAD_H, "the piano roll opens scrolled down to the notes");
        /* Click the instrument button where the user sees it: at the top of
           the window, not at the top of the work area (scrolled away). */
        fake.px = w->vis[0] + 400;
        fake.py = w->vis[3] - ED_HEAD_H / 2;
        fake.buttons = BUT_SELECT;
        b[0] = fake.px; b[1] = fake.py; b[2] = BUT_SELECT; b[3] = app.w_edit; b[4] = -1;
        *reason = 6;
      }
      return 1;
    case 74:
      CHECK (fake.menu != -1 && fake.menus > 0,
             "clicking the header's instrument button, scrolled, opens the Sound menu");
      fake.buttons = 0;
      b[0] = PATCH_BASS; b[1] = -1;   /* Synth Bass */
      *reason = 9;
      return 1;
    case 75:                      /* Quit from the icon bar menu with unsaved changes asks first */
      CHECK (app.song.tracks[2].patch == PATCH_BASS && app.song.tracks[2].gm == -1
             && !strcmp (app.song.tracks[2].name, gr_patch_name (PATCH_BASS)),
             "and choosing from it changes the instrument");
      app.unsaved = 1;
      fake.error_answer = 2;      /* Cancel */
      b[0] = 1700; b[1] = 20; b[2] = BUT_MENU; b[3] = -2; b[4] = 0;
      *reason = 6;
      return 1;
    case 76:
      {
        const int *m = (const int *) fake.menu;
        int errors = fake.errors;
        /* PRM / style guide: an icon bar menu's top is 96 + 44 per item +
           24 per dotted line above the bottom of the screen. */
        CHECK (fake.menu_y == 96 + 7 * 44 + 2 * 24, "the icon bar menu sits just above the icon bar");
        CHECK (m[7 + 1] > 0 && fake_win (m[7 + 1])->used
               && !strcmp (fake_win (m[7 + 1])->title, "About this program"),
               "its Info item leads to the standard Info box");
        b[0] = 6;                 /* Quit */
        b[1] = -1;
        *reason = 9;
        fake.errors_before = errors;
      }
      return 1;
    case 77:                      /* ---- the showcase, for the screenshots ---- */
      CHECK (fake.errors == fake.errors_before + 1, "Quit with unsaved changes asks first");
      fake.error_answer = 0;
      showcase_song ();
      b[0] = app.w_keys;          /* the keyboard window from earlier: closed */
      _swix (Wimp_CloseWindow, _IN (1), b);
      app.keys_open = 0;
      app.sel_track = 0;
      app.sel_region = region_drums;
      place (app.w_main, 40, 1000, 2900, 2070, 0);
      editor_show (region_drums);
      app.edit_open = 1;
      place (app.w_edit, 40, 120, 2900, 860, 0);
      browser_open ();
      place (app.w_loops, 2990, 360, 3550, 2070, 0);
      gr_locate (app.eng, 5 * GR_BAR + 2 * GR_PPQ);
      gr_play (app.eng);
      wait_nulls = 50;            /* play on a little: meters, playhead */
      *reason = 0;
      return 1;
    case 78:
      snapshot ("1-song-drums-loops");
      gr_stop (app.eng);
      /* Piano roll on the chords, with the keyboard window and a chord held. */
      app.sel_track = 2;
      app.sel_region = region_keys;
      editor_show (region_keys);
      place (app.w_edit, 40, 120, 2900, 860, 0);
      editor_show (region_keys);
      b[0] = app.w_loops;
      *reason = 3;
      return 1;
    case 79:
      keys_open ();
      place (app.w_keys, 2960, 120, 2960 + 832, 120 + 368, 0);
      fake.key_down[(256 - 66) & 0xFF] = fake.key_down[(256 - 51) & 0xFF] = fake.key_down[(256 - 84) & 0xFF] = 1;
      key (b, reason, app.w_keys, 'a');
      return 1;
    case 80: key (b, reason, app.w_keys, 'd'); return 1;
    case 81: key (b, reason, app.w_keys, 'g'); return 1;
    case 82:
      gr_locate (app.eng, 2 * GR_BAR + 3 * GR_STEP);
      gr_play (app.eng);
      wait_nulls = 8;
      *reason = 0;
      return 1;
    case 83:
      snapshot ("2-piano-roll-keyboard");
      memset (fake.key_down, 0, sizeof fake.key_down);
      gr_stop (app.eng);
      /* Recording on the lead track, zoomed in. */
      app.sel_track = 3;
      app.song.tracks[3].armed = 1;
      app.view.bar_w = 320;
      app.view.hscroll = 0;
      b[0] = app.w_keys;
      *reason = 3;
      return 1;
    case 84:
      b[0] = app.w_edit;
      *reason = 3;
      return 1;
    case 85:
      place (app.w_main, 200, 300, 3400, 1900, 0);
      gr_locate (app.eng, 3 * GR_BAR);
      tool_click (b, reason, TB_RECORD, BUT_SELECT);
      return 1;
    case 86:
      wait_nulls = 40;
      *reason = 0;
      return 1;
    case 87:
      snapshot ("3-recording");
      tool_click (b, reason, TB_RECORD, BUT_SELECT);
      return 1;
    case 88:                      /* ---- SoundFont sounds and MIDI files ---- */
      app.sel_track = 2;
      click (b, reason, app.w_main, 40, lay_track_top (2) - 20, BUT_MENU);
      return 1;
    case 89:
      CHECK (fake.menu != -1, "Menu on a track opens the main menu");
      /* Track > Sound > SoundFont > Piano > Acoustic Grand Piano */
      b[0] = 1; b[1] = 3; b[2] = PATCH_COUNT; b[3] = 0; b[4] = 0; b[5] = -1;
      *reason = 9;
      return 1;
    case 90:
      CHECK (gr_gm_ready (), "choosing a SoundFont sound loads !SoftMIDI's SoundFont");
      CHECK (app.song.tracks[2].gm == 0 && !strcmp (app.song.tracks[2].name, "Acoustic Grand Piano"),
             "and the track plays it, named after it");
      gr_locate (app.eng, 0);
      tool_click (b, reason, TB_PLAY, BUT_SELECT);
      return 1;
    case 91:
      wait_nulls = 30;
      *reason = 0;
      return 1;
    case 92:
      CHECK (gr_playing (app.eng) && fake.frames_played > 0, "the song plays with the SoundFont piano");
      tool_click (b, reason, TB_PLAY, BUT_SELECT);
      return 1;
    case 93:
      click (b, reason, app.w_main, 40, lay_track_top (2) - 20, BUT_MENU);
      return 1;
    case 94:
      b[0] = 0; b[1] = 3; b[2] = -1;   /* Song > Export as MIDI */
      *reason = 9;
      return 1;
    case 95:
      CHECK (fake.menu == app.w_save, "Song > Export as MIDI opens the save box");
      strcpy (save_path_buffer (), "/tmp/groove_ui.mid");
      key (b, reason, app.w_save, 13);
      return 1;
    case 96:
      CHECK (exists ("/tmp/groove_ui.mid") && fake.filetype_set == FILETYPE_MIDI,
             "the song is exported as a MIDI file");
      gr_stop (app.eng);
      arrange_new_song ();
      midi_before = app.song.ntracks;
      message (b, reason, MSG_DATALOAD, -2, "/tmp/groove_ui.mid", FILETYPE_MIDI);
      return 1;
    case 97:
      {
        int t, gm = 0;
        for (t = midi_before; t < app.song.ntracks; t++)
          if (app.song.tracks[t].gm >= 0)
            gm++;
        CHECK (fake.msg[4] == MSG_DATALOADACK && app.song.ntracks > midi_before && gm == app.song.ntracks - midi_before,
               "a MIDI file dropped on the icon bar imports as SoundFont tracks");
        CHECK (count_regions () > 0, "with its regions");
      }
      mark_saved ();
      b[0] = 1700; b[1] = 20; b[2] = BUT_MENU; b[3] = -2; b[4] = 0;   /* the icon bar menu */
      *reason = 6;
      return 1;
    case 98:
      b[0] = 2; b[1] = -1;        /* User guide */
      *reason = 9;
      return 1;
    case 99:
      CHECK (fake.starts == 1 && !strcmp (fake.started, "Filer_Run <Groove$Dir>.Guide.index/html"),
             "User guide on the icon bar menu opens the guide");
      b[0] = 1700; b[1] = 20; b[2] = BUT_MENU; b[3] = -2; b[4] = 0;
      *reason = 6;
      return 1;
    case 100:
      b[0] = 1; b[1] = -1;        /* Help */
      *reason = 9;
      return 1;
    case 101:
      CHECK (!strcmp (fake.started, "Filer_Run <Groove$Dir>.!Help"), "Help... opens !Help");
      /* No web browser: HTML would open in a text editor. */
      setenv ("Alias$@RunType_FAF", "Run <Edit$Dir>.!Run %*0", 1);
      fake.errors_before = fake.errors;
      b[0] = 1700; b[1] = 20; b[2] = BUT_MENU; b[3] = -2; b[4] = 0;
      *reason = 6;
      return 1;
    case 102:
      b[0] = 2; b[1] = -1;        /* User guide */
      *reason = 9;
      return 1;
    case 103:
      CHECK (fake.starts == 2 && fake.errors == fake.errors_before + 1,
             "with no web browser set up, User guide says so instead of opening the page in an editor");
      /* Double-clicking a song with unsaved changes asks first. */
      app.unsaved = 1;
      app.song.tempo = 77;
      fake.error_answer = 2;      /* Cancel */
      fake.errors_before = fake.errors;
      message (b, reason, MSG_DATAOPEN, 0, "/tmp/groove_ui.song", FILETYPE_SONG);
      return 1;
    case 104:
      CHECK (fake.errors == fake.errors_before + 1 && app.song.tempo == 77,
             "double-clicking a song with unsaved changes asks first (Cancel keeps the song)");
      fake.error_answer = 0;
      mark_saved ();
      /* ---- Undo and Redo ---- */
      undo_regions = count_regions ();
      for (undo_region = 0; undo_region < GR_MAX_REGIONS && !app.song.regions[undo_region].used; undo_region++)
        ;
      app.sel_region = undo_region;
      key (b, reason, app.w_main, 0x7F);          /* Delete the region */
      return 1;
    case 105:
      CHECK (count_regions () == undo_regions - 1 && app.unsaved, "(Delete removed the region)");
      key (b, reason, app.w_main, 0x1A);          /* Ctrl-Z */
      return 1;
    case 106:
      CHECK (count_regions () == undo_regions && app.song.regions[undo_region].used,
             "Ctrl-Z brings the deleted region back");
      CHECK (!app.unsaved, "and the song is as saved again (no *)");
      key (b, reason, app.w_main, 0x19);          /* Ctrl-Y */
      return 1;
    case 107:
      CHECK (count_regions () == undo_regions - 1 && app.unsaved, "Ctrl-Y deletes it again");
      /* Dragging a region is one step, however far it goes. */
      {
        int r;
        for (r = 0; r < GR_MAX_REGIONS && !app.song.regions[r].used; r++)
          ;
        undo_region = r;
        undo_start = app.song.regions[r].start;
        app.view.hscroll = 0;
        click (b, reason, app.w_main, lay_tick_to_x (&app.view, undo_start + GR_PPQ),
               lay_track_top (app.song.regions[r].track) - 60, BUT_SELECT);
        b[2] = BUT_SELECT;
      }
      return 1;
    case 108: case 109: case 110:
      fake.px += 60;              /* drag right, a bit at a time */
      *reason = 0;
      return 1;
    case 111:
      release (reason);
      return 1;
    case 112:
      *reason = 0;
      return 1;
    case 113:
      CHECK (app.song.regions[undo_region].start > undo_start, "(the region was dragged along)");
      key (b, reason, app.w_main, 0x1A);
      return 1;
    case 114:
      CHECK (app.song.regions[undo_region].start == undo_start, "one Undo puts a whole drag back");
      key (b, reason, app.w_main, 0x1A);
      return 1;
    case 115:
      CHECK (count_regions () == undo_regions, "and the next Undo goes on back (the deleted region)");
      mark_saved ();
      /* ---- the piano roll: select all, transpose, velocity, copy, delete, paste ---- */
      for (ed_region = 0; ed_region < GR_MAX_REGIONS; ed_region++)
        if (app.song.regions[ed_region].used && app.song.regions[ed_region].clip < 0
            && app.song.tracks[app.song.regions[ed_region].track].type == TRK_INST
            && app.song.patterns[app.song.regions[ed_region].pattern].nnotes > 1)
          break;
      app.sel_region = ed_region;
      app.sel_track = app.song.regions[ed_region].track;
      editor_show (ed_region);
      editor_open ();
      ed_before = app.song.patterns[app.song.regions[ed_region].pattern];
      key (b, reason, app.w_edit, 0x01);          /* Ctrl-A */
      return 1;
    case 116:
      {
        gr_pattern *p = &app.song.patterns[app.song.regions[ed_region].pattern];
        CHECK (editor_selected () == p->nnotes, "Ctrl-A in the piano roll selects every note");
      }
      key (b, reason, app.w_edit, 0x18F);         /* Up */
      return 1;
    case 117:
      {
        gr_pattern *p = &app.song.patterns[app.song.regions[ed_region].pattern];
        int k, ok = p->nnotes == ed_before.nnotes;
        for (k = 0; ok && k < p->nnotes; k++)
          ok = p->notes[k].pitch == ed_before.notes[k].pitch + 1 && p->notes[k].tick == ed_before.notes[k].tick;
        CHECK (ok, "Up moves the selected notes up a semitone");
        CHECK (editor_selected () == p->nnotes, "and they stay selected");
      }
      key (b, reason, app.w_edit, 0x1AF);         /* Ctrl-Up */
      return 1;
    case 118:
      {
        gr_pattern *p = &app.song.patterns[app.song.regions[ed_region].pattern];
        int k, ok = 1;
        for (k = 0; k < p->nnotes; k++)
          ok &= p->notes[k].vel == (ed_before.notes[k].vel + 10 > 127 ? 127 : ed_before.notes[k].vel + 10);
        CHECK (ok, "Ctrl-Up makes them louder (velocity +10)");
      }
      key (b, reason, app.w_edit, 0x03);          /* Ctrl-C */
      return 1;
    case 119:
      key (b, reason, app.w_edit, 0x7F);          /* Delete */
      return 1;
    case 120:
      CHECK (app.song.patterns[app.song.regions[ed_region].pattern].nnotes == 0,
             "Delete removes the selected notes (not the region)");
      CHECK (app.song.regions[ed_region].used, "(the region is still there)");
      gr_locate (app.eng, app.song.regions[ed_region].start);
      key (b, reason, app.w_edit, 0x16);          /* Ctrl-V */
      return 1;
    case 121:
      {
        gr_pattern *p = &app.song.patterns[app.song.regions[ed_region].pattern];
        int first = ed_before.notes[0].tick;
        CHECK (p->nnotes == ed_before.nnotes && p->notes[0].tick == 0
               && p->notes[p->nnotes - 1].tick == ed_before.notes[ed_before.nnotes - 1].tick - first,
               "Ctrl-V pastes the copied notes at the playhead");
        /* Drag the (selected) notes a step to the right by one of them. */
        click (b, reason, app.w_edit, lay_roll_x (app.edit_width, p->len, p->notes[0].tick) + 4,
               lay_roll_y (p->notes[0].pitch) - 5, BUT_SELECT);
        ed_tick0 = p->notes[0].tick;
        ed_pitch0 = p->notes[0].pitch;
      }
      return 1;
    case 122:
      {
        gr_pattern *p = &app.song.patterns[app.song.regions[ed_region].pattern];
        point (app.w_edit, lay_roll_x (app.edit_width, p->len, ed_tick0 + GR_STEP) + 4,
               lay_roll_y (ed_pitch0) - 5, BUT_SELECT);
      }
      *reason = 0;
      return 1;
    case 123:
      release (reason);
      return 1;
    case 124:
      {
        gr_pattern *p = &app.song.patterns[app.song.regions[ed_region].pattern];
        int k, ok = p->nnotes == ed_before.nnotes;
        for (k = 0; ok && k < p->nnotes; k++)
          ok = is_shifted (p, k);
        CHECK (ok && p->notes[0].tick == ed_tick0 + GR_STEP, "dragging one selected note moves them all");
        /* Shift-click a note: out of the selection. */
        fake.shift = 1;
        click (b, reason, app.w_edit, lay_roll_x (app.edit_width, p->len, p->notes[0].tick) + 4,
               lay_roll_y (p->notes[0].pitch) - 5, BUT_SELECT);
      }
      return 1;
    case 125:
      fake.shift = 0;
      release (reason);
      return 1;
    case 126:
      CHECK (editor_selected () == ed_before.nnotes - 1, "Shift-click takes a note out of the selection");
      /* ---- regions: split at the playhead, copy, paste ---- */
      {
        gr_region *r = &app.song.regions[ed_region];
        app.sel_region = ed_region;
        ed_regions = count_regions ();
        ed_end = r->start + r->len;
        gr_locate (app.eng, r->start + GR_BAR);
      }
      key (b, reason, app.w_main, 0x14);          /* Ctrl-T */
      return 1;
    case 127:
      CHECK (count_regions () == ed_regions + 1 && app.song.regions[ed_region].len == GR_BAR
             && app.sel_region != ed_region && app.song.regions[app.sel_region].start
             + app.song.regions[app.sel_region].len == ed_end,
             "Ctrl-T splits the selected region at the playhead");
      key (b, reason, app.w_main, 0x03);          /* Ctrl-C (the right part) */
      return 1;
    case 128:
      gr_locate (app.eng, 40 * GR_BAR);
      key (b, reason, app.w_main, 0x16);          /* Ctrl-V */
      return 1;
    case 129:
      CHECK (count_regions () == ed_regions + 2 && app.song.regions[app.sel_region].start == 40 * GR_BAR,
             "Ctrl-C and Ctrl-V paste a copy of the region at the playhead");
      mark_saved ();
      /* ---- the count-in, from the menu's option ---- */
      app.count_in = 1;
      key (b, reason, app.w_main, 'r');
      return 1;
    case 130:
      CHECK (gr_recording (app.eng) && gr_counting_in (app.eng) == 4 && !gr_playing (app.eng),
             "with Count-in on, R counts in a bar before recording");
      key (b, reason, app.w_main, ' ');
      return 1;
    case 131:
      CHECK (!gr_recording (app.eng) && !gr_counting_in (app.eng) && !gr_playing (app.eng),
             "Space during the count-in cancels the recording");
      mark_saved ();
      /* ---- tracks: rename, reorder, master volume, colours ---- */
      app.sel_track = 1;
      click (b, reason, app.w_main, HDR_W + 400, LANES_TOP - 900, BUT_MENU);
      return 1;
    case 132:
      {
        /* Main menu > Track > Rename: type into the writable item, press Return. */
        const int *m_main = (const int *) fake.menu, *m_track = (const int *) m_main[7 + 6 * 1 + 1];
        const int *m_ren = (const int *) m_track[7 + 6 * 4 + 1];
        char *buf = (char *) m_ren[7 + 3];
        CHECK ((m_ren[7] & 4) && !strcmp (buf, app.song.tracks[1].name),
               "Track > Rename is a writable item holding the track's name");
        strcpy (buf, "  My Keys ");
        b[0] = 1; b[1] = 4; b[2] = 0; b[3] = -1;
        *reason = 9;
      }
      return 1;
    case 133:
      CHECK (!strcmp (app.song.tracks[1].name, "My Keys"), "Return renames the track (spaces trimmed)");
      {
        /* A new sound keeps a name of your own. */
        int sel[3] = { PATCH_ORGAN, -1, -1 };
        (void) sel;
      }
      b[0] = 1; b[1] = 3; b[2] = PATCH_ORGAN; b[3] = -1;     /* Track > Sound > Drawbar Organ */
      *reason = 9;
      return 1;
    case 134:
      CHECK (app.song.tracks[1].patch == PATCH_ORGAN && app.song.tracks[1].gm == -1
             && !strcmp (app.song.tracks[1].name, "My Keys"), "choosing a sound keeps a name you gave the track");
      /* Drag track 0 by its name down to the third place. */
      ren_name0 = app.song.tracks[0].name[0];
      strcpy (ren_names[0], app.song.tracks[0].name);
      strcpy (ren_names[1], app.song.tracks[1].name);
      strcpy (ren_names[2], app.song.tracks[2].name);
      ren_regions = 0;
      {
        int i;
        for (i = 0; i < GR_MAX_REGIONS; i++)
          if (app.song.regions[i].used && app.song.regions[i].track == 0)
            ren_regions++;
      }
      app.view.hscroll = 0;
      click (b, reason, app.w_main, 40, lay_track_top (0) - 20, BUT_SELECT);
      return 1;
    case 135:
      point (app.w_main, 40, lay_track_top (2) - 50, BUT_SELECT);
      *reason = 0;
      return 1;
    case 136:
      release (reason);
      return 1;
    case 137:
      {
        int i, n = 0;
        for (i = 0; i < GR_MAX_REGIONS; i++)
          if (app.song.regions[i].used && app.song.regions[i].track == 2)
            n++;
        CHECK (!strcmp (app.song.tracks[2].name, ren_names[0]) && !strcmp (app.song.tracks[0].name, ren_names[1])
               && !strcmp (app.song.tracks[1].name, ren_names[2]),
               "dragging a track's name moves the track down the list");
        CHECK (n == ren_regions && app.sel_track == 2, "with its regions, and it stays selected");
      }
      app.song.master = 0.8f;
      tool_click (b, reason, TB_MASTER, BUT_SELECT);
      return 1;
    case 138:
      release (reason);
      return 1;
    case 139:
      {
        lay_box bx = lay_toolbar_box (TB_MASTER);
        float want = (float) ((bx.x0 + bx.x1) / 2 - bx.x0 - 72) / (float) (bx.x1 - 20 - bx.x0 - 72);
        CHECK (app.song.master > want - 0.02f && app.song.master < want + 0.02f,
               "the master volume slider sets the song's volume");
      }
      CHECK (gfx_track_colour (TRK_INST, PATCH_BASS, -1, 0) != gfx_track_colour (TRK_INST, PATCH_EPIANO, -1, 0)
             && gfx_track_colour (TRK_INST, PATCH_EPIANO, -1, 0) == gfx_track_colour (TRK_INST, 0, 0, 0)
             && gfx_track_colour (TRK_INST, 0, 33, 0) == gfx_track_colour (TRK_INST, PATCH_BASS, -1, 0),
             "tracks are coloured by family (bass, keys...), Groove's and SoundFont sounds alike");
      /* A Maestro file dropped on the song window: one bar of 3/4. */
      {
        static const unsigned char music[] = {
          1, 0x40, 0, 0, 0, 7,                                  /* 7 gate bytes */
          0x40, 0, 0, 0, 6, 0x40, 0, 0, 0, 0, 0x40, 0, 0, 0, 0, 0x40, 0, 0, 0, 0,
          0x40, 0, 0, 0, 0, 0x40, 0, 0, 0, 0, 0x40, 0, 0, 0, 0, 0x40, 0, 0, 0, 0,
          0, 0x65, 1, 1, 1, 0, 0x20,                            /* 3/4, three notes, bar line */
          16 << 3, 3 << 5, 17 << 3, 3 << 5, 18 << 3, 3 << 5 };  /* B, C, D crotchets */
        FILE *f = fopen ("/tmp/groove_ui,af1", "wb");
        fwrite ("Maestro\n\2", 1, 9, f);
        fwrite (music, 1, sizeof music, f);
        fclose (f);
      }
      gr_stop (app.eng);
      arrange_new_song ();
      midi_before = app.song.ntracks;
      message (b, reason, MSG_DATALOAD, app.w_main, "/tmp/groove_ui,af1", FILETYPE_MAESTRO);
      return 1;
    case 140:
      CHECK (fake.msg[4] == MSG_DATALOADACK && midi_before == 2 && app.song.ntracks == 1 && count_regions () == 1,
             "a Maestro file dropped on a new song becomes its track (the empty starter tracks make way)");
      CHECK (gr_meter_at (&app.song, 0)->num == 3 && gr_bar_tick (&app.song, 1) == 3 * GR_PPQ,
             "and the song takes its 3/4 time");
      CHECK (!strcmp (app.song.tracks[0].name, "WaveSynth-Beep 1"),
             "the channel's voice comes from the computer's voices (Sound_InstallVoice)");
      gr_locate (app.eng, gr_bar_tick (&app.song, 1));
      click (b, reason, app.w_main, 600, lay_track_top (0) - 50, BUT_MENU);
      return 1;
    case 141:
      b[0] = 0; b[1] = 5; b[2] = 4; b[3] = -1;   /* Song > Time signature > 6/8 */
      *reason = 9;
      return 1;
    case 142:
      CHECK (gr_meter_at (&app.song, 0)->num == 3 && gr_meter_at (&app.song, 1)->num == 6
             && gr_meter_at (&app.song, 1)->den == 8,
             "Song > Time signature changes it from the bar the playhead is in");
      key (b, reason, app.w_main, 0x1A);
      return 1;
    case 143:
      CHECK (app.song.nmeters == 1 && gr_meter_at (&app.song, 5)->num == 3, "Undo takes the time signature change back");
      /* The horizontal scroll bar, dragged to 800 OS units along. */
      b[0] = app.w_main;
      _swix (Wimp_GetWindowState, _IN (1), b);
      b[5] = 800;
      *reason = 2;
      return 1;
    case 144:
      {
        int s[10];
        s[0] = app.w_main;
        _swix (Wimp_GetWindowState, _IN (1), s);
        CHECK (app.view.hscroll == 800 * GR_BAR / app.view.bar_w && s[5] == 800,
               "dragging the horizontal scroll bar moves along the song");
      }
      memset (b, 0, 64);
      b[0] = app.w_main; b[8] = 1;          /* the wheel sideways: a bar on */
      *reason = 10;
      return 1;
    case 145:
      {
        int s[10];
        s[0] = app.w_main;
        _swix (Wimp_GetWindowState, _IN (1), s);
        CHECK (s[5] == (int) ((long long) app.view.hscroll * app.view.bar_w / GR_BAR) && s[5] > 800,
               "and moving along the song another way moves the scroll bar with it");
      }
      /* A MIDI keyboard, through a MIDI module. */
      gr_stop (app.eng);
      arrange_new_song ();
      app.count_in = 0;
      fake.midi_present = 1;
      CHECK (midiin_present () == 0, "(no MIDI module was there when Groove first looked)");
      _swix (Wimp_SetCaretPosition, _INR (0, 5), app.w_main, -1, 0, 0, 1 << 25, -1);
      app.sel_track = 1;
      *reason = 0;
      return 1;
    case 146:
      CHECK (!midiin_active (), "without a MIDI module, MIDI input is off");
      midiin_forget_module ();          /* (as when the MIDI module is loaded later) */
      CHECK (midiin_present () && midiin_active () && midiin_port () == MIDI_ANY,
             "with a MIDI module, Groove listens to any port");
      key (b, reason, app.w_main, 'r');
      return 1;
    case 147:
      CHECK (gr_recording (app.eng), "(recording)");
      fake.midi_n = fake.midi_i = 0;
      fake.midi_q[fake.midi_n++] = 0x90 | 60 << 8 | 90 << 16 | 3 << 24;   /* note on, middle C */
      fake.midi_q[fake.midi_n++] = 0x91 | 67 << 8 | 100 << 16 | 3 << 24;  /* and G, another channel */
      *reason = 0;
      return 1;
    case 148:
      CHECK (midiin_held () == 2 && fake.midi_port_asked == -1, "notes from the MIDI keyboard play");
      CHECK (midiin_lit (), "and light the MIDI light");
      fake.midi_q[fake.midi_n++] = 0xB0 | 64 << 8 | 127 << 16 | 3 << 24;  /* sustain pedal down */
      fake.midi_q[fake.midi_n++] = 0x80 | 60 << 8 | 64 << 16 | 3 << 24;   /* C up */
      fake.midi_q[fake.midi_n++] = 0x91 | 67 << 8 | 0 << 16 | 3 << 24;    /* G up (velocity 0) */
      *reason = 0;
      return 1;
    case 149:
      CHECK (midiin_held () == 1, "with the pedal down on its channel, C is held; G (another channel) ends");
      fake.midi_q[fake.midi_n++] = 0xB0 | 64 << 8 | 0 << 16 | 3 << 24;    /* pedal up */
      *reason = 0;
      return 1;
    case 150:
      CHECK (midiin_held () == 0, "letting the pedal go ends the note");
      key (b, reason, app.w_main, 'r');
      return 1;
    case 151:
      {
        int i, k, c = 0, g = 0;
        for (i = 0; i < GR_MAX_REGIONS; i++)
          if (app.song.regions[i].used && app.song.regions[i].pattern >= 0)
            {
              const gr_pattern *p = &app.song.patterns[app.song.regions[i].pattern];
              for (k = 0; k < p->nnotes; k++)
                {
                  c |= p->notes[k].pitch == 60 && p->notes[k].vel == 90;
                  g |= p->notes[k].pitch == 67 && p->notes[k].vel == 100;
                }
            }
        CHECK (!gr_recording (app.eng) && c && g, "they're recorded, with the keyboard's velocity");
      }
      /* Another program has the focus: Groove leaves the MIDI input alone. */
      _swix (Wimp_SetCaretPosition, _INR (0, 5), -1, -1, 0, 0, 1 << 25, -1);
      fake.midi_n = fake.midi_i = 0;
      fake.midi_q[fake.midi_n++] = 0x90 | 62 << 8 | 80 << 16 | 3 << 24;
      *reason = 0;
      return 1;
    case 152:
      CHECK (fake.midi_i == 0 && midiin_held () == 0, "without the input focus Groove doesn't read the MIDI keyboard");
      _swix (Wimp_SetCaretPosition, _INR (0, 5), app.w_main, -1, 0, 0, 1 << 25, -1);
      *reason = 0;
      return 1;
    case 153:
      CHECK (fake.midi_i == 1 && midiin_held () == 0, "and what waited meanwhile is thrown away, not played late");
      app.sel_track = 0;                  /* the drums */
      fake.midi_q[fake.midi_n++] = 0x99 | 38 << 8 | 110 << 16 | 3 << 24;  /* GM snare */
      *reason = 0;
      return 1;
    case 154:
      CHECK (midiin_held () == 1 && gr_drum_row_for_key (38) == 1, "on a drum track, a drum key plays its drum");
      b[0] = 6; b[1] = 0; b[2] = -1;     /* MIDI keyboard > Off */
      app_main_menu (100, 900);
      *reason = 9;
      return 1;
    case 155:
      CHECK (midiin_port () == MIDI_OFF && !midiin_active () && midiin_held () == 0,
             "MIDI keyboard > Off stops listening (and lets go of held notes)");
      /* Pete Everett's MIDI Support, loaded now: Groove registers as a driver. */
      fake.ms_present = 1;
      b[0] = 6; b[1] = 1; b[2] = -1;     /* MIDI keyboard > MIDI Support */
      app_main_menu (100, 900);          /* (building the menu registers) */
      CHECK (midiin_support () && !strcmp (fake.ms_name, "Groove") && fake.ms_flags == (1 | 2 | 4 | 8),
             "with MIDI Support there, Groove registers as a driver called Groove (sends and receives, queued, whole messages)");
      CHECK (fake.ms_version == 23 && fake.ms_date[0] != '0' && !strchr (fake.ms_date, '-'),
             "it gives MIDI Support its version in hundredths (23, shown as 0:23) and a date like the others' (4 Nov 2026)");
      *reason = 9;
      return 1;
    case 156:
      CHECK (midiin_port () == MIDI_SUPPORT && midiin_active (), "MIDI keyboard > MIDI Support listens to what's routed to Groove");
      _swix (Wimp_SetCaretPosition, _INR (0, 5), -1, -1, 0, 0, 1 << 25, -1);   /* another program has the focus */
      app.sel_track = 1;
      fake.ms_n = fake.ms_i = 0;
      fake.ms_q[fake.ms_n++] = 0x90 | 64 << 8 | 70 << 16 | 3 << 24;
      *reason = 0;
      return 1;
    case 157:
      CHECK (fake.ms_i == 1 && midiin_held () == 1,
             "notes routed to Groove play even when another program has the input focus");
      fake.ms_q[fake.ms_n++] = 0x80 | 64 << 8 | 0 << 16 | 3 << 24;
      *reason = 0;
      return 1;
    case 158:
      CHECK (midiin_held () == 0, "and end");
      /* ---- MIDI out ---- */
      CHECK (midiout_port () == MIDI_OFF, "MIDI out starts Off");
      app.sel_track = 1;
      app.song.tracks[1].type = TRK_INST;
      fake.ms_nsent = 0;
      b[0] = 1; b[1] = 6; b[2] = -1;     /* Track > Play on MIDI out */
      app_main_menu (100, 900);
      *reason = 9;
      return 1;
    case 159:
      CHECK (app.song.tracks[1].midi_out == 1 && midiout_port () == MIDI_SUPPORT,
             "Track > Play on MIDI out ticks the track, and with MIDI out Off picks MIDI Support");
      fake.ms_q[fake.ms_n++] = 0x90 | 62 << 8 | 80 << 16 | 3 << 24;   /* a key from the keyboard */
      *reason = 0;
      return 1;
    case 160:
      {
        int i, on = -1, prog = -1;
        for (i = 0; i < fake.ms_nsent; i++)
          {
            if ((fake.ms_sent[i] & 0xF0) == 0xC0) prog = i;
            if ((fake.ms_sent[i] & 0xFFFFF0) == (0x90u | 62 << 8 | 80 << 16)) on = i;
          }
        CHECK (on >= 0 && prog >= 0 && prog < on && (fake.ms_sent[on] >> 24) == 3,
               "a note played on a MIDI out track goes out through MIDI Support straight away, after its program");
      }
      fake.ms_q[fake.ms_n++] = 0x80 | 62 << 8 | 3 << 24;
      *reason = 0;
      return 1;
    case 161:
      {
        int p = gr_pattern_new (&app.song, "Out", GR_BAR);
        gr_pattern_add_note (&app.song.patterns[p], 0, GR_PPQ, 67, 100);
        gr_region_add (&app.song, 1, 0, GR_BAR, p);
      }
      CHECK (fake.ms_nsent >= 1 && (fake.ms_sent[fake.ms_nsent - 1] & 0xFFF0) == (0x80u | 62 << 8),
             "and its note-off");
      fake.ms_nsent = 0;
      saved_tracks = (int) fake.frames_played;
      gr_locate (app.eng, 0);
      gr_play (app.eng);
      wait_nulls = 0;
      *reason = 0;
      return 1;
    case 162:
      {
        int i, on = -1;
        for (i = 0; i < fake.ms_nsent; i++)
          if ((fake.ms_sent[i] & 0xFFFFF0) == (0x90u | 67 << 8 | 100 << 16)) on = i;
        if (on < 0 && ++wait_nulls < 20)
          {
            step--;               /* (wait for it) */
            *reason = 0;
            return 1;
          }
        CHECK (on >= 0 && (int) fake.ms_sent_at[on] - saved_tracks >= 2000,
               "a MIDI out track's notes in the song go out when Groove's sound from that moment is heard (~70 ms later)");
      }
      gr_stop (app.eng);
      fake.ms_nsent = 0;
      wait_nulls = 0;
      *reason = 0;
      return 1;
    case 163:
      {
        int i, off = 0;
        for (i = 0; i < fake.ms_nsent; i++)
          if ((fake.ms_sent[i] & 0xFFF0) == (0xB0u | 123 << 8)) off = 1;
        if (!off && ++wait_nulls < 20)
          {
            step--;
            *reason = 0;
            return 1;
          }
        CHECK (off, "stopping sends All Notes Off");
      }
      fake.midi_present = 1;
      b[0] = 7; b[1] = 3; b[2] = -1;     /* MIDI out > Port 2 */
      app_main_menu (100, 900);
      *reason = 9;
      return 1;
    case 164:
      CHECK (midiout_port () == 1, "MIDI out > Port 2 sends to a MIDI module's second port");
      fake.midi_ntx = 0;
      gr_live_on (app.eng, 1, 69, 90);
      gr_live_off (app.eng, 1, 69);
      {
        int i, on = 0;
        for (i = 0; i < fake.midi_ntx; i++)
          if (fake.midi_tx[i] == (0x13000090u | 69 << 8 | 90 << 16)) on = 1;
        CHECK (on, "with MIDI_TxCommand, port in the top bits");
      }
      b[0] = 7; b[1] = 0; b[2] = -1;     /* MIDI out > Off */
      app_main_menu (100, 900);
      *reason = 9;
      return 1;
    case 165:
      CHECK (midiout_port () == MIDI_OFF && !midiout_on (), "MIDI out > Off");
      fake.midi_ntx = fake.ms_nsent = 0;
      gr_live_on (app.eng, 1, 69, 90);
      gr_live_off (app.eng, 1, 69);
      CHECK (fake.midi_ntx == 0 && fake.ms_nsent == 0 && gr_busy (app.eng),
             "with MIDI out off, a MIDI out track plays Groove's own sound");
      /* ---- F10: cycle through the MIDI out choices ---- */
      key (b, reason, app.w_main, 0x1CA);
      return 1;
    case 166:
      CHECK (midiout_port () == MIDI_SUPPORT && midiout_on () && !strcmp (midiout_name (), "Support"),
             "F10 from Off: MIDI out > MIDI Support");
      key (b, reason, app.w_main, 0x1CA);
      return 1;
    case 167:
      CHECK (midiout_port () == 0, "F10 again: Port 1");
      CHECK (strstr (arrange_window_title (), "MIDI out: Port 1") != NULL,
             "the song window's title says where MIDI out goes");
      key (b, reason, app.w_main, 0x1CA);
      return 1;
    case 168:
      CHECK (midiout_port () == 1 && !strcmp (midiout_name (), "Port 2"), "Port 2");
      midiout_cycle ();
      midiout_cycle ();
      CHECK (midiout_port () == 3, "on to Port 4");
      midiout_cycle ();
      CHECK (midiout_port () == MIDI_OFF && !midiout_on (), "and round to Off");
      CHECK (!strstr (arrange_window_title (), "MIDI"), "with MIDI out off, the title is just the song's name");
      return 1;
    case 169:
      fake.midi_present = 0;               /* no MIDI module: its ports are skipped */
      midiout_cycle ();
      CHECK (midiout_port () == MIDI_SUPPORT, "without a MIDI module, F10 goes Off, MIDI Support...");
      midiout_cycle ();
      CHECK (midiout_port () == MIDI_OFF, "... and straight back to Off");
      fake.midi_present = 1;
      *reason = 0;
      return 1;
    case 170:
      /* ---- F11 / Shift-F11: Play on MIDI out for the track / the song ---- */
      {
        int t;
        for (t = 0; t < app.song.ntracks; t++)
          app.song.tracks[t].midi_out = 0;
      }
      app.sel_track = 0;
      key (b, reason, app.w_main, 0x1CB);
      return 1;
    case 171:
      CHECK (app.song.tracks[0].midi_out == 1 && midiout_on (),
             "F11: the selected track on MIDI out (MIDI out was Off: it picks the first there)");
      key (b, reason, app.w_main, 0x1CB);
      return 1;
    case 172:
      CHECK (app.song.tracks[0].midi_out == 0, "F11 again: off");
      key (b, reason, app.w_main, 0x1DB);
      return 1;
    case 173:
      {
        int t, all = 1;
        for (t = 0; t < app.song.ntracks; t++)
          if (app.song.tracks[t].type != TRK_AUDIO && !app.song.tracks[t].midi_out) all = 0;
          else if (app.song.tracks[t].type == TRK_AUDIO && app.song.tracks[t].midi_out) all = 0;
        CHECK (all, "Shift-F11: every drum and instrument track on MIDI out (audio tracks not)");
      }
      b[0] = 0; b[1] = 6; b[2] = -1;     /* Song > Play on MIDI out */
      app_main_menu (100, 900);
      *reason = 9;
      return 1;
    case 174:
      {
        int t, any = 0;
        for (t = 0; t < app.song.ntracks; t++)
          any |= app.song.tracks[t].midi_out;
        CHECK (!any, "Song > Play on MIDI out, with them all on: all off");
      }
      app.song.tracks[1].midi_out = 1;   /* one on: Song ticks the rest */
      app_song_midi_out ();
      {
        int t, all = 1;
        for (t = 0; t < app.song.ntracks; t++)
          if (app.song.tracks[t].type != TRK_AUDIO && !app.song.tracks[t].midi_out) all = 0;
        CHECK (all, "with only some on, it puts them all on");
      }
      app_song_midi_out ();
      *reason = 0;
      return 1;
    case 175:
    case 176:
      mark_saved ();
      *reason = 0;
      return 1;
    }
  return 0;                       /* the end: the fake sends Message_Quit */
}

int main (void)
{
  int rc;
  remove ("/tmp/groove_ui.song");
  remove ("/tmp/groove_ui_drag.song");
  remove ("/tmp/groove_ui.wav");
  remove ("/tmp/groove_ui.mid");
  /* A web browser for HTML, as NetSurf sets up. */
  setenv ("Alias$@RunType_FAF", "/<NetSurf$Dir>.!Run %*0", 1);
  /* !SoftMIDI's variable, as if the Filer had seen it. */
  if (getenv ("GROOVE_TEST_SOUNDFONT"))
    setenv ("SoftMIDI$SoundFont", getenv ("GROOVE_TEST_SOUNDFONT"), 1);
  mkdir ("/tmp/groove_shots", 0755);
  test_workspace ();
  rc = groove_main ();
  CHECK (rc == 0, "Groove exits cleanly");
  CHECK (step == 178, "the whole script ran");
  CHECK (fake.ms_removed == 7, "on quitting, Groove leaves MIDI Support (RemoveDriver)");
  CHECK (fake.closed_down && fake.ring == NULL, "on quit: SharedSound handler removed, Wimp_CloseDown");
  CHECK (fake.errors == 4, "four error boxes in all (two quit questions, no web browser, load question)");
  CHECK (fake.nulls_while_masked == 0, "no null events were forced on the app while it was idle");
  printf ("  %d polls, %d redraws, %d plots, %d texts, %d SWIs\n",
          fake.polls, fake.redraws, fake.plots, fake.texts, fake.swis);
  printf ("%d checks, %d failed\n", checks, failures);
  return failures != 0;
}

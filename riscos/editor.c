/* Groove - the editor: a step grid for drum regions, a piano roll for
   instrument regions. The grid always fills the window's width (resize the
   window and the steps get wider), so a one-bar beat has big friendly
   cells and a four-bar riff still fits without scrolling sideways.

   Edits copy a region's pattern first if other regions share it (made by
   Duplicate), so changing one copy never surprises you elsewhere. */
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <swis.h>
#include "app.h"
#include "gfx.h"

static int region = -1;

/* The selected notes in the piano roll, by start and pitch (indices change
   as notes are added and moved). */
typedef struct { int tick, pitch; } note_ref;
static note_ref sel[GR_MAX_NOTES];
static int nsel;
static int band, band_x0, band_y0, band_x1, band_y1;   /* a Shift-drag selection box */
static gr_note note_clip[GR_MAX_NOTES];                /* copied notes, from tick 0 */
static int note_clip_n;

static int is_sel (const gr_note *n)
{
  int i;
  for (i = 0; i < nsel; i++)
    if (sel[i].tick == n->tick && sel[i].pitch == n->pitch)
      return 1;
  return 0;
}

static void sel_add (const gr_note *n)
{
  if (!is_sel (n) && nsel < GR_MAX_NOTES)
    {
      sel[nsel].tick = n->tick;
      sel[nsel].pitch = n->pitch;
      nsel++;
    }
}

static void sel_remove (const gr_note *n)
{
  int i;
  for (i = 0; i < nsel; i++)
    if (sel[i].tick == n->tick && sel[i].pitch == n->pitch)
      {
        sel[i] = sel[--nsel];
        return;
      }
}

int editor_selected (void) { return nsel; }
static char title[64] = "Editor";
static int last_step = -1;      /* playhead column last drawn */
static int head_scroll;         /* the scroll offset the header was last drawn at */
static int lane_scroll;         /* and the controller lane */
static int lane_vis_h;          /* the window's visible height when last opened */

/* The header strip (the region's name, the sound, the length buttons) is
   pinned to the top of the window: it is drawn at the visible top whatever
   the scroll, so it never scrolls away with a long piano roll (seen on
   RISC OS 5.31-5). In work-area terms it covers [scroll - ED_HEAD_H, scroll]. */
static void redraw_head (int scroll)
{
  app_redraw_work (app.w_edit, 0, scroll - ED_HEAD_H, 0x2000, scroll);
}

/* Header buttons (work-area x ranges, in the ED_HEAD_H strip). */
enum { EB_SOUND, EB_LEN1, EB_LEN2, EB_LEN4, EB_CLEAR, EB_KEYS, EB_COUNT };
static const int eb_x[EB_COUNT][2] = { { 300, 640 }, { 660, 780 }, { 788, 908 }, { 916, 1036 }, { 1056, 1168 },
                                       { 1188, 1332 } };
#define ED_HINT_X 1352

/* ---- the step grid's rows -------------------------------------------------
   Each row is a drum key (0..127). Kit rows: Groove's eight drums, then any
   other keys the part uses, lowest first. All keys: 0 to 127. */
static int all_keys;
static int drow[128], ndrow = GR_DRUM_SOUNDS;

static gr_pattern *pat (void);
static gr_track *trk (void);

/* fix: also size the window's extent to the rows (not while redrawing). */
static void drum_rows (int fix)
{
  gr_pattern *p = pat ();
  unsigned char used[128];
  int k, i, before = ndrow;
  ndrow = 0;
  if (all_keys)
    for (k = 0; k < 128; k++)
      drow[ndrow++] = k;
  else
    {
      memset (used, 0, sizeof used);
      for (i = 0; i < GR_DRUM_SOUNDS; i++)
        {
          drow[ndrow++] = gr_drum_kit_key (i);
          used[drow[i]] = 2;
        }
      if (p)
        for (k = 0; k < p->nnotes; k++)
          if (!used[p->notes[k].pitch & 127])
            used[p->notes[k].pitch & 127] = 1;
      for (k = 0; k < 128; k++)
        if (used[k] == 1)
          drow[ndrow++] = k;
    }
  lay_drum_rows = ndrow;
  if (ndrow != before)
    lay_drum_h = lay_fit_drum_rows (0);
  if (fix)
    {
      gr_track *t = trk ();
      app_set_extent (app.w_edit, 0x2000, lay_editor_height (t ? t->type : TRK_INST));
    }
}

/* The song has changed (an edit, Undo, an import): the rows may have too,
   and selected notes may have gone. */
void editor_changed (void)
{
  gr_track *t = trk ();
  gr_pattern *p = pat ();
  int i, k;
  if (t && t->type == TRK_DRUMS)
    drum_rows (1);
  for (i = 0; i < nsel; )
    {
      for (k = 0; p && k < p->nnotes; k++)
        if (p->notes[k].tick == sel[i].tick && p->notes[k].pitch == sel[i].pitch)
          break;
      if (p && k < p->nnotes)
        i++;
      else
        sel[i] = sel[--nsel];
    }
}

/* The note on a drum key in a step (anywhere in it: an imported note may
   not be on the step), or -1. */
static int step_note (const gr_pattern *p, int step, int key)
{
  int k, t0 = step * GR_STEP;
  for (k = 0; k < p->nnotes; k++)
    if (p->notes[k].pitch == key && p->notes[k].tick >= t0 && p->notes[k].tick < t0 + GR_STEP)
      return k;
  return -1;
}

static gr_region *reg (void)
{
  if (region < 0 || !app.song.regions[region].used || app.song.regions[region].clip >= 0)
    return NULL;
  return &app.song.regions[region];
}

static gr_pattern *pat (void)
{
  gr_region *r = reg ();
  return r && r->pattern >= 0 ? &app.song.patterns[r->pattern] : NULL;
}

/* The time signature where the region starts: steps (sixteenths) in a
   bar and in a beat, for the grid lines. */
static void meter_steps (int *bar_steps, int *beat_steps)
{
  gr_region *r = reg ();
  int b = r ? gr_tick_bar (&app.song, r->start) : 0;
  *bar_steps = gr_bar_len (&app.song, b) / GR_STEP;
  *beat_steps = gr_beat_len (&app.song, b) / GR_STEP;
  if (*bar_steps < 1) *bar_steps = 1;
  if (*beat_steps < 1) *beat_steps = 1;
}

/* A bar of the region's time signature, in ticks. */
static int region_bar (void)
{
  gr_region *r = reg ();
  return gr_bar_len (&app.song, r ? gr_tick_bar (&app.song, r->start) : 0);
}


static gr_track *trk (void)
{
  gr_region *r = reg ();
  return r ? &app.song.tracks[r->track] : NULL;
}

static gr_pattern *pat_for_edit (void)
{
  if (!reg ())
    return NULL;
  gr_pattern_unshare (&app.song, region);
  return pat ();
}

int editor_create (void)
{
  int b[24];
  memset (b, 0, sizeof b);
  b[0] = 200; b[1] = 120; b[2] = 1500; b[3] = 640;
  b[6] = -1;
  b[7] = (int) 0xBF000002;
  ((unsigned char *) b)[32] = 7;  ((unsigned char *) b)[33] = 2;
  ((unsigned char *) b)[34] = 7;  ((unsigned char *) b)[35] = 255;
  ((unsigned char *) b)[36] = 3;  ((unsigned char *) b)[37] = 1;
  ((unsigned char *) b)[38] = 12;
  b[10] = 0; b[11] = -lay_editor_height (TRK_INST); b[12] = 0x2000; b[13] = 0;
  b[14] = 0x13D;
  b[15] = 3 << 12;
  b[16] = 1;
  ((short *) b)[34] = 640;  ((short *) b)[35] = 240;
  b[18] = (int) title; b[19] = -1; b[20] = sizeof title;
  b[21] = 0;
  app.edit_width = 1300;
  return (app_create_error = _swix (Wimp_CreateWindow, _IN (1) | _OUT (0), b, &app.w_edit)) != NULL;
}

static void set_title (void)
{
  gr_pattern *p = pat ();
  gr_track *t = trk ();
  if (p && t)
    snprintf (title, sizeof title, "%s - %s", p->name, t->name);
  else
    snprintf (title, sizeof title, "Editor - choose a region in the song");
  if (app.edit_open)
    {
      int o[5];
      o[0] = app.w_edit;
      _swix (Wimp_GetWindowOutline, _IN (1), o);
      _swix (Wimp_ForceRedraw, _INR (0, 4), -1, o[1], o[4] - 48, o[3], o[4]);
    }
}

void editor_show (int r)
{
  gr_track *t;
  if (r != region)
    nsel = 0;
  region = r;
  t = trk ();
  last_step = -1;
  set_title ();
  lay_label_w = t && t->type == TRK_DRUMS ? ED_LABEL_DRUMS : ED_LABEL_KEYS;
  if (t && t->type == TRK_DRUMS)
    drum_rows (0);
  app_set_extent (app.w_edit, 0x2000, lay_editor_height (t ? t->type : TRK_INST));
  if (!app.edit_open)
    return;
  /* For a piano roll, scroll to the notes (or to middle C). */
  if (t && t->type == TRK_INST)
    {
      gr_pattern *p = pat ();
      int s[10], lo = 60, hi = 60, k;
      if (p && p->nnotes)
        {
          lo = 127; hi = 0;
          for (k = 0; k < p->nnotes; k++)
            {
              if (p->notes[k].pitch < lo) lo = p->notes[k].pitch;
              if (p->notes[k].pitch > hi) hi = p->notes[k].pitch;
            }
        }
      s[0] = app.w_edit;
      _swix (Wimp_GetWindowState, _IN (1), s);
      s[6] = lay_roll_y ((lo + hi) / 2) + (s[4] - s[2]) / 2;
      if (s[6] > 0) s[6] = 0;
      _swix (Wimp_OpenWindow, _IN (1), s);
    }
  app_redraw_all (app.w_edit);
}

/* The step grid: all 128 drum keys, or the kit's rows (and keys in use).
   Showing all of them scrolls to General MIDI's drums (key 35 on). */
void editor_all_keys (int on)
{
  all_keys = on != 0;
  drum_rows (1);
  if (app.edit_open)
    {
      int s[10];
      s[0] = app.w_edit;
      _swix (Wimp_GetWindowState, _IN (1), s);
      s[6] = all_keys ? -35 * ED_DRUM_H : 0;
      _swix (Wimp_OpenWindow, _IN (1), s);
      app_redraw_all (app.w_edit);
    }
}

int editor_all_keys_on (void) { return all_keys; }

void editor_open (void)
{
  if (region < 0 && app.sel_region >= 0)
    region = app.sel_region;
  app_open_window (app.w_edit, WS_EDITOR);
  {
    /* Lay the grid out for the width it opened at (the workspace sizes it
       from the screen); resizes later come through editor_open_request. */
    int st[10];
    st[0] = app.w_edit;
    if (!_swix (Wimp_GetWindowState, _IN (1), st))
      {
        gr_track *t = trk ();
        app.edit_width = st[3] - st[1];
        lane_vis_h = st[4] - st[2];
        lane_scroll = st[6];
        lay_drum_h = lay_fit_drum_rows (lane_vis_h);
        if (t)
          app_set_extent (app.w_edit, 0x2000, lay_editor_height (t->type));
      }
  }
  app.edit_open = 1;
  editor_show (region);
  app_redraw_all (app.w_tool);
}

void editor_open_request (int *b)
{
  app_check (_swix (Wimp_OpenWindow, _IN (1), b));
  if (b[6] != head_scroll)
    {
      /* The Wimp moved the old header image with the scroll: redraw both
         where it was and where it now belongs. */
      redraw_head (head_scroll);
      redraw_head (b[6]);
      head_scroll = b[6];
    }
  if (b[3] - b[1] != app.edit_width)
    {
      app.edit_width = b[3] - b[1];
      app_redraw_all (app.w_edit);     /* re-flow the grid to the new width */
    }
  else if (b[4] - b[2] != lane_vis_h || b[6] != lane_scroll)
    app_redraw_all (app.w_edit);       /* the lane is pinned to the visible bottom */
  if (b[4] - b[2] != lane_vis_h)
    {
      /* Drum rows sized to fill what can be seen above the lane. */
      gr_track *t = trk ();
      lay_drum_h = lay_fit_drum_rows (b[4] - b[2]);
      if (t)
        app_set_extent (app.w_edit, 0x2000, lay_editor_height (t->type));
    }
  lane_vis_h = b[4] - b[2];
  lane_scroll = b[6];
  app.edit_open = 1;
}

/* ---- drawing ---------------------------------------------------------------- */

static int local_step (void)
{
  gr_region *r = reg ();
  gr_pattern *p = pat ();
  double pos;
  if (!r || !p || !gr_playing (app.eng))
    return -1;
  pos = gr_position (app.eng) - sound_latency () / (GR_RATE * 60.0 / ((double) app.song.tempo * GR_PPQ));
  if (pos < r->start || pos >= r->start + r->len)
    return -1;
  return ((int) (pos - r->start) % p->len) / GR_STEP;
}

static void paint_head (int ox, int oy, const int *clip)
{
  gr_pattern *p = pat ();
  gr_track *t = trk ();
  int i, y0 = oy - ED_HEAD_H, y1 = oy;
  gfx_colour (0x34363B);
  gfx_rect (clip[0], y0, clip[2], y1);
  if (!p || !t)
    {
      gfx_text (ox + 24, y0 + 20, "Click a region in the song, or double-click an empty track to make one.",
                C_TEXT_DIM, 0x34363B, 0);
      return;
    }
  gfx_text (ox + 16, y0 + 20, p->name, C_TEXT, 0x34363B, 0);
  for (i = 0; i < EB_COUNT; i++)
    {
      static const char *const labels[] = { "", "1 bar", "2 bars", "4 bars", "Clear", "" };
      int bl = region_bar ();
      int on = (i == EB_LEN1 && p->len == bl) || (i == EB_LEN2 && p->len == 2 * bl)
               || (i == EB_LEN4 && p->len == 4 * bl);
      unsigned bg = i == EB_SOUND ? gfx_track_colour (t->type, t->patch, t->gm, 1) : on ? C_BUTTON_ON : C_BUTTON;
      if (i == EB_KEYS && t->type != TRK_DRUMS)
        continue;                 /* (drums only) */
      gfx_colour (bg);
      gfx_round (ox + eb_x[i][0], y0 + 8, ox + eb_x[i][1], y1 - 8, 8);
      gfx_text (ox + (eb_x[i][0] + eb_x[i][1]) / 2, y0 + 20,
                i == EB_SOUND ? gr_sound_name (t) : i == EB_KEYS ? (all_keys ? "Kit rows" : "All keys")
                : labels[i], C_TEXT, bg, 1);
    }
  if (t->type == TRK_INST && nsel)
    {
      char hint[96];
      int k, v = 0;
      for (k = 0; k < p->nnotes; k++)
        if (is_sel (&p->notes[k]))
          {
            v = p->notes[k].vel;
            break;
          }
      snprintf (hint, sizeof hint, "%d selected  velocity %d: Ctrl+Up/Down   Up/Down: pitch", nsel, v);
      gfx_text (ox + ED_HINT_X, y0 + 20, hint, C_TEXT, 0x34363B, 0);
    }
  else
    gfx_text (ox + ED_HINT_X, y0 + 20, t->type == TRK_DRUMS ? "Select: on/off   Adjust: accent"
                                                      : "Select: draw/move   Adjust: delete   Shift: select",
              C_TEXT_DIM, 0x34363B, 0);
}

static void paint_grid (int ox, int oy, const int *clip)
{
  gr_pattern *p = pat ();
  gr_track *t = trk ();
  int w = app.edit_width, sw = lay_step_w (w, p->len), steps = p->len / GR_STEP;
  int row, st, play = local_step (), bar_st, beat_st;
  meter_steps (&bar_st, &beat_st);
  drum_rows (0);
  for (row = 0; row < ndrow; row++)
    {
      int top = oy - ED_HEAD_H - row * ED_DRUM_H, bot = top - ED_DRUM_H;
      if (!gfx_visible (clip, clip[0], bot, clip[2], top))
        continue;
      gfx_colour (row & 1 ? C_LANE_ALT : C_LANE);
      gfx_rect (clip[0], bot, clip[2], top);
      {
        /* Groove's own drums' names stand out; other keys are dimmer. */
        int own = gr_gm_drum_note (gr_drum_voice (drow[row])) == drow[row];
        gfx_colour (C_HEADER);
        gfx_rect (ox, bot, ox + ED_LABEL_W, top);
        char label[32];
        if (all_keys || !own)
          snprintf (label, sizeof label, "%3d  %s", drow[row], gr_drum_name (drow[row]));
        else
          snprintf (label, sizeof label, "%s", gr_drum_name (drow[row]));
        gfx_text (ox + 16, bot + (ED_DRUM_H - 20) / 2, label, own ? C_TEXT : C_TEXT_DIM, C_HEADER, 0);
      }
      for (st = 0; st < steps; st++)
        {
          int x0 = ox + ED_LABEL_W + st * sw, k = step_note (p, st, drow[row]);
          unsigned col;
          if (!gfx_visible (clip, x0, bot, x0 + sw, top))
            continue;
          if (k >= 0)
            {
              int v = p->notes[k].vel;
              col = v >= 110 ? 0xFFE27A : v >= 75 ? C_DRUMS : 0xA88F3A;
            }
          else
            col = st == play ? 0x5A5E68 : ((st % bar_st) / beat_st) & 1 ? 0x3A3D44 : 0x44484F;
          gfx_colour (col);
          gfx_round (x0 + 3, bot + 4, x0 + sw - 3, top - 4, 6);
        }
    }
  (void) t;
}

static int is_black (int pitch)
{
  int n = pitch % 12;
  return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
}

/* A note's colour by its velocity: the track's deep colour (soft) to a
   pale version of its colour (loud). */
static unsigned vel_colour (int vel)
{
  const gr_track *t = trk ();
  unsigned lo = gfx_track_colour (TRK_INST, t ? t->patch : 0, t ? t->gm : -1, 1);
  unsigned base = gfx_track_colour (TRK_INST, t ? t->patch : 0, t ? t->gm : -1, 0), hi = 0;
  int sh0;
  for (sh0 = 0; sh0 <= 16; sh0 += 8)     /* halfway from the colour to white */
    hi |= (unsigned) ((((base >> sh0) & 255) + 255) / 2) << sh0;
  unsigned c = 0;
  int sh;
  if (vel < 1) vel = 1;
  if (vel > 127) vel = 127;
  for (sh = 0; sh <= 16; sh += 8)
    {
      int a = (lo >> sh) & 255, b = (hi >> sh) & 255;
      c |= (unsigned) (a + (b - a) * vel / 127) << sh;
    }
  return c;
}

static void paint_roll (int ox, int oy, const int *clip)
{
  gr_pattern *p = pat ();
  int w = app.edit_width, sw = lay_step_w (w, p->len), steps = p->len / GR_STEP;
  int pitch, st, k, play = local_step (), px = 1 << app.xeig, bar_st, beat_st;
  char name[8];
  meter_steps (&bar_st, &beat_st);
  for (pitch = ED_HIGH; pitch >= ED_LOW; pitch--)
    {
      int top = oy + lay_roll_y (pitch), bot = top - ED_KEY_H;
      if (!gfx_visible (clip, clip[0], bot, clip[2], top))
        continue;
      gfx_colour (is_black (pitch) ? C_LANE_ALT : C_LANE);
      gfx_rect (ox + ED_LABEL_W, bot, clip[2], top);
      gfx_colour (is_black (pitch) ? 0x202124 : 0xE4E4E8);
      gfx_rect (ox, bot, ox + ED_LABEL_W - (is_black (pitch) ? 50 : 0), top);
      gfx_colour (0x8A8E98);
      gfx_rect (ox, bot, ox + ED_LABEL_W, bot + px);
      if (pitch % 12 == 0)
        gfx_text (ox + ED_LABEL_W - 12, bot + 4, gr_note_name (pitch, name), 0x303236, 0xE4E4E8, 2);
    }
  for (st = 0; st <= steps; st++)
    {
      int x = ox + ED_LABEL_W + st * sw;
      if (x < clip[0] || x > clip[2])
        continue;
      gfx_colour (st % bar_st == 0 ? C_GRID_BAR : (st % bar_st) % beat_st == 0 ? C_GRID : 0x33363B);
      gfx_rect (x, clip[1], x + px, clip[3] < oy - ED_HEAD_H ? clip[3] : oy - ED_HEAD_H);
    }
  if (play >= 0)
    {
      int x = ox + ED_LABEL_W + play * sw;
      gfx_colour (0x4A4E58);
      gfx_rect (x + px, clip[1], x + sw, clip[3] < oy - ED_HEAD_H ? clip[3] : oy - ED_HEAD_H);
    }
  for (k = 0; k < p->nnotes; k++)
    {
      const gr_note *n = &p->notes[k];
      int x0, x1, top;
      if (n->pitch > 127)
        continue;                 /* (ED_LOW..ED_HIGH is all of MIDI's notes) */
      x0 = ox + lay_roll_x (w, p->len, n->tick);
      x1 = ox + lay_roll_x (w, p->len, n->tick + n->len);
      top = oy + lay_roll_y (n->pitch);
      if (x1 < x0 + 6) x1 = x0 + 6;
      if (!gfx_visible (clip, x0, top - ED_KEY_H, x1, top))
        continue;
      /* Selected notes get a white edge; brighter means louder. */
      gfx_colour (is_sel (n) ? 0xFFFFFF : 0x1A1B1E);
      gfx_round (x0, top - ED_KEY_H + 1, x1, top - 1, 4);
      gfx_colour (vel_colour (n->vel));
      gfx_round (x0 + 2, top - ED_KEY_H + 3, x1 - 2, top - 3, 3);
    }
  if (band)
    {
      gfx_colour (0xFFFFFF);
      gfx_frame (ox + (band_x0 < band_x1 ? band_x0 : band_x1), oy + (band_y0 < band_y1 ? band_y0 : band_y1),
                 ox + (band_x0 < band_x1 ? band_x1 : band_x0), oy + (band_y0 < band_y1 ? band_y1 : band_y0));
    }
}

/* ---- the controller lane ---------------------------------------------------
   Pinned to the bottom of the window (drawn at the visible bottom whatever
   the scroll, like the header at the top). It shows one kind of controller
   event in the part (lane_type: a CC number or GR_CTL_*): a bar where each
   event is and its value held until the next. Select: a click puts one
   event, a drag draws them over time; Adjust: erases. */
#define LANE_RES (GR_STEP / 2)  /* drawing puts an event every 1/32 note */
#define LANE_BTN_X1 470         /* the controller's name: a button for its menu */
static int lane_type = 1;       /* Modulation to begin with */

int editor_lane_type (void) { return lane_type; }

void editor_set_lane (int type)
{
  if (type >= 0 && type < GR_CTL_TYPES)
    lane_type = type;
  if (app.edit_open)
    app_redraw_all (app.w_edit);
}

/* How many events of a type the part has (for the lane's menu). */
int editor_lane_count (int type)
{
  gr_pattern *p = pat ();
  int k, n = 0;
  if (p)
    for (k = 0; k < p->nctls; k++)
      n += p->ctls[k].type == type;
  return n;
}

static int lane_plot_y0 (int ybot) { return ybot + 12; }
static int lane_plot_y1 (int ybot) { return ybot + ED_LANE_H - ED_LANE_BAR - 8; }

static int lane_y (int ybot, int v)
{
  int lo = gr_ctl_min (lane_type), hi = gr_ctl_max (lane_type);
  return lane_plot_y0 (ybot) + (int) ((long) (v - lo) * (lane_plot_y1 (ybot) - lane_plot_y0 (ybot)) / (hi - lo));
}

static int lane_value (int ybot, int y)
{
  int lo = gr_ctl_min (lane_type), hi = gr_ctl_max (lane_type), h = lane_plot_y1 (ybot) - lane_plot_y0 (ybot);
  int v = lo + (int) ((long) (y - lane_plot_y0 (ybot)) * (hi - lo) / h);
  return v < lo ? lo : v > hi ? hi : v;
}

/* ox: the work area's origin on screen; ybot: the visible bottom. */
static void paint_lane (int ox, int ybot, const int *clip)
{
  gr_pattern *p = pat ();
  gr_track *t = trk ();
  int w = app.edit_width, y1 = ybot + ED_LANE_H, k, x;
  char label[48];
  if (!gfx_visible (clip, clip[0], ybot, clip[2], y1))
    return;
  gfx_colour (0x26282D);
  gfx_rect (clip[0], ybot, clip[2], y1);
  gfx_colour (0x34363B);
  gfx_rect (clip[0], y1 - ED_LANE_BAR, clip[2], y1);
  gfx_colour (0x4A4E58);
  gfx_rect (clip[0], y1 - 2, clip[2], y1);
  if (!p || !t)
    return;
  snprintf (label, sizeof label, "%s", gr_ctl_name (lane_type));
  gfx_colour (C_BUTTON);
  gfx_round (ox + 16, y1 - ED_LANE_BAR + 8, ox + LANE_BTN_X1, y1 - 8, 8);
  gfx_text (ox + 32, y1 - ED_LANE_BAR + 20, label, C_TEXT, C_BUTTON, 0);
  gfx_colour (C_TEXT);
  gfx_tri (ox + LANE_BTN_X1 - 40, y1 - 18, ox + LANE_BTN_X1 - 16, y1 - 18, ox + LANE_BTN_X1 - 28, y1 - 32);
  gfx_text (ox + LANE_BTN_X1 + 24, y1 - ED_LANE_BAR + 20,
            "Select: click for one event, drag to draw   Adjust: erase", C_TEXT_DIM, 0x34363B, 0);
  /* The plot: beat lines, the middle for a centred controller, the events. */
  {
    int bar_st, beat_st, st, steps = p->len / GR_STEP, sw = lay_step_w (w, p->len);
    meter_steps (&bar_st, &beat_st);
    for (st = 0; st <= steps; st += beat_st)
      {
        x = ox + ED_LABEL_W + st * sw;
        gfx_colour (st % bar_st ? 0x33363C : 0x44484F);
        gfx_rect (x, ybot + 4, x + 2, y1 - ED_LANE_BAR - 4);
      }
  }
  if (gr_ctl_min (lane_type) < 0 || lane_type == 10 || lane_type == 8)
    {
      int ym = lane_y (ybot, lane_type == GR_CTL_BEND ? 0 : 64);
      gfx_colour (0x3A3D44);
      gfx_rect (ox + ED_LABEL_W, ym, ox + ED_LABEL_W + lay_roll_x (w, p->len, p->len) - ED_LABEL_W, ym + 2);
    }
  {
    char v0[8], v1[8];
    snprintf (v1, sizeof v1, "%d", gr_ctl_max (lane_type));
    snprintf (v0, sizeof v0, "%d", gr_ctl_min (lane_type));
    gfx_text (ox + 16, lane_plot_y1 (ybot) - 20, v1, C_TEXT_DIM, 0x26282D, 0);
    gfx_text (ox + 16, lane_plot_y0 (ybot), v0, C_TEXT_DIM, 0x26282D, 0);
  }
  for (k = 0; k < p->nctls; k++)
    {
      const gr_ctl *c = &p->ctls[k];
      int j, nx, base, y, x0;
      unsigned col = t->type == TRK_DRUMS ? C_DRUMS : C_INST;
      if (c->type != lane_type)
        continue;
      for (j = k + 1; j < p->nctls && p->ctls[j].type != lane_type; j++)
        ;
      x0 = ox + lay_roll_x (w, p->len, c->tick);
      nx = ox + lay_roll_x (w, p->len, j < p->nctls ? p->ctls[j].tick : p->len);
      y = lane_y (ybot, c->value);
      base = lane_y (ybot, gr_ctl_min (lane_type) < 0 ? 0 : gr_ctl_min (lane_type));
      /* held until the next event: a line; the event itself: a bar */
      gfx_colour (0x6E7480);
      gfx_rect (x0, y - 1, nx, y + 1);
      gfx_colour (col);
      gfx_rect (x0, base < y ? base : y, x0 + 6, base < y ? y : base);
      gfx_rect (x0 - 2, y - 3, x0 + 8, y + 3);
      if (lane_type == GR_CTL_PROGRAM)
        {
          char n[8];
          snprintf (n, sizeof n, "%d", c->value);
          gfx_text (x0 + 10, y - 8, n, C_TEXT, 0x26282D, 0);
        }
    }
}

void editor_redraw (int *b)
{
  int more;
  _swix (Wimp_RedrawWindow, _IN (1) | _OUT (0), b, &more);
  while (more)
    {
      int ox = b[1] - b[5], oy = b[4] - b[6];
      gr_track *t = trk ();
      gfx_colour (C_BG);
      gfx_rect (b[7], b[8], b[9], b[10]);
      if (t && pat ())
        {
          if (t->type == TRK_DRUMS)
            paint_grid (ox, oy, &b[7]);
          else
            paint_roll (ox, oy, &b[7]);
        }
      paint_lane (ox, b[2], &b[7]);     /* pinned: at the visible bottom */
      paint_head (ox, b[4], &b[7]);     /* pinned: at the visible top */
      head_scroll = b[6];
      _swix (Wimp_GetRectangle, _IN (1) | _OUT (0), b, &more);
    }
}

/* ---- clicks and drags ----------------------------------------------------------- */

static int paint_row, paint_on, drag_note, live_pitch = -1;

static void to_work (int x, int y, int *wx, int *wy)
{
  int ox, oy;
  app_window_origin (app.w_edit, &ox, &oy);
  *wx = x - ox;
  *wy = y - oy;
}

static void edited (void)
{
  editor_changed ();
  app.unsaved = 1;
  app_redraw_all (app.w_edit);
  if (app.main_open)
    app_redraw_all (app.w_main);
  arrange_title ();
}

static void set_step (gr_pattern *p, int row, int step, int on, int vel)
{
  int k = step_note (p, step, drow[row]);
  if (on && k < 0)
    gr_pattern_add_note (p, step * GR_STEP, GR_STEP, drow[row], vel);
  else if (!on && k >= 0)
    gr_pattern_delete_note (p, k);
}

static void drag_paint (int x, int y, int done)
{
  gr_pattern *p = pat ();
  int wx, wy, row, step;
  (void) done;
  if (!p)
    return;
  to_work (x, y, &wx, &wy);
  wy = -ED_HEAD_H - paint_row * ED_DRUM_H - 4;   /* stay on the row we started on */
  if (lay_grid_hit (app.edit_width, p->len, wx, wy, &row, &step))
    {
      int had = step_note (p, step, drow[row]) >= 0;
      if (had != paint_on)
        {
          set_step (p, row, step, paint_on, 100);
          if (paint_on)
            gr_live_on (app.eng, reg ()->track, drow[row], 100);
          edited ();
        }
    }
}

static void drag_note_len (int x, int y, int done)
{
  gr_pattern *p = pat ();
  int wx, wy, end;
  (void) y;
  if (!p || drag_note < 0 || drag_note >= p->nnotes)
    return;
  to_work (x, y, &wx, &wy);
  end = (int) ((long long) (wx - ED_LABEL_W) * GR_STEP / lay_step_w (app.edit_width, p->len));
  end = (end / GR_STEP + 1) * GR_STEP;           /* to the end of the step under the pointer */
  if (end > p->len) end = p->len;
  if (end - p->notes[drag_note].tick < GR_STEP) end = p->notes[drag_note].tick + GR_STEP;
  if (end - p->notes[drag_note].tick != p->notes[drag_note].len)
    {
      p->notes[drag_note].len = (int16_t) (end - p->notes[drag_note].tick);
      edited ();
    }
  if (done && live_pitch >= 0)
    {
      gr_live_off (app.eng, reg ()->track, live_pitch);
      live_pitch = -1;
    }
}

/* Moving the selection: the pattern without it, and the selected notes as
   they were when the drag began. */
static gr_pattern move_base;
static gr_note move_notes[GR_MAX_NOTES];
static int move_n, move_tick, move_pitch, move_dt, move_dp;

/* Put the selected notes back moved by dt ticks and dp semitones. */
static void place_moved (gr_pattern *p, int dt, int dp)
{
  int i;
  *p = move_base;
  nsel = 0;
  for (i = 0; i < move_n; i++)
    {
      gr_note n = move_notes[i];
      n.tick = (int16_t) (n.tick + dt);
      n.pitch = (uint8_t) (n.pitch + dp);
      if (gr_pattern_add_note (p, n.tick, n.len, n.pitch, n.vel) >= 0)
        sel_add (&n);
    }
}

/* Take the selected notes out of p, ready to move them. */
static void lift_selection (gr_pattern *p)
{
  int k;
  move_n = 0;
  move_base = *p;
  move_base.nnotes = 0;
  for (k = 0; k < p->nnotes; k++)
    if (is_sel (&p->notes[k]))
      move_notes[move_n++] = p->notes[k];
    else
      move_base.notes[move_base.nnotes++] = p->notes[k];
}

/* How far the selection may move and stay in the pattern and the roll. */
static void clamp_move (const gr_pattern *p, int *dt, int *dp)
{
  int i;
  for (i = 0; i < move_n; i++)
    {
      const gr_note *n = &move_notes[i];
      if (n->tick + *dt < 0) *dt = -n->tick;
      if (n->tick + *dt > p->len - GR_STEP) *dt = p->len - GR_STEP - n->tick;
      if (n->pitch + *dp < ED_LOW) *dp = ED_LOW - n->pitch;
      if (n->pitch + *dp > ED_HIGH) *dp = ED_HIGH - n->pitch;
    }
}

static void drag_group_move (int x, int y, int done)
{
  gr_pattern *p = pat ();
  int wx, wy, pitch, tick, dt, dp;
  if (!p)
    return;
  to_work (x, y, &wx, &wy);
  if (lay_roll_hit (app.edit_width, p->len, wx > ED_LABEL_W ? wx : ED_LABEL_W, wy, &pitch, &tick))
    {
      dt = (tick - move_tick) / GR_STEP * GR_STEP;
      dp = pitch - move_pitch;
      clamp_move (p, &dt, &dp);
      if (dt != move_dt || dp != move_dp)
        {
          place_moved (p, dt, dp);
          if (dp != move_dp && live_pitch >= 0)
            {
              gr_live_off (app.eng, reg ()->track, live_pitch);
              live_pitch = move_pitch + dp;
              gr_live_on (app.eng, reg ()->track, live_pitch, 100);
            }
          move_dt = dt;
          move_dp = dp;
          edited ();
        }
    }
  if (done && live_pitch >= 0)
    {
      gr_live_off (app.eng, reg ()->track, live_pitch);
      live_pitch = -1;
    }
}

static void drag_band (int x, int y, int done)
{
  gr_pattern *p = pat ();
  int wx, wy;
  if (!p)
    return;
  to_work (x, y, &wx, &wy);
  band_x1 = wx;
  band_y1 = wy;
  if (done)
    {
      /* Every note that starts inside the box joins the selection. */
      int k, tx0, tx1, p0, p1, d;
      int bx0 = band_x0 < band_x1 ? band_x0 : band_x1, bx1 = band_x0 < band_x1 ? band_x1 : band_x0;
      int by0 = band_y0 < band_y1 ? band_y0 : band_y1, by1 = band_y0 < band_y1 ? band_y1 : band_y0;
      lay_roll_hit (app.edit_width, p->len, bx0 > ED_LABEL_W ? bx0 : ED_LABEL_W, by1, &p1, &tx0);
      lay_roll_hit (app.edit_width, p->len, bx1 > ED_LABEL_W ? bx1 : ED_LABEL_W, by0, &p0, &tx1);
      if (!lay_roll_hit (app.edit_width, p->len, ED_LABEL_W, by1, &p1, &d)) p1 = ED_HIGH;
      if (!lay_roll_hit (app.edit_width, p->len, ED_LABEL_W, by0, &p0, &d)) p0 = ED_LOW;
      for (k = 0; k < p->nnotes; k++)
        {
          const gr_note *n = &p->notes[k];
          if (n->pitch >= p0 && n->pitch <= p1 && n->tick >= tx0 / GR_STEP * GR_STEP && n->tick <= tx1)
            sel_add (n);
        }
      band = 0;
    }
  app_redraw_all (app.w_edit);
}

static void drag_key (int x, int y, int done)
{
  int wx, wy, pitch, tick;
  gr_pattern *p = pat ();
  if (!p)
    return;
  to_work (x, y, &wx, &wy);
  /* Slide along the keyboard strip: a glissando. */
  if (!done && lay_roll_hit (app.edit_width, p->len, 4, wy, &pitch, &tick) && pitch != live_pitch)
    {
      if (live_pitch >= 0)
        gr_live_off (app.eng, reg ()->track, live_pitch);
      gr_live_on (app.eng, reg ()->track, pitch, 100);
      live_pitch = pitch;
    }
  if (done && live_pitch >= 0)
    {
      gr_live_off (app.eng, reg ()->track, live_pitch);
      live_pitch = -1;
    }
}

static void head_click (int *b, int wx)
{
  gr_pattern *p;
  gr_track *t = trk ();
  int i;
  for (i = 0; i < EB_COUNT; i++)
    if (wx >= eb_x[i][0] && wx < eb_x[i][1])
      break;
  if (i == EB_COUNT || !t)
    return;
  if (i == EB_KEYS)
    {
      if (t->type == TRK_DRUMS)
        editor_all_keys (!all_keys);
      return;
    }
  if (i == EB_SOUND)
    {
      /* The Sound menu, under the button. */
      int ox, oy;
      app_window_origin (app.w_edit, &ox, &oy);
      app_sound_menu (ox + eb_x[EB_SOUND][0], b[1] - 24);
      return;
    }
  p = pat_for_edit ();
  if (!p)
    return;
  if (i == EB_CLEAR)
    p->nnotes = p->nctls = 0;
  else
    {
      int len = (i == EB_LEN1 ? 1 : i == EB_LEN2 ? 2 : 4) * region_bar (), k;
      /* Lengthening repeats what's there; shortening drops what's past the end. */
      if (len > p->len)
        {
          int old = p->len, n = p->nnotes, nc = p->nctls, rep;
          p->len = len;
          for (rep = old; rep < len; rep += old)
            {
              for (k = 0; k < n; k++)
                gr_pattern_add_note (p, p->notes[k].tick + rep, p->notes[k].len,
                                     p->notes[k].pitch, p->notes[k].vel);
              for (k = 0; k < nc; k++)
                gr_pattern_set_ctl (p, p->ctls[k].tick + rep, p->ctls[k].type, p->ctls[k].value);
            }
        }
      else
        {
          for (k = p->nnotes - 1; k >= 0; k--)
            if (p->notes[k].tick >= len)
              gr_pattern_delete_note (p, k);
          for (k = p->nctls - 1; k >= 0; k--)
            if (p->ctls[k].tick >= len)
              gr_pattern_delete_ctl (p, k);
          p->len = len;
        }
    }
  edited ();
}

/* The note sounding at (tick, pitch), or -1. */
static int note_at (const gr_pattern *p, int tick, int pitch)
{
  int k;
  for (k = p->nnotes - 1; k >= 0; k--)
    if (p->notes[k].pitch == pitch && tick >= p->notes[k].tick && tick < p->notes[k].tick + p->notes[k].len)
      return k;
  return -1;
}

static int shift_down (void)
{
  int r1 = 0;
  _swix (OS_Byte, _INR (0, 1) | _OUT (1), 121, 0x80, &r1);
  return r1 == 0xFF;
}

/* A point in the lane to a tick (on the 1/32 grid) and a value. */
static int lane_at (int wx, int y, int ybot, int *tick, int *value)
{
  gr_pattern *p = pat ();
  int t;
  if (!p || wx < ED_LABEL_W)
    return 0;
  t = (int) ((long long) (wx - ED_LABEL_W) * GR_STEP / lay_step_w (app.edit_width, p->len));
  if (t < 0 || t >= p->len)
    return 0;
  *tick = t / LANE_RES * LANE_RES;
  *value = lane_value (ybot, y);
  return 1;
}

static int lane_erasing, lane_last_tick, lane_last_value, lane_ybot;

/* Erase this type's events in [t0, t1]. */
static void lane_erase (gr_pattern *p, int t0, int t1)
{
  int k;
  if (t0 > t1) { int z = t0; t0 = t1; t1 = z; }
  for (k = p->nctls - 1; k >= 0; k--)
    if (p->ctls[k].type == lane_type && p->ctls[k].tick >= t0 && p->ctls[k].tick < t1 + LANE_RES)
      gr_pattern_delete_ctl (p, k);
}

/* Draw from the last point to this one: an event every 1/32 note, the
   values on the straight line between. */
static void lane_line (gr_pattern *p, int t1, int v1)
{
  int t0 = lane_last_tick, v0 = lane_last_value, t, step = t1 >= t0 ? LANE_RES : -LANE_RES;
  for (t = t0; ; t += step)
    {
      int v = t1 == t0 ? v1 : v0 + (int) ((long) (v1 - v0) * (t - t0) / (t1 - t0));
      if (gr_pattern_set_ctl (p, t, lane_type, v) < 0)
        break;                      /* (full) */
      if (t == t1)
        break;
    }
}

static void lane_drag (int x, int y, int done)
{
  gr_pattern *p = pat ();
  int wx, wy, tick, value;
  (void) done;
  if (!p)
    return;
  to_work (x, y, &wx, &wy);
  if (wx < ED_LABEL_W)
    wx = ED_LABEL_W;
  if (y < lane_ybot) y = lane_ybot;
  if (!lane_at (wx, y, lane_ybot, &tick, &value))
    {
      tick = p->len - 1;          /* (dragged past the end) */
      tick = tick / LANE_RES * LANE_RES;
      value = lane_value (lane_ybot, y);
    }
  if (tick == lane_last_tick && value == lane_last_value)
    return;
  if (lane_erasing)
    lane_erase (p, lane_last_tick, tick);
  else
    lane_line (p, tick, value);
  lane_last_tick = tick;
  lane_last_value = value;
  edited ();
}

static void lane_click (int *b, int wx, int ybot)
{
  gr_pattern *p;
  int tick, value;
  if (!trk () || !pat ())
    return;
  if (b[1] >= ybot + ED_LANE_H - ED_LANE_BAR)
    {
      /* The top strip: the controller's name opens the menu of them. */
      if (wx >= 16 && wx < LANE_BTN_X1)
        {
          int ox, oy;
          app_window_origin (app.w_edit, &ox, &oy);
          app_lane_menu (ox + 16, ybot + ED_LANE_H);
        }
      return;
    }
  if (!lane_at (wx, b[1], ybot, &tick, &value) || !(p = pat_for_edit ()))
    return;
  lane_erasing = b[2] == BUT_ADJUST;
  if (lane_erasing)
    lane_erase (p, tick, tick);
  else
    gr_pattern_set_ctl (p, tick, lane_type, value);
  lane_last_tick = tick;
  lane_last_value = value;
  lane_ybot = ybot;
  edited ();
  app_start_drag (app.w_edit, lane_drag);
}

void editor_click (int *b)
{
  gr_pattern *p;
  gr_track *t = trk ();
  int wx, wy;
  if (b[2] == BUT_MENU)
    {
      app_main_menu (b[0], b[1]);
      return;
    }
  to_work (b[0], b[1], &wx, &wy);
  {
    int st[10];
    st[0] = app.w_edit;
    _swix (Wimp_GetWindowState, _IN (1), st);
    if (b[1] >= st[4] - ED_HEAD_H)   /* the pinned header, at the visible top */
      {
        head_click (b, wx);
        return;
      }
    if (b[1] < st[2] + ED_LANE_H)    /* the pinned controller lane, at the bottom */
      {
        lane_click (b, wx, st[2]);
        return;
      }
  }
  if (wy > -ED_HEAD_H)
    {
      head_click (b, wx);
      return;
    }
  if (!t || !pat ())
    return;
  if (t->type == TRK_DRUMS)
    {
      int row, step;
      if (wx < ED_LABEL_W)
        {
          row = (-ED_HEAD_H - wy) / ED_DRUM_H;
          if (row >= 0 && row < ndrow)
            gr_live_on (app.eng, reg ()->track, drow[row], 110);   /* audition the sound */
          return;
        }
      p = pat_for_edit ();
      if (!lay_grid_hit (app.edit_width, p->len, wx, wy, &row, &step))
        return;
      if (b[2] == BUT_ADJUST)
        {
          int k = step_note (p, step, drow[row]);
          if (k >= 0)
            p->notes[k].vel = p->notes[k].vel >= 110 ? 80 : 127;
          else
            set_step (p, row, step, 1, 127);
          gr_live_on (app.eng, reg ()->track, drow[row], k >= 0 ? p->notes[k].vel : 127);
          edited ();
          return;
        }
      paint_row = row;
      paint_on = step_note (p, step, drow[row]) < 0;
      set_step (p, row, step, paint_on, 100);
      if (paint_on)
        gr_live_on (app.eng, reg ()->track, drow[row], 100);
      edited ();
      app_start_drag (app.w_edit, drag_paint);
    }
  else
    {
      int pitch, tick, k;
      p = pat ();
      if (!lay_roll_hit (app.edit_width, p->len, wx, wy, &pitch, &tick))
        return;
      if (tick < 0)               /* the keyboard strip: play it */
        {
          live_pitch = -1;
          drag_key (b[0], b[1], 0);
          app_start_drag (app.w_edit, drag_key);
          return;
        }
      p = pat_for_edit ();
      k = note_at (p, tick, pitch);
      if (b[2] == BUT_ADJUST)
        {
          if (k >= 0)
            {
              sel_remove (&p->notes[k]);
              gr_pattern_delete_note (p, k);
              edited ();
            }
          return;
        }
      if (shift_down ())
        {
          if (k >= 0)             /* Shift-click: in or out of the selection */
            {
              if (is_sel (&p->notes[k]))
                sel_remove (&p->notes[k]);
              else
                sel_add (&p->notes[k]);
              app_redraw_all (app.w_edit);
              return;
            }
          band = 1;               /* Shift-drag on empty space: a selection box */
          band_x0 = band_x1 = wx;
          band_y0 = band_y1 = wy;
          app_start_drag (app.w_edit, drag_band);
          return;
        }
      if (k >= 0)                 /* grab a note: it (or the selection it's in) moves */
        {
          if (!is_sel (&p->notes[k]))
            {
              nsel = 0;
              sel_add (&p->notes[k]);
            }
          gr_live_on (app.eng, reg ()->track, pitch, p->notes[k].vel);
          live_pitch = pitch;
          lift_selection (p);
          move_tick = tick / GR_STEP * GR_STEP;
          move_pitch = pitch;
          move_dt = move_dp = 0;
          app_redraw_all (app.w_edit);
          app_start_drag (app.w_edit, drag_group_move);
          return;
        }
      /* Draw a new note: a step long, then drag to lengthen it. */
      nsel = 0;
      drag_note = gr_pattern_add_note (p, tick / GR_STEP * GR_STEP, GR_STEP, pitch, 100);
      if (drag_note < 0)
        return;
      sel_add (&p->notes[drag_note]);
      gr_live_on (app.eng, reg ()->track, pitch, 100);
      live_pitch = pitch;
      edited ();
      app_start_drag (app.w_edit, drag_note_len);
    }
}

/* ---- keys in the piano roll ------------------------------------------------------ */

/* Change every selected note: transpose by dp, velocity by dv. */
static void change_selection (int dp, int dv)
{
  gr_pattern *p = pat_for_edit ();
  int i, dt = 0;
  if (!p || !nsel)
    return;
  lift_selection (p);
  clamp_move (p, &dt, &dp);
  for (i = 0; i < move_n; i++)
    {
      int v = move_notes[i].vel + dv;
      move_notes[i].vel = (uint8_t) (v < 1 ? 1 : v > 127 ? 127 : v);
    }
  place_moved (p, 0, dp);
  if (dp && move_n)
    gr_live_on (app.eng, reg ()->track, move_notes[0].pitch + dp, move_notes[0].vel);
  edited ();
}

/* Keys for the piano roll while the editor has the input focus. Returns 1
   if it used the key. */
int editor_key (int code)
{
  gr_track *t = trk ();
  gr_pattern *p = pat ();
  int k;
  if (!t || !p || t->type != TRK_INST)
    return 0;
  switch (code)
    {
    case 0x01:                    /* Ctrl-A: select all */
      for (k = 0; k < p->nnotes; k++)
        sel_add (&p->notes[k]);
      app_redraw_all (app.w_edit);
      return 1;
    case 0x1B:                    /* Escape: select none */
      if (!nsel)
        return 0;
      nsel = 0;
      app_redraw_all (app.w_edit);
      return 1;
    case 0x16:                    /* Ctrl-V: paste notes, at the playhead if it's in this region */
      {
        gr_region *r = reg ();
        int at = 0, pos = (int) gr_position (app.eng), i;
        if (!note_clip_n)
          return 0;
        if (pos >= r->start && pos < r->start + r->len)
          at = (pos - r->start) % p->len / GR_STEP * GR_STEP;
        p = pat_for_edit ();
        nsel = 0;
        for (i = 0; i < note_clip_n; i++)
          {
            gr_note n = note_clip[i];
            n.tick = (int16_t) (n.tick + at);
            if (gr_pattern_add_note (p, n.tick, n.len, n.pitch, n.vel) >= 0)
              sel_add (&n);
          }
        edited ();
        return 1;
      }
    }
  if (!nsel)
    return 0;
  switch (code)
    {
    case 0x7F: case 8:            /* Delete: the selected notes */
      p = pat_for_edit ();
      for (k = p->nnotes - 1; k >= 0; k--)
        if (is_sel (&p->notes[k]))
          gr_pattern_delete_note (p, k);
      nsel = 0;
      edited ();
      return 1;
    case 0x03:                    /* Ctrl-C: copy them, from the first one's start */
      {
        int first = p->len;
        note_clip_n = 0;
        for (k = 0; k < p->nnotes; k++)
          if (is_sel (&p->notes[k]) && p->notes[k].tick < first)
            first = p->notes[k].tick;
        for (k = 0; k < p->nnotes; k++)
          if (is_sel (&p->notes[k]))
            {
              note_clip[note_clip_n] = p->notes[k];
              note_clip[note_clip_n++].tick = (int16_t) (p->notes[k].tick - first);
            }
        return 1;
      }
    case 0x18F: change_selection (1, 0); return 1;       /* Up: a semitone up */
    case 0x18E: change_selection (-1, 0); return 1;      /* Down */
    case 0x19F: change_selection (12, 0); return 1;      /* Shift-Up: an octave */
    case 0x19E: change_selection (-12, 0); return 1;
    case 0x1AF: change_selection (0, 10); return 1;      /* Ctrl-Up: louder */
    case 0x1AE: change_selection (0, -10); return 1;     /* Ctrl-Down: softer */
    }
  return 0;
}

/* Move the playing-step highlight. */
void editor_tick (void)
{
  gr_pattern *p = pat ();
  int st;
  if (!app.edit_open || !p)
    return;
  st = local_step ();
  if (st != last_step)
    {
      int sw = lay_step_w (app.edit_width, p->len), h = -lay_editor_height (trk ()->type);
      if (last_step >= 0)
        app_redraw_work (app.w_edit, ED_LABEL_W + last_step * sw, h, ED_LABEL_W + (last_step + 1) * sw + 2, -ED_HEAD_H);
      if (st >= 0)
        app_redraw_work (app.w_edit, ED_LABEL_W + st * sw, h, ED_LABEL_W + (st + 1) * sw + 2, -ED_HEAD_H);
      last_step = st;
    }
}

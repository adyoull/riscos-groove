/* Groove - start-up, the poll loop, menus and messages.

   The poll loop is where "responsive" is won or lost on a cooperative
   desktop. While anything is sounding (or a drag or a held key is in
   progress) we ask for null events with Wimp_PollIdle every 2 cs and top up
   the sound ring each time we get control - after *every* event, not just
   nulls. When nothing is sounding we mask null events entirely and use no
   CPU at all. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel.h>
#include <swis.h>
#include "app.h"
#include "gfx.h"

app_state app;
const _kernel_oserror *app_create_error;

static int quit;
static int iconbar_icon = -1;

/* ---- small helpers -------------------------------------------------------- */

void app_error (const char *msg)
{
  _kernel_oserror e;
  e.errnum = 0;
  snprintf (e.errmess, sizeof e.errmess, "%s", msg);
  _swix (Wimp_ReportError, _INR (0, 2), &e, 1, APP_NAME);
}

void app_check (const _kernel_oserror *e)
{
  if (e)
    app_error (e->errmess);
}

static int ask (const char *msg)          /* 1 = OK, 0 = Cancel */
{
  _kernel_oserror e;
  int r = 2;
  e.errnum = 0;
  snprintf (e.errmess, sizeof e.errmess, "%s", msg);
  _swix (Wimp_ReportError, _INR (0, 2) | _OUT (1), &e, 3, APP_NAME, &r);
  return r == 1;
}

void app_window_origin (int window, int *ox, int *oy)
{
  int b[9];
  b[0] = window;
  if (_swix (Wimp_GetWindowState, _IN (1), b))
    {
      *ox = *oy = 0;
      return;
    }
  *ox = b[1] - b[5];
  *oy = b[4] - b[6];
}

void app_pointer (int *x, int *y, int *buttons, int *window, int *icon)
{
  int b[5] = { 0, 0, 0, -1, -1 };
  _swix (Wimp_GetPointerInfo, _IN (1), b);
  if (x) *x = b[0];
  if (y) *y = b[1];
  if (buttons) *buttons = b[2];
  if (window) *window = b[3];
  if (icon) *icon = b[4];
}

void app_redraw_all (int window)
{
  _swix (Wimp_ForceRedraw, _INR (0, 4), window, -0x1000000, -0x1000000, 0x1000000, 0x1000000);
}

void app_redraw_work (int window, int x0, int y0, int x1, int y1)
{
  _swix (Wimp_ForceRedraw, _INR (0, 4), window, x0, y0, x1, y1);
}

void app_set_extent (int window, int w, int h)
{
  int box[4] = { 0, -h, w, 0 };
  _swix (Wimp_SetExtent, _INR (0, 1), window, box);
}

/* Open a window on top. The first time, at its place in the workspace
   (sized from the screen); after that where the user last left it (the
   Wimp keeps a closed window's state). */
void app_open_window (int window, int which)
{
  static int placed[4];
  int b[10];
  b[0] = window;
  _swix (Wimp_GetWindowState, _IN (1), b);
  if (!(b[8] & (1 << 16)) && !placed[which])
    {
      lay_box w = lay_workspace (app.screen_w, app.screen_h, which);
      b[1] = w.x0; b[2] = w.y0; b[3] = w.x1; b[4] = w.y1;
      b[5] = 0; b[6] = 0;
      placed[which] = 1;
    }
  b[7] = -1;
  if (window == app.w_main)
    arrange_open_request (b);
  else
    app_check (_swix (Wimp_OpenWindow, _IN (1), b));
}

/* A 16th track with a SoundFont instrument needs the second SoundFont
   player (MIDI has 15 channels that aren't for drums): load it when the
   song first needs it, here rather than while playing. */
static void gm_second_check (void)
{
  static int failed;
  if (failed || !gr_gm_ready () || gr_gm_second_ready () || !gr_gm_need_second (&app.song))
    return;
  _swix (Hourglass_On, 0);
  if (gr_gm_load_second ())
    {
      char msg[252];
      failed = 1;
      _swix (Hourglass_Off, 0);
      snprintf (msg, sizeof msg, "Groove couldn't load the SoundFont again for the 16th track (%.80s), "
                "so that track plays Groove's own nearest sound.", gr_gm_error ());
      app_error (msg);
      return;
    }
  _swix (Hourglass_Off, 0);
}

void app_changed (void)
{
  app.unsaved = 1;
  gm_second_check ();
  if (app.main_open)
    {
      arrange_sync_hscroll ();       /* the song's length (the scroll bar) and its picture */
      app_redraw_all (app.w_tool);   /* the tempo and key (a song loaded, a MIDI file's tempo) */
    }
  editor_changed ();
  if (app.edit_open)
    app_redraw_all (app.w_edit);
  arrange_title ();
}

void app_start_drag (int window, drag_fn fn)
{
  app.drag = fn;
  app.drag_window = window;
}

static void read_eigen (void)
{
  int xl = 1279, yl = 1023;
  _swix (OS_ReadModeVariable, _INR (0, 1) | _OUT (2), -1, 4, &app.xeig);
  _swix (OS_ReadModeVariable, _INR (0, 1) | _OUT (2), -1, 5, &app.yeig);
  _swix (OS_ReadModeVariable, _INR (0, 1) | _OUT (2), -1, 11, &xl);   /* XWindLimit */
  _swix (OS_ReadModeVariable, _INR (0, 1) | _OUT (2), -1, 12, &yl);   /* YWindLimit */
  app.screen_w = (xl + 1) << app.xeig;
  app.screen_h = (yl + 1) << app.yeig;
  /* Drum rows sized for the editor's place in the workspace. */
  {
    lay_box e = lay_workspace (app.screen_w, app.screen_h, WS_EDITOR);
    lay_drum_h = lay_fit_drum_rows (e.y1 - e.y0);
  }
}

/* ---- menus ----------------------------------------------------------------- */

/* A menu is built into an int array: the header (title, colours, item
   width, height, gap) then 6-word items (flags, submenu, icon flags, then
   indirected text: pointer, validation, length). PRM Wimp_CreateMenu. */
#define MENU_WORDS(n) (7 + 6 * (n))
#define MI_TICK   1
#define MI_DOTTED 2
#define MI_WRITABLE 4
#define MI_LAST   0x80
#define MI_SHADED (1 << 22)

typedef struct { int *m; int n, maxlen; } menu_builder;

static void menu_begin (menu_builder *b, int *m, const char *title)
{
  b->m = m;
  b->n = 0;
  b->maxlen = (int) strlen (title);
  memset (m, 0, 28);
  strncpy ((char *) m, title, 12);
  ((unsigned char *) m)[12] = 7;  ((unsigned char *) m)[13] = 2;
  ((unsigned char *) m)[14] = 7;  ((unsigned char *) m)[15] = 0;
  m[5] = 44;                      /* item height */
  m[6] = 0;
}

static void menu_add (menu_builder *b, const char *text, int flags, void *sub, int shaded)
{
  int *it = b->m + 7 + 6 * b->n;
  it[0] = flags;
  it[1] = sub ? (int) sub : -1;
  /* Text, filled, indirected; black on white; shaded if asked. */
  it[2] = 0x07000121 | (shaded ? MI_SHADED : 0);
  it[3] = (int) text;
  it[4] = -1;
  it[5] = (int) strlen (text) + 1;
  if ((int) strlen (text) > b->maxlen)
    b->maxlen = (int) strlen (text);
  b->n++;
}

static void menu_end (menu_builder *b)
{
  b->m[7 + 6 * (b->n - 1)] |= MI_LAST;
  b->m[4] = (b->maxlen + 2) * 16;   /* item width: system font is 16 units */
}

enum { MENU_NONE, MENU_ICONBAR, MENU_MAIN, MENU_SOUND, MENU_LANE };
/* The controller lane's menu: the common controllers, and any CC by number. */
static const int lane_types[] = { 1, 2, 7, 10, 11, 64, 71, 74, 91, 93, GR_CTL_BEND, GR_CTL_PRESSURE, GR_CTL_PROGRAM };
#define NLANE ((int) (sizeof lane_types / sizeof lane_types[0]))
static int m_lane[7 + 6 * (NLANE + 1)], m_lane_cc[7 + 6];
static char lane_text[NLANE][40], lane_cc_buf[4];
static int open_menu = MENU_NONE, menu_x, menu_y;
static int m_iconbar[MENU_WORDS (7)];
static int m_midi[MENU_WORDS (7)], m_midiout[MENU_WORDS (6)];
static int m_main[MENU_WORDS (15)], m_song[MENU_WORDS (8)], m_key[MENU_WORDS (12)];
/* Song > Time signature: from the bar the playhead is in. */
static const int meters[][2] = { { 2, 4 }, { 3, 4 }, { 4, 4 }, { 5, 4 }, { 6, 8 }, { 7, 8 }, { 9, 8 }, { 12, 8 } };
#define NMETERS ((int) (sizeof meters / sizeof meters[0]))
static int m_meter[MENU_WORDS (NMETERS)];
static char meter_text[NMETERS][8];
static int m_rename[MENU_WORDS (1)];
static char rename_buf[GR_NAME];
/* Track > MIDI channel, and Bank & program: numbers typed in (blank: auto). */
static int m_chan[MENU_WORDS (17)], m_bank[MENU_WORDS (3)], m_num[3][MENU_WORDS (1)];
static char num_buf[3][4];
static const char *const chan_names[17] =
  { "Auto", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15", "16" };
static int m_track[MENU_WORDS (9)], m_sound[MENU_WORDS (PATCH_COUNT + 1)], m_region[MENU_WORDS (6)];
static int m_gm[MENU_WORDS (16)], m_gm_prog[16][MENU_WORDS (8)], m_gm_kits[MENU_WORDS (GM_NKITS)];
static const char *const key_names[12] =
  { "C", "C#/Db", "D", "D#/Eb", "E", "F", "F#/Gb", "G", "G#/Ab", "A", "A#/Bb", "B" };

/* The SoundFont !SoftMIDI names (its !Boot sets the variable when the
   Filer first sees it), or NULL. */
static const char *gm_variable (void)
{
  const char *v = getenv ("SoftMIDI$SoundFont");
  if (!v || !*v)
    v = getenv ("MIDISynth$SoundFont");      /* !SoftMIDI's name before 0.5.0 */
  return v && *v ? v : NULL;
}

/* Is there a SoundFont to be had? */
static int gm_available (void)
{
  return gr_gm_ready () || gm_variable ();
}

/* Instrument tracks: Groove's sounds, then "SoundFont" > family > sound.
   Drum tracks: Groove's kits, then "SoundFont kits" > kit. */
static void build_sound_menu (void)
{
  menu_builder b;
  int i, f, t = app.sel_track, has_t = t >= 0 && t < app.song.ntracks;
  const gr_track *tr = has_t ? &app.song.tracks[t] : NULL;
  int drums = tr && tr->type == TRK_DRUMS, gm = tr ? tr->gm : -1, avail = gm_available ();
  menu_begin (&b, m_gm_kits, "Kits");
  for (i = 0; i < GM_NKITS; i++)
    menu_add (&b, gr_gm_kit_name (gr_gm_kit (i)), drums && gm == gr_gm_kit (i) ? MI_TICK : 0, NULL, 0);
  menu_end (&b);
  for (f = 0; f < 16; f++)
    {
      menu_begin (&b, m_gm_prog[f], "Sound");
      for (i = 0; i < 8; i++)
        menu_add (&b, gr_gm_program_name (f * 8 + i), !drums && gm == f * 8 + i ? MI_TICK : 0, NULL, 0);
      menu_end (&b);
    }
  menu_begin (&b, m_gm, "SoundFont");
  for (f = 0; f < 16; f++)
    menu_add (&b, gr_gm_family_name (f), !drums && gm >= f * 8 && gm < f * 8 + 8 ? MI_TICK : 0,
              m_gm_prog[f], 0);
  menu_end (&b);

  menu_begin (&b, m_sound, drums ? "Kit" : "Instrument");
  if (drums)
    for (i = 0; i < KIT_COUNT; i++)
      menu_add (&b, gr_kit_name (i), gm < 0 && tr->patch == i ? MI_TICK : 0,
                NULL, 0);
  else
    for (i = 0; i < PATCH_COUNT; i++)
      menu_add (&b, gr_patch_name (i), has_t && gm < 0 && tr->patch == i ? MI_TICK : 0, NULL,
                !has_t || tr->type != TRK_INST);
  b.m[7 + 6 * (b.n - 1)] |= MI_DOTTED;
  menu_add (&b, avail ? (drums ? "SoundFont kits" : "SoundFont") : "SoundFont (needs !SoftMIDI)",
            gm >= 0 ? MI_TICK : 0, drums ? m_gm_kits : m_gm, !avail || !has_t || tr->type == TRK_AUDIO);
  menu_end (&b);
}

int app_gm_ensure (void)
{
  char msg[252];                /* a RISC OS error block's text */
  if (gr_gm_ready ())
    return 1;
  _swix (Hourglass_On, 0);
  if (gm_variable () && gr_gm_load (gm_variable ()) == 0)
    {
      _swix (Hourglass_Off, 0);
      gm_second_check ();
      return 1;
    }
  _swix (Hourglass_Off, 0);
  snprintf (msg, sizeof msg, "Groove couldn't load a SoundFont (%.80s). SoundFont sounds come "
            "from !SoftMIDI: install it and open its folder. Until then Groove's own sounds play.",
            gm_variable () ? gr_gm_error () : "SoftMIDI$SoundFont isn't set");
  app_error (msg);
  return 0;
}

/* A choice from the Sound menu: sel[0] is the item in it. */
static void sound_chosen (const int *sel)
{
  int t = app.sel_track, n, gm = -1, named_by_sound;
  gr_track *tr;
  if (t < 0 || t >= app.song.ntracks || sel[0] < 0)
    return;
  tr = &app.song.tracks[t];
  /* The track is named after its sound, unless you've given it a name of your own. */
  {
    char was[GR_NAME];
    snprintf (was, sizeof was, "%s", gr_sound_name (tr));
    named_by_sound = !strcmp (tr->name, was);
  }
  n = tr->type == TRK_DRUMS ? KIT_COUNT : PATCH_COUNT;
  if (sel[0] < n)
    {
      tr->patch = sel[0];
      tr->gm = -1;
      if (tr->type == TRK_INST && named_by_sound)
        snprintf (tr->name, GR_NAME, "%s", gr_patch_name (sel[0]));
    }
  else
    {
      if (tr->type == TRK_DRUMS && sel[1] >= 0)
        gm = gr_gm_kit (sel[1]);
      else if (tr->type == TRK_INST && sel[1] >= 0 && sel[2] >= 0)
        gm = sel[1] * 8 + sel[2];
      if (gm < 0 || !app_gm_ensure ())
        return;
      tr->gm = gm;
      if (tr->type == TRK_INST && named_by_sound)
        snprintf (tr->name, GR_NAME, "%s", gr_gm_program_name (gm));
    }
  app_changed ();
}

void app_sound_menu (int x, int y)
{
  build_sound_menu ();
  open_menu = MENU_SOUND;
  menu_x = x;
  menu_y = y;
  app_check (_swix (Wimp_CreateMenu, _INR (1, 3), m_sound, menu_x, menu_y));
}

static void build_lane_menu (void)
{
  menu_builder b;
  int i, cur = editor_lane_type (), other = 1;
  menu_begin (&b, m_lane, "Controller");
  for (i = 0; i < NLANE; i++)
    {
      int n = editor_lane_count (lane_types[i]);
      if (n)
        snprintf (lane_text[i], sizeof lane_text[i], "%s  (%d)", gr_ctl_name (lane_types[i]), n);
      else
        snprintf (lane_text[i], sizeof lane_text[i], "%s", gr_ctl_name (lane_types[i]));
      menu_add (&b, lane_text[i], (cur == lane_types[i] ? MI_TICK : 0)
                | (lane_types[i] == 93 || lane_types[i] == GR_CTL_PROGRAM ? MI_DOTTED : 0), NULL, 0);
      if (cur == lane_types[i])
        other = 0;
    }
  menu_add (&b, "Other CC", other ? MI_TICK : 0, m_lane_cc, 0);
  menu_end (&b);
  if (other && cur < 128)
    snprintf (lane_cc_buf, sizeof lane_cc_buf, "%d", cur & 127);
  else
    lane_cc_buf[0] = 0;
  menu_begin (&b, m_lane_cc, "CC number");
  menu_add (&b, lane_cc_buf, MI_WRITABLE, NULL, 0);
  b.m[7 + 3] = (int) lane_cc_buf;
  b.m[7 + 4] = (int) "A0-9";
  b.m[7 + 5] = sizeof lane_cc_buf;
  b.maxlen = 10;
  menu_end (&b);
}

void app_lane_menu (int x, int y)
{
  build_lane_menu ();
  open_menu = MENU_LANE;
  menu_x = x;
  menu_y = y;
  app_check (_swix (Wimp_CreateMenu, _INR (1, 3), m_lane, menu_x, menu_y));
}

/* Play on MIDI out: drum and instrument tracks can be; audio tracks can't. */
static int song_has_notes_tracks (void)
{
  int t;
  for (t = 0; t < app.song.ntracks; t++)
    if (app.song.tracks[t].type != TRK_AUDIO)
      return 1;
  return 0;
}

/* Song > Play on MIDI out is ticked when every drum and instrument track is. */
static int song_midi_out (void)
{
  int t;
  for (t = 0; t < app.song.ntracks; t++)
    if (app.song.tracks[t].type != TRK_AUDIO && !app.song.tracks[t].midi_out)
      return 0;
  return song_has_notes_tracks ();
}

static void midi_out_changed (void)
{
  int t;
  for (t = 0; t < app.song.ntracks; t++)
    if (app.song.tracks[t].midi_out && app.song.tracks[t].type != TRK_AUDIO)
      break;
  if (t < app.song.ntracks && !midiout_on ())
    midiout_cycle ();           /* (somewhere to send it: the first that's there) */
  app_changed ();
}

/* Track > Play on MIDI out (F11): the selected track, on or off. */
void app_track_midi_out (void)
{
  int t = app.sel_track;
  if (t < 0 || t >= app.song.ntracks || app.song.tracks[t].type == TRK_AUDIO)
    return;
  midiout_reset ();             /* (notes off, and the channels set up again) */
  app.song.tracks[t].midi_out = !app.song.tracks[t].midi_out;
  midi_out_changed ();
}

/* Song > Play on MIDI out (Shift-F11): every drum and instrument track on,
   or, if they all are, all off. */
void app_song_midi_out (void)
{
  int t, on;
  if (!song_has_notes_tracks ())
    return;
  on = !song_midi_out ();
  midiout_reset ();
  for (t = 0; t < app.song.ntracks; t++)
    if (app.song.tracks[t].type != TRK_AUDIO)
      app.song.tracks[t].midi_out = on;
  midi_out_changed ();
}

static void build_main_menu (void)
{
  menu_builder b;
  int i, t = app.sel_track, has_t = t >= 0 && t < app.song.ntracks;
  int has_r = app.sel_region >= 0 && app.song.regions[app.sel_region].used;

  menu_begin (&b, m_key, "Key");
  for (i = 0; i < 12; i++)
    menu_add (&b, key_names[i], app.song.key == i ? MI_TICK : 0, NULL, 0);
  menu_end (&b);

  {
    const gr_meter *now = gr_meter_at (&app.song, gr_tick_bar (&app.song, (int) gr_position (app.eng)));
    menu_begin (&b, m_meter, "Time sig");
    for (i = 0; i < NMETERS; i++)
      {
        snprintf (meter_text[i], sizeof meter_text[i], "%d/%d", meters[i][0], meters[i][1]);
        menu_add (&b, meter_text[i], now->num == meters[i][0] && now->den == meters[i][1] ? MI_TICK : 0, NULL, 0);
      }
    menu_end (&b);
  }

  menu_begin (&b, m_song, "Song");
  menu_add (&b, "New", MI_DOTTED, NULL, 0);
  menu_add (&b, "Save...", 0, NULL, 0);
  menu_add (&b, "Export as WAV...", 0, NULL, 0);
  menu_add (&b, "Export as MIDI...", MI_DOTTED, NULL, 0);
  menu_add (&b, "Key", 0, m_key, 0);
  menu_add (&b, "Time signature", MI_DOTTED, m_meter, 0);
  menu_add (&b, "Play on MIDI out  \x8B" "F11", (song_midi_out () ? MI_TICK : 0) | MI_DOTTED, NULL, !song_has_notes_tracks ());
  menu_add (&b, "Import WAV, MIDI, Maestro: drag in", 0, NULL, 1);
  menu_end (&b);

  build_sound_menu ();

  /* Rename: a writable item holding the name; Return renames. */
  snprintf (rename_buf, sizeof rename_buf, "%s", has_t ? app.song.tracks[t].name : "");
  menu_begin (&b, m_rename, "Name");
  menu_add (&b, rename_buf, MI_WRITABLE, NULL, 0);
  b.m[7 + 3] = (int) rename_buf;
  b.m[7 + 5] = sizeof rename_buf;
  b.maxlen = GR_NAME;
  menu_end (&b);

  menu_begin (&b, m_track, "Track");
  menu_add (&b, "New drummer track", 0, NULL, app.song.ntracks >= GR_MAX_TRACKS);
  menu_add (&b, "New instrument track", 0, NULL, app.song.ntracks >= GR_MAX_TRACKS);
  menu_add (&b, "New audio track", MI_DOTTED, NULL, app.song.ntracks >= GR_MAX_TRACKS);
  menu_add (&b, "Sound", 0, m_sound, !has_t || app.song.tracks[t].type == TRK_AUDIO);
  menu_add (&b, "Rename", MI_DOTTED, m_rename, !has_t);
  menu_add (&b, "Delete track", MI_DOTTED, NULL, !has_t);
  menu_add (&b, "Play on MIDI out  F11", has_t && app.song.tracks[t].midi_out ? MI_TICK : 0, NULL,
            !has_t || app.song.tracks[t].type == TRK_AUDIO);
  menu_add (&b, "MIDI channel", 0, m_chan, !has_t || app.song.tracks[t].type == TRK_AUDIO);
  menu_add (&b, "Bank & program", 0, m_bank, !has_t || app.song.tracks[t].type == TRK_AUDIO);
  menu_end (&b);

  /* The track's MIDI channel (for MIDI out and MIDI files): Auto, 1-16. */
  menu_begin (&b, m_chan, "Channel");
  for (i = 0; i < 17; i++)
    menu_add (&b, chan_names[i], (has_t && app.song.tracks[t].channel == i - 1 ? MI_TICK : 0) | (i == 0 ? MI_DOTTED : 0),
              NULL, 0);
  menu_end (&b);
  /* Bank Select MSB, LSB and program: each a number typed in, blank for
     automatic (no Bank Select; the sound's own program). */
  {
    static const char *const titles[3] = { "Bank MSB", "Bank LSB", "Program" };
    static const char *const items[3] = { "Bank MSB (CC 0)", "Bank LSB (CC 32)", "Program" };
    int k;
    menu_begin (&b, m_bank, "MIDI");
    for (k = 0; k < 3; k++)
      menu_add (&b, items[k], 0, m_num[k], 0);
    menu_end (&b);
    for (k = 0; k < 3; k++)
      {
        int v = !has_t ? -1 : k == 0 ? app.song.tracks[t].bank_msb : k == 1 ? app.song.tracks[t].bank_lsb
                : app.song.tracks[t].program;
        if (v >= 0 && v < 128)
          snprintf (num_buf[k], sizeof num_buf[k], "%d", v & 127);
        else
          num_buf[k][0] = 0;
        menu_begin (&b, m_num[k], titles[k]);
        menu_add (&b, num_buf[k], MI_WRITABLE, NULL, 0);
        b.m[7 + 3] = (int) num_buf[k];
        b.m[7 + 4] = (int) "A0-9";      /* digits only */
        b.m[7 + 5] = sizeof num_buf[k];
        b.maxlen = 10;
        menu_end (&b);
      }
  }

  menu_begin (&b, m_region, "Region");
  menu_add (&b, "Edit", 0, NULL, !has_r || app.song.regions[app.sel_region].clip >= 0);
  menu_add (&b, "Duplicate", 0, NULL, !has_r);
  menu_add (&b, "Delete", MI_DOTTED, NULL, !has_r);
  menu_add (&b, "Split at playhead  ^T", 0, NULL, !has_r);
  menu_add (&b, "Copy               ^C", 0, NULL, !has_r);
  menu_add (&b, "Paste              ^V", 0, NULL, !arrange_can_paste ());
  menu_end (&b);

  /* MIDI keyboard: MIDI Support (Groove's own driver there), or a MIDI
     module's ports. */
  {
    static const char *const names[7] = { "Off", "MIDI Support", "Any port", "Port 1", "Port 2", "Port 3", "Port 4" };
    static const int ports[7] = { MIDI_OFF, MIDI_SUPPORT, MIDI_ANY, 0, 1, 2, 3 };
    int p, ok, ms;
    midiin_forget_module ();      /* (they may have been loaded since we looked) */
    p = midiin_port ();
    ok = midiin_present ();
    ms = midiin_support ();
    menu_begin (&b, m_midi, "MIDI in");
    for (i = 0; i < 7; i++)
      menu_add (&b, names[i], (p == ports[i] ? MI_TICK : 0) | (i == 0 || i == 1 ? MI_DOTTED : 0), NULL,
                i == 1 ? !ms : i > 1 && !ok);
    menu_end (&b);
    /* MIDI out: where tracks with "Play on MIDI out" are sent. */
    p = midiout_port ();
    menu_begin (&b, m_midiout, "MIDI out");
    menu_add (&b, "Off", (p == MIDI_OFF ? MI_TICK : 0) | MI_DOTTED, NULL, 0);
    menu_add (&b, "MIDI Support", (p == MIDI_SUPPORT ? MI_TICK : 0) | MI_DOTTED, NULL, !ms);
    for (i = 0; i < 4; i++)
      menu_add (&b, names[3 + i], p == i ? MI_TICK : 0, NULL, !ok);
    menu_end (&b);
  }

  menu_begin (&b, m_main, APP_NAME);
  menu_add (&b, "Song", 0, m_song, 0);
  menu_add (&b, "Track", 0, m_track, 0);
  menu_add (&b, "Region", MI_DOTTED, m_region, 0);
  menu_add (&b, "Loop browser    F7", 0, NULL, 0);
  menu_add (&b, "Editor          F8", 0, NULL, 0);
  menu_add (&b, "Musical typing  F9", 0, NULL, 0);
  menu_add (&b, midiin_present () || midiin_support () ? "MIDI keyboard" : "MIDI keyboard (needs MIDI)",
            0, m_midi, 0);
  menu_add (&b, "MIDI out      F10", MI_DOTTED, m_midiout, 0);
  menu_add (&b, "Cycle", app.song.loop_on ? MI_TICK : 0, NULL, 0);
  menu_add (&b, "Metronome", app.song.metronome ? MI_TICK : 0, NULL, 0);
  menu_add (&b, "Count-in", app.count_in ? MI_TICK : 0, NULL, 0);
  menu_add (&b, "Merge recordings", app.merge ? MI_TICK : 0, NULL, 0);
  menu_add (&b, "Follow playhead", app.follow ? MI_TICK | MI_DOTTED : MI_DOTTED, NULL, 0);
  menu_add (&b, "Undo          ^Z", 0, NULL, !gr_history_can_undo (app.hist));
  menu_add (&b, "Redo          ^Y", 0, NULL, !gr_history_can_redo (app.hist));
  menu_end (&b);
}

void app_main_menu (int x, int y)
{
  build_main_menu ();
  open_menu = MENU_MAIN;
  menu_x = x - 64;
  menu_y = y;
  app_check (_swix (Wimp_CreateMenu, _INR (1, 3), m_main, menu_x, menu_y));
}

/* The Info box: the standard "About this program" window (Name, Purpose,
   Author, Version), opened by the Wimp as the Info item's submenu. */
static int info_window = -1;

static int info_create (void)
{
  static const char *const labels[4] = { "Name", "Purpose", "Author", "Version" };
  static const char *const values[4] = { "Groove", "Makes music: loops, beats and notes",
                                         "Andrew Youll", APP_VERSION " (" APP_DATE ")" };
  int b[22 + 8 * 8], i, w = 0;
  for (i = 0; i < 4; i++)
    if ((int) strlen (values[i]) * 16 + 24 > w)
      w = (int) strlen (values[i]) * 16 + 24;
  if (w < 450) w = 450;
  memset (b, 0, sizeof b);
  b[2] = 200 + w + 16; b[3] = 4 * 52 + 24;
  b[6] = -1;
  b[7] = (int) 0x84000012;      /* movable, title, Wimp redraws it */
  ((unsigned char *) b)[32] = 7;  ((unsigned char *) b)[33] = 2;
  ((unsigned char *) b)[34] = 7;  ((unsigned char *) b)[35] = 1;
  ((unsigned char *) b)[36] = 3;  ((unsigned char *) b)[37] = 1;
  ((unsigned char *) b)[38] = 12;
  b[10] = 0; b[11] = -(4 * 52 + 24); b[12] = 200 + w + 16; b[13] = 0;
  b[14] = 0x13D;                /* title: text, border, centred, filled, indirected */
  b[18] = (int) "About this program"; b[19] = -1; b[20] = 19;
  b[16] = 1;
  b[21] = 8;
  for (i = 0; i < 4; i++)
    {
      int *lab = &b[22 + 16 * i], *val = lab + 8, y1 = -12 - 52 * i, y0 = y1 - 44;
      lab[0] = 8; lab[1] = y0; lab[2] = 184; lab[3] = y1;
      lab[4] = 0x17000311;      /* text, vertically centred, indirected, right-justified */
      lab[5] = (int) labels[i]; lab[6] = -1; lab[7] = (int) strlen (labels[i]) + 1;
      val[0] = 200; val[1] = y0; val[2] = 200 + w; val[3] = y1;
      val[4] = 0x0700013D;      /* text, border, centred, filled, indirected: black on white */
      val[5] = (int) values[i]; val[6] = -1; val[7] = (int) strlen (values[i]) + 1;
    }
  return (app_create_error = _swix (Wimp_CreateWindow, _IN (1) | _OUT (0), b, &info_window)) != NULL;
}

/* Run a file as if double-clicked in the Filer (the guide, !Help). */
static void filer_run (const char *what)
{
  char cmd[300];
  snprintf (cmd, sizeof cmd, "Filer_Run <Groove$Dir>.%s", what);
  app_check (_swix (Wimp_StartTask, _IN (0), cmd));
}

/* The guide is an HTML page: open it only if a web browser will show it.
   With none set up, RISC OS hands HTML to a text editor, which would show
   the page's source, so say what's needed instead. */
static void open_guide (void)
{
  const char *run = getenv ("Alias$@RunType_FAF");
  char low[256];
  size_t i;
  snprintf (low, sizeof low, "%s", run ? run : "");
  for (i = 0; low[i]; i++)
    low[i] = (char) tolower ((unsigned char) low[i]);
  if (!low[0] || strstr (low, "edit") || strstr (low, "zap") || strstr (low, "stronged"))
    {
      app_error ("There's no web browser set up to show the User guide. Install one "
                 "(NetSurf, from PackMan) and open its directory once, then try again. "
                 "The guide is !Groove.Guide.index/html.");
      return;
    }
  filer_run ("Guide.index/html");
}

enum { IB_INFO, IB_HELP, IB_GUIDE, IB_SONG, IB_LOOPS, IB_NEW, IB_QUIT, IB_COUNT };

static void iconbar_menu (int x)
{
  menu_builder b;
  menu_begin (&b, m_iconbar, APP_NAME);
  menu_add (&b, "Info", 0, info_window >= 0 ? (void *) info_window : NULL, info_window < 0);
  menu_add (&b, "Help...", 0, NULL, 0);
  menu_add (&b, "User guide", MI_DOTTED, NULL, 0);
  menu_add (&b, "Song window", 0, NULL, 0);
  menu_add (&b, "Loop browser", 0, NULL, 0);
  menu_add (&b, "New song", MI_DOTTED, NULL, 0);
  menu_add (&b, "Quit", 0, NULL, 0);
  menu_end (&b);
  open_menu = MENU_ICONBAR;
  menu_x = x - 64;
  /* The style guide's place for an icon bar menu: its top 96 + 44 per item
     + 24 per dotted line above the bottom of the screen, so it sits just
     above the icon bar. */
  menu_y = 96 + IB_COUNT * 44 + 2 * 24;
  app_check (_swix (Wimp_CreateMenu, _INR (1, 3), m_iconbar, menu_x, menu_y));
}

static void new_track (gr_track_type t)
{
  int n = gr_track_add (&app.song, t, t == TRK_DRUMS ? "Drums" : t == TRK_INST ? gr_patch_name (PATCH_EPIANO) : "Audio",
                        t == TRK_DRUMS ? KIT_STUDIO : PATCH_EPIANO);
  if (n >= 0)
    {
      app.sel_track = n;
      app_changed ();
      arrange_set_extent ();
    }
}

static void menu_selection (int *sel)
{
  int t = app.sel_track, buttons;
  if (open_menu == MENU_ICONBAR)
    switch (sel[0])
      {
      case IB_HELP: filer_run ("!Help"); break;
      case IB_GUIDE: open_guide (); break;
      case IB_SONG: arrange_open (); break;
      case IB_LOOPS: browser_open (); break;
      case IB_NEW:
        if (!app.unsaved || ask ("This song has changes that haven't been saved. Start a new one anyway?"))
          {
            arrange_new_song ();
            arrange_open ();
          }
        break;
      case IB_QUIT:
        if (!app.unsaved || ask ("This song has changes that haven't been saved. Quit anyway?"))
          quit = 1;
        break;
      }
  else if (open_menu == MENU_SOUND)
    sound_chosen (sel);
  else if (open_menu == MENU_LANE)
    {
      if (sel[0] >= 0 && sel[0] < NLANE)
        editor_set_lane (lane_types[sel[0]]);
      else if (sel[0] == NLANE && sel[1] == 0)
        {
          char *e;
          long v = strtol (lane_cc_buf, &e, 10);
          if (e != lane_cc_buf && v >= 0 && v <= 127)
            editor_set_lane ((int) v);
        }
    }
  else if (open_menu == MENU_MAIN)
    switch (sel[0])
      {
      case 0:                     /* Song */
        if (sel[1] == 0)
          {
            if (!app.unsaved || ask ("This song has changes that haven't been saved. Start a new one anyway?"))
              arrange_new_song ();
          }
        else if (sel[1] == 1) savebox_open (FILETYPE_SONG);
        else if (sel[1] == 2) savebox_open (FILETYPE_WAVE);
        else if (sel[1] == 3) savebox_open (FILETYPE_MIDI);
        else if (sel[1] == 4 && sel[2] >= 0)
          {
            app.song.key = sel[2];
            app_changed ();
          }
        else if (sel[1] == 6)
          app_song_midi_out ();
        else if (sel[1] == 5 && sel[2] >= 0 && sel[2] < NMETERS)
          {
            /* From the bar the playhead is in (bar 1 at the start). */
            int bar = gr_tick_bar (&app.song, (int) gr_position (app.eng));
            gr_set_meter (&app.song, bar, meters[sel[2]][0], meters[sel[2]][1]);
            gr_locate (app.eng, gr_bar_tick (&app.song, bar));
            app_changed ();
          }
        break;
      case 1:                     /* Track */
        if (sel[1] == 0) new_track (TRK_DRUMS);
        else if (sel[1] == 1) new_track (TRK_INST);
        else if (sel[1] == 2) new_track (TRK_AUDIO);
        else if (sel[1] == 3)
          sound_chosen (sel + 2);
        else if (sel[1] == 4 && sel[2] == 0 && t >= 0 && t < app.song.ntracks)
          {
            /* Renamed (Return in the writable item); blank keeps the old name. */
            char *n = rename_buf, *e;
            while (*n == ' ')
              n++;
            for (e = n + strlen (n); e > n && (unsigned char) e[-1] <= ' '; e--)
              *(e - 1) = 0;
            if (*n)
              {
                snprintf (app.song.tracks[t].name, GR_NAME, "%s", n);
                app_changed ();
              }
          }
        else if (sel[1] == 5 && t >= 0 && t < app.song.ntracks)
          {
            gr_stop (app.eng);
            gr_track_delete (&app.song, t);
            app.sel_track = app.sel_region = -1;
            editor_show (-1);
            app_changed ();
          }
        else if (sel[1] == 6)
          app_track_midi_out ();
        else if (sel[1] == 7 && sel[2] >= 0 && sel[2] < 17 && t >= 0 && t < app.song.ntracks)
          {
            midiout_reset ();     /* (notes off on the old channel) */
            app.song.tracks[t].channel = sel[2] - 1;
            app_changed ();
          }
        else if (sel[1] == 8 && sel[2] >= 0 && sel[2] < 3 && sel[3] == 0 && t >= 0 && t < app.song.ntracks)
          {
            /* A number 0-127, or blank (or anything else) for automatic. */
            char *e;
            long v = strtol (num_buf[sel[2]], &e, 10);
            int *f = sel[2] == 0 ? &app.song.tracks[t].bank_msb : sel[2] == 1 ? &app.song.tracks[t].bank_lsb
                     : &app.song.tracks[t].program;
            *f = e != num_buf[sel[2]] && v >= 0 && v <= 127 ? (int) v : -1;
            midiout_reset ();     /* (sent again, with the new numbers) */
            app_changed ();
          }
        break;
      case 2:                     /* Region */
        if (app.sel_region >= 0 && app.song.regions[app.sel_region].used)
          {
            gr_region *r = &app.song.regions[app.sel_region];
            if (sel[1] == 0)
              {
                editor_show (app.sel_region);
                editor_open ();
              }
            else if (sel[1] == 1)
              {
                /* The copy shares the pattern until either is edited. */
                int n = gr_region_add (&app.song, r->track, r->start + r->len, r->len, r->pattern);
                if (n >= 0)
                  {
                    app.song.regions[n].clip = r->clip;
                    app.song.regions[n].clip_offset = r->clip_offset;
                    app.sel_region = n;
                  }
                app_changed ();
              }
            else if (sel[1] == 2)
              {
                gr_region_delete (&app.song, app.sel_region);
                app.sel_region = -1;
                editor_show (-1);
                app_changed ();
              }
            else if (sel[1] == 3)
              arrange_split ();
            else if (sel[1] == 4)
              arrange_copy ();
          }
        if (sel[1] == 5)
          arrange_paste ();
        break;
      case 3: browser_open (); break;
      case 4: editor_open (); break;
      case 5: keys_open (); break;
      case 6:
        {
          static const int ports[7] = { MIDI_OFF, MIDI_SUPPORT, MIDI_ANY, 0, 1, 2, 3 };
          if (sel[1] == 0 || (sel[1] == 1 && midiin_support ()) || (sel[1] > 1 && sel[1] < 7 && midiin_present ()))
            midiin_set_port (ports[sel[1]]);
        }
        break;
      case 7:
        {
          static const int ports[6] = { MIDI_OFF, MIDI_SUPPORT, 0, 1, 2, 3 };
          if (sel[1] == 0 || (sel[1] == 1 && midiin_support ()) || (sel[1] > 1 && sel[1] < 6 && midiin_present ()))
            {
              midiout_set_port (ports[sel[1]]);
              arrange_output_changed ();
            }
        }
        break;
      case 8: app.song.loop_on = !app.song.loop_on; app_changed (); break;
      case 9: app.song.metronome = !app.song.metronome; app_redraw_all (app.w_tool); break;
      case 10: app.count_in = !app.count_in; break;
      case 11: app.merge = !app.merge; break;
      case 12: app.follow = !app.follow; break;
      case 13: app_undo (0); break;
      case 14: app_undo (1); break;
      }
  /* Adjust keeps the menu open, as everywhere on RISC OS. */
  app_pointer (NULL, NULL, &buttons, NULL, NULL);
  if (buttons & BUT_ADJUST)
    {
      if (open_menu == MENU_MAIN)
        build_main_menu ();
      else if (open_menu == MENU_SOUND)
        build_sound_menu ();
      else if (open_menu == MENU_LANE)
        build_lane_menu ();
      _swix (Wimp_CreateMenu, _INR (1, 3), open_menu == MENU_MAIN ? m_main
             : open_menu == MENU_SOUND ? m_sound : open_menu == MENU_LANE ? m_lane : m_iconbar, menu_x, menu_y);
    }
}

/* ---- messages -------------------------------------------------------------- */

static void reply (int *m, int action)
{
  m[3] = m[2];                  /* your_ref = their my_ref */
  m[4] = action;
  _swix (Wimp_SendMessage, _INR (0, 2), EV_MESSAGE, m, m[1]);
}

static int is_song_file (const char *path)
{
  char line[16] = "";
  FILE *f = fopen (path, "r");
  if (!f)
    return 0;
  if (!fgets (line, sizeof line, f))
    line[0] = 0;
  fclose (f);
  return strncmp (line, "GROOVE ", 7) == 0;
}

static void load_song (const char *path)
{
  gr_stop (app.eng);
  gr_stop (app.eng);
  keys_release_all ();
  browser_stop_preview ();
  if (gr_song_load (&app.song, path))
    {
      app_error ("That file isn't a Groove song, or it couldn't be read.");
      arrange_new_song ();
      return;
    }
  gr_engine_set_song (app.eng, &app.song);
  snprintf (app.path, sizeof app.path, "%s", path);
  app.unsaved = 0;
  app.sel_track = app.song.ntracks ? 0 : -1;
  app.sel_region = -1;
  app.view.hscroll = 0;
  editor_show (-1);
  arrange_set_extent ();
  arrange_open ();
  app_changed ();
  gr_history_reset (app.hist, &app.song);
  app.unsaved = 0;
  arrange_title ();
  if (gr_song_uses_gm (&app.song))
    app_gm_ensure ();
}

static void message (int *m, int reason)
{
  const char *path = (const char *) &m[11];
  switch (m[4])
    {
    case MSG_QUIT:
      quit = 1;
      break;
    case MSG_PREQUIT:
      if (app.unsaved && !ask ("This song has changes that haven't been saved. Quit anyway?"))
        reply (m, MSG_PREQUIT);   /* acknowledging the message stops the shutdown */
      break;
    case MSG_DATASAVEACK:
      savebox_ack (m);
      break;
    case MSG_DATAOPEN:            /* double-clicked in a Filer window */
      if (m[10] == FILETYPE_SONG && is_song_file (path))
        {
          reply (m, MSG_DATALOADACK);
          if (!app.unsaved || ask ("Load this song and lose the changes to the current one?"))
            load_song (path);
        }
      break;
    case MSG_DATALOAD:
      if (m[5] != -2 && m[5] != app.w_main)
        break;
      if (m[10] == FILETYPE_WAVE
          || ((m[10] == FILETYPE_MIDI || m[10] == FILETYPE_MAESTRO) && m[5] == app.w_main))
        {
          reply (m, MSG_DATALOADACK);
          arrange_open ();
          arrange_drop_file (path, m[10], m[7], m[8]);
        }
      else if (m[10] == FILETYPE_MIDI || m[10] == FILETYPE_MAESTRO)   /* on the icon bar: from bar 1 */
        {
          reply (m, MSG_DATALOADACK);
          arrange_open ();
          arrange_import (path, m[10], 0);
        }
      else if (is_song_file (path))
        {
          reply (m, MSG_DATALOADACK);
          if (!app.unsaved || ask ("Load this song and lose the changes to the current one?"))
            load_song (path);
        }
      else if (reason != EV_MESSAGE_ACK)
        app_error ("Groove loads its own songs, WAV files, MIDI files and Maestro music.");
      break;
    case MSG_MODECHANGE:
      read_eigen ();
      if (app.main_open) app_redraw_all (app.w_main);
      break;
    }
}

/* ---- events ----------------------------------------------------------------- */

static void open_request (int *b)
{
  if (b[0] == app.w_main)
    arrange_open_request (b);
  else if (b[0] == app.w_edit)
    editor_open_request (b);
  else
    app_check (_swix (Wimp_OpenWindow, _IN (1), b));
}

static void close_request (int *b)
{
  if (b[0] == app.w_main)
    arrange_close ();
  else
    {
      _swix (Wimp_CloseWindow, _IN (1), b);
      if (b[0] == app.w_edit) app.edit_open = 0;
      if (b[0] == app.w_loops) { app.loops_open = 0; browser_stop_preview (); }
      if (b[0] == app.w_keys) { app.keys_open = 0; keys_release_all (); }
    }
}

static void redraw (int *b)
{
  if (b[0] == app.w_main) arrange_redraw (b);
  else if (b[0] == app.w_tool) arrange_tool_redraw (b);
  else if (b[0] == app.w_edit) editor_redraw (b);
  else if (b[0] == app.w_loops) browser_redraw (b);
  else if (b[0] == app.w_keys) keys_redraw (b);
}

static void give_caret (int window)
{
  /* An invisible caret (R4 bit 25), so key presses come to us. */
  _swix (Wimp_SetCaretPosition, _INR (0, 5), window, -1, 0, 0, 1 << 25, -1);
}

static void click (int *b)
{
  if (b[3] == -2)               /* the icon bar */
    {
      if (b[2] == BUT_MENU)
        iconbar_menu (b[0]);
      else if (b[2] == BUT_SELECT)
        arrange_open ();
      else if (b[2] == BUT_ADJUST)
        browser_open ();
      return;
    }
  if (b[3] == app.w_save)
    {
      savebox_click (b);
      return;
    }
  if (b[3] == app.w_main || b[3] == app.w_tool || b[3] == app.w_edit || b[3] == app.w_keys)
    give_caret (b[3] == app.w_tool ? app.w_main : b[3]);
  if (b[3] == app.w_main) arrange_click (b);
  else if (b[3] == app.w_tool) arrange_tool_click (b);
  else if (b[3] == app.w_edit) editor_click (b);
  else if (b[3] == app.w_loops) browser_click (b);
  else if (b[3] == app.w_keys) keys_click (b);
}

static void key (int *b)
{
  int code = b[6];
  if (b[0] == app.w_save && savebox_key (b))
    return;
  switch (code)
    {
    case 0x187: browser_open (); return;          /* F7 */
    case 0x188: editor_open (); return;           /* F8 */
    case 0x189: keys_open (); return;             /* F9 */
    case 0x1CA: midiout_cycle (); return;         /* F10 (&1CA: F10-F12 are &1CA-&1CC): the next MIDI out */
    case 0x1CB: app_track_midi_out (); return;    /* F11: the selected track on MIDI out, or not */
    case 0x1DB: app_song_midi_out (); return;     /* Shift-F11: every track */
    case 0x13: savebox_open (FILETYPE_SONG); return;   /* Ctrl-S */
    case 0x1A: app_undo (0); return;                    /* Ctrl-Z */
    case 0x19: app_undo (1); return;                    /* Ctrl-Y */
    }
  if (b[0] == app.w_edit && editor_key (code))
    return;
  if (keys_press (code))
    return;
  _swix (Wimp_ProcessKey, _IN (0), code);         /* not ours: pass it on */
}

static void null_event (void)
{
  static int last;
  int now;
  if (app.drag)
    {
      int x, y, buttons;
      app_pointer (&x, &y, &buttons, NULL, NULL);
      if (buttons & (BUT_SELECT | BUT_ADJUST))
        app.drag (x, y, 0);
      else
        {
          drag_fn fn = app.drag;
          app.drag = NULL;
          fn (x, y, 1);
        }
    }
  keys_poll ();
  _swix (OS_ReadMonotonicTime, _OUT (0), &now);
  if (now - last >= 4)          /* 25 times a second is plenty for the eye */
    {
      last = now;
      arrange_tick ();
      editor_tick ();
    }
}

static int create_iconbar_icon (void)
{
  int b[9];
  b[0] = -1;
  b[1] = 0; b[2] = 0; b[3] = 68; b[4] = 68;
  b[5] = 0x301A;                /* sprite, centred, click */
  memcpy (&b[6], "!groove", 8);
  return (app_create_error = _swix (Wimp_CreateIcon, _INR (0, 1) | _OUT (0), 0, b, &iconbar_icon)) != NULL;
}

/* After each user event, and when a drag ends: remember the song for
   Undo if it changed, and keep the title's "*" in step. Not during a drag
   (one step for the whole drag) or while recording (one for the take). */
static void note_change (int reason)
{
  static int was_dragging;
  int drag_ended = was_dragging && !app.drag;
  was_dragging = app.drag != NULL;
  if (app.drag || gr_recording (app.eng) || (reason == EV_NULL && !drag_ended))
    return;
  gr_history_note (app.hist, &app.song);
  app.unsaved = !gr_history_is_saved (app.hist);
}

void app_undo (int redo)
{
  int ok;
  keys_release_all ();
  browser_stop_preview ();
  if (gr_playing (app.eng))
    gr_stop (app.eng);
  gr_history_note (app.hist, &app.song);      /* (anything not yet noted) */
  ok = redo ? gr_history_redo (app.hist, &app.song) : gr_history_undo (app.hist, &app.song);
  if (!ok)
    return;
  gr_locate (app.eng, (int) gr_position (app.eng));   /* notes off */
  if (app.sel_track >= app.song.ntracks)
    app.sel_track = app.song.ntracks - 1;
  if (app.sel_region >= 0 && !app.song.regions[app.sel_region].used)
    app.sel_region = -1;
  editor_show (app.sel_region);
  editor_changed ();                          /* (notes undone drop out of the selection) */
  arrange_set_extent ();
  app.unsaved = !gr_history_is_saved (app.hist);
  if (app.main_open)
    {
      app_redraw_all (app.w_main);
      app_redraw_all (app.w_tool);
    }
  if (app.edit_open)
    app_redraw_all (app.w_edit);
  if (app.keys_open)
    app_redraw_all (app.w_keys);
  arrange_title ();
}

static int init (void)
{
  static const int messages[] = { MSG_DATASAVEACK, MSG_DATALOAD, MSG_DATAOPEN,
                                  MSG_PREQUIT, MSG_MODECHANGE, 0 };
  _kernel_oserror *e;
  startlog_init ();
  startlog_stage ("starting as a Wimp task");
  e = _swix (Wimp_Initialise, _INR (0, 3) | _OUT (1), 380, TASK_WORD, APP_NAME, messages, &app.task);
  if (e)
    {
      startlog_fail ("Wimp_Initialise", e);
      return 1;
    }
  startlog ("task handle &%X", app.task);
  startlog_stage ("reading the screen mode");
  read_eigen ();
  startlog ("screen %d x %d OS units, eig %d %d", app.screen_w, app.screen_h, app.xeig, app.yeig);
  startlog_stage ("setting up the engine");
  gr_song_init (&app.song);
  app.eng = gr_engine_new (&app.song);
  app.hist = gr_history_new (100, 16 * 1024 * 1024);
  if (!app.eng || !app.hist)
    {
      startlog ("FAILED: gr_engine_new: out of memory");
      app_error ("Groove hasn't enough memory to start. Give it a bigger WimpSlot.");
      return 1;
    }
  app.sel_track = app.sel_region = -1;
  app.view.bar_w = 160;
  app.follow = 1;
  app.count_in = 1;
  {
    static const struct { const char *what; int (*fn) (void); } steps[] = {
      { "the icon bar icon", create_iconbar_icon }, { "the song window", arrange_create },
      { "the editor window", editor_create }, { "the loop browser", browser_create },
      { "the keyboard window", keys_create }, { "the save box", savebox_create },
      { "the Info box", info_create },
    };
    unsigned i;
    for (i = 0; i < sizeof steps / sizeof steps[0]; i++)
      {
        startlog_stage (steps[i].what);
        app_create_error = NULL;
        if (steps[i].fn ())
          {
            startlog_fail (steps[i].what, app_create_error);
            return 1;
          }
      }
  }
  startlog_stage ("starting sound (SharedSound handler)");
  if (sound_init ())
    {
      startlog ("FAILED: sound_init");
      return 1;
    }
  startlog_stage ("MIDI");
  midiin_forget_module ();      /* (registers with MIDI Support, if it's there) */
  atexit (midiin_quit);
  startlog ("MIDI Support %s, MIDI module %s", midiin_support () ? "registered" : "not there",
            midiin_present () ? "there" : "not there");
  startlog_stage ("opening a new song");
  arrange_new_song ();
  /* Straight into the studio: the song window and the
     loops, laid out across the screen (the icon bar icon brings them back
     after they're closed). */
  startlog_stage ("opening the windows");
  arrange_open ();
  browser_open ();
  /* The input focus too, so Space and musical typing work straight away. */
  _swix (Wimp_SetCaretPosition, _INR (0, 5), app.w_main, -1, 0, 0, 1 << 25, -1);
  startlog_stage ("running");
  return 0;
}

int main (void)
{
  static int b[64];
  int reason;
  if (init ())
    return 1;
  while (!quit)
    {
      _kernel_oserror *e;
      int midi = midiin_active ();
      int busy;
      midi |= midiout_pending ();
      busy = gr_busy (app.eng) || app.drag || keys_held () || midi;
      if (busy)
        {
          int now;
          _swix (OS_ReadMonotonicTime, _OUT (0), &now);
          /* (every centisecond while a MIDI keyboard may be played) */
          e = _swix (Wimp_PollIdle, _INR (0, 2) | _OUT (0), 0, b, now + (midi ? 1 : 2), &reason);
        }
      else
        e = _swix (Wimp_Poll, _INR (0, 1) | _OUT (0), 1, b, &reason);
      if (e)
        {
          startlog_fail ("Wimp_Poll", e);
          break;
        }
      {
        /* Keep the title's "*" in step with unsaved changes, however they
           were made (some controls only mark the song as changed). */
        static int shown_unsaved = -1;
        if (app.unsaved != shown_unsaved)
          {
            shown_unsaved = app.unsaved;
            arrange_title ();
          }
      }
      {
        static int logged;
        if (logged < 12)
          startlog ("event %d, sound played to %u, queued %d", reason, sound_played (), sound_latency ()), logged++;
      }
      midiin_poll ();           /* notes from a MIDI keyboard, first */
      sound_fill (app.eng);     /* on every return from the Wimp */
      midiout_poll ();          /* MIDI out whose sound is now being heard */
      note_change (reason);
      switch (reason)
        {
        case EV_NULL: null_event (); break;
        case EV_REDRAW: redraw (b); break;
        case EV_OPEN: open_request (b); break;
        case EV_CLOSE: close_request (b); break;
        case EV_CLICK: click (b); break;
        case EV_DRAG:           /* a Wimp drag box ended: each checks it was its own */
          browser_drag_done ();
          savebox_drag_done ();
          break;
        case EV_KEY: key (b); break;
        case EV_MENU: menu_selection (b); break;
        case EV_SCROLL: if (b[0] == app.w_main) arrange_scroll (b); break;
        case EV_LOSE_CARET: keys_release_all (); break;
        case EV_MESSAGE:
        case EV_MESSAGE_RECORDED:
        case EV_MESSAGE_ACK:
          message (b, reason);
          break;
        }
      sound_fill (app.eng);     /* events can be slow: top up again */
      midiout_poll ();
    }
  gr_stop (app.eng);
  midiin_quit ();               /* "Groove" leaves MIDI Support's map */
  sound_close ();
  _swix (Wimp_CloseDown, _INR (0, 1), app.task, TASK_WORD);
  return 0;
}

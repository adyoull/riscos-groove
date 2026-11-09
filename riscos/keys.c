/* Groove - musical typing: play the selected track from the computer
   keyboard, laid out like a piano's:

        W E   T Y U   O P              black keys
       A S D F G H J K L ;             white keys, A = C
       Z/X octave down/up    C/V velocity down/up

   On a drum track, A to K are the eight drums.

   The Wimp reports key presses but never releases, so while a note is held
   we watch its key on null events with OS_Byte 129 (INKEY with a negative
   number: is this key down now?) and end the note when it comes up. */
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <swis.h>
#include "app.h"
#include "gfx.h"

#define NKEYS 17

/* Letter, semitone above C, BBC/RISC OS INKEY number (negative). */
static const struct { char c; int semi, inkey; } map[NKEYS] = {
  { 'a', 0, -66 }, { 'w', 1, -34 }, { 's', 2, -82 }, { 'e', 3, -35 }, { 'd', 4, -51 },
  { 'f', 5, -68 }, { 't', 6, -36 }, { 'g', 7, -84 }, { 'y', 8, -69 }, { 'h', 9, -85 },
  { 'u', 10, -54 }, { 'j', 11, -70 }, { 'k', 12, -71 }, { 'o', 13, -55 }, { 'l', 14, -87 },
  { 'p', 15, -56 }, { ';', 16, -88 },
};

/* Drum names that fit on a key (gr_drum_name is for wider places). */
static const char *const short_drum[GR_DRUM_SOUNDS] =
  { "Kick", "Snare", "Hat", "Open", "Clap", "Tom L", "Tom H", "Bell" };

static int held_pitch[NKEYS], held_track[NKEYS];
static int octave = 0, velocity = 100, initialised, mouse_key = -1;

#define KW 128                  /* white key width: big enough to hit and to label */
#define KH 320
#define BW (KW * 5 / 8)         /* black key width */
#define BH (KH * 6 / 10)        /* black key height */
#define KTOP 64                 /* status line above the keys */

static void init (void)
{
  int i;
  for (i = 0; i < NKEYS; i++)
    held_pitch[i] = -1;
  initialised = 1;
}

int keys_create (void)
{
  int b[24];
  init ();
  memset (b, 0, sizeof b);
  b[0] = 300; b[1] = 100; b[2] = 300 + 10 * KW + 32; b[3] = 100 + KTOP + KH + 24;
  b[6] = -1;
  b[7] = (int) 0x87000002;      /* movable, back, close, title */
  ((unsigned char *) b)[32] = 7;  ((unsigned char *) b)[33] = 2;
  ((unsigned char *) b)[34] = 7;  ((unsigned char *) b)[35] = 255;
  ((unsigned char *) b)[36] = 3;  ((unsigned char *) b)[37] = 1;
  ((unsigned char *) b)[38] = 12;
  b[10] = 0; b[11] = -(KTOP + KH + 24); b[12] = 10 * KW + 32; b[13] = 0;
  b[14] = 0x3D;
  b[15] = 3 << 12;
  b[16] = 1;
  strcpy ((char *) &b[18], "Keyboard");
  b[21] = 0;
  return (app_create_error = _swix (Wimp_CreateWindow, _IN (1) | _OUT (0), b, &app.w_keys)) != NULL;
}

void keys_open (void)
{
  app_open_window (app.w_keys, WS_KEYS);
  app.keys_open = 1;
  /* Give it the caret, so typing plays straight away. */
  _swix (Wimp_SetCaretPosition, _INR (0, 5), app.w_keys, -1, 0, 0, 1 << 25, -1);
  app_redraw_all (app.w_tool);
}

static int track_type (void)
{
  if (app.sel_track < 0 || app.sel_track >= app.song.ntracks)
    return -1;
  return app.song.tracks[app.sel_track].type;
}

static void redraw_keys (void)
{
  if (app.keys_open)
    app_redraw_all (app.w_keys);
}

static void note_on (int i)
{
  int t = app.sel_track, type = track_type ();
  if (held_pitch[i] >= 0 || type < 0 || type == TRK_AUDIO)
    return;
  if (type == TRK_DRUMS)
    {
      /* The white keys A..K are the eight drums. */
      static const int drum_of[NKEYS] = { 0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6, 7, -1, -1, -1, -1 };
      if (drum_of[i] < 0)
        return;
      gr_live_on (app.eng, t, gr_drum_kit_key (drum_of[i]), velocity);
      held_pitch[i] = gr_drum_kit_key (drum_of[i]);
    }
  else
    {
      int p = 60 + 12 * octave + map[i].semi;
      if (p < 0 || p > 127)
        return;
      gr_live_on (app.eng, t, p, velocity);
      held_pitch[i] = p;
    }
  held_track[i] = t;
  redraw_keys ();
}

static void note_off (int i)
{
  if (held_pitch[i] < 0)
    return;
  if (held_track[i] < app.song.ntracks && app.song.tracks[held_track[i]].type == TRK_INST)
    gr_live_off (app.eng, held_track[i], held_pitch[i]);
  held_pitch[i] = -1;
  redraw_keys ();
}

void keys_release_all (void)
{
  int i;
  if (!initialised)
    init ();
  for (i = 0; i < NKEYS; i++)
    note_off (i);
}

int keys_held_count (void)
{
  int i, n = 0;
  for (i = 0; i < NKEYS; i++)
    if (held_pitch[i] >= 0)
      n++;
  return n;
}

int keys_held (void)
{
  int i;
  for (i = 0; i < NKEYS; i++)
    if (held_pitch[i] >= 0)
      return 1;
  return mouse_key >= 0;
}

static int key_down (int inkey)
{
  int r1 = 0;
  _swix (OS_Byte, _INR (0, 2) | _OUT (1), 129, inkey & 0xFF, 0xFF, &r1);
  return r1 == 0xFF;
}

/* Does a Groove window have the input focus (so the keys are ours)? */
static int our_caret (void)
{
  int c[6];
  if (_swix (Wimp_GetCaretPosition, _IN (1), c))
    return 0;
  return c[0] >= 0 && (c[0] == app.w_main || c[0] == app.w_edit || c[0] == app.w_keys);
}

/* On null events while a note is held: end notes whose key is up, and
   start notes for keys that have gone down. The second matters for chords:
   RISC OS turns at most two held keys into key-press events (2-key
   rollover), so a third key never arrives as an event; scanning the
   keyboard (OS_Byte 129) sees every key the keyboard reports. */
void keys_poll (void)
{
  int i, held = 0;
  for (i = 0; i < NKEYS; i++)
    if (held_pitch[i] >= 0 && i != mouse_key && !key_down (map[i].inkey))
      note_off (i);
  for (i = 0; i < NKEYS; i++)
    if (held_pitch[i] >= 0)
      held = 1;
  if (!held || !our_caret ())
    return;
  for (i = 0; i < NKEYS; i++)
    if (held_pitch[i] < 0 && key_down (map[i].inkey))
      note_on (i);
}

int keys_press (int code)
{
  int i, c = code < 256 ? tolower (code) : code;
  if (arrange_key (code))
    return 1;
  switch (c)
    {
    case 'z': if (octave > -3) octave--; keys_release_all (); redraw_keys (); return 1;
    case 'x': if (octave < 3) octave++; keys_release_all (); redraw_keys (); return 1;
    case 'c': velocity = velocity > 20 ? velocity - 20 : 1; redraw_keys (); return 1;
    case 'v': velocity = velocity < 107 ? velocity + 20 : 127; redraw_keys (); return 1;
    }
  for (i = 0; i < NKEYS; i++)
    if (map[i].c == c)
      {
        note_on (i);            /* (auto-repeats arrive while held: ignored) */
        return 1;
      }
  return 0;
}

/* ---- the window ----------------------------------------------------------------- */

static int white_index (int i)    /* which white key, or -1 for a black one */
{
  static const int w[NKEYS] = { 0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6, 7, -1, 8, -1, 9 };
  return w[i];
}

static int black_x (int i)        /* left edge of black key i, in white-key units * 2 */
{
  /* Between whites: W (C#) after A, E after S, T after F, Y after G, U after H, O after K, P after L. */
  static const int after[NKEYS] = { 0, 0, 0, 1, 0, 0, 3, 0, 4, 0, 5, 0, 0, 7, 0, 8, 0 };
  return after[i];
}

static void paint (int ox, int oy, const int *clip)
{
  int i, type = track_type ();
  char buf[96], name[8];
  int x0 = ox + 16, top = oy - KTOP, bot = top - KH;
  (void) clip;
  gfx_colour (0x34363B);
  gfx_rect (clip[0], clip[1], clip[2], clip[3]);
  if (type == TRK_DRUMS)
    snprintf (buf, sizeof buf, "%s   velocity %d (C/V)", app.song.tracks[app.sel_track].name, velocity);
  else if (type == TRK_INST)
    snprintf (buf, sizeof buf, "%s    A = %s (Z/X)    velocity %d (C/V)",
              app.song.tracks[app.sel_track].name, gr_note_name (60 + 12 * octave, name), velocity);
  else
    snprintf (buf, sizeof buf, "Select a drum or instrument track to play it");
  gfx_text (ox + 16, oy - 40, buf, C_TEXT, 0x34363B, 0);

  for (i = 0; i < NKEYS; i++)
    {
      int w = white_index (i);
      char label[2] = { (char) toupper (map[i].c), 0 };
      if (w < 0)
        continue;
      gfx_colour (held_pitch[i] >= 0 ? (type == TRK_DRUMS ? C_DRUMS : C_INST) : 0xF2F2F4);
      gfx_round (x0 + w * KW + 2, bot, x0 + (w + 1) * KW - 2, top, 8);
      gfx_text (x0 + w * KW + KW / 2, bot + 20, label, 0x303236, held_pitch[i] >= 0 ? C_INST : 0xF2F2F4, 1);
      if (type == TRK_DRUMS && i <= 12)
        {
          static const int drum_of[NKEYS] = { 0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6, 7, -1, -1, -1, -1 };
          if (drum_of[i] >= 0)
            gfx_text (x0 + w * KW + KW / 2, bot + 60, short_drum[drum_of[i]], 0x606268,
                      held_pitch[i] >= 0 ? C_DRUMS : 0xF2F2F4, 1);
        }
    }
  if (type != TRK_DRUMS)
    for (i = 0; i < NKEYS; i++)
      {
        int bx;
        char label[2] = { (char) toupper (map[i].c), 0 };
        if (white_index (i) >= 0)
          continue;
        bx = x0 + (black_x (i) + 1) * KW - BW / 2;
        gfx_colour (held_pitch[i] >= 0 ? C_INST : 0x1E1F22);
        gfx_round (bx, top - BH, bx + BW, top, 6);
        gfx_text (bx + BW / 2, top - BH + 20, label, 0xC8CAD0, held_pitch[i] >= 0 ? C_INST : 0x1E1F22, 1);
      }
}

void keys_redraw (int *b)
{
  int more;
  _swix (Wimp_RedrawWindow, _IN (1) | _OUT (0), b, &more);
  while (more)
    {
      paint (b[1] - b[5], b[4] - b[6], &b[7]);
      _swix (Wimp_GetRectangle, _IN (1) | _OUT (0), b, &more);
    }
}

/* Clicking a key plays it until the button is let go. */
static void mouse_up (int x, int y, int done)
{
  (void) x; (void) y;
  if (done && mouse_key >= 0)
    {
      int k = mouse_key;
      mouse_key = -1;
      note_off (k);
    }
}

void keys_click (int *b)
{
  int ox, oy, wx, wy, i, hit = -1;
  if (b[2] == BUT_MENU)
    {
      app_main_menu (b[0], b[1]);
      return;
    }
  app_window_origin (app.w_keys, &ox, &oy);
  wx = b[0] - ox - 16;
  wy = b[1] - oy;
  if (wy > -KTOP || wy < -(KTOP + KH))
    return;
  if (track_type () != TRK_DRUMS && wy > -KTOP - BH)
    for (i = 0; i < NKEYS; i++)        /* black keys are on top */
      if (white_index (i) < 0)
        {
          int bx = (black_x (i) + 1) * KW - BW / 2;
          if (wx >= bx && wx < bx + BW)
            hit = i;
        }
  if (hit < 0)
    for (i = 0; i < NKEYS; i++)
      if (white_index (i) >= 0 && wx >= white_index (i) * KW && wx < (white_index (i) + 1) * KW)
        hit = i;
  if (hit < 0)
    return;
  note_on (hit);
  mouse_key = hit;
  app_start_drag (app.w_keys, mouse_up);
}

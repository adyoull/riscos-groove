/* Groove - the Score window (F6): the selected track as written music.

   The notation is the same as Export as MusicXML writes (engine/notation.c),
   laid out to the window's width (engine/engrave.c) and drawn
   anti-aliased (engine/raster.c) into a 32bpp sprite a strip at a time;
   on a screen that can't show that, and on paper, with the Draw module:
   music symbols from Bravura's outlines (engine/glyphs.c), beams and ties
   as filled paths, lines as rectangles at least a pixel thick. It's remade when the song changes, another track is selected or
   the window is resized. The playing bar is highlighted and kept in
   view; clicking a bar moves the playhead there.

   Printing (Song > Print score, Ctrl-P) goes through the printer driver
   !Printers loads, like any RISC OS program's: the score is laid out
   again to the page's width, split into pages at system breaks, and
   drawn the same way (the driver catches the drawing), with text in an
   outline font. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel.h"
#include "swis.h"
#include "app.h"
#include "gfx.h"
#include "layout.h"
#include "../engine/notation.h"
#include "../engine/engrave.h"
#include "../engine/glyphs.h"
#include "../engine/raster.h"

#ifndef Draw_Fill
#define Draw_Fill 0x40702
#endif
#ifndef PDriver_Info
#define PDriver_Info          0x80140
#define PDriver_PageSize      0x80143
#define PDriver_SelectJob     0x80145
#define PDriver_EndJob        0x80148
#define PDriver_AbortJob      0x80149
#define PDriver_GiveRectangle 0x8014B
#define PDriver_DrawPage      0x8014C
#define PDriver_GetRectangle  0x8014D
#define PDriver_DeclareFont   0x80155
#endif
#ifndef Font_FindFont
#define Font_FindFont 0x40081
#define Font_LoseFont 0x40082
#define Font_Paint    0x40086
#endif
#ifndef Font_ScanString
#define Font_ScanString 0x400A1
#endif
#ifndef ColourTrans_SetFontColours
#define ColourTrans_SetFontColours 0x4074F
#endif
#ifndef OS_Find
#define OS_Find 0x0D
#endif
#ifndef ColourTrans_SelectTable
#define ColourTrans_SelectTable 0x40740
#endif
#ifndef OS_SpriteOp
#define OS_SpriteOp 0x2E
#endif

/* Non-zero winding; the outline's own pixels inside and out too, so thin
   strokes (a clef's, a hollow notehead's) don't break up at small sizes. */
#define FILL_STYLE 0x38

#define PAPER     0xFFFEF8
#define INK       0x000000
#define HILITE    0xFFEFA8
#define DIM       0x8A8A8A
#define TOP_PAD   24              /* OS units above the music */

/* OS units a staff space: 20 is 100% (25% to see a whole orchestral
   page; 0.4.3 had 60% to 160%) */
static const int zooms[] = { 5, 6, 8, 10, 12, 16, 20, 26, 32 };
#define NZOOMS ((int) (sizeof zooms / sizeof zooms[0]))
static int zoom = 7;
static char title[64] = "Score";
static gr_engraving *eng;
static int dirty = 1, built_track = -2, built_width, shown_bar = -1, last_sel = -2;
static int *glyph_path[GL_COUNT];  /* each glyph as a Draw path, in font units */
static int print_sp;              /* printing: OS units a staff space (else 0) */

static int sp_os (void)           /* OS units a staff space, on screen or the page */
{
  return print_sp ? print_sp : zooms[zoom];
}

static int z_os (int units)       /* engraving units to OS units */
{
  return (int) ((long long) units * sp_os () / EG_SP);
}

static int os_z (int os)
{
  return (int) ((long long) os * EG_SP / sp_os ());
}

int score_create (void)
{
  int b[24];
  memset (b, 0, sizeof b);
  b[0] = 200; b[1] = 120; b[2] = 1400; b[3] = 900;
  b[6] = -1;
  b[7] = (int) 0xBF000102;      /* back, close, title, toggle, vertical scroll, adjust size
                                   (0.4 to 0.4.2-test2 had 0xBD: no close icon); Scroll_Request
                                   events, so Ctrl + the wheel can zoom */
  ((unsigned char *) b)[32] = 7;  ((unsigned char *) b)[33] = 2;
  ((unsigned char *) b)[34] = 7;  ((unsigned char *) b)[35] = 0;   /* white work area */
  ((unsigned char *) b)[36] = 3;  ((unsigned char *) b)[37] = 1;
  ((unsigned char *) b)[38] = 12;
  b[10] = 0; b[11] = -2000; b[12] = 4000; b[13] = 0;
  b[14] = 0x13D;
  b[15] = 3 << 12;              /* clicks */
  b[16] = 1;
  ((short *) b)[34] = 480;  ((short *) b)[35] = 240;
  b[18] = (int) title; b[19] = -1; b[20] = sizeof title;
  b[21] = 0;
  return (app_create_error = _swix (Wimp_CreateWindow, _IN (1) | _OUT (0), b, &app.w_score)) != NULL;
}

static int visible_width (void)
{
  int b[9];
  b[0] = app.w_score;
  if (_swix (Wimp_GetWindowState, _IN (1), b))
    return 1200;
  return b[3] - b[1];
}

static int full_score;          /* every part (else the selected track) */
static int concert;             /* transposing instruments at concert pitch */
static int repeats = 1;         /* music played again written once, in repeats */
/* the score shown: the bar each of the song's bars is written in, and
   where each written bar starts (the first time it's played) */
static int *shown_bar_of, shown_nsong, *shown_start, shown_nbars;
static int paints;              /* (logged: the first few after making a score) */

static int song_bars (void)
{
  return gr_tick_bar (&app.song, gr_song_end (&app.song) > 0 ? gr_song_end (&app.song) - 1 : 0) + 1;
}

/* The score to show or print, width engraving units wide: the full score
   (track < 0) or one track's part. NULL if there's nothing to show.
   shown: it's the one the window shows (so keep where its bars are). */
static gr_engraving *make (int track, int width, const char *title, int shown)
{
  int t0 = log_time ();
  gr_notation *nt;
  gr_engraving *g = NULL;
  startlog ("score: track %d, width %d, %d bars: notation", track, width, song_bars ());
  nt = gr_notation_make_ex (&app.song, track, song_bars (),
                            (concert ? GR_NT_CONCERT : 0) | (repeats ? GR_NT_REPEATS : 0));
  startlog ("score: notation %s, %d bars, %d parts, %d cs", nt ? "made" : "failed", nt ? nt->nbars : 0,
            nt ? nt->nparts : 0, log_time () - t0);
  if (shown)
    {
      paints = 0;
      free (shown_bar_of);
      free (shown_start);
      shown_bar_of = shown_start = NULL;
      shown_nsong = shown_nbars = 0;
      if (nt && (shown_bar_of = malloc ((size_t) nt->nsong * sizeof *shown_bar_of))
          && (shown_start = malloc ((size_t) nt->nbars * sizeof *shown_start)))
        {
          int i;
          memcpy (shown_bar_of, nt->bar_of, (size_t) nt->nsong * sizeof *shown_bar_of);
          for (i = 0; i < nt->nbars; i++)
            shown_start[i] = nt->bars[i].start;
          shown_nsong = nt->nsong;
          shown_nbars = nt->nbars;
        }
    }
  if (nt && (track < 0 || nt->nparts > 1))   /* (a track's part: on several tracks, joined) */
    {
      int parts[GR_MAX_TRACKS], i, n = nt->nparts < GR_MAX_TRACKS ? nt->nparts : GR_MAX_TRACKS;
      for (i = 0; i < n; i++)
        parts[i] = i;
      g = gr_engrave_parts (nt, parts, n, width, title, app.song.tempo, EG_MULTIRESTS);
    }
  else if (nt)
    g = gr_engrave_parts (nt, &(int) { 0 }, 1, width, title, app.song.tempo, EG_MULTIRESTS);
  startlog ("score: engraved, %d items, %d systems, %d cs", g ? g->n : -1, g ? g->nsys : 0, log_time () - t0);
  gr_notation_free (nt);
  return g;
}

int score_full (void)
{
  return full_score;
}

int score_concert (void)
{
  return concert;
}

void score_set_full (int on)
{
  full_score = on;
  score_changed ();
}

void score_set_concert (int on)
{
  concert = on;
  score_changed ();
}

int score_repeats (void)
{
  return repeats;
}

void score_set_repeats (int on)
{
  repeats = on;
  score_changed ();
}

/* Make the engraving again if anything it shows has changed. */
static void build (void)
{
  int t = app.sel_track, w = visible_width ();
  /* (Not "&& eng": with nothing to show, building again on every redraw
     asked for another redraw, for ever - the desktop froze.) */
  if (!dirty && (full_score || t == built_track) && w == built_width)
    return;
  gr_engraving_free (eng);
  eng = NULL;
  built_track = t;
  built_width = w;
  dirty = 0;
  if (full_score)
    {
      _swix (Hourglass_On, 0);
      eng = make (-1, os_z (w), NULL, 1);
      _swix (Hourglass_Off, 0);
      snprintf (title, sizeof title, "Score - Full score%s", concert ? " (concert pitch)" : "");
    }
  else if (t >= 0 && t < app.song.ntracks && app.song.tracks[t].type != TRK_AUDIO)
    {
      _swix (Hourglass_On, 0);
      eng = make (t, os_z (w), NULL, 1);
      _swix (Hourglass_Off, 0);
      snprintf (title, sizeof title, "Score - %s%s", app.song.tracks[t].name, concert ? " (concert pitch)" : "");
    }
  else
    snprintf (title, sizeof title, "Score");
  app_set_extent (app.w_score, w, eng ? z_os (eng->height) + TOP_PAD : 400);
  /* the title bar: the strip of screen above the work area, inside the
     window's outline (works on every Wimp) */
  {
    int st[9], ol[5];
    st[0] = ol[0] = app.w_score;
    if (!_swix (Wimp_GetWindowState, _IN (1), st) && !_swix (Wimp_GetWindowOutline, _IN (1), ol))
      _swix (Wimp_ForceRedraw, _INR (0, 4), -1, ol[1], st[4], ol[3], ol[4]);
  }
  app_redraw_all (app.w_score);
}

void score_open (void)
{
  dirty = 1;
  app_open_window (app.w_score, WS_SCORE);
  app.score_open = 1;
  build ();
  /* the caret, for + and - (zoom) */
  _swix (Wimp_SetCaretPosition, _INR (0, 5), app.w_score, -1, 0, 0, 1 << 25, -1);
}

void score_open_request (int *b)
{
  app_check (_swix (Wimp_OpenWindow, _IN (1), b));
  if (b[3] - b[1] != built_width)
    build ();
}

void score_close (void)
{
  _swix (Wimp_CloseWindow, _IN (1), &app.w_score);
  app.score_open = 0;
  gr_engraving_free (eng);
  eng = NULL;
}

void score_changed (void)
{
  dirty = 1;
  if (app.score_open)
    build ();
}

/* ---- drawing ------------------------------------------------------------------- */

static int *make_glyph_path (int g)
{
  const gr_glyph *gl = &gr_glyphs[g];
  const short *d = gr_glyph_data + gl->start, *end = d + gl->len;
  int *p = malloc ((size_t) (gl->len + 4) * sizeof *p), n = 0;
  if (!p)
    return NULL;
  while (d < end)
    switch (*d)
      {
      case 1: p[n++] = 2; p[n++] = d[1]; p[n++] = d[2]; d += 3; break;
      case 2: p[n++] = 8; p[n++] = d[1]; p[n++] = d[2]; d += 3; break;
      case 3:
        p[n++] = 6;
        p[n++] = d[1]; p[n++] = d[2]; p[n++] = d[3]; p[n++] = d[4]; p[n++] = d[5]; p[n++] = d[6];
        d += 7;
        break;
      default: p[n++] = 5; d++; break;
      }
  p[n++] = 0;
  p[n++] = 0;
  return p;
}

static void draw_glyph (int g, int sx, int sy, int scale)
{
  int m[6];
  double k = sp_os () * 256.0 / EG_SP * scale / 100.0 * 65536.0;
  if (!glyph_path[g] && !(glyph_path[g] = make_glyph_path (g)))
    return;
  m[0] = (int) k; m[1] = 0; m[2] = 0; m[3] = (int) k;
  m[4] = sx * 256; m[5] = sy * 256;
  _swix (Draw_Fill, _INR (0, 3), glyph_path[g], FILL_STYLE, m, 4);    /* flat to 4 font units */
}

/* A path in Draw units (1/256 OS unit) from screen OS units. */
static void fill (const int *p)
{
  _swix (Draw_Fill, _INR (0, 3), p, FILL_STYLE, 0, 64);           /* to a quarter OS unit */
}

static void draw_beam (int x0, int y0, int x1, int y1, int t)
{
  int p[16];
  p[0] = 2; p[1] = x0 * 256; p[2] = y0 * 256;
  p[3] = 8; p[4] = x1 * 256; p[5] = y1 * 256;
  p[6] = 8; p[7] = x1 * 256; p[8] = (y1 - t) * 256;
  p[9] = 8; p[10] = x0 * 256; p[11] = (y0 - t) * 256;
  p[12] = 5; p[13] = 0; p[14] = 0;
  fill (p);
}

/* A tie or slur: a crescent from x0,y0 to x1,y1, its middle h above
   (h > 0) or below them, t thick there. */
static void draw_arc (int x0, int y0, int x1, int y1, int h, int t)
{
  int p[24], len = x1 - x0;
  h = h * 4 / 3;
  t = (h > 0 ? t : -t) * 4 / 3;
  p[0] = 2; p[1] = x0 * 256; p[2] = y0 * 256;
  p[3] = 6; p[4] = (x0 + len / 4) * 256; p[5] = (y0 + h) * 256; p[6] = (x1 - len / 4) * 256;
  p[7] = (y1 + h) * 256; p[8] = x1 * 256; p[9] = y1 * 256;
  p[10] = 6; p[11] = (x1 - len / 4) * 256; p[12] = (y1 + h - t) * 256; p[13] = (x0 + len / 4) * 256;
  p[14] = (y0 + h - t) * 256; p[15] = x0 * 256; p[16] = y0 * 256;
  p[17] = 5; p[18] = 0; p[19] = 0;
  fill (p);
}

static void draw_tie (int x0, int y0, int x1, int y1, int up)
{
  int len = x1 - x0, sp = sp_os (), h = len / 5 < sp * 6 / 10 ? len / 5 : sp * 6 / 10;
  draw_arc (x0, y0, x1, y1, up ? h : -h, sp * 13 / 100 + 1);
}

/* The bar playing, as the score shows it (a bar played again is written
   once, inside repeats), or -1. */
static int playing_bar (void)
{
  int t = (int) gr_position (app.eng), b = gr_tick_bar (&app.song, t < 0 ? 0 : t);
  if (shown_bar_of)
    return b < shown_nsong ? shown_bar_of[b] : -1;
  return b;
}

/* Text on the page: the outline font, as tall as the item says. Its
   width, in OS units. */
static int print_text (int x, int y, int size, const char *s, int bold)
{
  int f, pt16 = size * 16 * 2 / 5, w = 0;    /* (2.5 OS units a point) */
  if (_swix (Font_FindFont, _INR (1, 5) | _OUT (0), bold == 1 ? "Trinity.Bold" : bold == 2 ? "Trinity.Medium.Italic" : "Trinity.Medium",
             pt16, pt16, 0, 0, &f)
      && (!bold || _swix (Font_FindFont, _INR (1, 5) | _OUT (0), "Trinity.Medium", pt16, pt16, 0, 0, &f)))
    return 0;
  _swix (ColourTrans_SetFontColours, _INR (0, 3), f, 0xFFFFFF00, 0, 14);
  _swix (Font_Paint, _INR (0, 4), f, s, (1 << 4) | (1 << 8), x, y);
  /* (how far it went: millipoints, 400 an OS unit) */
  if (!_swix (Font_ScanString, _INR (0, 4) | _OUT (3), f, s, 1 << 8, 0x7FFFFFFF, 0x7FFFFFFF, &w))
    w /= 400;
  else
    w = 0;
  _swix (Font_LoseFont, _IN (0), f);
  return w;
}

/* Text on the screen at a zoom under 100%, in Homerton as tall as the
   score has it (size, OS units), so it shrinks with the music (the
   desktop font would be four times too big at 25%); bold (style 1) or
   italic (2). Its width; -1 if there's no such font. The fonts are kept
   (a few sizes). */
static int screen_scaled (int x, int y, int size, const char *s, int style)
{
  static int fh[8] = { -1, -1, -1, -1, -1, -1, -1, -1 }, fkey[8], next;
  int pt16 = size * 16 * 2 / 5, key, i, f = -1, w = 0;    /* (2.5 OS units a point) */
  if (pt16 < 16)
    pt16 = 16;
  key = pt16 * 4 + (style & 3);
  for (i = 0; i < 8; i++)
    if (fh[i] >= 0 && fkey[i] == key)
      f = fh[i];
  if (f < 0)
    {
      if (_swix (Font_FindFont, _INR (1, 5) | _OUT (0),
                 style == 1 ? "Homerton.Bold" : style == 2 ? "Homerton.Medium.Oblique" : "Homerton.Medium",
                 pt16, pt16, 0, 0, &f))
        return -1;
      if (fh[next] >= 0)
        _swix (Font_LoseFont, _IN (0), fh[next]);
      fh[next] = f;
      fkey[next] = key;
      next = (next + 1) % 8;
    }
  _swix (ColourTrans_SetFontColours, _INR (0, 3), f,
         ((PAPER & 0xFFu) << 24) | (((PAPER >> 8) & 0xFFu) << 16) | (((PAPER >> 16) & 0xFFu) << 8),
         ((INK & 0xFFu) << 24) | (((INK >> 8) & 0xFFu) << 16) | (((INK >> 16) & 0xFFu) << 8), 14);
  _swix (Font_Paint, _INR (0, 4), f, s, (1 << 4) | (1 << 8), x, y);
  if (!_swix (Font_ScanString, _INR (0, 4) | _OUT (3), f, s, 1 << 8, 0x7FFFFFFF, 0x7FFFFFFF, &w))
    return w / 400;
  return 0;
}

/* Bold (style 1) or italic (2) text on the screen: the desktop font's
   (Homerton.Bold or .Medium.Oblique at 12 point), found once. Its width;
   -1 if there's no such font (then bold is struck twice, a pixel apart,
   and italic is upright). */
static int screen_bold (int x, int y, const char *s, int style)
{
  static int f[3] = { -1, -1, -1 }, tried[3];
  int w = 0;
  style = style == 2 ? 2 : 1;
  if (!tried[style])
    {
      tried[style] = 1;
      if (_swix (Font_FindFont, _INR (1, 5) | _OUT (0), style == 1 ? "Homerton.Bold" : "Homerton.Medium.Oblique",
                 12 * 16, 12 * 16, 0, 0, &f[style]))
        f[style] = -1;
    }
  if (f[style] < 0)
    return -1;
  _swix (ColourTrans_SetFontColours, _INR (0, 3), f[style],
         ((PAPER & 0xFFu) << 24) | (((PAPER >> 8) & 0xFFu) << 16) | (((PAPER >> 16) & 0xFFu) << 8),
         ((INK & 0xFFu) << 24) | (((INK >> 8) & 0xFFu) << 16) | (((INK >> 16) & 0xFFu) << 8), 14);
  _swix (Font_Paint, _INR (0, 4), f[style], s, (1 << 4) | (1 << 8), x, y);
  if (!_swix (Font_ScanString, _INR (0, 4) | _OUT (3), f[style], s, 1 << 8, 0x7FFFFFFF, 0x7FFFFFFF, &w))
    return w / 400;
  return 0;
}

/* A score's text at x,y (OS units): on the page (size: its height) or the
   screen (the desktop font; bold, its bold or struck twice a pixel apart); a metronome
   mark's notes in it (GR_CH_*) drawn as notes, after the letters before
   them. */
static void score_text (int x, int y, int size, const char *s, int bold, int printing, int px)
{
  char piece[160];
  size_t n = 0;
  /* (a note as tall as the letters: the desktop font's are about 30 OS units) */
  int small = !printing && sp_os () < 20;   /* (under 100%: scaled, as the music) */
  int scale = printing || small ? 60 : size > 0 ? 60 * 30 / size : 60;
  if (scale < 20) scale = 20;
  if (scale > 240) scale = 240;
  for (;; s++)
    {
      unsigned char c = (unsigned char) *s;
      if (c && (c < GR_CH_NOTE4 || c > GR_CH_NOTEDOT) && n + 1 < sizeof piece)
        {
          piece[n++] = (char) c;
          continue;
        }
      piece[n] = 0;
      if (n)
        {
          int sw;
          if (printing)
            x += print_text (x, y, size, piece, bold);
          else if (small && (sw = screen_scaled (x, y, size, piece, bold)) >= 0)
            x += sw;
          else
            {
              int bw = bold ? screen_bold (x, y, piece, bold) : -1;
              if (bw >= 0)
                x += bw;
              else
                {
                  gfx_text (x, y, piece, INK, PAPER, 0);
                  if (bold == 1)
                    gfx_text (x + px, y, piece, INK, PAPER, 0);
                  x += gfx_text_width (piece) + (bold == 1 ? px : 0);
                }
            }
        }
      n = 0;
      if (!c)
        break;
      {
        int g = c == GR_CH_NOTE2 ? GL_MET_HALF : c == GR_CH_NOTE8 ? GL_MET_8TH : c == GR_CH_NOTEDOT ? GL_MET_DOT : GL_MET_QUARTER;
        int adv = z_os (gr_glyphs[g].adv) * scale / 100;
        gfx_colour (INK);
        draw_glyph (g, x + (c == GR_CH_NOTEDOT ? 0 : adv / 6), y, scale);
        x += adv + (c == GR_CH_NOTEDOT ? 0 : adv / 3);
      }
    }
  gfx_colour (INK);
}

/* The engraving g, its top left at ox,oy, within clip: in the window, or
   (printing) on a page, without the paper or the playing bar. */
static void paint_eng (const gr_engraving *g, int ox, int oy, const int *clip)
{
  int i, px = print_sp ? 1 : 1 << app.xeig, py = print_sp ? 1 : 1 << app.yeig, bar = print_sp ? -1 : playing_bar ();
  const gr_engraving *eng = g;
  /* the playing bar */
  for (i = 0; i < eng->n; i++)
    if (eng->it[i].kind == EK_BAR && eng->it[i].w == bar)
      {
        gfx_colour (HILITE);
        gfx_rect (ox + z_os (eng->it[i].x0), oy + z_os (eng->it[i].y0), ox + z_os (eng->it[i].x1),
                  oy + z_os (eng->it[i].y1));
      }
  gfx_colour (INK);
  for (i = 0; i < eng->n; i++)
    {
      const gr_eitem *it = &eng->it[i];
      int x0 = ox + z_os (it->x0), y0 = oy + z_os (it->y0), x1 = ox + z_os (it->x1), y1 = oy + z_os (it->y1);
      int lo = y0 < y1 ? y0 : y1, hi = y0 > y1 ? y0 : y1;
      if (it->kind == EK_GLYPH || it->kind == EK_TEXT)
        lo -= sp_os () * 16, hi += sp_os () * 16;   /* (a brace or clef is tall) */
      if (it->kind == EK_SLUR)
        lo -= z_os (abs (it->w)), hi += z_os (abs (it->w));
      if (hi < clip[1] - 64 || lo > clip[3] + 64)
        continue;
      switch (it->kind)
        {
        case EK_GLYPH:
          draw_glyph (it->glyph, x0, y0, it->w);
          break;
        case EK_LINE:
          {
            int t = z_os (it->w);
            if (y0 == y1)
              {
                if (t < py) t = py;
                gfx_rect (x0, y0 - t / 2, x1, y0 - t / 2 + t);
              }
            else
              {
                if (t < px) t = px;
                gfx_rect (x0 - t / 2, lo, x0 - t / 2 + t, hi);
              }
          }
          break;
        case EK_BEAM:
          draw_beam (x0, y0, x1, y1, z_os (it->w));
          break;
        case EK_TIE:
          draw_tie (x0, y0, x1, y1, it->w > 0);
          break;
        case EK_SLUR:
          draw_arc (x0, y0, x1, y1, z_os (it->w), sp_os () * 16 / 100 + 1);
          break;
        case EK_TEXT:
          score_text (x0, y0, z_os (it->y1), eng->text + it->w, it->glyph, print_sp != 0, px);
          gfx_colour (INK);
          break;
        }
    }
}

/* On a 16 or 32-bit screen (what a Pi gives): the music anti-aliased
   (engine/raster.c), a strip of rows at a time, as a 32bpp sprite; the
   text over it, in the desktop font. 0 if it can't (a screen of 256
   colours or fewer, no memory): Draw draws it instead. */
#define STRIP_ROWS 64

static unsigned ct_word (unsigned rgb)     /* &RRGGBB to ColourTrans's &BBGGRR00 */
{
  return ((rgb & 0xFF) << 24) | (((rgb >> 8) & 0xFF) << 16) | (((rgb >> 16) & 0xFF) << 8);
}

static int paint_smooth (int ox, int oy, const int *clip)
{
  int l2bpp = 0, pw = 1 << app.xeig, ph = 1 << app.yeig, x0, y0, y1, w, rows, top, i, bar = playing_bar ();
  int *area;
  double scale;
  static void *table;               /* (256 colours or fewer: true colour to the palette) */
  static int table_bpp = -1;
  void *tab = NULL;
  if (app.xeig != app.yeig || _swix (OS_ReadModeVariable, _INR (0, 1) | _OUT (2), -1, 9, &l2bpp))
    return 0;
  x0 = clip[0] & ~(pw - 1);
  y0 = clip[1] & ~(ph - 1);
  y1 = clip[3];
  w = (clip[2] - x0 + pw - 1) / pw;
  rows = (y1 - y0 + ph - 1) / ph;
  if (w <= 0 || rows <= 0)
    return 1;
  if (paints < 3)
    startlog ("score: paint %d x %d pixels, %d bpp", w, rows, 1 << l2bpp);
  area = malloc (16 + 44 + (size_t) w * STRIP_ROWS * 4);
  if (!area)
    return 0;
  scale = (double) sp_os () / EG_SP / pw;
  top = y0 + rows * ph;
  if (l2bpp < 4)
    {
      /* ColourTrans's table from a 32bpp sprite to the screen's colours,
         made once for the screen's depth */
      if (table_bpp != l2bpp)
        {
          int size = 0, mode = (6 << 27) | (90 << 14) | (90 << 1) | 1;
          free (table);
          table = NULL;
          table_bpp = l2bpp;
          if (!_swix (ColourTrans_SelectTable, _INR (0, 5) | _OUT (4), mode, -1, -1, -1, 0, 0, &size) && size > 0
              && (table = malloc ((size_t) size))
              && _swix (ColourTrans_SelectTable, _INR (0, 5), mode, -1, -1, -1, table, 0))
            {
              free (table);
              table = NULL;
            }
        }
      if (!table)
        {
          free (area);
          return 0;
        }
      tab = table;
    }
  for (i = 0; i < rows; i += STRIP_ROWS)
    {
      int hs = rows - i < STRIP_ROWS ? rows - i : STRIP_ROWS, stop = top - i * ph, dpi = 180 >> app.xeig;
      int *spr = area + 4;
      area[0] = 16 + 44 + w * hs * 4;
      area[1] = 1;
      area[2] = 16;
      area[3] = area[0];
      spr[0] = 44 + w * hs * 4;
      memset (&spr[1], 0, 12);
      memcpy (&spr[1], "score", 5);
      spr[4] = w - 1;
      spr[5] = hs - 1;
      spr[6] = 0;
      spr[7] = 31;
      spr[8] = 44;
      spr[9] = 44;
      spr[10] = (6 << 27) | (dpi << 14) | (dpi << 1) | 1;
      if (gr_raster (eng, scale, (double) (ox - x0) / pw, (double) (stop - oy) / ph, (uint32_t *) (spr + 11), w, hs,
                     ct_word (PAPER), ct_word (INK), ct_word (HILITE), bar, 1.0))
        {
          free (area);
          return 0;
        }
      _swix (OS_SpriteOp, _INR (0, 7), 512 + 52, area, spr, x0, stop - hs * ph, 0, 0, tab);
    }
  free (area);
  if (paints < 3 && ++paints)
    startlog ("score: painted");
  for (i = 0; i < eng->n; i++)
    if (eng->it[i].kind == EK_TEXT)
      {
        int ty = oy + z_os (eng->it[i].y0);
        if (ty > clip[1] - 64 && ty < clip[3] + 64)
          score_text (ox + z_os (eng->it[i].x0), ty, z_os (eng->it[i].y1), eng->text + eng->it[i].w,
                      eng->it[i].glyph, 0, pw);
      }
  return 1;
}

static void paint (int ox, int oy, const int *clip)
{
  if (eng && paint_smooth (ox, oy - TOP_PAD, clip))
    return;
  gfx_colour (PAPER);
  gfx_rect (clip[0], clip[1], clip[2], clip[3]);
  if (!eng)
    {
      const char *msg = full_score ? "There are no notes to show yet."
        : app.sel_track >= 0 && app.sel_track < app.song.ntracks
        ? "An audio track has no notes to show." : "Select a drum or instrument track to see it as a score.";
      gfx_text (ox + 32, oy - 64, msg, DIM, PAPER, 0);
      return;
    }
  paint_eng (eng, ox, oy - TOP_PAD, clip);
}

void score_redraw (int *b)
{
  int more;
  build ();
  _swix (Wimp_RedrawWindow, _IN (1) | _OUT (0), b, &more);
  while (more)
    {
      paint (b[1] - b[5], b[4] - b[6], &b[7]);
      _swix (Wimp_GetRectangle, _IN (1) | _OUT (0), b, &more);
    }
}

/* ---- the playhead ------------------------------------------------------------- */

static void redraw_bar (int bar)
{
  int i;
  for (i = 0; eng && i < eng->n; i++)
    if (eng->it[i].kind == EK_BAR && eng->it[i].w == bar)
      app_redraw_work (app.w_score, z_os (eng->it[i].x0) - 8, z_os (eng->it[i].y0) - TOP_PAD - 8,
                       z_os (eng->it[i].x1) + 8, z_os (eng->it[i].y1) - TOP_PAD + 8);
}

/* Keeps the playing bar in view while playing. */
static void follow (int bar)
{
  int i, b[9];
  b[0] = app.w_score;
  if (!eng || _swix (Wimp_GetWindowState, _IN (1), b))
    return;
  for (i = 0; i < eng->n; i++)
    if (eng->it[i].kind == EK_BAR && eng->it[i].w == bar)
      {
        int top = z_os (eng->it[i].y1) - TOP_PAD, bot = z_os (eng->it[i].y0) - TOP_PAD;
        int vis_h = b[4] - b[2];
        if (top > b[6] || bot < b[6] - vis_h)
          {
            b[6] = top + 32;
            if (b[6] > 0)
              b[6] = 0;
            _swix (Wimp_OpenWindow, _IN (1), b);
          }
        return;
      }
}

void score_tick (void)
{
  int bar;
  if (!app.score_open)
    return;
  if (app.sel_track != last_sel)
    {
      last_sel = app.sel_track;
      if (app.sel_track != built_track && !full_score)
        score_changed ();
    }
  bar = playing_bar ();
  if (bar != shown_bar)
    {
      redraw_bar (shown_bar);
      redraw_bar (bar);
      shown_bar = bar;
      if (gr_playing (app.eng) && app.follow)
        follow (bar);
    }
}

/* ---- clicks and keys ---------------------------------------------------------- */

void score_click (int *b)
{
  int ox, oy, bar;
  if (b[2] == BUT_MENU)
    {
      app_score_menu (b[0] - 64, b[1]);
      return;
    }
  _swix (Wimp_SetCaretPosition, _INR (0, 5), app.w_score, -1, 0, 0, 1 << 25, -1);
  if (!eng)
    return;
  app_window_origin (app.w_score, &ox, &oy);
  bar = gr_engraving_bar_at (eng, os_z (b[0] - ox), os_z (b[1] - oy + TOP_PAD));
  if (bar >= 0 && (b[2] & BUT_SELECT))
    {
      gr_locate (app.eng, shown_start && bar < shown_nbars ? shown_start[bar] : gr_bar_tick (&app.song, bar));
      score_tick ();
      app_redraw_all (app.w_tool);
    }
}

/* Zoom i (of score_zoom_pc's): laid out again for the window's width at
   that size, the same part of the score still in view. */
void score_set_zoom (int i)
{
  int st[9];
  double at = 0;
  if (i < 0 || i >= NZOOMS || i == zoom)
    return;
  st[0] = app.w_score;
  if (app.score_open && eng && !_swix (Wimp_GetWindowState, _IN (1), st))
    at = (double) -st[6] / (z_os (eng->height) + TOP_PAD);
  zoom = i;
  dirty = 1;
  build ();
  if (app.score_open && eng && !_swix (Wimp_GetWindowState, _IN (1), st))
    {
      st[6] = -(int) (at * (z_os (eng->height) + TOP_PAD));
      _swix (Wimp_OpenWindow, _IN (1), st);
    }
}

int score_key (int code)
{
  if (code == '+' || code == '=')
    score_set_zoom (zoom + 1);
  else if (code == '-' || code == '_')
    score_set_zoom (zoom - 1);
  else
    return 0;
  return 1;
}

/* Zoom i's percentage, or -1 past the last. */
int score_zoom_pc (int i)
{
  return i >= 0 && i < NZOOMS ? zooms[i] * 100 / 20 : -1;
}

int score_zoom (void)
{
  return zoom;
}

/* The wheel over the Score window (a Scroll_Request): with Ctrl held, a
   size larger or smaller; else up and down (a notch three staff spaces'
   worth, a page at a time with the scroll bar's page arrows). */
void score_scroll (int *b)
{
  int ctrl = 0;
  _swix (OS_Byte, _INR (0, 1) | _OUT (1), 121, 0x81, &ctrl);
  if (ctrl && b[9])
    {
      score_set_zoom (zoom + (b[9] > 0 ? 1 : -1));
      return;
    }
  {
    int h = b[4] - b[2], line = zooms[zoom] * 3 < 32 ? 32 : zooms[zoom] * 3;
    b[6] += b[9] == 2 || b[9] == -2 ? (b[9] > 0 ? h : -h) : b[9] * line;
    _swix (Wimp_OpenWindow, _IN (1), b);
  }
}

/* ---- printing --------------------------------------------------------------------- */

#define PRINT_SP   64              /* OS units a staff space, as drawn for the printer */
#define PAGE_SP    4961            /* millipoints a staff space on paper (1.75mm) */
#define MARGIN_MP  42520           /* 15mm, at least, round the page */

/* The song's name, for titles: its file's leaf name, or "". */
static const char *song_name (void)
{
  const char *leaf = strrchr (app.path, '.');
  return leaf ? leaf + 1 : app.path;
}

/* Prints scores (n of them, made by make() for tracks[]: -1 the full
   score) on the printer !Printers has set up, one job: each laid out to
   the page and starting on a new one, as many pages as it takes. */
static void print_scores (const int *tracks, int n)
{
  static _kernel_oserror cant_open = { 0, "Groove can't open the printer" };
  int pw, ph, l, b, r, tp, fh = 0, old = 0, wu, hu, i, s, page = 0;
  double k = (double) PAGE_SP / (PRINT_SP * 400.0);     /* (400 millipoints an OS unit) */
  _kernel_oserror *err = NULL;
  if (_swix (PDriver_Info, 0)
      || _swix (PDriver_PageSize, _OUT (1) | _OUT (2) | _OUT (3) | _OUT (4) | _OUT (5) | _OUT (6), &pw, &ph, &l, &b, &r, &tp))
    {
      app_error ("There's no printer to print on. Open !Printers (in Printing, on your hard disc), "
                 "choose your printer, then try again.");
      return;
    }
  /* the area to print in: the printable area, at least 15mm in */
  if (l < MARGIN_MP) l = MARGIN_MP;
  if (b < MARGIN_MP) b = MARGIN_MP;
  if (r > pw - MARGIN_MP) r = pw - MARGIN_MP;
  if (tp > ph - MARGIN_MP) tp = ph - MARGIN_MP;
  wu = (int) ((double) (r - l) / PAGE_SP * EG_SP);
  hu = (int) ((double) (tp - b) / PAGE_SP * EG_SP);
  if (wu < EG_SP * 20 || hu < EG_SP * 20)
    {
      app_error ("The printer's page is too small for a score.");
      return;
    }
  _swix (Hourglass_On, 0);
  err = _swix (OS_Find, _INR (0, 1) | _OUT (0), 0x80, "printer:", &fh);
  if (!err && !fh)
    err = &cant_open;
  if (!err)
    err = _swix (PDriver_SelectJob, _INR (0, 1) | _OUT (0), fh, *song_name () ? song_name () : "Groove score", &old);
  if (!err)
    {
      /* (a PostScript printer needs to know the fonts) */
      _swix (PDriver_DeclareFont, _INR (0, 2), 0, "Trinity.Medium", 0);
      _swix (PDriver_DeclareFont, _INR (0, 2), 0, 0, 0);
    }
  for (s = 0; s < n && !err; s++)
    {
      char name[96];
      const char *what = tracks[s] < 0 ? "Full score" : app.song.tracks[tracks[s]].name;
      gr_engraving *g;
      int *page_top, *page_bot, npages = 0;
      if (*song_name ())
        snprintf (name, sizeof name, "%.40s - %s", song_name (), what);
      else
        snprintf (name, sizeof name, "%s", what);
      g = make (tracks[s], wu, name, 0);
      if (!g || !g->nsys)
        {
          gr_engraving_free (g);
          continue;
        }
      page_top = malloc ((size_t) g->nsys * sizeof *page_top);
      page_bot = malloc ((size_t) g->nsys * sizeof *page_bot);
      if (!page_top || !page_bot)
        {
          free (page_top);
          free (page_bot);
          gr_engraving_free (g);
          err = &cant_open;
          break;
        }
      /* pages: whole systems, as many as fit (the first with the title) */
      for (i = 0; i < g->nsys;)
        {
          int top = npages ? g->sys_top[i] : 0, j = i;
          while (j + 1 < g->nsys && top - g->sys_bot[j + 1] <= hu - EG_SP * 3)
            j++;
          page_top[npages] = top;
          page_bot[npages++] = g->sys_bot[j];
          i = j + 1;
        }
      print_sp = PRINT_SP;
      for (i = 0; i < npages && !err; i++)
        {
          int rect[4], m[4], org[2], block[4], more = 0, id = 0;
          int hos = z_os (page_top[i] - page_bot[i]), wos = z_os (wu), fos = z_os (EG_SP * 2);
          char foot[128];
          /* (under the music: the score's name and the page, if it has more than one) */
          if (npages > 1)
            snprintf (foot, sizeof foot, "%s - page %d of %d", what, i + 1, npages);
          else
            foot[0] = 0;
          rect[0] = 0; rect[1] = -hos; rect[2] = wos; rect[3] = 0;
          m[0] = m[3] = (int) (k * 65536); m[1] = m[2] = 0;
          org[0] = l;
          org[1] = tp - (int) (hos * 400.0 * k);
          err = _swix (PDriver_GiveRectangle, _INR (0, 4), 0, rect, m, org, 0xFFFFFF00);
          if (!err && foot[0])
            {
              /* (and a strip at the foot of the page for its number) */
              rect[0] = 0; rect[1] = 0; rect[2] = wos; rect[3] = fos;
              org[1] = b;
              err = _swix (PDriver_GiveRectangle, _INR (0, 4), 1, rect, m, org, 0xFFFFFF00);
            }
          if (!err)
            err = _swix (PDriver_DrawPage, _INR (0, 3) | _OUT (0) | _OUT (2), 1, block, ++page, 0, &more, &id);
          while (!err && more)
            {
              if (id == 0)
                paint_eng (g, 0, -z_os (page_top[i]), block);
              else
                {
                  int size = z_os (EG_SP * 6 / 5);
                  (void) print_text (wos / 2 - (int) strlen (foot) * size / 4, size / 2, size, foot, 0);
                }
              err = _swix (PDriver_GetRectangle, _IN (1) | _OUT (0) | _OUT (2), block, &more, &id);
            }
        }
      print_sp = 0;
      free (page_top);
      free (page_bot);
      gr_engraving_free (g);
    }
  if (fh)
    {
      if (err)
        _swix (PDriver_AbortJob, _IN (0), fh);
      else
        err = _swix (PDriver_EndJob, _IN (0), fh);
      _swix (OS_Find, _INR (0, 1), 0, fh);
      if (old)
        _swix (PDriver_SelectJob, _INR (0, 1), old, 0);
    }
  _swix (Hourglass_Off, 0);
  if (err)
    app_check (err);
}

/* Prints what the Score window shows: the full score, or the selected
   track's part. */
void score_print (void)
{
  int t = app.sel_track;
  if (full_score)
    t = -1;
  else if (t < 0 || t >= app.song.ntracks || app.song.tracks[t].type == TRK_AUDIO)
    {
      app_error ("Select a drum or instrument track to print it as a score.");
      return;
    }
  print_scores (&t, 1);
}

/* Prints every drum and instrument track with notes as its own part. */
void score_print_parts (void)
{
  int tracks[GR_MAX_TRACKS], n = 0, t;
  gr_notation *nt = gr_notation_make (&app.song, -1, 1);
  for (t = 0; nt && t < nt->nparts; t++)
    tracks[n++] = nt->parts[t].track;
  gr_notation_free (nt);
  if (!n)
    {
      app_error ("There are no notes to print yet.");
      return;
    }
  print_scores (tracks, n);
}

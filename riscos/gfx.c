/* Groove - drawing. OS_Plot for shapes, ColourTrans for colours,
   Wimp_TextOp for text in the desktop font (so it follows the user's
   choice of font and anti-aliasing, like every other desktop program). */
#include <string.h>
#include <kernel.h>
#include <swis.h>
#include "app.h"
#include "gfx.h"

/* OS_Plot codes (PRM 1-588): move, line, triangle, rectangle, circle;
   "+ 5" = absolute coordinates, in the foreground colour. */
#define PLOT_MOVE     4
#define PLOT_LINE     5
#define PLOT_TRIANGLE (80 + 5)
#define PLOT_RECT     (96 + 5)
#define PLOT_CIRCLE   (152 + 5)

static unsigned palette_word (unsigned rgb)       /* 0xRRGGBB -> &BBGGRR00 */
{
  return ((rgb & 0xFF) << 24) | ((rgb & 0xFF00) << 8) | ((rgb >> 8) & 0xFF00);
}

void gfx_colour (unsigned rgb)
{
  /* R3 = 0: foreground, no dithering; R4 = 0: GCOL action "set". */
  _swix (ColourTrans_SetGCOL, _IN (0) | _INR (3, 4), palette_word (rgb), 0, 0);
}

static void plot (int code, int x, int y)
{
  _swix (OS_Plot, _INR (0, 2), code, x, y);
}

void gfx_rect (int x0, int y0, int x1, int y1)
{
  if (x1 <= x0 || y1 <= y0)
    return;
  plot (PLOT_MOVE, x0, y0);
  plot (PLOT_RECT, x1 - 1, y1 - 1);              /* OS_Plot includes the far edge */
}

void gfx_line (int x0, int y0, int x1, int y1)
{
  plot (PLOT_MOVE, x0, y0);
  plot (PLOT_LINE, x1, y1);
}

void gfx_frame (int x0, int y0, int x1, int y1)
{
  int px = 1 << app.xeig, py = 1 << app.yeig;
  gfx_rect (x0, y0, x1, y0 + py);
  gfx_rect (x0, y1 - py, x1, y1);
  gfx_rect (x0, y0, x0 + px, y1);
  gfx_rect (x1 - px, y0, x1, y1);
}

void gfx_tri (int x0, int y0, int x1, int y1, int x2, int y2)
{
  plot (PLOT_MOVE, x0, y0);
  plot (PLOT_MOVE, x1, y1);
  plot (PLOT_TRIANGLE, x2, y2);
}

void gfx_circle (int cx, int cy, int r)
{
  plot (PLOT_MOVE, cx, cy);
  plot (PLOT_CIRCLE, cx + r, cy);
}

/* A rounded rectangle: two rectangles in a cross and four corner circles. */
void gfx_round (int x0, int y0, int x1, int y1, int r)
{
  if (2 * r > x1 - x0) r = (x1 - x0) / 2;
  if (2 * r > y1 - y0) r = (y1 - y0) / 2;
  if (r <= 2)
    {
      gfx_rect (x0, y0, x1, y1);
      return;
    }
  gfx_rect (x0 + r, y0, x1 - r, y1);
  gfx_rect (x0, y0 + r, x1, y1 - r);
  gfx_circle (x0 + r, y0 + r, r);
  gfx_circle (x1 - r - 1, y0 + r, r);
  gfx_circle (x0 + r, y1 - r - 1, r);
  gfx_circle (x1 - r - 1, y1 - r - 1, r);
}

int gfx_text_width (const char *s)
{
  int w = 0;
  /* R2 = the number of characters to measure. */
  _swix (Wimp_TextOp, _INR (0, 2) | _OUT (0), 1, s, (int) strlen (s), &w);
  return w;
}

void gfx_text (int x, int y, const char *s, unsigned fg, unsigned bg, int align)
{
  /* Wimp_TextOp 0 sets the colours (both are needed for anti-aliasing);
     2 paints with bit 30 set: y is the baseline. */
  _swix (Wimp_TextOp, _INR (0, 2), 0, palette_word (fg), palette_word (bg));
  if (align == 1)
    x -= gfx_text_width (s) / 2;
  else if (align == 2)
    x -= gfx_text_width (s);
  _swix (Wimp_TextOp, _INR (0, 5), 2 | (1u << 30), s, -1, -1, x, y);
}

int gfx_visible (const int *clip, int x0, int y0, int x1, int y1)
{
  return x1 > clip[0] && x0 < clip[2] && y1 > clip[1] && y0 < clip[3];
}

/* ---- track colours ----------------------------------------------------------- */

unsigned gfx_track_colour (gr_track_type type, int patch, int gm, int dark)
{
  /* GM family (0..15) -> colour group; Groove's sounds by their nearest family. */
  static const unsigned char group_of[16] = { 0, 0, 0, 1, 2, 3, 3, 4, 4, 4, 5, 6, 6, 1, 5, 7 };
  static const unsigned char family_of_patch[PATCH_COUNT] = { 0, 4, 11, 10, 3, 2, 1 };
  static const unsigned light[8] = { 0x5BBF6A, 0xE8913A, 0x9B6BE0, 0x3FC1B0, 0xE0694A, 0xE860A8, 0xC9A0F0, 0xA0A4AC };
  static const unsigned deep[8]  = { 0x2D6E38, 0x8A4E14, 0x553089, 0x1E6E64, 0x84321E, 0x8A2B5E, 0x6E4F94, 0x5A5E66 };
  int fam, g;
  if (type == TRK_DRUMS) return dark ? C_DRUMS_DK : C_DRUMS;
  if (type == TRK_AUDIO) return dark ? C_AUDIO_DK : C_AUDIO;
  fam = gm >= 0 ? (gm & 127) / 8 : patch >= 0 && patch < PATCH_COUNT ? family_of_patch[patch] : 0;
  g = group_of[fam];
  return dark ? deep[g] : light[g];
}

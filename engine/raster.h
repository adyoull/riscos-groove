/* Groove - drawing an engraving (engrave.h) into pixels, anti-aliased
   (raster.c), for the Score window: each symbol, line, beam, tie and
   slur filled with its edge pixels shaded by how much of them it covers,
   so the music looks smooth rather than blocky. Text isn't drawn here
   (the Score window writes it with the desktop font).

   The picture is a block of 32-bit pixels, &00BBGGRR (a RISC OS 32bpp
   sprite's), w by h, top row first. An engraving point (x, y) lands on
   pixel (x * scale + ox, oy - y * scale): ox, oy place the engraving's
   origin in the block; scale is pixels per engraving unit. */
#ifndef RASTER_H
#define RASTER_H
#include <stdint.h>
#include "engrave.h"

/* paper, ink, hilite: &BBGGRR00 colours (as ColourTrans takes them).
   hilite_bar: the bar to show lit (its box in hilite), or -1. min_line:
   the thinnest a line is drawn, in pixels (lines thinner than a pixel
   look faint). Returns -1 if there's no memory. */
int gr_raster (const gr_engraving *e, double scale, double ox, double oy, uint32_t *px, int w, int h,
               unsigned paper, unsigned ink, unsigned hilite, int hilite_bar, double min_line);

#endif

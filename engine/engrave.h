/* Groove - laying out written music (engrave.c): one part of a song's
   notation (notation.h) as systems of staves to fit a width, as a list of
   things to draw. The Score window draws the list (riscos/score.c); the
   tests draw it as SVG.

   Units: a staff space is 250 (as the music font's, glyphs.h). x runs
   right from 0; y runs up, from 0 at the top of the score down into
   negative numbers. */
#ifndef ENGRAVE_H
#define ENGRAVE_H
#include "notation.h"

#define EG_SP 250                /* a staff space */

enum
{
  EK_GLYPH,                     /* glyph (GL_*) at x0,y0, scaled by w/100 */
  EK_LINE,                      /* from x0,y0 to x1,y1, w thick (a filled rectangle if level or upright) */
  EK_BEAM,                      /* a beam from x0,y0 to x1,y1 (its top edge), w thick */
  EK_TIE,                       /* a tie (a thin crescent) from x0,y0 to x1,y1, bulging up if w > 0 */
  EK_TEXT,                      /* text (score->text + w) with its baseline's left at x0,y0, height y1;
                                   bold if glyph is 1, italic if 2; bytes GR_CH_NOTE4.. in it
                                   are a metronome mark's notes (GL_MET_*) */
  EK_SLUR,                      /* a slur from x0,y0 to x1,y1, its middle |w| above them (w > 0) or below */
  EK_BAR                        /* bar w's box, x0,y0 (bottom left) to x1,y1: for the playhead and clicks */
};

typedef struct
{
  unsigned char kind, glyph;
  int x0, y0, x1, y1, w;
} gr_eitem;

typedef struct
{
  gr_eitem *it;
  int n, cap;
  char *text;
  int tlen, tcap;
  int width, height;            /* the score's size (height is positive) */
  int nsys;                     /* its systems (lines of music), top down: */
  int *sys_top, *sys_bot;       /* where each starts and ends (y), for pages */
} gr_engraving;

/* Lays out part `part` of nt in systems `width` wide (bars of rest one
   after another as multi-bar rests, as a part is written). title: shown
   above the first system (or NULL); tempo: crotchets a minute, shown as a
   metronome mark. NULL if there's no memory. */
gr_engraving *gr_engrave (const gr_notation *nt, int part, int width, const char *title, int tempo);

/* The same for several parts (nparts of them, their numbers in parts[])
   together, as a full score: their staves in each system, the notes
   lined up, the parts' names down the left of the first. */
#define EG_MULTIRESTS 1         /* bars where every part rests: one multi-bar rest */
gr_engraving *gr_engrave_parts (const gr_notation *nt, const int *parts, int nparts, int width, const char *title,
                                int tempo, int flags);
void gr_engraving_free (gr_engraving *e);

/* The bar at (x, y) in the score, or -1. */
int gr_engraving_bar_at (const gr_engraving *e, int x, int y);

#endif

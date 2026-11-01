/* Groove - drawing in a redraw loop. Colours are 0xRRGGBB; coordinates
   are screen OS units. Rectangles take (x0,y0) inclusive and (x1,y1)
   exclusive, as the Wimp's boxes do. */
#ifndef GFX_H
#include "../engine/groove.h"

#define GFX_H

void gfx_colour (unsigned rgb);
void gfx_rect (int x0, int y0, int x1, int y1);
void gfx_frame (int x0, int y0, int x1, int y1);
void gfx_round (int x0, int y0, int x1, int y1, int r);
void gfx_line (int x0, int y0, int x1, int y1);
void gfx_tri (int x0, int y0, int x1, int y1, int x2, int y2);
void gfx_circle (int cx, int cy, int r);
/* Desktop-font text with its baseline at y. align: 0 left, 1 centre, 2 right. */
void gfx_text (int x, int y, const char *s, unsigned fg, unsigned bg, int align);
int  gfx_text_width (const char *s);
/* Is the box visible in the rectangle being redrawn (clip = x0 y0 x1 y1)? */
int  gfx_visible (const int *clip, int x0, int y0, int x1, int y1);

/* The palette: a dark studio look, readable in any colour mode. */
#define C_BG        0x26272B
#define C_LANE      0x2E3035
#define C_LANE_ALT  0x2A2C30
#define C_GRID      0x3A3D44
#define C_GRID_BAR  0x50545C
#define C_HEADER    0x3B3E45
#define C_HEADER_SEL 0x4C5260
#define C_TEXT      0xE8E8EC
#define C_TEXT_DIM  0x9A9EA8
#define C_RULER     0x34363C
#define C_PLAYHEAD  0xFFFFFF
#define C_CYCLE     0xE8B33C
#define C_DRUMS     0xE8C547
#define C_DRUMS_DK  0x8A7420
#define C_INST      0x5BBF6A
#define C_INST_DK   0x2D6E38
#define C_AUDIO     0x4A90E2
#define C_AUDIO_DK  0x23518A
#define C_RECORD    0xE5484D
/* A track's colour by what it plays: drums, audio, or the instrument's
   family (Groove's own sound, or the SoundFont's). dark: the deeper shade. */
unsigned gfx_track_colour (gr_track_type type, int patch, int gm, int dark);
#define C_MUTE      0x4FA3E8
#define C_SOLO      0xF0C43C
#define C_BUTTON    0x4A4D55
#define C_BUTTON_ON 0x6A7080
#define C_LCD       0x1B2B22
#define C_LCD_TEXT  0x8BF5A8

#endif

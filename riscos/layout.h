/* Groove - where things are on screen, and what a click hits.

   Pure arithmetic on OS units, with no SWIs, so it is tested on Linux
   (host/test_layout.c). All y values are work-area coordinates: 0 at the
   top of the window's work area and negative going down, as the Wimp has
   them. */
#ifndef LAYOUT_H
#define LAYOUT_H

#include "../engine/groove.h"

/* ---- the song window (arranger) --------------------------------------- */
#define TOOL_H     104          /* the toolbar pane over the top of it */
#define RULER_H    44
#define TRACK_H    112
#define HDR_W      360          /* track headers on the left */
#define EDGE_W     20           /* grab zone at a region's right end */
#define LANES_TOP  (-(TOOL_H + RULER_H))

typedef struct
{
  int hscroll;                  /* tick at the left edge of the timeline */
  int bar_w;                    /* OS units per bar (the zoom) */
  int width;                    /* visible width of the window */
} lay_view;

int lay_tick_to_x (const lay_view *v, int tick);   /* work-area x */
int lay_x_to_tick (const lay_view *v, int x);
int lay_snap (int tick, int grid);                  /* to the nearest */
int lay_track_top (int track);                      /* y of its top edge */
int lay_work_height (int ntracks);

enum
{
  HIT_NONE, HIT_RULER, HIT_EMPTY_LANE, HIT_REGION, HIT_REGION_END,
  HIT_NAME, HIT_MUTE, HIT_SOLO, HIT_ARM, HIT_VOLUME, HIT_PAN, HIT_ADD_TRACK
};

typedef struct { int what, track, region, tick; } lay_hit;

/* Header buttons, relative to the track's top-left corner. */
typedef struct { int x0, y0, x1, y1; } lay_box;
lay_box lay_header_box (int what);                 /* HIT_MUTE .. HIT_PAN */

lay_hit lay_arranger_hit (const lay_view *v, const gr_song *s, int x, int y);

/* ---- the toolbar ------------------------------------------------------- */
enum
{
  TB_REWIND, TB_PLAY, TB_RECORD, TB_CYCLE, TB_LCD, TB_TEMPO_DOWN, TB_TEMPO,
  TB_TEMPO_UP, TB_KEY, TB_METRONOME, TB_ZOOM_OUT, TB_ZOOM_IN, TB_LOOPS,
  TB_EDITOR, TB_KEYS, TB_MASTER, TB_COUNT
};
lay_box lay_toolbar_box (int button);              /* y from 0 down to -TOOL_H */
int lay_toolbar_hit (int x, int y);                /* TB_* or -1 */

/* ---- the editor -------------------------------------------------------- */
#define ED_HEAD_H  56           /* the strip with the region's name and sound */
#define ED_LABEL_W lay_label_w  /* drum names ("49 Crash Cymbal 1") / piano keys */
#define ED_LABEL_DRUMS 384
#define ED_LABEL_KEYS  140
extern int lay_label_w;         /* the editor sets it for the track it shows */
#define ED_DRUM_H  lay_drum_h  /* a step-grid row: grows to fill the editor */
#define ED_DRUM_MIN 56
#define ED_DRUM_ALL_H 32        /* rows when all 128 drum keys are shown */
extern int lay_drum_h;
extern int lay_drum_rows;       /* rows in the step grid (the editor sets it) */
#define ED_KEY_H   20           /* a piano-roll row */
#define ED_LANE_H  240          /* the controller lane, pinned to the bottom */
#define ED_LANE_BAR 48          /* its top strip: the controller's name */
#define ED_LOW     0            /* lowest note shown (C-1) */
#define ED_HIGH    127          /* highest (G9): all of MIDI's notes */

int lay_editor_height (gr_track_type t);
int lay_step_w (int width, int len_ticks);         /* grid column width, >= 8 */
/* Step grid: returns 1 and the row (drum sound) and step under (x,y). */
int lay_grid_hit (int width, int len, int x, int y, int *row, int *step);
/* Piano roll: returns 1 and the pitch and tick under (x,y). */
int lay_roll_hit (int width, int len, int x, int y, int *pitch, int *tick);
int lay_roll_y (int pitch);                        /* top edge of a pitch's row */
int lay_roll_x (int width, int len, int tick);

/* ---- the loop browser ------------------------------------------------- */
#define LB_ROW_H   48
#define LB_HEAD_H  40           /* a category heading */
#define LB_W       560
/* The y of loop i's row (top edge), with the category headings counted in. */
int lay_browser_row_y (int i);
int lay_browser_hit (int y);                       /* loop index or -1 */
int lay_browser_height (void);

/* ---- the workspace: where the windows open -------------------------- */
/* Given the screen size (OS units), the visible area (screen OS units,
   x0 y0 inclusive, x1 y1 exclusive, as Wimp_OpenWindow takes it) each
   window first opens at. Like one big window, made of RISC OS
   windows: the song window across the top, the editor below it, the loop
   browser as a column on the right, the keyboard over the editor's left
   end; clear of the icon bar, with room for each window's title bar and
   scroll bar. */
enum { WS_SONG, WS_EDITOR, WS_LOOPS, WS_KEYS };
#define WS_MARGIN   16
#define WS_ICONBAR  136         /* the icon bar's height, and a little */
#define WS_TITLE    44          /* a title bar */
#define WS_SCROLL   44          /* a vertical scroll bar */
#define WS_KEYS_W   1312        /* the keyboard window's size (keys.c: 10 keys of 128, + 32) */
#define WS_KEYS_H   408         /* 64 + 320 + 24 */
lay_box lay_workspace (int screen_w, int screen_h, int which);
/* Drum rows tall enough to fill an editor this high (visible area). */
int lay_fit_drum_rows (int editor_h);

#endif

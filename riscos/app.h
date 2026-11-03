/* Groove - the RISC OS front end: shared state and the calls between the
   window modules.

     main.c      start-up, the poll loop, menus, messages, files
     arrange.c   the song window (arranger) and its toolbar pane
     editor.c    the editor: step grid for drums, piano roll for the rest
     browser.c   the loop browser
     keys.c      musical typing, and its keyboard window
     savebox.c   the save box (drag to a Filer window, or type a path)
     gfx.c       drawing: colours, rectangles, text
     sound.c     the SharedSound handler and its ring buffer in the RMA
     layout.c    geometry and hit testing (portable, tested on Linux) */
#ifndef APP_H
#define APP_H

#include <kernel.h>
#include "../engine/groove.h"
#include "layout.h"

#define APP_NAME     "Groove"
#define APP_VERSION  "0.2.2"
#define APP_DATE     "02-Nov-2026"
#define TASK_WORD    0x4B534154

#define FILETYPE_SONG  0xFFF       /* Text, until a type is allocated */
#define FILETYPE_WAVE  0xFB1
#define FILETYPE_MIDI  0xFD4
#define FILETYPE_MAESTRO 0xAF1

/* SWIs the GCCSDK headers may not name. */
#ifndef SharedSound_InstallHandler
#define SharedSound_InstallHandler 0x4B440
#define SharedSound_RemoveHandler  0x4B441
#define SharedSound_SampleRate     0x4B446
#endif
#ifndef Wimp_TextOp
#define Wimp_TextOp 0x400F5
#endif

/* Wimp event reason codes and message numbers. */
enum
{
  EV_NULL = 0, EV_REDRAW = 1, EV_OPEN = 2, EV_CLOSE = 3, EV_LEAVING = 4,
  EV_ENTERING = 5, EV_CLICK = 6, EV_DRAG = 7, EV_KEY = 8, EV_MENU = 9,
  EV_SCROLL = 10, EV_LOSE_CARET = 11, EV_GAIN_CARET = 12,
  EV_MESSAGE = 17, EV_MESSAGE_RECORDED = 18, EV_MESSAGE_ACK = 19
};
enum
{
  MSG_QUIT = 0, MSG_DATASAVE = 1, MSG_DATASAVEACK = 2, MSG_DATALOAD = 3,
  MSG_DATALOADACK = 4, MSG_DATAOPEN = 5, MSG_PREQUIT = 8, MSG_MODECHANGE = 0x400C1
};
enum { BUT_ADJUST = 1, BUT_MENU = 2, BUT_SELECT = 4 };

/* A drag in progress: started by a click, followed on null events by
   reading the pointer until the button is let go. */
typedef void (*drag_fn) (int x, int y, int done);

typedef struct
{
  int task;
  gr_song song;
  gr_engine *eng;
  gr_history *hist;             /* Undo and Redo */
  int count_in, merge;          /* recording options (main menu) */

  int w_main, w_tool, w_edit, w_loops, w_keys, w_save;
  int main_open, edit_open, loops_open, keys_open;

  int sel_track, sel_region;    /* -1 = none */
  lay_view view;                /* the arranger's timeline */
  int follow;                   /* page the timeline along with the playhead */
  int edit_width;               /* the editor's visible width */
  int xeig, yeig;               /* OS units per pixel are 1 << these */
  int screen_w, screen_h;       /* OS units */

  char path[256];               /* where the song was last saved */
  int unsaved;

  drag_fn drag;                 /* or NULL */
  int drag_window;
  unsigned underruns_at_play;   /* for the "glitch" warning */
} app_state;

extern app_state app;

/* main.c */
/* The start-up log (startlog.c): <Wimp$ScrapDir>.GrooveLog. */
void startlog_init (void);
void startlog (const char *fmt, ...);
void startlog_stage (const char *what);
void startlog_fail (const char *step, const _kernel_oserror *e);
extern const _kernel_oserror *app_create_error;
unsigned sound_played (void);   /* why a window or icon wasn't made */
/* Undo / Redo the last change (main.c). */
/* Regions: split at the playhead, copy, paste (arrange.c). */
void arrange_split (void);
void arrange_copy (void);
void arrange_paste (void);
int  arrange_can_paste (void);
/* The piano roll's keys and selection (editor.c). */
int editor_key (int code);
int editor_selected (void);
void app_undo (int redo);
void app_error (const char *msg);
void app_check (const _kernel_oserror *e);
void app_changed (void);                       /* song edited: redraw, mark unsaved */
void app_start_drag (int window, drag_fn fn);
void app_window_origin (int window, int *ox, int *oy);   /* work (0,0) on screen */
void app_pointer (int *x, int *y, int *buttons, int *window, int *icon);
void app_open_window (int window, int which);  /* WS_SONG...: first time at its workspace place */
void app_redraw_all (int window);
void app_redraw_work (int window, int x0, int y0, int x1, int y1);
void app_main_menu (int x, int y);
void app_set_extent (int window, int w, int h);

/* arrange.c */
int  arrange_create (void);
void arrange_open (void);
void arrange_open_request (int *b);            /* keeps the pane in place */
void arrange_close (void);
void arrange_redraw (int *b);
void arrange_tool_redraw (int *b);
void arrange_click (int *b);
void arrange_tool_click (int *b);
void arrange_scroll (int *b);
void arrange_tick (void);                      /* on null events: playhead, meters */
void arrange_drop_loop (int loop, int x, int y);
void arrange_drop_file (const char *path, int type, int x, int y);
void arrange_new_song (void);
void arrange_title (void);
int  arrange_key (int code);                   /* Space, R, Return, cursors... */

/* editor.c */
int  editor_create (void);
void editor_open (void);
void editor_show (int region);                 /* the region to edit (or -1) */
void editor_redraw (int *b);
void editor_click (int *b);
void editor_open_request (int *b);
void editor_tick (void);

/* browser.c */
int  browser_create (void);
void browser_open (void);
void browser_redraw (int *b);
void browser_click (int *b);
void browser_drag_done (void);                 /* the Wimp drag box ended */
void browser_stop_preview (void);

/* keys.c */
int  keys_create (void);
void keys_open (void);
void keys_redraw (int *b);
void keys_click (int *b);
int  keys_press (int code);                    /* 1 = used the key */
void keys_poll (void);                         /* releases keys let go */
void keys_release_all (void);
int keys_held_count (void);                 /* notes held from the keyboard */
int  keys_held (void);

/* midiin.c: a MIDI keyboard through the MIDI module (MIDI_RxCommand). */
#define MIDI_SUPPORT (-3)          /* registered with MIDI Support as "Groove" */
#define MIDI_OFF (-2)
#define MIDI_ANY (-1)
int  midiin_present (void);        /* is there a MIDI module? */
void midiin_forget_module (void);  /* look again (and register with MIDI Support) */
int  midiin_support (void);        /* registered with MIDI Support? */
void midiin_quit (void);           /* leave MIDI Support */
int  midiin_active (void);         /* on, and a module to read */
int  midiin_port (void);           /* MIDI_OFF, MIDI_SUPPORT, MIDI_ANY or 0..3 */
void midiin_set_port (int port);
void midiin_poll (void);           /* on every return from the Wimp */
void midiin_message (int status, int d1, int d2);
void midiin_release_all (void);
int  midiin_lit (void);            /* a note in the last quarter second */
int  midiin_held (void);           /* notes held from MIDI (with the pedal's) */
/* MIDI out (midiin.c too, as it shares Groove's MIDI Support driver):
   where tracks with MIDI out on are sent. MIDI_OFF, MIDI_SUPPORT or a
   MIDI module's port 0..3. */
int  midiout_port (void);
void midiout_set_port (int port);
int  midiout_on (void);            /* somewhere to send, so MIDI out tracks use it */
int  midiout_pending (void);       /* commands waiting for their sound to be heard */
void midiout_poll (void);          /* send what is due */
void midiout_reset (void);         /* notes off, and resend each channel's settings */
void arrange_midi_light (void);    /* redraw the display's MIDI light */

/* savebox.c */
int  savebox_create (void);
/* The Sound menu for the selected track (Groove's sounds, then the
   SoundFont's), opened from the editor's sound button. */
void app_sound_menu (int x, int y);
/* Load the SoundFont if it isn't yet (says why not if it can't). */
int  app_gm_ensure (void);
void arrange_import (const char *path, int type, int tick);   /* MIDI or Maestro */
void arrange_set_extent (void);    /* the work area, from the song's length and the zoom */
void arrange_sync_hscroll (void);  /* the timeline moved: the scroll bar follows */
void savebox_open (int filetype);              /* FILETYPE_SONG, _WAVE or _MIDI */
void savebox_click (int *b);
int  savebox_key (int *b);
void savebox_drag_done (void);
void savebox_ack (int *msg);                   /* Message_DataSaveAck */
int  save_to (const char *path, int filetype); /* 0 = OK */

/* sound.c */
int  sound_init (void);
void sound_fill (gr_engine *e);
int  sound_latency (void);                     /* frames queued, not yet heard */
unsigned sound_underruns (void);
void sound_close (void);

#endif

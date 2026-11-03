/* The fake RISC OS's state, shared with the script (test_ui.c). */
#ifndef FAKE_RO_H
#define FAKE_RO_H

#define FAKE_WINDOWS 16
#define FAKE_FILER   0x9900      /* a window handle that isn't Groove's */

typedef struct
{
  int used, open, order, dirty;
  int vis[4], scroll[2], ext[4], flags;
  int min_w, min_h;              /* the window's minimum size (OS units) */
  int nicons, icons[8][8];
  const char *title;
} fake_window;

typedef struct
{
  fake_window w[FAKE_WINDOWS];
  int order, time, polls, swis;
  int px, py, buttons;           /* the pointer */
  int key_down[256];             /* by INKEY number & 0xFF */
  void *ring;                    /* the sound ring block */
  unsigned frames_played;
  int caret, menu, menus, dragging, drag_boxes, keys_passed_on;
  char started[256];             /* the last Wimp_StartTask command */
  int starts;
  int shift;                     /* Shift held (OS_Byte 121) */
  int midi_present;              /* a MIDI module (MIDI_RxCommand) */
  int midi_q[64], midi_n, midi_i; /* messages waiting, as MIDI_RxCommand packs them */
  int midi_port_asked;           /* the port the last MIDI_RxCommand asked for */
  int ms_present;                /* MIDI Support (MIDISupport_CreateDriver etc.) */
  int ms_handle, ms_flags, ms_removed; /* the driver Groove made; RemoveDriver's handle */
  char ms_name[32], ms_date[32];
  int ms_version;
  unsigned ms_sent[256]; int ms_nsent;   /* MIDISupport_Send: what Groove sent */
  unsigned midi_tx[256]; int midi_ntx;   /* MIDI_TxCommand */
  unsigned ms_sent_at[256];              /* frames heard when each was sent */
  int ms_q[64], ms_n, ms_i;      /* commands routed to Groove's driver */
  int errors, error_answer;
  int msg[64], msg_reason, msg_to, messages, my_ref;
  int redraws, force_redraws, plots, texts, nulls_while_masked;
  int filetype_set, closed_down, filer_window;
  int screen_px_w, screen_px_h;  /* 0: 1920 x 1080 */
  int menu_y, errors_before;
  void *log;                     /* FILE *: drawing commands for screenshots */
  int plot_x[2], plot_y[2];      /* the last two plot points */
  unsigned text_fg, text_bg, gcol;
} fake_state;

extern fake_state fake;

fake_window *fake_win (int handle);
int fake_window_at (int x, int y);
void fake_to_screen (int handle, int wx, int wy, int *x, int *y);
/* The script: fill in the next event; return 0 when there are no more. */
int fake_script (int *b, int *reason);

#endif

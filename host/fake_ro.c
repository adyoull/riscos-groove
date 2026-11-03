/* A fake RISC OS for running Groove's Wimp front end on Linux (32-bit, so
   ints hold pointers as on RISC OS). It answers every SWI Groove calls the
   way the PRMs say, keeps a small model of the windows (so redraws,
   opening, scrolling and the pointer work), and takes its events from a
   script (host/test_ui.c). Anything unexpected aborts the run.

   It is not RISC OS: it checks that the code drives the Wimp sensibly and
   that the drawing and editing code is memory-safe on real event
   sequences. Timing, look and the real modules are for the Pi. */
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#ifndef OS_SWINumberFromString
#define OS_SWINumberFromString 0x39
#endif
#include <stdlib.h>
#include <string.h>
#include "stubs/swis.h"
#include "fake_ro.h"
#include "fake_font.h"

static int text_width (const char *t, int n)
{
  int w = 0;
  for (; n-- > 0 && *t; t++)
    w += fake_char_w[*t & 127];
  return w;
}

fake_state fake;

/* The handler's bytes (a stand-in for handler.s). */
__asm__ (".section .rodata\n.globl groove_handler\ngroove_handler: .space 168\n"
         ".globl groove_handler_end\ngroove_handler_end:\n.previous");

#define SharedSound_InstallHandler 0x4B440
#define SharedSound_RemoveHandler  0x4B441
#define SharedSound_SampleRate     0x4B446
#define Wimp_TextOp                0x400F5

static void fail (const char *what, int swi)
{
  printf ("fake RISC OS: %s (SWI &%X)\n", what, swi);
  abort ();
}

fake_window *fake_win (int handle)
{
  int i = (handle - 0x1000) / 0x100;
  if (handle < 0x1000 || i >= FAKE_WINDOWS || !fake.w[i].used || (handle - 0x1000) % 0x100)
    return NULL;
  return &fake.w[i];
}

int fake_window_at (int x, int y)
{
  int i, best = -1, order = -1;
  for (i = 0; i < FAKE_WINDOWS; i++)
    {
      fake_window *w = &fake.w[i];
      if (w->used && w->open && x >= w->vis[0] && x < w->vis[2] && y >= w->vis[1] && y < w->vis[3]
          && w->order > order)
        {
          order = w->order;
          best = 0x1000 + i * 0x100;
        }
    }
  if (best < 0 && fake.filer_window && x >= 3000)
    return FAKE_FILER;            /* "a Filer window" off to the right */
  return best;
}

void fake_to_screen (int handle, int wx, int wy, int *x, int *y)
{
  fake_window *w = fake_win (handle);
  if (!w)
    fail ("fake_to_screen: no such window", 0);
  *x = w->vis[0] - w->scroll[0] + wx;
  *y = w->vis[3] - w->scroll[1] + wy;
}

/* Walk a menu tree, touching every string (AddressSanitizer checks them). */
static void check_menu (const int *m, int depth)
{
  const int *it = m + 7;
  int n = 0;
  if (depth > 4)
    fail ("menu tree too deep", 0x400D4);
  if (m[4] <= 0 || m[4] > 2000 || m[5] != 44)
    fail ("menu header: odd item width or height", 0x400D4);
  for (;; it += 6, n++)
    {
      if (n > 40)
        fail ("menu without a last item", 0x400D4);
      if (it[2] & 0x100)
        {
          const char *t = (const char *) it[3];
          if ((int) strlen (t) + 1 > it[5])
            fail ("menu item text longer than its buffer", 0x400D4);
        }
      /* A submenu pointer below &8000 is a window handle (an Info box,
         a save box), as on RISC OS; anything else is a menu. */
      if (it[1] != -1 && it[1] != 0)
        {
          if ((unsigned) it[1] < 0x8000)
            {
              if (!fake_win (it[1])->used)
                fail ("menu item's window doesn't exist", 0x400D4);
            }
          else
            check_menu ((const int *) it[1], depth + 1);
        }
      if (it[0] & 0x80)
        break;
    }
}

static void consume_audio (void)
{
  /* Pretend 2 cs of sound played since the last poll: the handler's side. */
  if (fake.ring)
    {
      unsigned *ring = (unsigned *) fake.ring;
      unsigned size = ring[1] - ring[0];
      unsigned used = (ring[3] + size - ring[2]) % size;
      unsigned take = 882 * 4;
      if (take > used)
        {
          take = used;
          ring[5]++;              /* an underrun, as the handler would count */
        }
      ring[2] = (ring[2] + take) % size;
      fake.frames_played += take / 4;
    }
}

static void poll (int mask, int *b, int *reason)
{
  int i;
  fake.time += 2;
  fake.polls++;
  consume_audio ();
  /* Redraws first, as the Wimp does. */
  for (i = 0; i < FAKE_WINDOWS; i++)
    if (fake.w[i].used && fake.w[i].open && fake.w[i].dirty)
      {
        fake.w[i].dirty = 0;
        b[0] = 0x1000 + i * 0x100;
        *reason = 1;
        fake.redraws++;
        return;
      }
  memset (b, 0, 64 * sizeof (int));
  *reason = 0;
  if (!fake_script (b, reason))
    {
      b[0] = 20; b[4] = 0;        /* Message_Quit */
      *reason = 17;
    }
  if (*reason == 0 && (mask & 1))
    fake.nulls_while_masked++;    /* the script asked for a null the app didn't want */
}

_kernel_oserror *_swix (int swi, unsigned mask, ...)
{
  static _kernel_oserror err;
  va_list ap;
  int r[10] = { 0 }, out[10] = { 0 }, i, *b;
  int *outp[10] = { 0 };
  fake_window *w;
  va_start (ap, mask);
  for (i = 0; i < 10; i++)
    if (mask & (1u << i))
      r[i] = va_arg (ap, int);
  for (i = 0; i < 10; i++)
    if (mask & (1u << (31 - i)))
      outp[i] = va_arg (ap, int *);
  va_end (ap);
  b = (int *) r[1];
  fake.swis++;

  switch (swi)
    {
    case Wimp_Initialise:
      if (r[0] < 380 || r[1] != 0x4B534154)
        fail ("Wimp_Initialise: version or TASK", swi);
      out[1] = 0x5000;
      break;
    case Wimp_CreateWindow:
      for (i = 0; i < FAKE_WINDOWS && fake.w[i].used; i++)
        ;
      if (i == FAKE_WINDOWS)
        fail ("too many windows", swi);
      w = &fake.w[i];
      memset (w, 0, sizeof *w);
      w->used = 1;
      memcpy (w->vis, b, 4 * sizeof (int));
      w->flags = b[7];
      memcpy (w->ext, &b[10], 4 * sizeof (int));
      w->min_w = ((const unsigned short *) b)[34];   /* PRM: halfwords at +68, +70 */
      w->min_h = ((const unsigned short *) b)[35];
      w->nicons = b[21];
      if (w->nicons > 8)
        fail ("too many icons for the fake", swi);
      memcpy (w->icons, &b[22], (size_t) w->nicons * 8 * sizeof (int));
      if (b[14] & 0x100)          /* indirected title: touch it */
        w->title = (const char *) b[18];
      else
        {
          static char titles[FAKE_WINDOWS][13];
          memcpy (titles[i], &b[18], 12);
          w->title = titles[i];
        }
      (void) strlen (w->title);
      out[0] = 0x1000 + i * 0x100;
      break;
    case Wimp_CreateIcon:
      out[0] = 0;
      break;
    case Wimp_OpenWindow:
      if (!(w = fake_win (b[0])))
        fail ("Wimp_OpenWindow: bad handle", swi);
      if (b[3] <= b[1] || b[4] <= b[2])
        fail ("Wimp_OpenWindow: empty visible area", swi);
      memcpy (w->vis, &b[1], 4 * sizeof (int));
      /* As the Wimp does: no bigger than the work area, then no smaller
         than the minimum size, keeping the top left (the minimum wins:
         seen on RISC OS 5.31, a 104-high pane with a minimum of 200 shows
         200 high). */
      if (w->vis[3] - w->vis[1] > w->ext[3] - w->ext[1]) w->vis[1] = w->vis[3] - (w->ext[3] - w->ext[1]);
      if (w->vis[2] - w->vis[0] < w->min_w) w->vis[2] = w->vis[0] + w->min_w;
      if (w->vis[3] - w->vis[1] < w->min_h) w->vis[1] = w->vis[3] - w->min_h;
      b[1] = w->vis[1]; b[3] = w->vis[2];
      w->scroll[0] = b[5];
      w->scroll[1] = b[6];
      /* The Wimp keeps the scroll offsets inside the extent. */
      if (w->scroll[1] > 0) w->scroll[1] = 0;
      if (w->scroll[1] < w->ext[1] + (w->vis[3] - w->vis[1]))
        w->scroll[1] = w->ext[1] + (w->vis[3] - w->vis[1]) > 0 ? 0 : w->ext[1] + (w->vis[3] - w->vis[1]);
      if (!w->open)
        w->dirty = 1;
      w->open = 1;
      /* The stack: on top (-1), or just behind the window named in b[7]. */
      if (b[7] >= 0 && fake_win (b[7]) && fake_win (b[7])->open && fake_win (b[7]) != w)
        {
          int k, at = fake_win (b[7])->order;
          for (k = 0; k < FAKE_WINDOWS; k++)
            if (fake.w[k].used && fake.w[k].open && &fake.w[k] != w && fake.w[k].order >= at)
              fake.w[k].order++;
          w->order = at;
          fake.order++;
        }
      else
        w->order = ++fake.order;
      w->dirty = 1;
      break;
    case Wimp_CloseWindow:
      if (!(w = fake_win (b[0])))
        fail ("Wimp_CloseWindow: bad handle", swi);
      w->open = 0;
      break;
    case Wimp_GetWindowState:
      if (!(w = fake_win (b[0])))
        fail ("Wimp_GetWindowState: bad handle", swi);
      memcpy (&b[1], w->vis, 4 * sizeof (int));
      b[5] = w->scroll[0];
      b[6] = w->scroll[1];
      b[7] = -1;
      b[8] = w->flags | (w->open ? 1 << 16 : 0);
      break;
    case Wimp_GetWindowOutline:
      if (!(w = fake_win (b[0])))
        fail ("Wimp_GetWindowOutline: bad handle", swi);
      b[1] = w->vis[0] - 4; b[2] = w->vis[1] - 4; b[3] = w->vis[2] + 44; b[4] = w->vis[3] + 44;
      break;
    case Wimp_GetIconState:
      if (!(w = fake_win (b[0])) || b[1] < 0 || b[1] >= w->nicons)
        fail ("Wimp_GetIconState: bad window or icon", swi);
      memcpy (&b[2], w->icons[b[1]], 8 * sizeof (int));
      break;
    case Wimp_RedrawWindow:
      if (!(w = fake_win (b[0])))
        fail ("Wimp_RedrawWindow: bad handle", swi);
      memcpy (&b[1], w->vis, 4 * sizeof (int));
      b[5] = w->scroll[0];
      b[6] = w->scroll[1];
      memcpy (&b[7], w->vis, 4 * sizeof (int));
      if (fake.log)
        fprintf (fake.log, "G %d %d %d %d\n", b[7], b[8], b[9] - 1, b[10] - 1);
      out[0] = 1;
      break;
    case Wimp_GetRectangle:
      out[0] = 0;
      break;
    case Wimp_ForceRedraw:
      if (r[0] == -1)
        break;                    /* screen coordinates: fine */
      if (!(w = fake_win (r[0])))
        fail ("Wimp_ForceRedraw: bad handle", swi);
      if (r[3] < r[1] || r[4] < r[2])
        fail ("Wimp_ForceRedraw: box inside out", swi);
      if (w->open)
        w->dirty = 1;
      fake.force_redraws++;
      break;
    case Wimp_SetExtent:
      if (!(w = fake_win (r[0])))
        fail ("Wimp_SetExtent: bad handle", swi);
      memcpy (w->ext, b, 4 * sizeof (int));
      break;
    case Wimp_GetPointerInfo:
      b[0] = fake.px; b[1] = fake.py; b[2] = fake.buttons;
      b[3] = fake_window_at (fake.px, fake.py);
      b[4] = -1;
      if ((w = fake_win (b[3])))
        for (i = 0; i < w->nicons; i++)
          {
            int x = fake.px - (w->vis[0] - w->scroll[0]), y = fake.py - (w->vis[3] - w->scroll[1]);
            if (x >= w->icons[i][0] && x < w->icons[i][2] && y >= w->icons[i][1] && y < w->icons[i][3])
              b[4] = i;
          }
      break;
    case Wimp_DragBox:
      if (b[1] != 5 || b[4] <= b[2] || b[5] <= b[3])
        fail ("Wimp_DragBox: expected a sensible fixed-size box", swi);
      fake.drag_boxes++;
      fake.dragging = 1;
      break;
    case Wimp_SetCaretPosition:
      fake.caret = r[0];
      break;
    case Wimp_StartTask:
      snprintf (fake.started, sizeof fake.started, "%s", (const char *) r[0]);
      fake.starts++;
      break;
    case Wimp_GetCaretPosition:
      ((int *) r[1])[0] = fake.caret;
      break;
    case Wimp_CreateMenu:
      fake.menu = r[1];
      fake.menu_y = r[3];
      if (r[1] != -1 && !fake_win (r[1]))
        check_menu ((const int *) r[1], 0);
      fake.menus++;
      break;
    case Wimp_ProcessKey:
      fake.keys_passed_on++;
      break;
    case Wimp_ReportError:
      printf ("  [error box] %s\n", ((const _kernel_oserror *) r[0])->errmess);
      fake.errors++;
      out[1] = fake.error_answer ? fake.error_answer : 1;
      break;
    case Wimp_SendMessage:
      {
        int size = b[0];
        if (size < 20 || size > 256 || (size & 3))
          fail ("Wimp_SendMessage: bad size", swi);
        b[2] = ++fake.my_ref;     /* the Wimp fills in my_ref */
        memcpy (fake.msg, b, (size_t) size);
        fake.msg_reason = r[0];
        fake.msg_to = r[2];
        fake.messages++;
      }
      break;
    case Wimp_Poll:
    case Wimp_PollIdle:
      poll (r[0], b, &out[0]);
      break;
    case Wimp_CloseDown:
      fake.closed_down = 1;
      break;
    case Wimp_TextOp:
      switch (r[0] & 0xFF)
        {
        case 0: fake.text_fg = (unsigned) r[1]; fake.text_bg = (unsigned) r[2]; break;
        case 1: out[0] = text_width ((const char *) r[1], r[2]); break;
        case 2:
          (void) strlen ((const char *) r[1]);
          if (r[4] < -100000 || r[4] > 100000 || r[5] < -100000 || r[5] > 100000)
            fail ("Wimp_TextOp: text far off any screen", swi);
          out[0] = text_width ((const char *) r[1], 1 << 30);
          if (fake.log)
            fprintf (fake.log, "X %d %d %08x %08x %s\n", r[4], r[5], fake.text_fg, fake.text_bg,
                     (const char *) r[1]);
          fake.texts++;
          break;
        default: fail ("Wimp_TextOp: reason", swi);
        }
      break;
    case OS_Plot:
      if (r[1] < -0x8000 || r[1] > 0x7FFF || r[2] < -0x8000 || r[2] > 0x7FFF)
        fail ("OS_Plot: coordinate outside 16 bits", swi);
      fake.plots++;
      if (fake.log)
        switch (r[0])
          {
          case 5:   fprintf (fake.log, "L %d %d %d %d\n", fake.plot_x[1], fake.plot_y[1], r[1], r[2]); break;
          case 85:  fprintf (fake.log, "T %d %d %d %d %d %d\n", fake.plot_x[0], fake.plot_y[0],
                             fake.plot_x[1], fake.plot_y[1], r[1], r[2]); break;
          case 101: fprintf (fake.log, "R %d %d %d %d\n", fake.plot_x[1], fake.plot_y[1], r[1], r[2]); break;
          case 157: fprintf (fake.log, "O %d %d %d %d\n", fake.plot_x[1], fake.plot_y[1], r[1], r[2]); break;
          }
      fake.plot_x[0] = fake.plot_x[1]; fake.plot_y[0] = fake.plot_y[1];
      fake.plot_x[1] = r[1]; fake.plot_y[1] = r[2];
      break;
    case ColourTrans_SetGCOL:
      if (fake.log)
        fprintf (fake.log, "C %08x\n", (unsigned) r[0]);
      break;
    case OS_ReadMonotonicTime:
      out[0] = fake.time;
      break;
    case OS_ReadModeVariable:
      /* A 1920 x 1080 mode, 2 OS units a pixel (fake.screen_px overrides). */
      switch (r[1])
        {
        case 4: case 5: out[2] = 1; break;
        case 11: out[2] = (fake.screen_px_w ? fake.screen_px_w : 1920) - 1; break;
        case 12: out[2] = (fake.screen_px_h ? fake.screen_px_h : 1080) - 1; break;
        default: fail ("OS_ReadModeVariable: variable", swi);
        }
      break;
    case OS_Byte:
      if (r[0] == 129)
        out[1] = fake.key_down[r[1] & 0xFF] ? 0xFF : 0;
      else if (r[0] == 121)       /* a key down? &80 = Shift */
        out[1] = (r[1] == 0x80 && fake.shift) ? 0xFF : 0;
      else
        fail ("OS_Byte: reason", swi);
      break;
    case OS_Module:
      if (r[0] == 6)
        out[2] = (int) malloc ((size_t) r[3]);
      else if (r[0] == 7)
        free ((void *) r[2]);
      else
        fail ("OS_Module: reason", swi);
      break;
    case SharedSound_InstallHandler:
      if (!(r[2] & 1) || r[4] != 0)
        fail ("SharedSound_InstallHandler: expected an immediate handler", swi);
      fake.ring = (void *) r[1];
      out[0] = 7;
      break;
    case SharedSound_SampleRate:
      if (r[0] != 7 || r[1] != 44100 * 1024)
        fail ("SharedSound_SampleRate: handler or rate", swi);
      break;
    case SharedSound_RemoveHandler:
      fake.ring = NULL;
      break;
    case OS_File:
      fake.filetype_set = r[2];
      break;
    case OS_WriteN:
      {
        const unsigned char *v = (const unsigned char *) r[0];
        if (r[1] == 9 && v[0] == 24 && fake.log)
          fprintf (fake.log, "G %d %d %d %d\n", (short) (v[1] | v[2] << 8), (short) (v[3] | v[4] << 8),
                   (short) (v[5] | v[6] << 8), (short) (v[7] | v[8] << 8));
      }
      break;
    case OS_SWINumberFromString:
      if (fake.midi_present && !strcmp ((const char *) (intptr_t) r[1], "MIDI_RxCommand"))
        out[0] = 0x404C8;
      else if (fake.ms_present && !strcmp ((const char *) (intptr_t) r[1], "MIDISupport_CreateDriver"))
        out[0] = 0x4EE83;
      else
        {
          static _kernel_oserror unknown = { 0x1E6, "SWI name not known" };
          return &unknown;
        }
      break;
    case 0x4EE83:               /* MIDISupport_CreateDriver */
      fake.ms_flags = r[0];
      snprintf (fake.ms_name, sizeof fake.ms_name, "%.31s", (const char *) (intptr_t) r[1]);
      fake.ms_version = r[2];
      snprintf (fake.ms_date, sizeof fake.ms_date, "%.31s", (const char *) (intptr_t) r[3]);
      out[0] = fake.ms_handle = 7;
      break;
    case 0x4EE85:               /* MIDISupport_Receive: R0=1, R1=driver, R2=2 (a command) */
      if (!fake.ms_present || r[1] != fake.ms_handle || r[0] != 1 || r[2] != 2)
        {
          static _kernel_oserror bad = { 0x1E6, "Invalid driver" };
          return &bad;
        }
      out[0] = fake.ms_i < fake.ms_n ? fake.ms_q[fake.ms_i++] : 0;
      break;
    case 0x4EE84:               /* MIDISupport_Send: R0=1 (a command), R1=driver, R2=command */
      if (!fake.ms_present || r[1] != fake.ms_handle || r[0] != 1)
        {
          static _kernel_oserror bad = { 0x1E6, "Invalid driver" };
          return &bad;
        }
      if (fake.ms_nsent < 256)
        {
          fake.ms_sent_at[fake.ms_nsent] = fake.frames_played;
          fake.ms_sent[fake.ms_nsent++] = (unsigned) r[2];
        }
      break;
    case 0x404CA:               /* MIDI_TxCommand: R0=command (port in bits 28-31), R1=time */
      if (!fake.midi_present)
        {
          static _kernel_oserror unknown = { 0x1E6, "SWI &404CA not known" };
          return &unknown;
        }
      if (fake.midi_ntx < 256)
        fake.midi_tx[fake.midi_ntx++] = (unsigned) r[0];
      break;
    case 0x4EE81:               /* MIDISupport_RemoveDriver */
      fake.ms_removed = r[1];
      break;
    case 0x404C8:               /* MIDI_RxCommand */
      if (!fake.midi_present)
        {
          static _kernel_oserror unknown = { 0x1E6, "SWI &404C8 not known" };
          return &unknown;
        }
      fake.midi_port_asked = r[0];
      out[0] = fake.midi_i < fake.midi_n ? fake.midi_q[fake.midi_i++] : 0;
      out[1] = 0;
      break;
    case 0x40183:               /* Sound_InstallVoice: RISC OS 5's nine voices */
      {
        static const char *const v[] = { "WaveSynth-Beep", "StringLib-Soft", "StringLib-Pluck",
                                         "StringLib-Steel", "StringLib-Hard", "Percussion-Soft",
                                         "Percussion-Medium", "Percussion-Snare", "Percussion-Noise" };
        if (r[0] == 0)
          out[1] = 10;
        else if (r[0] == 2 && r[1] >= 1 && r[1] <= 9)
          out[3] = (int) (intptr_t) v[r[1] - 1];
        else
          out[3] = 0;
      }
      break;
    case OS_SynchroniseCodeAreas:
    case Hourglass_On:
    case Hourglass_Off:
      break;
    default:
      fail ("unexpected SWI", swi);
    }
  for (i = 0; i < 10; i++)
    if (outp[i])
      *outp[i] = out[i];
  return NULL;
  (void) err;
}

/* Groove - a MIDI keyboard (or anything else that sends notes) through the
   RISC OS MIDI module: the original Acorn interface, which RISC OS 5's
   MIDI modules provide (Rick Murray and Dave Hinton's USBMIDI, for USB
   MIDI keyboards and interfaces, up to four ports; or Pete Everett's
   module of the same name).

   Notes go where musical typing's do: the selected track, played live and
   recorded the same way, with the keyboard's velocity. On a drum track the
   General MIDI drum keys play the step grid's drums. The sustain pedal
   (controller 64) holds notes until it's let go.

   Two ways in:
   - MIDI Support (Pete Everett's RISC OS 5 version of ESP's system): Groove
     registers as a driver called "Groove" (MIDISupport_CreateDriver, as
     Pete's own MIDIKeyboard does), so it appears in the MIDI Manager and
     *MIDIMap, and the user connects a keyboard or interface to it. What is
     routed to it is queued by MIDI Support and read with
     MIDISupport_Receive. The routing says it's meant for Groove, so Groove
     listens whether or not it has the input focus.
   - A MIDI module's ports (MIDI_RxCommand). Like the computer keyboard,
     Groove only reads them while one of its windows has the input focus,
     so another program (a score editor, say) can have the keyboard when it
     has the focus. What arrived while Groove didn't have the focus is
     thrown away when it gets it back, rather than played late.
   Neither has an event for "data arrived", so while MIDI input is on,
   Groove asks for null events every centisecond. */
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <swis.h>
#include "app.h"

#ifndef OS_SWINumberFromString
#define OS_SWINumberFromString 0x39
#endif
#ifndef MIDI_RxCommand
#define MIDI_RxCommand 0x404C8
#endif
#ifndef MIDI_TxCommand
#define MIDI_TxCommand 0x404CA
#endif
#define MIDISupport_RemoveDriver 0x4EE81
#define MIDISupport_CreateDriver 0x4EE83
#define MIDISupport_Send         0x4EE84
#define MIDISupport_Receive      0x4EE85
/* Driver flags: it sends and receives; MIDI Support queues for it, in
   whole messages. */
#define MS_FLAGS (1 | 2 | 4 | 8)

static int port = MIDI_ANY;          /* MIDI_OFF, MIDI_SUPPORT, MIDI_ANY or 0..3 */
static int chosen;                   /* has the user chosen (else the default follows what's there) */
static int present = -1;             /* is a MIDI module there? (-1: not looked yet) */
static int ms_handle;                /* our MIDI Support driver (0: none) */
static int had_focus;
static int last_seen;                /* monotonic time of the last message (activity light) */
static int lit;
static int out_port = MIDI_OFF, out_chosen, out_set = -1;   /* MIDI out */
static void out_update (void);

/* What each held key is playing: track + 1 (0 = nothing), per channel. */
static unsigned char held_track[16][128], held_pitch[16][128];
static unsigned char sustained[16][128];
static int pedal[16];

int midiin_present (void)
{
  if (present < 0)
    {
      int n = -1;
      present = !_swix (OS_SWINumberFromString, _INR (0, 1) | _OUT (0), 0, "MIDI_RxCommand", &n) && n > 0;
    }
  return present;
}

/* Register with MIDI Support, if it's there and we haven't. MIDI Support
   shows the version in hundredths (22: "0:22", for 0.2.2) and the date as
   given, so it's given the way the other drivers give theirs ("2 Nov 2026"
   rather than "02-Nov-2026"). */
#define MS_VERSION 22
static void ms_register (void)
{
  int n = -1, h = 0;
  char date[16];
  const char *d = APP_DATE;
  if (ms_handle || _swix (OS_SWINumberFromString, _INR (0, 1) | _OUT (0), 0, "MIDISupport_CreateDriver", &n) || n <= 0)
    return;
  if (d[0] == '0')
    d++;
  snprintf (date, sizeof date, "%.15s", d);
  for (n = 0; date[n]; n++)
    if (date[n] == '-')
      date[n] = ' ';
  if (!_swix (MIDISupport_CreateDriver, _INR (0, 3) | _OUT (0), MS_FLAGS, "Groove", MS_VERSION, date, &h) && h > 0)
    ms_handle = h;
}

int midiin_support (void) { return ms_handle != 0; }

/* At start-up, and whenever the main menu is built (MIDI Support or a MIDI
   module may have been loaded since). Until the user chooses, Groove uses
   MIDI Support if it's there, else a MIDI module's ports. */
void midiin_forget_module (void)
{
  present = -1;
  ms_register ();
  if (!chosen && port != MIDI_OFF)
    port = ms_handle ? MIDI_SUPPORT : MIDI_ANY;
  if (!out_chosen)
    out_port = ms_handle ? MIDI_SUPPORT : midiin_present () ? 0 : MIDI_OFF;
  out_update ();
}

/* On quitting (atexit): leave MIDI Support, so "Groove" goes from its map. */
void midiin_quit (void)
{
  if (out_set > 0)
    {
      midiout_reset ();         /* (no notes left sounding on the device) */
      gr_engine_set_midi_out (app.eng, NULL, NULL);
      out_set = 0;
    }
  if (ms_handle)
    _swix (MIDISupport_RemoveDriver, _INR (1, 1), ms_handle);
  ms_handle = 0;
}

int midiin_port (void) { return port; }

int midiin_active (void)
{
  if (port == MIDI_SUPPORT)
    return ms_handle != 0;
  return port != MIDI_OFF && midiin_present ();
}

static void end_note (int ch, int key)
{
  int t = held_track[ch][key] - 1;
  if (t < 0)
    return;
  if (t < app.song.ntracks && app.song.tracks[t].type == TRK_INST)
    gr_live_off (app.eng, t, held_pitch[ch][key]);
  held_track[ch][key] = 0;
  sustained[ch][key] = 0;
}

void midiin_release_all (void)
{
  int ch, k;
  for (ch = 0; ch < 16; ch++)
    {
      for (k = 0; k < 128; k++)
        end_note (ch, k);
      pedal[ch] = 0;
    }
}

void midiin_set_port (int p)
{
  midiin_release_all ();
  port = p;
  chosen = 1;
  had_focus = 0;                /* (so what's waiting is thrown away first) */
}

static void note_on (int ch, int key, int vel)
{
  int t = app.sel_track, pitch = key;
  if (held_track[ch][key])
    end_note (ch, key);         /* the same key again: end the old one first */
  if (t < 0 || t >= app.song.ntracks || app.song.tracks[t].type == TRK_AUDIO)
    return;
  if (app.song.tracks[t].type == TRK_DRUMS)
    pitch = gr_drum_row_for_key (key);
  gr_live_on (app.eng, t, pitch, vel);
  held_track[ch][key] = (unsigned char) (t + 1);
  held_pitch[ch][key] = (unsigned char) pitch;
}

static void note_off (int ch, int key)
{
  if (!held_track[ch][key])
    return;
  if (pedal[ch])
    sustained[ch][key] = 1;     /* held by the pedal until it's let go */
  else
    end_note (ch, key);
}

static void control (int ch, int cc, int v)
{
  int k;
  switch (cc)
    {
    case 64:                    /* sustain pedal */
      pedal[ch] = v >= 64;
      if (!pedal[ch])
        for (k = 0; k < 128; k++)
          if (sustained[ch][k])
            end_note (ch, k);
      break;
    case 120: case 123:         /* all sound off, all notes off */
      for (k = 0; k < 128; k++)
        end_note (ch, k);
      pedal[ch] = 0;
      break;
    }
}

/* One message from MIDI_RxCommand: status, two data bytes. */
void midiin_message (int status, int d1, int d2)
{
  int ch = status & 15;
  d1 &= 127;
  d2 &= 127;
  switch (status & 0xF0)
    {
    case 0x90:
      if (d2)
        {
          note_on (ch, d1, d2);
          break;
        }
      /* velocity 0 is a note off */
      /* fall through */
    case 0x80:
      note_off (ch, d1);
      break;
    case 0xB0:
      control (ch, d1, d2);
      break;
    }
}

static int focus (void)
{
  int c[6];
  if (_swix (Wimp_GetCaretPosition, _IN (1), c))
    return 0;
  return c[0] >= 0 && (c[0] == app.w_main || c[0] == app.w_edit || c[0] == app.w_keys);
}

static void light (void)
{
  _swix (OS_ReadMonotonicTime, _OUT (0), &last_seen);
  if (!lit)
    {
      lit = 1;
      arrange_midi_light ();
    }
}

static void light_out (void)
{
  int now;
  _swix (OS_ReadMonotonicTime, _OUT (0), &now);
  if (lit && now - last_seen > 25)
    {
      lit = 0;
      arrange_midi_light ();
    }
}

/* MIDI Support: what has been routed to "Groove". */
static void ms_poll (void)
{
  int i;
  for (i = 0; i < 256; i++)
    {
      int cmd = 0;
      if (_swix (MIDISupport_Receive, _INR (0, 2) | _OUT (0), 1, ms_handle, 2, &cmd))
        {
          ms_handle = 0;        /* MIDI Support has gone */
          midiin_release_all ();
          break;
        }
      if (!cmd)
        break;
      midiin_message (cmd & 0xFF, (cmd >> 8) & 0xFF, (cmd >> 16) & 0xFF);
      light ();
    }
  light_out ();
}

/* On every return from the Wimp while MIDI input is on: read what has
   come in. */
void midiin_poll (void)
{
  int i, f;
  if (!midiin_active ())
    return;
  if (port == MIDI_SUPPORT)
    {
      ms_poll ();
      return;
    }
  f = focus ();
  if (!f)
    {
      if (had_focus)
        midiin_release_all ();  /* (no notes left hanging when the focus goes) */
      had_focus = 0;
      return;
    }
  for (i = 0; i < 256; i++)     /* (a burst is read in one go; the rest next time) */
    {
      int r0 = 0, r1 = 0;
      _kernel_oserror *e = _swix (MIDI_RxCommand, _IN (0) | _OUT (0) | _OUT (1), port, &r0, &r1);
      if (e)
        {
          if ((e->errnum & 0xFFFFFF) == 0x1E6)      /* the module has gone */
            {
              present = 0;
              midiin_release_all ();
            }
          break;                /* (a receive overflow: carry on next time) */
        }
      if (r0 == 0)
        break;
      if (!had_focus)
        continue;               /* waiting since before we had the focus: old */
      midiin_message (r0 & 0xFF, (r0 >> 8) & 0xFF, (r0 >> 16) & 0xFF);
      light ();
    }
  had_focus = 1;
  light_out ();
}

int midiin_lit (void) { return lit; }

int midiin_held (void)
{
  int ch, k, n = 0;
  for (ch = 0; ch < 16; ch++)
    for (k = 0; k < 128; k++)
      n += held_track[ch][k] != 0;
  return n;
}

/* ---- MIDI out ------------------------------------------------------------

   Tracks with MIDI out on are played by sending MIDI: through MIDI Support
   (from the "Groove" driver, to whatever it is connected to: a USB or
   serial port and the instrument on it, Pete's MidiSynth, another
   program), or to a MIDI module's port (MIDI_TxCommand), for USBMIDI.

   The engine renders about 70 ms ahead of what is heard, so each command
   it sends carries the frame it belongs to, and waits here until the sound
   rendered with it is being heard (null events every centisecond while
   any wait); then a sent note and Groove's own sound line up. Notes played
   live carry frame 0 and go straight out. */

#define OUT_Q 1024
static struct { unsigned frame, cmd; } out_q[OUT_Q];
static int out_r, out_w;

int midiout_port (void) { return out_port; }

int midiout_on (void)
{
  if (out_port == MIDI_SUPPORT)
    return ms_handle != 0;
  return out_port >= 0 && midiin_present ();
}

static void tx (unsigned cmd)
{
  if (out_port == MIDI_SUPPORT && ms_handle)
    {
      if (_swix (MIDISupport_Send, _INR (0, 2), 1, ms_handle, cmd))
        {
          ms_handle = 0;        /* MIDI Support has gone */
          out_update ();
        }
    }
  else if (out_port >= 0 && present > 0)
    _swix (MIDI_TxCommand, _INR (0, 1), (cmd & 0x0FFFFFFF) | (unsigned) out_port << 28, 0);
}

static void sender (void *ctx, unsigned frame, unsigned cmd)
{
  int next = (out_w + 1) % OUT_Q;
  (void) ctx;
  if (frame == 0 || next == out_r)
    {
      tx (cmd);                 /* now (or, if the queue is full, early rather than never) */
      return;
    }
  out_q[out_w].frame = frame;
  out_q[out_w].cmd = cmd;
  out_w = next;
}

int midiout_pending (void) { return out_r != out_w; }

void midiout_poll (void)
{
  unsigned heard = gr_frames (app.eng) - (unsigned) sound_latency ();
  while (out_r != out_w && (int) (heard - out_q[out_r].frame) >= 0)
    {
      tx (out_q[out_r].cmd);
      out_r = (out_r + 1) % OUT_Q;
    }
}

/* What was waiting is dropped: the notes-off that follow cover it. */
void midiout_reset (void)
{
  out_r = out_w = 0;
  if (out_set > 0)
    gr_midi_out_reset (app.eng);
}

/* Give the engine the sender while there's somewhere to send (else MIDI
   out tracks play Groove's sounds). */
static void out_update (void)
{
  int on = midiout_on ();
  if (!app.eng || on == out_set)
    return;
  if (out_set > 0)
    midiout_reset ();
  out_r = out_w = 0;
  gr_engine_set_midi_out (app.eng, on ? sender : NULL, NULL);
  out_set = on;
}

void midiout_set_port (int p)
{
  if (out_set > 0)
    midiout_reset ();
  out_port = p;
  out_chosen = 1;
  out_set = -1;                 /* (so the sender is set again, for the new port) */
  out_r = out_w = 0;
  {
    int on = midiout_on ();
    gr_engine_set_midi_out (app.eng, on ? sender : NULL, NULL);
    out_set = on;
  }
}

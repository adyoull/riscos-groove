# Changelog

## 0.2.3 (4 Nov 2026)

- F10 steps through the MIDI out choices: Off, MIDI Support, Port 1-4
  and round again, skipping any that aren't there. Suggested by
  Jean-Michel Brück.
- The song window's title shows where MIDI out goes ("MIDI out:
  Support"); with MIDI out Off it's just the song's name.
- Song > Play on MIDI out (Shift-F11): every drum and instrument track on
  MIDI out at once, or all off. Track > Play on MIDI out is now F11.
- MIDI out now starts Off. Ticking Play on MIDI out on a track while it's
  Off picks MIDI Support, else Port 1.

## 0.2.2 (2 Nov 2026)

- MIDI keyboard: a MIDI keyboard plays the selected track like musical
  typing, with its velocity; recording, count-in and merging work the same
  way. Drum keys play a drum track's drums; the sustain pedal holds notes.
  A MIDI light in the toolbar's display. Two ways in:
  - MIDI Support: with Pete Everett's MIDI Support (!MidiSupp) loaded,
    Groove registers as a driver called "Groove", listed in the MIDI
    Manager and *MIDIMap. Anything connected to it (a USB or serial MIDI
    port, MIDIKeyboard, another program) plays Groove, whichever window has
    the input focus. Groove leaves MIDI Support when it quits.
  - A MIDI module's ports (Rick Murray and Dave Hinton's USBMIDI, for USB
    MIDI keyboards and interfaces), read only while Groove has the input
    focus, so other music programs can have the keyboard.
  Menu > MIDI keyboard: Off, MIDI Support, Any port, Port 1-4 (MIDI Support
  to begin with when it's there).
- MIDI out: Track > Play on MIDI out sends a track to a MIDI instrument
  instead of playing it in Groove: notes, General MIDI program, volume and
  pan, a channel per instrument track and channel 10 for drums. Menu > MIDI
  out: MIDI Support (from the "Groove" driver), Port 1-4 of a MIDI module
  (MIDI_TxCommand), or Off. Song notes are sent when Groove's own sound
  from that moment is heard; live notes at once; All Notes Off on stop.
  Saved in songs.

## 0.2 (1 Nov 2026): the first release

- Undo and Redo (Ctrl-Z, Ctrl-Y), up to 100 changes.
- Regions: split at the playhead (Ctrl-T), copy and paste at the playhead
  (Ctrl-C, Ctrl-V).
- Notes: Shift-click and Shift-drag selection, Ctrl-A; drag the selection
  together; transpose (Up/Down, with Shift an octave), louder and softer
  (Ctrl-Up/Down), delete, copy and paste. Louder notes are drawn brighter.
- Recording: a bar of count-in (on by default), and Merge recordings.
- Tracks: up to 16 (was 8); rename; reorder by dragging the name; coloured
  by kind of instrument; a master volume slider.
- Time signatures: Song > Time signature, from the playhead's bar, with
  changes part-way through; bars, the metronome, the count-in, snapping and
  recording follow them; saved in songs and carried in and out of MIDI files.
- Maestro music: drop a Maestro file on Groove and it plays it the way
  Maestro does, with each channel's voice, volume and stereo position.
- Imports into a song with no music yet replace its empty starter tracks.
- A horizontal scroll bar on the song window.
- SoundFont sounds through SoftMIDI's library, and MIDI import and export.
- A User guide (`!Groove.Guide`) and a `Licences` directory. MIT licence.
- Fixes: Groove opens its windows at start-up and takes the input focus;
  chords of three or more keys; a crash is reported instead of Groove
  vanishing; the Loops window has a close icon; loading a song asks before
  losing changes.

## 0.1 (1 Nov 2026)

- The first version: loops, step grid, piano roll, musical typing,
  recording, songs, WAV export and import.

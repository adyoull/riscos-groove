# Changelog

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

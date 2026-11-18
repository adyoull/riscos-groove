# Changelog

## 0.4 (18 Nov 2026)

MuseScore and MusicXML, for school (asked for by Raik on the ROOL forum):

- Import MuseScore scores (.mscz and .mscx, from MuseScore 2, 3 and 4)
  and MusicXML (.musicxml, .xml, and compressed .mxl): drop one on the
  song window or Groove's icon. Each part becomes a track with its
  instrument as a SoundFont sound; repeats and first/second endings are
  played out, tied notes joined, and tuplets, pickup bars, dynamics,
  transposing instruments and 8va lines kept. Into an empty song, the
  score's tempo and time signatures come too. Files are recognised by
  what's in them, so ones from a PC or a USB stick (Data, XML or an
  archive) work. Grace notes, ornaments, D.C./D.S. jumps and lyrics
  aren't read.
- Song > Export as MusicXML: the song as a score MuseScore (and Sibelius,
  Dorico and Finale) open, each drum and instrument track a part. Groove
  writes the notation: note values that respect the beat, ties, triplets
  where they fit, two staves for a piano-range part, a second voice for
  held notes, a key signature worked out from the notes (major or
  minor), drums on a percussion staff, and each note's velocity.
- Saved as XML (&F80), named Name/musicxml so it's a .musicxml file on a
  PC.
- The Score window (F6, or Menu > Score): the selected track written out
  as music, the same way as the MusicXML export, laid out to the
  window's width. The bar playing is lit and followed; click a bar to
  move the playhead there; + and - change the size. Its symbols come
  from the Bravura music font (SIL Open Font Licence; see
  !Groove.Licences).
- How it's played, in the Score window and in MusicXML: dynamics (ppp
  to fff) from the notes' velocities, a bar at a time; accents on notes
  played much harder than those round them; staccato for notes let go
  well before the next (written to the next note, with a dot, rather
  than a short note and a rest); slurs over notes played legato. Drums
  are written in two voices: hands stems up, feet (bass drum, pedal
  hi-hat) stems down. MusicXML's release attribute keeps how early or
  late each note is really let go, so a round trip through MusicXML
  gives back the notes as played; a staccato note from elsewhere plays
  for half its value, as in MuseScore (MuseScore files too).
- Hairpins: crescendo and diminuendo where the notes get steadily
  louder or softer (over a beat or more), ending at the dynamic they
  reach; loud notes inside one aren't marked as accents.
- Pedal marks: Ped. and * from the sustain pedal (controller 64). In
  MusicXML they're written as pedal marks, and a score's pedal marks
  (MusicXML, and MuseScore 2 to 4 files) come into Groove as the
  sustain pedal.
- Song > Print score (Ctrl-P): the selected track's score printed on the
  printer set up in !Printers, laid out to its paper, as many pages as
  it takes, breaking between lines of music.
- Score window: the music is drawn anti-aliased (smooth edges, like the
  desktop's text) on 16 and 32-bit screens, and in 256 colours through
  ColourTrans; ties, slurs and hairpins take Bravura's own thicknesses.
- Full score: Menu > Full score in the Score window shows every track
  together, one staff (or two for a keyboard) per part, named on the
  left, with a brace for each piano and the bars lined up down the page.
  Menu > Print parts prints a separate part for each player, each
  starting on a new page; Print score prints the full score.
- Transposing instruments are written as their players read them: a
  B flat clarinet, trumpet, soprano or tenor sax a tone up (the tenor
  an octave higher too), alto and baritone sax, horn and cor anglais in
  their keys, guitar and bass an octave up, piccolo, glockenspiel and
  xylophone lower. Each part gets its own key signature. Menu > Concert
  pitch shows the score as it sounds. MusicXML carries the
  transposition, so MuseScore shows the same.
- Clefs: treble-8 for guitar, alto for viola, and tenor for a cello,
  bassoon or trombone part that sits high.
- Better spelling: notes outside the key are written sharp or flat to
  suit the next note (a G sharp rising to A, an A flat falling to G),
  and otherwise as is usual in the key (a sharpened fourth in major, a
  sharpened sixth and seventh in minor).
- Multi-bar rests: bars of rest in a row are written as one bar with
  the count above, in a single part and in printed parts.
- Tuplets: crotchet triplets (three over two beats), quintuplets,
  sextuplets and, in 6/8 and other compound times, duplets, as well as
  quaver triplets. They're written to MusicXML too.
- Key changes: where a long passage moves to a new key (the last verse
  a tone up, say) and its key signature would save a good many
  accidentals, the key signature changes there, after a double bar
  line, with naturals cancelling the old sharps or flats. A few chromatic
  notes don't change it.
- Clef changes: a part that goes high or low for a passage changes clef
  for it, with a small clef at the bar where it changes: a cello or
  bassoon to the tenor clef, a viola to treble, a melody to bass.
- 8va and 8vb lines: a passage far above (or below) the staff is
  written an octave nearer under an 8va (or 8vb) line, for piano and
  other instruments, instead of with a ladder of ledger lines; two
  octaves out, 15ma (15mb). A line starts at the first note that needs
  it and ends after the last, part way through a bar if need be.
- A key or clef changing at the start of a line of music is shown at
  the end of the line before too (a courtesy key or clef).
- Repeats: music played again straight after is written once between
  repeat signs ("Play 4 times" over the end if more than twice), with
  first and second endings where only the end differs. The bar playing
  is still lit, the second time round too. Menu > Repeats in the Score
  window turns them off, to see it written out in full. MusicXML has
  them too, and MuseScore plays them out.
- Printing: a score or part of more than one page has its name and the
  page number ("page 2 of 6") at the foot of each page.
- In a bar of four beats, notes and rests that start after the downbeat
  don't run over the middle of the bar (a crotchet rest then a minim
  rest, not a dotted crotchet rest, a quaver rest and a crotchet rest).

## 0.3 (9 Nov 2026)

Pete Everett's suggestions, and the rest of his report:

- Drums: every note is a MIDI drum key (Kick 36, Snare 38 and so on).
  The Editor's new All keys button shows all 128 keys, with their General
  MIDI names; Kit rows shows the kit's eight, plus a row for any other key
  the region uses. Groove's own kits play other keys on the nearest drum.
- Piano roll: all 128 notes (C-1 to G9), was C1 to B7.
- Controller lane, under the notes in the Editor: draw controller events
  (modulation, breath, volume, pan, expression, sustain, any CC 0-127),
  pitch bend, channel pressure and program changes. They're part of the
  region's pattern: they move, copy, split and undo with it. They play on
  MIDI out and SoundFont sounds, and starting part-way sends the settings
  in force at that point first.
- Track > MIDI channel (Auto, 1-16) and Track > Bank & program (Bank MSB,
  Bank LSB, Program; empty for automatic). Used by MIDI out, SoundFont
  sounds and MIDI export; saved with the song.
- 32 tracks (was 16). Instrument tracks after the 30th with SoundFont
  sounds play Groove's own nearest sound.
- MIDI import keeps each part's channel, bank, later program changes and
  its controllers (bends, sustain, modulation and so on), and all its
  drum keys. MIDI export writes them back.
- SoundFont sounds can play 96 voices at once (was 48), 48 in the second
  player (was 24), as SoftMIDI does, so busy songs and the sustain pedal
  don't cut notes off.
- MIDI out: each channel's settings (Reset All Controllers, bank,
  program, volume, pan) are sent every time the song starts playing, not
  only the first time after choosing MIDI out.
- Importing a long MIDI file no longer says "The song is full" so soon: a
  song now holds 1024 regions (was 256).
- Fixes: undoing a note took it away but left it counted as selected;
  drum rows didn't fit the Editor when it first opened.
- Song files are now GROOVE 2 (track MIDI settings, controller lines,
  drum keys). GROOVE 1 songs still open. WimpSlot 10 MB.

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

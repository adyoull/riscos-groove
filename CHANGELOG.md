# Changelog

## 0.4.4 (not released yet)

- The Score window zooms out to 25% (it went no smaller than 60%), so a
  whole orchestral score shows many bars to a line: 25%, 30%, 40%, 50%,
  60%, 80%, 100%, 130% and 160%. A Zoom submenu on the Score window's
  menu lists them, and Ctrl with the scroll wheel zooms in and out, as
  well as + and -. The same part of the score stays in view, and the
  words (tempo, dynamics, lyrics, part names) shrink with the music.
  Asked for by Paul.

## 0.4.3 (23 November 2026)

- Groove started on a machine with alignment exceptions turned on (the
  usual setting on a Raspberry Pi with RISC OS 5) only if they had been
  turned off first (by !AlignEx, or another program such as !Iris).
  Some of UnixLib's data, built into Groove, had no alignment of its
  own and happened to land at an address no word can be read from, so
  Groove stopped as it started. Every version until now had this. The
  build now uses riscos-crossdev 1.13, whose UnixLib has its data
  word-aligned, refuses an older one, and checks every piece of data in
  the program is aligned. Reported by Geofrey Baxendale.
- MuseScore and MusicXML scores are written in the Score window as the
  score writes them, rather than worked out again from how the notes
  are played (after Paul's report that imported scores were close to
  unreadable):
  - each note on the staff and in the voice it was written in (a
    piano's notes no longer jump between staves by pitch);
  - its written value: a staccato crotchet is a crotchet with a dot,
    not a quaver and a rest; a syncopated note isn't cut in two; notes
    the score tied together stay tied, where it tied them;
  - its spelling (C flat, E sharp), the score's key signatures, its
    clefs and clef changes, and a transposing instrument's
    transposition (a bass clarinet is written an octave and a tone up);
  - a pickup (anacrusis) is a short first bar, numbered 0, and the
    bars after it start where the score's do (they were a pickup's
    length late);
  - a bar the score makes shorter or longer than its time signature
    keeps its length;
  - grace notes, pauses (fermatas), glissandos, trills, mordents,
    turns, tenuto lines and marcato accents are shown;
  - cross-staff notes, and a note under an 8va line at the pitch it
    sounds (it was an octave low);
  - the name the score prints for a part;
  - its rests as the score has them (in a second voice too, and none
    where the score hides one);
  - voices in MuseScore 2 files, a third voice, notes moved to the
    other staff, and a unison two voices share (each written);
  - tuplets as the score groups them (semiquaver triplets, sextuplets,
    quintuplets, duplets), double-dotted notes;
  - clef changes inside a bar (a small clef before the note), and
    tremolos (through a stem, or between two notes);
  - a score that changes time signature every bar (Groove had room
    for 32 changes; now 256), and a first bar longer than its time
    signature;
  - dynamics under the staff they're for;
  - the words (lyrics) under their notes, every verse, with hyphens
    between syllables;
  - up to four voices on a staff, each with its own rhythm (stems up
    in the first and third, down in the second and fourth), and a
    second voice's own rests;
  - a note tied into another voice, or over other notes to a later one
    (as an arpeggio's notes ring into a chord), tied as the score ties
    it; notes the score shows but doesn't play (cue notes, another
    verse's notes) written;
  - six quavers over two beats as one sextuplet (6), and a sextuplet
    in a beat as a 6; demisemiquaver triplets, and triplets in part of
    a beat with straight notes in the rest; dotted notes in triplets;
    rests in tuplets; tremolos between dotted notes; 64ths and 128ths;
  - cue notes small, and notes the score hides not shown (they play);
  - notes beamed (or not: flags, as songs' voices often are) as the
    score beams them; a bracket over a tuplet that isn't beamed;
  - the score's words over and under the staff ("Andante.",
    "p sostenuto", "rit.", "D.C. al Fine"), and C and cut C time
    signatures;
  - the words spaced so they don't run into each other, and a rest
    in a second voice clear of the other voice's notes;
  - a staff with a time signature of its own (3/8 over the piano's
    9/16) in time with the rest;
  - pedal marks, dynamics, hairpins and slurs the score hides not
    shown; quavers in 4/4 beamed four to a group, as MuseScore does,
    where there's nothing shorter;
  - no metronome mark ("♩ = 92") where the score gives its tempo only
    in words ("Andante"), or none at all, and the words over a
    multi-bar rest (which words now start, as in MuseScore);
  - a tempo's words in bold, with its metronome mark written as the
    score writes it ("Molto moderato. (♩ = 69)", the dotted crotchets
    and minims too), and words at the same place over each other
    rather than on top of each other;
  - a part on three or more staves (a piano with a staff over its two
    hands, an organ's pedals): on tracks of one or two staves, joined
    as one instrument in the Score window (one brace, bar lines
    through, its name once; an organ's pedals apart from the brace),
    and exported as one part. Selecting either track shows them all.
    Its staves with nothing on them are kept;
  - a long note the score writes as three or more notes tied together
    in a bar, written that way (it was two);
  - chords the score moves to the other staff beamed with the rest of
    their voice, the beam between the staves; notes it writes but
    doesn't play in bars where nothing is played;
  - stems the score turns up or down itself (on a chord or a whole
    beam), and a hand's chords moved onto the other staff kept in a
    voice of their own there, not merged into that hand's chords;
  - a spread chord's wavy line drawn through both of a piano's staves
    when the score spans it (or draws it on down that far), as one
    line, not a stretched one, in MusicXML too (where a score writes
    a spread chord on each staff, lined up, as MuseScore does);
  - dynamics and hairpins over the staff where the score puts them
    (a voice's, as MuseScore does);
  - the pedal as the score shows it: a "Ped." without its star where
    it has none, pedalling it hides ("simile") not shown, its line
    and hooks where it draws one, its words ("Con Ped.") as it writes
    them, and over the staff where it puts it there;
  - the gap between a piano's two staves fitted place by place, as
    MuseScore fits it, not made room for the tallest thing anywhere
    in the line (a dynamic between the staves in one bar and high
    left-hand notes in another no longer push them far apart);
  - words over the staff written partly in bold or italics ("**Vite**
    (inquiet, fiévreux)", "**Poco rit**.") in the score's order, with
    the space between (the plain part came first, run together);
  - tuplets as the score writes them: where it puts each one (off
    the beat too), any ratio (seven in the time of six, four in the
    time of six, duplets, sextuplets over half a beat), its notes'
    written values under one bracket, and its number and bracket
    only where the score shows them (most of a song's are hidden);
    they used to be fitted to a beat's grid, and those that didn't
    fit were written as dotted notes and ties. MusicXML export:
    10,080 divisions a crotchet, so a tuplet's notes add up exactly;
  - breves (double whole notes) in long bars (4/2...), as a breve
    rather than two tied semibreves, and a breve's rest;
  - two-note tremolos in a tuplet (each written twice as long as it
    plays): in their place, as the score writes them;
  - a MusicXML part with a time signature of its own, shorter than the
    others' (a voice's 4/4 over the piano's 12/8, as MuseScore exports
    a staff's own time signature): stretched to fill the bar, as in
    MuseScore, not running out of step with the other parts; the bar's
    time signature the longer one (12/8, not 3/2);
  - a drum part as the score's own kit writes it: each drum on its line
    there, a staff of one line where it has one, and its notes' lengths
    as written (they were all a crotchet or less);
  - a score shown in concert pitch (MuseScore's setting) written in
    concert pitch: a clarinet's notes as they sound;
  - MuseScore 3's multi-measure rests of one bar no longer add a bar;
  - a beam flat over a zig-zag figure (a middle note as high, or as
    low, as the end note nearer the beam), as MuseScore draws it;
  - a tenor's (or a guitar's) treble clef with an 8 under it: the notes
    an octave up on it, as MuseScore writes them.
  - bars the score writes out again are written out again in the
    Score window, and only the score's own repeats are written as
    repeats (with each verse's words under the bars played again).
  Checked against MuseScore 4's own reading of 1,352 songs (OpenScore's
  Lieder), 121 string quartets (OpenScore's), 507 of MuseScore's own
  test scores and the songs as MusicXML: in the songs, the bars written
  exactly as MuseScore writes them went from 22% to 99%, and notes in
  the right place from 64% to 99.7%; in the quartets, 99.6% of bars;
  the songs as MusicXML, 98.7%; MuseScore's test scores, 96%.
- Scores are played as MuseScore plays them: D.C. and D.S., al Fine
  and al Coda (in MusicXML too); a D.C. goes back to the start of its
  own movement (MuseScore's section), and a repeat with no start goes
  back to the movement's start or the end of the repeat before it.
  Measure repeats (the sign to play the last one, two or four
  measures again) are played and written out. MuseScore 3's
  multi-measure rests no longer add bars, and a long
  score (a whole quartet, 2,000 bars) fits in the song (it stopped
  part way: "The song is full"). A D.S. that goes on at a coda in a
  second ending plays it (Beethoven's Op. 130 left out its coda's first
  bar); a verse's
  ending that runs on part way into the next bar covers that bar; an
  ending with no repeat of its own and other endings straight after
  goes on to the next ending of the same number, and the endings are
  counted through the bars between them (Satie's "Je te veux" and
  "Tendrement": verses with "1.", "2." and "Pour finir" endings).
- A bar longer than 32 beats of its time signature (a cadenza bar of
  79/64 in d'Indy's first quartet) keeps its length; the music after
  it was a beat or so out. A bar the score makes longer or shorter
  than its time signature has no time signature written for it (nor
  the one after it), as MuseScore writes none; and a score that ends
  with a short bar has no empty bar after it.
- MusicXML: a hand's notes written on the other hand's staff (as
  MuseScore exports a piano's crossing notes) are kept in their own
  voice there, rather than merged into that staff's (whose long notes
  were cut up and tied around them).
- Score window: beams lie as they should (all but a group's first
  stem were moved twice as far), slope less, and quavers in 3/8 are
  beamed together.

## 0.4.2 (20 November 2026)

- The sustain pedal with SoundFont sounds: a key struck again while the
  pedal holds it lets the old note go (as a piano's string is damped
  and struck again), instead of the two sounding on together, so
  pedalled music (a piano part with the pedal down through repeated
  chords) uses far fewer voices. When every voice is busy, the oldest
  note held only by the pedal makes way for a new one, rather than the
  new note not sounding.
- If making the sound can't keep up (too many SoundFont voices for the
  machine), Groove lets go of the voices least missed (notes dying
  away, then notes held only by the pedal) instead of the desktop
  slowing to a crawl, and never spends long making sound in one go, so
  the desktop stays usable. The log (<Wimp$ScrapDir>.GrooveLog) says
  when it had to.
- At most 64 SoundFont voices sound at once (was 96).
- The log also records loading a score, building and drawing the Score
  window, and Play, with timings.
- Notes start exactly on time, to the sample: before, a note could be
  up to 0.7 ms late (as could MIDI out), from the sound being made in
  blocks; now a block ends where the next note starts. (Notes end at
  the end of a block, within 1.5 ms.)
- A big score (Money, Money, Money: 26 parts) played broken on a Pi in
  0.4.2-test2 to test7. When playing fell behind, 40% of the sounding
  notes were cut at once; now only a few are let go, and only when the
  sound really is about to break up. The SoundFont players have 96 and
  48 voices again, as in 0.4.1 (test2 to test8 had 64 and 32). The log
  records, after each play, any notes cut short for want of a voice and
  any underruns.
- A spread chord spreads only the notes the score marks (a piano's
  right-hand chord, say), not the other hand's notes at the same time.
- Playback takes about half the processor time it did, so more
  SoundFont voices play before anything has to give:
  - the SoundFont player keeps its place in each sample in whole
    numbers, not floating point, and its sound is mixed in directly
    rather than going through 16 bits first;
  - the reverb and mixing work on blocks of samples, not one at a time;
  - the track settings are checked once per batch of sound, not 1,400
    times a second, and songs without audio tracks skip that work.
  The log says how hard playing worked, after each play.
- A 26-part score uses half the memory: the second SoundFont player
  (for parts after the 15th) shares the first's samples instead of
  loading the SoundFont again.
- A score keeps its own marks: dynamics, hairpins, slurs, staccato,
  accents, 8va and 15ma lines, and spread chords (arpeggios, a new mark)
  are read from MuseScore and MusicXML files and written where the
  score has them, instead of being worked out from how the notes are
  played (as they still are for a part played in, or from a MIDI
  file). They're kept in the song file and exported in MusicXML.
  A spread chord plays spread, as MuseScore plays it: its notes start
  one after another, 35 ms apart, from the bottom (or from the top
  for one marked downwards), and end together; an exported MIDI file
  plays them the same way. One marked upwards or downwards has an
  arrowhead at that end of its wavy line. Reported by Paul.
- Tempo changes: a score's or MIDI file's changes of tempo come in with
  it and play (they were left out: everything played at the first
  tempo). The Score window shows each new tempo over its bar (a
  multi-bar rest is broken where the tempo changes, so it's shown); they're
  saved, undone and redone with the song, and exported in MusicXML and
  MIDI files. Changing the tempo on the toolbar changes the later ones
  in proportion. Reported by Paul.
- Opening the Score window with no track selected (after deleting
  one, say) froze the desktop. Reported by Paul.
- The Score window has a close icon. Reported by Paul.
- A score or MIDI file dropped into an empty song starts at bar 1,
  wherever it's dropped (it used to start at the bar it landed on, so
  could start after bars of silence).

## 0.4.1 (19 Nov 2026)

- MuseScore 4 files from 4.2 on open: they list their style file before
  the score, and Groove took the style file for the score ("That isn't
  a MusicXML score"). Reported by Raik.
- MuseScore files: brass and strings now get their own sounds (a horn
  the horn, a violin the violin), not the piano. Their sound is in a
  channel named "open" or "arco", which Groove didn't read.
- A full score can show all 32 tracks (was 16), so a big orchestral
  score shows every part.

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

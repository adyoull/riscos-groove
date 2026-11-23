/* Groove: a music studio for RISC OS.
   groove.h - the portable engine: song model, sequencer, synths, drums,
   mixer, loop library, song files and WAV I/O.

   Nothing in engine/ touches RISC OS. It is plain C99 with floats (VFPv3 on
   the Pi), so it builds and is tested on Linux, and the RISC OS front end
   only has to (a) call gr_render_words() to fill the sound ring buffer and
   (b) turn mouse clicks and key presses into the calls below.

   Time is measured in ticks: GR_PPQ ticks per quarter note (beat). A bar is
   4 beats (4/4 only for now). One sixteenth note = GR_PPQ/4 ticks. */
#ifndef GROOVE_H
#define GROOVE_H

#include <stddef.h>
#include <stdint.h>

#define GR_RATE          44100       /* the engine's own rate; SharedSound resamples */
#define GR_PPQ           96
#define GR_BAR           (4 * GR_PPQ)  /* four beats: a 4/4 bar, and the zoom scale */
#define GR_STEP          (GR_PPQ / 4)  /* one sixteenth */

#define GR_MAX_TRACKS    32
#define GR_MAX_REGIONS   1024          /* room for a long imported MIDI file:
                                          16 parts of 4-bar regions, 256 bars */
#define GR_MAX_PATTERNS  1024
#define GR_MAX_NOTES     384           /* per pattern */
#define GR_MAX_CTLS      512           /* controller events per pattern */
#define GR_MAX_MARKS     256           /* written marks per pattern */
#define GR_MAX_CLIPS     16
#define GR_MAX_METERS    256           /* time signature changes */
#define GR_MAX_TEMPOS    256           /* tempo changes */
#define GR_NAME          24

#define GR_DRUM_SOUNDS   8           /* Groove's own drum sounds (GV_*) */

typedef enum { TRK_DRUMS = 0, TRK_INST = 1, TRK_AUDIO = 2 } gr_track_type;

/* Drum notes are MIDI keys, 0..127, as in General MIDI's drum map. These
   are the eight that Groove's own kits have a sound of their own for
   (the step grid's standard rows); any other key plays the nearest of
   them (gr_drum_voice), and its own sound on a SoundFont kit or a MIDI
   instrument. Before 0.3 drum notes were 0..7, these in this order: song
   files of that age are converted when they're loaded. */
enum { DR_KICK = 36, DR_SNARE = 38, DR_CHAT = 42, DR_OHAT = 46, DR_CLAP = 39,
       DR_LTOM = 45, DR_HTOM = 50, DR_PERC = 56 };
/* Groove's own drum sounds (the voices of its kits). */
enum { GV_KICK, GV_SNARE, GV_CHAT, GV_OHAT, GV_CLAP, GV_LTOM, GV_HTOM, GV_PERC };

enum { PATCH_EPIANO, PATCH_BASS, PATCH_PAD, PATCH_LEAD, PATCH_PLUCK,
       PATCH_ORGAN, PATCH_BELL, PATCH_COUNT };

enum { KIT_STUDIO, KIT_ELECTRO, KIT_COUNT };

typedef struct
{
  int16_t tick;                 /* from the start of the pattern */
  int16_t len;                  /* ticks (ignored for drums) */
  uint8_t pitch;                /* MIDI note 0..127 (drums too: DR_* and the rest) */
  uint8_t vel;                  /* 1..127 */
  uint8_t wr;                   /* how a score Groove imported wrote it (GR_WR_*), or 0 */
  uint8_t spell;                /* and its accidental: GR_SP | (alter + 2), alter -2..2; or 0 */
  int16_t tie1;                 /* written as notes tied together: the first one's length in
                                   ticks (the rest as Groove writes it); 0 if not; or GR_TIE_* */
} gr_note;

/* gr_note.tie1 for a score's tie between notes that aren't next to each
   other (from an arpeggio's notes to the chord they ring into): this note
   is tied on to a later one, from an earlier one, or both. Each is played. */
#define GR_TIE_OUT  (-1)
#define GR_TIE_IN   (-2)
#define GR_TIE_BOTH (-3)

/* gr_note.wr: a note from an imported score (MuseScore, MusicXML) keeps
   the staff and voice it was written in, so the Score window writes it
   there rather than working them out from the pitches. */
#define GR_WR          0x80     /* written: the rest of wr says how */
#define GR_WR_VOICE    0x03     /* its voice on the staff, 0..3 */
#define GR_WR_STAFF    0x04     /* on the lower staff of a two-staff part */
#define GR_WR_HALF     0x08     /* played for half its written length (staccato) */
#define GR_WR_CROSS    0x10     /* moved to the other staff (staff is where it's shown) */
#define GR_WR_TUPLEN   0x60     /* in a tuplet as long as: 0x20 less than a beat, 0x40 a beat,
                                   0x60 two beats (0: not known) */
#define GR_SP_TUPLET   0xE0     /* gr_note.spell: its tuplet (GR_TUP_* << 5) */
enum { GR_TUP_NONE, GR_TUP_3, GR_TUP_5, GR_TUP_6, GR_TUP_2, GR_TUP_4, GR_TUP_7, GR_TUP_OTHER };
/* The tuplet bits (wr | spell << 8) for act notes in the time of norm,
   the whole of it glen quarters long, in a bar whose beat is beat quarters. */
int gr_tuplet_bits (int act, int norm, double glen, double beat);
/* A GR_MK_NOTE mark's staff, from the note's wr (with its spelling << 8):
   its staff (1), moved there from the other (2), tied on from the note
   playing then (4, tied), its tuplet (<< 3) and tuplet length (<< 6). */
#define gr_mknote_staff(wr, tied) (((wr) & GR_WR_STAFF ? 1 : 0) | ((wr) & GR_WR_CROSS ? 2 : 0) | ((tied) ? 4 : 0) \
                                   | (((wr) >> 13) & 7) << 3 | (((wr) & GR_WR_TUPLEN) >> 5) << 6)
/* A rest mark's value (GR_MK_REST) with its tuplet: gr_tuplet_bits' as
   (type << 2 | length), above the voice. */
#define gr_rest_tuplet(bits) ((((bits) >> 13) & 7) << 2 | ((bits) & 0x60))
#define GR_SP          0x10     /* gr_note.spell: spelled with an accidental, (spell & 7) - 2 */
#define GR_SP_HIDDEN   0x08     /* gr_note.spell: a score's note it plays but doesn't show */
/* Copies how a note was written (wr, spell, tie1) from one to another. */
#define GR_NOTE_WRITTEN(dst, src) ((dst)->wr = (src)->wr, (dst)->spell = (src)->spell, (dst)->tie1 = (src)->tie1)

/* A controller event: a MIDI Control Change (type 0..127, value 0..127),
   or a program change, pitch bend (-8192..8191, 0 is the middle) or
   channel pressure. They play on SoundFont sounds and MIDI out; Groove's
   own synths take no notice of them. */
enum { GR_CTL_PROGRAM = 128, GR_CTL_BEND = 129, GR_CTL_PRESSURE = 130, GR_CTL_TYPES = 131 };

typedef struct
{
  int16_t tick;                 /* from the start of the pattern */
  uint8_t type;                 /* 0..127: CC number; GR_CTL_* */
  uint8_t spare;
  int16_t value;
} gr_ctl;

/* A mark written in a score Groove imported (MuseScore, MusicXML): kept
   with the notes so the Score window writes what the score said, rather
   than working it out from how the notes are played. A part with none
   (played in, or from a MIDI file) is worked out as before. */
enum
{
  GR_MK_SLUR = 1,               /* tick to tick+len (the last note's start) */
  GR_MK_HAIRPIN,                /* value 1 crescendo, 2 diminuendo, + GR_MK_ABOVE: over the
                                   staff; tick to tick+len */
  GR_MK_DYNAMIC,                /* value: ND_PPP..ND_FFF (notation.h); pitch 1: over the staff */
  GR_MK_OTTAVA,                 /* value 1 8va, -1 8vb, 2 15ma, -2 15mb; to tick+len */
  GR_MK_ARPEGGIO,               /* the chord at tick is spread: value 1, 2 upwards (an arrow),
                                   -1 downwards, + GR_ARP_SPAN (- for down): its line down
                                   through the staff below too (pitch: how many, 1 if 0);
                                   its notes from pitch up to
                                   len (0, 0: all); staff >> 1: half spaces the score
                                   draws its line on down, past the chord */
  GR_MK_STACCATO,               /* the note (pitch; 0 every note) at tick */
  GR_MK_ACCENT,
  GR_MK_CLEF,                   /* the staff's clef changes to value (NC_*, notation.h) */
  GR_MK_TENUTO,                 /* the chord at tick: a tenuto line */
  GR_MK_MARCATO,                /* a strong accent (^) */
  GR_MK_FERMATA,                /* a pause over the chord or rest at tick (value -1: under it) */
  GR_MK_ORNAMENT,               /* value 1 a trill, 2 a mordent, 3 a turn */
  GR_MK_GLISS,                  /* a glissando from the note (pitch) at tick to the next */
  GR_MK_GRACE,                  /* a grace note (pitch) before the chord at tick: value its
                                   NV_* (| 0x40 slashed: an acciaccatura); len its place, 0 first */
  GR_MK_REST,                   /* a rest the score writes, len long, in voice value & 3, its
                                   tuplet (gr_rest_tuplet) above */
  GR_MK_TREMOLO,                /* a tremolo: value its strokes (1..4); | 0x10 between the chord
                                   at tick and the next (len the two), each written as NV_* pitch & 15,
                                   with pitch >> 4 dots */
  GR_MK_NOTE,                   /* a note the score writes but Groove doesn't play (a unison
                                   in another voice, a cue note): pitch, len long; value its
                                   voice (& 3) and spelling (gr_note.spell, >> 2), | 0x80 a cue
                                   note (shown small); staff as gr_mknote_staff */
  GR_MK_BEAM,                   /* how the score beams the chord at tick: value GR_BEAM_* | its
                                   voice << 3 */
  GR_MK_TIE,                    /* a long note the score writes as three or more tied: one
                                   more of its ties, at tick (the note pitch, in voice value & 3,
                                   on staff & 1; its first is gr_note.tie1) */
  GR_MK_STEM,                   /* the score sets the chord at tick's stem: value 1 up, 2 down,
                                   | its voice << 3 */
  GR_MK_NOPEDAL,                /* a pedal line the score doesn't show all of: from tick (down:
                                   value & 1, its "Ped." not shown) to tick + len (up: value & 2,
                                   its star not shown); value & 4: a line from one to the other
                                   instead (with a hook at its end, and its start if no
                                   "Ped."); value & 8: over the staff, not under; it still
                                   plays */
  GR_MK_TUPLET,                 /* a score's tuplet, tick to tick + len: pitch notes in the time
                                   of staff >> 1, each written value & 0x1c >> 2 (NV_*), in voice
                                   value & 3, on staff & 1; value & 0x20 inside another, 0x40 its
                                   number not shown, 0x80 no bracket */
  GR_MK_KINDS
};

#define GR_ARP_SPAN 4
#define GR_MK_ABOVE 4             /* (a hairpin's value: over the staff) */

/* GR_MK_BEAM: a beam starts at the chord, goes on through it, it has none
   (a flag), or a beam ends with it. */
enum { GR_BEAM_AUTO, GR_BEAM_BEGIN, GR_BEAM_MID, GR_BEAM_NONE, GR_BEAM_END };

typedef struct
{
  int16_t tick;                 /* from the start of the pattern */
  int16_t len;                  /* ticks, for marks that span */
  uint8_t kind;                 /* GR_MK_* */
  int8_t value;
  uint8_t pitch;
  uint8_t staff;                /* 0, or 1: the lower staff of a piano part */
} gr_mark;

typedef struct
{
  char name[GR_NAME];
  int used;
  int len;                      /* ticks; the region loops it */
  int nnotes;
  gr_note notes[GR_MAX_NOTES];  /* kept sorted by tick */
  int nctls;
  gr_ctl ctls[GR_MAX_CTLS];     /* kept sorted by tick */
  int nmarks;
  gr_mark marks[GR_MAX_MARKS];  /* kept sorted by tick */
} gr_pattern;

typedef struct
{
  char name[GR_NAME];
  int used;
  int16_t *data;                /* interleaved stereo, 16-bit */
  int frames, cap;              /* frames used / allocated */
  int rate;
  unsigned id;                  /* unique while the program runs (for undo) */
} gr_clip;

typedef struct
{
  int used;
  int track;
  int start, len;               /* ticks */
  int pattern;                  /* drums / instrument: index into patterns */
  int clip;                     /* audio: index into clips */
  int clip_offset;              /* audio: first frame played */
} gr_region;

typedef struct
{
  char name[GR_NAME];
  gr_track_type type;
  int patch;                    /* PATCH_* (instrument) or KIT_* (drums) */
  int gm;                       /* -1: Groove's own sound (patch). Otherwise a
                                   SoundFont sound, played by SoftMIDI's
                                   library: a General MIDI program 0..127, or
                                   on a drum track a GM kit (GM_KIT_*) */
  float vol;                    /* 0..1 */
  float pan;                    /* -1..1 */
  float reverb;                 /* send, 0..1 */
  int mute, solo, armed;
  int midi_out;                 /* 1: played on a MIDI device (gr_engine_set_midi_out)
                                   rather than by Groove, where there is one */
  /* For MIDI out and exported MIDI files; -1 for each means automatic. */
  int channel;                  /* 0..15 (shown 1..16); auto: drums 10, each
                                   instrument track the next free channel */
  int bank_msb, bank_lsb;       /* Bank Select (CC 0 and 32) before the program */
  int program;                  /* 0..127; auto: the sound's GM program */
  float meter;                  /* peak level for the UI, decays */
  /* How a score Groove imported wrote this part (written != 0), rather
     than as Groove would by its sound: */
  int written;
  int wtranspose;               /* semitones written above how it sounds */
  int wclef[2];                 /* each staff's clef at the start: NC_* + 1, or 0 */
  int wpedal;                   /* 1: the score doesn't show its pedalling (it plays) */
  int joined;                   /* GR_JOIN_*: its staves go on from the track before
                                   (one instrument on three or more staves) */
  int wlines;                   /* 1: written on a staff of one line (a score's drum); 0 five */
  unsigned char drumline[128];  /* a drum track from a score: each key's line as its kit has it
                                   (0 the top line, down in half spaces) + 64; 0 Groove's own */
} gr_track;

/* gr_track.joined: one brace over both tracks' staves, the name once; or
   (an organ's pedals) the bar lines through, a brace each. */
enum { GR_JOIN_NONE, GR_JOIN_BRACE, GR_JOIN_BARS };

/* A time signature from a bar (0-based) on: num beats of a 1/den note.
   den is 1, 2, 4, 8, 16, 32 or 64. */
typedef struct
{
  int bar, num, den;
  int symbol;                   /* written as a symbol: 1 common time (C), 2 cut (alla breve);
                                   3 not written (a bar of a score longer or shorter than its own) */
} gr_meter;

/* A key signature written in a score Groove imported, from a bar on:
   sharps (+) or flats (-) as it sounds (concert), and major or minor.
   A song with none has its keys worked out from its notes. */
typedef struct
{
  int bar, fifths, minor;
} gr_keysig;
#define GR_MAX_KEYSIGS   64

/* A syllable of a song's words, from a score Groove imported: under the
   note starting at tick on track's staff (0, or 1 the lower). syllabic:
   0 a word, 1 a word's first syllable, 2 one in the middle, 3 its last
   (a hyphen joins one to the next). */
#define GR_LYRIC 48
typedef struct
{
  int track, tick;
  unsigned char staff, verse, syllabic, spare;
  char text[GR_LYRIC];          /* Latin-1 */
} gr_lyric;

/* gr_lyric.verse for a score's words that aren't sung: written over the
   staff (as "dolce", "rit."), or under it (as "sostenuto"). */
#define GR_TEXT_ABOVE 100
#define GR_TEXT_BELOW 101
/* ...and a tempo's words ("Andante."), over the staff in bold, its
   metronome mark (if shown) after them; ending "(" if the mark is in
   brackets */
#define GR_TEXT_TEMPO 102
#define GR_TEXT_PEDAL 104         /* ...a pedal's words ("Con" Ped., "simile"), on its line */
#define GR_TEXT_TEMPO_MARK 103    /* (a metronome mark in it: the whole text, as "Andante. (\x86 = 66)") */
/* A metronome mark's notes in a score's text (Latin-1's unused 0x86..):
   a crotchet, minim, quaver, and a dot after one. */
#define GR_CH_NOTE4 0x86
#define GR_CH_NOTE2 0x87
#define GR_CH_NOTE8 0x88
#define GR_CH_NOTEDOT 0x89

/* A tempo change: from tick on, bpm crotchets a minute. */
typedef struct
{
  int tick;
  float bpm;
  int hidden;                   /* a score's: no metronome mark written for it */
} gr_tempo;

typedef struct
{
  int tempo;                    /* BPM (crotchets a minute) at the start */
  int ntempos;                  /* changes after the start, in order of tick */
  gr_tempo tempos[GR_MAX_TEMPOS];
  int key;                      /* 0 = C .. 11 = B; loops are transposed into it */
  int bars;                     /* song length */
  int loop_on, loop_start, loop_end;   /* ticks */
  int metronome;
  float master;
  int nmeters;                  /* meters[0].bar is always 0 */
  gr_meter meters[GR_MAX_METERS];
  int tempo_hidden;             /* a score's: no metronome mark written for its first tempo */
  int pickup;                   /* a score's pickup (anacrusis): the ticks at the start of the
                                   first bar that its score leaves out (0: none) */
  gr_lyric *lyrics;             /* the words, in no order (malloc'd; gr_song_free frees them) */
  int nlyrics, lyriccap;
  unsigned short *srcbar;       /* a score's: the measure each bar is played from, + 1 (0: not
                                   one), so the Score window writes only its repeats as repeats
                                   (malloc'd; gr_song_free frees it) */
  int nsrcbar;
  int nkeysigs;                 /* written key signatures, in order of bar */
  gr_keysig keysigs[GR_MAX_KEYSIGS];
  int ntracks;
  gr_track tracks[GR_MAX_TRACKS];
  gr_region regions[GR_MAX_REGIONS];
  gr_pattern patterns[GR_MAX_PATTERNS];
  gr_clip clips[GR_MAX_CLIPS];
} gr_song;

typedef struct gr_engine gr_engine;

/* ---- song model (song.c) ---------------------------------------------- */
void gr_song_init (gr_song *s);
void gr_song_free (gr_song *s);                  /* frees audio clips */
int  gr_track_add (gr_song *s, gr_track_type t, const char *name, int patch);
void gr_track_delete (gr_song *s, int track);
void gr_track_move (gr_song *s, int from, int to);   /* its regions go with it */
int  gr_pattern_new (gr_song *s, const char *name, int len);
int  gr_pattern_add_note (gr_pattern *p, int tick, int len, int pitch, int vel);
int  gr_pattern_find_note (const gr_pattern *p, int tick, int pitch);
void gr_pattern_delete_note (gr_pattern *p, int index);
/* Controller events. Set: one of this type at this tick (replacing one
   there); returns its index, or -1 if the pattern is full. */
int  gr_pattern_set_ctl (gr_pattern *p, int tick, int type, int value);
int  gr_pattern_find_ctl (const gr_pattern *p, int tick, int type);
/* A written mark (from an imported score); its index, or -1 if full. */
int  gr_pattern_add_mark (gr_pattern *p, const gr_mark *m);
/* Is the chord at tick spread (an arpeggio mark)? 1 from the bottom up,
   -1 from the top down, 0 not. And note k's turn in its spread chord (0
   first), with how many notes the chord has in *count. */
int  gr_arpeggio_at (const gr_pattern *p, int tick);
const gr_mark *gr_arpeggio_mark (const gr_pattern *p, int tick);
int  gr_arpeggio_has (const gr_mark *m, int pitch);     /* is this note in its spread? */
int  gr_arpeggio_turn (const gr_pattern *p, int k, int *count);
#define GR_ARP_STEP_MS 35       /* between the notes of a spread chord */
void gr_pattern_delete_ctl (gr_pattern *p, int index);
/* The value in force at tick (the last event at or before it), or dflt. */
int  gr_ctl_value_at (const gr_pattern *p, int tick, int type, int dflt);
/* A type's range, its value when nothing has set it, and its name
   ("Modulation (CC 1)", "Pitch bend"). */
int  gr_ctl_min (int type);
int  gr_ctl_max (int type);
int  gr_ctl_default (int type);
const char *gr_ctl_name (int type);
int  gr_region_add (gr_song *s, int track, int start, int len, int pattern);
void gr_region_delete (gr_song *s, int region);
int  gr_region_at (const gr_song *s, int track, int tick);
int  gr_pattern_unshare (gr_song *s, int region); /* copy-on-edit */
int  gr_region_split (gr_song *s, int region, int tick);   /* the new right part, or -1 */
/* Paste: a copy of a region (and its pattern, which may be a saved copy)
   at tick on track, with a pattern of its own. */
int  gr_region_copy (gr_song *s, const gr_region *r, const gr_pattern *p, int track, int tick);
int  gr_clip_new (gr_song *s, const char *name, int rate);
int  gr_clip_append (gr_clip *c, const int16_t *stereo, int frames);
void gr_song_gc (gr_song *s);                    /* drop unused patterns/clips */
int  gr_song_end (const gr_song *s);             /* last tick of music */

/* Time signatures: bars can be any length. Bars are numbered from 0 here
   (the display adds 1). Ticks before 0 count as bar 0. */
const gr_meter *gr_meter_at (const gr_song *s, int bar);
int  gr_bar_tick (const gr_song *s, int bar);    /* where a bar starts */
int  gr_tick_bar (const gr_song *s, int tick);   /* the bar a tick is in */
int  gr_bar_len (const gr_song *s, int bar);     /* in ticks */
int  gr_beat_len (const gr_song *s, int bar);    /* one beat of that bar's meter */
int  gr_bar_snap (const gr_song *s, int tick);   /* the nearest bar start */
/* From bar on (until the next change). 0, or -1 if it isn't a valid meter
   or there's no room. */
int  gr_set_meter (gr_song *s, int bar, int num, int den);
/* A written key signature from bar on (replacing one there). 0, or -1
   if there's no room. */
int  gr_set_keysig (gr_song *s, int bar, int fifths, int minor);
/* A syllable (text in Latin-1). 0, or -1 if there's no memory. */
int  gr_lyric_add (gr_song *s, int track, int tick, int staff, int verse, int syllabic, const char *text);
/* Bar bar is played from a score's measure (from 0); -1 if out of memory. */
int  gr_set_srcbar (gr_song *s, int bar, int measure);
/* The time signature from bar on written as a symbol (gr_meter.symbol). */
void gr_set_meter_symbol (gr_song *s, int bar, int symbol);

/* Tempo: s->tempo at the start, then any changes (from a score or a MIDI
   file). Times are from tick 0. Changing the starting tempo
   (gr_tempo_base) changes every later one in proportion, so a piece can
   be slowed down for practice as a whole. */
double gr_tempo_at (const gr_song *s, int tick);           /* BPM in force */
double gr_tick_seconds (const gr_song *s, double tick);
double gr_seconds_tick (const gr_song *s, double secs);
int  gr_tempo_add (gr_song *s, int tick, double bpm);      /* 0, or -1: no room */
void gr_tempo_base (gr_song *s, int bpm);
void gr_song_cover (gr_song *s, int end_tick);   /* make the song long enough */

/* ---- loop library (loops.c) ------------------------------------------- */
typedef struct
{
  const char *name;
  const char *category;         /* "Drums", "Bass", "Keys", "Lead" */
  gr_track_type type;
  int patch;                    /* the patch or kit it sounds best with */
  int bars;
} gr_loop_info;

int  gr_loop_count (void);
const gr_loop_info *gr_loop_get (int i);
/* Make a pattern from loop i, transposed into the song's key. Returns the
   pattern index or -1. */
int  gr_loop_make (gr_song *s, int i);
/* The same into a pattern of the caller's (for previews). */
int  gr_loop_fill (int i, int key, gr_pattern *p);
/* Drop loop i on the song at (track, tick): makes the pattern and a region
   of the loop's length. track < 0 means a new track of the right type.
   Returns the region index or -1. */
int  gr_loop_place (gr_song *s, int i, int track, int tick);

/* ---- engine (engine.c) ------------------------------------------------- */
gr_engine *gr_engine_new (gr_song *s);
void gr_engine_free (gr_engine *e);
void gr_engine_set_song (gr_engine *e, gr_song *s);

void gr_play (gr_engine *e);
void gr_stop (gr_engine *e);                     /* stop; second stop rewinds */
void gr_record (gr_engine *e);                   /* play and record armed tracks */
/* Before recording: a count-in of this many beats (0 = none). */
void gr_set_count_in (gr_engine *e, int beats);
int  gr_counting_in (const gr_engine *e);        /* beats of count-in left, or 0 */
/* Merge recordings into the regions they're recorded over. */
void gr_set_merge (gr_engine *e, int on);
void gr_locate (gr_engine *e, int tick);
int  gr_playing (const gr_engine *e);
int  gr_recording (const gr_engine *e);
double gr_position (const gr_engine *e);         /* ticks, fractional */
int  gr_busy (const gr_engine *e);               /* anything still sounding? */

/* MIDI out. With a sender set, tracks with midi_out on are played by
   sending MIDI instead of by Groove: their notes, and program, volume (CC7)
   and pan (CC10) when they change. Each instrument track has a channel of
   its own, in order and skipping channel 10 (index 9), which the drum
   tracks share, as in exported MIDI files. A command is packed as MIDI
   modules pack them: status | data1 << 8 | data2 << 16 | length << 24.
   `frame` is when it should be heard, in the frames gr_frames counts (0:
   now, for notes played live); the sender holds commands until the sound
   rendered with them is being heard. Without a sender (bouncing to a WAV,
   say), midi_out tracks play Groove's or the SoundFont's sound as usual. */
typedef void gr_midi_sender (void *ctx, unsigned frame, unsigned cmd);
void gr_engine_set_midi_out (gr_engine *e, gr_midi_sender *fn, void *ctx);
/* Notes off on every channel MIDI out has used, and everything resent:
   after a track's midi_out has been changed, say. */
void gr_midi_out_reset (gr_engine *e);
unsigned gr_frames (const gr_engine *e);        /* frames rendered so far */
int  gr_patch_program (int patch);              /* Groove's sound's nearest GM program */
/* A track's MIDI channel (0..15) and program (0..127) for MIDI out and
   MIDI files: its own if it has one, else the automatic one. */
int  gr_track_channel (const gr_song *s, int track);
int  gr_track_program (const gr_track *t);

/* Live playing (musical typing, MIDI, clicks on the piano roll). */
void gr_live_on (gr_engine *e, int track, int pitch, int vel);
void gr_live_off (gr_engine *e, int track, int pitch);
/* Preview: plays p in a loop (it must stay valid until gr_preview_stop). */
void gr_preview_pattern (gr_engine *e, int track_type, int patch, const gr_pattern *p);
int  gr_previewing (const gr_engine *e);
void gr_preview_stop (gr_engine *e);

/* Audio input (from audioin.h's backend) while recording. */
void gr_audio_in (gr_engine *e, const int16_t *stereo, int frames);

/* Render: n frames of float stereo, or of RISC OS 16-bit words
   (left in the top 16 bits, right in the bottom 16 - PRM "16 bit linear
   sound handler"). */
void gr_render (gr_engine *e, float *l, float *r, int n);
void gr_render_words (gr_engine *e, uint32_t *out, int n);

/* Offline bounce of the whole song (plus a reverb tail) to a WAV file. */
int  gr_bounce (gr_song *s, const char *path);

/* ---- files (songfile.c, wav.c) ----------------------------------------- */
int  gr_song_save (const gr_song *s, const char *path);
int  gr_song_load (gr_song *s, const char *path);
int  gr_wav_write (const char *path, const int16_t *stereo, int frames, int rate);
int  gr_wav_read (const char *path, int16_t **stereo, int *frames, int *rate);
int  gr_wav_import (gr_song *s, const char *path, int track, int tick);

/* ---- Undo and Redo (history.c) ------------------------------------------ */
typedef struct gr_history gr_history;
gr_history *gr_history_new (int max_levels, size_t max_bytes);
void gr_history_free (gr_history *h);
/* Start again from this song (a new or loaded song: the saved state). */
void gr_history_reset (gr_history *h, const gr_song *s);
/* After the user has done something: remember the song if it changed.
   Returns 1 if it had. */
int  gr_history_note (gr_history *h, const gr_song *s);
int  gr_history_can_undo (const gr_history *h);
int  gr_history_can_redo (const gr_history *h);
int  gr_history_undo (gr_history *h, gr_song *s);   /* 1 if it did */
int  gr_history_redo (gr_history *h, gr_song *s);
void gr_history_saved (gr_history *h);              /* the song was just saved */
int  gr_history_is_saved (const gr_history *h);     /* as saved (no unsaved changes)? */

/* ---- SoundFont sounds (gm.c), through SoftMIDI's libmidisynth --------- */
/* One SoundFont serves every engine. gr_gm_load(NULL) loads the one named
   by SoftMIDI$SoundFont (set when the Filer has seen !SoftMIDI; on Linux,
   MIDISYNTH_SOUNDFONT). Loading takes a few seconds, so it is done when
   first needed. Tracks with a SoundFont sound play Groove's own sound
   until it is loaded. */
enum { GM_KIT_STANDARD = 0, GM_KIT_ROOM = 8, GM_KIT_POWER = 16, GM_KIT_ELECTRONIC = 24,
       GM_KIT_808 = 25, GM_KIT_JAZZ = 32, GM_KIT_BRUSH = 40, GM_KIT_ORCHESTRA = 48 };
#define GM_NKITS 8
int  gr_gm_load (const char *soundfont);         /* 0, or -1: see gr_gm_error */
int  gr_gm_ready (void);
void gr_gm_unload (void);
/* A 16th track playing a SoundFont instrument needs a second synth (MIDI
   has 15 channels that aren't for drums). Until it's loaded, that track
   plays Groove's nearest sound. */
int  gr_gm_need_second (const gr_song *s);
int  gr_gm_second_ready (void);
int  gr_gm_load_second (void);                   /* 0, or -1 (gr_gm_error) */
const char *gr_gm_error (void);
int  gr_gm_voices (void);            /* SoundFont voices sounding */
int  gr_gm_stolen (void);    /* notes cut off for want of a voice, so far */
void gr_gm_shed (int percent);       /* end the least missed, leaving percent of them */
const char *gr_gm_program_name (int program);    /* "Acoustic Grand Piano" ... */
const char *gr_gm_family_name (int family);      /* 0..15: "Piano" ... */
int  gr_gm_kit (int i);                          /* i-th kit's program, i < GM_NKITS */
const char *gr_gm_kit_name (int program);
int  gr_gm_drum_note (int voice);                /* GV_* -> its GM drum key (DR_*) */
int  gr_song_uses_gm (const gr_song *s);
/* The name of a track's sound, whichever kind it is. */
const char *gr_sound_name (const gr_track *t);

/* ---- MIDI files (midifile.c) ------------------------------------------- */
/* Import a Standard MIDI File: a new track per MIDI channel that has notes
   (up to the free tracks), with SoundFont sounds, cut into 4-bar regions
   from `tick`. If the song has no music yet, its (empty) tracks make way
   and it takes the file's first tempo and its time signatures.
   Returns the number of tracks made, or -1 (and *why says why). */
int  gr_midi_import (gr_song *s, const char *path, int tick, const char **why);
/* Export the song as a type 1 MIDI file: a MIDI track per Groove track
   (not audio), drums on channel 10. */
int  gr_midi_export (const gr_song *s, const char *path);
/* Which of Groove's own drum sounds (GV_*) plays a drum key (35 kick, 38
   snare, 42 hi-hat...): its own, or the nearest. */
int  gr_drum_voice (int key);

/* ---- Maestro files (maestro.c) -------------------------------------------- */
/* Import a RISC OS Maestro file (&AF1): a track per channel that has notes,
   played as Maestro plays it (see maestro.c), from `tick`. voices[] are the
   names of the computer's voices (voices[0] is voice 1), so each channel
   gets the sound Maestro would give it; NULL means RISC OS 5's own nine.
   If the song has no music yet, its empty tracks make way, and it takes
   the tempo, key and time signatures too.
   Returns the number of tracks made, or -1 (and *why says why). */
int  gr_maestro_import (gr_song *s, const char *path, int tick,
                        const char *const *voices, int nvoices, const char **why);

/* ---- MuseScore and MusicXML (score.c, mscx.c, musicxml.c) ------------------ */
/* What kind of written score a file is, from what's in it (files from a
   PC or a USB stick may have no filetype): GR_SCORE_NONE if it isn't one. */
enum { GR_SCORE_NONE, GR_SCORE_MSCZ, GR_SCORE_MSCX, GR_SCORE_MXL, GR_SCORE_MUSICXML };
int  gr_score_kind (const char *path);
/* Import a MuseScore file (.mscz or .mscx, MuseScore 2 to 4) or MusicXML
   (.musicxml, .xml or compressed .mxl): a new track per part, with its
   instrument as a SoundFont sound, repeats played out and tied notes
   joined, from `tick`. Like MIDI import, a song with no music yet takes
   the score's tempo and time signatures, and its empty tracks make way.
   Returns the number of tracks made, or -1 (and *why says why). */
int  gr_score_import (gr_song *s, const char *path, int tick, const char **why);
/* Export the song as MusicXML (uncompressed), which MuseScore, Sibelius,
   Dorico and Finale open: a part per drum or instrument track, notes
   written as notation (note values, ties, rests, a key signature from the
   song's key). Returns 0, or -1. */
int  gr_musicxml_export (const gr_song *s, const char *path);
/* The same, with gr_notation_make_ex's flags (notation.h): the tests use
   it to compare a score with the one it was imported from. */
int  gr_musicxml_export_ex (const gr_song *s, const char *path, int flags);

/* ---- names for the UI --------------------------------------------------- */
const char *gr_patch_name (int patch);
const char *gr_kit_name (int kit);
/* A drum key's name: Groove's for its own eight ("Kick"), General MIDI's
   for the rest of 35..81 ("Crash Cymbal 1"), else the note ("C4 60"). */
const char *gr_drum_name (int key);
/* The step grid's standard rows: Groove's eight drums' keys, in order. */
int  gr_drum_kit_key (int row);
const char *gr_note_name (int pitch, char buf[8]);
const char *gr_key_name (int key);

#endif

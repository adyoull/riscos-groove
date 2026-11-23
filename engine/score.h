/* Groove - a written score on its way into the song (score.c), shared by
   the MuseScore (mscx.c) and MusicXML (musicxml.c) importers.

   The readers fill in measures and each part's notes, positioned within
   their measure in quarter notes; gs_place plays the repeats out, joins
   tied notes and puts the parts on new tracks, as MIDI import does. */
#ifndef SCORE_H
#define SCORE_H
#include "groove.h"

enum { GS_TIE_FWD = 1, GS_TIE_BACK = 2 };

#define GS_STAVES 8              /* staves a part may have */

typedef struct
{
  int measure;
  double off, len;              /* quarter notes, from the measure's start */
  int pitch, vel, tie;
  int wr;                       /* gr_note.wr: its staff and voice, and if it plays short */
  int st;                       /* the part's staff it's on (0, 1, 2...: gs_part.cur_staff) */
} gs_note;

typedef struct
{
  int measure;
  double off;                   /* quarter notes, from the measure's start */
  int down;                     /* the sustain pedal goes down (1) or up (0) */
} gs_pedal;

/* A mark written in the score (gr_mark's kinds): where it is, and for one
   that spans (a slur, hairpin, 8va line) where it ends, as a measure and
   an offset that may run past that measure's end or before its start. */
typedef struct
{
  int measure, emeasure;        /* emeasure -1: it doesn't span */
  double off, eoff;
  int kind, value, pitch, staff;
  int hi;                       /* an arpeggio: its chord's notes from pitch to hi; a grace
                                   note: its place before the chord (0 first) */
  int st;                       /* the part's staff it's on (gs_part.cur_staff) */
} gs_mark;

/* A syllable of the words, under the chord at measure, off. */
typedef struct
{
  int measure;
  double off;
  int staff, verse, syllabic;   /* gr_lyric's */
  int st;                       /* the part's staff (gs_part.cur_staff) */
  char text[GR_LYRIC];          /* Latin-1 */
} gs_lyric;

/* A tempo change. */
typedef struct
{
  int measure;
  double off;
  double bpm;                   /* quarters a minute */
  int hidden;                   /* no metronome mark written for it (words only, or none) */
} gs_tempo;

typedef struct
{
  char name[GR_NAME];
  int program, bank_msb, bank_lsb, channel;   /* -1: not given */
  int drums;
  int lines;                    /* 1: a staff of one line (a drum's); else five */
  unsigned char drumline[128];  /* the score's drum kit: each key's line (MuseScore's: 0 the
                                   top line, down in half spaces) + 64; 0 as Groove places it */
  gs_note *notes;
  int n, cap;
  gs_pedal *pedals;
  int npedals, pedcap;
  gs_mark *marks;
  int nmarks, markcap;
  gs_lyric *lyrics;
  int nlyrics, lyriccap;
  int has_transpose, transpose;  /* written this many semitones above how it sounds */
  int clef[GS_STAVES];          /* each staff's first clef: NC_* + 1, or 0 */
  int pedal_shown, pedal_hidden; /* its pedal lines the score shows, and those it doesn't */
  /* Its staves: a reader says which one it's adding to (0 the top); a part
     on three or more is made two or more parts on one or two staves each
     (gs_split_staves), joined: one instrument. */
  int cur_staff;
  int joined;                   /* GR_JOIN_*: it goes on from the part before it */
  int organ;                    /* (an organ: its pedals' staff apart, not braced) */
  int vocal;                    /* a voice's: its dynamics and hairpins above the staff, unless
                                   the score says */
} gs_part;

typedef struct
{
  double len;                   /* quarter notes */
  int num, den;                 /* the time signature in force */
  int timesym;                  /* its symbol where it's written: 1 C, 2 cut C (gr_meter.symbol) */
  int start_repeat;             /* a repeat starts here */
  int end_repeat;               /* times played, if a repeat ends here (0 none) */
  unsigned endings;             /* the volta it's in: bit n = ending n+1; 0 none */
  /* Jumps (D.C., D.S., al Fine, al Coda): the markers here by label
     (MuseScore's "segno", "fine", "coda" for To Coda, "codab" for the
     Coda...), and a jump at the end of the measure: to the measure with
     label jump_to ("start" for the first), playing until the end of the one
     with label until ("end" for all), then on at the one with label cont. */
  char labels[6][16];
  int nlabels;
  int section_end;              /* a section break ends this measure */
  int jump, jump_repeats;       /* jump_repeats: repeats played after it */
  char jump_to[12], until[12], cont[12];
  int has_key;                  /* a key signature starts here: */
  int fifths, minor;            /* as it sounds */
  int kvotes[15];               /* (while reading: how many staves have each, -7..7) */
} gs_measure;

typedef struct
{
  gs_part *parts;
  int nparts;
  gs_measure *meas;
  int nmeas, meascap;
  double tempo;                 /* first tempo, BPM in quarters; 0 if none */
  gs_tempo *tempos;             /* every tempo mark, the first too */
  int ntempos, tempocap;
} gs_score;

void gs_init (gs_score *sc);
void gs_free (gs_score *sc);
gs_part *gs_add_part (gs_score *sc, const char *name);
/* wr: GR_WR with its staff and voice (GR_WR_*), GR_WR_HALF if len is half
   what's written (staccato); 0 if not known. With its spelling
   (gr_note.spell) shifted up 8, where known. */
int gs_add_note (gs_part *p, int measure, double off, double len, int pitch, int vel, int tie, int wr);
int gs_add_pedal (gs_part *p, int measure, double off, int down);
/* A written mark; emeasure -1 for one that doesn't span. */
int gs_add_mark (gs_part *p, int measure, double off, int emeasure, double eoff,
                 int kind, int value, int pitch, int staff);
int gs_add_tempo (gs_score *sc, int measure, double off, double bpm);
/* A syllable (text in UTF-8): syllabic "single", "begin", "middle" or "end". */
int gs_add_lyric (gs_part *p, int measure, double off, int staff, int verse, const char *syllabic, const char *text);
gs_measure *gs_measure_at (gs_score *sc, int i);
/* Each part on three or more staves as parts on one or two, joined (a
   piano's extra staff over its two; an organ's manuals, then its pedals).
   0, or -1 out of memory. */
int gs_split_staves (gs_score *sc);
void gs_measure_label (gs_measure *m, const char *label);   /* a marker */     /* grows the list; NULL if out of memory */

/* Into the song from tick `from` (a bar). Returns the tracks made, or -1;
   *why says what went wrong, or what was left out. */
int gs_place (gr_song *s, gs_score *sc, int from, const char *kind, const char **why);

/* General MIDI name helpers live in gm.c; this is the nearest Groove sound. */
int gs_nearest_patch (int program);
/* A written note value ("quarter", "16th"...) in quarter notes, with its
   dots; 0 if it isn't one. */
double gs_note_value (const char *type, int dots);
/* The General MIDI program with this name (any case), or -1. */
int gs_gm_by_name (const char *name);

/* MuseScore files: .mscz, .mscx (mscx.c). MusicXML: .musicxml, .xml, .mxl
   (musicxml.c). Each returns 0 or -1 with *why. */
int gs_read_mscx (gs_score *sc, char *xml, size_t n, const char **why);
int gs_read_musicxml (gs_score *sc, char *xml, size_t n, const char **why);

#endif

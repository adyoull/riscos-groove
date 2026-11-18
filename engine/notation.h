/* Groove - the song as written music (notation.c): note values, ties,
   triplets, staves, voices, the key and how each note is spelled.

   It's made from the piano roll's notes the same way for the Score window
   (engrave.c, riscos/score.c) and for MusicXML (musicxml_out.c), so the
   two always agree. */
#ifndef NOTATION_H
#define NOTATION_H
#include "groove.h"

/* Written note values. */
enum { NV_WHOLE, NV_HALF, NV_QUARTER, NV_EIGHTH, NV_16TH, NV_32ND, NV_64TH, NV_128TH };
/* Noteheads (drums use the others). */
enum { NH_NORMAL, NH_X, NH_CIRCLE_X, NH_DIAMOND, NH_TRIANGLE_DOWN, NH_TRIANGLE_UP, NH_SLASHED, NH_SLASH };
/* Clefs: treble, bass, percussion, treble with an 8 under it (guitar,
   written an octave up), alto (viola) and tenor (high cello, bassoon,
   trombone). */
enum { NC_G, NC_F, NC_PERC, NC_G8, NC_C3, NC_C4 };
/* What an event is. */
enum { NE_NOTE, NE_REST, NE_SPACE, NE_BAR_REST };
/* Dynamics, from how hard the notes are played (0: no mark). */
enum { ND_NONE, ND_PPP, ND_PP, ND_P, ND_MP, ND_MF, ND_F, ND_FF, ND_FFF };
/* Articulations (bits): a note played short, or much harder than those around it. */
#define NA_STACCATO 1
#define NA_ACCENT   2
/* Hairpins (bits): a crescendo or diminuendo starts here, or one ends. */
#define NW_CRESC 1
#define NW_DIM   2
#define NW_STOP  4
/* Slurs (bits): a phrase played legato (each note overlapping the next)
   starts or ends here. */
#define NS_START 1
#define NS_STOP  2

typedef struct
{
  unsigned char pitch, vel;
  signed char step;             /* 0..6, C to B (drums: where it's written) */
  signed char alter;            /* -1 flat, 0, 1 sharp */
  signed char octave;           /* C4 is middle C */
  unsigned char head;           /* NH_* */
  unsigned char tie_start, tie_stop;
  unsigned char legato;         /* played legato into the next note */
  short release;                /* when it really stops, from its written end: earlier
                                   (staccato) or later (legato), in ticks */
} gr_nhead;

typedef struct
{
  int start, dur;               /* ticks from the bar's start */
  unsigned char kind;           /* NE_*: a chord, a rest, a gap in a second
                                   voice (not shown), or a whole bar's rest */
  unsigned char value, dots;    /* NV_*, and its dots */
  unsigned char tuplet;         /* 1: in a tuplet; |2 starts a group, |4 ends one */
  unsigned char tup_n, tup_m;   /* the tuplet's tup_n in the time of tup_m (3:2, 5:4,
                                   6:4, 2:3) */
  unsigned short tup_len;       /* its group's length in ticks (a beat, or two) */
  int head, nheads;             /* its notes, lowest first, in heads[] */
  unsigned char dyn;            /* ND_*: a dynamic mark here */
  unsigned char artic;          /* NA_* */
  unsigned char slur;           /* NS_* */
  unsigned char wedge;          /* NW_*: hairpins */
} gr_nevent;

/* Repeats (gr_nbar.rep): a start repeat at the bar's start, an end
   repeat at its end. */
#define NR_START 1
#define NR_END   2

typedef struct
{
  int start, len, num, den;     /* ticks; the time signature */
  int beat;                     /* ticks in a beat */
  int compound;                 /* 6/8, 9/8, 12/8...: beats are dotted */
  int fifths, minor;            /* the key here (as it sounds) */
  int src;                      /* the song's bar this is (the first time, if repeated) */
  unsigned char rep;            /* NR_* */
  unsigned char times;          /* an end repeat: played this many times in all */
  unsigned char ending;         /* in a first or second ending (1, 2), or 0 */
  unsigned char ending_start, ending_stop;   /* its bracket starts here; ends here
                                               (1: with a hook down, 2: open) */
} gr_nbar;

/* Each part has up to two staves of two voices: voice 0 has every bar,
   with rests; voice 1 (held notes, or a second rhythm) only where it
   plays, its gaps as NE_SPACE. */
#define GR_NVOICES 4            /* [staff * 2 + voice] */

typedef struct
{
  int first, n;                 /* events */
} gr_nrange;

/* The sustain pedal going down or up. */
typedef struct
{
  int bar, start;               /* ticks from the bar's start */
  int down;
} gr_npedal;

typedef struct
{
  int track;
  char name[GR_NAME];           /* the track's */
  int drums;
  int staves;                   /* 1 or 2 */
  int clef[2];                  /* NC_*, at the start */
  int fifths;                   /* its key signature at the start (written: a Bb clarinet's is two
                                   sharps more) */
  signed char *key;             /* [bar]: its key signature in each bar */
  unsigned char *clefs;         /* [bar * 2 + staff]: the clef in each bar */
  signed char *ottava;          /* [bar * 2 + staff]: 1 under an 8va line, -1 an 8vb, 2 a 15ma,
                                   -2 a 15mb, or 0 */
  int *ott_start, *ott_end;     /* [bar * 2 + staff]: where in the bar the line starts (its first
                                   bar) and ends (its last; 0: at the bar's end), in ticks */
  int transpose;                /* semitones it's written above how it sounds (Bb clarinet 2,
                                   guitar 12, piccolo -12) */
  gr_nrange *voice;             /* [bar * GR_NVOICES + staff * 2 + voice] */
  gr_npedal *pedal;             /* in time order: down, up, down... */
  int npedal;
} gr_npart;

typedef struct
{
  int nbars;
  gr_nbar *bars;
  int nparts;
  gr_npart *parts;
  gr_nevent *ev;
  int nev, evcap;
  gr_nhead *hd;
  int nhd, hdcap;
  int fifths, minor;            /* the key at the start: sharps (+) or flats (-), and mode */
  int nsong;                    /* the song's bars, */
  int *bar_of;                  /* [song bar]: and the bar each is written in (repeats
                                   write some bars once for two or more) */
} gr_notation;

/* The song written out: every drum and instrument track with notes, or
   (track >= 0) just that one, even with none. bars: at least this many.
   NULL if there's nothing to write or no memory. */
gr_notation *gr_notation_make (const gr_song *s, int track, int bars);
/* The same, with flags: GR_NT_CONCERT writes every part as it sounds
   (but guitars, basses, piccolo and glockenspiel still an octave or two
   off, as they always are). */
#define GR_NT_CONCERT 1
/* GR_NT_REPEATS: music that's played again straight after (every part
   the same, to the note) is written once inside repeat marks, with
   first and second endings where only its end differs. */
#define GR_NT_REPEATS 2
gr_notation *gr_notation_make_ex (const gr_song *s, int track, int bars, int flags);
void gr_notation_free (gr_notation *n);

/* A written value's name in MusicXML ("quarter") and its length in ticks
   (with no dots, not in a triplet). */
const char *gr_nvalue_name (int value);
/* A dynamic's name ("mf"), and the velocity it stands for. */
const char *gr_ndyn_name (int dyn);
int gr_nvalue_ticks (int value);

#endif

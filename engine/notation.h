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
#define NA_ARPEGGIO 4             /* a spread chord (from a score's mark) */
#define NA_ARP_DOWN 8             /* spread from the top down (an arrow down) */
#define NA_ARP_UP   16            /* an arrow up (spread from the bottom, as usual) */
#define NA_TENUTO   32            /* (these from a score's marks) */
#define NA_MARCATO  64
#define NA_FERMATA  128           /* a pause over it (or a rest) */
#define NA_FERM_BELOW 256         /* ... under it */
#define NA_GLISS    512           /* a glissando from it to the next note */
#define NA_CUE      1024          /* a cue note: the score shows it (small), nothing plays it */
#define NA_ARP_SPAN 2048          /* its spread's line goes on down through the chord on the staff below */
#define NA_ARP_SPANNED 4096       /* ...that one (no line of its own) */
/* Ornaments (gr_nevent.orn). */
enum { NO_NONE, NO_TRILL, NO_MORDENT, NO_TURN };
/* Hairpins (bits): a crescendo or diminuendo starts here, or one ends. */
#define NW_CRESC 1
#define NW_DIM   2
#define NW_STOP  4
#define NW_ABOVE 8                /* the hairpin starting here goes over the staff */
#define NW_DYN_ABOVE 16           /* ...the dynamic here */
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
  unsigned char tuplet;         /* 1: in a tuplet; |2 starts a group, |4 ends one; |8 its number
                                   not shown, |16 no bracket (as the score says) */
  unsigned char tup_n, tup_m;   /* the tuplet's tup_n in the time of tup_m (3:2, 5:4,
                                   6:4, 2:3) */
  unsigned short tup_len;       /* its group's length in ticks (a beat, or two) */
  unsigned char breve;           /* a whole note's value doubled: a breve (value NV_WHOLE) */
  unsigned short tup_at;        /* a score's tuplet: where in it this starts, in written ticks
                                   (0xFFFF: not known) */
  int head, nheads;             /* its notes, lowest first, in heads[] */
  unsigned char dyn;            /* ND_*: a dynamic mark here */
  unsigned short artic;         /* NA_* */
  unsigned char orn;            /* NO_* */
  unsigned char trem;           /* a tremolo's strokes (1..4); | 0x10 to the next chord, | 0x20 from
                                   the one before (each written twice as long as it plays) */
  unsigned char ngrace;         /* grace notes before it (in gr_notation.gr, ev this one) */
  unsigned char slur;           /* NS_* */
  unsigned char wedge;          /* NW_*: hairpins */
  unsigned char beam;           /* GR_BEAM_*: how a score beams it (0: as Groove does) */
  unsigned char stem;           /* a score's stem: 1 up, 2 down (0: as Groove has it) */
  unsigned char arp_ext;        /* a score's spread drawn longer down: half spaces */
  unsigned char arp_rows;       /* ...or through the staves below: how many (NA_ARP_SPAN) */
  unsigned char moved;          /* a score's chord moved here from the other staff (in a voice of
                                   this one's) */
  signed char xstaff;           /* a score's chord moved to the staff below (1) or above (-1)
                                   its voice's: written there, beamed with its voice */
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
  int tempo;                    /* the tempo changes here (in the bar): crotchets a minute; 0 */
  int pickup;                   /* the first bar, short: a score's pickup (numbered 0) */
  int timesym;                  /* its time signature written as C (1) or cut C (2) */
} gr_nbar;

/* Each part has up to two staves of up to four voices (as a score has
   them): voice 0 has every bar, with rests; voices 1 to 3 (held notes, a
   second rhythm, a score's other voices) only where they play, their
   gaps as NE_SPACE. Voices 0 and 2 have their stems up, 1 and 3 down. */
#define GR_VPS 4                /* voices a staff */
#define GR_NVOICES (2 * GR_VPS) /* [staff * GR_VPS + voice] */

typedef struct
{
  int first, n;                 /* events */
} gr_nrange;

/* The sustain pedal going down or up. */
typedef struct
{
  int bar, start;               /* ticks from the bar's start */
  int down;
  int hidden;                   /* the score doesn't show it ("simile") */
  int line;                     /* a line on from it (a down) or up to it (an up) */
  int above;                    /* over its staff (as the score has it) */
} gr_npedal;

typedef struct
{
  int track;
  char name[GR_NAME];           /* the track's */
  int drums;
  int lines;                    /* 1: its staff has one line (a score's drum); else 5 */
  int staves;                   /* 1 or 2 */
  int joined;                   /* GR_JOIN_*: its staves on from the part before's (one
                                   instrument on three or more) */
  int clef[2];                  /* NC_*, at the start */
  int fifths;                   /* its key signature at the start (written: a Bb clarinet's is two
                                   sharps more) */
  signed char *key;             /* [bar]: its key signature in each bar */
  unsigned char *clefs;         /* [bar * 2 + staff]: the clef in each bar */
  unsigned char *mclef;         /* [bar * 2 + staff]: a clef changed to inside the bar (NC_* + 1),
                                   or 0 (NULL: none anywhere) */
  int *mclef_at;                /* ... where, in ticks from the bar's start */
  signed char *ottava;          /* [bar * 2 + staff]: 1 under an 8va line, -1 an 8vb, 2 a 15ma,
                                   -2 a 15mb, or 0 */
  int *ott_start, *ott_end;     /* [bar * 2 + staff]: where in the bar the line starts (its first
                                   bar) and ends (its last; 0: at the bar's end), in ticks */
  int transpose;                /* semitones it's written above how it sounds (Bb clarinet 2,
                                   guitar 12, piccolo -12) */
  gr_nrange *voice;             /* [bar * GR_NVOICES + staff * GR_VPS + voice] */
  gr_npedal *pedal;             /* in time order: down, up, down... */
  int npedal;
} gr_npart;

/* A syllable of the words under a chord (nt->ly, in order of ev). */
typedef struct
{
  int ev;
  unsigned char verse, syllabic;   /* gr_lyric's */
  char text[GR_LYRIC];
} gr_nlyric;

/* A grace note, written small before a chord. */
typedef struct
{
  int ev;                       /* the chord it's before */
  unsigned char pitch, value, slash, place;   /* NV_*; slashed (an acciaccatura); its place, 0 first */
  signed char step, alter, octave;
} gr_ngrace;

typedef struct
{
  int nbars;
  gr_nbar *bars;
  gr_ngrace *gr;                /* grace notes */
  int ngr, grcap;
  gr_nlyric *ly;                /* the words */
  int nly;
  unsigned short *score_bar;    /* [bar]: the song's srcbar (a score's measure + 1, or 0), while
                                   its repeats are found; NULL after */
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
  int tempo_hidden;             /* the song's opening tempo has no metronome mark written */
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
/* The first of event ev's syllables in n->ly, or -1. */
int gr_notation_lyric (const gr_notation *n, int ev);

/* A written value's name in MusicXML ("quarter") and its length in ticks
   (with no dots, not in a triplet). */
const char *gr_nvalue_name (int value);
/* A dynamic's name ("mf"), and the velocity it stands for. */
const char *gr_ndyn_name (int dyn);
int gr_nvalue_ticks (int value);

#endif

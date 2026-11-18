/* Groove - a written score on its way into the song (score.c), shared by
   the MuseScore (mscx.c) and MusicXML (musicxml.c) importers.

   The readers fill in measures and each part's notes, positioned within
   their measure in quarter notes; gs_place plays the repeats out, joins
   tied notes and puts the parts on new tracks, as MIDI import does. */
#ifndef SCORE_H
#define SCORE_H
#include "groove.h"

enum { GS_TIE_FWD = 1, GS_TIE_BACK = 2 };

typedef struct
{
  int measure;
  double off, len;              /* quarter notes, from the measure's start */
  int pitch, vel, tie;
} gs_note;

typedef struct
{
  int measure;
  double off;                   /* quarter notes, from the measure's start */
  int down;                     /* the sustain pedal goes down (1) or up (0) */
} gs_pedal;

typedef struct
{
  char name[GR_NAME];
  int program, bank_msb, bank_lsb, channel;   /* -1: not given */
  int drums;
  gs_note *notes;
  int n, cap;
  gs_pedal *pedals;
  int npedals, pedcap;
} gs_part;

typedef struct
{
  double len;                   /* quarter notes */
  int num, den;                 /* the time signature in force */
  int start_repeat;             /* a repeat starts here */
  int end_repeat;               /* times played, if a repeat ends here (0 none) */
  unsigned endings;             /* the volta it's in: bit n = ending n+1; 0 none */
  int fine;                     /* (unused for now) */
} gs_measure;

typedef struct
{
  gs_part *parts;
  int nparts;
  gs_measure *meas;
  int nmeas, meascap;
  double tempo;                 /* first tempo, BPM in quarters; 0 if none */
} gs_score;

void gs_init (gs_score *sc);
void gs_free (gs_score *sc);
gs_part *gs_add_part (gs_score *sc, const char *name);
int gs_add_note (gs_part *p, int measure, double off, double len, int pitch, int vel, int tie);
int gs_add_pedal (gs_part *p, int measure, double off, int down);
gs_measure *gs_measure_at (gs_score *sc, int i);     /* grows the list; NULL if out of memory */

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

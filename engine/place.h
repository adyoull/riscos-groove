/* Groove - putting imported notes into regions (place.c), shared by the
   MIDI and Maestro importers. */
#ifndef PLACE_H
#define PLACE_H
#include "groove.h"

/* A note in song ticks from the import's start. key is the MIDI note, or
   on a drum track the drum row. */
typedef struct { int start, end, key, vel, wr, tie1; } gr_pnote;   /* wr: gr_note.wr | gr_note.spell << 8; tie1: gr_note's */
/* (key -1: no note, only a place a region must cover, for a mark there) */

/* Put n notes on track t, from tick 'from' (a bar start): a region per
   four bars of the song's own bars that have notes (fewer where it's
   busy; see gr_place_chunk). A region never ends while one of its notes is still
   sounding, so no note is cut short. Returns 0, or -1 when the song is
   full (*full_at = the tick it got to). Notes that don't fit in a pattern
   are counted in *dropped. */
int gr_place_notes (gr_song *s, int t, const gr_pnote *m, int n, int from, int *full_at, int *dropped);
/* Regions of this many bars rather than four (at least four), for a long
   import that would fill the song otherwise; busy bars as many as fit. */
void gr_place_chunk (int bars);

/* A controller event (gr_ctl's type and value) in song ticks from the
   import's start. */
typedef struct { int tick, type, value; } gr_pctl;

/* Put n controller events (sorted by tick) on track t after its notes:
   each into the region playing at its time, or, where no region is (a
   bend over a rest), a region made for its bars. Returns 0, or -1 when the
   song is full; events that don't fit are counted in *dropped. */
int gr_place_ctls (gr_song *s, int t, const gr_pctl *c, int n, int from, int *full_at, int *dropped);

/* A written mark (gr_mark) in song ticks from the import's start. */
typedef struct { int tick, len, kind, value, pitch, staff; } gr_pmark;

/* Put n marks on track t, each into the region playing at its time (one
   over a rest, where no region is, is left out). Marks that don't fit in
   a pattern are counted in *dropped. */
void gr_place_marks (gr_song *s, int t, const gr_pmark *m, int n, int from, int *dropped);

#endif

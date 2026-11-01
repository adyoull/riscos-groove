/* Groove - putting imported notes into regions (place.c), shared by the
   MIDI and Maestro importers. */
#ifndef PLACE_H
#define PLACE_H
#include "groove.h"

/* A note in song ticks from the import's start. key is the MIDI note, or
   on a drum track the drum row. */
typedef struct { int start, end, key, vel; } gr_pnote;

/* Put n notes on track t, from tick 'from' (a bar start): a region per
   four bars of the song's own bars that have notes (one-bar regions where
   it's busy). A region never ends while one of its notes is still
   sounding, so no note is cut short. Returns 0, or -1 when the song is
   full (*full_at = the tick it got to). Notes that don't fit in a pattern
   are counted in *dropped. */
int gr_place_notes (gr_song *s, int t, const gr_pnote *m, int n, int from, int *full_at, int *dropped);

#endif

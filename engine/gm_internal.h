/* Groove - between gm.c and engine.c. */
#ifndef GM_INTERNAL_H
#define GM_INTERNAL_H

/* Notes a SoundFont can sound at once (the second player has half as
   many): as in 0.4.1. A big score uses them: Money, Money, Money (26
   parts) would sound up to 102 at once on the first. Each costs processor
   time only while it sounds (a Pi playing Money: 4% of the time); when
   they run out TinySoundFont takes the one furthest into dying away. */
#define GM_VOICES 96

/* MIDI channels Groove uses: 0-15 on the first synth, 16-31 on a second
   one, which is only loaded for a 16th instrument track (libmidisynth's
   channels stop at 15, and one synth has only 15 that aren't for drums). */
#define GM_CHANNELS 32

struct midisynth;
struct midisynth *gm_synth (void);   /* NULL until a SoundFont is loaded */
struct midisynth *gm_synth_n (int n);   /* 0, or 1 (the second, if loaded) */
unsigned gm_generation (void);       /* changes when it is loaded again */
/* Silence it and make every engine tell it its channels again (a bounce
   uses it between the live engine's notes). */
void gm_restart (void);

#endif

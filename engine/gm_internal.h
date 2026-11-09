/* Groove - between gm.c and engine.c. */
#ifndef GM_INTERNAL_H
#define GM_INTERNAL_H

/* Notes a SoundFont can sound at once. Each costs processor time on the
   Pi; TinySoundFont steals the oldest when they run out. */
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

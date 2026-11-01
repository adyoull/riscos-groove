/*
 * midisynth - General MIDI software synthesiser for RISC OS programs
 *
 * Plays Standard MIDI Files, or MIDI events sent live, through a
 * SoundFont (.sf2 or .sf3) using TinySoundFont. It can render into a
 * buffer you supply (to mix with your own sound, e.g. from an SDL audio
 * callback), or play on its own on RISC OS 5 through SharedSoundBuffer or,
 * if that isn't available, DigitalRenderer.
 *
 * Copyright (c) 2026 Andrew Youll. MIT licence, see LICENSE.
 */
#ifndef MIDISYNTH_H
#define MIDISYNTH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The library version. Programs that link midisynth statically can check
   it at compile time, e.g. #if MIDISYNTH_VERSION_NUM >= 400 */
#define MIDISYNTH_VERSION_MAJOR 0
#define MIDISYNTH_VERSION_MINOR 5
#define MIDISYNTH_VERSION_PATCH 0
#define MIDISYNTH_VERSION       "0.5.0"
#define MIDISYNTH_VERSION_NUM   (MIDISYNTH_VERSION_MAJOR * 10000 + \
                                 MIDISYNTH_VERSION_MINOR * 100 + MIDISYNTH_VERSION_PATCH)

/* Conventions:
   - Functions returning int return 1 on success, 0 on failure. Then
     midisynth_last_error(ms) says why.
   - Threads: every call on a synth may be made from any thread; each synth
     has its own lock. The midisynth_output_* calls for one synth should
     all be made from the same thread. */

typedef struct midisynth midisynth;

/* ---- Creating a synth ---------------------------------------------------- */

/* Settings for midisynth_create_ex. Always fill one in with
   midisynth_config_init first, then change what you need: fields added in
   later versions then get their defaults. */
typedef struct midisynth_config
{
    const char *soundfont;   /* .sf2/.sf3 file; NULL (default) means the one
                                named by SoftMIDI$SoundFont on RISC OS (or
                                MIDISynth$SoundFont, its name before 0.5.0), or
                                the MIDISYNTH_SOUNDFONT environment variable
                                elsewhere */
    int sample_rate;         /* output rate in Hz, 8000-96000 (default 44100) */
    int max_voices;          /* notes that can sound at once, 1-256 (default 96) */
} midisynth_config;

void midisynth_config_init(midisynth_config *config);

/* Create a synthesiser. Output is 16-bit stereo. Returns NULL on failure;
   midisynth_last_error(NULL) says why. */
midisynth *midisynth_create_ex(const midisynth_config *config);

/* The same with the default settings, apart from the SoundFont (NULL for
   the default) and the sample rate. */
midisynth *midisynth_create(const char *soundfont, int sample_rate);

void midisynth_destroy(midisynth *ms);   /* also closes the output */

/* ---- Errors ---------------------------------------------------------------- */

/* Why the last call on this synth failed ("" if none has). With NULL, why
   the last midisynth_create failed. The text stays until the next failure. */
const char *midisynth_last_error(midisynth *ms);

/* The last failure on any synth. Kept for older programs: it is shared by
   all synths and threads, so prefer midisynth_last_error. */
const char *midisynth_error(void);

/* ---- Songs ------------------------------------------------------------------ */

/* Load a Standard MIDI File (from a file or memory). Any song already
   playing stops. The new song starts paused; call midisynth_play.
   Returns 0 if it isn't a MIDI file. The data given to
   midisynth_load_memory is copied, so it can be freed straight away. */
int midisynth_load_file(midisynth *ms, const char *path);
int midisynth_load_memory(midisynth *ms, const void *data, int size);

void midisynth_play(midisynth *ms);          /* start or resume */
void midisynth_pause(midisynth *ms);
void midisynth_stop(midisynth *ms);          /* stop and rewind; sounding notes
                                                fade out over 10 ms */
int  midisynth_playing(midisynth *ms);       /* 1 while a song is playing */
void midisynth_set_loop(midisynth *ms, int loop);
/* Volume 0.0 - 1.0. At 0 the synth is muted and costs almost nothing:
   sounding notes stop, new notes aren't started, but the song keeps its
   place and follows instrument and controller changes. */
void midisynth_set_volume(midisynth *ms, float volume);

/* ---- Live MIDI (channels 0-15; channel 9 is the drums) ---------------------- */

void midisynth_note_on(midisynth *ms, int channel, int key, int velocity);
void midisynth_note_off(midisynth *ms, int channel, int key);
void midisynth_program(midisynth *ms, int channel, int program);
void midisynth_control(midisynth *ms, int channel, int control, int value);
void midisynth_pitch_bend(midisynth *ms, int channel, int value); /* 0-16383 */
void midisynth_all_notes_off(midisynth *ms);
/* A raw MIDI message: status byte plus up to two data bytes */
void midisynth_send(midisynth *ms, int status, int data1, int data2);

/* ---- Getting the sound out --------------------------------------------------- */

/* Either render into your own buffer... Renders 'frames' stereo frames of
   16-bit samples. With mix != 0 the music is added to what's already
   there (with clipping), otherwise the buffer is overwritten. */
void midisynth_render(midisynth *ms, int16_t *buffer, int frames, int mix);

/* ...or let the synth play on its own, for programs without sound output
   of their own. RISC OS only: elsewhere open returns 0 ("not supported").
   - Uses SharedSoundBuffer, which mixes with other programs' sound. If
     that isn't available, uses DigitalRenderer directly, which only one
     program can use at a time (midisynth won't take it from another).
     Setting SoftMIDI$Output to SharedSoundBuffer or DigitalRenderer
     allows only that one.
   - DigitalRenderer may play at a different rate from the synth's; the
     synth then changes its rate to match.
   - After open, call midisynth_output_poll often (for example on every
     Wimp null event, at least every 50 ms): it renders more sound
     whenever the queue runs low.
   - 'name' is shown as the sound source's name (SharedSoundBuffer). */
int  midisynth_output_open(midisynth *ms, const char *name);
void midisynth_output_poll(midisynth *ms);
void midisynth_output_close(midisynth *ms);
/* "SharedSoundBuffer" or "DigitalRenderer" while open, otherwise NULL */
const char *midisynth_output_name(midisynth *ms);

#ifdef __cplusplus
}
#endif

#endif /* MIDISYNTH_H */

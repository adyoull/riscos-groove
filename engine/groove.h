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

#define GR_MAX_TRACKS    16
#define GR_MAX_REGIONS   256           /* room for an imported MIDI file */
#define GR_MAX_PATTERNS  256
#define GR_MAX_NOTES     384           /* per pattern */
#define GR_MAX_CLIPS     16
#define GR_MAX_METERS    32            /* time signature changes */
#define GR_NAME          24

#define GR_DRUM_SOUNDS   8           /* rows in the step grid */

typedef enum { TRK_DRUMS = 0, TRK_INST = 1, TRK_AUDIO = 2 } gr_track_type;

enum { DR_KICK, DR_SNARE, DR_CHAT, DR_OHAT, DR_CLAP, DR_LTOM, DR_HTOM, DR_PERC };

enum { PATCH_EPIANO, PATCH_BASS, PATCH_PAD, PATCH_LEAD, PATCH_PLUCK,
       PATCH_ORGAN, PATCH_BELL, PATCH_COUNT };

enum { KIT_STUDIO, KIT_ELECTRO, KIT_COUNT };

typedef struct
{
  int16_t tick;                 /* from the start of the pattern */
  int16_t len;                  /* ticks (ignored for drums) */
  uint8_t pitch;                /* MIDI note, or DR_* for drums */
  uint8_t vel;                  /* 1..127 */
} gr_note;

typedef struct
{
  char name[GR_NAME];
  int used;
  int len;                      /* ticks; the region loops it */
  int nnotes;
  gr_note notes[GR_MAX_NOTES];  /* kept sorted by tick */
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
  float meter;                  /* peak level for the UI, decays */
} gr_track;

/* A time signature from a bar (0-based) on: num beats of a 1/den note.
   den is 1, 2, 4, 8, 16, 32 or 64. */
typedef struct
{
  int bar, num, den;
} gr_meter;

typedef struct
{
  int tempo;                    /* BPM (crotchets a minute) */
  int key;                      /* 0 = C .. 11 = B; loops are transposed into it */
  int bars;                     /* song length */
  int loop_on, loop_start, loop_end;   /* ticks */
  int metronome;
  float master;
  int nmeters;                  /* meters[0].bar is always 0 */
  gr_meter meters[GR_MAX_METERS];
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
const char *gr_gm_program_name (int program);    /* "Acoustic Grand Piano" ... */
const char *gr_gm_family_name (int family);      /* 0..15: "Piano" ... */
int  gr_gm_kit (int i);                          /* i-th kit's program, i < GM_NKITS */
const char *gr_gm_kit_name (int program);
int  gr_gm_drum_note (int sound);                /* DR_* -> GM drum key */
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

/* ---- names for the UI --------------------------------------------------- */
const char *gr_patch_name (int patch);
const char *gr_kit_name (int kit);
const char *gr_drum_name (int sound);
const char *gr_note_name (int pitch, char buf[8]);
const char *gr_key_name (int key);

#endif

/*
 * midisynth - General MIDI software synthesiser for RISC OS programs
 * Copyright (c) 2026 Andrew Youll. MIT licence, see LICENSE.
 *
 * TinySoundFont (tsf.h, tml.h) by Bernhard Schelling, MIT licence.
 * stb_vorbis by Sean Barrett, public domain / MIT licence.
 *
 * The synth itself. Sound output (midisynth_output_*) is in output*.c;
 * see midisynth_internal.h for how the files fit together.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* stb_vorbis decodes the Ogg Vorbis samples in SF3 SoundFonts.
   TinySoundFont uses it when it has been included first. Only decoding
   from memory is needed. */
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_INTEGER_CONVERSION
#define STB_VORBIS_MAX_CHANNELS 2
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"   /* known false alarms */
#include "../third_party/stb/stb_vorbis.c"
#pragma GCC diagnostic pop

#define TSF_IMPLEMENTATION
#include "../third_party/TinySoundFont/tsf.h"
#define TML_IMPLEMENTATION
#include "../third_party/TinySoundFont/tml.h"

#include "midisynth_internal.h"

#ifdef __riscos__
#include <unixlib/local.h>
#endif

#define MS_CHANNELS    16
#define MS_DEFAULT_RATE    44100
#define MS_DEFAULT_VOICES  96      /* allocated up front: no malloc while rendering */
#define MS_MAX_VOICES      256
#define MS_MIN_RATE        8000
#define MS_MAX_RATE        96000
#define MS_BASE_GAIN   0.3f        /* General MIDI SoundFonts are loud */
/* A released note is stopped once its envelope is below -60 dB, rather
   than TinySoundFont's -80 dB. Over the OpenMSX songs this saves about a
   quarter of the work; the difference in the output is 70 dB below the
   music. Only the envelope is used: it never rises again after release,
   whereas the channel volume can (songs dip CC7 to 0 and back). */
#define MS_CULL_LEVEL  0.001f

/* The last failure on any synth, and the only record of why
   midisynth_create failed (see midisynth_error). */
static char ms_errbuf[MS_ERRLEN];

/* File names may be given the RISC OS way (from a system variable, say:
   "SDFS::Disc.$.Sounds.GM/sf2" or "<SoftMIDI$Dir>.GM/sf2"); UnixLib's
   fopen expects Unix-style names, so convert those first. */
static const char *
ms_path(const char *name, char *buf, size_t len)
{
#ifdef __riscos__
    if (name[0] != '/' && (strchr(name, ':') != NULL || strchr("<$@&%", name[0]) != NULL)) {
        if (__unixify_std(name, buf, len, __RISCOSIFY_FILETYPE_NOTSPECIFIED) != NULL)
            return buf;
    }
#else
    (void)buf; (void)len;
#endif
    return name;
}

const char *
midisynth_error(void)
{
    return ms_errbuf;
}

const char *
midisynth_last_error(midisynth *ms)
{
    return ms ? ms->errbuf : ms_errbuf;
}

void
ms_error(midisynth *ms, const char *msg, const char *extra)
{
    snprintf(ms_errbuf, sizeof(ms_errbuf), "%s%s%s", msg, extra ? ": " : "", extra ? extra : "");
    if (ms)
        memcpy(ms->errbuf, ms_errbuf, sizeof(ms->errbuf));
}

/* ---- TinySoundFont internals -------------------------------------------
   These two functions use TinySoundFont's private voice data (tsf->voices,
   voiceNum, playingPreset, ampenv) and its static tsf_voice_kill(), which
   we can reach only because the implementation is compiled into this
   file. They are the only places that do. Check them whenever
   third_party/TinySoundFont is updated: if the names change they stop
   compiling; if the meaning changes, the tests in tests/ should catch it. */

/* Stop every sounding voice at once, without resetting the channels'
   instruments and controllers (tsf_reset would). Lock held. */
static void
ms_kill_voices(midisynth *ms)
{
    struct tsf_voice *v = ms->sf->voices, *end = v + ms->sf->voiceNum;
    for (; v != end; v++)
        tsf_voice_kill(v);
}

/* Stop released notes that have faded below MS_CULL_LEVEL. Lock held. */
static void
ms_cull(midisynth *ms)
{
    struct tsf_voice *v = ms->sf->voices, *end = v + ms->sf->voiceNum;
    for (; v != end; v++)
        if (v->playingPreset != -1 && v->ampenv.segment == TSF_SEGMENT_RELEASE &&
            v->ampenv.level < MS_CULL_LEVEL)
            tsf_voice_kill(v);
}
/* ---- end of TinySoundFont internals ------------------------------------ */

/* Channel set-up: every channel exists (so rendering never allocates), 9 is
   the General MIDI drum kit. */
static void
ms_init_channels(midisynth *ms)
{
    int ch;
    for (ch = 0; ch < MS_CHANNELS; ch++) {
        if (ch == 9)
            tsf_channel_set_bank_preset(ms->sf, 9, 128, 0);
        else
            tsf_channel_set_presetnumber(ms->sf, ch, 0, 0);
    }
}

/* The SoundFont to use when none is given. !SoftMIDI sets SoftMIDI$SoundFont;
   MIDISynth$SoundFont is what it was called before 0.5.0 (the app was
   !MIDISynth), so that is still read if the new one isn't set. */
#ifdef __riscos__
#define MS_SOUNDFONT_VAR     "SoftMIDI$SoundFont"
#define MS_SOUNDFONT_VAR_OLD "MIDISynth$SoundFont"
#else
#define MS_SOUNDFONT_VAR     "MIDISYNTH_SOUNDFONT"
#define MS_SOUNDFONT_VAR_OLD NULL
#endif

const char *
ms_getenv(const char *name, const char *old_name)
{
    const char *v = getenv(name);
    if ((v == NULL || *v == 0) && old_name != NULL)
        v = getenv(old_name);
    return v;
}

void
midisynth_config_init(midisynth_config *config)
{
    memset(config, 0, sizeof(*config));
    config->soundfont = NULL;
    config->sample_rate = MS_DEFAULT_RATE;
    config->max_voices = MS_DEFAULT_VOICES;
}

midisynth *
midisynth_create_ex(const midisynth_config *config)
{
    midisynth *ms;
    const char *soundfont = config->soundfont;
    char pbuf[1024];

    if (soundfont == NULL || *soundfont == 0)
        soundfont = ms_getenv(MS_SOUNDFONT_VAR, MS_SOUNDFONT_VAR_OLD);
    if (soundfont == NULL || *soundfont == 0) {
        ms_error(NULL, "No SoundFont given (and " MS_SOUNDFONT_VAR " isn't set)", NULL);
        return NULL;
    }
    if (config->sample_rate < MS_MIN_RATE || config->sample_rate > MS_MAX_RATE) {
        ms_error(NULL, "Unsupported sample rate", NULL);
        return NULL;
    }
    if (config->max_voices < 1 || config->max_voices > MS_MAX_VOICES) {
        ms_error(NULL, "Unsupported number of voices", NULL);
        return NULL;
    }

    ms = (midisynth *) calloc(1, sizeof(*ms));
    if (ms == NULL) {
        ms_error(NULL, "Out of memory", NULL);
        return NULL;
    }
    ms->sf = tsf_load_filename(ms_path(soundfont, pbuf, sizeof(pbuf)));
    if (ms->sf == NULL) {
        ms_error(NULL, "Couldn't load the SoundFont", soundfont);
        free(ms);
        return NULL;
    }
    ms->rate = config->sample_rate;
    ms->volume = 1.0f;
    tsf_set_output(ms->sf, TSF_STEREO_INTERLEAVED, ms->rate, 0.0f);
    tsf_set_volume(ms->sf, MS_BASE_GAIN);
    tsf_set_max_voices(ms->sf, config->max_voices);
    ms_init_channels(ms);
    pthread_mutex_init(&ms->lock, NULL);
    ms_errbuf[0] = 0;
    return ms;
}

midisynth *
midisynth_create(const char *soundfont, int sample_rate)
{
    midisynth_config config;
    midisynth_config_init(&config);
    config.soundfont = soundfont;
    config.sample_rate = sample_rate;
    return midisynth_create_ex(&config);
}

static void
ms_unload(midisynth *ms)
{
    ms->playing = 0;
    ms->next = NULL;
    ms->msec = 0;
    if (ms->song) {
        tml_free(ms->song);
        ms->song = NULL;
    }
    tsf_reset(ms->sf);             /* silence and default every channel */
    ms_init_channels(ms);
}

void
midisynth_destroy(midisynth *ms)
{
    if (ms == NULL)
        return;
    midisynth_output_close(ms);
    pthread_mutex_lock(&ms->lock);
    ms_unload(ms);
    tsf_close(ms->sf);
    pthread_mutex_unlock(&ms->lock);
    pthread_mutex_destroy(&ms->lock);
    free(ms);
}

static int
ms_set_song(midisynth *ms, tml_message *song, const char *what)
{
    if (song == NULL) {
        ms_error(ms, "Not a MIDI file", what);
        return 0;
    }
    pthread_mutex_lock(&ms->lock);
    ms_unload(ms);
    ms->song = song;
    ms->next = song;
    pthread_mutex_unlock(&ms->lock);
    return 1;
}

int
midisynth_load_file(midisynth *ms, const char *path)
{
    char pbuf[1024];
    return ms_set_song(ms, tml_load_filename(ms_path(path, pbuf, sizeof(pbuf))), path);
}

int
midisynth_load_memory(midisynth *ms, const void *data, int size)
{
    return ms_set_song(ms, tml_load_memory(data, size), NULL);
}

void
midisynth_play(midisynth *ms)
{
    pthread_mutex_lock(&ms->lock);
    if (ms->song) {
        if (ms->next == NULL) {    /* finished: start again */
            ms->next = ms->song;
            ms->msec = 0;
        }
        ms->playing = 1;
    }
    pthread_mutex_unlock(&ms->lock);
}

void
midisynth_pause(midisynth *ms)
{
    pthread_mutex_lock(&ms->lock);
    ms->playing = 0;
    tsf_note_off_all(ms->sf);
    pthread_mutex_unlock(&ms->lock);
}

void
midisynth_stop(midisynth *ms)
{
    pthread_mutex_lock(&ms->lock);
    ms->playing = 0;
    ms->next = ms->song;
    ms->msec = 0;
    tsf_reset(ms->sf);
    ms_init_channels(ms);
    pthread_mutex_unlock(&ms->lock);
}

int
midisynth_playing(midisynth *ms)
{
    int p;
    pthread_mutex_lock(&ms->lock);
    p = ms->playing;
    pthread_mutex_unlock(&ms->lock);
    return p;
}

void
midisynth_set_loop(midisynth *ms, int loop)
{
    pthread_mutex_lock(&ms->lock);
    ms->loop = loop;
    pthread_mutex_unlock(&ms->lock);
}

void
midisynth_set_volume(midisynth *ms, float volume)
{
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    pthread_mutex_lock(&ms->lock);
    ms->volume = volume;
    tsf_set_volume(ms->sf, MS_BASE_GAIN * volume);
    if (volume == 0.0f)            /* muted: drop every note now */
        ms_kill_voices(ms);
    pthread_mutex_unlock(&ms->lock);
}

/* One MIDI event. Called with the lock held. */
static void
ms_event(midisynth *ms, int type, int channel, int a, int b)
{
    tsf *sf = ms->sf;
    channel &= 15;
    switch (type) {
    case TML_NOTE_ON:
        if (b > 0) {
            /* When muted, notes aren't started at all, so nothing is
               rendered. Controllers etc. are still followed, so the
               music carries on correctly when the volume comes back. */
            if (ms->volume > 0.0f)
                tsf_channel_note_on(sf, channel, a & 127, (b & 127) / 127.0f);
            break;
        }
        /* note on with velocity 0 is a note off */
        /* fall through */
    case TML_NOTE_OFF:
        tsf_channel_note_off(sf, channel, a & 127);
        break;
    case TML_PROGRAM_CHANGE:
        tsf_channel_set_presetnumber(sf, channel, a & 127, channel == 9);
        break;
    case TML_CONTROL_CHANGE:
        tsf_channel_midi_control(sf, channel, a & 127, b & 127);
        break;
    case TML_PITCH_BEND:
        tsf_channel_set_pitchwheel(sf, channel, a & 16383);
        break;
    default:
        break;
    }
}

void
midisynth_render(midisynth *ms, int16_t *buffer, int frames, int mix)
{
    pthread_mutex_lock(&ms->lock);
    while (frames > 0) {
        int block = frames > TSF_RENDER_EFFECTSAMPLEBLOCK ? TSF_RENDER_EFFECTSAMPLEBLOCK : frames;

        if (ms->playing) {
            ms->msec += block * (1000.0 / ms->rate);
            while (ms->next && ms->msec >= ms->next->time) {
                tml_message *m = ms->next;
                if (m->type == TML_PITCH_BEND)
                    ms_event(ms, m->type, m->channel, m->pitch_bend, 0);
                else
                    ms_event(ms, m->type, m->channel, m->key, m->velocity);
                ms->next = m->next;
            }
            if (ms->next == NULL) {
                if (ms->loop && ms->song) {
                    ms->next = ms->song;
                    ms->msec = 0;
                } else {
                    ms->playing = 0;   /* song over; notes still ring out */
                }
            }
        }
        if (ms->volume > 0.0f) {
            tsf_render_short(ms->sf, (short *)buffer, block, mix);
            ms_cull(ms);
        } else if (!mix) {
            memset(buffer, 0, block * 2 * sizeof(int16_t));
        }
        buffer += block * 2;
        frames -= block;
    }
    pthread_mutex_unlock(&ms->lock);
}

/* Live MIDI */
void
midisynth_note_on(midisynth *ms, int channel, int key, int velocity)
{
    pthread_mutex_lock(&ms->lock);
    ms_event(ms, TML_NOTE_ON, channel, key, velocity);
    pthread_mutex_unlock(&ms->lock);
}

void
midisynth_note_off(midisynth *ms, int channel, int key)
{
    pthread_mutex_lock(&ms->lock);
    ms_event(ms, TML_NOTE_OFF, channel, key, 0);
    pthread_mutex_unlock(&ms->lock);
}

void
midisynth_program(midisynth *ms, int channel, int program)
{
    pthread_mutex_lock(&ms->lock);
    ms_event(ms, TML_PROGRAM_CHANGE, channel, program, 0);
    pthread_mutex_unlock(&ms->lock);
}

void
midisynth_control(midisynth *ms, int channel, int control, int value)
{
    pthread_mutex_lock(&ms->lock);
    ms_event(ms, TML_CONTROL_CHANGE, channel, control, value);
    pthread_mutex_unlock(&ms->lock);
}

void
midisynth_pitch_bend(midisynth *ms, int channel, int value)
{
    pthread_mutex_lock(&ms->lock);
    ms_event(ms, TML_PITCH_BEND, channel, value, 0);
    pthread_mutex_unlock(&ms->lock);
}

void
midisynth_all_notes_off(midisynth *ms)
{
    pthread_mutex_lock(&ms->lock);
    tsf_note_off_all(ms->sf);
    pthread_mutex_unlock(&ms->lock);
}

void
midisynth_send(midisynth *ms, int status, int data1, int data2)
{
    int type = status & 0xF0, channel = status & 0x0F;
    if (type == TML_PITCH_BEND)
        data1 = (data1 & 127) | ((data2 & 127) << 7);
    pthread_mutex_lock(&ms->lock);
    ms_event(ms, type, channel, data1, data2);
    pthread_mutex_unlock(&ms->lock);
}

/* ---- Sample rate (for the output backends) ---------------------------- */

int
ms_set_rate(midisynth *ms, int rate)
{
    if (rate < MS_MIN_RATE || rate > MS_MAX_RATE)
        return 0;
    pthread_mutex_lock(&ms->lock);
    ms->rate = rate;
    /* tsf_set_output also sets the gain, so apply the volume again */
    tsf_set_output(ms->sf, TSF_STEREO_INTERLEAVED, rate, 0.0f);
    tsf_set_volume(ms->sf, MS_BASE_GAIN * ms->volume);
    pthread_mutex_unlock(&ms->lock);
    return 1;
}

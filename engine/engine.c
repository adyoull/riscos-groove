/* Groove - the real-time engine: sequencer, voices, mixer, reverb.

   Everything runs in the caller's thread. On RISC OS that is the Wimp task,
   on null events: gr_render_words() fills the RMA ring buffer that the
   SharedSound handler empties at interrupt time (see riscos/sound.c). So the
   UI can call gr_live_on() etc. directly, with no locking.

   Rendering is done in sub-blocks of SUB frames. Sequencer events are
   fired at sub-block starts, so timing jitter is at most SUB/GR_RATE
   (0.7 ms) - far below what anyone can hear. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "groove.h"

#include "gm_internal.h"
#include "midisynth.h"

#define SUB          32
#define NSYNTH       24
#define NDRUM        16
#define KS_MAX       2048       /* Karplus-Strong delay: down to ~22 Hz */
#define LIVE_TRACK   (-2)       /* preview voices aren't on a track */
#define TWO_PI       6.28318530718f
#define GM_PENDING   128        /* SoundFont notes waiting for their note-off */
#define GM_GAIN      4.5f       /* SoundFont level, matched to Groove's sounds */

/* ---------------------------------------------------------------------- */
/* Small DSP helpers                                                      */

static uint32_t rng = 0x12345678u;
static inline float noise (void)
{
  rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
  return (float) (int32_t) rng * (1.0f / 2147483648.0f);
}

static float mtof (float m) { return 440.0f * powf (2.0f, (m - 69.0f) / 12.0f); }

/* Fast sine for a phase in [0,1): parabolic approximation, good enough for
   synth oscillators and far cheaper than sinf on every sample. */
static inline float fsin (float ph)
{
  float x = ph * 2.0f - 1.0f;            /* -1..1 */
  float y = 4.0f * x * (1.0f - fabsf (x));
  y = 0.225f * (y * fabsf (y) - y) + y;
  return -y;
}

static inline float wrap1 (float p) { return p >= 1.0f ? p - (float) (int) p : p; }

/* Polynomial band-limited step: removes most of a naive saw's aliasing for
   two multiplies per sample. */
static inline float polyblep (float t, float dt)
{
  if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
  if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
  return 0.0f;
}

/* Chamberlin state-variable filter, low-pass output. */
typedef struct { float low, band; } svf;
static inline float svf_lp (svf *f, float in, float cut, float q)
{
  f->low += cut * f->band;
  f->band += cut * (in - f->low - q * f->band);
  return f->low;
}
static inline float svf_cut (float hz)
{
  if (hz > GR_RATE / 6.0f) hz = GR_RATE / 6.0f;
  if (hz < 20.0f) hz = 20.0f;
  return 2.0f * sinf (3.14159265f * hz / GR_RATE);
}

/* Constant-power pan: -3 dB each side in the centre, full at the edge. */
static inline void pan_gains (float vol, float pan, float *gl, float *gr)
{
  if (pan > 1.0f) pan = 1.0f;
  if (pan < -1.0f) pan = -1.0f;
  *gl = vol * sqrtf (0.5f * (1.0f - pan));
  *gr = vol * sqrtf (0.5f * (1.0f + pan));
}

/* ---------------------------------------------------------------------- */
/* Envelopes                                                              */

typedef struct { float a, d, s, r; } adsr_def;      /* seconds, seconds, level, seconds */
enum { ENV_OFF, ENV_A, ENV_D, ENV_S, ENV_R };
typedef struct { int stage; float level, ra, rd, rr, s; } adsr;

static void env_start (adsr *e, const adsr_def *d)
{
  e->stage = ENV_A;
  e->ra = 1.0f / (d->a * GR_RATE + 1.0f);
  e->rd = (1.0f - d->s) / (d->d * GR_RATE + 1.0f);
  e->rr = 1.0f / (d->r * GR_RATE + 1.0f);
  e->s = d->s;
}
static void env_release (adsr *e) { if (e->stage != ENV_OFF) e->stage = ENV_R; }
static inline float env_next (adsr *e)
{
  switch (e->stage)
    {
    case ENV_A: e->level += e->ra; if (e->level >= 1.0f) { e->level = 1.0f; e->stage = ENV_D; } break;
    case ENV_D:
      e->level -= e->rd;
      if (e->level <= e->s) { e->level = e->s; e->stage = ENV_S; }
      if (e->s <= 0.0f && e->level <= 0.0f) e->stage = ENV_OFF;
      break;
    case ENV_R: e->level -= e->rr; if (e->level <= 0.0f) { e->level = 0.0f; e->stage = ENV_OFF; } break;
    default: break;
    }
  return e->level;
}

/* ---------------------------------------------------------------------- */
/* Patches                                                                */

typedef struct
{
  adsr_def amp, filt;
  float cutoff, env_amt, q, gain;
} patch_def;

static const patch_def patches[PATCH_COUNT] = {
  /* amp ADSR                  filter ADSR               cut    env    q     gain */
  { {0.002f,1.6f,0.0f,0.30f}, {0.001f,0.6f,0.0f,0.2f}, 1800, 2500, 0.9f, 0.55f },  /* e-piano (FM) */
  { {0.003f,0.25f,0.6f,0.08f},{0.001f,0.22f,0.1f,0.1f},  180, 1600, 0.35f,0.36f },  /* bass */
  { {0.45f,0.8f,0.8f,0.9f},   {0.8f,1.0f,0.6f,1.0f},     500, 1400, 0.8f, 0.22f },  /* pad */
  { {0.005f,0.2f,0.7f,0.15f}, {0.002f,0.3f,0.4f,0.2f},  1200, 2600, 0.5f, 0.28f },  /* lead */
  { {0.001f,3.0f,0.0f,0.25f}, {0,0,0,0},                   0,    0, 0.0f, 0.75f },  /* pluck (KS) */
  { {0.005f,0.05f,1.0f,0.06f},{0,0,0,0},                   0,    0, 0.0f, 0.30f },  /* organ */
  { {0.001f,2.5f,0.0f,1.2f},  {0,0,0,0},                   0,    0, 0.0f, 0.40f },  /* bells (FM) */
};

/* ---------------------------------------------------------------------- */
/* Voices                                                                 */

typedef struct
{
  int active, track, pitch, patch;
  unsigned age;
  float vel, freq, ph[3], modph;
  int off_in;                   /* samples until note-off, or -1 = held */
  adsr amp, filt;
  svf f;
  float *ks;                    /* pluck delay line */
  int ks_len, ks_pos;
  float ks_last;
} synth_voice;

typedef struct
{
  int active, track, sound, kit;
  float vel, t, ph, ph2, decay, lp, hp_prev, hp_out;
  int len;                      /* samples left */
} drum_voice;

typedef struct { float buf[1617]; int len, pos; float store; } comb;
typedef struct { float buf[600]; int len, pos; } allpass;   /* longest is 556 + 23 */

struct gr_engine
{
  gr_song *song;
  int playing, recording;
  double pos;                   /* ticks */
  int last_tick;                /* last integer tick whose events fired */
  double play_from;
  synth_voice sv[NSYNTH];
  drum_voice dv[NDRUM];
  unsigned age;
  /* reverb: 4 combs + 2 allpasses per side */
  comb cl[4], cr[4];
  allpass al[2], ar[2];
  /* metronome click */
  float click_t, click_f;
  int click_len;
  /* preview of a loop from the loop browser */
  const gr_pattern *pv;
  int pv_type, pv_patch, pv_last;
  double pv_pos;
  /* recording */
  int rec_region[GR_MAX_TRACKS];
  int rec_start_tick[GR_MAX_TRACKS][128];   /* tick + 1 of a held note; 0 = none */
  uint8_t rec_vel[GR_MAX_TRACKS][128];
  int rec_clip;                 /* audio clip being recorded, or -1 */
  int busy_tail;                /* samples of tail still to render */
  /* Count-in before recording, and merging recordings into regions. */
  int count_beats, count_left, count_pos, merge;
  int count_beat;               /* ticks in one count-in beat */
  /* SoundFont sounds: what each MIDI channel was last told, notes waiting
     for their note-off, and how long to keep rendering after the last. */
  unsigned gm_gen;
  int gm_prog[GM_CHANNELS], gm_vol[GM_CHANNELS], gm_pan[GM_CHANNELS];
  struct { int ch, key, left; } gm_off[GM_PENDING];
  int gm_noffs, gm_tail;
  float gm_send;                /* the SoundFont's reverb send */
  /* MIDI out: the sender, frames rendered (its clock), what each channel
     was last sent, which channels have been used, and notes waiting for
     their note-off. */
  gr_midi_sender *mo_fn;
  void *mo_ctx;
  unsigned frames;
  int mo_live;                  /* sending for gr_live_on/off: now */
  int mo_prog[16], mo_vol[16], mo_pan[16];
  unsigned mo_used;
  struct { int ch, key, left; } mo_off[GM_PENDING];
  int mo_noffs;
};

/* ---------------------------------------------------------------------- */
/* Reverb (Freeverb-style, mono in, stereo out)                           */

static const int comb_len[4] = { 1116, 1188, 1277, 1356 };
static const int ap_len[2] = { 556, 441 };

static void reverb_init (gr_engine *e)
{
  int i;
  for (i = 0; i < 4; i++)
    {
      memset (&e->cl[i], 0, sizeof e->cl[i]);
      memset (&e->cr[i], 0, sizeof e->cr[i]);
      e->cl[i].len = comb_len[i];
      e->cr[i].len = comb_len[i] + 23;
    }
  for (i = 0; i < 2; i++)
    {
      memset (&e->al[i], 0, sizeof e->al[i]);
      memset (&e->ar[i], 0, sizeof e->ar[i]);
      e->al[i].len = ap_len[i];
      e->ar[i].len = ap_len[i] + 23;
    }
}

static inline float comb_run (comb *c, float in)
{
  float out = c->buf[c->pos];
  c->store = out * 0.8f + c->store * 0.2f;          /* damping */
  c->buf[c->pos] = in + c->store * 0.84f;           /* room size */
  if (++c->pos >= c->len) c->pos = 0;
  return out;
}
static inline float ap_run (allpass *a, float in)
{
  float b = a->buf[a->pos];
  a->buf[a->pos] = in + b * 0.5f;
  if (++a->pos >= a->len) a->pos = 0;
  return b - in;
}

/* ---------------------------------------------------------------------- */
/* Creating, freeing                                                      */

gr_engine *gr_engine_new (gr_song *s)
{
  int i;
  gr_engine *e = calloc (1, sizeof *e);
  if (!e)
    return NULL;
  for (i = 0; i < NSYNTH; i++)
    {
      e->sv[i].ks = calloc (KS_MAX, sizeof (float));
      if (!e->sv[i].ks)
        {
          gr_engine_free (e);
          return NULL;
        }
    }
  reverb_init (e);
  e->pv = NULL;
  e->rec_clip = -1;
  for (i = 0; i < GR_MAX_TRACKS; i++)
    e->rec_region[i] = -1;
  e->song = s;
  return e;
}

void gr_engine_free (gr_engine *e)
{
  int i;
  if (!e)
    return;
  for (i = 0; i < NSYNTH; i++)
    free (e->sv[i].ks);
  free (e);
}

static void gm_all_off (gr_engine *e)
{
  if (gm_synth () && e->gm_gen == gm_generation ())
    {
      midisynth_all_notes_off (gm_synth ());
      if (gm_synth_n (1))
        midisynth_all_notes_off (gm_synth_n (1));
    }
  e->gm_noffs = 0;
}

static void mo_all_off (gr_engine *e);

static void all_notes_off (gr_engine *e)
{
  int i;
  gm_all_off (e);
  mo_all_off (e);
  for (i = 0; i < NSYNTH; i++)
    if (e->sv[i].active && e->sv[i].track != LIVE_TRACK)
      {
        env_release (&e->sv[i].amp);
        env_release (&e->sv[i].filt);
        e->sv[i].off_in = -1;
      }
}

void gr_engine_set_song (gr_engine *e, gr_song *s)
{
  int i;
  for (i = 0; i < NSYNTH; i++) e->sv[i].active = 0;
  for (i = 0; i < NDRUM; i++) e->dv[i].active = 0;
  e->playing = e->recording = 0;
  e->pos = 0;
  e->last_tick = -1;
  e->pv = NULL;
  gm_all_off (e);
  e->gm_gen = 0;                /* tell the channels everything again */
  e->song = s;
}

/* ---------------------------------------------------------------------- */
/* Starting notes                                                         */

static void synth_on (gr_engine *e, int track, int patch, int pitch, int vel, int len_samples)
{
  int i, best = -1;
  unsigned oldest = ~0u;
  synth_voice *v;
  const patch_def *p;
  /* Retrigger the same pitch on the same track; else a free voice; else
     steal the oldest. */
  for (i = 0; i < NSYNTH && best < 0; i++)
    if (e->sv[i].active && e->sv[i].track == track && e->sv[i].pitch == pitch)
      best = i;
  for (i = 0; i < NSYNTH && best < 0; i++)
    if (!e->sv[i].active)
      best = i;
  if (best < 0)
    for (i = 0; i < NSYNTH; i++)
      if (e->sv[i].age < oldest)
        {
          oldest = e->sv[i].age;
          best = i;
        }
  v = &e->sv[best];
  p = &patches[patch];
  v->active = 1;
  v->track = track;
  v->pitch = pitch;
  v->patch = patch;
  v->age = ++e->age;
  v->vel = vel / 127.0f;
  v->freq = mtof ((float) pitch);
  v->ph[0] = 0.0f; v->ph[1] = 0.33f; v->ph[2] = 0.66f;
  v->modph = 0.0f;
  v->off_in = len_samples;
  v->f.low = v->f.band = 0.0f;
  v->amp.level = 0.0f;
  env_start (&v->amp, &p->amp);
  v->filt.level = 0.0f;
  env_start (&v->filt, &p->filt);
  if (patch == PATCH_PLUCK)
    {
      /* Fill the delay line with a burst of noise, low-passed a little by
         the velocity: soft notes sound duller, as on a real string. */
      float lp = 0.0f, k = 0.3f + 0.6f * v->vel;
      v->ks_len = (int) (GR_RATE / v->freq);
      if (v->ks_len > KS_MAX) v->ks_len = KS_MAX;
      if (v->ks_len < 2) v->ks_len = 2;
      for (i = 0; i < v->ks_len; i++)
        {
          lp += k * (noise () - lp);
          v->ks[i] = lp;
        }
      v->ks_pos = 0;
      v->ks_last = 0.0f;
    }
}

static void synth_off (gr_engine *e, int track, int pitch)
{
  int i;
  for (i = 0; i < NSYNTH; i++)
    {
      synth_voice *v = &e->sv[i];
      if (v->active && v->track == track && v->pitch == pitch && v->amp.stage != ENV_R)
        {
          env_release (&v->amp);
          env_release (&v->filt);
          v->off_in = -1;
        }
    }
}

static void drum_on (gr_engine *e, int track, int kit, int sound, int vel)
{
  int i, best = 0;
  float most = -1.0f;
  drum_voice *d;
  /* Hi-hats choke each other, like a real pedal. */
  if (sound == DR_CHAT || sound == DR_OHAT)
    for (i = 0; i < NDRUM; i++)
      if (e->dv[i].active && e->dv[i].track == track
          && (e->dv[i].sound == DR_OHAT || e->dv[i].sound == DR_CHAT))
        e->dv[i].len = e->dv[i].len < 64 ? e->dv[i].len : 64;
  for (i = 0; i < NDRUM; i++)
    {
      if (!e->dv[i].active) { best = i; break; }
      if (e->dv[i].t > most) { most = e->dv[i].t; best = i; }
    }
  d = &e->dv[best];
  memset (d, 0, sizeof *d);
  d->active = 1;
  d->track = track;
  d->kit = kit;
  d->sound = sound;
  d->vel = vel / 127.0f;
  switch (sound)
    {
    case DR_KICK:  d->len = kit == KIT_ELECTRO ? GR_RATE : GR_RATE / 2; break;
    case DR_SNARE: d->len = GR_RATE / 4; break;
    case DR_CHAT:  d->len = GR_RATE / 16; break;
    case DR_OHAT:  d->len = GR_RATE / 2; break;
    case DR_CLAP:  d->len = GR_RATE / 3; break;
    case DR_LTOM: case DR_HTOM: d->len = GR_RATE / 2; break;
    default:       d->len = GR_RATE / 5; break;
    }
}

/* ---------------------------------------------------------------------- */
/* Rendering a voice into a mono buffer                                   */

static int synth_render (synth_voice *v, float *out, int n)
{
  const patch_def *p = &patches[v->patch];
  float dt = v->freq / GR_RATE, a, s = 0.0f, fe;
  int i;
  for (i = 0; i < n; i++)
    {
      if (v->off_in >= 0 && --v->off_in < 0)
        {
          env_release (&v->amp);
          env_release (&v->filt);
        }
      a = env_next (&v->amp);
      if (v->amp.stage == ENV_OFF)
        {
          v->active = 0;
          for (; i < n; i++) out[i] = 0.0f;
          return 0;
        }
      switch (v->patch)
        {
        case PATCH_EPIANO:      /* 2-operator FM, index falls with the filter env */
          fe = env_next (&v->filt);
          v->modph = wrap1 (v->modph + dt);
          v->ph[0] = wrap1 (v->ph[0] + dt);
          s = fsin (wrap1 (v->ph[0] + (0.8f + 2.2f * v->vel) * fe * 0.25f * fsin (v->modph) + 1.0f));
          s += 0.15f * fsin (wrap1 (v->ph[0] * 2.0f + 0.0f));
          break;
        case PATCH_BELL:        /* FM with an inharmonic ratio */
          v->modph = wrap1 (v->modph + dt * 3.5f);
          v->ph[0] = wrap1 (v->ph[0] + dt);
          s = fsin (wrap1 (v->ph[0] + 0.5f * a * fsin (v->modph) + 1.0f));
          break;
        case PATCH_BASS:        /* saw + square sub, resonant filter sweep */
          v->ph[0] = wrap1 (v->ph[0] + dt);
          v->ph[1] = wrap1 (v->ph[1] + dt * 0.5f);
          s = (2.0f * v->ph[0] - 1.0f) - polyblep (v->ph[0], dt);
          s += 0.5f * (v->ph[1] < 0.5f ? 1.0f : -1.0f);
          fe = env_next (&v->filt);
          if ((i & 7) == 0) v->ks_last = svf_cut (p->cutoff + p->env_amt * fe * (0.4f + 0.6f * v->vel));
          s = svf_lp (&v->f, s, v->ks_last, p->q);
          break;
        case PATCH_PAD:         /* three detuned saws */
          {
            int k;
            static const float det[3] = { 1.0f, 1.0059f, 0.9941f };
            s = 0.0f;
            for (k = 0; k < 3; k++)
              {
                float d = dt * det[k];
                v->ph[k] = wrap1 (v->ph[k] + d);
                s += (2.0f * v->ph[k] - 1.0f) - polyblep (v->ph[k], d);
              }
            fe = env_next (&v->filt);
            if ((i & 7) == 0) v->ks_last = svf_cut (p->cutoff + p->env_amt * fe);
            s = svf_lp (&v->f, s, v->ks_last, p->q);
          }
          break;
        case PATCH_LEAD:        /* square with a little vibrato */
          v->modph = wrap1 (v->modph + 5.5f / GR_RATE);
          {
            float d = dt * (1.0f + 0.004f * fsin (v->modph));
            v->ph[0] = wrap1 (v->ph[0] + d);
            s = v->ph[0] < 0.5f ? 1.0f : -1.0f;
            s += polyblep (v->ph[0], d);
            s -= polyblep (wrap1 (v->ph[0] + 0.5f), d);
          }
          fe = env_next (&v->filt);
          if ((i & 7) == 0) v->ks_last = svf_cut (p->cutoff + p->env_amt * fe);
          s = svf_lp (&v->f, s, v->ks_last, p->q);
          break;
        case PATCH_PLUCK:       /* Karplus-Strong */
          {
            float x = v->ks[v->ks_pos];
            float y = 0.5f * (x + v->ks_last) * 0.996f;
            v->ks_last = x;
            v->ks[v->ks_pos] = y;
            if (++v->ks_pos >= v->ks_len) v->ks_pos = 0;
            s = x * 1.6f;
          }
          break;
        case PATCH_ORGAN:       /* drawbars 8', 4', 2 2/3', 2' */
          v->ph[0] = wrap1 (v->ph[0] + dt);
          s = fsin (v->ph[0]) + 0.6f * fsin (wrap1 (v->ph[0] * 2.0f))
            + 0.4f * fsin (wrap1 (v->ph[0] * 3.0f)) + 0.3f * fsin (wrap1 (v->ph[0] * 4.0f));
          break;
        default:
          s = 0.0f;
        }
      out[i] = s * a * v->vel * p->gain;
    }
  return 1;
}

static int drum_render (drum_voice *d, float *out, int n)
{
  int i;
  int electro = d->kit == KIT_ELECTRO;
  for (i = 0; i < n; i++)
    {
      float s = 0.0f, t = d->t, env;
      if (d->len-- <= 0)
        {
          d->active = 0;
          for (; i < n; i++) out[i] = 0.0f;
          return 0;
        }
      switch (d->sound)
        {
        case DR_KICK:
          {
            /* Sine with a fast falling pitch; the electro kick is the long
               booming 808 kind. */
            float f = (electro ? 48.0f : 52.0f) + (electro ? 120.0f : 180.0f) * expf (-t * 38.0f);
            d->ph = wrap1 (d->ph + f / GR_RATE);
            env = expf (-t * (electro ? 3.2f : 9.0f));
            s = fsin (d->ph) * env * 1.1f;
            if (t < 0.002f) s += noise () * 0.3f * (1.0f - t / 0.002f);   /* click */
          }
          break;
        case DR_SNARE:
          d->ph = wrap1 (d->ph + (electro ? 238.0f : 190.0f) / GR_RATE);
          d->lp += 0.45f * (noise () - d->lp);
          s = 0.5f * fsin (d->ph) * expf (-t * 30.0f) + 0.75f * (noise () - d->lp * 0.6f) * expf (-t * (electro ? 22.0f : 16.0f));
          break;
        case DR_CHAT:
        case DR_OHAT:
          {
            /* High-passed noise plus a cluster of square waves for metal. */
            float x = noise () * 0.6f;
            d->ph = wrap1 (d->ph + 3200.0f / GR_RATE);
            d->ph2 = wrap1 (d->ph2 + 4790.0f / GR_RATE);
            x += 0.25f * ((d->ph < 0.5f) - (d->ph2 < 0.5f));
            d->hp_out = 0.7f * (d->hp_out + x - d->hp_prev);
            d->hp_prev = x;
            s = d->hp_out * expf (-t * (d->sound == DR_CHAT ? 60.0f : 7.0f)) * 0.7f;
          }
          break;
        case DR_CLAP:
          {
            /* Three quick bursts then a tail: the classic clap trick. */
            float tt = t < 0.03f ? fmodf (t, 0.01f) : t - 0.02f;
            d->lp += 0.5f * (noise () - d->lp);
            s = (noise () - d->lp) * expf (-tt * (t < 0.03f ? 250.0f : 18.0f)) * 0.8f;
          }
          break;
        case DR_LTOM:
        case DR_HTOM:
          {
            float base = d->sound == DR_LTOM ? 95.0f : 150.0f;
            d->ph = wrap1 (d->ph + (base + base * 0.6f * expf (-t * 20.0f)) / GR_RATE);
            s = fsin (d->ph) * expf (-t * 7.0f) * 0.8f;
          }
          break;
        default:                /* cowbell: two detuned squares, band-limited a bit */
          d->ph = wrap1 (d->ph + 540.0f / GR_RATE);
          d->ph2 = wrap1 (d->ph2 + 800.0f / GR_RATE);
          d->lp += 0.3f * (((d->ph < 0.5f) - 0.5f) + ((d->ph2 < 0.5f) - 0.5f) - d->lp);
          s = d->lp * expf (-t * 14.0f) * 0.7f;
          break;
        }
      out[i] = s * d->vel * 0.55f;      /* kit level, matched to the synths */
      d->t += 1.0f / GR_RATE;
    }
  return 1;
}

/* (any_solo is used by the SoundFont code) */
static int any_solo (const gr_song *s);

/* ---------------------------------------------------------------------- */
/* SoundFont sounds                                                       */

/* Is this track played by the SoundFont? (If none is loaded, a track that
   asks for one plays Groove's own sound instead.) */
/* A MIDI channel per instrument track, in order and skipping 9; every
   drum track shares channel 9, the only one TinySoundFont plays kits on.
   The first 15 instrument tracks fit one synth; a 16th (a song with no
   drum or audio tracks) is channel 16: the second synth's first. */
static int gm_channel (const gr_song *s, int track)
{
  int t, k = 0;
  if (s->tracks[track].type == TRK_DRUMS)
    return 9;
  for (t = 0; t < track; t++)
    if (s->tracks[t].type == TRK_INST)
      k++;
  return k < 9 ? k : k + 1;
}

static midisynth *gm_synth_for (int ch) { return gm_synth_n (ch >> 4); }

static int gm_track (const gr_engine *e, int track)
{
  const gr_track *t;
  if (!gm_synth () || track < 0 || track >= e->song->ntracks)
    return 0;
  t = &e->song->tracks[track];
  if (t->gm < 0 || t->type == TRK_AUDIO)
    return 0;
  return gm_synth_for (gm_channel (e->song, track)) != NULL;   /* (else Groove's own sound) */
}

static int gm_audible (const gr_song *s, int track, int solo)
{
  const gr_track *t = &s->tracks[track];
  return !t->mute && (!solo || t->solo);
}

/* Bring each channel's program, volume and pan into line with its track.
   Only what has changed is sent. */
static void gm_sync (gr_engine *e, int solo)
{
  gr_song *s = e->song;
  int prog[GM_CHANNELS], vol[GM_CHANNELS], pan[GM_CHANNELS], t, ch, ngm = 0;
  float send = 0.0f;
  if (e->gm_gen != gm_generation ())
    {
      for (ch = 0; ch < GM_CHANNELS; ch++)
        e->gm_prog[ch] = e->gm_vol[ch] = e->gm_pan[ch] = -1;
      e->gm_noffs = 0;
      e->gm_gen = gm_generation ();
    }
  for (ch = 0; ch < GM_CHANNELS; ch++)
    prog[ch] = vol[ch] = pan[ch] = -1;
  for (t = 0; t < s->ntracks; t++)
    {
      const gr_track *tr = &s->tracks[t];
      int on;
      if (!gm_track (e, t))
        continue;
      ch = gm_channel (s, t);
      on = gm_audible (s, t, solo);
      /* Channel 9: the first drum track that can be heard sets the kit
         and pan; drum volume goes into each note's velocity. */
      if (ch == 9 && prog[9] >= 0 && (vol[9] > 0 || !on))
        continue;
      prog[ch] = tr->gm;
      vol[ch] = !on ? 0 : ch == 9 ? 127 : (int) (tr->vol * 127.0f + 0.5f);
      pan[ch] = (int) ((tr->pan + 1.0f) * 63.5f);
      if (on)
        {
          send += tr->reverb;
          ngm++;
        }
    }
  e->gm_send = ngm ? send / ngm : 0.0f;
  for (ch = 0; ch < GM_CHANNELS; ch++)
    {
      midisynth *ms = gm_synth_for (ch);
      if (prog[ch] < 0 || !ms)
        continue;
      if (vol[ch] > 127) vol[ch] = 127;
      if (pan[ch] < 0) pan[ch] = 0;
      if (pan[ch] > 127) pan[ch] = 127;
      if (prog[ch] != e->gm_prog[ch])
        midisynth_program (ms, ch & 15, prog[ch]);
      if (vol[ch] != e->gm_vol[ch])
        midisynth_control (ms, ch & 15, 7, vol[ch]);
      if (pan[ch] != e->gm_pan[ch])
        midisynth_control (ms, ch & 15, 10, pan[ch]);
      e->gm_prog[ch] = prog[ch];
      e->gm_vol[ch] = vol[ch];
      e->gm_pan[ch] = pan[ch];
    }
}

static void gm_off_now (gr_engine *e, int i)
{
  midisynth *ms = gm_synth_for (e->gm_off[i].ch);
  if (ms)
    midisynth_note_off (ms, e->gm_off[i].ch & 15, e->gm_off[i].key);
  e->gm_off[i] = e->gm_off[--e->gm_noffs];
}

/* A note on the track's channel; len_samples < 0: held until gm_note_off. */
static void gm_note_on (gr_engine *e, int track, int pitch, int vel, int len_samples)
{
  gr_song *s = e->song;
  gr_track *tr = &s->tracks[track];
  int ch, key, i;
  float level;
  if (e->gm_gen != gm_generation ())
    gm_sync (e, any_solo (s));
  ch = gm_channel (s, track);
  if (ch == 9)
    {
      key = gr_gm_drum_note (pitch % GR_DRUM_SOUNDS);
      vel = (int) (vel * tr->vol / 0.8f + 0.5f);   /* 0.8 is a new track's volume */
      if (vel > 127) vel = 127;
      if (vel < 1) return;
      if (len_samples < 0)
        len_samples = GR_RATE / 2;   /* kits don't need note-offs, but voices do */
    }
  else
    key = pitch & 127;
  /* The same key again: end the old note first, so its note-off can't cut
     the new one short. */
  for (i = 0; i < e->gm_noffs; i++)
    if (e->gm_off[i].ch == ch && e->gm_off[i].key == key)
      {
        gm_off_now (e, i);
        break;
      }
  if (!gm_synth_for (ch))
    return;
  midisynth_note_on (gm_synth_for (ch), ch & 15, key, vel);
  if (len_samples >= 0 && e->gm_noffs < GM_PENDING)
    {
      e->gm_off[e->gm_noffs].ch = ch;
      e->gm_off[e->gm_noffs].key = key;
      e->gm_off[e->gm_noffs].left = len_samples > 0 ? len_samples : 1;
      e->gm_noffs++;
    }
  level = vel / 127.0f * (ch == 9 ? 0.8f : tr->vol) * 0.7f;
  if (level > tr->meter)
    tr->meter = level;
  e->gm_tail = GR_RATE * 3;          /* the SoundFont's own release */
  e->busy_tail = GR_RATE * 2;
}

static void gm_note_off (gr_engine *e, int track, int pitch)
{
  int ch = gm_channel (e->song, track);
  if (ch != 9 && gm_synth_for (ch))
    midisynth_note_off (gm_synth_for (ch), ch & 15, pitch & 127);
}

/* Mix n frames of the SoundFont into the song. */
static void gm_render (gr_engine *e, float *l, float *r, float *rv, int n)
{
  int16_t buf[2 * SUB];
  int i;
  for (i = 0; i < e->gm_noffs; )
    if ((e->gm_off[i].left -= n) <= 0)
      gm_off_now (e, i);
    else
      i++;
  midisynth_render (gm_synth (), buf, n, 0);
  if (gm_synth_n (1))
    midisynth_render (gm_synth_n (1), buf, n, 1);   /* the second synth, added in */
  for (i = 0; i < n; i++)
    {
      float a = buf[2 * i] * (GM_GAIN / 32768.0f), b = buf[2 * i + 1] * (GM_GAIN / 32768.0f);
      l[i] += a;
      r[i] += b;
      rv[i] += (a + b) * 0.5f * e->gm_send;
    }
  e->gm_tail -= n;
}

/* ---------------------------------------------------------------------- */
/* MIDI out                                                               */

static int any_solo (const gr_song *s);

static int out_track (const gr_engine *e, int track)
{
  const gr_track *t;
  if (!e->mo_fn || track < 0 || track >= e->song->ntracks)
    return 0;
  t = &e->song->tracks[track];
  return t->midi_out && t->type != TRK_AUDIO;
}

/* (A 16th instrument track has no channel of its own: it shares the last.) */
static int mo_channel (const gr_song *s, int track)
{
  int ch = gm_channel (s, track);
  return ch > 15 ? 15 : ch;
}

static void mo_send (gr_engine *e, int status, int d1, int d2)
{
  int len = (status & 0xF0) == 0xC0 ? 2 : 3;
  e->mo_used |= 1u << (status & 15);
  e->mo_fn (e->mo_ctx, e->mo_live ? 0 : e->frames,
            (unsigned) status | (unsigned) (d1 & 127) << 8 | (unsigned) (d2 & 127) << 16 | (unsigned) len << 24);
}

/* Each channel's program, volume and pan, from its track; only what has
   changed is sent. (A muted track's channel goes to volume 0, so held
   notes fall silent too.) */
static void mo_sync (gr_engine *e)
{
  gr_song *s = e->song;
  int prog[16], vol[16], pan[16], t, ch, live, solo = any_solo (s);
  for (ch = 0; ch < 16; ch++)
    prog[ch] = -1;
  for (t = 0; t < s->ntracks; t++)
    {
      const gr_track *tr = &s->tracks[t];
      int on;
      if (!out_track (e, t))
        continue;
      ch = mo_channel (s, t);
      on = gm_audible (s, t, solo);
      if (prog[ch] >= 0 && (vol[ch] > 0 || !on))
        continue;               /* (the first drum track that can be heard) */
      if (tr->type == TRK_DRUMS)
        prog[ch] = tr->gm >= 0 ? tr->gm : tr->patch == KIT_ELECTRO ? GM_KIT_808 : GM_KIT_STANDARD;
      else
        prog[ch] = tr->gm >= 0 ? tr->gm : gr_patch_program (tr->patch);
      vol[ch] = !on ? 0 : ch == 9 ? 127 : (int) (tr->vol * 127.0f + 0.5f);
      pan[ch] = (int) ((tr->pan + 1.0f) * 63.5f);
      if (vol[ch] > 127) vol[ch] = 127;
      if (pan[ch] < 0) pan[ch] = 0;
      if (pan[ch] > 127) pan[ch] = 127;
    }
  /* Settings go out at once, not when the sound now being rendered is
     heard: a note played live mustn't overtake its channel's program. */
  live = e->mo_live;
  e->mo_live = 1;
  for (ch = 0; ch < 16; ch++)
    {
      if (prog[ch] < 0)
        continue;
      if (prog[ch] != e->mo_prog[ch])
        mo_send (e, 0xC0 | ch, prog[ch], 0);
      if (vol[ch] != e->mo_vol[ch])
        mo_send (e, 0xB0 | ch, 7, vol[ch]);
      if (pan[ch] != e->mo_pan[ch])
        mo_send (e, 0xB0 | ch, 10, pan[ch]);
      e->mo_prog[ch] = prog[ch];
      e->mo_vol[ch] = vol[ch];
      e->mo_pan[ch] = pan[ch];
    }
  e->mo_live = live;
}

static void mo_off_now (gr_engine *e, int i)
{
  mo_send (e, 0x80 | e->mo_off[i].ch, e->mo_off[i].key, 64);
  e->mo_off[i] = e->mo_off[--e->mo_noffs];
}

/* len_samples < 0: held until mo_note_off. */
static void mo_note_on (gr_engine *e, int track, int pitch, int vel, int len_samples)
{
  gr_song *s = e->song;
  gr_track *tr = &s->tracks[track];
  int ch = mo_channel (s, track), key, i;
  float level;
  mo_sync (e);
  if (tr->type == TRK_DRUMS)
    {
      key = gr_gm_drum_note (pitch % GR_DRUM_SOUNDS);
      vel = (int) (vel * tr->vol / 0.8f + 0.5f);   /* (drum tracks share a channel) */
      if (vel > 127) vel = 127;
      if (vel < 1) return;
      len_samples = GR_RATE / 10;   /* drums need no note-off, but some devices like one */
    }
  else
    key = pitch & 127;
  if (vel < 1)
    vel = 1;
  for (i = 0; i < e->mo_noffs; i++)
    if (e->mo_off[i].ch == ch && e->mo_off[i].key == key)
      {
        mo_off_now (e, i);      /* the same key again: end the old note first */
        break;
      }
  mo_send (e, 0x90 | ch, key, vel > 127 ? 127 : vel);
  if (len_samples >= 0 && e->mo_noffs < GM_PENDING)
    {
      e->mo_off[e->mo_noffs].ch = ch;
      e->mo_off[e->mo_noffs].key = key;
      e->mo_off[e->mo_noffs].left = len_samples > 0 ? len_samples : 1;
      e->mo_noffs++;
    }
  level = vel / 127.0f * (ch == 9 ? 0.8f : tr->vol) * 0.7f;
  if (level > tr->meter)
    tr->meter = level;
}

static void mo_note_off (gr_engine *e, int track, int pitch)
{
  if (e->song->tracks[track].type != TRK_DRUMS)
    mo_send (e, 0x80 | mo_channel (e->song, track), pitch & 127, 64);
}

/* Note-offs that are due in the n frames just rendered. */
static void mo_tick (gr_engine *e, int n)
{
  int i;
  for (i = 0; i < e->mo_noffs; )
    if ((e->mo_off[i].left -= n) <= 0)
      mo_off_now (e, i);
    else
      i++;
}

static void mo_all_off (gr_engine *e)
{
  int ch;
  if (!e->mo_fn)
    return;
  while (e->mo_noffs > 0)
    mo_off_now (e, e->mo_noffs - 1);
  for (ch = 0; ch < 16; ch++)
    if (e->mo_used & (1u << ch))
      mo_send (e, 0xB0 | ch, 123, 0);   /* all notes off (and so live notes too) */
}

void gr_midi_out_reset (gr_engine *e)
{
  int ch;
  e->mo_live = 1;
  mo_all_off (e);
  e->mo_live = 0;
  for (ch = 0; ch < 16; ch++)
    e->mo_prog[ch] = e->mo_vol[ch] = e->mo_pan[ch] = -1;
}

void gr_engine_set_midi_out (gr_engine *e, gr_midi_sender *fn, void *ctx)
{
  if (e->mo_fn)
    gr_midi_out_reset (e);
  e->mo_fn = fn;
  e->mo_ctx = ctx;
  e->mo_used = 0;
  e->mo_noffs = 0;
  gr_midi_out_reset (e);
}

unsigned gr_frames (const gr_engine *e) { return e->frames; }

/* ---------------------------------------------------------------------- */
/* Transport                                                              */

static double samples_per_tick (const gr_song *s)
{
  return GR_RATE * 60.0 / ((double) s->tempo * GR_PPQ);
}

void gr_play (gr_engine *e)
{
  if (e->playing)
    return;
  e->playing = 1;
  e->play_from = e->pos;
  e->last_tick = (int) e->pos - 1;
}

static void quantize_recorded (gr_engine *e);

void gr_stop (gr_engine *e)
{
  if (e->count_left > 0)        /* stopped during the count-in: nothing recorded */
    {
      e->count_left = 0;
      quantize_recorded (e);    /* (drops the empty recording regions) */
      e->recording = 0;
      return;
    }
  if (!e->playing)
    {
      e->pos = e->song->loop_on ? e->song->loop_start : 0;   /* second stop: rewind */
      return;
    }
  if (e->recording)
    quantize_recorded (e);
  e->playing = e->recording = 0;
  all_notes_off (e);
  e->busy_tail = GR_RATE * 2;     /* let the notes and the reverb ring out */
}

void gr_locate (gr_engine *e, int tick)
{
  if (tick < 0)
    tick = 0;
  all_notes_off (e);
  e->pos = tick;
  e->last_tick = tick - 1;
}

int gr_playing (const gr_engine *e) { return e->playing; }
int gr_previewing (const gr_engine *e) { return e->pv != NULL; }
int gr_recording (const gr_engine *e) { return e->recording; }
double gr_position (const gr_engine *e) { return e->pos; }

int gr_busy (const gr_engine *e)
{
  int i;
  if (e->playing || e->pv || e->busy_tail > 0 || e->count_left > 0 || (e->gm_tail > 0 && gm_synth ()))
    return 1;
  for (i = 0; i < NSYNTH; i++) if (e->sv[i].active) return 1;
  for (i = 0; i < NDRUM; i++) if (e->dv[i].active) return 1;
  return 0;
}

/* ---------------------------------------------------------------------- */
/* Recording                                                              */

void gr_record (gr_engine *e)
{
  int t;
  gr_song *s = e->song;
  for (t = 0; t < s->ntracks; t++)
    {
      e->rec_region[t] = -1;
      if (s->tracks[t].armed && s->tracks[t].type != TRK_AUDIO)
        {
          /* A region from the bar we start in; it grows as we play. */
          int bar = gr_tick_bar (s, (int) e->pos), start = gr_bar_tick (s, bar);
          int p = gr_pattern_new (s, "Recording", gr_bar_len (s, bar));
          if (p >= 0)
            e->rec_region[t] = gr_region_add (s, t, start, gr_bar_len (s, bar), p);
        }
      else if (s->tracks[t].armed && s->tracks[t].type == TRK_AUDIO && e->rec_clip < 0)
        {
          int c = gr_clip_new (s, "Recording", GR_RATE);
          if (c >= 0)
            {
              int r = gr_region_add (s, t, (int) e->pos, GR_BAR, -1);
              if (r >= 0)
                {
                  s->regions[r].clip = c;
                  e->rec_region[t] = r;
                  e->rec_clip = c;
                }
            }
        }
    }
  e->recording = 1;
  if (e->count_beats > 0)
    {
      /* A bar of the meter we start in: its beats, at its beat length. */
      int bar = gr_tick_bar (s, (int) e->pos);
      e->count_beat = gr_beat_len (s, bar);
      e->count_beats = gr_meter_at (s, bar)->num;
      /* A bar of clicks first; time stands still until it's done. */
      e->count_left = (int) (e->count_beats * e->count_beat * samples_per_tick (s));
      e->count_pos = 0;
      return;
    }
  gr_play (e);
}

/* On (beats > 0) or off. When on, recording is counted in with a bar of
   the meter it starts in. */
void gr_set_count_in (gr_engine *e, int beats) { e->count_beats = beats < 0 ? 0 : beats; }
void gr_set_merge (gr_engine *e, int on) { e->merge = on; }

int gr_counting_in (const gr_engine *e)
{
  double beat;
  if (e->count_left <= 0)
    return 0;
  beat = e->count_beat * samples_per_tick (e->song);
  return (int) ((e->count_left + beat - 1) / beat);    /* beats left, rounded up */
}

static void rec_grow (gr_engine *e)
{
  gr_song *s = e->song;
  int t, now = (int) e->pos;
  for (t = 0; t < s->ntracks; t++)
    {
      gr_region *r;
      if (e->rec_region[t] < 0)
        continue;
      r = &s->regions[e->rec_region[t]];
      if (now >= r->start + r->len)
        {
          int len = gr_bar_tick (s, gr_tick_bar (s, now) + 1) - r->start;
          r->len = len;
          if (r->pattern >= 0)
            s->patterns[r->pattern].len = len;
          gr_song_cover (s, r->start + len);
        }
    }
}

static void rec_note_on (gr_engine *e, int track, int pitch)
{
  if (track >= 0 && track < GR_MAX_TRACKS && e->recording && e->count_left <= 0 && e->rec_region[track] >= 0)
    e->rec_start_tick[track][pitch & 127] = (int) e->pos + 1;   /* +1: 0 means none */
}

static void rec_note_off (gr_engine *e, int track, int pitch, int vel)
{
  gr_song *s = e->song;
  gr_region *r;
  int start;
  if (track < 0 || track >= GR_MAX_TRACKS || !e->recording || e->rec_region[track] < 0)
    return;
  start = e->rec_start_tick[track][pitch & 127] - 1;
  e->rec_start_tick[track][pitch & 127] = 0;
  r = &s->regions[e->rec_region[track]];
  if (start < r->start || r->pattern < 0)
    return;
  rec_grow (e);
  gr_pattern_add_note (&s->patterns[r->pattern], start - r->start,
                       (int) e->pos - start, pitch, vel);
}

/* Merge recordings: notes recorded over another region on the same track
   go into that region (each region gets its own copy of a shared pattern
   first). What's left over stays in the new region; if nothing is, the new
   region goes. */
static void merge_recording (gr_song *s, int ri)
{
  gr_region *r = &s->regions[ri];
  gr_pattern *p = &s->patterns[r->pattern], keep;
  int k, j;
  keep = *p;
  keep.nnotes = 0;
  for (k = 0; k < p->nnotes; k++)
    {
      const gr_note *n = &p->notes[k];
      int at = r->start + n->tick;
      for (j = 0; j < GR_MAX_REGIONS; j++)
        {
          const gr_region *o = &s->regions[j];
          if (j != ri && o->used && o->track == r->track && o->clip < 0 && o->pattern >= 0
              && at >= o->start && at < o->start + o->len)
            break;
        }
      if (j < GR_MAX_REGIONS)
        {
          int q = gr_pattern_unshare (s, j);
          gr_pattern *qp = &s->patterns[q];
          int local = (at - s->regions[j].start) % qp->len;
          if (gr_pattern_find_note (qp, local, n->pitch) < 0)
            gr_pattern_add_note (qp, local, n->len, n->pitch, n->vel);
        }
      else
        keep.notes[keep.nnotes++] = *n;
    }
  *p = keep;
  if (!keep.nnotes)
    r->used = 0;
}

/* Snap recorded note starts to the nearest sixteenth ("quantize"), so
   small timing slips are tidied away. */
static void quantize_recorded (gr_engine *e)
{
  gr_song *s = e->song;
  int t, i;
  for (t = 0; t < s->ntracks; t++)
    {
      gr_region *r;
      gr_pattern *p, tmp;
      if (e->rec_region[t] < 0)
        continue;
      r = &s->regions[e->rec_region[t]];
      e->rec_region[t] = -1;
      if (r->clip >= 0)
        {
          /* Audio: the region is exactly as long as what was recorded. */
          gr_clip *c = &s->clips[r->clip];
          r->len = (int) (c->frames / samples_per_tick (s)) + 1;
          e->rec_clip = -1;
          continue;
        }
      p = &s->patterns[r->pattern];
      if (p->nnotes == 0)
        {
          r->used = 0;          /* nothing played: no empty region left behind */
          continue;
        }
      tmp = *p;
      p->nnotes = 0;
      for (i = 0; i < tmp.nnotes; i++)
        {
          int tick = ((tmp.notes[i].tick + GR_STEP / 2) / GR_STEP) * GR_STEP;
          int len = tmp.notes[i].len < GR_STEP / 2 ? GR_STEP / 2 : tmp.notes[i].len;
          if (tick >= p->len) tick = p->len - GR_STEP;
          if (gr_pattern_find_note (p, tick, tmp.notes[i].pitch) < 0)
            gr_pattern_add_note (p, tick, len, tmp.notes[i].pitch, tmp.notes[i].vel);
        }
      if (e->merge)
        merge_recording (s, (int) (r - s->regions));
    }
  gr_song_gc (s);
}

void gr_audio_in (gr_engine *e, const int16_t *stereo, int frames)
{
  gr_song *s = e->song;
  int t;
  if (!e->recording || e->rec_clip < 0)
    return;
  gr_clip_append (&s->clips[e->rec_clip], stereo, frames);
  for (t = 0; t < s->ntracks; t++)
    if (e->rec_region[t] >= 0 && s->regions[e->rec_region[t]].clip == e->rec_clip)
      {
        gr_region *r = &s->regions[e->rec_region[t]];
        int len = (int) (s->clips[e->rec_clip].frames / samples_per_tick (s)) + 1;
        if (len > r->len)
          r->len = len;
      }
}

/* ---------------------------------------------------------------------- */
/* Live playing                                                           */

void gr_live_on (gr_engine *e, int track, int pitch, int vel)
{
  gr_song *s = e->song;
  if (track < 0 || track >= s->ntracks)
    return;
  if (out_track (e, track))
    {
      e->mo_live = 1;
      mo_note_on (e, track, s->tracks[track].type == TRK_DRUMS ? pitch % GR_DRUM_SOUNDS : pitch, vel, -1);
      e->mo_live = 0;
    }
  if (s->tracks[track].type == TRK_DRUMS)
    {
      if (out_track (e, track))
        ;
      else if (gm_track (e, track))
        gm_note_on (e, track, pitch % GR_DRUM_SOUNDS, vel, -1);
      else
        drum_on (e, track, s->tracks[track].patch, pitch % GR_DRUM_SOUNDS, vel);
      if (e->recording && e->rec_region[track] >= 0)
        {
          rec_note_on (e, track, pitch % GR_DRUM_SOUNDS);
          rec_note_off (e, track, pitch % GR_DRUM_SOUNDS, vel);
        }
    }
  else if (s->tracks[track].type == TRK_INST)
    {
      if (out_track (e, track))
        ;
      else if (gm_track (e, track))
        gm_note_on (e, track, pitch, vel, -1);
      else
        synth_on (e, track, s->tracks[track].patch, pitch, vel, -1);
      rec_note_on (e, track, pitch);
      e->rec_vel[track][pitch & 127] = (uint8_t) vel;
    }
}

void gr_live_off (gr_engine *e, int track, int pitch)
{
  gr_song *s = e->song;
  int vel;
  if (track < 0 || track >= s->ntracks || s->tracks[track].type != TRK_INST)
    return;
  if (out_track (e, track))
    {
      e->mo_live = 1;
      mo_note_off (e, track, pitch);
      e->mo_live = 0;
    }
  else if (gm_track (e, track))
    gm_note_off (e, track, pitch);
  else
    synth_off (e, track, pitch);
  vel = e->rec_vel[track][pitch & 127];
  rec_note_off (e, track, pitch, vel ? vel : 100);
}

void gr_preview_pattern (gr_engine *e, int type, int patch, const gr_pattern *p)
{
  gr_preview_stop (e);
  e->pv = p;
  e->pv_type = type;
  e->pv_patch = patch;
  e->pv_pos = 0.0;
  e->pv_last = -1;
}

void gr_preview_stop (gr_engine *e)
{
  int i;
  e->pv = NULL;
  for (i = 0; i < NSYNTH; i++)
    if (e->sv[i].active && e->sv[i].track == LIVE_TRACK)
      {
        env_release (&e->sv[i].amp);
        e->sv[i].off_in = -1;
      }
}

/* ---------------------------------------------------------------------- */
/* Sequencer                                                              */

static int any_solo (const gr_song *s)
{
  int t;
  for (t = 0; t < s->ntracks; t++)
    if (s->tracks[t].solo)
      return 1;
  return 0;
}

/* Fire every note that starts at integer tick `tick` of the song. */
static void fire_tick (gr_engine *e, int tick)
{
  gr_song *s = e->song;
  int i, k, solo = any_solo (s);
  double spt = samples_per_tick (s);
  for (i = 0; i < GR_MAX_REGIONS; i++)
    {
      const gr_region *r = &s->regions[i];
      const gr_track *tr;
      const gr_pattern *p;
      int local;
      if (!r->used || r->clip >= 0 || r->pattern < 0 || tick < r->start || tick >= r->start + r->len)
        continue;
      if (e->recording && r->track < GR_MAX_TRACKS && e->rec_region[r->track] == i)
        continue;               /* don't play back what is being recorded */
      tr = &s->tracks[r->track];
      p = &s->patterns[r->pattern];
      local = (tick - r->start) % p->len;   /* regions loop their pattern */
      for (k = 0; k < p->nnotes && p->notes[k].tick <= local; k++)
        {
          const gr_note *n = &p->notes[k];
          if (n->tick != local)
            continue;
          if (out_track (e, r->track))
            {
              int len = n->len;
              if (!gm_audible (s, r->track, solo))
                continue;
              if (tick + len > r->start + r->len)
                len = r->start + r->len - tick;
              mo_note_on (e, r->track, n->pitch, n->vel, (int) (len * spt));
            }
          else if (gm_track (e, r->track))
            {
              int len = n->len;
              if (!gm_audible (s, r->track, solo))
                continue;           /* (Groove's voices are muted in the mix instead) */
              if (tick + len > r->start + r->len)
                len = r->start + r->len - tick;
              gm_note_on (e, r->track, n->pitch, n->vel,
                          tr->type == TRK_DRUMS ? -1 : (int) (len * spt));
            }
          else if (tr->type == TRK_DRUMS)
            drum_on (e, r->track, tr->patch, n->pitch % GR_DRUM_SOUNDS, n->vel);
          else
            {
              /* Cut the note at the end of the region. */
              int len = n->len;
              if (tick + len > r->start + r->len)
                len = r->start + r->len - tick;
              synth_on (e, r->track, tr->patch, n->pitch, n->vel, (int) (len * spt));
            }
        }
    }
  if (s->metronome && tick >= 0)
    {
      int bar = gr_tick_bar (s, tick), in = tick - gr_bar_tick (s, bar);
      if (in % gr_beat_len (s, bar) == 0)
        {
          e->click_t = 0.0f;
          e->click_f = in == 0 ? 1500.0f : 1000.0f;   /* higher on the bar's first beat */
          e->click_len = GR_RATE / 40;
        }
    }
}

static void preview_tick (gr_engine *e, double spt)
{
  const gr_pattern *p = e->pv;
  int tick, k;
  if (!p || p->len <= 0)
    return;
  tick = (int) e->pv_pos;
  while (e->pv_last < tick)
    {
      int t = ++e->pv_last % p->len;
      for (k = 0; k < p->nnotes && p->notes[k].tick <= t; k++)
        if (p->notes[k].tick == t)
          {
            if (e->pv_type == TRK_DRUMS)
              drum_on (e, LIVE_TRACK, e->pv_patch, p->notes[k].pitch % GR_DRUM_SOUNDS, p->notes[k].vel);
            else
              synth_on (e, LIVE_TRACK, e->pv_patch, p->notes[k].pitch, p->notes[k].vel,
                        (int) (p->notes[k].len * spt));
          }
    }
}

/* Audio regions: copy the right part of the clip into the track's mix. */
static void audio_regions (gr_engine *e, float *l, float *r, float *rv, int n, int solo)
{
  gr_song *s = e->song;
  double spt = samples_per_tick (s);
  int i, j;
  for (i = 0; i < GR_MAX_REGIONS; i++)
    {
      const gr_region *g = &s->regions[i];
      const gr_track *tr;
      const gr_clip *c;
      double off;
      float gl, gr, gv, peak = 0.0f;
      if (!g->used || g->clip < 0 || e->pos < g->start || e->pos >= g->start + g->len)
        continue;
      if (e->recording && e->rec_region[g->track] == i)
        continue;
      tr = &s->tracks[g->track];
      if (tr->mute || (solo && !tr->solo))
        continue;
      c = &s->clips[g->clip];
      if (!c->used || !c->data)
        continue;
      off = (e->pos - g->start) * spt * c->rate / GR_RATE + g->clip_offset;
      pan_gains (tr->vol, tr->pan, &gl, &gr);
      gv = tr->vol * tr->reverb;
      for (j = 0; j < n; j++)
        {
          int f = (int) (off + (double) j * c->rate / GR_RATE);
          float a, b;
          if (f < 0 || f >= c->frames)
            break;
          a = c->data[2 * f] * (1.0f / 32768.0f);
          b = c->data[2 * f + 1] * (1.0f / 32768.0f);
          l[j] += a * gl;
          r[j] += b * gr;
          rv[j] += (a + b) * 0.5f * gv;
          if (fabsf (a) > peak) peak = fabsf (a);
        }
      if (peak > s->tracks[g->track].meter)
        s->tracks[g->track].meter = peak;
    }
}

/* ---------------------------------------------------------------------- */
/* The mixer                                                              */

static void render_sub (gr_engine *e, float *l, float *r, int n)
{
  gr_song *s = e->song;
  float tmp[SUB], rv[SUB];
  int i, j, solo = any_solo (s);
  double spt = samples_per_tick (s);

  memset (l, 0, n * sizeof *l);
  memset (r, 0, n * sizeof *r);
  memset (rv, 0, sizeof rv);

  if (gm_synth ())
    gm_sync (e, solo);
  if (e->mo_fn)
    mo_sync (e);

  /* The count-in: a click on every beat, time standing still, then play. */
  if (e->count_left > 0)
    {
      double beat = e->count_beat * spt;
      double next = ((int) ((e->count_pos + beat - 1) / beat)) * beat;
      if (next < e->count_pos + n)
        {
          e->click_t = 0.0f;
          e->click_f = next < 1.0 ? 1500.0f : 1000.0f;
          e->click_len = GR_RATE / 40;
        }
      e->count_pos += n;
      e->count_left -= n;
      if (e->count_left <= 0)
        {
          e->count_left = 0;
          gr_play (e);
        }
    }

  /* Fire events for every tick we pass in this sub-block. */
  if (e->playing)
    {
      int now = (int) e->pos;
      while (e->last_tick < now)
        fire_tick (e, ++e->last_tick);
    }
  preview_tick (e, spt);

  for (i = 0; i < NSYNTH; i++)
    {
      synth_voice *v = &e->sv[i];
      float gl = 0.5f, gr = 0.5f, gv = 0.15f;
      if (!v->active)
        continue;
      synth_render (v, tmp, n);
      e->busy_tail = GR_RATE * 2;   /* reverb tail after the last note */
      if (v->track >= 0)
        {
          gr_track *tr = &s->tracks[v->track];
          float peak = 0.0f;
          if (tr->mute || (solo && !tr->solo))
            continue;
          pan_gains (tr->vol, tr->pan, &gl, &gr);
          gv = tr->vol * tr->reverb;
          for (j = 0; j < n; j++)
            if (fabsf (tmp[j]) > peak) peak = fabsf (tmp[j]);
          if (peak > tr->meter) tr->meter = peak;
        }
      for (j = 0; j < n; j++)
        {
          l[j] += tmp[j] * gl;
          r[j] += tmp[j] * gr;
          rv[j] += tmp[j] * gv;
        }
    }
  for (i = 0; i < NDRUM; i++)
    {
      drum_voice *d = &e->dv[i];
      float gl = 0.5f, gr = 0.5f, gv = 0.05f, pan = 0.0f;
      if (!d->active)
        continue;
      drum_render (d, tmp, n);
      e->busy_tail = GR_RATE * 2;
      /* Spread the kit a little, as if heard from the drummer's stool. */
      if (d->sound == DR_CHAT || d->sound == DR_OHAT) pan = 0.25f;
      else if (d->sound == DR_HTOM) pan = -0.2f;
      else if (d->sound == DR_LTOM) pan = 0.2f;
      if (d->track >= 0)
        {
          gr_track *tr = &s->tracks[d->track];
          float peak = 0.0f;
          if (tr->mute || (solo && !tr->solo))
            continue;
          pan_gains (tr->vol, pan + tr->pan, &gl, &gr);
          gv = tr->vol * tr->reverb;
          for (j = 0; j < n; j++)
            if (fabsf (tmp[j]) > peak) peak = fabsf (tmp[j]);
          if (peak > tr->meter) tr->meter = peak;
        }
      for (j = 0; j < n; j++)
        {
          l[j] += tmp[j] * gl;
          r[j] += tmp[j] * gr;
          rv[j] += tmp[j] * gv;
        }
    }
  if (e->playing)
    audio_regions (e, l, r, rv, n, solo);
  if (gm_synth () && e->gm_tail > 0)
    gm_render (e, l, r, rv, n);

  /* Metronome */
  if (e->click_len > 0)
    for (j = 0; j < n && e->click_len > 0; j++, e->click_len--)
      {
        float c = sinf (TWO_PI * e->click_f * e->click_t) * expf (-e->click_t * 120.0f) * 0.35f;
        l[j] += c;
        r[j] += c;
        e->click_t += 1.0f / GR_RATE;
      }

  /* Reverb send, then master and a soft limiter. */
  for (j = 0; j < n; j++)
    {
      float in = rv[j] * 0.3f, ol = 0.0f, orr = 0.0f, x;
      int k;
      for (k = 0; k < 4; k++)
        {
          ol += comb_run (&e->cl[k], in);
          orr += comb_run (&e->cr[k], in);
        }
      for (k = 0; k < 2; k++)
        {
          ol = ap_run (&e->al[k], ol);
          orr = ap_run (&e->ar[k], orr);
        }
      x = (l[j] + ol) * s->master;
      l[j] = x > 0.9f ? 0.9f + 0.1f * tanhf ((x - 0.9f) * 10.0f) : x < -0.9f ? -0.9f + 0.1f * tanhf ((x + 0.9f) * 10.0f) : x;
      x = (r[j] + orr) * s->master;
      r[j] = x > 0.9f ? 0.9f + 0.1f * tanhf ((x - 0.9f) * 10.0f) : x < -0.9f ? -0.9f + 0.1f * tanhf ((x + 0.9f) * 10.0f) : x;
    }

  /* Advance time. */
  if (e->pv)
    e->pv_pos += n / spt;
  if (e->playing)
    {
      e->pos += n / spt;
      if (e->recording)
        rec_grow (e);
      if (s->loop_on && s->loop_end > s->loop_start && e->pos >= s->loop_end && !e->recording)
        {
          e->pos = s->loop_start + (e->pos - s->loop_end);
          e->last_tick = s->loop_start - 1;
        }
      else if (!e->recording && e->pos >= (double) gr_bar_tick (s, s->bars))
        {
          e->playing = 0;
          all_notes_off (e);
          e->pos = 0;
          e->busy_tail = GR_RATE * 2;
        }
    }
  if (e->busy_tail > 0)
    e->busy_tail -= n;
  if (e->mo_noffs)
    mo_tick (e, n);
  e->frames += (unsigned) n;
}

void gr_render (gr_engine *e, float *l, float *r, int n)
{
  int t;
  /* Meters fall at about 20 dB a second, measured per call. */
  for (t = 0; t < e->song->ntracks; t++)
    e->song->tracks[t].meter *= 0.9f;
  while (n > 0)
    {
      int k = n < SUB ? n : SUB;
      render_sub (e, l, r, k);
      l += k; r += k; n -= k;
    }
}

static inline int32_t to16 (float x)
{
  int32_t v = (int32_t) lrintf (x * 32767.0f);
  return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
}

void gr_render_words (gr_engine *e, uint32_t *out, int n)
{
  float l[256], r[256];
  int i;
  while (n > 0)
    {
      int k = n < 256 ? n : 256;
      gr_render (e, l, r, k);
      for (i = 0; i < k; i++)
        out[i] = ((uint32_t) to16 (l[i]) << 16) | ((uint32_t) to16 (r[i]) & 0xFFFFu);
      out += k; n -= k;
    }
}

/* ---------------------------------------------------------------------- */
/* Bounce                                                                 */

int gr_bounce (gr_song *s, const char *path)
{
  gr_engine *e = gr_engine_new (s);
  int16_t *pcm;
  int frames, i, rc, loop = s->loop_on, bars = s->bars;
  float l[256], r[256];
  if (!e)
    return -1;
  /* Render to the end of the last region, then two seconds of tail. */
  if (gr_song_end (s) > 0)
    s->bars = gr_tick_bar (s, gr_song_end (s) - 1) + 1;
  frames = (int) (gr_bar_tick (s, s->bars) * samples_per_tick (s)) + 2 * GR_RATE;
  pcm = malloc ((size_t) frames * 2 * sizeof (int16_t));
  if (!pcm)
    {
      gr_engine_free (e);
      s->bars = bars;
      return -1;
    }
  s->loop_on = 0;
  gm_restart ();                /* the SoundFont is shared with the live engine */
  gr_play (e);
  for (i = 0; i < frames; i += 256)
    {
      int k = frames - i < 256 ? frames - i : 256, j;
      gr_render (e, l, r, k);
      for (j = 0; j < k; j++)
        {
          pcm[2 * (i + j)] = (int16_t) to16 (l[j]);
          pcm[2 * (i + j) + 1] = (int16_t) to16 (r[j]);
        }
    }
  s->loop_on = loop;
  s->bars = bars;
  gm_restart ();
  rc = gr_wav_write (path, pcm, frames, GR_RATE);
  free (pcm);
  gr_engine_free (e);
  return rc;
}

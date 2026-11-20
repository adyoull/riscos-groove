/* Groove engine tests, run on Linux (make test).
   Each CHECK prints what it checked; any failure makes the run exit 1.
   With an argument, also bounces the demo song to that WAV file. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../engine/groove.h"
#include "../engine/unzip.h"
#include "../engine/xml.h"
#include "../engine/notation.h"
#include "../engine/engrave.h"
#include "../engine/glyphs.h"

int gr_engraving_svg (const gr_engraving *e, const char *path, double px_per_sp, int highlight_bar);

static int failures, checks;
#define CHECK(c, what) do { checks++; if (!(c)) { failures++; printf ("FAIL %s (%s:%d)\n", what, __FILE__, __LINE__); } } while (0)

static int find_loop (const char *name)
{
  int i;
  for (i = 0; i < gr_loop_count (); i++)
    if (!strcmp (gr_loop_get (i)->name, name))
      return i;
  printf ("no loop %s\n", name);
  exit (1);
}

static int demo_tempo (void) { return 96; }

/* The demo: 8 bars of drums, chords and bass, with a hook from bar 5. */
static void demo_song (gr_song *s)
{
  int r, t;
  gr_song_init (s);
  s->tempo = 96;
  s->key = 0;
  r = gr_loop_place (s, find_loop ("Boom Bap"), -1, 0);
  t = s->regions[r].track;
  s->regions[r].len = 8 * GR_BAR;            /* drag the region out: it loops */
  r = gr_loop_place (s, find_loop ("Pop Chords"), -1, 0);
  s->regions[r].len = 8 * GR_BAR;
  r = gr_loop_place (s, find_loop ("Root Pump"), -1, 0);
  s->regions[r].len = 8 * GR_BAR;
  r = gr_loop_place (s, find_loop ("Hook Melody"), -1, 4 * GR_BAR);
  s->regions[r].len = 4 * GR_BAR;
  r = gr_loop_place (s, find_loop ("Tom Fill"), t, 7 * GR_BAR);
  s->bars = 8;
}

static void test_loops (void)
{
  gr_song s;
  int i, k;
  gr_song_init (&s);
  for (i = 0; i < gr_loop_count (); i++)
    {
      const gr_loop_info *li = gr_loop_get (i);
      int p = gr_loop_make (&s, i);
      char what[80];
      snprintf (what, sizeof what, "loop %s makes a pattern with notes", li->name);
      CHECK (p >= 0 && s.patterns[p].nnotes > 0, what);
      if (p < 0)
        continue;
      snprintf (what, sizeof what, "loop %s is %d bars", li->name, li->bars);
      CHECK (s.patterns[p].len == li->bars * GR_BAR, what);
      for (k = 1; k < s.patterns[p].nnotes; k++)
        if (s.patterns[p].notes[k].tick < s.patterns[p].notes[k - 1].tick)
          break;
      snprintf (what, sizeof what, "loop %s notes sorted, in range", li->name);
      CHECK (k == s.patterns[p].nnotes, what);
      s.patterns[p].used = 0;
    }
  /* Placing: snaps to the bar, makes the right kind of track. */
  gr_song_init (&s);
  i = gr_loop_place (&s, find_loop ("Pop Chords"), -1, GR_BAR + 10);
  CHECK (i >= 0 && s.regions[i].start == GR_BAR, "dropped loop snaps to the nearest bar");
  CHECK (s.ntracks == 1 && s.tracks[0].type == TRK_INST && s.tracks[0].patch == PATCH_EPIANO,
         "dropping a keys loop on empty space makes an Electric Piano track");
  i = gr_loop_place (&s, find_loop ("Boom Bap"), 0, 0);
  CHECK (s.ntracks == 2 && s.tracks[1].type == TRK_DRUMS,
         "a drum loop dropped on a keys track gets its own drum track");
  /* Key: a song in D moves a loop up two semitones. */
  gr_song_init (&s);
  k = gr_loop_make (&s, find_loop ("Root Pump"));
  s.key = 2;
  i = gr_loop_make (&s, find_loop ("Root Pump"));
  CHECK (s.patterns[i].notes[0].pitch == s.patterns[k].notes[0].pitch + 2, "loops follow the song key");
}

static void test_render (void)
{
  static gr_song s;
  gr_engine *e;
  float l[512], r[512], peak = 0, sum = 0, meters[GR_MAX_TRACKS] = { 0 };
  int i, j, n = 0, bad = 0, t, limited = 0;
  demo_song (&s);
  e = gr_engine_new (&s);
  gr_play (e);
  for (i = 0; i < GR_RATE * 21 / 512; i++)   /* 8 bars at 96 BPM is 20 s */
    {
      gr_render (e, l, r, 512);
      for (t = 0; t < s.ntracks; t++)
        if (s.tracks[t].meter > meters[t])
          meters[t] = s.tracks[t].meter;
      for (j = 0; j < 512; j++)
        {
          if (!isfinite (l[j]) || !isfinite (r[j]))
            bad++;
          if (fabsf (l[j]) > peak) peak = fabsf (l[j]);
          if (fabsf (r[j]) > peak) peak = fabsf (r[j]);
          limited += (fabsf (l[j]) > 0.9f) + (fabsf (r[j]) > 0.9f);
          sum += l[j] * l[j] + r[j] * r[j];
          n += 2;
        }
    }
  printf ("  demo: peak %.3f, rms %.3f, limiter active %.2f%% of samples\n", peak, sqrtf (sum / n), 100.0 * limited / n);
  for (t = 0; t < s.ntracks; t++)
    printf ("  track %d %-16s peak %.2f\n", t, s.tracks[t].name, meters[t]);
  CHECK (bad == 0, "no NaN or infinity in the output");
  CHECK (peak <= 1.0f, "the limiter keeps the output within full scale");
  CHECK (limited < n / 1000, "the default mix leaves headroom (limiter on under 0.1% of samples)");
  CHECK (sqrtf (sum / n) > 0.05f, "the demo is audible (rms above -26 dBFS)");
  for (t = 0; t < s.ntracks; t++)
    {
      char what[64];
      snprintf (what, sizeof what, "track %d (%s) made sound", t, s.tracks[t].name);
      CHECK (meters[t] > 0.01f, what);
    }
  /* Stop releases the notes; then everything goes quiet. */
  gr_stop (e);
  for (i = 0; i < GR_RATE * 6 / 512; i++)
    gr_render (e, l, r, 512);
  CHECK (!gr_busy (e), "after stop, the engine goes idle (no stuck notes)");
  gr_stop (e);
  CHECK (gr_position (e) == 0.0, "a second stop rewinds");

  /* Mute: a muted drum track and nothing else is silent. */
  for (t = 0; t < s.ntracks; t++)
    s.tracks[t].mute = 1;
  gr_locate (e, 0);
  gr_play (e);
  peak = 0;
  for (i = 0; i < GR_RATE * 2 / 512; i++)
    {
      gr_render (e, l, r, 512);
      for (j = 0; j < 512; j++)
        if (fabsf (l[j]) > peak) peak = fabsf (l[j]);
    }
  CHECK (peak < 0.02f, "all tracks muted is silent");
  gr_engine_free (e);
}

static void test_loop_playback (void)
{
  static gr_song s;
  gr_engine *e;
  float l[256], r[256];
  int i, wrapped = 0;
  double last = 0;
  demo_song (&s);
  s.loop_on = 1;
  s.loop_start = 2 * GR_BAR;
  s.loop_end = 3 * GR_BAR;
  e = gr_engine_new (&s);
  gr_locate (e, 2 * GR_BAR);
  gr_play (e);
  for (i = 0; i < GR_RATE * 8 / 256; i++)
    {
      gr_render (e, l, r, 256);
      if (gr_position (e) < last)
        wrapped++;
      last = gr_position (e);
      if (last < 2 * GR_BAR || last >= 3 * GR_BAR)
        break;
    }
  CHECK (i == GR_RATE * 8 / 256, "the playhead stays inside the cycle region");
  CHECK (wrapped >= 2, "the cycle region repeats");
  gr_engine_free (e);
}

static void test_words (void)
{
  static gr_song s;
  gr_engine *e;
  uint32_t w[64];
  int t, i, lr_ok = 1;
  gr_song_init (&s);
  t = gr_track_add (&s, TRK_INST, "Lead", PATCH_ORGAN);
  s.tracks[t].pan = -1.0f;                       /* hard left */
  s.tracks[t].reverb = 0.0f;
  e = gr_engine_new (&s);
  gr_live_on (e, t, 60, 127);
  for (i = 0; i < 20; i++)
    gr_render_words (e, w, 64);
  for (i = 0; i < 64; i++)
    if ((int16_t) (w[i] & 0xFFFF) != 0)
      lr_ok = 0;
  CHECK (lr_ok, "hard-left sound has silence in the low (right) half of each word");
  for (i = 0, lr_ok = 0; i < 64; i++)
    if ((int16_t) (w[i] >> 16) != 0)
      lr_ok = 1;
  CHECK (lr_ok, "and sound in the high (left) half");
  gr_live_off (e, t, 60);
  gr_engine_free (e);
}

static void test_record (void)
{
  static gr_song s;
  gr_engine *e;
  float l[256], r[256];
  int t, d, i, reg = -1;
  double spt;
  gr_song_init (&s);
  s.tempo = 120;
  t = gr_track_add (&s, TRK_INST, "Keys", PATCH_EPIANO);
  d = gr_track_add (&s, TRK_DRUMS, "Drums", KIT_STUDIO);
  s.tracks[t].armed = 1;
  s.tracks[d].armed = 1;
  e = gr_engine_new (&s);
  spt = GR_RATE * 60.0 / (120.0 * GR_PPQ);
  gr_record (e);
  /* Play C E G a little late on beats 1, 2, 3, and a kick on beat 1. */
  for (i = 0; i < (int) (4 * GR_PPQ * spt / 256); i++)
    {
      double pos = gr_position (e);
      int beat = (int) (pos / GR_PPQ), within = (int) pos % GR_PPQ;
      if (within >= 5 && within < 5 + (int) (256 / spt) + 1 && beat < 3)
        {
          gr_live_on (e, t, 60 + 4 * beat - (beat == 2), 100);
          if (beat == 0)
            gr_live_on (e, d, DR_KICK, 110);
        }
      if (within >= 60 && within < 60 + (int) (256 / spt) + 1 && beat < 3)
        gr_live_off (e, t, 60 + 4 * beat - (beat == 2));
      gr_render (e, l, r, 256);
    }
  gr_stop (e);
  for (i = 0; i < GR_MAX_REGIONS; i++)
    if (s.regions[i].used && s.regions[i].track == t)
      reg = i;
  CHECK (reg >= 0, "recording made a region on the armed keys track");
  if (reg >= 0)
    {
      gr_pattern *p = &s.patterns[s.regions[reg].pattern];
      CHECK (p->nnotes == 3, "it holds the three notes played");
      CHECK (p->nnotes == 3 && p->notes[0].tick == 0 && p->notes[1].tick == GR_PPQ
             && p->notes[2].tick == 2 * GR_PPQ, "late notes were quantized onto the beat");
      CHECK (p->nnotes == 3 && p->notes[0].pitch == 60 && p->notes[1].pitch == 64
             && p->notes[2].pitch == 67, "pitches recorded (C E G)");
    }
  for (i = 0, reg = -1; i < GR_MAX_REGIONS; i++)
    if (s.regions[i].used && s.regions[i].track == d)
      reg = i;
  CHECK (reg >= 0 && s.patterns[s.regions[reg].pattern].nnotes == 1, "the kick was recorded on the drum track");
  gr_engine_free (e);
}

static void test_audio_record (void)
{
  static gr_song s;
  gr_engine *e;
  float l[256], r[256];
  int16_t in[512];
  int t, i, j, reg = -1;
  gr_song_init (&s);
  t = gr_track_add (&s, TRK_AUDIO, "Voice", 0);
  s.tracks[t].armed = 1;
  e = gr_engine_new (&s);
  gr_record (e);
  for (i = 0; i < GR_RATE / 256; i++)          /* one second of a 440 Hz tone */
    {
      for (j = 0; j < 256; j++)
        in[2 * j] = in[2 * j + 1] = (int16_t) (8000 * sinf (6.2831853f * 440.0f * (i * 256 + j) / GR_RATE));
      gr_audio_in (e, in, 256);
      gr_render (e, l, r, 256);
    }
  gr_stop (e);
  for (i = 0; i < GR_MAX_REGIONS; i++)
    if (s.regions[i].used && s.regions[i].track == t)
      reg = i;
  CHECK (reg >= 0 && s.regions[reg].clip >= 0, "audio recording made a clip region");
  if (reg >= 0)
    CHECK (s.clips[s.regions[reg].clip].frames == (GR_RATE / 256) * 256, "the clip holds every frame recorded");
  /* Play it back. */
  gr_locate (e, 0);
  gr_play (e);
  {
    float peak = 0;
    for (i = 0; i < 40; i++)
      {
        gr_render (e, l, r, 256);
        for (j = 0; j < 256; j++)
          if (fabsf (l[j]) > peak) peak = fabsf (l[j]);
      }
    CHECK (peak > 0.1f, "the recorded clip plays back");
  }
  gr_engine_free (e);
  gr_song_free (&s);
}

static void test_files (void)
{
  static gr_song a, b;
  int i, same = 1;
  demo_song (&a);
  CHECK (gr_song_save (&a, "/tmp/groove_test_song") == 0, "song saves");
  CHECK (gr_song_load (&b, "/tmp/groove_test_song") == 0, "song loads");
  CHECK (a.tempo == b.tempo && a.ntracks == b.ntracks && a.bars == b.bars, "tempo, tracks, length round-trip");
  for (i = 0; i < GR_MAX_REGIONS; i++)
    if (a.regions[i].used != b.regions[i].used
        || (a.regions[i].used && (a.regions[i].start != b.regions[i].start
                                  || a.regions[i].len != b.regions[i].len
                                  || a.patterns[a.regions[i].pattern].nnotes
                                     != b.patterns[b.regions[i].pattern].nnotes)))
      same = 0;
  CHECK (same, "regions and their notes round-trip");
  for (i = 0; i < a.ntracks; i++)
    if (strcmp (a.tracks[i].name, b.tracks[i].name) || a.tracks[i].patch != b.tracks[i].patch)
      same = 0;
  CHECK (same, "track names and instruments round-trip");
  CHECK (gr_song_load (&b, "/dev/null") != 0, "a file that isn't a song is refused");
}

static void test_bounce (const char *path)
{
  static gr_song s;
  int16_t *pcm;
  int frames, rate;
  clock_t c0;
  double secs;
  demo_song (&s);
  c0 = clock ();
  CHECK (gr_bounce (&s, path) == 0, "bounce writes a WAV");
  secs = (double) (clock () - c0) / CLOCKS_PER_SEC;
  CHECK (gr_wav_read (path, &pcm, &frames, &rate) == 0 && rate == GR_RATE, "the bounced WAV reads back");
  if (pcm)
    {
      double len = (double) frames / rate;
      printf ("  bounce: %.1f s of audio rendered in %.2f s CPU (%.0fx real time on this host)\n",
              len, secs, len / secs);
      CHECK (len > 8 * 4 * 60.0 / 96, "it is as long as the song plus the tail");
      free (pcm);
    }
  /* Import it back as an audio track. */
  CHECK (gr_wav_import (&s, path, -1, 0) >= 0 && s.tracks[s.ntracks - 1].type == TRK_AUDIO,
         "a WAV imports as an audio track");
  gr_song_free (&s);
}


/* ---- SoundFont sounds and MIDI files ------------------------------------------ */

static float render_peak (gr_engine *e, int frames)
{
  float l[256], r[256], peak = 0;
  int i, j;
  for (i = 0; i < frames; i += 256)
    {
      gr_render (e, l, r, 256);
      for (j = 0; j < 256; j++)
        {
          if (fabsf (l[j]) > peak) peak = fabsf (l[j]);
          if (fabsf (r[j]) > peak) peak = fabsf (r[j]);
        }
    }
  return peak;
}

/* A one-bar song: a piano chord (or whatever track 0's sound is) and a
   kick and snare. */
static void gm_song (gr_song *s, int piano_gm, int kit_gm)
{
  int p, t;
  gr_song_init (s);
  s->tempo = 120;
  s->bars = 1;
  t = gr_track_add (s, TRK_INST, "Piano", PATCH_EPIANO);
  s->tracks[t].gm = piano_gm;
  p = gr_pattern_new (s, "Chord", GR_BAR);
  gr_pattern_add_note (&s->patterns[p], 0, GR_PPQ * 2, 60, 100);
  gr_pattern_add_note (&s->patterns[p], 0, GR_PPQ * 2, 64, 100);
  gr_pattern_add_note (&s->patterns[p], 0, GR_PPQ * 2, 67, 100);
  gr_region_add (s, t, 0, GR_BAR, p);
  t = gr_track_add (s, TRK_DRUMS, "Drums", KIT_STUDIO);
  s->tracks[t].gm = kit_gm;
  p = gr_pattern_new (s, "Beat", GR_BAR);
  gr_pattern_add_note (&s->patterns[p], 0, GR_STEP, DR_KICK, 120);
  gr_pattern_add_note (&s->patterns[p], GR_PPQ, GR_STEP, DR_SNARE, 120);
  gr_region_add (s, t, 0, GR_BAR, p);
}

static void test_gm_names (void)
{
  static gr_song s;
  CHECK (!strcmp (gr_gm_program_name (0), "Acoustic Grand Piano")
         && !strcmp (gr_gm_program_name (127), "Gunshot"), "the General MIDI sound names");
  CHECK (!strcmp (gr_gm_family_name (4), "Bass") && !strcmp (gr_gm_family_name (13), "World"),
         "and their families (GM's \"Ethnic\" called World)");
  CHECK (gr_gm_kit (4) == GM_KIT_808 && !strcmp (gr_gm_kit_name (GM_KIT_808), "808 Kit"), "kit names");
  CHECK (gr_gm_drum_note (GV_KICK) == DR_KICK && DR_KICK == 36 && gr_gm_drum_note (GV_SNARE) == 38
         && gr_gm_drum_note (GV_CHAT) == 42, "Groove's drums are GM drum keys");
  CHECK (gr_drum_voice (36) == GV_KICK && gr_drum_voice (40) == GV_SNARE && gr_drum_voice (49) == GV_PERC,
         "any drum key plays one of Groove's own drums");
  CHECK (!strcmp (gr_drum_name (DR_KICK), "Kick") && !strcmp (gr_drum_name (49), "Crash Cymbal 1")
         && !strcmp (gr_drum_name (90), "F#6 90"), "every drum key has a name");
  gm_song (&s, 0, -1);
  CHECK (!strcmp (gr_sound_name (&s.tracks[0]), "Acoustic Grand Piano")
         && !strcmp (gr_sound_name (&s.tracks[1]), "Studio Kit"), "a track's sound name, either kind");
  CHECK (gr_song_uses_gm (&s), "a song with a SoundFont track says so");
  s.tracks[0].gm = -1;
  CHECK (!gr_song_uses_gm (&s), "and one without doesn't");
}

static void test_gm_files (void)
{
  static gr_song a, b;
  FILE *f;
  gm_song (&a, 40, GM_KIT_JAZZ);
  CHECK (gr_song_save (&a, "/tmp/groove_gm.song") == 0 && gr_song_load (&b, "/tmp/groove_gm.song") == 0
         && b.tracks[0].gm == 40 && b.tracks[1].gm == GM_KIT_JAZZ, "SoundFont sounds are saved and loaded");
  /* A song from before SoundFont sounds. */
  f = fopen ("/tmp/groove_old.song", "w");
  fputs ("GROOVE 1\ntempo 100\ntrack 1 2 \"Pad\" 0.80 0.00 0.18 0 0\n", f);
  fclose (f);
  CHECK (gr_song_load (&b, "/tmp/groove_old.song") == 0 && b.ntracks == 1 && b.tracks[0].gm == -1
         && b.tracks[0].patch == 2, "an older song loads with Groove's own sounds");
  gr_song_free (&a);
  gr_song_free (&b);
}

/* Without a SoundFont, SoundFont tracks play Groove's own sounds. */
static void test_gm_fallback (void)
{
  static gr_song s;
  gr_engine *e;
  gr_gm_unload ();
  CHECK (gr_gm_load ("/nonexistent/none.sf2") < 0 && !gr_gm_ready () && gr_gm_error ()[0],
         "a missing SoundFont fails with a reason");
  gm_song (&s, 0, 0);
  e = gr_engine_new (&s);
  gr_play (e);
  CHECK (render_peak (e, GR_RATE / 2) > 0.05f, "with no SoundFont, its tracks play Groove's sounds");
  gr_engine_free (e);
}

static void test_gm_play (const char *sf)
{
  static gr_song s;
  gr_engine *e;
  float groove_peak, gm_peak, muted, peak;
  clock_t c0;
  double secs;
  int i;
  c0 = clock ();
  if (gr_gm_load (sf) < 0)
    {
      printf ("FAIL couldn't load %s: %s\n", sf, gr_gm_error ());
      failures++;
      return;
    }
  printf ("  loaded %s in %.2f s\n", sf, (double) (clock () - c0) / CLOCKS_PER_SEC);
  CHECK (gr_gm_ready (), "a SoundFont loads");

  /* The same chord with Groove's piano and the SoundFont's: about as loud. */
  gm_song (&s, -1, -1);
  s.tracks[1].mute = 1;
  e = gr_engine_new (&s);
  gr_play (e);
  groove_peak = render_peak (e, GR_RATE);
  gr_engine_free (e);
  gm_song (&s, 0, 0);
  s.tracks[1].mute = 1;
  e = gr_engine_new (&s);
  gr_play (e);
  gm_peak = render_peak (e, GR_RATE);
  printf ("  chord peak: Groove's piano %.2f, the SoundFont's %.2f\n", groove_peak, gm_peak);
  CHECK (gm_peak > 0.1f, "a SoundFont piano track sounds");
  CHECK (gm_peak > groove_peak * 0.4f && gm_peak < groove_peak * 2.5f,
         "about as loud as Groove's own piano");
  CHECK (s.tracks[0].meter > 0.0f, "its meter moves");
  gr_engine_free (e);

  /* The sustain pedal: a chord struck again and again with the pedal down
     doesn't pile up voices (each strike lets the last one go), and when
     the sound can't be made fast enough the voices can be thinned. */
  {
    int p, t, k, before, n2;
    gr_song_init (&s);
    s.tempo = 120;
    s.bars = 2;
    t = gr_track_add (&s, TRK_INST, "Organ", PATCH_EPIANO);
    s.tracks[t].gm = 16;          /* (an organ: it sounds for as long as it's held) */
    p = gr_pattern_new (&s, "Pedalled", 2 * GR_BAR);
    gr_pattern_set_ctl (&s.patterns[p], 0, 64, 127);
    for (k = 0; k < 32; k++)
      {
        gr_pattern_add_note (&s.patterns[p], k * GR_PPQ / 2, GR_PPQ / 3, 60, 100);
        gr_pattern_add_note (&s.patterns[p], k * GR_PPQ / 2, GR_PPQ / 3, 64, 100);
        gr_pattern_add_note (&s.patterns[p], k * GR_PPQ / 2, GR_PPQ / 3, 67, 100);
        gr_pattern_add_note (&s.patterns[p], k * GR_PPQ / 2, GR_PPQ / 3, 72, 100);
      }
    gr_region_add (&s, t, 0, 2 * GR_BAR, p);
    e = gr_engine_new (&s);
    gr_play (e);
    render_peak (e, GR_RATE * 3 + GR_RATE / 20);   /* (just after a strike) */
    before = gr_gm_voices ();
    printf ("  a chord struck 24 times with the pedal down: %d voices sounding\n", before);
    CHECK (before > 0 && before <= 24, "a chord struck again and again under the pedal doesn't pile up voices");
    gr_gm_shed (50);
    render_peak (e, GR_RATE / 100);                /* (before the next) */
    n2 = gr_gm_voices ();
    CHECK (n2 <= before / 2 + 1, "thinning the voices when it can't keep up");
    gr_engine_free (e);
    gr_song_free (&s);
  }

  /* Mute and solo. */
  gm_song (&s, 0, 0);
  s.tracks[0].mute = 1;
  s.tracks[1].mute = 1;
  e = gr_engine_new (&s);
  gr_play (e);
  muted = render_peak (e, GR_RATE);
  CHECK (muted < 0.01f, "muted SoundFont tracks are silent");
  gr_stop (e);
  gr_stop (e);
  s.tracks[1].mute = 0;
  s.tracks[0].mute = 0;
  s.tracks[0].solo = 1;
  gr_play (e);
  CHECK (render_peak (e, GR_RATE) > 0.1f, "solo: the soloed SoundFont track plays");
  gr_engine_free (e);

  /* Drums only, from the kit on channel 10. */
  gm_song (&s, 0, GM_KIT_STANDARD);
  s.tracks[0].mute = 1;
  e = gr_engine_new (&s);
  gr_play (e);
  peak = render_peak (e, GR_RATE / 4);
  CHECK (peak > 0.1f, "a SoundFont drum track sounds");
  gr_engine_free (e);

  /* Notes end, the tail ends, and the engine goes idle. */
  gm_song (&s, 0, 0);
  e = gr_engine_new (&s);
  gr_play (e);
  render_peak (e, GR_RATE * 2);                  /* the song ends after a bar */
  for (i = 0; i < 20 && gr_busy (e); i++)
    render_peak (e, GR_RATE / 2);
  CHECK (!gr_busy (e), "the engine goes idle after the SoundFont's notes end");
  CHECK (render_peak (e, 4096) < 1e-4f, "and is silent");

  /* Musical typing on a SoundFont track: held until let go. */
  gr_live_on (e, 0, 72, 100);
  render_peak (e, 4096);
  peak = render_peak (e, GR_RATE);
  CHECK (peak > 0.05f, "a held note from the keyboard keeps sounding");
  gr_live_off (e, 0, 72);
  render_peak (e, GR_RATE * 2);
  CHECK (render_peak (e, 4096) < 0.003f, "and stops when the key comes up");

  /* Stop in the middle of a long note: silent soon after. */
  s.patterns[s.regions[0].pattern].notes[0].len = GR_BAR;
  gr_locate (e, 0);
  gr_play (e);
  render_peak (e, GR_RATE / 4);
  gr_stop (e);
  render_peak (e, GR_RATE * 2);
  CHECK (render_peak (e, 4096) < 0.003f, "stop ends SoundFont notes");
  gr_engine_free (e);

  /* Bounce, then the live engine still plays (the synth is shared). */
  gm_song (&s, 0, 0);
  e = gr_engine_new (&s);
  gr_live_on (e, 0, 60, 100);
  render_peak (e, 2048);
  CHECK (gr_bounce (&s, "/tmp/groove_gm.wav") == 0, "a song with SoundFont tracks bounces");
  {
    int16_t *pcm;
    int frames, rate, k, big = 0;
    if (gr_wav_read ("/tmp/groove_gm.wav", &pcm, &frames, &rate) == 0)
      {
        for (k = 0; k < frames / 2; k++)
          if (abs (pcm[2 * k]) > 3000)
            big++;
        free (pcm);
      }
    CHECK (big > 100, "and the bounce has the SoundFont in it");
  }
  gr_locate (e, 0);
  gr_play (e);
  CHECK (render_peak (e, GR_RATE / 2) > 0.1f, "the live engine plays after a bounce");
  gr_engine_free (e);

  /* What it costs: the demo with every track on the SoundFont. */
  demo_song (&s);
  for (i = 0; i < s.ntracks; i++)
    s.tracks[i].gm = s.tracks[i].type == TRK_DRUMS ? 0 : 4 * i;
  e = gr_engine_new (&s);
  gr_play (e);
  c0 = clock ();
  render_peak (e, GR_RATE * 8);
  secs = (double) (clock () - c0) / CLOCKS_PER_SEC;
  printf ("  demo on the SoundFont: 8 s of audio in %.2f s CPU (%.0fx real time, with ASan)\n",
          secs, 8.0 / (secs > 0 ? secs : 1e-3));
  gr_engine_free (e);
  gr_song_free (&s);
}

/* Bytes of a little MIDI file: format 1, 96 per beat, a tempo track, then
   a piano part with running status and a velocity-0 note-off, and drums. */
static const unsigned char test_mid[] = {
  'M','T','h','d', 0,0,0,6, 0,1, 0,3, 0,96,
  'M','T','r','k', 0,0,0,11, 0,0xFF,0x51,3, 0x07,0xA1,0x20,   /* 500000 us: 120 */
                   0,0xFF,0x2F,0,
  'M','T','r','k', 0,0,0,20, 0,0xC2,25,                   /* channel 3: Steel Guitar */
                   0,0x92,60,100, 0,64,90,                /* C and E, running status */
                   96,60,0, 0,64,0,                       /* a beat later, both off (vel 0) */
                   0,0xFF,0x2F,0,
  'M','T','r','k', 0,0,0,21, 0,0x99,36,120, 48,0x89,36,64,   /* kick at 0 */
                   0x81,0x10,0x99,38,110, 48,0x89,38,0,   /* snare at 192 */
                   0,0xFF,0x2F,0,
};

static void test_midi (void)
{
  static gr_song s, b;
  FILE *f;
  const char *why;
  int n, t, i, notes = 0;
  f = fopen ("/tmp/groove_test.mid", "wb");
  fwrite (test_mid, 1, sizeof test_mid, f);
  fclose (f);
  gr_song_init (&s);
  n = gr_midi_import (&s, "/tmp/groove_test.mid", 0, &why);
  if (why) printf ("  import said: %s\n", why);
  CHECK (n == 2 && s.ntracks == 2, "a MIDI file imports as a track per channel");
  CHECK (s.tempo == 120, "with the file's tempo");
  t = s.tracks[0].type == TRK_INST ? 0 : 1;
  CHECK (s.tracks[t].gm == 25 && !strcmp (s.tracks[t].name, "Steel Guitar")
         && s.tracks[t].patch == PATCH_PLUCK, "its program becomes the SoundFont sound (Groove's pluck if none)");
  CHECK (s.tracks[1 - t].type == TRK_DRUMS && s.tracks[1 - t].gm == GM_KIT_STANDARD,
         "channel 10 becomes a drum track");
  for (i = 0; i < GR_MAX_REGIONS; i++)
    if (s.regions[i].used && s.regions[i].track == t)
      {
        const gr_pattern *p = &s.patterns[s.regions[i].pattern];
        notes += p->nnotes;
        CHECK (s.regions[i].start == 0 && s.regions[i].len == 4 * GR_BAR, "in 4-bar regions");
        CHECK (p->nnotes == 2 && p->notes[0].len == GR_PPQ && p->notes[0].vel >= 90,
               "notes keep their timing, length and velocity");
      }
  CHECK (notes == 2, "running status and velocity-0 note-offs are understood");
  for (i = 0; i < GR_MAX_REGIONS; i++)
    if (s.regions[i].used && s.regions[i].track == 1 - t)
      {
        const gr_pattern *p = &s.patterns[s.regions[i].pattern];
        CHECK (p->nnotes == 2 && p->notes[0].pitch == DR_KICK && p->notes[1].pitch == DR_SNARE
               && p->notes[1].tick == 2 * GR_PPQ, "drum keys land on the step grid's rows");
      }

  /* Export, then import what was exported: the same notes. */
  demo_song (&s);
  CHECK (gr_midi_export (&s, "/tmp/groove_demo.mid") == 0, "the demo exports as a MIDI file");
  gr_song_init (&b);
  n = gr_midi_import (&b, "/tmp/groove_demo.mid", 0, &why);
  if (why) printf ("  import said: %s\n", why);
  CHECK (n == s.ntracks && b.tempo == s.tempo, "and imports again: a track each, same tempo");
  {
    /* Count note starts per track, each region's pattern looped over it.
       Two drum regions can strike the same drum at once (the demo's fill
       over its beat); a MIDI file has that once, so count it once. */
    static unsigned char seen[2][GR_DRUM_SOUNDS][16 * GR_BAR];
    int ta, want[GR_MAX_TRACKS] = { 0 }, got[GR_MAX_TRACKS] = { 0 }, ok = 1;
    const gr_song *ss[2] = { &s, &b };
    int *cnt[2] = { want, got }, k;
    memset (seen, 0, sizeof seen);
    for (k = 0; k < 2; k++)
      for (i = 0; i < GR_MAX_REGIONS; i++)
        {
          const gr_region *r = &ss[k]->regions[i];
          const gr_pattern *p;
          int rep, j, drums;
          if (!r->used || r->pattern < 0)
            continue;
          p = &ss[k]->patterns[r->pattern];
          drums = ss[k]->tracks[r->track].type == TRK_DRUMS;
          for (rep = 0; rep * p->len < r->len; rep++)
            for (j = 0; j < p->nnotes; j++)
              {
                int at = r->start + rep * p->len + p->notes[j].tick;
                if (rep * p->len + p->notes[j].tick >= r->len)
                  continue;
                if (drums)
                  {
                    unsigned char *sn = &seen[k][p->notes[j].pitch % GR_DRUM_SOUNDS][at % (16 * GR_BAR)];
                    if (*sn)
                      continue;
                    *sn = 1;
                  }
                cnt[k][drums ? 7 : r->track]++;
              }
        }
    /* Imported tracks come in MIDI channel order: the instrument tracks
       in their order, then the drums (channel 10). */
    {
      int tb = 0;
      for (ta = 0; ta < s.ntracks; ta++)
        if (s.tracks[ta].type != TRK_DRUMS)
          {
            while (tb < b.ntracks && b.tracks[tb].type == TRK_DRUMS)
              tb++;
            if (tb >= b.ntracks || want[ta] != got[tb]) ok = 0;
            printf ("  notes: %s %d -> %s %d\n", s.tracks[ta].name, want[ta],
                    tb < b.ntracks ? b.tracks[tb].name : "-", tb < b.ntracks ? got[tb] : 0);
            tb++;
          }
    }
    if (want[7] != got[7]) ok = 0;
    printf ("  notes: drums %d -> %d\n", want[7], got[7]);
    CHECK (ok, "every note survives the round trip");
  }
  /* Not MIDI files. */
  CHECK (gr_midi_import (&b, "/tmp/groove_gm.song", 0, &why) < 0 && why, "a song file isn't a MIDI file");
  CHECK (gr_midi_import (&b, "/nonexistent.mid", 0, &why) < 0 && why, "a missing file is reported");
  f = fopen ("/tmp/groove_cut.mid", "wb");
  fwrite (test_mid, 1, 40, f);
  fclose (f);
  gr_song_init (&b);
  gr_midi_import (&b, "/tmp/groove_cut.mid", 0, &why);
  CHECK (1, "a truncated MIDI file doesn't crash");

  /* A long, busy file fills the song and says so. */
  {
    FILE *g = fopen ("/tmp/groove_long.mid", "wb");
    unsigned char hdr[22] = { 'M','T','h','d',0,0,0,6,0,0,0,1,0,96,'M','T','r','k',0,0,0,0 };
    long len = 0;
    int bar;
    fwrite (hdr, 1, 22, g);
    for (bar = 0; bar < 400; bar++)
      for (i = 0; i < 16; i++)
        {
          int ch;
          for (ch = 0; ch < 12; ch++)
            {
              unsigned char ev[8] = { 0, (unsigned char) (0x90 | ch), 60, 100, 24, (unsigned char) (0x80 | ch), 60, 0 };
              fwrite (ev, 1, 8, g);
              len += 8;
            }
        }
    fwrite ("\0\xFF\x2F\0", 1, 4, g);
    len += 4;
    fseek (g, 18, SEEK_SET);
    fputc ((int) (len >> 24) & 255, g); fputc ((int) (len >> 16) & 255, g);
    fputc ((int) (len >> 8) & 255, g); fputc ((int) len & 255, g);
    fclose (g);
    gr_song_init (&b);
    n = gr_midi_import (&b, "/tmp/groove_long.mid", 0, &why);
    printf ("  long file: %d tracks, said: %s\n", n, why ? why : "(nothing)");
    CHECK (n >= 1 && why && strstr (why, "full"), "a file too long for the song fills it and says so");
  }
  gr_song_free (&s);
  gr_song_free (&b);
}

/* ---- Undo and Redo ----------------------------------------------------------- */
static void test_history (void)
{
  static gr_song s;
  gr_history *h = gr_history_new (100, 0);
  int r, p, i, n;
  demo_song (&s);
  gr_history_reset (h, &s);
  CHECK (!gr_history_can_undo (h) && !gr_history_can_redo (h) && gr_history_is_saved (h),
         "a fresh history: nothing to undo, and the song is as saved");
  CHECK (!gr_history_note (h, &s), "noting an unchanged song adds nothing");
  s.tracks[0].meter = 0.9f;
  s.tracks[0].armed = 1;
  CHECK (!gr_history_note (h, &s), "meters and arming aren't edits");
  s.tracks[0].armed = 0;
  s.tempo = 130;
  CHECK (gr_history_note (h, &s) && gr_history_can_undo (h) && !gr_history_is_saved (h),
         "a tempo change is remembered, and the song is no longer as saved");
  r = 0;
  for (i = 0; i < GR_MAX_REGIONS; i++)
    if (s.regions[i].used) { r = i; break; }
  p = s.regions[r].pattern;
  n = s.patterns[p].nnotes;
  gr_region_delete (&s, r);
  gr_history_note (h, &s);
  CHECK (!s.regions[r].used, "(a region deleted)");
  CHECK (gr_history_undo (h, &s) && s.regions[r].used && s.regions[r].pattern == p
         && s.patterns[p].used && s.patterns[p].nnotes == n && s.tempo == 130,
         "Undo brings the region and its notes back");
  CHECK (gr_history_undo (h, &s) && s.tempo == demo_tempo () && gr_history_is_saved (h),
         "Undo again: the tempo goes back, and the song is as saved again");
  CHECK (!gr_history_undo (h, &s), "no further back than the start");
  CHECK (gr_history_redo (h, &s) && s.tempo == 130 && gr_history_redo (h, &s) && !s.regions[r].used,
         "Redo does both again");
  CHECK (!gr_history_redo (h, &s), "and no further");
  gr_history_undo (h, &s);
  s.key = 5;
  gr_history_note (h, &s);
  CHECK (!gr_history_can_redo (h), "a new change after Undo drops what could be redone");
  gr_history_saved (h);
  CHECK (gr_history_is_saved (h), "saving marks the current state");

  /* Audio: the recording survives its region's deletion, for Undo. */
  {
    int16_t pcm[2000];
    int c, ra;
    for (i = 0; i < 2000; i++) pcm[i] = (int16_t) (i * 7);
    c = gr_clip_new (&s, "Take", GR_RATE);
    gr_clip_append (&s.clips[c], pcm, 1000);
    ra = gr_region_add (&s, 0, 0, GR_BAR, -1);
    s.regions[ra].clip = c;
    gr_history_note (h, &s);
    gr_region_delete (&s, ra);
    gr_history_note (h, &s);
    CHECK (!s.clips[c].used && s.clips[c].data, "a clip with no region left is unused, but its audio is kept");
    CHECK (gr_history_undo (h, &s) && s.regions[ra].used && s.clips[c].used
           && s.clips[c].frames == 1000 && s.clips[c].data[2 * 999 + 1] == (int16_t) (1999 * 7),
           "Undo brings back the audio region with its recording");
    gr_history_redo (h, &s);
    i = gr_clip_new (&s, "Another", GR_RATE);
    CHECK (i != c, "a new clip doesn't take a slot whose audio Undo may need, while there are others");
  }

  /* The levels are limited. */
  gr_history_free (h);
  h = gr_history_new (5, 0);
  gr_song_free (&s);
  demo_song (&s);
  gr_history_reset (h, &s);
  for (i = 0; i < 10; i++)
    {
      s.tempo = 100 + i;
      gr_history_note (h, &s);
    }
  for (n = 0; gr_history_undo (h, &s); n++)
    ;
  CHECK (n == 4 && s.tempo == 105, "only the last few levels are kept (5 here)");
  gr_history_free (h);
  gr_song_free (&s);
}

/* ---- splitting and copying regions --------------------------------------------- */

/* What a region plays: note-ons as (song tick, pitch), sorted. */
static int region_events (const gr_song *s, int track, int *ev, int max)
{
  int i, n = 0, a, b;
  for (i = 0; i < GR_MAX_REGIONS; i++)
    {
      const gr_region *r = &s->regions[i];
      const gr_pattern *p;
      int rep, k;
      if (!r->used || r->track != track || r->pattern < 0)
        continue;
      p = &s->patterns[r->pattern];
      for (rep = 0; rep * p->len < r->len; rep++)
        for (k = 0; k < p->nnotes; k++)
          if (rep * p->len + p->notes[k].tick < r->len && n < max)
            ev[n++] = (r->start + rep * p->len + p->notes[k].tick) * 128 + p->notes[k].pitch;
    }
  for (a = 1; a < n; a++)
    for (b = a; b > 0 && ev[b - 1] > ev[b]; b--)
      { int t = ev[b]; ev[b] = ev[b - 1]; ev[b - 1] = t; }
  return n;
}

static void test_split_copy (void)
{
  static gr_song s;
  static int before[4096], after[4096];
  int r, n, nb, na, i, t, c;
  gr_song_init (&s);
  r = gr_loop_place (&s, find_loop ("Root Pump"), -1, 0);      /* 2 bars */
  t = s.regions[r].track;
  s.regions[r].len = 6 * GR_BAR;
  nb = region_events (&s, t, before, 4096);
  n = gr_region_split (&s, r, 3 * GR_BAR + GR_PPQ);           /* in the middle of a repeat */
  na = region_events (&s, t, after, 4096);
  CHECK (n >= 0 && s.regions[r].len == 3 * GR_BAR + GR_PPQ && s.regions[n].start == 3 * GR_BAR + GR_PPQ
         && s.regions[n].start + s.regions[n].len == 6 * GR_BAR, "a region splits in two at the tick");
  for (i = 0; i < nb && i < na && before[i] == after[i]; i++)
    ;
  CHECK (nb == na && i == nb, "and the two parts play exactly the notes it played");
  CHECK (s.regions[n].pattern != s.regions[r].pattern, "split mid-pattern: the right part's pattern is rotated");
  c = gr_region_split (&s, r, 2 * GR_BAR);                     /* on a repeat */
  CHECK (c >= 0 && s.regions[c].pattern == s.regions[r].pattern, "split on a repeat: the parts share the pattern");
  CHECK (gr_region_split (&s, r, 0) < 0 && gr_region_split (&s, r, s.regions[r].len) < 0,
         "not at a region's edges");
  /* Copy and paste. */
  n = gr_region_copy (&s, &s.regions[r], &s.patterns[s.regions[r].pattern], t, 8 * GR_BAR);
  CHECK (n >= 0 && s.regions[n].start == 8 * GR_BAR && s.regions[n].len == s.regions[r].len
         && s.regions[n].pattern != s.regions[r].pattern
         && s.patterns[s.regions[n].pattern].nnotes == s.patterns[s.regions[r].pattern].nnotes,
         "a pasted region gets a copy of its pattern");
  /* Audio: the right part starts further into the recording. */
  {
    int16_t pcm[2 * 44100];
    int a, b2;
    double spt = GR_RATE * 60.0 / ((double) s.tempo * GR_PPQ);
    t = gr_track_add (&s, TRK_AUDIO, "Audio", 0);
    c = gr_clip_new (&s, "Take", GR_RATE);
    memset (pcm, 0, sizeof pcm);
    gr_clip_append (&s.clips[c], pcm, 44100);
    a = gr_region_add (&s, t, 0, GR_BAR, -1);
    s.regions[a].clip = c;
    b2 = gr_region_split (&s, a, GR_PPQ);
    CHECK (b2 >= 0 && s.regions[b2].clip == c && abs (s.regions[b2].clip_offset - (int) (GR_PPQ * spt)) <= 1,
           "an audio region's right part starts further into the recording");
  }
  gr_song_free (&s);
}

/* ---- count-in and merged recordings ------------------------------------------------ */
static void test_count_in_merge (void)
{
  static gr_song s;
  gr_engine *e;
  float l[256], r[256], peak = 0;
  int t, i, j, base, n0, regions = 0;
  double spt = GR_RATE * 60.0 / (120.0 * GR_PPQ), beat = GR_PPQ * spt;
  gr_song_init (&s);
  s.tempo = 120;
  t = gr_track_add (&s, TRK_INST, "Keys", PATCH_EPIANO);
  s.tracks[t].armed = 1;
  e = gr_engine_new (&s);
  gr_set_count_in (e, 4);
  gr_record (e);
  CHECK (gr_recording (e) && !gr_playing (e) && gr_counting_in (e) == 4, "recording starts with a four-beat count-in");
  /* Three beats in: still counting, time standing still; a note played now isn't recorded. */
  gr_live_on (e, t, 72, 100);
  for (i = 0; i < (int) (3 * beat / 256); i++)
    {
      gr_render (e, l, r, 256);
      for (j = 0; j < 256; j++)
        if (fabsf (l[j]) > peak) peak = fabsf (l[j]);
    }
  gr_live_off (e, t, 72);
  CHECK (gr_counting_in (e) >= 1 && gr_counting_in (e) <= 2 && gr_position (e) == 0.0 && !gr_playing (e),
         "during the count-in the playhead stays put");
  CHECK (peak > 0.1f, "and it clicks");
  for (i = 0; i < (int) (2 * beat / 256); i++)
    gr_render (e, l, r, 256);
  CHECK (!gr_counting_in (e) && gr_playing (e) && gr_position (e) > 0, "then playing and recording begin");
  gr_live_on (e, t, 60, 100);
  for (i = 0; i < 20; i++) gr_render (e, l, r, 256);
  gr_live_off (e, t, 60);
  gr_stop (e);
  for (i = 0; i < GR_MAX_REGIONS; i++)
    if (s.regions[i].used)
      {
        regions++;
        CHECK (s.patterns[s.regions[i].pattern].nnotes == 1 && s.patterns[s.regions[i].pattern].notes[0].pitch == 60,
               "only the note played after the count-in is recorded");
      }
  CHECK (regions == 1, "(one region)");
  /* Stop during the count-in: nothing left behind. */
  gr_locate (e, 4 * GR_BAR);
  gr_record (e);
  gr_render (e, l, r, 256);
  gr_stop (e);
  for (i = 0, n0 = 0; i < GR_MAX_REGIONS; i++) n0 += s.regions[i].used;
  CHECK (n0 == 1 && !gr_recording (e) && !gr_counting_in (e), "stopping during the count-in leaves nothing behind");

  /* Merge: a note recorded over the existing region goes into it. */
  gr_set_count_in (e, 0);
  gr_set_merge (e, 1);
  for (base = 0; !s.regions[base].used; base++)
    ;
  n0 = s.patterns[s.regions[base].pattern].nnotes;
  gr_locate (e, s.regions[base].start);
  gr_record (e);
  for (i = 0; i < (int) (GR_PPQ * 2 * spt / 256); i++)
    gr_render (e, l, r, 256);
  gr_live_on (e, t, 64, 90);
  for (i = 0; i < 10; i++) gr_render (e, l, r, 256);
  gr_live_off (e, t, 64);
  gr_stop (e);
  for (i = 0, regions = 0; i < GR_MAX_REGIONS; i++) regions += s.regions[i].used;
  CHECK (regions == 1 && s.patterns[s.regions[base].pattern].nnotes == n0 + 1,
         "with Merge recordings on, a take over a region goes into it (no new region)");
  gr_engine_free (e);
  gr_song_free (&s);
}


/* Time signatures: bars of any length, changes part-way, saved, undone,
   in MIDI files, and followed by the metronome, count-in and recording. */
static void test_meters (void)
{
  static gr_song s, b;
  gr_engine *e;
  gr_history *h;
  const char *why;
  float l[256], r[256];
  int t, i, n, regions = 0, ok;
  gr_song_init (&s);
  CHECK (s.nmeters == 1 && gr_bar_len (&s, 0) == GR_BAR && gr_bar_tick (&s, 3) == 3 * GR_BAR,
         "a new song is in 4/4");
  CHECK (gr_set_meter (&s, 2, 3, 4) == 0 && gr_set_meter (&s, 4, 6, 8) == 0, "time signature changes at bars 3 and 5");
  CHECK (gr_bar_tick (&s, 2) == 2 * GR_BAR && gr_bar_tick (&s, 3) == 2 * GR_BAR + 3 * GR_PPQ
         && gr_bar_tick (&s, 4) == 2 * GR_BAR + 6 * GR_PPQ && gr_bar_tick (&s, 5) == 2 * GR_BAR + 9 * GR_PPQ,
         "bars after a change are the new length (3/4 = 3 beats, 6/8 = 6 quavers)");
  ok = 1;
  for (i = 0; i < 12; i++)
    ok &= gr_tick_bar (&s, gr_bar_tick (&s, i)) == i && gr_tick_bar (&s, gr_bar_tick (&s, i + 1) - 1) == i;
  CHECK (ok, "the bar a tick is in matches where bars start, across the changes");
  CHECK (gr_beat_len (&s, 3) == GR_PPQ && gr_beat_len (&s, 4) == GR_PPQ / 2 && gr_meter_at (&s, 9)->num == 6,
         "a beat is the meter's note");
  CHECK (gr_bar_snap (&s, gr_bar_tick (&s, 3) + 10) == gr_bar_tick (&s, 3), "snapping goes to the nearest real bar line");
  CHECK (gr_set_meter (&s, 6, 6, 8) == 0 && s.nmeters == 3, "setting the meter already in force adds no change");
  CHECK (gr_set_meter (&s, 1, 0, 4) < 0 && gr_set_meter (&s, 1, 3, 5) < 0, "nonsense meters are refused");

  /* Saved and loaded; and in MIDI files. */
  t = gr_track_add (&s, TRK_INST, "Keys", PATCH_EPIANO);
  n = gr_pattern_new (&s, "Tune", 3 * GR_PPQ);
  gr_pattern_add_note (&s.patterns[n], 0, GR_PPQ, 60, 100);
  gr_region_add (&s, t, gr_bar_tick (&s, 2), 3 * GR_PPQ, n);
  CHECK (gr_song_save (&s, "/tmp/groove_meter.song") == 0 && gr_song_load (&b, "/tmp/groove_meter.song") == 0
         && b.nmeters == 3 && !memcmp (b.meters, s.meters, 3 * sizeof s.meters[0]), "time signatures are saved and loaded");
  gr_song_free (&b);
  CHECK (gr_midi_export (&s, "/tmp/groove_meter.mid") == 0, "a song with time signatures exports as MIDI");
  gr_song_init (&b);
  gr_track_delete (&b, 0); gr_track_delete (&b, 0);
  while (b.ntracks) gr_track_delete (&b, 0);
  n = gr_midi_import (&b, "/tmp/groove_meter.mid", 0, &why);
  CHECK (n >= 1 && b.nmeters == 3 && b.meters[1].bar == 2 && b.meters[1].num == 3 && b.meters[2].bar == 4
         && b.meters[2].den == 8, "and imported again with the same time signatures");
  for (i = 0; i < GR_MAX_REGIONS; i++)
    if (b.regions[i].used)
      CHECK (b.regions[i].start + b.patterns[b.regions[i].pattern].notes[0].tick == gr_bar_tick (&b, 2),
             "the imported note is on the first beat of the 3/4 bar");
  gr_song_free (&b);

  /* Undo covers it. */
  h = gr_history_new (10, 0);
  gr_history_reset (h, &s);
  gr_set_meter (&s, 0, 5, 4);
  gr_history_note (h, &s);
  CHECK (gr_history_undo (h, &s) && gr_meter_at (&s, 0)->num == 4 && s.nmeters == 3, "Undo puts the time signature back");
  gr_history_free (h);

  /* Count-in and recording in 3/4: three clicks, a region a 3/4 bar long. */
  s.tracks[t].armed = 1;
  e = gr_engine_new (&s);
  gr_set_count_in (e, 4);
  gr_locate (e, gr_bar_tick (&s, 3));
  gr_record (e);
  CHECK (gr_counting_in (e) == 3, "in 3/4 the count-in is three beats");
  for (i = 0; i < 2000 && gr_counting_in (e); i++)
    gr_render (e, l, r, 256);
  gr_live_on (e, t, 67, 100);
  for (i = 0; i < 20; i++) gr_render (e, l, r, 256);
  gr_live_off (e, t, 67);
  gr_stop (e);
  for (i = 0; i < GR_MAX_REGIONS; i++)
    if (s.regions[i].used && s.regions[i].start == gr_bar_tick (&s, 3))
      {
        regions++;
        CHECK (s.regions[i].len == 3 * GR_PPQ, "the recording is a 3/4 bar long");
      }
  CHECK (regions == 1, "(one recording)");
  gr_engine_free (e);
  gr_song_free (&s);
}


/* All of a track's notes as song ticks, in order: n[k] = { start, end, pitch }. */
static int track_notes (const gr_song *s, int t, int n[][3], int max)
{
  int i, k, count = 0, a, b;
  for (i = 0; i < GR_MAX_REGIONS; i++)
    if (s->regions[i].used && s->regions[i].track == t && s->regions[i].pattern >= 0)
      {
        const gr_pattern *p = &s->patterns[s->regions[i].pattern];
        for (k = 0; k < p->nnotes && count < max; k++, count++)
          {
            n[count][0] = s->regions[i].start + p->notes[k].tick;
            n[count][1] = n[count][0] + p->notes[k].len;
            n[count][2] = p->notes[k].pitch;
          }
      }
  for (a = 1; a < count; a++)
    for (b = a; b > 0 && (n[b][0] < n[b - 1][0] || (n[b][0] == n[b - 1][0] && n[b][2] < n[b - 1][2])); b--)
      {
        int x[3];
        memcpy (x, n[b], sizeof x); memcpy (n[b], n[b - 1], sizeof x); memcpy (n[b - 1], x, sizeof x);
      }
  return count;
}

static int find_track (const gr_song *s, const char *name)
{
  int t;
  for (t = 0; t < s->ntracks; t++)
    if (!strcmp (s->tracks[t].name, name))
      return t;
  return -1;
}

static void put_int (FILE *f, int v)
{
  fputc (0x40, f); fputc (v >> 24, f); fputc (v >> 16, f); fputc (v >> 8, f); fputc (v, f);
}

/* Maestro files: a made-up one that tests each of Maestro's playing rules,
   then (if they're there) the eight that come with RISC OS 5. */
static void test_maestro (const char *dir)
{
  static gr_song s;
  static const unsigned char gates[] = {
    0, 0x65,                    /* 3/4 */
    0, 0x0A,                    /* one sharp */
    0, 0x5C,                    /* bass clef on stave 2 */
    0x91, 0x01, 0x01, 0x01,     /* bar 1 */
    0, 0x20,
    0x11, 0x01, 0x01,           /* bar 2 */
    0, 0x20 };
  static const unsigned char q0[] = {
    16 << 3, 3 << 5,            /* B4 crotchet */
    13 << 3, 3 << 5,            /* F crotchet: F#4 from the key */
    13 << 3, 4 << 5 | 1,        /* F natural quaver */
    13 << 3, 4 << 5,            /* F quaver: still natural, the same bar */
    13 << 3, 3 << 5 | 1 << 3,   /* bar 2: F dotted crotchet: F#4 again */
    0, 4 << 5,                  /* quaver rest */
    16 << 3, 4 << 5 };          /* B4 quaver */
  static const unsigned char q4[] = {
    16 << 3 | 4, 2 << 5,        /* bass clef middle line: D3 minim, tied... */
    16 << 3, 3 << 5 };          /* ...to a crotchet over the bar line */
  static const unsigned char q7[] = { 10 << 3, 4 << 5 };   /* percussion stave: a quaver */
  FILE *f = fopen ("/tmp/groove_test,af1", "wb");
  const char *why;
  int c, n[64][3], k, t, t4, t7;
  fwrite ("Maestro\n\2", 1, 9, f);
  fputc (2, f); fputc (1, f); fputc (1, f);               /* two staves and percussion */
  fputc (3, f);
  for (c = 0; c < 8; c++) { fputc (c + 1, f); fputc (c == 4 ? 5 : c == 7 ? 9 : 2, f); }
  fputc (4, f); for (c = 0; c < 8; c++) fputc (c == 4 ? 6 : 7, f);
  fputc (5, f); for (c = 0; c < 8; c++) fputc (c == 0 ? 0 : c == 4 ? 6 : 3, f);
  fputc (6, f); fputc (6, f);                             /* 90 a minute */
  fputc (1, f);
  put_int (f, sizeof gates);
  for (c = 0; c < 8; c++) put_int (f, c == 0 ? (int) sizeof q0 : c == 4 ? (int) sizeof q4 : c == 7 ? (int) sizeof q7 : 0);
  fwrite (gates, 1, sizeof gates, f);
  fwrite (q0, 1, sizeof q0, f);
  fwrite (q4, 1, sizeof q4, f);
  fwrite (q7, 1, sizeof q7, f);
  fclose (f);

  gr_song_init (&s);
  CHECK (gr_maestro_import (&s, "/tmp/groove_test,af1", 0, NULL, 0, &why) == 3, "a Maestro file: a track per channel with notes");
  CHECK (s.tempo == 90 && s.key == 7 && gr_meter_at (&s, 0)->num == 3 && gr_meter_at (&s, 0)->den == 4,
         "it takes Maestro's tempo, key (one sharp: G) and time signature (3/4)");
  t = find_track (&s, "StringLib-Soft 1");
  t4 = find_track (&s, "StringLib-Hard 5");
  t7 = find_track (&s, "Percussion-Noise 8");
  CHECK (t >= 0 && t4 >= 0 && t7 >= 0, "tracks are named after the channel's voice");
  if (t < 0 || t4 < 0 || t7 < 0)
    return;
  k = track_notes (&s, t, n, 64);
  CHECK (k == 6, "channel 1 has six notes (a rest isn't a note)");
  CHECK (n[0][0] == 0 && n[0][1] == 96 && n[0][2] == 71, "treble clef middle line: B4, a crotchet");
  CHECK (n[1][0] == 96 && n[1][2] == 66, "an F with no accidental is F sharp in G");
  CHECK (n[2][0] == 192 && n[2][1] == 240 && n[2][2] == 65, "a natural sign makes it F");
  CHECK (n[3][0] == 240 && n[3][2] == 65, "and the next F on that line in the bar is natural too");
  CHECK (n[4][0] == 288 && n[4][1] == 288 + 144 && n[4][2] == 66,
         "in the next bar (a 3/4 bar is 288 ticks) the key applies again; a dotted crotchet is 144");
  CHECK (n[5][0] == 288 + 192 && n[5][2] == 71, "a quaver rest moves time on");
  k = track_notes (&s, t4, n, 64);
  CHECK (k == 1 && n[0][0] == 0 && n[0][1] == 288 && n[0][2] == 50,
         "bass clef middle line: D3; tied over the bar line it sounds once, for both notes");
  k = track_notes (&s, t7, n, 64);
  CHECK (s.tracks[t7].type == TRK_DRUMS && k == 1 && n[0][2] == DR_CHAT, "Percussion-Noise on the percussion stave: a hi-hat (key 42)");
  CHECK (s.tracks[t].gm == 24 && s.tracks[t4].gm == 6, "StringLib voices get plucked SoundFont sounds");
  CHECK (s.tracks[t].vol > 0.79f && s.tracks[t4].vol < 0.8f * 0.53f && s.tracks[t4].vol > 0.8f * 0.51f,
         "Maestro volume 6 is 15 log steps (about 5.6 dB) below 7");
  CHECK (s.tracks[t].pan == -1.0f && s.tracks[t4].pan == 1.0f, "stereo positions 0 and 6 are hard left and right");
  gr_song_free (&s);

  CHECK (gr_maestro_import (&s, "/tmp/groove_meter.song", 0, NULL, 0, &why) < 0 && why, "a song file isn't a Maestro file");

  /* The files that come with RISC OS 5. */
  if (dir)
    {
      static const char *const names[] = { "BachFXVI", "EdWelch", "Fanfare", "Gigue", "HandlMes1a", "HandlMes1b", "ShostF1", "ShostF7" };
      int i, ok = 0, notes = 0, tracks, j;
      for (i = 0; i < 8; i++)
        {
          char path[512];
          snprintf (path, sizeof path, "%s/%s,af1", dir, names[i]);
          gr_song_init (&s);
          tracks = gr_maestro_import (&s, path, 0, NULL, 0, &why);
          if (tracks > 0 && !why)
            ok++;
          else
            printf ("  %s: %d %s\n", names[i], tracks, why ? why : "");
          for (j = 0; j < GR_MAX_PATTERNS; j++)
            if (s.patterns[j].used)
              notes += s.patterns[j].nnotes;
          if (!strcmp (names[i], "HandlMes1b"))
            {
              int b, changes = 0;
              for (b = 1; b < s.bars; b++)
                if (gr_meter_at (&s, b)->num != gr_meter_at (&s, b - 1)->num)
                  changes++;
              CHECK (changes >= 1, "HandlMes1b changes time signature part-way");
            }
          gr_song_free (&s);
        }
      CHECK (ok == 8 && notes > 4000, "the eight Maestro files from RISC OS 5 all import");
    }
}


/* Sixteen tracks: the most a song holds; MIDI channels for each; and the
   16th instrument track's second SoundFont player. */
/* MIDI out: what a sender is given. */
static struct { unsigned frame, cmd; } mo_log[512];
static int mo_n;
static void mo_capture (void *ctx, unsigned frame, unsigned cmd)
{
  (void) ctx;
  if (mo_n < 512)
    {
      mo_log[mo_n].frame = frame;
      mo_log[mo_n].cmd = cmd;
      mo_n++;
    }
}

static int mo_find (unsigned cmd, unsigned mask)
{
  int i;
  for (i = 0; i < mo_n; i++)
    if ((mo_log[i].cmd & mask) == cmd)
      return i;
  return -1;
}

/* A long 16-part MIDI file (Pete Everett's "Song full" on import): 16
   tracks of 160 bars, a crotchet every beat, exported and imported back. */
/* Drum notes are MIDI keys (0.3): songs from before convert, MIDI files
   keep every key, and any key plays. */
static void test_drum_keys (void)
{
  static gr_song s, b;
  FILE *f = fopen ("/tmp/groove_v1.song", "w");
  int t, p, k, n49 = 0, i;
  const char *why = NULL;
  gr_engine *e;
  float peak;
  fputs ("GROOVE 1\ntempo 120\nkey 0\nbars 4\nloop 0 0 0\nmetronome 0\nmaster 0.80\n"
         "track 0 0 \"Drums\" 0.80 0.00 0.08 0 0 -1\n"
         "track 1 0 \"Keys\" 0.80 0.00 0.08 0 0 -1\n"
         "pattern 0 \"Beat\" 384\nn 0 24 0 120\nn 96 24 1 100\nn 192 24 7 90\nend\n"
         "pattern 1 \"Tune\" 384\nn 0 96 1 100\nend\n"
         "region 0 0 384 0 -1 0\nregion 1 0 384 1 -1 0\n", f);
  fclose (f);
  gr_song_init (&s);
  CHECK (gr_song_load (&s, "/tmp/groove_v1.song") == 0, "a song from before 0.3 loads");
  p = s.regions[0].pattern;
  CHECK (s.patterns[p].nnotes == 3 && s.patterns[p].notes[0].pitch == DR_KICK
         && s.patterns[p].notes[1].pitch == DR_SNARE && s.patterns[p].notes[2].pitch == DR_PERC,
         "its drum rows become MIDI keys (kick 36, snare 38, cowbell 56)");
  CHECK (s.patterns[s.regions[1].pattern].notes[0].pitch == 1, "an instrument's notes stay as they were");
  CHECK (gr_song_save (&s, "/tmp/groove_v2.song") == 0 && gr_song_load (&b, "/tmp/groove_v2.song") == 0
         && b.patterns[b.regions[0].pattern].notes[0].pitch == DR_KICK, "saved again (GROOVE 2), they load as keys");
  gr_song_free (&b);
  gr_song_free (&s);

  /* Any key: a crash cymbal (49) on a drum track, through a MIDI file. */
  gr_song_init (&s);
  t = gr_track_add (&s, TRK_DRUMS, "Drums", KIT_STUDIO);
  p = gr_pattern_new (&s, "P", GR_BAR);
  gr_pattern_add_note (&s.patterns[p], 0, GR_STEP, 49, 110);
  gr_pattern_add_note (&s.patterns[p], GR_PPQ, GR_STEP, 27, 100);
  gr_pattern_add_note (&s.patterns[p], 2 * GR_PPQ, GR_STEP, DR_KICK, 100);
  gr_region_add (&s, t, 0, GR_BAR, p);
  CHECK (gr_midi_export (&s, "/tmp/groove_keys.mid") == 0, "drum keys export");
  gr_song_init (&b);
  gr_midi_import (&b, "/tmp/groove_keys.mid", 0, &why);
  for (i = 0; i < GR_MAX_REGIONS; i++)
    if (b.regions[i].used)
      for (k = 0; k < b.patterns[b.regions[i].pattern].nnotes; k++)
        {
          int key = b.patterns[b.regions[i].pattern].notes[k].pitch;
          n49 += key == 49 ? 1 : key == 27 ? 10 : key == 36 ? 100 : 1000;
        }
  CHECK (n49 == 111, "and import as the same keys (49 crash, 27, 36 kick), not the nearest of eight");
  gr_song_free (&b);
  e = gr_engine_new (&s);
  gr_play (e);
  peak = render_peak (e, GR_RATE / 4);
  CHECK (peak > 0.02f, "a key Groove has no drum of its own for plays the nearest (a crash: the cowbell)");
  gr_engine_free (e);
  gr_song_free (&s);
}

/* A track's own MIDI channel, Bank Select and program (Pete Everett). */
static void test_track_midi (void)
{
  static gr_song s, b;
  gr_engine *e;
  int t, p, i, cc0 = -1, cc32 = -1, pc = -1, on = -1;
  const char *why = NULL;
  gr_song_init (&s);
  t = gr_track_add (&s, TRK_INST, "Synth", PATCH_PAD);
  CHECK (s.tracks[t].channel == -1 && s.tracks[t].bank_msb == -1 && s.tracks[t].program == -1,
         "a new track's channel, bank and program are automatic");
  s.tracks[t].channel = 4;
  s.tracks[t].bank_msb = 1;
  s.tracks[t].bank_lsb = 2;
  s.tracks[t].program = 50;
  s.tracks[t].midi_out = 1;
  p = gr_pattern_new (&s, "P", GR_BAR);
  gr_pattern_add_note (&s.patterns[p], 0, GR_PPQ, 60, 100);
  gr_region_add (&s, t, 0, GR_BAR, p);
  e = gr_engine_new (&s);
  mo_n = 0;
  gr_engine_set_midi_out (e, mo_capture, NULL);
  gr_play (e);
  render_peak (e, GR_RATE / 4);
  for (i = 0; i < mo_n; i++)
    {
      unsigned c = mo_log[i].cmd & 0xFFFFFF;
      if (c == (0xB4u | 0 << 8 | 1 << 16)) cc0 = i;
      if (c == (0xB4u | 32 << 8 | 2 << 16)) cc32 = i;
      if ((c & 0xFFFF) == (0xC4u | 50 << 8)) pc = i;
      if ((c & 0xFFFF) == (0x94u | 60 << 8)) on = i;
    }
  CHECK (cc0 >= 0 && cc32 > cc0 && pc > cc32 && on > pc,
         "MIDI out: on the track's channel (5), Bank Select MSB and LSB, then its program, then the note");
  gr_engine_free (e);
  CHECK (gr_midi_export (&s, "/tmp/groove_chan.mid") == 0, "it exports");
  gr_song_init (&b);
  gr_midi_import (&b, "/tmp/groove_chan.mid", 0, &why);
  CHECK (b.ntracks == 1 && b.tracks[0].channel == 4 && b.tracks[0].bank_msb == 1 && b.tracks[0].bank_lsb == 2
         && b.tracks[0].gm == 50, "and imports with the same channel, bank and program");
  CHECK (gr_song_save (&s, "/tmp/groove_chan.song") == 0, "the song saves");
  gr_song_free (&b);
  gr_song_init (&b);
  CHECK (gr_song_load (&b, "/tmp/groove_chan.song") == 0 && b.tracks[0].channel == 4 && b.tracks[0].bank_msb == 1
         && b.tracks[0].bank_lsb == 2 && b.tracks[0].program == 50, "and loads with them");
  gr_song_free (&b);
  gr_song_free (&s);
}

/* Controller events (Pete Everett: controllers, program changes after the
   first, and a lane to edit them). */
static void test_controllers (void)
{
  static gr_song s, b;
  gr_engine *e;
  gr_history *h;
  int t, p, i, k, cc1 = -1, reset = -1, prog = -1, pc2 = -1, on2 = -1, bend = -1;
  const char *why = NULL;
  gr_song_init (&s);
  s.tempo = 120;
  t = gr_track_add (&s, TRK_INST, "Lead", PATCH_LEAD);
  p = gr_pattern_new (&s, "P", GR_BAR);
  gr_pattern_add_note (&s.patterns[p], 0, GR_PPQ, 60, 100);
  gr_pattern_add_note (&s.patterns[p], 2 * GR_PPQ, GR_PPQ, 62, 100);
  CHECK (gr_pattern_set_ctl (&s.patterns[p], 0, 1, 90) == 0, "a controller event goes in a pattern");
  gr_pattern_set_ctl (&s.patterns[p], 2 * GR_PPQ, GR_CTL_PROGRAM, 40);
  gr_pattern_set_ctl (&s.patterns[p], GR_PPQ, GR_CTL_BEND, 4096);
  gr_pattern_set_ctl (&s.patterns[p], GR_PPQ, GR_CTL_BEND, 2000);
  CHECK (s.patterns[p].nctls == 3 && s.patterns[p].ctls[1].value == 2000 && s.patterns[p].ctls[2].type == GR_CTL_PROGRAM,
         "kept in time order, one of a type at a tick (the newer replaces it)");
  CHECK (gr_ctl_value_at (&s.patterns[p], GR_PPQ + 10, GR_CTL_BEND, 0) == 2000
         && gr_ctl_value_at (&s.patterns[p], 10, GR_CTL_BEND, 0) == 0, "the value in force at a time");
  CHECK (gr_pattern_set_ctl (&s.patterns[p], 0, GR_CTL_BEND, 99999) >= 0
         && gr_ctl_value_at (&s.patterns[p], 0, GR_CTL_BEND, 0) == 8191, "values are kept in range");
  gr_pattern_delete_ctl (&s.patterns[p], gr_pattern_find_ctl (&s.patterns[p], 0, GR_CTL_BEND));
  CHECK (!strcmp (gr_ctl_name (1), "Modulation (CC 1)") && !strcmp (gr_ctl_name (GR_CTL_BEND), "Pitch bend")
         && !strcmp (gr_ctl_name (20), "CC 20"), "controllers have names");
  gr_region_add (&s, t, 0, GR_BAR, p);
  s.tracks[t].midi_out = 1;

  e = gr_engine_new (&s);
  gr_engine_set_midi_out (e, mo_capture, NULL);
  mo_n = 0;
  gr_play (e);
  render_peak (e, GR_RATE * 3 / 2);
  for (i = 0; i < mo_n; i++)
    {
      unsigned c = mo_log[i].cmd;
      if ((c & 0xFFFF) == (0xB0u | 121 << 8) && reset < 0) reset = i;
      if ((c & 0xFF) == 0xC0 && prog < 0) prog = i;
      if ((c & 0xFFFFFF) == (0xB0u | 1 << 8 | 90 << 16)) cc1 = i;
      if ((c & 0xFFFFFF) == (0xE0u | ((2000 + 8192) & 127) << 8 | ((2000 + 8192) >> 7) << 16)) bend = i;
      if (c == (0x020000C0u | 40 << 8)) pc2 = i;
      if ((c & 0xFFFF) == (0x90u | 62 << 8)) on2 = i;
    }
  CHECK (reset >= 0 && prog > reset && cc1 > prog,
         "MIDI out: Reset All Controllers, the track's program, then the pattern's controller at its time");
  CHECK (bend > cc1 && (int) mo_log[bend].frame - (int) mo_log[cc1].frame > 20000,
         "pitch bend half a second later, as two 7-bit halves");
  CHECK (pc2 > bend && on2 > pc2, "the program change on beat 3 comes before the note that starts with it");
  {
    int again = 0;
    for (i = pc2 + 1; i < mo_n; i++)
      if ((mo_log[i].cmd & 0xFF) == 0xC0)
        again = 1;
    CHECK (!again, "and the track's own program isn't sent again to undo it");
  }
  gr_stop (e);
  /* Starting at beat 2: the controllers in force there go out first. */
  mo_n = 0;
  gr_locate (e, GR_PPQ + 48);
  gr_play (e);
  render_peak (e, GR_RATE / 20);
  cc1 = bend = -1;
  for (i = 0; i < mo_n; i++)
    {
      if ((mo_log[i].cmd & 0xFFFFFF) == (0xB0u | 1 << 8 | 90 << 16)) cc1 = i;
      if ((mo_log[i].cmd & 0xFF) == 0xE0) bend = i;
    }
  CHECK (cc1 >= 0 && bend >= 0, "starting part-way, the controllers in force there are sent (chased)");
  gr_stop (e);
  gr_engine_free (e);

  /* Saved, Undo, split. */
  CHECK (gr_song_save (&s, "/tmp/groove_ctl.song") == 0 && gr_song_load (&b, "/tmp/groove_ctl.song") == 0
         && b.patterns[b.regions[0].pattern].nctls == 3
         && b.patterns[b.regions[0].pattern].ctls[2].type == GR_CTL_PROGRAM, "controllers are saved in songs");
  gr_song_free (&b);
  h = gr_history_new (16, 1 << 20);
  gr_history_reset (h, &s);
  gr_pattern_set_ctl (&s.patterns[p], 3 * GR_PPQ, 7, 50);
  gr_history_note (h, &s);
  gr_history_undo (h, &s);
  CHECK (s.patterns[s.regions[0].pattern].nctls == 3, "Undo takes a controller edit back");
  gr_history_free (h);
  k = gr_region_split (&s, 0, GR_PPQ + GR_PPQ / 2);
  CHECK (k >= 0 && s.patterns[s.regions[k].pattern].nctls == 3
         && gr_pattern_find_ctl (&s.patterns[s.regions[k].pattern], 2 * GR_PPQ - (GR_PPQ + GR_PPQ / 2), GR_CTL_PROGRAM) >= 0,
         "a split region's second half keeps the controllers, in their places");

  /* A MIDI file: later program changes, controllers and bends come in. */
  gr_song_free (&s);
  gr_song_init (&s);
  t = gr_track_add (&s, TRK_INST, "Lead", PATCH_LEAD);
  p = gr_pattern_new (&s, "P", 2 * GR_BAR);
  gr_pattern_add_note (&s.patterns[p], 0, GR_PPQ, 60, 100);
  gr_pattern_add_note (&s.patterns[p], GR_BAR + GR_PPQ, GR_PPQ, 64, 100);
  gr_pattern_set_ctl (&s.patterns[p], GR_BAR, GR_CTL_PROGRAM, 81);
  gr_pattern_set_ctl (&s.patterns[p], 10, 11, 70);
  gr_pattern_set_ctl (&s.patterns[p], 20, GR_CTL_BEND, -3000);
  gr_pattern_set_ctl (&s.patterns[p], 30, GR_CTL_PRESSURE, 33);
  gr_region_add (&s, t, 0, 2 * GR_BAR, p);
  s.tracks[t].gm = 5;
  CHECK (gr_midi_export (&s, "/tmp/groove_ctl.mid") == 0, "a song with controllers exports as MIDI");
  gr_song_init (&b);
  gr_midi_import (&b, "/tmp/groove_ctl.mid", 0, &why);
  {
    int got = 0;
    for (i = 0; i < GR_MAX_REGIONS; i++)
      if (b.regions[i].used)
        {
          const gr_pattern *q = &b.patterns[b.regions[i].pattern];
          int at = b.regions[i].start;
          for (k = 0; k < q->nctls; k++)
            {
              const gr_ctl *c = &q->ctls[k];
              if (c->type == GR_CTL_PROGRAM && c->value == 81 && at + c->tick == GR_BAR) got |= 1;
              if (c->type == 11 && c->value == 70) got |= 2;
              if (c->type == GR_CTL_BEND && c->value == -3000) got |= 4;
              if (c->type == GR_CTL_PRESSURE && c->value == 33) got |= 8;
            }
        }
    CHECK (b.ntracks == 1 && b.tracks[0].gm == 5 && got == 15,
           "and imports with its later program change, expression, pitch bend and pressure (not just the first program)");
  }
  gr_song_free (&b);
  gr_song_free (&s);
}

/* Controllers on a SoundFont sound: a pitch bend raises the note. */
static double gm_freq (int bend)
{
  static gr_song s;
  static float l[GR_RATE], r[GR_RATE];
  gr_engine *e;
  int t, p, i, cross = 0, first = -1, last = -1;
  gr_song_init (&s);
  s.tempo = 60;
  t = gr_track_add (&s, TRK_INST, "Lead", PATCH_LEAD);
  s.tracks[t].gm = 80;                       /* Square Lead */
  p = gr_pattern_new (&s, "P", GR_BAR);
  gr_pattern_add_note (&s.patterns[p], 0, GR_BAR, 57, 100);   /* A3, 220 Hz */
  if (bend)
    gr_pattern_set_ctl (&s.patterns[p], 0, GR_CTL_BEND, bend);
  gr_region_add (&s, t, 0, GR_BAR, p);
  e = gr_engine_new (&s);
  gr_play (e);
  for (i = 0; i < GR_RATE; i += 512)
    gr_render (e, l + i, r + i, GR_RATE - i < 512 ? GR_RATE - i : 512);
  gr_engine_free (e);
  gr_song_free (&s);
  /* The period: the lag (100-600 Hz) where the sound best matches itself. */
  {
    int lag, best = 0;
    double most = -1e30;
    const float *x = l + GR_RATE / 4;
    (void) cross; (void) first; (void) last;
    for (lag = GR_RATE / 600; lag <= GR_RATE / 100; lag++)
      {
        double sum = 0;
        for (i = 0; i < 8192; i++)
          sum += (double) x[i] * x[i + lag];
        if (sum > most)
          {
            most = sum;
            best = lag;
          }
      }
    return best ? (double) GR_RATE / best : 0;
  }
}

static void test_gm_controllers (const char *sf)
{
  double f0, f1;
  if (!gr_gm_ready () && gr_gm_load (sf) < 0)
    return;
  f0 = gm_freq (0);
  f1 = gm_freq (8191);
  printf ("  SoundFont A3: %.1f Hz, bent up: %.1f Hz\n", f0, f1);
  CHECK (f0 > 210 && f0 < 230 && f1 > 240 && f1 < 252,
         "a pitch bend event bends a SoundFont note (+2 semitones: 220 Hz to about 247)");
}

static void test_long_import (void)
{
  static gr_song s, b;
  int t, bar, total = 0, n = 0, i;
  const char *why = NULL;
  gr_song_init (&s);
  s.tempo = 120;
  for (t = 0; t < 16; t++)
    {
      if (gr_track_add (&s, t == 9 ? TRK_DRUMS : TRK_INST, "Part", t == 9 ? KIT_STUDIO : PATCH_EPIANO) < 0)
        break;
    }
  for (t = 0; t < s.ntracks; t++)
    for (bar = 0; bar < 160; bar += 8)
      {
        int p = gr_pattern_new (&s, "P", 8 * GR_BAR), k;
        for (k = 0; k < 32; k++)
          gr_pattern_add_note (&s.patterns[p], k * GR_PPQ, GR_PPQ / 2, s.tracks[t].type == TRK_DRUMS ? DR_KICK : 60 + t, 100);
        gr_region_add (&s, t, bar * GR_BAR, 8 * GR_BAR, p);
        total += 32;
      }
  CHECK (gr_midi_export (&s, "/tmp/groove_long.mid") == 0, "a long 16-part song exports as MIDI");
  gr_song_init (&b);
  CHECK (gr_midi_import (&b, "/tmp/groove_long.mid", 0, &why) == 16 && why == NULL,
         "and imports, all 16 parts, without \"The song is full\"");
  for (i = 0; i < GR_MAX_REGIONS; i++)
    if (b.regions[i].used)
      n += b.patterns[b.regions[i].pattern].nnotes;
  CHECK (n == total, "with every note");
  gr_song_free (&b);
  gr_song_free (&s);
}

static void test_midi_out (void)
{
  static gr_song s, b;
  gr_engine *e;
  int k, d, p, i, on, off;
  float peak;
  double spt = GR_RATE * 60.0 / (120.0 * GR_PPQ);
  gr_song_init (&s);
  s.tempo = 120;
  d = gr_track_add (&s, TRK_DRUMS, "Drums", KIT_STUDIO);
  k = gr_track_add (&s, TRK_INST, "Keys", PATCH_EPIANO);
  p = gr_pattern_new (&s, "P", GR_BAR);
  gr_pattern_add_note (&s.patterns[p], GR_PPQ, GR_PPQ, 60, 100);   /* beat 2, a crotchet */
  gr_region_add (&s, k, 0, GR_BAR, p);
  p = gr_pattern_new (&s, "D", GR_BAR);
  gr_pattern_add_note (&s.patterns[p], 0, GR_PPQ / 4, DR_KICK, 120);
  gr_region_add (&s, d, 0, GR_BAR, p);
  s.tracks[k].midi_out = 1;
  s.tracks[k].vol = 0.5f;
  s.tracks[k].pan = 1.0f;
  s.tracks[d].mute = 1;         /* (to hear only the MIDI out track: nothing) */

  e = gr_engine_new (&s);
  mo_n = 0;
  gr_engine_set_midi_out (e, mo_capture, NULL);
  gr_play (e);
  peak = 0;
  for (i = 0; i < GR_RATE; i += 256)
    {
      float l[256], r[256];
      int j;
      gr_render (e, l, r, 256);
      for (j = 0; j < 256; j++)
          if (fabsf (l[j]) > peak) peak = fabsf (l[j]);
    }
  /* Keys: the first instrument track, so channel 1 (index 0). */
  CHECK (mo_find (0x020000C0u | (unsigned) gr_patch_program (PATCH_EPIANO) << 8, 0xFFFFFFFF) >= 0,
         "MIDI out: the track's program goes out (Groove's sound's nearest GM program), as a 2-byte command");
  CHECK (mo_find (0x030000B0u | 7 << 8 | 64 << 16, 0xFFFFFFFF) >= 0, "and its volume (CC7, 0.5 -> 64)");
  CHECK (mo_find (0x030000B0u | 10 << 8 | 127 << 16, 0xFFFFFFFF) >= 0, "and its pan (CC10, right -> 127)");
  on = mo_find (0x03000090u | 60 << 8 | 100 << 16, 0xFFFFFFFF);
  off = mo_find (0x80 | 60 << 8, 0xFFFF);
  CHECK (on >= 0 && fabs (mo_log[on].frame - GR_PPQ * spt) <= 64,
         "the note goes out stamped with when it's heard (beat 2 at 120 bpm: half a second in)");
  CHECK (off > on && fabs ((double) (mo_log[off].frame - mo_log[on].frame) - GR_PPQ * spt) <= 64,
         "and its note-off a crotchet later");
  CHECK (mo_find (0x99, 0xFF) < 0, "a drum track without MIDI out sends nothing");
  CHECK (peak < 0.01f, "a MIDI out track makes no sound of its own in Groove");
  mo_n = 0;
  gr_live_on (e, k, 64, 90);
  gr_live_off (e, k, 64);
  CHECK (mo_n == 2 && mo_log[0].frame == 0 && mo_log[0].cmd == (0x03000090u | 64 << 8 | 90 << 16)
         && mo_log[1].frame == 0 && (mo_log[1].cmd & 0xFFFF) == (0x80u | 64 << 8),
         "notes played live go out straight away (frame 0)");
  /* Drums out: channel 10, GM keys. */
  s.tracks[d].mute = 0;
  s.tracks[d].midi_out = 1;
  mo_n = 0;
  gr_live_on (e, d, DR_SNARE, 100);
  CHECK (mo_find (0x99 | (unsigned) DR_SNARE << 8, 0xFFFF) >= 0,
         "drum tracks play on channel 10 with General MIDI drum keys");
  CHECK (mo_find (0xC9, 0xFF) >= 0, "with a kit program sent first");
  mo_n = 0;
  gr_stop (e);
  CHECK (mo_find (0xB0 | 123 << 8, 0xFFFF) >= 0 && mo_find (0xB9 | 123 << 8, 0xFFFF) >= 0,
         "stopping sends All Notes Off on the channels used");
  /* Playing again sends every channel's settings again (Pete Everett: they
     only went the first time). */
  mo_n = 0;
  gr_locate (e, 0);
  gr_play (e);
  render_peak (e, GR_RATE / 10);
  CHECK (mo_find (0x020000C0u | (unsigned) gr_patch_program (PATCH_EPIANO) << 8, 0xFFFFFFFF) >= 0
         && mo_find (0x030000B0u | 7 << 8 | 64 << 16, 0xFFFFFFFF) >= 0
         && mo_find (0x030000B0u | 10 << 8 | 127 << 16, 0xFFFFFFFF) >= 0,
         "playing again sends the program, volume and pan again");
  gr_stop (e);
  /* Mute: the channel's volume goes to 0 and no notes go out. */
  s.tracks[k].mute = 1;
  mo_n = 0;
  gr_locate (e, 0);
  gr_play (e);
  render_peak (e, GR_RATE);
  CHECK (mo_find (0xB0 | 7 << 8, 0xFFFFFF) >= 0 && mo_find (0x90, 0xFF) < 0,
         "a muted MIDI out track sends volume 0 and no notes");
  gr_stop (e);
  s.tracks[k].mute = 0;
  gr_engine_set_midi_out (e, NULL, NULL);
  gr_locate (e, 0);
  gr_play (e);
  render_peak (e, GR_RATE / 4);
  peak = render_peak (e, GR_RATE / 2);
  CHECK (peak > 0.02f, "without a sender (bouncing a WAV, say), a MIDI out track plays Groove's sound");
  gr_engine_free (e);
  CHECK (gr_song_save (&s, "/tmp/groove_mo.txt") == 0 && gr_song_load (&b, "/tmp/groove_mo.txt") == 0
         && b.tracks[k].midi_out == 1 && b.tracks[d].midi_out == 1 && b.tracks[k].gm == -1,
         "MIDI out is saved in the song");
  gr_song_free (&b);
  gr_song_free (&s);
}

static void test_sixteen (const char *sf)
{
  static gr_song s, b;
  gr_engine *e;
  const char *why;
  int t, n, p;
  gr_song_init (&s);
  for (t = 0; t < 16; t++)
    {
      int k = gr_track_add (&s, t == 3 ? TRK_DRUMS : TRK_INST, "T", PATCH_EPIANO);
      if (k < 0) break;
      s.tracks[k].gm = t == 3 ? 0 : t * 4;
      p = gr_pattern_new (&s, "P", GR_BAR);
      gr_pattern_add_note (&s.patterns[p], 0, GR_PPQ, t == 3 ? 0 : 48 + t, 100);
      gr_region_add (&s, k, 0, GR_BAR, p);
    }
  CHECK (s.ntracks == 16, "sixteen tracks");
  {
    static gr_song big;
    gr_song_init (&big);
    for (t = 0; t < 40; t++)
      gr_track_add (&big, TRK_INST, "T", PATCH_EPIANO);
    CHECK (big.ntracks == 32 && GR_MAX_TRACKS == 32, "a song holds 32 tracks (Pete Everett: more than 16), and no more");
    CHECK (gr_track_channel (&big, 31) == 15 && gr_track_channel (&big, 8) == 8 && gr_track_channel (&big, 9) == 10,
           "automatic channels skip 10 for drums; from the 16th instrument track on they share channel 16");
    big.tracks[20].channel = 4;
    CHECK (gr_track_channel (&big, 20) == 4, "a track can have a channel of its own");
    gr_song_free (&big);
  }
  CHECK (!gr_gm_need_second (&s), "with a drum track among them, one SoundFont player is enough");
  CHECK (gr_midi_export (&s, "/tmp/groove_16.mid") == 0, "16 tracks export as MIDI");
  gr_song_init (&b);
  n = gr_midi_import (&b, "/tmp/groove_16.mid", 0, &why);
  CHECK (n == 16 && b.ntracks == 16, "and import again as 16 tracks: every part had a channel of its own");
  gr_song_free (&b);
  /* All sixteen instruments: the last needs the second player. */
  s.tracks[3].type = TRK_INST;
  s.tracks[3].gm = 12;
  CHECK (gr_gm_need_second (&s), "16 SoundFont instrument tracks need the second player");
  if (sf && *sf)
    {
      float peak;
      if (gr_gm_load (sf) == 0)
        {
          for (t = 0; t < 15; t++)
            s.tracks[t].mute = 1;           /* hear only the 16th */
          e = gr_engine_new (&s);
          gr_play (e);
          peak = render_peak (e, GR_RATE / 2);
          gr_engine_free (e);
          CHECK (peak > 0.02f && !gr_gm_second_ready (), "without the second player the 16th track still sounds (Groove's own sound)");
          CHECK (gr_gm_load_second () == 0 && gr_gm_second_ready (), "the second SoundFont player loads");
          e = gr_engine_new (&s);
          gr_play (e);
          peak = render_peak (e, GR_RATE / 2);
          gr_engine_free (e);
          CHECK (peak > 0.02f, "and the 16th track plays its SoundFont sound on it");
          gr_gm_unload ();
          CHECK (!gr_gm_second_ready (), "unloading the SoundFont unloads both players");
        }
    }
  gr_song_free (&s);
}


/* ---- MuseScore and MusicXML ------------------------------------------------- */

static unsigned char *slurp (const char *path, size_t *n)
{
  FILE *f = fopen (path, "rb");
  unsigned char *d;
  long size;
  if (!f)
    return NULL;
  fseek (f, 0, SEEK_END);
  size = ftell (f);
  fseek (f, 0, SEEK_SET);
  d = malloc ((size_t) size + 1);
  if (d && fread (d, 1, (size_t) size, f) != (size_t) size)
    {
      free (d);
      d = NULL;
    }
  fclose (f);
  *n = (size_t) size;
  return d;
}

static void test_unzip_xml (void)
{
  size_t n, m, len;
  unsigned char *zip = slurp ("host/scores/repeats3.mscz", &n), *plain = slurp ("host/scores/repeats3.mscx", &m);
  gr_zip_entry e[8];
  int k, i, found = -1;
  CHECK (zip && plain, "the test scores are there (host/scores)");
  if (!zip || !plain)
    return;
  k = gr_zip_names (zip, n, e, 8);
  CHECK (k == 4, "a .mscz lists its four files");
  for (i = 0; i < k; i++)
    if (!strcmp (e[i].name, "Repeats.mscx"))
      found = i;
  CHECK (found >= 0 && e[found].method == 8, "its score is deflated");
  if (found >= 0)
    {
      unsigned char *d = gr_zip_read (zip, n, &e[found], &len);
      CHECK (d && len == m && !memcmp (d, plain, m), "unzipped, it's the score byte for byte");
      free (d);
      /* Damage: a cut-short zip, and scrambled deflate data. */
      CHECK (gr_zip_names (zip, 40, e, 8) < 0, "a cut-short zip isn't read");
      for (i = 0; i < 64; i++)
        zip[e[found].offset + 60 + (size_t) i * 7 % 300] ^= (unsigned char) (i * 37 + 1);
      d = gr_zip_read (zip, n, &e[found], &len);
      CHECK (1, "scrambled deflate data doesn't crash");
      free (d);
    }
  CHECK (gr_zip_names (plain, m, e, 8) < 0, "an XML file isn't a zip");
  free (zip);
  free (plain);

  {
    char doc[] = "\xEF\xBB\xBF<?xml version=\"1.0\"?>\n<!DOCTYPE a [ <!ENTITY x \"y\"> ]>\n"
                 "<!-- a comment <b>not</b> -->\n<a one='1' two=\"&lt;2&gt;\">\n  <b>caf&#233; &amp; &#x266d;</b>"
                 "<c/><b k=\"v\" >second</b><d><![CDATA[x<y]]></d>\n</a>";
    gx_doc *d = gx_parse (doc, strlen (doc));
    gx_node *r = d ? gx_root (d) : NULL, *b;
    CHECK (r && !strcmp (r->name, "a") && !strcmp (gx_attr (r, "one"), "1") && !strcmp (gx_attr (r, "two"), "<2>"),
           "XML: the root and its attributes (both quotes, entities)");
    b = r ? gx_child (r, "b") : NULL;
    CHECK (b && !strcmp (gx_text (b, NULL), "caf\xC3\xA9 & \xE2\x99\xAD"), "XML: text with entities and character references, as UTF-8");
    CHECK (b && gx_next (b, "b") && !strcmp (gx_text (gx_next (b, "b"), NULL), "second")
           && !strcmp (gx_attr (gx_next (b, "b"), "k"), "v"), "XML: the next element of a name");
    CHECK (r && gx_child (r, "c") && !gx_child (gx_child (r, "c"), "b"), "XML: an empty element");
    CHECK (r && !strcmp (gx_text (r, "d"), "x<y"), "XML: CDATA");
    gx_free (d);
  }
  {
    char bad1[] = "<a><b></a", bad2[] = "<a x=1></a>", bad3[] = "just text";
    gx_doc *d;
    CHECK (!(d = gx_parse (bad3, strlen (bad3))), "XML: text with no element isn't a document");
    gx_free (d);
    d = gx_parse (bad2, strlen (bad2));
    CHECK (!d, "XML: an unquoted attribute isn't read");
    gx_free (d);
    d = gx_parse (bad1, strlen (bad1));
    CHECK (1, "XML: unclosed elements don't crash");
    gx_free (d);
  }
}

typedef struct { int tick, pitch, len, vel; } xnote;

/* A track's notes, in song ticks, in order (tick, then pitch). */
static int track_note_list (const gr_song *s, int t, xnote *out, int max)
{
  int n = 0, r, k, i, j;
  for (r = 0; r < GR_MAX_REGIONS; r++)
    {
      const gr_region *g = &s->regions[r];
      if (!g->used || g->track != t || g->pattern < 0)
        continue;
      for (k = 0; k < s->patterns[g->pattern].nnotes && n < max; k++)
        {
          const gr_note *x = &s->patterns[g->pattern].notes[k];
          out[n].tick = g->start + x->tick;
          out[n].pitch = x->pitch;
          out[n].len = x->len;
          out[n].vel = x->vel;
          n++;
        }
    }
  for (i = 1; i < n; i++)
    for (j = i; j > 0 && (out[j].tick < out[j - 1].tick
                          || (out[j].tick == out[j - 1].tick && out[j].pitch < out[j - 1].pitch)); j--)
      {
        xnote x = out[j];
        out[j] = out[j - 1];
        out[j - 1] = x;
      }
  return n;
}

static void test_score_import (void)
{
  /* host/scores/repeats.*: a pickup, |: D E~ | E (F+A) triplet | 1. C :| 2. D (8va) | (grace) E + C below,
     in 3/4. Played as MuseScore plays it: the repeat, then the second ending. */
  static const xnote want[] = {
    { 0, 72, 96, 45 }, { 96, 74, 96, 80 }, { 192, 76, 288, 80 }, { 480, 77, 96, 80 }, { 480, 81, 96, 80 },
    { 576, 79, 32, 80 }, { 608, 81, 32, 80 }, { 640, 83, 32, 80 }, { 672, 84, 288, 80 },
    { 960, 74, 96, 80 }, { 1056, 76, 288, 80 }, { 1344, 77, 96, 80 }, { 1344, 81, 96, 80 },
    { 1440, 79, 32, 80 }, { 1472, 81, 32, 80 }, { 1504, 83, 32, 80 }, { 1536, 86, 288, 80 },
    { 1824, 60, 288, 49 }, { 1824, 76, 192, 49 } };
  static const char *files[] = { "host/scores/repeats.musicxml", "host/scores/repeats.mxl",
                                 "host/scores/repeats-utf16.musicxml", "host/scores/repeats3.mscx",
                                 "host/scores/repeats3.mscz" };
  static const int kinds[] = { GR_SCORE_MUSICXML, GR_SCORE_MXL, GR_SCORE_MUSICXML, GR_SCORE_MSCX, GR_SCORE_MSCZ };
  static gr_song s;
  xnote got[64];
  const char *why;
  int f, n, k, ok;
  char what[160];
  for (f = 0; f < 5; f++)
    {
      int mscore = f >= 3;
      gr_song_init (&s);
      snprintf (what, sizeof what, "%s is a score (kind %d)", files[f], kinds[f]);
      CHECK (gr_score_kind (files[f]) == kinds[f], what);
      n = gr_score_import (&s, files[f], 0, &why);
      snprintf (what, sizeof what, "%s imports (%s)", files[f], why ? why : "no message");
      CHECK (n == (mscore ? 1 : 2) && !why, what);
      if (n < 1)
        continue;
      snprintf (what, sizeof what, "%s: it replaces the new song's empty tracks; its tempo and 3/4", files[f]);
      CHECK (s.ntracks == n && s.tempo == (mscore ? 120 : 100) && s.nmeters == 1 && s.meters[0].num == 3
             && s.meters[0].den == 4, what);
      n = track_note_list (&s, 0, got, 64);
      ok = n == (int) (sizeof want / sizeof want[0]);
      for (k = 0; ok && k < n; k++)
        ok = got[k].tick == want[k].tick && got[k].pitch == want[k].pitch && got[k].len == want[k].len
             && (got[k].vel == want[k].vel || (mscore && k == 0 && got[k].vel == 80));
      snprintf (what, sizeof what, "%s: repeats and endings played out, ties joined, triplets, a chord, "
                "grace notes left out, the 8va, a second voice, dynamics (%d notes)", files[f], n);
      CHECK (ok, what);
      if (!mscore)
        {
          snprintf (what, sizeof what, "%s: parts keep their names and instruments", files[f]);
          CHECK (!strcmp (s.tracks[1].name, "Clarinet in Bb"), "a part's name in RISC OS's characters (B-flat as Bb)");
          CHECK (!strcmp (s.tracks[0].name, "Flute & friends") && s.tracks[0].gm == 73
                 && s.tracks[1].gm == 71 && s.tracks[0].type == TRK_INST, what);
          n = track_note_list (&s, 1, got, 64);
          snprintf (what, sizeof what, "%s: a transposing part sounds where it should (B-flat clarinet: written D, heard C), in the repeat too", files[f]);
          CHECK (n == 2 && got[0].tick == 96 && got[0].pitch == 72 && got[1].tick == 960 && got[1].pitch == 72, what);
        }
      else
        {
          snprintf (what, sizeof what, "%s: the part's name and program (not MuseScore's own sound bank)", files[f]);
          CHECK (!strcmp (s.tracks[0].name, "Piano") && s.tracks[0].gm == 0 && s.tracks[0].bank_msb == -1, what);
        }
      gr_song_free (&s);
    }

  /* MuseScore 2's own .mscz of repeats.musicxml (its voltas, ties and
     tuplets are written differently again). */
  gr_song_init (&s);
  n = gr_score_import (&s, "host/scores/repeats2.mscz", 0, &why);
  CHECK (n == 2 && gr_score_kind ("host/scores/repeats2.mscz") == GR_SCORE_MSCZ, "MuseScore 2: a .mscz imports");
  n = track_note_list (&s, 0, got, 64);
  ok = n == (int) (sizeof want / sizeof want[0]);
  for (k = 0; ok && k < n; k++)
    ok = got[k].tick == want[k].tick && got[k].pitch == want[k].pitch && got[k].len == want[k].len;
  CHECK (ok, "MuseScore 2: the same notes, repeats, endings and ties");
  n = track_note_list (&s, 1, got, 64);
  CHECK (n == 2 && got[0].pitch == 72, "MuseScore 2: the clarinet at concert pitch");
  gr_song_free (&s);

  /* MuseScore 4.4: ties on the note they end on, voltas listed after the
     staves, velocities as velocities, a cue note that isn't played. */
  gr_song_init (&s);
  n = gr_score_import (&s, "host/scores/ties44.mscx", 0, &why);
  CHECK (n == 1 && !strcmp (s.tracks[0].name, "Violin") && s.tracks[0].gm == 40, "MuseScore 4.4: the instrument's name from its label");
  n = track_note_list (&s, 0, got, 64);
  CHECK (n == 4 && got[0].tick == 0 && got[0].len == 384 && got[1].tick == 384 && got[1].pitch == 62
         && got[2].tick == 768 && got[2].pitch == 60 && got[3].tick == 1152 && got[3].pitch == 64 && got[3].vel == 100
         && got[3].len == 192,
         "MuseScore 4.4: the tie joined, the repeat and its endings, a note's velocity, the cue note left out");
  gr_song_free (&s);

  /* Into a song that has music: new tracks from the bar, the song's tempo
     and meter kept. */
  gr_song_init (&s);
  gr_loop_place (&s, find_loop ("Boom Bap"), 0, 0);
  k = s.ntracks;
  n = gr_score_import (&s, "host/scores/repeats3.mscz", 4 * GR_BAR, &why);
  CHECK (n == 1 && s.ntracks == k + 1 && s.tempo == 110 && s.meters[0].num == 4,
         "into a song with music: a new track, the song's tempo and meter kept");
  n = track_note_list (&s, k, got, 64);
  CHECK (n > 0 && got[0].tick == 4 * GR_BAR && got[0].pitch == 72, "it starts at the bar it was dropped on");
  gr_song_free (&s);

  /* MuseScore 4.2 and later: container.xml lists the style file (and the
     thumbnail and settings) before the score; a violin's sound is its
     "arco" channel, with no "normal" one. */
  gr_song_init (&s);
  CHECK (gr_score_kind ("host/scores/ms4.mscz") == GR_SCORE_MSCZ, "MuseScore 4.2: a .mscz with its style file listed first");
  n = gr_score_import (&s, "host/scores/ms4.mscz", 0, &why);
  CHECK (n == 1 && s.ntracks == 1 && track_note_list (&s, 0, got, 64) > 0, "reads its score");
  CHECK (n == 1 && s.tracks[0].gm == 40, "a violin plays the violin (its arco channel's program)");
  gr_song_free (&s);

  /* Not scores. */
  CHECK (gr_score_kind ("host/test_engine.c") == GR_SCORE_NONE, "a C file isn't a score");
  gr_song_init (&s);
  CHECK (gr_score_import (&s, "host/test_engine.c", 0, &why) < 0 && why, "importing it says why not");
  CHECK (gr_score_import (&s, "/nonexistent/score.mscz", 0, &why) < 0 && why, "a missing file says so");
  gr_song_free (&s);
}

/* A song written out as MusicXML comes back the same when it's read in.
   lengths: 0 not compared, 1 exactly, 2 to the nearest 32nd note. */
static int same_notes (const gr_song *a, int ta, const gr_song *b, int tb, int lengths, int drums)
{
  static xnote x[4096], y[4096];
  int n = track_note_list (a, ta, x, 4096), m = track_note_list (b, tb, y, 4096), k;
  if (n != m)
    {
      printf ("  (%d notes, %d back)\n", n, m);
      return 0;
    }
  for (k = 0; k < n; k++)
    if (x[k].tick != y[k].tick || x[k].pitch != y[k].pitch
        || (lengths == 1 && !drums && x[k].len != y[k].len)
        || (lengths == 2 && !drums && abs (x[k].len - y[k].len) > 6) || abs (x[k].vel - y[k].vel) > 1)
      {
        printf ("  (note %d: %d %d %d %d, back %d %d %d %d)\n", k, x[k].tick, x[k].pitch, x[k].len, x[k].vel,
                y[k].tick, y[k].pitch, y[k].len, y[k].vel);
        return 0;
      }
  return 1;
}

/* Groove's loops: drums, bass, chords and a tune, eight bars. */
static void loops_song (gr_song *s)
{
  static const char *names[] = { "Boom Bap", "Funk Pop", "Pop Chords", "Hook Melody" };
  int i, b;
  gr_song_init (s);
  while (s->ntracks)
    gr_track_delete (s, s->ntracks - 1);
  for (i = 0; i < 4; i++)
    {
      int l = find_loop (names[i]);
      for (b = 0; b < 8; b += gr_loop_get (l)->bars)
        gr_loop_place (s, l, b ? s->ntracks - 1 : -1, b * GR_BAR);
    }
}

/* By hand: 4/4, a bar of 6/8, 3/4; a bass held under eighths (two
   voices), a chord tied over the bar line, triplets, dotted rhythms,
   sixteenths, a wide part on two staves, in C minor. Returns its track. */
static int hand_song (gr_song *sp)
{
  int t, p, r;
  gr_song_init (sp);
  while (sp->ntracks)
    gr_track_delete (sp, sp->ntracks - 1);
  sp->tempo = 96;
  gr_set_meter (sp, 2, 6, 8);
  gr_set_meter (sp, 3, 3, 4);
  t = gr_track_add (sp, TRK_INST, "Piano", PATCH_EPIANO);
  sp->tracks[t].gm = 0;
  p = gr_pattern_new (sp, "Piano", gr_bar_tick (sp, 4));
  r = gr_region_add (sp, t, 0, gr_bar_tick (sp, 4), p);
  CHECK (p >= 0 && r >= 0, "(a region for the piece)");
  {
    static const gr_note notes[] = {
      /* bar 1 (4/4): C minor - C2 held a bar, eighths with a G held over them */
      { 0, 384, 36, 70 }, { 0, 192, 79, 70 }, { 0, 48, 60, 80 }, { 48, 48, 63, 81 }, { 96, 48, 67, 82 }, { 144, 48, 72, 83 },
      { 192, 72, 75, 90 }, { 264, 24, 74, 60 }, { 288, 32, 72, 70 }, { 320, 32, 70, 71 }, { 352, 32, 68, 72 },
      /* bar 2: a chord held into bar 3 (tied), sixteenths below */
      { 384, 480, 60, 64 }, { 384, 480, 63, 64 }, { 384, 480, 67, 64 },
      { 384, 24, 48, 50 }, { 408, 24, 50, 51 }, { 432, 24, 51, 52 }, { 456, 24, 53, 53 },
      /* bar 3 (6/8): eighths in two groups of three */
      { 768, 48, 55, 60 }, { 816, 48, 56, 60 }, { 864, 48, 58, 60 }, { 912, 144, 43, 70 },
      /* bar 4 (3/4) */
      { 1056, 192, 72, 100 }, { 1248, 96, 71, 100 } };
    int i;
    for (i = 0; i < (int) (sizeof notes / sizeof notes[0]); i++)
      gr_pattern_add_note (&sp->patterns[p], notes[i].tick, notes[i].len, notes[i].pitch, notes[i].vel);
  }
  return t;
}

static void test_musicxml_export (void)
{
  static gr_song s, back;
  const char *path = "/tmp/groove_test.musicxml", *why;
  int t, n, ok;
  size_t len;
  char *xml;

  /* Groove's loops: drums, bass, chords and a tune. */
  loops_song (&s);
  CHECK (gr_musicxml_export (&s, path) == 0, "MusicXML: Groove's loops export");
  gr_song_init (&back);
  n = gr_score_import (&back, path, 0, &why);
  CHECK (n == 4 && back.ntracks == 4 && back.tracks[0].type == TRK_DRUMS && back.tempo == s.tempo,
         "and come back as four tracks, drums as drums, at the song's tempo");
  for (t = 0, ok = 1; t < 4; t++)
    ok &= same_notes (&s, t, &back, t, 2, s.tracks[t].type == TRK_DRUMS);
  CHECK (ok, "every note comes back where it was, the same key (drum keys too) and velocity, its length to the nearest 32nd note");
  CHECK (!strcmp (back.tracks[2].name, s.tracks[2].name) && back.tracks[2].gm == gr_track_program (&s.tracks[2]),
         "each part is named after its track, with its General MIDI instrument");
  xml = (char *) slurp (path, &len);
  CHECK (xml && strstr (xml, "<score-partwise version=\"4.0\">") && strstr (xml, "<sign>percussion</sign>")
         && strstr (xml, "<midi-unpitched>37</midi-unpitched>") && strstr (xml, "<fifths>0</fifths><mode>major</mode>"),
         "it's MusicXML 4.0, drums on a percussion staff with their keys, in C major");
  if (xml)
    {
      gx_doc *d = gx_parse (xml, len);
      CHECK (d != NULL, "and well-formed XML");
      gx_free (d);
      free (xml);
    }
  gr_song_free (&s);
  gr_song_free (&back);

  t = hand_song (&s);
  CHECK (gr_musicxml_export (&s, path) == 0, "MusicXML: a piece written by hand exports");
  gr_song_init (&back);
  n = gr_score_import (&back, path, 0, &why);
  CHECK (n == 1 && same_notes (&s, t, &back, 0, 1, 0),
         "and comes back note for note: held notes, ties over the bar line, triplets, dotted rhythms");
  CHECK (back.nmeters == 3 && back.meters[1].bar == 2 && back.meters[1].num == 6 && back.meters[1].den == 8
         && back.meters[2].num == 3, "its time signatures too");
  xml = (char *) slurp (path, &len);
  CHECK (xml && strstr (xml, "<fifths>-3</fifths><mode>minor</mode>") && strstr (xml, "<step>E</step><alter>-1</alter>"),
         "the key is C minor (three flats), and E-flat is spelled E-flat");
  CHECK (xml && strstr (xml, "<staves>2</staves>") && strstr (xml, "<voice>2</voice>")
         && strstr (xml, "<actual-notes>3</actual-notes>") && strstr (xml, "<tied type=\"start\"/>")
         && strstr (xml, "<type>eighth</type>\n        <dot/>"),
         "it's written on two staves, with a second voice, triplets, ties and a dotted eighth");
  free (xml);
  gr_song_free (&s);
  gr_song_free (&back);

  /* Nothing to write. */
  gr_song_init (&s);
  CHECK (gr_musicxml_export (&s, path) < 0, "a song with no notes isn't exported");
  gr_song_free (&s);
}

static int count_kind (const gr_engraving *e, int kind, int glyph)
{
  int i, n = 0;
  for (i = 0; i < e->n; i++)
    if (e->it[i].kind == kind && (glyph < 0 || e->it[i].glyph == glyph))
      n++;
  return n;
}

/* Every bar has a box, in order, that bar_at finds; nothing is drawn
   outside the score. */
static int engraving_sane (const gr_engraving *e, int bars)
{
  int i, b = 0;
  for (i = 0; i < e->n; i++)
    {
      const gr_eitem *it = &e->it[i];
      if (it->x0 < 0 || it->x0 > e->width + EG_SP || it->y0 > 0 || it->y0 < -e->height)
        return 0;
      if (it->kind == EK_BAR)
        {
          if (it->w != b++)
            return 0;
          if (gr_engraving_bar_at (e, (it->x0 + it->x1) / 2, (it->y0 + it->y1) / 2) != it->w)
            return 0;
        }
    }
  return b == bars && gr_engraving_bar_at (e, -EG_SP * 4, 0) < 0 && gr_engraving_bar_at (e, 0, -e->height - EG_SP * 4) < 0;
}

static void test_engrave (void)
{
  static gr_song s;
  gr_notation *nt;
  gr_engraving *e, *narrow;
  int t, ok = 1;

  /* Each of the loops' tracks on its own, as the Score window shows it. */
  loops_song (&s);
  for (t = 0; t < s.ntracks; t++)
    {
      nt = gr_notation_make (&s, t, 8);
      e = nt ? gr_engrave (nt, 0, 60 * EG_SP, NULL, s.tempo) : NULL;
      if (!e || !engraving_sane (e, 8) || count_kind (e, EK_GLYPH, -1) < 10)
        ok = 0;
      if (nt && t == 0)
        {
          const gr_nrange *hands = &nt->parts[0].voice[0], *feet = &nt->parts[0].voice[1];
          int k, h, kick_up = 0, kick_down = 0;
          for (k = 0; k < hands->n; k++)
            for (h = 0; h < nt->ev[hands->first + k].nheads; h++)
              kick_up += nt->hd[nt->ev[hands->first + k].head + h].pitch == 36;
          for (k = 0; k < feet->n; k++)
            for (h = 0; h < nt->ev[feet->first + k].nheads; h++)
              kick_down += nt->hd[nt->ev[feet->first + k].head + h].pitch == 36;
          CHECK (!kick_up && kick_down == 3, "Score: drums in two voices, the kick (feet) apart from the hands");
        }
      if (e && t == 0)
        {
          CHECK (count_kind (e, EK_GLYPH, GL_PERC_CLEF) > 0 && count_kind (e, EK_GLYPH, GL_HEAD_X) > 0,
                 "Score: drums on a percussion staff, cymbals with x noteheads");
          gr_engraving_svg (e, "/tmp/groove_test_drums.svg", 8, -1);
        }
      if (e && t == 1)
        CHECK (count_kind (e, EK_GLYPH, GL_F_CLEF) > 0 && count_kind (e, EK_GLYPH, GL_G_CLEF) == 0,
               "the bass on a bass staff");
      if (e && t == 3)
        {
          CHECK (count_kind (e, EK_GLYPH, GL_G_CLEF) > 0 && count_kind (e, EK_BEAM, -1) > 0
                 && count_kind (e, EK_GLYPH, GL_MET_QUARTER) == 1,
                 "the tune on a treble staff, with beams, and a metronome mark");
          narrow = gr_engrave (nt, 0, 25 * EG_SP, NULL, s.tempo);
          CHECK (narrow && engraving_sane (narrow, 8) && narrow->height > e->height,
                 "a narrower window gives more, shorter systems");
          gr_engraving_free (narrow);
        }
      gr_engraving_free (e);
      gr_notation_free (nt);
    }
  CHECK (ok, "every loop track is laid out, a box for each bar that a click finds, nothing outside the score");
  /* The full score: all four parts in each system, lined up, named. */
  {
    int parts[8], n, i, k, names = 0, braces;
    nt = gr_notation_make (&s, -1, 8);
    n = nt ? nt->nparts : 0;
    for (i = 0; i < n; i++)
      parts[i] = i;
    e = nt ? gr_engrave_parts (nt, parts, n, 90 * EG_SP, "Loops", s.tempo, EG_MULTIRESTS) : NULL;
    for (i = 0; e && i < e->n; i++)
      if (e->it[i].kind == EK_TEXT)
        for (k = 0; k < n; k++)
          names += !strcmp (e->text + e->it[i].w, nt->parts[k].name);
    braces = e ? count_kind (e, EK_GLYPH, GL_BRACE) : 0;
    CHECK (n == 4 && e && engraving_sane (e, 8) && names == 4 && braces == e->nsys
           && count_kind (e, EK_GLYPH, GL_PERC_CLEF) == e->nsys && count_kind (e, EK_GLYPH, GL_F_CLEF) == 2 * e->nsys,
           "Full score: four parts in each system (drums, bass, piano on two staves), named once");
    if (e)
      gr_engraving_svg (e, "/tmp/groove_test_fullscore.svg", 6, -1);
    gr_engraving_free (e);
    gr_notation_free (nt);
  }
  gr_song_free (&s);

  /* The hand-written piece: two staves, ties, triplets, dots, meters. */
  t = hand_song (&s);
  nt = gr_notation_make (&s, t, 4);
  e = nt ? gr_engrave (nt, 0, 60 * EG_SP, "Piece", s.tempo) : NULL;
  CHECK (e && engraving_sane (e, 4), "Score: a piece written by hand is laid out");
  if (e)
    {
      CHECK (count_kind (e, EK_GLYPH, GL_BRACE) >= 1 && count_kind (e, EK_GLYPH, GL_G_CLEF) == count_kind (e, EK_GLYPH, GL_BRACE)
             && count_kind (e, EK_GLYPH, GL_F_CLEF) == count_kind (e, EK_GLYPH, GL_BRACE),
             "on a grand staff, braced, both clefs on each system");
      CHECK (count_kind (e, EK_TIE, -1) >= 3 && count_kind (e, EK_GLYPH, GL_TUPLET_3) >= 1
             && count_kind (e, EK_GLYPH, GL_DOT) >= 1 && count_kind (e, EK_GLYPH, GL_FLAT) >= 3,
             "with the chord tied over the bar line, a triplet, a dotted note and three flats");
      CHECK (count_kind (e, EK_GLYPH, GL_TIME_6) == 2 && count_kind (e, EK_GLYPH, GL_TIME_8) == 2
             && count_kind (e, EK_GLYPH, GL_TIME_3) == 2, "6/8 and 3/4 where the meter changes");
      CHECK (count_kind (e, EK_TEXT, -1) >= 1, "and its title");
      gr_engraving_svg (e, "/tmp/groove_test_piece.svg", 8, 1);
    }
  gr_engraving_free (e);
  gr_notation_free (nt);

  /* Played in: a legato phrase (each note overlapping the next), short
     notes, getting louder, with one note hit hard. */
  gr_song_free (&s);
  gr_song_init (&s);
  while (s.ntracks)
    gr_track_delete (&s, s.ntracks - 1);
  t = gr_track_add (&s, TRK_INST, "Flute", PATCH_EPIANO);
  {
    int p = gr_pattern_new (&s, "Flute", gr_bar_tick (&s, 4)), i;
    static const gr_note notes[] = {
      /* bar 1, p: four quarters, legato */
      { 0, 104, 72, 45 }, { 96, 104, 74, 47 }, { 192, 104, 76, 46 }, { 288, 96, 77, 48 },
      /* bar 2, mf: eighths played short, one accented */
      { 384, 20, 79, 80 }, { 432, 20, 77, 78 }, { 480, 20, 76, 110 }, { 528, 20, 74, 80 },
      { 576, 192, 72, 82 },
      /* bars 3-4, f */
      { 768, 384, 67, 100 }, { 1152, 384, 72, 100 } };
    gr_region_add (&s, t, 0, gr_bar_tick (&s, 4), p);
    for (i = 0; i < (int) (sizeof notes / sizeof notes[0]); i++)
      gr_pattern_add_note (&s.patterns[p], notes[i].tick, notes[i].len, notes[i].pitch, notes[i].vel);
  }
  nt = gr_notation_make (&s, t, 4);
  e = nt ? gr_engrave (nt, 0, 60 * EG_SP, NULL, s.tempo) : NULL;
  CHECK (e && engraving_sane (e, 4), "Score: a part played in is laid out");
  if (e)
    {
      CHECK (count_kind (e, EK_SLUR, -1) == 1, "the legato phrase under one slur");
      CHECK (count_kind (e, EK_GLYPH, GL_STACCATO_ABOVE) + count_kind (e, EK_GLYPH, GL_STACCATO_BELOW) == 4,
             "the four short notes staccato");
      CHECK (count_kind (e, EK_GLYPH, GL_ACCENT_ABOVE) + count_kind (e, EK_GLYPH, GL_ACCENT_BELOW) == 1,
             "the one hit hard accented");
      CHECK (count_kind (e, EK_GLYPH, GL_DYN_P) == 1 && count_kind (e, EK_GLYPH, GL_DYN_MF) == 1
             && count_kind (e, EK_GLYPH, GL_DYN_F) == 1, "p, then mf, then f");
      gr_engraving_svg (e, "/tmp/groove_test_played.svg", 8, -1);
    }
  gr_engraving_free (e);
  gr_notation_free (nt);
  {
    static gr_song back;
    const char *path = "/tmp/groove_test_played.musicxml", *why;
    size_t len;
    char *xml;
    CHECK (gr_musicxml_export (&s, path) == 0, "MusicXML: a part played in exports");
    xml = (char *) slurp (path, &len);
    CHECK (xml && strstr (xml, "<slur type=\"start\"") && strstr (xml, "<slur type=\"stop\"")
           && strstr (xml, "<staccato/>") && strstr (xml, "<accent/>") && strstr (xml, "release=\"-120\"")
           && strstr (xml, "<dynamics><p/></dynamics>") && strstr (xml, "<dynamics><mf/></dynamics>"),
           "with its slur, staccato (and how short), accent and dynamics");
    free (xml);
    gr_song_init (&back);
    CHECK (gr_score_import (&back, path, 0, &why) == 1 && same_notes (&s, t, &back, 0, 2, 0),
           "and comes back with the staccato notes as short as they were played");
    gr_song_free (&back);
  }

  /* Getting louder then softer, with the sustain pedal held a bar at a
     time (let up and down again at the bar line). */
  gr_song_free (&s);
  gr_song_init (&s);
  while (s.ntracks)
    gr_track_delete (&s, s.ntracks - 1);
  t = gr_track_add (&s, TRK_INST, "Piano", PATCH_EPIANO);
  {
    int p = gr_pattern_new (&s, "Piano", gr_bar_tick (&s, 4)), i, cresc = 0, dim = 0, stop = 0, acc = 0;
    gr_region_add (&s, t, 0, gr_bar_tick (&s, 4), p);
    for (i = 0; i < 8; i++)
      {
        gr_pattern_add_note (&s.patterns[p], i * 48, 48, 60 + i, 40 + i * 10);
        gr_pattern_add_note (&s.patterns[p], 384 + i * 48, 48, 67 - i, 110 - i * 10);
      }
    gr_pattern_add_note (&s.patterns[p], 768, 384, 60, 40);
    gr_pattern_add_note (&s.patterns[p], 1152, 384, 64, 40);
    gr_pattern_set_ctl (&s.patterns[p], 0, 64, 127);
    gr_pattern_set_ctl (&s.patterns[p], 378, 64, 0);
    gr_pattern_set_ctl (&s.patterns[p], 384, 64, 127);
    gr_pattern_set_ctl (&s.patterns[p], 766, 64, 0);
    nt = gr_notation_make (&s, t, 4);
    for (i = 0; nt && i < nt->nev; i++)
      {
        cresc += (nt->ev[i].wedge & NW_CRESC) != 0;
        dim += (nt->ev[i].wedge & NW_DIM) != 0;
        stop += (nt->ev[i].wedge & NW_STOP) != 0;
        acc += (nt->ev[i].artic & NA_ACCENT) != 0;
      }
    CHECK (nt && cresc == 1 && dim == 1 && stop == 2 && !acc,
           "Score: a crescendo then a diminuendo (and the loud notes in them aren't accents)");
    CHECK (nt && nt->parts[0].npedal == 4 && nt->parts[0].pedal[0].down && !nt->parts[0].pedal[1].down
           && nt->parts[0].pedal[1].bar == 1 && nt->parts[0].pedal[1].start == 0 && nt->parts[0].pedal[2].down
           && !nt->parts[0].pedal[3].down && nt->parts[0].pedal[3].bar == 2,
           "the pedal: down, up and down again at the bar line, up");
    e = nt ? gr_engrave (nt, 0, 60 * EG_SP, NULL, s.tempo) : NULL;
    CHECK (e && engraving_sane (e, 4) && count_kind (e, EK_GLYPH, GL_PED) == 2 && count_kind (e, EK_GLYPH, GL_PED_UP) == 2,
           "Ped. and * marks under the staff");
    {
      int k, lines = 0;
      for (k = 0; e && k < e->n; k++)
        lines += e->it[k].kind == EK_BEAM && e->it[k].w == EG_SP * 4 / 25;
      CHECK (lines == 4, "two hairpins, drawn");
    }
    if (e)
      gr_engraving_svg (e, "/tmp/groove_test_hairpins.svg", 8, -1);
    gr_engraving_free (e);
    gr_notation_free (nt);
  }
  {
    static gr_song back;
    const char *path = "/tmp/groove_test_hairpins.musicxml", *why;
    size_t len;
    char *xml;
    int r, ok = 0;
    CHECK (gr_musicxml_export (&s, path) == 0, "MusicXML: hairpins and the pedal export");
    xml = (char *) slurp (path, &len);
    CHECK (xml && strstr (xml, "<wedge type=\"crescendo\"") && strstr (xml, "<wedge type=\"diminuendo\"")
           && strstr (xml, "<wedge type=\"stop\"") && strstr (xml, "<pedal type=\"start\"")
           && strstr (xml, "<pedal type=\"stop\""), "as wedges and pedal marks");
    free (xml);
    gr_song_init (&back);
    CHECK (gr_score_import (&back, path, 0, &why) == 1 && same_notes (&s, t, &back, 0, 2, 0),
           "and come back, notes as they were");
    for (r = 0; r < GR_MAX_REGIONS; r++)
      if (back.regions[r].used && back.regions[r].pattern >= 0)
        {
          const gr_pattern *bp = &back.patterns[back.regions[r].pattern];
          int k, n64 = 0;
          for (k = 0; k < bp->nctls; k++)
            n64 += bp->ctls[k].type == 64;
          ok += n64;
        }
    CHECK (ok == 4, "with the pedal as controller 64");
    gr_song_free (&back);
  }

  /* Spelling: a chromatic scale up is sharps, down is flats; in A minor
     the leading note is G#, and Bb then A is a flat. */
  gr_song_free (&s);
  gr_song_init (&s);
  while (s.ntracks)
    gr_track_delete (&s, s.ntracks - 1);
  t = gr_track_add (&s, TRK_INST, "Piano", PATCH_EPIANO);
  {
    int p = gr_pattern_new (&s, "Piano", gr_bar_tick (&s, 4)), i, up = 1, down = 1, k;
    gr_region_add (&s, t, 0, gr_bar_tick (&s, 4), p);
    for (i = 0; i < 13; i++)
      gr_pattern_add_note (&s.patterns[p], i * 24, 24, 60 + i, 90);
    for (i = 0; i < 13; i++)
      gr_pattern_add_note (&s.patterns[p], 384 + i * 24, 24, 72 - i, 90);
    /* (C major chords, so the key's clear) */
    for (i = 0; i < 3; i++)
      {
        gr_pattern_add_note (&s.patterns[p], 768, 384, 60 + i * 4 - (i == 2), 90);
        gr_pattern_add_note (&s.patterns[p], 1152, 384, 60 + i * 4 - (i == 2), 90);
      }
    nt = gr_notation_make (&s, t, 4);
    for (k = 0; nt && k < nt->nev; k++)
      {
        const gr_nevent *ev = &nt->ev[k];
        int b = 0;
        if (ev->kind != NE_NOTE)
          continue;
        while (b + 1 < nt->nbars && nt->parts[0].voice[(b + 1) * GR_NVOICES].first <= k)
          b++;
        for (i = 0; i < ev->nheads; i++)
          {
            const gr_nhead *h = &nt->hd[ev->head + i];
            if (h->alter && b == 0 && h->alter != 1)
              up = 0;
            if (h->alter && b == 1 && h->alter != -1)
              down = 0;
          }
      }
    CHECK (nt && up && down, "Spelling: a chromatic scale up in sharps, down in flats");
    gr_notation_free (nt);
  }

  /* Instruments: a Bb clarinet written a tone up in D major (for the
     piece's C), unless asked for concert pitch; a guitar an octave up on
     a treble-8 clef; a viola on the alto clef. And back from MusicXML as
     they sound. */
  gr_song_free (&s);
  gr_song_init (&s);
  while (s.ntracks)
    gr_track_delete (&s, s.ntracks - 1);
  {
    static const int gms[3] = { 71, 25, 41 };
    int tk[3], i, j;
    for (i = 0; i < 3; i++)
      {
        int p;
        tk[i] = gr_track_add (&s, TRK_INST, i == 0 ? "Clarinet" : i == 1 ? "Guitar" : "Viola", PATCH_LEAD);
        s.tracks[tk[i]].gm = gms[i];
        p = gr_pattern_new (&s, "x", gr_bar_tick (&s, 2));
        gr_region_add (&s, tk[i], 0, gr_bar_tick (&s, 2), p);
        /* C major: C E G C, F A C, G B D */
        for (j = 0; j < 8; j++)
          {
            static const int tune[8] = { 60, 64, 67, 72, 65, 69, 67, 71 };
            gr_pattern_add_note (&s.patterns[p], j * 96, 96, tune[j] - (i == 1 ? 12 : 0) - (i == 2 ? 7 : 0), 90);
          }
      }
    nt = gr_notation_make (&s, tk[0], 2);
    CHECK (nt && nt->parts[0].fifths == 2 && nt->parts[0].transpose == 2
           && nt->hd[nt->ev[nt->parts[0].voice[0].first].head].step == 1,
           "Clarinet in Bb: written a tone up (C sounds as D), in D major");
    gr_notation_free (nt);
    nt = gr_notation_make_ex (&s, tk[0], 2, GR_NT_CONCERT);
    CHECK (nt && nt->parts[0].fifths == 0 && nt->parts[0].transpose == 0, "or at concert pitch");
    gr_notation_free (nt);
    nt = gr_notation_make (&s, tk[1], 2);
    CHECK (nt && nt->parts[0].clef[0] == NC_G8 && nt->parts[0].transpose == 12
           && nt->hd[nt->ev[nt->parts[0].voice[0].first].head].octave == 4,
           "Guitar: a treble clef with an 8 under it, written an octave up");
    gr_notation_free (nt);
    nt = gr_notation_make (&s, tk[2], 2);
    CHECK (nt && nt->parts[0].clef[0] == NC_C3, "Viola: the alto clef");
    e = nt ? gr_engrave (nt, 0, 60 * EG_SP, NULL, s.tempo) : NULL;
    CHECK (e && count_kind (e, EK_GLYPH, GL_C_CLEF) >= 1, "drawn with a C clef");
    gr_engraving_free (e);
    gr_notation_free (nt);
    {
      static gr_song back;
      const char *path = "/tmp/groove_test_instruments.musicxml", *why;
      size_t len;
      char *xml;
      CHECK (gr_musicxml_export (&s, path) == 0, "MusicXML: transposing instruments export");
      xml = (char *) slurp (path, &len);
      CHECK (xml && strstr (xml, "<transpose><diatonic>-1</diatonic><chromatic>-2</chromatic></transpose>")
             && strstr (xml, "<clef-octave-change>-1</clef-octave-change>") && strstr (xml, "<sign>C</sign><line>3</line>")
             && strstr (xml, "<fifths>2</fifths>"), "with their transpositions, clefs and keys");
      free (xml);
      gr_song_init (&back);
      CHECK (gr_score_import (&back, path, 0, &why) == 3, "and come back, three parts");
      CHECK (same_notes (&s, tk[0], &back, 0, 1, 0), "the clarinet sounding as it did");
      CHECK (same_notes (&s, tk[1], &back, 1, 1, 0), "the guitar sounding as it did");
      CHECK (same_notes (&s, tk[2], &back, 2, 1, 0), "the viola sounding as it did");
      gr_song_free (&back);
    }
  }

  /* Tuplets: crotchet triplets over beats 1-2, a quintuplet on beat 3, a
     sextuplet on beat 4; then a bar of 6/8 with duplets. */
  gr_song_free (&s);
  gr_song_init (&s);
  while (s.ntracks)
    gr_track_delete (&s, s.ntracks - 1);
  gr_set_meter (&s, 1, 6, 8);
  t = gr_track_add (&s, TRK_INST, "Piano", PATCH_EPIANO);
  {
    static const int q5t[6] = { 0, 19, 38, 58, 77, 96 };
    int p = gr_pattern_new (&s, "Piano", gr_bar_tick (&s, 2)), i, k, ctrip = 0, quint = 0, sext = 0, dup = 0;
    gr_region_add (&s, t, 0, gr_bar_tick (&s, 2), p);
    for (i = 0; i < 3; i++)
      gr_pattern_add_note (&s.patterns[p], i * 64, 64, 72 + i, 90);
    for (i = 0; i < 5; i++)
      gr_pattern_add_note (&s.patterns[p], 192 + q5t[i], q5t[i + 1] - q5t[i], 72 + i, 90);
    for (i = 0; i < 6; i++)
      gr_pattern_add_note (&s.patterns[p], 288 + i * 16, 16, 72 + i, 90);
    for (i = 0; i < 4; i++)
      gr_pattern_add_note (&s.patterns[p], 384 + i * 72, 72, 72 + i, 90);
    nt = gr_notation_make (&s, t, 2);
    for (k = 0; nt && k < nt->nev; k++)
      if (nt->ev[k].kind == NE_NOTE && nt->ev[k].tuplet)
        {
          const gr_nevent *ev = &nt->ev[k];
          ctrip += ev->tup_n == 3 && ev->tup_len == 192 && ev->value == NV_QUARTER;
          quint += ev->tup_n == 5 && ev->tup_m == 4 && ev->value == NV_16TH;
          sext += ev->tup_n == 6 && ev->tup_m == 4 && ev->value == NV_16TH;
          dup += ev->tup_n == 2 && ev->tup_m == 3 && ev->value == NV_EIGHTH;
        }
    CHECK (ctrip == 3, "Tuplets: three crotchet triplets over two beats");
    CHECK (quint == 5, "a quintuplet: five semiquavers in a beat");
    CHECK (sext == 6, "a sextuplet: six semiquavers in a beat, marked 6");
    CHECK (dup == 4, "duplets in 6/8: two quavers in a dotted crotchet");
    e = nt ? gr_engrave (nt, 0, 60 * EG_SP, NULL, s.tempo) : NULL;
    CHECK (e && engraving_sane (e, 2) && count_kind (e, EK_GLYPH, GL_TUPLET_5) == 1 && count_kind (e, EK_GLYPH, GL_TUPLET_6) == 1
           && count_kind (e, EK_GLYPH, GL_TUPLET_2) == 2 && count_kind (e, EK_GLYPH, GL_TUPLET_3) == 1,
           "drawn with their numbers: 3, 5, 6, 2 and 2");
    if (e)
      gr_engraving_svg (e, "/tmp/groove_test_tuplets.svg", 8, -1);
    gr_engraving_free (e);
    gr_notation_free (nt);
    {
      static gr_song back;
      const char *path = "/tmp/groove_test_tuplets.musicxml", *why;
      CHECK (gr_musicxml_export (&s, path) == 0, "MusicXML: tuplets export");
      gr_song_init (&back);
      CHECK (gr_score_import (&back, path, 0, &why) == 1 && same_notes (&s, t, &back, 0, 2, 0),
             "and come back where they were");
      gr_song_free (&back);
    }
  }

  /* A track with no notes: four bars' rest, as one multi-bar rest (and
     a box for each bar, for clicks). */
  gr_song_free (&s);
  gr_song_init (&s);
  t = gr_track_add (&s, TRK_INST, "Empty", PATCH_EPIANO);
  nt = gr_notation_make (&s, t, 4);
  e = nt ? gr_engrave (nt, 0, 60 * EG_SP, NULL, s.tempo) : NULL;
  CHECK (e && engraving_sane (e, 4) && count_kind (e, EK_GLYPH, GL_REST_WHOLE) == 0
         && count_kind (e, EK_GLYPH, GL_TIME_4) == 3,
         "Score: a track with no notes shows a 4-bar rest (and its 4/4)");
  gr_engraving_free (e);
  gr_notation_free (nt);
  gr_song_free (&s);
}


/* A track of four crotchets a bar, or eight quavers, for the tests below. */
static int one_part (gr_song *s, const char *name, int gm, int bars)
{
  int t, p;
  gr_song_init (s);
  while (s->ntracks)
    gr_track_delete (s, s->ntracks - 1);
  t = gr_track_add (s, TRK_INST, name, PATCH_EPIANO);
  s->tracks[t].gm = gm;
  p = gr_pattern_new (s, name, gr_bar_tick (s, bars));
  gr_region_add (s, t, 0, gr_bar_tick (s, bars), p);
  return p;
}

static void crotchets (gr_song *s, int p, int bar, int a, int b, int c, int d)
{
  int n[4], k;
  n[0] = a, n[1] = b, n[2] = c, n[3] = d;
  for (k = 0; k < 4; k++)
    gr_pattern_add_note (&s->patterns[p], bar * GR_BAR + k * GR_PPQ, GR_PPQ, n[k], 80);
}

static int has_text (const gr_engraving *e, const char *t)
{
  int i, n = 0;
  for (i = 0; e && i < e->n; i++)
    n += e->it[i].kind == EK_TEXT && !strcmp (e->text + e->it[i].w, t);
  return n;
}

/* Exports s, reads it back and checks the notes are the same. xml: the file. */
static int round_trip (gr_song *s, char **xml)
{
  static gr_song back;
  const char *path = "/tmp/groove_test_rt.musicxml", *why;
  size_t len;
  int ok;
  *xml = NULL;
  if (gr_musicxml_export (s, path))
    return 0;
  gr_song_init (&back);
  ok = gr_score_import (&back, path, 0, &why) == 1 && same_notes (s, 0, &back, 0, 1, 0);
  gr_song_free (&back);
  *xml = (char *) slurp (path, &len);
  return ok;
}

/* A glyph g to the right of bar b's box (after its bar line), level with it:
   a courtesy clef or key at the end of a line. */
static int after_bar (const gr_engraving *e, int b, int g)
{
  int i, j, n = 0;
  for (i = 0; e && i < e->n; i++)
    if (e->it[i].kind == EK_BAR && e->it[i].w == b)
      for (j = 0; j < e->n; j++)
        if (e->it[j].kind == EK_GLYPH && e->it[j].glyph == g && e->it[j].x0 >= e->it[i].x1
            && e->it[j].y0 >= e->it[i].y0 && e->it[j].y0 <= e->it[i].y1)
          n++;
  return n;
}

/* The width (in staff spaces, from lo up) at which bar b starts a line, or 0. */
static int width_breaking_at (const gr_notation *nt, int b, int lo)
{
  int w;
  for (w = lo; w < lo + 60; w++)
    {
      gr_engraving *e = gr_engrave (nt, 0, w * EG_SP, NULL, 100);
      int i, ya = 1, yb = 0;
      for (i = 0; e && i < e->n; i++)
        if (e->it[i].kind == EK_BAR && e->it[i].w == b - 1)
          ya = e->it[i].y0;
        else if (e->it[i].kind == EK_BAR && e->it[i].w == b)
          yb = e->it[i].y0;
      gr_engraving_free (e);
      if (ya != yb)
        return w;
    }
  return 0;
}

static void test_signs (void)
{
  static gr_song s;
  gr_notation *nt;
  gr_engraving *e;
  char *xml;
  int p, b, ok;

  /* A tune in C, then a tone up in D for its last verse: the key changes. */
  p = one_part (&s, "Flute", 73, 20);
  for (b = 0; b < 20; b++)
    {
      static const int c[8] = { 72, 74, 76, 77, 79, 81, 83, 84 }, d[8] = { 74, 76, 78, 79, 81, 83, 85, 86 };
      int j;
      for (j = 0; j < 8; j++)
        gr_pattern_add_note (&s.patterns[p], b * GR_BAR + j * GR_PPQ / 2, GR_PPQ / 2, (b < 8 ? c : d)[(b & 1) ? 7 - j : j], 80);
    }
  nt = gr_notation_make (&s, -1, 1);
  for (b = 0, ok = nt != NULL; ok && b < 20; b++)
    ok = nt->parts[0].key[b] == (b < 8 ? 0 : 2) && nt->bars[b].fifths == (b < 8 ? 0 : 2) && !nt->bars[b].minor;
  CHECK (ok, "Keys: a tune in C, then a tone up: two sharps from the bar it goes up");
  e = nt ? gr_engrave (nt, 0, 70 * EG_SP, NULL, s.tempo) : NULL;
  CHECK (e && engraving_sane (e, 20) && count_kind (e, EK_GLYPH, GL_SHARP) >= 4 && count_kind (e, EK_GLYPH, GL_SHARP) % 2 == 0
         && count_kind (e, EK_GLYPH, GL_NATURAL) == 0,
         "the new key signature drawn where it changes and on each line after");
  gr_engraving_free (e);
  b = nt ? width_breaking_at (nt, 8, 40) : 0;
  e = b ? gr_engrave (nt, 0, b * EG_SP, NULL, s.tempo) : NULL;
  CHECK (e && engraving_sane (e, 20) && after_bar (e, 7, GL_SHARP) == 2,
         "when the new key starts a line, it's shown at the end of the line before too");
  gr_engraving_free (e);
  gr_notation_free (nt);
  CHECK (round_trip (&s, &xml) && xml && strstr (xml, "<key><fifths>0</fifths>")
         && strstr (xml, "<key><fifths>2</fifths>") > strstr (xml, "<key><fifths>0</fifths>"),
         "in MusicXML, the key change, and the notes come back");
  free (xml);
  gr_song_free (&s);

  /* A bar of chromatic notes in a tune in C: no change. */
  p = one_part (&s, "Flute", 73, 8);
  for (b = 0; b < 8; b++)
    if (b == 4)
      crotchets (&s, p, b, 78, 79, 73, 74);
    else
      crotchets (&s, p, b, b & 1 ? 72 : 76, 74, b & 2 ? 77 : 79, 72);
  nt = gr_notation_make (&s, -1, 1);
  for (b = 0, ok = nt != NULL; ok && b < 8; b++)
    ok = nt->parts[0].key[b] == 0;
  CHECK (ok, "but a bar of sharps in a tune in C doesn't change the key");
  gr_notation_free (nt);
  gr_song_free (&s);

  /* A cello going high for four bars: tenor clef, then back to bass. */
  p = one_part (&s, "Cello", 42, 12);
  for (b = 0; b < 12; b++)
    if (b >= 4 && b < 8)
      crotchets (&s, p, b, 64, 67, 69, 65);
    else
      crotchets (&s, p, b, 36, 43, 48, 40);
  nt = gr_notation_make (&s, -1, 1);
  for (b = 0, ok = nt != NULL; ok && b < 12; b++)
    ok = nt->parts[0].clefs[b * 2] == (b >= 4 && b < 8 ? NC_C4 : NC_F) && !nt->parts[0].ottava[b * 2];
  CHECK (ok && nt->parts[0].clef[0] == NC_F, "Clefs: a cello part going high changes to the tenor clef for it, then back");
  e = nt ? gr_engrave (nt, 0, 70 * EG_SP, NULL, s.tempo) : NULL;
  CHECK (e && engraving_sane (e, 12) && count_kind (e, EK_GLYPH, GL_C_CLEF_SMALL) + count_kind (e, EK_GLYPH, GL_C_CLEF) >= 1
         && count_kind (e, EK_GLYPH, GL_F_CLEF_SMALL) + count_kind (e, EK_GLYPH, GL_F_CLEF) >= 2,
         "drawn as a small clef where it changes");
  gr_engraving_free (e);
  b = nt ? width_breaking_at (nt, 4, 30) : 0;
  e = b ? gr_engrave (nt, 0, b * EG_SP, NULL, s.tempo) : NULL;
  CHECK (e && engraving_sane (e, 12) && after_bar (e, 3, GL_C_CLEF_SMALL) == 1,
         "and at the end of the line before, when it changes at the start of one");
  gr_engraving_free (e);
  gr_notation_free (nt);
  CHECK (round_trip (&s, &xml) && xml && strstr (xml, "<sign>C</sign><line>4</line>"), "and a clef change in MusicXML");
  free (xml);
  gr_song_free (&s);

  /* A piano tune going three octaves up for two bars: a 15ma line; an
     octave and more up, an 8va; and one going up and down again in the
     middle of bars: the line starts and ends there. */
  {
    static const struct { int hi[4], ott; } cases[] = { { { 96, 100, 103, 98 }, 2 }, { { 88, 91, 95, 91 }, 1 } };
    int c;
    for (c = 0; c < 2; c++)
      {
        p = one_part (&s, "Piano", 0, 8);
        for (b = 0; b < 8; b++)
          {
            if (b >= 3 && b < 5)
              crotchets (&s, p, b, cases[c].hi[0], cases[c].hi[1], cases[c].hi[2], cases[c].hi[3]);
            else
              crotchets (&s, p, b, 72, 76, 79, 74);
            gr_pattern_add_note (&s.patterns[p], b * GR_BAR, GR_BAR, 48, 70);
          }
        nt = gr_notation_make (&s, -1, 1);
        for (b = 0, ok = nt != NULL && nt->parts[0].staves == 2; ok && b < 8; b++)
          ok = nt->parts[0].ottava[b * 2] == (b >= 3 && b < 5 ? cases[c].ott : 0) && !nt->parts[0].ottava[b * 2 + 1]
            && !nt->parts[0].ott_start[b * 2] && !nt->parts[0].ott_end[b * 2];
        CHECK (ok, c ? "8va: a passage an octave and more above the staff goes under an 8va line"
               : "15ma: one three octaves up goes under a 15ma line");
        e = nt ? gr_engrave (nt, 0, 70 * EG_SP, NULL, s.tempo) : NULL;
        CHECK (e && engraving_sane (e, 8) && count_kind (e, EK_GLYPH, c ? GL_OTTAVA_8VA : GL_QUINDICESIMA_A) >= 1,
               "drawn over the staff");
        gr_engraving_free (e);
        gr_notation_free (nt);
        CHECK (round_trip (&s, &xml) && xml && strstr (xml, c ? "<octave-shift type=\"down\" size=\"8\"/>" : "<octave-shift type=\"down\" size=\"15\"/>")
               && strstr (xml, "<octave-shift type=\"stop\"") && strstr (xml, c ? "<step>B</step><octave>6</octave>" : "<step>C</step><octave>7</octave>"),
               "in MusicXML, an octave-shift with the notes at the pitch they sound, and they come back the same");
        free (xml);
        gr_song_free (&s);
      }
    p = one_part (&s, "Piano", 0, 8);
    for (b = 0; b < 8; b++)
      {
        if (b == 3)
          crotchets (&s, p, b, 72, 76, 88, 91);
        else if (b == 4)
          crotchets (&s, p, b, 95, 91, 93, 95);
        else if (b == 5)
          crotchets (&s, p, b, 91, 88, 76, 72);
        else
          crotchets (&s, p, b, 72, 76, 79, 74);
        gr_pattern_add_note (&s.patterns[p], b * GR_BAR, GR_BAR, 48, 70);
      }
    nt = gr_notation_make (&s, -1, 1);
    for (b = 0, ok = nt != NULL; ok && b < 8; b++)
      ok = nt->parts[0].ottava[b * 2] == (b >= 3 && b < 6);
    CHECK (ok && nt->parts[0].ott_start[3 * 2] == 2 * GR_PPQ && nt->parts[0].ott_end[5 * 2] == 2 * GR_PPQ,
           "an 8va line can start and end in the middle of a bar, at the notes that need it");
    e = nt ? gr_engrave (nt, 0, 70 * EG_SP, NULL, s.tempo) : NULL;
    CHECK (e && engraving_sane (e, 8) && count_kind (e, EK_GLYPH, GL_OTTAVA_8VA) >= 1, "(drawn from the note it starts at)");
    if (e)
      gr_engraving_svg (e, "/tmp/groove_test_8va.svg", 8, -1);
    gr_engraving_free (e);
    gr_notation_free (nt);
    CHECK (round_trip (&s, &xml), "and its notes come back the same from MusicXML");
    free (xml);
    gr_song_free (&s);
  }

  /* Repeats: AB AB, C, D three times, then EFG H EFG I (first and second endings). */
  p = one_part (&s, "Flute", 73, 16);
  crotchets (&s, p, 0, 72, 74, 76, 77);
  crotchets (&s, p, 1, 79, 77, 76, 74);
  crotchets (&s, p, 2, 72, 74, 76, 77);
  crotchets (&s, p, 3, 79, 77, 76, 74);
  crotchets (&s, p, 4, 72, 72, 79, 79);
  for (b = 5; b < 8; b++)
    crotchets (&s, p, b, 81, 81, 79, 79);
  for (b = 8; b < 16; b += 4)
    {
      crotchets (&s, p, b, 77, 77, 76, 76);
      crotchets (&s, p, b + 1, 74, 74, 72, 72);
      crotchets (&s, p, b + 2, 76, 77, 79, 81);
    }
  crotchets (&s, p, 11, 79, 77, 76, 74);
  crotchets (&s, p, 15, 72, 72, 72, 72);
  nt = gr_notation_make_ex (&s, -1, 1, GR_NT_REPEATS);
  {
    static const int bar_of[16] = { 0, 1, 0, 1, 2, 3, 3, 3, 4, 5, 6, 7, 4, 5, 6, 8 };
    for (b = 0, ok = nt && nt->nbars == 9 && nt->nsong == 16; ok && b < 16; b++)
      ok = nt->bar_of[b] == bar_of[b];
    CHECK (ok, "Repeats: music played again written once (16 bars in 9)");
  }
  CHECK (nt && nt->bars[0].rep == NR_START && nt->bars[1].rep == NR_END && nt->bars[1].times == 2
         && nt->bars[3].rep == (NR_START | NR_END) && nt->bars[3].times == 3 && nt->bars[4].rep == NR_START
         && nt->bars[7].ending == 1 && nt->bars[7].rep == NR_END && nt->bars[7].ending_stop == 1
         && nt->bars[8].ending == 2 && nt->bars[8].ending_stop == 2 && nt->bars[7].start == 11 * GR_BAR,
         "two bars twice, a bar three times, and three bars with first and second endings");
  e = nt ? gr_engrave (nt, 0, 70 * EG_SP, NULL, s.tempo) : NULL;
  CHECK (e && engraving_sane (e, 9) && has_text (e, "Play 3 times") == 1 && has_text (e, "1.") == 1 && has_text (e, "2.") == 1,
         "with \"Play 3 times\" and the endings numbered");
  gr_engraving_free (e);
  gr_notation_free (nt);
  nt = gr_notation_make (&s, -1, 1);
  CHECK (nt && nt->nbars == 16 && nt->bar_of[15] == 15, "(written out in full without them)");
  gr_notation_free (nt);
  CHECK (round_trip (&s, &xml) && xml && strstr (xml, "<repeat direction=\"backward\" times=\"3\"/>")
         && strstr (xml, "<ending number=\"1\" type=\"start\">1.</ending>") && strstr (xml, "<ending number=\"2\" type=\"discontinue\"/>"),
         "in MusicXML too, played out note for note when read back");
  free (xml);
  gr_song_free (&s);
}


/* ---- a score's own marks and tempo changes ---------------------------------- */

static char *read_file (const char *path)
{
  FILE *f = fopen (path, "rb");
  long n;
  char *d;
  if (!f)
    return NULL;
  fseek (f, 0, SEEK_END);
  n = ftell (f);
  fseek (f, 0, SEEK_SET);
  d = malloc ((size_t) n + 1);
  if (d && fread (d, 1, (size_t) n, f) != (size_t) n)
    n = 0;
  if (d)
    d[n] = 0;
  fclose (f);
  return d;
}

static int count_marks (const gr_song *s, int kind)
{
  int i, k, n = 0;
  for (i = 0; i < GR_MAX_PATTERNS; i++)
    if (s->patterns[i].used)
      for (k = 0; k < s->patterns[i].nmarks; k++)
        n += s->patterns[i].marks[k].kind == kind;
  return n;
}

static void test_score_marks (void)
{
  static gr_song s, s2;
  const char *why;
  gr_notation *nt;
  gr_engraving *eg;
  gr_engine *e;
  gr_history *h;
  char *xml = NULL;
  int b, k, ok, n = 0;
  float l[256], r[256];

  gr_song_init (&s);
  CHECK (gr_score_import (&s, "host/scores/marks.musicxml", 0, &why) == 1, "Marks: the test score imports");
  CHECK (count_marks (&s, GR_MK_SLUR) == 1 && count_marks (&s, GR_MK_HAIRPIN) == 1 && count_marks (&s, GR_MK_DYNAMIC) == 2
         && count_marks (&s, GR_MK_ARPEGGIO) == 1 && count_marks (&s, GR_MK_STACCATO) == 1
         && count_marks (&s, GR_MK_ACCENT) == 1 && count_marks (&s, GR_MK_OTTAVA) == 1,
         "Marks: the slur, crescendo, two dynamics, spread chord, staccato, accent and 8va line are read");
  CHECK (s.tempo == 100 && s.ntempos == 1 && s.tempos[0].tick == gr_bar_tick (&s, 2) && fabs (s.tempos[0].bpm - 60) < 0.01,
         "Tempo: 100, and a change to 60 from bar 3");
  CHECK (fabs (gr_tick_seconds (&s, gr_bar_tick (&s, 4)) - (4.8 + 8.0)) < 1e-6
         && fabs (gr_seconds_tick (&s, 6.0) - (2 * GR_BAR + 1.2 * GR_PPQ)) < 1e-6,
         "Tempo: two bars at 100 take 4.8 s and two at 60 take 8 s; 6 s in is 1.2 beats into bar 3");

  /* written as the score has it: none of it is in how the notes are played */
  nt = gr_notation_make (&s, -1, 1);
  ok = nt != NULL;
  if (ok)
    {
      const gr_npart *p = &nt->parts[0];
      int slur_s = 0, slur_e = 0, cresc = 0, stop = 0, arp = 0, st = 0, ac = 0, dp = 0, df = 0;
      for (b = 0; b < nt->nbars; b++)
        {
          const gr_nrange *rg = &p->voice[b * GR_NVOICES];
          for (k = 0; k < rg->n; k++)
            {
              const gr_nevent *ev = &nt->ev[rg->first + k];
              slur_s += (ev->slur & NS_START) && b == 0 && ev->start == 0;
              slur_e += (ev->slur & NS_STOP) && b == 0 && ev->start == 3 * GR_PPQ;
              cresc += (ev->wedge & NW_CRESC) && b == 0 && ev->start == 0;
              stop += (ev->wedge & NW_STOP) && b == 0;
              arp += (ev->artic & NA_ARPEGGIO) && b == 1 && ev->start == 0 && ev->nheads == 3;
              st += (ev->artic & NA_STACCATO) && b == 1 && ev->start == 2 * GR_PPQ;
              ac += (ev->artic & NA_ACCENT) && b == 1 && ev->start == 3 * GR_PPQ;
              dp += ev->dyn == ND_P && b == 0;
              df += ev->dyn == ND_F && b == 1;
              n += ev->dyn != 0;
            }
        }
      ok = slur_s == 1 && slur_e == 1 && cresc == 1 && stop == 1 && arp == 1 && st == 1 && ac == 1 && dp == 1 && df == 1 && n == 2;
      CHECK (p->ottava[2 * 2] == 1 && p->ottava[1 * 2] == 0 && p->ottava[3 * 2] == 0, "Marks: the 8va line over bar 3, as written");
      CHECK (nt->bars[2].tempo == 60 && nt->bars[1].tempo == 0, "Marks: the tempo change shown at bar 3");
    }
  CHECK (ok, "Marks: slur, crescendo, p and f, spread chord, staccato and accent on the notes the score puts them");
  eg = nt ? gr_engrave (nt, 0, 70 * EG_SP, NULL, s.tempo) : NULL;
  CHECK (eg && count_kind (eg, EK_GLYPH, GL_ARPEGGIATO) == 1 && count_kind (eg, EK_GLYPH, GL_MET_QUARTER) == 2
         && count_kind (eg, EK_SLUR, 0) >= 1 && count_kind (eg, EK_GLYPH, GL_OTTAVA_8VA) == 1,
         "Engraved: the spread chord's wavy line, two metronome marks, the slur and the 8va");
  gr_engraving_free (eg);
  gr_notation_free (nt);

  /* playing: it slows down at bar 3 */
  e = gr_engine_new (&s);
  gr_play (e);
  for (k = 0; k < GR_RATE * 6 / 256; k++)
    gr_render (e, l, r, 256);
  CHECK (fabs (gr_position (e) - gr_seconds_tick (&s, k * 256.0 / GR_RATE)) < 2.0,
         "Playing: 6 s in it's where the tempo change puts it (1.2 beats into bar 3, not 2 beats)");
  gr_engine_free (e);

  /* kept: saved and loaded, undone and redone, MusicXML and MIDI out */
  CHECK (gr_song_save (&s, "/tmp/groove_marks.song") == 0 && (gr_song_init (&s2), gr_song_load (&s2, "/tmp/groove_marks.song") == 0)
         && s2.ntempos == 1 && fabs (s2.tempos[0].bpm - 60) < 0.01 && count_marks (&s2, GR_MK_SLUR) == 1
         && count_marks (&s2, GR_MK_OTTAVA) == 1 && count_marks (&s2, GR_MK_ARPEGGIO) == 1,
         "Saved and loaded: the tempo change and the marks come back");
  gr_song_free (&s2);
  h = gr_history_new (10, 1 << 24);
  gr_history_reset (h, &s);
  gr_tempo_base (&s, 50);
  for (b = 0; b < GR_MAX_PATTERNS; b++)
    if (s.patterns[b].used)
      s.patterns[b].nmarks = 0;
  gr_history_note (h, &s);
  CHECK (s.tempo == 50 && fabs (s.tempos[0].bpm - 30) < 0.01, "Tempo: halving the starting tempo halves the change too");
  gr_history_undo (h, &s);
  CHECK (s.tempo == 100 && s.ntempos == 1 && fabs (s.tempos[0].bpm - 60) < 0.01 && count_marks (&s, GR_MK_SLUR) == 1,
         "Undo brings back the tempo change and the marks");
  gr_history_free (h);
  CHECK (gr_musicxml_export (&s, "/tmp/groove_marks.musicxml") == 0 && (xml = read_file ("/tmp/groove_marks.musicxml")) != NULL
         && strstr (xml, "<arpeggiate/>") && strstr (xml, "<sound tempo=\"60.00\"/>") && strstr (xml, "octave-shift")
         && strstr (xml, "<slur type=\"start\"") && strstr (xml, "wedge type=\"crescendo\""),
         "MusicXML out: the spread chord, tempo change, 8va, slur and crescendo");
  if (xml)
    {
      gr_song_init (&s2);
      CHECK (gr_score_import (&s2, "/tmp/groove_marks.musicxml", 0, &why) == 1 && s2.ntempos == 1
             && count_marks (&s2, GR_MK_ARPEGGIO) == 1 && count_marks (&s2, GR_MK_OTTAVA) == 1,
             "and Groove reads its own MusicXML back with them");
      gr_song_free (&s2);
    }
  free (xml);
  CHECK (gr_midi_export (&s, "/tmp/groove_marks.mid") == 0 && (gr_song_init (&s2), gr_midi_import (&s2, "/tmp/groove_marks.mid", 0, &why) > 0)
         && s2.tempo == 100 && s2.ntempos == 1 && s2.tempos[0].tick == gr_bar_tick (&s2, 2) && fabs (s2.tempos[0].bpm - 60) < 0.1,
         "MIDI out and back in: the tempo change");
  gr_song_free (&s2);
  gr_song_free (&s);

  /* a tempo change in bars of rest: the multi-bar rest is broken there
     to show it (bars 2-4 and 5-9 rest, 60 from bar 5) */
  {
    int pp = one_part (&s, "Flute", 73, 10), sy, met = 0, rests = 0;
    gr_pattern_add_note (&s.patterns[pp], 0, GR_BAR, 72, 80);
    gr_pattern_add_note (&s.patterns[pp], 9 * GR_BAR, GR_BAR, 72, 80);
    gr_tempo_add (&s, 4 * GR_BAR, 60);
    nt = gr_notation_make (&s, -1, 1);
    eg = nt ? gr_engrave (nt, 0, 70 * EG_SP, NULL, s.tempo) : NULL;
    for (k = 0; eg && k < eg->n; k++)
      {
        met += eg->it[k].kind == EK_GLYPH && eg->it[k].glyph == GL_MET_QUARTER;
        rests += eg->it[k].kind == EK_BEAM;          /* (a multi-bar rest's thick bar) */
      }
    (void) sy;
    CHECK (met == 2 && rests == 2, "A tempo change in bars of rest: two multi-bar rests, the new tempo over the second");
    gr_engraving_free (eg);
    gr_notation_free (nt);
    gr_song_free (&s);
  }

  /* the spread chord plays spread: C, E, G 35 ms apart from bar 2 (2.4 s in) */
  gr_song_init (&s);
  if (gr_score_import (&s, "host/scores/marks.musicxml", 0, &why) == 1)
    {
      int at[3] = { -1, -1, -1 }, pit[3] = { 72, 76, 79 }, j;
      s.tracks[0].midi_out = 1;
      e = gr_engine_new (&s);
      mo_n = 0;
      gr_engine_set_midi_out (e, mo_capture, NULL);
      gr_play (e);
      for (k = 0; k < GR_RATE * 3 / 256; k++)
        gr_render (e, l, r, 256);
      for (k = 0; k < mo_n; k++)
        for (j = 0; j < 3; j++)
          if ((mo_log[k].cmd & 0xF0) == 0x90 && ((mo_log[k].cmd >> 8) & 127) == (unsigned) pit[j]
              && (mo_log[k].cmd >> 16 & 127) && mo_log[k].frame > GR_RATE * 23 / 10 && at[j] < 0)
            at[j] = (int) mo_log[k].frame;
      printf ("  spread chord: C at %d, E at %d, G at %d frames\n", at[0], at[1], at[2]);
      CHECK (at[0] >= 0 && abs (at[0] - GR_RATE * 24 / 10) <= 128 && abs (at[1] - at[0] - GR_RATE * 35 / 1000) <= 128
             && abs (at[2] - at[1] - GR_RATE * 35 / 1000) <= 128,
             "Playing: the spread chord's notes start one after another, from the bottom, 35 ms apart");
      gr_engine_free (e);

      /* exported as MIDI, spread the same way (at 100 bpm 35 ms is 6 ticks) */
      if (gr_midi_export (&s, "/tmp/groove_arp.mid") == 0)
        {
          int ons[3] = { -1, -1, -1 }, i2, kk;
          gr_song_init (&s2);
          if (gr_midi_import (&s2, "/tmp/groove_arp.mid", 0, &why) > 0)
            for (i2 = 0; i2 < GR_MAX_REGIONS; i2++)
              if (s2.regions[i2].used && s2.regions[i2].pattern >= 0)
                for (kk = 0; kk < s2.patterns[s2.regions[i2].pattern].nnotes; kk++)
                  {
                    const gr_note *nn = &s2.patterns[s2.regions[i2].pattern].notes[kk];
                    int on = s2.regions[i2].start + nn->tick;
                    for (j = 0; j < 3; j++)
                      if (nn->pitch == pit[j] && on >= GR_BAR && on < GR_BAR + GR_PPQ)
                        ons[j] = on;
                  }
          CHECK (ons[0] == GR_BAR && ons[1] == GR_BAR + 6 && ons[2] == GR_BAR + 11,
                 "MIDI out: the spread chord's notes start one after another there too");
          gr_song_free (&s2);
        }

      /* spread downwards: an arrowhead at the bottom of the wavy line */
      for (j = 0; j < GR_MAX_PATTERNS; j++)
        if (s.patterns[j].used)
          for (k = 0; k < s.patterns[j].nmarks; k++)
            if (s.patterns[j].marks[k].kind == GR_MK_ARPEGGIO)
              s.patterns[j].marks[k].value = -1;
      nt = gr_notation_make (&s, -1, 1);
      eg = nt ? gr_engrave (nt, 0, 70 * EG_SP, NULL, s.tempo) : NULL;
      CHECK (eg && count_kind (eg, EK_GLYPH, GL_ARPEGGIATO) == 1 && count_kind (eg, EK_GLYPH, GL_ARROWHEAD_DOWN) == 1
             && count_kind (eg, EK_GLYPH, GL_ARROWHEAD_UP) == 0,
             "A chord spread downwards: the wavy line with an arrowhead down");
      gr_engraving_free (eg);
      gr_notation_free (nt);
      e = gr_engine_new (&s);
      mo_n = 0;
      at[0] = at[1] = at[2] = -1;
      gr_engine_set_midi_out (e, mo_capture, NULL);
      gr_play (e);
      for (k = 0; k < GR_RATE * 3 / 256; k++)
        gr_render (e, l, r, 256);
      for (k = 0; k < mo_n; k++)
        for (j = 0; j < 3; j++)
          if ((mo_log[k].cmd & 0xF0) == 0x90 && ((mo_log[k].cmd >> 8) & 127) == (unsigned) pit[j]
              && (mo_log[k].cmd >> 16 & 127) && mo_log[k].frame > GR_RATE * 23 / 10 && at[j] < 0)
            at[j] = (int) mo_log[k].frame;
      CHECK (at[2] >= 0 && at[2] < at[1] && at[1] < at[0], "and played from the top down");
      gr_engine_free (e);
    }
  gr_song_free (&s);

  /* the same score as MuseScore writes it (spanners, Tempo, Ottava) */
  gr_song_init (&s);
  CHECK (gr_score_import (&s, "host/scores/marks.mscx", 0, &why) == 1
         && count_marks (&s, GR_MK_SLUR) == 1 && count_marks (&s, GR_MK_HAIRPIN) == 1 && count_marks (&s, GR_MK_DYNAMIC) == 2
         && count_marks (&s, GR_MK_ARPEGGIO) == 1 && count_marks (&s, GR_MK_STACCATO) == 1
         && count_marks (&s, GR_MK_ACCENT) == 1 && count_marks (&s, GR_MK_OTTAVA) == 1,
         "MuseScore: the slur, crescendo, dynamics, spread chord, staccato, accent and 8va line are read");
  CHECK (s.tempo == 100 && s.ntempos == 1 && s.tempos[0].tick == gr_bar_tick (&s, 2) && fabs (s.tempos[0].bpm - 60) < 0.01,
         "MuseScore: tempo 100, then 60 from bar 3");
  nt = gr_notation_make (&s, -1, 1);
  ok = nt != NULL;
  if (ok)
    {
      const gr_npart *p = &nt->parts[0];
      int sl = 0, cr = 0, ar = 0;
      for (b = 0; b < nt->nbars; b++)
        for (k = 0; k < p->voice[b * GR_NVOICES].n; k++)
          {
            const gr_nevent *ev = &nt->ev[p->voice[b * GR_NVOICES].first + k];
            sl += b == 0 && ((ev->start == 0 && (ev->slur & NS_START)) || (ev->start == 3 * GR_PPQ && (ev->slur & NS_STOP)));
            cr += b == 0 && ((ev->start == 0 && (ev->wedge & NW_CRESC)) || (ev->wedge & NW_STOP));
            ar += b == 1 && ev->start == 0 && (ev->artic & NA_ARPEGGIO);
          }
      ok = sl == 2 && cr == 2 && ar == 1 && p->ottava[2 * 2] == 1 && p->ottava[3 * 2] == 0 && p->ottava[1 * 2] == 0;
    }
  CHECK (ok, "MuseScore: written where the score puts them, the 8va over bar 3 only");
  gr_notation_free (nt);
  gr_song_free (&s);
}

#define SUB_FRAMES 64
/* Notes start and stop on time, to the sample: notes on odd ticks, of odd
   lengths, at 117 bpm, sent to MIDI out, against when they should be. */
static void test_timing (void)
{
  static gr_song s;
  gr_engine *e;
  int t, p, k, n = 0, worst = 0, worst_off = 0;
  float l[256], r[256];
  double spt = GR_RATE * 60.0 / (117.0 * GR_PPQ);
  gr_song_init (&s);
  s.tempo = 117;
  t = gr_track_add (&s, TRK_INST, "Keys", PATCH_EPIANO);
  p = gr_pattern_new (&s, "P", 16 * GR_BAR);
  for (k = 31; k < 100; k++)
    gr_pattern_add_note (&s.patterns[p], (k - 30) * 37, 23 + (k % 5) * 11, k, 100);
  gr_region_add (&s, t, 0, 16 * GR_BAR, p);
  s.tracks[t].midi_out = 1;
  e = gr_engine_new (&s);
  mo_n = 0;
  gr_engine_set_midi_out (e, mo_capture, NULL);
  gr_play (e);
  for (k = 0; k < GR_RATE * 8 / 256; k++)
    gr_render (e, l, r, 256);
  for (k = 0; k < mo_n; k++)
    {
      unsigned st = mo_log[k].cmd & 0xF0, key = (mo_log[k].cmd >> 8) & 127;
      double want;
      if (st == 0x90 && (mo_log[k].cmd >> 16 & 127))
        {
          want = (key - 30) * 37 * spt;
          if (fabs (mo_log[k].frame - want) > worst)
            worst = (int) ceil (fabs (mo_log[k].frame - want));
        }
      else if (st == 0x80)
        {
          want = ((key - 30) * 37 + 23 + (key % 5) * 11) * spt;
          if (mo_log[k].frame - want > worst_off || want - mo_log[k].frame > 1)
            worst_off = (int) ceil (fabs (mo_log[k].frame - want));
        }
      else
        continue;
      n++;
    }
  printf ("  timing: %d note-ons and offs, ons at worst %d frames out, offs %d late\n", n, worst, worst_off);
  CHECK (n > 60 && worst <= 1, "Timing: every note starts to the sample (within one frame of its tick)");
  CHECK (worst_off <= SUB_FRAMES, "Timing: and stops within a block (64 frames, 1.5 ms) of its end, never early");
  gr_engine_free (e);
  gr_song_free (&s);
}

int main (int argc, char **argv)
{
  test_loops ();
  test_render ();
  test_loop_playback ();
  test_words ();
  test_record ();
  test_audio_record ();
  test_files ();
  test_bounce (argc > 1 ? argv[1] : "/tmp/groove_test.wav");
  test_history ();
  test_split_copy ();
  test_count_in_merge ();
  test_meters ();
  test_maestro (getenv ("MAESTRO_DIR"));
  test_gm_names ();
  test_gm_files ();
  test_midi ();
  test_gm_fallback ();
  test_sixteen (getenv ("GROOVE_TEST_SOUNDFONT"));
  test_midi_out ();
  test_long_import ();
  test_drum_keys ();
  test_track_midi ();
  test_controllers ();
  test_unzip_xml ();
  test_score_import ();
  test_musicxml_export ();
  test_engrave ();
  test_signs ();
  test_score_marks ();
  test_timing ();
  if (getenv ("GROOVE_TEST_SOUNDFONT") && *getenv ("GROOVE_TEST_SOUNDFONT"))
    test_gm_play (getenv ("GROOVE_TEST_SOUNDFONT"));
  if (getenv ("GROOVE_TEST_SOUNDFONT") && *getenv ("GROOVE_TEST_SOUNDFONT"))
    test_gm_controllers (getenv ("GROOVE_TEST_SOUNDFONT"));
  else
    printf ("SKIP SoundFont playback: set GROOVE_TEST_SOUNDFONT (make test does)\n");
  gr_gm_unload ();
  printf ("%d checks, %d failed\n", checks, failures);
  return failures != 0;
}

/* Groove engine tests, run on Linux (make test).
   Each CHECK prints what it checked; any failure makes the run exit 1.
   With an argument, also bounces the demo song to that WAV file. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../engine/groove.h"

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
  CHECK (gr_gm_drum_note (DR_KICK) == 36 && gr_gm_drum_note (DR_SNARE) == 38
         && gr_gm_drum_note (DR_CHAT) == 42, "the step grid's rows are GM drum keys");
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
          for (ch = 0; ch < 3; ch++)
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
  CHECK (s.tracks[t7].type == TRK_DRUMS && k == 1 && n[0][2] == 2, "Percussion-Noise on the percussion stave: a hi-hat");
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
static void test_sixteen (const char *sf)
{
  static gr_song s, b;
  gr_engine *e;
  const char *why;
  int t, n, p;
  gr_song_init (&s);
  for (t = 0; t < GR_MAX_TRACKS; t++)
    {
      int k = gr_track_add (&s, t == 3 ? TRK_DRUMS : TRK_INST, "T", PATCH_EPIANO);
      if (k < 0) break;
      s.tracks[k].gm = t == 3 ? 0 : t * 4;
      p = gr_pattern_new (&s, "P", GR_BAR);
      gr_pattern_add_note (&s.patterns[p], 0, GR_PPQ, t == 3 ? 0 : 48 + t, 100);
      gr_region_add (&s, k, 0, GR_BAR, p);
    }
  CHECK (s.ntracks == 16 && gr_track_add (&s, TRK_INST, "17", 0) < 0, "a song holds 16 tracks, and no more");
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
  if (getenv ("GROOVE_TEST_SOUNDFONT") && *getenv ("GROOVE_TEST_SOUNDFONT"))
    test_gm_play (getenv ("GROOVE_TEST_SOUNDFONT"));
  else
    printf ("SKIP SoundFont playback: set GROOVE_TEST_SOUNDFONT (make test does)\n");
  gr_gm_unload ();
  printf ("%d checks, %d failed\n", checks, failures);
  return failures != 0;
}

/* Groove - SoundFont sounds, played by libmidisynth (SoftMIDI's library,
   TinySoundFont underneath).

   There is one synth for the whole program: a SoundFont is several
   megabytes, too much to load twice without need. engine.c plays it like
   a MIDI instrument, one MIDI channel per Groove track (see gm_channel
   there), and mixes what it renders into the song's mix. MIDI has 15
   channels that aren't for drums, so a song whose 16 tracks are all
   SoundFont instruments needs a second synth for the last one: that is
   loaded (from the same SoundFont) only then, by gr_gm_load_second. */
#include <stdio.h>
#include <string.h>
#include "groove.h"
#include "gm_internal.h"
#include "midisynth.h"

static midisynth *synth, *synth2;
static char sf_path[512];
static int sf_default = 1;             /* loaded without a path: libmidisynth's own choice */
static unsigned generation = 1;      /* changes when the synth is replaced */
static char error[160];

int gr_gm_load (const char *soundfont)
{
  midisynth_config c;
  midisynth *ms;
  if (synth && !soundfont)
    return 0;                         /* already loaded */
  midisynth_config_init (&c);
  c.soundfont = soundfont;
  c.sample_rate = GR_RATE;
  c.max_voices = GM_VOICES;
  ms = midisynth_create_ex (&c);
  if (!ms)
    {
      snprintf (error, sizeof error, "%s", midisynth_last_error (NULL));
      return -1;
    }
  gr_gm_unload ();
  synth = ms;
  sf_default = !soundfont;
  snprintf (sf_path, sizeof sf_path, "%s", soundfont ? soundfont : "");
  generation++;
  error[0] = 0;
  return 0;
}

int gr_gm_ready (void) { return synth != NULL; }

void gr_gm_unload (void)
{
  if (synth)
    midisynth_destroy (synth);
  if (synth2)
    midisynth_destroy (synth2);
  synth = synth2 = NULL;
  generation++;
}

int gr_gm_load_second (void)
{
  midisynth_config c;
  midisynth *ms;
  if (!synth)
    return -1;
  if (synth2)
    return 0;
  midisynth_config_init (&c);
  c.soundfont = sf_default ? NULL : sf_path;
  c.sample_rate = GR_RATE;
  c.max_voices = GM_VOICES / 2;
  ms = midisynth_create_ex (&c);
  if (!ms)
    {
      snprintf (error, sizeof error, "%s", midisynth_last_error (NULL));
      return -1;
    }
  synth2 = ms;
  generation++;
  return 0;
}

int gr_gm_need_second (const gr_song *s)
{
  int t, k = 0;
  for (t = 0; t < s->ntracks; t++)
    if (s->tracks[t].type == TRK_INST && k++ >= 15 && s->tracks[t].gm >= 0)
      return 1;               /* the 16th instrument track on (see gm_channel) */
  return 0;
}

int gr_gm_second_ready (void) { return synth2 != NULL; }

const char *gr_gm_error (void) { return error; }

struct midisynth *gm_synth (void) { return synth; }
/* (Channels from 32 on, for a 31st instrument track, have no synth: such a
   track plays Groove's own sound.) */
struct midisynth *gm_synth_n (int n) { return n == 0 ? synth : n == 1 ? synth2 : NULL; }
unsigned gm_generation (void) { return generation; }

void gm_restart (void)
{
  if (synth)
    midisynth_all_notes_off (synth);
  if (synth2)
    midisynth_all_notes_off (synth2);
  generation++;
}

/* ---- names ------------------------------------------------------------------ */

/* The General MIDI Level 1 sound set. */
static const char *const programs[128] = {
  "Acoustic Grand Piano", "Bright Acoustic Piano", "Electric Grand Piano", "Honky-tonk Piano",
  "Electric Piano 1", "Electric Piano 2", "Harpsichord", "Clavinet",
  "Celesta", "Glockenspiel", "Music Box", "Vibraphone",
  "Marimba", "Xylophone", "Tubular Bells", "Dulcimer",
  "Drawbar Organ", "Percussive Organ", "Rock Organ", "Church Organ",
  "Reed Organ", "Accordion", "Harmonica", "Tango Accordion",
  "Nylon Guitar", "Steel Guitar", "Jazz Guitar", "Clean Guitar",
  "Muted Guitar", "Overdriven Guitar", "Distortion Guitar", "Guitar Harmonics",
  "Acoustic Bass", "Fingered Bass", "Picked Bass", "Fretless Bass",
  "Slap Bass 1", "Slap Bass 2", "Synth Bass 1", "Synth Bass 2",
  "Violin", "Viola", "Cello", "Contrabass",
  "Tremolo Strings", "Pizzicato Strings", "Orchestral Harp", "Timpani",
  "String Ensemble 1", "String Ensemble 2", "Synth Strings 1", "Synth Strings 2",
  "Choir Aahs", "Voice Oohs", "Synth Voice", "Orchestra Hit",
  "Trumpet", "Trombone", "Tuba", "Muted Trumpet",
  "French Horn", "Brass Section", "Synth Brass 1", "Synth Brass 2",
  "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax",
  "Oboe", "English Horn", "Bassoon", "Clarinet",
  "Piccolo", "Flute", "Recorder", "Pan Flute",
  "Blown Bottle", "Shakuhachi", "Whistle", "Ocarina",
  "Square Lead", "Sawtooth Lead", "Calliope Lead", "Chiff Lead",
  "Charang Lead", "Voice Lead", "Fifths Lead", "Bass + Lead",
  "New Age Pad", "Warm Pad", "Polysynth Pad", "Choir Pad",
  "Bowed Pad", "Metallic Pad", "Halo Pad", "Sweep Pad",
  "Rain", "Soundtrack", "Crystal", "Atmosphere",
  "Brightness", "Goblins", "Echoes", "Sci-fi",
  "Sitar", "Banjo", "Shamisen", "Koto",
  "Kalimba", "Bagpipe", "Fiddle", "Shanai",
  "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock",
  "Taiko Drum", "Melodic Tom", "Synth Drum", "Reverse Cymbal",
  "Guitar Fret Noise", "Breath Noise", "Seashore", "Bird Tweet",
  "Telephone Ring", "Helicopter", "Applause", "Gunshot",
};

/* The General MIDI families, in its order. GM calls family 13 (Sitar,
   Banjo, Shamisen, Koto, Kalimba, Bagpipe, Fiddle, Shanai) "Ethnic";
   Groove says "World", as most current software does. */
static const char *const families[16] = {
  "Piano", "Chromatic percussion", "Organ", "Guitar", "Bass", "Strings",
  "Ensemble", "Brass", "Reed", "Pipe", "Synth lead", "Synth pad",
  "Synth effects", "World", "Percussive", "Sound effects",
};

static const struct { int program; const char *name; } kits[GM_NKITS] = {
  { GM_KIT_STANDARD, "Standard Kit" }, { GM_KIT_ROOM, "Room Kit" },
  { GM_KIT_POWER, "Power Kit" }, { GM_KIT_ELECTRONIC, "Electronic Kit" },
  { GM_KIT_808, "808 Kit" }, { GM_KIT_JAZZ, "Jazz Kit" },
  { GM_KIT_BRUSH, "Brush Kit" }, { GM_KIT_ORCHESTRA, "Orchestra Kit" },
};

const char *gr_gm_program_name (int p) { return p >= 0 && p < 128 ? programs[p] : "?"; }
const char *gr_gm_family_name (int f) { return f >= 0 && f < 16 ? families[f] : "?"; }
int gr_gm_kit (int i) { return i >= 0 && i < GM_NKITS ? kits[i].program : 0; }

const char *gr_gm_kit_name (int program)
{
  int i;
  for (i = 0; i < GM_NKITS; i++)
    if (kits[i].program == program)
      return kits[i].name;
  return "Standard Kit";       /* (a SoundFont without that kit plays the standard one) */
}

/* The step grid's eight rows, as General MIDI drum keys. */
int gr_gm_drum_note (int sound)
{
  static const int key[GR_DRUM_SOUNDS] = { 36, 38, 42, 46, 39, 45, 50, 56 };
  return sound >= 0 && sound < GR_DRUM_SOUNDS ? key[sound] : 36;
}

int gr_song_uses_gm (const gr_song *s)
{
  int t;
  for (t = 0; t < s->ntracks; t++)
    if (s->tracks[t].gm >= 0 && s->tracks[t].type != TRK_AUDIO)
      return 1;
  return 0;
}

const char *gr_sound_name (const gr_track *t)
{
  if (t->type == TRK_AUDIO)
    return "Audio";
  if (t->gm >= 0)
    return t->type == TRK_DRUMS ? gr_gm_kit_name (t->gm) : gr_gm_program_name (t->gm);
  return t->type == TRK_DRUMS ? gr_kit_name (t->patch) : gr_patch_name (t->patch);
}

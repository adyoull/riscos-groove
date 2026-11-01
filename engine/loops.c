/* Groove - the built-in loop library (the "Apple Loops" of Groove).

   Every loop is MIDI-like data, not audio, so it is tiny, follows the song's
   tempo and key, and can be opened in the editor and changed.

   Drum loops are written as step strings, one per drum sound, a character
   per sixteenth:  'X' accent, 'x' normal, 'o' soft (ghost), '.' rest.
   Melodic loops are lists of {start, length, note} in sixteenths, with the
   note in semitones above the song's key (written in C). */
#include <stdio.h>
#include <string.h>
#include "groove.h"

typedef struct { int sound; const char *steps; } drum_row;
typedef struct { int16_t at, len; int8_t note; uint8_t vel; } mnote;

typedef struct
{
  gr_loop_info info;
  const drum_row *rows;         /* drums: rows ended by sound -1 */
  const mnote *notes;           /* melodic: ended by len 0 */
  int octave;                   /* MIDI note of the key's root (C3=48, C4=60) */
} loop_def;

/* ---- drums ---------------------------------------------------------- */
static const drum_row four_floor[] = {
  { DR_KICK,  "X...x...X...x..." },
  { DR_CLAP,  "....X.......X..." },
  { DR_CHAT,  "x.x.x.x.x.x.x.x." },
  { DR_OHAT,  "..o...o...o...o." }, { -1, 0 } };
static const drum_row boom_bap[] = {
  { DR_KICK,  "X......x..X....." },
  { DR_SNARE, "....X.......X..o" },
  { DR_CHAT,  "x.x.x.x.x.x.x.xo" }, { -1, 0 } };
static const drum_row rock[] = {
  { DR_KICK,  "X.......X.x....." },
  { DR_SNARE, "....X.......X..." },
  { DR_CHAT,  "x.x.x.x.x.x.x.x." }, { -1, 0 } };
static const drum_row breakbeat[] = {
  { DR_KICK,  "X.........X.....X.x.......x....." },
  { DR_SNARE, "....X..o.o..X..o....X..o.o..X..." },
  { DR_CHAT,  "x.x.x.x.x.x.x.x.x.x.x.x.x.x.o.x." },
  { DR_OHAT,  "..............x................." }, { -1, 0 } };
static const drum_row trap[] = {
  { DR_KICK,  "X......x..X.....X.........x....." },
  { DR_SNARE, "........X...............X......." },
  { DR_CHAT,  "x.x.x.x.xxx.x.x.x.x.x.x.x.xxxxxx" }, { -1, 0 } };
static const drum_row reggaeton[] = {
  { DR_KICK,  "X...X...X...X..." },
  { DR_SNARE, "...x..x....x..x." },
  { DR_CHAT,  "x.x.x.x.x.x.x.x." },
  { DR_PERC,  "......o.......o." }, { -1, 0 } };
static const drum_row disco[] = {
  { DR_KICK,  "X...X...X...X..." },
  { DR_SNARE, "....X.......X..." },
  { DR_OHAT,  "..x...x...x...x." },
  { DR_CHAT,  "x...x...x...x..." },
  { DR_PERC,  "x..x..x...x..x.." }, { -1, 0 } };
static const drum_row toms_fill[] = {
  { DR_SNARE, "X.x.x.x........." },
  { DR_HTOM,  "........X.x....." },
  { DR_LTOM,  "............X.x." },
  { DR_KICK,  "X..............X" }, { -1, 0 } };

/* ---- bass (semitones above the key) --------------------------------- */
static const mnote root_pump[] = {
  {0,2,0,110},{2,2,0,80},{4,2,0,100},{6,2,0,80},{8,2,0,110},{10,2,0,80},{12,2,0,100},{14,2,0,80},
  {16,2,-4,110},{18,2,-4,80},{20,2,-4,100},{22,2,-4,80},{24,2,-5,110},{26,2,-5,80},{28,2,-5,100},{30,2,-5,80},
  {0,0,0,0} };
static const mnote octave_bounce[] = {
  {0,2,0,115},{2,2,12,90},{4,2,0,110},{6,2,12,90},{8,2,0,115},{10,2,12,90},{12,2,0,110},{14,2,12,95},
  {0,0,0,0} };
static const mnote walking[] = {
  {0,4,0,105},{4,4,4,95},{8,4,7,100},{12,4,9,95},
  {16,4,5,105},{20,4,9,95},{24,4,12,100},{28,4,11,95},
  {0,0,0,0} };
static const mnote funk_bass[] = {
  {0,1,0,120},{3,1,0,90},{4,1,12,100},{6,2,10,95},{9,1,0,110},{10,1,7,90},{12,2,10,100},{14,1,12,110},
  {0,0,0,0} };

/* ---- keys ------------------------------------------------------------- */
/* I - V - vi - IV, a bar each: the four chords of a thousand pop songs. */
static const mnote pop_chords[] = {
  {0,15,0,95},{0,15,4,85},{0,15,7,85},
  {16,15,-5,95},{16,15,-1,85},{16,15,2,85},
  {32,15,-3,95},{32,15,0,85},{32,15,4,85},
  {48,15,-7,95},{48,15,-3,85},{48,15,0,85},
  {0,0,0,0} };
/* i - VI - III - VII in the relative minor feel, long pads. */
static const mnote minor_pads[] = {
  {0,16,0,90},{0,16,3,80},{0,16,7,80},
  {16,16,-4,90},{16,16,0,80},{16,16,3,80},
  {32,16,3,90},{32,16,7,80},{32,16,10,80},
  {48,16,-2,90},{48,16,2,80},{48,16,5,80},
  {0,0,0,0} };
static const mnote arp_up[] = {
  {0,2,0,100},{2,2,4,85},{4,2,7,90},{6,2,12,85},{8,2,7,90},{10,2,4,85},{12,2,0,95},{14,2,4,85},
  {16,2,-3,100},{18,2,0,85},{20,2,4,90},{22,2,9,85},{24,2,4,90},{26,2,0,85},{28,2,-3,95},{30,2,0,85},
  {0,0,0,0} };
static const mnote offbeat_stabs[] = {
  {2,1,0,105},{2,1,4,95},{2,1,7,95},{6,1,0,100},{6,1,4,90},{6,1,7,90},
  {10,1,0,105},{10,1,4,95},{10,1,7,95},{14,1,0,100},{14,1,4,90},{14,1,7,90},
  {0,0,0,0} };

/* ---- lead ------------------------------------------------------------- */
static const mnote hook[] = {
  {0,2,12,110},{2,2,16,95},{4,4,19,105},{8,2,16,95},{10,2,14,90},{12,4,12,100},
  {16,2,9,105},{18,2,12,95},{20,6,14,105},{28,4,7,90},
  {0,0,0,0} };
static const mnote bell_motif[] = {
  {0,4,19,100},{4,4,16,90},{8,4,12,95},{12,4,16,90},
  {16,4,21,100},{20,4,19,90},{24,8,16,95},
  {0,0,0,0} };

#define DRUMS(n, k, bars, rows)  { { n, "Drums", TRK_DRUMS, k, bars }, rows, NULL, 0 }
#define MEL(n, c, p, bars, notes, oct) { { n, c, TRK_INST, p, bars }, NULL, notes, oct }

static const loop_def loops[] = {
  DRUMS ("Four on the Floor", KIT_ELECTRO, 1, four_floor),
  DRUMS ("Boom Bap",          KIT_STUDIO,  1, boom_bap),
  DRUMS ("Rock Steady",       KIT_STUDIO,  1, rock),
  DRUMS ("Amen-ish Break",    KIT_STUDIO,  2, breakbeat),
  DRUMS ("Half-time Trap",    KIT_ELECTRO, 2, trap),
  DRUMS ("Dembow",            KIT_ELECTRO, 1, reggaeton),
  DRUMS ("Disco Shuffle",     KIT_STUDIO,  1, disco),
  DRUMS ("Tom Fill",          KIT_STUDIO,  1, toms_fill),
  MEL ("Root Pump",      "Bass", PATCH_BASS,   2, root_pump, 36),
  MEL ("Octave Bounce",  "Bass", PATCH_BASS,   1, octave_bounce, 36),
  MEL ("Walking Bass",   "Bass", PATCH_PLUCK,  2, walking, 36),
  MEL ("Funk Pop",       "Bass", PATCH_BASS,   1, funk_bass, 36),
  MEL ("Pop Chords",     "Keys", PATCH_EPIANO, 4, pop_chords, 60),
  MEL ("Minor Pads",     "Keys", PATCH_PAD,    4, minor_pads, 60),
  MEL ("Arpeggio",       "Keys", PATCH_PLUCK,  2, arp_up, 60),
  MEL ("Offbeat Stabs",  "Keys", PATCH_ORGAN,  1, offbeat_stabs, 60),
  MEL ("Hook Melody",    "Lead", PATCH_LEAD,   2, hook, 60),
  MEL ("Bell Motif",     "Lead", PATCH_BELL,   2, bell_motif, 60),
};

#define NLOOPS ((int) (sizeof loops / sizeof loops[0]))

int gr_loop_count (void) { return NLOOPS; }
const gr_loop_info *gr_loop_get (int i) { return i >= 0 && i < NLOOPS ? &loops[i].info : NULL; }

int gr_loop_fill (int i, int key, gr_pattern *p)
{
  const loop_def *d;
  if (i < 0 || i >= NLOOPS)
    return -1;
  d = &loops[i];
  memset (p, 0, sizeof *p);
  p->used = 1;
  p->len = d->info.bars * GR_BAR;
  snprintf (p->name, GR_NAME, "%s", d->info.name);
  if (d->rows)
    {
      const drum_row *r;
      for (r = d->rows; r->sound >= 0; r++)
        {
          /* A one-bar row in a two-bar loop repeats. */
          int n = (int) strlen (r->steps), j;
          for (j = 0; j < d->info.bars * 16; j++)
            {
              char c = r->steps[j % n];
              int vel = c == 'X' ? 120 : c == 'x' ? 95 : c == 'o' ? 55 : 0;
              if (vel)
                gr_pattern_add_note (p, j * GR_STEP, GR_STEP, r->sound, vel);
            }
        }
    }
  else
    {
      const mnote *n;
      /* Keep the key's root within a fifth of the written octave, so a song
         in A doesn't push the bass up nearly an octave. */
      int root = d->octave + (key > 6 ? key - 12 : key);
      for (n = d->notes; n->len > 0; n++)
        gr_pattern_add_note (p, n->at * GR_STEP, n->len * GR_STEP - 2, root + n->note, n->vel);
    }
  return 0;
}

int gr_loop_make (gr_song *s, int i)
{
  int pi;
  if (i < 0 || i >= NLOOPS)
    return -1;
  pi = gr_pattern_new (s, loops[i].info.name, loops[i].info.bars * GR_BAR);
  if (pi >= 0)
    gr_loop_fill (i, s->key, &s->patterns[pi]);
  return pi;
}

int gr_loop_place (gr_song *s, int i, int track, int tick)
{
  const loop_def *d;
  int p, r;
  if (i < 0 || i >= NLOOPS)
    return -1;
  d = &loops[i];
  if (track >= 0 && track < s->ntracks && s->tracks[track].type != d->info.type)
    track = -1;                 /* a drum loop on a piano track makes a new track */
  if (track < 0 || track >= s->ntracks)
    {
      char name[GR_NAME];
      if (d->info.type == TRK_DRUMS)
        snprintf (name, sizeof name, "Drums");
      else
        snprintf (name, sizeof name, "%s", gr_patch_name (d->info.patch));
      track = gr_track_add (s, d->info.type, name, d->info.patch);
      if (track < 0)
        return -1;
    }
  p = gr_loop_make (s, i);
  if (p < 0)
    return -1;
  /* Snap to the nearest bar when a loop is dropped. */
  tick = gr_bar_snap (s, tick);
  r = gr_region_add (s, track, tick, s->patterns[p].len, p);
  if (r < 0)
    s->patterns[p].used = 0;
  return r;
}

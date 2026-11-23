/* Groove tests: import a score file (MuseScore, MusicXML or MIDI) and
   write what the Score window would show as SVG, to look at.

     out/render_score FILE OUT.svg [TRACK|-1] [width in staff spaces]  */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../engine/groove.h"
#include "../engine/notation.h"
#include "../engine/engrave.h"

int gr_engraving_svg (const gr_engraving *e, const char *path, double px_per_sp, int highlight_bar);
int gr_score_import (gr_song *s, const char *path, int tick, const char **why);
int gr_midi_import (gr_song *s, const char *path, int tick, const char **why);

int main (int argc, char **argv)
{
  static gr_song s;
  const char *why = NULL;
  int track = argc > 3 ? atoi (argv[3]) : -1, width = argc > 4 ? atoi (argv[4]) : 120, n, bars;
  gr_notation *nt;
  gr_engraving *e;
  if (argc < 3)
    return fprintf (stderr, "usage: %s FILE OUT.svg [TRACK] [WIDTH]\n       %s FILE OUT.musicxml (played out: no repeats)\n", argv[0], argv[0]), 2;
  gr_song_init (&s);
  n = gr_score_import (&s, argv[1], 0, &why);
  if (n < 0)
    return fprintf (stderr, "import: %s\n", why ? why : "?"), 1;
  if (strstr (argv[2], ".musicxml"))
    {
      int r = gr_musicxml_export_ex (&s, argv[2], 0);
      gr_song_free (&s);
      return r ? 1 : 0;
    }
  bars = gr_tick_bar (&s, gr_song_end (&s) > 0 ? gr_song_end (&s) - 1 : 0) + 1;
  fprintf (stderr, "%d tracks, %d bars%s%s\n", n, bars, why ? ": " : "", why ? why : "");
  nt = gr_notation_make_ex (&s, track, bars, GR_NT_REPEATS);
  if (!nt)
    return fprintf (stderr, "no notation\n"), 1;
  {
    int parts[64], i, np = nt->nparts < 64 ? nt->nparts : 64;
    for (i = 0; i < np; i++)
      parts[i] = i;
    e = gr_engrave_parts (nt, parts, np, width * EG_SP, "Score", s.tempo, EG_MULTIRESTS);
  }
  if (!e)
    return 1;
  gr_engraving_svg (e, argv[2], 8, argc > 5 ? atoi (argv[5]) : -1);
  fprintf (stderr, "%d systems\n", e->nsys);
  for (n = 0; n < nt->nparts; n++)
    fprintf (stderr, "part %d: track %d %s, %d staves\n", n, nt->parts[n].track, nt->parts[n].name, nt->parts[n].staves);
  gr_engraving_free (e);
  gr_notation_free (nt);
  gr_song_free (&s);
  return 0;
}

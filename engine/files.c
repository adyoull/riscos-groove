/* Groove - song files and WAV files.

   A song is a short text file, so it survives being emailed, diffed and
   read by a person:

     GROOVE 1
     tempo 110
     key 0
     bars 16
     loop 0 0 1536
     metronome 0
     master 0.80
     track 0 1 "Drums" 0.80 0.00 0.08 0 0 -1    type patch name vol pan reverb mute solo
                                                 sound (-1 Groove's, else SoundFont program/kit)
     pattern 3 "Boom Bap" 384                    index name length, then its notes:
     n 0 24 0 120                                tick length pitch velocity
     end
     region 0 0 384 3 -1 0                       track start length pattern clip offset
     clip 0 "Recording" 44100                    index name rate

   A clip's audio is saved beside the song as a WAV file, "<song>_A<index>".
   On RISC OS UnixLib maps the names, so "Song_A0" is just another leaf. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "groove.h"

/* ---- WAV --------------------------------------------------------------- */

static void put16 (unsigned char *p, unsigned v) { p[0] = v & 255; p[1] = (v >> 8) & 255; }
static void put32 (unsigned char *p, unsigned long v)
{ p[0] = v & 255; p[1] = (v >> 8) & 255; p[2] = (v >> 16) & 255; p[3] = (v >> 24) & 255; }
static unsigned get16 (const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned long get32 (const unsigned char *p)
{ return p[0] | (p[1] << 8) | ((unsigned long) p[2] << 16) | ((unsigned long) p[3] << 24); }

int gr_wav_write (const char *path, const int16_t *stereo, int frames, int rate)
{
  unsigned char h[44], buf[4096];
  FILE *f = fopen (path, "wb");
  int i, n = 0;
  if (!f)
    return -1;
  memcpy (h, "RIFF", 4); put32 (h + 4, 36 + (unsigned long) frames * 4);
  memcpy (h + 8, "WAVEfmt ", 8); put32 (h + 16, 16); put16 (h + 20, 1); put16 (h + 22, 2);
  put32 (h + 24, (unsigned long) rate); put32 (h + 28, (unsigned long) rate * 4);
  put16 (h + 32, 4); put16 (h + 34, 16);
  memcpy (h + 36, "data", 4); put32 (h + 40, (unsigned long) frames * 4);
  fwrite (h, 1, 44, f);
  /* Byte by byte: little-endian on every machine, and no unaligned access. */
  for (i = 0; i < frames * 2; i++)
    {
      put16 (buf + n, (unsigned) (uint16_t) stereo[i]);
      n += 2;
      if (n == sizeof buf)
        {
          fwrite (buf, 1, n, f);
          n = 0;
        }
    }
  if (n)
    fwrite (buf, 1, n, f);
  return fclose (f) ? -1 : 0;
}

/* Reads 8/16/24-bit PCM, mono or stereo, into 16-bit stereo. */
int gr_wav_read (const char *path, int16_t **out, int *frames, int *rate)
{
  unsigned char h[12], ck[8], fmt[16];
  FILE *f = fopen (path, "rb");
  int channels = 0, bits = 0, have_fmt = 0;
  *out = NULL;
  if (!f)
    return -1;
  if (fread (h, 1, 12, f) != 12 || memcmp (h, "RIFF", 4) || memcmp (h + 8, "WAVE", 4))
    goto bad;
  while (fread (ck, 1, 8, f) == 8)
    {
      unsigned long len = get32 (ck + 4);
      if (!memcmp (ck, "fmt ", 4) && len >= 16)
        {
          if (fread (fmt, 1, 16, f) != 16 || get16 (fmt) != 1)
            goto bad;             /* PCM only */
          channels = (int) get16 (fmt + 2);
          *rate = (int) get32 (fmt + 4);
          bits = (int) get16 (fmt + 14);
          have_fmt = 1;
          fseek (f, (long) (len - 16 + (len & 1)), SEEK_CUR);
        }
      else if (!memcmp (ck, "data", 4) && have_fmt)
        {
          int bps = bits / 8, i, n;
          unsigned char *raw;
          if (channels < 1 || channels > 2 || bps < 1 || bps > 3)
            goto bad;
          n = (int) (len / (unsigned long) (bps * channels));
          raw = malloc (len);
          *out = malloc ((size_t) n * 2 * sizeof (int16_t));
          if (!raw || !*out || fread (raw, 1, len, f) != len)
            {
              free (raw);
              goto bad;
            }
          for (i = 0; i < n * channels; i++)
            {
              const unsigned char *p = raw + i * bps;
              int v = bps == 1 ? ((int) p[0] - 128) << 8
                    : bps == 2 ? (int16_t) get16 (p)
                    : (int16_t) get16 (p + 1);
              if (channels == 1)
                (*out)[2 * i] = (*out)[2 * i + 1] = (int16_t) v;
              else
                (*out)[i] = (int16_t) v;
            }
          free (raw);
          *frames = n;
          fclose (f);
          return 0;
        }
      else
        fseek (f, (long) (len + (len & 1)), SEEK_CUR);
    }
bad:
  free (*out);
  *out = NULL;
  fclose (f);
  return -1;
}

int gr_wav_import (gr_song *s, const char *path, int track, int tick)
{
  int16_t *pcm;
  int frames, rate, c, r;
  const char *leaf = strrchr (path, '.');
  if (gr_wav_read (path, &pcm, &frames, &rate))
    return -1;
  if (track < 0 || track >= s->ntracks || s->tracks[track].type != TRK_AUDIO)
    track = gr_track_add (s, TRK_AUDIO, "Audio", 0);
  c = track < 0 ? -1 : gr_clip_new (s, leaf ? leaf + 1 : path, rate);
  if (c < 0)
    {
      free (pcm);
      return -1;
    }
  s->clips[c].data = pcm;
  s->clips[c].frames = s->clips[c].cap = frames;
  tick = gr_bar_snap (s, tick);
  r = gr_region_add (s, track, tick,
                     (int) ((double) frames / rate * s->tempo / 60.0 * GR_PPQ) + 1, -1);
  if (r < 0)
    {
      gr_song_gc (s);
      return -1;
    }
  s->regions[r].clip = c;
  return r;
}

/* ---- song files --------------------------------------------------------- */

static void clip_path (char *out, size_t n, const char *song, int i)
{
  snprintf (out, n, "%s_A%d", song, i);
}

/* Names are written quoted; quotes inside become apostrophes. */
static void put_name (FILE *f, const char *name)
{
  fputc ('"', f);
  for (; *name; name++)
    fputc (*name == '"' ? '\'' : *name, f);
  fputc ('"', f);
}

static const char *get_name (const char *p, char *out)
{
  int n = 0;
  while (*p == ' ') p++;
  if (*p != '"')
    {
      out[0] = 0;
      return p;
    }
  for (p++; *p && *p != '"'; p++)
    if (n < GR_NAME - 1)
      out[n++] = *p;
  out[n] = 0;
  return *p ? p + 1 : p;
}

int gr_song_save (const gr_song *s, const char *path)
{
  FILE *f = fopen (path, "w");
  int i, k;
  if (!f)
    return -1;
  fprintf (f, "GROOVE 1\ntempo %d\nkey %d\nbars %d\nloop %d %d %d\nmetronome %d\nmaster %.2f\n",
           s->tempo, s->key, s->bars, s->loop_on, s->loop_start, s->loop_end,
           s->metronome, s->master);
  /* Time signatures, from bar 0. (A plain 4/4 song has none: older
     Groove reads it unchanged.) */
  for (i = 0; i < s->nmeters; i++)
    if (i > 0 || s->meters[i].num != 4 || s->meters[i].den != 4)
      fprintf (f, "meter %d %d %d\n", s->meters[i].bar, s->meters[i].num, s->meters[i].den);
  for (i = 0; i < s->ntracks; i++)
    {
      const gr_track *t = &s->tracks[i];
      fprintf (f, "track %d %d ", (int) t->type, t->patch);
      put_name (f, t->name);
      fprintf (f, " %.2f %.2f %.2f %d %d %d\n", t->vol, t->pan, t->reverb, t->mute, t->solo, t->gm);
    }
  for (i = 0; i < GR_MAX_PATTERNS; i++)
    {
      const gr_pattern *p = &s->patterns[i];
      if (!p->used)
        continue;
      fprintf (f, "pattern %d ", i);
      put_name (f, p->name);
      fprintf (f, " %d\n", p->len);
      for (k = 0; k < p->nnotes; k++)
        fprintf (f, "n %d %d %d %d\n", p->notes[k].tick, p->notes[k].len,
                 p->notes[k].pitch, p->notes[k].vel);
      fprintf (f, "end\n");
    }
  for (i = 0; i < GR_MAX_CLIPS; i++)
    {
      const gr_clip *c = &s->clips[i];
      char wav[256];
      if (!c->used)
        continue;
      clip_path (wav, sizeof wav, path, i);
      if (gr_wav_write (wav, c->data, c->frames, c->rate))
        {
          fclose (f);
          return -1;
        }
      fprintf (f, "clip %d ", i);
      put_name (f, c->name);
      fprintf (f, " %d\n", c->rate);
    }
  for (i = 0; i < GR_MAX_REGIONS; i++)
    {
      const gr_region *r = &s->regions[i];
      if (r->used)
        fprintf (f, "region %d %d %d %d %d %d\n", r->track, r->start, r->len,
                 r->pattern, r->clip, r->clip_offset);
    }
  return fclose (f) ? -1 : 0;
}

int gr_song_load (gr_song *s, const char *path)
{
  FILE *f = fopen (path, "r");
  char line[256], name[GR_NAME];
  gr_pattern *cur = NULL;
  int version = 0;
  if (!f)
    return -1;
  gr_song_free (s);
  gr_song_init (s);
  s->bars = 0;
  while (fgets (line, sizeof line, f))
    {
      int a, b, c, d, e, g;
      float x, y, z;
      const char *rest;
      if (sscanf (line, "GROOVE %d", &version) == 1) continue;
      if (version == 0) break;                  /* not a song file */
      if (cur)
        {
          if (sscanf (line, "n %d %d %d %d", &a, &b, &c, &d) == 4)
            gr_pattern_add_note (cur, a, b, c, d);
          else if (!strncmp (line, "end", 3))
            cur = NULL;
          continue;
        }
      if (sscanf (line, "tempo %d", &a) == 1) s->tempo = a < 40 ? 40 : a > 240 ? 240 : a;
      else if (sscanf (line, "key %d", &a) == 1) s->key = ((a % 12) + 12) % 12;
      else if (sscanf (line, "bars %d", &a) == 1) s->bars = a;
      else if (sscanf (line, "loop %d %d %d", &a, &b, &c) == 3)
        { s->loop_on = a; s->loop_start = b; s->loop_end = c; }
      else if (sscanf (line, "metronome %d", &a) == 1) s->metronome = a;
      else if (sscanf (line, "master %f", &x) == 1) s->master = x;
      else if (sscanf (line, "meter %d %d %d", &a, &b, &c) == 3) gr_set_meter (s, a, b, c);
      else if (sscanf (line, "track %d %d", &a, &b) == 2)
        {
          int t;
          rest = strchr (line + 6, '"');
          rest = get_name (rest ? rest : "", name);
          t = gr_track_add (s, (gr_track_type) a, name, b);
          if (t >= 0 && sscanf (rest, "%f %f %f %d %d", &x, &y, &z, &c, &d) == 5)
            {
              int g;
              s->tracks[t].vol = x; s->tracks[t].pan = y; s->tracks[t].reverb = z;
              s->tracks[t].mute = c; s->tracks[t].solo = d;
              /* (songs from before SoundFont sounds have no seventh field) */
              if (sscanf (rest, "%*f %*f %*f %*d %*d %d", &g) == 1 && g >= -1 && g <= 127)
                s->tracks[t].gm = g;
            }
        }
      else if (sscanf (line, "pattern %d", &a) == 1 && a >= 0 && a < GR_MAX_PATTERNS)
        {
          rest = strchr (line, '"');
          rest = get_name (rest ? rest : "", name);
          cur = &s->patterns[a];
          memset (cur, 0, sizeof *cur);
          cur->used = 1;
          snprintf (cur->name, GR_NAME, "%s", name);
          cur->len = sscanf (rest, "%d", &b) == 1 && b > 0 ? b : GR_BAR;
        }
      else if (sscanf (line, "clip %d", &a) == 1 && a >= 0 && a < GR_MAX_CLIPS)
        {
          char wav[256];
          gr_clip *cl = &s->clips[a];
          int16_t *pcm;
          int frames, rate;
          rest = strchr (line, '"');
          get_name (rest ? rest : "", name);
          clip_path (wav, sizeof wav, path, a);
          if (gr_wav_read (wav, &pcm, &frames, &rate) == 0)
            {
              cl->used = 1;
              snprintf (cl->name, GR_NAME, "%s", name);
              cl->data = pcm;
              cl->frames = cl->cap = frames;
              cl->rate = rate;
            }
        }
      else if (sscanf (line, "region %d %d %d %d %d %d", &a, &b, &c, &d, &e, &g) == 6
               && a >= 0 && a < s->ntracks)
        {
          int r = gr_region_add (s, a, b, c, d);
          if (r >= 0)
            {
              s->regions[r].clip = e;
              s->regions[r].clip_offset = g;
              /* Drop a region whose pattern or clip didn't load. */
              if ((e < 0 && (d < 0 || d >= GR_MAX_PATTERNS || !s->patterns[d].used))
                  || (e >= 0 && (e >= GR_MAX_CLIPS || !s->clips[e].used)))
                s->regions[r].used = 0;
            }
        }
    }
  fclose (f);
  if (version == 0)
    {
      gr_song_init (s);
      return -1;
    }
  if (s->bars < 4)
    s->bars = 4;
  gr_song_gc (s);
  return 0;
}

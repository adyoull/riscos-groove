/* Groove tests: an engraving (engine/engrave.h) drawn as SVG, the way the
   Score window draws it on RISC OS (riscos/score.c), to look at. */
#include <stdio.h>
#include "../engine/engrave.h"
#include "../engine/glyphs.h"

/* scale: SVG pixels a staff space. */
int gr_engraving_svg (const gr_engraving *e, const char *path, double px_per_sp, int highlight_bar)
{
  FILE *f = fopen (path, "w");
  double k = px_per_sp / EG_SP;
  int i;
  if (!f)
    return -1;
  fprintf (f, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%.0f\" height=\"%.0f\">\n"
           "<rect width=\"100%%\" height=\"100%%\" fill=\"#fffef8\"/>\n", e->width * k, e->height * k);
  for (i = 0; i < e->n; i++)
    {
      const gr_eitem *it = &e->it[i];
      double x0 = it->x0 * k, y0 = -it->y0 * k, x1 = it->x1 * k, y1 = -it->y1 * k;
      switch (it->kind)
        {
        case EK_BAR:
          if (it->w == highlight_bar)
            fprintf (f, "<rect x=\"%.1f\" y=\"%.1f\" width=\"%.1f\" height=\"%.1f\" fill=\"#fff1b8\"/>\n",
                     x0, y1, x1 - x0, y0 - y1);
          break;
        case EK_GLYPH:
          {
            const gr_glyph *g = &gr_glyphs[it->glyph];
            const short *d = gr_glyph_data + g->start, *end = d + g->len;
            double s = it->w / 100.0;
            fprintf (f, "<path transform=\"translate(%.2f %.2f) scale(%.5f %.5f)\" d=\"", x0, y0, k * s, -k * s);
            while (d < end)
              switch (*d)
                {
                case 1: fprintf (f, "M%d %d", d[1], d[2]); d += 3; break;
                case 2: fprintf (f, "L%d %d", d[1], d[2]); d += 3; break;
                case 3: fprintf (f, "C%d %d %d %d %d %d", d[1], d[2], d[3], d[4], d[5], d[6]); d += 7; break;
                default: fprintf (f, "Z"); d++; break;
                }
            fprintf (f, "\"/>\n");
          }
          break;
        case EK_LINE:
          fprintf (f, "<line x1=\"%.2f\" y1=\"%.2f\" x2=\"%.2f\" y2=\"%.2f\" stroke=\"#000\" stroke-width=\"%.2f\"/>\n",
                   x0, y0, x1, y1, it->w * k);
          break;
        case EK_BEAM:
          fprintf (f, "<polygon points=\"%.2f,%.2f %.2f,%.2f %.2f,%.2f %.2f,%.2f\"/>\n",
                   x0, y0, x1, y1, x1, y1 + it->w * k, x0, y0 + it->w * k);
          break;
        case EK_TIE:
          {
            double len = x1 - x0, h = (len * 0.2 < 0.6 * px_per_sp ? len * 0.2 : 0.6 * px_per_sp) * (it->w > 0 ? -1 : 1);
            double t = 0.13 * px_per_sp * (it->w > 0 ? -1 : 1);
            fprintf (f, "<path d=\"M%.2f %.2f C%.2f %.2f %.2f %.2f %.2f %.2f C%.2f %.2f %.2f %.2f %.2f %.2fZ\"/>\n",
                     x0, y0, x0 + len / 4, y0 + h * 1.33, x1 - len / 4, y1 + h * 1.33, x1, y1,
                     x1 - len / 4, y1 + (h - t) * 1.33, x0 + len / 4, y0 + (h - t) * 1.33, x0, y0);
          }
          break;
        case EK_SLUR:
          {
            double len = x1 - x0, h = -it->w * k, t = 0.16 * px_per_sp * (it->w > 0 ? -1 : 1);
            fprintf (f, "<path d=\"M%.2f %.2f C%.2f %.2f %.2f %.2f %.2f %.2f C%.2f %.2f %.2f %.2f %.2f %.2fZ\"/>\n",
                     x0, y0, x0 + len / 4, y0 + h * 1.33, x1 - len / 4, y1 + h * 1.33, x1, y1,
                     x1 - len / 4, y1 + (h - t) * 1.33, x0 + len / 4, y0 + (h - t) * 1.33, x0, y0);
          }
          break;
        case EK_TEXT:
          {
            /* (the text's Latin-1, as on RISC OS: written as UTF-8, with
               XML's own characters escaped) */
            const unsigned char *t = (const unsigned char *) e->text + it->w;
            fprintf (f, "<text x=\"%.1f\" y=\"%.1f\" font-family=\"serif\" font-size=\"%.1f\">", x0, y0, -y1 * -1 * k);
            for (; *t; t++)
              if (*t == '<')
                fputs ("&lt;", f);
              else if (*t == '>')
                fputs ("&gt;", f);
              else if (*t == '&')
                fputs ("&amp;", f);
              else if (*t >= 128)
                fprintf (f, "%c%c", 0xC0 | (*t >> 6), 0x80 | (*t & 63));
              else
                fputc (*t, f);
            fputs ("</text>\n", f);
          }
          break;
        }
    }
  fprintf (f, "</svg>\n");
  return fclose (f);
}

/* Groove - an engraving drawn into pixels, anti-aliased (raster.h).

   Each shape (a glyph, a line, a beam, a tie...) is turned into straight
   edges (curves flattened to a tenth of a pixel), and the edges are
   accumulated as signed areas, a row of cells at a time, the way font
   rasterisers do it: summed along a row, they give how much of each pixel
   the shape covers. A shape's coverage is merged with the picture's
   (a + b - ab, so overlapping ink doesn't darken or cancel), and at the
   end each pixel is mixed between the paper (or the lit bar's colour)
   and the ink by its coverage. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "raster.h"
#include "glyphs.h"

typedef struct
{
  float *cov;                   /* the picture's coverage, w * h */
  int w, h;
  double scale, ox, oy;
  float *acc;                   /* scratch for one shape */
  size_t acc_cap;
  /* the shape being built: its edges, then filled */
  double *pts;                  /* x, y pairs; NAN, NAN between contours */
  int npts, ptcap;
} raster;

static int add_pt (raster *r, double x, double y)
{
  if (r->npts == r->ptcap)
    {
      int cap = r->ptcap ? r->ptcap * 2 : 1024;
      double *a = realloc (r->pts, (size_t) cap * 2 * sizeof *a);
      if (!a)
        return -1;
      r->pts = a;
      r->ptcap = cap;
    }
  r->pts[r->npts * 2] = x;
  r->pts[r->npts * 2 + 1] = y;
  r->npts++;
  return 0;
}

/* A point in engraving units, to pixels. */
static double px_x (const raster *r, double x)
{
  return x * r->scale + r->ox;
}

static double px_y (const raster *r, double y)
{
  return r->oy - y * r->scale;
}

/* Cubic Bezier from (x0,y0) (pixels), flattened. */
static int curve (raster *r, double x0, double y0, double x1, double y1, double x2, double y2, double x3,
                  double y3)
{
  double d = fabs (x1 - x0) + fabs (y1 - y0) + fabs (x2 - x1) + fabs (y2 - y1) + fabs (x3 - x2) + fabs (y3 - y2);
  int n = (int) (sqrt (d) * 1.5) + 1, i;
  if (n > 64)
    n = 64;
  for (i = 1; i <= n; i++)
    {
      double t = (double) i / n, u = 1 - t;
      double x = u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3;
      double y = u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3;
      if (add_pt (r, x, y))
        return -1;
    }
  return 0;
}

static int end_contour (raster *r)
{
  return add_pt (r, NAN, NAN);
}

/* One edge into the cells (font-rs's way), the cells' box starting at
   pixel (bx, by), bw wide and bh high; the edge may run above or below
   the box (it's clipped to it), but not to its left or right. */
static void edge (float *acc, int bw, int bh, double x0, double y0, double x1, double y1)
{
  double dxdy, x, ystart;
  float dir;
  int y, yend, stride = bw + 2;
  if (y0 == y1)
    return;
  if (y0 < y1)
    dir = 1;
  else
    {
      double t;
      dir = -1;
      t = x0; x0 = x1; x1 = t;
      t = y0; y0 = y1; y1 = t;
    }
  if (y1 <= 0 || y0 >= bh)
    return;
  dxdy = (x1 - x0) / (y1 - y0);
  ystart = y0 < 0 ? 0 : y0;
  y = (int) floor (ystart);
  x = x0 + dxdy * (ystart - y0);
  yend = (int) ceil (y1);
  if (yend > bh)
    yend = bh;
  for (; y < yend; y++)
    {
      float *row = acc + (size_t) y * stride;
      double top = y > y0 ? y : y0, bot = y + 1 < y1 ? y + 1 : y1;
      double dy = bot - top, xnext = x + dxdy * dy, d = dy * dir;
      double xa = x < xnext ? x : xnext, xb = x < xnext ? xnext : x;
      double xaf = floor (xa);
      int xai = (int) xaf, xbi = (int) ceil (xb);
      if (xai < 0) xai = 0, xaf = 0;
      if (xbi > bw + 1) xbi = bw + 1;
      if (xbi <= xai + 1)
        {
          double xmf = 0.5 * (x + xnext) - xaf;
          row[xai] += (float) (d - d * xmf);
          if (xai + 1 < stride)
            row[xai + 1] += (float) (d * xmf);
        }
      else
        {
          double s = 1.0 / (xb - xa), x0f = xa - xaf, a0 = 0.5 * s * (1 - x0f) * (1 - x0f);
          double x1f = xb - xbi + 1, am = 0.5 * s * x1f * x1f;
          int xi;
          row[xai] += (float) (d * a0);
          if (xbi == xai + 2)
            row[xai + 1] += (float) (d * (1 - a0 - am));
          else
            {
              double a1 = s * (1.5 - x0f), a2;
              row[xai + 1] += (float) (d * (a1 - a0));
              for (xi = xai + 2; xi < xbi - 1; xi++)
                row[xi] += (float) (d * s);
              a2 = a1 + (xbi - xai - 3) * s;
              row[xbi - 1] += (float) (d * (1 - a2 - am));
            }
          if (xbi < stride)
            row[xbi] += (float) (d * am);
        }
      x = xnext;
    }
}

/* Fill the shape built in r->pts (non-zero, near enough) and merge it
   into the picture. */
static int fill (raster *r)
{
  double minx = 1e30, maxx = -1e30, miny = 1e30, maxy = -1e30;
  int i, start, bx, by, bw, bh, x, y, stride;
  size_t need;
  if (!r->npts)
    return 0;
  for (i = 0; i < r->npts; i++)
    {
      double x = r->pts[i * 2], y = r->pts[i * 2 + 1];
      if (isnan (x))
        continue;
      if (x < minx) minx = x;
      if (x > maxx) maxx = x;
      if (y < miny) miny = y;
      if (y > maxy) maxy = y;
    }
  if (maxx < 0 || minx >= r->w || maxy < 0 || miny >= r->h || minx > maxx)
    {
      r->npts = 0;
      return 0;
    }
  bx = (int) floor (minx) - 1;
  bw = (int) ceil (maxx) - bx + 2;
  by = (int) floor (miny);
  bh = (int) ceil (maxy) - by + 1;
  /* (only the rows in the picture) */
  {
    int y0 = by < 0 ? 0 : by, y1 = by + bh > r->h ? r->h : by + bh;
    bh = y1 - y0;
    by = y0;
  }
  if (bh <= 0)
    {
      r->npts = 0;
      return 0;
    }
  stride = bw + 2;
  need = (size_t) stride * (size_t) (bh + 1);
  if (need > r->acc_cap)
    {
      float *a = realloc (r->acc, need * sizeof *a);
      if (!a)
        return -1;
      r->acc = a;
      r->acc_cap = need;
    }
  memset (r->acc, 0, need * sizeof *r->acc);
  start = 0;
  for (i = 0; i <= r->npts; i++)
    if (i == r->npts || isnan (r->pts[i * 2]))
      {
        int k;
        /* a contour start..i-1, closed */
        for (k = start; k < i; k++)
          {
            int n = k + 1 < i ? k + 1 : start;
            edge (r->acc, bw, bh, r->pts[k * 2] - bx, r->pts[k * 2 + 1] - by, r->pts[n * 2] - bx,
                  r->pts[n * 2 + 1] - by);
          }
        start = i + 1;
      }
  /* sum along each row: the coverage; merged into the picture */
  for (y = 0; y < bh; y++)
    {
      float *row = r->acc + (size_t) y * stride, sum = 0, *out = r->cov + (size_t) (by + y) * r->w;
      for (x = 0; x < bw + 1; x++)
        {
          float c;
          int px = bx + x;
          sum += row[x];
          c = sum < 0 ? -sum : sum;
          if (c > 1)
            c = 1;
          if (c > 0.002f && px >= 0 && px < r->w)
            out[px] = out[px] + c - out[px] * c;
        }
    }
  r->npts = 0;
  return 0;
}

/* A rectangle in pixels, at least min wide and high (centred). */
static int rect (raster *r, double x0, double y0, double x1, double y1, double min)
{
  double t;
  if (x0 > x1) t = x0, x0 = x1, x1 = t;
  if (y0 > y1) t = y0, y0 = y1, y1 = t;
  if (x1 - x0 < min)
    {
      double c = (x0 + x1) / 2;
      x0 = c - min / 2;
      x1 = c + min / 2;
    }
  if (y1 - y0 < min)
    {
      double c = (y0 + y1) / 2;
      y0 = c - min / 2;
      y1 = c + min / 2;
    }
  if (add_pt (r, x0, y0) || add_pt (r, x1, y0) || add_pt (r, x1, y1) || add_pt (r, x0, y1) || end_contour (r))
    return -1;
  return fill (r);
}

static int glyph (raster *r, int g, double gx, double gy, double size)
{
  const gr_glyph *gl = &gr_glyphs[g];
  const short *d = gr_glyph_data + gl->start, *end = d + gl->len;
  double k = size / 100.0, cx = 0, cy = 0;
  int open = 0;
  /* (cull by its box) */
  if (px_x (r, gx + gl->x1 * k) < 0 || px_x (r, gx + gl->x0 * k) >= r->w
      || px_y (r, gy + gl->y0 * k) < 0 || px_y (r, gy + gl->y1 * k) >= r->h)
    return 0;
  while (d < end)
    switch (*d)
      {
      case 1:
        if (open && end_contour (r))
          return -1;
        cx = px_x (r, gx + d[1] * k);
        cy = px_y (r, gy + d[2] * k);
        if (add_pt (r, cx, cy))
          return -1;
        open = 1;
        d += 3;
        break;
      case 2:
        cx = px_x (r, gx + d[1] * k);
        cy = px_y (r, gy + d[2] * k);
        if (add_pt (r, cx, cy))
          return -1;
        d += 3;
        break;
      case 3:
        {
          double x3 = px_x (r, gx + d[5] * k), y3 = px_y (r, gy + d[6] * k);
          if (curve (r, cx, cy, px_x (r, gx + d[1] * k), px_y (r, gy + d[2] * k), px_x (r, gx + d[3] * k),
                     px_y (r, gy + d[4] * k), x3, y3))
            return -1;
          cx = x3;
          cy = y3;
          d += 7;
        }
        break;
      default:
        if (open && end_contour (r))
          return -1;
        open = 0;
        d++;
        break;
      }
  return fill (r);
}

/* A tie or slur: a crescent, h (engraving units, + up) at its middle,
   mid thick there and end thick at its ends (as Bravura says: 0.22 and
   0.1 of a staff space). */
static int arc (raster *r, double x0, double y0, double x1, double y1, double h, double mid, double endt)
{
  double len = x1 - x0, k = 4.0 / 3, t = h > 0 ? mid : -mid, e = h > 0 ? endt / 2 : -endt / 2;
  double ax = px_x (r, x0), ay0 = px_y (r, y0 + e), ay1 = px_y (r, y0 - e);
  double bx = px_x (r, x1), by0 = px_y (r, y1 + e), by1 = px_y (r, y1 - e);
  if (add_pt (r, ax, ay1))
    return -1;
  /* the outer edge, then back along the inner */
  if (curve (r, ax, ay1, px_x (r, x0 + len / 4), px_y (r, y0 + h * k), px_x (r, x1 - len / 4), px_y (r, y1 + h * k), bx,
             by1)
      || add_pt (r, bx, by0)
      || curve (r, bx, by0, px_x (r, x1 - len / 4), px_y (r, y1 + (h - t) * k), px_x (r, x0 + len / 4),
                px_y (r, y0 + (h - t) * k), ax, ay0)
      || end_contour (r))
    return -1;
  return fill (r);
}

int gr_raster (const gr_engraving *e, double scale, double ox, double oy, uint32_t *px, int w, int h,
               unsigned paper, unsigned ink, unsigned hilite, int hilite_bar, double min_line)
{
  raster r;
  int i, rc = 0;
  size_t n = (size_t) w * (size_t) h;
  memset (&r, 0, sizeof r);
  r.w = w;
  r.h = h;
  r.scale = scale;
  r.ox = ox;
  r.oy = oy;
  if (!(r.cov = calloc (n ? n : 1, sizeof *r.cov)))
    return -1;
  for (i = 0; i < e->n && !rc; i++)
    {
      const gr_eitem *it = &e->it[i];
      switch (it->kind)
        {
        case EK_GLYPH:
          rc = glyph (&r, it->glyph, it->x0, it->y0, it->w);
          break;
        case EK_LINE:
          {
            double t = it->w / 2.0;
            if (it->y0 == it->y1)
              rc = rect (&r, px_x (&r, it->x0), px_y (&r, it->y0 + t), px_x (&r, it->x1), px_y (&r, it->y0 - t), min_line);
            else
              rc = rect (&r, px_x (&r, it->x0 - t), px_y (&r, it->y0), px_x (&r, it->x0 + t), px_y (&r, it->y1), min_line);
          }
          break;
        case EK_BEAM:
          {
            double t = it->w;
            if (fabs (t) * scale < min_line)
              t = (t < 0 ? -min_line : min_line) / scale;
            if (add_pt (&r, px_x (&r, it->x0), px_y (&r, it->y0)) || add_pt (&r, px_x (&r, it->x1), px_y (&r, it->y1))
                || add_pt (&r, px_x (&r, it->x1), px_y (&r, it->y1 - t))
                || add_pt (&r, px_x (&r, it->x0), px_y (&r, it->y0 - t)) || end_contour (&r))
              rc = -1;
            else
              rc = fill (&r);
          }
          break;
        case EK_TIE:
          {
            double len = it->x1 - it->x0, hh = len / 5 < EG_SP * 6 / 10 ? len / 5 : EG_SP * 6 / 10;
            rc = arc (&r, it->x0, it->y0, it->x1, it->y1, it->w > 0 ? hh : -hh, EG_SP * 0.22, EG_SP * 0.1);
          }
          break;
        case EK_SLUR:
          rc = arc (&r, it->x0, it->y0, it->x1, it->y1, it->w, EG_SP * 0.22, EG_SP * 0.1);
          break;
        default:
          break;
        }
    }
  if (!rc)
    {
      /* paper (or the lit bar), ink by coverage */
      int bx0 = -1, by0 = 0, bx1 = -1, by1 = 0, x, y;
      for (i = 0; i < e->n && hilite_bar >= 0; i++)
        if (e->it[i].kind == EK_BAR && e->it[i].w == hilite_bar)
          {
            bx0 = (int) floor (px_x (&r, e->it[i].x0) + 0.5);
            bx1 = (int) floor (px_x (&r, e->it[i].x1) + 0.5);
            by0 = (int) floor (px_y (&r, e->it[i].y1) + 0.5);
            by1 = (int) floor (px_y (&r, e->it[i].y0) + 0.5);
          }
      for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
          {
            unsigned bg = x >= bx0 && x < bx1 && y >= by0 && y < by1 ? hilite : paper;
            float c = r.cov[(size_t) y * w + x];
            int k;
            uint32_t out = 0;
            for (k = 0; k < 3; k++)
              {
                int b = (bg >> (8 + 8 * k)) & 255, f = (ink >> (8 + 8 * k)) & 255;
                int v = (int) (b + (f - b) * c + 0.5f);
                out |= (uint32_t) v << (8 * k);
              }
            px[(size_t) y * w + x] = out;
          }
    }
  free (r.cov);
  free (r.acc);
  free (r.pts);
  return rc;
}

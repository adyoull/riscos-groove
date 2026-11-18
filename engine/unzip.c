/* Groove - reading one file out of a zip archive (MuseScore's .mscz and
   compressed MusicXML, .mxl, are zip files). Stored and deflated entries;
   inflate as in RFC 1951, decoding Huffman codes the way Mark Adler's
   puff.c does (canonical codes, read a bit at a time: small and plenty
   fast for a score). */
#include <stdlib.h>
#include <string.h>
#include "unzip.h"

/* ---- inflate ------------------------------------------------------------------- */

typedef struct
{
  const unsigned char *in, *in_end;
  unsigned bits, nbits;
  unsigned char *out;
  size_t n, cap, max;
  int bad;
} inf;

typedef struct
{
  short count[16];              /* codes of each length */
  short sym[320];               /* symbols in code order */
} huff;

static unsigned getbits (inf *s, unsigned need)
{
  unsigned v;
  while (s->nbits < need)
    {
      if (s->in >= s->in_end)
        {
          s->bad = 1;
          return 0;
        }
      s->bits |= (unsigned) *s->in++ << s->nbits;
      s->nbits += 8;
    }
  v = s->bits & ((1u << need) - 1);
  s->bits >>= need;
  s->nbits -= need;
  return v;
}

/* Canonical Huffman code from code lengths. Returns 0, or -1 if the
   lengths can't make a code (an incomplete one is allowed, as zlib does
   for a single distance code). */
static int build (huff *h, const unsigned char *len, int n)
{
  short offs[16];
  int i, left = 1;
  memset (h->count, 0, sizeof h->count);
  for (i = 0; i < n; i++)
    h->count[len[i]]++;
  h->count[0] = 0;
  for (i = 1; i < 16; i++)
    {
      left <<= 1;
      left -= h->count[i];
      if (left < 0)
        return -1;
    }
  offs[1] = 0;
  for (i = 1; i < 15; i++)
    offs[i + 1] = (short) (offs[i] + h->count[i]);
  for (i = 0; i < n; i++)
    if (len[i])
      h->sym[offs[len[i]]++] = (short) i;
  return 0;
}

static int decode (inf *s, const huff *h)
{
  int code = 0, first = 0, index = 0, len;
  for (len = 1; len < 16; len++)
    {
      int count;
      code |= (int) getbits (s, 1);
      if (s->bad)
        return -1;
      count = h->count[len];
      if (code - count < first)
        return h->sym[index + (code - first)];
      index += count;
      first += count;
      first <<= 1;
      code <<= 1;
    }
  s->bad = 1;
  return -1;
}

static int put (inf *s, unsigned char c)
{
  if (s->n == s->cap)
    {
      size_t cap = s->cap ? s->cap * 2 : 65536;
      unsigned char *o;
      if (cap > s->max)
        cap = s->max;
      if (cap <= s->n || !(o = realloc (s->out, cap)))
        {
          s->bad = 1;
          return -1;
        }
      s->out = o;
      s->cap = cap;
    }
  s->out[s->n++] = c;
  return 0;
}

static const short lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const short lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const short dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                 8193, 12289, 16385, 24577 };
static const short dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int codes (inf *s, const huff *lc, const huff *dc)
{
  for (;;)
    {
      int sym = decode (s, lc);
      if (sym < 0)
        return -1;
      if (sym < 256)
        {
          if (put (s, (unsigned char) sym))
            return -1;
        }
      else if (sym == 256)
        return 0;
      else
        {
          int len, dist;
          sym -= 257;
          if (sym >= 29)
            return -1;
          len = lbase[sym] + (int) getbits (s, (unsigned) lext[sym]);
          sym = decode (s, dc);
          if (sym < 0 || sym >= 30)
            return -1;
          dist = dbase[sym] + (int) getbits (s, (unsigned) dext[sym]);
          if (s->bad || (size_t) dist > s->n)
            return -1;
          while (len--)
            if (put (s, s->out[s->n - (size_t) dist]))
              return -1;
        }
    }
}

static int fixed_block (inf *s)
{
  static huff lc, dc;
  static int made;
  if (!made)
    {
      unsigned char l[288];
      int i;
      for (i = 0; i < 144; i++) l[i] = 8;
      for (; i < 256; i++) l[i] = 9;
      for (; i < 280; i++) l[i] = 7;
      for (; i < 288; i++) l[i] = 8;
      build (&lc, l, 288);
      for (i = 0; i < 30; i++) l[i] = 5;
      build (&dc, l, 30);
      made = 1;
    }
  return codes (s, &lc, &dc);
}

static int dynamic_block (inf *s)
{
  static const unsigned char order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
  unsigned char len[320];
  huff lc, dc;
  int nlen = (int) getbits (s, 5) + 257, ndist = (int) getbits (s, 5) + 1, ncode = (int) getbits (s, 4) + 4;
  int i;
  if (s->bad || nlen > 286 || ndist > 30)
    return -1;
  memset (len, 0, sizeof len);
  for (i = 0; i < ncode; i++)
    len[order[i]] = (unsigned char) getbits (s, 3);
  if (s->bad || build (&lc, len, 19))
    return -1;
  for (i = 0; i < nlen + ndist; )
    {
      int sym = decode (s, &lc), rep, v = 0;
      if (sym < 0)
        return -1;
      if (sym < 16)
        {
          len[i++] = (unsigned char) sym;
          continue;
        }
      if (sym == 16)
        {
          if (!i)
            return -1;
          v = len[i - 1];
          rep = 3 + (int) getbits (s, 2);
        }
      else if (sym == 17)
        rep = 3 + (int) getbits (s, 3);
      else
        rep = 11 + (int) getbits (s, 7);
      if (s->bad || i + rep > nlen + ndist)
        return -1;
      while (rep--)
        len[i++] = (unsigned char) v;
    }
  if (!len[256] || build (&lc, len, nlen) || build (&dc, len + nlen, ndist))
    return -1;
  return codes (s, &lc, &dc);
}

unsigned char *gr_inflate (const unsigned char *in, size_t n, size_t max, size_t *outlen)
{
  inf s;
  int last;
  memset (&s, 0, sizeof s);
  s.in = in;
  s.in_end = in + n;
  s.max = max;
  do
    {
      int type;
      last = (int) getbits (&s, 1);
      type = (int) getbits (&s, 2);
      if (s.bad)
        break;
      if (type == 0)
        {
          unsigned len;
          s.bits = s.nbits = 0;           /* to a byte boundary */
          if (s.in_end - s.in < 4)
            {
              s.bad = 1;
              break;
            }
          len = s.in[0] | (s.in[1] << 8);
          if ((len ^ 0xFFFFu) != (unsigned) (s.in[2] | (s.in[3] << 8)))
            {
              s.bad = 1;
              break;
            }
          s.in += 4;
          if ((size_t) (s.in_end - s.in) < len)
            {
              s.bad = 1;
              break;
            }
          while (len--)
            if (put (&s, *s.in++))
              break;
        }
      else if (type == 1)
        s.bad |= fixed_block (&s) != 0;
      else if (type == 2)
        s.bad |= dynamic_block (&s) != 0;
      else
        s.bad = 1;
    }
  while (!last && !s.bad);
  if (s.bad)
    {
      free (s.out);
      return NULL;
    }
  if (!s.out)
    s.out = malloc (1);
  *outlen = s.n;
  return s.out;
}

/* ---- zip ------------------------------------------------------------------------- */

static unsigned le16 (const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned long le32 (const unsigned char *p)
{
  return p[0] | (p[1] << 8) | ((unsigned long) p[2] << 16) | ((unsigned long) p[3] << 24);
}

int gr_zip_names (const unsigned char *zip, size_t n, gr_zip_entry *e, int max)
{
  long i;
  unsigned long cd, count, k;
  const unsigned char *p;
  int got = 0;
  if (n < 22)
    return -1;
  for (i = (long) n - 22; i >= 0 && i >= (long) n - 22 - 65535; i--)   /* the end record */
    if (le32 (zip + i) == 0x06054b50UL)
      break;
  if (i < 0 || i < (long) n - 22 - 65535)
    return -1;
  count = le16 (zip + i + 10);
  cd = le32 (zip + i + 16);
  if (cd >= n)
    return -1;
  p = zip + cd;
  for (k = 0; k < count && got < max; k++)
    {
      unsigned nl, xl, cl;
      unsigned long off;
      if ((size_t) (p - zip) + 46 > n || le32 (p) != 0x02014b50UL)
        return -1;
      nl = le16 (p + 28);
      xl = le16 (p + 30);
      cl = le16 (p + 32);
      off = le32 (p + 42);
      if ((size_t) (p - zip) + 46 + nl > n)
        return -1;
      e[got].method = (int) le16 (p + 10);
      e[got].csize = le32 (p + 20);
      e[got].usize = le32 (p + 24);
      e[got].offset = off;
      if (nl >= sizeof e[got].name)
        nl = sizeof e[got].name - 1;
      memcpy (e[got].name, p + 46, nl);
      e[got].name[nl] = 0;
      got++;
      p += 46 + le16 (p + 28) + xl + cl;
    }
  return got;
}

unsigned char *gr_zip_read (const unsigned char *zip, size_t n, const gr_zip_entry *e, size_t *outlen)
{
  const unsigned char *h = zip + e->offset, *d;
  size_t data;
  if (e->offset + 30 > n || le32 (h) != 0x04034b50UL)
    return NULL;
  data = e->offset + 30 + le16 (h + 26) + le16 (h + 28);
  if (data > n || e->csize > n - data || e->usize > 64UL * 1024 * 1024)
    return NULL;
  d = zip + data;
  if (e->method == 0)
    {
      unsigned char *o = malloc (e->csize + 1);
      if (!o)
        return NULL;
      memcpy (o, d, e->csize);
      *outlen = e->csize;
      return o;
    }
  if (e->method == 8)
    return gr_inflate (d, e->csize, e->usize + 1 > 65536 ? e->usize + 1 : 65536, outlen);
  return NULL;
}

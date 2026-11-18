/* Groove - a small XML reader for MusicXML and MuseScore files.

   Enough of XML 1.0 for files written by music programs: elements,
   attributes, text, the five named entities and character references,
   comments, CDATA, processing instructions and a DOCTYPE (skipped, with
   any internal subset). Text is kept per element; text split by child
   elements is joined. */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "xml.h"

#define BLOCK 4096

struct gx_doc
{
  gx_node *root;
  struct blk { struct blk *next; size_t used; gx_node n[BLOCK]; } *blocks;
  char **attrs;                 /* every element's attributes, in one array */
  size_t nattrs, attrcap;
};

static gx_node *new_node (gx_doc *d)
{
  if (!d->blocks || d->blocks->used == BLOCK)
    {
      struct blk *b = malloc (sizeof *b);
      if (!b)
        return NULL;
      b->next = d->blocks;
      b->used = 0;
      d->blocks = b;
    }
  memset (&d->blocks->n[d->blocks->used], 0, sizeof (gx_node));
  return &d->blocks->n[d->blocks->used++];
}

static void put_utf8 (char **o, unsigned long c)
{
  char *p = *o;
  if (c < 0x80)
    *p++ = (char) c;
  else if (c < 0x800)
    {
      *p++ = (char) (0xC0 | (c >> 6));
      *p++ = (char) (0x80 | (c & 0x3F));
    }
  else if (c < 0x10000)
    {
      *p++ = (char) (0xE0 | (c >> 12));
      *p++ = (char) (0x80 | ((c >> 6) & 0x3F));
      *p++ = (char) (0x80 | (c & 0x3F));
    }
  else
    {
      *p++ = (char) (0xF0 | (c >> 18));
      *p++ = (char) (0x80 | ((c >> 12) & 0x3F));
      *p++ = (char) (0x80 | ((c >> 6) & 0x3F));
      *p++ = (char) (0x80 | (c & 0x3F));
    }
  *o = p;
}

/* Decodes entities in s..e in place (the result is never longer); returns
   the new end. */
static char *unescape (char *s, char *e)
{
  char *o = s;
  while (s < e)
    {
      if (*s == '&')
        {
          char *semi = memchr (s, ';', (size_t) (e - s));
          if (semi && semi - s <= 10)
            {
              size_t l = (size_t) (semi - s - 1);
              char *nm = s + 1;
              unsigned long c = 0;
              int ok = 1;
              if (l == 2 && !memcmp (nm, "lt", 2)) c = '<';
              else if (l == 2 && !memcmp (nm, "gt", 2)) c = '>';
              else if (l == 3 && !memcmp (nm, "amp", 3)) c = '&';
              else if (l == 4 && !memcmp (nm, "quot", 4)) c = '"';
              else if (l == 4 && !memcmp (nm, "apos", 4)) c = '\'';
              else if (l >= 2 && nm[0] == '#')
                c = nm[1] == 'x' ? strtoul (nm + 2, NULL, 16) : strtoul (nm + 1, NULL, 10);
              else
                ok = 0;
              if (ok && c && c < 0x110000)
                {
                  put_utf8 (&o, c);
                  s = semi + 1;
                  continue;
                }
            }
        }
      *o++ = *s++;
    }
  return o;
}

static int add_attr (gx_doc *d, char *v)
{
  if (d->nattrs == d->attrcap)
    {
      size_t cap = d->attrcap ? d->attrcap * 2 : 256;
      char **a = realloc (d->attrs, cap * sizeof *a);
      if (!a)
        return -1;
      d->attrs = a;
      d->attrcap = cap;
    }
  d->attrs[d->nattrs++] = v;
  return 0;
}

/* An element's text: its first piece that isn't just white space (music
   files don't mix text and elements, so the white space between child
   elements is all that's left out). */
static void add_text (gx_node *n, char *s, char *e)
{
  char *t;
  if (n->text && *n->text)
    return;
  for (t = s; t < e && isspace ((unsigned char) *t); t++)
    ;
  if (t == e)
    return;
  e = unescape (s, e);
  n->text = s;
  *e = 0;
}

gx_doc *gx_parse (char *buf, size_t n)
{
  gx_doc *d = calloc (1, sizeof *d);
  char *p = buf, *end = buf + n;
  gx_node *cur = NULL;
  size_t *attr_at = NULL, nat = 0, atcap = 0;   /* each node's first attribute index */
  gx_node **who = NULL;
  struct blk *b;
  size_t i;
  if (!d)
    return NULL;
  buf[n] = 0;
  if (n >= 3 && (unsigned char) p[0] == 0xEF && (unsigned char) p[1] == 0xBB && (unsigned char) p[2] == 0xBF)
    p += 3;
  while (p < end)
    {
      char *lt = memchr (p, '<', (size_t) (end - p));
      if (!lt)
        lt = end;
      if (cur)
        add_text (cur, p, lt);
      if (lt >= end)
        break;
      p = lt + 1;
      if (!strncmp (p, "!--", 3))
        {
          char *q = strstr (p + 3, "-->");
          if (!q)
            goto bad;
          p = q + 3;
        }
      else if (!strncmp (p, "![CDATA[", 8))
        {
          char *q = strstr (p + 8, "]]>");
          if (!q)
            goto bad;
          if (cur)
            {
              /* (CDATA isn't unescaped: copy it so add_text's unescape
                 leaves it alone - music files don't put & in CDATA) */
              add_text (cur, p + 8, q);
            }
          p = q + 3;
        }
      else if (*p == '?')
        {
          char *q = strstr (p, "?>");
          if (!q)
            goto bad;
          p = q + 2;
        }
      else if (*p == '!')
        {
          int depth = 0;                         /* <!DOCTYPE ... [ ... ]> */
          for (; p < end; p++)
            {
              if (*p == '[') depth++;
              else if (*p == ']') depth--;
              else if (*p == '>' && depth <= 0) break;
            }
          if (p >= end)
            goto bad;
          p++;
        }
      else if (*p == '/')
        {
          char *q = memchr (p, '>', (size_t) (end - p));
          if (!q || !cur)
            goto bad;
          *p = 0;
          cur = cur->parent;
          p = q + 1;
        }
      else
        {
          gx_node *nn = new_node (d);
          char *q;
          int selfclose = 0;
          if (!nn)
            goto bad;
          nn->name = p;
          nn->text = "";
          while (p < end && !isspace ((unsigned char) *p) && *p != '>' && *p != '/')
            p++;
          q = p;
          if (nat == atcap)
            {
              size_t cap = atcap ? atcap * 2 : 1024;
              size_t *a = realloc (attr_at, cap * sizeof *a);
              gx_node **w = realloc (who, cap * sizeof *w);
              if (a) attr_at = a;
              if (w) who = w;
              if (!a || !w)
                goto bad;
              atcap = cap;
            }
          attr_at[nat] = d->nattrs;
          who[nat++] = nn;
          for (;;)
            {
              char *an, quote, *v, *ve;
              while (p < end && isspace ((unsigned char) *p))
                p++;
              if (p >= end)
                goto bad;
              if (*p == '>')
                {
                  p++;
                  break;
                }
              if (*p == '/')
                {
                  selfclose = 1;
                  p = memchr (p, '>', (size_t) (end - p));
                  if (!p)
                    goto bad;
                  p++;
                  break;
                }
              an = p;
              while (p < end && *p != '=' && !isspace ((unsigned char) *p) && *p != '>')
                p++;
              ve = p;
              while (p < end && isspace ((unsigned char) *p))
                p++;
              if (p >= end || *p != '=')
                goto bad;
              *ve = 0;
              p++;
              while (p < end && isspace ((unsigned char) *p))
                p++;
              if (p >= end || (*p != '"' && *p != '\''))
                goto bad;
              quote = *p++;
              v = p;
              p = memchr (p, quote, (size_t) (end - p));
              if (!p)
                goto bad;
              *unescape (v, p) = 0;
              p++;
              if (add_attr (d, an) || add_attr (d, v))
                goto bad;
            }
          *q = 0;
          nn->parent = cur;
          if (cur)
            {
              if (cur->last)
                cur->last->next = nn;
              else
                cur->child = nn;
              cur->last = nn;
            }
          else if (!d->root)
            d->root = nn;
          if (!selfclose)
            cur = nn;
        }
    }
  if (!d->root)
    goto bad;
  /* Attribute pointers, now the array has stopped moving. */
  for (i = 0; i < nat; i++)
    {
      size_t to = i + 1 < nat ? attr_at[i + 1] : d->nattrs;
      who[i]->attr = d->attrs + attr_at[i];
      who[i]->nattr = (int) ((to - attr_at[i]) / 2);
    }
  free (attr_at);
  free (who);
  return d;
bad:
  free (attr_at);
  free (who);
  while ((b = d->blocks))
    {
      d->blocks = b->next;
      free (b);
    }
  free (d->attrs);
  free (d);
  return NULL;
}

gx_node *gx_root (gx_doc *d)
{
  return d->root;
}

void gx_free (gx_doc *d)
{
  struct blk *b;
  if (!d)
    return;
  while ((b = d->blocks))
    {
      d->blocks = b->next;
      free (b);
    }
  free (d->attrs);
  free (d);
}

gx_node *gx_child (const gx_node *n, const char *name)
{
  gx_node *c;
  for (c = n ? n->child : NULL; c; c = c->next)
    if (!strcmp (c->name, name))
      return c;
  return NULL;
}

gx_node *gx_next (const gx_node *n, const char *name)
{
  gx_node *c;
  for (c = n ? n->next : NULL; c; c = c->next)
    if (!strcmp (c->name, name))
      return c;
  return NULL;
}

const char *gx_attr (const gx_node *n, const char *name)
{
  int i;
  for (i = 0; n && i < n->nattr; i++)
    if (!strcmp (n->attr[2 * i], name))
      return n->attr[2 * i + 1];
  return NULL;
}

const char *gx_text (const gx_node *n, const char *child)
{
  const gx_node *c = child ? gx_child (n, child) : n;
  return c && c->text ? c->text : "";
}

int gx_int (const gx_node *n, const char *child, int dflt)
{
  const char *t = gx_text (n, child);
  char *e;
  long v;
  while (isspace ((unsigned char) *t))
    t++;
  v = strtol (t, &e, 10);
  return e == t ? dflt : (int) v;
}

double gx_num (const gx_node *n, const char *child, double dflt)
{
  const char *t = gx_text (n, child);
  char *e;
  double v;
  while (isspace ((unsigned char) *t))
    t++;
  v = strtod (t, &e);
  return e == t ? dflt : v;
}

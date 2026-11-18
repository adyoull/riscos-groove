/* Groove - a small XML reader (xml.c) for MusicXML and MuseScore files.
   The whole document becomes a tree of elements; names, attributes and
   text point into the buffer it was given, which it changes. */
#ifndef XML_H
#define XML_H
#include <stddef.h>

typedef struct gx_node
{
  char *name;
  char **attr;                  /* name, value, name, value ... */
  int nattr;
  char *text;                   /* the element's own text ("" if none) */
  struct gx_node *parent, *child, *last, *next;
} gx_node;

typedef struct gx_doc gx_doc;

/* Parses buf (n bytes; it must have room for a 0 after them). NULL if it
   isn't well-formed enough to read, or memory runs out. */
gx_doc *gx_parse (char *buf, size_t n);
gx_node *gx_root (gx_doc *d);
void gx_free (gx_doc *d);

gx_node *gx_child (const gx_node *n, const char *name);         /* first child called name */
gx_node *gx_next (const gx_node *n, const char *name);          /* next sibling called name */
const char *gx_attr (const gx_node *n, const char *name);       /* NULL if not there */
const char *gx_text (const gx_node *n, const char *child);      /* a child's text, "" if none */
int gx_int (const gx_node *n, const char *child, int dflt);     /* a child's text as a number */
double gx_num (const gx_node *n, const char *child, double dflt);

#endif

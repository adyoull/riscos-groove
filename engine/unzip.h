/* Groove - reading files out of zip archives (unzip.c). */
#ifndef UNZIP_H
#define UNZIP_H
#include <stddef.h>

typedef struct
{
  char name[128];
  int method;                   /* 0 stored, 8 deflated */
  unsigned long csize, usize, offset;
} gr_zip_entry;

/* Inflates a raw deflate stream (no zlib header) into a new buffer of at
   most max bytes. NULL if it's damaged or too big. */
unsigned char *gr_inflate (const unsigned char *in, size_t n, size_t max, size_t *outlen);

/* Lists up to max entries of the zip held in memory. Returns how many, or
   -1 if it isn't a zip. */
int gr_zip_names (const unsigned char *zip, size_t n, gr_zip_entry *e, int max);

/* One entry's contents, in a new buffer (free it). NULL on failure. */
unsigned char *gr_zip_read (const unsigned char *zip, size_t n, const gr_zip_entry *e, size_t *outlen);

#endif

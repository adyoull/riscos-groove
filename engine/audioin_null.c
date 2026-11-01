/* Groove - the "no input" audio backend. */
#include "audioin.h"

static int n_open (int rate) { (void) rate; return -1; }
static int n_read (int16_t *s, int max) { (void) s; (void) max; return 0; }
static float n_level (void) { return 0.0f; }
static void n_close (void) { }

const audioin_backend audioin_null = { "No input", n_open, n_read, n_level, n_close };

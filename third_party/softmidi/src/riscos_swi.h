/*
 * midisynth - SWI calls for the output backends.
 * Copyright (c) 2026 Andrew Youll. MIT licence, see LICENSE.
 *
 * On RISC OS this is <kernel.h>. The host tests build the backends with
 * -Itests/fake, whose kernel.h sends the calls to tests/fake_swi.c.
 */
#ifndef MIDISYNTH_RISCOS_SWI_H
#define MIDISYNTH_RISCOS_SWI_H

#include <stdint.h>
#include <kernel.h>

#define MS_XSWI 0x20000            /* X bit: errors are returned, not raised */

/* A pointer, to pass in a register. On a 64-bit PC a pointer doesn't fit
   in an int, so the tests' fake SWIs swap it for a token and back. */
#ifdef MIDISYNTH_FAKE_SWI
int fake_ptr_token(const void *p);
#define MS_PTR(p) fake_ptr_token(p)
#else
#define MS_PTR(p) ((int)(intptr_t)(p))
#endif

#endif

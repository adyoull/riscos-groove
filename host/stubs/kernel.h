/* Host stand-in for GCCSDK's <kernel.h>: just what Groove uses. */
#ifndef KERNEL_H
#define KERNEL_H
typedef struct { int errnum; char errmess[252]; } _kernel_oserror;
#endif

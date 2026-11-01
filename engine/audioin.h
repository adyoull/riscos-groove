/* Groove - audio input backends.

   RISC OS has no standard audio-input API (see the design doc): input comes
   from whatever driver the machine has. On a Pi that means a USB audio
   class device through the isochronous USB stack, read from DeviceFS. So
   recording goes through this small interface, and each driver is one
   backend file. The engine only ever sees 16-bit stereo frames, pushed in
   with gr_audio_in() on null events.

   Backends so far:
     audioin_null.c   no input (the default): Record still works for
                      instrument and drum tracks
     audioin_test.c   (host tests) a sine wave, to test the recording path
   Planned:
     audioin_usb.c    USB audio class device via DeviceFS                 */
#ifndef AUDIOIN_H
#define AUDIOIN_H

#include <stdint.h>

typedef struct
{
  const char *name;                     /* shown in the Input menu */
  int (*open) (int rate);               /* 0 = OK */
  int (*read) (int16_t *stereo, int max_frames);   /* frames read, never blocks */
  float (*level) (void);                /* 0..1 peak since the last call, for the meter */
  void (*close) (void);
} audioin_backend;

extern const audioin_backend audioin_null;

#endif

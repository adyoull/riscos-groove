/* Host stand-in for GCCSDK's <swis.h>: the _swix call, its register masks
   (the same encoding as GCCSDK's) and the SWI numbers Groove uses, from the
   PRMs. host/fake_ro.c implements _swix. */
#ifndef SWIS_H
#define SWIS_H
#include "kernel.h"
#define _IN(n)      (1u << (n))
#define _INR(a, b)  ((~0u << (a)) & ~(~0u << ((b) + 1)))
#define _OUT(n)     (1u << (31 - (n)))
_kernel_oserror *_swix (int swi, unsigned mask, ...);

#define OS_Byte                  0x06
#define OS_File                  0x08
#define OS_Module                0x1E
#define OS_ReadModeVariable      0x35
#define OS_ReadMonotonicTime     0x42
#define OS_Plot                  0x45
#define OS_WriteN                0x46
#define OS_SynchroniseCodeAreas  0x6E
#define Wimp_Initialise          0x400C0
#define Wimp_CreateWindow        0x400C1
#define Wimp_CreateIcon          0x400C2
#define Wimp_OpenWindow          0x400C5
#define Wimp_CloseWindow         0x400C6
#define Wimp_Poll                0x400C7
#define Wimp_RedrawWindow        0x400C8
#define Wimp_GetRectangle        0x400CA
#define Wimp_GetWindowState      0x400CB
#define Wimp_GetIconState        0x400CE
#define Wimp_GetPointerInfo      0x400CF
#define Wimp_DragBox             0x400D0
#define Wimp_ForceRedraw         0x400D1
#define Wimp_SetCaretPosition    0x400D2
#define Wimp_GetCaretPosition 0x400D3
#define Wimp_StartTask        0x400DE
#define Wimp_CreateMenu          0x400D4
#define Wimp_SetExtent           0x400D7
#define Wimp_ProcessKey          0x400DC
#define Wimp_CloseDown           0x400DD
#define Wimp_ReportError         0x400DF
#define Wimp_GetWindowOutline    0x400E0
#define Wimp_PollIdle            0x400E1
#define Wimp_SendMessage         0x400E7
#define Hourglass_On             0x406C0
#define Hourglass_Off            0x406C1
#define ColourTrans_SetGCOL      0x40743
#endif

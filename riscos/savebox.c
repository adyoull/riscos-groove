/* Groove - the save box, the RISC OS way: drag the file icon to a Filer
   window (or any program that takes the file), or type a full pathname
   and press Return. Used for songs, "Export as WAV" and "Export as MIDI".

   The drag uses the data transfer protocol (PRM 3-252):
     we -> DataSave (leaf name, type) -> them
     them -> DataSaveAck (full path to use) -> we save there
     we -> DataLoad (the path) -> them, so a Filer shows the new file */
#include <stdio.h>
#include <string.h>
#include <kernel.h>
#include <swis.h>
#include "app.h"

static char path[256], sprite[12], validation[] = "Ktar";
static int filetype = FILETYPE_SONG, pressed, wimp_dragging, my_ref;

enum { I_FILE, I_PATH, I_SAVE };

int savebox_create (void)
{
  int b[22 + 3 * 8];
  memset (b, 0, sizeof b);
  b[0] = 0; b[1] = 0; b[2] = 600; b[3] = 220;
  b[6] = -1;
  b[7] = (int) 0x84000012;      /* movable, title, Wimp redraws it */
  ((unsigned char *) b)[32] = 7;  ((unsigned char *) b)[33] = 2;
  ((unsigned char *) b)[34] = 7;  ((unsigned char *) b)[35] = 1;
  ((unsigned char *) b)[36] = 3;  ((unsigned char *) b)[37] = 1;
  ((unsigned char *) b)[38] = 12;
  b[10] = 0; b[11] = -220; b[12] = 600; b[13] = 0;
  b[14] = 0x3D;
  b[15] = 0;
  b[16] = 1;
  strcpy ((char *) &b[18], "Save as");
  b[21] = 3;
  /* The file icon: an indirected sprite (from the Wimp's pool), click. */
  b[22] = 266; b[23] = -116; b[24] = 334; b[25] = -48;
  b[26] = 0x0000311A;
  b[27] = (int) sprite; b[28] = 1; b[29] = sizeof sprite;
  /* The pathname: writable, black on white. */
  b[30] = 16; b[31] = -192; b[32] = 440; b[33] = -136;
  b[34] = 0x0700F135;
  b[35] = (int) path; b[36] = (int) validation; b[37] = sizeof path;
  /* The Save button. */
  b[38] = 456; b[39] = -196; b[40] = 584; b[41] = -132;
  b[42] = 0x1700303D;
  strcpy ((char *) &b[43], "Save");
  return (app_create_error = _swix (Wimp_CreateWindow, _IN (1) | _OUT (0), b, &app.w_save)) != NULL;
}

static const char *leaf (const char *p)
{
  const char *l = strrchr (p, '.');
  return l ? l + 1 : p;
}

void savebox_open (int type)
{
  int x, y;
  filetype = type;
  snprintf (sprite, sizeof sprite, "file_%03x", type);
  if (type == FILETYPE_SONG)
    snprintf (path, sizeof path, "%s", app.path[0] ? app.path : "Song");
  else if (app.path[0])
    snprintf (path, sizeof path, "%.250s_%s", app.path, type == FILETYPE_MIDI ? "midi" : "mix");
  else
    snprintf (path, sizeof path, type == FILETYPE_MIDI ? "MIDI" : "Mix");
  app_pointer (&x, &y, NULL, NULL, NULL);
  /* Opened as a menu: it goes away when you click elsewhere, as save boxes do. */
  app_check (_swix (Wimp_CreateMenu, _INR (1, 3), app.w_save, x - 300, y + 110));
  _swix (Wimp_SetCaretPosition, _INR (0, 5), app.w_save, I_PATH, -1, -1, -1, (int) strlen (path));
}

int save_to (const char *p, int type)
{
  int rc;
  _swix (Hourglass_On, 0);
  rc = type == FILETYPE_SONG ? gr_song_save (&app.song, p)
       : type == FILETYPE_MIDI ? gr_midi_export (&app.song, p) : gr_bounce (&app.song, p);
  _swix (Hourglass_Off, 0);
  if (rc)
    {
      app_error (type == FILETYPE_SONG ? "Groove couldn't save the song there."
                 : type == FILETYPE_MIDI ? "Groove couldn't write the MIDI file there."
                 : "Groove couldn't write the WAV file there.");
      return -1;
    }
  _swix (OS_File, _INR (0, 2), 18, p, type);     /* set its filetype */
  if (type == FILETYPE_SONG)
    {
      snprintf (app.path, sizeof app.path, "%s", p);
      gr_history_note (app.hist, &app.song);
      gr_history_saved (app.hist);
      app.unsaved = 0;
      arrange_title ();
    }
  return 0;
}

static void close_box (void)
{
  _swix (Wimp_CreateMenu, _IN (1), -1);
}

static void save_typed (void)
{
  if (!strchr (path, '.') && !strchr (path, ':'))
    {
      app_error ("To save, drag the icon to a directory display.");
      return;
    }
  if (save_to (path, filetype) == 0)
    close_box ();
}

static void watch_press (int x, int y, int done)
{
  int ib[10], d[10], ox, oy;
  (void) x; (void) y;
  if (done)
    return;                     /* a click on the icon: nothing to do */
  /* Moved with the button down: start the Wimp drag of the file icon. */
  ib[0] = app.w_save;
  ib[1] = I_FILE;
  _swix (Wimp_GetIconState, _IN (1), ib);
  app_window_origin (app.w_save, &ox, &oy);
  d[0] = app.w_save;
  d[1] = 5;                     /* a fixed-size box: the icon's */
  d[2] = ox + ib[2]; d[3] = oy + ib[3]; d[4] = ox + ib[4]; d[5] = oy + ib[5];
  d[6] = -0x8000; d[7] = -0x8000; d[8] = 0x7FFF; d[9] = 0x7FFF;
  if (!_swix (Wimp_DragBox, _IN (1), d))
    wimp_dragging = 1;
  app.drag = NULL;
  pressed = 0;
}

void savebox_click (int *b)
{
  if (b[4] == I_FILE && (b[2] == BUT_SELECT || b[2] == BUT_ADJUST))
    {
      pressed = 1;
      app_start_drag (app.w_save, watch_press);
    }
  else if (b[4] == I_SAVE && b[2] != BUT_MENU)
    save_typed ();
}

int savebox_key (int *b)
{
  if (b[6] == 13)               /* Return in the pathname */
    {
      save_typed ();
      return 1;
    }
  if (b[6] == 0x1B)
    {
      close_box ();
      return 1;
    }
  return 0;
}

void savebox_drag_done (void)
{
  int m[64], x, y, w, i;
  const char *l;
  if (!wimp_dragging)
    return;
  wimp_dragging = 0;
  app_pointer (&x, &y, NULL, &w, &i);
  if (w == app.w_save)
    return;                     /* dropped back on the box */
  l = leaf (path);
  memset (m, 0, sizeof m);
  m[0] = (44 + (int) strlen (l) + 1 + 3) & ~3;
  m[3] = 0;
  m[4] = MSG_DATASAVE;
  m[5] = w; m[6] = i; m[7] = x; m[8] = y;
  m[9] = filetype == FILETYPE_WAVE ? 4 * 1024 * 1024 : 8192;   /* an estimate */
  m[10] = filetype;
  strcpy ((char *) &m[11], l);
  if (!_swix (Wimp_SendMessage, _INR (0, 3), EV_MESSAGE_RECORDED, m, w, i))
    my_ref = m[2];              /* the Wimp fills in my_ref */
}

void savebox_ack (int *m)
{
  char *p = (char *) &m[11];
  if (m[3] != my_ref || my_ref == 0)
    return;
  my_ref = 0;
  if (save_to (p, filetype))
    return;
  if (m[9] == -1 && filetype == FILETYPE_SONG)
    app.path[0] = 0;            /* a scrap file (another program): not our home */
  /* Tell them it's there. */
  m[3] = m[2];
  m[4] = MSG_DATALOAD;
  _swix (Wimp_SendMessage, _INR (0, 2), EV_MESSAGE_RECORDED, m, m[1]);
  close_box ();
  arrange_title ();
}

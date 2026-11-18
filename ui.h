/* Kiboard screens: state, drawing into an 8-bit buffer (only 0 and 255 are used), touch handling that
 * returns HID reports. No device code here, so the same file runs on the PC for tests and screenshots. */
#ifndef KB_UI_H
#define KB_UI_H

#include "gesture.h"
#include "hid.h"

#define UI_W 1272 /* the size everything is drawn and touched in; main.c fits it to the real screen */
#define UI_H 1696

typedef struct { int x, y, w, h; } Rect;

enum { S_MODE, S_MAIN };                                                   /* screen */
enum { ST_PICK, ST_USB_SETUP, ST_USB_WAIT, ST_USB_ON, ST_USB_FAIL,           /* status */
       ST_BT_SETUP, ST_BT_WAIT, ST_BT_ON, ST_BT_FAIL };
enum { A_NONE, A_CLOSE, A_USB, A_BT };                                     /* action for main.c */
enum { T_DOWN, T_MOVE, T_UP };                                             /* touch phase */

typedef struct {
	int screen, status;
	int swapped;                /* 0 = keyboard on the top half */
	int layer;                  /* 0 = letters, 1 = symbols, 2 = F keys */
	unsigned latched;           /* modifiers that are down: under a finger, or locked for the next key */
	unsigned buttons;           /* mouse buttons held */
	unsigned char held[6];      /* keys held */
	struct { unsigned char zone, used; short idx; long ms; } own[G_SLOTS]; /* what each finger is on, since when */
	Gesture g;
} Ui;

/* What one touch event produced. The caller clears it before each call or group of calls. */
typedef struct {
	unsigned char rep[16][HID_KBD_LEN], len[16];
	int n;                      /* HID reports to send, in order */
	Rect tog[8]; int ntog;      /* areas to invert on the screen (ui_invert), unless redraw is set */
	Rect redraw;                /* w > 0: render again and refresh this area */
	int action;
} Out;

int  ui_fonts(const char *dir);                  /* loads the font; 0 = ok */
void ui_render(const Ui *ui, unsigned char *px); /* px: UI_W * UI_H bytes, 0 = black, 255 = white */
void ui_invert(unsigned char *px, Rect r);
Rect ui_scale(const unsigned char *px, Rect r, int sw, int sh, unsigned char *out); /* area r for a sw x sh screen; returns the area there */
void ui_touch(Ui *ui, int slot, int phase, int x, int y, long ms, Out *o);
void ui_status(Ui *ui, int status, Out *o);      /* ST_USB_ON and ST_BT_ON also open the main screen */
int  ui_key_xy(const Ui *ui, const char *label, int *x, int *y); /* centre of a key; 0 = found */
void ui_icon(unsigned char *px);                 /* px: 300 * 450 bytes, the library icon */

#endif

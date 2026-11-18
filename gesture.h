/* Trackpad gestures from touch slots. No device code.
 * 1 finger drag = move, 1 finger tap = left click, 2 finger tap = right click, 2 finger drag = scroll. */
#ifndef KB_GESTURE_H
#define KB_GESTURE_H

#define G_SLOTS 10
#define G_SLOP 24      /* px of travel (all fingers added) before a touch stops being a tap */
#define G_TAP_MS 250   /* a tap is shorter than this */
#define G_GAIN_NUM 3   /* cursor px = finger px * 3 / 2 */
#define G_GAIN_DEN 2
#define G_WHEEL 60     /* px of travel (both fingers added) per wheel step */

typedef struct {
	int n, maxn, moved, travel; /* fingers down now, most fingers in this gesture */
	long t0;
	int x[G_SLOTS], y[G_SLOTS];
	char down[G_SLOTS];
	int rx, ry, rw;             /* remainders */
} Gesture;

typedef struct { int dx, dy, wheel, click; } GOut; /* click: 0 none, 1 left, 2 right */

void g_down(Gesture *g, int slot, int x, int y, long ms);
void g_move(Gesture *g, int slot, int x, int y, GOut *o);
void g_up(Gesture *g, int slot, long ms, GOut *o);

#endif

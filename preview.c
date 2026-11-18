/* PC tool: checks reports, gestures and touch handling, then writes each screen and the icon as PNG.
 * Usage: preview FONT_DIR OUT_DIR */
#include "ui.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static unsigned char px[UI_W * UI_H];
static Out o;

static void shot(const Ui *ui, const char *dir, const char *name)
{
	char path[256];
	snprintf(path, sizeof path, "%s/%s.png", dir, name);
	ui_render(ui, px);
	assert(stbi_write_png(path, UI_W, UI_H, 1, px, UI_W));
}

/* The picture of the last shot as another screen size gets it (Kindle 11: 1072x1448, Scribe: 1860x2480). */
static void shot_at(const char *dir, const char *name, int w, int h)
{
	static unsigned char out[1860 * 2480];
	char path[256];
	Rect d = ui_scale(px, (Rect) { 0, 0, UI_W, UI_H }, w, h, out);
	assert(d.x == 0 && d.y == 0 && d.w == w && d.h == h);
	if (w == UI_W && h == UI_H) { assert(!memcmp(out, px, sizeof px)); return; } /* the tested screen: nothing changes */
	static unsigned char part[200 * 300];
	Rect key = ui_scale(px, (Rect) { 400, 300, 106, 160 }, w, h, part); /* a part lands inside the screen, same pixels */
	assert(key.x >= 0 && key.y >= 0 && key.x + key.w <= w && key.y + key.h <= h && key.w * key.h <= (int) sizeof part);
	for (int j = 0; j < key.h; j++) assert(!memcmp(part + j * key.w, out + (key.y + j) * w + key.x, key.w));
	snprintf(path, sizeof path, "%s/%s-%dx%d.png", dir, name, w, h);
	assert(stbi_write_png(path, w, h, 1, out, w));
}

static void key(Ui *ui, int slot, int phase, const char *label, long ms)
{
	int x, y;
	if (ui_key_xy(ui, label, &x, &y)) { fprintf(stderr, "no key %s on layer %d\n", label, ui->layer); assert(0); }
	ui_touch(ui, slot, phase, x, y, ms, &o);
}

static void tap(Ui *ui, const char *label)
{
	int x, y;
	memset(&o, 0, sizeof o);
	key(ui, 0, T_DOWN, label, 0);
	ui_key_xy(ui, "Space", &x, &y); /* a layer key has changed the keys by now; the finger comes up anywhere */
	ui_touch(ui, 0, T_UP, x, y, 50, &o);
}

/* report i is a keyboard report with these modifiers and this first key */
static int is_kbd(int i, unsigned mods, unsigned code)
{
	return o.len[i] == HID_KBD_LEN && o.rep[i][0] == 1 && o.rep[i][1] == mods && o.rep[i][3] == code;
}

static int is_mouse(int i, unsigned buttons, int dx, int dy, int wheel)
{
	const unsigned char *r = o.rep[i];
	return o.len[i] == HID_MOUSE_LEN && r[0] == 2 && r[1] == buttons && (signed char) r[2] == dx
	       && (signed char) r[3] == dy && (signed char) r[4] == wheel;
}

static void test_gesture(void)
{
	Gesture g = { 0 };
	GOut q = { 0 };
	g_down(&g, 0, 100, 100, 0); g_up(&g, 0, 80, &q);
	assert(q.click == 1);                                   /* 1 finger tap = left */
	q = (GOut) { 0 };
	g_down(&g, 0, 100, 100, 1000); g_down(&g, 1, 300, 100, 1010); g_up(&g, 0, 1060, &q); g_up(&g, 1, 1070, &q);
	assert(q.click == 2);                                   /* 2 finger tap = right */
	q = (GOut) { 0 };
	g_down(&g, 0, 100, 100, 2000); g_up(&g, 0, 2400, &q);
	assert(q.click == 0);                                   /* a long press is not a tap */
	q = (GOut) { 0 };
	g_down(&g, 0, 100, 100, 3000);
	g_move(&g, 0, 110, 100, &q);
	assert(q.dx == 0);                                      /* jitter does not move the cursor */
	g_move(&g, 0, 130, 100, &q); g_move(&g, 0, 150, 110, &q);
	assert(q.dx == 30 && q.dy == 15);                       /* 20,10 px at gain 3/2 */
	g_up(&g, 0, 3050, &q);
	assert(q.click == 0);                                   /* a drag is not a tap */
	q = (GOut) { 0 };
	g_down(&g, 0, 100, 100, 4000); g_down(&g, 1, 300, 100, 4000);
	g_move(&g, 0, 100, 130, &q); g_move(&g, 1, 300, 130, &q);   /* used up as jitter */
	g_move(&g, 0, 100, 190, &q); g_move(&g, 1, 300, 190, &q);
	assert(q.wheel == 2 && q.dx == 0 && q.dy == 0);         /* 2 fingers down 60 px = 2 steps up */
	g_up(&g, 1, 4100, &q);
	g_move(&g, 0, 400, 400, &q);
	assert(q.dx == 0);                                      /* the finger that stays does not jump the cursor */
	g_up(&g, 0, 4200, &q);
	assert(q.click == 0 && g.n == 0);
}

static void test_ui(void)
{
	Ui ui = { 0 };
	int x, y;

	memset(&o, 0, sizeof o);
	ui_touch(&ui, 0, T_DOWN, 300, 500, 0, &o);
	assert(o.action == A_USB);
	ui_touch(&ui, 0, T_DOWN, 900, 500, 0, &o);
	assert(o.action == A_BT);
	memset(&o, 0, sizeof o);
	ui_status(&ui, ST_USB_WAIT, &o);
	assert(ui.screen == S_MODE && o.redraw.h == UI_H);
	ui_status(&ui, ST_USB_ON, &o);
	assert(ui.screen == S_MAIN);
	{
		Ui b = { 0 };                                       /* Bluetooth: the keyboard opens when the computer connects */
		ui_status(&b, ST_BT_SETUP, &o); ui_status(&b, ST_BT_WAIT, &o);
		assert(b.screen == S_MODE);
		ui_status(&b, ST_BT_ON, &o);
		assert(b.screen == S_MAIN);
		ui_status(&b, ST_BT_WAIT, &o);
		assert(b.screen == S_MAIN && o.redraw.h == 96);
	}

	tap(&ui, "A");                                          /* press on finger down, release on finger up */
	assert(o.n == 2 && is_kbd(0, 0, 4) && is_kbd(1, 0, 0) && o.ntog == 2);

	tap(&ui, "Win");                                        /* a tapped modifier is a key: Win opens the menu */
	assert(o.n == 2 && is_kbd(0, MOD_GUI, 0) && is_kbd(1, 0, 0) && ui.latched == 0 && o.ntog == 2);

	tap(&ui, "Shift");                                      /* Shift as on a phone: a tap is for the next key */
	assert(o.n == 1 && is_kbd(0, MOD_SHIFT, 0) && ui.latched == MOD_SHIFT && o.ntog == 1);
	tap(&ui, "A");
	assert(is_kbd(0, MOD_SHIFT, 4) && is_kbd(1, 0, 0) && ui.latched == 0);
	tap(&ui, "Shift"); tap(&ui, "Shift");                   /* a second tap takes it back */
	assert(o.n == 1 && is_kbd(0, 0, 0) && ui.latched == 0);

	memset(&o, 0, sizeof o);                                /* the others: held 500 ms alone = locked for the next key */
	key(&ui, 0, T_DOWN, "Alt", 0); key(&ui, 0, T_UP, "Alt", 500);
	assert(o.n == 1 && is_kbd(0, MOD_ALT, 0) && ui.latched == MOD_ALT && o.ntog == 1);
	tap(&ui, "A");
	assert(is_kbd(0, MOD_ALT, 4) && is_kbd(1, 0, 0) && ui.latched == 0);

	memset(&o, 0, sizeof o);                                /* a touch on a locked modifier unlocks it */
	key(&ui, 0, T_DOWN, "Ctrl", 0); key(&ui, 0, T_UP, "Ctrl", 600);
	key(&ui, 0, T_DOWN, "Ctrl", 700); key(&ui, 0, T_UP, "Ctrl", 1400);
	assert(o.n == 2 && is_kbd(1, 0, 0) && ui.latched == 0);

	memset(&o, 0, sizeof o);                                /* held with keys: Alt down, Tab, Tab, Alt up: no lock */
	key(&ui, 0, T_DOWN, "Alt", 0);
	key(&ui, 1, T_DOWN, "Tab", 10); key(&ui, 1, T_UP, "Tab", 20);
	key(&ui, 1, T_DOWN, "Tab", 30); key(&ui, 1, T_UP, "Tab", 40);
	assert(o.n == 5 && is_kbd(0, MOD_ALT, 0) && is_kbd(1, MOD_ALT, 43) && is_kbd(2, MOD_ALT, 0) && is_kbd(3, MOD_ALT, 43) && is_kbd(4, MOD_ALT, 0));
	key(&ui, 0, T_UP, "Alt", 900);
	assert(o.n == 6 && is_kbd(5, 0, 0) && ui.latched == 0);

	memset(&o, 0, sizeof o);                                /* two keys at once */
	key(&ui, 0, T_DOWN, "A", 0); key(&ui, 1, T_DOWN, "S", 5);
	assert(o.rep[1][3] == 4 && o.rep[1][4] == 22);
	key(&ui, 0, T_UP, "A", 10); key(&ui, 1, T_UP, "S", 15);
	assert(is_kbd(3, 0, 0) && o.rep[3][4] == 0);

	tap(&ui, "Fn");                                         /* F keys */
	assert(ui.layer == 2 && o.redraw.w == UI_W && o.redraw.h == 864);
	tap(&ui, "F5");
	assert(is_kbd(0, 0, 62));
	tap(&ui, "Fn");
	assert(ui.layer == 0);

	tap(&ui, "?123");                                       /* symbols: the key types the character on it */
	assert(ui.layer == 1);
	tap(&ui, "!");
	assert(o.n == 2 && is_kbd(0, MOD_SHIFT, 30) && is_kbd(1, 0, 0));
	tap(&ui, "[");
	assert(o.n == 2 && is_kbd(0, 0, 47) && is_kbd(1, 0, 0));
	memset(&o, 0, sizeof o);                                /* Ctrl held, +: Ctrl+Shift+= */
	key(&ui, 0, T_DOWN, "Ctrl", 0); key(&ui, 1, T_DOWN, "+", 10); key(&ui, 1, T_UP, "+", 20); key(&ui, 0, T_UP, "Ctrl", 30);
	assert(o.n == 4 && is_kbd(1, MOD_CTRL | MOD_SHIFT, 46) && is_kbd(2, MOD_CTRL, 0) && is_kbd(3, 0, 0));
	for (int c = '!'; c <= '~'; c++) {                      /* every character of a US keyboard has a key */
		char label[2] = { (char) c, 0 };
		if (c >= 'a' && c <= 'z') continue;                 /* the key shows the capital */
		ui.layer = 0;
		if (ui_key_xy(&ui, label, &x, &y)) { ui.layer = 1; assert(ui_key_xy(&ui, label, &x, &y) == 0); }
	}
	ui.layer = 1;
	tap(&ui, "ABC");
	assert(ui.layer == 0);

	/* trackpad half: bottom, y 960..1695; pad on top, buttons at its bottom */
	memset(&o, 0, sizeof o);
	ui_touch(&ui, 0, T_DOWN, 600, 1100, 0, &o); ui_touch(&ui, 0, T_UP, 600, 1100, 60, &o);
	assert(o.n == 2 && is_mouse(0, BTN_L, 0, 0, 0) && is_mouse(1, 0, 0, 0, 0));
	memset(&o, 0, sizeof o);
	ui_touch(&ui, 0, T_DOWN, 600, 1100, 1000, &o); ui_touch(&ui, 1, T_DOWN, 800, 1100, 1005, &o);
	ui_touch(&ui, 0, T_UP, 600, 1100, 1060, &o); ui_touch(&ui, 1, T_UP, 800, 1100, 1065, &o);
	assert(o.n == 2 && is_mouse(0, BTN_R, 0, 0, 0) && is_mouse(1, 0, 0, 0, 0));
	memset(&o, 0, sizeof o);                                /* a fast swipe is split into several reports */
	ui_touch(&ui, 0, T_DOWN, 100, 1100, 2000, &o);
	ui_touch(&ui, 0, T_MOVE, 130, 1100, 2010, &o);
	ui_touch(&ui, 0, T_MOVE, 330, 1100, 2020, &o);
	assert(o.n == 3 && is_mouse(0, 0, 127, 0, 0) && is_mouse(1, 0, 127, 0, 0) && is_mouse(2, 0, 46, 0, 0));
	ui_touch(&ui, 0, T_UP, 330, 1100, 2030, &o);

	memset(&o, 0, sizeof o);                                /* drag: hold LEFT with one finger, move with another */
	ui_touch(&ui, 0, T_DOWN, 300, 1600, 3000, &o);
	assert(is_mouse(0, BTN_L, 0, 0, 0) && o.ntog == 1);
	ui_touch(&ui, 1, T_DOWN, 600, 1100, 3010, &o);
	ui_touch(&ui, 1, T_MOVE, 600, 1130, 3020, &o); ui_touch(&ui, 1, T_MOVE, 600, 1150, 3030, &o);
	assert(is_mouse(1, BTN_L, 0, 30, 0));
	ui_touch(&ui, 1, T_UP, 600, 1150, 3500, &o); ui_touch(&ui, 0, T_UP, 300, 1600, 3510, &o);
	assert(is_mouse(o.n - 1, 0, 0, 0, 0));
	memset(&o, 0, sizeof o);
	ui_touch(&ui, 0, T_DOWN, 900, 1600, 4000, &o);
	assert(is_mouse(0, BTN_R, 0, 0, 0));
	ui_touch(&ui, 0, T_UP, 900, 1600, 4050, &o);

	memset(&o, 0, sizeof o);                                /* SWAP: the keyboard moves to the bottom half */
	ui_touch(&ui, 0, T_DOWN, 1150, 40, 5000, &o); ui_touch(&ui, 0, T_UP, 1150, 40, 5050, &o);
	assert(ui.swapped && o.redraw.h == UI_H && ui_key_xy(&ui, "A", &x, &y) == 0 && y > 896);
	memset(&o, 0, sizeof o);
	ui_touch(&ui, 0, T_DOWN, 600, 300, 6000, &o); ui_touch(&ui, 0, T_UP, 600, 300, 6050, &o);
	assert(is_mouse(0, BTN_L, 0, 0, 0));                    /* ... and the trackpad to the top */
	memset(&o, 0, sizeof o);
	ui_status(&ui, ST_USB_WAIT, &o);
	assert(ui.screen == S_MAIN && o.redraw.h == 96);        /* a lost cable changes the bar only */
	memset(&o, 0, sizeof o);
	ui_touch(&ui, 0, T_DOWN, 100, 40, 7000, &o);
	assert(o.action == A_CLOSE);
}

int main(int argc, char **argv)
{
	char path[256];
	unsigned char rep[HID_KBD_LEN], keys[6] = { 4, 5 };
	assert(argc == 3 && ui_fonts(argv[1]) == 0);

	hid_kbd(rep, MOD_SHIFT, keys);
	assert(!memcmp(rep, "\1\2\0\4\5\0\0\0\0", 9));
	hid_mouse(rep, BTN_R, -3, 4, -1);
	assert(!memcmp(rep, "\2\2\375\4\377", 5));
	assert(hid_report_desc[hid_report_desc_len - 1] == 0xC0);
	test_gesture();
	test_ui();

	Ui ui = { 0 };
	shot(&ui, argv[2], "mode");
	shot_at(argv[2], "mode", UI_W, UI_H); shot_at(argv[2], "mode", 1072, 1448); shot_at(argv[2], "mode", 1860, 2480);
	ui.status = ST_USB_WAIT;
	shot(&ui, argv[2], "mode-usb");
	ui.status = ST_BT_WAIT;
	shot(&ui, argv[2], "mode-bt");
	ui.screen = S_MAIN; ui.status = ST_USB_ON;
	shot(&ui, argv[2], "main");
	shot_at(argv[2], "main", 1072, 1448); shot_at(argv[2], "main", 1860, 2480);
	tap(&ui, "Shift"); /* on for the next key */
	memset(&o, 0, sizeof o);
	ui_touch(&ui, 0, T_DOWN, 300, 1600, 0, &o);
	shot(&ui, argv[2], "main-shift-left");
	ui_touch(&ui, 0, T_UP, 300, 1600, 500, &o);
	tap(&ui, "?123");
	shot(&ui, argv[2], "main-sym");
	tap(&ui, "Fn");
	ui.swapped = 1;
	shot(&ui, argv[2], "main-fn-swapped");

	ui_icon(px);
	snprintf(path, sizeof path, "%s/icon.png", argv[2]);
	assert(stbi_write_png(path, 300, 450, 1, px, 300));
	puts("preview: all checks passed");
	return 0;
}

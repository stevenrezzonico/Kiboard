/* Kiboard screens. Layout tables are used by drawing and by touch handling, so they cannot disagree. */
#include "ui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

enum { BLACK = 0, WHITE = 0xFF };
enum { Z_NONE, Z_KEY, Z_MOD, Z_BTN, Z_PAD };

static stbtt_fontinfo sans;
static unsigned char *P;          /* buffer being drawn */
static int BW = UI_W, BH = UI_H;  /* its size */

/* ---- layout ---- */
#define BAR_H 96
#define KB_H 864  /* keyboard half */
#define TP_H 736  /* trackpad half */
#define NROW 6
#define ROW_H (KB_H / NROW)
static const Rect R_FULL  = { 0, 0, UI_W, UI_H };
static const Rect R_BAR   = { 0, 0, UI_W, BAR_H };
static const Rect R_CLOSE = { 16, 12, 200, 72 };
static const Rect R_SWAP  = { UI_W - 216, 12, 200, 72 };
static const Rect R_USB   = { 64, 300, 540, 360 };
static const Rect R_BT    = { 668, 300, 540, 360 };
/* trackpad half, relative to its top */
static const Rect R_PAD   = { 8, 8, UI_W - 16, 608 };
static const Rect R_BTN[2] = { { 8, 624, 624, 104 }, { 640, 624, 624, 104 } };

/* code: HID usage, MOD_ bit, or the layer a K_FN key opens. shift: the character needs Shift on a US layout. */
typedef struct { const char *label; unsigned char code, kind, w, shift; } Key;
enum { K_KEY, K_MOD, K_FN, K_GAP };
enum { L_ABC, L_SYM, L_FN, NLAYER };
#define K(l, c) { l, c, K_KEY, 2, 0 }
#define S(l, c) { l, c, K_KEY, 2, 1 }
#define W(l, c, w) { l, c, K_KEY, w, 0 }
#define MOD(l, bit, w) { l, bit, K_MOD, w, 0 }
#define FN(l, layer, w) { l, layer, K_FN, w, 0 }
#define GAP { "", 0, K_GAP, 1, 0 }

/* letters, as on a phone */
static const Key N0[] = { K("1", 30), K("2", 31), K("3", 32), K("4", 33), K("5", 34), K("6", 35), K("7", 36),
	K("8", 37), K("9", 38), K("0", 39), { 0 } };
static const Key A1[] = { K("Q", 20), K("W", 26), K("E", 8), K("R", 21), K("T", 23), K("Y", 28), K("U", 24),
	K("I", 12), K("O", 18), K("P", 19), { 0 } };
static const Key A2[] = { GAP, K("A", 4), K("S", 22), K("D", 7), K("F", 9), K("G", 10), K("H", 11), K("J", 13),
	K("K", 14), K("L", 15), GAP, { 0 } };
static const Key A3[] = { MOD("Shift", MOD_SHIFT, 3), K("Z", 29), K("X", 27), K("C", 6), K("V", 25), K("B", 5),
	K("N", 17), K("M", 16), W("Bksp", 42, 3), { 0 } };
static const Key A4[] = { FN("?123", L_SYM, 3), K(",", 54), W("Space", 44, 8), K(".", 55), W("Enter", 40, 5), { 0 } };
/* symbols: each key types the character on it */
static const Key Y1[] = { S("@", 31), S("#", 32), S("$", 33), S("%", 34), S("&", 36), S("*", 37), K("-", 45),
	S("+", 46), S("(", 38), S(")", 39), { 0 } };
static const Key Y2[] = { S("!", 30), S("\"", 52), K("'", 52), S(":", 51), K(";", 51), K("/", 56), S("?", 56),
	K("=", 46), S("_", 45), S("~", 53), { 0 } };
static const Key Y3[] = { K("`", 53), S("^", 35), K("\\", 49), S("|", 49), S("<", 54), S(">", 55), K("[", 47),
	K("]", 48), S("{", 47), S("}", 48), W("Bksp", 42, 3), { 0 } };
static const Key Y4[] = { FN("ABC", L_ABC, 3), K(",", 54), W("Space", 44, 8), K(".", 55), W("Enter", 40, 5), { 0 } };
/* F keys and the rest of a PC keyboard */
static const Key F0[] = { K("F1", 58), K("F2", 59), K("F3", 60), K("F4", 61), K("F5", 62), K("F6", 63), { 0 } };
static const Key F1[] = { K("F7", 64), K("F8", 65), K("F9", 66), K("F10", 67), K("F11", 68), K("F12", 69), { 0 } };
static const Key F2[] = { K("Ins", 73), K("Del", 76), K("Home", 74), K("End", 77), K("PgUp", 75), K("PgDn", 78), { 0 } };
static const Key F3[] = { K("Caps", 57), K("PrtSc", 70), K("Menu", 101), K("Bksp", 42), { 0 } };
/* the bottom row is the same on every layer */
static const Key P5[] = { K("Esc", 41), K("Tab", 43), MOD("Ctrl", MOD_CTRL, 2), MOD("Win", MOD_GUI, 2),
	MOD("Alt", MOD_ALT, 2), FN("Fn", L_FN, 2), K("Left", 80), K("Down", 81), K("Up", 82), K("Right", 79), { 0 } };
static const Key *const ROWS[NLAYER][NROW] = { { N0, A1, A2, A3, A4, P5 }, { N0, Y1, Y2, Y3, Y4, P5 },
	{ F0, F1, F2, F3, A4, P5 } };

static struct Box { Rect r; const Key *k; } box[NLAYER][64]; /* r is relative to the top of the keyboard half */
static int nbox[NLAYER];

static void layout(void)
{
	if (nbox[0]) return;
	for (int l = 0; l < NLAYER; l++)
		for (int row = 0; row < NROW; row++) {
			int total = 0, cum = 0;
			for (const Key *k = ROWS[l][row]; k->label; k++) total += k->w;
			for (const Key *k = ROWS[l][row]; k->label; k++) {
				int x0 = cum * UI_W / total, x1 = (cum + k->w) * UI_W / total;
				if (k->kind != K_GAP) box[l][nbox[l]++] = (struct Box) { { x0 + 4, row * ROW_H + 4, x1 - x0 - 8, ROW_H - 8 }, k };
				cum += k->w;
			}
		}
}

static int inside(Rect r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }
static Rect down(Rect r, int dy) { r.y += dy; return r; }
static int kb_y(const Ui *ui) { return BAR_H + (ui->swapped ? TP_H : 0); }
static int tp_y(const Ui *ui) { return BAR_H + (ui->swapped ? 0 : KB_H); }

/* ---- drawing ---- */
static void blend(int x, int y, int color, float a)
{
	if (x < 0 || y < 0 || x >= BW || y >= BH || a <= 0) return;
	unsigned char *d = &P[y * BW + x];
	*d = (unsigned char) (*d + (color - *d) * (a > 1 ? 1 : a));
}

/* Filled rectangle with rounded corners. rad 0 = plain rectangle. */
static void rrect(Rect r, int rad, int color)
{
	for (int y = r.y; y < r.y + r.h; y++)
		for (int x = r.x; x < r.x + r.w; x++) {
			float dx = fmaxf(fmaxf(r.x + rad - x - 0.5f, x + 0.5f - (r.x + r.w - rad)), 0);
			float dy = fmaxf(fmaxf(r.y + rad - y - 0.5f, y + 0.5f - (r.y + r.h - rad)), 0);
			blend(x, y, color, dx > 0 && dy > 0 ? rad - sqrtf(dx * dx + dy * dy) + 0.5f : 1);
		}
}

static void frame(Rect r, int rad, int thick, int border, int fill)
{
	Rect in = { r.x + thick, r.y + thick, r.w - 2 * thick, r.h - 2 * thick };
	rrect(r, rad, border);
	rrect(in, rad - thick, fill);
}

static int text_w(float px, const char *s)
{
	float sc = stbtt_ScaleForPixelHeight(&sans, px), w = 0;
	for (; *s; s++) {
		int adv, lsb;
		stbtt_GetCodepointHMetrics(&sans, *s, &adv, &lsb);
		w += adv * sc + (s[1] ? stbtt_GetCodepointKernAdvance(&sans, s[0], s[1]) * sc : 0);
	}
	return (int) (w + 0.5f);
}

/* Draws s with its left edge at x and its baseline at base. */
static void text(float px, int x, int base, const char *s, int color)
{
	float sc = stbtt_ScaleForPixelHeight(&sans, px), pen = x;
	for (; *s; s++) {
		int adv, lsb, w, h, xo, yo;
		unsigned char *g = stbtt_GetCodepointBitmap(&sans, sc, sc, *s, &w, &h, &xo, &yo);
		for (int j = 0; g && j < h; j++)
			for (int i = 0; i < w; i++)
				blend((int) pen + xo + i, base + yo + j, color, g[j * w + i] / 255.0f);
		stbtt_FreeBitmap(g, NULL);
		stbtt_GetCodepointHMetrics(&sans, *s, &adv, &lsb);
		pen += adv * sc + (s[1] ? stbtt_GetCodepointKernAdvance(&sans, s[0], s[1]) * sc : 0);
	}
}

/* Centers s in r. Capital letters are about 0.72 of the pixel height. */
static void text_mid(float px, Rect r, const char *s, int color)
{
	text(px, r.x + (r.w - text_w(px, s)) / 2, r.y + r.h / 2 + (int) (px * 0.36f), s, color);
}

/* Arrow head in the middle of r. dir: 0 up, 1 down, 2 left, 3 right. */
static void arrow(Rect r, int dir)
{
	int cx = r.x + r.w / 2, cy = r.y + r.h / 2, n = 34;
	for (int j = 0; j < n; j++)       /* j = distance from the tip */
		for (int i = -j; i <= j; i++) {
			int a = j - n / 2;
			if (dir == 0) blend(cx + i, cy + a, BLACK, 1);
			else if (dir == 1) blend(cx + i, cy - a, BLACK, 1);
			else if (dir == 2) blend(cx + a, cy + i, BLACK, 1);
			else blend(cx - a, cy + i, BLACK, 1);
		}
}

void ui_invert(unsigned char *px, Rect r)
{
	for (int y = r.y + 8; y < r.y + r.h - 8; y++)
		for (int x = r.x + 8; x < r.x + r.w - 8; x++)
			px[y * UI_W + x] ^= 0xFF;
}

/* The picture is always drawn for UI_W x UI_H. Kindle screens are all close to 3:4, so another screen gets
 * the same picture stretched to its size: each screen pixel is mixed from the 4 picture pixels around it,
 * then made black or white again. On a UI_W x UI_H screen the result is the picture itself. */
Rect ui_scale(const unsigned char *px, Rect r, int sw, int sh, unsigned char *out)
{
	Rect d = { r.x * sw / UI_W, r.y * sh / UI_H, 0, 0 };
	d.w = ((r.x + r.w) * sw + UI_W - 1) / UI_W - d.x;
	d.h = ((r.y + r.h) * sh + UI_H - 1) / UI_H - d.y;
	for (int j = 0; j < d.h; j++)
		for (int i = 0; i < d.w; i++) {
			long u = (2L * (d.x + i) + 1) * UI_W * 128 / sw - 128, v = (2L * (d.y + j) + 1) * UI_H * 128 / sh - 128; /* 1/256 pixels */
			if (u < 0) u = 0;
			if (v < 0) v = 0;
			int x = (int) (u >> 8), y = (int) (v >> 8), fx = (int) (u & 255), fy = (int) (v & 255);
			int x1 = x + 1 < UI_W ? x + 1 : x, y1 = y + 1 < UI_H ? y + 1 : y;
			int top = px[y * UI_W + x] * (256 - fx) + px[y * UI_W + x1] * fx, bot = px[y1 * UI_W + x] * (256 - fx) + px[y1 * UI_W + x1] * fx;
			out[j * d.w + i] = (top * (256 - fy) + bot * fy) >> 16 < 128 ? BLACK : WHITE;
		}
	return d;
}

static const char *const STATUS[] = { "Choose a connection", "Starting USB...", "Waiting for the computer",
	"USB connected", "USB did not start. See log.txt",
	"Starting Bluetooth, about 10 seconds...", "Waiting: add Kindle on the computer",
	"Bluetooth connected", "Bluetooth did not start. See log.txt" };
static const char *const SHORT[] = { "", "Starting USB", "USB: plug in the cable", "USB connected", "USB failed",
	"Starting Bluetooth", "Bluetooth: waiting for the computer", "Bluetooth connected", "Bluetooth failed" };

static void button(Rect r, const char *s, int on_black)
{
	frame(r, 18, 4, on_black ? WHITE : BLACK, on_black ? BLACK : WHITE);
	text_mid(36, r, s, on_black ? WHITE : BLACK);
}

void ui_render(const Ui *ui, unsigned char *px)
{
	layout();
	P = px; BW = UI_W; BH = UI_H;
	memset(px, WHITE, UI_W * UI_H);
	rrect(R_BAR, 0, BLACK);
	button(R_CLOSE, "CLOSE", 1);

	if (ui->screen == S_MODE) {
		static const char *const HELP[] = { "USB", "1. Tap USB.",
			"2. Plug the cable into the computer, if it is not in.", "BLUETOOTH", "1. Tap BLUETOOTH and wait about 10 seconds.",
			"2. On the computer, add the Bluetooth device Kindle.", "3. If that fails the first time, add it again.",
			"The keyboard opens when the computer answers." };
		int usb = ui->status >= ST_USB_SETUP && ui->status <= ST_USB_ON;
		int bt = ui->status >= ST_BT_SETUP && ui->status <= ST_BT_ON;
		text_mid(48, R_BAR, "Kiboard", WHITE);
		text(56, 64, 230, "Connect with", BLACK);
		frame(R_USB, 28, 6, BLACK, usb ? BLACK : WHITE);
		text_mid(84, R_USB, "USB", usb ? WHITE : BLACK);
		frame(R_BT, 28, 6, BLACK, bt ? BLACK : WHITE);
		text_mid(64, R_BT, "BLUETOOTH", bt ? WHITE : BLACK);
		text(60, 64, 800, STATUS[ui->status], BLACK);
		rrect((Rect) { 64, 850, UI_W - 128, 6 }, 0, BLACK);
		for (int i = 0, y = 960; i < 8; i++) { /* a gap before each heading and before the last line */
			int head = i == 0 || i == 3;
			if (i == 3 || i == 7) y += 36;
			text(head ? 48 : 40, 64, y, HELP[i], BLACK);
			y += 70;
		}
	} else {
		int ky = kb_y(ui), ty = tp_y(ui);
		button(R_SWAP, "SWAP", 1);
		text_mid(36, R_BAR, SHORT[ui->status], WHITE);
		for (int i = 0; i < nbox[ui->layer]; i++) {
			const Key *k = box[ui->layer][i].k;
			Rect r = down(box[ui->layer][i].r, ky);
			frame(r, 14, 4, BLACK, WHITE);
			if (k->kind == K_KEY && k->code >= 79 && k->code <= 82)
				arrow(r, k->code == 82 ? 0 : k->code == 81 ? 1 : k->code == 80 ? 2 : 3);
			else if (k->code != 44)
				text_mid(k->label[1] ? 30 : 52, r, k->label, BLACK);
		}
		frame(down(R_PAD, ty), 20, 4, BLACK, WHITE);
		button(down(R_BTN[0], ty), "LEFT", 0);
		button(down(R_BTN[1], ty), "RIGHT", 0);
	}

	/* 2 levels only: the fast e-ink refresh shows no grays, and edges stay crisp */
	for (int i = 0; i < UI_W * UI_H; i++) px[i] = px[i] < 128 ? BLACK : WHITE;

	if (ui->screen != S_MAIN) return;
	for (int i = 0; i < nbox[ui->layer]; i++) { /* state shown as inverted keys */
		const Key *k = box[ui->layer][i].k;
		if ((k->kind == K_MOD && (ui->latched & k->code)) || (k->kind == K_FN && k->code && k->code == ui->layer))
			ui_invert(px, down(box[ui->layer][i].r, kb_y(ui)));
	}
	for (int s = 0; s < G_SLOTS; s++) {
		if (ui->own[s].zone == Z_KEY) ui_invert(px, down(box[ui->layer][ui->own[s].idx].r, kb_y(ui)));
		if (ui->own[s].zone == Z_BTN) ui_invert(px, down(R_BTN[ui->own[s].idx], tp_y(ui)));
	}
}

/* ---- touch ---- */
#define LOCK_MS 500
static unsigned char *rep(Out *o, int len)
{
	static unsigned char lost[HID_KBD_LEN];
	if (o->n >= 16) return lost;
	o->len[o->n] = (unsigned char) len;
	return o->rep[o->n++];
}

static void tog(Out *o, Rect r) { if (o->ntog < 8) o->tog[o->ntog++] = r; }
static void kbd(const Ui *ui, Out *o, unsigned mods)
{
	for (int s = 0; s < G_SLOTS; s++) /* a key such as ! is Shift+1 for the computer */
		if (ui->own[s].zone == Z_KEY && box[ui->layer][ui->own[s].idx].k->shift) mods |= MOD_SHIFT;
	hid_kbd(rep(o, HID_KBD_LEN), mods, ui->held);
}
static int clamp(int v) { return v > 127 ? 127 : v < -127 ? -127 : v; }

static void mouse(const Ui *ui, Out *o, unsigned buttons, int dx, int dy, int wheel)
{
	do { /* a fast swipe is more than one report can hold */
		int x = clamp(dx), y = clamp(dy), w = clamp(wheel);
		hid_mouse(rep(o, HID_MOUSE_LEN), buttons, x, y, w);
		dx -= x; dy -= y; wheel -= w;
	} while (dx || dy || wheel);
	(void) ui;
}

static void set_latched(Ui *ui, unsigned v, Out *o)
{
	unsigned diff = ui->latched ^ v;
	ui->latched = v;
	for (int i = 0; i < nbox[ui->layer]; i++)
		if (box[ui->layer][i].k->kind == K_MOD && (box[ui->layer][i].k->code & diff))
			tog(o, down(box[ui->layer][i].r, kb_y(ui)));
}

/* Lets go of everything on the host. Used before the layout changes under the fingers. */
static void release_all(Ui *ui, Out *o)
{
	memset(ui->held, 0, sizeof ui->held);
	memset(ui->own, 0, sizeof ui->own);
	memset(&ui->g, 0, sizeof ui->g);
	ui->latched = ui->buttons = 0;
	kbd(ui, o, 0);
	mouse(ui, o, 0, 0, 0, 0);
}

static void pad_out(Ui *ui, const GOut *g, Out *o)
{
	if (g->dx || g->dy || g->wheel) mouse(ui, o, ui->buttons, g->dx, g->dy, g->wheel);
	if (g->click) {
		mouse(ui, o, ui->buttons | (g->click == 1 ? BTN_L : BTN_R), 0, 0, 0);
		mouse(ui, o, ui->buttons, 0, 0, 0);
	}
}

static void main_down(Ui *ui, int slot, int x, int y, long ms, Out *o)
{
	if (inside(R_CLOSE, x, y)) { o->action = A_CLOSE; return; }
	if (inside(R_SWAP, x, y)) { release_all(ui, o); ui->swapped ^= 1; o->redraw = R_FULL; return; }
	if (y < BAR_H) return;

	if (y >= kb_y(ui) && y < kb_y(ui) + KB_H) {
		int i = 0, ry = y - kb_y(ui);
		for (; i < nbox[ui->layer]; i++) {
			Rect r = box[ui->layer][i].r; /* the 4 px gaps belong to the keys too */
			if (inside((Rect) { r.x - 4, r.y - 4, r.w + 8, r.h + 8 }, x, ry)) break;
		}
		if (i == nbox[ui->layer]) return;
		const Key *k = box[ui->layer][i].k;
		Rect r = down(box[ui->layer][i].r, kb_y(ui));
		if (k->kind == K_FN) {
			release_all(ui, o);
			ui->layer = ui->layer == k->code ? L_ABC : k->code; /* Fn on the Fn layer goes back to the letters */
			o->redraw = (Rect) { 0, kb_y(ui), UI_W, KB_H };
		} else if (k->kind == K_MOD) {
			/* a modifier goes down like any key; a touch on a locked one unlocks it */
			ui->own[slot].zone = Z_MOD; ui->own[slot].idx = (short) i; ui->own[slot].ms = ms;
			ui->own[slot].used = (ui->latched & k->code) != 0;
			set_latched(ui, ui->latched ^ k->code, o);
			kbd(ui, o, ui->latched);
		} else {
			for (int s = 0; s < G_SLOTS; s++) if (ui->own[s].zone == Z_MOD) ui->own[s].used = 1;
			for (int h = 0; h < 6; h++) if (!ui->held[h]) { ui->held[h] = k->code; break; }
			ui->own[slot].zone = Z_KEY; ui->own[slot].idx = (short) i;
			tog(o, r);
			kbd(ui, o, ui->latched);
		}
		return;
	}

	int ry = y - tp_y(ui);
	if (ry >= R_BTN[0].y) {
		int b = x >= UI_W / 2;
		ui->own[slot].zone = Z_BTN; ui->own[slot].idx = (short) b;
		ui->buttons |= b ? BTN_R : BTN_L;
		tog(o, down(R_BTN[b], tp_y(ui)));
		mouse(ui, o, ui->buttons, 0, 0, 0);
	} else {
		ui->own[slot].zone = Z_PAD;
		g_down(&ui->g, slot, x, y, ms);
	}
}

static void main_up(Ui *ui, int slot, long ms, Out *o)
{
	int zone = ui->own[slot].zone, i = ui->own[slot].idx;
	ui->own[slot].zone = Z_NONE;
	if (zone == Z_KEY) {
		unsigned fingers = 0; /* a modifier under a finger stays on: hold Alt, tap Tab twice */
		for (int h = 0; h < 6; h++) if (ui->held[h] == box[ui->layer][i].k->code) { ui->held[h] = 0; break; }
		for (int s = 0; s < G_SLOTS; s++) if (ui->own[s].zone == Z_MOD) fingers |= box[ui->layer][ui->own[s].idx].k->code;
		tog(o, down(box[ui->layer][i].r, kb_y(ui)));
		set_latched(ui, ui->latched & fingers, o);
		kbd(ui, o, ui->latched);
	} else if (zone == Z_MOD) {
		/* Stays down for the next key: Shift after a tap, as on a phone; the others when held alone for LOCK_MS. */
		unsigned code = box[ui->layer][i].k->code;
		if ((ui->latched & code) && (ui->own[slot].used || (code != MOD_SHIFT && ms - ui->own[slot].ms < LOCK_MS))) {
			set_latched(ui, ui->latched & ~code, o);
			kbd(ui, o, ui->latched);
		}
	} else if (zone == Z_BTN) {
		ui->buttons &= ~(i ? BTN_R : BTN_L);
		tog(o, down(R_BTN[i], tp_y(ui)));
		mouse(ui, o, ui->buttons, 0, 0, 0);
	} else if (zone == Z_PAD) {
		GOut g = { 0 };
		g_up(&ui->g, slot, ms, &g);
		pad_out(ui, &g, o);
	}
}

void ui_touch(Ui *ui, int slot, int phase, int x, int y, long ms, Out *o)
{
	layout();
	if (slot < 0 || slot >= G_SLOTS) return;
	if (ui->screen == S_MODE) {
		if (phase != T_DOWN) return;
		if (inside(R_CLOSE, x, y)) o->action = A_CLOSE;
		else if (inside(R_USB, x, y)) o->action = A_USB;
		else if (inside(R_BT, x, y)) o->action = A_BT;
	} else if (phase == T_DOWN) {
		if (ui->own[slot].zone == Z_NONE) main_down(ui, slot, x, y, ms, o);
	} else if (phase == T_UP) {
		main_up(ui, slot, ms, o);
	} else if (ui->own[slot].zone == Z_PAD) {
		GOut g = { 0 };
		g_move(&ui->g, slot, x, y, &g);
		pad_out(ui, &g, o);
	}
}

void ui_status(Ui *ui, int status, Out *o)
{
	if (status == ui->status) return;
	ui->status = status;
	if (ui->screen == S_MODE && (status == ST_USB_ON || status == ST_BT_ON)) ui->screen = S_MAIN;
	else if (ui->screen == S_MAIN) { o->redraw = R_BAR; return; }
	o->redraw = R_FULL;
}

int ui_key_xy(const Ui *ui, const char *label, int *x, int *y)
{
	layout();
	for (int i = 0; i < nbox[ui->layer]; i++)
		if (!strcmp(box[ui->layer][i].k->label, label)) {
			Rect r = down(box[ui->layer][i].r, kb_y(ui));
			*x = r.x + r.w / 2; *y = r.y + r.h / 2;
			return 0;
		}
	return -1;
}

void ui_icon(unsigned char *px)
{
	Rect f = { 24, 60, 252, 330 }, pad = { 60, 250, 180, 100 };
	P = px; BW = 300; BH = 450;
	memset(px, WHITE, 300 * 450);
	frame(f, 30, 10, BLACK, WHITE);
	for (int j = 0; j < 2; j++)
		for (int i = 0; i < 4; i++) rrect((Rect) { 60 + i * 48, 110 + j * 56, 36, 44 }, 8, BLACK);
	frame(pad, 14, 8, BLACK, WHITE);
}

static int load(stbtt_fontinfo *f, const char *dir, const char *name)
{
	char path[256];
	snprintf(path, sizeof path, "%s/%s", dir, name);
	FILE *fp = fopen(path, "rb");
	if (!fp) return -1;
	fseek(fp, 0, SEEK_END);
	long n = ftell(fp);
	rewind(fp);
	unsigned char *data = malloc(n); /* kept for the life of the program: stb reads from it */
	int ok = data && fread(data, 1, n, fp) == (size_t) n && stbtt_InitFont(f, data, 0);
	fclose(fp);
	return ok ? 0 : -1;
}

int ui_fonts(const char *dir) { return load(&sans, dir, "Inter-Bold.otf"); }

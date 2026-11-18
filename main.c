/* Device side: e-ink output through FBInk, multi-touch from evdev, reports out through usb.c or bt.c.
 * The screens and all touch logic are in ui.c. Everything on stderr goes to log.txt. */
#include "ui.h"
#include "usb.h"
#include "bt.h"
#include "fbink.h"

#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define IDLE_CLOSE_S 600 /* the app closes itself: a dead touch screen must never lock the Kindle UI */

static const char *dir = APP_DIR;
static Ui ui;
static unsigned char px[UI_W * UI_H], *part; /* the picture, and the piece of it on its way to the screen */
static int sw = UI_W, sh = UI_H;             /* the real screen */
static int fbfd = -1, usbfd = -1, btfd = -1;
static FBInkConfig cfg;
static volatile sig_atomic_t quit;

static void on_signal(int s) { (void) s; quit = 1; }

static long now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000L + t.tv_nsec / 1000000;
}

/* Sends one area of px to the screen. Small areas use the fast black-and-white refresh, with no flash. */
static void refresh(Rect r)
{
	int full = r.h == UI_H;
	r = ui_scale(px, r, sw, sh, part);
	cfg.is_flashing = full;
	cfg.wfm_mode = full ? WFM_GC16 : WFM_DU;
	int rc = fbink_print_raw_data(fbfd, part, r.w, r.h, (size_t) r.w * r.h, (short) r.x, (short) r.y, &cfg);
	if (rc < 0) fprintf(stderr, "fbink_print_raw_data: %d\n", rc);
}

static void apply(Out *o);

static void status(int st)
{
	Out o = { 0 };
	ui_status(&ui, st, &o);
	apply(&o);
}

static void apply(Out *o)
{
	for (int i = 0; i < o->n; i++) {
		if (usbfd >= 0) usb_send(o->rep[i], o->len[i]);
		else if (btfd >= 0) bt_send(o->rep[i], o->len[i]);
	}
	if (o->redraw.w) { ui_render(&ui, px); refresh(o->redraw); }
	else for (int i = 0; i < o->ntog; i++) { ui_invert(px, o->tog[i]); refresh(o->tog[i]); }

	if (o->action == A_CLOSE) quit = 1;
	else if (usbfd >= 0 || btfd >= 0) return; /* one connection per run */
	else if (o->action == A_USB) {
		status(ST_USB_SETUP);
		usbfd = usb_open(dir);
		status(usbfd < 0 ? ST_USB_FAIL : ST_USB_WAIT);
	} else if (o->action == A_BT) {
		status(ST_BT_SETUP);
		btfd = bt_open(dir);
		if (btfd < 0) status(ST_BT_FAIL);
	}
}

static int max_x, max_y;

/* Finds the multi-touch screen and takes it, so the Kindle UI below gets no taps. Also finds the power key. */
static void open_inputs(int *touch, int *power)
{
	*touch = *power = -1;
	for (int i = 0; i < 16; i++) {
		char path[32], name[80] = "?";
		unsigned long bits[(ABS_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = { 0 }, keys[(KEY_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = { 0 };
		struct input_absinfo ax, ay;
		snprintf(path, sizeof path, "/dev/input/event%d", i);
		int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC); /* not for the Bluetooth helper: it would keep the touch screen taken */
		if (fd < 0) continue;
		ioctl(fd, EVIOCGNAME(sizeof name), name);
		if (*touch < 0 && ioctl(fd, EVIOCGBIT(EV_ABS, sizeof bits), bits) >= 0
		    && (bits[ABS_MT_POSITION_X / (8 * sizeof(long))] >> (ABS_MT_POSITION_X % (8 * sizeof(long))) & 1)
		    && ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ax) >= 0 && ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay) >= 0
		    && ax.maximum > 0 && ay.maximum > 0) {
			max_x = ax.maximum; max_y = ay.maximum;
			fprintf(stderr, "touch: %s \"%s\" x 0..%d y 0..%d grab=%d\n", path, name, max_x, max_y, ioctl(fd, EVIOCGRAB, 1));
			*touch = fd;
		} else if (*power < 0 && ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) >= 0 /* any device that has a power key */
		           && (keys[KEY_POWER / (8 * sizeof(long))] >> (KEY_POWER % (8 * sizeof(long))) & 1)) {
			fprintf(stderr, "power key: %s \"%s\"\n", path, name);
			*power = fd;
		} else close(fd);
	}
}

/* Multi-touch protocol B: events fill slots, SYN_REPORT ends a frame. */
static struct { int x, y, active, down, up, moved; } sl[G_SLOTS];

/* The kernel sends a position only when it differs from the slot's last one, also across programs.
 * So a first touch at the place of the last touch before we started would arrive with no position. */
static void seed_slots(int fd)
{
	struct { __u32 code; __s32 v[G_SLOTS]; } qx = { ABS_MT_POSITION_X, { 0 } }, qy = { ABS_MT_POSITION_Y, { 0 } };
	if (ioctl(fd, EVIOCGMTSLOTS(sizeof qx), &qx) < 0 || ioctl(fd, EVIOCGMTSLOTS(sizeof qy), &qy) < 0) return;
	for (int s = 0; s < G_SLOTS; s++) {
		sl[s].x = (int) ((long) qx.v[s] * UI_W / (max_x + 1));
		sl[s].y = (int) ((long) qy.v[s] * UI_H / (max_y + 1));
	}
}

static void touch_events(int fd)
{
	static int cur;
	struct input_event ev[64];
	ssize_t got;
	while ((got = read(fd, ev, sizeof ev)) > 0)
		for (int i = 0; i < (int) (got / sizeof ev[0]); i++) {
			int v = ev[i].value;
			if (ev[i].type == EV_ABS && ev[i].code == ABS_MT_SLOT) cur = v >= 0 && v < G_SLOTS ? v : -1;
			else if (cur < 0) continue;
			else if (ev[i].type == EV_ABS && ev[i].code == ABS_MT_TRACKING_ID) { if (v < 0) sl[cur].up = 1; else sl[cur].down = 1; }
			else if (ev[i].type == EV_ABS && ev[i].code == ABS_MT_POSITION_X) { sl[cur].x = (int) ((long) v * UI_W / (max_x + 1)); sl[cur].moved = 1; }
			else if (ev[i].type == EV_ABS && ev[i].code == ABS_MT_POSITION_Y) { sl[cur].y = (int) ((long) v * UI_H / (max_y + 1)); sl[cur].moved = 1; }
			else if (ev[i].type == EV_SYN && ev[i].code == SYN_REPORT) {
				Out o = { 0 };
				long ms = now_ms();
				for (int s = 0; s < G_SLOTS; s++) {
					if (sl[s].down) { sl[s].active = 1; ui_touch(&ui, s, T_DOWN, sl[s].x, sl[s].y, ms, &o); }
					else if (sl[s].moved && sl[s].active) ui_touch(&ui, s, T_MOVE, sl[s].x, sl[s].y, ms, &o);
					if (sl[s].up && sl[s].active) { sl[s].active = 0; ui_touch(&ui, s, T_UP, sl[s].x, sl[s].y, ms, &o); }
					sl[s].down = sl[s].up = sl[s].moved = 0;
				}
				apply(&o);
			}
		}
}

int main(void)
{
	if (getenv("APP_DIR")) dir = getenv("APP_DIR");
	char fonts[256];
	snprintf(fonts, sizeof fonts, "%s/font", dir);
	if (ui_fonts(fonts)) { fprintf(stderr, "cannot load fonts from %s\n", fonts); return 1; }

	FBInkState st;
	cfg.is_quiet = true;
	if ((fbfd = fbink_open()) < 0 || fbink_init(fbfd, &cfg) < 0) { fprintf(stderr, "cannot open the screen\n"); return 1; }
	fbink_get_state(&cfg, &st);
	fprintf(stderr, "screen: %s %ux%u bpp %u rota %u\n", st.device_name, st.screen_width, st.screen_height, st.bpp, st.current_rota);
	sw = (int) st.screen_width; sh = (int) st.screen_height;
	if (sw != UI_W || sh != UI_H) fprintf(stderr, "the picture is drawn for %dx%d and stretched to this screen (not tested on one)\n", UI_W, UI_H);
	if (sw < 300 || sh < 300 || sw > 4000 || sh > 4000 || !(part = malloc((size_t) sw * sh))) { fprintf(stderr, "cannot use this screen\n"); return 1; }

	int tfd, pfd;
	open_inputs(&tfd, &pfd);
	if (tfd < 0) fprintf(stderr, "no touch screen found: the app closes after %d s\n", IDLE_CLOSE_S);
	else seed_slots(tfd);

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGHUP, on_signal);

	ui_render(&ui, px);
	refresh((Rect) { 0, 0, UI_W, UI_H });

	long last = now_ms(), again = last + 1500;
	while (!quit) {
		struct pollfd p[4] = { { tfd, POLLIN, 0 }, { pfd, POLLIN, 0 }, { usbfd, POLLIN, 0 }, { btfd, POLLIN, 0 } }; /* poll skips fd -1 */
		if (poll(p, 4, 500) < 0) break;
		/* Seen once: started from the library, the screen stayed white although the picture was in the frame
		 * buffer. The library's own page change had come after ours. So the picture is sent a second time. */
		if (again && now_ms() > again) { again = 0; ui_render(&ui, px); refresh((Rect) { 0, 0, UI_W, UI_H }); }
		if (p[0].revents) { last = now_ms(); touch_events(tfd); }
		if (p[1].revents) { /* the power key is the way out that needs no touch screen */
			struct input_event ev[8];
			ssize_t got = read(pfd, ev, sizeof ev);
			for (int i = 0; i < (int) (got / sizeof ev[0]); i++)
				if (ev[i].type == EV_KEY && ev[i].code == KEY_POWER && ev[i].value == 1) { fprintf(stderr, "power key: closing\n"); quit = 1; }
		}
		if (p[2].revents) {
			int on = usb_event();
			if (ui.status == ST_USB_WAIT || ui.status == ST_USB_ON) status(on ? ST_USB_ON : ST_USB_WAIT);
		}
		if (p[3].revents) {
			static const int st[] = { ST_BT_SETUP, ST_BT_WAIT, ST_BT_ON, ST_BT_FAIL };
			int s = bt_event();
			status(st[s]);
			if (s == BT_FAILED) { bt_close(); btfd = -1; } /* the Kindle's own Bluetooth is back; USB can be chosen */
		}
		if (now_ms() - last > IDLE_CLOSE_S * 1000L) { fprintf(stderr, "idle: closing\n"); break; }
	}
	usb_close();
	bt_close();
	if (tfd >= 0) ioctl(tfd, EVIOCGRAB, 0);
	fbink_close(fbfd);
	return 0;
}

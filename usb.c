/* USB HID device through FunctionFS. The Kindle kernel has no HID gadget function, so this program
 * answers the HID requests itself. usb.sh makes and removes the gadget. Little-endian CPU assumed.
 * Two interfaces, keyboard and mouse, each with reports that have no report ID: that is the fixed
 * "boot" form a BIOS or boot menu reads without looking at the report descriptor. */
#include "usb.h"
#include "hid.h"

#include <fcntl.h>
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FFS "/dev/usb-ffs/kiboard"

struct hid_desc {
	__u8 bLength, bDescriptorType;
	__le16 bcdHID;
	__u8 bCountryCode, bNumDescriptors, bReportType;
	__le16 wReportLength;
} __attribute__((packed));

struct func {
	struct usb_interface_descriptor intf;
	struct hid_desc hid;
	struct usb_endpoint_descriptor_no_audio ep;
} __attribute__((packed));

/* i: interface 0 = keyboard, 1 = mouse. Subclass 1 = boot; protocol 1 = keyboard, 2 = mouse. */
#define FUNC(i, interval) { \
	{ sizeof(struct usb_interface_descriptor), USB_DT_INTERFACE, i, 0, 1, USB_CLASS_HID, 1, i + 1, 1 }, \
	{ sizeof(struct hid_desc), 0x21, 0x0111, 0, 1, 0x22, 0 }, \
	{ sizeof(struct usb_endpoint_descriptor_no_audio), USB_DT_ENDPOINT, (i + 1) | USB_DIR_IN, USB_ENDPOINT_XFER_INT, 16, interval } }

static struct {
	struct usb_functionfs_descs_head_v2 head;
	__le32 fs_count, hs_count;
	struct func fs[2], hs[2];
} __attribute__((packed)) descs = {
	{ FUNCTIONFS_DESCRIPTORS_MAGIC_V2, sizeof descs, FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC },
	6, 6, { FUNC(0, 1), FUNC(1, 1) }, { FUNC(0, 4), FUNC(1, 4) }, /* all intervals are 1 ms */
};

static unsigned char rdesc[2][96]; /* report descriptor of each interface: its part of hid_report_desc, no report ID */
static int rlen[2];
static unsigned char proto[2] = { 1, 1 }; /* 0 = the computer asked for boot reports */

static const struct {
	struct usb_functionfs_strings_head head;
	__le16 lang;
	char name[8];
} __attribute__((packed)) strs = { { FUNCTIONFS_STRINGS_MAGIC, sizeof strs, 1, 1 }, 0x0409, "Kiboard" };

static int ep0 = -1, ep[2] = { -1, -1 }, on; /* on: the computer is awake */

/* Reports on their way out, per interface. A write to an endpoint waits until the computer takes the report:
 * 1 ms when it asks for reports, forever when it does not (a BIOS never asks the mouse). The main loop must
 * not wait: it also answers the computer's setup requests. So one thread per interface does the writes.
 * A BIOS was seen to talk to the keyboard for only 1.3 s. So the report waits in the endpoint, as in a real
 * keyboard, and each time the computer sets the device up it gets the keys that are down at that moment:
 * a key held through the start of the computer arrives at the first question.
 * Measured on an HP laptop: its BIOS set boot protocol, asked every 10 ms, took Esc and F9, and did not act
 * on them. Nothing in the reports or requests was left to correct. Keys are never written to the log. */
#define QLEN 32

/* Seconds since the Kindle started, as in the kernel log (dmesg), so the two can be read side by side. */
static double up(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}
static struct { unsigned char rep[QLEN][HID_KBD_LEN], len[QLEN]; int head, n; } q[2];
static unsigned char keys[HID_KBD_LEN - 1]; /* the keyboard as it is now */
static int live;                            /* the computer has set the device up */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;

static void push(int i, const unsigned char *rep, int len) /* with the lock held */
{
	if (q[i].n == QLEN) { q[i].head = (q[i].head + 1) % QLEN; q[i].n--; } /* nobody takes them: the oldest goes */
	int at = (q[i].head + q[i].n++) % QLEN;
	memcpy(q[i].rep[at], rep, len);
	q[i].len[at] = (unsigned char) len;
	pthread_cond_broadcast(&wake);
}

static void *writer(void *arg)
{
	int i = arg != NULL;
	for (;;) {
		unsigned char rep[HID_KBD_LEN];
		pthread_mutex_lock(&lock);
		while (!live || !q[i].n) pthread_cond_wait(&wake, &lock);
		int len = q[i].len[q[i].head], fd = ep[i];
		memcpy(rep, q[i].rep[q[i].head], len);
		q[i].head = (q[i].head + 1) % QLEN; q[i].n--;
		pthread_mutex_unlock(&lock);
		if (write(fd, rep, len) < 0) perror("usb: send"); /* the computer reset the device: it gets the keys again */
	}
	return NULL;
}

/* The computer set the device up (up = 1) or let it go. */
static void set_live(int up)
{
	pthread_mutex_lock(&lock);
	live = up;
	q[0].n = q[1].n = 0;
	if (up) push(0, keys, sizeof keys);
	else proto[0] = proto[1] = 1; /* a reset puts a HID device back in report protocol */
	pthread_mutex_unlock(&lock);
}
static char script[300];

static int sh(const char *arg)
{
	char cmd[340];
	snprintf(cmd, sizeof cmd, "sh %s %s", script, arg);
	return system(cmd);
}

int usb_open(const char *dir)
{
	snprintf(script, sizeof script, "%s/bin/usb.sh", dir);
	for (int i = 0; i < 2; i++) {
		const unsigned char *part = hid_report_desc + (i ? HID_KBD_DESC_LEN : 0);
		rlen[i] = (i ? hid_report_desc_len - HID_KBD_DESC_LEN : HID_KBD_DESC_LEN) - 2;
		memcpy(rdesc[i], part, 6);                       /* bytes 6 and 7 are the report ID */
		memcpy(rdesc[i] + 6, part + 8, rlen[i] - 6);
		descs.fs[i].hid.wReportLength = descs.hs[i].hid.wReportLength = (__le16) rlen[i];
		proto[i] = 1;
	}
	if (sh("on")) { fprintf(stderr, "usb: usb.sh on failed\n"); return -1; }
	if ((ep0 = open(FFS "/ep0", O_RDWR)) < 0
	    || write(ep0, &descs, sizeof descs) != sizeof descs
	    || write(ep0, &strs, sizeof strs) != sizeof strs
	    || (ep[0] = open(FFS "/ep1", O_WRONLY)) < 0 || (ep[1] = open(FFS "/ep2", O_WRONLY)) < 0) { perror("usb: functionfs"); usb_close(); return -1; }
	pthread_t t;
	if (pthread_create(&t, NULL, writer, NULL) || pthread_create(&t, NULL, writer, &t)) { fprintf(stderr, "usb: no thread\n"); usb_close(); return -1; }
	if (sh("bind")) { fprintf(stderr, "usb: no USB controller is free\n"); usb_close(); return -1; }
	return ep0;
}

static void setup(const struct usb_ctrlrequest *rq)
{
	unsigned char buf[64] = { 0 };
	const void *data = NULL;
	int n = -1; /* -1 = not supported */
	int i = (rq->wIndex & 0xFF) == 1; /* FunctionFS gives the interface as numbered in descs */
	if (rq->bRequestType == (USB_DIR_IN | USB_RECIP_INTERFACE) && rq->bRequest == USB_REQ_GET_DESCRIPTOR) {
		if (rq->wValue >> 8 == 0x22) { data = rdesc[i]; n = rlen[i]; }
		else if (rq->wValue >> 8 == 0x21) { data = &descs.hs[i].hid; n = sizeof descs.hs[i].hid; }
	} else if (rq->bRequestType == (USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE)) {
		data = buf;
		if (rq->bRequest == 1) n = (i ? HID_MOUSE_LEN : HID_KBD_LEN) - 1; /* GET_REPORT: nothing held */
		else if (rq->bRequest == 2) n = 1;                 /* GET_IDLE: 0 */
		else if (rq->bRequest == 3) { buf[0] = proto[i]; n = 1; } /* GET_PROTOCOL */
	} else if (rq->bRequestType == (USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE)) {
		/* SET_REPORT (keyboard lights), SET_IDLE: accept and ignore. SET_PROTOCOL: the keyboard's boot report
		 * is its normal report; the mouse's is the first 3 bytes of it (usb_send). */
		if (rq->bRequest == 9 || rq->bRequest == 10 || rq->bRequest == 11) {
			if (rq->bRequest == 11) { proto[i] = rq->wValue != 0; fprintf(stderr, "usb: interface %d protocol %d\n", i, proto[i]); }
			if (read(ep0, buf, rq->wLength < sizeof buf ? rq->wLength : sizeof buf) < 0) perror("usb: ep0 read");
			return;
		}
	}
	if (n < 0) { /* reading on an IN request or writing on an OUT request stalls it: "not supported" */
		fprintf(stderr, "usb: refused request type %02x req %02x value %04x\n", rq->bRequestType, rq->bRequest, rq->wValue);
		if (rq->bRequestType & USB_DIR_IN) n = read(ep0, buf, 0); else n = write(ep0, buf, 0);
		return;
	}
	if (write(ep0, data, n < rq->wLength ? n : rq->wLength) < 0) perror("usb: ep0 write");
}

int usb_event(void)
{
	struct usb_functionfs_event ev;
	if (read(ep0, &ev, sizeof ev) != sizeof ev) return on;
	switch (ev.type) {
	case FUNCTIONFS_SETUP:   setup(&ev.u.setup); break;
	case FUNCTIONFS_ENABLE:  set_live(1); on = 1; break;
	case FUNCTIONFS_RESUME:  on = 1; break;
	case FUNCTIONFS_DISABLE:
	case FUNCTIONFS_UNBIND:  set_live(0); on = 0; break;
	case FUNCTIONFS_SUSPEND: on = 0; break; /* reports still go out: a BIOS was seen silent on the bus, not asleep */
	}
	if (ev.type != FUNCTIONFS_SETUP) fprintf(stderr, "usb: %.3f event %d, connected %d\n", up(), ev.type, on);
	return on;
}

void usb_send(const unsigned char *report, int len)
{
	int i = report[0] == 2; /* hid.c puts a report ID first (Bluetooth needs it): 1 keyboard, 2 mouse. Not sent here. */
	pthread_mutex_lock(&lock);
	if (!i) memcpy(keys, report + 1, sizeof keys);
	if (live) push(i, report + 1, i && !proto[1] ? 3 : len - 1);
	pthread_mutex_unlock(&lock);
}

void usb_close(void)
{
	if (!script[0]) return;
	set_live(0); /* the writers stop; a write that waits ends when usb.sh takes the gadget away */
	for (int i = 0; i < 2; i++) if (ep[i] >= 0) close(ep[i]);
	if (ep0 >= 0) close(ep0);
	ep0 = ep[0] = ep[1] = -1; on = 0;
	sh("off");
}

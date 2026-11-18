/* USB HID device through FunctionFS. The Kindle kernel has no HID gadget function, so this program
 * answers the HID requests itself. usb.sh makes and removes the gadget. Little-endian CPU assumed. */
#include "usb.h"
#include "hid.h"

#include <fcntl.h>
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

#define FUNC(interval) { \
	{ sizeof(struct usb_interface_descriptor), USB_DT_INTERFACE, 0, 0, 1, USB_CLASS_HID, 0, 0, 1 }, \
	{ sizeof(struct hid_desc), 0x21, 0x0111, 0, 1, 0x22, 0 }, \
	{ sizeof(struct usb_endpoint_descriptor_no_audio), USB_DT_ENDPOINT, 1 | USB_DIR_IN, USB_ENDPOINT_XFER_INT, 16, interval } }

static struct {
	struct usb_functionfs_descs_head_v2 head;
	__le32 fs_count, hs_count;
	struct func fs, hs;
} __attribute__((packed)) descs = {
	{ FUNCTIONFS_DESCRIPTORS_MAGIC_V2, sizeof descs, FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC },
	3, 3, FUNC(1), FUNC(4), /* both intervals are 1 ms */
};

static const struct {
	struct usb_functionfs_strings_head head;
	__le16 lang;
	char name[8];
} __attribute__((packed)) strs = { { FUNCTIONFS_STRINGS_MAGIC, sizeof strs, 1, 1 }, 0x0409, "Kiboard" };

static int ep0 = -1, ep1 = -1, on;
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
	descs.fs.hid.wReportLength = descs.hs.hid.wReportLength = (__le16) hid_report_desc_len;
	if (sh("on")) { fprintf(stderr, "usb: usb.sh on failed\n"); return -1; }
	if ((ep0 = open(FFS "/ep0", O_RDWR)) < 0
	    || write(ep0, &descs, sizeof descs) != sizeof descs
	    || write(ep0, &strs, sizeof strs) != sizeof strs
	    || (ep1 = open(FFS "/ep1", O_WRONLY)) < 0) { perror("usb: functionfs"); usb_close(); return -1; }
	if (sh("bind")) { fprintf(stderr, "usb: no USB controller is free\n"); usb_close(); return -1; }
	return ep0;
}

static void setup(const struct usb_ctrlrequest *rq)
{
	unsigned char buf[64] = { 0 };
	const void *data = NULL;
	int n = -1; /* -1 = not supported */
	if (rq->bRequestType == (USB_DIR_IN | USB_RECIP_INTERFACE) && rq->bRequest == USB_REQ_GET_DESCRIPTOR) {
		if (rq->wValue >> 8 == 0x22) { data = hid_report_desc; n = hid_report_desc_len; }
		else if (rq->wValue >> 8 == 0x21) { data = &descs.hs.hid; n = sizeof descs.hs.hid; }
	} else if (rq->bRequestType == (USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE)) {
		data = buf;
		if (rq->bRequest == 1) { buf[0] = (unsigned char) rq->wValue; n = (rq->wValue & 0xFF) == 2 ? HID_MOUSE_LEN : HID_KBD_LEN; } /* GET_REPORT: nothing held */
		else if (rq->bRequest == 2) n = 1;                 /* GET_IDLE: 0 */
		else if (rq->bRequest == 3) { buf[0] = 1; n = 1; } /* GET_PROTOCOL: report protocol */
	} else if (rq->bRequestType == (USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE)) {
		/* SET_REPORT (keyboard lights), SET_IDLE, SET_PROTOCOL: accept and ignore */
		if (rq->bRequest == 9 || rq->bRequest == 10 || rq->bRequest == 11) {
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
	case FUNCTIONFS_ENABLE:
	case FUNCTIONFS_RESUME:  on = 1; break;
	case FUNCTIONFS_DISABLE:
	case FUNCTIONFS_UNBIND:
	case FUNCTIONFS_SUSPEND: on = 0; break;
	}
	if (ev.type != FUNCTIONFS_SETUP) fprintf(stderr, "usb: event %d, connected %d\n", ev.type, on);
	return on;
}

void usb_send(const unsigned char *report, int len)
{
	/* shortcut: this write waits for the computer to take the report (1 ms when it is awake).
	 * Upgrade to non-blocking I/O if a computer that stops asking ever freezes the screen. */
	if (on && write(ep1, report, len) < 0) perror("usb: send");
}

void usb_close(void)
{
	if (!script[0]) return;
	if (ep1 >= 0) close(ep1);
	if (ep0 >= 0) close(ep0);
	ep0 = ep1 = -1; on = 0;
	sh("off");
}

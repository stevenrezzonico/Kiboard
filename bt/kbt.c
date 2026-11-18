/* kiboard-bt: BLE HID keyboard + mouse (HID over GATT) on the Kindle's own Bluetooth stack.
 * Kiboard starts it and writes reports to its stdin: one length byte, then the report as for USB
 * (report ID first). Status goes to stdout as lines: "adv", "connected", "disconnected", "fail".
 * Unlike the rest of Kiboard this is a glibc program that links the Kindle's libace_bt.so. */
#include <sys/queue.h>
#include <ace/bluetooth_api.h>
#include <ace/bluetooth_beacon_api.h>
#include <ace/bluetooth_ble_api.h>
#include <ace/bluetooth_ble_gatt_server_api.h>
#include <ace/bluetooth_common_api.h>
#include <ace/bluetooth_session_api.h>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../hid.h"

#define LOG(...) (fprintf(stderr, "bt: " __VA_ARGS__), fputc('\n', stderr))

static aceBT_sessionHandle S;
static aceBT_bleConnHandle conn;
static aceBT_bdAddr_t peer;
static aceBT_bleGattsService_t *svc;
static aceBT_advInstanceHandle adv;
static volatile aceBT_state_t radio = ACEBT_STATE_DISABLED;
static volatile int registered, ble_ready;
static int radio_was_off;
static aceBT_scanMode_t old_scan = ACEBT_SCAN_MODE_CONNECTABLE;
static uint16_t h_report[3]; /* attribute handle of the input report with ID 1 and 2 */

static void say(const char *s) { printf("%s\n", s); fflush(stdout); }

static aceBT_uuid_t U(uint16_t v)
{
	aceBT_uuid_t u;
	memset(&u, 0, sizeof u);
	u.uu[0] = (uint8_t) (v >> 8); u.uu[1] = (uint8_t) v; u.type = ACEBT_UUID_TYPE_16;
	return u;
}

/* The Kindle's stack takes characteristics and descriptors only as 128-bit UUIDs: the 16-bit number
 * inside the Bluetooth base UUID 0000xxxx-0000-1000-8000-00805F9B34FB. It sends the 16 bytes as they are,
 * and on the air a UUID goes last byte first, so they are stored last byte first (seen in a packet log). */
static aceBT_uuid_t UL(uint16_t v)
{
	static const uint8_t base[16] = { 0xFB, 0x34, 0x9B, 0x5F, 0x80, 0, 0, 0x80, 0, 0x10, 0, 0, 0, 0, 0, 0 };
	aceBT_uuid_t u;
	memcpy(u.uu, base, 16);
	u.uu[12] = (uint8_t) v; u.uu[13] = (uint8_t) (v >> 8); u.type = ACEBT_UUID_TYPE_128;
	return u;
}
static unsigned u16(const aceBT_uuid_t *u) { return u->type == ACEBT_UUID_TYPE_16 ? u->uu[0] << 8 | u->uu[1] : u->uu[13] << 8 | u->uu[12]; }

/* The stack never tells us the attribute handles, and requests for the input reports and for their
 * descriptors all arrive as "characteristic 2A4D, handle N". Handles follow the order of
 * build_service(): service +0, then 2 per characteristic and 1 per descriptor. The service handle
 * is read from the system log, where the stack prints it; 40 was measured on firmware 5.19.6. */
static unsigned base;
static int logfd = -1; /* /var/log/messages, opened before root is given up */

static void set_base(unsigned b)
{
	base = b;
	h_report[1] = (uint16_t) (b + 8); h_report[2] = (uint16_t) (b + 12);
	LOG("service handle %u: keyboard report %u, mouse report %u", b, h_report[1], h_report[2]);
}

static void find_base(void)
{
	static char buf[16385];
	const char *key = "Add Service Success Added ", *p = buf, *last = NULL;
	off_t end = lseek(logfd, 0, SEEK_END);
	ssize_t n = end < 0 ? -1 : pread(logfd, buf, sizeof buf - 1, end > 16384 ? end - 16384 : 0);
	if (n > 0) {
		buf[n] = 0;
		while ((p = strstr(p, key))) { /* the last line about the HID service */
			const char *eol = strchr(p, '\n'), *u = strstr(p, "uuid 1812");
			if (u && (!eol || u < eol)) last = p;
			p++;
		}
	}
	set_base(last && atoi(last + strlen(key)) > 0 ? (unsigned) atoi(last + strlen(key)) : 40);
}

/* A request for a characteristic with a unique UUID proves the service handle: correct a wrong one. */
static void learn(unsigned uuid, unsigned h)
{
	unsigned b = uuid == 0x2A4A ? h - 2 : uuid == 0x2A4B ? h - 4 : uuid == 0x2A4C ? h - 6 : 0;
	if (h && b && b != base) set_base(b);
}

/* ---- values of the HID service ---- */
static int value(unsigned uuid, unsigned handle, const unsigned char **data)
{
	static const unsigned char info[] = { 0x11, 0x01, 0x00, 0x02 }, zero[8] = { 0 };
	static const unsigned char ref1[] = { 1, 1 }, ref2[] = { 2, 1 }, ref3[] = { 1, 2 };
	static const unsigned char pnp[] = { 0x02, 0x6b, 0x1d, 0x05, 0x01, 0x00, 0x01 }, full[] = { 100 };
	learn(uuid, handle);
	*data = zero;
	switch (uuid) {
	case 0x2A4A: *data = info; return sizeof info;                    /* HID information */
	case 0x2A4B: *data = hid_report_desc; return hid_report_desc_len; /* report map */
	case 0x2A50: *data = pnp; return sizeof pnp;                      /* PnP ID: USB 1d6b:0105, version 1.00 */
	case 0x2A19: *data = full; return 1;                              /* battery level: not measured, say 100 % */
	case 0x2A4D:
		switch (handle - base) {
		case 10: *data = ref1; return 2;  /* report reference: ID 1, input */
		case 14: *data = ref2; return 2;  /* report reference: ID 2, input */
		case 9: case 13: return 2;        /* client configuration */
		case 12: return HID_MOUSE_LEN - 1;
		case 16: return 1;                /* keyboard lights (output report) */
		case 17: *data = ref3; return 2;  /* report reference: ID 1, output */
		default: return HID_KBD_LEN - 1;
		}
	default: return 2;
	}
}

static void respond(aceBT_bleConnHandle c, int req, unsigned uuid, unsigned handle, unsigned off)
{
	const unsigned char *d;
	int n = value(uuid, handle, &d);
	aceBT_bleGattResp_t r;
	memset(&r, 0, sizeof r);
	if (off > (unsigned) n) off = n;
	r.status = ACEBT_STATUS_SUCCESS; r.requestId = req;
	r.blobValue.data = (uint8_t *) d + off; r.blobValue.size = (uint16_t) (n - off); r.blobValue.offset = (uint16_t) off;
	LOG("read uuid %04x handle %u offset %u -> %d bytes, status %d", uuid, handle, off, n - off, aceBT_bleGattSendResponse(S, c, r));
}

static void ack(aceBT_bleConnHandle c, int req)
{
	aceBT_bleGattResp_t r;
	memset(&r, 0, sizeof r);
	r.status = ACEBT_STATUS_SUCCESS; r.requestId = req;
	aceBT_bleGattSendResponse(S, c, r);
}

/* ---- GATT server callbacks ---- */
static void on_registered(aceBT_bleGattInstanceHandle h, aceBT_status_t st) { (void) h; LOG("gatt service registered: %d", st); registered = st == ACEBT_STATUS_SUCCESS ? 1 : -1; }
/* The "connected" callback of the server never comes on this firmware: take the connection from any request.
 * (The call that would keep the link, aceBT_bleReconnect, answers "not supported": bt.sh patches the stack.) */
static void seen(aceBT_bleConnHandle c)
{
	if (!c || conn == c) return;
	conn = c;
	aceBT_bleGetBdaddr(c, &peer);
	say("connected");
}
static void on_char_read(aceBT_bleConnHandle c, int req, aceBT_bleGattCharacteristicsValue_t v)
{
	seen(c);
	respond(c, req, u16(&v.gattRecord.uuid), v.gattRecord.handle, v.blobValue.offset);
}
static void on_desc_read(aceBT_bleConnHandle c, int req, aceBT_bleGattCharacteristicsValue_t v)
{
	LOG("descriptor read: char %04x h %u, desc %04x h %u", u16(&v.gattRecord.uuid), v.gattRecord.handle,
	    u16(&v.gattDescriptor.gattRecord.uuid), v.gattDescriptor.gattRecord.handle);
	respond(c, req, u16(&v.gattDescriptor.gattRecord.uuid), v.gattDescriptor.gattRecord.handle, v.gattDescriptor.blobValue.offset);
}
static void on_char_write(aceBT_bleConnHandle c, int req, bool prep, aceBT_responseType_t resp, aceBT_bleGattCharacteristicsValue_t v)
{
	(void) prep;
	seen(c);
	LOG("write char %04x h %u, %u bytes", u16(&v.gattRecord.uuid), v.gattRecord.handle, v.blobValue.size);
	learn(u16(&v.gattRecord.uuid), v.gattRecord.handle);
	if (resp == ACEBT_BLE_WRITE_TYPE_RESP_REQUIRED) ack(c, req);
}
static void on_desc_write(aceBT_bleConnHandle c, int req, bool prep, aceBT_responseType_t resp, aceBT_bleGattCharacteristicsValue_t v)
{
	(void) prep;
	LOG("write desc of char %04x h %u (desc %04x h %u)", u16(&v.gattRecord.uuid), v.gattRecord.handle,
	    u16(&v.gattDescriptor.gattRecord.uuid), v.gattDescriptor.gattRecord.handle);
	if (resp == ACEBT_BLE_WRITE_TYPE_RESP_REQUIRED) ack(c, req);
}
static void on_exec_write(aceBT_bleConnHandle c, int req, bool exec) { (void) exec; ack(c, req); }
static void on_mtu(aceBT_bleConnHandle c, int mtu) { seen(c); LOG("mtu %d", mtu); }
static void on_notified(aceBT_bleConnHandle c, aceBT_status_t st) { (void) c; if (st) LOG("notify status %d", st); }
static void on_gatt_conn(aceBT_bleConnHandle c, int connected)
{
	LOG("gatt connection %p: %d", (void *) c, connected);
	conn = connected ? c : NULL;
	say(connected ? "connected" : "disconnected");
}

static aceBT_bleGattServerCallbacks_t gatts_cb = {
	.size = sizeof gatts_cb,
	.on_ble_gatts_service_registered_cb = on_registered,
	.on_ble_write_characteristics_cb = on_char_write,
	.on_ble_mtu_changed_cb = on_mtu,
	.on_ble_read_characteristics_req_cb = on_char_read,
	.on_ble_gatt_execute_write_req_cb = on_exec_write,
	.on_ble_gatt_descriptor_write_req_cb = on_desc_write,
	.on_ble_gatt_descriptor_read_req_cb = on_desc_read,
	.on_ble_notification_sent_cb = on_notified,
	.on_ble_gatt_conn_indication_cb = on_gatt_conn,
};

/* ---- adapter, BLE and pairing callbacks ---- */
static void on_adapter(aceBT_state_t st) { LOG("radio state %d", st); radio = st; }
static void on_bond(aceBT_status_t st, aceBT_bdAddr_t *a, aceBT_bondState_t b)
{
	LOG("bond state %d, status %d", b, st);
	if (b != ACEBT_BOND_STATE_BONDING) return;
	/* The stack sends the "allow pairing?" question to the Kindle's own Bluetooth service, which shows
	 * nothing while Kiboard is on the screen. It arrives just after this event: answer yes here.
	 * Then both sides compute a 6-digit number and each asks "same number?". Nobody can compare
	 * them here, so say yes: this is pairing without protection against a man in the middle.
	 * A computer the Kindle has met before gets only the second question. An answer to it that comes
	 * before the computer's own answer fails the key check, so both answers wait 1.5 s. */
	usleep(1500000);
	LOG("accept pairing: %d", aceBT_setSSPReply(a, true, ACEBT_SSP_VARIANT_CONSENT, 0));
	usleep(1500000);
	LOG("confirm number: %d", aceBT_setSSPReply(a, true, ACEBT_SSP_VARIANT_PASSKEY_CONFIRMATION, 0));
}
static void on_acl(ace_status_t st, aceBT_connState_t state, const aceBT_bdAddr_t *a, aceBt_aclData_t d)
{
	(void) st; (void) a; (void) d;
	LOG("link state %d", state);
	if (state == ACEBT_CONN_STATE_DISCONNECTED && conn) { conn = NULL; say("disconnected"); }
}
static void on_ble_registered(aceBT_status_t st) { LOG("ble registered: %d", st); ble_ready = st == ACEBT_STATUS_SUCCESS ? 1 : -1; }
static void on_ble_conn(aceBT_bleConnState_t state, aceBT_gattStatus_t gs, const aceBT_bleConnHandle c, aceBT_bdAddr_t *a)
{
	(void) a;
	LOG("ble connection %p: state %d, gatt status %d", (void *) c, state, gs);
}
static void on_ble_mtu(aceBT_status_t st, aceBT_bleConnHandle c, int mtu) { (void) st; (void) c; LOG("ble mtu %d", mtu); }

static aceBT_bleCallbacks_t ble_cb = {
	.size = sizeof ble_cb,
	.common_cbs = { .size = sizeof(aceBT_commonCallbacks_t), .adapter_state_cb = on_adapter, .bond_state_cb = on_bond,
	                .acl_state_changed_cb = on_acl },
	.ble_registered_cb = on_ble_registered,
	.connection_state_change_cb = on_ble_conn,
	.on_ble_mtu_updated_cb = on_ble_mtu,
};

/* The computer asks to pair: say yes. The Kindle has no place to show or type a number here. */
static void on_pin(aceBT_bdAddr_t *a, aceBT_bdName_t *n, int cod, bool min16) { (void) a; (void) n; (void) cod; (void) min16; LOG("PIN request: not supported"); }
static void on_ssp(aceBT_bdAddr_t *a, aceBT_bdName_t *n, int cod, aceBT_sspVariant_t variant, uint32_t passkey)
{
	(void) n; (void) cod;
	LOG("pairing request: variant %d, passkey %06u -> accept, status %d", variant, passkey, aceBT_setSSPReply(a, true, variant, passkey));
}
static aceBT_securityCallbacks_t sec_cb = { .size = sizeof sec_cb, .pin_req_cb = on_pin, .ssp_req_cb = on_ssp };

static void on_adv(aceBT_advInstanceHandle i, aceBT_beaconAdvState_t st, aceBT_beaconPowerMode_t p, aceBT_beaconAdvMode_t m)
{
	(void) i; (void) p; (void) m;
	LOG("advertising state %d", st);
	if (st == ACEBT_BEACON_ADV_STARTED) say("adv");
}
static void on_beacon_client(aceBT_status_t st) { LOG("beacon client registered: %d", st); }
static aceBT_beaconCallbacks_t beacon_cb = { .size = sizeof beacon_cb, .advStateChanged = on_adv, .onclientRegistered = on_beacon_client };

/* ---- setup ---- */
static int wait_for(volatile int *flag, int tenths)
{
	while (!*flag && tenths-- > 0) usleep(100000);
	return *flag;
}

#define OK(what, call) do { aceBT_status_t st_ = (call); LOG("%s: %d", what, st_); if (st_ != ACEBT_STATUS_SUCCESS) return -1; } while (0)

static int build_service(void)
{
	const aceBT_bleGattAttributeProperty R = ACEBT_BLE_GATT_PROP_READ, N = ACEBT_BLE_GATT_PROP_NOTIFY, WN = ACEBT_BLE_GATT_PROP_WRITE_NO_RESPONSE;
	const aceBT_bleGattAttributePermission PR = ACEBT_BLE_GATT_PERM_READ, PW = ACEBT_BLE_GATT_PERM_WRITE;
	OK("create HID service", aceBT_bleCreateGattService(&svc, U(0x1812), ACEBT_BLE_GATT_SERVICE_TYPE_PRIMARY));
	OK("HID information", aceBT_bleAddCharacteristics(svc, R, PR, UL(0x2A4A), ACEBT_BLE_FORMAT_BLOB));
	OK("report map", aceBT_bleAddCharacteristics(svc, R, PR, UL(0x2A4B), ACEBT_BLE_FORMAT_BLOB));
	OK("control point", aceBT_bleAddCharacteristics(svc, WN, PW, UL(0x2A4C), ACEBT_BLE_FORMAT_BLOB));
	/* No protocol mode characteristic: it would promise the boot protocol, and with it three more characteristics. */
	for (int id = 1; id <= 2; id++) { /* input report: client configuration + report reference */
		aceBT_bleGattDescriptor_t d[2];
		memset(d, 0, sizeof d);
		d[0].gattRecord.uuid = UL(0x2902); d[0].gattRecord.attPerm = PR | PW;
		d[1].gattRecord.uuid = UL(0x2908); d[1].gattRecord.attPerm = PR;
		OK("input report", aceBT_bleAddCharacteristicsWithDescArray(svc, R | N, PR, UL(0x2A4D), ACEBT_BLE_FORMAT_BLOB, d, 2));
	}
	/* The report map has an output report (keyboard lights): Windows wants a characteristic for it. */
	OK("output report", aceBT_bleAddCharacteristicsWithDesc(svc, R | WN | ACEBT_BLE_GATT_PROP_WRITE, PR | PW, UL(0x2A4D),
	                                                        ACEBT_BLE_FORMAT_BLOB, PR, UL(0x2908)));
	return 0;
}


static int advertise(void)
{
	static aceBT_BeaconData_t data;
	aceBT_BeaconAdvSettings set;
	aceBT_BeaconPayloadList_t payload = { 1, &data };
	aceBT_uuid_t hid = U(0x1218); /* 0x1812: the advertisement wants the two bytes the other way round */
	memset(&set, 0, sizeof set);
	set.connectable = true; set.priority = ACEBT_BEACON_PRIORITY_HIGH; set.mode = ACEBT_BEACON_MODE_LOW_LATENCY;
	set.addrType = ACEBT_ADDRESS_TYPE_PUBLIC; set.advType = ACEBT_BLE_ADV_CONN_IND;
	aceBT_initBeaconData(&data);
	aceBT_appendServiceUUIDs(&data, &hid, 1);
	data.beaconType = ACEBT_BEACON_TYPE_PASSTHROUGH;
	data.includeDeviceName = true;
	OK("start advertising", aceBT_startBeacon(S, set, ACE_BEACON_CLIENT_TYPE_UNKNOWN, payload, &adv));
	return 0;
}

static int start(void)
{
	aceBT_status_t open = aceBT_openSession(ACEBT_SESSION_TYPE_DUAL_MODE, NULL, &S);
	for (int i = 1; i < 45 && open != ACEBT_STATUS_SUCCESS; i++) { sleep(1); open = aceBT_openSession(ACEBT_SESSION_TYPE_DUAL_MODE, NULL, &S); LOG("open session, try %d: %d", i + 1, open); }
	OK("open session", open);
	OK("ble register", aceBT_bleRegister(S, &ble_cb));
	wait_for(&ble_ready, 50);
	aceBT_state_t st = ACEBT_STATE_DISABLED;
	aceBT_getRadioState(&st);
	radio = st;
	LOG("radio state at start: %d", st);
	if (st != ACEBT_STATE_ENABLED) {
		radio_was_off = 1;
		/* A btmanagerd that bt.sh has just restarted can refuse the radio (-25 was seen once; two later
		 * starts took it at the first try): ask again. */
		aceBT_status_t on = aceBT_enableRadio(S);
		for (int i = 1; i < 45 && on != ACEBT_STATUS_SUCCESS; i++) { sleep(1); on = aceBT_enableRadio(S); LOG("enable radio, try %d: %d", i + 1, on); }
		OK("enable radio", on);
		for (int i = 0; i < 100 && radio != ACEBT_STATE_ENABLED; i++) usleep(100000);
		if (radio != ACEBT_STATE_ENABLED) { LOG("radio did not come on"); return -1; }
	}
	/* Without this the advertisement carries no "discoverable" flag and a computer will not offer to pair. */
	aceBT_getScanMode(S, &old_scan);
	LOG("discoverable (was scan mode %d): %d", old_scan, aceBT_setScanMode(S, ACEBT_SCAN_MODE_CONNECTABLE_DISCOVERABLE, 0));
	sleep(2);
	LOG("security client: %d", aceBT_registerAsSecurityClient(S, &sec_cb));
	if (build_service()) return -1;
	OK("register gatt server", aceBT_bleRegisterGattServer(S, &gatts_cb, svc));
	if (wait_for(&registered, 100) != 1) { LOG("service was not registered"); return -1; }
	find_base();

	/* The HID over GATT profile also wants these two services; Windows does not start the keyboard without them. */
	static const uint16_t extra[2][2] = { { 0x180A, 0x2A50 }, { 0x180F, 0x2A19 } }; /* device information, battery */
	for (int i = 0; i < 2; i++) {
		aceBT_bleGattsService_t *s = NULL;
		registered = 0;
		OK("create service", aceBT_bleCreateGattService(&s, U(extra[i][0]), ACEBT_BLE_GATT_SERVICE_TYPE_PRIMARY));
		OK("its characteristic", aceBT_bleAddCharacteristics(s, ACEBT_BLE_GATT_PROP_READ, ACEBT_BLE_GATT_PERM_READ, UL(extra[i][1]), ACEBT_BLE_FORMAT_BLOB));
		OK("add service", aceBT_bleAddGattService(S, s));
		if (wait_for(&registered, 100) != 1) { LOG("service %04x was not registered", extra[i][0]); return -1; }
	}

	LOG("beacon client: %d", aceBT_RegisterBeaconClient(S, &beacon_cb));
	return advertise();
}

static int notify(int id, const unsigned char *data, int len)
{
	aceBT_bleGattCharacteristicsValue_t v;
	memset(&v, 0, sizeof v);
	v.format = ACEBT_BLE_FORMAT_BLOB;
	v.gattRecord.handle = h_report[id];
	v.blobValue.data = (uint8_t *) data; v.blobValue.size = (uint16_t) len;
	return aceBT_bleGattNotifyCharacteristicsChanged(S, conn, v, false);
}

/* The stack tells this program nothing about the link: no connect or disconnect callback arrives, and
 * its "is it connected" calls say no for a connected computer (measured). The only place where it shows
 * is the system log, where the stack writes one line per connect and disconnect. So read new log lines
 * once per second. The stack also stops advertising when a computer connects: when the computer is
 * gone, advertise again. */
#define SYSLOG "/var/log/messages"
static off_t logpos;

static int log_link(void) /* 1 = a computer connected, 0 = it went away, -1 = no news */
{
	static char buf[8193];
	const char *key = "aceBtHalConnectionCallback()]: conn_id ";
	struct stat here, path;
	int r = -1;
	if (logfd < 0 || fstat(logfd, &here)) return -1;
	if (stat(SYSLOG, &path) == 0 && (path.st_ino != here.st_ino || path.st_size < logpos)) { /* the log was rotated */
		close(logfd);
		logfd = open(SYSLOG, O_RDONLY);
		logpos = 0;
	}
	for (ssize_t n; (n = pread(logfd, buf, sizeof buf - 1, logpos)) > 0;) {
		buf[n] = 0;
		char *end = strrchr(buf, '\n'); /* whole lines only */
		if (!end) { if (n < (ssize_t) sizeof buf - 1) break; logpos += n; continue; }
		*end = 0;
		logpos += end - buf + 1;
		for (char *p = buf; (p = strstr(p, key)); p++) {
			char *s = strstr(p, " status ");
			if (s) r = atoi(s + 8) != 0;
		}
	}
	return r;
}

static void check_link(void)
{
	int r = log_link();
	if (r < 0) return;
	LOG("link: %s", r ? "connected" : "gone");
	if (r) return; /* "connected" is said when the computer's first request brings the connection handle */
	if (conn) say("disconnected");
	conn = NULL;
	if (adv) aceBT_stopBeacon(S, adv);
	adv = NULL;
	usleep(300000); /* the controller cannot take stop and start back to back */
	advertise();
}

static void on_signal(int s) { (void) s; close(0); }
int kbt_patch(const char *src, const char *dst); /* patch.c */

int main(int argc, char **argv)
{
	/* the stack refuses root: run as the Kindle's "bluetooth" user (1003) */
	if (argc == 4 && !strcmp(argv[1], "patch")) return kbt_patch(argv[2], argv[3]); /* from bt.sh, as root */
	logfd = open("/var/log/messages", O_RDONLY);
	logpos = lseek(logfd, 0, SEEK_END);
	if (setgid(1003) || setuid(1003)) { perror("bt: drop to bluetooth user"); return 1; }
	if (argc > 1 && !strcmp(argv[1], "off")) { /* from bt.sh: Bluetooth was off before Kiboard, turn it off again */
		if (aceBT_openSession(ACEBT_SESSION_TYPE_DUAL_MODE, NULL, &S) != ACEBT_STATUS_SUCCESS) return 1;
		LOG("radio off, as it was: %d", aceBT_disableRadio(S));
		sleep(2);
		aceBT_closeSession(S);
		return 0;
	}
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGPIPE, SIG_IGN);

	int rc = start();
	if (rc) say("fail");
	while (!rc) {
		unsigned char len, rep[16];
		struct pollfd p = { 0, POLLIN, 0 };
		int n = poll(&p, 1, 1000);
		if (n < 0) break;
		if (n == 0) { check_link(); continue; }
		if (read(0, &len, 1) != 1 || len < 2 || len > sizeof rep || read(0, rep, len) != len) break;
		if (!conn || rep[0] < 1 || rep[0] > 2) continue;
		aceBT_status_t st = notify(rep[0], rep + 1, len - 1); /* the report ID is not sent over GATT */
		if (st) LOG("notify: %d", st);
	}
	if (S) {
		if (adv) LOG("stop advertising: %d", aceBT_stopBeacon(S, adv));
		if (conn) {
			/* Let go of the computer BEFORE the services go away. Otherwise the stack tells it "services
			 * changed", the computer looks again, finds no keyboard, deletes its keyboard and mouse
			 * devices and never comes back (seen with Windows). */
			LOG("disconnect: %d", aceBT_bleServerDisconnect(S, conn, &peer));
			for (int i = 0; i < 40 && log_link() != 0; i++) usleep(100000);
		}
		aceBT_setScanMode(S, old_scan, 0);
		LOG("deregister gatt server: %d", aceBT_bleDeRegisterGattServer(S));
		aceBT_bleDeRegister(S);
		if (radio_was_off || getenv("KBT_RADIO_OFF")) LOG("radio off, as it was: %d", aceBT_disableRadio(S));
		LOG("close session: %d", aceBT_closeSession(S));
	}
	return rc ? 1 : 0;
}

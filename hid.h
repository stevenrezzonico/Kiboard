/* HID reports: one interface, report 1 = keyboard, report 2 = mouse with wheel. No device code. */
#ifndef KB_HID_H
#define KB_HID_H

#define HID_KBD_LEN 9
#define HID_MOUSE_LEN 5

enum { MOD_CTRL = 0x01, MOD_SHIFT = 0x02, MOD_ALT = 0x04, MOD_GUI = 0x08 };
enum { BTN_L = 1, BTN_R = 2 };

#define HID_KBD_DESC_LEN 65 /* the keyboard's part of hid_report_desc; the mouse's part is the rest */
extern const unsigned char hid_report_desc[];
extern const int hid_report_desc_len;

void hid_kbd(unsigned char *out, unsigned mods, const unsigned char keys[6]); /* writes HID_KBD_LEN bytes */
void hid_mouse(unsigned char *out, unsigned buttons, int dx, int dy, int wheel); /* -127..127 each */

#endif

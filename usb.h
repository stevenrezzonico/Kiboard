#ifndef KB_USB_H
#define KB_USB_H

int  usb_open(const char *dir);   /* switches the Kindle's USB port to HID; returns a file to poll, or -1 */
int  usb_event(void);             /* call when that file is readable; returns 1 while a computer listens */
void usb_send(const unsigned char *report, int len);
void usb_close(void);             /* gives the USB port back to the Kindle */

#endif

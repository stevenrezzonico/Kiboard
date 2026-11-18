#ifndef KB_BT_H
#define KB_BT_H

enum { BT_STARTING, BT_WAITING, BT_CONNECTED, BT_FAILED };

int  bt_open(const char *dir);   /* starts Bluetooth mode; returns a file to poll, or -1 */
int  bt_event(void);             /* call when that file is readable, also at its end; returns a BT_ state */
void bt_send(const unsigned char *report, int len);
void bt_close(void);             /* stops the helper and gives the Kindle its own Bluetooth back */

#endif

#!/bin/sh
# Makes and removes the USB gadget for Kiboard. usb.c calls: on, bind, off. The launcher calls off at exit.
# Same recipe as the KindleSSH USB network (sshctl.sh usb_on), with a FunctionFS function in place of RNDIS.
export PATH=/usr/sbin:/usr/bin:/sbin:/bin:$PATH
G=/sys/kernel/config/usb_gadget/kiboard
F=/dev/usb-ffs/kiboard

off() {
	[ -d $G ] || return 0
	echo "" >$G/UDC
	umount $F; rmdir $F
	rm -f $G/configs/c.1/ffs.kiboard
	rmdir $G/configs/c.1/strings/0x409 $G/configs/c.1 $G/functions/ffs.kiboard $G/strings/0x409 $G
	lipc-set-prop com.lab126.volumd useUsbForNetwork 0   # the USB port is the Kindle's drive again
} 2>/dev/null

case "$1" in
on)
	off
	# The firmware's own switch keeps the Kindle out of drive (MTP) mode. It loads g_ether, which we replace.
	lipc-set-prop com.lab126.volumd useUsbForNetwork 1
	i=0; while [ $i -lt 8 ] && ! ifconfig usb0 2>/dev/null | grep -q 'inet addr'; do sleep 1; i=$((i + 1)); done
	rmmod g_ether 2>/dev/null
	modprobe usb_f_fs
	mkdir $G && cd $G || exit 1
	echo 0x1d6b >idVendor; echo 0x0105 >idProduct; echo 0x0200 >bcdUSB
	mkdir strings/0x409
	echo Kiboard >strings/0x409/manufacturer; echo "Kiboard keyboard and trackpad" >strings/0x409/product
	echo KIBOARD0001 >strings/0x409/serialnumber
	mkdir functions/ffs.kiboard || exit 1
	mkdir -p configs/c.1/strings/0x409; echo HID >configs/c.1/strings/0x409/configuration
	ln -s $G/functions/ffs.kiboard configs/c.1/
	mkdir -p $F && mount -t functionfs kiboard $F
	;;
bind)	ls /sys/class/udc >$G/UDC;;
off)	off;;
*)	echo "usage: usb.sh on|bind|off" >&2; exit 2;;
esac

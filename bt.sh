#!/bin/sh
# Bluetooth mode for Kiboard. bt.c calls "run"; "off" undoes everything (also called by the launcher at exit).
#
# The Kindle's Bluetooth stack (libbluetooth.so, loaded by btmanagerd) cannot work as a keyboard as it is:
#   1. it removes the "discoverable" flag from every advertisement, so no computer offers to pair;
#   2. it advertises from a random address, and then fails its own key check during pairing;
#   3. it drops a computer 1 second after pairing, because no Kindle program "asked" for that link.
# Each is one instruction. "on" makes a copy of the library in RAM with those three instructions changed,
# mounts the copy over the original and restarts btmanagerd. Nothing on the system partition is written:
# "off" or a restart of the Kindle brings the original back. While it is on, the Kindle's own Bluetooth
# (audio) runs on the changed copy.
export PATH=/usr/sbin:/usr/bin:/sbin:/bin:$PATH
L=/usr/lib/libbluetooth.so
T=/var/tmp/kiboard-libbluetooth.so
W=/var/tmp/kiboard-bt-was   # the Bluetooth switch before Kiboard: 0 = off

mounted() { grep -q " $L " /proc/mounts; }

off() {
	mounted || { killall kiboard-bt 2>/dev/null; return 0; }
	# Order matters. The stack must die BEFORE the helper: a helper that leaves first takes its services
	# away over a live link, the computer is told "services changed", finds no keyboard, deletes its
	# keyboard and mouse devices and never reconnects (seen with Windows).
	stop btmanagerd
	killall -9 kiboard-bt 2>/dev/null
	umount $L
	rm -f $T
	start btmanagerd
	# Bluetooth was off before Kiboard: turn it off again, once btmanagerd takes clients.
	if [ "$(cat $W 2>/dev/null)" = 0 ]; then
		( sleep 30; "$(dirname "$0")/kiboard-bt" off ) </dev/null >/dev/null 2>&1 &
	fi
	rm -f $W
}

on() {
	mounted && return 0
	# kiboard-bt finds the three instructions by function name and code (bt/patch.c). Tested with the library
	# of firmware 5.19.6. A library where it does not find them is left alone.
	"$(dirname "$0")/kiboard-bt" patch $L $T || { rm -f $T; return 1; }
	chmod 755 $T                                    # btmanagerd runs as user "bluetooth": it must be able to read it
	lipc-get-prop com.lab126.btfd BTstate >$W 2>/dev/null
	stop btmanagerd
	mount -o bind $T $L || { rm -f $T; start btmanagerd; return 1; }
	start btmanagerd                                # it refuses the radio for a while: kiboard-bt asks until it agrees
}

case "$1" in
run)	on || { echo fail; exit 1; }; exec "$(dirname "$0")/kiboard-bt";;
on)	on;;
off)	off;;
*)	echo "usage: bt.sh run|on|off" >&2; exit 2;;
esac

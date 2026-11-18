#!/bin/bash
# Builds the Bluetooth helper out/kiboard-bt. Run in WSL:  bash bt/build.sh  (then bash build.sh puts it in dist/)
# Unlike Kiboard itself, it is a glibc program that links the Kindle's own libace_bt.so. Needs:
#   ~/kiboard-ref/acs/inc     the "ace/sdk/include" folder of acs_rtos_reference_ffs_1.15.0.tar.gz (ACE headers)
#   ~/kiboard-ref/lib/libace_bt.so   copied from the Kindle:  kdev pull /usr/lib/libace_bt.so ~/kiboard-ref/lib/libace_bt.so
set -euo pipefail
P=$(cd "$(dirname "$0")/.." && pwd)
R=${KIBOARD_REF:-$HOME/kiboard-ref}
CC="zig cc -target arm-linux-gnueabihf.2.35 -mcpu=cortex_a7 -Os -Wall -Wextra -Wno-deprecated-declarations -I $R/acs/inc"
mkdir -p $P/out
# PC check of patch.c, when a copy of the library of firmware 5.19.6 is there (kdev pull /usr/lib/libbluetooth.so ...):
# it must change the three known places, 4 bytes each, and nothing else.
if [ -f $R/lib/libbluetooth.so ]; then
	gcc -O2 -Wall -Wextra -DPATCH_MAIN -o $P/out/patch $P/bt/patch.c
	$P/out/patch $R/lib/libbluetooth.so $P/out/libbluetooth.patched
	got=$(cmp -l $R/lib/libbluetooth.so $P/out/libbluetooth.patched | awk '{ printf "%x ", $1 - 1 }' || true)
	[ "$got" = "12697f 127cd4 127cd5 127cd6 127cd7 1556f8 1556f9 1556fa " ] || { echo "patch check FAILED: changed bytes at: $got" >&2; exit 1; }
	echo "patch check passed"
fi
$CC -o $P/out/kiboard-bt $P/bt/kbt.c $P/bt/patch.c $P/hid.c $R/lib/libace_bt.so -Wl,--allow-shlib-undefined

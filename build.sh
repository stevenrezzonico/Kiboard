#!/bin/bash
# Builds Kiboard into dist/ (laid out like the Kindle drive). Run in WSL:  bash build.sh
# Needs the KindleSSH build first (bash build.sh in the KindleSSH repository): it leaves the compiler
# wrappers, FBInk and the fonts in ~/kssh-build.
set -euo pipefail
APP=Kiboard   # folder on the Kindle (/mnt/us/Kiboard) and the name in the library
BIN=kiboard   # program name
P=$(cd "$(dirname "$0")" && pwd)
B=${KSSH_BUILD:-$HOME/kssh-build}
export PATH=$B/bin:$PATH
CORE="$P/ui.c $P/gesture.c $P/hid.c"
mkdir -p $P/out $P/shots

# PC: tests, then screenshots in shots/ (look at them before the Kindle sees the app) and the icon.
gcc -O2 -Wall -Wextra -I$P -isystem $B/FBInk/stb -o $P/out/preview $P/preview.c $CORE -lm
$P/out/preview $B/out/font $P/shots

# Kindle: static ARM program, needs no library from the Kindle.
arm-cc -Os -Wall -Wextra -DAPP_DIR="\"/mnt/us/$APP\"" -I$P -I$B/FBInk -isystem $B/FBInk/stb -o $P/out/$BIN \
	$P/main.c $P/usb.c $P/bt.c $CORE $B/FBInk/Release/libfbink.a $B/FBInk/libi2c-staged/lib/libi2c.a -lm -static -s

rm -rf $P/dist
mkdir -p $P/dist/documents $P/dist/$APP/bin $P/dist/$APP/font
sed "s/@APP@/$APP/g; s/@BIN@/$BIN/g" $P/launcher.sh >$P/dist/documents/$APP.sh
cp $P/out/$BIN $P/usb.sh $P/bt.sh $P/dist/$APP/bin/
[ -f $P/out/kiboard-bt ] && cp $P/out/kiboard-bt $P/dist/$APP/bin/   # built by bt/build.sh; without it there is no Bluetooth mode
cp $B/out/font/Inter-Bold.otf $P/dist/$APP/font/
cp $P/shots/icon.png $P/dist/$APP/
du -sh $P/dist

#!/bin/sh
# Push the engine, the probe client and the device-side script to the card, then
# run the script on the device: it starts the engine, asks it `probe` over the
# unix socket and stops it again.
#
#   tools/probe.sh                 # after `make`
#   SOCKET=/tmp/other.sock tools/probe.sh
#
# The socket lives in /tmp because /tmp is tmpfs and the card is vfat: a unix
# socket cannot be created on vfat at all. The binaries can be run from the card
# (vfat mounts with fmask=0022, so every file is 0755 and noexec is not set).
#
# The device-side half is a file and not a heredoc on purpose: `adb shell` here
# always allocates a pty (this adb refuses -T), and a pty echoes every line of a
# script fed to its stdin, which turns the output into noise.
set -eu

cd "$(dirname "$0")/.."

BIN=target/mipsel-unknown-linux-musl/release/spotify-engine-mips
CLIENT=target/mipsel-linux-musl/probe-client
SOCKET=${SOCKET:-/tmp/spotify-engine.sock}
DEVICE=/mnt/sd_0/tmp

for file in "$BIN" "$CLIENT" tools/probe-device.sh; do
	[ -f "$file" ] || { echo "probe.sh: $file is missing; run make first" >&2; exit 1; }
done

adb push "$BIN" "$DEVICE/spotify-engine-mips" >/dev/null
adb push "$CLIENT" "$DEVICE/probe-client" >/dev/null
adb push tools/probe-device.sh "$DEVICE/probe-device.sh" >/dev/null

adb shell "chmod +x $DEVICE/spotify-engine-mips $DEVICE/probe-client 2>/dev/null; sh $DEVICE/probe-device.sh '$SOCKET'"

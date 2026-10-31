#!/bin/sh
# Runs on the device, not on the host: `tools/probe.sh` pushes this file next to
# the two binaries and calls it.
#
#   sh /mnt/sd_0/tmp/probe-device.sh [socket-path]
#
# It starts the engine in the background, waits for the socket to appear, asks
# it `probe` and one unknown op, prints the log, and stops the engine again.
# The busybox here has no `nc` applet and no socat, which is why the requests go
# through probe-client; it has pgrep and kill but no pkill applet, and its
# setsid eats the program's own --options, which is why the engine is started as
# a plain background job with its pid remembered.
set -u

DEVICE=/mnt/sd_0/tmp
SOCKET=${1:-/tmp/spotify-engine.sock}
LOG=/tmp/spotify-engine.log
PIDFILE=/tmp/spotify-engine.pid

# Kill an engine left behind by a previous run, and only that: `pgrep -f` on the
# binary's path is not safe here, because the shell that started this script has
# the whole `chmod ... /mnt/sd_0/tmp/spotify-engine-mips ...; sh ...` command
# line in its argv and would match itself -- the script then dies with no output
# at all. The pattern is the engine's own arguments instead, and $$/$PPID are
# skipped for good measure.
if [ -f "$PIDFILE" ]; then
	kill "$(cat "$PIDFILE")" 2>/dev/null
	rm -f "$PIDFILE"
fi
for pid in $(pgrep -f "spotify-engine-mips --socket $SOCKET"); do
	case "$pid" in
	"$$" | "$PPID") continue ;;
	esac
	kill "$pid" 2>/dev/null
done
sleep 1
rm -f "$SOCKET"

"$DEVICE/spotify-engine-mips" --socket "$SOCKET" --credentials /data/spotify/credentials \
	</dev/null >"$LOG" 2>&1 &
engine=$!
echo "$engine" >"$PIDFILE"
echo "engine pid=$engine"

waited=0
while [ ! -S "$SOCKET" ]; do
	sleep 1
	waited=$((waited + 1))
	if [ "$waited" -gt 5 ]; then
		echo "probe-device.sh: $SOCKET did not appear" >&2
		cat "$LOG" >&2
		kill "$engine" 2>/dev/null
		exit 1
	fi
done
echo "socket $SOCKET is up after about ${waited}s"

echo "--- probe"
"$DEVICE/probe-client" "$SOCKET" '{"op":"probe"}'
echo "--- unknown op"
"$DEVICE/probe-client" "$SOCKET" '{"op":"nonsense"}'
echo "--- self-test (librespot runs, getrandom works)"
"$DEVICE/spotify-engine-mips" --self-test
echo "self-test exit=$?"
echo "--- log"
cat "$LOG"

kill "$engine" 2>/dev/null
sleep 1
if kill -0 "$engine" 2>/dev/null; then
	echo "probe-device.sh: engine $engine ignored SIGTERM" >&2
	kill -9 "$engine" 2>/dev/null
	exit 1
fi
echo "engine stopped"
if [ -S "$SOCKET" ]; then
	echo "probe-device.sh: $SOCKET was left behind" >&2
	exit 1
fi
exit 0

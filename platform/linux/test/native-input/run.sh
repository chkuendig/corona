#!/usr/bin/env bash
# Drive main.lua in a headless simulator image through SOLAR2D_INPUT_PIPE and
# check native text field input end to end:
#
#   - the first tap on a text field focuses it ("began")
#   - .text reads back what was typed, in the listener and afterwards
#   - a listener that rewrites .text keeps the rewrite
#   - Enter fires "submitted"
#   - the first tap after a field reaches the display object under it
#   - text longer than one SDL event arrives whole, split at characters
#   - invalid UTF-8 is rejected
#   - a mouse listener sees the injected button pressed and dragging
#
# Usage: run.sh <image>     e.g. run.sh ghcr.io/chkuendig/solar2d:latest
set -euo pipefail

IMAGE="${1:?usage: run.sh <image>}"
HERE="$(cd "$(dirname "$0")" && pwd)"
NAME="native-input-$$"
FIFO=/dev/shm/input.fifo

cleanup() { docker rm -f "$NAME" >/dev/null 2>&1 || true; }
trap cleanup EXIT

docker run -d --rm --name "$NAME" \
	-e SOLAR2D_INPUT_PIPE="$FIFO" \
	-v "$HERE":/project:ro \
	"$IMAGE" simulate >/dev/null

wait_for() {
	local pattern="$1" deadline=$((SECONDS + ${2:-30}))
	until docker logs "$NAME" 2>&1 | grep -qF -- "$pattern"; do
		if (( SECONDS >= deadline )); then
			echo "timed out waiting for: $pattern" >&2
			return 1
		fi
		sleep 0.2
	done
}

send() {
	docker exec "$NAME" sh -c "printf '%s\n' \"\$1\" > $FIFO" sh "$1"
	sleep 0.5
}

wait_for "[T] ready" 60
docker exec "$NAME" sh -c "until [ -e $FIFO.ready ]; do sleep 0.1; done"

send "tap 160 80"            # first tap on the email field
for c in a @ b . c o; do send "text $c"; done
send "key return"
send "tap 160 160"           # code field
for c in 1 2 x 3 4 5 6 7; do send "text $c"; done
send "tap 160 360"           # first tap after a field
send "tap 160 240"           # long field
send "text abcdefghijklmnopqrstuvwxyz0123456789ABCD"   # 40 bytes: 2 events
send "text ääääääääääääääää€x"                         # ä at byte 30, € at 32
send "text abcdefghijklmnopqrstuvwxyzabc€"            # € at bytes 29-31
docker exec "$NAME" sh -c "printf 'text \\377\\n' > $FIFO"   # invalid UTF-8
sleep 0.5
send "drag 40 440 120 440 300"   # on the background, for the mouse listener
sleep 1

# Only what the driven input caused: creation must not fire began/ended.
LOG="$(docker logs "$NAME" 2>&1 | sed -n '/^\[T\] ready/,$p')"
fail=0
expect() {
	if grep -qF -- "$2" <<<"$LOG"; then
		echo "ok   - $1"
	else
		echo "FAIL - $1 (missing: $2)"
		fail=1
	fi
}

if docker logs "$NAME" 2>&1 | sed '/^\[T\] ready/q' | grep -qE '^\[T\] (email|code) (began|ended)'; then
	echo "FAIL - creating a field fires began/ended"
	fail=1
else
	echo "ok   - creating a field fires no focus events"
fi
expect "first tap focuses the field"       "[T] email began"
expect "editing carries the new text"      "[T] email editing text=a@b.co read=a@b.co new=o"
expect "Enter fires submitted"             "[T] email submitted read=a@b.co"
expect "a listener's .text rewrite sticks" "[T] code editing read=123456"
expect "first tap after a field lands"     "[T] button ended"
expect ".text reads back afterwards"       "[T] final email=a@b.co code=123456"
expect "40 ASCII bytes arrive whole"        "[T] long editing new=abcdefghijklmnopqrstuvwxyz0123456789ABCD"
expect "...in two events"                   "[INPUT] dispatched text (40 codepoints, 40 bytes, 2 events)"
expect "2- and 3-byte chars survive a split" "[T] long editing new=ääääääääääääääää€x"
expect "...counted as codepoints"           "[INPUT] dispatched text (18 codepoints, 36 bytes, 2 events)"
expect "a char across the 31-byte cut survives" "[T] long editing new=abcdefghijklmnopqrstuvwxyzabc€"
expect "...and counts as one codepoint"     "[INPUT] dispatched text (30 codepoints, 32 bytes, 2 events)"
expect "invalid UTF-8 is rejected"          "[INPUT] ignored: text (invalid UTF-8 at byte 0)"
expect "mouse press reports the button"     "[T] mouse down primary=true"
expect "an injected drag is a mouse drag"   "[T] mouse drag primary=true"
expect "release reports it up"              "[T] mouse up primary=false"

# Exactly one insertion per valid text command: a split must not show up
# as extra editing events, and the rejected line must not insert anything.
n="$(grep -cF '[T] long editing' <<<"$LOG" || true)"
if [[ "$n" == 3 ]]; then
	echo "ok   - one editing event per text command"
else
	echo "FAIL - one editing event per text command (saw $n, want 3)"
	fail=1
fi

if (( fail )); then
	echo "--- simulator output"
	grep -E '^\[(T|INPUT)\]' <<<"$LOG" || true
	exit 1
fi

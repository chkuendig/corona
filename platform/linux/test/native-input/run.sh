#!/usr/bin/env bash
# Drive main.lua in a headless simulator image through SOLAR2D_INPUT_PIPE and
# check native text field input end to end:
#
#   - the first tap on a text field focuses it ("began")
#   - .text reads back what was typed, in the listener and afterwards
#   - a listener that rewrites .text keeps the rewrite
#   - Enter fires "submitted"
#   - the first tap after a field reaches the display object under it
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

if (( fail )); then
	echo "--- simulator output"
	grep -E '^\[(T|INPUT)\]' <<<"$LOG" || true
	exit 1
fi

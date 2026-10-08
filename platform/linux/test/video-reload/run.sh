#!/usr/bin/env bash
# Reload a project in a headless simulator image while its video frame tap is
# streaming, and check that the stream survives the reload:
#
#   - the file watcher reloads in place: same process, the tap set up once
#   - the reader's open FIFO never sees EOF, and frame sequence numbers keep
#     rising without gaps
#   - the frame size does not change across a same-size reload
#   - the frames show the old scene, then the new one, never the old again
#
# The project is copied into the container rather than read from the bind
# mount, so the reload never depends on inotify crossing a mount.
#
# Usage: run.sh <image>     e.g. run.sh ghcr.io/chkuendig/solar2d:latest
set -euo pipefail

IMAGE="${1:?usage: run.sh <image>}"
HERE="$(cd "$(dirname "$0")" && pwd)"
NAME="video-reload-$$"
FIFO=/dev/shm/video.fifo
FRAMES=/tmp/frames.log
MIN_FRAMES=60
GRACE=3   # frames allowed to show neither scene while the reload swaps them

cleanup() { docker rm -f "$NAME" >/dev/null 2>&1 || true; }
trap cleanup EXIT

docker run -d --rm --name "$NAME" \
	-e SOLAR2D_VIDEO_PIPE="$FIFO" \
	-e SOLAR2D_VIDEO_FPS=30 \
	-v "$HERE":/src:ro \
	--entrypoint sh \
	"$IMAGE" -c 'cp -r /src /tmp/project && exec /usr/local/bin/entrypoint.sh simulate /tmp/project/main.lua' >/dev/null

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

frames() { docker exec "$NAME" cat "$FRAMES" 2>/dev/null || true; }

wait_frames() {
	local colour="$1" deadline=$((SECONDS + ${2:-60}))
	until (( $(frames | grep -c " $colour\$" || true) >= MIN_FRAMES )); do
		if (( SECONDS >= deadline )); then
			echo "timed out waiting for $MIN_FRAMES $colour frames" >&2
			return 1
		fi
		sleep 0.5
	done
}

wait_for "[T] loaded red" 60
docker exec "$NAME" sh -c "until [ -e $FIFO.ready ]; do sleep 0.1; done"
PID_BEFORE="$(docker exec "$NAME" cat "$FIFO.ready")"

docker exec -d "$NAME" python3 /tmp/project/reader.py "$FIFO" "$FRAMES"
wait_frames red

docker exec "$NAME" sh -c 'printf "return \"blue\"\n" > /tmp/project/scene.lua'
wait_for "[T] loaded blue" 60
wait_frames blue
PID_AFTER="$(docker exec "$NAME" cat "$FIFO.ready")"

LOG="$(docker logs "$NAME" 2>&1)"
FRAME_LOG="$(frames)"
fail=0
check() {
	if [[ "$2" == ok ]]; then
		echo "ok   - $1"
	else
		echo "FAIL - $1 ($2)"
		fail=1
	fi
}

n="$(grep -cF 'Loading project from:' <<<"$LOG" || true)"
check "the project loaded twice (once plus the reload)" "$([[ $n == 2 ]] && echo ok || echo "saw $n")"
n="$(grep -cF '[VIDEOTAP] streaming' <<<"$LOG" || true)"
check "the tap was set up once" "$([[ $n == 1 ]] && echo ok || echo "saw $n")"
check "the reload kept the process" "$([[ "$PID_BEFORE" == "$PID_AFTER" ]] && echo ok || echo "$PID_BEFORE -> $PID_AFTER")"
check "the stream never ended" "$(grep -qE '^(EOF|BADMAGIC)$' <<<"$FRAME_LOG" && echo "$(grep -E '^(EOF|BADMAGIC)$' <<<"$FRAME_LOG")" || echo ok)"

# Sequence numbers rise by exactly one. A gap is a dropped frame: legal for
# the tap, but here it means the run was starved, which would hide a stall.
check "sequence numbers rise without gaps" "$(awk '
	$1 ~ /^[0-9]+$/ {
		if (seen && $1 <= prev) { print "not increasing at " $1; bad = 1; exit }
		if (seen && $1 != prev + 1) gaps += $1 - prev - 1
		prev = $1; seen = 1
	}
	END { if (!bad) print (gaps ? gaps " frames dropped" : "ok") }' <<<"$FRAME_LOG")"

check "the frame size held across the reload" "$(awk '
	$1 ~ /^[0-9]+$/ { sizes[$2 "x" $3] = 1 }
	END { n = 0; s = ""; for (k in sizes) { n++; s = s " " k }; print (n == 1 ? "ok" : "sizes" s) }' <<<"$FRAME_LOG")"

# red ... red [up to GRACE others] blue ... blue, and never red after blue.
check "old scene, then new, never back" "$(awk -v grace="$GRACE" '
	$1 ~ /^[0-9]+$/ {
		if ($5 == "blue") blue = 1
		else if ($5 == "red" && blue) { print "red frame " $1 " after blue"; bad = 1; exit }
		else if ($5 == "other") other++
	}
	END { if (!bad) print (other > grace ? other " frames showed neither scene" : "ok") }' <<<"$FRAME_LOG")"

if (( fail )); then
	echo "--- simulator output"
	grep -E '^\[(T|VIDEOTAP)\]|Loading project from' <<<"$LOG" || true
	echo "--- first and last frames"
	head -5 <<<"$FRAME_LOG"
	echo "..."
	tail -5 <<<"$FRAME_LOG"
	exit 1
fi

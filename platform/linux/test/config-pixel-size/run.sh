#!/usr/bin/env bash
# Check that config.lua sees the launch window size on the Linux simulator:
# with the window pinned through the sandbox's app.conf (as capture.sh
# does), display.pixelWidth/Height at config time must match the window
# main.lua later reports.
#
# Usage: run.sh <image>
set -euo pipefail

IMAGE="${1:?usage: run.sh <image>}"
HERE="$(cd "$(dirname "$0")" && pwd)"
W=1043 H=700

# Run until main.lua reports ready (or 60s), then stop the simulator.
LOG="$(docker run --rm -v "$HERE":/project:ro --entrypoint sh "$IMAGE" -c "
	mkdir -p \"\$HOME/.Solar2D/Sandbox/project\"
	printf 'h=$H\ntitle=project\nw=$W\nx=0\ny=0\n' > \"\$HOME/.Solar2D/Sandbox/project/app.conf\"
	/usr/local/bin/entrypoint.sh simulate > /tmp/out 2>&1 &
	for i in \$(seq 300); do grep -q '^\[T\] ready' /tmp/out && break; sleep 0.2; done
	kill \$! 2>/dev/null; cat /tmp/out" 2>&1 || true)"

grep '^\[T\]' <<<"$LOG" || true
if grep -qF "[T] config pixel=${W}x${H}" <<<"$LOG" && grep -qF "[T] main pixel=${W}x${H}" <<<"$LOG"; then
	echo "ok   - config.lua sees the ${W}x${H} launch window"
else
	echo "FAIL - config.lua does not see the ${W}x${H} launch window"
	exit 1
fi

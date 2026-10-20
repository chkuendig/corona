#!/usr/bin/env bash
# Hosted macOS only. Keep the upstream Release build and source stamping.
set -euo pipefail
[[ "$(uname -s)" == Darwin ]]
[[ "$BUILD_NUMBER" == 3736 && "$YEAR" == 2026 && "$MONTH" == 10 && "$DAY" == 12 ]]
[[ "$MAKEFLAGS" == -j3 ]]
export WEBTEMPLATE_EVIDENCE="$RUNNER_TEMP/webtemplate-evidence"
export WEBTEMPLATE_SDK="$RUNNER_TEMP/sdk/emsdk"
export WEBTEMPLATE_OUTPUT="$GITHUB_WORKSPACE/output"
mkdir -p "$WEBTEMPLATE_EVIDENCE" "$WEBTEMPLATE_OUTPUT"
WEBTEMPLATE_PYTHON="$(command -v python3)"
source "$WEBTEMPLATE_SDK/emsdk_env.sh"
"$WEBTEMPLATE_PYTHON" tools/GHAction/webtemplate.py build
emar t platform/emscripten/gmake/obj/Release/libjpeg.a > "$WEBTEMPLATE_EVIDENCE/jpeg-archive.txt"
grep -Fx jmemnobs.o "$WEBTEMPLATE_EVIDENCE/jpeg-archive.txt"
if grep -Fq jmemansi.o "$WEBTEMPLATE_EVIDENCE/jpeg-archive.txt"; then
    echo "Unexpected JPEG backing-store object" >&2
    exit 1
fi
grep -E 'emcc.*libjpeg\.a|jmemnobs\.(c|o)' "$WEBTEMPLATE_EVIDENCE/compile.log" \
    > "$WEBTEMPLATE_EVIDENCE/jpeg-link.txt"
"$WEBTEMPLATE_PYTHON" tools/GHAction/webtemplate.py archive

#!/usr/bin/env bash
# Packages two apps against existing templates; never compiles a runtime.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
IMAGE= BASELINE= CANDIDATE= OUTPUT=
while (( $# )); do
    case "$1" in
        --image) IMAGE="$2"; shift 2 ;;
        --baseline-template) BASELINE="$2"; shift 2 ;;
        --candidate-template) CANDIDATE="$2"; shift 2 ;;
        --output) OUTPUT="$2"; shift 2 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done
[[ "$IMAGE" =~ @sha256:[0-9a-f]{64}$ ]] || { echo "An immutable image is required" >&2; exit 2; }
for variable in BASELINE_TEMPLATE_SHA256 CANDIDATE_TEMPLATE_SHA256 CHROMIUM_PATH CHROMIUM_SHA256; do
    [[ -n "${!variable:-}" ]] || { echo "Required variable: $variable" >&2; exit 2; }
done
[[ -n "$OUTPUT" && ! -e "$OUTPUT" ]] || { echo "Output must be a new scratch directory" >&2; exit 2; }
BASELINE="$(realpath "$BASELINE")"
CANDIDATE="$(realpath "$CANDIDATE")"
mkdir -p "$OUTPUT"
OUTPUT="$(realpath "$OUTPUT")"
python3 "$HERE/template-inputs.py" "$BASELINE" "$CANDIDATE" "$OUTPUT/inputs.json" "$IMAGE"
for arm in baseline candidate; do
    mkdir -p "$OUTPUT/$arm/project" "$OUTPUT/$arm/output"
    cp "$HERE/main.lua" "$HERE/config.lua" "$OUTPUT/$arm/project/"
    template="$BASELINE"
    [[ "$arm" == baseline ]] || template="$CANDIDATE"
    docker run --rm --user "$(id -u):$(id -g)" --network none \
        -e HOME=/tmp \
        -v "$OUTPUT/$arm/project:/project" \
        -v "$OUTPUT/$arm/output:/output" \
        -v "$template:/template.zip:ro" \
        "$IMAGE" build --app-name TextFieldRegression --webtemplate /template.zip \
        >"$OUTPUT/$arm/package.log" 2>&1
done
python3 "$HERE/template-inputs.py" --packaged "$OUTPUT"
node "$HERE/browser-driver.cjs" "$OUTPUT"

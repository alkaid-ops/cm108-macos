#!/bin/zsh
set -euo pipefail
cd "$(dirname "$0")"

OUT="build/RH899X.driver"
mkdir -p "$OUT/Contents/MacOS" "$OUT/Contents/Resources"
cp RH899X.plist "$OUT/Contents/Info.plist"

TUNE_FLAGS=()
[[ -n "${RH899X_BYTES_PER_FRAME:-}" ]] && TUNE_FLAGS+=("-DRH899X_BYTES_PER_FRAME=${RH899X_BYTES_PER_FRAME}u")
[[ -n "${RH899X_BUFFER_FRAMES:-}" ]] && TUNE_FLAGS+=("-DRH899X_BUFFER_FRAMES=${RH899X_BUFFER_FRAMES}u")
[[ -n "${RH899X_LATENCY_FRAMES:-}" ]] && TUNE_FLAGS+=("-DRH899X_LATENCY_FRAMES=${RH899X_LATENCY_FRAMES}u")
[[ -n "${RH899X_SAFETY_FRAMES:-}" ]] && TUNE_FLAGS+=("-DRH899X_SAFETY_FRAMES=${RH899X_SAFETY_FRAMES}u")
if (( ${#TUNE_FLAGS} )); then
  echo "tuning: ${TUNE_FLAGS[*]}"
fi

clang -std=c11 -bundle -fPIC -O2 -Wall \
  -Wno-deprecated-declarations -pthread \
  "${TUNE_FLAGS[@]}" \
  -o "$OUT/Contents/MacOS/RH899X" BarrotAudio.c \
  -framework CoreAudio -framework CoreFoundation -framework IOKit

IDENTITY="${CODESIGN_IDENTITY:-}"
if [[ -z "$IDENTITY" ]]; then
  IDENTITY="$(security find-identity -v -p codesigning 2>/dev/null \
    | awk '/Apple Development/ {print $2; exit}')"
fi

if [[ -n "$IDENTITY" ]]; then
  echo "signing with identity: $IDENTITY"
  codesign --force --deep --sign "$IDENTITY" --timestamp=none \
    --identifier com.ranhe.rh899x.audio "$OUT"
else
  echo "WARNING: no Apple Development identity found; ad-hoc signing for selftest only"
  codesign --force --deep --sign - \
    --identifier com.ranhe.rh899x.audio "$OUT"
fi

echo "built $OUT"
codesign -dv --verbose=2 "$OUT" 2>&1 | grep -E "Identifier|Authority|TeamIdentifier|Signature|flags"

#!/bin/zsh
set -euo pipefail
cd "$(dirname "$0")"

OUT="build/BarrotBT10.driver"
mkdir -p "$OUT/Contents/MacOS" "$OUT/Contents/Resources"
cp BarrotBT10.plist "$OUT/Contents/Info.plist"

clang -std=c11 -bundle -fPIC -O2 -Wall \
  -Wno-deprecated-declarations -pthread \
  -o "$OUT/Contents/MacOS/BarrotBT10" BarrotAudio.c \
  -framework CoreAudio -framework CoreFoundation -framework IOKit

IDENTITY="${CODESIGN_IDENTITY:-}"
if [[ -z "$IDENTITY" ]]; then
  IDENTITY="$(security find-identity -v -p codesigning 2>/dev/null \
    | awk '/Apple Development/ {print $2; exit}')"
fi

if [[ -n "$IDENTITY" ]]; then
  echo "signing with identity: $IDENTITY"
  codesign --force --deep --sign "$IDENTITY" --timestamp=none \
    --identifier com.barrot.bt10.audio "$OUT"
else
  echo "WARNING: no Apple Development identity found; ad-hoc signing for selftest only"
  codesign --force --deep --sign - \
    --identifier com.barrot.bt10.audio "$OUT"
fi

echo "built $OUT"
codesign -dv --verbose=2 "$OUT" 2>&1 | grep -E "Identifier|Authority|TeamIdentifier|Signature|flags"

#!/bin/zsh
set -euo pipefail
cd "$(dirname "$0")"

BUNDLE="build/BarrotBT10.driver"
DEST="/Library/Audio/Plug-Ins/HAL/BarrotBT10.driver"

if [[ ! -d "$BUNDLE" ]]; then
  echo "missing $BUNDLE; run ./build.sh first" >&2
  exit 1
fi

SIG="$(codesign -dv --verbose=4 "$BUNDLE" 2>&1 || true)"
if grep -q "Signature=adhoc" <<<"$SIG"; then
  echo "REFUSING TO INSTALL: the bundle is ad-hoc signed." >&2
  echo "coreaudiod's AMFI check rejects ad-hoc HAL plug-ins with error -423." >&2
  echo "Rebuild with an Apple-issued identity, for example:" >&2
  echo '  CODESIGN_IDENTITY="Apple Development: Your Name (TEAMID)" ./build.sh' >&2
  echo '  CODESIGN_IDENTITY="Developer ID Application: Your Name (TEAMID)" ./build.sh' >&2
  exit 2
fi
if ! grep -Eq "Authority=(Apple Development|Developer ID Application)" <<<"$SIG"; then
  echo "REFUSING TO INSTALL: signature is not an Apple-issued signing identity." >&2
  echo "A local self-signed certificate also fails AMFI with -423." >&2
  echo "Sign with Apple Development or Developer ID Application." >&2
  exit 2
fi
if ! codesign --verify --strict --deep "$BUNDLE" >/dev/null 2>&1; then
  echo "REFUSING TO INSTALL: codesign --verify --strict failed." >&2
  exit 2
fi

LIST_AUDIO="${TMPDIR:-/tmp}/barrot-list-audio-$$"
clang -std=c11 -O2 -o "$LIST_AUDIO" tools/list_audio.c \
  -framework CoreAudio -framework CoreFoundation
trap 'rm -f "$LIST_AUDIO"' EXIT
DEVICES_BEFORE="$("$LIST_AUDIO" 2>/dev/null || true)"

echo "$SIG" | grep -E "Authority=|TeamIdentifier=|Signature="
echo "Installing $BUNDLE -> $DEST"

STAMP="$(date +%Y%m%d-%H%M%S)"
if [[ -d "$DEST" ]]; then
  sudo mv "$DEST" "/Library/Audio/Plug-Ins/HAL/BarrotBT10.driver.backup-$STAMP"
fi
sudo cp -R "$BUNDLE" "$DEST"
sudo chown -R root:wheel "$DEST"

restart_audio() {
  if sudo launchctl print system/com.apple.audio.coreaudiod >/dev/null 2>&1; then
    sudo launchctl kickstart -k system/com.apple.audio.coreaudiod
  else
    sudo killall coreaudiod
  fi
  sleep 4
}

restart_audio

echo "--- CoreAudio devices ---"
DEVICES="$("$LIST_AUDIO" 2>/dev/null || true)"
echo "$DEVICES"

if [[ "$DEVICES_BEFORE" == *BuiltInSpeakerDevice* &&
      "$DEVICES" != *BuiltInSpeakerDevice* ]]; then
  echo "HEALTH CHECK FAILED: built-in speaker disappeared." >&2
  echo "Rolling back BarrotBT10.driver only." >&2
  sudo mv "$DEST" "/Library/Audio/Plug-Ins/HAL/BarrotBT10.driver.rollback-$STAMP" 2>/dev/null || true
  restart_audio
  echo "--- After rollback ---"
  "$LIST_AUDIO" 2>/dev/null || true
  exit 4
fi

echo "--- load check ---"
if grep -q "BarrotBT10-0c1c-3a1d" <<<"$DEVICES"; then
  echo "SUCCESS: Barrot BT10 is registered with CoreAudio."
else
  echo "NOT REGISTERED. Check:"
  echo "  sudo log show --last 2m --info --debug --predicate 'process == \"amfid\"'"
  echo "Rollback:"
  echo "  sudo mv $DEST /tmp/ && sudo killall coreaudiod"
  exit 3
fi

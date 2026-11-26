#!/bin/zsh
set -euo pipefail
cd "$(dirname "$0")"

BUNDLE="build/RH899X.driver"
DEST="/Library/Audio/Plug-Ins/HAL/RH899X.driver"
LEGACY_DEST="/Library/Audio/Plug-Ins/HAL/BarrotBT10.driver"

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

LIST_AUDIO="${TMPDIR:-/tmp}/rh899x-list-audio-$$"
clang -std=c11 -O2 -o "$LIST_AUDIO" tools/list_audio.c \
  -framework CoreAudio -framework CoreFoundation
trap 'rm -f "$LIST_AUDIO"' EXIT
DEVICES_BEFORE="$("$LIST_AUDIO" 2>/dev/null || true)"

echo "$SIG" | grep -E "Authority=|TeamIdentifier=|Signature="
echo "Installing $BUNDLE -> $DEST"

restart_audio() {
  if sudo launchctl print system/com.apple.audio.coreaudiod >/dev/null 2>&1; then
    sudo launchctl kickstart -k system/com.apple.audio.coreaudiod
  else
    sudo killall coreaudiod
  fi
  sleep 4
}

STAMP="$(date +%Y%m%d-%H%M%S)"
NEW_BACKUP=""
LEGACY_BACKUP=""

if [[ -d "$DEST" ]]; then
  NEW_BACKUP="${DEST}.backup-$STAMP"
  sudo mv "$DEST" "$NEW_BACKUP"
fi
if [[ -d "$LEGACY_DEST" ]]; then
  LEGACY_BACKUP="${LEGACY_DEST}.backup-$STAMP"
  sudo mv "$LEGACY_DEST" "$LEGACY_BACKUP"
fi

rollback() {
  echo "Rolling back RH899X.driver." >&2
  if [[ -d "$DEST" ]]; then
    sudo mv "$DEST" "${DEST}.rollback-$STAMP" 2>/dev/null || true
  fi
  if [[ -n "$NEW_BACKUP" && -d "$NEW_BACKUP" ]]; then
    sudo mv "$NEW_BACKUP" "$DEST" 2>/dev/null || true
  fi
  if [[ -n "$LEGACY_BACKUP" && -d "$LEGACY_BACKUP" && ! -d "$LEGACY_DEST" ]]; then
    sudo mv "$LEGACY_BACKUP" "$LEGACY_DEST" 2>/dev/null || true
  fi
  restart_audio
}

sudo cp -R "$BUNDLE" "$DEST"
sudo chown -R root:wheel "$DEST"

restart_audio

echo "--- CoreAudio devices ---"
DEVICES="$("$LIST_AUDIO" 2>/dev/null || true)"
echo "$DEVICES"

if [[ "$DEVICES_BEFORE" == *BuiltInSpeakerDevice* &&
      "$DEVICES" != *BuiltInSpeakerDevice* ]]; then
  echo "HEALTH CHECK FAILED: built-in speaker disappeared." >&2
  rollback
  echo "--- After rollback ---"
  "$LIST_AUDIO" 2>/dev/null || true
  exit 4
fi

echo "--- load check ---"
if grep -q "RH899X-0c1c-3a1d" <<<"$DEVICES"; then
  echo "SUCCESS: RH-899X is registered with CoreAudio."
else
  echo "NOT REGISTERED. Check:"
  echo "  sudo log show --last 2m --info --debug --predicate 'process == \"amfid\"'"
  echo "Rollback:"
  echo "  sudo mv $DEST /tmp/ && sudo killall coreaudiod"
  exit 3
fi

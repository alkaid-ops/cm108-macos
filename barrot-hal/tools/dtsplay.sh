#!/bin/zsh
# Play the first DTS / AC-3 track of any media file through RH-899X.
# Audio-only digital passthrough (alt 2). No video.
#   ./tools/dtsplay.sh <file> [seconds]

FF="${RH899X_FFMPEG:-$HOME/Library/Python/3.13/lib/python/site-packages/imageio_ffmpeg/binaries/ffmpeg-macos-aarch64-v9.0}"
if [ ! -x "$FF" ]; then
    FF="$(command -v ffmpeg)"
fi
if [ -z "$FF" ] || [ ! -x "$FF" ]; then
    echo "ffmpeg not found; set RH899X_FFMPEG" >&2
    exit 1
fi

SRC="$1"
SECS="$2"
if [ -z "$SRC" ]; then
    echo "usage: dtsplay.sh <file> [seconds]" >&2
    exit 1
fi

cd "$(dirname "$0")/.." || exit 1

INFO="$("$FF" -hide_banner -i "$SRC" 2>&1)"

case "$INFO" in
    *"Audio: dts "*)  EXT=dts ;;
    *"Audio: ac3 "*)  EXT=ac3 ;;
    *"Audio: eac3 "*) echo "E-AC-3 not supported by this device" >&2; exit 2 ;;
    *"Audio: truehd "*) echo "TrueHD not supported by this device" >&2; exit 2 ;;
    *) echo "no DTS/AC-3 track found in $SRC" >&2; exit 3 ;;
esac

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

"$FF" -hide_banner -loglevel error -y -i "$SRC" -map 0:a:0 -c copy \
      "$TMP/track.$EXT" || exit 4

if [ -z "$SECS" ] || [ "$SECS" = "0" ]; then
    SECS="$(echo "$INFO" | sed -n 's/.*Duration: 0\([0-9][0-9]\):\([0-9][0-9]\):\([0-9][0-9]\).*/\1 \2 \3/p' \
          | awk '{print $1*3600 + $2*60 + $3}' | head -1)"
    if [ -z "$SECS" ] || [ "$SECS" -lt 1 ] 2>/dev/null; then
        SECS=10
    fi
fi

echo "playing $EXT track for ${SECS}s through RH-899X (encoded output)"
./coreaudio-bitstream-test "$TMP/track.$EXT" "$SECS"

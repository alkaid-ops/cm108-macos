#!/bin/zsh
set -euo pipefail
cd "$(dirname "$0")"
./build.sh
clang -std=c11 -O2 -Wno-deprecated-declarations -o selftest selftest.c \
  -framework CoreAudio -framework CoreFoundation -ldl
exec ./selftest build/RH899X.driver/Contents/MacOS/RH899X "${1:-6}"

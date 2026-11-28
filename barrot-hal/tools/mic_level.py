#!/usr/bin/env python3
"""Live 440 Hz level meter for the acoustic convergence harness.

Run this while VLC plays the gate440.dts asset through
`RH-899X (Encoded Output)`. Move the speaker (or the MacBook) until the
bar rises well above the floor; then run tools/autotune.py.

Usage:
    python3 tools/mic_level.py [seconds] [freq]
"""

import math
import os
import subprocess
import sys
import time
from pathlib import Path

RATE = 48000
BLOCK = RATE // 10          # 100 ms
WIN = RATE // 2             # 500 ms analysis window


def find_ffmpeg() -> str:
    candidates = [
        os.environ.get("RH899X_FFMPEG", ""),
        str(Path.home() / "Library/Python/3.13/lib/python/site-packages/"
            "imageio_ffmpeg/binaries/ffmpeg-macos-aarch64-v9.0"),
    ]
    for c in candidates:
        if c and Path(c).is_file():
            return c
    raise SystemExit("ffmpeg not found; set RH899X_FFMPEG")


def goertzel_amp(chunk, freq):
    n = len(chunk)
    k = round(freq * n / RATE)
    w = 2 * math.pi * k / n
    coeff = 2 * math.cos(w)
    s1 = s2 = 0.0
    for x in chunk:
        s0 = x + coeff * s1 - s2
        s2, s1 = s1, s0
    power = s1 * s1 + s2 * s2 - coeff * s1 * s2
    return math.sqrt(max(power, 0.0)) / n


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0
    freq = float(sys.argv[2]) if len(sys.argv) > 2 else 440.0
    ff = find_ffmpeg()
    proc = subprocess.Popen(
        [ff, "-hide_banner", "-loglevel", "error", "-f", "avfoundation",
         "-i", ":0", "-ar", str(RATE), "-ac", "1", "-f", "s16le", "-"],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    print(f"listening {seconds:.0f}s at {freq:.0f} Hz; play the DTS asset now",
            flush=True)
    buf = []
    t0 = time.time()
    peak = -200.0
    try:
        while time.time() - t0 < seconds:
            raw = proc.stdout.read(BLOCK * 2)
            if not raw:
                break
            vals = [int.from_bytes(raw[i:i + 2], "little", signed=True) / 32768.0
                    for i in range(0, len(raw) - 1, 2)]
            buf.extend(vals)
            if len(buf) > WIN:
                buf = buf[-WIN:]
            if len(buf) < WIN:
                continue
            amp = goertzel_amp(buf, freq)
            db = 20 * math.log10(amp) if amp > 0 else -120.0
            peak = max(peak, db)
            filled = max(0, min(50, int((db + 90) / 1.5)))
            bar = "#" * filled + "." * (50 - filled)
            print(f"\r{db:6.1f} dBFS [{bar}]", end="", flush=True)
    finally:
        proc.send_signal(2)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        print(f"\npeak {peak:.1f} dBFS at {freq:.0f} Hz")
    return 0


if __name__ == "__main__":
    sys.exit(main())

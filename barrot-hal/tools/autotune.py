#!/usr/bin/env python3
"""Acoustic convergence harness for the RH-899X HAL plug-in.

Playback: VLC CLI -> RH-899X (Encoded Output) -> USB -> downstream decoder
Capture:   MacBook built-in microphone (AVFoundation device 0)
Score:     normalised cross-correlation of the 10 ms RMS energy envelopes

The harness rebuilds and reinstalls the driver for every candidate, then
records the room while VLC plays a gated 440 Hz AC3 burst pattern. A trial
passes when the recorded envelope tracks the reference envelope and the
playback is clearly louder than the room noise.

No third-party Python packages are required.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import shutil
import signal
import struct
import subprocess
import sys
import time
import wave
from dataclasses import asdict, dataclass, field
from pathlib import Path

HAL_DIR = Path(__file__).resolve().parent.parent
DEVICE_UID = "RH899X-0c1c-3a1d"
VLC_BIN = Path("/Applications/VLC.app/Contents/MacOS/VLC")
INSTALL_DIR = Path("/Library/Audio/Plug-Ins/HAL")

RATE = 48000
FRAME_MS = 50              # envelope hop, also the lag unit
WIN_MS = 500               # sliding Goertzel window (narrow-band gain)
MAX_LAG_FRAMES = 200          # 10 s of search at a 50 ms hop
SCORE_PASS = 0.75
GAIN_PASS_DB = 10.0
CLIP_LIMIT = 0.005

PASS_DURATIONS = [(1.6, 0.7), (1.0, 0.5), (2.0, 0.6), (1.0, 0.6)]
PREROLL_S = 4.0
TAIL_S = 1.5


def log(msg: str) -> None:
    print(f"[autotune] {msg}", flush=True)


def find_ffmpeg() -> str:
    candidates = [
        os.environ.get("RH899X_FFMPEG", ""),
        str(Path.home() / "Library/Python/3.13/lib/python/site-packages/"
            "imageio_ffmpeg/binaries/ffmpeg-macos-aarch64-v9.0"),
        shutil.which("ffmpeg") or "",
    ]
    for c in candidates:
        if c and Path(c).is_file():
            return c
    raise SystemExit("ffmpeg not found; set RH899X_FFMPEG")


FFMPEG = find_ffmpeg()


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)


def sudo_refresh() -> None:
    password = os.environ.get("RH899X_SUDO_PASS")
    if not password:
        raise SystemExit("set RH899X_SUDO_PASS to the sudo password")
    p = subprocess.run(["sudo", "-S", "-p", "", "-v"], input=password + "\n",
                       text=True, capture_output=True)
    if p.returncode != 0:
        raise SystemExit(f"sudo -v failed: {p.stderr.strip()}")


def compile_tool(src: Path, out: Path) -> None:
    run(["clang", "-std=c11", "-O2", "-Wno-deprecated-declarations",
         "-o", str(out), str(src),
         "-framework", "CoreAudio", "-framework", "CoreFoundation"])


LIST_AUDIO = Path("/tmp/rh899x_list_audio")


def device_id(uid: str = DEVICE_UID) -> int:
    if not LIST_AUDIO.exists():
        compile_tool(HAL_DIR / "tools/list_audio.c", LIST_AUDIO)
    out = subprocess.run([str(LIST_AUDIO)], capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.split("\t")
        if len(parts) >= 3 and parts[2].strip() == uid:
            return int(parts[0])
    raise SystemExit(f"{uid} not found in CoreAudio")


def usb_present() -> bool:
    probe = HAL_DIR.parent / "usbdesc"
    if not probe.exists():
        return True
    return probe.exists() and subprocess.run(
        [str(probe), "0x0c1c", "0x3a1d"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0


def require_usb() -> None:
    if not usb_present():
        raise SystemExit(
            "RH-899X (0c1c:3a1d) is not on the USB bus; replug the device and rerun")


def gen_asset(work: Path) -> tuple[Path, Path]:
    """Return (ac3 asset, reference wav)."""
    raw = work / "gate440.wav"
    mono: list[int] = []
    amp = 0.7
    phase = 0.0
    for on_s, off_s in PASS_DURATIONS:
        n = int(round(on_s * RATE))
        for _ in range(n):
            mono.append(int(32767 * amp * math.sin(phase)))
            phase += 2 * math.pi * 440 / RATE
        m = int(round(off_s * RATE))
        mono.extend([0] * m)
    # 5.1 (FL FR FC LFE BL BR): tone on the front pair, silence elsewhere.
    frames = b"".join(struct.pack("<6h", v, v, 0, 0, 0, 0) for v in mono)
    with wave.open(str(raw), "wb") as w:
        w.setnchannels(6)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(frames)

    dts = work / "gate440.dts"
    run([FFMPEG, "-hide_banner", "-loglevel", "error", "-y",
         "-i", str(raw), "-c:a", "dca", "-b:a", "1536k", "-ac", "6",
         "-strict", "-2", str(dts)])
    ref = work / "reference.wav"
    run([FFMPEG, "-hide_banner", "-loglevel", "error", "-y",
         "-i", str(dts), "-ac", "1", "-ar", str(RATE), "-c:a", "pcm_s16le",
         str(ref)])
    return dts, ref


def read_wav(path: Path) -> list[float]:
    with wave.open(str(path), "rb") as w:
        raw = w.readframes(w.getnframes())
        width = w.getsampwidth()
        if width != 2:
            raise SystemExit(f"{path}: expected 16-bit PCM")
        n = len(raw) // 2
    return [v / 32768.0 for v in struct.unpack(f"<{n}h", raw[:n * 2])]


def goertzel_amp(chunk: list[float], freq: float) -> float:
    n = len(chunk)
    k = int(round(freq * n / RATE))
    w = 2 * math.pi * k / n
    coeff = 2 * math.cos(w)
    s1 = s2 = 0.0
    for x in chunk:
        s0 = x + coeff * s1 - s2
        s2, s1 = s1, s0
    power = s1 * s1 + s2 * s2 - coeff * s1 * s2
    return math.sqrt(max(power, 0.0)) / n


def envelope(samples: list[float], freq: float = 0.0) -> list[float]:
    win = RATE * WIN_MS // 1000
    hop = RATE * FRAME_MS // 1000
    env = []
    for i in range(0, len(samples) - win + 1, hop):
        chunk = samples[i:i + win]
        if freq > 0:
            env.append(goertzel_amp(chunk, freq))
        else:
            env.append(math.sqrt(sum(x * x for x in chunk) / win))
    return env


def zscore(values: list[float]) -> list[float]:
    mean = sum(values) / len(values)
    var = sum((v - mean) ** 2 for v in values) / len(values)
    std = math.sqrt(var)
    if std < 1e-12:
        return [0.0] * len(values)
    return [(v - mean) / std for v in values]


def best_correlation(ref: list[float], rec: list[float]) -> tuple[float, int]:
    a = zscore(ref)
    b = zscore(rec)
    n = len(a)
    if len(b) <= n:
        m = min(len(b), n)
        a, n = a[:m], m
    best, best_lag = -1.0, 0
    for lag in range(0, min(MAX_LAG_FRAMES, len(b) - n) + 1):
        s = sum(a[i] * b[lag + i] for i in range(n)) / n
        if s > best:
            best, best_lag = s, lag
    return best, best_lag


def rms_of(samples: list[float]) -> float:
    return math.sqrt(sum(x * x for x in samples) / max(len(samples), 1))


def signal_stats(samples: list[float], lag_s: float, ambient: float) -> dict:
    """Split the recording at the correlation-derived playback position."""
    start = int((lag_s + 0.2) * RATE)
    end = min(len(samples), start + int(8.0 * RATE))
    span = samples[start:end]
    peak = rms_of(span)
    clip = sum(1 for x in samples if abs(x) >= 0.999) / max(len(samples), 1)
    gain_db = 20.0 * math.log10(peak / ambient) if ambient > 1e-9 and peak > 0 else -99.0
    return {"ambient_rms": ambient, "peak_rms": peak,
            "gain_db": gain_db, "clip_frac": clip}


def measure_ambient(work: Path) -> float:
    wav = work / "ambient.wav"
    rec = start_recorder(wav)
    time.sleep(4)
    stop_recorder(rec)
    samples = read_wav(wav)
    return rms_of(samples[-int(3 * RATE):])


def start_recorder(out_wav: Path):
    rec = subprocess.Popen(
        [FFMPEG, "-hide_banner", "-loglevel", "error",
         "-f", "avfoundation", "-i", ":0",
         "-ar", str(RATE), "-ac", "1", "-c:a", "pcm_s16le",
         "-y", str(out_wav)],
        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE, text=True)
    deadline = time.time() + 15
    while time.time() < deadline:
        try:
            if out_wav.exists() and out_wav.stat().st_size > 8192:
                break
        except OSError:
            pass
        time.sleep(0.15)
    time.sleep(PREROLL_S)
    return rec


def stop_recorder(rec):
    rec.send_signal(signal.SIGINT)
    try:
        return rec.communicate(timeout=10)[1]
    except subprocess.TimeoutExpired:
        rec.kill()
        return rec.communicate()[1]


def record_with_playback(out_wav: Path, play_cmd: list[str], timeout_s: int) -> dict:
    rec = start_recorder(out_wav)
    started = time.time()
    play = subprocess.Popen(play_cmd, stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True)
    try:
        out, _ = play.communicate(timeout=timeout_s)
        play_rc, timed_out = play.returncode, False
    except subprocess.TimeoutExpired:
        play.kill()
        out, _ = play.communicate()
        play_rc, timed_out = -9, True
    time.sleep(TAIL_S)
    rec_err = stop_recorder(rec)
    return {"play_rc": play_rc, "play_timed_out": timed_out,
            "play_seconds": round(time.time() - started, 2),
            "play_log": out or "", "rec_stderr": rec_err or ""}


@dataclass
class Config:
    bytes_per_frame: int = 4
    buffer_frames: int = 512
    latency_frames: int = 0
    safety_frames: int = 0


@dataclass
class Trial:
    index: int
    config: Config
    kind: str = "hal"
    score: float = 0.0
    lag_ms: int = 0
    gain_db: float = -99.0
    clip_frac: float = 0.0
    ambient_rms: float = 0.0
    peak_rms: float = 0.0
    passed: bool = False
    notes: str = ""
    extra: dict = field(default_factory=dict)


def build_and_install(cfg: Config) -> None:
    env = os.environ.copy()
    env["RH899X_BYTES_PER_FRAME"] = str(cfg.bytes_per_frame)
    env["RH899X_BUFFER_FRAMES"] = str(cfg.buffer_frames)
    env["RH899X_LATENCY_FRAMES"] = str(cfg.latency_frames)
    env["RH899X_SAFETY_FRAMES"] = str(cfg.safety_frames)
    run(["./build.sh"], cwd=HAL_DIR, env=env,
        stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    sudo_refresh()
    run(["./install.sh"], cwd=HAL_DIR, stdout=subprocess.DEVNULL,
        stderr=subprocess.STDOUT)
    deadline = time.time() + 20
    while time.time() < deadline:
        try:
            device_id()
            return
        except SystemExit:
            time.sleep(0.5)
    raise SystemExit("RH-899X did not re-appear after install")


def hal_stats_since(t0: float) -> str:
    p = subprocess.run(["sudo", "log", "show", "--last", "90s",
                        "--info", "--debug"], capture_output=True, text=True)
    lines = [l.strip() for l in p.stdout.splitlines() if "StopIO client" in l]
    return lines[-1] if lines else ""


def vlc_cmd(asset: Path, dev_id: int) -> list[str]:
    return [str(VLC_BIN), "-I", "dummy", "--no-video", "--no-media-library",
            "--aout=auhal", f"--auhal-audio-device={dev_id | 0xf00000}",
            "--play-and-exit", str(asset)]


def run_trial(idx: int, cfg: Config, kind: str, work: Path,
              asset: Path, ref_env: list[float], dev_id: int,
              ambient: float, prerecorded: Path | None = None) -> Trial:
    subprocess.run(["pkill", "-f", "VLC.app/Contents/MacOS/VLC"],
                   check=False, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    time.sleep(0.3)
    trial_dir = work / f"trial-{idx:02d}-{kind}-bpf{cfg.bytes_per_frame}-buf{cfg.buffer_frames}-lat{cfg.latency_frames}-saf{cfg.safety_frames}"
    trial_dir.mkdir(parents=True, exist_ok=True)
    (trial_dir / "config.json").write_text(json.dumps(asdict(cfg), indent=2))
    wav = trial_dir / "mic.wav"
    t0 = time.time()

    if kind == "hal":
        require_usb()
        build_and_install(cfg)
        cmd = vlc_cmd(asset, dev_id)
        info = record_with_playback(wav, cmd, timeout_s=30)
    elif kind == "tool":
        cmd = [str(HAL_DIR / "coreaudio-bitstream-test"), str(asset), "8"]
        info = record_with_playback(wav, cmd, timeout_s=30)
    elif kind == "silence":
        info = {"play_rc": 0, "play_timed_out": False, "play_seconds": 0.0,
                "play_log": "", "rec_stderr": ""}
        rec = start_recorder(wav)
        time.sleep(8 + TAIL_S)
        stop_recorder(rec)
    elif kind == "miconly":
        info = {"play_rc": 0, "play_timed_out": False, "play_seconds": 0.0,
                "play_log": "", "rec_stderr": ""}
        rec = start_recorder(wav)
        time.sleep(4)
        stop_recorder(rec)
    else:
        raise SystemExit(f"unknown trial kind {kind}")

    (trial_dir / "playback.log").write_text(info["play_log"])
    (trial_dir / "recorder.log").write_text(info["rec_stderr"])
    if kind == "hal":
        stats = hal_stats_since(t0)
        (trial_dir / "hal_stopio.txt").write_text(stats + "\n")
        info["hal_stopio"] = stats

    if not Path(wav).exists() or Path(wav).stat().st_size < 1000:
        t = Trial(idx, cfg, kind, notes="no recording", extra=info)
        (trial_dir / "result.json").write_text(json.dumps(asdict(t), indent=2))
        return t

    samples = read_wav(wav)
    rec_env = envelope(samples, 440.0)
    score, lag = best_correlation(ref_env, rec_env)
    st = signal_stats(samples, lag * FRAME_MS / 1000.0, ambient)
    passed = (score >= SCORE_PASS and st["gain_db"] >= GAIN_PASS_DB
              and st["clip_frac"] < CLIP_LIMIT and kind != "silence")
    t = Trial(idx, cfg, kind, score=round(score, 3), lag_ms=lag * FRAME_MS,
              gain_db=round(st["gain_db"], 1),
              clip_frac=round(st["clip_frac"], 5),
              ambient_rms=round(st["ambient_rms"], 6),
              peak_rms=round(st["peak_rms"], 6),
              passed=passed, extra=info)
    (trial_dir / "result.json").write_text(json.dumps(asdict(t), indent=2))
    (trial_dir / "envelope.json").write_text(json.dumps(
        {"ref": ref_env, "rec": rec_env}))
    log(f"trial {idx} {kind} bpf={cfg.bytes_per_frame} buf={cfg.buffer_frames} "
        f"lat={cfg.latency_frames} saf={cfg.safety_frames} -> "
        f"score={t.score} lag={t.lag_ms}ms gain={t.gain_db}dB "
        f"clip={t.clip_frac} pass={t.passed}")
    return t


def write_summary(work: Path, trials: list[Trial]) -> None:
    lines = ["# RH-899X acoustic autotune summary", ""]
    lines.append(f"sensitivity: pass = score >= {SCORE_PASS}, "
                 f"gain >= {GAIN_PASS_DB} dB, clip < {CLIP_LIMIT}")
    lines.append("")
    lines.append("| # | kind | bpf | buf | lat | saf | score | lag ms | gain dB | pass |")
    lines.append("|---|---|---|---|---|---|---|---|---|---|")
    for t in trials:
        lines.append(
            f"| {t.index} | {t.kind} | {t.config.bytes_per_frame} | "
            f"{t.config.buffer_frames} | {t.config.latency_frames} | "
            f"{t.config.safety_frames} | {t.score} | {t.lag_ms} | "
            f"{t.gain_db} | {'yes' if t.passed else 'no'} |")
    best = max((t for t in trials if t.kind == "hal"),
               key=lambda t: t.score, default=None)
    lines.append("")
    if best:
        lines.append(f"best HAL trial: #{best.index} score={best.score} "
                     f"config={asdict(best.config)}")
    (work / "summary.md").write_text("\n".join(lines) + "\n")
    (work / "summary.json").write_text(json.dumps(
        [{"trial": t.index, "kind": t.kind, "config": asdict(t.config),
          "score": t.score, "lag_ms": t.lag_ms, "gain_db": t.gain_db,
          "clip_frac": t.clip_frac, "passed": t.passed, "notes": t.notes}
         for t in trials], indent=2))


def apply_defaults(cfg: Config) -> None:
    src = HAL_DIR / "BarrotAudio.c"
    text = src.read_text()
    text = re.sub(r"#define RH899X_BYTES_PER_FRAME [0-9]+u",
                  f"#define RH899X_BYTES_PER_FRAME {cfg.bytes_per_frame}u", text)
    text = re.sub(r"#define RH899X_BUFFER_FRAMES [0-9]+u",
                  f"#define RH899X_BUFFER_FRAMES {cfg.buffer_frames}u", text)
    text = re.sub(r"#define RH899X_LATENCY_FRAMES [0-9]+u",
                  f"#define RH899X_LATENCY_FRAMES {cfg.latency_frames}u", text)
    text = re.sub(r"#define RH899X_SAFETY_FRAMES [0-9]+u",
                  f"#define RH899X_SAFETY_FRAMES {cfg.safety_frames}u", text)
    src.write_text(text)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default="", help="output directory")
    ap.add_argument("--skip-controls", action="store_true")
    ap.add_argument("--only-controls", action="store_true")
    ap.add_argument("--no-apply", action="store_true",
                    help="do not rewrite shipping defaults")
    args = ap.parse_args()

    work = Path(args.out) if args.out else (
        HAL_DIR / "results" / "autotune" /
        time.strftime("%Y%m%d-%H%M%S"))
    work.mkdir(parents=True, exist_ok=True)
    log(f"work dir: {work}")
    require_usb()

    backups_before = sorted(INSTALL_DIR.glob("RH899X.driver.backup-*"))
    asset, ref = gen_asset(work)
    ref_env = envelope(read_wav(ref), 440.0)
    ambient = measure_ambient(work)
    log(f"ambient rms {ambient:.6f}")
    dev = device_id()
    log(f"device id {dev}, encoded id {dev | 0xf00000}, ref frames {len(ref_env)}")

    tool = HAL_DIR / "coreaudio-bitstream-test"
    compile_tool(HAL_DIR / "coreaudio_bitstream_test.c", tool)

    trials: list[Trial] = []
    idx = 0
    if not args.skip_controls:
        trials.append(run_trial(idx, Config(), "tool", work, asset, ref_env, dev, ambient))
        idx += 1
        trials.append(run_trial(idx, Config(), "silence", work, asset, ref_env, dev, ambient))
        idx += 1
        trials.append(run_trial(idx, Config(), "miconly", work, asset, ref_env, dev, ambient))
        idx += 1
        pos = trials[0]
        mic = trials[2]
        if mic.ambient_rms <= 1e-6:
            log("MIC CONTROL FAILED: the built-in microphone returned silence; "
                "check microphone permission")
            write_summary(work, trials)
            return 2
        if not pos.passed:
            log("POSITIVE CONTROL FAILED: no tone was recorded from RH-899X; "
                "check the decoder input (PC-USB), power and volume")
            write_summary(work, trials)
            return 3
        if args.only_controls:
            write_summary(work, trials)
            return 0

    grid = [Config(bpf, buf) for bpf in (0, 4) for buf in (192, 256, 384, 512)]
    for cfg in grid:
        trials.append(run_trial(idx, cfg, "hal", work, asset, ref_env, dev, ambient))
        idx += 1
        best = max(trials, key=lambda t: t.score)
        if best.passed and best.score >= 0.85:
            log(f"early converge at trial {best.index}")
            break

    hal = [t for t in trials if t.kind == "hal"]
    best = max(hal, key=lambda t: t.score) if hal else None
    if best:
        for lat in (1536, 3072):
            cfg = Config(best.config.bytes_per_frame, best.config.buffer_frames,
                         lat, best.config.safety_frames)
            trials.append(run_trial(idx, cfg, "hal", work, asset, ref_env, dev, ambient))
            idx += 1
        hal = [t for t in trials if t.kind == "hal"]
        best = max(hal, key=lambda t: t.score)
        for saf in (512,):
            cfg = Config(best.config.bytes_per_frame, best.config.buffer_frames,
                         best.config.latency_frames, saf)
            trials.append(run_trial(idx, cfg, "hal", work, asset, ref_env, dev, ambient))
            idx += 1

    write_summary(work, trials)
    hal = [t for t in trials if t.kind == "hal"]
    best = max(hal, key=lambda t: t.score) if hal else None
    if not best or not best.passed:
        log("NO CONFIG PASSED; shipping defaults left untouched")
        log(f"best score {best.score if best else 0} "
            f"config {asdict(best.config) if best else None}")
        return 4

    if not args.no_apply:
        log(f"applying {asdict(best.config)} to shipping defaults")
        apply_defaults(best.config)
        build_and_install(best.config)
        for _ in range(2):
            trials.append(run_trial(idx, best.config, "hal", work, asset,
                                    ref_env, dev, ambient))
            idx += 1
        write_summary(work, trials)
        confirm = trials[-2:]
        if not all(t.passed for t in confirm):
            log("confirmation failed; defaults were applied but not verified")
            return 5

    for b in sorted(INSTALL_DIR.glob("RH899X.driver.backup-*")):
        if b not in backups_before:
            sudo_refresh()
            subprocess.run(["sudo", "rm", "-rf", str(b)], check=False)
    log(f"done: {work / 'summary.md'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

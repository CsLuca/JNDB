#!/usr/bin/env python3
import argparse
import math
import struct
import wave
from pathlib import Path


def main() -> int:
    p = argparse.ArgumentParser(description="Generate deterministic short WAV for CI E2E smoke")
    p.add_argument("--out", required=True)
    p.add_argument("--sample-rate", type=int, default=8000)
    p.add_argument("--seconds", type=float, default=6.0)
    p.add_argument("--tone-hz", type=float, default=700.0)
    args = p.parse_args()

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)

    sr = max(1000, args.sample_rate)
    n_total = int(sr * max(1.0, args.seconds))

    # Repeated Morse-like pattern for T I
    pattern = [("on", 0.24), ("off", 0.08), ("on", 0.08), ("off", 0.32)] * 6
    samples = []
    idx = 0
    for state, dt in pattern:
        n = int(dt * sr)
        for _ in range(n):
            x = 0.7 * math.sin(2.0 * math.pi * args.tone_hz * (idx / sr)) if state == "on" else 0.0
            samples.append(max(-1.0, min(1.0, x)))
            idx += 1
    if len(samples) < n_total:
        samples.extend([0.0] * (n_total - len(samples)))
    samples = samples[:n_total]

    with wave.open(str(out), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        frames = b"".join(struct.pack("<h", int(s * 32767.0)) for s in samples)
        w.writeframes(frames)
    print(str(out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

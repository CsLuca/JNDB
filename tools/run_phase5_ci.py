#!/usr/bin/env python3
import argparse
import json
import subprocess
import sys
from pathlib import Path


def main() -> int:
    p = argparse.ArgumentParser(description="Phase 5 CI wrapper with A/B regression gates")
    p.add_argument("--python", default=sys.executable, help="Python executable")
    p.add_argument("--validator", default="tools/benchmark_phase5_validation.py")
    p.add_argument("--manifest", required=True)
    p.add_argument("--beacon-db", required=True)
    p.add_argument("--mode", choices=["known-list", "blind"], default="known-list")
    p.add_argument("--decoder-a", required=True)
    p.add_argument("--decoder-b", required=True)
    p.add_argument("--name-a", default="baseline")
    p.add_argument("--name-b", default="candidate")
    p.add_argument("--config-a", default="")
    p.add_argument("--config-b", default="")
    p.add_argument("--out-dir", required=True)
    p.add_argument("--msys-bash", default="")
    p.add_argument("--run", action="store_true")
    p.add_argument("--min-confidence", type=float, default=0.0)
    p.add_argument("--freq-tol-hz", type=float, default=2.0)
    p.add_argument("--min-overlap-sec", type=float, default=0.2)
    p.add_argument("--id-freq-tol-hz", type=float, default=3.0)
    p.add_argument("--min-delta-f1", type=float, default=-0.01)
    p.add_argument("--min-delta-precision", type=float, default=-0.02)
    p.add_argument("--min-delta-recall", type=float, default=-0.02)
    p.add_argument("--max-delta-false-alarm", type=int, default=3)
    p.add_argument("--max-delta-miss", type=int, default=2)
    p.add_argument("--max-delta-confusion", type=int, default=2)
    p.add_argument("--max-delta-xrt", type=float, default=0.2)
    args = p.parse_args()

    cmd = [
        args.python,
        args.validator,
        "--manifest",
        args.manifest,
        "--beacon-db",
        args.beacon_db,
        "--mode",
        args.mode,
        "--decoder-a",
        args.decoder_a,
        "--decoder-b",
        args.decoder_b,
        "--name-a",
        args.name_a,
        "--name-b",
        args.name_b,
        "--out-dir",
        args.out_dir,
        "--min-confidence",
        str(args.min_confidence),
        "--freq-tol-hz",
        str(args.freq_tol_hz),
        "--min-overlap-sec",
        str(args.min_overlap_sec),
        "--id-freq-tol-hz",
        str(args.id_freq_tol_hz),
    ]
    if args.run:
        cmd.append("--run")
    if args.msys_bash:
        cmd += ["--msys-bash", args.msys_bash]
    if args.config_a:
        cmd += ["--config-a", args.config_a]
    if args.config_b:
        cmd += ["--config-b", args.config_b]

    cp = subprocess.run(cmd, check=False, text=True)
    if cp.returncode != 0:
        return cp.returncode

    summary_path = Path(args.out_dir) / "phase5_validation_summary.json"
    if not summary_path.exists():
        print(f"Missing summary: {summary_path}")
        return 2

    summary = json.loads(summary_path.read_text(encoding="utf-8"))
    delta = summary.get("delta_B_minus_A")
    if not delta:
        print("Missing delta_B_minus_A in summary (A/B run required)")
        return 2

    checks = [
        ("delta_f1", float(delta.get("f1", 0.0)) >= args.min_delta_f1,
         f"{delta.get('f1', 0.0):.6f} >= {args.min_delta_f1:.6f}"),
        ("delta_precision", float(delta.get("precision", 0.0)) >= args.min_delta_precision,
         f"{delta.get('precision', 0.0):.6f} >= {args.min_delta_precision:.6f}"),
        ("delta_recall", float(delta.get("recall", 0.0)) >= args.min_delta_recall,
         f"{delta.get('recall', 0.0):.6f} >= {args.min_delta_recall:.6f}"),
        ("delta_false_alarm", int(delta.get("false_alarm", 0)) <= args.max_delta_false_alarm,
         f"{int(delta.get('false_alarm', 0))} <= {args.max_delta_false_alarm}"),
        ("delta_miss", int(delta.get("miss", 0)) <= args.max_delta_miss,
         f"{int(delta.get('miss', 0))} <= {args.max_delta_miss}"),
        ("delta_confusion", int(delta.get("confusion", 0)) <= args.max_delta_confusion,
         f"{int(delta.get('confusion', 0))} <= {args.max_delta_confusion}"),
        ("delta_xrt", float(delta.get("runtime_x_realtime", 0.0)) <= args.max_delta_xrt,
         f"{float(delta.get('runtime_x_realtime', 0.0)):.6f} <= {args.max_delta_xrt:.6f}"),
    ]

    failed = [c for c in checks if not c[1]]
    print("Phase 5 CI gates")
    for name, ok, msg in checks:
        print(f"- {name}: {'OK' if ok else 'FAIL'} ({msg})")

    if failed:
        print("Regression gates FAILED")
        return 3

    print("Regression gates PASSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

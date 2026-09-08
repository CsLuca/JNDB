#!/usr/bin/env python3
import argparse
import csv
import json
import shlex
import subprocess
from pathlib import Path


def parse_quiet(stdout: str):
    out = {"rows": None, "quality": None, "mean_conf": None, "decode_ratio": None}
    txt = (stdout or "").strip()
    for part in txt.split():
        if "=" not in part:
            continue
        k, v = part.split("=", 1)
        try:
            if k == "rows":
                out[k] = int(v)
            elif k in out:
                out[k] = float(v)
        except Exception:
            pass
    return out


def run_case(msys: Path, exe: str, wav: str, out_csv: str, out_json: str, calib: str,
             prior_mode: str, prior_file: str, max_seconds: int):
    args = [
        shlex.quote(exe),
        shlex.quote(wav),
        shlex.quote(out_csv),
        "--mode", "phase4-serious",
        "--confidence-calibration", shlex.quote(calib),
        "--max-seconds", str(max_seconds),
        "--metrics", shlex.quote(out_json),
        "--quiet",
        "--no-progress",
    ]
    if prior_mode != "off":
        args += ["--freq-prior-file", shlex.quote(prior_file)]
        if prior_mode == "require":
            args += ["--require-prior-match"]
    shell = "export MSYSTEM=UCRT64; source /etc/profile; " + " ".join(args)
    cp = subprocess.run([str(msys), "-lc", shell], capture_output=True, text=True, timeout=420)
    rec = {
        "rc": cp.returncode,
        "stdout": cp.stdout.strip(),
        "stderr": cp.stderr.strip(),
    }
    rec.update(parse_quiet(cp.stdout))
    return rec


def main() -> int:
    p = argparse.ArgumentParser(description="Phase 4 A/B benchmark on calibration and priors")
    p.add_argument("--root", default=r"C:\Users\LBiondi\OneDrive - centrosoftware.com\Documenti\Default Project\JNDB")
    p.add_argument("--wav-dir", default=None)
    p.add_argument("--out-dir", default=None)
    p.add_argument("--prior-file", default=None)
    p.add_argument("--max-seconds", type=int, default=10)
    args = p.parse_args()

    root = Path(args.root)
    wav_dir = Path(args.wav_dir) if args.wav_dir else root / "samples" / "ndb_phase3"
    out_dir = Path(args.out_dir) if args.out_dir else root / "benchmarks" / "phase4" / "ab_runs"
    prior_file = Path(args.prior_file) if args.prior_file else root / "benchmarks" / "phase4" / "priors.example.csv"
    msys = Path(r"C:\msys64\usr\bin\bash.exe")
    exe = "/c/Users/LBiondi/OneDrive - centrosoftware.com/Documenti/Default Project/JNDB/build_ucrt64/ndb_decode.exe"

    out_dir.mkdir(parents=True, exist_ok=True)

    wav_files = [wav_dir / "ori.wav", wav_dir / "gaz.wav", wav_dir / "pla.wav"]
    calibrations = ["none", "platt", "isotonic"]
    prior_modes = ["off", "on"]

    rows = []
    for wav in wav_files:
        wav_posix = "/c/" + str(wav).replace("\\", "/").split(":", 1)[1].lstrip("/")
        for calib in calibrations:
            for pmode in prior_modes:
                tag = f"{wav.stem}_{calib}_prior-{pmode}"
                out_csv = out_dir / f"{tag}.csv"
                out_json = out_dir / f"{tag}.json"
                prior_posix = "/c/" + str(prior_file).replace("\\", "/").split(":", 1)[1].lstrip("/")
                rec = run_case(
                    msys=msys,
                    exe=exe,
                    wav=wav_posix,
                    out_csv="/c/" + str(out_csv).replace("\\", "/").split(":", 1)[1].lstrip("/"),
                    out_json="/c/" + str(out_json).replace("\\", "/").split(":", 1)[1].lstrip("/"),
                    calib=calib,
                    prior_mode=pmode,
                    prior_file=prior_posix,
                    max_seconds=args.max_seconds,
                )
                rows.append(
                    {
                        "file": wav.name,
                        "calibration": calib,
                        "prior_mode": pmode,
                        **rec,
                    }
                )

    out_json = out_dir / "phase4_ab_summary.json"
    out_csv = out_dir / "phase4_ab_summary.csv"
    with out_json.open("w", encoding="utf-8") as f:
        json.dump(rows, f, indent=2)
    with out_csv.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(
            f,
            fieldnames=[
                "file",
                "calibration",
                "prior_mode",
                "rc",
                "rows",
                "quality",
                "mean_conf",
                "decode_ratio",
                "stdout",
                "stderr",
            ],
        )
        w.writeheader()
        for r in rows:
            w.writerow(r)

    print(json.dumps(rows, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

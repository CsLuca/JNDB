#!/usr/bin/env python3
import argparse
import csv
import itertools
import json
import subprocess
import tempfile
from pathlib import Path
from typing import Dict, List


def run_benchmark(decoder: Path, manifest: Path, config: Path, out_dir: Path, msys_bash: Path, no_history: bool) -> Dict:
    cmd = [
        "python",
        str(Path("tools") / "benchmark_phase0.py"),
        "--decoder",
        str(decoder),
        "--manifest",
        str(manifest),
        "--config",
        str(config),
        "--out-dir",
        str(out_dir),
        "--run",
    ]
    if msys_bash:
        cmd.extend(["--msys-bash", str(msys_bash)])
    if no_history:
        cmd.append("--no-history")

    cp = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if cp.returncode != 0:
        raise RuntimeError(
            "benchmark_phase0 failed\n"
            f"cmd={' '.join(cmd)}\n"
            f"stdout={cp.stdout}\n"
            f"stderr={cp.stderr}"
        )

    report_path = out_dir / "benchmark_report.json"
    with report_path.open("r", encoding="utf-8") as f:
        return json.load(f)


def score_candidate(report: Dict, baseline: Dict) -> float:
    q = float(report.get("quality_score_mean", 0.0))
    p = float(report.get("precision", 0.0))
    r = float(report.get("recall", 0.0))
    fph = float(report.get("false_positives_per_hour", 0.0))
    xrt = float(report.get("runtime_x_realtime", 0.0))

    bq = float(baseline.get("quality_score_mean", 0.0))
    bp = float(baseline.get("precision", 0.0))
    br = float(baseline.get("recall", 0.0))
    bfph = float(baseline.get("false_positives_per_hour", 0.0))

    if p + 0.005 < bp:
        return -1e9
    if r + 0.01 < br:
        return -1e9
    if fph > max(bfph * 1.10, bfph + 0.2):
        return -1e9

    return (
        (q - bq) * 3.0
        + (p - bp) * 200.0
        + (r - br) * 120.0
        - (fph - bfph) * 25.0
        - xrt * 0.1
    )


def build_grid() -> List[Dict[str, float]]:
    sigma_dot_values = [0.35, 0.40, 0.45]
    sigma_dash_values = [0.65, 0.75, 0.85]
    sigma_intra_values = [0.40, 0.45, 0.55]
    sigma_char_values = [0.80, 0.90, 1.05]
    sigma_word_values = [1.35, 1.60, 1.90]
    on_char_values = [0.20, 0.25, 0.30]
    on_word_values = [0.03, 0.05, 0.07]
    off_dash_values = [0.20, 0.25, 0.30]

    grid: List[Dict[str, float]] = []
    for sd, sda, si, sc, sw, oc, ow, od in itertools.product(
        sigma_dot_values,
        sigma_dash_values,
        sigma_intra_values,
        sigma_char_values,
        sigma_word_values,
        on_char_values,
        on_word_values,
        off_dash_values,
    ):
        on_intra = 1.0 - oc - ow
        off_dot = 1.0 - od
        if on_intra <= 0.05 or off_dot <= 0.05:
            continue
        grid.append(
            {
                "hmm-sigma-dot": sd,
                "hmm-sigma-dash": sda,
                "hmm-sigma-intra": si,
                "hmm-sigma-char": sc,
                "hmm-sigma-word": sw,
                "hmm-on-intra": on_intra,
                "hmm-on-char": oc,
                "hmm-on-word": ow,
                "hmm-off-dot": off_dot,
                "hmm-off-dash": od,
            }
        )
    return grid


def main() -> int:
    parser = argparse.ArgumentParser(description="Rapid HMM calibration over phase0 benchmark dataset")
    parser.add_argument("--decoder", required=True)
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--base-config", required=True)
    parser.add_argument("--msys-bash", default="")
    parser.add_argument("--max-candidates", type=int, default=18)
    parser.add_argument("--only-annotated", action="store_true")
    parser.add_argument("--max-files", type=int, default=0)
    parser.add_argument("--results-csv", default="")
    parser.add_argument("--best-config-out", required=True)
    parser.add_argument("--no-history", action="store_true")
    args = parser.parse_args()

    decoder = Path(args.decoder)
    manifest_path = Path(args.manifest)
    base_config_path = Path(args.base_config)
    msys_bash = Path(args.msys_bash) if args.msys_bash else None
    best_config_out = Path(args.best_config_out)
    results_csv = Path(args.results_csv) if args.results_csv else None

    with base_config_path.open("r", encoding="utf-8") as f:
        base_cfg = json.load(f)
    with manifest_path.open("r", encoding="utf-8") as f:
        manifest = json.load(f)

    with tempfile.TemporaryDirectory(prefix="hmm_cal_") as td:
        tmp_root = Path(td)

        run_manifest = manifest
        if args.only_annotated:
            run_manifest = dict(manifest)
            run_manifest["files"] = [x for x in manifest.get("files", []) if x.get("annotations")]
        if args.max_files > 0:
            run_manifest = dict(run_manifest)
            run_manifest["files"] = list(run_manifest.get("files", []))[: args.max_files]
        manifest_used_path = tmp_root / "manifest_used.json"
        with manifest_used_path.open("w", encoding="utf-8") as f:
            json.dump(run_manifest, f, indent=2)

        baseline_cfg = tmp_root / "baseline_config.json"
        with baseline_cfg.open("w", encoding="utf-8") as f:
            json.dump(base_cfg, f, indent=2)

        baseline_out = tmp_root / "baseline"
        baseline = run_benchmark(
            decoder,
            manifest_used_path,
            baseline_cfg,
            baseline_out,
            msys_bash,
            args.no_history,
        )

        grid = build_grid()
        grid = grid[: max(1, args.max_candidates)]

        rows = []
        best_score = -1e30
        best_report = baseline
        best_overrides = {}

        for i, overrides in enumerate(grid, start=1):
            cfg = json.loads(json.dumps(base_cfg))
            opts = cfg.setdefault("decoder_options", {})
            for k, v in overrides.items():
                opts[k] = v

            cfg_path = tmp_root / f"cfg_{i:03d}.json"
            out_dir = tmp_root / f"run_{i:03d}"
            with cfg_path.open("w", encoding="utf-8") as f:
                json.dump(cfg, f, indent=2)

            report = run_benchmark(
                decoder,
                manifest_used_path,
                cfg_path,
                out_dir,
                msys_bash,
                args.no_history,
            )
            cand_score = score_candidate(report, baseline)

            row = {
                "idx": i,
                "score": cand_score,
                "precision": report.get("precision", 0.0),
                "recall": report.get("recall", 0.0),
                "false_positives_per_hour": report.get("false_positives_per_hour", 0.0),
                "quality_score_mean": report.get("quality_score_mean", 0.0),
                "runtime_x_realtime": report.get("runtime_x_realtime", 0.0),
            }
            row.update(overrides)
            rows.append(row)

            if cand_score > best_score:
                best_score = cand_score
                best_report = report
                best_overrides = overrides

        best_cfg = json.loads(json.dumps(base_cfg))
        best_opts = best_cfg.setdefault("decoder_options", {})
        for k, v in best_overrides.items():
            best_opts[k] = v

        best_cfg["name"] = str(best_cfg.get("name", "phase0_frozen_v1")) + "_hmm_calibrated"
        best_cfg["_calibration"] = {
            "baseline": {
                "precision": baseline.get("precision", 0.0),
                "recall": baseline.get("recall", 0.0),
                "false_positives_per_hour": baseline.get("false_positives_per_hour", 0.0),
                "quality_score_mean": baseline.get("quality_score_mean", 0.0),
            },
            "best": {
                "precision": best_report.get("precision", 0.0),
                "recall": best_report.get("recall", 0.0),
                "false_positives_per_hour": best_report.get("false_positives_per_hour", 0.0),
                "quality_score_mean": best_report.get("quality_score_mean", 0.0),
            },
            "best_score": best_score,
            "tested_candidates": len(grid),
        }

        best_config_out.parent.mkdir(parents=True, exist_ok=True)
        with best_config_out.open("w", encoding="utf-8") as f:
            json.dump(best_cfg, f, indent=2)

        if results_csv:
            results_csv.parent.mkdir(parents=True, exist_ok=True)
            fields = [
                "idx",
                "score",
                "precision",
                "recall",
                "false_positives_per_hour",
                "quality_score_mean",
                "runtime_x_realtime",
                "hmm-on-intra",
                "hmm-on-char",
                "hmm-on-word",
                "hmm-off-dot",
                "hmm-off-dash",
                "hmm-sigma-dot",
                "hmm-sigma-dash",
                "hmm-sigma-intra",
                "hmm-sigma-char",
                "hmm-sigma-word",
            ]
            with results_csv.open("w", encoding="utf-8", newline="") as f:
                w = csv.DictWriter(f, fieldnames=fields)
                w.writeheader()
                for r in rows:
                    w.writerow(r)

        print("HMM calibration completed")
        print(f"- files used={len(run_manifest.get('files', []))}")
        print(f"- baseline quality={baseline.get('quality_score_mean', 0.0):.3f}")
        print(f"- best quality={best_report.get('quality_score_mean', 0.0):.3f}")
        print(f"- baseline precision={baseline.get('precision', 0.0):.4f}")
        print(f"- best precision={best_report.get('precision', 0.0):.4f}")
        print(f"- baseline recall={baseline.get('recall', 0.0):.4f}")
        print(f"- best recall={best_report.get('recall', 0.0):.4f}")
        print(f"- best config: {best_config_out}")
        if results_csv:
            print(f"- results csv: {results_csv}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
import argparse
import csv
import itertools
import json
import subprocess
import tempfile
from pathlib import Path
from typing import Dict, List, Tuple


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
    cp = subprocess.run(cmd, check=False, capture_output=True, text=True)
    if cp.returncode != 0:
        raise RuntimeError(
            "benchmark_phase0 failed\n"
            f"cmd={' '.join(cmd)}\n"
            f"stdout={cp.stdout}\n"
            f"stderr={cp.stderr}"
        )
    with (out_dir / "benchmark_report.json").open("r", encoding="utf-8") as f:
        return json.load(f)


def score(report: Dict, baseline: Dict) -> float:
    q = float(report.get("quality_score_mean", 0.0))
    p = float(report.get("precision", 0.0))
    r = float(report.get("recall", 0.0))
    fph = float(report.get("false_positives_per_hour", 0.0))

    bp = float(baseline.get("precision", 0.0))
    br = float(baseline.get("recall", 0.0))
    bfph = float(baseline.get("false_positives_per_hour", 0.0))

    if p + 0.005 < bp:
        return -1e9
    if r + 0.01 < br:
        return -1e9
    if fph > max(bfph * 1.12, bfph + 0.25):
        return -1e9

    return q * 2.0 + p * 140.0 + r * 100.0 - fph * 0.02


def hsmm_grid() -> List[Dict[str, float]]:
    on_char = [0.18, 0.20, 0.22]
    on_word = [0.06, 0.08, 0.10]
    off_dash = [0.16, 0.18, 0.22]
    s_dot = [0.34, 0.38, 0.44]
    s_dash = [0.65, 0.72, 0.82]
    s_intra = [0.38, 0.42, 0.50]
    s_char = [0.78, 0.85, 0.95]
    s_word = [1.30, 1.45, 1.65]
    tail_mix = [0.12, 0.18, 0.24]
    time_gain = [0.40, 0.55, 0.75]

    out: List[Dict[str, float]] = []
    for oc, ow, od, sd, sda, si, sc, sw, tm, tg in itertools.product(
        on_char,
        on_word,
        off_dash,
        s_dot,
        s_dash,
        s_intra,
        s_char,
        s_word,
        tail_mix,
        time_gain,
    ):
        oi = 1.0 - oc - ow
        odot = 1.0 - od
        if oi <= 0.05 or odot <= 0.05:
            continue
        out.append(
            {
                "hsmm-on-intra": oi,
                "hsmm-on-char": oc,
                "hsmm-on-word": ow,
                "hsmm-off-dot": odot,
                "hsmm-off-dash": od,
                "hsmm-sigma-dot": sd,
                "hsmm-sigma-dash": sda,
                "hsmm-sigma-intra": si,
                "hsmm-sigma-char": sc,
                "hsmm-sigma-word": sw,
                "hsmm-tail-mix": tm,
                "hsmm-time-gain": tg,
            }
        )
    return out


def apply_overrides(base_cfg: Dict, model: str, overrides: Dict[str, float]) -> Dict:
    cfg = json.loads(json.dumps(base_cfg))
    opts = cfg.setdefault("decoder_options", {})
    opts["decoder-model"] = model
    for k, v in overrides.items():
        opts[k] = v
    return cfg


def write_json(path: Path, data: Dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)


def build_manifest_subset(manifest: Dict, only_annotated: bool, max_files: int) -> Dict:
    run_manifest = manifest
    if only_annotated:
        run_manifest = dict(run_manifest)
        run_manifest["files"] = [x for x in run_manifest.get("files", []) if x.get("annotations")]
    if max_files > 0:
        run_manifest = dict(run_manifest)
        run_manifest["files"] = list(run_manifest.get("files", []))[:max_files]
    return run_manifest


def main() -> int:
    p = argparse.ArgumentParser(description="Tune and compare HMM/HSMM/Auto models for Phase0")
    p.add_argument("--decoder", required=True)
    p.add_argument("--manifest", required=True)
    p.add_argument("--base-config", required=True)
    p.add_argument("--msys-bash", default="")
    p.add_argument("--max-hsmm-candidates", type=int, default=16)
    p.add_argument("--only-annotated", action="store_true")
    p.add_argument("--max-files", type=int, default=0)
    p.add_argument("--results-csv", required=True)
    p.add_argument("--best-hsmm-config", required=True)
    p.add_argument("--best-auto-config", required=True)
    p.add_argument("--no-history", action="store_true")
    args = p.parse_args()

    decoder = Path(args.decoder)
    manifest_path = Path(args.manifest)
    base_config_path = Path(args.base_config)
    msys_bash = Path(args.msys_bash) if args.msys_bash else None

    with base_config_path.open("r", encoding="utf-8") as f:
        base_cfg = json.load(f)
    with manifest_path.open("r", encoding="utf-8") as f:
        manifest = json.load(f)

    with tempfile.TemporaryDirectory(prefix="seq_models_") as td:
        root = Path(td)
        run_manifest = build_manifest_subset(manifest, args.only_annotated, args.max_files)
        man_used = root / "manifest_used.json"
        write_json(man_used, run_manifest)

        rows: List[Dict] = []

        cfg_hmm = apply_overrides(base_cfg, "hmm", {})
        path_hmm = root / "cfg_hmm.json"
        out_hmm = root / "run_hmm"
        write_json(path_hmm, cfg_hmm)
        rep_hmm = run_benchmark(decoder, man_used, path_hmm, out_hmm, msys_bash, args.no_history)
        baseline = rep_hmm
        rows.append({"model": "hmm", "candidate": "baseline", **{k: rep_hmm.get(k, 0.0) for k in ["precision", "recall", "false_positives_per_hour", "quality_score_mean", "runtime_x_realtime"]}, "score": score(rep_hmm, baseline)})

        best_hsmm_score = -1e30
        best_hsmm_overrides: Dict[str, float] = {}
        best_hsmm_report = rep_hmm
        grid = hsmm_grid()[: max(1, args.max_hsmm_candidates)]

        for i, ov in enumerate(grid, start=1):
            cfg = apply_overrides(base_cfg, "hsmm", ov)
            cfg_path = root / f"cfg_hsmm_{i:03d}.json"
            out_dir = root / f"run_hsmm_{i:03d}"
            write_json(cfg_path, cfg)
            rep = run_benchmark(decoder, man_used, cfg_path, out_dir, msys_bash, args.no_history)
            sc = score(rep, baseline)
            row = {
                "model": "hsmm",
                "candidate": i,
                "precision": rep.get("precision", 0.0),
                "recall": rep.get("recall", 0.0),
                "false_positives_per_hour": rep.get("false_positives_per_hour", 0.0),
                "quality_score_mean": rep.get("quality_score_mean", 0.0),
                "runtime_x_realtime": rep.get("runtime_x_realtime", 0.0),
                "score": sc,
            }
            row.update(ov)
            rows.append(row)
            if sc > best_hsmm_score:
                best_hsmm_score = sc
                best_hsmm_overrides = ov
                best_hsmm_report = rep

        best_hsmm_cfg = apply_overrides(base_cfg, "hsmm", best_hsmm_overrides)
        best_hsmm_cfg["name"] = str(best_hsmm_cfg.get("name", "phase0")) + "_hsmm_tuned"
        best_hsmm_cfg["_tuning"] = {
            "baseline_hmm": {
                "precision": rep_hmm.get("precision", 0.0),
                "recall": rep_hmm.get("recall", 0.0),
                "false_positives_per_hour": rep_hmm.get("false_positives_per_hour", 0.0),
                "quality_score_mean": rep_hmm.get("quality_score_mean", 0.0),
            },
            "best_hsmm": {
                "precision": best_hsmm_report.get("precision", 0.0),
                "recall": best_hsmm_report.get("recall", 0.0),
                "false_positives_per_hour": best_hsmm_report.get("false_positives_per_hour", 0.0),
                "quality_score_mean": best_hsmm_report.get("quality_score_mean", 0.0),
            },
            "tested_hsmm_candidates": len(grid),
        }
        write_json(Path(args.best_hsmm_config), best_hsmm_cfg)

        auto_cfg = apply_overrides(base_cfg, "auto", best_hsmm_overrides)
        auto_cfg["name"] = str(auto_cfg.get("name", "phase0")) + "_auto_tuned"
        auto_path = root / "cfg_auto.json"
        auto_out = root / "run_auto"
        write_json(auto_path, auto_cfg)
        rep_auto = run_benchmark(decoder, man_used, auto_path, auto_out, msys_bash, args.no_history)
        rows.append({"model": "auto", "candidate": "best_hsmm_overrides", **{k: rep_auto.get(k, 0.0) for k in ["precision", "recall", "false_positives_per_hour", "quality_score_mean", "runtime_x_realtime"]}, "score": score(rep_auto, baseline)})

        auto_cfg["_tuning"] = {
            "baseline_hmm": {
                "precision": rep_hmm.get("precision", 0.0),
                "recall": rep_hmm.get("recall", 0.0),
                "false_positives_per_hour": rep_hmm.get("false_positives_per_hour", 0.0),
                "quality_score_mean": rep_hmm.get("quality_score_mean", 0.0),
            },
            "auto": {
                "precision": rep_auto.get("precision", 0.0),
                "recall": rep_auto.get("recall", 0.0),
                "false_positives_per_hour": rep_auto.get("false_positives_per_hour", 0.0),
                "quality_score_mean": rep_auto.get("quality_score_mean", 0.0),
            },
            "hsmm_overrides_source": str(Path(args.best_hsmm_config)),
        }
        write_json(Path(args.best_auto_config), auto_cfg)

        fields = [
            "model",
            "candidate",
            "score",
            "precision",
            "recall",
            "false_positives_per_hour",
            "quality_score_mean",
            "runtime_x_realtime",
            "hsmm-on-intra",
            "hsmm-on-char",
            "hsmm-on-word",
            "hsmm-off-dot",
            "hsmm-off-dash",
            "hsmm-sigma-dot",
            "hsmm-sigma-dash",
            "hsmm-sigma-intra",
            "hsmm-sigma-char",
            "hsmm-sigma-word",
            "hsmm-tail-mix",
            "hsmm-time-gain",
        ]
        results_csv = Path(args.results_csv)
        results_csv.parent.mkdir(parents=True, exist_ok=True)
        with results_csv.open("w", encoding="utf-8", newline="") as f:
            w = csv.DictWriter(f, fieldnames=fields)
            w.writeheader()
            for r in rows:
                w.writerow(r)

    print("Sequence model tuning completed")
    print(f"- files used={len(run_manifest.get('files', []))}")
    print(f"- baseline hmm quality={rep_hmm.get('quality_score_mean', 0.0):.3f}")
    print(f"- best hsmm quality={best_hsmm_report.get('quality_score_mean', 0.0):.3f}")
    print(f"- auto quality={rep_auto.get('quality_score_mean', 0.0):.3f}")
    print(f"- best hsmm config: {args.best_hsmm_config}")
    print(f"- best auto config: {args.best_auto_config}")
    print(f"- results csv: {args.results_csv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
import argparse
import csv
import json
import re
import shlex
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple


ID_TOKEN_RE = re.compile(r"[A-Z]{2,3}")


@dataclass
class Annotation:
    ann_id: str
    freq_hz: float
    start_sec: float
    end_sec: float
    snr_db_est: Optional[float]


@dataclass
class Prediction:
    pred_id: str
    freq_hz: float
    start_sec: float
    end_sec: float
    confidence: float
    row_key: str


def load_json(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def parse_annotations(file_item: dict) -> List[Annotation]:
    out: List[Annotation] = []
    for a in file_item.get("annotations", []):
        try:
            out.append(
                Annotation(
                    ann_id=str(a["id"]).upper(),
                    freq_hz=float(a["freq_hz"]),
                    start_sec=float(a["start_sec"]),
                    end_sec=float(a["end_sec"]),
                    snr_db_est=None if a.get("snr_db_est") is None else float(a.get("snr_db_est")),
                )
            )
        except Exception:
            continue
    return out


def parse_predictions(csv_path: Path) -> List[Prediction]:
    preds: List[Prediction] = []
    if not csv_path.exists():
        return preds
    with csv_path.open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            text = (row.get("text") or "").upper()
            tokens = ID_TOKEN_RE.findall(text)
            if not tokens:
                continue
            try:
                freq_hz = float(row.get("freq_hz", 0.0))
                start_sec = float(row.get("start_sec", 0.0))
                end_sec = float(row.get("end_sec", 0.0))
                conf = float(row.get("confidence", 0.0))
            except Exception:
                continue
            row_key = f"{row.get('track_id','')}|{freq_hz:.3f}|{start_sec:.3f}|{end_sec:.3f}"
            unique_tokens = sorted(set(tokens))
            for tok in unique_tokens:
                preds.append(
                    Prediction(
                        pred_id=tok,
                        freq_hz=freq_hz,
                        start_sec=start_sec,
                        end_sec=end_sec,
                        confidence=conf,
                        row_key=f"{row_key}|{tok}",
                    )
                )
    return preds


def overlap_seconds(a0: float, a1: float, b0: float, b1: float) -> float:
    return max(0.0, min(a1, b1) - max(a0, b0))


def match_predictions(
    annotations: List[Annotation],
    predictions: List[Prediction],
    freq_tol_hz: float,
    min_overlap_sec: float,
) -> Tuple[int, int, int, List[float], List[float]]:
    used = set()
    tp = 0
    fn = 0
    latencies: List[float] = []
    confs: List[float] = []

    for ann in annotations:
        best_idx = None
        best_score = None
        for i, pred in enumerate(predictions):
            if i in used:
                continue
            if pred.pred_id != ann.ann_id:
                continue
            if abs(pred.freq_hz - ann.freq_hz) > freq_tol_hz:
                continue
            ov = overlap_seconds(ann.start_sec, ann.end_sec, pred.start_sec, pred.end_sec)
            if ov < min_overlap_sec:
                continue
            score = (abs(pred.freq_hz - ann.freq_hz), -ov, abs(pred.start_sec - ann.start_sec))
            if best_score is None or score < best_score:
                best_score = score
                best_idx = i
        if best_idx is None:
            fn += 1
        else:
            used.add(best_idx)
            tp += 1
            latencies.append(predictions[best_idx].start_sec - ann.start_sec)
            confs.append(predictions[best_idx].confidence)

    fp = sum(1 for i in range(len(predictions)) if i not in used)
    return tp, fp, fn, latencies, confs


def to_msys_path(path: Path) -> str:
    s = str(path)
    if len(s) >= 3 and s[1] == ":" and (s[2] == "\\" or s[2] == "/"):
        drive = s[0].lower()
        rest = s[2:].replace("\\", "/")
        return f"/{drive}{rest}"
    return s.replace("\\", "/")


def run_decoder_native(
    decoder: Path, wav_path: Path, csv_out: Path, metrics_out: Path, options: List[str]
) -> float:
    cmd = [str(decoder), str(wav_path), str(csv_out), "--metrics", str(metrics_out), "--no-progress", "--quiet"]
    cmd.extend(options)
    t0 = time.perf_counter()
    cp = subprocess.run(cmd, check=False, capture_output=True, text=True)
    t1 = time.perf_counter()
    if cp.returncode != 0:
        raise RuntimeError(
            "Decoder failed (native)\n"
            f"returncode={cp.returncode}\n"
            f"cmd={' '.join(cmd)}\n"
            f"stdout={cp.stdout}\n"
            f"stderr={cp.stderr}"
        )
    return t1 - t0


def run_decoder_msys_ucrt64(
    msys_bash: Path, decoder: Path, wav_path: Path, csv_out: Path, metrics_out: Path, options: List[str]
) -> float:
    cmd_parts = [
        shlex.quote(to_msys_path(decoder)),
        shlex.quote(to_msys_path(wav_path)),
        shlex.quote(to_msys_path(csv_out)),
        "--metrics",
        shlex.quote(to_msys_path(metrics_out)),
        "--no-progress",
        "--quiet",
    ]
    for o in options:
        cmd_parts.append(shlex.quote(str(o)))

    shell_cmd = "export MSYSTEM=UCRT64; source /etc/profile; " + " ".join(cmd_parts)
    t0 = time.perf_counter()
    cp = subprocess.run([str(msys_bash), "-lc", shell_cmd], check=False, capture_output=True, text=True)
    t1 = time.perf_counter()
    if cp.returncode != 0:
        raise RuntimeError(
            "Decoder failed (MSYS2 UCRT64 wrapper)\n"
            f"returncode={cp.returncode}\n"
            f"cmd={shell_cmd}\n"
            f"stdout={cp.stdout}\n"
            f"stderr={cp.stderr}"
        )
    return t1 - t0


def safe_div(a: float, b: float) -> float:
    return a / b if b else 0.0


def main() -> int:
    p = argparse.ArgumentParser(description="Phase 0 benchmark for JNDB")
    p.add_argument("--decoder", required=True, help="Path to ndb_decode executable")
    p.add_argument("--manifest", required=True, help="Path to gold dataset manifest JSON")
    p.add_argument("--config", required=True, help="Frozen benchmark config JSON")
    p.add_argument("--out-dir", required=True, help="Output directory for benchmark artifacts")
    p.add_argument("--run", action="store_true", help="Run decoder for each file")
    p.add_argument(
        "--msys-bash",
        default="",
        help="Optional path to MSYS2 bash.exe. If set, decoder is executed via UCRT64 shell.",
    )
    args = p.parse_args()

    decoder = Path(args.decoder)
    manifest_path = Path(args.manifest)
    config_path = Path(args.config)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    manifest = load_json(manifest_path)
    cfg = load_json(config_path)
    msys_bash = Path(args.msys_bash) if args.msys_bash else None
    freq_tol_hz = float(cfg.get("match", {}).get("freq_tolerance_hz", 2.0))
    min_overlap_sec = float(cfg.get("match", {}).get("min_time_overlap_sec", 0.2))

    cli_opts = []
    for k, v in (cfg.get("decoder_options") or {}).items():
        cli_opts += [f"--{k}", str(v)]

    per_file_rows: List[Dict[str, object]] = []
    total_tp = total_fp = total_fn = 0
    total_runtime = 0.0
    total_audio_sec = 0.0
    all_latencies: List[float] = []
    quality_scores: List[float] = []
    missing_files = 0

    for item in manifest.get("files", []):
        file_id = str(item.get("file_id", ""))
        wav_path = Path(item.get("wav_path", ""))
        annotations = parse_annotations(item)

        if not wav_path.exists():
            missing_files += 1
            per_file_rows.append(
                {
                    "file_id": file_id,
                    "wav_path": str(wav_path),
                    "status": "missing",
                    "tp": 0,
                    "fp": 0,
                    "fn": len(annotations),
                    "precision": 0.0,
                    "recall": 0.0,
                    "false_positives_per_hour": 0.0,
                    "id_latency_sec": 0.0,
                    "runtime_x_realtime": 0.0,
                    "quality_score": 0.0,
                }
            )
            total_fn += len(annotations)
            continue

        pred_csv = out_dir / f"{file_id}_pred.csv"
        metrics_json = out_dir / f"{file_id}_metrics.json"
        runtime_sec = 0.0
        if args.run:
            if msys_bash is not None:
                runtime_sec = run_decoder_msys_ucrt64(msys_bash, decoder, wav_path, pred_csv, metrics_json, cli_opts)
            else:
                runtime_sec = run_decoder_native(decoder, wav_path, pred_csv, metrics_json, cli_opts)

        predictions = parse_predictions(pred_csv)
        tp, fp, fn, latencies, _confs = match_predictions(
            annotations, predictions, freq_tol_hz=freq_tol_hz, min_overlap_sec=min_overlap_sec
        )

        audio_sec = 0.0
        quality_score = 0.0
        if metrics_json.exists():
            m = load_json(metrics_json)
            sr = float(m.get("input_sample_rate", 0.0))
            n = float(m.get("input_samples", 0.0))
            audio_sec = safe_div(n, sr)
            quality_score = float(m.get("quality_score", 0.0))
            quality_scores.append(quality_score)
        if audio_sec <= 0.0:
            audio_sec = float(item.get("duration_sec", 0.0) or 0.0)

        p_val = safe_div(tp, tp + fp)
        r_val = safe_div(tp, tp + fn)
        fph = safe_div(fp * 3600.0, audio_sec)
        lat = safe_div(sum(latencies), len(latencies)) if latencies else 0.0
        xrt = safe_div(runtime_sec, audio_sec)

        per_file_rows.append(
            {
                "file_id": file_id,
                "wav_path": str(wav_path),
                "status": "ok",
                "tp": tp,
                "fp": fp,
                "fn": fn,
                "precision": p_val,
                "recall": r_val,
                "false_positives_per_hour": fph,
                "id_latency_sec": lat,
                "runtime_x_realtime": xrt,
                "quality_score": quality_score,
            }
        )

        total_tp += tp
        total_fp += fp
        total_fn += fn
        total_runtime += runtime_sec
        total_audio_sec += audio_sec
        all_latencies.extend(latencies)

    global_precision = safe_div(total_tp, total_tp + total_fp)
    global_recall = safe_div(total_tp, total_tp + total_fn)
    global_fph = safe_div(total_fp * 3600.0, total_audio_sec)
    global_latency = safe_div(sum(all_latencies), len(all_latencies)) if all_latencies else 0.0
    global_xrt = safe_div(total_runtime, total_audio_sec)
    mean_quality = safe_div(sum(quality_scores), len(quality_scores)) if quality_scores else 0.0

    summary = {
        "dataset_name": manifest.get("dataset_name", "unknown"),
        "files_total": len(manifest.get("files", [])),
        "files_missing": missing_files,
        "files_evaluated": len(manifest.get("files", [])) - missing_files,
        "files_with_annotations": sum(1 for x in manifest.get("files", []) if x.get("annotations")),
        "precision": global_precision,
        "recall": global_recall,
        "false_positives_per_hour": global_fph,
        "id_latency_sec": global_latency,
        "runtime_x_realtime": global_xrt,
        "quality_score_mean": mean_quality,
        "tp": total_tp,
        "fp": total_fp,
        "fn": total_fn,
        "total_audio_sec": total_audio_sec,
        "total_runtime_sec": total_runtime,
        "frozen_config": cfg,
    }

    report_json = out_dir / "benchmark_report.json"
    with report_json.open("w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2)

    per_file_csv = out_dir / "benchmark_per_file.csv"
    headers = [
        "file_id",
        "wav_path",
        "status",
        "tp",
        "fp",
        "fn",
        "precision",
        "recall",
        "false_positives_per_hour",
        "id_latency_sec",
        "runtime_x_realtime",
        "quality_score",
    ]
    with per_file_csv.open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=headers)
        w.writeheader()
        for r in per_file_rows:
            w.writerow(r)

    summary_csv = out_dir / "benchmark_summary.csv"
    with summary_csv.open("w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(["metric", "value"])
        for k in [
            "precision",
            "recall",
            "false_positives_per_hour",
            "id_latency_sec",
            "runtime_x_realtime",
            "quality_score_mean",
            "tp",
            "fp",
            "fn",
            "files_total",
            "files_missing",
            "files_evaluated",
        ]:
            w.writerow([k, summary[k]])

    print("Benchmark completed")
    print(f"- report: {report_json}")
    print(f"- summary_csv: {summary_csv}")
    print(f"- per_file_csv: {per_file_csv}")
    print(
        f"- precision={global_precision:.4f} recall={global_recall:.4f} "
        f"FP/h={global_fph:.3f} latency={global_latency:.3f}s xRT={global_xrt:.3f} "
        f"quality_mean={mean_quality:.3f}"
    )
    if summary["files_with_annotations"] == 0:
        print("- warning: no annotations present in manifest; precision/recall are not meaningful yet")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

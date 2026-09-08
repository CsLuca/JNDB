#!/usr/bin/env python3
import argparse
import csv
import json
import re
import shlex
import subprocess
import time
from collections import defaultdict
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, List, Optional, Tuple


ID_TOKEN_RE = re.compile(r"[A-Z]{2,3}")
MISS_LABEL = "__MISS__"
NONE_LABEL = "__NONE__"


@dataclass
class Beacon:
    beacon_id: str
    freq_hz: float
    area: str


@dataclass
class Annotation:
    ann_id: str
    freq_hz: float
    start_sec: float
    end_sec: float


@dataclass
class Prediction:
    pred_id: str
    freq_hz: float
    start_sec: float
    end_sec: float
    confidence: float


def load_json(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def safe_div(a: float, b: float) -> float:
    return a / b if b else 0.0


def git_value(args: List[str]) -> str:
    try:
        cp = subprocess.run(args, check=False, capture_output=True, text=True)
        if cp.returncode == 0:
            return cp.stdout.strip()
    except Exception:
        pass
    return ""


def to_msys_path(path: Path) -> str:
    s = str(path)
    if len(s) >= 3 and s[1] == ":" and (s[2] == "\\" or s[2] == "/"):
        drive = s[0].lower()
        rest = s[2:].replace("\\", "/")
        return f"/{drive}{rest}"
    return s.replace("\\", "/")


def parse_decoder_options(config_path: Optional[Path]) -> List[str]:
    if config_path is None:
        return []
    cfg = load_json(config_path)
    opts = []
    for k, v in (cfg.get("decoder_options") or {}).items():
        opts += [f"--{k}", str(v)]
    return opts


def load_beacon_db(path: Path) -> Dict[str, Beacon]:
    out: Dict[str, Beacon] = {}
    with path.open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            bid = (row.get("id") or "").strip().upper()
            if not bid:
                continue
            try:
                freq = float(row.get("freq_hz") or 0.0)
            except Exception:
                continue
            area = (row.get("area") or "").strip()
            out[bid] = Beacon(beacon_id=bid, freq_hz=freq, area=area)
    return out


def parse_annotations(file_item: dict) -> List[Annotation]:
    out: List[Annotation] = []
    for a in file_item.get("annotations", []):
        try:
            out.append(
                Annotation(
                    ann_id=str(a.get("id", "")).upper(),
                    freq_hz=float(a.get("freq_hz", 0.0)),
                    start_sec=float(a.get("start_sec", 0.0)),
                    end_sec=float(a.get("end_sec", 0.0)),
                )
            )
        except Exception:
            continue
    return out


def parse_predictions(csv_path: Path, min_confidence: float) -> List[Prediction]:
    preds: List[Prediction] = []
    if not csv_path.exists():
        return preds
    with csv_path.open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                freq_hz = float(row.get("freq_hz", 0.0))
                start_sec = float(row.get("start_sec", 0.0))
                end_sec = float(row.get("end_sec", 0.0))
                conf = float(row.get("confidence", 0.0))
            except Exception:
                continue
            if conf < min_confidence:
                continue
            plausible = (row.get("plausible_id") or "").upper().strip()
            tokens = []
            if plausible and ID_TOKEN_RE.fullmatch(plausible):
                tokens = [plausible]
            else:
                text = (row.get("text") or "").upper()
                tokens = sorted(set(ID_TOKEN_RE.findall(text)))
            for tok in tokens:
                preds.append(
                    Prediction(
                        pred_id=tok,
                        freq_hz=freq_hz,
                        start_sec=start_sec,
                        end_sec=end_sec,
                        confidence=conf,
                    )
                )
    return preds


def overlap_seconds(a0: float, a1: float, b0: float, b1: float) -> float:
    return max(0.0, min(a1, b1) - max(a0, b0))


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


def median_freq(preds: List[Prediction], pred_id: str) -> Optional[float]:
    vals = sorted([p.freq_hz for p in preds if p.pred_id == pred_id])
    if not vals:
        return None
    m = len(vals) // 2
    return vals[m]


def eval_known_list(
    expected_ids: List[str],
    predictions: List[Prediction],
    beacon_db: Dict[str, Beacon],
    id_freq_tolerance_hz: float,
) -> Tuple[List[Tuple[str, str]], List[str], List[str]]:
    expected = sorted(set([x.upper() for x in expected_ids if x]))
    detected = sorted(set([p.pred_id for p in predictions]))

    pairs: List[Tuple[str, str]] = []
    rem_expected = set(expected)
    rem_detected = set(detected)

    for bid in expected:
        if bid in rem_detected:
            pairs.append((bid, bid))
            rem_expected.discard(bid)
            rem_detected.discard(bid)

    det_freq = {d: median_freq(predictions, d) for d in rem_detected}
    for det in sorted(list(rem_detected)):
        best_exp = None
        best_df = None
        f_det = det_freq.get(det)
        if f_det is None:
            continue
        for exp in sorted(list(rem_expected)):
            b = beacon_db.get(exp)
            if not b:
                continue
            df = abs(f_det - b.freq_hz)
            if df > id_freq_tolerance_hz:
                continue
            if best_df is None or df < best_df:
                best_df = df
                best_exp = exp
        if best_exp is not None:
            pairs.append((best_exp, det))
            rem_expected.discard(best_exp)
            rem_detected.discard(det)

    misses = sorted(list(rem_expected))
    false_alarms = sorted(list(rem_detected))
    return pairs, misses, false_alarms


def eval_blind(
    annotations: List[Annotation],
    predictions: List[Prediction],
    freq_tolerance_hz: float,
    min_overlap_sec: float,
) -> Tuple[List[Tuple[str, str]], List[str], List[str]]:
    used = set()
    pairs: List[Tuple[str, str]] = []
    misses: List[str] = []

    for ann in annotations:
        best_idx = None
        best_score = None
        for i, pred in enumerate(predictions):
            if i in used:
                continue
            if abs(pred.freq_hz - ann.freq_hz) > freq_tolerance_hz:
                continue
            ov = overlap_seconds(ann.start_sec, ann.end_sec, pred.start_sec, pred.end_sec)
            if ov < min_overlap_sec:
                continue
            score = (abs(pred.freq_hz - ann.freq_hz), -ov, abs(pred.start_sec - ann.start_sec))
            if best_score is None or score < best_score:
                best_score = score
                best_idx = i
        if best_idx is None:
            misses.append(ann.ann_id)
        else:
            used.add(best_idx)
            pairs.append((ann.ann_id, predictions[best_idx].pred_id))

    false_alarms = [predictions[i].pred_id for i in range(len(predictions)) if i not in used]
    return pairs, sorted(misses), sorted(false_alarms)


def add_confusion(conf_map: Dict[str, Dict[str, int]], truth_id: str, pred_id: str) -> None:
    conf_map[truth_id][pred_id] += 1


def serialize_confusion(conf_map: Dict[str, Dict[str, int]]) -> List[Dict[str, object]]:
    rows = []
    for t in sorted(conf_map.keys()):
        for p in sorted(conf_map[t].keys()):
            rows.append({"truth_id": t, "pred_id": p, "count": conf_map[t][p]})
    return rows


def write_csv(path: Path, rows: List[Dict[str, object]], fieldnames: List[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames)
        w.writeheader()
        for r in rows:
            w.writerow(r)


def append_history(history_csv: Path, history_jsonl: Path, record: dict) -> None:
    history_csv.parent.mkdir(parents=True, exist_ok=True)
    history_jsonl.parent.mkdir(parents=True, exist_ok=True)
    fields = [
        "run_id",
        "timestamp_utc",
        "git_commit",
        "git_branch",
        "dataset_name",
        "variant",
        "mode",
        "files_total",
        "files_missing",
        "tp_id",
        "confusion",
        "miss",
        "false_alarm",
        "precision",
        "recall",
        "f1",
        "runtime_x_realtime",
        "total_audio_sec",
        "total_runtime_sec",
    ]
    exists = history_csv.exists()
    with history_csv.open("a", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        if not exists:
            w.writeheader()
        w.writerow({k: record.get(k, "") for k in fields})
    with history_jsonl.open("a", encoding="utf-8") as f:
        f.write(json.dumps(record, ensure_ascii=False) + "\n")


def evaluate_variant(
    label: str,
    decoder: Path,
    options: List[str],
    manifest: dict,
    beacon_db: Dict[str, Beacon],
    mode: str,
    out_root: Path,
    run_decoder: bool,
    msys_bash: Optional[Path],
    min_confidence: float,
    freq_tolerance_hz: float,
    min_overlap_sec: float,
    id_freq_tolerance_hz: float,
) -> dict:
    var_dir = out_root / label
    pred_dir = var_dir / "predictions"
    pred_dir.mkdir(parents=True, exist_ok=True)

    per_file = []
    conf_map: Dict[str, Dict[str, int]] = defaultdict(lambda: defaultdict(int))
    total_tp_id = 0
    total_confusion = 0
    total_miss = 0
    total_false_alarm = 0
    total_runtime = 0.0
    total_audio = 0.0
    missing_files = 0

    for item in manifest.get("files", []):
        file_id = str(item.get("file_id", ""))
        wav_path = Path(item.get("wav_path", ""))
        duration_sec = float(item.get("duration_sec", 0.0) or 0.0)
        pred_csv = pred_dir / f"{file_id}_pred.csv"
        met_json = pred_dir / f"{file_id}_metrics.json"

        if not wav_path.exists():
            missing_files += 1
            per_file.append(
                {
                    "file_id": file_id,
                    "status": "missing",
                    "expected_ids": "",
                    "detected_ids": "",
                    "tp_ids": "",
                    "confusions": "",
                    "misses": "",
                    "false_alarms": "",
                    "precision": 0.0,
                    "recall": 0.0,
                    "f1": 0.0,
                    "runtime_sec": 0.0,
                    "audio_sec": duration_sec,
                }
            )
            continue

        runtime_sec = 0.0
        if run_decoder:
            pred_csv.parent.mkdir(parents=True, exist_ok=True)
            if msys_bash is not None:
                runtime_sec = run_decoder_msys_ucrt64(msys_bash, decoder, wav_path, pred_csv, met_json, options)
            else:
                runtime_sec = run_decoder_native(decoder, wav_path, pred_csv, met_json, options)
        total_runtime += runtime_sec

        audio_sec = duration_sec
        if met_json.exists():
            m = load_json(met_json)
            sr = float(m.get("input_sample_rate", 0.0))
            n = float(m.get("input_samples", 0.0))
            computed = safe_div(n, sr)
            if computed > 0.0:
                audio_sec = computed
        total_audio += audio_sec

        preds = parse_predictions(pred_csv, min_confidence=min_confidence)
        detected_ids = sorted(set([p.pred_id for p in preds]))

        if mode == "known-list":
            exp = [str(x).upper() for x in (item.get("expected_ids") or [])]
            if not exp:
                exp = sorted(set([a.ann_id for a in parse_annotations(item)]))
            pairs, misses, false_alarms = eval_known_list(
                expected_ids=exp,
                predictions=preds,
                beacon_db=beacon_db,
                id_freq_tolerance_hz=id_freq_tolerance_hz,
            )
            truth_total = len(exp)
            pred_total = len(detected_ids)
        else:
            annotations = parse_annotations(item)
            pairs, misses, false_alarms = eval_blind(
                annotations=annotations,
                predictions=preds,
                freq_tolerance_hz=freq_tolerance_hz,
                min_overlap_sec=min_overlap_sec,
            )
            truth_total = len(annotations)
            pred_total = len(pairs) + len(false_alarms)

        tp_ids = [(t, p) for (t, p) in pairs if t == p]
        confusions = [(t, p) for (t, p) in pairs if t != p]

        for t, p in pairs:
            add_confusion(conf_map, t, p)
        for m in misses:
            add_confusion(conf_map, m, MISS_LABEL)
        for fa in false_alarms:
            add_confusion(conf_map, NONE_LABEL, fa)

        tp_cnt = len(tp_ids)
        conf_cnt = len(confusions)
        miss_cnt = len(misses)
        fa_cnt = len(false_alarms)

        precision = safe_div(tp_cnt, pred_total)
        recall = safe_div(tp_cnt, truth_total)
        f1 = safe_div(2.0 * precision * recall, precision + recall)

        total_tp_id += tp_cnt
        total_confusion += conf_cnt
        total_miss += miss_cnt
        total_false_alarm += fa_cnt

        per_file.append(
            {
                "file_id": file_id,
                "status": "ok",
                "expected_ids": "|".join(sorted(set([x[0] for x in pairs] + misses))),
                "detected_ids": "|".join(detected_ids),
                "tp_ids": "|".join(sorted(set([x[0] for x in tp_ids]))),
                "confusions": "|".join([f"{t}->{p}" for (t, p) in confusions]),
                "misses": "|".join(misses),
                "false_alarms": "|".join(false_alarms),
                "precision": precision,
                "recall": recall,
                "f1": f1,
                "runtime_sec": runtime_sec,
                "audio_sec": audio_sec,
            }
        )

    truth_total = total_tp_id + total_confusion + total_miss
    pred_total = total_tp_id + total_confusion + total_false_alarm
    precision = safe_div(total_tp_id, pred_total)
    recall = safe_div(total_tp_id, truth_total)
    f1 = safe_div(2.0 * precision * recall, precision + recall)
    xrt = safe_div(total_runtime, total_audio)

    report = {
        "variant": label,
        "mode": mode,
        "files_total": len(manifest.get("files", [])),
        "files_missing": missing_files,
        "tp_id": total_tp_id,
        "confusion": total_confusion,
        "miss": total_miss,
        "false_alarm": total_false_alarm,
        "precision": precision,
        "recall": recall,
        "f1": f1,
        "runtime_x_realtime": xrt,
        "total_audio_sec": total_audio,
        "total_runtime_sec": total_runtime,
    }

    write_csv(
        var_dir / "diff_report.csv",
        per_file,
        fieldnames=[
            "file_id",
            "status",
            "expected_ids",
            "detected_ids",
            "tp_ids",
            "confusions",
            "misses",
            "false_alarms",
            "precision",
            "recall",
            "f1",
            "runtime_sec",
            "audio_sec",
        ],
    )
    conf_rows = serialize_confusion(conf_map)
    write_csv(var_dir / "confusion_matrix.csv", conf_rows, fieldnames=["truth_id", "pred_id", "count"])
    with (var_dir / "validation_report.json").open("w", encoding="utf-8") as f:
        json.dump({"summary": report, "per_file": per_file, "confusion_matrix": conf_rows}, f, indent=2)
    return report


def main() -> int:
    p = argparse.ArgumentParser(description="Phase 5 validation with beacon DB and known-list/blind modes")
    p.add_argument("--manifest", required=True, help="Validation manifest JSON")
    p.add_argument("--beacon-db", required=True, help="Beacon DB CSV: id,freq_hz,area")
    p.add_argument("--mode", choices=["known-list", "blind"], default="known-list")
    p.add_argument("--decoder-a", required=True, help="Decoder A executable")
    p.add_argument("--decoder-b", default="", help="Decoder B executable (optional)")
    p.add_argument("--name-a", default="A", help="Label for variant A")
    p.add_argument("--name-b", default="B", help="Label for variant B")
    p.add_argument("--config-a", default="", help="Config JSON for variant A (decoder_options)")
    p.add_argument("--config-b", default="", help="Config JSON for variant B (decoder_options)")
    p.add_argument("--extra-option-a", action="append", default=[], help="Extra CLI option for variant A")
    p.add_argument("--extra-option-b", action="append", default=[], help="Extra CLI option for variant B")
    p.add_argument("--out-dir", required=True, help="Output directory")
    p.add_argument("--run", action="store_true", help="Run decoder before validation")
    p.add_argument("--msys-bash", default="", help="Optional MSYS2 bash.exe path (UCRT64 wrapper)")
    p.add_argument("--min-confidence", type=float, default=0.0)
    p.add_argument("--freq-tol-hz", type=float, default=2.0)
    p.add_argument("--min-overlap-sec", type=float, default=0.2)
    p.add_argument("--id-freq-tol-hz", type=float, default=3.0)
    p.add_argument("--history-csv", default="", help="Append-only history CSV path")
    p.add_argument("--history-jsonl", default="", help="Append-only history JSONL path")
    p.add_argument("--ab-history-csv", default="", help="Append-only A/B history CSV path")
    p.add_argument("--ab-history-jsonl", default="", help="Append-only A/B history JSONL path")
    p.add_argument("--no-history", action="store_true")
    args = p.parse_args()

    manifest = load_json(Path(args.manifest))
    beacon_db = load_beacon_db(Path(args.beacon_db))
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    msys_bash = Path(args.msys_bash) if args.msys_bash else None

    options_a = parse_decoder_options(Path(args.config_a)) if args.config_a else []
    options_b = parse_decoder_options(Path(args.config_b)) if args.config_b else []
    options_a.extend(args.extra_option_a)
    options_b.extend(args.extra_option_b)

    decoder_a = Path(args.decoder_a).resolve()
    decoder_b = Path(args.decoder_b).resolve() if args.decoder_b else None

    rep_a = evaluate_variant(
        label=args.name_a,
        decoder=decoder_a,
        options=options_a,
        manifest=manifest,
        beacon_db=beacon_db,
        mode=args.mode,
        out_root=out_dir,
        run_decoder=args.run,
        msys_bash=msys_bash,
        min_confidence=args.min_confidence,
        freq_tolerance_hz=args.freq_tol_hz,
        min_overlap_sec=args.min_overlap_sec,
        id_freq_tolerance_hz=args.id_freq_tol_hz,
    )

    rep_b = None
    if decoder_b is not None:
        rep_b = evaluate_variant(
            label=args.name_b,
            decoder=decoder_b,
            options=options_b,
            manifest=manifest,
            beacon_db=beacon_db,
            mode=args.mode,
            out_root=out_dir,
            run_decoder=args.run,
            msys_bash=msys_bash,
            min_confidence=args.min_confidence,
            freq_tolerance_hz=args.freq_tol_hz,
            min_overlap_sec=args.min_overlap_sec,
            id_freq_tolerance_hz=args.id_freq_tol_hz,
        )

    out = {"dataset_name": manifest.get("dataset_name", "unknown"), "mode": args.mode, "A": rep_a}
    if rep_b is not None:
        out["B"] = rep_b
        out["delta_B_minus_A"] = {
            "precision": rep_b["precision"] - rep_a["precision"],
            "recall": rep_b["recall"] - rep_a["recall"],
            "f1": rep_b["f1"] - rep_a["f1"],
            "false_alarm": rep_b["false_alarm"] - rep_a["false_alarm"],
            "miss": rep_b["miss"] - rep_a["miss"],
            "confusion": rep_b["confusion"] - rep_a["confusion"],
            "runtime_x_realtime": rep_b["runtime_x_realtime"] - rep_a["runtime_x_realtime"],
        }

    with (out_dir / "phase5_validation_summary.json").open("w", encoding="utf-8") as f:
        json.dump(out, f, indent=2)

    if not args.no_history:
        ts = datetime.now(timezone.utc)
        run_id = ts.strftime("%Y%m%dT%H%M%SZ")
        timestamp_utc = ts.strftime("%Y-%m-%dT%H:%M:%SZ")
        git_commit = git_value(["git", "rev-parse", "--short", "HEAD"])
        git_branch = git_value(["git", "rev-parse", "--abbrev-ref", "HEAD"])

        def write_variant_history(rep: dict, name: str) -> None:
            default_hist = out_dir / "history"
            h_csv = Path(args.history_csv) if args.history_csv else (default_hist / "validation_history.csv")
            h_jsonl = (
                Path(args.history_jsonl)
                if args.history_jsonl
                else (default_hist / "validation_history.jsonl")
            )
            append_history(
                h_csv,
                h_jsonl,
                {
                    "run_id": run_id,
                    "timestamp_utc": timestamp_utc,
                    "git_commit": git_commit,
                    "git_branch": git_branch,
                    "dataset_name": manifest.get("dataset_name", "unknown"),
                    "variant": name,
                    "mode": args.mode,
                    **rep,
                },
            )

        write_variant_history(rep_a, args.name_a)
        if rep_b is not None:
            write_variant_history(rep_b, args.name_b)

            ab_default_hist = out_dir / "history"
            ab_csv = Path(args.ab_history_csv) if args.ab_history_csv else (ab_default_hist / "ab_history.csv")
            ab_jsonl = (
                Path(args.ab_history_jsonl)
                if args.ab_history_jsonl
                else (ab_default_hist / "ab_history.jsonl")
            )
            ab_fields = [
                "run_id",
                "timestamp_utc",
                "git_commit",
                "git_branch",
                "dataset_name",
                "mode",
                "name_a",
                "name_b",
                "precision_a",
                "precision_b",
                "recall_a",
                "recall_b",
                "f1_a",
                "f1_b",
                "delta_f1",
                "delta_precision",
                "delta_recall",
                "delta_false_alarm",
                "delta_miss",
                "delta_confusion",
                "xrt_a",
                "xrt_b",
                "delta_xrt",
            ]
            ab_row = {
                "run_id": run_id,
                "timestamp_utc": timestamp_utc,
                "git_commit": git_commit,
                "git_branch": git_branch,
                "dataset_name": manifest.get("dataset_name", "unknown"),
                "mode": args.mode,
                "name_a": args.name_a,
                "name_b": args.name_b,
                "precision_a": rep_a["precision"],
                "precision_b": rep_b["precision"],
                "recall_a": rep_a["recall"],
                "recall_b": rep_b["recall"],
                "f1_a": rep_a["f1"],
                "f1_b": rep_b["f1"],
                "delta_f1": rep_b["f1"] - rep_a["f1"],
                "delta_precision": rep_b["precision"] - rep_a["precision"],
                "delta_recall": rep_b["recall"] - rep_a["recall"],
                "delta_false_alarm": rep_b["false_alarm"] - rep_a["false_alarm"],
                "delta_miss": rep_b["miss"] - rep_a["miss"],
                "delta_confusion": rep_b["confusion"] - rep_a["confusion"],
                "xrt_a": rep_a["runtime_x_realtime"],
                "xrt_b": rep_b["runtime_x_realtime"],
                "delta_xrt": rep_b["runtime_x_realtime"] - rep_a["runtime_x_realtime"],
            }
            ab_exists = ab_csv.exists()
            with ab_csv.open("a", encoding="utf-8", newline="") as f:
                w = csv.DictWriter(f, fieldnames=ab_fields)
                if not ab_exists:
                    w.writeheader()
                w.writerow({k: ab_row.get(k, "") for k in ab_fields})
            with ab_jsonl.open("a", encoding="utf-8") as f:
                f.write(json.dumps(ab_row, ensure_ascii=False) + "\n")

    print("Phase 5 validation completed")
    print(f"- summary: {out_dir / 'phase5_validation_summary.json'}")
    print(f"- variant A report: {out_dir / args.name_a / 'validation_report.json'}")
    if rep_b is not None:
        print(f"- variant B report: {out_dir / args.name_b / 'validation_report.json'}")
    print(
        f"- A precision={rep_a['precision']:.4f} recall={rep_a['recall']:.4f} f1={rep_a['f1']:.4f} "
        f"false_alarm={rep_a['false_alarm']} miss={rep_a['miss']} confusion={rep_a['confusion']}"
    )
    if rep_b is not None:
        print(
            f"- B precision={rep_b['precision']:.4f} recall={rep_b['recall']:.4f} f1={rep_b['f1']:.4f} "
            f"false_alarm={rep_b['false_alarm']} miss={rep_b['miss']} confusion={rep_b['confusion']}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

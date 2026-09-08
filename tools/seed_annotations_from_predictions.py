#!/usr/bin/env python3
import argparse
import csv
import json
import re
from pathlib import Path
from typing import Dict, List


ID_TOKEN_RE = re.compile(r"[A-Z]{2,3}")


def load_json(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def save_json(path: Path, obj: dict) -> None:
    with path.open("w", encoding="utf-8") as f:
        json.dump(obj, f, indent=2)


def read_predictions(pred_csv: Path, min_conf: float) -> List[dict]:
    rows: List[dict] = []
    if not pred_csv.exists():
        return rows

    with pred_csv.open("r", encoding="utf-8", newline="") as f:
        r = csv.DictReader(f)
        for row in r:
            text = (row.get("text") or "").upper()
            tokens = sorted(set(ID_TOKEN_RE.findall(text)))
            if not tokens:
                continue
            try:
                conf = float(row.get("confidence", 0.0))
                freq = float(row.get("freq_hz", 0.0))
                start = float(row.get("start_sec", 0.0))
                end = float(row.get("end_sec", 0.0))
            except Exception:
                continue

            if conf < min_conf:
                continue

            for tok in tokens:
                rows.append(
                    {
                        "id": tok,
                        "freq_hz": freq,
                        "start_sec": start,
                        "end_sec": end,
                        "confidence": conf,
                    }
                )
    return rows


def dedupe_candidates(cands: List[dict], freq_tol: float, time_tol: float) -> List[dict]:
    cands = sorted(cands, key=lambda x: (-x["confidence"], x["id"], x["freq_hz"], x["start_sec"]))
    out: List[dict] = []
    for c in cands:
        duplicate = False
        for e in out:
            if c["id"] != e["id"]:
                continue
            if abs(c["freq_hz"] - e["freq_hz"]) > freq_tol:
                continue
            if abs(c["start_sec"] - e["start_sec"]) > time_tol:
                continue
            duplicate = True
            break
        if not duplicate:
            out.append(c)
    return out


def seed_for_file(item: dict, pred_csv: Path, min_conf: float, max_per_file: int) -> int:
    raw = read_predictions(pred_csv, min_conf=min_conf)
    dedup = dedupe_candidates(raw, freq_tol=2.0, time_tol=1.0)

    existing = item.get("annotations", [])
    has_existing = len(existing) > 0
    seeded = []
    for i, c in enumerate(dedup[:max_per_file], start=1):
        seeded.append(
            {
                "id": c["id"],
                "freq_hz": round(c["freq_hz"], 3),
                "start_sec": round(c["start_sec"], 3),
                "end_sec": round(c["end_sec"], 3),
                "snr_db_est": None,
                "_seed_confidence": round(c["confidence"], 3),
                "_seed_status": "needs_review",
            }
        )

    if not has_existing:
        item["annotations"] = seeded
    else:
        item.setdefault("_seed_candidates", seeded)

    return len(seeded)


def main() -> int:
    p = argparse.ArgumentParser(description="Seed annotation candidates from benchmark prediction CSVs")
    p.add_argument("--manifest", required=True, help="Path to local manifest JSON")
    p.add_argument("--pred-dir", required=True, help="Directory with <file_id>_pred.csv")
    p.add_argument("--out", default="", help="Output manifest path (default: overwrite input)")
    p.add_argument("--min-confidence", type=float, default=0.7, help="Minimum confidence")
    p.add_argument("--max-per-file", type=int, default=8, help="Max seeded annotations per file")
    args = p.parse_args()

    manifest_path = Path(args.manifest)
    pred_dir = Path(args.pred_dir)
    out_path = Path(args.out) if args.out else manifest_path

    data = load_json(manifest_path)
    files = data.get("files", [])
    total_seeded = 0
    files_with_seed = 0

    for item in files:
        file_id = str(item.get("file_id", ""))
        pred_csv = pred_dir / f"{file_id}_pred.csv"
        seeded = seed_for_file(item, pred_csv, min_conf=args.min_confidence, max_per_file=args.max_per_file)
        total_seeded += seeded
        if seeded > 0:
            files_with_seed += 1

    data.setdefault("notes", "")
    data["notes"] = (data["notes"] + "\nSeeded candidates added with _seed_status=needs_review.").strip()

    save_json(out_path, data)
    print("Annotation seeding completed")
    print(f"- manifest_in: {manifest_path}")
    print(f"- manifest_out: {out_path}")
    print(f"- files: {len(files)}")
    print(f"- files_with_seed: {files_with_seed}")
    print(f"- total_seeded_annotations: {total_seeded}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

# Phase 0 Baseline (Measurable)

This folder contains the frozen benchmark baseline requested in Phase 0:

- Metrics: precision, recall, false positives/hour, ID latency, runtime x real-time
- Frozen decoder settings and matching tolerances
- Gold dataset manifest schema for 20-50 annotated WAV files
- Reproducible benchmark script and fixed output report format

## Files

- `frozen_config.json`: fixed decoder options and matching criteria
- `gold_dataset_manifest.example.json`: schema/template for gold annotations
- `../../tools/benchmark_phase0.py`: benchmark runner

## Build decoder

Example (Windows UCRT64):

```bash
cmake -S . -B build-ucrt64 -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-ucrt64 -j
```

## Create your gold dataset

1. Copy `gold_dataset_manifest.example.json` to `gold_dataset_manifest.json`
2. Add 20-50 WAV entries with:
   - `file_id` unique identifier
   - `wav_path` absolute or reproducible path
   - `duration_sec` optional fallback
   - `annotations` list with:
     - `id` (2-3 uppercase chars)
     - `freq_hz`
     - `start_sec`, `end_sec`
     - `snr_db_est` optional

## Run benchmark (decode + evaluate)

```bash
python tools/benchmark_phase0.py \
  --decoder build-ucrt64/ndb_decode.exe \
  --manifest benchmarks/phase0/gold_dataset_manifest.json \
  --config benchmarks/phase0/frozen_config.json \
  --out-dir benchmarks/phase0/results \
  --msys-bash C:/msys64/usr/bin/bash.exe \
  --run
```

## Evaluate-only mode (no decode)

If per-file predictions/metrics already exist in output directory:

```bash
python tools/benchmark_phase0.py \
  --decoder build-ucrt64/ndb_decode.exe \
  --manifest benchmarks/phase0/gold_dataset_manifest.json \
  --config benchmarks/phase0/frozen_config.json \
  --msys-bash C:/msys64/usr/bin/bash.exe \
  --out-dir benchmarks/phase0/results
```

## Reports

Generated in `--out-dir`:

- `benchmark_report.json`: aggregate summary
- `benchmark_summary.csv`: aggregate metrics as key-value
- `benchmark_per_file.csv`: per-file metrics
- `<file_id>_pred.csv`: decoder output (if `--run`)
- `<file_id>_metrics.json`: decoder quality metrics (if `--run`)

## Matching policy (frozen v1)

- ID token match: exact uppercase token extracted from decoded text (`[A-Z]{2,3}`)
- Frequency tolerance: `2.0 Hz`
- Minimum temporal overlap: `0.2 s`
- One-to-one greedy assignment annotation->prediction

Keep this policy unchanged for baseline comparability.

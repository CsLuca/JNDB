# Phase 0 Baseline (Measurable)

This folder contains the frozen benchmark baseline requested in Phase 0:

- Metrics: precision, recall, false positives/hour, ID latency, runtime x real-time
- Frozen decoder settings and matching tolerances
- Gold dataset manifest schema for 20-50 annotated WAV files
- Reproducible benchmark script and fixed output report format

## Files

- `frozen_config.json`: fixed decoder options and matching criteria
- `frozen_config_hmm_legacy.json`: legacy HMM defaults expressed explicitly
- `frozen_config_hmm_calibrated.json`: calibrated HMM config (quality-oriented, robustness constrained)
- `frozen_config_hsmm_tuned.json`: explicit HSMM tuned config
- `frozen_config_auto_tuned.json`: auto-select model config (HMM vs HSMM per track)
- `gold_dataset_manifest.example.json`: schema/template for gold annotations
- `../../tools/benchmark_phase0.py`: benchmark runner
- `../../tools/calibrate_hmm_phase0.py`: rapid HMM transition/sigma calibration helper
- `../../tools/tune_sequence_models_phase0.py`: sequence model tuning (HMM/HSMM/auto)

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

### Fast bootstrap from one long WAV (local)

You can generate 20-50 chunk files and a local manifest to annotate:

```bash
python tools/prepare_phase0_chunks.py \
  --input-wav C:/Users/LBiondi/Downloads/ndb-315-415_013.wav \
  --out-dir C:/Users/LBiondi/Downloads/jndb_phase0_chunks \
  --manifest-out benchmarks/phase0/gold_dataset_manifest.local.json \
  --chunk-sec 5 \
  --count 30 \
  --start-sec 0 \
  --prefix phase0
```

Then fill `annotations` for each chunk in the local manifest.

### Semi-automatic annotation starter (recommended)

After at least one benchmark run (`--run`), pre-seed annotation candidates from prediction CSV files:

```bash
python tools/seed_annotations_from_predictions.py \
  --manifest benchmarks/phase0/gold_dataset_manifest.local.json \
  --pred-dir benchmarks/phase0/results \
  --min-confidence 0.7 \
  --max-per-file 8
```

This fills each file `annotations` (or `_seed_candidates` if already annotated) with candidates marked:

- `_seed_status: needs_review`
- `_seed_confidence`

Review each candidate and keep only valid ground truth events.

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

Append-only history (automatic by default):

- `benchmarks/phase0/history/benchmark_history.csv`
- `benchmarks/phase0/history/benchmark_history.jsonl`

Each run appends timestamp, git commit/branch, and aggregate metrics so optimization progress is preserved over time.

## Rapid HMM calibration

Use the calibration helper to search HMM transition and sigma parameters quickly.

```bash
python tools/calibrate_hmm_phase0.py \
  --decoder build-ucrt64/ndb_decode.exe \
  --manifest benchmarks/phase0/gold_dataset_manifest.local.json \
  --base-config benchmarks/phase0/frozen_config_hmm_legacy.json \
  --msys-bash C:/msys64/usr/bin/bash.exe \
  --max-candidates 12 \
  --results-csv benchmarks/phase0/calibration/hmm_calibration_results_full.csv \
  --best-config-out benchmarks/phase0/frozen_config_hmm_calibrated.json
```

The script writes a new config JSON with the selected HMM parameters and embeds baseline vs best benchmark metrics.

## HSMM and auto model tuning

```bash
python tools/tune_sequence_models_phase0.py \
  --decoder build-ucrt64/ndb_decode.exe \
  --manifest benchmarks/phase0/gold_dataset_manifest.local.json \
  --base-config benchmarks/phase0/frozen_config_hmm_legacy.json \
  --msys-bash C:/msys64/usr/bin/bash.exe \
  --only-annotated \
  --max-hsmm-candidates 6 \
  --results-csv benchmarks/phase0/calibration/sequence_models_tuning_annotated.csv \
  --best-hsmm-config benchmarks/phase0/frozen_config_hsmm_tuned.json \
  --best-auto-config benchmarks/phase0/frozen_config_auto_tuned.json
```

`decoder-model` options:

- `hmm`: force HMM decoder
- `hsmm`: force explicit HSMM decoder
- `auto`: run both and pick the best-scoring decoded sequence per track

## Matching policy (frozen v1)

- ID token match: exact uppercase token extracted from decoded text (`[A-Z]{2,3}`)
- Frequency tolerance: `2.0 Hz`
- Minimum temporal overlap: `0.2 s`
- One-to-one greedy assignment annotation->prediction

Keep this policy unchanged for baseline comparability.

## Notes about metrics validity

- If manifest files have empty `annotations`, benchmark still runs and reports runtime/quality metrics.
- In that condition, precision/recall are placeholders and not scientifically meaningful.
- The script prints a warning when annotation coverage is zero.

# JNDB

`JNDB` is a C++ command-line decoder that scans a WAV recording and extracts likely NDB Morse beacon candidates.

It is designed for weak-signal workflows and includes robust thresholding, multi-track tone detection, and Morse-like decoding heuristics.

## Features

- WAV input support: PCM 16-bit/24-bit mono/stereo, IEEE float32 mono/stereo
- Time-frequency candidate extraction from STFT
- Robust MAD-based peak detection and thresholding
- AMTC-lite multi-track continuity tracking
- Envelope, boxcar integration, adaptive dot-length estimation
- CSV export for downstream review and filtering

## Algorithmic notes (paper-inspired)

- `arXiv:1911.10105`
  - non-coherent envelope detection
  - adaptive thresholding instead of fixed thresholds
- `arXiv:1612.07882`
  - semi-coherent flavor via per-track mix-down and envelope processing
- `arXiv:2005.06686`
  - AMTC-like continuity tracking for weak spectral traces
- `arXiv:2605.05342`
  - robust MAD-style statistics for non-stationary/noisy conditions

This is a practical engineering implementation inspired by these ideas, not a strict reproduction of each paper's full communication model.

## Build

Requirements:

- CMake 3.20+
- C++20 compiler

### Linux / generic

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### Windows (MSYS2 UCRT64)

```bash
cmake -S . -B build-ucrt64 -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-ucrt64 -j
```

## Command line

```text
ndb_decode /h
ndb_decode /?
ndb_decode <input.wav> [output.csv] [options]
```

## GUI mode (Windows)

If you start the program with no arguments on Windows, it opens a graphical interface.

- Start from Explorer by double-clicking `ndb_decode.exe`
- Or from terminal with no args:

```powershell
ndb_decode
```

GUI design goals:

- Simple layout with only key actions: input WAV, output CSV, metrics JSON, start
- Consistent controls and feedback
- Large readable text and high-contrast neutral colors
- Progress bar with live percent and compact quality summary
- Responsive behavior (decode runs in background thread)

Professional analytics dashboard (Windows GUI):

- Loads benchmark history CSV and plots all key optimization metrics
- Charts included:
  - Precision
  - Recall
  - False positives/hour
  - ID latency (seconds)
  - Runtime x realtime
  - Quality score
- Shows last-value delta (improving/worsening color-coded)
- Refresh button for immediate comparison after each benchmark run
- Supports zoom/pan exploration directly in charts
- Exports dashboard to PNG for reports
- Includes compare overlay mode (second history CSV)
- Hover tooltip on chart points with run id, timestamp, commit, branch, and exact metric value
- Preset selector for decode profiles: `Default`, `Strict DX`, `Relaxed`

Tip: for automated flows and scripts, use CLI mode with explicit arguments.

### Help switches

- `/h`
- `/?`
- `-h`
- `--help`

Note: inside MSYS2 bash, arguments starting with `/` can be path-converted by the shell. In that case prefer `-h` or `--help`.

### Options

- `--fft <int>`: FFT size (default `512`)
- `--hop <int>`: STFT hop size (default `128`)
- `--mad-factor <float>`: MAD threshold factor (default `4.0`)
- `--guard-bins <int>`: guard bins around peaks (default `2`)
- `--max-step-bins <int>`: max frequency-bin drift/frame for track linking (default `3`)
- `--min-track-frames <int>`: minimum frames to keep a track (default `15`)
- `--sustain-penalty <float>`: tracking sustain term (default `0.02`)
- `--envelope-alpha <float>`: envelope exponential smoothing alpha (default `0.05`)
- `--threshold-k <float>`: MAD K for OOK decision threshold (default `2.5`)
- `--min-dot-ms <int>`: minimum dot length hypothesis in ms (default `40`)
- `--max-dot-ms <int>`: maximum dot length hypothesis in ms (default `220`)
- `--target-sr <int>`: target sample rate after decimation (default `8000`)
- `--max-seconds <int>`: max seconds analyzed from the file (default `90`)
- `--rf-input`: interpret frequency gate as RF NDB range
- `--ndb-min-hz <float>` / `--ndb-max-hz <float>`: RF NDB range gate (default `190..535`)
- `--audio-min-hz <float>` / `--audio-max-hz <float>`: audio gate range (default `80..2000`)
- `--cluster-freq-tol <float>`: track cluster frequency merge tolerance (default `2.0`)
- `--cluster-gap-sec <float>`: merge gap for overlapping/nearby tracks (default `0.4`)
- `--dedup-freq-tol <float>`: repeated-ID dedup frequency tolerance (default `2.0`)
- `--require-plausible-id`: enforce plausible cyclic beacon ID filter (default on)
- `--allow-any-id`: disable plausible ID gating
- `--plausible-id-min <float>`: plausible ID minimum score (default `0.30`)
- `--strict-beacon`: require repeated beacon observations before output
- `--strict-min-repeats <int>`: minimum `hit_count` in strict mode (default `3`)
- `--decoder-model <name>`: sequence decoder model (`auto`, `hmm`, `hsmm`)
- `--hmm-on-intra <float>` / `--hmm-on-char <float>` / `--hmm-on-word <float>`: ON->OFF HMM transition weights (default `0.70/0.25/0.05`)
- `--hmm-off-dot <float>` / `--hmm-off-dash <float>`: OFF->ON HMM transition weights (default `0.75/0.25`)
- `--hmm-sigma-dot <float>` / `--hmm-sigma-dash <float>`: ON-state duration sigmas (default `0.40/0.75`)
- `--hmm-sigma-intra <float>` / `--hmm-sigma-char <float>` / `--hmm-sigma-word <float>`: OFF-state duration sigmas (default `0.45/0.90/1.60`)
- `--hsmm-on-intra <float>` / `--hsmm-on-char <float>` / `--hsmm-on-word <float>`: ON->OFF HSMM transition weights
- `--hsmm-off-dot <float>` / `--hsmm-off-dash <float>`: OFF->ON HSMM transition weights
- `--hsmm-sigma-dot <float>` / `--hsmm-sigma-dash <float>`: HSMM ON-state duration sigmas
- `--hsmm-sigma-intra <float>` / `--hsmm-sigma-char <float>` / `--hsmm-sigma-word <float>`: HSMM OFF-state duration sigmas
- `--hsmm-tail-mix <float>`: heavy-tail mix for explicit duration distribution
- `--hsmm-time-gain <float>`: strength of time-dependent transition adaptation
- `--mode <preset>`: preset profile (`default`, `strict-dx`, `relaxed`)
- `--min-confidence <float>`: output filter; keep rows with confidence >= value
- `--metrics <path.json>`: write run quality metrics JSON for trend tracking
- `--no-progress`: disable progress output
- `--quiet`: compact final output only

## Examples

### Show help

```bash
ndb_decode /?
```

### Decode and print to terminal

```bash
ndb_decode capture.wav
```

### Decode and save CSV

```bash
ndb_decode capture.wav results.csv
```

### Longer analysis window + custom decimation target

```bash
ndb_decode capture.wav results.csv --max-seconds 180 --target-sr 12000
```

### Stricter output confidence and custom detector sensitivity

```bash
ndb_decode capture.wav results.csv --min-confidence 0.70 --mad-factor 3.5 --threshold-k 3.0
```

### Strict DX preset

```bash
ndb_decode capture.wav results.csv --mode strict-dx
```

`strict-dx` applies conservative defaults for weak-signal reliability:

- plausible ID required (`plausible_id_score >= 0.30`)
- strict beacon mode enabled (`strict_min_repeats = 3`)
- tighter cluster/dedup tolerances
- slightly stricter thresholding

### Save quality metrics for comparison between versions

```bash
ndb_decode capture.wav results.csv --metrics run_metrics.json
```

### Quiet mode for automation

```bash
ndb_decode capture.wav results.csv --quiet --metrics run_metrics.json
```

## Output format

CSV columns:

- `track_id`
- `freq_hz`
- `text`
- `confidence`
- `start_sec`
- `end_sec`

Metrics JSON fields:

- `quality_score`: synthetic 0..100 score to compare runs over time
- `mean_confidence`, `median_confidence`, `max_confidence`
- `decode_ratio`: decoded tracks / total tracks
- `id_like_token_ratio`: ratio of 2-3 uppercase tokens in decoded text
- `track_count`, `decoded_count`, `candidate_bin_count`, `frame_count`
- `clustered_count`, `dedup_count`, `mean_composite_score`
- `plausible_id_rejected`, `plausible_id_ratio`
- `strict_rejected`

Phase 1 detection cleanup now includes:

- frequency clustering and merge of overlapping tracks
- repeated-ID dedup (`first_seen`, `last_seen`, `hit_count`)
- plausible beacon ID extraction (2-3 uppercase tokens with cyclic repetition score)
- optional strict beacon mode: emit only IDs observed at least `N` times
- composite track score from:
  - energy
  - continuity
  - frequency stability
  - keying periodicity
- realistic range gate for RF NDB or audio-domain processing

### Rapid HMM calibration helper

- Script: `tools/calibrate_hmm_phase0.py`
- Purpose: quick grid search for HMM transitions/sigmas on the Phase0 benchmark manifest.
- Output artifacts:
  - best config JSON (`--best-config-out`)
  - optional candidate scoreboard CSV (`--results-csv`)

Example:

```bash
python tools/calibrate_hmm_phase0.py \
  --decoder build_ucrt64/ndb_decode.exe \
  --manifest benchmarks/phase0/gold_dataset_manifest.local.json \
  --base-config benchmarks/phase0/frozen_config.json \
  --msys-bash C:/msys64/usr/bin/bash.exe \
  --only-annotated \
  --max-candidates 18 \
  --results-csv benchmarks/phase0/calibration/hmm_calibration_results.csv \
  --best-config-out benchmarks/phase0/frozen_config_hmm_calibrated.json
```

### HSMM + auto model tuning helper

- Script: `tools/tune_sequence_models_phase0.py`
- Purpose: compare/tune `hmm`, explicit `hsmm`, and `auto` selection on Phase0.
- Outputs:
  - tuned HSMM config JSON
  - tuned auto config JSON
  - CSV scoreboard

Example:

```bash
python tools/tune_sequence_models_phase0.py \
  --decoder build_ucrt64/ndb_decode.exe \
  --manifest benchmarks/phase0/gold_dataset_manifest.local.json \
  --base-config benchmarks/phase0/frozen_config_hmm_legacy.json \
  --msys-bash C:/msys64/usr/bin/bash.exe \
  --only-annotated \
  --max-hsmm-candidates 6 \
  --results-csv benchmarks/phase0/calibration/sequence_models_tuning_annotated.csv \
  --best-hsmm-config benchmarks/phase0/frozen_config_hsmm_tuned.json \
  --best-auto-config benchmarks/phase0/frozen_config_auto_tuned.json
```

## Phase 2 robust Morse decoder

The decoder now uses a Viterbi/HMM style sequence model instead of hard threshold symbol splitting.

- States model ON/OFF durations around dot/dash/intra-char/char-gap/word-gap
- Adaptive timing recovery: local dot estimate changes over time (fading/drift aware)
- Probabilistic dot/dash classification via posterior-like likelihoods
- Strong post-decoding grammar prior via plausible NDB ID extraction and cyclic repetition scoring

Quality score formula:

- `100 * (0.45*mean_confidence + 0.25*median_confidence + 0.20*decode_ratio + 0.10*id_like_token_ratio)`

Use this as a trend indicator across the same benchmark set. It is not an absolute scientific accuracy metric.

## Practical limitations

- If the recording is audio from a narrow receiver chain, only tones present in that audio are decodable.
- For wideband "all beacon in span" workflows, IQ capture is typically preferred.

## Phase 0 baseline benchmark

A reproducible Phase 0 benchmark framework is included.

- Frozen config: `benchmarks/phase0/frozen_config.json`
- Gold dataset template: `benchmarks/phase0/gold_dataset_manifest.example.json`
- Benchmark runner: `tools/benchmark_phase0.py`
- Guide: `benchmarks/phase0/README.md`

This tracks the requested metrics over time:

- precision
- recall
- false positives/hour
- ID latency
- runtime x real-time
- plus mean `quality_score`

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
- `--min-confidence <float>`: output filter; keep rows with confidence >= value

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

## Output format

CSV columns:

- `track_id`
- `freq_hz`
- `text`
- `confidence`
- `start_sec`
- `end_sec`

## Practical limitations

- If the recording is audio from a narrow receiver chain, only tones present in that audio are decodable.
- For wideband "all beacon in span" workflows, IQ capture is typically preferred.

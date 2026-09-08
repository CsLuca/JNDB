# NDB Decoder (WAV -> CSV)

This tool decodes likely NDB Morse identifiers from an input WAV file.

## What is implemented from the cited papers

- `arXiv:1911.10105` (optimal non-coherent detector):
  - non-coherent envelope-based detection
  - robust adaptive thresholding (MAD-based) instead of fixed threshold
  - energy-integration behavior via boxcar smoothing

- `arXiv:1612.07882` (semi-coherent improvements):
  - practical semi-coherent flavor through per-track tone estimation and mix-down
  - thresholding after tone isolation and envelope smoothing

- `arXiv:2005.06686` (AMTC-like weak trace tracking):
  - multi-frame track building over spectrogram peaks
  - continuity-aware assignment with drift constraints (AMTC-lite)

- `arXiv:2605.05342` (robust statistics in non-stationary noise):
  - MAD-based bin selection in each spectrogram frame
  - robust thresholding for OOK/CW symbol decision

## Build

Requires a C++20 compiler and CMake 3.20+.

```powershell
cmake -S . -B build
cmake --build build --config Release
```

## Usage

```powershell
ndb_decode <input.wav> [output.csv]
```

- Input WAV: PCM 16-bit/24-bit mono/stereo, or IEEE-float 32-bit mono/stereo.
- If `output.csv` is omitted, results are printed to stdout.

CSV columns:

- `track_id`
- `freq_hz`
- `text`
- `confidence`
- `start_sec`
- `end_sec`

## Notes

- This is a practical implementation inspired by the papers, not a verbatim reproduction of their full communication-system models.
- For best results on "all NDB in band", use wideband IQ recordings. Audio WAV from a narrow receiver front-end limits what can be decoded.
- For runtime control, the default config now decimates and analyzes up to 90 seconds of audio.

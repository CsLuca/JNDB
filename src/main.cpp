#include "ndb_decoder.hpp"
#include "wav.hpp"

#ifdef _WIN32
#include "gui_win32.hpp"
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct CliArgs {
  bool help = false;
  bool progress = true;
  bool quiet = false;
  bool streamMode = false;
  std::string inputPath;
  std::optional<std::string> outputPath;
  std::optional<std::string> outputJsonPath;
  std::optional<std::string> metricsPath;
  std::optional<std::string> diagnosticsLogPath;
  std::optional<std::string> dashboardMode;
  std::optional<std::string> sessionExportDir;
  std::optional<std::string> configPath;
  int streamPollMs = 1000;
  int streamIterations = 1;
  int streamTailSeconds = 0;
  float minConfidence = 0.0f;
  ndb::DecoderConfig cfg;
};

bool ApplyModePreset(const std::string& mode, ndb::DecoderConfig* cfg, std::string* error) {
  if (!cfg || !error) {
    return false;
  }
  if (mode == "default") {
    return true;
  }
  if (mode == "strict-dx") {
    cfg->requirePlausibleId = true;
    cfg->plausibleIdMinScore = 0.30f;
    cfg->strictBeaconMode = true;
    cfg->strictMinRepeats = 3;
    cfg->dedupFreqTolHz = 1.5f;
    cfg->clusterFreqTolHz = 2.0f;
    cfg->clusterGapSec = 0.35f;
    cfg->thresholdK = 2.6f;
    return true;
  }
  if (mode == "relaxed") {
    cfg->requirePlausibleId = false;
    cfg->strictBeaconMode = false;
    cfg->dedupFreqTolHz = 2.5f;
    cfg->clusterFreqTolHz = 2.5f;
    cfg->clusterGapSec = 0.6f;
    cfg->thresholdK = 2.2f;
    return true;
  }
  if (mode == "phase3-balanced") {
    cfg->enableBandLimit = true;
    cfg->bandLowHz = 110.0f;
    cfg->bandHighHz = 2000.0f;
    cfg->enableAutoNotch = false;
    cfg->enableImpulseBlanker = false;
    cfg->enableCfar2d = false;
    cfg->useAmtcFull = false;
    return true;
  }
  if (mode == "phase3-selective") {
    cfg->enableBandLimit = true;
    cfg->bandLowHz = 110.0f;
    cfg->bandHighHz = 2000.0f;
    cfg->enableAutoNotch = false;
    cfg->enableImpulseBlanker = false;
    cfg->enableCfar2d = true;
    cfg->cfarTrainTime = 3;
    cfg->cfarGuardTime = 1;
    cfg->cfarTrainFreq = 4;
    cfg->cfarGuardFreq = 1;
    cfg->cfarScale = 1.35f;
    cfg->useAmtcFull = false;
    return true;
  }
  if (mode == "phase4-serious") {
    cfg->useAmtcFull = true;
    cfg->maxTrackGapFrames = 4;
    cfg->enableCochannelSeparation = true;
    cfg->cochannelMaxTracks = 2;
    cfg->cochannelMaxGapFrames = 4;
    cfg->cochannelMaxStepHz = 5.0f;
    cfg->confidenceCalibration = "platt";
    return true;
  }
  if (mode == "quiet") {
    cfg->enableBandLimit = true;
    cfg->bandLowHz = 130.0f;
    cfg->bandHighHz = 1200.0f;
    cfg->enableAutoNotch = false;
    cfg->enableImpulseBlanker = false;
    cfg->enableCfar2d = false;
    cfg->thresholdK = 2.4f;
    cfg->useAmtcFull = false;
    return true;
  }
  if (mode == "urban-noise") {
    cfg->enableBandLimit = true;
    cfg->bandLowHz = 100.0f;
    cfg->bandHighHz = 2200.0f;
    cfg->enableAutoNotch = true;
    cfg->autoNotchMaxCount = 4;
    cfg->autoNotchSnrDb = 7.0f;
    cfg->enableImpulseBlanker = true;
    cfg->impulseBlankerSigma = 7.5f;
    cfg->impulseBlankerHalfWindow = 2;
    cfg->enableCfar2d = true;
    cfg->cfarTrainTime = 3;
    cfg->cfarGuardTime = 1;
    cfg->cfarTrainFreq = 4;
    cfg->cfarGuardFreq = 1;
    cfg->cfarScale = 1.45f;
    cfg->useAmtcFull = false;
    return true;
  }
  if (mode == "weak-signal-dx") {
    cfg->useAmtcFull = true;
    cfg->maxTrackGapFrames = 5;
    cfg->strictBeaconMode = true;
    cfg->strictMinRepeats = 3;
    cfg->requirePlausibleId = true;
    cfg->plausibleIdMinScore = 0.35f;
    cfg->confidenceCalibration = "platt";
    cfg->thresholdK = 2.6f;
    return true;
  }
  *error =
      "Invalid value for --mode (use: default, strict-dx, relaxed, phase3-balanced, phase3-selective, phase4-serious, quiet, urban-noise, weak-signal-dx)";
  return false;
}

bool ApplyPerformanceProfile(const std::string& profile, ndb::DecoderConfig* cfg, std::string* error) {
  if (!cfg || !error) {
    return false;
  }
  if (profile == "balanced") {
    cfg->decodeThreads = std::max(1u, std::thread::hardware_concurrency() / 2u);
    cfg->targetSampleRate = 8000;
    cfg->fftSize = 512;
    cfg->hopSize = 128;
    return true;
  }
  if (profile == "fast") {
    cfg->decodeThreads = std::max(1u, std::thread::hardware_concurrency());
    cfg->targetSampleRate = 6000;
    cfg->fftSize = 256;
    cfg->hopSize = 128;
    cfg->useAmtcFull = false;
    return true;
  }
  if (profile == "deep") {
    cfg->decodeThreads = std::max(1u, std::thread::hardware_concurrency() / 2u);
    cfg->targetSampleRate = 12000;
    cfg->fftSize = 1024;
    cfg->hopSize = 128;
    cfg->useAmtcFull = true;
    cfg->maxAnalyzeSeconds = std::max(cfg->maxAnalyzeSeconds, 180);
    return true;
  }
  *error = "Invalid value for --profile (use: fast, balanced, deep)";
  return false;
}

void PrintUsage() {
  std::cout
      << "NDB Decoder - decode likely NDB Morse IDs from WAV\n\n"
      << "Usage:\n"
      << "  ndb_decode /h\n"
      << "  ndb_decode /?\n"
      << "  ndb_decode <input.wav> [output.csv] [options]\n\n"
      << "Options:\n"
      << "  --fft <int>                FFT size (default: 512)\n"
      << "  --hop <int>                STFT hop size (default: 128)\n"
      << "  --mad-factor <float>       MAD threshold factor (default: 4.0)\n"
      << "  --guard-bins <int>         Guard bins around peaks (default: 2)\n"
      << "  --max-step-bins <int>      Max track drift in bins/frame (default: 3)\n"
      << "  --min-track-frames <int>   Min frames to keep a track (default: 15)\n"
      << "  --sustain-penalty <float>  Track sustain bonus term (default: 0.02)\n"
      << "  --amtc-lite                Use AMTC-lite greedy tracker (default)\n"
      << "  --amtc-full                Use AMTC-full DP tracker\n"
      << "  --max-track-gap <int>      Max gap frames for AMTC-full (default: 3)\n"
      << "  --envelope-alpha <float>   Envelope smoother alpha (default: 0.05)\n"
      << "  --band-limit               Enable front-end band-limit\n"
      << "  --no-band-limit            Disable front-end band-limit (default off)\n"
      << "  --band-low-hz <float>      Front-end band-pass low cutoff (default: 90)\n"
      << "  --band-high-hz <float>     Front-end band-pass high cutoff (default: 2200)\n"
      << "  --auto-notch               Enable automatic notch filtering\n"
      << "  --no-auto-notch            Disable automatic notch filtering (default off)\n"
      << "  --auto-notch-max <int>     Max auto-notch tones (default: 3)\n"
      << "  --auto-notch-snr-db <float> Auto-notch trigger SNR in dB (default: 8.0)\n"
      << "  --impulse-blanker          Enable impulse blanker\n"
      << "  --no-impulse-blanker       Disable impulse blanker (default off)\n"
      << "  --impulse-sigma <float>    Impulse blanker sigma threshold (default: 6.0)\n"
      << "  --impulse-window <int>     Impulse blanker half-window (default: 3)\n"
      << "  --threshold-k <float>      MAD K for OOK threshold (default: 2.5)\n"
      << "  --cfar2d                   Enable 2D-CFAR candidate gating\n"
      << "  --no-cfar2d                Disable 2D-CFAR candidate gating (default off)\n"
      << "  --cfar-train-time <int>    2D-CFAR train cells in time (default: 4)\n"
      << "  --cfar-guard-time <int>    2D-CFAR guard cells in time (default: 1)\n"
      << "  --cfar-train-freq <int>    2D-CFAR train cells in freq (default: 6)\n"
      << "  --cfar-guard-freq <int>    2D-CFAR guard cells in freq (default: 1)\n"
      << "  --cfar-scale <float>       2D-CFAR noise scaling (default: 2.8)\n"
      << "  --min-dot-ms <int>         Min dot length ms (default: 40)\n"
      << "  --max-dot-ms <int>         Max dot length ms (default: 220)\n"
      << "  --target-sr <int>          Target sample rate after decimation (default: 8000)\n"
      << "  --max-seconds <int>        Max seconds to analyze (default: 90)\n"
      << "  --rf-input                 Treat frequency as RF-scale NDB (190..535)\n"
      << "  --ndb-min-hz <float>       RF lower gate (default: 190)\n"
      << "  --ndb-max-hz <float>       RF upper gate (default: 535)\n"
      << "  --audio-min-hz <float>     Audio lower gate (default: 80)\n"
      << "  --audio-max-hz <float>     Audio upper gate (default: 2000)\n"
      << "  --cluster-freq-tol <float> Cluster merge freq tolerance (default: 2.0)\n"
      << "  --cluster-gap-sec <float>  Cluster merge time gap (default: 0.4)\n"
      << "  --dedup-freq-tol <float>   ID dedup freq tolerance (default: 2.0)\n"
      << "  --cochannel-sep            Enable co-channel split for nearby overlapping beacons\n"
      << "  --no-cochannel-sep         Disable co-channel split\n"
      << "  --cochannel-max-tracks <int> Max split tracks per cluster (default: 2)\n"
      << "  --cochannel-max-gap <int>  Max frame gap inside split tracks (default: 4)\n"
      << "  --cochannel-max-step-hz <float> Max frequency step per point in split tracker (default: 6)\n"
      << "  --require-plausible-id     Keep only plausible cyclic 2-3 char beacon IDs\n"
      << "  --allow-any-id             Disable plausible ID filter\n"
      << "  --plausible-id-min <float> Plausible ID score threshold (default: 0.30)\n"
      << "  --strict-beacon            Require repeated ID hits before emitting\n"
      << "  --strict-min-repeats <int> Minimum hit_count for strict mode (default: 3)\n"
      << "  --decoder-model <name>     Decoder model: auto | hmm | hsmm (default: auto)\n"
      << "  --hmm-on-intra <float>     HMM P(on->intra) base weight (default: 0.73)\n"
      << "  --hmm-on-char <float>      HMM P(on->char-gap) base weight (default: 0.20)\n"
      << "  --hmm-on-word <float>      HMM P(on->word-gap) base weight (default: 0.07)\n"
      << "  --hmm-off-dot <float>      HMM P(off->dot) base weight (default: 0.80)\n"
      << "  --hmm-off-dash <float>     HMM P(off->dash) base weight (default: 0.20)\n"
      << "  --hmm-sigma-dot <float>    HMM sigma for dot duration (default: 0.35)\n"
      << "  --hmm-sigma-dash <float>   HMM sigma for dash duration (default: 0.65)\n"
      << "  --hmm-sigma-intra <float>  HMM sigma for intra gap (default: 0.40)\n"
      << "  --hmm-sigma-char <float>   HMM sigma for char gap (default: 0.80)\n"
      << "  --hmm-sigma-word <float>   HMM sigma for word gap (default: 1.35)\n"
      << "  --hsmm-on-intra <float>    HSMM P(on->intra) base weight (default: 0.72)\n"
      << "  --hsmm-on-char <float>     HSMM P(on->char-gap) base weight (default: 0.20)\n"
      << "  --hsmm-on-word <float>     HSMM P(on->word-gap) base weight (default: 0.08)\n"
      << "  --hsmm-off-dot <float>     HSMM P(off->dot) base weight (default: 0.82)\n"
      << "  --hsmm-off-dash <float>    HSMM P(off->dash) base weight (default: 0.18)\n"
      << "  --hsmm-sigma-dot <float>   HSMM sigma for dot duration (default: 0.38)\n"
      << "  --hsmm-sigma-dash <float>  HSMM sigma for dash duration (default: 0.72)\n"
      << "  --hsmm-sigma-intra <float> HSMM sigma for intra gap (default: 0.42)\n"
      << "  --hsmm-sigma-char <float>  HSMM sigma for char gap (default: 0.85)\n"
      << "  --hsmm-sigma-word <float>  HSMM sigma for word gap (default: 1.45)\n"
      << "  --hsmm-tail-mix <float>    HSMM duration heavy-tail mix (default: 0.18)\n"
      << "  --hsmm-time-gain <float>   HSMM time-dependent transition gain (default: 0.55)\n"
      << "  --confidence-calibration <name> Confidence calibration: none | platt | isotonic\n"
      << "  --platt-a <float>          Platt sigmoid slope parameter A (default: 5.0)\n"
      << "  --platt-b <float>          Platt sigmoid bias parameter B (default: -2.5)\n"
      << "  --freq-prior-file <path>   Optional frequency prior CSV: freq_hz,ID1|ID2|...\n"
      << "  --freq-prior-tol <float>   Frequency tolerance for prior shortlist (default: 2.5)\n"
      << "  --require-prior-match      If prior exists near freq, keep only matching ID\n"
      << "  --decode-threads <int>     Worker threads for per-track decode (default: 1)\n"
      << "  --seed <int>               Deterministic seed for stable ordering (default: 1337)\n"
      << "  --profile <name>           Performance profile: fast | balanced | deep\n"
      << "  --config <path.json>       Optional config file for startup values\n"
      << "  --stream                   Streaming mode (polling same WAV path repeatedly)\n"
      << "  --stream-poll-ms <int>     Streaming polling interval ms (default: 1000)\n"
      << "  --stream-iterations <int>  Number of streaming iterations (default: 1)\n"
      << "  --stream-tail-seconds <int> Keep last N seconds per stream iteration (default: 0=all)\n"
      << "  --output-json <path.json>  Write stable JSON output alongside CSV\n"
      << "  --diag-log <path.log>      Append diagnostic run logs\n"
      << "  --dashboard rich           Print minimal terminal dashboard (waterfall/tracks/timeline)\n"
      << "  --session-export <dir>     Export session evidence bundle (audio snippets + scores + params)\n"
      << "  --mode <preset>            Preset: default | strict-dx | relaxed | phase3-balanced | phase3-selective | phase4-serious | quiet | urban-noise | weak-signal-dx\n"
      << "  --min-confidence <float>   Keep only rows with confidence >= value\n"
      << "  --metrics <path.json>      Write quality metrics JSON\n"
      << "  --no-progress              Disable progress output\n"
      << "  --quiet                    Print only final essentials\n\n"
      << "Examples:\n"
      << "  ndb_decode /?\n"
      << "  ndb_decode C:\\radio\\capture.wav\n"
      << "  ndb_decode C:\\radio\\capture.wav C:\\radio\\out.csv\n"
      << "  ndb_decode capture.wav out.csv --max-seconds 180 --target-sr 12000\n"
      << "  ndb_decode capture.wav --min-confidence 0.7 --mad-factor 3.5\n"
      << "  ndb_decode capture.wav out.csv --metrics run_metrics.json\n"
      << "  ndb_decode capture.wav out.csv --mode strict-dx\n"
      << "  ndb_decode capture.wav out.csv --mode phase3-balanced\n"
      << "  ndb_decode capture.wav out.csv --mode phase4-serious --freq-prior-file priors.csv\n"
      << "  ndb_decode capture.wav out.csv --profile fast --decode-threads 8 --seed 42\n"
      << "  ndb_decode capture.wav out.csv --output-json out.json --diag-log run.log\n"
      << "  ndb_decode capture.wav out.csv --mode urban-noise --dashboard rich --session-export session_001\n";
}

bool WriteCsv(const std::string& path, const std::vector<ndb::DecodeResult>& results,
              std::string* error) {
  std::ofstream out(path);
  if (!out) {
    *error = "Cannot write output file: " + path;
    return false;
  }
  out << "track_id,freq_hz,text,plausible_id,plausible_id_score,confidence,confidence_raw,confidence_calibrated,prior_matched,prior_candidates,decoder_model,start_sec,end_sec,first_seen_sec,last_seen_sec,hit_count,composite_score,energy_score,continuity_score,freq_stability_score,keying_periodicity_score\n";
  for (const auto& r : results) {
    out << r.trackId << ',' << std::fixed << std::setprecision(2) << r.freqHz << ',' << '"'
        << r.text << '"' << ',' << '"' << r.plausibleId << '"' << ','
        << std::setprecision(3) << r.plausibleIdScore << ',' << r.confidence << ','
        << r.confidenceRaw << ',' << r.confidenceCalibrated << ','
        << (r.priorMatched ? 1 : 0) << ',' << '"' << r.priorCandidates << '"' << ','
        << r.decoderModel << ','
        << std::setprecision(3) << r.startSec << ',' << r.endSec << ','
        << r.firstSeenSec << ',' << r.lastSeenSec << ',' << r.hitCount << ','
        << r.compositeScore << ',' << r.energyScore << ',' << r.continuityScore << ','
        << r.freqStabilityScore << ',' << r.keyingPeriodicityScore << '\n';
  }
  return true;
}

std::string EscapeJson(const std::string& s) {
  std::ostringstream o;
  for (char c : s) {
    switch (c) {
      case '"':
        o << "\\\"";
        break;
      case '\\':
        o << "\\\\";
        break;
      case '\n':
        o << "\\n";
        break;
      case '\r':
        o << "\\r";
        break;
      case '\t':
        o << "\\t";
        break;
      default:
        o << c;
        break;
    }
  }
  return o.str();
}

bool WriteJson(const std::string& path, const std::vector<ndb::DecodeResult>& results,
               const ndb::DecodeStats& s, std::string* error) {
  std::ofstream out(path);
  if (!out) {
    *error = "Cannot write JSON output file: " + path;
    return false;
  }
  out << "{\n";
  out << "  \"schema_version\": \"jndb.decode.v1\",\n";
  out << "  \"stats\": {\n";
  out << "    \"quality_score\": " << std::fixed << std::setprecision(3) << s.qualityScore << ",\n";
  out << "    \"decoded_count\": " << s.decodedCount << ",\n";
  out << "    \"track_count\": " << s.trackCount << "\n";
  out << "  },\n";
  out << "  \"results\": [\n";
  for (std::size_t i = 0; i < results.size(); ++i) {
    const auto& r = results[i];
    out << "    {\"track_id\": " << r.trackId << ", \"freq_hz\": " << std::fixed
        << std::setprecision(2) << r.freqHz << ", \"text\": \"" << EscapeJson(r.text)
        << "\", \"plausible_id\": \"" << EscapeJson(r.plausibleId)
        << "\", \"confidence\": " << std::setprecision(3) << r.confidence
        << ", \"confidence_raw\": " << r.confidenceRaw
        << ", \"confidence_calibrated\": " << r.confidenceCalibrated
        << ", \"decoder_model\": \"" << EscapeJson(r.decoderModel)
        << "\", \"start_sec\": " << std::setprecision(3) << r.startSec
        << ", \"end_sec\": " << r.endSec << "}";
    if (i + 1 < results.size()) {
      out << ',';
    }
    out << "\n";
  }
  out << "  ]\n";
  out << "}\n";
  return true;
}

void PrintRichDashboard(const std::vector<ndb::DecodeResult>& results, const ndb::DecodeStats& stats) {
  std::cout << "\n=== JNDB Dashboard (Rich) ===\n";
  std::cout << "Waterfall (synthetic density by confidence)\n";
  for (const auto& r : results) {
    const int n = std::max(1, std::min(30, static_cast<int>(std::round(r.confidence * 30.0f))));
    std::cout << std::fixed << std::setprecision(1) << std::setw(6) << r.freqHz << "Hz | "
              << std::string(static_cast<std::size_t>(n), '#') << '\n';
  }
  std::cout << "Tracks\n";
  for (const auto& r : results) {
    std::cout << "- T" << r.trackId << " " << std::fixed << std::setprecision(2) << r.freqHz
              << "Hz id=" << r.plausibleId << " conf=" << std::setprecision(3) << r.confidence
              << " score=" << r.compositeScore << "\n";
  }
  std::cout << "Decode timeline\n";
  for (const auto& r : results) {
    std::cout << "- [" << std::fixed << std::setprecision(2) << r.startSec << ".." << r.endSec
              << "] " << (r.plausibleId.empty() ? r.text : r.plausibleId) << "\n";
  }
  std::cout << "Summary quality=" << std::fixed << std::setprecision(2) << stats.qualityScore
            << " decoded=" << stats.decodedCount << " tracks=" << stats.trackCount << "\n";
}

bool ExportSessionEvidence(const std::string& dir, const ndb::WavData& wav,
                           const std::vector<ndb::DecodeResult>& results,
                           const ndb::DecodeStats& stats, const CliArgs& args,
                           std::string* error) {
  const std::filesystem::path baseDir(dir);
  std::error_code ec;
  std::filesystem::create_directories(baseDir, ec);
  if (ec) {
    *error = "Cannot create session export directory: " + baseDir.string();
    return false;
  }

  const std::filesystem::path summaryPath = baseDir / "session_summary.json";
  std::ofstream out(summaryPath.string());
  if (!out) {
    *error = "Cannot write session summary: " + summaryPath.string();
    return false;
  }
  out << "{\n";
  out << "  \"quality_score\": " << std::fixed << std::setprecision(3) << stats.qualityScore << ",\n";
  out << "  \"decoded_count\": " << stats.decodedCount << ",\n";
  out << "  \"track_count\": " << stats.trackCount << ",\n";
  out << "  \"sample_rate\": " << wav.sampleRate << ",\n";
  out << "  \"decode_threads\": " << args.cfg.decodeThreads << ",\n";
  out << "  \"seed\": " << args.cfg.deterministicSeed << ",\n";
  out << "  \"mode_stream\": " << (args.streamMode ? "true" : "false") << ",\n";
  out << "  \"results\": [\n";
  for (std::size_t i = 0; i < results.size(); ++i) {
    const auto& r = results[i];
    out << "    {\"track_id\":" << r.trackId << ",\"freq_hz\":" << std::fixed
        << std::setprecision(2) << r.freqHz << ",\"id\":\"" << EscapeJson(r.plausibleId)
        << "\",\"score\":" << std::setprecision(3) << r.compositeScore << "}";
    if (i + 1 < results.size()) {
      out << ',';
    }
    out << "\n";
  }
  out << "  ]\n";
  out << "}\n";

  const std::filesystem::path snipDir = baseDir / "snippets";
  std::filesystem::create_directories(snipDir, ec);
  if (ec) {
    *error = "Cannot create snippets directory: " + snipDir.string();
    return false;
  }

  auto writeLe16 = [](std::ofstream& o, unsigned int v) {
    const char b[2] = {static_cast<char>(v & 0xffU), static_cast<char>((v >> 8) & 0xffU)};
    o.write(b, 2);
  };
  auto writeLe32 = [](std::ofstream& o, unsigned int v) {
    const char b[4] = {
        static_cast<char>(v & 0xffU),
        static_cast<char>((v >> 8) & 0xffU),
        static_cast<char>((v >> 16) & 0xffU),
        static_cast<char>((v >> 24) & 0xffU),
    };
    o.write(b, 4);
  };
  auto writeSnippetWav = [&](const std::string& path, int sr, const std::vector<float>& s,
                             std::size_t i0, std::size_t i1) {
    if (i1 <= i0 || i0 >= s.size()) {
      return;
    }
    const std::size_t end = std::min<std::size_t>(i1, s.size());
    const std::size_t n = end - i0;
    std::ofstream o(path, std::ios::binary);
    if (!o) {
      return;
    }
    const unsigned int dataBytes = static_cast<unsigned int>(n * 2U);
    o.write("RIFF", 4);
    writeLe32(o, 36U + dataBytes);
    o.write("WAVE", 4);
    o.write("fmt ", 4);
    writeLe32(o, 16);
    writeLe16(o, 1);
    writeLe16(o, 1);
    writeLe32(o, static_cast<unsigned int>(sr));
    writeLe32(o, static_cast<unsigned int>(sr * 2));
    writeLe16(o, 2);
    writeLe16(o, 16);
    o.write("data", 4);
    writeLe32(o, dataBytes);
    for (std::size_t i = i0; i < end; ++i) {
      const float x = std::max(-1.0f, std::min(1.0f, s[i]));
      const int iv = static_cast<int>(std::round(x * 32767.0f));
      const unsigned int uv = static_cast<unsigned int>(static_cast<unsigned short>(iv & 0xffff));
      writeLe16(o, uv);
    }
  };

  const std::filesystem::path cuePath = snipDir / "snippet_index.csv";
  std::ofstream cue(cuePath.string());
  if (cue) {
    cue << "track_id,start_sec,end_sec,freq_hz,id,score,snippet_wav\n";
    for (const auto& r : results) {
      const std::string wavName = "track_" + std::to_string(r.trackId) + "_" +
                                  std::to_string(static_cast<int>(std::round(r.startSec * 1000.0f))) +
                                  "_" +
                                  std::to_string(static_cast<int>(std::round(r.endSec * 1000.0f))) + ".wav";
      const std::filesystem::path wavPath = snipDir / wavName;
      const std::size_t i0 = wav.sampleRate > 0
                                 ? static_cast<std::size_t>(std::max(0.0f, r.startSec) * wav.sampleRate)
                                 : 0;
      const std::size_t i1 = wav.sampleRate > 0
                                 ? static_cast<std::size_t>(std::max(0.0f, r.endSec) * wav.sampleRate)
                                 : 0;
      writeSnippetWav(wavPath.string(), std::max(1, wav.sampleRate), wav.samples, i0, i1);
      cue << r.trackId << ',' << std::fixed << std::setprecision(3) << r.startSec << ',' << r.endSec
          << ',' << std::setprecision(2) << r.freqHz << ',' << r.plausibleId << ','
          << std::setprecision(3) << r.compositeScore << ',' << wavName << '\n';
    }
    if (results.empty() && wav.sampleRate > 0 && !wav.samples.empty()) {
      const float endSec = std::min(5.0f,
                                    static_cast<float>(wav.samples.size()) /
                                        static_cast<float>(wav.sampleRate));
      const std::string wavName = "raw_head.wav";
      const std::filesystem::path wavPath = snipDir / wavName;
      const std::size_t i0 = 0;
      const std::size_t i1 = static_cast<std::size_t>(endSec * wav.sampleRate);
      writeSnippetWav(wavPath.string(), wav.sampleRate, wav.samples, i0, i1);
      cue << "0,0.000," << std::fixed << std::setprecision(3) << endSec
          << ",0.00,,0.000," << wavName << '\n';
    }
  }
  return true;
}

void AppendDiagnostics(const std::optional<std::string>& path, const std::string& msg) {
  if (!path.has_value()) {
    return;
  }
  std::ofstream out(*path, std::ios::app);
  if (!out) {
    return;
  }
  out << msg << '\n';
}

bool ApplyConfigJson(const std::string& path, CliArgs* args, std::string* error) {
  if (!args || !error) {
    return false;
  }
  std::ifstream in(path);
  if (!in) {
    *error = "Cannot open config file: " + path;
    return false;
  }
  std::stringstream buf;
  buf << in.rdbuf();
  const std::string txt = buf.str();

  auto parseString = [&](const std::string& key, std::string* out) {
    const std::string k = "\"" + key + "\"";
    const std::size_t p = txt.find(k);
    if (p == std::string::npos) return;
    const std::size_t q1 = txt.find('"', txt.find(':', p) + 1);
    if (q1 == std::string::npos) return;
    const std::size_t q2 = txt.find('"', q1 + 1);
    if (q2 == std::string::npos) return;
    *out = txt.substr(q1 + 1, q2 - q1 - 1);
  };
  auto parseFloat = [&](const std::string& key, float* out) {
    const std::string k = "\"" + key + "\"";
    const std::size_t p = txt.find(k);
    if (p == std::string::npos) return;
    const std::size_t c = txt.find(':', p);
    if (c == std::string::npos) return;
    std::size_t e = c + 1;
    while (e < txt.size() && (txt[e] == ' ' || txt[e] == '\t')) ++e;
    std::size_t z = e;
    while (z < txt.size() && ((txt[z] >= '0' && txt[z] <= '9') || txt[z] == '-' || txt[z] == '.')) {
      ++z;
    }
    if (z > e) {
      *out = std::strtof(txt.substr(e, z - e).c_str(), nullptr);
    }
  };
  auto parseBool = [&](const std::string& key, bool* out) {
    const std::string k = "\"" + key + "\"";
    const std::size_t p = txt.find(k);
    if (p == std::string::npos) return;
    const std::size_t c = txt.find(':', p);
    if (c == std::string::npos) return;
    std::size_t e = c + 1;
    while (e < txt.size() && (txt[e] == ' ' || txt[e] == '\t')) ++e;
    if (txt.compare(e, 4, "true") == 0) {
      *out = true;
    } else if (txt.compare(e, 5, "false") == 0) {
      *out = false;
    }
  };
  auto parseInt = [&](const std::string& key, int* out) {
    const std::string k = "\"" + key + "\"";
    const std::size_t p = txt.find(k);
    if (p == std::string::npos) return;
    const std::size_t c = txt.find(':', p);
    if (c == std::string::npos) return;
    std::size_t e = c + 1;
    while (e < txt.size() && (txt[e] == ' ' || txt[e] == '\t')) ++e;
    std::size_t z = e;
    while (z < txt.size() && ((txt[z] >= '0' && txt[z] <= '9') || txt[z] == '-')) ++z;
    if (z > e) {
      *out = std::atoi(txt.substr(e, z - e).c_str());
    }
  };
  auto parseUInt = [&](const std::string& key, unsigned int* out) {
    int tmp = static_cast<int>(*out);
    parseInt(key, &tmp);
    if (tmp >= 0) {
      *out = static_cast<unsigned int>(tmp);
    }
  };

  parseInt("decode_threads", &args->cfg.decodeThreads);
  parseUInt("seed", &args->cfg.deterministicSeed);
  parseFloat("min_confidence", &args->minConfidence);
  parseInt("stream_poll_ms", &args->streamPollMs);
  parseInt("stream_iterations", &args->streamIterations);
  parseInt("stream_tail_seconds", &args->streamTailSeconds);
  parseBool("stream", &args->streamMode);

  std::string mode;
  parseString("mode", &mode);
  if (!mode.empty() && !ApplyModePreset(mode, &args->cfg, error)) {
    return false;
  }
  std::string profile;
  parseString("profile", &profile);
  if (!profile.empty() && !ApplyPerformanceProfile(profile, &args->cfg, error)) {
    return false;
  }

  std::string outputJson;
  parseString("output_json", &outputJson);
  if (!outputJson.empty()) {
    args->outputJsonPath = outputJson;
  }
  std::string diagLog;
  parseString("diag_log", &diagLog);
  if (!diagLog.empty()) {
    args->diagnosticsLogPath = diagLog;
  }
  return true;
}

bool WriteMetrics(const std::string& path, const ndb::DecodeStats& s, std::string* error) {
  std::ofstream out(path);
  if (!out) {
    *error = "Cannot write metrics file: " + path;
    return false;
  }
  out << "{\n";
  out << "  \"input_sample_rate\": " << s.inputSampleRate << ",\n";
  out << "  \"work_sample_rate\": " << s.workSampleRate << ",\n";
  out << "  \"input_samples\": " << s.inputSamples << ",\n";
  out << "  \"work_samples\": " << s.workSamples << ",\n";
  out << "  \"frame_count\": " << s.frameCount << ",\n";
  out << "  \"candidate_bin_count\": " << s.candidateBinCount << ",\n";
  out << "  \"track_count\": " << s.trackCount << ",\n";
  out << "  \"filtered_by_frequency\": " << s.filteredByFrequency << ",\n";
  out << "  \"clustered_count\": " << s.clusteredCount << ",\n";
  out << "  \"dedup_count\": " << s.dedupCount << ",\n";
  out << "  \"plausible_id_rejected\": " << s.plausibleIdRejected << ",\n";
  out << "  \"strict_rejected\": " << s.strictRejected << ",\n";
  out << "  \"decoded_count\": " << s.decodedCount << ",\n";
  out << "  \"mean_confidence\": " << std::fixed << std::setprecision(6) << s.meanConfidence
      << ",\n";
  out << "  \"median_confidence\": " << std::fixed << std::setprecision(6) << s.medianConfidence
      << ",\n";
  out << "  \"max_confidence\": " << std::fixed << std::setprecision(6) << s.maxConfidence
      << ",\n";
  out << "  \"decode_ratio\": " << std::fixed << std::setprecision(6) << s.decodeRatio << ",\n";
  out << "  \"id_like_token_ratio\": " << std::fixed << std::setprecision(6)
      << s.idLikeTokenRatio << ",\n";
  out << "  \"plausible_id_ratio\": " << std::fixed << std::setprecision(6)
      << s.plausibleIdRatio << ",\n";
  out << "  \"mean_composite_score\": " << std::fixed << std::setprecision(6)
      << s.meanCompositeScore << ",\n";
  out << "  \"quality_score\": " << std::fixed << std::setprecision(3) << s.qualityScore << "\n";
  out << "}\n";
  return true;
}

void PrintMetricsSummary(const ndb::DecodeStats& s) {
  std::cout << "\nQuality Metrics\n"
            << "  quality_score      : " << std::fixed << std::setprecision(2) << s.qualityScore
            << "/100\n"
            << "  mean_confidence    : " << std::setprecision(3) << s.meanConfidence << "\n"
            << "  median_confidence  : " << std::setprecision(3) << s.medianConfidence << "\n"
            << "  decode_ratio       : " << std::setprecision(3) << s.decodeRatio << "\n"
            << "  id_like_token_ratio: " << std::setprecision(3) << s.idLikeTokenRatio << "\n"
            << "  plausible_id_ratio : " << std::setprecision(3) << s.plausibleIdRatio << "\n"
            << "  mean_composite     : " << std::setprecision(3) << s.meanCompositeScore << "\n"
            << "  strict_rejected    : " << s.strictRejected << "\n"
            << "  tracks(decoded/all): " << s.decodedCount << "/" << s.trackCount << "\n";
}

bool IsHelpToken(const std::string& token) {
  return token == "/h" || token == "/H" || token == "/?" || token == "-h" || token == "--help";
}

bool ParseInt(const std::string& text, int* out) {
  if (!out) {
    return false;
  }
  char* end = nullptr;
  const long v = std::strtol(text.c_str(), &end, 10);
  if (end == text.c_str() || *end != '\0') {
    return false;
  }
  *out = static_cast<int>(v);
  return true;
}

bool ParseFloat(const std::string& text, float* out) {
  if (!out) {
    return false;
  }
  char* end = nullptr;
  const float v = std::strtof(text.c_str(), &end);
  if (end == text.c_str() || *end != '\0') {
    return false;
  }
  *out = v;
  return true;
}

bool ParseArgs(int argc, char** argv, CliArgs* out, std::string* error) {
  if (!out || !error) {
    return false;
  }
  *out = CliArgs();
  *error = "";

  std::vector<std::string> positional;
  for (int i = 1; i < argc; ++i) {
    const std::string token = argv[i];
    if (IsHelpToken(token)) {
      out->help = true;
      continue;
    }

    auto parseOptionValue = [&](const std::string& optName, std::string* value) -> bool {
      if (i + 1 >= argc) {
        *error = "Missing value for option " + optName;
        return false;
      }
      *value = argv[++i];
      return true;
    };

    if (token == "--fft") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseInt(value, &out->cfg.fftSize)) {
        *error = "Invalid integer for --fft";
        return false;
      }
      continue;
    }
    if (token == "--hop") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseInt(value, &out->cfg.hopSize)) {
        *error = "Invalid integer for --hop";
        return false;
      }
      continue;
    }
    if (token == "--mad-factor") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.madFactor)) {
        *error = "Invalid float for --mad-factor";
        return false;
      }
      continue;
    }
    if (token == "--guard-bins") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseInt(value, &out->cfg.guardBins)) {
        *error = "Invalid integer for --guard-bins";
        return false;
      }
      continue;
    }
    if (token == "--max-step-bins") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseInt(value, &out->cfg.maxTrackStepBins)) {
        *error = "Invalid integer for --max-step-bins";
        return false;
      }
      continue;
    }
    if (token == "--min-track-frames") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseInt(value, &out->cfg.minTrackFrames)) {
        *error = "Invalid integer for --min-track-frames";
        return false;
      }
      continue;
    }
    if (token == "--sustain-penalty") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.sustainPenalty)) {
        *error = "Invalid float for --sustain-penalty";
        return false;
      }
      continue;
    }
    if (token == "--amtc-lite") {
      out->cfg.useAmtcFull = false;
      continue;
    }
    if (token == "--amtc-full") {
      out->cfg.useAmtcFull = true;
      continue;
    }
    if (token == "--max-track-gap") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseInt(value, &out->cfg.maxTrackGapFrames)) {
        *error = "Invalid integer for --max-track-gap";
        return false;
      }
      continue;
    }
    if (token == "--envelope-alpha") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.envelopeAlpha)) {
        *error = "Invalid float for --envelope-alpha";
        return false;
      }
      continue;
    }
    if (token == "--no-band-limit") {
      out->cfg.enableBandLimit = false;
      continue;
    }
    if (token == "--band-limit") {
      out->cfg.enableBandLimit = true;
      continue;
    }
    if (token == "--band-low-hz") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.bandLowHz)) {
        *error = "Invalid float for --band-low-hz";
        return false;
      }
      continue;
    }
    if (token == "--band-high-hz") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.bandHighHz)) {
        *error = "Invalid float for --band-high-hz";
        return false;
      }
      continue;
    }
    if (token == "--no-auto-notch") {
      out->cfg.enableAutoNotch = false;
      continue;
    }
    if (token == "--auto-notch") {
      out->cfg.enableAutoNotch = true;
      continue;
    }
    if (token == "--auto-notch-max") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseInt(value, &out->cfg.autoNotchMaxCount)) {
        *error = "Invalid integer for --auto-notch-max";
        return false;
      }
      continue;
    }
    if (token == "--auto-notch-snr-db") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.autoNotchSnrDb)) {
        *error = "Invalid float for --auto-notch-snr-db";
        return false;
      }
      continue;
    }
    if (token == "--no-impulse-blanker") {
      out->cfg.enableImpulseBlanker = false;
      continue;
    }
    if (token == "--impulse-blanker") {
      out->cfg.enableImpulseBlanker = true;
      continue;
    }
    if (token == "--impulse-sigma") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.impulseBlankerSigma)) {
        *error = "Invalid float for --impulse-sigma";
        return false;
      }
      continue;
    }
    if (token == "--impulse-window") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseInt(value, &out->cfg.impulseBlankerHalfWindow)) {
        *error = "Invalid integer for --impulse-window";
        return false;
      }
      continue;
    }
    if (token == "--threshold-k") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.thresholdK)) {
        *error = "Invalid float for --threshold-k";
        return false;
      }
      continue;
    }
    if (token == "--no-cfar2d") {
      out->cfg.enableCfar2d = false;
      continue;
    }
    if (token == "--cfar2d") {
      out->cfg.enableCfar2d = true;
      continue;
    }
    if (token == "--cfar-train-time") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseInt(value, &out->cfg.cfarTrainTime)) {
        *error = "Invalid integer for --cfar-train-time";
        return false;
      }
      continue;
    }
    if (token == "--cfar-guard-time") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseInt(value, &out->cfg.cfarGuardTime)) {
        *error = "Invalid integer for --cfar-guard-time";
        return false;
      }
      continue;
    }
    if (token == "--cfar-train-freq") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseInt(value, &out->cfg.cfarTrainFreq)) {
        *error = "Invalid integer for --cfar-train-freq";
        return false;
      }
      continue;
    }
    if (token == "--cfar-guard-freq") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseInt(value, &out->cfg.cfarGuardFreq)) {
        *error = "Invalid integer for --cfar-guard-freq";
        return false;
      }
      continue;
    }
    if (token == "--cfar-scale") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.cfarScale)) {
        *error = "Invalid float for --cfar-scale";
        return false;
      }
      continue;
    }
    if (token == "--min-dot-ms") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseInt(value, &out->cfg.minDotMs)) {
        *error = "Invalid integer for --min-dot-ms";
        return false;
      }
      continue;
    }
    if (token == "--max-dot-ms") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseInt(value, &out->cfg.maxDotMs)) {
        *error = "Invalid integer for --max-dot-ms";
        return false;
      }
      continue;
    }
    if (token == "--target-sr") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseInt(value, &out->cfg.targetSampleRate)) {
        *error = "Invalid integer for --target-sr";
        return false;
      }
      continue;
    }
    if (token == "--max-seconds") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseInt(value, &out->cfg.maxAnalyzeSeconds)) {
        *error = "Invalid integer for --max-seconds";
        return false;
      }
      continue;
    }
    if (token == "--rf-input") {
      out->cfg.rfFrequencyInput = true;
      continue;
    }
    if (token == "--ndb-min-hz") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.ndbRfMinHz)) {
        *error = "Invalid float for --ndb-min-hz";
        return false;
      }
      continue;
    }
    if (token == "--ndb-max-hz") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.ndbRfMaxHz)) {
        *error = "Invalid float for --ndb-max-hz";
        return false;
      }
      continue;
    }
    if (token == "--audio-min-hz") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.audioMinHz)) {
        *error = "Invalid float for --audio-min-hz";
        return false;
      }
      continue;
    }
    if (token == "--audio-max-hz") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.audioMaxHz)) {
        *error = "Invalid float for --audio-max-hz";
        return false;
      }
      continue;
    }
    if (token == "--cluster-freq-tol") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.clusterFreqTolHz)) {
        *error = "Invalid float for --cluster-freq-tol";
        return false;
      }
      continue;
    }
    if (token == "--cluster-gap-sec") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.clusterGapSec)) {
        *error = "Invalid float for --cluster-gap-sec";
        return false;
      }
      continue;
    }
    if (token == "--dedup-freq-tol") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.dedupFreqTolHz)) {
        *error = "Invalid float for --dedup-freq-tol";
        return false;
      }
      continue;
    }
    if (token == "--cochannel-sep") {
      out->cfg.enableCochannelSeparation = true;
      continue;
    }
    if (token == "--no-cochannel-sep") {
      out->cfg.enableCochannelSeparation = false;
      continue;
    }
    if (token == "--cochannel-max-tracks") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseInt(value, &out->cfg.cochannelMaxTracks)) {
        *error = "Invalid integer for --cochannel-max-tracks";
        return false;
      }
      continue;
    }
    if (token == "--cochannel-max-gap") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseInt(value, &out->cfg.cochannelMaxGapFrames)) {
        *error = "Invalid integer for --cochannel-max-gap";
        return false;
      }
      continue;
    }
    if (token == "--cochannel-max-step-hz") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.cochannelMaxStepHz)) {
        *error = "Invalid float for --cochannel-max-step-hz";
        return false;
      }
      continue;
    }
    if (token == "--require-plausible-id") {
      out->cfg.requirePlausibleId = true;
      continue;
    }
    if (token == "--allow-any-id") {
      out->cfg.requirePlausibleId = false;
      continue;
    }
    if (token == "--plausible-id-min") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.plausibleIdMinScore)) {
        *error = "Invalid float for --plausible-id-min";
        return false;
      }
      continue;
    }
    if (token == "--strict-beacon") {
      out->cfg.strictBeaconMode = true;
      continue;
    }
    if (token == "--strict-min-repeats") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseInt(value, &out->cfg.strictMinRepeats)) {
        *error = "Invalid integer for --strict-min-repeats";
        return false;
      }
      continue;
    }
    if (token == "--decoder-model") {
      std::string value;
      if (!parseOptionValue(token, &value)) {
        return false;
      }
      if (value != "auto" && value != "hmm" && value != "hsmm") {
        *error = "Invalid value for --decoder-model (use: auto, hmm, hsmm)";
        return false;
      }
      out->cfg.decoderModel = value;
      continue;
    }
    if (token == "--hmm-on-intra") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.hmmTransOnToIntra)) {
        *error = "Invalid float for --hmm-on-intra";
        return false;
      }
      continue;
    }
    if (token == "--hmm-on-char") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.hmmTransOnToChar)) {
        *error = "Invalid float for --hmm-on-char";
        return false;
      }
      continue;
    }
    if (token == "--hmm-on-word") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.hmmTransOnToWord)) {
        *error = "Invalid float for --hmm-on-word";
        return false;
      }
      continue;
    }
    if (token == "--hmm-off-dot") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.hmmTransOffToDot)) {
        *error = "Invalid float for --hmm-off-dot";
        return false;
      }
      continue;
    }
    if (token == "--hmm-off-dash") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.hmmTransOffToDash)) {
        *error = "Invalid float for --hmm-off-dash";
        return false;
      }
      continue;
    }
    if (token == "--hmm-sigma-dot") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.hmmSigmaOnDot)) {
        *error = "Invalid float for --hmm-sigma-dot";
        return false;
      }
      continue;
    }
    if (token == "--hmm-sigma-dash") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.hmmSigmaOnDash)) {
        *error = "Invalid float for --hmm-sigma-dash";
        return false;
      }
      continue;
    }
    if (token == "--hmm-sigma-intra") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.hmmSigmaOffIntra)) {
        *error = "Invalid float for --hmm-sigma-intra";
        return false;
      }
      continue;
    }
    if (token == "--hmm-sigma-char") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.hmmSigmaOffChar)) {
        *error = "Invalid float for --hmm-sigma-char";
        return false;
      }
      continue;
    }
    if (token == "--hmm-sigma-word") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.hmmSigmaOffWord)) {
        *error = "Invalid float for --hmm-sigma-word";
        return false;
      }
      continue;
    }
    if (token == "--hsmm-on-intra") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.hsmmTransOnToIntra)) {
        *error = "Invalid float for --hsmm-on-intra";
        return false;
      }
      continue;
    }
    if (token == "--hsmm-on-char") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.hsmmTransOnToChar)) {
        *error = "Invalid float for --hsmm-on-char";
        return false;
      }
      continue;
    }
    if (token == "--hsmm-on-word") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.hsmmTransOnToWord)) {
        *error = "Invalid float for --hsmm-on-word";
        return false;
      }
      continue;
    }
    if (token == "--hsmm-off-dot") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.hsmmTransOffToDot)) {
        *error = "Invalid float for --hsmm-off-dot";
        return false;
      }
      continue;
    }
    if (token == "--hsmm-off-dash") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.hsmmTransOffToDash)) {
        *error = "Invalid float for --hsmm-off-dash";
        return false;
      }
      continue;
    }
    if (token == "--hsmm-sigma-dot") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.hsmmSigmaOnDot)) {
        *error = "Invalid float for --hsmm-sigma-dot";
        return false;
      }
      continue;
    }
    if (token == "--hsmm-sigma-dash") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.hsmmSigmaOnDash)) {
        *error = "Invalid float for --hsmm-sigma-dash";
        return false;
      }
      continue;
    }
    if (token == "--hsmm-sigma-intra") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.hsmmSigmaOffIntra)) {
        *error = "Invalid float for --hsmm-sigma-intra";
        return false;
      }
      continue;
    }
    if (token == "--hsmm-sigma-char") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.hsmmSigmaOffChar)) {
        *error = "Invalid float for --hsmm-sigma-char";
        return false;
      }
      continue;
    }
    if (token == "--hsmm-sigma-word") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.hsmmSigmaOffWord)) {
        *error = "Invalid float for --hsmm-sigma-word";
        return false;
      }
      continue;
    }
    if (token == "--hsmm-tail-mix") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.hsmmDurationTailMix)) {
        *error = "Invalid float for --hsmm-tail-mix";
        return false;
      }
      continue;
    }
    if (token == "--hsmm-time-gain") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.hsmmTimeTransitionGain)) {
        *error = "Invalid float for --hsmm-time-gain";
        return false;
      }
      continue;
    }
    if (token == "--confidence-calibration") {
      std::string value;
      if (!parseOptionValue(token, &value)) {
        return false;
      }
      if (value != "none" && value != "platt" && value != "isotonic") {
        *error = "Invalid value for --confidence-calibration (use: none, platt, isotonic)";
        return false;
      }
      out->cfg.confidenceCalibration = value;
      continue;
    }
    if (token == "--platt-a") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.plattA)) {
        *error = "Invalid float for --platt-a";
        return false;
      }
      continue;
    }
    if (token == "--platt-b") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.plattB)) {
        *error = "Invalid float for --platt-b";
        return false;
      }
      continue;
    }
    if (token == "--freq-prior-file") {
      std::string value;
      if (!parseOptionValue(token, &value)) {
        return false;
      }
      out->cfg.enableFreqPriors = true;
      out->cfg.freqPriorFile = value;
      continue;
    }
    if (token == "--freq-prior-tol") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.freqPriorTolHz)) {
        *error = "Invalid float for --freq-prior-tol";
        return false;
      }
      continue;
    }
    if (token == "--require-prior-match") {
      out->cfg.requirePriorMatch = true;
      out->cfg.enableFreqPriors = true;
      continue;
    }
    if (token == "--decode-threads") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseInt(value, &out->cfg.decodeThreads)) {
        *error = "Invalid integer for --decode-threads";
        return false;
      }
      continue;
    }
    if (token == "--seed") {
      std::string value;
      int iv = 0;
      if (!parseOptionValue(token, &value) || !ParseInt(value, &iv) || iv < 0) {
        *error = "Invalid non-negative integer for --seed";
        return false;
      }
      out->cfg.deterministicSeed = static_cast<unsigned int>(iv);
      continue;
    }
    if (token == "--profile") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ApplyPerformanceProfile(value, &out->cfg, error)) {
        return false;
      }
      continue;
    }
    if (token == "--config") {
      std::string value;
      if (!parseOptionValue(token, &value)) {
        return false;
      }
      out->configPath = value;
      if (!ApplyConfigJson(value, out, error)) {
        return false;
      }
      continue;
    }
    if (token == "--stream") {
      out->streamMode = true;
      continue;
    }
    if (token == "--stream-poll-ms") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseInt(value, &out->streamPollMs)) {
        *error = "Invalid integer for --stream-poll-ms";
        return false;
      }
      continue;
    }
    if (token == "--stream-iterations") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseInt(value, &out->streamIterations)) {
        *error = "Invalid integer for --stream-iterations";
        return false;
      }
      continue;
    }
    if (token == "--stream-tail-seconds") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseInt(value, &out->streamTailSeconds)) {
        *error = "Invalid integer for --stream-tail-seconds";
        return false;
      }
      continue;
    }
    if (token == "--output-json") {
      std::string value;
      if (!parseOptionValue(token, &value)) {
        return false;
      }
      out->outputJsonPath = value;
      continue;
    }
    if (token == "--diag-log") {
      std::string value;
      if (!parseOptionValue(token, &value)) {
        return false;
      }
      out->diagnosticsLogPath = value;
      continue;
    }
    if (token == "--dashboard") {
      std::string value;
      if (!parseOptionValue(token, &value)) {
        return false;
      }
      if (value != "rich") {
        *error = "Invalid value for --dashboard (use: rich)";
        return false;
      }
      out->dashboardMode = value;
      continue;
    }
    if (token == "--session-export") {
      std::string value;
      if (!parseOptionValue(token, &value)) {
        return false;
      }
      out->sessionExportDir = value;
      continue;
    }
    if (token == "--mode") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ApplyModePreset(value, &out->cfg, error)) {
        if (error->empty()) {
          *error = "Invalid value for --mode";
        }
        return false;
      }
      continue;
    }
    if (token == "--min-confidence") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->minConfidence)) {
        *error = "Invalid float for --min-confidence";
        return false;
      }
      continue;
    }
    if (token == "--metrics") {
      std::string value;
      if (!parseOptionValue(token, &value)) {
        return false;
      }
      out->metricsPath = value;
      continue;
    }
    if (token == "--no-progress") {
      out->progress = false;
      continue;
    }
    if (token == "--quiet") {
      out->quiet = true;
      continue;
    }

    if (!token.empty() && token[0] == '-') {
      *error = "Unknown option: " + token;
      return false;
    }

    positional.push_back(token);
  }

  if (out->help) {
    return true;
  }

  if (positional.empty()) {
    *error = "Missing input WAV file";
    return false;
  }
  if (positional.size() > 2) {
    *error = "Too many positional arguments";
    return false;
  }
  out->inputPath = positional[0];
  if (positional.size() == 2) {
    out->outputPath = positional[1];
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  if (argc == 1) {
    return RunGuiApplication(GetModuleHandleW(nullptr), SW_SHOWDEFAULT);
  }
#endif

  if (argc < 2) {
    PrintUsage();
    return 1;
  }

  CliArgs args;
  std::string error;
  if (!ParseArgs(argc, argv, &args, &error)) {
    std::cerr << error << "\n\n";
    PrintUsage();
    return 1;
  }
  if (args.help) {
    PrintUsage();
    return 0;
  }

  AppendDiagnostics(args.diagnosticsLogPath,
                    "startup decode_threads=" + std::to_string(args.cfg.decodeThreads) +
                        " seed=" + std::to_string(args.cfg.deterministicSeed) +
                        " stream=" + std::string(args.streamMode ? "true" : "false"));

  auto runOneDecode = [&](int iter) -> int {
    ndb::WavData wav;
    if (!ndb::ReadWavMono16(args.inputPath, &wav, &error)) {
      std::cerr << error << '\n';
      AppendDiagnostics(args.diagnosticsLogPath, "read_failed iter=" + std::to_string(iter) +
                                                    " err=" + error);
      return 2;
    }
    if (args.streamTailSeconds > 0 && wav.sampleRate > 0) {
      const std::size_t keep = static_cast<std::size_t>(args.streamTailSeconds) *
                               static_cast<std::size_t>(wav.sampleRate);
      if (wav.samples.size() > keep) {
        wav.samples.erase(wav.samples.begin(), wav.samples.end() - static_cast<std::ptrdiff_t>(keep));
      }
    }

    int lastPercent = -1;
    ndb::DecodeStats stats;
    auto progressCb = [&](int percent, const std::string& stage) {
      if (!args.progress || args.quiet) {
        return;
      }
      percent = std::max(0, std::min(100, percent));
      if (percent == lastPercent && percent != 100) {
        return;
      }
      lastPercent = percent;
      std::cout << "[" << std::setw(3) << percent << "%] " << stage;
      if (args.streamMode) {
        std::cout << " (iter " << iter << ")";
      }
      std::cout << '\n';
    };

    auto results = ndb::DecodeNdbFromWav(wav.samples, wav.sampleRate, args.cfg, &stats, progressCb);
    if (args.minConfidence > 0.0f) {
      std::vector<ndb::DecodeResult> filtered;
      filtered.reserve(results.size());
      for (const auto& r : results) {
        if (r.confidence >= args.minConfidence) {
          filtered.push_back(r);
        }
      }
      results = std::move(filtered);
    }

    if (args.outputPath.has_value()) {
      if (!WriteCsv(*args.outputPath, results, &error)) {
        std::cerr << error << '\n';
        return 3;
      }
      if (!args.quiet) {
        std::cout << "Written " << results.size() << " rows to " << *args.outputPath << '\n';
      }
    } else {
      if (!args.quiet) {
        std::cout << "Detected candidates: " << results.size() << '\n';
      }
      for (const auto& r : results) {
        std::cout << "track=" << r.trackId << " freq=" << std::fixed << std::setprecision(2)
                  << r.freqHz << "Hz id=\"" << r.plausibleId << "\" pid="
                  << std::setprecision(3) << r.plausibleIdScore << " conf=" << r.confidence
                  << " raw=" << r.confidenceRaw
                  << " cal=" << r.confidenceCalibrated
                  << " score=" << r.compositeScore << " hits=" << r.hitCount
                  << " t=[" << r.startSec << "," << r.endSec << "]\n";
      }
    }

    if (args.outputJsonPath.has_value()) {
      if (!WriteJson(*args.outputJsonPath, results, stats, &error)) {
        std::cerr << error << '\n';
        return 5;
      }
    }

    if (args.dashboardMode.has_value() && *args.dashboardMode == "rich") {
      PrintRichDashboard(results, stats);
    }

    if (args.sessionExportDir.has_value()) {
      if (!ExportSessionEvidence(*args.sessionExportDir, wav, results, stats, args, &error)) {
        std::cerr << error << '\n';
        return 6;
      }
    }

    if (args.metricsPath.has_value()) {
      if (!WriteMetrics(*args.metricsPath, stats, &error)) {
        std::cerr << error << '\n';
        return 4;
      }
      if (!args.quiet) {
        std::cout << "Metrics written to " << *args.metricsPath << '\n';
      }
    }

    AppendDiagnostics(args.diagnosticsLogPath,
                      "iter=" + std::to_string(iter) + " rows=" + std::to_string(results.size()) +
                          " quality=" + std::to_string(stats.qualityScore));

    if (args.quiet) {
      std::cout << "rows=" << results.size() << " quality=" << std::fixed << std::setprecision(2)
                << stats.qualityScore << " mean_conf=" << std::setprecision(3)
                << stats.meanConfidence << " decode_ratio=" << stats.decodeRatio << '\n';
    } else {
      PrintMetricsSummary(stats);
    }
    return 0;
  };

  const int iters = args.streamMode ? std::max(1, args.streamIterations) : 1;
  for (int i = 1; i <= iters; ++i) {
    const int rc = runOneDecode(i);
    if (rc != 0) {
      return rc;
    }
    if (args.streamMode && i < iters) {
      std::this_thread::sleep_for(std::chrono::milliseconds(std::max(1, args.streamPollMs)));
    }
  }

  return 0;
}

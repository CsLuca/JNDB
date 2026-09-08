#include "ndb_decoder.hpp"
#include "wav.hpp"

#ifdef _WIN32
#include "gui_win32.hpp"
#include <windows.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

struct CliArgs {
  bool help = false;
  bool progress = true;
  bool quiet = false;
  std::string inputPath;
  std::optional<std::string> outputPath;
  std::optional<std::string> metricsPath;
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
  *error = "Invalid value for --mode (use: default, strict-dx, relaxed)";
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
      << "  --envelope-alpha <float>   Envelope smoother alpha (default: 0.05)\n"
      << "  --threshold-k <float>      MAD K for OOK threshold (default: 2.5)\n"
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
      << "  --require-plausible-id     Keep only plausible cyclic 2-3 char beacon IDs\n"
      << "  --allow-any-id             Disable plausible ID filter\n"
      << "  --plausible-id-min <float> Plausible ID score threshold (default: 0.30)\n"
      << "  --strict-beacon            Require repeated ID hits before emitting\n"
      << "  --strict-min-repeats <int> Minimum hit_count for strict mode (default: 3)\n"
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
      << "  --mode <preset>            Preset: default | strict-dx | relaxed\n"
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
      << "  ndb_decode capture.wav out.csv --mode strict-dx\n";
}

bool WriteCsv(const std::string& path, const std::vector<ndb::DecodeResult>& results,
              std::string* error) {
  std::ofstream out(path);
  if (!out) {
    *error = "Cannot write output file: " + path;
    return false;
  }
  out << "track_id,freq_hz,text,plausible_id,plausible_id_score,confidence,start_sec,end_sec,first_seen_sec,last_seen_sec,hit_count,composite_score,energy_score,continuity_score,freq_stability_score,keying_periodicity_score\n";
  for (const auto& r : results) {
    out << r.trackId << ',' << std::fixed << std::setprecision(2) << r.freqHz << ',' << '"'
        << r.text << '"' << ',' << '"' << r.plausibleId << '"' << ','
        << std::setprecision(3) << r.plausibleIdScore << ',' << r.confidence << ','
        << std::setprecision(3) << r.startSec << ',' << r.endSec << ','
        << r.firstSeenSec << ',' << r.lastSeenSec << ',' << r.hitCount << ','
        << r.compositeScore << ',' << r.energyScore << ',' << r.continuityScore << ','
        << r.freqStabilityScore << ',' << r.keyingPeriodicityScore << '\n';
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
    if (token == "--envelope-alpha") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.envelopeAlpha)) {
        *error = "Invalid float for --envelope-alpha";
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

  ndb::WavData wav;
  if (!ndb::ReadWavMono16(args.inputPath, &wav, &error)) {
    std::cerr << error << '\n';
    return 2;
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
    std::cout << "[" << std::setw(3) << percent << "%] " << stage << '\n';
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
                << " score=" << r.compositeScore << " hits=" << r.hitCount
                << " t=[" << r.startSec << "," << r.endSec << "]\n";
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

  if (args.quiet) {
    std::cout << "rows=" << results.size() << " quality=" << std::fixed << std::setprecision(2)
              << stats.qualityScore << " mean_conf=" << std::setprecision(3)
              << stats.meanConfidence << " decode_ratio=" << stats.decodeRatio << '\n';
  } else {
    PrintMetricsSummary(stats);
  }

  return 0;
}

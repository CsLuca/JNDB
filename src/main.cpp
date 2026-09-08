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
      << "  ndb_decode capture.wav out.csv --metrics run_metrics.json\n";
}

bool WriteCsv(const std::string& path, const std::vector<ndb::DecodeResult>& results,
              std::string* error) {
  std::ofstream out(path);
  if (!out) {
    *error = "Cannot write output file: " + path;
    return false;
  }
  out << "track_id,freq_hz,text,confidence,start_sec,end_sec\n";
  for (const auto& r : results) {
    out << r.trackId << ',' << std::fixed << std::setprecision(2) << r.freqHz << ',' << '"'
        << r.text << '"' << ',' << std::setprecision(3) << r.confidence << ','
        << std::setprecision(3) << r.startSec << ',' << r.endSec << '\n';
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
                << r.freqHz << "Hz text=\"" << r.text << "\" conf=" << std::setprecision(3)
                << r.confidence << " t=[" << r.startSec << "," << r.endSec << "]\n";
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

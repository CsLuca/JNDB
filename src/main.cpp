#include "ndb_decoder.hpp"
#include "wav.hpp"

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
  std::string inputPath;
  std::optional<std::string> outputPath;
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
      << "  --min-confidence <float>   Keep only rows with confidence >= value\n\n"
      << "Examples:\n"
      << "  ndb_decode /?\n"
      << "  ndb_decode C:\\radio\\capture.wav\n"
      << "  ndb_decode C:\\radio\\capture.wav C:\\radio\\out.csv\n"
      << "  ndb_decode capture.wav out.csv --max-seconds 180 --target-sr 12000\n"
      << "  ndb_decode capture.wav --min-confidence 0.7 --mad-factor 3.5\n";
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
        if (error->empty()) {
          *error = "Invalid integer for --fft";
        }
        return false;
      }
      continue;
    }
    if (token == "--hop") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseInt(value, &out->cfg.hopSize)) {
        if (error->empty()) {
          *error = "Invalid integer for --hop";
        }
        return false;
      }
      continue;
    }
    if (token == "--mad-factor") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.madFactor)) {
        if (error->empty()) {
          *error = "Invalid float for --mad-factor";
        }
        return false;
      }
      continue;
    }
    if (token == "--guard-bins") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseInt(value, &out->cfg.guardBins)) {
        if (error->empty()) {
          *error = "Invalid integer for --guard-bins";
        }
        return false;
      }
      continue;
    }
    if (token == "--max-step-bins") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseInt(value, &out->cfg.maxTrackStepBins)) {
        if (error->empty()) {
          *error = "Invalid integer for --max-step-bins";
        }
        return false;
      }
      continue;
    }
    if (token == "--min-track-frames") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseInt(value, &out->cfg.minTrackFrames)) {
        if (error->empty()) {
          *error = "Invalid integer for --min-track-frames";
        }
        return false;
      }
      continue;
    }
    if (token == "--sustain-penalty") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.sustainPenalty)) {
        if (error->empty()) {
          *error = "Invalid float for --sustain-penalty";
        }
        return false;
      }
      continue;
    }
    if (token == "--envelope-alpha") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.envelopeAlpha)) {
        if (error->empty()) {
          *error = "Invalid float for --envelope-alpha";
        }
        return false;
      }
      continue;
    }
    if (token == "--threshold-k") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->cfg.thresholdK)) {
        if (error->empty()) {
          *error = "Invalid float for --threshold-k";
        }
        return false;
      }
      continue;
    }
    if (token == "--min-dot-ms") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseInt(value, &out->cfg.minDotMs)) {
        if (error->empty()) {
          *error = "Invalid integer for --min-dot-ms";
        }
        return false;
      }
      continue;
    }
    if (token == "--max-dot-ms") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseInt(value, &out->cfg.maxDotMs)) {
        if (error->empty()) {
          *error = "Invalid integer for --max-dot-ms";
        }
        return false;
      }
      continue;
    }
    if (token == "--target-sr") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseInt(value, &out->cfg.targetSampleRate)) {
        if (error->empty()) {
          *error = "Invalid integer for --target-sr";
        }
        return false;
      }
      continue;
    }
    if (token == "--max-seconds") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseInt(value, &out->cfg.maxAnalyzeSeconds)) {
        if (error->empty()) {
          *error = "Invalid integer for --max-seconds";
        }
        return false;
      }
      continue;
    }
    if (token == "--min-confidence") {
      std::string value;
      if (!parseOptionValue(token, &value) || !ParseFloat(value, &out->minConfidence)) {
        if (error->empty()) {
          *error = "Invalid float for --min-confidence";
        }
        return false;
      }
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

  auto results = ndb::DecodeNdbFromWav(wav.samples, wav.sampleRate, args.cfg);
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
    std::cout << "Written " << results.size() << " rows to " << *args.outputPath << '\n';
  } else {
    std::cout << "Detected candidates: " << results.size() << '\n';
    for (const auto& r : results) {
      std::cout << "track=" << r.trackId << " freq=" << std::fixed << std::setprecision(2)
                << r.freqHz << "Hz text=\"" << r.text << "\" conf=" << std::setprecision(3)
                << r.confidence << " t=[" << r.startSec << "," << r.endSec << "]\n";
    }
  }

  return 0;
}

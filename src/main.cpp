#include "ndb_decoder.hpp"
#include "wav.hpp"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

void PrintUsage() {
  std::cout << "Usage: ndb_decode <input.wav> [output.csv]\n";
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

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3) {
    PrintUsage();
    return 1;
  }

  const std::string inputPath = argv[1];
  const bool writeFile = (argc == 3);
  const std::string outputPath = writeFile ? std::string(argv[2]) : std::string();

  ndb::WavData wav;
  std::string error;
  if (!ndb::ReadWavMono16(inputPath, &wav, &error)) {
    std::cerr << error << '\n';
    return 2;
  }

  ndb::DecoderConfig cfg;
  const auto results = ndb::DecodeNdbFromWav(wav.samples, wav.sampleRate, cfg);

  if (writeFile) {
    if (!WriteCsv(outputPath, results, &error)) {
      std::cerr << error << '\n';
      return 3;
    }
    std::cout << "Written " << results.size() << " rows to " << outputPath << '\n';
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

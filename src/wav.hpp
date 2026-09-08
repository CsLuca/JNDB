#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ndb {

struct WavData {
  int sampleRate = 0;
  std::vector<float> samples;
};

bool ReadWavMono16(const std::string& path, WavData* out, std::string* error);

}  // namespace ndb

#include "wav.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace ndb {
namespace {

template <typename T>
bool ReadValue(std::ifstream* in, T* out) {
  in->read(reinterpret_cast<char*>(out), sizeof(T));
  return static_cast<bool>(*in);
}

}  // namespace

bool ReadWavMono16(const std::string& path, WavData* out, std::string* error) {
  if (!out || !error) {
    return false;
  }
  *out = WavData();
  *error = "";

  std::ifstream in(path, std::ios::binary);
  if (!in) {
    *error = "Cannot open WAV file: " + path;
    return false;
  }

  char riff[4] = {};
  std::uint32_t riffSize = 0;
  char wave[4] = {};
  in.read(riff, 4);
  if (!ReadValue(&in, &riffSize)) {
    *error = "Invalid WAV header";
    return false;
  }
  in.read(wave, 4);
  if (!in || std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(wave, "WAVE", 4) != 0) {
    *error = "Not a RIFF/WAVE file";
    return false;
  }

  bool haveFmt = false;
  bool haveData = false;
  std::uint16_t audioFormat = 0;
  std::uint16_t channels = 0;
  std::uint32_t sampleRate = 0;
  std::uint16_t bitsPerSample = 0;
  std::vector<std::uint8_t> pcmBytes;

  while (in && !haveData) {
    char chunkId[4] = {};
    std::uint32_t chunkSize = 0;
    in.read(chunkId, 4);
    if (!in) {
      break;
    }
    if (!ReadValue(&in, &chunkSize)) {
      *error = "Malformed WAV chunk";
      return false;
    }

    if (std::memcmp(chunkId, "fmt ", 4) == 0) {
      haveFmt = true;
      if (chunkSize < 16) {
        *error = "Unsupported fmt chunk";
        return false;
      }
      std::uint16_t blockAlign = 0;
      std::uint32_t byteRate = 0;
      if (!ReadValue(&in, &audioFormat) || !ReadValue(&in, &channels) ||
          !ReadValue(&in, &sampleRate) || !ReadValue(&in, &byteRate) ||
          !ReadValue(&in, &blockAlign) || !ReadValue(&in, &bitsPerSample)) {
        *error = "Invalid fmt chunk";
        return false;
      }
      const std::streamoff remain = static_cast<std::streamoff>(chunkSize) - 16;
      if (remain > 0) {
        in.seekg(remain, std::ios::cur);
      }
    } else if (std::memcmp(chunkId, "data", 4) == 0) {
      if (!haveFmt) {
        *error = "fmt chunk not found before data";
        return false;
      }
      if ((audioFormat != 1 && audioFormat != 3) || (channels != 1 && channels != 2)) {
        *error = "Only PCM/IEEE-float mono/stereo WAV is supported";
        return false;
      }
      if (audioFormat == 1 && bitsPerSample != 16 && bitsPerSample != 24) {
        *error = "Only PCM 16-bit or 24-bit WAV is supported";
        return false;
      }
      if (audioFormat == 3 && bitsPerSample != 32) {
        *error = "Only IEEE-float 32-bit WAV is supported";
        return false;
      }

      const std::streampos dataStart = in.tellg();
      in.seekg(0, std::ios::end);
      const std::streampos fileEnd = in.tellg();
      in.seekg(dataStart, std::ios::beg);
      std::size_t readable = 0;
      if (dataStart >= 0 && fileEnd >= dataStart) {
        readable = static_cast<std::size_t>(fileEnd - dataStart);
      }
      const std::size_t requested = static_cast<std::size_t>(chunkSize);
      const std::size_t toRead = std::min(requested, readable);
      pcmBytes.resize(toRead);
      in.read(reinterpret_cast<char*>(pcmBytes.data()), static_cast<std::streamsize>(toRead));
      if (!in) {
        *error = "Failed to read WAV data chunk";
        return false;
      }
      haveData = true;
    } else {
      in.seekg(static_cast<std::streamoff>(chunkSize), std::ios::cur);
    }

    if ((chunkSize % 2U) != 0U) {
      in.seekg(1, std::ios::cur);
    }
  }

  if (!haveFmt || !haveData || sampleRate == 0 || pcmBytes.empty()) {
    *error = "Missing required WAV chunks";
    return false;
  }

  out->sampleRate = static_cast<int>(sampleRate);
  const int bytesPerSample = bitsPerSample / 8;
  const int frameBytes = bytesPerSample * channels;
  if (frameBytes <= 0) {
    *error = "Invalid WAV frame format";
    return false;
  }
  const std::size_t frames = pcmBytes.size() / static_cast<std::size_t>(frameBytes);
  out->samples.resize(frames);

  auto readSampleAsFloat = [&](const std::uint8_t* p) -> float {
    if (audioFormat == 3 && bitsPerSample == 32) {
      float v = 0.0f;
      std::memcpy(&v, p, sizeof(float));
      return std::clamp(v, -1.0f, 1.0f);
    }
    if (bitsPerSample == 16) {
      std::int16_t v = 0;
      std::memcpy(&v, p, sizeof(std::int16_t));
      return static_cast<float>(v) / 32768.0f;
    }
    if (bitsPerSample == 24) {
      const std::uint32_t u = static_cast<std::uint32_t>(p[0]) |
                              (static_cast<std::uint32_t>(p[1]) << 8) |
                              (static_cast<std::uint32_t>(p[2]) << 16);
      std::int32_t s = static_cast<std::int32_t>(u);
      if ((s & 0x00800000) != 0) {
        s |= ~0x00FFFFFF;
      }
      return static_cast<float>(s) / 8388608.0f;
    }
    return 0.0f;
  };

  for (std::size_t i = 0; i < frames; ++i) {
    const std::size_t off = i * static_cast<std::size_t>(frameBytes);
    if (channels == 1) {
      out->samples[i] = readSampleAsFloat(&pcmBytes[off]);
    } else {
      const float l = readSampleAsFloat(&pcmBytes[off]);
      const float r = readSampleAsFloat(&pcmBytes[off + static_cast<std::size_t>(bytesPerSample)]);
      out->samples[i] = 0.5f * (l + r);
    }
  }

  return true;
}

}  // namespace ndb

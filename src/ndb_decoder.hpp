#pragma once

#include <functional>
#include <string>
#include <vector>

namespace ndb {

struct DecodeResult {
  int trackId = 0;
  float freqHz = 0.0f;
  std::string morse;
  std::string text;
  float confidence = 0.0f;
  float startSec = 0.0f;
  float endSec = 0.0f;
};

struct DecoderConfig {
  int fftSize = 512;
  int hopSize = 128;
  float madFactor = 4.0f;
  int guardBins = 2;
  int maxTrackStepBins = 3;
  int minTrackFrames = 15;
  float sustainPenalty = 0.02f;
  float envelopeAlpha = 0.05f;
  float thresholdK = 2.5f;
  int minDotMs = 40;
  int maxDotMs = 220;
  int targetSampleRate = 8000;
  int maxAnalyzeSeconds = 90;
};

struct DecodeStats {
  int inputSampleRate = 0;
  int workSampleRate = 0;
  std::size_t inputSamples = 0;
  std::size_t workSamples = 0;
  int frameCount = 0;
  int candidateBinCount = 0;
  int trackCount = 0;
  int filteredByFrequency = 0;
  int decodedCount = 0;
  float meanConfidence = 0.0f;
  float medianConfidence = 0.0f;
  float maxConfidence = 0.0f;
  float decodeRatio = 0.0f;
  float idLikeTokenRatio = 0.0f;
  float qualityScore = 0.0f;
};

using ProgressCallback = std::function<void(int percent, const std::string& stage)>;

std::vector<DecodeResult> DecodeNdbFromWav(const std::vector<float>& samples, int sampleRate,
                                           const DecoderConfig& cfg,
                                           DecodeStats* stats = nullptr,
                                           ProgressCallback progress = {});

}  // namespace ndb

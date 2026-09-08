#pragma once

#include <complex>
#include <cstddef>
#include <vector>

namespace ndb {

struct TrackPoint {
  int frame = 0;
  float freqHz = 0.0f;
  float score = 0.0f;
};

struct Track {
  int id = 0;
  std::vector<TrackPoint> points;
};

struct Spectrogram {
  int fftSize = 0;
  int hopSize = 0;
  int sampleRate = 0;
  int frameCount = 0;
  int binCount = 0;
  std::vector<float> magnitude;

  float At(int frame, int bin) const {
    return magnitude[static_cast<std::size_t>(frame) * static_cast<std::size_t>(binCount) +
                     static_cast<std::size_t>(bin)];
  }
};

struct FrontEndConfig {
  bool enableBandLimit = true;
  float bandLowHz = 90.0f;
  float bandHighHz = 2200.0f;
  bool enableAutoNotch = true;
  int autoNotchMaxCount = 3;
  float autoNotchSnrDb = 8.0f;
  bool enableImpulseBlanker = true;
  float impulseBlankerSigma = 6.0f;
  int impulseBlankerHalfWindow = 3;
};

struct CandidateDetectorConfig {
  float madFactor = 4.0f;
  int guardBins = 2;
  bool enableCfar2d = true;
  int cfarTrainTime = 4;
  int cfarGuardTime = 1;
  int cfarTrainFreq = 6;
  int cfarGuardFreq = 1;
  float cfarScale = 2.8f;
};

Spectrogram ComputeSpectrogram(const std::vector<float>& samples, int sampleRate, int fftSize,
                               int hopSize);
std::vector<float> ApplyFrontEndDenoise(const std::vector<float>& samples, int sampleRate,
                                        const FrontEndConfig& cfg);
std::vector<std::vector<int>> DetectCandidateBinsMad(const Spectrogram& spec, float madFactor,
                                                      int guardBins);
std::vector<std::vector<int>> DetectCandidateBinsMadCfar2D(const Spectrogram& spec,
                                                            const CandidateDetectorConfig& cfg);
std::vector<Track> TrackTonesAmtcLite(const Spectrogram& spec,
                                      const std::vector<std::vector<int>>& candidates,
                                      int maxStepBins, int minTrackLengthFrames,
                                      float sustainPenalty);
std::vector<Track> TrackTonesAmtcFull(const Spectrogram& spec,
                                      const std::vector<std::vector<int>>& candidates,
                                      int maxStepBins, int minTrackLengthFrames,
                                      float sustainPenalty, int maxGapFrames);
std::vector<std::complex<float>> MixDown(const std::vector<float>& x, int sampleRate,
                                         float freqHz);
std::vector<float> DecimateAverage(const std::vector<float>& x, int factor);
std::vector<float> Envelope(const std::vector<std::complex<float>>& iq);
std::vector<float> ExponentialSmoother(const std::vector<float>& x, float alpha);
std::vector<float> Boxcar(const std::vector<float>& x, int length);
float RobustMadThreshold(const std::vector<float>& x, float k);
std::vector<int> BinaryByThreshold(const std::vector<float>& x, float threshold);

}  // namespace ndb

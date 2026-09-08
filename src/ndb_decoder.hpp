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
  float confidenceRaw = 0.0f;
  float confidenceCalibrated = 0.0f;
  float startSec = 0.0f;
  float endSec = 0.0f;
  int hitCount = 1;
  float firstSeenSec = 0.0f;
  float lastSeenSec = 0.0f;
  float compositeScore = 0.0f;
  float energyScore = 0.0f;
  float continuityScore = 0.0f;
  float freqStabilityScore = 0.0f;
  float keyingPeriodicityScore = 0.0f;
  std::string plausibleId;
  float plausibleIdScore = 0.0f;
  std::string decoderModel;
  bool priorMatched = false;
  std::string priorCandidates;
};

struct DecoderConfig {
  int fftSize = 512;
  int hopSize = 128;
  float madFactor = 4.0f;
  int guardBins = 2;
  int maxTrackStepBins = 3;
  int minTrackFrames = 15;
  float sustainPenalty = 0.02f;
  bool useAmtcFull = false;
  int maxTrackGapFrames = 3;
  float envelopeAlpha = 0.05f;
  float thresholdK = 2.5f;
  int minDotMs = 40;
  int maxDotMs = 220;
  int targetSampleRate = 8000;
  int maxAnalyzeSeconds = 90;
  bool rfFrequencyInput = false;
  float ndbRfMinHz = 190.0f;
  float ndbRfMaxHz = 535.0f;
  float audioMinHz = 80.0f;
  float audioMaxHz = 2000.0f;
  float clusterFreqTolHz = 2.0f;
  float clusterGapSec = 0.4f;
  float dedupFreqTolHz = 2.0f;
  bool enableCochannelSeparation = true;
  int cochannelMaxTracks = 2;
  int cochannelMaxGapFrames = 4;
  float cochannelMaxStepHz = 6.0f;
  bool requirePlausibleId = true;
  float plausibleIdMinScore = 0.30f;
  bool strictBeaconMode = false;
  int strictMinRepeats = 3;
  std::string decoderModel = "auto";
  float hmmTransOnToIntra = 0.73f;
  float hmmTransOnToChar = 0.20f;
  float hmmTransOnToWord = 0.07f;
  float hmmTransOffToDot = 0.80f;
  float hmmTransOffToDash = 0.20f;
  float hmmSigmaOnDot = 0.35f;
  float hmmSigmaOnDash = 0.65f;
  float hmmSigmaOffIntra = 0.40f;
  float hmmSigmaOffChar = 0.80f;
  float hmmSigmaOffWord = 1.35f;
  float hsmmTransOnToIntra = 0.72f;
  float hsmmTransOnToChar = 0.20f;
  float hsmmTransOnToWord = 0.08f;
  float hsmmTransOffToDot = 0.82f;
  float hsmmTransOffToDash = 0.18f;
  float hsmmSigmaOnDot = 0.38f;
  float hsmmSigmaOnDash = 0.72f;
  float hsmmSigmaOffIntra = 0.42f;
  float hsmmSigmaOffChar = 0.85f;
  float hsmmSigmaOffWord = 1.45f;
  float hsmmDurationTailMix = 0.18f;
  float hsmmTimeTransitionGain = 0.55f;
  bool enableBandLimit = false;
  float bandLowHz = 90.0f;
  float bandHighHz = 2200.0f;
  bool enableAutoNotch = false;
  int autoNotchMaxCount = 3;
  float autoNotchSnrDb = 8.0f;
  bool enableImpulseBlanker = false;
  float impulseBlankerSigma = 6.0f;
  int impulseBlankerHalfWindow = 3;
  bool enableCfar2d = false;
  int cfarTrainTime = 4;
  int cfarGuardTime = 1;
  int cfarTrainFreq = 6;
  int cfarGuardFreq = 1;
  float cfarScale = 2.8f;
  std::string confidenceCalibration = "none";
  float plattA = 5.0f;
  float plattB = -2.5f;
  bool enableFreqPriors = false;
  std::string freqPriorFile;
  float freqPriorTolHz = 2.5f;
  bool requirePriorMatch = false;
  int decodeThreads = 1;
  unsigned int deterministicSeed = 1337U;
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
  int clusteredCount = 0;
  int dedupCount = 0;
  int plausibleIdRejected = 0;
  int strictRejected = 0;
  int decodedCount = 0;
  float meanConfidence = 0.0f;
  float medianConfidence = 0.0f;
  float maxConfidence = 0.0f;
  float decodeRatio = 0.0f;
  float idLikeTokenRatio = 0.0f;
  float plausibleIdRatio = 0.0f;
  float meanCompositeScore = 0.0f;
  float qualityScore = 0.0f;
};

using ProgressCallback = std::function<void(int percent, const std::string& stage)>;

std::vector<DecodeResult> DecodeNdbFromWav(const std::vector<float>& samples, int sampleRate,
                                           const DecoderConfig& cfg,
                                           DecodeStats* stats = nullptr,
                                           ProgressCallback progress = {});

}  // namespace ndb

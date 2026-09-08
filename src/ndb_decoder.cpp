#include "ndb_decoder.hpp"

#include "dsp.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <sstream>
#include <unordered_map>

namespace ndb {
namespace {

struct Run {
  int value = 0;
  int length = 0;
};

std::vector<Run> RunLengthEncode(const std::vector<int>& bits) {
  std::vector<Run> runs;
  if (bits.empty()) {
    return runs;
  }
  int current = bits[0];
  int len = 1;
  for (std::size_t i = 1; i < bits.size(); ++i) {
    if (bits[i] == current) {
      ++len;
    } else {
      runs.push_back(Run{current, len});
      current = bits[i];
      len = 1;
    }
  }
  runs.push_back(Run{current, len});
  return runs;
}

const std::unordered_map<std::string, char>& MorseTable() {
  static const std::unordered_map<std::string, char> table = {
      {".-", 'A'},   {"-...", 'B'}, {"-.-.", 'C'}, {"-..", 'D'},  {".", 'E'},
      {"..-.", 'F'}, {"--.", 'G'},  {"....", 'H'}, {"..", 'I'},   {".---", 'J'},
      {"-.-", 'K'},  {".-..", 'L'}, {"--", 'M'},   {"-.", 'N'},   {"---", 'O'},
      {".--.", 'P'}, {"--.-", 'Q'}, {".-.", 'R'},  {"...", 'S'},  {"-", 'T'},
      {"..-", 'U'},  {"...-", 'V'}, {".--", 'W'},  {"-..-", 'X'}, {"-.--", 'Y'},
      {"--..", 'Z'},
  };
  return table;
}

float MeanFrequency(const Track& tr) {
  if (tr.points.empty()) {
    return 0.0f;
  }
  double sum = 0.0;
  for (const auto& p : tr.points) {
    sum += p.freqHz;
  }
  return static_cast<float>(sum / static_cast<double>(tr.points.size()));
}

float MedianConfidence(std::vector<float> values) {
  if (values.empty()) {
    return 0.0f;
  }
  const std::size_t n = values.size();
  const std::size_t m = n / 2;
  std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(m), values.end());
  float med = values[m];
  if ((n % 2U) == 0U) {
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(m - 1),
                     values.end());
    med = 0.5f * (med + values[m - 1]);
  }
  return med;
}

bool IsLikelyIdToken(const std::string& token) {
  if (token.size() < 2 || token.size() > 3) {
    return false;
  }
  for (char c : token) {
    if (c < 'A' || c > 'Z') {
      return false;
    }
  }
  return true;
}

float ComputeIdLikeTokenRatio(const std::vector<DecodeResult>& results) {
  int total = 0;
  int good = 0;
  for (const auto& r : results) {
    std::istringstream iss(r.text);
    std::string tok;
    while (iss >> tok) {
      ++total;
      if (IsLikelyIdToken(tok)) {
        ++good;
      }
    }
  }
  if (total == 0) {
    return 0.0f;
  }
  return static_cast<float>(good) / static_cast<float>(total);
}

void Report(const ProgressCallback& progress, int percent, const std::string& stage) {
  if (progress) {
    progress(percent, stage);
  }
}

std::pair<std::string, float> DecodeMorseLikeHmm(const std::vector<Run>& runs, int sampleRate,
                                                  int dotSamples) {
  if (runs.empty() || dotSamples <= 0) {
    return {"", 0.0f};
  }

  std::string morse;
  std::string text;
  int symbolCount = 0;
  int goodCount = 0;

  const float dot = static_cast<float>(dotSamples);
  std::string currentSymbol;

  for (const auto& r : runs) {
    const float units = static_cast<float>(r.length) / dot;
    if (r.value == 1) {
      ++symbolCount;
      if (units < 2.0f) {
        currentSymbol.push_back('.');
        morse.push_back('.');
        ++goodCount;
      } else {
        currentSymbol.push_back('-');
        morse.push_back('-');
        ++goodCount;
      }
    } else {
      if (units >= 6.0f) {
        if (!currentSymbol.empty()) {
          const auto it = MorseTable().find(currentSymbol);
          if (it != MorseTable().end()) {
            text.push_back(it->second);
          } else {
            text.push_back('?');
          }
          currentSymbol.clear();
        }
        text.push_back(' ');
        morse.append(" /");
      } else if (units >= 2.5f) {
        if (!currentSymbol.empty()) {
          const auto it = MorseTable().find(currentSymbol);
          if (it != MorseTable().end()) {
            text.push_back(it->second);
          } else {
            text.push_back('?');
          }
          currentSymbol.clear();
        }
        morse.push_back(' ');
      }
    }
  }

  if (!currentSymbol.empty()) {
    const auto it = MorseTable().find(currentSymbol);
    if (it != MorseTable().end()) {
      text.push_back(it->second);
    } else {
      text.push_back('?');
    }
  }

  while (!text.empty() && text.back() == ' ') {
    text.pop_back();
  }

  const float confidence = symbolCount > 0 ? static_cast<float>(goodCount) / symbolCount : 0.0f;
  return {text.empty() ? morse : text, confidence};
}

int EstimateDotSamplesMatched(const std::vector<float>& env, int sampleRate, int minDotMs,
                              int maxDotMs) {
  const int minDot = std::max(1, sampleRate * minDotMs / 1000);
  const int maxDot = std::max(minDot, sampleRate * maxDotMs / 1000);

  int bestDot = minDot;
  float bestScore = -1.0f;
  for (int dot = minDot; dot <= maxDot; dot += std::max(1, sampleRate / 500)) {
    const std::vector<float> boxed = Boxcar(env, dot);
    const float thr = RobustMadThreshold(boxed, 2.0f);
    const std::vector<int> bits = BinaryByThreshold(boxed, thr);
    const std::vector<Run> runs = RunLengthEncode(bits);

    float score = 0.0f;
    for (const auto& r : runs) {
      if (r.value == 1) {
        const float u = static_cast<float>(r.length) / static_cast<float>(dot);
        const float d1 = std::fabs(u - 1.0f);
        const float d3 = std::fabs(u - 3.0f);
        score += 1.0f / (0.2f + std::min(d1, d3));
      }
    }
    if (score > bestScore) {
      bestScore = score;
      bestDot = dot;
    }
  }
  return bestDot;
}

}  // namespace

std::vector<DecodeResult> DecodeNdbFromWav(const std::vector<float>& samples, int sampleRate,
                                           const DecoderConfig& cfg, DecodeStats* stats,
                                           ProgressCallback progress) {
  std::vector<DecodeResult> results;
  if (stats) {
    *stats = DecodeStats();
    stats->inputSampleRate = sampleRate;
    stats->inputSamples = samples.size();
  }
  if (samples.empty() || sampleRate <= 0) {
    return results;
  }
  Report(progress, 0, "init");

  const int decim = std::max(1, sampleRate / std::max(1000, cfg.targetSampleRate));
  std::vector<float> work = DecimateAverage(samples, decim);
  int workRate = sampleRate / decim;
  if (workRate <= 0) {
    work = samples;
    workRate = sampleRate;
  }
  if (stats) {
    stats->workSampleRate = workRate;
    stats->workSamples = work.size();
  }
  Report(progress, 8, "preprocess");
  if (cfg.maxAnalyzeSeconds > 0) {
    const std::size_t maxN = static_cast<std::size_t>(cfg.maxAnalyzeSeconds) *
                             static_cast<std::size_t>(workRate);
    if (work.size() > maxN) {
      work.resize(maxN);
    }
  }

  const Spectrogram spec = ComputeSpectrogram(work, workRate, cfg.fftSize, cfg.hopSize);
  if (spec.frameCount <= 0) {
    return results;
  }
  if (stats) {
    stats->frameCount = spec.frameCount;
  }
  Report(progress, 28, "spectrogram");

  const auto candidates = DetectCandidateBinsMad(spec, cfg.madFactor, cfg.guardBins);
  if (stats) {
    int bins = 0;
    for (const auto& v : candidates) {
      bins += static_cast<int>(v.size());
    }
    stats->candidateBinCount = bins;
  }
  Report(progress, 40, "candidate-detection");

  const auto tracks = TrackTonesAmtcLite(spec, candidates, cfg.maxTrackStepBins, cfg.minTrackFrames,
                                         cfg.sustainPenalty);
  if (stats) {
    stats->trackCount = static_cast<int>(tracks.size());
  }
  Report(progress, 55, "tracking");

  const int totalTracks = std::max(1, static_cast<int>(tracks.size()));
  int processedTracks = 0;
  int filteredByFreq = 0;
  for (const auto& tr : tracks) {
    const float f0 = MeanFrequency(tr);
    if (f0 < 80.0f || f0 > 2000.0f) {
      ++filteredByFreq;
      ++processedTracks;
      const int p = 55 + (35 * processedTracks) / totalTracks;
      Report(progress, p, "decode-tracks");
      continue;
    }

    std::vector<std::complex<float>> iq = MixDown(work, workRate, f0);
    std::vector<float> env = Envelope(iq);
    env = ExponentialSmoother(env, cfg.envelopeAlpha);

    const int dotSamples = EstimateDotSamplesMatched(env, workRate, cfg.minDotMs, cfg.maxDotMs);
    std::vector<float> boxed = Boxcar(env, dotSamples);
    const float thr = RobustMadThreshold(boxed, cfg.thresholdK);
    const std::vector<int> bits = BinaryByThreshold(boxed, thr);
    const std::vector<Run> runs = RunLengthEncode(bits);
    const auto decoded = DecodeMorseLikeHmm(runs, workRate, dotSamples);
    const std::string decodedText = decoded.first;
    const float confidence = decoded.second;

    if (decodedText.empty()) {
      continue;
    }

    DecodeResult r;
    r.trackId = tr.id;
    r.freqHz = f0;
    r.text = decodedText;
    r.morse = decodedText;
    r.confidence = confidence;
    r.startSec = static_cast<float>(tr.points.front().frame * cfg.hopSize) /
                 static_cast<float>(workRate);
    r.endSec = static_cast<float>(tr.points.back().frame * cfg.hopSize + cfg.fftSize) /
               static_cast<float>(workRate);
    results.push_back(std::move(r));

    ++processedTracks;
    const int p = 55 + (35 * processedTracks) / totalTracks;
    Report(progress, p, "decode-tracks");
  }

  std::sort(results.begin(), results.end(), [](const DecodeResult& a, const DecodeResult& b) {
    if (a.confidence != b.confidence) {
      return a.confidence > b.confidence;
    }
    return a.freqHz < b.freqHz;
  });

  if (stats) {
    stats->filteredByFrequency = filteredByFreq;
    stats->decodedCount = static_cast<int>(results.size());
    std::vector<float> conf;
    conf.reserve(results.size());
    double sum = 0.0;
    float maxc = 0.0f;
    for (const auto& r : results) {
      conf.push_back(r.confidence);
      sum += r.confidence;
      maxc = std::max(maxc, r.confidence);
    }
    stats->meanConfidence = results.empty() ? 0.0f : static_cast<float>(sum / results.size());
    stats->medianConfidence = MedianConfidence(conf);
    stats->maxConfidence = maxc;
    stats->decodeRatio = stats->trackCount > 0
                             ? static_cast<float>(stats->decodedCount) /
                                   static_cast<float>(stats->trackCount)
                             : 0.0f;
    stats->idLikeTokenRatio = ComputeIdLikeTokenRatio(results);
    stats->qualityScore = 100.0f * (0.45f * stats->meanConfidence + 0.25f * stats->medianConfidence +
                                    0.20f * stats->decodeRatio +
                                    0.10f * stats->idLikeTokenRatio);
  }
  Report(progress, 100, "done");
  return results;
}

}  // namespace ndb

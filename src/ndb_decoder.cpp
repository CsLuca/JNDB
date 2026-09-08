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

struct ClusteredTrack {
  int id = 0;
  float freqHz = 0.0f;
  float startSec = 0.0f;
  float endSec = 0.0f;
  std::vector<TrackPoint> points;
};

struct DecodedCandidate {
  DecodeResult result;
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

void Report(const ProgressCallback& progress, int percent, const std::string& stage) {
  if (progress) {
    progress(percent, stage);
  }
}

float Mean(const std::vector<float>& v) {
  if (v.empty()) {
    return 0.0f;
  }
  double s = 0.0;
  for (float x : v) {
    s += x;
  }
  return static_cast<float>(s / static_cast<double>(v.size()));
}

float Median(std::vector<float> v) {
  if (v.empty()) {
    return 0.0f;
  }
  const std::size_t m = v.size() / 2;
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(m), v.end());
  float med = v[m];
  if ((v.size() % 2U) == 0U) {
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(m - 1), v.end());
    med = 0.5f * (med + v[m - 1]);
  }
  return med;
}

float MeanFrequency(const std::vector<TrackPoint>& points) {
  if (points.empty()) {
    return 0.0f;
  }
  double s = 0.0;
  for (const auto& p : points) {
    s += p.freqHz;
  }
  return static_cast<float>(s / static_cast<double>(points.size()));
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

std::pair<std::string, float> DecodeMorseLikeHmm(const std::vector<Run>& runs, int dotSamples) {
  if (runs.empty() || dotSamples <= 0) {
    return {"", 0.0f};
  }
  std::string text;
  std::string current;
  int symbols = 0;
  int valid = 0;
  const float dot = static_cast<float>(dotSamples);

  for (const auto& r : runs) {
    const float units = static_cast<float>(r.length) / dot;
    if (r.value == 1) {
      ++symbols;
      if (units < 2.0f) {
        current.push_back('.');
        ++valid;
      } else {
        current.push_back('-');
        ++valid;
      }
    } else {
      if (units >= 2.5f) {
        if (!current.empty()) {
          const auto it = MorseTable().find(current);
          text.push_back(it == MorseTable().end() ? '?' : it->second);
          current.clear();
        }
        if (units >= 6.0f) {
          text.push_back(' ');
        }
      }
    }
  }
  if (!current.empty()) {
    const auto it = MorseTable().find(current);
    text.push_back(it == MorseTable().end() ? '?' : it->second);
  }
  while (!text.empty() && text.back() == ' ') {
    text.pop_back();
  }
  const float conf = symbols > 0 ? static_cast<float>(valid) / symbols : 0.0f;
  return {text, conf};
}

std::vector<std::string> ExtractUpperTokens(const std::string& text) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : text) {
    if (c >= 'A' && c <= 'Z') {
      cur.push_back(c);
    } else {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
    }
  }
  if (!cur.empty()) {
    out.push_back(cur);
  }
  return out;
}

std::pair<std::string, float> ExtractPlausibleId(const std::string& text) {
  const auto tokens = ExtractUpperTokens(text);
  std::unordered_map<std::string, int> cnt;
  int validTotal = 0;
  for (const auto& t : tokens) {
    if (t.size() >= 2 && t.size() <= 3) {
      ++cnt[t];
      ++validTotal;
    }
  }
  if (validTotal == 0 || cnt.empty()) {
    return {"", 0.0f};
  }

  std::string best;
  int bestCount = 0;
  for (const auto& kv : cnt) {
    if (kv.second > bestCount) {
      bestCount = kv.second;
      best = kv.first;
    }
  }

  int cycHits = 0;
  int cycTotal = 0;
  for (std::size_t i = 1; i < tokens.size(); ++i) {
    if ((tokens[i - 1].size() >= 2 && tokens[i - 1].size() <= 3) &&
        (tokens[i].size() >= 2 && tokens[i].size() <= 3)) {
      ++cycTotal;
      if (tokens[i] == tokens[i - 1]) {
        ++cycHits;
      }
    }
  }

  const float support = static_cast<float>(bestCount) / static_cast<float>(validTotal);
  const float repetition = cycTotal > 0 ? static_cast<float>(cycHits) / static_cast<float>(cycTotal)
                                        : 0.0f;
  const float score = std::max(0.0f, std::min(1.0f, 0.65f * support + 0.35f * repetition));
  return {best, score};
}

int EstimateDotSamplesMatched(const std::vector<float>& env, int sampleRate, int minDotMs,
                              int maxDotMs) {
  const int minDot = std::max(1, sampleRate * minDotMs / 1000);
  const int maxDot = std::max(minDot, sampleRate * maxDotMs / 1000);
  int bestDot = minDot;
  float bestScore = -1.0f;
  for (int dot = minDot; dot <= maxDot; dot += std::max(1, sampleRate / 500)) {
    const auto boxed = Boxcar(env, dot);
    const float thr = RobustMadThreshold(boxed, 2.0f);
    const auto bits = BinaryByThreshold(boxed, thr);
    const auto runs = RunLengthEncode(bits);
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

bool InNdbRange(float freqHz, const DecoderConfig& cfg) {
  if (cfg.rfFrequencyInput) {
    return freqHz >= cfg.ndbRfMinHz && freqHz <= cfg.ndbRfMaxHz;
  }
  return freqHz >= cfg.audioMinHz && freqHz <= cfg.audioMaxHz;
}

float Clamp01(float x) {
  if (x < 0.0f) return 0.0f;
  if (x > 1.0f) return 1.0f;
  return x;
}

float ComputeContinuityScore(const std::vector<TrackPoint>& points) {
  if (points.size() < 2) {
    return 0.0f;
  }
  int gaps = 0;
  for (std::size_t i = 1; i < points.size(); ++i) {
    gaps += std::max(0, points[i].frame - points[i - 1].frame - 1);
  }
  const int span = std::max(1, points.back().frame - points.front().frame + 1);
  const float gapRatio = static_cast<float>(gaps) / static_cast<float>(span);
  return Clamp01(1.0f - gapRatio);
}

float ComputeFreqStabilityScore(const std::vector<TrackPoint>& points) {
  if (points.size() < 2) {
    return 0.0f;
  }
  const float meanF = MeanFrequency(points);
  double var = 0.0;
  for (const auto& p : points) {
    const double d = static_cast<double>(p.freqHz - meanF);
    var += d * d;
  }
  var /= static_cast<double>(points.size());
  const float sigma = static_cast<float>(std::sqrt(var));
  return Clamp01(1.0f - sigma / 6.0f);
}

float ComputeEnergyScore(const std::vector<TrackPoint>& points) {
  if (points.empty()) {
    return 0.0f;
  }
  std::vector<float> e;
  e.reserve(points.size());
  for (const auto& p : points) {
    e.push_back(std::max(0.0f, p.score));
  }
  const float m = Mean(e);
  return Clamp01(m / (m + 0.03f));
}

float ComputeKeyingPeriodicityScore(const std::vector<int>& runsOn, int dotSamples) {
  if (runsOn.empty() || dotSamples <= 0) {
    return 0.0f;
  }
  double err = 0.0;
  for (int len : runsOn) {
    const float u = static_cast<float>(len) / static_cast<float>(dotSamples);
    const float d1 = std::fabs(u - 1.0f);
    const float d3 = std::fabs(u - 3.0f);
    err += std::min(d1, d3);
  }
  const float e = static_cast<float>(err / static_cast<double>(runsOn.size()));
  return Clamp01(1.0f - e / 1.8f);
}

std::vector<ClusteredTrack> ClusterTracks(const std::vector<Track>& tracks, int hopSize, int sampleRate,
                                          float freqTolHz, float gapSec) {
  std::vector<ClusteredTrack> out;
  if (tracks.empty()) {
    return out;
  }

  std::vector<bool> used(tracks.size(), false);
  int nextId = 1;

  auto trackStartSec = [&](const Track& t) {
    return static_cast<float>(t.points.front().frame * hopSize) / static_cast<float>(sampleRate);
  };
  auto trackEndSec = [&](const Track& t) {
    return static_cast<float>(t.points.back().frame * hopSize) / static_cast<float>(sampleRate);
  };

  for (std::size_t i = 0; i < tracks.size(); ++i) {
    if (used[i] || tracks[i].points.empty()) {
      continue;
    }

    ClusteredTrack c;
    c.id = nextId++;
    c.points = tracks[i].points;
    c.freqHz = MeanFrequency(c.points);
    c.startSec = trackStartSec(tracks[i]);
    c.endSec = trackEndSec(tracks[i]);
    used[i] = true;

    bool grown = true;
    while (grown) {
      grown = false;
      for (std::size_t j = 0; j < tracks.size(); ++j) {
        if (used[j] || tracks[j].points.empty()) {
          continue;
        }
        const float f = MeanFrequency(tracks[j].points);
        if (std::fabs(f - c.freqHz) > freqTolHz) {
          continue;
        }
        const float s = trackStartSec(tracks[j]);
        const float e = trackEndSec(tracks[j]);
        const bool overlapOrNear = (s <= c.endSec + gapSec) && (e >= c.startSec - gapSec);
        if (!overlapOrNear) {
          continue;
        }

        c.points.insert(c.points.end(), tracks[j].points.begin(), tracks[j].points.end());
        c.startSec = std::min(c.startSec, s);
        c.endSec = std::max(c.endSec, e);
        c.freqHz = MeanFrequency(c.points);
        used[j] = true;
        grown = true;
      }
    }

    std::sort(c.points.begin(), c.points.end(), [](const TrackPoint& a, const TrackPoint& b) {
      return a.frame < b.frame;
    });
    out.push_back(std::move(c));
  }

  return out;
}

std::vector<DecodeResult> DedupById(const std::vector<DecodeResult>& in, float freqTolHz) {
  std::vector<DecodeResult> out;
  for (const auto& r : in) {
    if (r.plausibleId.empty()) {
      continue;
    }
    bool merged = false;
    for (auto& e : out) {
      if (e.plausibleId != r.plausibleId) {
        continue;
      }
      if (std::fabs(e.freqHz - r.freqHz) > freqTolHz) {
        continue;
      }
      e.firstSeenSec = std::min(e.firstSeenSec, r.firstSeenSec);
      e.lastSeenSec = std::max(e.lastSeenSec, r.lastSeenSec);
      e.startSec = e.firstSeenSec;
      e.endSec = e.lastSeenSec;
      e.hitCount += r.hitCount;
      e.compositeScore = std::max(e.compositeScore, r.compositeScore);
      e.confidence = std::max(e.confidence, r.confidence);
      e.plausibleIdScore = std::max(e.plausibleIdScore, r.plausibleIdScore);
      merged = true;
      break;
    }
    if (!merged) {
      out.push_back(r);
    }
  }

  std::sort(out.begin(), out.end(), [](const DecodeResult& a, const DecodeResult& b) {
    if (a.compositeScore != b.compositeScore) {
      return a.compositeScore > b.compositeScore;
    }
    return a.freqHz < b.freqHz;
  });
  return out;
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
  if (cfg.maxAnalyzeSeconds > 0) {
    const std::size_t maxN = static_cast<std::size_t>(cfg.maxAnalyzeSeconds) *
                             static_cast<std::size_t>(workRate);
    if (work.size() > maxN) {
      work.resize(maxN);
    }
  }
  if (stats) {
    stats->workSampleRate = workRate;
    stats->workSamples = work.size();
  }
  Report(progress, 10, "preprocess");

  const Spectrogram spec = ComputeSpectrogram(work, workRate, cfg.fftSize, cfg.hopSize);
  if (spec.frameCount <= 0) {
    return results;
  }
  if (stats) {
    stats->frameCount = spec.frameCount;
  }
  Report(progress, 24, "spectrogram");

  const auto candidates = DetectCandidateBinsMad(spec, cfg.madFactor, cfg.guardBins);
  if (stats) {
    int bins = 0;
    for (const auto& v : candidates) {
      bins += static_cast<int>(v.size());
    }
    stats->candidateBinCount = bins;
  }
  Report(progress, 35, "candidate-detection");

  const auto tracks = TrackTonesAmtcLite(spec, candidates, cfg.maxTrackStepBins, cfg.minTrackFrames,
                                         cfg.sustainPenalty);
  if (stats) {
    stats->trackCount = static_cast<int>(tracks.size());
  }
  Report(progress, 47, "tracking");

  const auto clustered = ClusterTracks(tracks, cfg.hopSize, workRate, cfg.clusterFreqTolHz,
                                       cfg.clusterGapSec);
  if (stats) {
    stats->clusteredCount = static_cast<int>(clustered.size());
  }
  Report(progress, 58, "cluster-merge");

  int filteredByFreq = 0;
  int plausibleRejected = 0;
  std::vector<DecodeResult> decoded;
  decoded.reserve(clustered.size());

  const int total = std::max(1, static_cast<int>(clustered.size()));
  int done = 0;

  for (const auto& tr : clustered) {
    const float f0 = tr.freqHz;
    if (!InNdbRange(f0, cfg)) {
      ++filteredByFreq;
      ++done;
      Report(progress, 58 + (28 * done) / total, "decode-clusters");
      continue;
    }

    auto iq = MixDown(work, workRate, f0);
    auto env = Envelope(iq);
    env = ExponentialSmoother(env, cfg.envelopeAlpha);

    const int dotSamples = EstimateDotSamplesMatched(env, workRate, cfg.minDotMs, cfg.maxDotMs);
    auto boxed = Boxcar(env, dotSamples);
    const float thr = RobustMadThreshold(boxed, cfg.thresholdK);
    const auto bits = BinaryByThreshold(boxed, thr);
    const auto runs = RunLengthEncode(bits);
    const auto decodedText = DecodeMorseLikeHmm(runs, dotSamples);
    if (decodedText.first.empty()) {
      ++done;
      Report(progress, 58 + (28 * done) / total, "decode-clusters");
      continue;
    }

    std::vector<int> onRuns;
    for (const auto& r : runs) {
      if (r.value == 1) {
        onRuns.push_back(r.length);
      }
    }

    DecodeResult r;
    r.trackId = tr.id;
    r.freqHz = f0;
    r.text = decodedText.first;
    r.morse = decodedText.first;
    r.confidence = decodedText.second;
    r.startSec = tr.startSec;
    r.endSec = tr.endSec;
    r.firstSeenSec = tr.startSec;
    r.lastSeenSec = tr.endSec;
    r.hitCount = 1;

    r.energyScore = ComputeEnergyScore(tr.points);
    r.continuityScore = ComputeContinuityScore(tr.points);
    r.freqStabilityScore = ComputeFreqStabilityScore(tr.points);
    r.keyingPeriodicityScore = ComputeKeyingPeriodicityScore(onRuns, dotSamples);
    r.compositeScore = 0.35f * r.energyScore + 0.25f * r.continuityScore +
                       0.20f * r.freqStabilityScore + 0.20f * r.keyingPeriodicityScore;

    const auto plausible = ExtractPlausibleId(r.text);
    r.plausibleId = plausible.first;
    r.plausibleIdScore = plausible.second;
    if (cfg.requirePlausibleId &&
        (r.plausibleId.empty() || r.plausibleIdScore < cfg.plausibleIdMinScore)) {
      ++plausibleRejected;
      ++done;
      Report(progress, 58 + (28 * done) / total, "decode-clusters");
      continue;
    }
    if (!r.plausibleId.empty()) {
      r.text = r.plausibleId;
    }

    decoded.push_back(std::move(r));
    ++done;
    Report(progress, 58 + (28 * done) / total, "decode-clusters");
  }

  auto dedup = DedupById(decoded, cfg.dedupFreqTolHz);
  if (stats) {
    stats->filteredByFrequency = filteredByFreq;
    stats->dedupCount = static_cast<int>(dedup.size());
    stats->plausibleIdRejected = plausibleRejected;
  }
  Report(progress, 90, "dedup-id");

  results = std::move(dedup);

  if (stats) {
    stats->decodedCount = static_cast<int>(results.size());
    std::vector<float> conf;
    std::vector<float> comp;
    int withPlausibleId = 0;
    conf.reserve(results.size());
    comp.reserve(results.size());
    float maxConf = 0.0f;
    for (const auto& r : results) {
      conf.push_back(r.confidence);
      comp.push_back(r.compositeScore);
      maxConf = std::max(maxConf, r.confidence);
      if (!r.plausibleId.empty()) {
        ++withPlausibleId;
      }
    }
    stats->meanConfidence = Mean(conf);
    stats->medianConfidence = Median(conf);
    stats->maxConfidence = maxConf;
    stats->decodeRatio = stats->clusteredCount > 0
                             ? static_cast<float>(stats->decodedCount) /
                                   static_cast<float>(stats->clusteredCount)
                             : 0.0f;
    stats->idLikeTokenRatio = ComputeIdLikeTokenRatio(results);
    stats->plausibleIdRatio = results.empty() ? 0.0f
                                              : static_cast<float>(withPlausibleId) /
                                                    static_cast<float>(results.size());
    stats->meanCompositeScore = Mean(comp);
    stats->qualityScore = 100.0f * (0.35f * stats->meanConfidence + 0.20f * stats->medianConfidence +
                                    0.15f * stats->decodeRatio +
                                    0.05f * stats->idLikeTokenRatio +
                                    0.10f * stats->plausibleIdRatio +
                                    0.15f * stats->meanCompositeScore);
  }

  Report(progress, 100, "done");
  return results;
}

}  // namespace ndb

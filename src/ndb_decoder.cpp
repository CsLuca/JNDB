#include "ndb_decoder.hpp"

#include "dsp.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <fstream>
#include <numeric>
#include <mutex>
#include <sstream>
#include <thread>
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

struct PriorEntry {
  float freqHz = 0.0f;
  std::vector<std::string> ids;
};

struct SequenceDecode {
  std::string text;
  float confidence = 0.0f;
  float avgLogLike = -1e9f;
  std::string model;
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

std::vector<PriorEntry> LoadFreqPriors(const std::string& path) {
  std::vector<PriorEntry> out;
  if (path.empty()) {
    return out;
  }
  std::ifstream in(path);
  if (!in) {
    return out;
  }
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    std::istringstream iss(line);
    std::string f;
    std::string ids;
    if (!std::getline(iss, f, ',')) {
      continue;
    }
    if (!std::getline(iss, ids)) {
      continue;
    }
    PriorEntry e;
    try {
      e.freqHz = std::stof(f);
    } catch (...) {
      continue;
    }
    std::istringstream idss(ids);
    std::string id;
    while (std::getline(idss, id, '|')) {
      for (char& c : id) {
        if (c >= 'a' && c <= 'z') {
          c = static_cast<char>(c - 'a' + 'A');
        }
      }
      const bool tokenLike = (id.size() >= 2 && id.size() <= 3) &&
                             std::all_of(id.begin(), id.end(), [](char c) {
                               return c >= 'A' && c <= 'Z';
                             });
      if (tokenLike) {
        e.ids.push_back(id);
      }
    }
    if (!e.ids.empty()) {
      out.push_back(std::move(e));
    }
  }
  return out;
}

std::vector<std::string> FindPriorCandidates(float freqHz, const std::vector<PriorEntry>& priors,
                                             float tolHz) {
  std::vector<std::string> out;
  for (const auto& p : priors) {
    if (std::fabs(p.freqHz - freqHz) <= tolHz) {
      out.insert(out.end(), p.ids.begin(), p.ids.end());
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

bool ContainsToken(const std::vector<std::string>& v, const std::string& x) {
  for (const auto& a : v) {
    if (a == x) {
      return true;
    }
  }
  return false;
}

std::string JoinPipe(const std::vector<std::string>& v) {
  std::string out;
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (i > 0) {
      out.push_back('|');
    }
    out += v[i];
  }
  return out;
}

float CalibrateConfidence(float raw, const DecoderConfig& cfg) {
  const float x = std::max(1e-4f, std::min(0.9999f, raw));
  if (cfg.confidenceCalibration == "platt") {
    const float z = cfg.plattA * x + cfg.plattB;
    return 1.0f / (1.0f + std::exp(-z));
  }
  if (cfg.confidenceCalibration == "isotonic") {
    const std::array<float, 7> xp = {0.0f, 0.15f, 0.30f, 0.50f, 0.70f, 0.85f, 1.0f};
    const std::array<float, 7> yp = {0.02f, 0.12f, 0.26f, 0.52f, 0.72f, 0.86f, 0.97f};
    for (std::size_t i = 1; i < xp.size(); ++i) {
      if (x <= xp[i]) {
        const float t = (x - xp[i - 1]) / (xp[i] - xp[i - 1] + 1e-6f);
        return yp[i - 1] + t * (yp[i] - yp[i - 1]);
      }
    }
    return yp.back();
  }
  return x;
}

float GaussianLike(float x, float mu, float sigma) {
  const float s = std::max(0.05f, sigma);
  const float d = (x - mu) / s;
  return std::exp(-0.5f * d * d);
}

std::vector<float> AdaptiveDotPerRun(const std::vector<Run>& runs, float baseDot) {
  std::vector<float> dots(runs.size(), baseDot);
  std::vector<float> onLens;
  onLens.reserve(runs.size());
  for (std::size_t i = 0; i < runs.size(); ++i) {
    if (runs[i].value == 1) {
      onLens.push_back(static_cast<float>(runs[i].length));
    }
    if (onLens.size() >= 4) {
      std::vector<float> tmp = onLens;
      std::sort(tmp.begin(), tmp.end());
      const std::size_t q = tmp.size() / 4;
      float local = tmp[q];
      local = std::max(0.6f * baseDot, std::min(1.6f * baseDot, local));
      dots[i] = 0.85f * dots[std::max<std::size_t>(1, i) - 1] + 0.15f * local;
    } else if (i > 0) {
      dots[i] = dots[i - 1];
    }
  }
  return dots;
}

float DecodeTextHeuristicScore(const std::string& text, float conf, float avgLogLike) {
  if (text.empty()) {
    return -1e9f;
  }
  int letters = 0;
  int questions = 0;
  int repeats = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] >= 'A' && text[i] <= 'Z') {
      ++letters;
      if (i > 0 && text[i] == text[i - 1]) {
        ++repeats;
      }
    } else if (text[i] == '?') {
      ++questions;
    }
  }
  const float len = static_cast<float>(std::max<std::size_t>(1, text.size()));
  const float letterRatio = static_cast<float>(letters) / len;
  const float qRatio = static_cast<float>(questions) / len;
  const float repRatio = static_cast<float>(repeats) / len;
  return 0.60f * conf + 0.30f * letterRatio - 0.40f * qRatio + 0.08f * repRatio +
         0.08f * avgLogLike;
}

SequenceDecode DecodeMorseViterbiHmm(const std::vector<Run>& runs, int dotSamples,
                                     const DecoderConfig& cfg) {
  if (runs.empty() || dotSamples <= 0) {
    return {};
  }

  enum State { kOnDot = 0, kOnDash = 1, kOffIntra = 2, kOffChar = 3, kOffWord = 4, kN = 5 };
  const float kNeg = -1e30f;

  auto allowed = [](int runValue, int st) {
    if (runValue == 1) {
      return st == kOnDot || st == kOnDash;
    }
    return st == kOffIntra || st == kOffChar || st == kOffWord;
  };

  float trans[kN][kN];
  for (int a = 0; a < kN; ++a) {
    for (int b = 0; b < kN; ++b) {
      trans[a][b] = -8.0f;
    }
  }
  const float onToIntra = std::max(1e-4f, cfg.hmmTransOnToIntra);
  const float onToChar = std::max(1e-4f, cfg.hmmTransOnToChar);
  const float onToWord = std::max(1e-4f, cfg.hmmTransOnToWord);
  const float onNorm = onToIntra + onToChar + onToWord;

  const float offToDot = std::max(1e-4f, cfg.hmmTransOffToDot);
  const float offToDash = std::max(1e-4f, cfg.hmmTransOffToDash);
  const float offNorm = offToDot + offToDash;

  trans[kOnDot][kOffIntra] = std::log(onToIntra / onNorm);
  trans[kOnDot][kOffChar] = std::log(onToChar / onNorm);
  trans[kOnDot][kOffWord] = std::log(onToWord / onNorm);
  trans[kOnDash][kOffIntra] = std::log(onToIntra / onNorm);
  trans[kOnDash][kOffChar] = std::log(onToChar / onNorm);
  trans[kOnDash][kOffWord] = std::log(onToWord / onNorm);
  trans[kOffIntra][kOnDot] = std::log(offToDot / offNorm);
  trans[kOffIntra][kOnDash] = std::log(offToDash / offNorm);
  trans[kOffChar][kOnDot] = std::log(offToDot / offNorm);
  trans[kOffChar][kOnDash] = std::log(offToDash / offNorm);
  trans[kOffWord][kOnDot] = std::log(offToDot / offNorm);
  trans[kOffWord][kOnDash] = std::log(offToDash / offNorm);

  const auto dotVec = AdaptiveDotPerRun(runs, static_cast<float>(dotSamples));
  const std::size_t T = runs.size();
  std::vector<std::array<float, kN>> dp(T);
  std::vector<std::array<int, kN>> prev(T);
  for (std::size_t t = 0; t < T; ++t) {
    for (int s = 0; s < kN; ++s) {
      dp[t][s] = kNeg;
      prev[t][s] = -1;
    }
  }

  auto emission = [&](std::size_t t, int s) {
    const float dot = std::max(1.0f, dotVec[t]);
    const float u = static_cast<float>(runs[t].length) / dot;
    if (s == kOnDot) return std::log(std::max(1e-6f, GaussianLike(u, 1.0f, cfg.hmmSigmaOnDot)));
    if (s == kOnDash) {
      return std::log(std::max(1e-6f, GaussianLike(u, 3.0f, cfg.hmmSigmaOnDash)));
    }
    if (s == kOffIntra) {
      return std::log(std::max(1e-6f, GaussianLike(u, 1.0f, cfg.hmmSigmaOffIntra)));
    }
    if (s == kOffChar) {
      return std::log(std::max(1e-6f, GaussianLike(u, 3.0f, cfg.hmmSigmaOffChar)));
    }
    return std::log(std::max(1e-6f, GaussianLike(u, 7.0f, cfg.hmmSigmaOffWord)));
  };

  for (int s = 0; s < kN; ++s) {
    if (allowed(runs[0].value, s)) {
      dp[0][s] = emission(0, s);
    }
  }

  for (std::size_t t = 1; t < T; ++t) {
    for (int s = 0; s < kN; ++s) {
      if (!allowed(runs[t].value, s)) {
        continue;
      }
      const float em = emission(t, s);
      float best = kNeg;
      int bestPrev = -1;
      for (int p = 0; p < kN; ++p) {
        if (dp[t - 1][p] <= kNeg / 2) {
          continue;
        }
        const float cand = dp[t - 1][p] + trans[p][s] + em;
        if (cand > best) {
          best = cand;
          bestPrev = p;
        }
      }
      dp[t][s] = best;
      prev[t][s] = bestPrev;
    }
  }

  int bestState = 0;
  float bestScore = kNeg;
  for (int s = 0; s < kN; ++s) {
    if (dp[T - 1][s] > bestScore) {
      bestScore = dp[T - 1][s];
      bestState = s;
    }
  }

  std::vector<int> path(T, 0);
  path[T - 1] = bestState;
  for (std::size_t ti = T - 1; ti > 0; --ti) {
    const int p = prev[ti][path[ti]];
    path[ti - 1] = (p >= 0 ? p : path[ti]);
  }

  std::string text;
  std::string current;
  float confAcc = 0.0f;
  int confN = 0;

  for (std::size_t t = 0; t < T; ++t) {
    const int st = path[t];
    const float dot = std::max(1.0f, dotVec[t]);
    const float u = static_cast<float>(runs[t].length) / dot;
    if (st == kOnDot || st == kOnDash) {
      const float pDot = GaussianLike(u, 1.0f, cfg.hmmSigmaOnDot);
      const float pDash = GaussianLike(u, 3.0f, cfg.hmmSigmaOnDash);
      const float z = pDot + pDash + 1e-9f;
      const float postDot = pDot / z;
      const float postDash = pDash / z;
      if (st == kOnDot) {
        current.push_back('.');
        confAcc += postDot;
      } else {
        current.push_back('-');
        confAcc += postDash;
      }
      ++confN;
    } else if (st == kOffChar || st == kOffWord) {
      if (!current.empty()) {
        const auto it = MorseTable().find(current);
        text.push_back(it == MorseTable().end() ? '?' : it->second);
        current.clear();
      }
      if (st == kOffWord) {
        text.push_back(' ');
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
  const float conf = confN > 0 ? confAcc / static_cast<float>(confN) : 0.0f;
  SequenceDecode out;
  out.text = std::move(text);
  out.confidence = conf;
  out.avgLogLike = bestScore / static_cast<float>(std::max<std::size_t>(1, T));
  out.model = "hmm";
  return out;
}

float DurationLogLikeHsmm(float u, float mu, float sigma, float tailMix) {
  const float g = GaussianLike(u, mu, sigma);
  const float tail = 1.0f / (1.0f + std::fabs(u - mu));
  const float mix = std::max(0.0f, std::min(0.45f, tailMix));
  const float p = (1.0f - mix) * g + mix * tail;
  return std::log(std::max(1e-6f, p));
}

float TimeDependentTransition(int prevState, int nextState, float baseProb, float u,
                              float gain) {
  auto expected = [](int st) {
    if (st == 0 || st == 2) {
      return 1.0f;
    }
    if (st == 1 || st == 3) {
      return 3.0f;
    }
    return 7.0f;
  };
  const float mu = expected(nextState);
  const float shape = std::exp(-std::fabs(u - mu) * std::max(0.0f, gain));
  return std::log(std::max(1e-6f, baseProb * (0.55f + 0.45f * shape)));
}

SequenceDecode DecodeMorseHsmmExplicit(const std::vector<Run>& runs, int dotSamples,
                                       const DecoderConfig& cfg) {
  if (runs.empty() || dotSamples <= 0) {
    return {};
  }

  enum State { kOnDot = 0, kOnDash = 1, kOffIntra = 2, kOffChar = 3, kOffWord = 4, kN = 5 };
  const float kNeg = -1e30f;

  auto allowed = [](int runValue, int st) {
    if (runValue == 1) {
      return st == kOnDot || st == kOnDash;
    }
    return st == kOffIntra || st == kOffChar || st == kOffWord;
  };

  const float onToIntra = std::max(1e-4f, cfg.hsmmTransOnToIntra);
  const float onToChar = std::max(1e-4f, cfg.hsmmTransOnToChar);
  const float onToWord = std::max(1e-4f, cfg.hsmmTransOnToWord);
  const float onNorm = onToIntra + onToChar + onToWord;

  const float offToDot = std::max(1e-4f, cfg.hsmmTransOffToDot);
  const float offToDash = std::max(1e-4f, cfg.hsmmTransOffToDash);
  const float offNorm = offToDot + offToDash;

  float trans[kN][kN];
  for (int a = 0; a < kN; ++a) {
    for (int b = 0; b < kN; ++b) {
      trans[a][b] = -8.0f;
    }
  }
  trans[kOnDot][kOffIntra] = onToIntra / onNorm;
  trans[kOnDot][kOffChar] = onToChar / onNorm;
  trans[kOnDot][kOffWord] = onToWord / onNorm;
  trans[kOnDash][kOffIntra] = onToIntra / onNorm;
  trans[kOnDash][kOffChar] = onToChar / onNorm;
  trans[kOnDash][kOffWord] = onToWord / onNorm;
  trans[kOffIntra][kOnDot] = offToDot / offNorm;
  trans[kOffIntra][kOnDash] = offToDash / offNorm;
  trans[kOffChar][kOnDot] = offToDot / offNorm;
  trans[kOffChar][kOnDash] = offToDash / offNorm;
  trans[kOffWord][kOnDot] = offToDot / offNorm;
  trans[kOffWord][kOnDash] = offToDash / offNorm;

  const auto dotVec = AdaptiveDotPerRun(runs, static_cast<float>(dotSamples));
  const std::size_t T = runs.size();
  std::vector<std::array<float, kN>> dp(T);
  std::vector<std::array<int, kN>> prev(T);
  for (std::size_t t = 0; t < T; ++t) {
    for (int s = 0; s < kN; ++s) {
      dp[t][s] = kNeg;
      prev[t][s] = -1;
    }
  }

  auto durationLike = [&](std::size_t t, int s) {
    const float dot = std::max(1.0f, dotVec[t]);
    const float u = static_cast<float>(runs[t].length) / dot;
    if (s == kOnDot) {
      return DurationLogLikeHsmm(u, 1.0f, cfg.hsmmSigmaOnDot, cfg.hsmmDurationTailMix);
    }
    if (s == kOnDash) {
      return DurationLogLikeHsmm(u, 3.0f, cfg.hsmmSigmaOnDash, cfg.hsmmDurationTailMix);
    }
    if (s == kOffIntra) {
      return DurationLogLikeHsmm(u, 1.0f, cfg.hsmmSigmaOffIntra, cfg.hsmmDurationTailMix);
    }
    if (s == kOffChar) {
      return DurationLogLikeHsmm(u, 3.0f, cfg.hsmmSigmaOffChar, cfg.hsmmDurationTailMix);
    }
    return DurationLogLikeHsmm(u, 7.0f, cfg.hsmmSigmaOffWord, cfg.hsmmDurationTailMix);
  };

  for (int s = 0; s < kN; ++s) {
    if (allowed(runs[0].value, s)) {
      dp[0][s] = durationLike(0, s);
    }
  }

  const float gain = std::max(0.0f, cfg.hsmmTimeTransitionGain);
  for (std::size_t t = 1; t < T; ++t) {
    const float dot = std::max(1.0f, dotVec[t]);
    const float u = static_cast<float>(runs[t].length) / dot;
    for (int s = 0; s < kN; ++s) {
      if (!allowed(runs[t].value, s)) {
        continue;
      }
      const float dur = durationLike(t, s);
      float best = kNeg;
      int bestPrev = -1;
      for (int p = 0; p < kN; ++p) {
        if (dp[t - 1][p] <= kNeg / 2) {
          continue;
        }
        if (trans[p][s] <= 0.0f) {
          continue;
        }
        const float tr = TimeDependentTransition(p, s, trans[p][s], u, gain);
        const float cand = dp[t - 1][p] + tr + dur;
        if (cand > best) {
          best = cand;
          bestPrev = p;
        }
      }
      dp[t][s] = best;
      prev[t][s] = bestPrev;
    }
  }

  int bestState = 0;
  float bestScore = kNeg;
  for (int s = 0; s < kN; ++s) {
    if (dp[T - 1][s] > bestScore) {
      bestScore = dp[T - 1][s];
      bestState = s;
    }
  }

  std::vector<int> path(T, 0);
  path[T - 1] = bestState;
  for (std::size_t ti = T - 1; ti > 0; --ti) {
    const int p = prev[ti][path[ti]];
    path[ti - 1] = (p >= 0 ? p : path[ti]);
  }

  std::string text;
  std::string current;
  float confAcc = 0.0f;
  int confN = 0;

  for (std::size_t t = 0; t < T; ++t) {
    const int st = path[t];
    const float dot = std::max(1.0f, dotVec[t]);
    const float u = static_cast<float>(runs[t].length) / dot;
    if (st == kOnDot || st == kOnDash) {
      const float pDot = GaussianLike(u, 1.0f, cfg.hsmmSigmaOnDot);
      const float pDash = GaussianLike(u, 3.0f, cfg.hsmmSigmaOnDash);
      const float z = pDot + pDash + 1e-9f;
      if (st == kOnDot) {
        current.push_back('.');
        confAcc += pDot / z;
      } else {
        current.push_back('-');
        confAcc += pDash / z;
      }
      ++confN;
    } else if (st == kOffChar || st == kOffWord) {
      if (!current.empty()) {
        const auto it = MorseTable().find(current);
        text.push_back(it == MorseTable().end() ? '?' : it->second);
        current.clear();
      }
      if (st == kOffWord) {
        text.push_back(' ');
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

  SequenceDecode out;
  out.text = std::move(text);
  out.confidence = confN > 0 ? confAcc / static_cast<float>(confN) : 0.0f;
  out.avgLogLike = bestScore / static_cast<float>(std::max<std::size_t>(1, T));
  out.model = "hsmm";
  return out;
}

SequenceDecode DecodeMorseAuto(const std::vector<Run>& runs, int dotSamples, const DecoderConfig& cfg) {
  const auto hmm = DecodeMorseViterbiHmm(runs, dotSamples, cfg);
  const auto hsmm = DecodeMorseHsmmExplicit(runs, dotSamples, cfg);
  const float sh = DecodeTextHeuristicScore(hmm.text, hmm.confidence, hmm.avgLogLike);
  const float ss = DecodeTextHeuristicScore(hsmm.text, hsmm.confidence, hsmm.avgLogLike);
  if (ss > sh + 0.01f) {
    return hsmm;
  }
  return hmm;
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

int DeterministicTrackOrderKey(const ClusteredTrack& tr, unsigned int seed) {
  const int f = static_cast<int>(std::round(tr.freqHz * 10.0f));
  const int t = static_cast<int>(std::round(tr.startSec * 100.0f));
  const unsigned int h = static_cast<unsigned int>((f * 73856093) ^ (t * 19349663) ^
                                                    (tr.id * 83492791) ^
                                                    static_cast<int>(seed * 2654435761U));
  return static_cast<int>(h & 0x7fffffff);
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

std::vector<ClusteredTrack> SplitCochannelTracks(const std::vector<ClusteredTrack>& in,
                                                 int hopSize, int sampleRate,
                                                 const DecoderConfig& cfg) {
  if (!cfg.enableCochannelSeparation || cfg.cochannelMaxTracks <= 1) {
    return in;
  }
  std::vector<ClusteredTrack> out;
  int nextId = 1;
  for (const auto& c : in) {
    if (c.points.empty()) {
      continue;
    }
    std::vector<std::vector<TrackPoint>> buckets(static_cast<std::size_t>(cfg.cochannelMaxTracks));
    std::vector<int> lastFrame(static_cast<std::size_t>(cfg.cochannelMaxTracks), -100000);
    std::vector<float> lastFreq(static_cast<std::size_t>(cfg.cochannelMaxTracks), c.freqHz);

    for (const auto& p : c.points) {
      int best = -1;
      float bestCost = 1e30f;
      for (int k = 0; k < cfg.cochannelMaxTracks; ++k) {
        const float df = std::fabs(p.freqHz - lastFreq[static_cast<std::size_t>(k)]);
        const int dt = p.frame - lastFrame[static_cast<std::size_t>(k)];
        if (!buckets[static_cast<std::size_t>(k)].empty()) {
          if (df > cfg.cochannelMaxStepHz || dt > cfg.cochannelMaxGapFrames) {
            continue;
          }
        }
        const float cst = df + 0.5f * static_cast<float>(std::max(0, dt - 1));
        if (cst < bestCost) {
          bestCost = cst;
          best = k;
        }
      }
      if (best < 0) {
        int empt = -1;
        for (int k = 0; k < cfg.cochannelMaxTracks; ++k) {
          if (buckets[static_cast<std::size_t>(k)].empty()) {
            empt = k;
            break;
          }
        }
        best = (empt >= 0 ? empt : 0);
      }
      buckets[static_cast<std::size_t>(best)].push_back(p);
      lastFrame[static_cast<std::size_t>(best)] = p.frame;
      lastFreq[static_cast<std::size_t>(best)] = p.freqHz;
    }

    for (auto& b : buckets) {
      if (b.size() < 6) {
        continue;
      }
      ClusteredTrack s;
      s.id = nextId++;
      s.points = std::move(b);
      std::sort(s.points.begin(), s.points.end(), [](const TrackPoint& a, const TrackPoint& b) {
        return a.frame < b.frame;
      });
      s.freqHz = MeanFrequency(s.points);
      const int firstFrame = s.points.front().frame;
      const int lastFrame = s.points.back().frame;
      s.startSec = static_cast<float>(firstFrame * hopSize) / static_cast<float>(sampleRate);
      s.endSec = static_cast<float>(lastFrame * hopSize) / static_cast<float>(sampleRate);
      out.push_back(std::move(s));
    }
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
      if (r.confidence >= e.confidence) {
        e.confidence = r.confidence;
        e.confidenceRaw = r.confidenceRaw;
        e.confidenceCalibrated = r.confidenceCalibrated;
        e.decoderModel = r.decoderModel;
        e.priorMatched = r.priorMatched;
        e.priorCandidates = r.priorCandidates;
      }
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

std::vector<DecodeResult> ApplyStrictBeaconMode(const std::vector<DecodeResult>& in,
                                                int minRepeats,
                                                int* rejectedCount) {
  if (rejectedCount) {
    *rejectedCount = 0;
  }
  if (minRepeats <= 1) {
    return in;
  }
  std::vector<DecodeResult> out;
  out.reserve(in.size());
  for (const auto& r : in) {
    if (r.hitCount >= minRepeats) {
      out.push_back(r);
    } else if (rejectedCount) {
      ++(*rejectedCount);
    }
  }
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
  const FrontEndConfig frontCfg{
      cfg.enableBandLimit,
      cfg.bandLowHz,
      cfg.bandHighHz,
      cfg.enableAutoNotch,
      cfg.autoNotchMaxCount,
      cfg.autoNotchSnrDb,
      cfg.enableImpulseBlanker,
      cfg.impulseBlankerSigma,
      cfg.impulseBlankerHalfWindow,
  };
  const auto denoised = ApplyFrontEndDenoise(samples, sampleRate, frontCfg);
  std::vector<float> work = DecimateAverage(denoised, decim);
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

  const CandidateDetectorConfig detCfg{
      cfg.madFactor,
      cfg.guardBins,
      cfg.enableCfar2d,
      cfg.cfarTrainTime,
      cfg.cfarGuardTime,
      cfg.cfarTrainFreq,
      cfg.cfarGuardFreq,
      cfg.cfarScale,
  };
  const auto candidates = DetectCandidateBinsMadCfar2D(spec, detCfg);
  if (stats) {
    int bins = 0;
    for (const auto& v : candidates) {
      bins += static_cast<int>(v.size());
    }
    stats->candidateBinCount = bins;
  }
  Report(progress, 35, "candidate-detection");

  const auto tracks =
      cfg.useAmtcFull
          ? TrackTonesAmtcFull(spec, candidates, cfg.maxTrackStepBins, cfg.minTrackFrames,
                               cfg.sustainPenalty, cfg.maxTrackGapFrames)
          : TrackTonesAmtcLite(spec, candidates, cfg.maxTrackStepBins, cfg.minTrackFrames,
                               cfg.sustainPenalty);
  if (stats) {
    stats->trackCount = static_cast<int>(tracks.size());
  }
  Report(progress, 47, "tracking");

  const auto clusteredRaw = ClusterTracks(tracks, cfg.hopSize, workRate, cfg.clusterFreqTolHz,
                                          cfg.clusterGapSec);
  const auto clustered = SplitCochannelTracks(clusteredRaw, cfg.hopSize, workRate, cfg);
  if (stats) {
    stats->clusteredCount = static_cast<int>(clustered.size());
  }
  Report(progress, 58, "cluster-merge");

  const auto priors = cfg.enableFreqPriors ? LoadFreqPriors(cfg.freqPriorFile) : std::vector<PriorEntry>{};

  int filteredByFreq = 0;
  int plausibleRejected = 0;
  std::vector<DecodeResult> decoded;
  decoded.reserve(clustered.size());

  const int total = std::max(1, static_cast<int>(clustered.size()));
  std::vector<ClusteredTrack> ordered = clustered;
  std::sort(ordered.begin(), ordered.end(), [&](const ClusteredTrack& a, const ClusteredTrack& b) {
    const int ka = DeterministicTrackOrderKey(a, cfg.deterministicSeed);
    const int kb = DeterministicTrackOrderKey(b, cfg.deterministicSeed);
    if (ka != kb) {
      return ka < kb;
    }
    return a.id < b.id;
  });

  std::atomic<int> nextIdx{0};
  std::atomic<int> done{0};
  std::mutex outMutex;

  const int workerN = std::max(1, cfg.decodeThreads);
  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(workerN));

  auto workerFn = [&]() {
    while (true) {
      const int idx = nextIdx.fetch_add(1);
      if (idx >= static_cast<int>(ordered.size())) {
        break;
      }
      const auto& tr = ordered[static_cast<std::size_t>(idx)];
    const float f0 = tr.freqHz;
    if (!InNdbRange(f0, cfg)) {
      {
        std::lock_guard<std::mutex> lock(outMutex);
        ++filteredByFreq;
      }
      const int nowDone = done.fetch_add(1) + 1;
      Report(progress, 58 + (28 * nowDone) / total, "decode-clusters");
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
    SequenceDecode decodedText;
    if (cfg.decoderModel == "hmm") {
      decodedText = DecodeMorseViterbiHmm(runs, dotSamples, cfg);
    } else if (cfg.decoderModel == "hsmm") {
      decodedText = DecodeMorseHsmmExplicit(runs, dotSamples, cfg);
    } else {
      decodedText = DecodeMorseAuto(runs, dotSamples, cfg);
    }

    if (decodedText.text.empty()) {
      const int nowDone = done.fetch_add(1) + 1;
      Report(progress, 58 + (28 * nowDone) / total, "decode-clusters");
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
    r.text = decodedText.text;
    r.morse = decodedText.text;
    r.confidenceRaw = decodedText.confidence;
    r.confidenceCalibrated = CalibrateConfidence(decodedText.confidence, cfg);
    r.confidence = r.confidenceCalibrated;
    r.decoderModel = decodedText.model;
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

    if (cfg.enableFreqPriors) {
      const auto cand = FindPriorCandidates(r.freqHz, priors, cfg.freqPriorTolHz);
      r.priorCandidates = JoinPipe(cand);
      r.priorMatched = (!r.plausibleId.empty() && ContainsToken(cand, r.plausibleId));
      if (cfg.requirePriorMatch && !cand.empty() && !r.priorMatched) {
        {
          std::lock_guard<std::mutex> lock(outMutex);
          ++plausibleRejected;
        }
        const int nowDone = done.fetch_add(1) + 1;
        Report(progress, 58 + (28 * nowDone) / total, "decode-clusters");
        continue;
      }
    }
    if (cfg.requirePlausibleId &&
        (r.plausibleId.empty() || r.plausibleIdScore < cfg.plausibleIdMinScore)) {
      {
        std::lock_guard<std::mutex> lock(outMutex);
        ++plausibleRejected;
      }
      const int nowDone = done.fetch_add(1) + 1;
      Report(progress, 58 + (28 * nowDone) / total, "decode-clusters");
      continue;
    }
    if (!r.plausibleId.empty()) {
      r.text = r.plausibleId;
    }

      {
        std::lock_guard<std::mutex> lock(outMutex);
        decoded.push_back(std::move(r));
      }
      const int nowDone = done.fetch_add(1) + 1;
      Report(progress, 58 + (28 * nowDone) / total, "decode-clusters");
    }
  };

  for (int i = 0; i < workerN; ++i) {
    workers.emplace_back(workerFn);
  }
  for (auto& th : workers) {
    th.join();
  }

  auto dedup = DedupById(decoded, cfg.dedupFreqTolHz);
  int strictRejected = 0;
  if (cfg.strictBeaconMode) {
    dedup = ApplyStrictBeaconMode(dedup, cfg.strictMinRepeats, &strictRejected);
  }
  if (stats) {
    stats->filteredByFrequency = filteredByFreq;
    stats->dedupCount = static_cast<int>(dedup.size());
    stats->plausibleIdRejected = plausibleRejected;
    stats->strictRejected = strictRejected;
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

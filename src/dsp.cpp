#include "dsp.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <numeric>

#if defined(NDB_HAS_FFTW3F)
#include <fftw3.h>
#endif

namespace ndb {
namespace {

constexpr float kPi = 3.14159265358979323846f;

std::vector<float> Hann(int n) {
  std::vector<float> w(static_cast<std::size_t>(n), 0.0f);
  if (n <= 1) {
    return w;
  }
  for (int i = 0; i < n; ++i) {
    w[static_cast<std::size_t>(i)] = 0.5f - 0.5f * std::cos(2.0f * kPi * static_cast<float>(i) /
                                                             static_cast<float>(n - 1));
  }
  return w;
}

float Median(std::vector<float> values) {
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

float MadSigma(const std::vector<float>& values) {
  if (values.empty()) {
    return 0.0f;
  }
  const float med = Median(values);
  std::vector<float> dev(values.size(), 0.0f);
  for (std::size_t i = 0; i < values.size(); ++i) {
    dev[i] = std::fabs(values[i] - med);
  }
  return 1.4826f * Median(dev) + 1e-12f;
}

bool IsPowerOfTwo(int n) {
  return n > 0 && (n & (n - 1)) == 0;
}

void FftInPlace(std::vector<std::complex<float>>& a) {
  const int n = static_cast<int>(a.size());
  int j = 0;
  for (int i = 1; i < n; ++i) {
    int bit = n >> 1;
    while (j & bit) {
      j ^= bit;
      bit >>= 1;
    }
    j ^= bit;
    if (i < j) {
      std::swap(a[static_cast<std::size_t>(i)], a[static_cast<std::size_t>(j)]);
    }
  }

  for (int len = 2; len <= n; len <<= 1) {
    const float ang = -2.0f * kPi / static_cast<float>(len);
    const std::complex<float> wlen(std::cos(ang), std::sin(ang));
    for (int i = 0; i < n; i += len) {
      std::complex<float> w(1.0f, 0.0f);
      const int half = len >> 1;
      for (int j2 = 0; j2 < half; ++j2) {
        const auto u = a[static_cast<std::size_t>(i + j2)];
        const auto v = a[static_cast<std::size_t>(i + j2 + half)] * w;
        a[static_cast<std::size_t>(i + j2)] = u + v;
        a[static_cast<std::size_t>(i + j2 + half)] = u - v;
        w *= wlen;
      }
    }
  }
}

std::vector<float> FftMagnitude(const float* frame, int n, int outBins) {
  std::vector<std::complex<float>> x(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    x[static_cast<std::size_t>(i)] = std::complex<float>(frame[i], 0.0f);
  }
  FftInPlace(x);
  const float scale = 1.0f / static_cast<float>(n);
  std::vector<float> mag(static_cast<std::size_t>(outBins), 0.0f);
  for (int k = 0; k < outBins; ++k) {
    mag[static_cast<std::size_t>(k)] = std::abs(x[static_cast<std::size_t>(k)]) * scale;
  }
  return mag;
}

#if defined(NDB_HAS_FFTW3F)
std::vector<float> FftwMagnitude(const float* frame, int n, int outBins) {
  std::vector<float> mag(static_cast<std::size_t>(outBins), 0.0f);
  if (n <= 0) {
    return mag;
  }
  float* in = static_cast<float*>(fftwf_malloc(sizeof(float) * static_cast<std::size_t>(n)));
  fftwf_complex* out = static_cast<fftwf_complex*>(
      fftwf_malloc(sizeof(fftwf_complex) * static_cast<std::size_t>(outBins)));
  if (!in || !out) {
    if (in) {
      fftwf_free(in);
    }
    if (out) {
      fftwf_free(out);
    }
    return mag;
  }
  for (int i = 0; i < n; ++i) {
    in[static_cast<std::size_t>(i)] = frame[i];
  }
  fftwf_plan p = fftwf_plan_dft_r2c_1d(n, in, out, FFTW_ESTIMATE);
  if (!p) {
    fftwf_free(in);
    fftwf_free(out);
    return mag;
  }
  fftwf_execute(p);
  const float scale = 1.0f / static_cast<float>(n);
  for (int k = 0; k < outBins; ++k) {
    const float re = out[static_cast<std::size_t>(k)][0];
    const float im = out[static_cast<std::size_t>(k)][1];
    mag[static_cast<std::size_t>(k)] = std::sqrt(re * re + im * im) * scale;
  }
  fftwf_destroy_plan(p);
  fftwf_free(in);
  fftwf_free(out);
  return mag;
}
#endif

std::vector<float> DftMagnitude(const float* frame, int n, int outBins) {
  std::vector<float> mag(static_cast<std::size_t>(outBins), 0.0f);
  const float scale = 1.0f / static_cast<float>(n);
  for (int k = 0; k < outBins; ++k) {
    float re = 0.0f;
    float im = 0.0f;
    for (int t = 0; t < n; ++t) {
      const float a = -2.0f * kPi * static_cast<float>(k) * static_cast<float>(t) /
                      static_cast<float>(n);
      re += frame[t] * std::cos(a);
      im += frame[t] * std::sin(a);
    }
    mag[static_cast<std::size_t>(k)] = std::sqrt(re * re + im * im) * scale;
  }
  return mag;
}

std::vector<float> ComputeMagnitudeSpectrum(const float* frame, int n, int outBins) {
#if defined(NDB_HAS_FFTW3F)
  return FftwMagnitude(frame, n, outBins);
#else
  if (IsPowerOfTwo(n)) {
    return FftMagnitude(frame, n, outBins);
  }
  return DftMagnitude(frame, n, outBins);
#endif
}

float Clamp01(float x) {
  if (x < 0.0f) {
    return 0.0f;
  }
  if (x > 1.0f) {
    return 1.0f;
  }
  return x;
}

std::vector<float> OnePoleLowPass(const std::vector<float>& x, int sampleRate, float cutoffHz) {
  if (x.empty() || sampleRate <= 0 || cutoffHz <= 0.0f) {
    return x;
  }
  const float dt = 1.0f / static_cast<float>(sampleRate);
  const float rc = 1.0f / (2.0f * kPi * cutoffHz);
  const float a = dt / (rc + dt);
  std::vector<float> y(x.size(), 0.0f);
  y[0] = x[0];
  for (std::size_t i = 1; i < x.size(); ++i) {
    y[i] = y[i - 1] + a * (x[i] - y[i - 1]);
  }
  return y;
}

std::vector<float> OnePoleHighPass(const std::vector<float>& x, int sampleRate, float cutoffHz) {
  if (x.empty() || sampleRate <= 0 || cutoffHz <= 0.0f) {
    return x;
  }
  const float dt = 1.0f / static_cast<float>(sampleRate);
  const float rc = 1.0f / (2.0f * kPi * cutoffHz);
  const float a = rc / (rc + dt);
  std::vector<float> y(x.size(), 0.0f);
  y[0] = x[0];
  for (std::size_t i = 1; i < x.size(); ++i) {
    y[i] = a * (y[i - 1] + x[i] - x[i - 1]);
  }
  return y;
}

std::vector<float> ApplyBiquadNotch(const std::vector<float>& x, int sampleRate, float f0, float q) {
  if (x.empty() || sampleRate <= 0 || f0 <= 0.0f || f0 >= 0.5f * static_cast<float>(sampleRate)) {
    return x;
  }
  const float w0 = 2.0f * kPi * f0 / static_cast<float>(sampleRate);
  const float alpha = std::sin(w0) / (2.0f * std::max(0.2f, q));
  const float b0 = 1.0f;
  const float b1 = -2.0f * std::cos(w0);
  const float b2 = 1.0f;
  const float a0 = 1.0f + alpha;
  const float a1 = -2.0f * std::cos(w0);
  const float a2 = 1.0f - alpha;

  const float bb0 = b0 / a0;
  const float bb1 = b1 / a0;
  const float bb2 = b2 / a0;
  const float aa1 = a1 / a0;
  const float aa2 = a2 / a0;

  std::vector<float> y(x.size(), 0.0f);
  float x1 = 0.0f;
  float x2 = 0.0f;
  float y1 = 0.0f;
  float y2 = 0.0f;
  for (std::size_t i = 0; i < x.size(); ++i) {
    const float v = bb0 * x[i] + bb1 * x1 + bb2 * x2 - aa1 * y1 - aa2 * y2;
    y[i] = v;
    x2 = x1;
    x1 = x[i];
    y2 = y1;
    y1 = v;
  }
  return y;
}

std::vector<float> AutoNotch(const std::vector<float>& x, int sampleRate, const FrontEndConfig& cfg) {
  if (!cfg.enableAutoNotch || cfg.autoNotchMaxCount <= 0 || x.size() < 1024U || sampleRate <= 0) {
    return x;
  }
  const int n = 1024;
  const int bins = (n / 2) + 1;
  const auto win = Hann(n);
  std::vector<float> frame(static_cast<std::size_t>(n), 0.0f);
  const std::size_t start = (x.size() > static_cast<std::size_t>(n)) ? (x.size() - n) : 0U;
  for (int i = 0; i < n && (start + static_cast<std::size_t>(i)) < x.size(); ++i) {
    frame[static_cast<std::size_t>(i)] = x[start + static_cast<std::size_t>(i)] * win[static_cast<std::size_t>(i)];
  }
  const auto mag = ComputeMagnitudeSpectrum(frame.data(), n, bins);
  const float med = Median(mag);
  const float sigma = MadSigma(mag);
  const float thr = med + std::pow(10.0f, cfg.autoNotchSnrDb / 20.0f) * sigma;

  struct Pk {
    int b = 0;
    float v = 0.0f;
  };
  std::vector<Pk> peaks;
  for (int b = 2; b < bins - 2; ++b) {
    if (mag[static_cast<std::size_t>(b)] <= thr) {
      continue;
    }
    if (mag[static_cast<std::size_t>(b)] >= mag[static_cast<std::size_t>(b - 1)] &&
        mag[static_cast<std::size_t>(b)] >= mag[static_cast<std::size_t>(b + 1)]) {
      peaks.push_back(Pk{b, mag[static_cast<std::size_t>(b)]});
    }
  }
  std::sort(peaks.begin(), peaks.end(), [](const Pk& a, const Pk& b) { return a.v > b.v; });

  std::vector<float> y = x;
  const int count = std::min(cfg.autoNotchMaxCount, static_cast<int>(peaks.size()));
  for (int i = 0; i < count; ++i) {
    const float f0 = static_cast<float>(peaks[static_cast<std::size_t>(i)].b) *
                     static_cast<float>(sampleRate) / static_cast<float>(n);
    y = ApplyBiquadNotch(y, sampleRate, f0, 20.0f);
  }
  return y;
}

std::vector<float> ImpulseBlanker(const std::vector<float>& x, const FrontEndConfig& cfg) {
  if (!cfg.enableImpulseBlanker || x.size() < 8U) {
    return x;
  }
  std::vector<float> absx(x.size(), 0.0f);
  for (std::size_t i = 0; i < x.size(); ++i) {
    absx[i] = std::fabs(x[i]);
  }
  const float med = Median(absx);
  const float sigma = MadSigma(absx);
  const float thr = med + std::max(1.0f, cfg.impulseBlankerSigma) * sigma;
  const int w = std::max(1, cfg.impulseBlankerHalfWindow);
  std::vector<float> y = x;
  for (std::size_t i = 0; i < x.size(); ++i) {
    if (std::fabs(x[i]) <= thr) {
      continue;
    }
    const std::size_t a = (i > static_cast<std::size_t>(w)) ? (i - static_cast<std::size_t>(w)) : 0U;
    const std::size_t b = std::min(x.size() - 1, i + static_cast<std::size_t>(w));
    double s = 0.0;
    int n = 0;
    for (std::size_t j = a; j <= b; ++j) {
      if (j == i) {
        continue;
      }
      s += x[j];
      ++n;
    }
    if (n > 0) {
      y[i] = static_cast<float>(s / static_cast<double>(n));
    }
  }
  return y;
}

float CfarCellNoise(const Spectrogram& spec, int f0, int b0, int trainT, int guardT, int trainF,
                    int guardF) {
  double s = 0.0;
  int n = 0;
  const int fMin = std::max(0, f0 - (trainT + guardT));
  const int fMax = std::min(spec.frameCount - 1, f0 + trainT + guardT);
  const int bMin = std::max(0, b0 - (trainF + guardF));
  const int bMax = std::min(spec.binCount - 1, b0 + trainF + guardF);
  for (int f = fMin; f <= fMax; ++f) {
    for (int b = bMin; b <= bMax; ++b) {
      const int df = std::abs(f - f0);
      const int db = std::abs(b - b0);
      if (df <= guardT && db <= guardF) {
        continue;
      }
      if (df <= trainT + guardT && db <= trainF + guardF) {
        s += spec.At(f, b);
        ++n;
      }
    }
  }
  if (n <= 0) {
    return 0.0f;
  }
  return static_cast<float>(s / static_cast<double>(n));
}

}  // namespace

std::vector<float> ApplyFrontEndDenoise(const std::vector<float>& samples, int sampleRate,
                                        const FrontEndConfig& cfg) {
  std::vector<float> y = samples;
  if (cfg.enableBandLimit) {
    y = OnePoleHighPass(y, sampleRate, std::max(10.0f, cfg.bandLowHz));
    y = OnePoleLowPass(y, sampleRate, std::max(cfg.bandLowHz + 20.0f, cfg.bandHighHz));
  }
  y = AutoNotch(y, sampleRate, cfg);
  y = ImpulseBlanker(y, cfg);
  return y;
}

Spectrogram ComputeSpectrogram(const std::vector<float>& samples, int sampleRate, int fftSize,
                               int hopSize) {
  Spectrogram spec;
  spec.fftSize = fftSize;
  spec.hopSize = hopSize;
  spec.sampleRate = sampleRate;
  spec.binCount = (fftSize / 2) + 1;
  if (samples.size() < static_cast<std::size_t>(fftSize) || fftSize <= 0 || hopSize <= 0 ||
      sampleRate <= 0) {
    return spec;
  }

  const int frameCount =
      1 + static_cast<int>((samples.size() - static_cast<std::size_t>(fftSize)) /
                           static_cast<std::size_t>(hopSize));
  spec.frameCount = frameCount;
  spec.magnitude.resize(static_cast<std::size_t>(frameCount) * static_cast<std::size_t>(spec.binCount),
                        0.0f);

  const std::vector<float> window = Hann(fftSize);
  std::vector<float> frame(static_cast<std::size_t>(fftSize), 0.0f);

  for (int f = 0; f < frameCount; ++f) {
    const std::size_t start = static_cast<std::size_t>(f) * static_cast<std::size_t>(hopSize);
    for (int i = 0; i < fftSize; ++i) {
      frame[static_cast<std::size_t>(i)] =
          samples[start + static_cast<std::size_t>(i)] * window[static_cast<std::size_t>(i)];
    }
    const std::vector<float> mag = ComputeMagnitudeSpectrum(frame.data(), fftSize, spec.binCount);
    const std::size_t row = static_cast<std::size_t>(f) * static_cast<std::size_t>(spec.binCount);
    std::copy(mag.begin(), mag.end(), spec.magnitude.begin() + static_cast<std::ptrdiff_t>(row));
  }

  return spec;
}

std::vector<std::vector<int>> DetectCandidateBinsMad(const Spectrogram& spec, float madFactor,
                                                      int guardBins) {
  std::vector<std::vector<int>> out(static_cast<std::size_t>(spec.frameCount));
  for (int f = 0; f < spec.frameCount; ++f) {
    std::vector<float> row(static_cast<std::size_t>(spec.binCount));
    for (int b = 0; b < spec.binCount; ++b) {
      row[static_cast<std::size_t>(b)] = spec.At(f, b);
    }
    const float med = Median(row);
    const float sigma = MadSigma(row);
    const float thr = med + madFactor * sigma;

    std::vector<int> bins;
    for (int b = std::max(guardBins, 1); b < spec.binCount - guardBins - 1; ++b) {
      const float v = row[static_cast<std::size_t>(b)];
      if (v <= thr) {
        continue;
      }
      if (v >= row[static_cast<std::size_t>(b - 1)] && v >= row[static_cast<std::size_t>(b + 1)]) {
        bins.push_back(b);
      }
    }
    out[static_cast<std::size_t>(f)] = std::move(bins);
  }
  return out;
}

std::vector<std::vector<int>> DetectCandidateBinsMadCfar2D(const Spectrogram& spec,
                                                            const CandidateDetectorConfig& cfg) {
  auto mad = DetectCandidateBinsMad(spec, cfg.madFactor, cfg.guardBins);
  if (!cfg.enableCfar2d || spec.frameCount <= 0 || spec.binCount <= 0) {
    return mad;
  }

  std::vector<std::vector<int>> out(static_cast<std::size_t>(spec.frameCount));
  for (int f = 0; f < spec.frameCount; ++f) {
    for (int b : mad[static_cast<std::size_t>(f)]) {
      const float v = spec.At(f, b);
      const float noise = CfarCellNoise(spec, f, b, cfg.cfarTrainTime, cfg.cfarGuardTime,
                                        cfg.cfarTrainFreq, cfg.cfarGuardFreq);
      const float thr = noise * std::max(1.1f, cfg.cfarScale);
      if (v > thr) {
        out[static_cast<std::size_t>(f)].push_back(b);
      }
    }
  }
  return out;
}

std::vector<Track> TrackTonesAmtcLite(const Spectrogram& spec,
                                      const std::vector<std::vector<int>>& candidates,
                                      int maxStepBins, int minTrackLengthFrames,
                                      float sustainPenalty) {
  struct ActiveTrack {
    Track track;
    int lastFrame = -1;
    int lastBin = -1;
    float cost = 0.0f;
  };

  std::vector<Track> completed;
  std::vector<ActiveTrack> active;
  int nextId = 1;

  for (int f = 0; f < spec.frameCount; ++f) {
    const auto& bins = candidates[static_cast<std::size_t>(f)];
    std::vector<bool> used(static_cast<std::size_t>(bins.size()), false);

    for (auto& tr : active) {
      int bestIdx = -1;
      float bestCost = std::numeric_limits<float>::max();
      for (std::size_t i = 0; i < bins.size(); ++i) {
        if (used[i]) {
          continue;
        }
        const int step = std::abs(bins[i] - tr.lastBin);
        if (step > maxStepBins) {
          continue;
        }
        const float localEnergy = spec.At(f, bins[i]);
        const float c = static_cast<float>(step) - sustainPenalty * localEnergy;
        if (c < bestCost) {
          bestCost = c;
          bestIdx = static_cast<int>(i);
        }
      }
      if (bestIdx >= 0) {
        used[static_cast<std::size_t>(bestIdx)] = true;
        const int b = bins[static_cast<std::size_t>(bestIdx)];
        const float freq = static_cast<float>(b) * static_cast<float>(spec.sampleRate) /
                           static_cast<float>(spec.fftSize);
        const float score = spec.At(f, b);
        tr.track.points.push_back(TrackPoint{f, freq, score});
        tr.lastFrame = f;
        tr.lastBin = b;
        tr.cost += bestCost;
      }
    }

    for (std::size_t i = 0; i < bins.size(); ++i) {
      if (used[i]) {
        continue;
      }
      ActiveTrack tr;
      tr.track.id = nextId++;
      const int b = bins[i];
      const float freq = static_cast<float>(b) * static_cast<float>(spec.sampleRate) /
                         static_cast<float>(spec.fftSize);
      tr.track.points.push_back(TrackPoint{f, freq, spec.At(f, b)});
      tr.lastFrame = f;
      tr.lastBin = b;
      active.push_back(std::move(tr));
    }

    std::vector<ActiveTrack> next;
    next.reserve(active.size());
    for (auto& tr : active) {
      if (f - tr.lastFrame > 2) {
        if (static_cast<int>(tr.track.points.size()) >= minTrackLengthFrames) {
          completed.push_back(std::move(tr.track));
        }
      } else {
        next.push_back(std::move(tr));
      }
    }
    active = std::move(next);
  }

  for (auto& tr : active) {
    if (static_cast<int>(tr.track.points.size()) >= minTrackLengthFrames) {
      completed.push_back(std::move(tr.track));
    }
  }
  return completed;
}

std::vector<Track> TrackTonesAmtcFull(const Spectrogram& spec,
                                      const std::vector<std::vector<int>>& candidates,
                                      int maxStepBins, int minTrackLengthFrames,
                                      float sustainPenalty, int maxGapFrames) {
  const int T = spec.frameCount;
  if (T <= 0) {
    return {};
  }

  struct Node {
    int frame = 0;
    int bin = 0;
    float energy = 0.0f;
  };

  std::vector<Node> nodes;
  std::vector<std::vector<int>> idxByFrame(static_cast<std::size_t>(T));
  for (int f = 0; f < T; ++f) {
    for (int b : candidates[static_cast<std::size_t>(f)]) {
      const int idx = static_cast<int>(nodes.size());
      nodes.push_back(Node{f, b, spec.At(f, b)});
      idxByFrame[static_cast<std::size_t>(f)].push_back(idx);
    }
  }

  const int N = static_cast<int>(nodes.size());
  if (N == 0) {
    return {};
  }

  std::vector<float> best(static_cast<std::size_t>(N), -1e30f);
  std::vector<int> prev(static_cast<std::size_t>(N), -1);
  std::vector<int> len(static_cast<std::size_t>(N), 1);

  for (int i = 0; i < N; ++i) {
    best[static_cast<std::size_t>(i)] = nodes[static_cast<std::size_t>(i)].energy;
    const int f = nodes[static_cast<std::size_t>(i)].frame;
    const int b = nodes[static_cast<std::size_t>(i)].bin;
    const int fMin = std::max(0, f - std::max(1, maxGapFrames));
    for (int pf = fMin; pf < f; ++pf) {
      for (int j : idxByFrame[static_cast<std::size_t>(pf)]) {
        const int db = std::abs(b - nodes[static_cast<std::size_t>(j)].bin);
        if (db > maxStepBins) {
          continue;
        }
        const int dt = f - nodes[static_cast<std::size_t>(j)].frame;
        const float gapCost = 0.35f * static_cast<float>(dt - 1);
        const float stepCost = 0.12f * static_cast<float>(db);
        const float cand = best[static_cast<std::size_t>(j)] + nodes[static_cast<std::size_t>(i)].energy -
                           stepCost - gapCost + sustainPenalty;
        if (cand > best[static_cast<std::size_t>(i)]) {
          best[static_cast<std::size_t>(i)] = cand;
          prev[static_cast<std::size_t>(i)] = j;
          len[static_cast<std::size_t>(i)] = len[static_cast<std::size_t>(j)] + 1;
        }
      }
    }
  }

  std::vector<int> order(static_cast<std::size_t>(N));
  std::iota(order.begin(), order.end(), 0);
  std::sort(order.begin(), order.end(), [&](int a, int b) {
    return best[static_cast<std::size_t>(a)] > best[static_cast<std::size_t>(b)];
  });

  std::vector<bool> used(static_cast<std::size_t>(N), false);
  std::vector<Track> tracks;
  int nextId = 1;
  for (int root : order) {
    if (used[static_cast<std::size_t>(root)] || len[static_cast<std::size_t>(root)] < minTrackLengthFrames) {
      continue;
    }
    std::vector<int> path;
    int cur = root;
    bool overlap = false;
    while (cur >= 0) {
      if (used[static_cast<std::size_t>(cur)]) {
        overlap = true;
        break;
      }
      path.push_back(cur);
      cur = prev[static_cast<std::size_t>(cur)];
    }
    if (overlap || static_cast<int>(path.size()) < minTrackLengthFrames) {
      continue;
    }
    std::reverse(path.begin(), path.end());
    Track tr;
    tr.id = nextId++;
    tr.points.reserve(path.size());
    for (int idx : path) {
      used[static_cast<std::size_t>(idx)] = true;
      const auto& n = nodes[static_cast<std::size_t>(idx)];
      const float freq = static_cast<float>(n.bin) * static_cast<float>(spec.sampleRate) /
                         static_cast<float>(spec.fftSize);
      tr.points.push_back(TrackPoint{n.frame, freq, n.energy});
    }
    tracks.push_back(std::move(tr));
  }

  return tracks;
}

std::vector<std::complex<float>> MixDown(const std::vector<float>& x, int sampleRate,
                                         float freqHz) {
  std::vector<std::complex<float>> y(x.size());
  const float w = -2.0f * kPi * freqHz / static_cast<float>(sampleRate);
  for (std::size_t n = 0; n < x.size(); ++n) {
    const float ph = w * static_cast<float>(n);
    y[n] = std::complex<float>(x[n] * std::cos(ph), x[n] * std::sin(ph));
  }
  return y;
}

std::vector<float> DecimateAverage(const std::vector<float>& x, int factor) {
  if (factor <= 1 || x.empty()) {
    return x;
  }
  const std::size_t n = x.size() / static_cast<std::size_t>(factor);
  std::vector<float> y(n, 0.0f);
  for (std::size_t i = 0; i < n; ++i) {
    double acc = 0.0;
    const std::size_t base = i * static_cast<std::size_t>(factor);
    for (int k = 0; k < factor; ++k) {
      acc += x[base + static_cast<std::size_t>(k)];
    }
    y[i] = static_cast<float>(acc / static_cast<double>(factor));
  }
  return y;
}

std::vector<float> Envelope(const std::vector<std::complex<float>>& iq) {
  std::vector<float> env(iq.size(), 0.0f);
  for (std::size_t i = 0; i < iq.size(); ++i) {
    env[i] = std::abs(iq[i]);
  }
  return env;
}

std::vector<float> ExponentialSmoother(const std::vector<float>& x, float alpha) {
  if (x.empty()) {
    return {};
  }
  std::vector<float> y(x.size(), 0.0f);
  y[0] = x[0];
  const float a = std::clamp(alpha, 0.001f, 0.999f);
  for (std::size_t i = 1; i < x.size(); ++i) {
    y[i] = a * x[i] + (1.0f - a) * y[i - 1];
  }
  return y;
}

std::vector<float> Boxcar(const std::vector<float>& x, int length) {
  if (x.empty() || length <= 1) {
    return x;
  }
  std::vector<float> y(x.size(), 0.0f);
  double acc = 0.0;
  for (std::size_t i = 0; i < x.size(); ++i) {
    acc += x[i];
    if (static_cast<int>(i) >= length) {
      acc -= x[i - static_cast<std::size_t>(length)];
    }
    const int denom = std::min(static_cast<int>(i) + 1, length);
    y[i] = static_cast<float>(acc / static_cast<double>(denom));
  }
  return y;
}

float RobustMadThreshold(const std::vector<float>& x, float k) {
  if (x.empty()) {
    return 0.0f;
  }
  const float med = Median(x);
  std::vector<float> dev(x.size());
  for (std::size_t i = 0; i < x.size(); ++i) {
    dev[i] = std::fabs(x[i] - med);
  }
  const float sigma = 1.4826f * Median(dev) + 1e-12f;
  return med + k * sigma;
}

std::vector<int> BinaryByThreshold(const std::vector<float>& x, float threshold) {
  std::vector<int> out(x.size(), 0);
  for (std::size_t i = 0; i < x.size(); ++i) {
    out[i] = x[i] > threshold ? 1 : 0;
  }
  return out;
}

}  // namespace ndb

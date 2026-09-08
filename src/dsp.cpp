#include "dsp.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

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

std::vector<float> DftMagnitude(const float* frame, int n, int outBins) {
  std::vector<float> mag(static_cast<std::size_t>(outBins), 0.0f);
  const float scale = 1.0f / static_cast<float>(n);
  for (int k = 0; k < outBins; ++k) {
    float re = 0.0f;
    float im = 0.0f;
    for (int t = 0; t < n; ++t) {
      const float a = -2.0f * kPi * static_cast<float>(k) * static_cast<float>(t) /
                      static_cast<float>(n);
      const float c = std::cos(a);
      const float s = std::sin(a);
      re += frame[t] * c;
      im += frame[t] * s;
    }
    mag[static_cast<std::size_t>(k)] = std::sqrt(re * re + im * im) * scale;
  }
  return mag;
}

}  // namespace

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

  const int frameCount = 1 + static_cast<int>((samples.size() - static_cast<std::size_t>(fftSize)) /
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
    const std::vector<float> mag = DftMagnitude(frame.data(), fftSize, spec.binCount);
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

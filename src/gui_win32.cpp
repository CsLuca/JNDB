#ifdef _WIN32

#include "gui_win32.hpp"

#include "dsp.hpp"
#include "ndb_decoder.hpp"
#include "wav.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <gdiplus.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "Gdiplus.lib")
#pragma comment(lib, "Msimg32.lib")

namespace {

constexpr int kIdInputEdit = 1001;
constexpr int kIdInputBrowse = 1002;
constexpr int kIdOutputEdit = 1003;
constexpr int kIdOutputBrowse = 1004;
constexpr int kIdMetricsEdit = 1005;
constexpr int kIdMetricsBrowse = 1006;
constexpr int kIdRun = 1007;
constexpr int kIdProgress = 1008;
constexpr int kIdStatus = 1009;
constexpr int kIdSummary = 1010;
constexpr int kIdHistoryEdit = 1011;
constexpr int kIdHistoryBrowse = 1012;
constexpr int kIdHistoryRefresh = 1013;
constexpr int kIdChartPanel = 1014;
constexpr int kIdCompareEdit = 1015;
constexpr int kIdCompareBrowse = 1016;
constexpr int kIdExportPng = 1017;
constexpr int kIdResetView = 1018;
constexpr int kIdPresetCombo = 1019;
constexpr int kIdCalibCombo = 1020;
constexpr int kIdPriorEdit = 1021;
constexpr int kIdPriorBrowse = 1022;
constexpr int kIdRequirePrior = 1023;
constexpr int kIdWaterfallViewCombo = 1024;
constexpr int kIdYawSlider = 1025;
constexpr int kIdPitchSlider = 1026;
constexpr int kIdShadingCheck = 1027;
constexpr int kIdColormapCombo = 1028;
constexpr int kIdWaterfallDxPreset = 1029;
constexpr int kIdWaterfallFpsCombo = 1030;

constexpr UINT kMsgProgress = WM_APP + 1;
constexpr UINT kMsgDone = WM_APP + 2;
constexpr UINT_PTR kWaterfallTimerId = 0x4E44;

struct DecodeThreadResult {
  bool ok = false;
  std::string error;
  std::string outputPath;
  std::string metricsPath;
  ndb::DecodeStats stats;
  std::size_t rowCount = 0;
  std::vector<ndb::DecodeResult> decodedRows;
};

struct HistoryEntry {
  std::string runId;
  std::string ts;
  std::string gitCommit;
  std::string gitBranch;
  double precision = 0.0;
  double recall = 0.0;
  double fph = 0.0;
  double latency = 0.0;
  double xrt = 0.0;
  double quality = 0.0;
};

struct ChartDef {
  std::wstring title;
  COLORREF color = RGB(30, 30, 30);
  double HistoryEntry::*field = nullptr;
  bool lowerIsBetter = false;
};

struct AppState {
  HWND hwnd = nullptr;
  HWND inputEdit = nullptr;
  HWND outputEdit = nullptr;
  HWND metricsEdit = nullptr;
  HWND presetCombo = nullptr;
  HWND calibCombo = nullptr;
  HWND priorEdit = nullptr;
  HWND priorCheck = nullptr;
  HWND waterfallViewCombo = nullptr;
  HWND yawSlider = nullptr;
  HWND pitchSlider = nullptr;
  HWND shadingCheck = nullptr;
  HWND colormapCombo = nullptr;
  HWND waterfallFpsCombo = nullptr;
  HWND runButton = nullptr;
  HWND progressBar = nullptr;
  HWND statusText = nullptr;
  HWND summaryText = nullptr;
  HWND presetHintText = nullptr;
  HWND historyEdit = nullptr;
  HWND compareEdit = nullptr;
  HWND chartPanel = nullptr;

  HFONT font = nullptr;
  HFONT fontBig = nullptr;
  HFONT fontMono = nullptr;

  std::atomic<bool> running{false};
  std::thread worker;
  std::vector<HistoryEntry> history;
  std::vector<HistoryEntry> historyCompare;
  ndb::WavData previewWav;
  std::vector<std::uint8_t> waterfallRgb;
  std::vector<float> waterfallDbRender;
  int waterfallW = 0;
  int waterfallH = 0;
  std::vector<float> panInstantDb;
  std::vector<float> panAvgDb;
  std::vector<float> panPeakDb;
  float panMinDb = -120.0f;
  float panMaxDb = -20.0f;
  std::vector<ndb::DecodeResult> overlayRows;
  int decodeProgressPct = 0;
  double decodeProgressVisualPct = 0.0;
  ULONGLONG decodeStartTickMs = 0;
  int waterfallViewMode = 1;  // 0: 2D only, 1: 2D+3D, 2: 3D large
  int yawDeg = 36;
  int pitchDeg = 24;
  bool shadingEnabled = true;
  int colormap3d = 0;
  int waterfallFps = 30;
  double waterfallZoom = 1.0;
  int waterfallPanPx = 0;
  bool waterfallDragging = false;
  int waterfallDragStartX = 0;
  int waterfallPanStartPx = 0;
  double chartZoom = 1.0;
  int chartPanPx = 0;
  bool dragging = false;
  int dragStartX = 0;
  int panStartPx = 0;
  bool hoverActive = false;
  POINT hoverPoint = {0, 0};
  std::wstring hoverText;
  bool waterfallHoverActive = false;
  POINT waterfallHoverPoint = {0, 0};
  std::wstring waterfallHoverText;
  bool mouseLeaveArmed = false;
};

void ApplyPresetToConfig(const std::string& mode, ndb::DecoderConfig* cfg) {
  if (!cfg) {
    return;
  }
  if (mode == "strict-dx") {
    cfg->requirePlausibleId = true;
    cfg->plausibleIdMinScore = 0.30f;
    cfg->strictBeaconMode = true;
    cfg->strictMinRepeats = 3;
    cfg->dedupFreqTolHz = 1.5f;
    cfg->clusterFreqTolHz = 2.0f;
    cfg->clusterGapSec = 0.35f;
    cfg->thresholdK = 2.6f;
    return;
  }
  if (mode == "relaxed") {
    cfg->requirePlausibleId = false;
    cfg->strictBeaconMode = false;
    cfg->dedupFreqTolHz = 2.5f;
    cfg->clusterFreqTolHz = 2.5f;
    cfg->clusterGapSec = 0.6f;
    cfg->thresholdK = 2.2f;
    return;
  }
  if (mode == "phase3-balanced") {
    cfg->enableBandLimit = true;
    cfg->bandLowHz = 110.0f;
    cfg->bandHighHz = 2000.0f;
    cfg->enableAutoNotch = false;
    cfg->enableImpulseBlanker = false;
    cfg->enableCfar2d = false;
    cfg->useAmtcFull = false;
    return;
  }
  if (mode == "phase3-selective") {
    cfg->enableBandLimit = true;
    cfg->bandLowHz = 110.0f;
    cfg->bandHighHz = 2000.0f;
    cfg->enableAutoNotch = false;
    cfg->enableImpulseBlanker = false;
    cfg->enableCfar2d = true;
    cfg->cfarTrainTime = 3;
    cfg->cfarGuardTime = 1;
    cfg->cfarTrainFreq = 4;
    cfg->cfarGuardFreq = 1;
    cfg->cfarScale = 1.35f;
    cfg->useAmtcFull = false;
    return;
  }
  if (mode == "phase4-serious") {
    cfg->useAmtcFull = true;
    cfg->maxTrackGapFrames = 4;
    cfg->enableCochannelSeparation = true;
    cfg->cochannelMaxTracks = 2;
    cfg->cochannelMaxGapFrames = 4;
    cfg->cochannelMaxStepHz = 5.0f;
    cfg->confidenceCalibration = "platt";
    return;
  }
  if (mode == "quiet") {
    cfg->enableBandLimit = true;
    cfg->bandLowHz = 130.0f;
    cfg->bandHighHz = 1200.0f;
    cfg->enableAutoNotch = false;
    cfg->enableImpulseBlanker = false;
    cfg->enableCfar2d = false;
    cfg->thresholdK = 2.4f;
    cfg->useAmtcFull = false;
    return;
  }
  if (mode == "urban-noise") {
    cfg->enableBandLimit = true;
    cfg->bandLowHz = 100.0f;
    cfg->bandHighHz = 2200.0f;
    cfg->enableAutoNotch = true;
    cfg->autoNotchMaxCount = 4;
    cfg->autoNotchSnrDb = 7.0f;
    cfg->enableImpulseBlanker = true;
    cfg->impulseBlankerSigma = 7.5f;
    cfg->impulseBlankerHalfWindow = 2;
    cfg->enableCfar2d = true;
    cfg->cfarTrainTime = 3;
    cfg->cfarGuardTime = 1;
    cfg->cfarTrainFreq = 4;
    cfg->cfarGuardFreq = 1;
    cfg->cfarScale = 1.45f;
    cfg->useAmtcFull = false;
    return;
  }
  if (mode == "weak-signal-dx") {
    cfg->useAmtcFull = true;
    cfg->maxTrackGapFrames = 5;
    cfg->strictBeaconMode = true;
    cfg->strictMinRepeats = 3;
    cfg->requirePlausibleId = true;
    cfg->plausibleIdMinScore = 0.35f;
    cfg->confidenceCalibration = "platt";
    cfg->thresholdK = 2.6f;
    return;
  }
  if (mode == "step3-fusion") {
    cfg->useAmtcFull = true;
    cfg->maxTrackGapFrames = 5;
    cfg->requirePlausibleId = true;
    cfg->plausibleIdMinScore = 0.30f;
    cfg->confidenceCalibration = "platt";
    cfg->thresholdK = 2.6f;
    cfg->enableGlrt = true;
    cfg->glrtPfa = 0.10f;
    cfg->glrtMinSnrDb = -3.0f;
    cfg->scoreFusionWGlrt = 0.35f;
    cfg->scoreFusionWCyclo = 0.35f;
    cfg->scoreFusionWDecoder = 0.30f;
    cfg->scoreFusionBias = 0.0f;
    return;
  }
}

std::wstring PresetHintFromSelection(int sel) {
  switch (sel) {
    case 1:
      return L"Strict DX: conservative filter, repeated IDs required.";
    case 2:
      return L"Relaxed: wider detection, plausible-ID gate disabled.";
    case 3:
      return L"Phase3 Balanced: light band-limit only, robust default for noisy audio.";
    case 4:
      return L"Phase3 Selective: band-limit + soft CFAR, more selective in interference.";
    case 5:
      return L"Phase4 Serious: AMTC-full + co-channel split + Platt confidence.";
    case 6:
      return L"Quiet: conservative low-noise profile for clean channels.";
    case 7:
      return L"Urban Noise: notch + blanker + CFAR for interference-heavy RF.";
    case 8:
      return L"Weak-signal DX: strict repeats + AMTC-full for marginal IDs.";
    case 9:
      return L"Step3 Fusion: GLRT + cyclo-like + decoder confidence fusion.";
    default:
      return L"Default: baseline profile for general-purpose decoding.";
  }
}

void UpdatePresetHint(AppState* app) {
  if (!app || !app->presetCombo || !app->presetHintText) {
    return;
  }
  const int sel = static_cast<int>(SendMessageW(app->presetCombo, CB_GETCURSEL, 0, 0));
  SetWindowTextW(app->presetHintText, PresetHintFromSelection(sel).c_str());
}

void ApplyPresetUiDefaults(AppState* app) {
  if (!app || !app->presetCombo || !app->calibCombo) {
    return;
  }
  const int sel = static_cast<int>(SendMessageW(app->presetCombo, CB_GETCURSEL, 0, 0));
  if (sel == 5) {
    SendMessageW(app->calibCombo, CB_SETCURSEL, 1, 0);
  }
}

std::wstring ToWide(const std::string& s) {
  if (s.empty()) {
    return L"";
  }
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0) {
    return L"";
  }
  std::wstring w(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::string ToUtf8(const std::wstring& w) {
  if (w.empty()) {
    return "";
  }
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0,
                                    nullptr, nullptr);
  if (n <= 0) {
    return "";
  }
  std::string s(static_cast<std::size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr,
                      nullptr);
  return s;
}

std::wstring GetText(HWND h) {
  const int n = GetWindowTextLengthW(h);
  std::wstring out(static_cast<std::size_t>(n), L'\0');
  GetWindowTextW(h, out.data(), n + 1);
  return out;
}

void SetText(HWND h, const std::wstring& s) {
  SetWindowTextW(h, s.c_str());
}

std::wstring ChooseOpenFile(HWND owner, const wchar_t* title, const wchar_t* filter) {
  wchar_t fileName[MAX_PATH] = {};
  OPENFILENAMEW ofn = {};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = owner;
  ofn.lpstrTitle = title;
  ofn.lpstrFilter = filter;
  ofn.lpstrFile = fileName;
  ofn.nMaxFile = MAX_PATH;
  ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
  if (GetOpenFileNameW(&ofn)) {
    return fileName;
  }
  return L"";
}

std::wstring ChooseSaveFile(HWND owner, const wchar_t* title, const wchar_t* filter,
                            const wchar_t* defaultExt) {
  wchar_t fileName[MAX_PATH] = {};
  OPENFILENAMEW ofn = {};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = owner;
  ofn.lpstrTitle = title;
  ofn.lpstrFilter = filter;
  ofn.lpstrDefExt = defaultExt;
  ofn.lpstrFile = fileName;
  ofn.nMaxFile = MAX_PATH;
  ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
  if (GetSaveFileNameW(&ofn)) {
    return fileName;
  }
  return L"";
}

int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
  UINT num = 0;
  UINT size = 0;
  Gdiplus::GetImageEncodersSize(&num, &size);
  if (size == 0) {
    return -1;
  }
  auto* pImageCodecInfo = reinterpret_cast<Gdiplus::ImageCodecInfo*>(malloc(size));
  if (pImageCodecInfo == nullptr) {
    return -1;
  }
  Gdiplus::GetImageEncoders(num, size, pImageCodecInfo);
  for (UINT j = 0; j < num; ++j) {
    if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0) {
      *pClsid = pImageCodecInfo[j].Clsid;
      free(pImageCodecInfo);
      return static_cast<int>(j);
    }
  }
  free(pImageCodecInfo);
  return -1;
}

bool WriteCsv(const std::string& path, const std::vector<ndb::DecodeResult>& results,
              std::string* error) {
  std::ofstream out(path);
  if (!out) {
    *error = "Cannot write output file: " + path;
    return false;
  }
  out << "track_id,freq_hz,text,confidence,start_sec,end_sec\n";
  for (const auto& r : results) {
    out << r.trackId << ',' << std::fixed << std::setprecision(2) << r.freqHz << ',' << '"'
        << r.text << '"' << ',' << std::setprecision(3) << r.confidence << ','
        << std::setprecision(3) << r.startSec << ',' << r.endSec << '\n';
  }
  return true;
}

bool WriteMetrics(const std::string& path, const ndb::DecodeStats& s, std::string* error) {
  std::ofstream out(path);
  if (!out) {
    *error = "Cannot write metrics file: " + path;
    return false;
  }
  out << "{\n";
  out << "  \"quality_score\": " << std::fixed << std::setprecision(3) << s.qualityScore << ",\n";
  out << "  \"mean_confidence\": " << std::fixed << std::setprecision(6) << s.meanConfidence
      << ",\n";
  out << "  \"median_confidence\": " << std::fixed << std::setprecision(6) << s.medianConfidence
      << ",\n";
  out << "  \"decode_ratio\": " << std::fixed << std::setprecision(6) << s.decodeRatio << ",\n";
  out << "  \"id_like_token_ratio\": " << std::fixed << std::setprecision(6)
      << s.idLikeTokenRatio << ",\n";
  out << "  \"track_count\": " << s.trackCount << ",\n";
  out << "  \"decoded_count\": " << s.decodedCount << "\n";
  out << "}\n";
  return true;
}

void SetStatus(AppState* app, const std::wstring& s) {
  SetText(app->statusText, s);
}

void SetSummary(AppState* app, const std::wstring& s) {
  SetText(app->summaryText, s);
}

void SetBusy(AppState* app, bool busy) {
  EnableWindow(app->runButton, busy ? FALSE : TRUE);
  EnableWindow(app->inputEdit, busy ? FALSE : TRUE);
  EnableWindow(app->outputEdit, busy ? FALSE : TRUE);
  EnableWindow(app->metricsEdit, busy ? FALSE : TRUE);
  if (app->calibCombo) {
    EnableWindow(app->calibCombo, busy ? FALSE : TRUE);
  }
  if (app->priorEdit) {
    EnableWindow(app->priorEdit, busy ? FALSE : TRUE);
  }
  if (app->priorCheck) {
    EnableWindow(app->priorCheck, busy ? FALSE : TRUE);
  }
}

std::wstring BuildSummary(const DecodeThreadResult& r) {
  std::wstringstream ss;
  ss << L"Rows: " << r.rowCount << L"\r\n"
     << L"Quality score: " << std::fixed << std::setprecision(2) << r.stats.qualityScore
     << L"/100\r\n"
     << L"Mean confidence: " << std::setprecision(3) << r.stats.meanConfidence << L"\r\n"
     << L"Decode ratio: " << std::setprecision(3) << r.stats.decodeRatio << L"\r\n"
     << L"ID-like ratio: " << std::setprecision(3) << r.stats.idLikeTokenRatio;
  return ss.str();
}

std::vector<std::string> SplitCsvLine(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  bool quoted = false;
  for (char c : line) {
    if (c == '"') {
      quoted = !quoted;
      continue;
    }
    if (c == ',' && !quoted) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  out.push_back(cur);
  return out;
}

bool ToDouble(const std::string& s, double* out) {
  try {
    *out = std::stod(s);
    return true;
  } catch (...) {
    return false;
  }
}

bool LoadHistoryCsv(const std::string& path, std::vector<HistoryEntry>* out, std::string* error) {
  out->clear();
  std::ifstream in(path);
  if (!in) {
    *error = "Cannot open history CSV: " + path;
    return false;
  }

  std::string headerLine;
  if (!std::getline(in, headerLine)) {
    *error = "Empty history CSV";
    return false;
  }
  const auto header = SplitCsvLine(headerLine);
  auto idx = [&](const std::string& key) -> int {
    for (std::size_t i = 0; i < header.size(); ++i) {
      if (header[i] == key) {
        return static_cast<int>(i);
      }
    }
    return -1;
  };

  const int iRun = idx("run_id");
  const int iTs = idx("timestamp_utc");
  const int iCommit = idx("git_commit");
  const int iBranch = idx("git_branch");
  const int iPrec = idx("precision");
  const int iRec = idx("recall");
  const int iFph = idx("false_positives_per_hour");
  const int iLat = idx("id_latency_sec");
  const int iXrt = idx("runtime_x_realtime");
  const int iQual = idx("quality_score_mean");
  if (iRun < 0 || iTs < 0 || iPrec < 0 || iRec < 0 || iFph < 0 || iLat < 0 || iXrt < 0 || iQual < 0) {
    *error = "History CSV missing required columns";
    return false;
  }

  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    auto c = SplitCsvLine(line);
    const int need = std::max({iRun, iTs, iPrec, iRec, iFph, iLat, iXrt, iQual});
    if (static_cast<int>(c.size()) <= need) {
      continue;
    }
    HistoryEntry e;
    e.runId = c[static_cast<std::size_t>(iRun)];
    e.ts = c[static_cast<std::size_t>(iTs)];
    if (iCommit >= 0 && static_cast<int>(c.size()) > iCommit) {
      e.gitCommit = c[static_cast<std::size_t>(iCommit)];
    }
    if (iBranch >= 0 && static_cast<int>(c.size()) > iBranch) {
      e.gitBranch = c[static_cast<std::size_t>(iBranch)];
    }
    if (!ToDouble(c[static_cast<std::size_t>(iPrec)], &e.precision)) continue;
    if (!ToDouble(c[static_cast<std::size_t>(iRec)], &e.recall)) continue;
    if (!ToDouble(c[static_cast<std::size_t>(iFph)], &e.fph)) continue;
    if (!ToDouble(c[static_cast<std::size_t>(iLat)], &e.latency)) continue;
    if (!ToDouble(c[static_cast<std::size_t>(iXrt)], &e.xrt)) continue;
    if (!ToDouble(c[static_cast<std::size_t>(iQual)], &e.quality)) continue;
    out->push_back(e);
  }

  if (out->empty()) {
    *error = "No valid rows in history CSV";
    return false;
  }
  return true;
}

void EnsureWaterfallPreview(AppState* app) {
  if (!app || app->previewWav.samples.empty() || app->waterfallW > 0 || app->waterfallH > 0) {
    return;
  }

  const int fft = 512;
  const int hop = 128;
  const auto spec = ndb::ComputeSpectrogram(app->previewWav.samples, app->previewWav.sampleRate, fft, hop);
  if (spec.frameCount <= 0 || spec.binCount <= 1) {
    return;
  }

  app->waterfallW = std::max(1, spec.frameCount);
  app->waterfallH = std::max(1, spec.binCount - 1);
  app->waterfallRgb.assign(static_cast<std::size_t>(app->waterfallW * app->waterfallH * 3), 0);
  app->waterfallDbRender.assign(static_cast<std::size_t>(app->waterfallW * app->waterfallH), -120.0f);
  app->panInstantDb.assign(static_cast<std::size_t>(app->waterfallH), -120.0f);
  app->panAvgDb.assign(static_cast<std::size_t>(app->waterfallH), -120.0f);
  app->panPeakDb.assign(static_cast<std::size_t>(app->waterfallH), -120.0f);

  std::vector<float> lv(static_cast<std::size_t>(spec.frameCount * spec.binCount), 0.0f);
  std::vector<float> dbVals;
  dbVals.reserve(static_cast<std::size_t>(spec.frameCount * std::max(1, spec.binCount - 1)));
  for (int t = 0; t < spec.frameCount; ++t) {
    for (int b = 1; b < spec.binCount; ++b) {
      const float v = 20.0f * std::log10(1e-12f + spec.At(t, b));
      lv[static_cast<std::size_t>(t * spec.binCount + b)] = v;
      dbVals.push_back(v);
    }
  }

  const int tailN = std::min(spec.frameCount, 12);
  for (int bi = 1; bi < spec.binCount; ++bi) {
    float inst = lv[static_cast<std::size_t>((spec.frameCount - 1) * spec.binCount + bi)];
    double acc = 0.0;
    int n = 0;
    for (int k = 0; k < tailN; ++k) {
      const int t = spec.frameCount - 1 - k;
      if (t < 0) {
        break;
      }
      acc += lv[static_cast<std::size_t>(t * spec.binCount + bi)];
      ++n;
    }
    const float avg = (n > 0) ? static_cast<float>(acc / static_cast<double>(n)) : inst;
    app->panInstantDb[static_cast<std::size_t>(bi - 1)] = inst;
    app->panAvgDb[static_cast<std::size_t>(bi - 1)] = avg;
    app->panPeakDb[static_cast<std::size_t>(bi - 1)] = std::max(inst, avg);
  }

  auto percentile = [](std::vector<float> vals, float q) {
    if (vals.empty()) {
      return -120.0f;
    }
    const float qq = std::clamp(q, 0.0f, 1.0f);
    const std::size_t idx = static_cast<std::size_t>(qq * static_cast<float>(vals.size() - 1));
    std::nth_element(vals.begin(), vals.begin() + static_cast<std::ptrdiff_t>(idx), vals.end());
    return vals[idx];
  };

  float floorDb = percentile(dbVals, 0.20f);
  float ceilDb = percentile(dbVals, 0.995f);
  if (ceilDb < floorDb + 8.0f) {
    ceilDb = floorDb + 8.0f;
  }
  app->panMinDb = floorDb;
  app->panMaxDb = ceilDb;

  std::vector<float> norm(static_cast<std::size_t>(spec.frameCount * spec.binCount), 0.0f);
  for (int t = 0; t < spec.frameCount; ++t) {
    for (int b = 1; b < spec.binCount; ++b) {
      float n = (lv[static_cast<std::size_t>(t * spec.binCount + b)] - floorDb) /
                std::max(1.0f, (ceilDb - floorDb));
      n = std::clamp((n - 0.01f) * 1.18f, 0.0f, 1.0f);
      n = std::pow(n, 0.78f);
      norm[static_cast<std::size_t>(t * spec.binCount + b)] = n;
    }
  }

  // HDSDR-like persistence: fast attack, slow decay for thin CW/NDB traces.
  for (int b = 1; b < spec.binCount; ++b) {
    float prev = 0.0f;
    for (int t = 0; t < spec.frameCount; ++t) {
      const std::size_t idx = static_cast<std::size_t>(t * spec.binCount + b);
      const float cur = norm[idx];
      prev = std::max(cur, prev * 0.965f);
      norm[idx] = prev;
    }
  }

  auto rampHdsdr = [](float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    struct Stop {
      float p;
      int r;
      int g;
      int b;
    };
    constexpr Stop k[] = {
        {0.00f, 0, 0, 0},       {0.12f, 0, 10, 50},   {0.24f, 0, 45, 140},
        {0.36f, 0, 120, 190},   {0.50f, 0, 175, 90},  {0.66f, 220, 220, 0},
        {0.82f, 240, 110, 0},   {0.93f, 255, 40, 20}, {1.00f, 255, 245, 230},
    };
    for (int i = 1; i < static_cast<int>(std::size(k)); ++i) {
      if (x <= k[i].p) {
        const float t = (x - k[i - 1].p) / std::max(1e-6f, (k[i].p - k[i - 1].p));
        const int r = static_cast<int>(std::round(k[i - 1].r + t * (k[i].r - k[i - 1].r)));
        const int g = static_cast<int>(std::round(k[i - 1].g + t * (k[i].g - k[i - 1].g)));
        const int b = static_cast<int>(std::round(k[i - 1].b + t * (k[i].b - k[i - 1].b)));
        return RGB(r, g, b);
      }
    }
    return RGB(255, 245, 230);
  };

  for (int t = 0; t < app->waterfallW; ++t) {
    for (int y = 0; y < app->waterfallH; ++y) {
      const int b = app->waterfallH - y;
      const float v = lv[static_cast<std::size_t>(t * spec.binCount + b)];
      float n = norm[static_cast<std::size_t>(t * spec.binCount + b)];
      const COLORREF c = rampHdsdr(n);
      const std::size_t idx = static_cast<std::size_t>((y * app->waterfallW + t) * 3);
      app->waterfallRgb[idx + 0] = GetBValue(c);
      app->waterfallRgb[idx + 1] = GetGValue(c);
      app->waterfallRgb[idx + 2] = GetRValue(c);
      app->waterfallDbRender[static_cast<std::size_t>(y * app->waterfallW + t)] = v;
    }
  }
}

RECT GetWaterfallPlotRect(const RECT& clientRc) {
  const int outerGap = 10;
  const int wfH = 360;
  RECT wfRc = {clientRc.left + outerGap, clientRc.top + outerGap, clientRc.right - outerGap,
               clientRc.top + wfH};
  RECT plot = {wfRc.left + 14, wfRc.top + 34, wfRc.right - 14, wfRc.bottom - 34};
  return plot;
}

UINT WaterfallTimerMs(const AppState* app) {
  if (!app) {
    return 33;
  }
  const int fps = std::clamp(app->waterfallFps, 10, 120);
  return static_cast<UINT>(std::max(8, 1000 / fps));
}

void ComputeWaterfallSourceWindow(const AppState* app, int* srcX, int* srcW) {
  if (!app || app->waterfallW <= 0) {
    *srcX = 0;
    *srcW = 0;
    return;
  }
  if (app->running) {
    const int pvis = std::clamp(static_cast<int>(std::round(app->decodeProgressVisualPct)), 0, 100);
    const int curCol = std::clamp((pvis * std::max(1, app->waterfallW - 1)) / 100,
                                  0, std::max(0, app->waterfallW - 1));
    const int win = std::max(120, app->waterfallW / 2);
    *srcW = std::min(app->waterfallW, win);
    *srcX = std::max(0, curCol - *srcW + 1);
    return;
  }

  const double zoom = std::clamp(app->waterfallZoom, 1.0, 8.0);
  *srcW = std::max(60, std::min(app->waterfallW,
                                static_cast<int>(std::round(static_cast<double>(app->waterfallW) / zoom))));
  const int maxPan = std::max(0, app->waterfallW - *srcW);
  const int pan = std::clamp(app->waterfallPanPx, 0, maxPan);
  *srcX = pan;
}

void DrawPanadapter(HDC hdc, const RECT& rc, const AppState* app) {
  HBRUSH bg = CreateSolidBrush(RGB(9, 16, 26));
  FillRect(hdc, &rc, bg);
  DeleteObject(bg);

  HPEN border = CreatePen(PS_SOLID, 1, RGB(45, 70, 96));
  auto oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, border));
  MoveToEx(hdc, rc.left, rc.top, nullptr);
  LineTo(hdc, rc.right - 1, rc.top);
  LineTo(hdc, rc.right - 1, rc.bottom - 1);
  LineTo(hdc, rc.left, rc.bottom - 1);
  LineTo(hdc, rc.left, rc.top);
  SelectObject(hdc, oldPen);
  DeleteObject(border);

  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, RGB(170, 198, 222));
  RECT tr = {rc.left + 8, rc.top + 2, rc.right - 8, rc.top + 18};
  DrawTextW(hdc, L"Panadapter  Instant/Avg/Peak", -1, &tr, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

  if (!app || app->panInstantDb.empty()) {
    RECT m = {rc.left + 8, rc.top + 18, rc.right - 8, rc.bottom - 6};
    SetTextColor(hdc, RGB(120, 145, 168));
    DrawTextW(hdc, L"Load WAV to show panadapter", -1, &m, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    return;
  }

  RECT plot = {rc.left + 36, rc.top + 18, rc.right - 8, rc.bottom - 14};
  const float minDb = app->panMinDb;
  const float maxDb = app->panMaxDb;
  const int gridN = 4;
  HPEN grid = CreatePen(PS_DOT, 1, RGB(36, 52, 72));
  oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, grid));
  for (int i = 0; i <= gridN; ++i) {
    const int y = plot.top + ((plot.bottom - plot.top) * i) / gridN;
    MoveToEx(hdc, plot.left, y, nullptr);
    LineTo(hdc, plot.right, y);
    const float db = maxDb - (maxDb - minDb) * (static_cast<float>(i) / static_cast<float>(gridN));
    std::wstringstream ss;
    ss << std::fixed << std::setprecision(0) << db;
    RECT lr = {rc.left + 2, y - 8, plot.left - 4, y + 8};
    SetTextColor(hdc, RGB(116, 145, 172));
    DrawTextW(hdc, ss.str().c_str(), -1, &lr, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
  }
  SelectObject(hdc, oldPen);
  DeleteObject(grid);

  auto drawSeries = [&](const std::vector<float>& src, COLORREF c, int width) {
    if (src.size() < 2) {
      return;
    }
    HPEN pen = CreatePen(PS_SOLID, width, c);
    auto old = reinterpret_cast<HPEN>(SelectObject(hdc, pen));
    for (std::size_t i = 0; i < src.size(); ++i) {
      const float n = static_cast<float>(i) / static_cast<float>(src.size() - 1);
      const int x = plot.left + static_cast<int>(n * (plot.right - plot.left));
      const float yn = std::clamp((src[i] - minDb) / std::max(1.0f, (maxDb - minDb)), 0.0f, 1.0f);
      const int y = plot.bottom - static_cast<int>(yn * (plot.bottom - plot.top));
      if (i == 0) {
        MoveToEx(hdc, x, y, nullptr);
      } else {
        LineTo(hdc, x, y);
      }
    }
    SelectObject(hdc, old);
    DeleteObject(pen);
  };

  drawSeries(app->panPeakDb, RGB(255, 104, 94), 1);
  drawSeries(app->panAvgDb, RGB(95, 190, 240), 1);
  drawSeries(app->panInstantDb, RGB(255, 226, 92), 2);
}

void DrawWaterfallCard(AppState* app, HDC hdc, const RECT& rc) {
  const int viewMode = app ? app->waterfallViewMode : 1;
  TRIVERTEX tv[2] = {
      {rc.left, rc.top, 0x0A00, 0x1400, 0x2200, 0x0000},
      {rc.right, rc.bottom, 0x1100, 0x2200, 0x3800, 0x0000},
  };
  GRADIENT_RECT gr = {0, 1};
  GradientFill(hdc, tv, 2, &gr, 1, GRADIENT_FILL_RECT_V);

  HBRUSH panel = CreateSolidBrush(RGB(12, 24, 38));
  FillRect(hdc, &rc, panel);
  DeleteObject(panel);

  HPEN border = CreatePen(PS_SOLID, 1, RGB(35, 55, 75));
  auto oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, border));
  MoveToEx(hdc, rc.left, rc.top, nullptr);
  LineTo(hdc, rc.right - 1, rc.top);
  LineTo(hdc, rc.right - 1, rc.bottom - 1);
  LineTo(hdc, rc.left, rc.bottom - 1);
  LineTo(hdc, rc.left, rc.top);
  SelectObject(hdc, oldPen);
  DeleteObject(border);

  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, RGB(208, 224, 240));
  RECT titleRc = {rc.left + 10, rc.top + 6, rc.right - 10, rc.top + 28};
  DrawTextW(hdc, L"NDB Waterfall Preview", -1, &titleRc, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

  const int panH = 112;
  RECT panRc = {rc.left + 14, rc.top + 30, rc.right - 14, rc.top + 30 + panH};
  DrawPanadapter(hdc, panRc, app);
  RECT plot = {rc.left + 14, panRc.bottom + 8, rc.right - 14, rc.bottom - 34};
  if (!app || app->previewWav.samples.empty()) {
    SetTextColor(hdc, RGB(130, 150, 170));
    DrawTextW(hdc, L"Open an input WAV to render waterfall.", -1, &plot,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    return;
  }

  EnsureWaterfallPreview(app);
  if (app->waterfallRgb.empty() || app->waterfallW <= 0 || app->waterfallH <= 0) {
    SetTextColor(hdc, RGB(130, 150, 170));
    DrawTextW(hdc, L"Waterfall unavailable for this file.", -1, &plot,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    return;
  }

  BITMAPINFO bmi = {};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = app->waterfallW;
  bmi.bmiHeader.biHeight = -app->waterfallH;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 24;
  bmi.bmiHeader.biCompression = BI_RGB;
  int srcX = 0;
  int srcW = app->waterfallW;
  ComputeWaterfallSourceWindow(app, &srcX, &srcW);

  StretchDIBits(hdc, plot.left, plot.top, plot.right - plot.left, plot.bottom - plot.top,
                srcX, 0, srcW, app->waterfallH, app->waterfallRgb.data(), &bmi, DIB_RGB_COLORS,
                SRCCOPY);

  if (app && app->running) {
    const int p = std::clamp(static_cast<int>(std::round(app->decodeProgressVisualPct)), 0, 100);
    const int xSweep = plot.left + ((plot.right - plot.left) * p) / 100;
    HPEN sweep = CreatePen(PS_SOLID, 2, RGB(255, 245, 160));
    auto oldSweep = reinterpret_cast<HPEN>(SelectObject(hdc, sweep));
    MoveToEx(hdc, xSweep, plot.top, nullptr);
    LineTo(hdc, xSweep, plot.bottom);
    SelectObject(hdc, oldSweep);
    DeleteObject(sweep);

    RECT pbox = {plot.right - 130, plot.top + 8, plot.right - 10, plot.top + 30};
    HBRUSH bb = CreateSolidBrush(RGB(18, 30, 46));
    FillRect(hdc, &pbox, bb);
    DeleteObject(bb);
    HPEN bp = CreatePen(PS_SOLID, 1, RGB(68, 102, 138));
    auto oldBp = reinterpret_cast<HPEN>(SelectObject(hdc, bp));
    MoveToEx(hdc, pbox.left, pbox.top, nullptr);
    LineTo(hdc, pbox.right - 1, pbox.top);
    LineTo(hdc, pbox.right - 1, pbox.bottom - 1);
    LineTo(hdc, pbox.left, pbox.bottom - 1);
    LineTo(hdc, pbox.left, pbox.top);
    SelectObject(hdc, oldBp);
    DeleteObject(bp);
    std::wstringstream ps;
    ps << L"Scan " << p << L"%";
    SetTextColor(hdc, RGB(220, 235, 250));
    DrawTextW(hdc, ps.str().c_str(), -1, &pbox, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }

  HBRUSH glaze = CreateSolidBrush(RGB(255, 255, 255));
  BLENDFUNCTION bf = {AC_SRC_OVER, 0, 20, 0};
  RECT topBand = {plot.left, plot.top, plot.right,
                  plot.top + std::max<int>(8, static_cast<int>((plot.bottom - plot.top) / 10))};
  HDC memdc = CreateCompatibleDC(hdc);
  HBITMAP membmp = CreateCompatibleBitmap(hdc, topBand.right - topBand.left, topBand.bottom - topBand.top);
  auto oldBmp = reinterpret_cast<HBITMAP>(SelectObject(memdc, membmp));
  RECT memRect = {0, 0, topBand.right - topBand.left, topBand.bottom - topBand.top};
  FillRect(memdc, &memRect, glaze);
  AlphaBlend(hdc, topBand.left, topBand.top, topBand.right - topBand.left, topBand.bottom - topBand.top,
             memdc, 0, 0, topBand.right - topBand.left, topBand.bottom - topBand.top, bf);
  SelectObject(memdc, oldBmp);
  DeleteObject(membmp);
  DeleteDC(memdc);
  DeleteObject(glaze);

  HPEN axis = CreatePen(PS_SOLID, 1, RGB(88, 118, 148));
  oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, axis));
  MoveToEx(hdc, plot.left, plot.bottom, nullptr);
  LineTo(hdc, plot.right, plot.bottom);
  MoveToEx(hdc, plot.left, plot.top, nullptr);
  LineTo(hdc, plot.left, plot.bottom);
  SelectObject(hdc, oldPen);
  DeleteObject(axis);

  SetTextColor(hdc, RGB(166, 194, 220));
  RECT xLab = {plot.left, plot.bottom + 2, plot.right, plot.bottom + 20};
  DrawTextW(hdc, L"Time ->", -1, &xLab, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
  RECT yLab = {plot.left, plot.top - 2, plot.left + 140, plot.top + 16};
  DrawTextW(hdc, L"Frequency (kHz)", -1, &yLab, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

  const float nyqTicks = app && app->previewWav.sampleRate > 0
                             ? 0.5f * static_cast<float>(app->previewWav.sampleRate)
                             : 4000.0f;
  const float fMinTicks = 80.0f;
  const float fMaxTicks = std::min(2200.0f, nyqTicks - 20.0f);
  const int tickCount = 5;
  HPEN tickPen = CreatePen(PS_SOLID, 1, RGB(110, 140, 168));
  auto oldTickPen = reinterpret_cast<HPEN>(SelectObject(hdc, tickPen));
  SetTextColor(hdc, RGB(176, 202, 226));
  for (int i = 0; i < tickCount; ++i) {
    const float u = static_cast<float>(i) / static_cast<float>(tickCount - 1);
    const int y = plot.bottom - static_cast<int>(u * (plot.bottom - plot.top));
    MoveToEx(hdc, plot.left, y, nullptr);
    LineTo(hdc, plot.left + 6, y);
    const float fHz = fMinTicks + u * (fMaxTicks - fMinTicks);
    std::wstringstream ss;
    ss << std::fixed << std::setprecision(2) << (fHz / 1000.0f) << L" kHz";
    RECT tr = {plot.left + 10, y - 9, plot.left + 92, y + 9};
    DrawTextW(hdc, ss.str().c_str(), -1, &tr, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
  }
  SelectObject(hdc, oldTickPen);
  DeleteObject(tickPen);

  if (app && app->waterfallHoverActive && PtInRect(&plot, app->waterfallHoverPoint)) {
    HPEN cr = CreatePen(PS_SOLID, 1, RGB(255, 255, 180));
    auto oldCr = reinterpret_cast<HPEN>(SelectObject(hdc, cr));
    MoveToEx(hdc, plot.left, app->waterfallHoverPoint.y, nullptr);
    LineTo(hdc, plot.right, app->waterfallHoverPoint.y);
    MoveToEx(hdc, app->waterfallHoverPoint.x, plot.top, nullptr);
    LineTo(hdc, app->waterfallHoverPoint.x, plot.bottom);
    SelectObject(hdc, oldCr);
    DeleteObject(cr);
  }

  if (viewMode != 0 && app->waterfallW > 4 && app->waterfallH > 8) {
    RECT mini;
    if (viewMode == 2) {
      mini = {plot.left + 8, plot.top + 8, plot.right - 8, plot.top + (plot.bottom - plot.top) / 2};
    } else {
      mini = {plot.right - (plot.right - plot.left) / 3, plot.top + 10, plot.right - 12,
              plot.top + (plot.bottom - plot.top) / 2};
    }
    HBRUSH miniBg = CreateSolidBrush(RGB(10, 18, 30));
    FillRect(hdc, &mini, miniBg);
    DeleteObject(miniBg);
    HPEN miniBorder = CreatePen(PS_SOLID, 1, RGB(60, 90, 124));
    auto oldMiniPen = reinterpret_cast<HPEN>(SelectObject(hdc, miniBorder));
    MoveToEx(hdc, mini.left, mini.top, nullptr);
    LineTo(hdc, mini.right - 1, mini.top);
    LineTo(hdc, mini.right - 1, mini.bottom - 1);
    LineTo(hdc, mini.left, mini.bottom - 1);
    LineTo(hdc, mini.left, mini.top);
    SelectObject(hdc, oldMiniPen);
    DeleteObject(miniBorder);

    const int depth = (viewMode == 2) ? 34 : 22;
    const int freqSamples = (viewMode == 2) ? 78 : 52;
    const int stride = std::max(1, srcW / depth);
    const int ampPx = std::max<int>(12, static_cast<int>((mini.bottom - mini.top) / 3));
    const int pad = 8;
    const float sx = static_cast<float>(std::max(60L, (mini.right - mini.left - pad * 2))) * 0.62f;
    const float sy = static_cast<float>(std::max(40L, (mini.bottom - mini.top - pad * 2))) * 0.36f;
    const float ox = static_cast<float>(mini.left + pad + 24);
    const float oy = static_cast<float>(mini.bottom - pad - 6);

    const float yaw = static_cast<float>(app->yawDeg) * 3.1415926f / 180.0f;
    const float pitch = static_cast<float>(app->pitchDeg) * 3.1415926f / 180.0f;
    const float cy = std::cos(yaw);
    const float syaw = std::sin(yaw);
    const float cp = std::cos(pitch);
    const float sp = std::sin(pitch);

    auto project = [&](float u, float v, float z) {
      const float x = (u - 0.5f) * 2.0f;
      const float y = (v - 0.5f) * 2.0f;
      const float zz = z;
      const float xr = cy * x - syaw * y;
      const float yr = syaw * x + cy * y;
      const float zr = cp * zz - sp * yr;
      const float yr2 = sp * zz + cp * yr;
      const float px = ox + xr * sx;
      const float py = oy - (0.55f * yr2 * sy + zr * static_cast<float>(ampPx));
      return POINT{static_cast<LONG>(std::round(px)), static_cast<LONG>(std::round(py))};
    };

    std::vector<std::vector<POINT>> mesh(static_cast<std::size_t>(depth),
                                         std::vector<POINT>(static_cast<std::size_t>(freqSamples)));
    for (int d = 0; d < depth; ++d) {
      const int col = std::clamp(srcX + srcW - 1 - d * stride, 0, app->waterfallW - 1);
      const float v = static_cast<float>(d) / static_cast<float>(std::max(1, depth - 1));
      for (int i = 0; i < freqSamples; ++i) {
        const int yb = (i * std::max(1, app->waterfallH - 1)) / std::max(1, freqSamples - 1);
        const float u = static_cast<float>(i) / static_cast<float>(std::max(1, freqSamples - 1));
        const std::size_t idx = static_cast<std::size_t>((yb * app->waterfallW + col) * 3);
        const float lum = (0.11f * app->waterfallRgb[idx + 0] + 0.59f * app->waterfallRgb[idx + 1] +
                           0.30f * app->waterfallRgb[idx + 2]) /
                          255.0f;
        const float z = std::pow(std::clamp(lum, 0.0f, 1.0f), 0.75f);
        mesh[static_cast<std::size_t>(d)][static_cast<std::size_t>(i)] = project(u, v, z);
      }
    }

    auto mapColor = [&](int d) {
      const float t = static_cast<float>(d) / static_cast<float>(std::max(1, depth - 1));
      int r = 90;
      int g = 170;
      int b = 235;
      if (app->colormap3d == 1) {
        r = static_cast<int>(40 + 210 * t);
        g = static_cast<int>(180 - 70 * t);
        b = static_cast<int>(220 - 170 * t);
      } else if (app->colormap3d == 2) {
        r = static_cast<int>(70 + 170 * t);
        g = static_cast<int>(90 + 130 * t);
        b = static_cast<int>(255 - 210 * t);
      }
      if (app->shadingEnabled) {
        const float shade = 0.72f + 0.28f * t;
        r = static_cast<int>(std::clamp(r * shade, 0.0f, 255.0f));
        g = static_cast<int>(std::clamp(g * shade, 0.0f, 255.0f));
        b = static_cast<int>(std::clamp(b * shade, 0.0f, 255.0f));
      }
      return RGB(r, g, b);
    };

    for (int d = depth - 1; d >= 0; --d) {
      HPEN rowPen = CreatePen(PS_SOLID, 1, mapColor(d));
      auto oldR = reinterpret_cast<HPEN>(SelectObject(hdc, rowPen));
      Polyline(hdc, mesh[static_cast<std::size_t>(d)].data(), freqSamples);
      SelectObject(hdc, oldR);
      DeleteObject(rowPen);
    }

    const int colStep = std::max(2, depth / 7);
    for (int d = 0; d < depth; d += colStep) {
      HPEN colPen = CreatePen(PS_SOLID, 1, RGB(76, 122, 178));
      auto oldC = reinterpret_cast<HPEN>(SelectObject(hdc, colPen));
      for (int i = 1; i < freqSamples; ++i) {
        MoveToEx(hdc, mesh[static_cast<std::size_t>(d)][static_cast<std::size_t>(i - 1)].x,
                 mesh[static_cast<std::size_t>(d)][static_cast<std::size_t>(i - 1)].y, nullptr);
        LineTo(hdc, mesh[static_cast<std::size_t>(d)][static_cast<std::size_t>(i)].x,
               mesh[static_cast<std::size_t>(d)][static_cast<std::size_t>(i)].y);
      }
      SelectObject(hdc, oldC);
      DeleteObject(colPen);
    }

    RECT miniLbl = {mini.left + 6, mini.top + 2, mini.right - 6, mini.top + 18};
    SetTextColor(hdc, RGB(170, 205, 235));
    DrawTextW(hdc, viewMode == 2 ? L"3D Surface (Large)" : L"3D Surface", -1, &miniLbl,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER);
  }

  if (!app->overlayRows.empty() && app->previewWav.sampleRate > 0) {
    const float nyq = 0.5f * static_cast<float>(app->previewWav.sampleRate);
    const float fMin = 80.0f;
    const float fMax = std::min(2200.0f, nyq - 20.0f);
    HPEN trk = CreatePen(PS_SOLID, 2, RGB(255, 221, 87));
    auto oldT = reinterpret_cast<HPEN>(SelectObject(hdc, trk));
    SetTextColor(hdc, RGB(255, 235, 130));
    SetBkMode(hdc, TRANSPARENT);
    const float dur = static_cast<float>(app->previewWav.samples.size()) /
                      static_cast<float>(std::max(1, app->previewWav.sampleRate));
    const float viewStart = (static_cast<float>(srcX) / std::max(1, app->waterfallW - 1)) * dur;
    const float viewDur = (static_cast<float>(srcW) / std::max(1, app->waterfallW)) * dur;
    const float viewEnd = viewStart + viewDur;
    for (const auto& r : app->overlayRows) {
      if (r.endSec < viewStart || r.startSec > viewEnd) {
        continue;
      }
      const float f = std::clamp(r.freqHz, fMin, fMax);
      const float yN = 1.0f - (f - fMin) / std::max(1.0f, (fMax - fMin));
      const int y = plot.top + static_cast<int>(yN * (plot.bottom - plot.top));
      const float x0n = std::clamp((r.startSec - viewStart) / std::max(0.1f, viewDur), 0.0f, 1.0f);
      const float x1n = std::clamp((r.endSec - viewStart) / std::max(0.1f, viewDur), 0.0f, 1.0f);
      const int x0 = plot.left + static_cast<int>(x0n * (plot.right - plot.left));
      const int x1 = plot.left + static_cast<int>(x1n * (plot.right - plot.left));
      MoveToEx(hdc, x0, y, nullptr);
      LineTo(hdc, std::max(x0 + 1, x1), y);
      RECT lbl = {x0 + 4, y - 14, std::min<int>(plot.right - 4, x0 + 120), y + 2};
      const std::wstring tag = ToWide(!r.plausibleId.empty() ? r.plausibleId : r.text);
      DrawTextW(hdc, tag.c_str(), -1, &lbl, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    SelectObject(hdc, oldT);
    DeleteObject(trk);
  }
}

void DrawMetricChart(HDC hdc, const RECT& rc, const std::vector<HistoryEntry>& history,
                     const std::vector<HistoryEntry>& compare, const ChartDef& cd,
                     double zoom, int panPx) {
  HBRUSH panel = CreateSolidBrush(RGB(255, 255, 255));
  FillRect(hdc, &rc, panel);
  DeleteObject(panel);

  HPEN border = CreatePen(PS_SOLID, 1, RGB(220, 225, 232));
  auto oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, border));
  MoveToEx(hdc, rc.left, rc.top, nullptr);
  LineTo(hdc, rc.right - 1, rc.top);
  LineTo(hdc, rc.right - 1, rc.bottom - 1);
  LineTo(hdc, rc.left, rc.bottom - 1);
  LineTo(hdc, rc.left, rc.top);
  SelectObject(hdc, oldPen);
  DeleteObject(border);

  const int pad = 10;
  RECT titleRc = {rc.left + pad, rc.top + 4, rc.right - pad, rc.top + 24};
  SetTextColor(hdc, RGB(38, 50, 56));
  SetBkMode(hdc, TRANSPARENT);
  DrawTextW(hdc, cd.title.c_str(), -1, &titleRc, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

  if (history.size() < 2 && compare.size() < 2) {
    RECT msg = {rc.left + pad, rc.top + 28, rc.right - pad, rc.bottom - pad};
    SetTextColor(hdc, RGB(120, 128, 138));
    DrawTextW(hdc, L"Need >= 2 runs", -1, &msg, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    return;
  }

  double minV = std::numeric_limits<double>::max();
  double maxV = std::numeric_limits<double>::lowest();
  auto includeRange = [&](const std::vector<HistoryEntry>& src) {
    for (const auto& e : src) {
      const double v = e.*(cd.field);
      minV = std::min(minV, v);
      maxV = std::max(maxV, v);
    }
  };
  includeRange(history);
  includeRange(compare);
  if (minV == std::numeric_limits<double>::max()) {
    minV = 0.0;
    maxV = 1.0;
  }
  if (std::fabs(maxV - minV) < 1e-12) {
    maxV += 1.0;
    minV -= 1.0;
  }

  RECT plot = {rc.left + 8, rc.top + 28, rc.right - 8, rc.bottom - 24};
  HPEN grid = CreatePen(PS_DOT, 1, RGB(232, 236, 242));
  oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, grid));
  for (int i = 1; i <= 3; ++i) {
    const int y = plot.top + (plot.bottom - plot.top) * i / 4;
    MoveToEx(hdc, plot.left, y, nullptr);
    LineTo(hdc, plot.right, y);
  }
  SelectObject(hdc, oldPen);
  DeleteObject(grid);

  auto drawSeries = [&](const std::vector<HistoryEntry>& src, COLORREF color, int dotR, bool dashed) {
    if (src.size() < 2) {
      return;
    }
    HPEN pen = CreatePen(dashed ? PS_DASH : PS_SOLID, 2, color);
    auto oldP = reinterpret_cast<HPEN>(SelectObject(hdc, pen));
    for (std::size_t i = 0; i < src.size(); ++i) {
      const double v = src[i].*(cd.field);
      double t = static_cast<double>(i) / static_cast<double>(src.size() - 1);
      t = (t - 0.5) * zoom + 0.5;
      const int x = plot.left + static_cast<int>(t * (plot.right - plot.left)) + panPx;
      const double yn = (v - minV) / (maxV - minV);
      const int y = plot.bottom - static_cast<int>(yn * (plot.bottom - plot.top));
      if (i == 0) {
        MoveToEx(hdc, x, y, nullptr);
      } else {
        LineTo(hdc, x, y);
      }
      HBRUSH dot = CreateSolidBrush(color);
      RECT dr = {x - dotR, y - dotR, x + dotR + 1, y + dotR + 1};
      FillRect(hdc, &dr, dot);
      DeleteObject(dot);
    }
    SelectObject(hdc, oldP);
    DeleteObject(pen);
  };

  drawSeries(compare, RGB(130, 130, 130), 1, true);
  drawSeries(history, cd.color, 2, false);

  double lv = 0.0;
  double pv = 0.0;
  if (history.size() >= 2) {
    const auto& last = history.back();
    const auto& prev = history[history.size() - 2];
    lv = last.*(cd.field);
    pv = prev.*(cd.field);
  } else {
    const auto& last = compare.back();
    const auto& prev = compare[compare.size() - 2];
    lv = last.*(cd.field);
    pv = prev.*(cd.field);
  }
  const double delta = lv - pv;
  bool good = cd.lowerIsBetter ? (delta <= 0.0) : (delta >= 0.0);

  std::wstringstream ss;
  ss << std::fixed << std::setprecision(3) << lv << L"  (" << (delta >= 0.0 ? L"+" : L"")
     << std::setprecision(3) << delta << L")";
  RECT valRc = {rc.left + 8, rc.bottom - 20, rc.right - 8, rc.bottom - 2};
  SetTextColor(hdc, good ? RGB(0, 122, 94) : RGB(192, 57, 43));
  DrawTextW(hdc, ss.str().c_str(), -1, &valRc, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
}

struct HitPoint {
  bool ok = false;
  POINT pt = {0, 0};
  std::wstring text;
};

struct WaterfallReadout {
  bool ok = false;
  POINT pt = {0, 0};
  std::wstring text;
};

void DrawTooltip(HDC hdc, const RECT& canvas, POINT anchor, const std::wstring& text) {
  if (text.empty()) {
    return;
  }
  RECT tr = {0, 0, 380, 200};
  DrawTextW(hdc, text.c_str(), -1, &tr, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_CALCRECT);

  const int pad = 8;
  int w = (tr.right - tr.left) + pad * 2;
  int h = (tr.bottom - tr.top) + pad * 2;
  int x = anchor.x + 14;
  int y = anchor.y + 14;
  if (x + w > canvas.right - 4) {
    x = anchor.x - w - 14;
  }
  if (y + h > canvas.bottom - 4) {
    y = anchor.y - h - 14;
  }
  x = std::max(static_cast<int>(canvas.left) + 4, x);
  y = std::max(static_cast<int>(canvas.top) + 4, y);

  RECT box = {x, y, x + w, y + h};
  HBRUSH bg = CreateSolidBrush(RGB(255, 255, 245));
  FillRect(hdc, &box, bg);
  DeleteObject(bg);

  HPEN pen = CreatePen(PS_SOLID, 1, RGB(120, 120, 100));
  auto old = reinterpret_cast<HPEN>(SelectObject(hdc, pen));
  MoveToEx(hdc, box.left, box.top, nullptr);
  LineTo(hdc, box.right - 1, box.top);
  LineTo(hdc, box.right - 1, box.bottom - 1);
  LineTo(hdc, box.left, box.bottom - 1);
  LineTo(hdc, box.left, box.top);
  SelectObject(hdc, old);
  DeleteObject(pen);

  RECT tx = {box.left + pad, box.top + pad, box.right - pad, box.bottom - pad};
  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, RGB(30, 30, 30));
  DrawTextW(hdc, text.c_str(), -1, &tx, DT_LEFT | DT_TOP | DT_WORDBREAK);
}

WaterfallReadout HitTestWaterfall(const AppState* app, const RECT& rc, POINT mouse) {
  WaterfallReadout out;
  if (!app || app->waterfallW <= 0 || app->waterfallH <= 0 || app->previewWav.sampleRate <= 0) {
    return out;
  }
  const int outerGap = 10;
  const int wfH = 360;
  RECT wfRc = {rc.left + outerGap, rc.top + outerGap, rc.right - outerGap, rc.top + wfH};
  const int panH = 112;
  RECT panRc = {wfRc.left + 14, wfRc.top + 30, wfRc.right - 14, wfRc.top + 30 + panH};
  RECT plot = {wfRc.left + 14, panRc.bottom + 8, wfRc.right - 14, wfRc.bottom - 34};
  if (!PtInRect(&plot, mouse)) {
    return out;
  }

  int srcX = 0;
  int srcW = app->waterfallW;
  ComputeWaterfallSourceWindow(app, &srcX, &srcW);
  const float xn = static_cast<float>(mouse.x - plot.left) /
                   std::max<int>(1, static_cast<int>(plot.right - plot.left));
  const float yn = static_cast<float>(mouse.y - plot.top) /
                   std::max<int>(1, static_cast<int>(plot.bottom - plot.top));
  const int xCol = std::clamp(srcX + static_cast<int>(xn * srcW), 0, app->waterfallW - 1);
  const int yRow = std::clamp(static_cast<int>(yn * app->waterfallH), 0, app->waterfallH - 1);

  const float nyq = 0.5f * static_cast<float>(app->previewWav.sampleRate);
  const float fMin = 80.0f;
  const float fMax = std::min(2200.0f, nyq - 20.0f);
  const float fHz = fMax - (static_cast<float>(yRow) / std::max(1.0f, static_cast<float>(app->waterfallH - 1))) *
                                (fMax - fMin);
  const float dur = static_cast<float>(app->previewWav.samples.size()) /
                    static_cast<float>(std::max(1, app->previewWav.sampleRate));
  const float tSec = (static_cast<float>(xCol) / std::max(1.0f, static_cast<float>(app->waterfallW - 1))) * dur;

  float db = -120.0f;
  if (!app->waterfallDbRender.empty()) {
    db = app->waterfallDbRender[static_cast<std::size_t>(yRow * app->waterfallW + xCol)];
  }

  float noiseDb = app->panMinDb;
  float snrDb = db - noiseDb;

  std::wstringstream ss;
  ss << L"Waterfall Readout\n"
     << L"f: " << std::fixed << std::setprecision(3) << (fHz / 1000.0f) << L" kHz\n"
     << L"t: " << std::fixed << std::setprecision(2) << tSec << L" s\n"
     << L"dB: " << std::fixed << std::setprecision(1) << db << L"\n"
     << L"SNR~: " << std::fixed << std::setprecision(1) << snrDb << L" dB";

  out.ok = true;
  out.pt = mouse;
  out.text = ss.str();
  return out;
}

HitPoint HitTestCharts(const AppState* app, const RECT& rc, POINT mouse) {
  HitPoint hit;
  if (!app) {
    return hit;
  }

  std::vector<ChartDef> defs = {
      {L"Precision", RGB(27, 94, 32), &HistoryEntry::precision, false},
      {L"Recall", RGB(21, 101, 192), &HistoryEntry::recall, false},
      {L"False Positives / hour", RGB(211, 47, 47), &HistoryEntry::fph, true},
      {L"ID Latency (s)", RGB(255, 143, 0), &HistoryEntry::latency, true},
      {L"Runtime x Realtime", RGB(123, 31, 162), &HistoryEntry::xrt, true},
      {L"Quality Score", RGB(0, 105, 92), &HistoryEntry::quality, false},
  };

  const int outerGap = 10;
  const int wfH = 360;
  RECT chartsRc = {rc.left, rc.top + wfH + outerGap, rc.right, rc.bottom};

  const int cols = 2;
  const int rows = 3;
  const int gap = 10;
  const int w = (chartsRc.right - chartsRc.left - gap * (cols + 1)) / cols;
  const int h = (chartsRc.bottom - chartsRc.top - gap * (rows + 1)) / rows;

  auto trySeries = [&](const std::vector<HistoryEntry>& src, const ChartDef& cd, const RECT& plot,
                       const RECT& chartRc, bool isCompare) {
    if (src.size() < 2) {
      return;
    }
    double minV = std::numeric_limits<double>::max();
    double maxV = std::numeric_limits<double>::lowest();
    for (const auto& e : app->history) {
      const double v = e.*(cd.field);
      minV = std::min(minV, v);
      maxV = std::max(maxV, v);
    }
    for (const auto& e : app->historyCompare) {
      const double v = e.*(cd.field);
      minV = std::min(minV, v);
      maxV = std::max(maxV, v);
    }
    if (minV == std::numeric_limits<double>::max()) {
      minV = 0.0;
      maxV = 1.0;
    }
    if (std::fabs(maxV - minV) < 1e-12) {
      maxV += 1.0;
      minV -= 1.0;
    }

    const int radius = isCompare ? 3 : 5;
    for (std::size_t i = 0; i < src.size(); ++i) {
      const double v = src[i].*(cd.field);
      double t = static_cast<double>(i) / static_cast<double>(src.size() - 1);
      t = (t - 0.5) * app->chartZoom + 0.5;
      const int x = plot.left + static_cast<int>(t * (plot.right - plot.left)) + app->chartPanPx;
      const double yn = (v - minV) / (maxV - minV);
      const int y = plot.bottom - static_cast<int>(yn * (plot.bottom - plot.top));

      const int dx = mouse.x - x;
      const int dy = mouse.y - y;
      if ((dx * dx + dy * dy) <= radius * radius * 4) {
        hit.ok = true;
        hit.pt = {x, y};
        std::wstringstream ss;
        ss << cd.title << L"\n"
           << L"Run: " << ToWide(src[i].runId) << L"\n"
           << L"Time: " << ToWide(src[i].ts) << L"\n"
           << L"Commit: " << ToWide(src[i].gitCommit) << L"\n"
           << L"Branch: " << ToWide(src[i].gitBranch) << L"\n"
           << L"Value: " << std::fixed << std::setprecision(6) << v
           << (isCompare ? L"\nSeries: Compare" : L"\nSeries: Primary");
        hit.text = ss.str();
        return;
      }
    }
  };

  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      const int idx = r * cols + c;
      RECT cr = {chartsRc.left + gap + c * (w + gap), chartsRc.top + gap + r * (h + gap),
                 chartsRc.left + gap + c * (w + gap) + w,
                 chartsRc.top + gap + r * (h + gap) + h};
      if (mouse.x < cr.left || mouse.x > cr.right || mouse.y < cr.top || mouse.y > cr.bottom) {
        continue;
      }
      RECT plot = {cr.left + 8, cr.top + 28, cr.right - 8, cr.bottom - 24};
      trySeries(app->history, defs[idx], plot, cr, false);
      if (hit.ok) {
        return hit;
      }
      trySeries(app->historyCompare, defs[idx], plot, cr, true);
      if (hit.ok) {
        return hit;
      }
    }
  }

  return hit;
}

void DrawCharts(AppState* app, HDC hdc, const RECT& rc) {
  HBRUSH bg = CreateSolidBrush(RGB(248, 250, 252));
  FillRect(hdc, &rc, bg);
  DeleteObject(bg);

  std::vector<ChartDef> defs = {
      {L"Precision", RGB(27, 94, 32), &HistoryEntry::precision, false},
      {L"Recall", RGB(21, 101, 192), &HistoryEntry::recall, false},
      {L"False Positives / hour", RGB(211, 47, 47), &HistoryEntry::fph, true},
      {L"ID Latency (s)", RGB(255, 143, 0), &HistoryEntry::latency, true},
      {L"Runtime x Realtime", RGB(123, 31, 162), &HistoryEntry::xrt, true},
      {L"Quality Score", RGB(0, 105, 92), &HistoryEntry::quality, false},
  };

  const int outerGap = 10;
  const int wfH = 360;
  RECT wfRc = {rc.left + outerGap, rc.top + outerGap, rc.right - outerGap, rc.top + wfH};
  DrawWaterfallCard(app, hdc, wfRc);

  RECT chartsRc = {rc.left, rc.top + wfH + outerGap, rc.right, rc.bottom};
  const int cols = 2;
  const int rows = 3;
  const int gap = 10;
  const int w = (chartsRc.right - chartsRc.left - gap * (cols + 1)) / cols;
  const int h = (chartsRc.bottom - chartsRc.top - gap * (rows + 1)) / rows;

  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      const int idx = r * cols + c;
      RECT cr = {chartsRc.left + gap + c * (w + gap), chartsRc.top + gap + r * (h + gap),
                 chartsRc.left + gap + c * (w + gap) + w,
                 chartsRc.top + gap + r * (h + gap) + h};
      DrawMetricChart(hdc, cr, app->history, app->historyCompare, defs[idx], app->chartZoom,
                      app->chartPanPx);
    }
  }

  if (app->waterfallHoverActive) {
    DrawTooltip(hdc, rc, app->waterfallHoverPoint, app->waterfallHoverText);
  } else if (app->hoverActive) {
    DrawTooltip(hdc, rc, app->hoverPoint, app->hoverText);
  }
}

void RefreshHistory(AppState* app) {
  const std::string path = ToUtf8(GetText(app->historyEdit));
  if (path.empty()) {
    SetStatus(app, L"History path is empty");
    return;
  }
  std::string error;
  std::vector<HistoryEntry> hist;
  if (!LoadHistoryCsv(path, &hist, &error)) {
    SetStatus(app, L"History load failed");
    SetSummary(app, ToWide(error));
    InvalidateRect(app->chartPanel, nullptr, TRUE);
    return;
  }
  app->history = std::move(hist);
  std::wstringstream ss;
  ss << L"History loaded: " << app->history.size() << L" runs";
  SetStatus(app, ss.str());
  InvalidateRect(app->chartPanel, nullptr, TRUE);
}

void RefreshCompareHistory(AppState* app) {
  const std::string path = ToUtf8(GetText(app->compareEdit));
  if (path.empty()) {
    app->historyCompare.clear();
    InvalidateRect(app->chartPanel, nullptr, TRUE);
    return;
  }
  std::string error;
  std::vector<HistoryEntry> hist;
  if (!LoadHistoryCsv(path, &hist, &error)) {
    SetSummary(app, L"Compare history load failed:\r\n" + ToWide(error));
    return;
  }
  app->historyCompare = std::move(hist);
  InvalidateRect(app->chartPanel, nullptr, TRUE);
}

void RefreshWaterfallFromInput(AppState* app) {
  if (!app) {
    return;
  }
  app->previewWav = ndb::WavData();
  app->overlayRows.clear();
  app->waterfallRgb.clear();
  app->waterfallW = 0;
  app->waterfallH = 0;
  app->waterfallZoom = 1.0;
  app->waterfallPanPx = 0;

  const std::wstring inW = GetText(app->inputEdit);
  if (inW.empty()) {
    InvalidateRect(app->chartPanel, nullptr, TRUE);
    return;
  }

  ndb::WavData wav;
  std::string err;
  if (ndb::ReadWavMono16(ToUtf8(inW), &wav, &err)) {
    app->previewWav = std::move(wav);
  }
  InvalidateRect(app->chartPanel, nullptr, TRUE);
}

void ExportChartPanelPng(AppState* app) {
  if (!app || !app->chartPanel) {
    return;
  }
  const std::wstring path = ChooseSaveFile(app->hwnd, L"Export dashboard PNG",
                                           L"PNG files (*.png)\0*.png\0All files (*.*)\0*.*\0",
                                           L"png");
  if (path.empty()) {
    return;
  }

  RECT rc;
  GetClientRect(app->chartPanel, &rc);
  const int w = rc.right - rc.left;
  const int h = rc.bottom - rc.top;

  HDC hdc = GetDC(app->chartPanel);
  HDC memdc = CreateCompatibleDC(hdc);
  HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
  auto oldBmp = reinterpret_cast<HBITMAP>(SelectObject(memdc, bmp));

  DrawCharts(app, memdc, rc);

  Gdiplus::Bitmap gbmp(bmp, nullptr);
  CLSID pngClsid;
  if (GetEncoderClsid(L"image/png", &pngClsid) < 0) {
    MessageBoxW(app->hwnd, L"PNG encoder not available.", L"Export failed", MB_OK | MB_ICONERROR);
  } else {
    gbmp.Save(path.c_str(), &pngClsid, nullptr);
    MessageBoxW(app->hwnd, L"Dashboard exported successfully.", L"Export complete",
                MB_OK | MB_ICONINFORMATION);
  }

  SelectObject(memdc, oldBmp);
  DeleteObject(bmp);
  DeleteDC(memdc);
  ReleaseDC(app->chartPanel, hdc);
}

void StartDecode(AppState* app) {
  if (app->running) {
    return;
  }

  const std::wstring inW = GetText(app->inputEdit);
  const std::wstring outW = GetText(app->outputEdit);
  const std::wstring metW = GetText(app->metricsEdit);
  if (inW.empty() || outW.empty()) {
    MessageBoxW(app->hwnd, L"Select input WAV and output CSV.", L"Missing fields",
                MB_ICONWARNING | MB_OK);
    return;
  }

  app->running = true;
  app->decodeProgressPct = 0;
  app->decodeProgressVisualPct = 0.0;
  app->decodeStartTickMs = GetTickCount64();
  SetTimer(app->hwnd, kWaterfallTimerId, WaterfallTimerMs(app), nullptr);
  SetBusy(app, true);
  SendMessageW(app->progressBar, PBM_SETPOS, 0, 0);
  SetStatus(app, L"Starting...");
  SetSummary(app, L"");

  const std::string inputPath = ToUtf8(inW);
  const std::string outputPath = ToUtf8(outW);
  const std::string metricsPath = ToUtf8(metW);
  const std::string priorPath = ToUtf8(GetText(app->priorEdit));
  int sel = 0;
  if (app->presetCombo) {
    sel = static_cast<int>(SendMessageW(app->presetCombo, CB_GETCURSEL, 0, 0));
  }
  int calibSel = 0;
  if (app->calibCombo) {
    calibSel = static_cast<int>(SendMessageW(app->calibCombo, CB_GETCURSEL, 0, 0));
  }
  const bool requirePrior =
      app->priorCheck && (SendMessageW(app->priorCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
  std::string mode = "default";
  if (sel == 1) {
    mode = "strict-dx";
  } else if (sel == 2) {
    mode = "relaxed";
  } else if (sel == 3) {
    mode = "phase3-balanced";
  } else if (sel == 4) {
    mode = "phase3-selective";
  } else if (sel == 5) {
    mode = "phase4-serious";
  } else if (sel == 6) {
    mode = "quiet";
  } else if (sel == 7) {
    mode = "urban-noise";
  } else if (sel == 8) {
    mode = "weak-signal-dx";
  } else if (sel == 9) {
    mode = "step3-fusion";
  }

  app->worker = std::thread([app, inputPath, outputPath, metricsPath, priorPath, mode, calibSel,
                             requirePrior]() {
    auto* result = new DecodeThreadResult();
    result->outputPath = outputPath;
    result->metricsPath = metricsPath;

    ndb::WavData wav;
    if (!ndb::ReadWavMono16(inputPath, &wav, &result->error)) {
      PostMessageW(app->hwnd, kMsgDone, 0, reinterpret_cast<LPARAM>(result));
      return;
    }

    ndb::DecoderConfig cfg;
    ApplyPresetToConfig(mode, &cfg);
    if (calibSel == 1) {
      cfg.confidenceCalibration = "platt";
    } else if (calibSel == 2) {
      cfg.confidenceCalibration = "isotonic";
    } else {
      cfg.confidenceCalibration = "none";
    }
    if (!priorPath.empty()) {
      cfg.enableFreqPriors = true;
      cfg.freqPriorFile = priorPath;
      cfg.requirePriorMatch = requirePrior;
    } else {
      cfg.enableFreqPriors = false;
      cfg.requirePriorMatch = false;
    }
    auto progress = [app](int percent, const std::string&) {
      PostMessageW(app->hwnd, kMsgProgress, static_cast<WPARAM>(percent), 0);
    };
    auto rows = ndb::DecodeNdbFromWav(wav.samples, wav.sampleRate, cfg, &result->stats, progress);
    result->rowCount = rows.size();
    result->decodedRows = rows;

    if (!WriteCsv(outputPath, rows, &result->error)) {
      PostMessageW(app->hwnd, kMsgDone, 0, reinterpret_cast<LPARAM>(result));
      return;
    }
    if (!metricsPath.empty()) {
      if (!WriteMetrics(metricsPath, result->stats, &result->error)) {
        PostMessageW(app->hwnd, kMsgDone, 0, reinterpret_cast<LPARAM>(result));
        return;
      }
    }

    result->ok = true;
    PostMessageW(app->hwnd, kMsgDone, 0, reinterpret_cast<LPARAM>(result));
  });
  app->worker.detach();
}

void OnDone(AppState* app, DecodeThreadResult* result) {
  app->running = false;
  app->decodeProgressPct = 100;
  app->decodeProgressVisualPct = 100.0;
  KillTimer(app->hwnd, kWaterfallTimerId);
  SetBusy(app, false);
  SendMessageW(app->progressBar, PBM_SETPOS, 100, 0);

  if (!result->ok) {
    SetStatus(app, L"Failed");
    SetSummary(app, ToWide(result->error));
    MessageBoxW(app->hwnd, ToWide(result->error).c_str(), L"Decode failed", MB_ICONERROR | MB_OK);
  } else {
    app->overlayRows = result->decodedRows;
    InvalidateRect(app->chartPanel, nullptr, TRUE);
    SetStatus(app, L"Completed");
    SetSummary(app, BuildSummary(*result));
    std::wstring msg = L"CSV saved:\n" + ToWide(result->outputPath);
    if (!result->metricsPath.empty()) {
      msg += L"\n\nMetrics saved:\n" + ToWide(result->metricsPath);
    }
    MessageBoxW(app->hwnd, msg.c_str(), L"Done", MB_OK | MB_ICONINFORMATION);
  }

  delete result;
}

LRESULT CALLBACK ChartProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  auto* app = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_LBUTTONDOWN:
      if (app) {
        const POINT p = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        RECT rc;
        GetClientRect(hwnd, &rc);
        const RECT wfPlot = GetWaterfallPlotRect(rc);
        if (PtInRect(&wfPlot, p)) {
          app->waterfallDragging = true;
          app->waterfallDragStartX = p.x;
          app->waterfallPanStartPx = app->waterfallPanPx;
        } else {
          app->dragging = true;
          app->dragStartX = p.x;
          app->panStartPx = app->chartPanPx;
        }
        SetCapture(hwnd);
      }
      return 0;
    case WM_MOUSEMOVE:
      if (app) {
        if (!app->mouseLeaveArmed) {
          TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
          TrackMouseEvent(&tme);
          app->mouseLeaveArmed = true;
        }
        const POINT p = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (app->waterfallDragging) {
          const int dx = app->waterfallDragStartX - p.x;
          const int vis = std::max(60, static_cast<int>(std::round(
                                       static_cast<double>(std::max(1, app->waterfallW)) /
                                       std::max(1.0, app->waterfallZoom))));
          const int maxPan = std::max(0, app->waterfallW - vis);
          app->waterfallPanPx = std::clamp(app->waterfallPanStartPx + dx, 0, maxPan);
          app->hoverActive = false;
          app->waterfallHoverActive = false;
          InvalidateRect(hwnd, nullptr, TRUE);
        } else if (app->dragging) {
          app->chartPanPx = app->panStartPx + (p.x - app->dragStartX);
          app->hoverActive = false;
          app->waterfallHoverActive = false;
          InvalidateRect(hwnd, nullptr, TRUE);
        } else {
          RECT rc;
          GetClientRect(hwnd, &rc);
          auto wf = HitTestWaterfall(app, rc, p);
          if (wf.ok) {
            app->waterfallHoverActive = true;
            app->waterfallHoverPoint = wf.pt;
            app->waterfallHoverText = wf.text;
            app->hoverActive = false;
          } else {
            app->waterfallHoverActive = false;
            auto hit = HitTestCharts(app, rc, p);
            if (hit.ok) {
              app->hoverActive = true;
              app->hoverPoint = hit.pt;
              app->hoverText = hit.text;
            } else {
              app->hoverActive = false;
            }
          }
          InvalidateRect(hwnd, nullptr, TRUE);
        }
      }
      return 0;
    case WM_LBUTTONUP:
      if (app && (app->dragging || app->waterfallDragging)) {
        app->dragging = false;
        app->waterfallDragging = false;
        ReleaseCapture();
      }
      return 0;
    case WM_MOUSELEAVE:
      if (app) {
        app->mouseLeaveArmed = false;
        app->hoverActive = false;
        app->waterfallHoverActive = false;
        InvalidateRect(hwnd, nullptr, TRUE);
      }
      return 0;
  }

  if (msg == WM_PAINT) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (app) {
      HDC memdc = CreateCompatibleDC(hdc);
      HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right - rc.left, rc.bottom - rc.top);
      auto oldBmp = reinterpret_cast<HBITMAP>(SelectObject(memdc, bmp));
      DrawCharts(app, memdc, rc);
      BitBlt(hdc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, memdc, 0, 0, SRCCOPY);
      SelectObject(memdc, oldBmp);
      DeleteObject(bmp);
      DeleteDC(memdc);
    }
    EndPaint(hwnd, &ps);
    return 0;
  }
  if (msg == WM_ERASEBKGND) {
    return 1;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  AppState* app = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_CREATE: {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
      app = reinterpret_cast<AppState*>(cs->lpCreateParams);
      app->hwnd = hwnd;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));

      NONCLIENTMETRICSW ncm = {sizeof(ncm)};
      SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
      app->font = CreateFontIndirectW(&ncm.lfMessageFont);
      LOGFONTW lf = ncm.lfMessageFont;
      lf.lfHeight += 4;
      lf.lfWeight = FW_SEMIBOLD;
      app->fontBig = CreateFontIndirectW(&lf);

      LOGFONTW mono = ncm.lfMessageFont;
      wcscpy_s(mono.lfFaceName, LF_FACESIZE, L"Consolas");
      app->fontMono = CreateFontIndirectW(&mono);

      WNDCLASSW cc = {};
      cc.lpfnWndProc = ChartProc;
      cc.hInstance = cs->hInstance;
      cc.lpszClassName = L"JNDBChartPanel";
      cc.hCursor = LoadCursor(nullptr, IDC_ARROW);
      RegisterClassW(&cc);

      const int m = 16;
      const int leftW = 860;
      const int rightX = m + leftW + 12;
      const int rightW = 1820 - rightX - m;
      int y = 16;

      HWND title = CreateWindowW(L"STATIC", L"JNDB Professional Decoder", WS_CHILD | WS_VISIBLE,
                                 m, y, 520, 34, hwnd, nullptr, nullptr, nullptr);
      SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(app->fontBig), TRUE);
      y += 40;

      auto addRow = [&](const wchar_t* label, int editId, int btnId, int yrow, const wchar_t* btnText) {
        CreateWindowW(L"STATIC", label, WS_CHILD | WS_VISIBLE, m, yrow + 6, 100, 22, hwnd, nullptr,
                      nullptr, nullptr);
        HWND e = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                               m + 100, yrow, 250, 30, hwnd,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(editId)), nullptr,
                               nullptr);
        CreateWindowW(L"BUTTON", btnText, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, m + 358, yrow, 96,
                      32, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(btnId)), nullptr,
                      nullptr);
        SendMessageW(e, WM_SETFONT, reinterpret_cast<WPARAM>(app->font), TRUE);
        return e;
      };

      app->inputEdit = addRow(L"Input WAV", kIdInputEdit, kIdInputBrowse, y, L"Browse");
      y += 40;
      app->outputEdit = addRow(L"Output CSV", kIdOutputEdit, kIdOutputBrowse, y, L"Browse");
      y += 40;
      app->metricsEdit = addRow(L"Metrics", kIdMetricsEdit, kIdMetricsBrowse, y, L"Browse");
      CreateWindowW(L"STATIC", L"Preset", WS_CHILD | WS_VISIBLE, m + 464, y + 6, 56, 22, hwnd,
                    nullptr, nullptr, nullptr);
      app->presetCombo = CreateWindowW(
          L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
          m + 520, y, 168, 140, hwnd, (HMENU)kIdPresetCombo, nullptr, nullptr);
      SendMessageW(app->presetCombo, CB_ADDSTRING, 0, (LPARAM)L"Default");
      SendMessageW(app->presetCombo, CB_ADDSTRING, 0, (LPARAM)L"Strict DX");
      SendMessageW(app->presetCombo, CB_ADDSTRING, 0, (LPARAM)L"Relaxed");
      SendMessageW(app->presetCombo, CB_ADDSTRING, 0, (LPARAM)L"Phase3 Balanced");
      SendMessageW(app->presetCombo, CB_ADDSTRING, 0, (LPARAM)L"Phase3 Selective");
      SendMessageW(app->presetCombo, CB_ADDSTRING, 0, (LPARAM)L"Phase4 Serious");
      SendMessageW(app->presetCombo, CB_ADDSTRING, 0, (LPARAM)L"Quiet");
      SendMessageW(app->presetCombo, CB_ADDSTRING, 0, (LPARAM)L"Urban Noise");
      SendMessageW(app->presetCombo, CB_ADDSTRING, 0, (LPARAM)L"Weak-signal DX");
      SendMessageW(app->presetCombo, CB_ADDSTRING, 0, (LPARAM)L"Step3 Fusion");
      SendMessageW(app->presetCombo, CB_SETCURSEL, 0, 0);
      app->presetHintText = CreateWindowW(
          L"STATIC", L"", WS_CHILD | WS_VISIBLE, m + 464, y + 34, 340, 28, hwnd, nullptr,
          nullptr, nullptr);
      SendMessageW(app->presetHintText, WM_SETFONT, reinterpret_cast<WPARAM>(app->font), TRUE);
      UpdatePresetHint(app);
      y += 40;

      CreateWindowW(L"STATIC", L"Calib", WS_CHILD | WS_VISIBLE, m, y + 6, 56, 22, hwnd, nullptr,
                    nullptr, nullptr);
      app->calibCombo = CreateWindowW(L"COMBOBOX", L"",
                                      WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
                                      m + 100, y, 160, 140, hwnd, (HMENU)kIdCalibCombo, nullptr,
                                      nullptr);
      SendMessageW(app->calibCombo, CB_ADDSTRING, 0, (LPARAM)L"None");
      SendMessageW(app->calibCombo, CB_ADDSTRING, 0, (LPARAM)L"Platt");
      SendMessageW(app->calibCombo, CB_ADDSTRING, 0, (LPARAM)L"Isotonic");
      SendMessageW(app->calibCombo, CB_SETCURSEL, 0, 0);
      CreateWindowW(L"STATIC", L"Waterfall", WS_CHILD | WS_VISIBLE, m + 280, y + 6, 70, 22, hwnd,
                    nullptr, nullptr, nullptr);
      app->waterfallViewCombo = CreateWindowW(
          L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST, m + 352, y,
          180, 120, hwnd, (HMENU)kIdWaterfallViewCombo, nullptr, nullptr);
      SendMessageW(app->waterfallViewCombo, CB_ADDSTRING, 0, (LPARAM)L"2D only");
      SendMessageW(app->waterfallViewCombo, CB_ADDSTRING, 0, (LPARAM)L"2D + 3D");
      SendMessageW(app->waterfallViewCombo, CB_ADDSTRING, 0, (LPARAM)L"3D large");
      SendMessageW(app->waterfallViewCombo, CB_SETCURSEL, 1, 0);
      y += 40;

      CreateWindowW(L"STATIC", L"Yaw", WS_CHILD | WS_VISIBLE, m, y + 6, 36, 22, hwnd, nullptr,
                    nullptr, nullptr);
      app->yawSlider = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
                                     m + 40, y, 160, 28, hwnd, (HMENU)kIdYawSlider, nullptr,
                                     nullptr);
      SendMessageW(app->yawSlider, TBM_SETRANGEMIN, FALSE, 10);
      SendMessageW(app->yawSlider, TBM_SETRANGEMAX, FALSE, 75);
      SendMessageW(app->yawSlider, TBM_SETPOS, TRUE, app->yawDeg);

      CreateWindowW(L"STATIC", L"Pitch", WS_CHILD | WS_VISIBLE, m + 212, y + 6, 40, 22, hwnd,
                    nullptr, nullptr, nullptr);
      app->pitchSlider = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
                                       m + 256, y, 160, 28, hwnd, (HMENU)kIdPitchSlider, nullptr,
                                       nullptr);
      SendMessageW(app->pitchSlider, TBM_SETRANGEMIN, FALSE, 8);
      SendMessageW(app->pitchSlider, TBM_SETRANGEMAX, FALSE, 60);
      SendMessageW(app->pitchSlider, TBM_SETPOS, TRUE, app->pitchDeg);

      app->shadingCheck = CreateWindowW(L"BUTTON", L"Directional shading",
                                        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, m + 430, y + 4,
                                        150, 24, hwnd, (HMENU)kIdShadingCheck, nullptr, nullptr);
      SendMessageW(app->shadingCheck, BM_SETCHECK, BST_CHECKED, 0);

      CreateWindowW(L"STATIC", L"3D Colormap", WS_CHILD | WS_VISIBLE, m + 588, y + 6, 88, 22,
                    hwnd, nullptr, nullptr, nullptr);
      app->colormapCombo = CreateWindowW(L"COMBOBOX", L"",
                                         WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
                                         m + 676, y, 120, 110, hwnd,
                                         (HMENU)kIdColormapCombo, nullptr, nullptr);
      SendMessageW(app->colormapCombo, CB_ADDSTRING, 0, (LPARAM)L"Aurora");
      SendMessageW(app->colormapCombo, CB_ADDSTRING, 0, (LPARAM)L"Magma");
      SendMessageW(app->colormapCombo, CB_ADDSTRING, 0, (LPARAM)L"Neon");
      SendMessageW(app->colormapCombo, CB_SETCURSEL, 0, 0);
      CreateWindowW(L"STATIC", L"FPS", WS_CHILD | WS_VISIBLE, m + 968, y + 6, 28, 22, hwnd,
                    nullptr, nullptr, nullptr);
      app->waterfallFpsCombo = CreateWindowW(L"COMBOBOX", L"",
                                             WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
                                             m + 1000, y, 84, 100, hwnd,
                                             (HMENU)kIdWaterfallFpsCombo, nullptr, nullptr);
      SendMessageW(app->waterfallFpsCombo, CB_ADDSTRING, 0, (LPARAM)L"30 FPS");
      SendMessageW(app->waterfallFpsCombo, CB_ADDSTRING, 0, (LPARAM)L"60 FPS");
      SendMessageW(app->waterfallFpsCombo, CB_SETCURSEL, 0, 0);
      CreateWindowW(L"BUTTON", L"3D DX Weak Preset", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    m + 1092, y, 156, 30, hwnd, (HMENU)kIdWaterfallDxPreset, nullptr, nullptr);
      y += 38;

      CreateWindowW(L"STATIC", L"Prior CSV", WS_CHILD | WS_VISIBLE, m, y + 6, 100, 22, hwnd,
                    nullptr, nullptr, nullptr);
      app->priorEdit = CreateWindowW(L"EDIT", L"",
                                     WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                     m + 100, y, 250, 30, hwnd, (HMENU)kIdPriorEdit, nullptr,
                                     nullptr);
      CreateWindowW(L"BUTTON", L"Browse", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, m + 358, y,
                    96, 32, hwnd, (HMENU)kIdPriorBrowse, nullptr, nullptr);
      app->priorCheck = CreateWindowW(L"BUTTON", L"Require prior match",
                                      WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, m + 464, y + 5,
                                      170, 24, hwnd, (HMENU)kIdRequirePrior, nullptr, nullptr);
      y += 40;
      app->historyEdit = addRow(L"History", kIdHistoryEdit, kIdHistoryBrowse, y, L"Browse");
      y += 40;
      app->compareEdit = addRow(L"Compare", kIdCompareEdit, kIdCompareBrowse, y, L"Browse");

      CreateWindowW(L"BUTTON", L"Refresh Charts", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, m + 260,
                    y + 40, 130, 34, hwnd, (HMENU)kIdHistoryRefresh, nullptr, nullptr);
      CreateWindowW(L"BUTTON", L"Reset View", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, m + 394, y + 40,
                    120, 34, hwnd, (HMENU)kIdResetView, nullptr, nullptr);
      CreateWindowW(L"BUTTON", L"Export PNG", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, m + 520, y + 40,
                    120, 34, hwnd, (HMENU)kIdExportPng, nullptr, nullptr);
      app->runButton = CreateWindowW(L"BUTTON", L"Start Decode", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                     m + 100, y + 40, 152, 34, hwnd, (HMENU)kIdRun, nullptr, nullptr);
      y += 82;

      app->progressBar = CreateWindowW(PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                                       m, y, leftW - 8, 22, hwnd, (HMENU)kIdProgress, nullptr, nullptr);
      SendMessageW(app->progressBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
      y += 30;

      app->statusText = CreateWindowW(L"STATIC", L"Ready", WS_CHILD | WS_VISIBLE, m, y, leftW - 8, 22,
                                      hwnd, (HMENU)kIdStatus, nullptr, nullptr);
      y += 28;

      app->summaryText = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE |
                                                     ES_READONLY | WS_VSCROLL,
                                       m, y, leftW - 8, 440, hwnd, (HMENU)kIdSummary, nullptr, nullptr);
      SendMessageW(app->summaryText, WM_SETFONT, reinterpret_cast<WPARAM>(app->fontMono), TRUE);

      app->chartPanel = CreateWindowW(L"JNDBChartPanel", nullptr,
                                      WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP,
                                      rightX, 16, rightW, 860, hwnd, (HMENU)kIdChartPanel, nullptr,
                                      nullptr);
      SetWindowLongPtrW(app->chartPanel, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));

      const HWND controls[] = {app->runButton, app->statusText, app->historyEdit,
                               app->compareEdit, app->inputEdit, app->outputEdit, app->metricsEdit,
                               app->presetCombo, app->calibCombo, app->waterfallViewCombo,
                               app->yawSlider, app->pitchSlider, app->shadingCheck,
                               app->colormapCombo, app->waterfallFpsCombo,
                               app->priorEdit, app->priorCheck};
      for (HWND c : controls) {
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(app->font), TRUE);
      }

      wchar_t modPath[MAX_PATH] = {};
      GetModuleFileNameW(nullptr, modPath, MAX_PATH);
      std::filesystem::path p(modPath);
      auto hist = p.parent_path().parent_path().parent_path() / "benchmarks" / "phase0" / "history" /
                  "benchmark_history.csv";
      SetText(app->historyEdit, hist.wstring());
      RefreshHistory(app);
      RefreshCompareHistory(app);
      RefreshWaterfallFromInput(app);
      return 0;
    }
    case WM_COMMAND: {
      if (!app) return 0;
      switch (LOWORD(wParam)) {
        case kIdInputBrowse: {
          const auto p = ChooseOpenFile(hwnd, L"Open WAV",
                                        L"WAV files (*.wav)\0*.wav\0All files (*.*)\0*.*\0");
          if (!p.empty()) {
            SetText(app->inputEdit, p);
            if (GetText(app->outputEdit).empty()) {
              auto csv = p;
              const auto pos = csv.find_last_of(L'.');
              if (pos != std::wstring::npos) csv = csv.substr(0, pos);
              csv += L"_out.csv";
              SetText(app->outputEdit, csv);
            }
            if (GetText(app->metricsEdit).empty()) {
              auto js = p;
              const auto pos = js.find_last_of(L'.');
              if (pos != std::wstring::npos) js = js.substr(0, pos);
              js += L"_metrics.json";
              SetText(app->metricsEdit, js);
            }
            RefreshWaterfallFromInput(app);
          }
          return 0;
        }
        case kIdOutputBrowse: {
          const auto p = ChooseSaveFile(
              hwnd, L"Save CSV", L"CSV files (*.csv)\0*.csv\0All files (*.*)\0*.*\0", L"csv");
          if (!p.empty()) SetText(app->outputEdit, p);
          return 0;
        }
        case kIdMetricsBrowse: {
          const auto p =
              ChooseSaveFile(hwnd, L"Save metrics JSON",
                             L"JSON files (*.json)\0*.json\0All files (*.*)\0*.*\0", L"json");
          if (!p.empty()) SetText(app->metricsEdit, p);
          return 0;
        }
        case kIdPriorBrowse: {
          const auto p = ChooseOpenFile(
              hwnd, L"Open frequency prior CSV",
              L"CSV files (*.csv)\0*.csv\0All files (*.*)\0*.*\0");
          if (!p.empty()) {
            SetText(app->priorEdit, p);
          }
          return 0;
        }
        case kIdHistoryBrowse: {
          const auto p = ChooseOpenFile(
              hwnd, L"Open benchmark history CSV",
              L"CSV files (*.csv)\0*.csv\0All files (*.*)\0*.*\0");
          if (!p.empty()) {
            SetText(app->historyEdit, p);
            RefreshHistory(app);
          }
          return 0;
        }
        case kIdCompareBrowse: {
          const auto p = ChooseOpenFile(
              hwnd, L"Open compare benchmark history CSV",
              L"CSV files (*.csv)\0*.csv\0All files (*.*)\0*.*\0");
          if (!p.empty()) {
            SetText(app->compareEdit, p);
            RefreshCompareHistory(app);
          }
          return 0;
        }
        case kIdHistoryRefresh:
          RefreshHistory(app);
          RefreshCompareHistory(app);
          return 0;
        case kIdResetView:
          app->chartZoom = 1.0;
          app->chartPanPx = 0;
          app->waterfallZoom = 1.0;
          app->waterfallPanPx = 0;
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        case kIdExportPng:
          ExportChartPanelPng(app);
          return 0;
        case kIdRun:
          StartDecode(app);
          return 0;
        case kIdPresetCombo:
          if (HIWORD(wParam) == CBN_SELCHANGE) {
            UpdatePresetHint(app);
            ApplyPresetUiDefaults(app);
          }
          return 0;
        case kIdWaterfallViewCombo:
          if (HIWORD(wParam) == CBN_SELCHANGE && app->waterfallViewCombo) {
            const int sel = static_cast<int>(SendMessageW(app->waterfallViewCombo, CB_GETCURSEL, 0, 0));
            app->waterfallViewMode = std::clamp(sel, 0, 2);
            InvalidateRect(app->chartPanel, nullptr, TRUE);
          }
          return 0;
        case kIdShadingCheck:
          app->shadingEnabled =
              (SendMessageW(app->shadingCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        case kIdColormapCombo:
          if (HIWORD(wParam) == CBN_SELCHANGE && app->colormapCombo) {
            const int sel = static_cast<int>(SendMessageW(app->colormapCombo, CB_GETCURSEL, 0, 0));
            app->colormap3d = std::clamp(sel, 0, 2);
            InvalidateRect(app->chartPanel, nullptr, TRUE);
          }
          return 0;
        case kIdWaterfallDxPreset:
          app->waterfallViewMode = 2;
          app->yawDeg = 44;
          app->pitchDeg = 31;
          app->shadingEnabled = true;
          app->colormap3d = 1;
          app->waterfallFps = 60;
          if (app->waterfallViewCombo) {
            SendMessageW(app->waterfallViewCombo, CB_SETCURSEL, app->waterfallViewMode, 0);
          }
          if (app->yawSlider) {
            SendMessageW(app->yawSlider, TBM_SETPOS, TRUE, app->yawDeg);
          }
          if (app->pitchSlider) {
            SendMessageW(app->pitchSlider, TBM_SETPOS, TRUE, app->pitchDeg);
          }
          if (app->shadingCheck) {
            SendMessageW(app->shadingCheck, BM_SETCHECK,
                         app->shadingEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
          }
          if (app->colormapCombo) {
            SendMessageW(app->colormapCombo, CB_SETCURSEL, app->colormap3d, 0);
          }
          if (app->waterfallFpsCombo) {
            SendMessageW(app->waterfallFpsCombo, CB_SETCURSEL, 1, 0);
          }
          if (app->running) {
            SetTimer(app->hwnd, kWaterfallTimerId, WaterfallTimerMs(app), nullptr);
          }
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        case kIdWaterfallFpsCombo:
          if (HIWORD(wParam) == CBN_SELCHANGE && app->waterfallFpsCombo) {
            const int sel = static_cast<int>(SendMessageW(app->waterfallFpsCombo, CB_GETCURSEL, 0, 0));
            app->waterfallFps = (sel == 1) ? 60 : 30;
            if (app->running) {
              SetTimer(app->hwnd, kWaterfallTimerId, WaterfallTimerMs(app), nullptr);
            }
            InvalidateRect(app->chartPanel, nullptr, TRUE);
          }
          return 0;
      }
      return 0;
    }
    case WM_HSCROLL:
      if (app) {
        HWND src = reinterpret_cast<HWND>(lParam);
        if (src == app->yawSlider) {
          app->yawDeg = static_cast<int>(SendMessageW(app->yawSlider, TBM_GETPOS, 0, 0));
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        }
        if (src == app->pitchSlider) {
          app->pitchDeg = static_cast<int>(SendMessageW(app->pitchSlider, TBM_GETPOS, 0, 0));
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        }
      }
      break;
    case kMsgProgress:
      if (app) {
        app->decodeProgressPct = static_cast<int>(wParam);
        SendMessageW(app->progressBar, PBM_SETPOS, static_cast<int>(wParam), 0);
        std::wstringstream ss;
        ss << L"Processing... " << static_cast<int>(wParam) << L"%";
        SetStatus(app, ss.str());
        InvalidateRect(app->chartPanel, nullptr, TRUE);
      }
      return 0;
    case kMsgDone:
      if (app) {
        OnDone(app, reinterpret_cast<DecodeThreadResult*>(lParam));
      }
      return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
      HDC hdc = reinterpret_cast<HDC>(wParam);
      SetBkMode(hdc, TRANSPARENT);
      SetTextColor(hdc, RGB(24, 24, 24));
      static HBRUSH bg = CreateSolidBrush(RGB(248, 250, 252));
      return reinterpret_cast<LRESULT>(bg);
    }
    case WM_ERASEBKGND: {
      RECT r;
      GetClientRect(hwnd, &r);
      HBRUSH b = CreateSolidBrush(RGB(248, 250, 252));
      FillRect(reinterpret_cast<HDC>(wParam), &r, b);
      DeleteObject(b);
      return 1;
    }
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    case WM_MOUSEWHEEL:
      if (app) {
        const short z = GET_WHEEL_DELTA_WPARAM(wParam);
        const double factor = z > 0 ? 1.12 : (1.0 / 1.12);
        POINT pScr = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (app->chartPanel) {
          POINT pPanel = pScr;
          ScreenToClient(app->chartPanel, &pPanel);
          RECT rcPanel;
          GetClientRect(app->chartPanel, &rcPanel);
          RECT wfPlot = GetWaterfallPlotRect(rcPanel);
          if (PtInRect(&wfPlot, pPanel)) {
            app->waterfallZoom = std::clamp(app->waterfallZoom * factor, 1.0, 8.0);
            const int vis = std::max(60, static_cast<int>(std::round(
                                         static_cast<double>(std::max(1, app->waterfallW)) /
                                         std::max(1.0, app->waterfallZoom))));
            const int maxPan = std::max(0, app->waterfallW - vis);
            app->waterfallPanPx = std::clamp(app->waterfallPanPx, 0, maxPan);
          } else {
            app->chartZoom = std::clamp(app->chartZoom * factor, 1.0, 8.0);
          }
        } else {
          app->chartZoom = std::clamp(app->chartZoom * factor, 1.0, 8.0);
        }
        InvalidateRect(app->chartPanel, nullptr, TRUE);
      }
      return 0;
    case WM_TIMER:
      if (app && wParam == kWaterfallTimerId && app->running) {
        const ULONGLONG now = GetTickCount64();
        const double elapsedMs = static_cast<double>(now - app->decodeStartTickMs);
        double rtPct = 0.0;
        if (app->previewWav.sampleRate > 0 && !app->previewWav.samples.empty()) {
          const double durSec = static_cast<double>(app->previewWav.samples.size()) /
                                static_cast<double>(app->previewWav.sampleRate);
          if (durSec > 0.01) {
            rtPct = (elapsedMs / (durSec * 1000.0)) * 100.0;
          }
        }
        const double target = std::clamp(std::max(static_cast<double>(app->decodeProgressPct), rtPct),
                                         0.0, 100.0);
        app->decodeProgressVisualPct += (target - app->decodeProgressVisualPct) * 0.24;
        if (target - app->decodeProgressVisualPct > 2.5) {
          app->decodeProgressVisualPct += 0.45;
        }
        app->decodeProgressVisualPct = std::clamp(app->decodeProgressVisualPct, 0.0, 100.0);
        InvalidateRect(app->chartPanel, nullptr, TRUE);
      }
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

int RunGuiApplication(HINSTANCE instance, int nCmdShow) {
  INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_PROGRESS_CLASS};
  InitCommonControlsEx(&icc);

  AppState app;

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = instance;
  wc.lpszClassName = L"JNDBMainWindow";
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  if (!RegisterClassExW(&wc)) {
    return 1;
  }

  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"JNDB - Professional NDB Decoder", WS_OVERLAPPED |
                                                                                              WS_CAPTION |
                                                                                              WS_SYSMENU |
                                                                                              WS_MINIMIZEBOX,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1860, 980, nullptr, nullptr, instance,
                              &app);
  if (!hwnd) {
    return 1;
  }

  ShowWindow(hwnd, nCmdShow);
  UpdateWindow(hwnd);

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return static_cast<int>(msg.wParam);
}

#endif

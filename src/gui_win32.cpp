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
constexpr int kIdAgcFloorSlider = 1031;
constexpr int kIdAgcSpanSlider = 1032;
constexpr int kIdAgcGainSlider = 1033;
constexpr int kIdAgcGammaSlider = 1034;
constexpr int kIdAgcAutoCheck = 1035;
constexpr int kIdPeakLockButton = 1036;
constexpr int kIdBookmarkAdd = 1037;
constexpr int kIdBookmarkClear = 1038;
constexpr int kIdBookmarkExport = 1039;
constexpr int kIdBookmarkImport = 1040;
constexpr int kIdAutoBookmarkCheck = 1041;
constexpr int kIdAutoBookmarkConfSlider = 1042;
constexpr int kIdAutoBookmarkMidSlider = 1043;
constexpr int kIdAutoBookmarkHighSlider = 1044;
constexpr int kIdAutoMarkPresetDx = 1045;
constexpr int kIdAutoMarkPresetBalanced = 1046;
constexpr int kIdAutoMarkPresetWeak = 1047;
constexpr int kIdResetUiSession = 1048;
constexpr int kIdPalettePresetCombo = 1049;
constexpr int kIdPaletteLoadLut = 1050;
constexpr int kIdPanAvgAlphaSlider = 1051;
constexpr int kIdPanPeakDecaySlider = 1052;
constexpr int kIdManualNotchCheck = 1053;
constexpr int kIdManualNotchClear = 1054;
constexpr int kIdManualNotchExport = 1055;
constexpr int kIdManualNotchImport = 1056;
constexpr int kIdWaterfallPersistCombo = 1057;
constexpr int kIdWideViewCheck = 1058;
constexpr int kIdRidgeOverlayCheck = 1059;
constexpr int kIdWideViewButton = 1060;
constexpr int kIdRidgeOverlayButton = 1061;
constexpr int kIdLensStrengthSlider = 1062;
constexpr int kIdBgRemovalSlider = 1063;
constexpr int kIdSplitPointSlider = 1064;
constexpr int kIdAutoFocusStrengthSlider = 1065;
constexpr int kIdVisualPresetDxWeak = 1066;
constexpr int kIdVisualPresetBalanced = 1067;
constexpr int kIdVisualPresetClean = 1068;
constexpr int kIdFreezeButton = 1069;
constexpr int kIdQrmPresetNo = 1070;
constexpr int kIdQrmPresetHeavy = 1071;
constexpr int kIdDiffWaterfallCheck = 1072;
constexpr int kIdDotDashAssistCheck = 1073;
constexpr int kIdFftPreviewCombo = 1074;

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

struct ColorStop {
  float p = 0.0f;
  int r = 0;
  int g = 0;
  int b = 0;
};

struct NotchBand {
  float freqHz = 0.0f;
  float widthHz = 24.0f;
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
  HWND palettePresetCombo = nullptr;
  HWND paletteLoadButton = nullptr;
  HWND waterfallFpsCombo = nullptr;
  HWND waterfallPersistCombo = nullptr;
  HWND panAvgAlphaSlider = nullptr;
  HWND panPeakDecaySlider = nullptr;
  HWND lensStrengthSlider = nullptr;
  HWND bgRemovalSlider = nullptr;
  HWND splitPointSlider = nullptr;
  HWND autoFocusStrengthSlider = nullptr;
  HWND agcFloorSlider = nullptr;
  HWND agcSpanSlider = nullptr;
  HWND agcGainSlider = nullptr;
  HWND agcGammaSlider = nullptr;
  HWND agcAutoCheck = nullptr;
  HWND peakLockButton = nullptr;
  HWND manualNotchCheck = nullptr;
  HWND wideViewCheck = nullptr;
  HWND ridgeOverlayCheck = nullptr;
  HWND wideViewButton = nullptr;
  HWND ridgeOverlayButton = nullptr;
  HWND freezeButton = nullptr;
  HWND diffWaterfallCheck = nullptr;
  HWND dotDashAssistCheck = nullptr;
  HWND fftPreviewCombo = nullptr;
  HWND autoBookmarkCheck = nullptr;
  HWND autoBookmarkConfSlider = nullptr;
  HWND autoBookmarkMidSlider = nullptr;
  HWND autoBookmarkHighSlider = nullptr;
  HWND runButton = nullptr;
  HWND progressBar = nullptr;
  HWND statusText = nullptr;
  HWND summaryText = nullptr;
  HWND presetHintText = nullptr;
  HWND resetUiButton = nullptr;
  HWND tooltipWnd = nullptr;
  HWND historyEdit = nullptr;
  HWND compareEdit = nullptr;
  HWND chartPanel = nullptr;
  std::wstring uiStatePath;

  HFONT font = nullptr;
  HFONT fontBig = nullptr;
  HFONT fontMono = nullptr;

  std::atomic<bool> running{false};
  std::thread worker;
  std::vector<HistoryEntry> history;
  std::vector<HistoryEntry> historyCompare;
  ndb::WavData previewWav;
  std::vector<std::uint8_t> waterfallRgb;
  std::vector<std::uint8_t> waterfallDiffRgb;
  std::vector<std::uint8_t> waterfallRidgeMask;
  std::vector<std::uint8_t> waterfallRidgeStrength;
  std::vector<float> waterfallDbRender;
  int waterfallW = 0;
  int waterfallH = 0;
  std::vector<float> panInstantDb;
  std::vector<float> panAvgDb;
  std::vector<float> panSlowDb;
  std::vector<float> panPeakDb;
  float panAvgAlpha = 0.08f;
  float panPeakDecay = 0.12f;
  float lensStrength = 1.0f;
  float bgRemovalStrength = 1.0f;
  float splitTonePoint = 0.58f;
  float qrmBirdieSuppression = 1.0f;
  float qrmRidgeAggressiveness = 1.0f;
  float panMinDb = -120.0f;
  float panMaxDb = -20.0f;
  int panLastCol = -1;
  std::vector<ndb::DecodeResult> overlayRows;
  int decodeProgressPct = 0;
  double decodeProgressVisualPct = 0.0;
  ULONGLONG decodeStartTickMs = 0;
  int waterfallViewMode = 1;  // 0: 2D only, 1: 2D+3D, 2: 3D large
  int yawDeg = 36;
  int pitchDeg = 24;
  bool shadingEnabled = true;
  int colormap3d = 0;
  int palettePreset = 0;  // 0 HDSDR, 1 SDR#, 2 CubicSDR, 3 Custom, 4 Cividis, 5 Viridis
  std::vector<ColorStop> customPalette;
  std::wstring customPalettePath;
  int waterfallFps = 30;
  int waterfallPersistenceMode = 1;  // 0 Fast, 1 Medium, 2 Long
  int previewFftSize = 512;
  bool showWideView = true;
  bool showRidgeOverlay = true;
  bool differenceWaterfallEnabled = false;
  bool dotDashAssistEnabled = true;
  bool autoFocusEnabled = true;
  float autoFocusStrength = 0.28f;
  float agcFloorOffsetDb = -3.0f;
  float agcSpanDb = 22.0f;
  float agcGain = 1.35f;
  float agcGamma = 0.72f;
  bool agcAutoContrast = true;
  bool peakLockEnabled = false;
  int peakLockBin = -1;
  float peakLockHz = 0.0f;
  bool manualNotchEnabled = true;
  std::vector<NotchBand> manualNotches;
  int activeManualNotch = -1;
  bool manualNotchDragging = false;
  std::vector<float> bookmarksSec;
  std::vector<std::uint8_t> bookmarkAuto;
  std::vector<float> bookmarkConfidence;
  bool autoBookmarkEnabled = true;
  float autoBookmarkMinConfidence = 0.65f;
  float autoBookmarkMidThreshold = 0.65f;
  float autoBookmarkHighThreshold = 0.85f;
  bool showAutoBookmarks = true;
  double waterfallZoom = 1.0;
  int waterfallPanPx = 0;
  bool waterfallDragging = false;
  int waterfallDragStartX = 0;
  int waterfallPanStartPx = 0;
  bool waterfallZoomBoxActive = false;
  POINT waterfallZoomBoxStart = {0, 0};
  POINT waterfallZoomBoxEnd = {0, 0};
  bool waterfallFrozen = false;
  int waterfallFreezeCenterCol = -1;
  bool notchGhostActive = false;
  float notchGhostFreqHz = 0.0f;
  float notchGhostWidthHz = 24.0f;
  std::vector<std::uint8_t> snapshotA;
  std::vector<std::uint8_t> snapshotB;
  int snapshotW = 0;
  int snapshotH = 0;
  int snapshotWipePct = 50;
  bool snapshotWipeDragging = false;
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
  bool waterfallReadoutLocked = false;
  bool waterfallReadoutValid = false;
  float waterfallReadoutFreqHz = 0.0f;
  float waterfallReadoutTimeSec = 0.0f;
  float waterfallReadoutDb = -120.0f;
  float waterfallReadoutSnrDb = 0.0f;
  float waterfallReadoutNoiseFloorDb = -120.0f;
  float waterfallReadoutDeltaFreqHz = 0.0f;
  std::vector<float> readoutSnrTrend;
  bool compareCursorBValid = false;
  POINT compareCursorBPt = {0, 0};
  float compareCursorBFreqHz = 0.0f;
  float compareCursorBTimeSec = 0.0f;
  float compareCursorBDb = -120.0f;
  int selectedTrackId = -1;
  bool mouseLeaveArmed = false;
  bool suppressNextResetConfirm = false;
  int baseClientW = 0;
  int baseClientH = 0;
  struct ChildLayout {
    HWND hwnd = nullptr;
    RECT rc = {0, 0, 0, 0};
  };
  std::vector<ChildLayout> childLayouts;
};

void InvalidateWaterfallCache(AppState* app);
void SortAndMergeManualNotches(AppState* app);
void ClampManualNotchesToRange(AppState* app);
void SaveUiState(AppState* app);
void ComputeWaterfallSourceWindow(const AppState* app, int* srcX, int* srcW);

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

bool WriteBookmarksCsv(const std::string& path, const std::vector<float>& bookmarksSec,
                       const std::vector<std::uint8_t>& bookmarkAuto,
                       const std::vector<float>& bookmarkConfidence, std::string* error) {
  std::ofstream out(path);
  if (!out) {
    if (error) {
      *error = "Cannot write bookmarks file: " + path;
    }
    return false;
  }
  out << "index,time_sec,type,confidence\n";
  for (std::size_t i = 0; i < bookmarksSec.size(); ++i) {
    const bool isAuto = (i < bookmarkAuto.size() && bookmarkAuto[i] != 0);
    const float conf = (i < bookmarkConfidence.size()) ? bookmarkConfidence[i] : (isAuto ? 0.70f : 1.00f);
    out << (i + 1) << ',' << std::fixed << std::setprecision(3) << bookmarksSec[i] << ','
        << (isAuto ? "auto" : "manual") << ',' << std::setprecision(3) << conf << '\n';
  }
  return true;
}

bool ReadBookmarksCsv(const std::string& path, std::vector<float>* bookmarksSec,
                      std::vector<std::uint8_t>* bookmarkAuto,
                      std::vector<float>* bookmarkConfidence, std::string* error) {
  if (!bookmarksSec) {
    if (error) {
      *error = "Internal error: bookmarks target is null";
    }
    return false;
  }
  if (!bookmarkAuto) {
    if (error) {
      *error = "Internal error: bookmark auto target is null";
    }
    return false;
  }
  if (!bookmarkConfidence) {
    if (error) {
      *error = "Internal error: bookmark confidence target is null";
    }
    return false;
  }
  std::ifstream in(path);
  if (!in) {
    if (error) {
      *error = "Cannot open bookmarks file: " + path;
    }
    return false;
  }
  std::string line;
  struct Rec {
    float t = 0.0f;
    std::uint8_t autoFlag = 0;
    float conf = 1.0f;
  };
  std::vector<Rec> parsed;
  bool headerSkipped = false;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    if (!headerSkipped) {
      headerSkipped = true;
      if (line.find("time_sec") != std::string::npos) {
        continue;
      }
    }
    const std::size_t c0 = line.find(',');
    const std::size_t c1 = (c0 == std::string::npos) ? std::string::npos : line.find(',', c0 + 1);
    std::string t = (c0 == std::string::npos)
                        ? line
                        : (c1 == std::string::npos ? line.substr(c0 + 1) : line.substr(c0 + 1, c1 - c0 - 1));
    const std::size_t c2 = (c1 == std::string::npos) ? std::string::npos : line.find(',', c1 + 1);
    std::string type = (c1 == std::string::npos)
                           ? "manual"
                           : (c2 == std::string::npos ? line.substr(c1 + 1)
                                                      : line.substr(c1 + 1, c2 - c1 - 1));
    std::string confS = (c2 == std::string::npos) ? "" : line.substr(c2 + 1);
    try {
      float v = std::stof(t);
      if (std::isfinite(v) && v >= 0.0f) {
        Rec r;
        r.t = v;
        r.autoFlag = (type.find("auto") != std::string::npos || type.find("AUTO") != std::string::npos) ? 1 : 0;
        if (!confS.empty()) {
          try {
            r.conf = std::clamp(std::stof(confS), 0.0f, 1.0f);
          } catch (...) {
            r.conf = r.autoFlag ? 0.70f : 1.00f;
          }
        } else {
          r.conf = r.autoFlag ? 0.70f : 1.00f;
        }
        parsed.push_back(r);
      }
    } catch (...) {
    }
  }
  std::sort(parsed.begin(), parsed.end(), [](const Rec& a, const Rec& b) { return a.t < b.t; });
  std::vector<float> tOut;
  std::vector<std::uint8_t> aOut;
  std::vector<float> cOut;
  for (const auto& r : parsed) {
    if (!tOut.empty() && std::fabs(tOut.back() - r.t) <= 0.05f) {
      aOut.back() = static_cast<std::uint8_t>(aOut.back() & r.autoFlag);
      cOut.back() = std::max(cOut.back(), r.conf);
      continue;
    }
    tOut.push_back(r.t);
    aOut.push_back(r.autoFlag);
    cOut.push_back(r.conf);
  }
  *bookmarksSec = std::move(tOut);
  *bookmarkAuto = std::move(aOut);
  *bookmarkConfidence = std::move(cOut);
  return true;
}

bool WriteManualNotchesCsv(const std::string& path, const std::vector<NotchBand>& notches,
                           std::string* error) {
  std::ofstream out(path);
  if (!out) {
    if (error) {
      *error = "Cannot write manual notch CSV: " + path;
    }
    return false;
  }
  out << "freq_hz,width_hz\n";
  for (const auto& n : notches) {
    out << std::fixed << std::setprecision(3) << n.freqHz << ',' << std::setprecision(3)
        << n.widthHz << '\n';
  }
  return true;
}

bool ReadManualNotchesCsv(const std::string& path, std::vector<NotchBand>* outNotches,
                          std::string* error) {
  if (!outNotches) {
    if (error) {
      *error = "Internal error: manual notch target is null";
    }
    return false;
  }
  std::ifstream in(path);
  if (!in) {
    if (error) {
      *error = "Cannot open manual notch CSV: " + path;
    }
    return false;
  }
  std::string line;
  std::vector<NotchBand> parsed;
  bool headerSkipped = false;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    if (!headerSkipped) {
      headerSkipped = true;
      if (line.find("freq") != std::string::npos) {
        continue;
      }
    }
    std::replace(line.begin(), line.end(), ',', ' ');
    std::istringstream iss(line);
    float f = 0.0f;
    float w = 24.0f;
    if (!(iss >> f)) {
      continue;
    }
    if (!(iss >> w)) {
      w = 24.0f;
    }
    if (!std::isfinite(f) || f <= 0.0f) {
      continue;
    }
    NotchBand n;
    n.freqHz = f;
    n.widthHz = std::clamp(w, 6.0f, 200.0f);
    parsed.push_back(n);
  }
  std::sort(parsed.begin(), parsed.end(),
            [](const NotchBand& a, const NotchBand& b) { return a.freqHz < b.freqHz; });
  parsed.erase(std::unique(parsed.begin(), parsed.end(),
                           [](const NotchBand& a, const NotchBand& b) {
                             return std::fabs(a.freqHz - b.freqHz) < 1.0f;
                           }),
               parsed.end());
  *outNotches = std::move(parsed);
  return true;
}

void SetStatus(AppState* app, const std::wstring& s) {
  SetText(app->statusText, s);
}

void SetSummary(AppState* app, const std::wstring& s) {
  SetText(app->summaryText, s);
}

void UpdateWaterfallToggleButtons(AppState* app) {
  if (!app) {
    return;
  }
  if (app->wideViewButton) {
    SetWindowTextW(app->wideViewButton, app->showWideView ? L"WIDE ON" : L"WIDE OFF");
  }
  if (app->ridgeOverlayButton) {
    SetWindowTextW(app->ridgeOverlayButton,
                   app->showRidgeOverlay ? L"RIDGE ON" : L"RIDGE OFF");
  }
}

void UpdateFreezeButton(AppState* app) {
  if (!app || !app->freezeButton) {
    return;
  }
  SetWindowTextW(app->freezeButton, app->waterfallFrozen ? L"FREEZE ON" : L"FREEZE OFF");
}

void ToggleWaterfallFreeze(AppState* app) {
  if (!app) {
    return;
  }
  app->waterfallFrozen = !app->waterfallFrozen;
  if (app->waterfallFrozen) {
    int sx = 0;
    int sw = app->waterfallW;
    ComputeWaterfallSourceWindow(app, &sx, &sw);
    app->waterfallFreezeCenterCol = sx + sw / 2;
  }
  UpdateFreezeButton(app);
  SaveUiState(app);
  SetStatus(app, app->waterfallFrozen ? L"Waterfall freeze ON [F]" : L"Waterfall freeze OFF [F]");
  InvalidateRect(app->chartPanel, nullptr, TRUE);
}

float IniReadFloat(const std::wstring& path, const wchar_t* section, const wchar_t* key, float defVal) {
  wchar_t buf[64] = {};
  GetPrivateProfileStringW(section, key, L"", buf, 64, path.c_str());
  if (buf[0] == 0) return defVal;
  try {
    return std::stof(std::wstring(buf));
  } catch (...) {
    return defVal;
  }
}

bool IniReadBool(const std::wstring& path, const wchar_t* section, const wchar_t* key, bool defVal) {
  const UINT v = GetPrivateProfileIntW(section, key, defVal ? 1 : 0, path.c_str());
  return v != 0;
}

int IniReadInt(const std::wstring& path, const wchar_t* section, const wchar_t* key, int defVal) {
  return static_cast<int>(GetPrivateProfileIntW(section, key, defVal, path.c_str()));
}

void SaveUiState(AppState* app) {
  if (!app || app->uiStatePath.empty()) {
    return;
  }
  const auto s = app->uiStatePath.c_str();
  WritePrivateProfileStringW(L"bookmarks", L"auto_enabled", app->autoBookmarkEnabled ? L"1" : L"0", s);
  WritePrivateProfileStringW(L"bookmarks", L"show_auto", app->showAutoBookmarks ? L"1" : L"0", s);

  auto saveFloat = [&](const wchar_t* section, const wchar_t* k, float v) {
    wchar_t b[32] = {};
    swprintf(b, 32, L"%.3f", v);
    WritePrivateProfileStringW(section, k, b, s);
  };
  saveFloat(L"bookmarks", L"min_conf", app->autoBookmarkMinConfidence);
  saveFloat(L"bookmarks", L"mid_thr", app->autoBookmarkMidThreshold);
  saveFloat(L"bookmarks", L"high_thr", app->autoBookmarkHighThreshold);

  WritePrivateProfileStringW(L"view", L"mode", std::to_wstring(app->waterfallViewMode).c_str(), s);
  WritePrivateProfileStringW(L"view", L"yaw", std::to_wstring(app->yawDeg).c_str(), s);
  WritePrivateProfileStringW(L"view", L"pitch", std::to_wstring(app->pitchDeg).c_str(), s);
  WritePrivateProfileStringW(L"view", L"fps", std::to_wstring(app->waterfallFps).c_str(), s);
  WritePrivateProfileStringW(L"view", L"persist_mode", std::to_wstring(app->waterfallPersistenceMode).c_str(), s);
  WritePrivateProfileStringW(L"view", L"preview_fft", std::to_wstring(app->previewFftSize).c_str(), s);
  WritePrivateProfileStringW(L"view", L"show_wide", app->showWideView ? L"1" : L"0", s);
  WritePrivateProfileStringW(L"view", L"show_ridge", app->showRidgeOverlay ? L"1" : L"0", s);
  WritePrivateProfileStringW(L"view", L"diff_waterfall", app->differenceWaterfallEnabled ? L"1" : L"0", s);
  WritePrivateProfileStringW(L"view", L"dotdash_assist", app->dotDashAssistEnabled ? L"1" : L"0", s);
  WritePrivateProfileStringW(L"view", L"frozen", app->waterfallFrozen ? L"1" : L"0", s);
  WritePrivateProfileStringW(L"view", L"freeze_col", std::to_wstring(app->waterfallFreezeCenterCol).c_str(), s);
  WritePrivateProfileStringW(L"view", L"zoom", std::to_wstring(app->waterfallZoom).c_str(), s);
  WritePrivateProfileStringW(L"view", L"pan", std::to_wstring(app->waterfallPanPx).c_str(), s);
  WritePrivateProfileStringW(L"view", L"shading", app->shadingEnabled ? L"1" : L"0", s);
  WritePrivateProfileStringW(L"view", L"colormap3d", std::to_wstring(app->colormap3d).c_str(), s);
  WritePrivateProfileStringW(L"view", L"palette_preset", std::to_wstring(app->palettePreset).c_str(), s);
  WritePrivateProfileStringW(L"view", L"peak_lock", app->peakLockEnabled ? L"1" : L"0", s);
  WritePrivateProfileStringW(L"view", L"manual_notch", app->manualNotchEnabled ? L"1" : L"0", s);

  WritePrivateProfileStringW(L"charts", L"zoom", std::to_wstring(app->chartZoom).c_str(), s);
  WritePrivateProfileStringW(L"charts", L"pan", std::to_wstring(app->chartPanPx).c_str(), s);

  WritePrivateProfileStringW(L"agc", L"auto", app->agcAutoContrast ? L"1" : L"0", s);
  saveFloat(L"agc", L"floor", app->agcFloorOffsetDb);
  saveFloat(L"agc", L"span", app->agcSpanDb);
  saveFloat(L"agc", L"gain", app->agcGain);
  saveFloat(L"agc", L"gamma", app->agcGamma);
  saveFloat(L"pan", L"avg_alpha", app->panAvgAlpha);
  saveFloat(L"pan", L"peak_decay", app->panPeakDecay);
  saveFloat(L"visual", L"lens_strength", app->lensStrength);
  saveFloat(L"visual", L"bg_remove", app->bgRemovalStrength);
  saveFloat(L"visual", L"split_point", app->splitTonePoint);
  saveFloat(L"visual", L"autofocus_strength", app->autoFocusStrength);
  saveFloat(L"visual", L"qrm_birdie", app->qrmBirdieSuppression);
  saveFloat(L"visual", L"qrm_ridge", app->qrmRidgeAggressiveness);

  const int maxNotchStore = 48;
  const int nCount = std::min(static_cast<int>(app->manualNotches.size()), maxNotchStore);
  WritePrivateProfileStringW(L"manual_notch", L"count", std::to_wstring(nCount).c_str(), s);
  for (int i = 0; i < maxNotchStore; ++i) {
    wchar_t kf[24] = {};
    wchar_t kw[24] = {};
    swprintf(kf, 24, L"f_%02d", i);
    swprintf(kw, 24, L"w_%02d", i);
    if (i < nCount) {
      saveFloat(L"manual_notch", kf, app->manualNotches[static_cast<std::size_t>(i)].freqHz);
      saveFloat(L"manual_notch", kw, app->manualNotches[static_cast<std::size_t>(i)].widthHz);
    } else {
      WritePrivateProfileStringW(L"manual_notch", kf, nullptr, s);
      WritePrivateProfileStringW(L"manual_notch", kw, nullptr, s);
    }
  }
}

void LoadUiState(AppState* app) {
  if (!app || app->uiStatePath.empty()) {
    return;
  }
  app->autoBookmarkEnabled = IniReadBool(app->uiStatePath, L"bookmarks", L"auto_enabled", app->autoBookmarkEnabled);
  app->showAutoBookmarks = IniReadBool(app->uiStatePath, L"bookmarks", L"show_auto", app->showAutoBookmarks);
  app->autoBookmarkMinConfidence = std::clamp(
      IniReadFloat(app->uiStatePath, L"bookmarks", L"min_conf", app->autoBookmarkMinConfidence), 0.30f, 0.95f);
  app->autoBookmarkMidThreshold = std::clamp(
      IniReadFloat(app->uiStatePath, L"bookmarks", L"mid_thr", app->autoBookmarkMidThreshold), 0.40f, 0.95f);
  app->autoBookmarkHighThreshold = std::clamp(
      IniReadFloat(app->uiStatePath, L"bookmarks", L"high_thr", app->autoBookmarkHighThreshold), 0.45f, 0.98f);
  if (app->autoBookmarkHighThreshold <= app->autoBookmarkMidThreshold) {
    app->autoBookmarkHighThreshold = std::min(0.98f, app->autoBookmarkMidThreshold + 0.01f);
  }

  app->waterfallViewMode = std::clamp(IniReadInt(app->uiStatePath, L"view", L"mode", app->waterfallViewMode), 0, 2);
  app->yawDeg = std::clamp(IniReadInt(app->uiStatePath, L"view", L"yaw", app->yawDeg), 10, 75);
  app->pitchDeg = std::clamp(IniReadInt(app->uiStatePath, L"view", L"pitch", app->pitchDeg), 8, 60);
  app->waterfallFps = std::clamp(IniReadInt(app->uiStatePath, L"view", L"fps", app->waterfallFps), 10, 120);
  app->waterfallPersistenceMode = std::clamp(IniReadInt(app->uiStatePath, L"view", L"persist_mode", app->waterfallPersistenceMode), 0, 2);
  app->previewFftSize = std::clamp(IniReadInt(app->uiStatePath, L"view", L"preview_fft", app->previewFftSize), 256, 1024);
  app->showWideView = IniReadBool(app->uiStatePath, L"view", L"show_wide", app->showWideView);
  app->showRidgeOverlay = IniReadBool(app->uiStatePath, L"view", L"show_ridge", app->showRidgeOverlay);
  app->differenceWaterfallEnabled = IniReadBool(app->uiStatePath, L"view", L"diff_waterfall", app->differenceWaterfallEnabled);
  app->dotDashAssistEnabled = IniReadBool(app->uiStatePath, L"view", L"dotdash_assist", app->dotDashAssistEnabled);
  app->waterfallFrozen = IniReadBool(app->uiStatePath, L"view", L"frozen", app->waterfallFrozen);
  app->waterfallFreezeCenterCol = IniReadInt(app->uiStatePath, L"view", L"freeze_col", app->waterfallFreezeCenterCol);
  app->waterfallZoom = std::clamp(static_cast<double>(IniReadFloat(app->uiStatePath, L"view", L"zoom", static_cast<float>(app->waterfallZoom))), 1.0, 8.0);
  app->waterfallPanPx = std::max(0, IniReadInt(app->uiStatePath, L"view", L"pan", app->waterfallPanPx));
  app->shadingEnabled = IniReadBool(app->uiStatePath, L"view", L"shading", app->shadingEnabled);
  app->colormap3d = std::clamp(IniReadInt(app->uiStatePath, L"view", L"colormap3d", app->colormap3d), 0, 2);
  app->palettePreset = std::clamp(IniReadInt(app->uiStatePath, L"view", L"palette_preset", app->palettePreset), 0, 5);
  app->peakLockEnabled = IniReadBool(app->uiStatePath, L"view", L"peak_lock", app->peakLockEnabled);
  app->manualNotchEnabled = IniReadBool(app->uiStatePath, L"view", L"manual_notch", app->manualNotchEnabled);
  app->chartZoom = std::clamp(static_cast<double>(IniReadFloat(app->uiStatePath, L"charts", L"zoom", static_cast<float>(app->chartZoom))), 1.0, 8.0);
  app->chartPanPx = IniReadInt(app->uiStatePath, L"charts", L"pan", app->chartPanPx);

  app->agcAutoContrast = IniReadBool(app->uiStatePath, L"agc", L"auto", app->agcAutoContrast);
  app->agcFloorOffsetDb = std::clamp(IniReadFloat(app->uiStatePath, L"agc", L"floor", app->agcFloorOffsetDb), -30.0f, 30.0f);
  app->agcSpanDb = std::clamp(IniReadFloat(app->uiStatePath, L"agc", L"span", app->agcSpanDb), 8.0f, 80.0f);
  app->agcGain = std::clamp(IniReadFloat(app->uiStatePath, L"agc", L"gain", app->agcGain), 0.50f, 2.50f);
  app->agcGamma = std::clamp(IniReadFloat(app->uiStatePath, L"agc", L"gamma", app->agcGamma), 0.40f, 1.60f);
  app->panAvgAlpha = std::clamp(IniReadFloat(app->uiStatePath, L"pan", L"avg_alpha", app->panAvgAlpha), 0.01f, 0.40f);
  app->panPeakDecay = std::clamp(IniReadFloat(app->uiStatePath, L"pan", L"peak_decay", app->panPeakDecay), 0.01f, 1.20f);
  app->lensStrength = std::clamp(IniReadFloat(app->uiStatePath, L"visual", L"lens_strength", app->lensStrength), 0.50f, 2.00f);
  app->bgRemovalStrength = std::clamp(IniReadFloat(app->uiStatePath, L"visual", L"bg_remove", app->bgRemovalStrength), 0.00f, 1.60f);
  app->splitTonePoint = std::clamp(IniReadFloat(app->uiStatePath, L"visual", L"split_point", app->splitTonePoint), 0.35f, 0.80f);
  app->autoFocusStrength = std::clamp(IniReadFloat(app->uiStatePath, L"visual", L"autofocus_strength", app->autoFocusStrength), 0.0f, 1.0f);
  app->qrmBirdieSuppression = std::clamp(IniReadFloat(app->uiStatePath, L"visual", L"qrm_birdie", app->qrmBirdieSuppression), 0.5f, 2.0f);
  app->qrmRidgeAggressiveness = std::clamp(IniReadFloat(app->uiStatePath, L"visual", L"qrm_ridge", app->qrmRidgeAggressiveness), 0.5f, 2.0f);

  app->manualNotches.clear();
  const int notchCount = std::clamp(IniReadInt(app->uiStatePath, L"manual_notch", L"count", 0), 0, 48);
  for (int i = 0; i < notchCount; ++i) {
    wchar_t kf[24] = {};
    wchar_t kw[24] = {};
    swprintf(kf, 24, L"f_%02d", i);
    swprintf(kw, 24, L"w_%02d", i);
    NotchBand n;
    n.freqHz = IniReadFloat(app->uiStatePath, L"manual_notch", kf, 0.0f);
    n.widthHz = IniReadFloat(app->uiStatePath, L"manual_notch", kw, 24.0f);
    if (n.freqHz > 0.0f) {
      app->manualNotches.push_back(n);
    }
  }
  ClampManualNotchesToRange(app);
  SortAndMergeManualNotches(app);
  app->activeManualNotch = app->manualNotches.empty() ? -1 : 0;

  if (app->autoBookmarkCheck) {
    SendMessageW(app->autoBookmarkCheck, BM_SETCHECK,
                 app->autoBookmarkEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
  }
  if (app->autoBookmarkConfSlider) {
    SendMessageW(app->autoBookmarkConfSlider, TBM_SETPOS, TRUE,
                 static_cast<LPARAM>(std::round(app->autoBookmarkMinConfidence * 100.0f)));
  }
  if (app->autoBookmarkMidSlider) {
    SendMessageW(app->autoBookmarkMidSlider, TBM_SETPOS, TRUE,
                 static_cast<LPARAM>(std::round(app->autoBookmarkMidThreshold * 100.0f)));
  }
  if (app->autoBookmarkHighSlider) {
    SendMessageW(app->autoBookmarkHighSlider, TBM_SETPOS, TRUE,
                 static_cast<LPARAM>(std::round(app->autoBookmarkHighThreshold * 100.0f)));
  }
  if (app->waterfallViewCombo) {
    SendMessageW(app->waterfallViewCombo, CB_SETCURSEL, app->waterfallViewMode, 0);
  }
  if (app->palettePresetCombo) {
    SendMessageW(app->palettePresetCombo, CB_SETCURSEL, app->palettePreset, 0);
  }
  if (app->yawSlider) {
    SendMessageW(app->yawSlider, TBM_SETPOS, TRUE, app->yawDeg);
  }
  if (app->pitchSlider) {
    SendMessageW(app->pitchSlider, TBM_SETPOS, TRUE, app->pitchDeg);
  }
  if (app->waterfallFpsCombo) {
    SendMessageW(app->waterfallFpsCombo, CB_SETCURSEL, app->waterfallFps >= 60 ? 1 : 0, 0);
  }
  if (app->waterfallPersistCombo) {
    SendMessageW(app->waterfallPersistCombo, CB_SETCURSEL, app->waterfallPersistenceMode, 0);
  }
  if (app->fftPreviewCombo) {
    const int fsel = (app->previewFftSize >= 1024) ? 2 : ((app->previewFftSize <= 256) ? 0 : 1);
    SendMessageW(app->fftPreviewCombo, CB_SETCURSEL, fsel, 0);
  }
  if (app->wideViewCheck) {
    SendMessageW(app->wideViewCheck, BM_SETCHECK, app->showWideView ? BST_CHECKED : BST_UNCHECKED, 0);
  }
  if (app->ridgeOverlayCheck) {
    SendMessageW(app->ridgeOverlayCheck, BM_SETCHECK,
                 app->showRidgeOverlay ? BST_CHECKED : BST_UNCHECKED, 0);
  }
  if (app->diffWaterfallCheck) {
    SendMessageW(app->diffWaterfallCheck, BM_SETCHECK,
                 app->differenceWaterfallEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
  }
  if (app->dotDashAssistCheck) {
    SendMessageW(app->dotDashAssistCheck, BM_SETCHECK,
                 app->dotDashAssistEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
  }
  UpdateWaterfallToggleButtons(app);
  UpdateFreezeButton(app);
  if (app->shadingCheck) {
    SendMessageW(app->shadingCheck, BM_SETCHECK,
                 app->shadingEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
  }
  if (app->colormapCombo) {
    SendMessageW(app->colormapCombo, CB_SETCURSEL, app->colormap3d, 0);
  }
  if (app->peakLockButton) {
    SetWindowTextW(app->peakLockButton, app->peakLockEnabled ? L"Peak Lock: ON" : L"Peak Lock: OFF");
  }
  if (app->manualNotchCheck) {
    SendMessageW(app->manualNotchCheck, BM_SETCHECK,
                 app->manualNotchEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
  }
  if (app->agcFloorSlider) {
    SendMessageW(app->agcFloorSlider, TBM_SETPOS, TRUE, static_cast<LPARAM>(app->agcFloorOffsetDb));
  }
  if (app->agcSpanSlider) {
    SendMessageW(app->agcSpanSlider, TBM_SETPOS, TRUE, static_cast<LPARAM>(app->agcSpanDb));
  }
  if (app->agcGainSlider) {
    SendMessageW(app->agcGainSlider, TBM_SETPOS, TRUE,
                 static_cast<LPARAM>(std::round(app->agcGain * 100.0f)));
  }
  if (app->agcGammaSlider) {
    SendMessageW(app->agcGammaSlider, TBM_SETPOS, TRUE,
                 static_cast<LPARAM>(std::round(app->agcGamma * 100.0f)));
  }
  if (app->panAvgAlphaSlider) {
    SendMessageW(app->panAvgAlphaSlider, TBM_SETPOS, TRUE,
                 static_cast<LPARAM>(std::round(app->panAvgAlpha * 100.0f)));
  }
  if (app->panPeakDecaySlider) {
    SendMessageW(app->panPeakDecaySlider, TBM_SETPOS, TRUE,
                 static_cast<LPARAM>(std::round(app->panPeakDecay * 100.0f)));
  }
  if (app->lensStrengthSlider) {
    SendMessageW(app->lensStrengthSlider, TBM_SETPOS, TRUE,
                 static_cast<LPARAM>(std::round(app->lensStrength * 100.0f)));
  }
  if (app->bgRemovalSlider) {
    SendMessageW(app->bgRemovalSlider, TBM_SETPOS, TRUE,
                 static_cast<LPARAM>(std::round(app->bgRemovalStrength * 100.0f)));
  }
  if (app->splitPointSlider) {
    SendMessageW(app->splitPointSlider, TBM_SETPOS, TRUE,
                 static_cast<LPARAM>(std::round(app->splitTonePoint * 100.0f)));
  }
  if (app->autoFocusStrengthSlider) {
    SendMessageW(app->autoFocusStrengthSlider, TBM_SETPOS, TRUE,
                 static_cast<LPARAM>(std::round(app->autoFocusStrength * 100.0f)));
  }
  if (app->agcAutoCheck) {
    SendMessageW(app->agcAutoCheck, BM_SETCHECK,
                 app->agcAutoContrast ? BST_CHECKED : BST_UNCHECKED, 0);
  }
}

void CaptureBaseChildLayout(AppState* app) {
  if (!app || !app->hwnd) return;
  RECT rcClient = {};
  GetClientRect(app->hwnd, &rcClient);
  app->baseClientW = std::max(1, static_cast<int>(rcClient.right - rcClient.left));
  app->baseClientH = std::max(1, static_cast<int>(rcClient.bottom - rcClient.top));
  app->childLayouts.clear();

  HWND child = GetWindow(app->hwnd, GW_CHILD);
  while (child) {
    RECT r = {};
    GetWindowRect(child, &r);
    POINT tl = {r.left, r.top};
    POINT br = {r.right, r.bottom};
    ScreenToClient(app->hwnd, &tl);
    ScreenToClient(app->hwnd, &br);
    app->childLayouts.push_back(AppState::ChildLayout{child, RECT{tl.x, tl.y, br.x, br.y}});
    child = GetWindow(child, GW_HWNDNEXT);
  }
}

void ApplyResponsiveLayout(AppState* app) {
  if (!app || !app->hwnd || app->baseClientW <= 0 || app->baseClientH <= 0 || app->childLayouts.empty()) {
    return;
  }
  RECT rcClient = {};
  GetClientRect(app->hwnd, &rcClient);
  const int cw = std::max(1, static_cast<int>(rcClient.right - rcClient.left));
  const int ch = std::max(1, static_cast<int>(rcClient.bottom - rcClient.top));
  const double sx = static_cast<double>(cw) / static_cast<double>(app->baseClientW);
  const double sy = static_cast<double>(ch) / static_cast<double>(app->baseClientH);

  for (const auto& c : app->childLayouts) {
    if (!IsWindow(c.hwnd)) continue;
    const int x = static_cast<int>(std::round(c.rc.left * sx));
    const int y = static_cast<int>(std::round(c.rc.top * sy));
    const int w = std::max(24, static_cast<int>(std::round((c.rc.right - c.rc.left) * sx)));
    const int h = std::max(20, static_cast<int>(std::round((c.rc.bottom - c.rc.top) * sy)));
    MoveWindow(c.hwnd, x, y, w, h, TRUE);
  }
}

void ApplyAutoMarkPreset(AppState* app, float minConf, float midThr, float highThr,
                         const wchar_t* name) {
  if (!app) {
    return;
  }
  app->autoBookmarkMinConfidence = std::clamp(minConf, 0.30f, 0.95f);
  app->autoBookmarkMidThreshold = std::clamp(midThr, 0.40f, 0.95f);
  app->autoBookmarkHighThreshold = std::clamp(highThr, 0.45f, 0.98f);
  if (app->autoBookmarkHighThreshold <= app->autoBookmarkMidThreshold) {
    app->autoBookmarkHighThreshold = std::min(0.98f, app->autoBookmarkMidThreshold + 0.01f);
  }

  if (app->autoBookmarkConfSlider) {
    SendMessageW(app->autoBookmarkConfSlider, TBM_SETPOS, TRUE,
                 static_cast<LPARAM>(std::round(app->autoBookmarkMinConfidence * 100.0f)));
  }
  if (app->autoBookmarkMidSlider) {
    SendMessageW(app->autoBookmarkMidSlider, TBM_SETPOS, TRUE,
                 static_cast<LPARAM>(std::round(app->autoBookmarkMidThreshold * 100.0f)));
  }
  if (app->autoBookmarkHighSlider) {
    SendMessageW(app->autoBookmarkHighSlider, TBM_SETPOS, TRUE,
                 static_cast<LPARAM>(std::round(app->autoBookmarkHighThreshold * 100.0f)));
  }

  std::wstringstream ss;
  ss << L"AutoMark preset " << name << L": min/mid/high " << std::fixed << std::setprecision(2)
     << app->autoBookmarkMinConfidence << L"/" << app->autoBookmarkMidThreshold << L"/"
     << app->autoBookmarkHighThreshold;
  SetStatus(app, ss.str());
  InvalidateRect(app->chartPanel, nullptr, TRUE);
}

void ApplyVisualTuningPreset(AppState* app, float lens, float bg, float split,
                             float autoFocus, const wchar_t* name) {
  if (!app) {
    return;
  }
  app->lensStrength = std::clamp(lens, 0.50f, 2.00f);
  app->bgRemovalStrength = std::clamp(bg, 0.00f, 1.60f);
  app->splitTonePoint = std::clamp(split, 0.35f, 0.80f);
  app->autoFocusStrength = std::clamp(autoFocus, 0.0f, 1.0f);

  if (app->lensStrengthSlider) {
    SendMessageW(app->lensStrengthSlider, TBM_SETPOS, TRUE,
                 static_cast<LPARAM>(std::round(app->lensStrength * 100.0f)));
  }
  if (app->bgRemovalSlider) {
    SendMessageW(app->bgRemovalSlider, TBM_SETPOS, TRUE,
                 static_cast<LPARAM>(std::round(app->bgRemovalStrength * 100.0f)));
  }
  if (app->splitPointSlider) {
    SendMessageW(app->splitPointSlider, TBM_SETPOS, TRUE,
                 static_cast<LPARAM>(std::round(app->splitTonePoint * 100.0f)));
  }
  if (app->autoFocusStrengthSlider) {
    SendMessageW(app->autoFocusStrengthSlider, TBM_SETPOS, TRUE,
                 static_cast<LPARAM>(std::round(app->autoFocusStrength * 100.0f)));
  }

  SaveUiState(app);
  InvalidateWaterfallCache(app);
  std::wstringstream ss;
  ss << L"Visual preset " << name << L": lens/bg/split/AF " << std::fixed
     << std::setprecision(2) << app->lensStrength << L"/" << app->bgRemovalStrength << L"/"
     << app->splitTonePoint << L"/" << app->autoFocusStrength;
  SetStatus(app, ss.str());
  InvalidateRect(app->chartPanel, nullptr, TRUE);
}

void ResetUiSessionState(AppState* app) {
  if (!app) {
    return;
  }
  app->waterfallViewMode = 1;
  app->yawDeg = 36;
  app->pitchDeg = 24;
  app->shadingEnabled = true;
  app->colormap3d = 0;
  app->palettePreset = 0;
  app->waterfallFps = 30;
  app->waterfallPersistenceMode = 1;
  app->previewFftSize = 512;
  app->showWideView = true;
  app->showRidgeOverlay = true;
  app->differenceWaterfallEnabled = false;
  app->dotDashAssistEnabled = true;
  app->waterfallZoom = 1.0;
  app->waterfallPanPx = 0;
  app->waterfallFrozen = false;
  app->waterfallFreezeCenterCol = -1;
  app->notchGhostActive = false;
  app->compareCursorBValid = false;
  app->selectedTrackId = -1;
  app->snapshotA.clear();
  app->snapshotB.clear();
  app->chartZoom = 1.0;
  app->chartPanPx = 0;
  app->agcFloorOffsetDb = -3.0f;
  app->agcSpanDb = 22.0f;
  app->agcGain = 1.35f;
  app->agcGamma = 0.72f;
  app->lensStrength = 1.0f;
  app->bgRemovalStrength = 1.0f;
  app->splitTonePoint = 0.58f;
  app->qrmBirdieSuppression = 1.0f;
  app->qrmRidgeAggressiveness = 1.0f;
  app->panAvgAlpha = 0.08f;
  app->panPeakDecay = 0.12f;
  app->agcAutoContrast = true;
  app->peakLockEnabled = false;
  app->peakLockBin = -1;
  app->peakLockHz = 0.0f;
  app->manualNotchEnabled = true;
  app->manualNotches.clear();
  app->activeManualNotch = -1;
  app->manualNotchDragging = false;
  app->autoBookmarkEnabled = true;
  app->autoBookmarkMinConfidence = 0.65f;
  app->autoBookmarkMidThreshold = 0.65f;
  app->autoBookmarkHighThreshold = 0.85f;
  app->showAutoBookmarks = true;
  app->autoFocusStrength = 0.28f;
  app->readoutSnrTrend.clear();

  if (app->waterfallViewCombo) SendMessageW(app->waterfallViewCombo, CB_SETCURSEL, app->waterfallViewMode, 0);
  if (app->yawSlider) SendMessageW(app->yawSlider, TBM_SETPOS, TRUE, app->yawDeg);
  if (app->pitchSlider) SendMessageW(app->pitchSlider, TBM_SETPOS, TRUE, app->pitchDeg);
  if (app->shadingCheck) SendMessageW(app->shadingCheck, BM_SETCHECK, BST_CHECKED, 0);
  if (app->colormapCombo) SendMessageW(app->colormapCombo, CB_SETCURSEL, app->colormap3d, 0);
  if (app->palettePresetCombo) SendMessageW(app->palettePresetCombo, CB_SETCURSEL, app->palettePreset, 0);
  if (app->waterfallFpsCombo) SendMessageW(app->waterfallFpsCombo, CB_SETCURSEL, 0, 0);
  if (app->waterfallPersistCombo) SendMessageW(app->waterfallPersistCombo, CB_SETCURSEL, app->waterfallPersistenceMode, 0);
  if (app->fftPreviewCombo) SendMessageW(app->fftPreviewCombo, CB_SETCURSEL, 1, 0);
  if (app->wideViewCheck) SendMessageW(app->wideViewCheck, BM_SETCHECK, BST_CHECKED, 0);
  if (app->ridgeOverlayCheck) SendMessageW(app->ridgeOverlayCheck, BM_SETCHECK, BST_CHECKED, 0);
  if (app->diffWaterfallCheck) SendMessageW(app->diffWaterfallCheck, BM_SETCHECK, BST_UNCHECKED, 0);
  if (app->dotDashAssistCheck) SendMessageW(app->dotDashAssistCheck, BM_SETCHECK, BST_CHECKED, 0);
  UpdateWaterfallToggleButtons(app);
  UpdateFreezeButton(app);
  if (app->agcFloorSlider) SendMessageW(app->agcFloorSlider, TBM_SETPOS, TRUE, static_cast<LPARAM>(app->agcFloorOffsetDb));
  if (app->agcSpanSlider) SendMessageW(app->agcSpanSlider, TBM_SETPOS, TRUE, static_cast<LPARAM>(app->agcSpanDb));
  if (app->agcGainSlider) SendMessageW(app->agcGainSlider, TBM_SETPOS, TRUE, static_cast<LPARAM>(std::round(app->agcGain * 100.0f)));
  if (app->agcGammaSlider) SendMessageW(app->agcGammaSlider, TBM_SETPOS, TRUE, static_cast<LPARAM>(std::round(app->agcGamma * 100.0f)));
  if (app->panAvgAlphaSlider) SendMessageW(app->panAvgAlphaSlider, TBM_SETPOS, TRUE, static_cast<LPARAM>(std::round(app->panAvgAlpha * 100.0f)));
  if (app->panPeakDecaySlider) SendMessageW(app->panPeakDecaySlider, TBM_SETPOS, TRUE, static_cast<LPARAM>(std::round(app->panPeakDecay * 100.0f)));
  if (app->lensStrengthSlider) SendMessageW(app->lensStrengthSlider, TBM_SETPOS, TRUE, static_cast<LPARAM>(std::round(app->lensStrength * 100.0f)));
  if (app->bgRemovalSlider) SendMessageW(app->bgRemovalSlider, TBM_SETPOS, TRUE, static_cast<LPARAM>(std::round(app->bgRemovalStrength * 100.0f)));
  if (app->splitPointSlider) SendMessageW(app->splitPointSlider, TBM_SETPOS, TRUE, static_cast<LPARAM>(std::round(app->splitTonePoint * 100.0f)));
  if (app->autoFocusStrengthSlider) SendMessageW(app->autoFocusStrengthSlider, TBM_SETPOS, TRUE, static_cast<LPARAM>(std::round(app->autoFocusStrength * 100.0f)));
  if (app->agcAutoCheck) SendMessageW(app->agcAutoCheck, BM_SETCHECK, BST_CHECKED, 0);
  if (app->peakLockButton) SetWindowTextW(app->peakLockButton, L"Peak Lock: OFF");
  if (app->manualNotchCheck) SendMessageW(app->manualNotchCheck, BM_SETCHECK, BST_CHECKED, 0);
  if (app->autoBookmarkCheck) SendMessageW(app->autoBookmarkCheck, BM_SETCHECK, BST_CHECKED, 0);
  if (app->autoBookmarkConfSlider) SendMessageW(app->autoBookmarkConfSlider, TBM_SETPOS, TRUE, 65);
  if (app->autoBookmarkMidSlider) SendMessageW(app->autoBookmarkMidSlider, TBM_SETPOS, TRUE, 65);
  if (app->autoBookmarkHighSlider) SendMessageW(app->autoBookmarkHighSlider, TBM_SETPOS, TRUE, 85);

  if (!app->uiStatePath.empty()) {
    DeleteFileW(app->uiStatePath.c_str());
  }
  SaveUiState(app);
  InvalidateWaterfallCache(app);
  InvalidateRect(app->chartPanel, nullptr, TRUE);
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

std::vector<ColorStop> PaletteStopsByPreset(const AppState* app) {
  const int preset = app ? app->palettePreset : 0;
  if (preset == 1) {
    return {
        {0.00f, 0, 0, 0},   {0.15f, 8, 12, 64},  {0.32f, 12, 60, 168},
        {0.48f, 16, 138, 168}, {0.62f, 26, 176, 98}, {0.78f, 220, 184, 30},
        {0.90f, 245, 84, 24}, {1.00f, 255, 248, 232},
    };
  }
  if (preset == 2) {
    return {
        {0.00f, 0, 0, 0},   {0.12f, 20, 12, 40}, {0.28f, 52, 42, 128},
        {0.44f, 84, 96, 206}, {0.58f, 54, 154, 176}, {0.74f, 92, 196, 94},
        {0.88f, 230, 170, 44}, {1.00f, 255, 250, 236},
    };
  }
  if (preset == 3 && app && !app->customPalette.empty()) {
    return app->customPalette;
  }
  if (preset == 4) {
    return {
        {0.00f, 0, 34, 78},   {0.16f, 34, 63, 112}, {0.32f, 65, 91, 126},
        {0.50f, 96, 118, 129}, {0.68f, 132, 146, 123}, {0.84f, 182, 176, 105},
        {1.00f, 252, 231, 92},
    };
  }
  if (preset == 5) {
    return {
        {0.00f, 68, 1, 84},   {0.16f, 59, 82, 139}, {0.32f, 33, 145, 140},
        {0.50f, 94, 201, 98}, {0.68f, 170, 220, 50}, {0.84f, 219, 227, 39},
        {1.00f, 253, 231, 37},
    };
  }
  return {
      {0.00f, 0, 0, 0},       {0.12f, 0, 10, 50},   {0.24f, 0, 45, 140},
      {0.36f, 0, 120, 190},   {0.50f, 0, 175, 90},  {0.66f, 220, 220, 0},
      {0.82f, 240, 110, 0},   {0.93f, 255, 40, 20}, {1.00f, 255, 245, 230},
  };
}

COLORREF SamplePalette(const std::vector<ColorStop>& stops, float x) {
  if (stops.empty()) return RGB(0, 0, 0);
  if (x <= stops.front().p) return RGB(stops.front().r, stops.front().g, stops.front().b);
  if (x >= stops.back().p) return RGB(stops.back().r, stops.back().g, stops.back().b);
  for (std::size_t i = 1; i < stops.size(); ++i) {
    if (x <= stops[i].p) {
      const auto& a = stops[i - 1];
      const auto& b = stops[i];
      const float t = (x - a.p) / std::max(1e-6f, (b.p - a.p));
      const int r = static_cast<int>(std::round(a.r + t * (b.r - a.r)));
      const int g = static_cast<int>(std::round(a.g + t * (b.g - a.g)));
      const int bb = static_cast<int>(std::round(a.b + t * (b.b - a.b)));
      return RGB(r, g, bb);
    }
  }
  return RGB(stops.back().r, stops.back().g, stops.back().b);
}

std::wstring FormatFreqSmart(float fHz) {
  if (fHz >= 1000.0f) {
    std::wstringstream ss;
    const float fk = fHz / 1000.0f;
    const int decimals = fk < 10.0f ? 3 : (fk < 100.0f ? 2 : 1);
    ss << std::fixed << std::setprecision(decimals) << fk << L" kHz";
    return ss.str();
  }
  std::wstringstream ss;
  ss << std::fixed << std::setprecision(0) << fHz << L" Hz";
  return ss.str();
}

bool LoadCustomPaletteLut(const std::wstring& path, std::vector<ColorStop>* outStops,
                         std::string* error) {
  if (!outStops) {
    if (error) *error = "Internal error: null stops";
    return false;
  }
  std::ifstream in(ToUtf8(path));
  if (!in) {
    if (error) *error = "Cannot open LUT file";
    return false;
  }
  std::vector<ColorStop> stops;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::replace(line.begin(), line.end(), ',', ' ');
    std::istringstream iss(line);
    float p = 0.0f;
    int r = 0, g = 0, b = 0;
    if (!(iss >> p >> r >> g >> b)) continue;
    ColorStop s;
    s.p = std::clamp(p, 0.0f, 1.0f);
    s.r = std::clamp(r, 0, 255);
    s.g = std::clamp(g, 0, 255);
    s.b = std::clamp(b, 0, 255);
    stops.push_back(s);
  }
  if (stops.size() < 2) {
    if (error) *error = "LUT must contain >=2 rows: p r g b";
    return false;
  }
  std::sort(stops.begin(), stops.end(), [](const ColorStop& a, const ColorStop& b) { return a.p < b.p; });
  outStops->swap(stops);
  return true;
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

  const int fft = std::clamp(app->previewFftSize, 256, 1024);
  const int hop = 128;
  const auto spec = ndb::ComputeSpectrogram(app->previewWav.samples, app->previewWav.sampleRate, fft, hop);
  if (spec.frameCount <= 0 || spec.binCount <= 1) {
    return;
  }

  app->waterfallW = std::max(1, spec.frameCount);
  app->waterfallH = std::max(1, spec.binCount - 1);
  app->waterfallRgb.assign(static_cast<std::size_t>(app->waterfallW * app->waterfallH * 3), 0);
  app->waterfallRidgeMask.assign(static_cast<std::size_t>(app->waterfallW * app->waterfallH), 0);
  app->waterfallRidgeStrength.assign(static_cast<std::size_t>(app->waterfallW * app->waterfallH), 0);
  app->waterfallDbRender.assign(static_cast<std::size_t>(app->waterfallW * app->waterfallH), -120.0f);
  app->panInstantDb.assign(static_cast<std::size_t>(app->waterfallH), -120.0f);
  app->panAvgDb.assign(static_cast<std::size_t>(app->waterfallH), -120.0f);
  app->panSlowDb.assign(static_cast<std::size_t>(app->waterfallH), -120.0f);
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

  // Per-column background removal: estimate and subtract local floor over time.
  std::vector<float> colFloor(static_cast<std::size_t>(spec.frameCount), 0.0f);
  for (int t = 0; t < spec.frameCount; ++t) {
    std::vector<float> col;
    col.reserve(static_cast<std::size_t>(std::max(1, spec.binCount - 1)));
    for (int b = 1; b < spec.binCount; ++b) {
      col.push_back(lv[static_cast<std::size_t>(t * spec.binCount + b)]);
    }
    const std::size_t qi = static_cast<std::size_t>(0.22f * static_cast<float>(std::max<std::size_t>(1, col.size() - 1)));
    std::nth_element(col.begin(), col.begin() + static_cast<std::ptrdiff_t>(qi), col.end());
    colFloor[static_cast<std::size_t>(t)] = col[qi];
  }
  for (int t = 1; t < spec.frameCount; ++t) {
    colFloor[static_cast<std::size_t>(t)] =
        0.88f * colFloor[static_cast<std::size_t>(t - 1)] +
        0.12f * colFloor[static_cast<std::size_t>(t)];
  }

  std::vector<float> lvAdj = lv;
  std::vector<float> dbAdjVals;
  dbAdjVals.reserve(static_cast<std::size_t>(spec.frameCount * std::max(1, spec.binCount - 1)));
  for (int t = 0; t < spec.frameCount; ++t) {
    for (int b = 1; b < spec.binCount; ++b) {
      const std::size_t idx = static_cast<std::size_t>(t * spec.binCount + b);
      lvAdj[idx] = lv[idx] - app->bgRemovalStrength * colFloor[static_cast<std::size_t>(t)];
      dbAdjVals.push_back(lvAdj[idx]);
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
    app->panSlowDb[static_cast<std::size_t>(bi - 1)] = avg;
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

  float floorDb = percentile(dbAdjVals, 0.20f) + app->agcFloorOffsetDb;
  float spanDb = std::clamp(app->agcSpanDb, 8.0f, 80.0f);
  if (app->agcAutoContrast) {
    const float p10 = percentile(dbAdjVals, 0.10f);
    const float p85 = percentile(dbAdjVals, 0.85f);
    const float p995 = percentile(dbAdjVals, 0.995f);
    floorDb = 0.65f * floorDb + 0.35f * p10;
    const float autoSpan = std::clamp((p995 - p85) + 24.0f, 10.0f, 56.0f);
    spanDb = 0.55f * spanDb + 0.45f * autoSpan;
  }
  float ceilDb = floorDb + spanDb;
  app->panMinDb = floorDb;
  app->panMaxDb = ceilDb;

  std::vector<float> norm(static_cast<std::size_t>(spec.frameCount * spec.binCount), 0.0f);
  for (int t = 0; t < spec.frameCount; ++t) {
    for (int b = 1; b < spec.binCount; ++b) {
      float n = (lvAdj[static_cast<std::size_t>(t * spec.binCount + b)] - floorDb) /
                std::max(1.0f, (ceilDb - floorDb));
      n = std::clamp(n, 0.0f, 1.0f);
      // Dual-range split tone map: more detail in weak region, softer high compression.
      const float split = std::clamp(app->splitTonePoint, 0.35f, 0.80f);
      if (n < split) {
        const float x = n / std::max(1e-6f, split);
        n = split * std::pow(std::clamp(x, 0.0f, 1.0f), 0.78f);
      } else {
        const float x = (n - split) / std::max(1e-6f, (1.0f - split));
        n = split + (1.0f - split) * std::pow(std::clamp(x, 0.0f, 1.0f), 1.28f);
      }
      n = std::clamp((n - 0.010f) * app->agcGain, 0.0f, 1.0f);
      n = std::pow(std::clamp(n, 0.0f, 1.0f), std::clamp(app->agcGamma, 0.40f, 1.60f));
      norm[static_cast<std::size_t>(t * spec.binCount + b)] = n;
    }
  }

  // HDSDR-like persistence: fast attack, slow decay for thin CW/NDB traces.
  float persistenceDecay = 0.975f;
  if (app && app->waterfallPersistenceMode == 0) {
    persistenceDecay = 0.955f;
  } else if (app && app->waterfallPersistenceMode == 2) {
    persistenceDecay = 0.988f;
  }
  for (int b = 1; b < spec.binCount; ++b) {
    float prev = 0.0f;
    for (int t = 0; t < spec.frameCount; ++t) {
      const std::size_t idx = static_cast<std::size_t>(t * spec.binCount + b);
      const float cur = norm[idx];
      prev = std::max(cur, prev * persistenceDecay);
      norm[idx] = prev;
    }
  }

  // Selective smoothing v2: anisotropic kernel oriented for thin CW ridges
  // (more smoothing along time, conservative across frequency).
  std::vector<float> den = norm;
  auto at = [&](int tt, int bb) -> float {
    return norm[static_cast<std::size_t>(tt * spec.binCount + bb)];
  };
  for (int t = 1; t + 1 < spec.frameCount; ++t) {
    for (int b = 2; b + 2 < spec.binCount; ++b) {
      const std::size_t i = static_cast<std::size_t>(t * spec.binCount + b);
      const float c = norm[i];

      const float tm = at(t - 1, b);
      const float tp = at(t + 1, b);
      const float fm = at(t, b - 1);
      const float fp = at(t, b + 1);
      const float tmm = at(t - 2, b);
      const float tpp = at(t + 2, b);
      const float fmm = at(t, b - 2);
      const float fpp = at(t, b + 2);

      const float gradT = std::fabs(tp - tm);
      const float gradF = std::fabs(fp - fm);
      const float ridgeLike = (gradF > (1.25f * gradT) && c > 0.20f) ? 1.0f : 0.0f;

      const float gateT0 = std::exp(-8.0f * std::fabs(tm - c));
      const float gateT1 = std::exp(-8.0f * std::fabs(tp - c));
      const float gateT2 = std::exp(-9.5f * std::fabs(tmm - c));
      const float gateT3 = std::exp(-9.5f * std::fabs(tpp - c));
      const float gateF0 = std::exp(-11.0f * std::fabs(fm - c));
      const float gateF1 = std::exp(-11.0f * std::fabs(fp - c));
      const float gateF2 = std::exp(-12.0f * std::fabs(fmm - c));
      const float gateF3 = std::exp(-12.0f * std::fabs(fpp - c));

      float wtC = 0.62f;
      float wtT1 = 0.16f;
      float wtT2 = 0.06f;
      float wtF1 = 0.08f;
      float wtF2 = 0.02f;
      if (ridgeLike > 0.5f) {
        wtC = 0.68f;
        wtT1 = 0.19f;
        wtT2 = 0.08f;
        wtF1 = 0.035f;
        wtF2 = 0.008f;
      }

      const float sumW = wtC + wtT1 * (gateT0 + gateT1) + wtT2 * (gateT2 + gateT3) +
                         wtF1 * (gateF0 + gateF1) + wtF2 * (gateF2 + gateF3);
      const float smooth = (wtC * c + wtT1 * (gateT0 * tm + gateT1 * tp) +
                            wtT2 * (gateT2 * tmm + gateT3 * tpp) +
                            wtF1 * (gateF0 * fm + gateF1 * fp) +
                            wtF2 * (gateF2 * fmm + gateF3 * fpp)) /
                           std::max(1e-6f, sumW);

      const float noiseLike = std::max(0.0f, 0.75f - c) *
                              std::exp(-7.0f * std::min(0.35f, std::fabs(gradF - gradT)));
      const float blend = std::clamp(0.15f + 0.55f * noiseLike, 0.10f, ridgeLike > 0.5f ? 0.38f : 0.62f);
      den[i] = (1.0f - blend) * c + blend * smooth;
    }
  }
  norm.swap(den);

  // Adaptive local contrast (CLAHE-like lite): boosts weak traces while
  // preserving strong ridges and avoiding global over-compression.
  std::vector<float> local = norm;
  for (int t = 2; t + 2 < spec.frameCount; ++t) {
    for (int b = 2; b + 2 < spec.binCount; ++b) {
      const std::size_t i = static_cast<std::size_t>(t * spec.binCount + b);
      const float c = norm[i];
      double sum = 0.0;
      double sum2 = 0.0;
      int cnt = 0;
      for (int dt = -2; dt <= 2; ++dt) {
        for (int db = -1; db <= 1; ++db) {
          const float v = norm[static_cast<std::size_t>((t + dt) * spec.binCount + (b + db))];
          sum += v;
          sum2 += static_cast<double>(v) * static_cast<double>(v);
          ++cnt;
        }
      }
      const float mu = static_cast<float>(sum / std::max(1, cnt));
      const float var = static_cast<float>(std::max(0.0, (sum2 / std::max(1, cnt)) -
                                                             static_cast<double>(mu * mu)));
      const float sigma = std::sqrt(var + 1e-6f);

      const float z = (c - mu) / std::max(0.05f, 1.85f * sigma);
      float ce = 0.5f + 0.5f * std::tanh(1.35f * z);
      ce = std::clamp(ce, 0.0f, 1.0f);

      const float lowContrast = std::clamp((0.16f - sigma) / 0.16f, 0.0f, 1.0f);
      const float weakTone = std::clamp((0.68f - c) / 0.68f, 0.0f, 1.0f);
      const float boost = std::clamp(0.10f + 0.55f * lowContrast * weakTone, 0.0f, 0.58f);
      local[i] = (1.0f - boost) * c + boost * ce;
    }
  }
  norm.swap(local);

  // Birdie suppressor + two-pass ridge candidates.
  std::vector<std::uint8_t> ridgeCoarse(static_cast<std::size_t>(app->waterfallW * app->waterfallH), 0);
  std::vector<int> ridgeHitsByRow(static_cast<std::size_t>(app->waterfallH), 0);
  const float qBirdie = std::clamp(app->qrmBirdieSuppression, 0.5f, 2.0f);
  const float qRidge = std::clamp(app->qrmRidgeAggressiveness, 0.5f, 2.0f);
  for (int t = 2; t + 2 < app->waterfallW; ++t) {
    for (int y = 2; y + 2 < app->waterfallH; ++y) {
      const int b = app->waterfallH - y;
      const float c = norm[static_cast<std::size_t>(t * spec.binCount + b)];
      const float tL = norm[static_cast<std::size_t>((t - 1) * spec.binCount + b)];
      const float tR = norm[static_cast<std::size_t>((t + 1) * spec.binCount + b)];
      const float fD = norm[static_cast<std::size_t>(t * spec.binCount + (b - 1))];
      const float fU = norm[static_cast<std::size_t>(t * spec.binCount + (b + 1))];
      const float gradT = std::fabs(tR - tL);
      const float gradF = std::fabs(fU - fD);
      const float ridgeMin = 0.24f - 0.05f * (qRidge - 1.0f);
      const float gradRatio = 1.15f - 0.20f * (qRidge - 1.0f);
      if (c > ridgeMin && gradF > gradRatio * gradT) {
        ridgeCoarse[static_cast<std::size_t>(y * app->waterfallW + t)] = 1;
        ridgeHitsByRow[static_cast<std::size_t>(y)] += 1;
      }
    }
  }
  for (int y = 0; y < app->waterfallH; ++y) {
    const float occ = static_cast<float>(ridgeHitsByRow[static_cast<std::size_t>(y)]) /
                      std::max(1.0f, static_cast<float>(app->waterfallW));
    const float birdieOccThr = std::clamp(0.82f - 0.12f * (qBirdie - 1.0f), 0.60f, 0.90f);
    if (occ > birdieOccThr) {
      for (int t = 0; t < app->waterfallW; ++t) {
        const int b = app->waterfallH - y;
        const std::size_t idx = static_cast<std::size_t>(t * spec.binCount + b);
        const float atten = std::clamp(0.86f - 0.16f * qBirdie, 0.45f, 0.82f);
        norm[idx] = std::clamp(norm[idx] * atten, 0.0f, 1.0f);
        ridgeCoarse[static_cast<std::size_t>(y * app->waterfallW + t)] = 0;
      }
    }
  }

  const auto palette = PaletteStopsByPreset(app);

  for (int t = 0; t < app->waterfallW; ++t) {
    for (int y = 0; y < app->waterfallH; ++y) {
      const int b = app->waterfallH - y;
      const float v = lv[static_cast<std::size_t>(t * spec.binCount + b)];
      float n = norm[static_cast<std::size_t>(t * spec.binCount + b)];

      // Manual band-focus windows: subtle gain around operator-selected bands.
      if (app->manualNotchEnabled && !app->manualNotches.empty() && app->previewWav.sampleRate > 0) {
        const float nyq = 0.5f * static_cast<float>(app->previewWav.sampleRate);
        const float fMin = 80.0f;
        const float fMax = std::min(2200.0f, nyq - 20.0f);
        const float fHz = fMax - (static_cast<float>(y) / std::max(1.0f, static_cast<float>(app->waterfallH - 1))) *
                                     (fMax - fMin);
        float focus = 0.0f;
        const int lim = std::min<int>(3, static_cast<int>(app->manualNotches.size()));
        for (int fi = 0; fi < lim; ++fi) {
          const auto& nb = app->manualNotches[static_cast<std::size_t>(fi)];
          const float span = std::max(18.0f, nb.widthHz * 2.2f);
          const float d = std::fabs(fHz - nb.freqHz);
          const float w = std::clamp(1.0f - d / span, 0.0f, 1.0f);
          focus = std::max(focus, w);
        }
        if (focus > 0.0f) {
          n = std::clamp(n * (1.0f + 0.12f * focus), 0.0f, 1.0f);
        }
      }

      std::uint8_t ridge = 0;
      std::uint8_t ridgeStrength = 0;
      if (t > 2 && t + 2 < app->waterfallW && y > 0 && y + 1 < app->waterfallH) {
        const std::size_t m0 = static_cast<std::size_t>(y * app->waterfallW + t);
        if (ridgeCoarse[m0] != 0) {
          const int temporal = static_cast<int>(ridgeCoarse[static_cast<std::size_t>(y * app->waterfallW + (t - 1))]) +
                               static_cast<int>(ridgeCoarse[static_cast<std::size_t>(y * app->waterfallW + (t + 1))]) +
                               static_cast<int>(ridgeCoarse[static_cast<std::size_t>(y * app->waterfallW + (t - 2))]) +
                               static_cast<int>(ridgeCoarse[static_cast<std::size_t>(y * app->waterfallW + (t + 2))]);
          const int lateral = static_cast<int>(ridgeCoarse[static_cast<std::size_t>((y - 1) * app->waterfallW + t)]) +
                              static_cast<int>(ridgeCoarse[static_cast<std::size_t>((y + 1) * app->waterfallW + t)]);
          if (temporal >= 2 && lateral <= 1) {
            ridge = 255;
            ridgeStrength = static_cast<std::uint8_t>(std::clamp(45 + temporal * 40 - lateral * 20, 0, 255));
          }
        }
      }

      const COLORREF c = SamplePalette(palette, n);
      const std::size_t idx = static_cast<std::size_t>((y * app->waterfallW + t) * 3);
      app->waterfallRgb[idx + 0] = GetBValue(c);
      app->waterfallRgb[idx + 1] = GetGValue(c);
      app->waterfallRgb[idx + 2] = GetRValue(c);
      app->waterfallRidgeMask[static_cast<std::size_t>(y * app->waterfallW + t)] = ridge;
      app->waterfallRidgeStrength[static_cast<std::size_t>(y * app->waterfallW + t)] = ridgeStrength;
      app->waterfallDbRender[static_cast<std::size_t>(y * app->waterfallW + t)] = v;
    }
  }

  app->waterfallDiffRgb.assign(static_cast<std::size_t>(app->waterfallW * app->waterfallH * 3), 0);
  for (int y = 0; y < app->waterfallH; ++y) {
    for (int t = 1; t < app->waterfallW; ++t) {
      const float d0 = app->waterfallDbRender[static_cast<std::size_t>(y * app->waterfallW + (t - 1))];
      const float d1 = app->waterfallDbRender[static_cast<std::size_t>(y * app->waterfallW + t)];
      const float dd = std::clamp((d1 - d0) / 8.0f, -1.0f, 1.0f);
      const float a = std::fabs(dd);
      const std::size_t idx = static_cast<std::size_t>((y * app->waterfallW + t) * 3);
      if (dd >= 0.0f) {
        app->waterfallDiffRgb[idx + 2] = static_cast<std::uint8_t>(40 + 215 * a);
        app->waterfallDiffRgb[idx + 1] = static_cast<std::uint8_t>(20 + 190 * a);
        app->waterfallDiffRgb[idx + 0] = static_cast<std::uint8_t>(18 + 86 * a);
      } else {
        app->waterfallDiffRgb[idx + 2] = static_cast<std::uint8_t>(18 + 80 * a);
        app->waterfallDiffRgb[idx + 1] = static_cast<std::uint8_t>(28 + 170 * a);
        app->waterfallDiffRgb[idx + 0] = static_cast<std::uint8_t>(40 + 215 * a);
      }
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

RECT GetWaterfallMapRect(const RECT& clientRc) {
  const int outerGap = 10;
  const int wfH = 360;
  RECT wfRc = {clientRc.left + outerGap, clientRc.top + outerGap, clientRc.right - outerGap,
               clientRc.top + wfH};
  const int panH = 112;
  RECT panRc = {wfRc.left + 14, wfRc.top + 30, wfRc.right - 14, wfRc.top + 30 + panH};
  RECT plot = {wfRc.left + 14, panRc.bottom + 8, wfRc.right - 14, wfRc.bottom - 34};
  RECT map = {plot.left, plot.bottom - 12, plot.right, plot.bottom - 2};
  return map;
}

float WaterfallFreqRangeMinHz(const AppState* app) {
  if (!app || app->previewWav.sampleRate <= 0) {
    return 80.0f;
  }
  return 80.0f;
}

float WaterfallFreqRangeMaxHz(const AppState* app) {
  if (!app || app->previewWav.sampleRate <= 0) {
    return 2200.0f;
  }
  const float nyq = 0.5f * static_cast<float>(app->previewWav.sampleRate);
  return std::min(2200.0f, nyq - 20.0f);
}

float YToFreqHz(const AppState* app, const RECT& plot, int y) {
  const float fMin = WaterfallFreqRangeMinHz(app);
  const float fMax = WaterfallFreqRangeMaxHz(app);
  const float yn = static_cast<float>(std::clamp(y, static_cast<int>(plot.top), static_cast<int>(plot.bottom - 1)) -
                                       plot.top) /
                   std::max<int>(1, static_cast<int>(plot.bottom - plot.top));
  return fMax - yn * (fMax - fMin);
}

int FreqToY(const AppState* app, const RECT& plot, float freqHz) {
  const float fMin = WaterfallFreqRangeMinHz(app);
  const float fMax = WaterfallFreqRangeMaxHz(app);
  if (fMax <= fMin) {
    return plot.bottom;
  }
  const float yn = 1.0f - (std::clamp(freqHz, fMin, fMax) - fMin) / (fMax - fMin);
  return plot.top + static_cast<int>(std::round(yn * (plot.bottom - plot.top)));
}

int HitTestManualNotch(const AppState* app, const RECT& plot, POINT p) {
  if (!app || app->manualNotches.empty() || !PtInRect(&plot, p)) {
    return -1;
  }
  int best = -1;
  int bestDy = 99999;
  for (std::size_t i = 0; i < app->manualNotches.size(); ++i) {
    const int y = FreqToY(app, plot, app->manualNotches[i].freqHz);
    const int dy = std::abs(p.y - y);
    if (dy < bestDy) {
      bestDy = dy;
      best = static_cast<int>(i);
    }
  }
  return (bestDy <= 9) ? best : -1;
}

void SortAndMergeManualNotches(AppState* app) {
  if (!app) return;
  std::sort(app->manualNotches.begin(), app->manualNotches.end(),
            [](const NotchBand& a, const NotchBand& b) { return a.freqHz < b.freqHz; });
  std::vector<NotchBand> merged;
  merged.reserve(app->manualNotches.size());
  for (const auto& n : app->manualNotches) {
    if (!merged.empty() && std::fabs(merged.back().freqHz - n.freqHz) < 1.0f) {
      merged.back().widthHz = std::max(merged.back().widthHz, n.widthHz);
    } else {
      merged.push_back(n);
    }
  }
  app->manualNotches.swap(merged);
}

void ClampManualNotchesToRange(AppState* app) {
  if (!app) {
    return;
  }
  const float fMin = WaterfallFreqRangeMinHz(app);
  const float fMax = WaterfallFreqRangeMaxHz(app);
  for (auto& n : app->manualNotches) {
    n.freqHz = std::clamp(n.freqHz, fMin, fMax);
    n.widthHz = std::clamp(n.widthHz, 6.0f, 200.0f);
  }
}

void AddManualNotchAtFreq(AppState* app, float freqHz) {
  if (!app) {
    return;
  }
  const float fMin = WaterfallFreqRangeMinHz(app);
  const float fMax = WaterfallFreqRangeMaxHz(app);
  const float f = std::clamp(freqHz, fMin, fMax);
  for (std::size_t i = 0; i < app->manualNotches.size(); ++i) {
    if (std::fabs(app->manualNotches[i].freqHz - f) < 4.0f) {
      app->activeManualNotch = static_cast<int>(i);
      return;
    }
  }
  NotchBand n;
  n.freqHz = f;
  n.widthHz = 24.0f;
  app->manualNotches.push_back(n);
  SortAndMergeManualNotches(app);
  for (std::size_t i = 0; i < app->manualNotches.size(); ++i) {
    if (std::fabs(app->manualNotches[i].freqHz - f) < 1.0f) {
      app->activeManualNotch = static_cast<int>(i);
      break;
    }
  }
}

void RemoveManualNotchByIndex(AppState* app, int idx) {
  if (!app || idx < 0 || idx >= static_cast<int>(app->manualNotches.size())) {
    return;
  }
  app->manualNotches.erase(app->manualNotches.begin() + idx);
  if (app->manualNotches.empty()) {
    app->activeManualNotch = -1;
  } else {
    app->activeManualNotch = std::clamp(idx, 0, static_cast<int>(app->manualNotches.size()) - 1);
  }
}

void UpdateActiveManualNotchFromPoint(AppState* app, const RECT& plot, POINT p) {
  if (!app || app->activeManualNotch < 0 || app->activeManualNotch >= static_cast<int>(app->manualNotches.size())) {
    return;
  }
  const float f = YToFreqHz(app, plot, p.y);
  app->manualNotches[static_cast<std::size_t>(app->activeManualNotch)].freqHz = f;
  SortAndMergeManualNotches(app);
  ClampManualNotchesToRange(app);
}

void ApplyWaterfallZoomBox(AppState* app, const RECT& plot, POINT p0, POINT p1) {
  if (!app || app->waterfallW <= 0) {
    return;
  }
  const int x0 = std::clamp(static_cast<int>(std::min(p0.x, p1.x)), static_cast<int>(plot.left),
                            static_cast<int>(plot.right - 1));
  const int x1 = std::clamp(static_cast<int>(std::max(p0.x, p1.x)), static_cast<int>(plot.left + 1),
                            static_cast<int>(plot.right));
  const int selPx = std::max(2, x1 - x0);
  const int plotW = std::max<int>(1, static_cast<int>(plot.right - plot.left));
  if (selPx < 18) {
    return;
  }

  const double currentZoom = std::clamp(app->waterfallZoom, 1.0, 8.0);
  const int visCur = std::max(60, static_cast<int>(std::round(static_cast<double>(std::max(1, app->waterfallW)) /
                                                               currentZoom)));
  const int panCur = std::clamp(app->waterfallPanPx, 0, std::max(0, app->waterfallW - visCur));
  const double startN = static_cast<double>(x0 - plot.left) / static_cast<double>(plotW);
  const double endN = static_cast<double>(x1 - plot.left) / static_cast<double>(plotW);
  const int col0 = panCur + static_cast<int>(std::round(startN * visCur));
  const int col1 = panCur + static_cast<int>(std::round(endN * visCur));
  const int selCols = std::max(2, col1 - col0);

  const double targetZoom = std::clamp(static_cast<double>(app->waterfallW) / static_cast<double>(selCols), 1.0,
                                       8.0);
  app->waterfallZoom = targetZoom;
  const int visNew = std::max(60, static_cast<int>(std::round(static_cast<double>(std::max(1, app->waterfallW)) /
                                                               targetZoom)));
  const int centerCol = (col0 + col1) / 2;
  const int maxPan = std::max(0, app->waterfallW - visNew);
  app->waterfallPanPx = std::clamp(centerCol - visNew / 2, 0, maxPan);
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
  if (app->waterfallFrozen) {
    const double zoom = std::clamp(app->waterfallZoom, 1.0, 8.0);
    *srcW = std::max(60, std::min(app->waterfallW,
                                  static_cast<int>(std::round(static_cast<double>(app->waterfallW) / zoom))));
    int center = app->waterfallFreezeCenterCol;
    if (center < 0) {
      center = app->waterfallW / 2;
    }
    const int maxPan = std::max(0, app->waterfallW - *srcW);
    *srcX = std::clamp(center - *srcW / 2, 0, maxPan);
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

float PreviewDurationSec(const AppState* app) {
  if (!app || app->previewWav.sampleRate <= 0 || app->previewWav.samples.empty()) {
    return 0.0f;
  }
  return static_cast<float>(app->previewWav.samples.size()) /
         static_cast<float>(std::max(1, app->previewWav.sampleRate));
}

float CurrentBookmarkTimeSec(const AppState* app) {
  const float dur = PreviewDurationSec(app);
  if (dur <= 0.0f || !app) {
    return 0.0f;
  }
  if (app->running) {
    const float p = static_cast<float>(std::clamp(app->decodeProgressVisualPct, 0.0, 100.0) / 100.0);
    return std::clamp(p * dur, 0.0f, dur);
  }
  int srcX = 0;
  int srcW = 0;
  ComputeWaterfallSourceWindow(app, &srcX, &srcW);
  const float centerCol = static_cast<float>(srcX) + 0.5f * static_cast<float>(std::max(1, srcW));
  const float n = centerCol / std::max(1.0f, static_cast<float>(app->waterfallW - 1));
  return std::clamp(n * dur, 0.0f, dur);
}

void AddBookmarkAtCurrent(AppState* app) {
  if (!app) {
    return;
  }
  const float tSec = CurrentBookmarkTimeSec(app);
  const float dur = PreviewDurationSec(app);
  if (dur <= 0.0f) {
    return;
  }
  for (std::size_t i = 0; i < app->bookmarksSec.size(); ++i) {
    if (std::fabs(app->bookmarksSec[i] - tSec) <= 0.08f) {
      if (i < app->bookmarkAuto.size()) {
        app->bookmarkAuto[i] = 0;
      }
      if (i < app->bookmarkConfidence.size()) {
        app->bookmarkConfidence[i] = 1.0f;
      }
      return;
    }
  }
  app->bookmarksSec.push_back(std::clamp(tSec, 0.0f, dur));
  app->bookmarkAuto.push_back(0);
  app->bookmarkConfidence.push_back(1.0f);
}

void AddBookmarkAtTime(AppState* app, float tSec, bool automatic, float confidence = 1.0f) {
  if (!app) {
    return;
  }
  const float dur = PreviewDurationSec(app);
  if (dur <= 0.0f) {
    return;
  }
  for (std::size_t i = 0; i < app->bookmarksSec.size(); ++i) {
    if (std::fabs(app->bookmarksSec[i] - tSec) <= 0.08f) {
      if (i < app->bookmarkAuto.size() && !automatic) {
        app->bookmarkAuto[i] = 0;
      }
      if (i < app->bookmarkConfidence.size()) {
        app->bookmarkConfidence[i] = std::max(app->bookmarkConfidence[i], std::clamp(confidence, 0.0f, 1.0f));
      }
      return;
    }
  }
  app->bookmarksSec.push_back(std::clamp(tSec, 0.0f, dur));
  app->bookmarkAuto.push_back(automatic ? 1 : 0);
  app->bookmarkConfidence.push_back(std::clamp(confidence, 0.0f, 1.0f));

  std::vector<std::size_t> idx(app->bookmarksSec.size());
  for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = i;
  std::sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) {
    return app->bookmarksSec[a] < app->bookmarksSec[b];
  });
  std::vector<float> t2;
  std::vector<std::uint8_t> a2;
  std::vector<float> c2;
  t2.reserve(idx.size());
  a2.reserve(idx.size());
  c2.reserve(idx.size());
  for (std::size_t i : idx) {
    t2.push_back(app->bookmarksSec[i]);
    a2.push_back((i < app->bookmarkAuto.size()) ? app->bookmarkAuto[i] : 0);
    c2.push_back((i < app->bookmarkConfidence.size()) ? app->bookmarkConfidence[i] : 1.0f);
  }
  app->bookmarksSec.swap(t2);
  app->bookmarkAuto.swap(a2);
  app->bookmarkConfidence.swap(c2);
}

bool BookmarkVisible(const AppState* app, std::size_t idx) {
  if (!app || idx >= app->bookmarksSec.size()) {
    return false;
  }
  const bool isAuto = (idx < app->bookmarkAuto.size() && app->bookmarkAuto[idx] != 0);
  return (!isAuto) || app->showAutoBookmarks;
}

bool JumpToBookmarkFromMapClick(AppState* app, const RECT& mapRc, POINT p) {
  if (!app || app->bookmarksSec.empty() || app->waterfallW <= 0 || !PtInRect(&mapRc, p)) {
    return false;
  }
  const float dur = PreviewDurationSec(app);
  if (dur <= 0.0f) {
    return false;
  }

  float bestT = -1.0f;
  int bestDx = 999999;
  for (std::size_t i = 0; i < app->bookmarksSec.size(); ++i) {
    if (!BookmarkVisible(app, i)) continue;
    const float tsec = app->bookmarksSec[i];
    const float tn = std::clamp(tsec / std::max(0.1f, dur), 0.0f, 1.0f);
    const int x = mapRc.left + static_cast<int>(tn * (mapRc.right - mapRc.left - 1));
    const int dx = std::abs(p.x - x);
    if (dx < bestDx) {
      bestDx = dx;
      bestT = tsec;
    }
  }
  if (bestDx > 8 || bestT < 0.0f) {
    return false;
  }

  const int vis = std::max(60, static_cast<int>(std::round(static_cast<double>(std::max(1, app->waterfallW)) /
                                                            std::max(1.0, app->waterfallZoom))));
  const int maxPan = std::max(0, app->waterfallW - vis);
  const int col = std::clamp(static_cast<int>(std::round((bestT / dur) * (app->waterfallW - 1))),
                             0, app->waterfallW - 1);
  app->waterfallPanPx = std::clamp(col - vis / 2, 0, maxPan);
  return true;
}

bool RemoveBookmarkFromMapClick(AppState* app, const RECT& mapRc, POINT p) {
  if (!app || app->bookmarksSec.empty() || !PtInRect(&mapRc, p)) {
    return false;
  }
  const float dur = PreviewDurationSec(app);
  if (dur <= 0.0f) {
    return false;
  }
  std::size_t bestIdx = 0;
  int bestDx = 999999;
  for (std::size_t i = 0; i < app->bookmarksSec.size(); ++i) {
    if (!BookmarkVisible(app, i)) continue;
    const float tn = std::clamp(app->bookmarksSec[i] / std::max(0.1f, dur), 0.0f, 1.0f);
    const int x = mapRc.left + static_cast<int>(tn * (mapRc.right - mapRc.left - 1));
    const int dx = std::abs(p.x - x);
    if (dx < bestDx) {
      bestDx = dx;
      bestIdx = i;
    }
  }
  if (bestDx > 8) {
    return false;
  }
  app->bookmarksSec.erase(app->bookmarksSec.begin() + static_cast<std::ptrdiff_t>(bestIdx));
  if (bestIdx < app->bookmarkAuto.size()) {
    app->bookmarkAuto.erase(app->bookmarkAuto.begin() + static_cast<std::ptrdiff_t>(bestIdx));
  }
  if (bestIdx < app->bookmarkConfidence.size()) {
    app->bookmarkConfidence.erase(app->bookmarkConfidence.begin() + static_cast<std::ptrdiff_t>(bestIdx));
  }
  return true;
}

void SetWaterfallPanToTime(AppState* app, float tSec) {
  if (!app || app->waterfallW <= 0) {
    return;
  }
  const float dur = PreviewDurationSec(app);
  if (dur <= 0.0f) {
    return;
  }
  const int vis = std::max(60, static_cast<int>(std::round(static_cast<double>(std::max(1, app->waterfallW)) /
                                                            std::max(1.0, app->waterfallZoom))));
  const int maxPan = std::max(0, app->waterfallW - vis);
  const int col = std::clamp(static_cast<int>(std::round((tSec / dur) * (app->waterfallW - 1))),
                             0, app->waterfallW - 1);
  app->waterfallPanPx = std::clamp(col - vis / 2, 0, maxPan);
}

bool JumpToAdjacentBookmark(AppState* app, int dir) {
  if (!app || app->bookmarksSec.empty()) {
    return false;
  }
  const float now = CurrentBookmarkTimeSec(app);
  if (dir >= 0) {
    for (std::size_t i = 0; i < app->bookmarksSec.size(); ++i) {
      if (!BookmarkVisible(app, i)) continue;
      const float t = app->bookmarksSec[i];
      if (t > now + 0.05f) {
        SetWaterfallPanToTime(app, t);
        return true;
      }
    }
    for (std::size_t i = 0; i < app->bookmarksSec.size(); ++i) {
      if (BookmarkVisible(app, i)) {
        SetWaterfallPanToTime(app, app->bookmarksSec[i]);
        return true;
      }
    }
    return false;
  }
  for (std::size_t i = app->bookmarksSec.size(); i-- > 0;) {
    if (!BookmarkVisible(app, i)) continue;
    if (app->bookmarksSec[i] < now - 0.05f) {
      SetWaterfallPanToTime(app, app->bookmarksSec[i]);
      return true;
    }
  }
  for (std::size_t i = app->bookmarksSec.size(); i-- > 0;) {
    if (BookmarkVisible(app, i)) {
      SetWaterfallPanToTime(app, app->bookmarksSec[i]);
      return true;
    }
  }
  return false;
}

bool JumpToBookmarkIndex(AppState* app, int index0) {
  if (!app || index0 < 0) {
    return false;
  }
  int visIdx = 0;
  for (std::size_t i = 0; i < app->bookmarksSec.size(); ++i) {
    if (!BookmarkVisible(app, i)) continue;
    if (visIdx == index0) {
      SetWaterfallPanToTime(app, app->bookmarksSec[i]);
      return true;
    }
    ++visIdx;
  }
  return false;
}

bool RemoveBookmarkNearestCurrent(AppState* app) {
  if (!app || app->bookmarksSec.empty()) {
    return false;
  }
  const float now = CurrentBookmarkTimeSec(app);
  std::size_t bestIdx = 0;
  float bestDt = std::numeric_limits<float>::max();
  for (std::size_t i = 0; i < app->bookmarksSec.size(); ++i) {
    if (!BookmarkVisible(app, i)) continue;
    const float dt = std::fabs(app->bookmarksSec[i] - now);
    if (dt < bestDt) {
      bestDt = dt;
      bestIdx = i;
    }
  }
  if (bestDt == std::numeric_limits<float>::max()) {
    return false;
  }
  app->bookmarksSec.erase(app->bookmarksSec.begin() + static_cast<std::ptrdiff_t>(bestIdx));
  if (bestIdx < app->bookmarkAuto.size()) {
    app->bookmarkAuto.erase(app->bookmarkAuto.begin() + static_cast<std::ptrdiff_t>(bestIdx));
  }
  if (bestIdx < app->bookmarkConfidence.size()) {
    app->bookmarkConfidence.erase(app->bookmarkConfidence.begin() + static_cast<std::ptrdiff_t>(bestIdx));
  }
  return true;
}

void UpdatePanadapterPersistence(AppState* app) {
  if (!app || app->waterfallW <= 0 || app->waterfallH <= 0 || app->waterfallDbRender.empty()) {
    return;
  }
  if (app->panInstantDb.size() != static_cast<std::size_t>(app->waterfallH)) {
    app->panInstantDb.assign(static_cast<std::size_t>(app->waterfallH), app->panMinDb);
  }
  if (app->panAvgDb.size() != static_cast<std::size_t>(app->waterfallH)) {
    app->panAvgDb.assign(static_cast<std::size_t>(app->waterfallH), app->panMinDb);
  }
  if (app->panSlowDb.size() != static_cast<std::size_t>(app->waterfallH)) {
    app->panSlowDb.assign(static_cast<std::size_t>(app->waterfallH), app->panMinDb);
  }
  if (app->panPeakDb.size() != static_cast<std::size_t>(app->waterfallH)) {
    app->panPeakDb.assign(static_cast<std::size_t>(app->waterfallH), app->panMinDb);
  }

  const int col = std::clamp(
      static_cast<int>((std::clamp(app->decodeProgressVisualPct, 0.0, 100.0) / 100.0) *
                       std::max(1, app->waterfallW - 1)),
      0, app->waterfallW - 1);
  if (col == app->panLastCol) {
    return;
  }
  app->panLastCol = col;

  for (int y = 0; y < app->waterfallH; ++y) {
    const float inst = app->waterfallDbRender[static_cast<std::size_t>(y * app->waterfallW + col)];
    app->panInstantDb[static_cast<std::size_t>(y)] = inst;
    const float a = std::clamp(app->panAvgAlpha, 0.01f, 0.40f);
    const float aSlow = std::clamp(a * 0.25f, 0.005f, 0.12f);
    app->panAvgDb[static_cast<std::size_t>(y)] =
        (1.0f - a) * app->panAvgDb[static_cast<std::size_t>(y)] + a * inst;
    app->panSlowDb[static_cast<std::size_t>(y)] =
        (1.0f - aSlow) * app->panSlowDb[static_cast<std::size_t>(y)] + aSlow * inst;
    const float decayed = app->panPeakDb[static_cast<std::size_t>(y)] -
                          std::clamp(app->panPeakDecay, 0.01f, 1.20f);
    app->panPeakDb[static_cast<std::size_t>(y)] = std::max(decayed, inst);
  }
}

void DrawPanadapter(HDC hdc, const RECT& rc, AppState* app) {
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
  DrawTextW(hdc, L"Panadapter  Fast/Slow/Max", -1, &tr, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

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
  drawSeries(app->panSlowDb, RGB(122, 144, 255), 1);
  drawSeries(app->panAvgDb, RGB(95, 190, 240), 2);
  drawSeries(app->panInstantDb, RGB(255, 226, 92), 2);

  // Noise-floor and dynamic threshold overlays.
  if (!app->panSlowDb.empty()) {
    std::vector<float> tmp = app->panSlowDb;
    const auto qAt = [&](float q) {
      const std::size_t idx = static_cast<std::size_t>(std::clamp(q, 0.0f, 1.0f) *
                                                       static_cast<float>(std::max<std::size_t>(1, tmp.size() - 1)));
      std::nth_element(tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(idx), tmp.end());
      return tmp[idx];
    };
    const float noiseFloorDb = qAt(0.22f);
    const float thrDb = noiseFloorDb + 8.0f;
    const auto yForDb = [&](float db) {
      const float yn = std::clamp((db - minDb) / std::max(1.0f, (maxDb - minDb)), 0.0f, 1.0f);
      return plot.bottom - static_cast<int>(yn * (plot.bottom - plot.top));
    };
    const int yNoise = yForDb(noiseFloorDb);
    const int yThr = yForDb(thrDb);

    HPEN pn = CreatePen(PS_DOT, 1, RGB(118, 172, 214));
    auto oldPn = reinterpret_cast<HPEN>(SelectObject(hdc, pn));
    MoveToEx(hdc, plot.left, yNoise, nullptr);
    LineTo(hdc, plot.right, yNoise);
    SelectObject(hdc, oldPn);
    DeleteObject(pn);

    HPEN pt = CreatePen(PS_DASH, 1, RGB(255, 168, 98));
    auto oldPt = reinterpret_cast<HPEN>(SelectObject(hdc, pt));
    MoveToEx(hdc, plot.left, yThr, nullptr);
    LineTo(hdc, plot.right, yThr);
    SelectObject(hdc, oldPt);
    DeleteObject(pt);

    RECT nr = {plot.left + 6, yNoise - 10, plot.left + 180, yNoise + 6};
    RECT trr = {plot.left + 186, yThr - 10, plot.left + 360, yThr + 6};
    std::wstringstream nss;
    nss << L"NF " << std::fixed << std::setprecision(1) << noiseFloorDb << L" dB";
    std::wstringstream tss;
    tss << L"TH " << std::fixed << std::setprecision(1) << thrDb << L" dB";
    SetTextColor(hdc, RGB(156, 206, 244));
    DrawTextW(hdc, nss.str().c_str(), -1, &nr, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    SetTextColor(hdc, RGB(255, 186, 120));
    DrawTextW(hdc, tss.str().c_str(), -1, &trr, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
  }

  if (app->previewWav.sampleRate > 0) {
    const float nyq = 0.5f * static_cast<float>(app->previewWav.sampleRate);
    const auto xForBin = [&](int bi) {
      const float fn = static_cast<float>(bi) /
                       std::max(1.0f, static_cast<float>(std::max(1, app->waterfallH - 1)));
      return plot.left + static_cast<int>(fn * (plot.right - plot.left));
    };

    // Notch/lock bands from decoded rows (crowded-channel readability)
    for (const auto& r : app->overlayRows) {
      if (r.freqHz <= 0.0f || r.freqHz >= nyq) {
        continue;
      }
      const int bi = std::clamp(static_cast<int>((r.freqHz / std::max(1.0f, nyq)) * app->waterfallH),
                                0, std::max(0, app->waterfallH - 1));
      const int x = xForBin(bi);
      RECT band = {x - 2, plot.top + 1, x + 2, plot.bottom - 1};
      HBRUSH bb = CreateSolidBrush(RGB(36, 76, 122));
      FillRect(hdc, &band, bb);
      DeleteObject(bb);
    }

    if (app->manualNotchEnabled && !app->manualNotches.empty()) {
      HPEN mpen = CreatePen(PS_SOLID, 1, RGB(255, 142, 106));
      HPEN mpenSel = CreatePen(PS_SOLID, 2, RGB(255, 196, 136));
      auto oldMp = reinterpret_cast<HPEN>(SelectObject(hdc, mpen));
      for (std::size_t i = 0; i < app->manualNotches.size(); ++i) {
        const auto& n = app->manualNotches[i];
        const int bi = std::clamp(static_cast<int>((n.freqHz / std::max(1.0f, nyq)) * app->waterfallH),
                                  0, std::max(0, app->waterfallH - 1));
        const int x = xForBin(bi);
        const int halfPx = std::max(1, static_cast<int>(std::round((n.widthHz / std::max(1.0f, nyq)) *
                                                                    (plot.right - plot.left) * 0.5f)));
        RECT mb = {x - halfPx, plot.top + 1, x + halfPx, plot.bottom - 1};
        HBRUSH mf = CreateSolidBrush((static_cast<int>(i) == app->activeManualNotch)
                                         ? RGB(68, 42, 30)
                                         : RGB(46, 30, 24));
        FillRect(hdc, &mb, mf);
        DeleteObject(mf);
        SelectObject(hdc, (static_cast<int>(i) == app->activeManualNotch) ? mpenSel : mpen);
        MoveToEx(hdc, x, plot.top + 1, nullptr);
        LineTo(hdc, x, plot.bottom - 1);
      }
      SelectObject(hdc, oldMp);
      DeleteObject(mpen);
      DeleteObject(mpenSel);
    }

    // Peak markers from instant spectrum (HDSDR-like RF markers)
    struct Peak {
      int bin = 0;
      float db = -120.0f;
    };
    std::vector<Peak> peaks;
    const float thr = minDb + 0.62f * (maxDb - minDb);
    for (int i = 2; i + 2 < static_cast<int>(app->panInstantDb.size()); ++i) {
      const float c = app->panInstantDb[static_cast<std::size_t>(i)];
      if (c < thr) {
        continue;
      }
      if (c >= app->panInstantDb[static_cast<std::size_t>(i - 1)] &&
          c >= app->panInstantDb[static_cast<std::size_t>(i + 1)] &&
          c > app->panInstantDb[static_cast<std::size_t>(i - 2)] &&
          c > app->panInstantDb[static_cast<std::size_t>(i + 2)]) {
        peaks.push_back(Peak{i, c});
      }
    }
    std::sort(peaks.begin(), peaks.end(), [](const Peak& a, const Peak& b) { return a.db > b.db; });
    if (peaks.size() > 5) {
      peaks.resize(5);
    }

    if (app->peakLockEnabled && !peaks.empty()) {
      int bestBin = peaks.front().bin;
      float bestDist = std::numeric_limits<float>::max();
      for (const auto& p : peaks) {
        const float d = std::fabs(static_cast<float>(p.bin - std::max(0, app->peakLockBin)));
        if (d < bestDist) {
          bestDist = d;
          bestBin = p.bin;
        }
      }
      app->peakLockBin = bestBin;
      app->peakLockHz = (static_cast<float>(bestBin) /
                         std::max(1.0f, static_cast<float>(app->waterfallH))) * nyq;
    }

    for (const auto& p : peaks) {
      const int x = xForBin(p.bin);
      HPEN mk = CreatePen(PS_DASH, 1, RGB(255, 240, 150));
      auto oldMk = reinterpret_cast<HPEN>(SelectObject(hdc, mk));
      MoveToEx(hdc, x, plot.top + 1, nullptr);
      LineTo(hdc, x, plot.bottom - 1);
      SelectObject(hdc, oldMk);
      DeleteObject(mk);

      const float fHz = (static_cast<float>(p.bin) / std::max(1.0f, static_cast<float>(app->waterfallH))) * nyq;
      std::wstringstream ss;
      ss << std::fixed << std::setprecision(3) << (fHz / 1000.0f);
      RECT lr = {x + 3, plot.top + 2, x + 68, plot.top + 16};
      SetTextColor(hdc, RGB(250, 232, 140));
      DrawTextW(hdc, ss.str().c_str(), -1, &lr, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }

    if (app->peakLockEnabled && app->peakLockBin >= 0) {
      const int lx = xForBin(std::clamp(app->peakLockBin, 0, std::max(0, app->waterfallH - 1)));
      HPEN lk = CreatePen(PS_SOLID, 2, RGB(120, 255, 170));
      auto oldLk = reinterpret_cast<HPEN>(SelectObject(hdc, lk));
      MoveToEx(hdc, lx, plot.top + 1, nullptr);
      LineTo(hdc, lx, plot.bottom - 1);
      SelectObject(hdc, oldLk);
      DeleteObject(lk);

      RECT lb = {lx + 6, plot.bottom - 16, lx + 130, plot.bottom - 2};
      std::wstringstream ls;
      ls << L"LOCK " << std::fixed << std::setprecision(3) << (app->peakLockHz / 1000.0f)
         << L" kHz";
      SetTextColor(hdc, RGB(160, 255, 192));
      DrawTextW(hdc, ls.str().c_str(), -1, &lb, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
  }
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

  // Active mode badges
  int bx = rc.left + 220;
  const int by = rc.top + 6;
  auto drawBadge = [&](const wchar_t* txt, COLORREF fg, COLORREF bg) {
    RECT br = {bx, by, bx + 78, by + 18};
    HBRUSH bb = CreateSolidBrush(bg);
    FillRect(hdc, &br, bb);
    DeleteObject(bb);
    HPEN bp = CreatePen(PS_SOLID, 1, RGB(64, 84, 104));
    auto oldBp = reinterpret_cast<HPEN>(SelectObject(hdc, bp));
    MoveToEx(hdc, br.left, br.top, nullptr);
    LineTo(hdc, br.right - 1, br.top);
    LineTo(hdc, br.right - 1, br.bottom - 1);
    LineTo(hdc, br.left, br.bottom - 1);
    LineTo(hdc, br.left, br.top);
    SelectObject(hdc, oldBp);
    DeleteObject(bp);
    SetTextColor(hdc, fg);
    DrawTextW(hdc, txt, -1, &br, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    bx += 84;
  };
  if (app && app->differenceWaterfallEnabled) {
    drawBadge(L"DIFF", RGB(232, 238, 246), RGB(54, 68, 90));
  }
  if (app && app->dotDashAssistEnabled) {
    drawBadge(L"DOTDASH", RGB(224, 248, 255), RGB(50, 80, 96));
  }
  if (app && app->waterfallFrozen) {
    drawBadge(L"FREEZE", RGB(255, 238, 176), RGB(92, 76, 28));
  }
  if (app) {
    const bool heavyQrm = (app->qrmBirdieSuppression > 1.15f || app->qrmRidgeAggressiveness > 1.15f);
    const bool noQrm = (app->qrmBirdieSuppression < 0.90f && app->qrmRidgeAggressiveness < 0.95f);
    if (heavyQrm) {
      drawBadge(L"QRM HEAVY", RGB(255, 224, 202), RGB(112, 58, 44));
    } else if (noQrm) {
      drawBadge(L"QRM NO", RGB(212, 255, 220), RGB(44, 98, 66));
    }
  }

  // Quick legend for advanced visual controls.
  RECT lg = {rc.right - 338, rc.top + 6, rc.right - 10, rc.top + 44};
  HBRUSH lbg = CreateSolidBrush(RGB(10, 18, 28));
  FillRect(hdc, &lg, lbg);
  DeleteObject(lbg);
  HPEN lpen = CreatePen(PS_SOLID, 1, RGB(74, 104, 134));
  auto oldLp = reinterpret_cast<HPEN>(SelectObject(hdc, lpen));
  MoveToEx(hdc, lg.left, lg.top, nullptr);
  LineTo(hdc, lg.right - 1, lg.top);
  LineTo(hdc, lg.right - 1, lg.bottom - 1);
  LineTo(hdc, lg.left, lg.bottom - 1);
  LineTo(hdc, lg.left, lg.top);
  SelectObject(hdc, oldLp);
  DeleteObject(lpen);
  RECT l1 = {lg.left + 8, lg.top + 2, lg.right - 8, lg.top + 18};
  RECT l2 = {lg.left + 8, lg.top + 18, lg.right - 8, lg.bottom - 2};
  SetTextColor(hdc, RGB(184, 212, 236));
  DrawTextW(hdc, L"F9/F10/F11 snap A/B/clear | Ctrl+Click set B", -1, &l1,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
  DrawTextW(hdc, L"X diff | G dotdash | F freeze", -1, &l2,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

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
  const std::vector<std::uint8_t>& wfBase =
      (app->differenceWaterfallEnabled && !app->waterfallDiffRgb.empty())
          ? app->waterfallDiffRgb
          : app->waterfallRgb;

  StretchDIBits(hdc, plot.left, plot.top, plot.right - plot.left, plot.bottom - plot.top,
                srcX, 0, srcW, app->waterfallH, wfBase.data(), &bmi, DIB_RGB_COLORS,
                SRCCOPY);

  // Session compare snapshots (A/B) with wipe slider.
  if (!app->snapshotA.empty() && !app->snapshotB.empty() && app->snapshotW == app->waterfallW &&
      app->snapshotH == app->waterfallH) {
    const int wipeX = plot.left + ((plot.right - plot.left) * std::clamp(app->snapshotWipePct, 0, 100)) / 100;
    StretchDIBits(hdc, plot.left, plot.top, wipeX - plot.left, plot.bottom - plot.top,
                  srcX, 0, std::max(1, (srcW * std::clamp(app->snapshotWipePct, 0, 100)) / 100),
                  app->waterfallH, app->snapshotA.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
    StretchDIBits(hdc, wipeX, plot.top, plot.right - wipeX, plot.bottom - plot.top,
                  srcX + std::max(1, (srcW * std::clamp(app->snapshotWipePct, 0, 100)) / 100), 0,
                  std::max(1, srcW - (srcW * std::clamp(app->snapshotWipePct, 0, 100)) / 100),
                  app->waterfallH, app->snapshotB.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
    HPEN wp = CreatePen(PS_SOLID, 2, RGB(255, 228, 154));
    auto oldWp = reinterpret_cast<HPEN>(SelectObject(hdc, wp));
    MoveToEx(hdc, wipeX, plot.top, nullptr);
    LineTo(hdc, wipeX, plot.bottom);
    SelectObject(hdc, oldWp);
    DeleteObject(wp);
    RECT wr = {plot.left + 6, plot.top + 2, plot.left + 220, plot.top + 18};
    SetTextColor(hdc, RGB(255, 238, 176));
    DrawTextW(hdc, L"Compare A/B [F9/F10/F11]", -1, &wr, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
  }

  // Ridge overlay: highlight thin CW-like tonal lines.
  if (app->showRidgeOverlay && !app->waterfallRidgeMask.empty()) {
    for (int x = plot.left; x < plot.right; x += 2) {
      const float xn = static_cast<float>(x - plot.left) /
                       std::max<int>(1, static_cast<int>(plot.right - plot.left - 1));
      const int sx = std::clamp(srcX + static_cast<int>(xn * std::max(1, srcW - 1)), 0,
                                app->waterfallW - 1);
      for (int y = plot.top; y < plot.bottom; y += 2) {
        const float yn = static_cast<float>(y - plot.top) /
                         std::max<int>(1, static_cast<int>(plot.bottom - plot.top - 1));
        const int sy = std::clamp(static_cast<int>(yn * std::max(1, app->waterfallH - 1)), 0,
                                  app->waterfallH - 1);
        const std::size_t mi = static_cast<std::size_t>(sy * app->waterfallW + sx);
        if (app->waterfallRidgeMask[mi] != 0) {
          const int s = static_cast<int>(app->waterfallRidgeStrength.empty() ? 160 : app->waterfallRidgeStrength[mi]);
          const COLORREF c = RGB(std::clamp(80 + s / 3, 0, 255), std::clamp(130 + s / 2, 0, 255),
                                 std::clamp(110 + s / 4, 0, 255));
          SetPixelV(hdc, x, y, c);
        }
      }
    }
  }

  // Dual waterfall view: wide context strip with current zoom window marker.
  RECT wide = {plot.left + 8, plot.top + 8, std::min(plot.right - 8, plot.left + 208),
               std::min(plot.bottom - 8, plot.top + 74)};
  if (app->showWideView && (wide.right - wide.left) > 80 && (wide.bottom - wide.top) > 24) {
    StretchDIBits(hdc, wide.left, wide.top, wide.right - wide.left, wide.bottom - wide.top,
                  0, 0, app->waterfallW, app->waterfallH, wfBase.data(), &bmi,
                  DIB_RGB_COLORS, SRCCOPY);
    HPEN wb = CreatePen(PS_SOLID, 1, RGB(120, 160, 194));
    auto oldWb = reinterpret_cast<HPEN>(SelectObject(hdc, wb));
    MoveToEx(hdc, wide.left, wide.top, nullptr);
    LineTo(hdc, wide.right - 1, wide.top);
    LineTo(hdc, wide.right - 1, wide.bottom - 1);
    LineTo(hdc, wide.left, wide.bottom - 1);
    LineTo(hdc, wide.left, wide.top);
    const int wx0 = wide.left + (srcX * (wide.right - wide.left)) / std::max(1, app->waterfallW);
    const int wx1 = wide.left + ((srcX + srcW) * (wide.right - wide.left)) / std::max(1, app->waterfallW);
    HPEN wv = CreatePen(PS_SOLID, 2, RGB(255, 224, 146));
    auto oldWv = reinterpret_cast<HPEN>(SelectObject(hdc, wv));
    MoveToEx(hdc, wx0, wide.top + 1, nullptr);
    LineTo(hdc, wx0, wide.bottom - 1);
    MoveToEx(hdc, wx1, wide.top + 1, nullptr);
    LineTo(hdc, wx1, wide.bottom - 1);
    SelectObject(hdc, oldWv);
    DeleteObject(wv);
    SelectObject(hdc, oldWb);
    DeleteObject(wb);
    RECT wt = {wide.left + 4, wide.top + 2, wide.right - 4, wide.top + 16};
    SetTextColor(hdc, RGB(220, 236, 250));
    DrawTextW(hdc, L"Wide view", -1, &wt, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
  }

  // Weak Signal Lens: local contrast/ridge boost around cursor or peak-lock.
  {
    bool hasAnchor = false;
    int ax = 0;
    int ay = 0;
    if (app->waterfallHoverActive && PtInRect(&plot, app->waterfallHoverPoint)) {
      ax = app->waterfallHoverPoint.x;
      ay = app->waterfallHoverPoint.y;
      hasAnchor = true;
    } else if (app->peakLockEnabled && app->peakLockBin >= 0) {
      const int lx = plot.left + ((plot.right - plot.left) * std::clamp(app->peakLockBin, 0, std::max(0, app->waterfallH - 1))) /
                                   std::max(1, app->waterfallH - 1);
      ax = lx;
      ay = (plot.top + plot.bottom) / 2;
      hasAnchor = true;
    }
    if (hasAnchor) {
      const int lensW = 136;
      const int lensH = 96;
      RECT lens = {std::max(static_cast<int>(plot.left) + 8,
                            std::min(static_cast<int>(plot.right) - lensW - 8, ax + 18)),
                   std::max(static_cast<int>(plot.top) + 8,
                            std::min(static_cast<int>(plot.bottom) - lensH - 8, ay - lensH / 2)),
                   0,
                   0};
      lens.right = lens.left + lensW;
      lens.bottom = lens.top + lensH;

      const int sw = 78;
      const int sh = 56;
      const int srcCxRaw = srcX +
                           (static_cast<int>(ax - static_cast<int>(plot.left)) * std::max(1, srcW - 1)) /
                               std::max<int>(1, static_cast<int>(plot.right - plot.left - 1));
      const int srcCyRaw =
          (static_cast<int>(ay - static_cast<int>(plot.top)) * std::max(1, app->waterfallH - 1)) /
          std::max<int>(1, static_cast<int>(plot.bottom - plot.top - 1));
      const int srcCx = std::clamp(srcCxRaw, 0, app->waterfallW - 1);
      const int srcCy = std::clamp(srcCyRaw, 0, app->waterfallH - 1);
      const int sx0 = std::clamp(srcCx - sw / 2, 0, std::max(0, app->waterfallW - sw));
      const int sy0 = std::clamp(srcCy - sh / 2, 0, std::max(0, app->waterfallH - sh));
      std::vector<std::uint8_t> lensRgb(static_cast<std::size_t>(sw * sh * 3), 0);
      for (int yy = 0; yy < sh; ++yy) {
        for (int xx = 0; xx < sw; ++xx) {
          const int sx = sx0 + xx;
          const int sy = sy0 + yy;
          const std::size_t sidx = static_cast<std::size_t>((sy * app->waterfallW + sx) * 3);
          const std::size_t didx = static_cast<std::size_t>((yy * sw + xx) * 3);
          float r = wfBase[sidx + 2] / 255.0f;
          float g = wfBase[sidx + 1] / 255.0f;
          float b = wfBase[sidx + 0] / 255.0f;
          const float yLum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
          float boost = std::pow(std::clamp(yLum, 0.0f, 1.0f), 0.72f);
          const float lensK = std::clamp(app->lensStrength, 0.50f, 2.00f);
          boost = std::clamp((boost - 0.08f) * (1.05f + 0.40f * lensK), 0.0f, 1.0f);
          if (!app->waterfallRidgeMask.empty() &&
              app->waterfallRidgeMask[static_cast<std::size_t>(sy * app->waterfallW + sx)] != 0) {
            boost = std::min(1.0f, boost + 0.22f);
          }
          lensRgb[didx + 2] = static_cast<std::uint8_t>(std::clamp(boost * 255.0f, 0.0f, 255.0f));
          lensRgb[didx + 1] = static_cast<std::uint8_t>(std::clamp(boost * 235.0f, 0.0f, 255.0f));
          lensRgb[didx + 0] = static_cast<std::uint8_t>(std::clamp(boost * 170.0f, 0.0f, 255.0f));
        }
      }
      BITMAPINFO lbi = {};
      lbi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      lbi.bmiHeader.biWidth = sw;
      lbi.bmiHeader.biHeight = -sh;
      lbi.bmiHeader.biPlanes = 1;
      lbi.bmiHeader.biBitCount = 24;
      lbi.bmiHeader.biCompression = BI_RGB;
      StretchDIBits(hdc, lens.left, lens.top, lens.right - lens.left, lens.bottom - lens.top,
                    0, 0, sw, sh, lensRgb.data(), &lbi, DIB_RGB_COLORS, SRCCOPY);
      HPEN lp = CreatePen(PS_SOLID, 1, RGB(132, 188, 242));
      auto oldLp = reinterpret_cast<HPEN>(SelectObject(hdc, lp));
      MoveToEx(hdc, lens.left, lens.top, nullptr);
      LineTo(hdc, lens.right - 1, lens.top);
      LineTo(hdc, lens.right - 1, lens.bottom - 1);
      LineTo(hdc, lens.left, lens.bottom - 1);
      LineTo(hdc, lens.left, lens.top);
      SelectObject(hdc, oldLp);
      DeleteObject(lp);
      RECT lt = {lens.left + 4, lens.top + 2, lens.right - 4, lens.top + 16};
      SetTextColor(hdc, RGB(218, 236, 252));
      DrawTextW(hdc, L"Weak Signal Lens", -1, &lt, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }
  }

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
  } else if (app && app->waterfallFrozen) {
    RECT pbox = {plot.right - 130, plot.top + 8, plot.right - 10, plot.top + 30};
    HBRUSH bb = CreateSolidBrush(RGB(28, 30, 18));
    FillRect(hdc, &pbox, bb);
    DeleteObject(bb);
    HPEN bp = CreatePen(PS_SOLID, 1, RGB(158, 142, 72));
    auto oldBp = reinterpret_cast<HPEN>(SelectObject(hdc, bp));
    MoveToEx(hdc, pbox.left, pbox.top, nullptr);
    LineTo(hdc, pbox.right - 1, pbox.top);
    LineTo(hdc, pbox.right - 1, pbox.bottom - 1);
    LineTo(hdc, pbox.left, pbox.bottom - 1);
    LineTo(hdc, pbox.left, pbox.top);
    SelectObject(hdc, oldBp);
    DeleteObject(bp);
    SetTextColor(hdc, RGB(250, 236, 172));
    DrawTextW(hdc, L"FROZEN", -1, &pbox, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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

  const float dur = (app->previewWav.sampleRate > 0)
                        ? (static_cast<float>(app->previewWav.samples.size()) /
                           static_cast<float>(std::max(1, app->previewWav.sampleRate)))
                        : 0.0f;
  const float viewStart = (static_cast<float>(srcX) / std::max(1, app->waterfallW - 1)) *
                          std::max(0.1f, dur);
  const float viewDur = (static_cast<float>(srcW) / std::max(1, app->waterfallW)) *
                        std::max(0.1f, dur);
  const float viewEnd = viewStart + viewDur;

  SetTextColor(hdc, RGB(166, 194, 220));
  RECT xLab = {plot.left, plot.bottom + 2, plot.right, plot.bottom + 20};
  DrawTextW(hdc, L"Time ->", -1, &xLab, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
  RECT yLab = {plot.left, plot.top - 2, plot.left + 140, plot.top + 16};
  DrawTextW(hdc, L"Frequency (kHz)", -1, &yLab, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

  RECT rp = {rc.right - 250, rc.top + 34, rc.right - 12, rc.top + 168};
  HBRUSH rbg = CreateSolidBrush(RGB(10, 18, 28));
  FillRect(hdc, &rp, rbg);
  DeleteObject(rbg);
  HPEN rbp = CreatePen(PS_SOLID, 1, RGB(74, 104, 134));
  auto oldRbp = reinterpret_cast<HPEN>(SelectObject(hdc, rbp));
  MoveToEx(hdc, rp.left, rp.top, nullptr);
  LineTo(hdc, rp.right - 1, rp.top);
  LineTo(hdc, rp.right - 1, rp.bottom - 1);
  LineTo(hdc, rp.left, rp.bottom - 1);
  LineTo(hdc, rp.left, rp.top);
  SelectObject(hdc, oldRbp);
  DeleteObject(rbp);

  SetTextColor(hdc, RGB(184, 212, 236));
  RECT rh = {rp.left + 8, rp.top + 4, rp.right - 8, rp.top + 22};
  DrawTextW(hdc, app->waterfallReadoutLocked ? L"RF Readout [LOCKED]" : L"RF Readout [LIVE]", -1,
            &rh, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
  std::wstringstream rs;
  if (app->waterfallReadoutValid) {
    rs << L"f: " << std::fixed << std::setprecision(3) << (app->waterfallReadoutFreqHz / 1000.0f)
       << L" kHz\n"
       << L"t: " << std::fixed << std::setprecision(2) << app->waterfallReadoutTimeSec << L" s\n"
       << L"dB: " << std::fixed << std::setprecision(1) << app->waterfallReadoutDb << L"\n"
       << L"NF: " << std::fixed << std::setprecision(1) << app->waterfallReadoutNoiseFloorDb << L" dB\n"
       << L"dB-NF: " << std::fixed << std::setprecision(1) << app->waterfallReadoutSnrDb << L" dB\n"
       << L"df(lock): " << std::showpos << std::fixed << std::setprecision(1)
       << app->waterfallReadoutDeltaFreqHz << L" Hz" << std::noshowpos << L"\n"
       << L"Toggle lock: L";
  } else {
    rs << L"f: --\nt: --\ndB: --\nNF: --\ndB-NF: --\ndf(lock): --\nToggle lock: L";
  }
  RECT rv = {rp.left + 10, rp.top + 26, rp.right - 8, rp.bottom - 8};
  SetTextColor(hdc, RGB(224, 236, 248));
  DrawTextW(hdc, rs.str().c_str(), -1, &rv, DT_LEFT | DT_TOP | DT_WORDBREAK);

  if (app && app->readoutSnrTrend.size() >= 3) {
    RECT sr = {rp.left + 8, rp.bottom + 2, rp.right - 8, rp.bottom + 22};
    HPEN sp = CreatePen(PS_SOLID, 1, RGB(132, 226, 178));
    auto oldSp = reinterpret_cast<HPEN>(SelectObject(hdc, sp));
    float minS = *std::min_element(app->readoutSnrTrend.begin(), app->readoutSnrTrend.end());
    float maxS = *std::max_element(app->readoutSnrTrend.begin(), app->readoutSnrTrend.end());
    if (maxS - minS < 0.5f) {
      maxS = minS + 0.5f;
    }
    for (std::size_t i = 0; i < app->readoutSnrTrend.size(); ++i) {
      const float tn = static_cast<float>(i) /
                       static_cast<float>(std::max<std::size_t>(1, app->readoutSnrTrend.size() - 1));
      const int x = sr.left + static_cast<int>(tn * (sr.right - sr.left));
      const float yn = (app->readoutSnrTrend[i] - minS) / (maxS - minS);
      const int y = sr.bottom - static_cast<int>(yn * (sr.bottom - sr.top));
      if (i == 0) {
        MoveToEx(hdc, x, y, nullptr);
      } else {
        LineTo(hdc, x, y);
      }
    }
    SelectObject(hdc, oldSp);
    DeleteObject(sp);
  }

  RECT ps = {rp.left, rp.bottom + 8, rp.right, rp.bottom + 52};
  HBRUSH pbg = CreateSolidBrush(RGB(10, 18, 28));
  FillRect(hdc, &ps, pbg);
  DeleteObject(pbg);
  HPEN ppen = CreatePen(PS_SOLID, 1, RGB(74, 104, 134));
  auto oldPp = reinterpret_cast<HPEN>(SelectObject(hdc, ppen));
  MoveToEx(hdc, ps.left, ps.top, nullptr);
  LineTo(hdc, ps.right - 1, ps.top);
  LineTo(hdc, ps.right - 1, ps.bottom - 1);
  LineTo(hdc, ps.left, ps.bottom - 1);
  LineTo(hdc, ps.left, ps.top);
  SelectObject(hdc, oldPp);
  DeleteObject(ppen);

  const auto pal = PaletteStopsByPreset(app);
  RECT grad = {ps.left + 8, ps.top + 20, ps.right - 8, ps.bottom - 8};
  for (int x = grad.left; x < grad.right; ++x) {
    const float n = static_cast<float>(x - grad.left) /
                    std::max<int>(1, static_cast<int>(grad.right - grad.left - 1));
    HPEN px = CreatePen(PS_SOLID, 1, SamplePalette(pal, n));
    auto oldPx = reinterpret_cast<HPEN>(SelectObject(hdc, px));
    MoveToEx(hdc, x, grad.top, nullptr);
    LineTo(hdc, x, grad.bottom);
    SelectObject(hdc, oldPx);
    DeleteObject(px);
  }
  std::wstring pname = L"Palette: HDSDR";
  if (app->palettePreset == 1) pname = L"Palette: SDR#";
  else if (app->palettePreset == 2) pname = L"Palette: CubicSDR";
  else if (app->palettePreset == 3) pname = L"Palette: Custom LUT";
  else if (app->palettePreset == 4) pname = L"Palette: Cividis";
  else if (app->palettePreset == 5) pname = L"Palette: Viridis";
  RECT pt = {ps.left + 8, ps.top + 2, ps.right - 8, ps.top + 18};
  SetTextColor(hdc, RGB(184, 212, 236));
  DrawTextW(hdc, pname.c_str(), -1, &pt, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

  if (app && !app->overlayRows.empty()) {
    RECT tpnl = {ps.left, ps.bottom + 6, ps.right, ps.bottom + 64};
    HBRUSH tbg = CreateSolidBrush(RGB(10, 18, 28));
    FillRect(hdc, &tpnl, tbg);
    DeleteObject(tbg);
    HPEN tpen = CreatePen(PS_SOLID, 1, RGB(74, 104, 134));
    auto oldTp = reinterpret_cast<HPEN>(SelectObject(hdc, tpen));
    MoveToEx(hdc, tpnl.left, tpnl.top, nullptr);
    LineTo(hdc, tpnl.right - 1, tpnl.top);
    LineTo(hdc, tpnl.right - 1, tpnl.bottom - 1);
    LineTo(hdc, tpnl.left, tpnl.bottom - 1);
    LineTo(hdc, tpnl.left, tpnl.top);
    SelectObject(hdc, oldTp);
    DeleteObject(tpen);

    const ndb::DecodeResult* best = nullptr;
    float bestC = -1.0f;
    for (const auto& r : app->overlayRows) {
      if (app->selectedTrackId >= 0 && r.trackId != app->selectedTrackId) {
        continue;
      }
      if (r.confidence > bestC) {
        bestC = r.confidence;
        best = &r;
      }
    }
    if (best) {
      RECT trc = {tpnl.left + 8, tpnl.top + 2, tpnl.right - 8, tpnl.top + 16};
      std::wstring tname = L"Track conf timeline: " + ToWide(!best->plausibleId.empty() ? best->plausibleId : best->text);
      SetTextColor(hdc, RGB(182, 216, 240));
      DrawTextW(hdc, tname.c_str(), -1, &trc, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
      RECT plotc = {tpnl.left + 8, tpnl.top + 18, tpnl.right - 8, tpnl.bottom - 8};
      HPEN cp = CreatePen(PS_SOLID, 2, RGB(130, 255, 170));
      auto oldCp = reinterpret_cast<HPEN>(SelectObject(hdc, cp));
      const float x0n = std::clamp((best->startSec - viewStart) / std::max(0.1f, viewDur), 0.0f, 1.0f);
      const float x1n = std::clamp((best->endSec - viewStart) / std::max(0.1f, viewDur), 0.0f, 1.0f);
      const int x0 = plotc.left + static_cast<int>(x0n * (plotc.right - plotc.left));
      const int x1 = plotc.left + static_cast<int>(x1n * (plotc.right - plotc.left));
      const int yv = plotc.bottom - static_cast<int>(std::clamp(best->confidence, 0.0f, 1.0f) * (plotc.bottom - plotc.top));
      MoveToEx(hdc, x0, yv, nullptr);
      LineTo(hdc, std::max(x0 + 1, x1), yv);
      SelectObject(hdc, oldCp);
      DeleteObject(cp);
    }
  }

  if (app && app->waterfallW > 0 && app->waterfallH > 0 && !app->waterfallDbRender.empty() &&
      app->waterfallHoverActive && PtInRect(&plot, app->waterfallHoverPoint)) {
    RECT hp = {ps.left, ps.bottom + 68, ps.right, ps.bottom + 136};
    HBRUSH hbg = CreateSolidBrush(RGB(10, 18, 28));
    FillRect(hdc, &hp, hbg);
    DeleteObject(hbg);
    HPEN hpn = CreatePen(PS_SOLID, 1, RGB(74, 104, 134));
    auto oldHp = reinterpret_cast<HPEN>(SelectObject(hdc, hpn));
    MoveToEx(hdc, hp.left, hp.top, nullptr);
    LineTo(hdc, hp.right - 1, hp.top);
    LineTo(hdc, hp.right - 1, hp.bottom - 1);
    LineTo(hdc, hp.left, hp.bottom - 1);
    LineTo(hdc, hp.left, hp.top);
    SelectObject(hdc, oldHp);
    DeleteObject(hpn);

    int srcXh = 0;
    int srcWh = app->waterfallW;
    ComputeWaterfallSourceWindow(app, &srcXh, &srcWh);
    const float xn = static_cast<float>(app->waterfallHoverPoint.x - plot.left) /
                     std::max<int>(1, static_cast<int>(plot.right - plot.left - 1));
    const float yn = static_cast<float>(app->waterfallHoverPoint.y - plot.top) /
                     std::max<int>(1, static_cast<int>(plot.bottom - plot.top - 1));
    const int xCol = std::clamp(srcXh + static_cast<int>(xn * std::max(1, srcWh - 1)), 0, app->waterfallW - 1);
    const int yRow = std::clamp(static_cast<int>(yn * std::max(1, app->waterfallH - 1)), 0, app->waterfallH - 1);

    const int bins = 24;
    std::vector<int> histR(static_cast<std::size_t>(bins), 0);
    std::vector<int> histC(static_cast<std::size_t>(bins), 0);
    const float minDb = app->panMinDb;
    const float maxDb = app->panMaxDb;
    for (int x = 0; x < app->waterfallW; ++x) {
      const float v = app->waterfallDbRender[static_cast<std::size_t>(yRow * app->waterfallW + x)];
      const int bi = std::clamp(static_cast<int>(((v - minDb) / std::max(1.0f, (maxDb - minDb))) * bins), 0, bins - 1);
      histR[static_cast<std::size_t>(bi)]++;
    }
    for (int y = 0; y < app->waterfallH; ++y) {
      const float v = app->waterfallDbRender[static_cast<std::size_t>(y * app->waterfallW + xCol)];
      const int bi = std::clamp(static_cast<int>(((v - minDb) / std::max(1.0f, (maxDb - minDb))) * bins), 0, bins - 1);
      histC[static_cast<std::size_t>(bi)]++;
    }
    const int maxR = std::max(1, *std::max_element(histR.begin(), histR.end()));
    const int maxC = std::max(1, *std::max_element(histC.begin(), histC.end()));
    RECT hpr = {hp.left + 8, hp.top + 16, hp.right - 8, hp.bottom - 8};
    for (int i = 0; i < bins; ++i) {
      const int x = hpr.left + (i * (hpr.right - hpr.left)) / bins;
      const int w = std::max<int>(1, static_cast<int>((hpr.right - hpr.left) / bins - 1));
      const int hr = (histR[static_cast<std::size_t>(i)] * (hpr.bottom - hpr.top)) / maxR;
      const int hc = (histC[static_cast<std::size_t>(i)] * (hpr.bottom - hpr.top)) / maxC;
      RECT rr = {x, hpr.bottom - hr, x + w / 2, hpr.bottom};
      RECT rc2 = {x + w / 2, hpr.bottom - hc, x + w, hpr.bottom};
      HBRUSH br = CreateSolidBrush(RGB(108, 190, 255));
      FillRect(hdc, &rr, br);
      DeleteObject(br);
      HBRUSH bc = CreateSolidBrush(RGB(255, 182, 126));
      FillRect(hdc, &rc2, bc);
      DeleteObject(bc);
    }
    RECT ht = {hp.left + 8, hp.top + 2, hp.right - 8, hp.top + 14};
    SetTextColor(hdc, RGB(188, 220, 242));
    DrawTextW(hdc, L"Histogram row/col", -1, &ht, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
  }

  const float nyqTicks = app && app->previewWav.sampleRate > 0
                             ? 0.5f * static_cast<float>(app->previewWav.sampleRate)
                             : 4000.0f;
  const float fMinTicks = 80.0f;
  const float fMaxTicks = std::min(2200.0f, nyqTicks - 20.0f);
  const int tickCount = 5;
  const int minorPerMajor = (srcW < std::max(1, app->waterfallW / 2)) ? 6 : 4;
  HPEN tickPen = CreatePen(PS_SOLID, 1, RGB(110, 140, 168));
  auto oldTickPen = reinterpret_cast<HPEN>(SelectObject(hdc, tickPen));
  SetTextColor(hdc, RGB(176, 202, 226));
  for (int i = 0; i < tickCount; ++i) {
    const float u = static_cast<float>(i) / static_cast<float>(tickCount - 1);
    const int y = plot.bottom - static_cast<int>(u * (plot.bottom - plot.top));
    MoveToEx(hdc, plot.left, y, nullptr);
    LineTo(hdc, plot.left + 6, y);
    const float fHz = fMinTicks + u * (fMaxTicks - fMinTicks);
    const std::wstring lbl = FormatFreqSmart(fHz);
    RECT tr = {plot.left + 10, y - 9, plot.left + 122, y + 9};
    DrawTextW(hdc, lbl.c_str(), -1, &tr, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
  }

  HPEN minorPen = CreatePen(PS_SOLID, 1, RGB(74, 102, 128));
  auto oldMinorPen = reinterpret_cast<HPEN>(SelectObject(hdc, minorPen));
  for (int i = 0; i < tickCount - 1; ++i) {
    const int y0 = plot.bottom - ((plot.bottom - plot.top) * i) / (tickCount - 1);
    const int y1 = plot.bottom - ((plot.bottom - plot.top) * (i + 1)) / (tickCount - 1);
    for (int m = 1; m <= minorPerMajor; ++m) {
      const float a = static_cast<float>(m) / static_cast<float>(minorPerMajor + 1);
      const int y = y0 + static_cast<int>(a * (y1 - y0));
      MoveToEx(hdc, plot.left, y, nullptr);
      LineTo(hdc, plot.left + 4, y);
    }
  }
  SelectObject(hdc, oldMinorPen);
  DeleteObject(minorPen);

  SelectObject(hdc, oldTickPen);
  DeleteObject(tickPen);

  // Mini-map timeline + viewport (SDR-style overview for long files)
  if (app->waterfallW > 4) {
    RECT map = {plot.left, plot.bottom - 12, plot.right, plot.bottom - 2};
    HBRUSH mbg = CreateSolidBrush(RGB(8, 14, 22));
    FillRect(hdc, &map, mbg);
    DeleteObject(mbg);
    HPEN mb = CreatePen(PS_SOLID, 1, RGB(66, 92, 118));
    auto oldMb = reinterpret_cast<HPEN>(SelectObject(hdc, mb));
    MoveToEx(hdc, map.left, map.top, nullptr);
    LineTo(hdc, map.right - 1, map.top);
    LineTo(hdc, map.right - 1, map.bottom - 1);
    LineTo(hdc, map.left, map.bottom - 1);
    LineTo(hdc, map.left, map.top);
    SelectObject(hdc, oldMb);
    DeleteObject(mb);

    // overview energy trace (top-of-band proxy)
    if (!app->waterfallDbRender.empty()) {
      HPEN tr = CreatePen(PS_SOLID, 1, RGB(96, 168, 232));
      auto oldTr = reinterpret_cast<HPEN>(SelectObject(hdc, tr));
      for (int x = map.left; x < map.right; ++x) {
        const float xn = static_cast<float>(x - map.left) /
                         std::max<int>(1, static_cast<int>(map.right - map.left - 1));
        const int col = std::clamp(static_cast<int>(xn * (app->waterfallW - 1)), 0, app->waterfallW - 1);
        float maxDb = -120.0f;
        for (int y = 0; y < app->waterfallH; y += std::max(1, app->waterfallH / 42)) {
          maxDb = std::max(maxDb, app->waterfallDbRender[static_cast<std::size_t>(y * app->waterfallW + col)]);
        }
        const float yn = std::clamp((maxDb - app->panMinDb) / std::max(1.0f, app->panMaxDb - app->panMinDb),
                                    0.0f, 1.0f);
        const int yy = map.bottom - 1 - static_cast<int>(yn * (map.bottom - map.top - 2));
        if (x == map.left) {
          MoveToEx(hdc, x, yy, nullptr);
        } else {
          LineTo(hdc, x, yy);
        }
      }
      SelectObject(hdc, oldTr);
      DeleteObject(tr);
    }

    // visible viewport window
    const int vx0 = map.left + (srcX * (map.right - map.left)) / std::max(1, app->waterfallW);
    const int vx1 = map.left + ((srcX + srcW) * (map.right - map.left)) / std::max(1, app->waterfallW);
    RECT vp = {std::clamp<int>(vx0, static_cast<int>(map.left), static_cast<int>(map.right - 1)),
               static_cast<LONG>(map.top + 1),
               std::clamp<int>(vx1, static_cast<int>(map.left + 1), static_cast<int>(map.right)),
               static_cast<LONG>(map.bottom - 1)};
    HBRUSH vb = CreateSolidBrush(RGB(255, 240, 96));
    FillRect(hdc, &vp, vb);
    DeleteObject(vb);
    HPEN vpPen = CreatePen(PS_SOLID, 1, RGB(255, 250, 160));
    auto oldVp = reinterpret_cast<HPEN>(SelectObject(hdc, vpPen));
    MoveToEx(hdc, vp.left, vp.top, nullptr);
    LineTo(hdc, vp.right - 1, vp.top);
    LineTo(hdc, vp.right - 1, vp.bottom - 1);
    LineTo(hdc, vp.left, vp.bottom - 1);
    LineTo(hdc, vp.left, vp.top);
    SelectObject(hdc, oldVp);
    DeleteObject(vpPen);

    if (!app->bookmarksSec.empty() && app->previewWav.sampleRate > 0) {
      HPEN bmManual = CreatePen(PS_SOLID, 1, RGB(255, 178, 88));
      HPEN bmAutoHi = CreatePen(PS_SOLID, 1, RGB(112, 236, 160));
      HPEN bmAutoMid = CreatePen(PS_SOLID, 1, RGB(120, 212, 255));
      HPEN bmAutoLow = CreatePen(PS_SOLID, 1, RGB(146, 160, 255));
      auto oldBm = reinterpret_cast<HPEN>(SelectObject(hdc, bmManual));
      int bmIdx = 0;
      int shownCount = 0;
      int autoCount = 0;
      int autoHi = 0;
      int autoMid = 0;
      int autoLow = 0;
      for (std::size_t i = 0; i < app->bookmarksSec.size(); ++i) {
        if (!BookmarkVisible(app, i)) continue;
        const float tsec = app->bookmarksSec[i];
        const bool isAuto = (i < app->bookmarkAuto.size() && app->bookmarkAuto[i] != 0);
        const float conf = (i < app->bookmarkConfidence.size()) ? app->bookmarkConfidence[i] : (isAuto ? 0.70f : 1.00f);
        if (!isAuto) {
          SelectObject(hdc, bmManual);
        } else if (conf >= app->autoBookmarkHighThreshold) {
          SelectObject(hdc, bmAutoHi);
          ++autoHi;
        } else if (conf >= app->autoBookmarkMidThreshold) {
          SelectObject(hdc, bmAutoMid);
          ++autoMid;
        } else {
          SelectObject(hdc, bmAutoLow);
          ++autoLow;
        }
        const float tn = std::clamp(tsec / std::max(0.1f, dur), 0.0f, 1.0f);
        const int xMap = map.left + static_cast<int>(tn * (map.right - map.left));
        MoveToEx(hdc, xMap, map.top + 1, nullptr);
        LineTo(hdc, xMap, map.bottom - 1);

        if (bmIdx < 9) {
          wchar_t nbuf[4] = {};
          swprintf(nbuf, 4, L"%d", bmIdx + 1);
          RECT nr = {xMap - 6, map.top - 14, xMap + 12, map.top - 1};
          DrawTextW(hdc, nbuf, -1, &nr, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        }

        if (tsec >= viewStart && tsec <= viewEnd) {
          const float vxn = (tsec - viewStart) / std::max(0.1f, viewDur);
          const int xPlot = plot.left + static_cast<int>(vxn * (plot.right - plot.left));
          MoveToEx(hdc, xPlot, plot.top + 2, nullptr);
          LineTo(hdc, xPlot, plot.bottom - 14);
        }
        ++shownCount;
        autoCount += isAuto ? 1 : 0;
        ++bmIdx;
      }
      SelectObject(hdc, oldBm);
      DeleteObject(bmManual);
      DeleteObject(bmAutoHi);
      DeleteObject(bmAutoMid);
      DeleteObject(bmAutoLow);

      std::wstringstream bss;
      bss << L"Bookmarks: " << shownCount << L" (M" << (shownCount - autoCount) << L"/A" << autoCount
          << L" h/m/l " << autoHi << L"/" << autoMid << L"/" << autoLow << L")  Auto: "
          << (app->autoBookmarkEnabled ? L"ON" : L"OFF") << L" @"
          << std::fixed << std::setprecision(2) << app->autoBookmarkMinConfidence << L" M/H "
          << app->autoBookmarkMidThreshold << L"/" << app->autoBookmarkHighThreshold
          << L"  ShowA: "
          << (app->showAutoBookmarks ? L"ON" : L"OFF");
      RECT br = {map.left, map.top - 16, map.left + 520, map.top - 1};
      SetTextColor(hdc, RGB(255, 196, 120));
      DrawTextW(hdc, bss.str().c_str(), -1, &br, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }
    RECT hk = {map.left, map.top - 16, map.right, map.top - 1};
    SetTextColor(hdc, RGB(178, 206, 228));
    DrawTextW(hdc, L"Hotkeys: A show auto, B/C add-clear, D del, F freeze, G dotdash, X diff, L lock readout, M manual notch, O ridge, V wide, F6/F7/F8 visual presets, Ctrl+Click set B cursor, N/P nav, 1..9 jump, E/I exp-imp, Shift+Drag zoom box, Ctrl+R reset (Shift=skip prompt)",
              -1, &hk,
              DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
  }

  // RF Zoom lane around peak-lock (local magnifier like SDR narrow view)
  if (app->peakLockEnabled && app->peakLockBin >= 0 && srcW > 4 && !app->waterfallRgb.empty()) {
    RECT zr = {plot.right - 190, plot.top + 26, plot.right - 10, plot.bottom - 24};
    HBRUSH zbg = CreateSolidBrush(RGB(6, 12, 20));
    FillRect(hdc, &zr, zbg);
    DeleteObject(zbg);
    HPEN zb = CreatePen(PS_SOLID, 1, RGB(72, 106, 140));
    auto oldZ = reinterpret_cast<HPEN>(SelectObject(hdc, zb));
    MoveToEx(hdc, zr.left, zr.top, nullptr);
    LineTo(hdc, zr.right - 1, zr.top);
    LineTo(hdc, zr.right - 1, zr.bottom - 1);
    LineTo(hdc, zr.left, zr.bottom - 1);
    LineTo(hdc, zr.left, zr.top);
    SelectObject(hdc, oldZ);
    DeleteObject(zb);

    const int spanBins = std::max(18, std::min(44, app->waterfallH / 12));
    const int row0 = std::max(0, app->peakLockBin - spanBins);
    const int row1 = std::min(app->waterfallH - 1, app->peakLockBin + spanBins);
    const int srcH = std::max(1, row1 - row0 + 1);
    const int srcCols = std::max(1, srcW);
    std::vector<std::uint8_t> lane(static_cast<std::size_t>(srcCols * srcH * 3), 0);
    for (int yy = 0; yy < srcH; ++yy) {
      const int row = row0 + yy;
      for (int xx = 0; xx < srcCols; ++xx) {
        const int col = std::clamp(srcX + xx, 0, app->waterfallW - 1);
        const std::size_t sidx = static_cast<std::size_t>((row * app->waterfallW + col) * 3);
        const std::size_t didx = static_cast<std::size_t>((yy * srcCols + xx) * 3);
        lane[didx + 0] = app->waterfallRgb[sidx + 0];
        lane[didx + 1] = app->waterfallRgb[sidx + 1];
        lane[didx + 2] = app->waterfallRgb[sidx + 2];
      }
    }

    BITMAPINFO lbi = {};
    lbi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    lbi.bmiHeader.biWidth = srcCols;
    lbi.bmiHeader.biHeight = -srcH;
    lbi.bmiHeader.biPlanes = 1;
    lbi.bmiHeader.biBitCount = 24;
    lbi.bmiHeader.biCompression = BI_RGB;
    StretchDIBits(hdc, zr.left + 6, zr.top + 16, zr.right - zr.left - 12, zr.bottom - zr.top - 24,
                  0, 0, srcCols, srcH, lane.data(), &lbi, DIB_RGB_COLORS, SRCCOPY);

    const int cy = zr.top + 16 + ((zr.bottom - zr.top - 24) * (app->peakLockBin - row0)) /
                                std::max(1, srcH - 1);
    HPEN lk = CreatePen(PS_SOLID, 2, RGB(170, 255, 186));
    auto oldL = reinterpret_cast<HPEN>(SelectObject(hdc, lk));
    MoveToEx(hdc, zr.left + 6, cy, nullptr);
    LineTo(hdc, zr.right - 6, cy);
    SelectObject(hdc, oldL);
    DeleteObject(lk);

    RECT zt = {zr.left + 8, zr.top + 2, zr.right - 8, zr.top + 16};
    std::wstringstream zs;
    zs << L"RF Zoom " << std::fixed << std::setprecision(3) << (app->peakLockHz / 1000.0f)
       << L" kHz";
    SetTextColor(hdc, RGB(170, 230, 198));
    DrawTextW(hdc, zs.str().c_str(), -1, &zt, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
  }

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

  if (app && app->compareCursorBValid && PtInRect(&plot, app->compareCursorBPt)) {
    HPEN cb = CreatePen(PS_DASH, 1, RGB(255, 164, 170));
    auto oldCb = reinterpret_cast<HPEN>(SelectObject(hdc, cb));
    MoveToEx(hdc, plot.left, app->compareCursorBPt.y, nullptr);
    LineTo(hdc, plot.right, app->compareCursorBPt.y);
    MoveToEx(hdc, app->compareCursorBPt.x, plot.top, nullptr);
    LineTo(hdc, app->compareCursorBPt.x, plot.bottom);
    SelectObject(hdc, oldCb);
    DeleteObject(cb);

    if (app->waterfallReadoutValid) {
      const float df = app->waterfallReadoutFreqHz - app->compareCursorBFreqHz;
      const float dt = app->waterfallReadoutTimeSec - app->compareCursorBTimeSec;
      const float dd = app->waterfallReadoutDb - app->compareCursorBDb;
      RECT dr = {plot.left + 8, plot.bottom - 30, std::min(plot.right - 8, plot.left + 360), plot.bottom - 12};
      std::wstringstream ds;
      ds << L"A-B: df=" << std::showpos << std::fixed << std::setprecision(1) << df
         << L" Hz  dt=" << std::setprecision(2) << dt << L" s  ddB=" << std::setprecision(1)
         << dd << std::noshowpos;
      SetTextColor(hdc, RGB(255, 186, 196));
      DrawTextW(hdc, ds.str().c_str(), -1, &dr, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }
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
    SetBkMode(hdc, TRANSPARENT);
    int lastLblX = -9999;
    int lastLblY = -9999;
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
      COLORREF cc = RGB(255, 190, 120);
      std::wstring q = L"C";
      int thick = 1;
      if (r.confidence >= 0.85f) {
        cc = RGB(130, 255, 170);
        q = L"A";
        thick = 3;
      } else if (r.confidence >= 0.65f) {
        cc = RGB(248, 226, 116);
        q = L"B";
        thick = 2;
      }
      const bool pinnedOther = (app->selectedTrackId >= 0 && r.trackId != app->selectedTrackId);
      if (pinnedOther) {
        cc = RGB(GetRValue(cc) / 2, GetGValue(cc) / 2, GetBValue(cc) / 2);
        thick = 1;
      }
      HPEN trk = CreatePen(PS_SOLID, thick, cc);
      auto oldT = reinterpret_cast<HPEN>(SelectObject(hdc, trk));
      MoveToEx(hdc, x0, y, nullptr);
      LineTo(hdc, std::max(x0 + 1, x1), y);
      SelectObject(hdc, oldT);
      DeleteObject(trk);
      const int lblX = x0 + 4;
      const int lblY = y - 14;
      const bool nearOther = (std::abs(lblX - lastLblX) < 64 && std::abs(lblY - lastLblY) < 16);
      const bool showLbl = (r.confidence >= 0.72f) || !nearOther;
      if (showLbl) {
        RECT lbl = {lblX, lblY, std::min<int>(plot.right - 4, x0 + 120), y + 2};
        const std::wstring tag = ToWide(!r.plausibleId.empty() ? r.plausibleId : r.text) + L" [" + q + L"]";
        SetTextColor(hdc, cc);
        DrawTextW(hdc, tag.c_str(), -1, &lbl, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        lastLblX = lblX;
        lastLblY = lblY;
      }
    }
  }

  if (app && app->manualNotchEnabled && !app->manualNotches.empty()) {
    HPEN np = CreatePen(PS_SOLID, 1, RGB(230, 118, 86));
    HPEN npSel = CreatePen(PS_SOLID, 2, RGB(255, 176, 120));
    auto oldN = reinterpret_cast<HPEN>(SelectObject(hdc, np));
    SetBkMode(hdc, TRANSPARENT);
    for (std::size_t i = 0; i < app->manualNotches.size(); ++i) {
      const auto& n = app->manualNotches[i];
      const int y = FreqToY(app, plot, n.freqHz);
      const float fMin = WaterfallFreqRangeMinHz(app);
      const float fMax = WaterfallFreqRangeMaxHz(app);
      const float widthFrac = n.widthHz / std::max(1.0f, (fMax - fMin));
      const int halfH = std::max(1, static_cast<int>(std::round(widthFrac * (plot.bottom - plot.top) * 0.5f)));
      RECT nb = {plot.left + 1, y - halfH, plot.right - 1, y + halfH};
      HBRUSH fill = CreateSolidBrush((static_cast<int>(i) == app->activeManualNotch)
                                         ? RGB(82, 42, 30)
                                         : RGB(54, 28, 22));
      FillRect(hdc, &nb, fill);
      DeleteObject(fill);

      SelectObject(hdc, (static_cast<int>(i) == app->activeManualNotch) ? npSel : np);
      MoveToEx(hdc, plot.left + 1, y, nullptr);
      LineTo(hdc, plot.right - 1, y);

      if (i < 9) {
        wchar_t idbuf[6] = {};
        swprintf(idbuf, 6, L"N%u", static_cast<unsigned>(i + 1));
        RECT lr = {plot.left + 6, y - 10, plot.left + 42, y + 10};
        SetTextColor(hdc, RGB(255, 212, 172));
        DrawTextW(hdc, idbuf, -1, &lr, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
      }
    }
    SelectObject(hdc, oldN);
    DeleteObject(np);
    DeleteObject(npSel);

    RECT nr = {plot.left + 8, plot.top + 2, plot.left + 420, plot.top + 18};
    std::wstringstream ns;
    ns << L"Manual Notch ON: " << app->manualNotches.size()
       << L"  (Alt+Click add/select, Alt+Drag move, Shift+Wheel width, Del remove)";
    SetTextColor(hdc, RGB(248, 188, 148));
    DrawTextW(hdc, ns.str().c_str(), -1, &nr, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
  }

  if (app && app->manualNotchEnabled && app->notchGhostActive) {
    const int y = FreqToY(app, plot, app->notchGhostFreqHz);
    const float fMin = WaterfallFreqRangeMinHz(app);
    const float fMax = WaterfallFreqRangeMaxHz(app);
    const float widthFrac = app->notchGhostWidthHz / std::max(1.0f, (fMax - fMin));
    const int halfH = std::max(1, static_cast<int>(std::round(widthFrac * (plot.bottom - plot.top) * 0.5f)));
    RECT nb = {plot.left + 1, y - halfH, plot.right - 1, y + halfH};
    HBRUSH fill = CreateSolidBrush(RGB(72, 74, 52));
    FillRect(hdc, &nb, fill);
    DeleteObject(fill);
    HPEN gp = CreatePen(PS_DOT, 1, RGB(255, 230, 148));
    auto oldGp = reinterpret_cast<HPEN>(SelectObject(hdc, gp));
    MoveToEx(hdc, plot.left + 1, y, nullptr);
    LineTo(hdc, plot.right - 1, y);
    SelectObject(hdc, oldGp);
    DeleteObject(gp);
    RECT gr = {plot.left + 8, y - 12, plot.left + 210, y + 2};
    SetTextColor(hdc, RGB(255, 236, 168));
    DrawTextW(hdc, L"Notch ghost preview", -1, &gr, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
  }

  if (app && app->dotDashAssistEnabled && app->peakLockEnabled && app->peakLockHz > 0.0f) {
    const int yLock = FreqToY(app, plot, app->peakLockHz);
    const int dotPx = std::max<int>(8, static_cast<int>((plot.right - plot.left) / 42));
    const int gapPx = std::max(4, dotPx / 2);
    const int dashPx = dotPx * 3;
    HPEN dp = CreatePen(PS_SOLID, 1, RGB(164, 232, 255));
    auto oldDp = reinterpret_cast<HPEN>(SelectObject(hdc, dp));
    for (int x = plot.left + 2; x + dotPx < plot.right - 2; x += (dotPx + gapPx) * 2) {
      MoveToEx(hdc, x, yLock - 2, nullptr);
      LineTo(hdc, x + dotPx, yLock - 2);
      const int x2 = x + dotPx + gapPx;
      MoveToEx(hdc, x2, yLock + 2, nullptr);
      LineTo(hdc, std::min<int>(static_cast<int>(plot.right) - 2, x2 + dashPx), yLock + 2);
    }
    SelectObject(hdc, oldDp);
    DeleteObject(dp);
    RECT dr = {plot.right - 180, yLock - 14, plot.right - 6, yLock + 2};
    SetTextColor(hdc, RGB(186, 236, 252));
    DrawTextW(hdc, L"Dot/Dash Assist", -1, &dr, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
  }

  if (app->waterfallZoomBoxActive) {
    RECT zb = {std::min(app->waterfallZoomBoxStart.x, app->waterfallZoomBoxEnd.x),
               std::min(app->waterfallZoomBoxStart.y, app->waterfallZoomBoxEnd.y),
               std::max(app->waterfallZoomBoxStart.x, app->waterfallZoomBoxEnd.x),
               std::max(app->waterfallZoomBoxStart.y, app->waterfallZoomBoxEnd.y)};
    zb.left = std::clamp(static_cast<int>(zb.left), static_cast<int>(plot.left),
                         static_cast<int>(plot.right - 1));
    zb.right = std::clamp(static_cast<int>(zb.right), static_cast<int>(plot.left + 1),
                          static_cast<int>(plot.right));
    zb.top = std::clamp(static_cast<int>(zb.top), static_cast<int>(plot.top),
                        static_cast<int>(plot.bottom - 1));
    zb.bottom = std::clamp(static_cast<int>(zb.bottom), static_cast<int>(plot.top + 1),
                           static_cast<int>(plot.bottom));
    HBRUSH zbb = CreateSolidBrush(RGB(38, 68, 94));
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 70, 0};
    HDC memdc = CreateCompatibleDC(hdc);
    const int zbW = std::max(1, static_cast<int>(zb.right - zb.left));
    const int zbH = std::max(1, static_cast<int>(zb.bottom - zb.top));
    HBITMAP bmp = CreateCompatibleBitmap(hdc, zbW, zbH);
    auto oldBmp = reinterpret_cast<HBITMAP>(SelectObject(memdc, bmp));
    RECT fr = {0, 0, zbW, zbH};
    FillRect(memdc, &fr, zbb);
    AlphaBlend(hdc, zb.left, zb.top, fr.right, fr.bottom, memdc, 0, 0, fr.right, fr.bottom, bf);
    SelectObject(memdc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(memdc);
    DeleteObject(zbb);

    HPEN zpen = CreatePen(PS_DASH, 1, RGB(164, 212, 255));
    auto oldZp = reinterpret_cast<HPEN>(SelectObject(hdc, zpen));
    MoveToEx(hdc, zb.left, zb.top, nullptr);
    LineTo(hdc, zb.right - 1, zb.top);
    LineTo(hdc, zb.right - 1, zb.bottom - 1);
    LineTo(hdc, zb.left, zb.bottom - 1);
    LineTo(hdc, zb.left, zb.top);
    SelectObject(hdc, oldZp);
    DeleteObject(zpen);
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
  float freqHz = 0.0f;
  float timeSec = 0.0f;
  float db = -120.0f;
  float snrDb = 0.0f;
  float noiseFloorDb = -120.0f;
  float deltaFreqHz = 0.0f;
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
  int yPix = mouse.y;
  const int tickCount = 5;
  const int snapPx = 7;
  for (int i = 0; i < tickCount; ++i) {
    const int yMajor = plot.bottom - ((plot.bottom - plot.top) * i) / (tickCount - 1);
    if (std::abs(yPix - yMajor) <= snapPx) {
      yPix = yMajor;
      break;
    }
  }
  const float ynSnap = static_cast<float>(yPix - plot.top) /
                       std::max<int>(1, static_cast<int>(plot.bottom - plot.top));
  const int yRow = std::clamp(static_cast<int>(ynSnap * app->waterfallH), 0, app->waterfallH - 1);

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
  if (!app->waterfallDbRender.empty()) {
    std::vector<float> col;
    col.reserve(static_cast<std::size_t>(app->waterfallH));
    for (int yy = 0; yy < app->waterfallH; ++yy) {
      col.push_back(app->waterfallDbRender[static_cast<std::size_t>(yy * app->waterfallW + xCol)]);
    }
    const std::size_t qi = static_cast<std::size_t>(0.22f *
                                                    static_cast<float>(std::max<std::size_t>(1, col.size() - 1)));
    std::nth_element(col.begin(), col.begin() + static_cast<std::ptrdiff_t>(qi), col.end());
    noiseDb = col[qi];
  }
  float snrDb = db - noiseDb;
  const float deltaFHz = app->peakLockEnabled ? (fHz - app->peakLockHz) : 0.0f;

  std::wstringstream ss;
  ss << L"Waterfall Readout\n"
     << L"f: " << std::fixed << std::setprecision(3) << (fHz / 1000.0f) << L" kHz\n"
     << L"t: " << std::fixed << std::setprecision(2) << tSec << L" s\n"
     << L"dB: " << std::fixed << std::setprecision(1) << db << L"\n"
     << L"NF: " << std::fixed << std::setprecision(1) << noiseDb << L" dB\n"
     << L"dB-NF: " << std::fixed << std::setprecision(1) << snrDb << L" dB\n"
     << L"df(lock): " << std::showpos << std::fixed << std::setprecision(1)
     << (deltaFHz / 1.0f) << L" Hz" << std::noshowpos;

  out.ok = true;
  out.pt = POINT{mouse.x, yPix};
  out.text = ss.str();
  out.freqHz = fHz;
  out.timeSec = tSec;
  out.db = db;
  out.snrDb = snrDb;
  out.noiseFloorDb = noiseDb;
  out.deltaFreqHz = deltaFHz;
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
  app->bookmarksSec.clear();
  app->bookmarkAuto.clear();
  app->bookmarkConfidence.clear();
  app->readoutSnrTrend.clear();
  app->waterfallReadoutLocked = false;
  app->waterfallReadoutValid = false;
  InvalidateWaterfallCache(app);
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
    ClampManualNotchesToRange(app);
    SortAndMergeManualNotches(app);
    app->activeManualNotch = app->manualNotches.empty() ? -1 : std::clamp(app->activeManualNotch, 0, static_cast<int>(app->manualNotches.size()) - 1);
  }
  InvalidateRect(app->chartPanel, nullptr, TRUE);
}

void InvalidateWaterfallCache(AppState* app) {
  if (!app) {
    return;
  }
  app->waterfallRgb.clear();
  app->waterfallDiffRgb.clear();
  app->waterfallRidgeMask.clear();
  app->waterfallRidgeStrength.clear();
  app->waterfallDbRender.clear();
  app->panInstantDb.clear();
  app->panAvgDb.clear();
  app->panSlowDb.clear();
  app->panPeakDb.clear();
  app->panLastCol = -1;
  app->waterfallW = 0;
  app->waterfallH = 0;
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
  const bool manualNotchEnabled = app->manualNotchEnabled;
  std::vector<float> manualNotchFreqHz;
  std::vector<float> manualNotchWidthHz;
  manualNotchFreqHz.reserve(app->manualNotches.size());
  manualNotchWidthHz.reserve(app->manualNotches.size());
  for (const auto& n : app->manualNotches) {
    manualNotchFreqHz.push_back(n.freqHz);
    manualNotchWidthHz.push_back(n.widthHz);
  }
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
                             requirePrior, manualNotchEnabled, manualNotchFreqHz,
                             manualNotchWidthHz]() {
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
    cfg.enableManualNotch = manualNotchEnabled;
    cfg.manualNotchFreqHz = manualNotchFreqHz;
    cfg.manualNotchWidthHz = manualNotchWidthHz;
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
    if (app->autoBookmarkEnabled) {
        const std::size_t before = app->bookmarksSec.size();
      for (const auto& r : result->decodedRows) {
        if (r.confidence >= app->autoBookmarkMinConfidence) {
          AddBookmarkAtTime(app, std::max(0.0f, r.startSec), true, std::clamp(r.confidence, 0.0f, 1.0f));
        }
      }
      if (app->bookmarksSec.size() > before) {
        std::wstringstream ss;
        ss << L"Completed + auto marks (" << (app->bookmarksSec.size() - before) << L")";
        SetStatus(app, ss.str());
      } else {
        SetStatus(app, L"Completed");
      }
    } else {
      SetStatus(app, L"Completed");
    }

    if (app->autoFocusEnabled && app->waterfallW > 0 && !app->overlayRows.empty() &&
        app->previewWav.sampleRate > 0 && !app->previewWav.samples.empty()) {
      const ndb::DecodeResult* best = nullptr;
      float bestScore = -1e9f;
      for (const auto& r : app->overlayRows) {
        const float s = 0.65f * r.confidence + 0.35f * r.compositeScore;
        if (s > bestScore) {
          bestScore = s;
          best = &r;
        }
      }
      if (best) {
        const float dur = static_cast<float>(app->previewWav.samples.size()) /
                          static_cast<float>(std::max(1, app->previewWav.sampleRate));
        const float tCenter = 0.5f * (best->startSec + best->endSec);
        const int col = std::clamp(static_cast<int>(std::round((tCenter / std::max(0.1f, dur)) *
                                                                std::max(1, app->waterfallW - 1))),
                                   0, app->waterfallW - 1);
        const float stab = std::clamp(best->freqStabilityScore, 0.0f, 1.0f);
        const double targetZoom = 1.7 + 1.5 * static_cast<double>(stab);
        const double af = std::clamp(static_cast<double>(app->autoFocusStrength), 0.0, 1.0);
        const double zEase = 0.10 + 0.35 * af;
        app->waterfallZoom = std::clamp((1.0 - zEase) * app->waterfallZoom + zEase * targetZoom, 1.0, 8.0);
        const int vis = std::max(60, static_cast<int>(std::round(
                                     static_cast<double>(std::max(1, app->waterfallW)) /
                                     std::max(1.0, app->waterfallZoom))));
        const int targetPan = std::clamp(col - vis / 2, 0, std::max(0, app->waterfallW - vis));
        const double pEase = 0.12 + 0.38 * af;
        app->waterfallPanPx = static_cast<int>(std::round((1.0 - pEase) * static_cast<double>(app->waterfallPanPx) +
                                                          pEase * static_cast<double>(targetPan)));
      }
      SaveUiState(app);
    }

    InvalidateRect(app->chartPanel, nullptr, TRUE);
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
        SetFocus(hwnd);
        const POINT p = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        RECT rc;
        GetClientRect(hwnd, &rc);
        const RECT wfPlot = GetWaterfallPlotRect(rc);
        const RECT wfMap = GetWaterfallMapRect(rc);
        if (!app->snapshotA.empty() && !app->snapshotB.empty() && PtInRect(&wfPlot, p)) {
          const int wipeX = wfPlot.left + ((wfPlot.right - wfPlot.left) * std::clamp(app->snapshotWipePct, 0, 100)) / 100;
          if (std::abs(p.x - wipeX) <= 8) {
            app->snapshotWipeDragging = true;
            SetCapture(hwnd);
            return 0;
          }
        }
        if ((GetKeyState(VK_CONTROL) & 0x8000) && PtInRect(&wfPlot, p)) {
          auto wf = HitTestWaterfall(app, rc, p);
          if (wf.ok) {
            app->compareCursorBValid = true;
            app->compareCursorBPt = wf.pt;
            app->compareCursorBFreqHz = wf.freqHz;
            app->compareCursorBTimeSec = wf.timeSec;
            app->compareCursorBDb = wf.db;
            SetStatus(app, L"Compare cursor B set [Ctrl+Click]");
            InvalidateRect(hwnd, nullptr, TRUE);
          }
          return 0;
        }
        if (app->waterfallFrozen && PtInRect(&wfMap, p) && app->waterfallW > 0) {
          const float xn = static_cast<float>(p.x - wfMap.left) /
                           std::max<int>(1, static_cast<int>(wfMap.right - wfMap.left - 1));
          app->waterfallFreezeCenterCol =
              std::clamp(static_cast<int>(std::round(xn * std::max(1, app->waterfallW - 1))),
                         0, app->waterfallW - 1);
          app->waterfallDragging = true;
          app->waterfallDragStartX = p.x;
          SetCapture(hwnd);
          InvalidateRect(hwnd, nullptr, TRUE);
          return 0;
        }
        if (app->manualNotchEnabled && PtInRect(&wfPlot, p) && (GetKeyState(VK_MENU) & 0x8000)) {
          const int hit = HitTestManualNotch(app, wfPlot, p);
          if (hit >= 0) {
            app->activeManualNotch = hit;
          } else {
            AddManualNotchAtFreq(app, YToFreqHz(app, wfPlot, p.y));
          }
          app->manualNotchDragging = true;
          SetCapture(hwnd);
          SaveUiState(app);
          InvalidateRect(hwnd, nullptr, TRUE);
          return 0;
        }
        if (JumpToBookmarkFromMapClick(app, wfMap, p)) {
          SetStatus(app, L"Jumped to bookmark");
          InvalidateRect(hwnd, nullptr, TRUE);
          return 0;
        }
        if (PtInRect(&wfPlot, p) && !app->overlayRows.empty() && !(GetKeyState(VK_SHIFT) & 0x8000)) {
          int srcX = 0;
          int srcW = app->waterfallW;
          ComputeWaterfallSourceWindow(app, &srcX, &srcW);
          const float dur = PreviewDurationSec(app);
          const float viewStart = (static_cast<float>(srcX) / std::max(1, app->waterfallW - 1)) *
                                  std::max(0.1f, dur);
          const float viewDur = (static_cast<float>(srcW) / std::max(1, app->waterfallW)) *
                                std::max(0.1f, dur);
          const float viewEnd = viewStart + viewDur;
          const float nyq = 0.5f * static_cast<float>(std::max(1, app->previewWav.sampleRate));
          const float fMin = 80.0f;
          const float fMax = std::min(2200.0f, nyq - 20.0f);
          const float xn = static_cast<float>(p.x - wfPlot.left) /
                           std::max<int>(1, static_cast<int>(wfPlot.right - wfPlot.left));
          const float yn = static_cast<float>(p.y - wfPlot.top) /
                           std::max<int>(1, static_cast<int>(wfPlot.bottom - wfPlot.top));
          const float tSec = viewStart + std::clamp(xn, 0.0f, 1.0f) * viewDur;
          const float fHz = fMax - std::clamp(yn, 0.0f, 1.0f) * (fMax - fMin);
          int bestId = -1;
          float bestDist = 1e9f;
          for (const auto& r : app->overlayRows) {
            if (r.endSec < viewStart || r.startSec > viewEnd) continue;
            const float tc = 0.5f * (r.startSec + r.endSec);
            const float dt = std::fabs(tc - tSec) / std::max(0.2f, viewDur);
            const float df = std::fabs(r.freqHz - fHz) / 220.0f;
            const float d = dt + df;
            if (d < bestDist) {
              bestDist = d;
              bestId = r.trackId;
            }
          }
          if (bestId >= 0 && bestDist < 0.35f) {
            app->selectedTrackId = bestId;
            SetStatus(app, L"Track pinned (focus)");
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
          }
        }
        if (PtInRect(&wfPlot, p) && (GetKeyState(VK_SHIFT) & 0x8000)) {
          app->waterfallZoomBoxActive = true;
          app->waterfallZoomBoxStart = p;
          app->waterfallZoomBoxEnd = p;
          SetCapture(hwnd);
          return 0;
        }
        if (PtInRect(&wfPlot, p) || PtInRect(&wfMap, p)) {
          app->waterfallDragging = true;
          app->waterfallDragStartX = p.x;
          app->waterfallPanStartPx = app->waterfallPanPx;
          if (PtInRect(&wfMap, p) && app->waterfallW > 0) {
            const float xn = static_cast<float>(p.x - wfMap.left) /
                             std::max<int>(1, static_cast<int>(wfMap.right - wfMap.left - 1));
            const int vis = std::max(60, static_cast<int>(std::round(
                                         static_cast<double>(std::max(1, app->waterfallW)) /
                                         std::max(1.0, app->waterfallZoom))));
            const int centerCol = std::clamp(static_cast<int>(xn * app->waterfallW), 0, app->waterfallW - 1);
            const int maxPan = std::max(0, app->waterfallW - vis);
            app->waterfallPanPx = std::clamp(centerCol - vis / 2, 0, maxPan);
            app->waterfallPanStartPx = app->waterfallPanPx;
          }
        } else {
          app->dragging = true;
          app->dragStartX = p.x;
          app->panStartPx = app->chartPanPx;
        }
        SetCapture(hwnd);
      }
      return 0;
    case WM_KEYDOWN:
      if (app) {
        const UINT vk = static_cast<UINT>(wParam);
        if (vk == VK_F6) {
          ApplyVisualTuningPreset(app, 1.38f, 1.25f, 0.52f, 0.45f, L"DX Weak");
          return 0;
        }
        if (vk == VK_F7) {
          ApplyVisualTuningPreset(app, 1.00f, 1.00f, 0.58f, 0.28f, L"Balanced");
          return 0;
        }
        if (vk == VK_F8) {
          ApplyVisualTuningPreset(app, 0.76f, 0.45f, 0.64f, 0.16f, L"Clean");
          return 0;
        }
        if (vk == VK_F9 || vk == VK_F10) {
          const auto& src = (app->differenceWaterfallEnabled && !app->waterfallDiffRgb.empty())
                                ? app->waterfallDiffRgb
                                : app->waterfallRgb;
          if (!src.empty() && app->waterfallW > 0 && app->waterfallH > 0) {
            if (vk == VK_F9) {
              app->snapshotA = src;
              app->snapshotW = app->waterfallW;
              app->snapshotH = app->waterfallH;
              SetStatus(app, L"Snapshot A captured [F9]");
            } else {
              app->snapshotB = src;
              app->snapshotW = app->waterfallW;
              app->snapshotH = app->waterfallH;
              SetStatus(app, L"Snapshot B captured [F10]");
            }
            InvalidateRect(hwnd, nullptr, TRUE);
          }
          return 0;
        }
        if (vk == VK_F11) {
          app->snapshotA.clear();
          app->snapshotB.clear();
          SetStatus(app, L"Snapshots cleared [F11]");
          InvalidateRect(hwnd, nullptr, TRUE);
          return 0;
        }
        if (vk == 'F') {
          ToggleWaterfallFreeze(app);
          return 0;
        }
        if (vk == VK_DELETE && app->activeManualNotch >= 0) {
          RemoveManualNotchByIndex(app, app->activeManualNotch);
          SaveUiState(app);
          SetStatus(app, L"Manual notch removed [Del]");
          InvalidateRect(hwnd, nullptr, TRUE);
          return 0;
        }
        if (vk == 'M') {
          app->manualNotchEnabled = !app->manualNotchEnabled;
          SaveUiState(app);
          SetStatus(app, app->manualNotchEnabled ? L"Manual notch ON [M]" : L"Manual notch OFF [M]");
          InvalidateRect(hwnd, nullptr, TRUE);
          return 0;
        }
        if (vk == 'X') {
          app->differenceWaterfallEnabled = !app->differenceWaterfallEnabled;
          if (app->diffWaterfallCheck) {
            SendMessageW(app->diffWaterfallCheck, BM_SETCHECK,
                         app->differenceWaterfallEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
          }
          SaveUiState(app);
          SetStatus(app, app->differenceWaterfallEnabled ? L"Difference waterfall ON [X]"
                                                         : L"Difference waterfall OFF [X]");
          InvalidateRect(hwnd, nullptr, TRUE);
          return 0;
        }
        if (vk == 'G') {
          app->dotDashAssistEnabled = !app->dotDashAssistEnabled;
          if (app->dotDashAssistCheck) {
            SendMessageW(app->dotDashAssistCheck, BM_SETCHECK,
                         app->dotDashAssistEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
          }
          SaveUiState(app);
          SetStatus(app, app->dotDashAssistEnabled ? L"Dot/Dash assist ON [G]"
                                                   : L"Dot/Dash assist OFF [G]");
          InvalidateRect(hwnd, nullptr, TRUE);
          return 0;
        }
        if (vk == 'V') {
          app->showWideView = !app->showWideView;
          if (app->wideViewCheck) {
            SendMessageW(app->wideViewCheck, BM_SETCHECK,
                         app->showWideView ? BST_CHECKED : BST_UNCHECKED, 0);
          }
          UpdateWaterfallToggleButtons(app);
          SaveUiState(app);
          SetStatus(app, app->showWideView ? L"Wide view ON [V]" : L"Wide view OFF [V]");
          InvalidateRect(hwnd, nullptr, TRUE);
          return 0;
        }
        if (vk == 'O') {
          app->showRidgeOverlay = !app->showRidgeOverlay;
          if (app->ridgeOverlayCheck) {
            SendMessageW(app->ridgeOverlayCheck, BM_SETCHECK,
                         app->showRidgeOverlay ? BST_CHECKED : BST_UNCHECKED, 0);
          }
          UpdateWaterfallToggleButtons(app);
          SaveUiState(app);
          SetStatus(app, app->showRidgeOverlay ? L"Ridge overlay ON [O]"
                                               : L"Ridge overlay OFF [O]");
          InvalidateRect(hwnd, nullptr, TRUE);
          return 0;
        }
        if (vk == 'B') {
          const std::size_t before = app->bookmarksSec.size();
          AddBookmarkAtCurrent(app);
          const std::size_t after = app->bookmarksSec.size();
          SetStatus(app, after > before ? L"Bookmark added [B]" : L"Bookmark already present");
          InvalidateRect(hwnd, nullptr, TRUE);
          return 0;
        }
        if (vk == 'C') {
          app->bookmarksSec.clear();
          app->bookmarkAuto.clear();
          app->bookmarkConfidence.clear();
          SetStatus(app, L"Bookmarks cleared [C]");
          InvalidateRect(hwnd, nullptr, TRUE);
          return 0;
        }
        if (vk == 'A') {
          app->showAutoBookmarks = !app->showAutoBookmarks;
          SaveUiState(app);
          SetStatus(app, app->showAutoBookmarks ? L"Show auto bookmarks [A]: ON"
                                                : L"Show auto bookmarks [A]: OFF");
          InvalidateRect(hwnd, nullptr, TRUE);
          return 0;
        }
        if (vk == 'N') {
          if (JumpToAdjacentBookmark(app, +1)) {
            SetStatus(app, L"Next bookmark [N]");
            InvalidateRect(hwnd, nullptr, TRUE);
          }
          return 0;
        }
        if (vk == 'P') {
          if (JumpToAdjacentBookmark(app, -1)) {
            SetStatus(app, L"Previous bookmark [P]");
            InvalidateRect(hwnd, nullptr, TRUE);
          }
          return 0;
        }
        if (vk >= '1' && vk <= '9') {
          const int idx = static_cast<int>(vk - '1');
          if (JumpToBookmarkIndex(app, idx)) {
            std::wstringstream ss;
            ss << L"Bookmark " << (idx + 1) << L" [" << static_cast<wchar_t>(vk) << L"]";
            SetStatus(app, ss.str());
            InvalidateRect(hwnd, nullptr, TRUE);
          }
          return 0;
        }
        if (vk == 'D') {
          if (RemoveBookmarkNearestCurrent(app)) {
            SetStatus(app, L"Nearest bookmark removed [D]");
            InvalidateRect(hwnd, nullptr, TRUE);
          }
          return 0;
        }
        if (vk == 'L') {
          app->waterfallReadoutLocked = !app->waterfallReadoutLocked;
          SetStatus(app, app->waterfallReadoutLocked ? L"Readout locked [L]" : L"Readout live [L]");
          InvalidateRect(hwnd, nullptr, TRUE);
          return 0;
        }
        if (vk == 'E') {
          SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(kIdBookmarkExport, BN_CLICKED), 0);
          return 0;
        }
        if (vk == 'I') {
          SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(kIdBookmarkImport, BN_CLICKED), 0);
          return 0;
        }
        if (vk == 'R' && (GetKeyState(VK_CONTROL) & 0x8000)) {
          app->suppressNextResetConfirm = ((GetKeyState(VK_SHIFT) & 0x8000) != 0);
          SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(kIdResetUiSession, BN_CLICKED), 0);
          return 0;
        }
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
        RECT rc;
        GetClientRect(hwnd, &rc);
        const RECT wfPlot = GetWaterfallPlotRect(rc);
        if (app->snapshotWipeDragging) {
          const float xn = static_cast<float>(p.x - wfPlot.left) /
                           std::max<int>(1, static_cast<int>(wfPlot.right - wfPlot.left));
          app->snapshotWipePct = std::clamp(static_cast<int>(std::round(xn * 100.0f)), 0, 100);
          InvalidateRect(hwnd, nullptr, TRUE);
          return 0;
        }
        if ((GetKeyState(VK_MENU) & 0x8000) && PtInRect(&wfPlot, p) && app->manualNotchEnabled) {
          app->notchGhostActive = true;
          app->notchGhostFreqHz = YToFreqHz(app, wfPlot, p.y);
          app->notchGhostWidthHz = 24.0f;
        } else {
          app->notchGhostActive = false;
        }
        if (app->manualNotchDragging) {
          UpdateActiveManualNotchFromPoint(app, wfPlot, p);
          InvalidateRect(hwnd, nullptr, TRUE);
          return 0;
        }
        if (app->waterfallZoomBoxActive) {
          app->waterfallZoomBoxEnd = p;
          app->hoverActive = false;
          app->waterfallHoverActive = false;
          InvalidateRect(hwnd, nullptr, TRUE);
        } else if (app->waterfallDragging) {
          if (app->waterfallFrozen) {
            RECT rc2;
            GetClientRect(hwnd, &rc2);
            const RECT wfMap = GetWaterfallMapRect(rc2);
            if (PtInRect(&wfMap, p) && app->waterfallW > 0) {
              const float xn = static_cast<float>(p.x - wfMap.left) /
                               std::max<int>(1, static_cast<int>(wfMap.right - wfMap.left - 1));
              app->waterfallFreezeCenterCol =
                  std::clamp(static_cast<int>(std::round(xn * std::max(1, app->waterfallW - 1))),
                             0, app->waterfallW - 1);
            }
          } else {
            const int dx = app->waterfallDragStartX - p.x;
            const int vis = std::max(60, static_cast<int>(std::round(
                                         static_cast<double>(std::max(1, app->waterfallW)) /
                                         std::max(1.0, app->waterfallZoom))));
            const int maxPan = std::max(0, app->waterfallW - vis);
            app->waterfallPanPx = std::clamp(app->waterfallPanStartPx + dx, 0, maxPan);
          }
          app->hoverActive = false;
          app->waterfallHoverActive = false;
          InvalidateRect(hwnd, nullptr, TRUE);
        } else if (app->dragging) {
          app->chartPanPx = app->panStartPx + (p.x - app->dragStartX);
          app->hoverActive = false;
          app->waterfallHoverActive = false;
          InvalidateRect(hwnd, nullptr, TRUE);
        } else {
          auto wf = HitTestWaterfall(app, rc, p);
          if (wf.ok) {
            app->waterfallHoverActive = true;
            app->waterfallHoverPoint = wf.pt;
            app->waterfallHoverText = wf.text;
            if (!app->waterfallReadoutLocked) {
              app->waterfallReadoutValid = true;
              app->waterfallReadoutFreqHz = wf.freqHz;
              app->waterfallReadoutTimeSec = wf.timeSec;
              app->waterfallReadoutDb = wf.db;
              app->waterfallReadoutSnrDb = wf.snrDb;
              app->waterfallReadoutNoiseFloorDb = wf.noiseFloorDb;
              app->waterfallReadoutDeltaFreqHz = wf.deltaFreqHz;
              app->readoutSnrTrend.push_back(wf.snrDb);
              if (app->readoutSnrTrend.size() > 96) {
                app->readoutSnrTrend.erase(app->readoutSnrTrend.begin(),
                                           app->readoutSnrTrend.begin() +
                                               static_cast<std::ptrdiff_t>(app->readoutSnrTrend.size() - 96));
              }
            }
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
      if (app && app->snapshotWipeDragging) {
        app->snapshotWipeDragging = false;
        ReleaseCapture();
      } else if (app && app->manualNotchDragging) {
        app->manualNotchDragging = false;
        SaveUiState(app);
        ReleaseCapture();
        InvalidateRect(hwnd, nullptr, TRUE);
      } else if (app && app->waterfallZoomBoxActive) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        const RECT wfPlot = GetWaterfallPlotRect(rc);
        ApplyWaterfallZoomBox(app, wfPlot, app->waterfallZoomBoxStart, app->waterfallZoomBoxEnd);
        app->waterfallZoomBoxActive = false;
        SaveUiState(app);
        InvalidateRect(hwnd, nullptr, TRUE);
        ReleaseCapture();
      } else if (app && (app->dragging || app->waterfallDragging)) {
        app->dragging = false;
        app->waterfallDragging = false;
        ReleaseCapture();
        SaveUiState(app);
      }
      return 0;
    case WM_RBUTTONDOWN:
      if (app) {
        const POINT p = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        RECT rc;
        GetClientRect(hwnd, &rc);
        const RECT wfPlot = GetWaterfallPlotRect(rc);
        if (app->manualNotchEnabled && PtInRect(&wfPlot, p)) {
          const int hit = HitTestManualNotch(app, wfPlot, p);
          if (hit >= 0) {
            RemoveManualNotchByIndex(app, hit);
            SaveUiState(app);
            SetStatus(app, L"Manual notch removed");
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
          }
        }
        const RECT wfMap = GetWaterfallMapRect(rc);
        if (RemoveBookmarkFromMapClick(app, wfMap, p)) {
          SetStatus(app, L"Bookmark removed");
          InvalidateRect(hwnd, nullptr, TRUE);
        }
      }
      return 0;
    case WM_MOUSELEAVE:
      if (app) {
        app->mouseLeaveArmed = false;
        app->hoverActive = false;
        app->waterfallHoverActive = false;
        app->notchGhostActive = false;
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
      const int fullW = 1820 - (2 * m);
      const int leftW = fullW;
      const int rightX = m;
      const int rightW = fullW;
      int y = 860;

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
      CreateWindowW(L"STATIC", L"Palette", WS_CHILD | WS_VISIBLE, m + 540, y + 6, 56, 22, hwnd,
                    nullptr, nullptr, nullptr);
      app->palettePresetCombo = CreateWindowW(
          L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST, m + 600, y,
          170, 120, hwnd, (HMENU)kIdPalettePresetCombo, nullptr, nullptr);
      SendMessageW(app->palettePresetCombo, CB_ADDSTRING, 0, (LPARAM)L"HDSDR");
      SendMessageW(app->palettePresetCombo, CB_ADDSTRING, 0, (LPARAM)L"SDR#");
      SendMessageW(app->palettePresetCombo, CB_ADDSTRING, 0, (LPARAM)L"CubicSDR");
      SendMessageW(app->palettePresetCombo, CB_ADDSTRING, 0, (LPARAM)L"Custom LUT");
      SendMessageW(app->palettePresetCombo, CB_ADDSTRING, 0, (LPARAM)L"Cividis");
      SendMessageW(app->palettePresetCombo, CB_ADDSTRING, 0, (LPARAM)L"Viridis");
      SendMessageW(app->palettePresetCombo, CB_SETCURSEL, app->palettePreset, 0);
      app->paletteLoadButton = CreateWindowW(L"BUTTON", L"Load LUT", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                             m + 776, y, 92, 30, hwnd, (HMENU)kIdPaletteLoadLut,
                                             nullptr, nullptr);
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
      CreateWindowW(L"STATIC", L"Persist", WS_CHILD | WS_VISIBLE, m + 1092, y + 6, 52, 22,
                    hwnd, nullptr, nullptr, nullptr);
      app->waterfallPersistCombo = CreateWindowW(
          L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
          m + 1148, y, 92, 120, hwnd, (HMENU)kIdWaterfallPersistCombo, nullptr, nullptr);
      SendMessageW(app->waterfallPersistCombo, CB_ADDSTRING, 0, (LPARAM)L"Fast");
      SendMessageW(app->waterfallPersistCombo, CB_ADDSTRING, 0, (LPARAM)L"Medium");
      SendMessageW(app->waterfallPersistCombo, CB_ADDSTRING, 0, (LPARAM)L"Long");
      SendMessageW(app->waterfallPersistCombo, CB_SETCURSEL, app->waterfallPersistenceMode, 0);
      CreateWindowW(L"STATIC", L"FFT", WS_CHILD | WS_VISIBLE, m + 1246, y + 6, 28, 22, hwnd,
                    nullptr, nullptr, nullptr);
      app->fftPreviewCombo = CreateWindowW(
          L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
          m + 1276, y, 76, 120, hwnd, (HMENU)kIdFftPreviewCombo, nullptr, nullptr);
      SendMessageW(app->fftPreviewCombo, CB_ADDSTRING, 0, (LPARAM)L"256");
      SendMessageW(app->fftPreviewCombo, CB_ADDSTRING, 0, (LPARAM)L"512");
      SendMessageW(app->fftPreviewCombo, CB_ADDSTRING, 0, (LPARAM)L"1024");
      SendMessageW(app->fftPreviewCombo, CB_SETCURSEL,
                   (app->previewFftSize >= 1024) ? 2 : ((app->previewFftSize <= 256) ? 0 : 1),
                   0);
      app->wideViewCheck = CreateWindowW(L"BUTTON", L"Wide View",
                                         WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, m + 1360, y + 4,
                                         92, 24, hwnd, (HMENU)kIdWideViewCheck, nullptr, nullptr);
      SendMessageW(app->wideViewCheck, BM_SETCHECK,
                   app->showWideView ? BST_CHECKED : BST_UNCHECKED, 0);
      app->ridgeOverlayCheck = CreateWindowW(
          L"BUTTON", L"Ridge",
          WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, m + 1454, y + 4, 72, 24, hwnd,
          (HMENU)kIdRidgeOverlayCheck, nullptr, nullptr);
      SendMessageW(app->ridgeOverlayCheck, BM_SETCHECK,
                   app->showRidgeOverlay ? BST_CHECKED : BST_UNCHECKED, 0);
      app->diffWaterfallCheck = CreateWindowW(
          L"BUTTON", L"Diff",
          WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, m + 1530, y + 4, 58, 24, hwnd,
          (HMENU)kIdDiffWaterfallCheck, nullptr, nullptr);
      SendMessageW(app->diffWaterfallCheck, BM_SETCHECK,
                   app->differenceWaterfallEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
      app->dotDashAssistCheck = CreateWindowW(
          L"BUTTON", L"DotDash",
          WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, m + 1590, y + 4, 78, 24, hwnd,
          (HMENU)kIdDotDashAssistCheck, nullptr, nullptr);
      SendMessageW(app->dotDashAssistCheck, BM_SETCHECK,
                   app->dotDashAssistEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
      app->wideViewButton = CreateWindowW(L"BUTTON", L"WIDE ON",
                                          WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                          m + 1670, y, 72, 30, hwnd,
                                          (HMENU)kIdWideViewButton, nullptr, nullptr);
      app->ridgeOverlayButton = CreateWindowW(L"BUTTON", L"RIDGE ON",
                                              WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                              m + 1746, y, 72, 30, hwnd,
                                              (HMENU)kIdRidgeOverlayButton, nullptr, nullptr);
      app->freezeButton = CreateWindowW(L"BUTTON", L"FREEZE OFF",
                                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                        m + 1568, y + 32, 104, 26, hwnd,
                                        (HMENU)kIdFreezeButton, nullptr, nullptr);
      CreateWindowW(L"BUTTON", L"3D DX Weak Preset", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    m + 1676, y + 32, 132, 26, hwnd, (HMENU)kIdWaterfallDxPreset, nullptr, nullptr);
      UpdateWaterfallToggleButtons(app);
      UpdateFreezeButton(app);
      y += 62;

      CreateWindowW(L"STATIC", L"Pan Avg Alpha", WS_CHILD | WS_VISIBLE, m + 1092, y + 6, 92, 22,
                    hwnd, nullptr, nullptr, nullptr);
      app->panAvgAlphaSlider = CreateWindowW(TRACKBAR_CLASSW, L"",
                                             WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, m + 1188, y,
                                             170, 28, hwnd, (HMENU)kIdPanAvgAlphaSlider, nullptr,
                                             nullptr);
      SendMessageW(app->panAvgAlphaSlider, TBM_SETRANGEMIN, FALSE, 1);
      SendMessageW(app->panAvgAlphaSlider, TBM_SETRANGEMAX, FALSE, 40);
      SendMessageW(app->panAvgAlphaSlider, TBM_SETPOS, TRUE,
                   static_cast<LPARAM>(std::round(app->panAvgAlpha * 100.0f)));
      CreateWindowW(L"STATIC", L"Peak Decay", WS_CHILD | WS_VISIBLE, m + 1364, y + 6, 72, 22,
                    hwnd, nullptr, nullptr, nullptr);
      app->panPeakDecaySlider = CreateWindowW(TRACKBAR_CLASSW, L"",
                                              WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, m + 1438, y,
                                              170, 28, hwnd, (HMENU)kIdPanPeakDecaySlider, nullptr,
                                              nullptr);
      SendMessageW(app->panPeakDecaySlider, TBM_SETRANGEMIN, FALSE, 1);
      SendMessageW(app->panPeakDecaySlider, TBM_SETRANGEMAX, FALSE, 120);
      SendMessageW(app->panPeakDecaySlider, TBM_SETPOS, TRUE,
                   static_cast<LPARAM>(std::round(app->panPeakDecay * 100.0f)));
      y += 36;

      CreateWindowW(L"STATIC", L"Lens", WS_CHILD | WS_VISIBLE, m + 1092, y + 6, 36, 22,
                    hwnd, nullptr, nullptr, nullptr);
      app->lensStrengthSlider = CreateWindowW(TRACKBAR_CLASSW, L"",
                                              WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, m + 1128, y,
                                              110, 28, hwnd, (HMENU)kIdLensStrengthSlider, nullptr,
                                              nullptr);
      SendMessageW(app->lensStrengthSlider, TBM_SETRANGEMIN, FALSE, 50);
      SendMessageW(app->lensStrengthSlider, TBM_SETRANGEMAX, FALSE, 200);
      SendMessageW(app->lensStrengthSlider, TBM_SETPOS, TRUE,
                   static_cast<LPARAM>(std::round(app->lensStrength * 100.0f)));
      CreateWindowW(L"STATIC", L"BG", WS_CHILD | WS_VISIBLE, m + 1244, y + 6, 24, 22, hwnd,
                    nullptr, nullptr, nullptr);
      app->bgRemovalSlider = CreateWindowW(TRACKBAR_CLASSW, L"",
                                           WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, m + 1270, y,
                                           110, 28, hwnd, (HMENU)kIdBgRemovalSlider, nullptr,
                                           nullptr);
      SendMessageW(app->bgRemovalSlider, TBM_SETRANGEMIN, FALSE, 0);
      SendMessageW(app->bgRemovalSlider, TBM_SETRANGEMAX, FALSE, 160);
      SendMessageW(app->bgRemovalSlider, TBM_SETPOS, TRUE,
                   static_cast<LPARAM>(std::round(app->bgRemovalStrength * 100.0f)));
      CreateWindowW(L"STATIC", L"Split", WS_CHILD | WS_VISIBLE, m + 1386, y + 6, 36, 22,
                    hwnd, nullptr, nullptr, nullptr);
      app->splitPointSlider = CreateWindowW(TRACKBAR_CLASSW, L"",
                                            WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, m + 1424, y,
                                            96, 28, hwnd, (HMENU)kIdSplitPointSlider, nullptr,
                                            nullptr);
      SendMessageW(app->splitPointSlider, TBM_SETRANGEMIN, FALSE, 35);
      SendMessageW(app->splitPointSlider, TBM_SETRANGEMAX, FALSE, 80);
      SendMessageW(app->splitPointSlider, TBM_SETPOS, TRUE,
                   static_cast<LPARAM>(std::round(app->splitTonePoint * 100.0f)));
      CreateWindowW(L"STATIC", L"AutoF", WS_CHILD | WS_VISIBLE, m + 1526, y + 6, 40, 22,
                    hwnd, nullptr, nullptr, nullptr);
      app->autoFocusStrengthSlider = CreateWindowW(TRACKBAR_CLASSW, L"",
                                                   WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
                                                   m + 1568, y, 88, 28, hwnd,
                                                   (HMENU)kIdAutoFocusStrengthSlider, nullptr,
                                                   nullptr);
      SendMessageW(app->autoFocusStrengthSlider, TBM_SETRANGEMIN, FALSE, 0);
      SendMessageW(app->autoFocusStrengthSlider, TBM_SETRANGEMAX, FALSE, 100);
      SendMessageW(app->autoFocusStrengthSlider, TBM_SETPOS, TRUE,
                   static_cast<LPARAM>(std::round(app->autoFocusStrength * 100.0f)));
      CreateWindowW(L"BUTTON", L"DX Weak", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    m + 1464, y, 68, 28, hwnd, (HMENU)kIdVisualPresetDxWeak, nullptr, nullptr);
      CreateWindowW(L"BUTTON", L"Balanced", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    m + 1536, y, 72, 28, hwnd, (HMENU)kIdVisualPresetBalanced, nullptr,
                    nullptr);
      CreateWindowW(L"BUTTON", L"Clean", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    m + 1612, y, 56, 28, hwnd, (HMENU)kIdVisualPresetClean, nullptr, nullptr);
      CreateWindowW(L"BUTTON", L"No-QRM", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    m + 1672, y, 64, 28, hwnd, (HMENU)kIdQrmPresetNo, nullptr, nullptr);
      CreateWindowW(L"BUTTON", L"Heavy", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    m + 1740, y, 60, 28, hwnd, (HMENU)kIdQrmPresetHeavy, nullptr, nullptr);
      y += 34;

      CreateWindowW(L"STATIC", L"AGC Floor", WS_CHILD | WS_VISIBLE, m, y + 6, 64, 22, hwnd,
                    nullptr, nullptr, nullptr);
      app->agcFloorSlider = CreateWindowW(TRACKBAR_CLASSW, L"",
                                          WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, m + 66, y,
                                          170, 28, hwnd, (HMENU)kIdAgcFloorSlider, nullptr,
                                          nullptr);
      SendMessageW(app->agcFloorSlider, TBM_SETRANGEMIN, FALSE, -30);
      SendMessageW(app->agcFloorSlider, TBM_SETRANGEMAX, FALSE, 30);
      SendMessageW(app->agcFloorSlider, TBM_SETPOS, TRUE, static_cast<LPARAM>(app->agcFloorOffsetDb));

      CreateWindowW(L"STATIC", L"Span", WS_CHILD | WS_VISIBLE, m + 244, y + 6, 36, 22, hwnd,
                    nullptr, nullptr, nullptr);
      app->agcSpanSlider = CreateWindowW(TRACKBAR_CLASSW, L"",
                                         WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, m + 282, y, 160,
                                         28, hwnd, (HMENU)kIdAgcSpanSlider, nullptr, nullptr);
      SendMessageW(app->agcSpanSlider, TBM_SETRANGEMIN, FALSE, 8);
      SendMessageW(app->agcSpanSlider, TBM_SETRANGEMAX, FALSE, 80);
      SendMessageW(app->agcSpanSlider, TBM_SETPOS, TRUE, static_cast<LPARAM>(app->agcSpanDb));

      CreateWindowW(L"STATIC", L"Gain", WS_CHILD | WS_VISIBLE, m + 450, y + 6, 36, 22, hwnd,
                    nullptr, nullptr, nullptr);
      app->agcGainSlider = CreateWindowW(TRACKBAR_CLASSW, L"",
                                         WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, m + 488, y, 160,
                                         28, hwnd, (HMENU)kIdAgcGainSlider, nullptr, nullptr);
      SendMessageW(app->agcGainSlider, TBM_SETRANGEMIN, FALSE, 50);
      SendMessageW(app->agcGainSlider, TBM_SETRANGEMAX, FALSE, 250);
      SendMessageW(app->agcGainSlider, TBM_SETPOS, TRUE,
                   static_cast<LPARAM>(std::round(app->agcGain * 100.0f)));

      CreateWindowW(L"STATIC", L"Gamma", WS_CHILD | WS_VISIBLE, m + 656, y + 6, 44, 22, hwnd,
                    nullptr, nullptr, nullptr);
      app->agcGammaSlider = CreateWindowW(TRACKBAR_CLASSW, L"",
                                          WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, m + 704, y,
                                          160, 28, hwnd, (HMENU)kIdAgcGammaSlider, nullptr,
                                          nullptr);
      SendMessageW(app->agcGammaSlider, TBM_SETRANGEMIN, FALSE, 40);
      SendMessageW(app->agcGammaSlider, TBM_SETRANGEMAX, FALSE, 160);
      SendMessageW(app->agcGammaSlider, TBM_SETPOS, TRUE,
                   static_cast<LPARAM>(std::round(app->agcGamma * 100.0f)));
      app->agcAutoCheck = CreateWindowW(L"BUTTON", L"Auto Contrast",
                                        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                        m + 874, y + 4, 130, 24, hwnd,
                                        (HMENU)kIdAgcAutoCheck, nullptr, nullptr);
      SendMessageW(app->agcAutoCheck, BM_SETCHECK,
                   app->agcAutoContrast ? BST_CHECKED : BST_UNCHECKED, 0);
      app->peakLockButton = CreateWindowW(L"BUTTON", L"Peak Lock: OFF",
                                          WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                          m + 1010, y, 140, 30, hwnd,
                                          (HMENU)kIdPeakLockButton, nullptr, nullptr);
      app->manualNotchCheck = CreateWindowW(L"BUTTON", L"Manual Notch",
                                            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                            m + 1010, y + 32, 140, 24, hwnd,
                                            (HMENU)kIdManualNotchCheck, nullptr, nullptr);
      SendMessageW(app->manualNotchCheck, BM_SETCHECK,
                   app->manualNotchEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
      CreateWindowW(L"BUTTON", L"Clear Notch", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    m + 1160, y + 32, 100, 24, hwnd, (HMENU)kIdManualNotchClear, nullptr,
                    nullptr);
      CreateWindowW(L"BUTTON", L"Export N", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    m + 1264, y + 32, 92, 24, hwnd, (HMENU)kIdManualNotchExport, nullptr,
                    nullptr);
      CreateWindowW(L"BUTTON", L"Import N", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    m + 1360, y + 32, 92, 24, hwnd, (HMENU)kIdManualNotchImport, nullptr,
                    nullptr);
      app->autoBookmarkCheck = CreateWindowW(L"BUTTON", L"Auto Marks",
                                             WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                             m + 1160, y + 4, 100, 24, hwnd,
                                             (HMENU)kIdAutoBookmarkCheck, nullptr, nullptr);
      SendMessageW(app->autoBookmarkCheck, BM_SETCHECK,
                   app->autoBookmarkEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
      app->autoBookmarkConfSlider = CreateWindowW(TRACKBAR_CLASSW, L"",
                                                  WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, m + 1262,
                                                  y, 110, 28, hwnd,
                                                  (HMENU)kIdAutoBookmarkConfSlider, nullptr,
                                                  nullptr);
      SendMessageW(app->autoBookmarkConfSlider, TBM_SETRANGEMIN, FALSE, 30);
      SendMessageW(app->autoBookmarkConfSlider, TBM_SETRANGEMAX, FALSE, 95);
      SendMessageW(app->autoBookmarkConfSlider, TBM_SETPOS, TRUE,
                   static_cast<LPARAM>(std::round(app->autoBookmarkMinConfidence * 100.0f)));
      CreateWindowW(L"STATIC", L"Mid", WS_CHILD | WS_VISIBLE, m + 1378, y + 6, 26, 22, hwnd,
                    nullptr, nullptr, nullptr);
      app->autoBookmarkMidSlider = CreateWindowW(TRACKBAR_CLASSW, L"",
                                                 WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, m + 1406,
                                                 y, 92, 28, hwnd,
                                                 (HMENU)kIdAutoBookmarkMidSlider, nullptr,
                                                 nullptr);
      SendMessageW(app->autoBookmarkMidSlider, TBM_SETRANGEMIN, FALSE, 40);
      SendMessageW(app->autoBookmarkMidSlider, TBM_SETRANGEMAX, FALSE, 90);
      SendMessageW(app->autoBookmarkMidSlider, TBM_SETPOS, TRUE,
                   static_cast<LPARAM>(std::round(app->autoBookmarkMidThreshold * 100.0f)));

      CreateWindowW(L"STATIC", L"High", WS_CHILD | WS_VISIBLE, m + 1502, y + 6, 30, 22, hwnd,
                    nullptr, nullptr, nullptr);
      app->autoBookmarkHighSlider = CreateWindowW(TRACKBAR_CLASSW, L"",
                                                  WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, m + 1534,
                                                  y, 92, 28, hwnd,
                                                  (HMENU)kIdAutoBookmarkHighSlider, nullptr,
                                                  nullptr);
      SendMessageW(app->autoBookmarkHighSlider, TBM_SETRANGEMIN, FALSE, 55);
      SendMessageW(app->autoBookmarkHighSlider, TBM_SETRANGEMAX, FALSE, 98);
      SendMessageW(app->autoBookmarkHighSlider, TBM_SETPOS, TRUE,
                   static_cast<LPARAM>(std::round(app->autoBookmarkHighThreshold * 100.0f)));
      y += 36;

      CreateWindowW(L"STATIC", L"AutoMark Preset", WS_CHILD | WS_VISIBLE, m + 1160, y + 6, 110, 22,
                    hwnd, nullptr, nullptr, nullptr);
      CreateWindowW(L"BUTTON", L"DX strict", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, m + 1276, y,
                    92, 28, hwnd, (HMENU)kIdAutoMarkPresetDx, nullptr, nullptr);
      CreateWindowW(L"BUTTON", L"Balanced", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, m + 1372, y,
                    92, 28, hwnd, (HMENU)kIdAutoMarkPresetBalanced, nullptr, nullptr);
      CreateWindowW(L"BUTTON", L"Weak-Sig", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, m + 1468, y,
                    92, 28, hwnd, (HMENU)kIdAutoMarkPresetWeak, nullptr, nullptr);
      y += 34;

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
      CreateWindowW(L"BUTTON", L"Add Bookmark", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, m + 646,
                    y + 40, 130, 34, hwnd, (HMENU)kIdBookmarkAdd, nullptr, nullptr);
      CreateWindowW(L"BUTTON", L"Clear Marks", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, m + 782,
                    y + 40, 120, 34, hwnd, (HMENU)kIdBookmarkClear, nullptr, nullptr);
      CreateWindowW(L"BUTTON", L"Export Marks", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, m + 908,
                    y + 40, 124, 34, hwnd, (HMENU)kIdBookmarkExport, nullptr, nullptr);
      CreateWindowW(L"BUTTON", L"Import Marks", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, m + 1038,
                    y + 40, 124, 34, hwnd, (HMENU)kIdBookmarkImport, nullptr, nullptr);
      app->resetUiButton = CreateWindowW(L"BUTTON", L"Reset Session UI",
                                         WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, m + 1168,
                                         y + 40, 140, 34, hwnd, (HMENU)kIdResetUiSession, nullptr,
                                         nullptr);
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
                                      rightX, 16, rightW, 820, hwnd, (HMENU)kIdChartPanel, nullptr,
                                      nullptr);
      SetWindowLongPtrW(app->chartPanel, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));

      const HWND controls[] = {app->runButton, app->statusText, app->historyEdit,
                               app->compareEdit, app->inputEdit, app->outputEdit, app->metricsEdit,
                               app->presetCombo, app->calibCombo, app->waterfallViewCombo,
                               app->palettePresetCombo, app->paletteLoadButton,
                               app->yawSlider, app->pitchSlider, app->shadingCheck,
                               app->colormapCombo, app->waterfallFpsCombo,
                               app->waterfallPersistCombo, app->fftPreviewCombo,
                               app->wideViewCheck,
                               app->ridgeOverlayCheck, app->diffWaterfallCheck,
                               app->dotDashAssistCheck, app->wideViewButton,
                               app->ridgeOverlayButton, app->freezeButton,
                               app->panAvgAlphaSlider, app->panPeakDecaySlider,
                               app->lensStrengthSlider, app->bgRemovalSlider,
                               app->splitPointSlider, app->autoFocusStrengthSlider,
                               app->agcFloorSlider, app->agcSpanSlider, app->agcGainSlider,
                               app->agcGammaSlider, app->agcAutoCheck,
                               app->peakLockButton, app->manualNotchCheck, app->autoBookmarkCheck,
                               app->autoBookmarkConfSlider, app->autoBookmarkMidSlider,
                               app->autoBookmarkHighSlider, app->resetUiButton,
                               app->priorEdit, app->priorCheck};
      for (HWND c : controls) {
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(app->font), TRUE);
      }

      app->tooltipWnd = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT,
                                        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hwnd, nullptr,
                                        nullptr, nullptr);
      if (app->tooltipWnd && app->resetUiButton) {
        TOOLINFOW ti = {};
        ti.cbSize = sizeof(ti);
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = hwnd;
        ti.uId = reinterpret_cast<UINT_PTR>(app->resetUiButton);
        ti.lpszText = const_cast<wchar_t*>(L"Reset all saved UI session settings to default values");
        GetClientRect(app->resetUiButton, &ti.rect);
        SendMessageW(app->tooltipWnd, TTM_ADDTOOL, 0, reinterpret_cast<LPARAM>(&ti));
      }

      wchar_t modPath[MAX_PATH] = {};
      GetModuleFileNameW(nullptr, modPath, MAX_PATH);
      std::filesystem::path p(modPath);
      app->uiStatePath = (p.parent_path() / L"ndb_gui_state.ini").wstring();
      auto hist = p.parent_path().parent_path().parent_path() / "benchmarks" / "phase0" / "history" /
                  "benchmark_history.csv";
      SetText(app->historyEdit, hist.wstring());
      LoadUiState(app);
      RefreshHistory(app);
      RefreshCompareHistory(app);
      RefreshWaterfallFromInput(app);
      CaptureBaseChildLayout(app);
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
          SaveUiState(app);
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
            SaveUiState(app);
            InvalidateRect(app->chartPanel, nullptr, TRUE);
          }
          return 0;
        case kIdPalettePresetCombo:
          if (HIWORD(wParam) == CBN_SELCHANGE && app->palettePresetCombo) {
            const int sel = static_cast<int>(SendMessageW(app->palettePresetCombo, CB_GETCURSEL, 0, 0));
            app->palettePreset = std::clamp(sel, 0, 5);
            if (app->palettePreset == 3 && app->customPalette.empty()) {
              app->palettePreset = 0;
              SendMessageW(app->palettePresetCombo, CB_SETCURSEL, 0, 0);
              SetStatus(app, L"Load a LUT file first");
            }
            SaveUiState(app);
            InvalidateWaterfallCache(app);
            InvalidateRect(app->chartPanel, nullptr, TRUE);
          }
          return 0;
        case kIdPaletteLoadLut: {
          const auto p = ChooseOpenFile(hwnd, L"Load palette LUT",
                                        L"Text files (*.txt;*.lut)\0*.txt;*.lut\0All files (*.*)\0*.*\0");
          if (p.empty()) return 0;
          std::vector<ColorStop> stops;
          std::string err;
          if (!LoadCustomPaletteLut(p, &stops, &err)) {
            SetStatus(app, L"LUT load failed");
            MessageBoxW(hwnd, ToWide(err).c_str(), L"Palette LUT", MB_OK | MB_ICONERROR);
            return 0;
          }
          app->customPalette = std::move(stops);
          app->customPalettePath = p;
          app->palettePreset = 3;
          if (app->palettePresetCombo) SendMessageW(app->palettePresetCombo, CB_SETCURSEL, 3, 0);
          SaveUiState(app);
          SetStatus(app, L"Custom LUT loaded");
          InvalidateWaterfallCache(app);
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        }
        case kIdShadingCheck:
          app->shadingEnabled =
              (SendMessageW(app->shadingCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
          SaveUiState(app);
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        case kIdColormapCombo:
          if (HIWORD(wParam) == CBN_SELCHANGE && app->colormapCombo) {
            const int sel = static_cast<int>(SendMessageW(app->colormapCombo, CB_GETCURSEL, 0, 0));
            app->colormap3d = std::clamp(sel, 0, 2);
            SaveUiState(app);
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
          app->waterfallPersistenceMode = 2;
          app->agcFloorOffsetDb = -6.0f;
          app->agcSpanDb = 20.0f;
          app->agcGain = 1.50f;
          app->agcGamma = 0.68f;
          app->agcAutoContrast = true;
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
          if (app->waterfallPersistCombo) {
            SendMessageW(app->waterfallPersistCombo, CB_SETCURSEL, app->waterfallPersistenceMode, 0);
          }
          if (app->agcFloorSlider) {
            SendMessageW(app->agcFloorSlider, TBM_SETPOS, TRUE, static_cast<LPARAM>(app->agcFloorOffsetDb));
          }
          if (app->agcSpanSlider) {
            SendMessageW(app->agcSpanSlider, TBM_SETPOS, TRUE, static_cast<LPARAM>(app->agcSpanDb));
          }
          if (app->agcGainSlider) {
            SendMessageW(app->agcGainSlider, TBM_SETPOS, TRUE,
                         static_cast<LPARAM>(std::round(app->agcGain * 100.0f)));
          }
          if (app->agcGammaSlider) {
            SendMessageW(app->agcGammaSlider, TBM_SETPOS, TRUE,
                         static_cast<LPARAM>(std::round(app->agcGamma * 100.0f)));
          }
          if (app->agcAutoCheck) {
            SendMessageW(app->agcAutoCheck, BM_SETCHECK,
                         app->agcAutoContrast ? BST_CHECKED : BST_UNCHECKED, 0);
          }
          if (app->running) {
            SetTimer(app->hwnd, kWaterfallTimerId, WaterfallTimerMs(app), nullptr);
          }
          SaveUiState(app);
          InvalidateWaterfallCache(app);
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        case kIdWaterfallFpsCombo:
          if (HIWORD(wParam) == CBN_SELCHANGE && app->waterfallFpsCombo) {
            const int sel = static_cast<int>(SendMessageW(app->waterfallFpsCombo, CB_GETCURSEL, 0, 0));
            app->waterfallFps = (sel == 1) ? 60 : 30;
            if (app->running) {
              SetTimer(app->hwnd, kWaterfallTimerId, WaterfallTimerMs(app), nullptr);
            }
            SaveUiState(app);
            InvalidateRect(app->chartPanel, nullptr, TRUE);
          }
          return 0;
        case kIdWaterfallPersistCombo:
          if (HIWORD(wParam) == CBN_SELCHANGE && app->waterfallPersistCombo) {
            const int sel = static_cast<int>(SendMessageW(app->waterfallPersistCombo, CB_GETCURSEL, 0, 0));
            app->waterfallPersistenceMode = std::clamp(sel, 0, 2);
            SaveUiState(app);
            InvalidateWaterfallCache(app);
            InvalidateRect(app->chartPanel, nullptr, TRUE);
          }
          return 0;
        case kIdFftPreviewCombo:
          if (HIWORD(wParam) == CBN_SELCHANGE && app->fftPreviewCombo) {
            const int sel = static_cast<int>(SendMessageW(app->fftPreviewCombo, CB_GETCURSEL, 0, 0));
            app->previewFftSize = (sel == 2) ? 1024 : (sel == 0 ? 256 : 512);
            SaveUiState(app);
            InvalidateWaterfallCache(app);
            InvalidateRect(app->chartPanel, nullptr, TRUE);
          }
          return 0;
        case kIdWideViewCheck:
          app->showWideView =
              (SendMessageW(app->wideViewCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
          UpdateWaterfallToggleButtons(app);
          SaveUiState(app);
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        case kIdRidgeOverlayCheck:
          app->showRidgeOverlay =
              (SendMessageW(app->ridgeOverlayCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
          UpdateWaterfallToggleButtons(app);
          SaveUiState(app);
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        case kIdDiffWaterfallCheck:
          app->differenceWaterfallEnabled =
              (SendMessageW(app->diffWaterfallCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
          SaveUiState(app);
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        case kIdDotDashAssistCheck:
          app->dotDashAssistEnabled =
              (SendMessageW(app->dotDashAssistCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
          SaveUiState(app);
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        case kIdWideViewButton:
          app->showWideView = !app->showWideView;
          if (app->wideViewCheck) {
            SendMessageW(app->wideViewCheck, BM_SETCHECK,
                         app->showWideView ? BST_CHECKED : BST_UNCHECKED, 0);
          }
          UpdateWaterfallToggleButtons(app);
          SaveUiState(app);
          SetStatus(app, app->showWideView ? L"Wide view ON" : L"Wide view OFF");
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        case kIdRidgeOverlayButton:
          app->showRidgeOverlay = !app->showRidgeOverlay;
          if (app->ridgeOverlayCheck) {
            SendMessageW(app->ridgeOverlayCheck, BM_SETCHECK,
                         app->showRidgeOverlay ? BST_CHECKED : BST_UNCHECKED, 0);
          }
          UpdateWaterfallToggleButtons(app);
          SaveUiState(app);
          SetStatus(app, app->showRidgeOverlay ? L"Ridge overlay ON" : L"Ridge overlay OFF");
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        case kIdAgcAutoCheck:
          app->agcAutoContrast =
              (SendMessageW(app->agcAutoCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
          SaveUiState(app);
          InvalidateWaterfallCache(app);
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        case kIdManualNotchCheck:
          app->manualNotchEnabled =
              (SendMessageW(app->manualNotchCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
          SaveUiState(app);
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        case kIdManualNotchClear:
          app->manualNotches.clear();
          app->activeManualNotch = -1;
          SaveUiState(app);
          SetStatus(app, L"Manual notches cleared");
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        case kIdManualNotchExport: {
          const auto p = ChooseSaveFile(hwnd, L"Export manual notch CSV",
                                        L"CSV files (*.csv)\0*.csv\0All files (*.*)\0*.*\0",
                                        L"csv");
          if (p.empty()) {
            return 0;
          }
          std::string err;
          if (!WriteManualNotchesCsv(ToUtf8(p), app->manualNotches, &err)) {
            SetStatus(app, L"Manual notch export failed");
            MessageBoxW(hwnd, ToWide(err).c_str(), L"Manual Notch", MB_OK | MB_ICONERROR);
            return 0;
          }
          SetStatus(app, L"Manual notches exported");
          return 0;
        }
        case kIdManualNotchImport: {
          const auto p = ChooseOpenFile(hwnd, L"Import manual notch CSV",
                                        L"CSV files (*.csv)\0*.csv\0All files (*.*)\0*.*\0");
          if (p.empty()) {
            return 0;
          }
          std::string err;
          std::vector<NotchBand> loaded;
          if (!ReadManualNotchesCsv(ToUtf8(p), &loaded, &err)) {
            SetStatus(app, L"Manual notch import failed");
            MessageBoxW(hwnd, ToWide(err).c_str(), L"Manual Notch", MB_OK | MB_ICONERROR);
            return 0;
          }
          app->manualNotches = std::move(loaded);
          ClampManualNotchesToRange(app);
          app->activeManualNotch = app->manualNotches.empty() ? -1 : 0;
          SaveUiState(app);
          std::wstringstream ss;
          ss << L"Manual notches imported: " << app->manualNotches.size();
          SetStatus(app, ss.str());
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        }
        case kIdAutoBookmarkCheck:
          app->autoBookmarkEnabled =
              (SendMessageW(app->autoBookmarkCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
          SaveUiState(app);
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        case kIdAutoMarkPresetDx:
          ApplyAutoMarkPreset(app, 0.78f, 0.74f, 0.88f, L"DX strict");
          SaveUiState(app);
          return 0;
        case kIdAutoMarkPresetBalanced:
          ApplyAutoMarkPreset(app, 0.65f, 0.65f, 0.85f, L"Balanced");
          SaveUiState(app);
          return 0;
        case kIdAutoMarkPresetWeak:
          ApplyAutoMarkPreset(app, 0.52f, 0.60f, 0.78f, L"Weak-Sig");
          SaveUiState(app);
          return 0;
        case kIdVisualPresetDxWeak:
          ApplyVisualTuningPreset(app, 1.38f, 1.25f, 0.52f, 0.45f, L"DX Weak");
          return 0;
        case kIdVisualPresetBalanced:
          ApplyVisualTuningPreset(app, 1.00f, 1.00f, 0.58f, 0.28f, L"Balanced");
          return 0;
        case kIdVisualPresetClean:
          ApplyVisualTuningPreset(app, 0.76f, 0.45f, 0.64f, 0.16f, L"Clean");
          return 0;
        case kIdQrmPresetNo:
          app->qrmBirdieSuppression = 0.72f;
          app->qrmRidgeAggressiveness = 0.82f;
          SaveUiState(app);
          InvalidateWaterfallCache(app);
          SetStatus(app, L"QRM preset: No-QRM");
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        case kIdQrmPresetHeavy:
          app->qrmBirdieSuppression = 1.42f;
          app->qrmRidgeAggressiveness = 1.35f;
          SaveUiState(app);
          InvalidateWaterfallCache(app);
          SetStatus(app, L"QRM preset: Heavy-QRM");
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        case kIdFreezeButton:
          ToggleWaterfallFreeze(app);
          return 0;
        case kIdPeakLockButton:
          app->peakLockEnabled = !app->peakLockEnabled;
          if (!app->peakLockEnabled) {
            app->peakLockBin = -1;
            app->peakLockHz = 0.0f;
          }
          SaveUiState(app);
          if (app->peakLockButton) {
            SetWindowTextW(app->peakLockButton,
                           app->peakLockEnabled ? L"Peak Lock: ON" : L"Peak Lock: OFF");
          }
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        case kIdBookmarkAdd: {
          const std::size_t before = app->bookmarksSec.size();
          AddBookmarkAtCurrent(app);
          const std::size_t after = app->bookmarksSec.size();
          if (after > before) {
            std::wstringstream ss;
            ss << L"Bookmark #" << after << L" added";
            SetStatus(app, ss.str());
          } else {
            SetStatus(app, L"Bookmark already present / unavailable");
          }
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        }
        case kIdBookmarkClear:
          app->bookmarksSec.clear();
          app->bookmarkAuto.clear();
          app->bookmarkConfidence.clear();
          SetStatus(app, L"Bookmarks cleared");
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        case kIdBookmarkExport: {
          const auto p = ChooseSaveFile(hwnd, L"Export bookmark CSV",
                                        L"CSV files (*.csv)\0*.csv\0All files (*.*)\0*.*\0",
                                        L"csv");
          if (p.empty()) {
            return 0;
          }
          std::string err;
          if (!WriteBookmarksCsv(ToUtf8(p), app->bookmarksSec, app->bookmarkAuto,
                                 app->bookmarkConfidence, &err)) {
            SetStatus(app, L"Bookmark export failed");
            MessageBoxW(hwnd, ToWide(err).c_str(), L"Bookmarks", MB_OK | MB_ICONERROR);
            return 0;
          }
          SetStatus(app, L"Bookmarks exported");
          return 0;
        }
        case kIdBookmarkImport: {
          const auto p = ChooseOpenFile(hwnd, L"Import bookmark CSV",
                                        L"CSV files (*.csv)\0*.csv\0All files (*.*)\0*.*\0");
          if (p.empty()) {
            return 0;
          }
          std::string err;
          std::vector<float> loaded;
          std::vector<std::uint8_t> loadedAuto;
          std::vector<float> loadedConf;
          if (!ReadBookmarksCsv(ToUtf8(p), &loaded, &loadedAuto, &loadedConf, &err)) {
            SetStatus(app, L"Bookmark import failed");
            MessageBoxW(hwnd, ToWide(err).c_str(), L"Bookmarks", MB_OK | MB_ICONERROR);
            return 0;
          }
          const float dur = PreviewDurationSec(app);
          if (dur > 0.0f) {
            for (std::size_t i = 0; i < loaded.size();) {
              if (loaded[i] > dur) {
                loaded.erase(loaded.begin() + static_cast<std::ptrdiff_t>(i));
                if (i < loadedAuto.size()) {
                  loadedAuto.erase(loadedAuto.begin() + static_cast<std::ptrdiff_t>(i));
                }
                if (i < loadedConf.size()) {
                  loadedConf.erase(loadedConf.begin() + static_cast<std::ptrdiff_t>(i));
                }
              } else {
                ++i;
              }
            }
          }
          app->bookmarksSec = std::move(loaded);
          app->bookmarkAuto = std::move(loadedAuto);
          app->bookmarkConfidence = std::move(loadedConf);
          std::wstringstream ss;
          ss << L"Bookmarks imported: " << app->bookmarksSec.size();
          SetStatus(app, ss.str());
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        }
        case kIdResetUiSession:
          if (!app->suppressNextResetConfirm) {
            const int r = MessageBoxW(
                hwnd,
                L"Reset all saved UI session settings to defaults?\n\nTip: hold Shift while pressing Ctrl+R to skip this confirmation once.",
                L"Reset Session UI", MB_OKCANCEL | MB_ICONQUESTION);
            if (r != IDOK) {
              return 0;
            }
          }
          app->suppressNextResetConfirm = false;
          ResetUiSessionState(app);
          SetStatus(app, L"Session UI reset to defaults");
          return 0;
      }
      return 0;
    }
    case WM_HSCROLL:
      if (app) {
        HWND src = reinterpret_cast<HWND>(lParam);
        if (src == app->yawSlider) {
          app->yawDeg = static_cast<int>(SendMessageW(app->yawSlider, TBM_GETPOS, 0, 0));
          SaveUiState(app);
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        }
        if (src == app->pitchSlider) {
          app->pitchDeg = static_cast<int>(SendMessageW(app->pitchSlider, TBM_GETPOS, 0, 0));
          SaveUiState(app);
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        }
        if (src == app->agcFloorSlider) {
          app->agcFloorOffsetDb = static_cast<float>(
              static_cast<int>(SendMessageW(app->agcFloorSlider, TBM_GETPOS, 0, 0)));
          SaveUiState(app);
          InvalidateWaterfallCache(app);
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        }
        if (src == app->agcSpanSlider) {
          app->agcSpanDb = static_cast<float>(
              static_cast<int>(SendMessageW(app->agcSpanSlider, TBM_GETPOS, 0, 0)));
          SaveUiState(app);
          InvalidateWaterfallCache(app);
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        }
        if (src == app->agcGainSlider) {
          const int v = static_cast<int>(SendMessageW(app->agcGainSlider, TBM_GETPOS, 0, 0));
          app->agcGain = static_cast<float>(v) / 100.0f;
          SaveUiState(app);
          InvalidateWaterfallCache(app);
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        }
        if (src == app->agcGammaSlider) {
          const int v = static_cast<int>(SendMessageW(app->agcGammaSlider, TBM_GETPOS, 0, 0));
          app->agcGamma = static_cast<float>(v) / 100.0f;
          SaveUiState(app);
          InvalidateWaterfallCache(app);
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        }
        if (src == app->panAvgAlphaSlider) {
          const int v = static_cast<int>(SendMessageW(app->panAvgAlphaSlider, TBM_GETPOS, 0, 0));
          app->panAvgAlpha = static_cast<float>(v) / 100.0f;
          SaveUiState(app);
          std::wstringstream ss;
          ss << L"Pan Avg Alpha: " << std::fixed << std::setprecision(2) << app->panAvgAlpha;
          SetStatus(app, ss.str());
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        }
        if (src == app->panPeakDecaySlider) {
          const int v = static_cast<int>(SendMessageW(app->panPeakDecaySlider, TBM_GETPOS, 0, 0));
          app->panPeakDecay = static_cast<float>(v) / 100.0f;
          SaveUiState(app);
          std::wstringstream ss;
          ss << L"Pan Peak Decay: " << std::fixed << std::setprecision(2) << app->panPeakDecay;
          SetStatus(app, ss.str());
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        }
        if (src == app->lensStrengthSlider) {
          const int v = static_cast<int>(SendMessageW(app->lensStrengthSlider, TBM_GETPOS, 0, 0));
          app->lensStrength = static_cast<float>(v) / 100.0f;
          SaveUiState(app);
          std::wstringstream ss;
          ss << L"Lens strength: " << std::fixed << std::setprecision(2) << app->lensStrength;
          SetStatus(app, ss.str());
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        }
        if (src == app->bgRemovalSlider) {
          const int v = static_cast<int>(SendMessageW(app->bgRemovalSlider, TBM_GETPOS, 0, 0));
          app->bgRemovalStrength = static_cast<float>(v) / 100.0f;
          SaveUiState(app);
          std::wstringstream ss;
          ss << L"BG removal: " << std::fixed << std::setprecision(2) << app->bgRemovalStrength;
          SetStatus(app, ss.str());
          InvalidateWaterfallCache(app);
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        }
        if (src == app->splitPointSlider) {
          const int v = static_cast<int>(SendMessageW(app->splitPointSlider, TBM_GETPOS, 0, 0));
          app->splitTonePoint = static_cast<float>(v) / 100.0f;
          SaveUiState(app);
          std::wstringstream ss;
          ss << L"Split point: " << std::fixed << std::setprecision(2) << app->splitTonePoint;
          SetStatus(app, ss.str());
          InvalidateWaterfallCache(app);
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        }
        if (src == app->autoFocusStrengthSlider) {
          const int v = static_cast<int>(SendMessageW(app->autoFocusStrengthSlider, TBM_GETPOS, 0, 0));
          app->autoFocusStrength = static_cast<float>(v) / 100.0f;
          SaveUiState(app);
          std::wstringstream ss;
          ss << L"Auto-focus strength: " << std::fixed << std::setprecision(2)
             << app->autoFocusStrength;
          SetStatus(app, ss.str());
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        }
        if (src == app->autoBookmarkConfSlider) {
          const int v = static_cast<int>(SendMessageW(app->autoBookmarkConfSlider, TBM_GETPOS, 0, 0));
          app->autoBookmarkMinConfidence = static_cast<float>(v) / 100.0f;
          SaveUiState(app);
          std::wstringstream ss;
          ss << L"Auto mark min conf: " << std::fixed << std::setprecision(2)
             << app->autoBookmarkMinConfidence;
          SetStatus(app, ss.str());
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        }
        if (src == app->autoBookmarkMidSlider) {
          int vm = static_cast<int>(SendMessageW(app->autoBookmarkMidSlider, TBM_GETPOS, 0, 0));
          int vh = static_cast<int>(SendMessageW(app->autoBookmarkHighSlider, TBM_GETPOS, 0, 0));
          vm = std::min(vm, vh - 1);
          SendMessageW(app->autoBookmarkMidSlider, TBM_SETPOS, TRUE, vm);
          app->autoBookmarkMidThreshold = static_cast<float>(vm) / 100.0f;
          SaveUiState(app);
          std::wstringstream ss;
          ss << L"Auto mark mid/high: " << std::fixed << std::setprecision(2)
             << app->autoBookmarkMidThreshold << L"/" << app->autoBookmarkHighThreshold;
          SetStatus(app, ss.str());
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        }
        if (src == app->autoBookmarkHighSlider) {
          int vm = static_cast<int>(SendMessageW(app->autoBookmarkMidSlider, TBM_GETPOS, 0, 0));
          int vh = static_cast<int>(SendMessageW(app->autoBookmarkHighSlider, TBM_GETPOS, 0, 0));
          vh = std::max(vh, vm + 1);
          SendMessageW(app->autoBookmarkHighSlider, TBM_SETPOS, TRUE, vh);
          app->autoBookmarkHighThreshold = static_cast<float>(vh) / 100.0f;
          SaveUiState(app);
          std::wstringstream ss;
          ss << L"Auto mark mid/high: " << std::fixed << std::setprecision(2)
             << app->autoBookmarkMidThreshold << L"/" << app->autoBookmarkHighThreshold;
          SetStatus(app, ss.str());
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        }
      }
      break;
    case WM_SIZE:
      if (app) {
        ApplyResponsiveLayout(app);
      }
      return 0;
    case kMsgProgress:
      if (app) {
        app->decodeProgressPct = static_cast<int>(wParam);
        UpdatePanadapterPersistence(app);
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
      SetTextColor(hdc, RGB(216, 228, 240));
      SetBkColor(hdc, RGB(18, 24, 32));
      static HBRUSH bg = CreateSolidBrush(RGB(18, 24, 32));
      return reinterpret_cast<LRESULT>(bg);
    }
    case WM_ERASEBKGND: {
      RECT r;
      GetClientRect(hwnd, &r);
      HBRUSH b = CreateSolidBrush(RGB(14, 20, 28));
      FillRect(reinterpret_cast<HDC>(wParam), &r, b);
      DeleteObject(b);
      return 1;
    }
    case WM_DESTROY:
      if (app) {
        SaveUiState(app);
      }
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
            if (app->manualNotchEnabled && (GetKeyState(VK_SHIFT) & 0x8000) &&
                app->activeManualNotch >= 0 &&
                app->activeManualNotch < static_cast<int>(app->manualNotches.size())) {
              NotchBand& n = app->manualNotches[static_cast<std::size_t>(app->activeManualNotch)];
              n.widthHz = std::clamp(n.widthHz + ((z > 0) ? 2.0f : -2.0f), 6.0f, 200.0f);
              SaveUiState(app);
              std::wstringstream ss;
              ss << L"Notch width: " << std::fixed << std::setprecision(1) << n.widthHz
                 << L" Hz";
              SetStatus(app, ss.str());
              InvalidateRect(app->chartPanel, nullptr, TRUE);
              return 0;
            }
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
        SaveUiState(app);
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
        UpdatePanadapterPersistence(app);
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
                              CW_USEDEFAULT, CW_USEDEFAULT, 1860, 1760, nullptr, nullptr, instance,
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

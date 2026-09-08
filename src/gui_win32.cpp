#ifdef _WIN32

#include "gui_win32.hpp"

#include "ndb_decoder.hpp"
#include "wav.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <gdiplus.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
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

constexpr UINT kMsgProgress = WM_APP + 1;
constexpr UINT kMsgDone = WM_APP + 2;

struct DecodeThreadResult {
  bool ok = false;
  std::string error;
  std::string outputPath;
  std::string metricsPath;
  ndb::DecodeStats stats;
  std::size_t rowCount = 0;
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
  HWND runButton = nullptr;
  HWND progressBar = nullptr;
  HWND statusText = nullptr;
  HWND summaryText = nullptr;
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
  double chartZoom = 1.0;
  int chartPanPx = 0;
  bool dragging = false;
  int dragStartX = 0;
  int panStartPx = 0;
  bool hoverActive = false;
  POINT hoverPoint = {0, 0};
  std::wstring hoverText;
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

  const int cols = 2;
  const int rows = 3;
  const int gap = 10;
  const int w = (rc.right - rc.left - gap * (cols + 1)) / cols;
  const int h = (rc.bottom - rc.top - gap * (rows + 1)) / rows;

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
      RECT cr = {rc.left + gap + c * (w + gap), rc.top + gap + r * (h + gap),
                 rc.left + gap + c * (w + gap) + w, rc.top + gap + r * (h + gap) + h};
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

  const int cols = 2;
  const int rows = 3;
  const int gap = 10;
  const int w = (rc.right - rc.left - gap * (cols + 1)) / cols;
  const int h = (rc.bottom - rc.top - gap * (rows + 1)) / rows;

  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      const int idx = r * cols + c;
      RECT cr = {rc.left + gap + c * (w + gap), rc.top + gap + r * (h + gap),
                 rc.left + gap + c * (w + gap) + w, rc.top + gap + r * (h + gap) + h};
      DrawMetricChart(hdc, cr, app->history, app->historyCompare, defs[idx], app->chartZoom,
                      app->chartPanPx);
    }
  }

  if (app->hoverActive) {
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
  SetBusy(app, true);
  SendMessageW(app->progressBar, PBM_SETPOS, 0, 0);
  SetStatus(app, L"Starting...");
  SetSummary(app, L"");

  const std::string inputPath = ToUtf8(inW);
  const std::string outputPath = ToUtf8(outW);
  const std::string metricsPath = ToUtf8(metW);
  int sel = 0;
  if (app->presetCombo) {
    sel = static_cast<int>(SendMessageW(app->presetCombo, CB_GETCURSEL, 0, 0));
  }
  std::string mode = "default";
  if (sel == 1) {
    mode = "strict-dx";
  } else if (sel == 2) {
    mode = "relaxed";
  } else if (sel == 3) {
    mode = "phase3-balanced";
  } else if (sel == 4) {
    mode = "phase3-selective";
  }

  app->worker = std::thread([app, inputPath, outputPath, metricsPath, mode]() {
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
    auto progress = [app](int percent, const std::string&) {
      PostMessageW(app->hwnd, kMsgProgress, static_cast<WPARAM>(percent), 0);
    };
    auto rows = ndb::DecodeNdbFromWav(wav.samples, wav.sampleRate, cfg, &result->stats, progress);
    result->rowCount = rows.size();

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
  SetBusy(app, false);
  SendMessageW(app->progressBar, PBM_SETPOS, 100, 0);

  if (!result->ok) {
    SetStatus(app, L"Failed");
    SetSummary(app, ToWide(result->error));
    MessageBoxW(app->hwnd, ToWide(result->error).c_str(), L"Decode failed", MB_ICONERROR | MB_OK);
  } else {
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
        app->dragging = true;
        app->dragStartX = GET_X_LPARAM(lParam);
        app->panStartPx = app->chartPanPx;
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
        if (app->dragging) {
          app->chartPanPx = app->panStartPx + (p.x - app->dragStartX);
          app->hoverActive = false;
          InvalidateRect(hwnd, nullptr, TRUE);
        } else {
          RECT rc;
          GetClientRect(hwnd, &rc);
          auto hit = HitTestCharts(app, rc, p);
          if (hit.ok) {
            app->hoverActive = true;
            app->hoverPoint = hit.pt;
            app->hoverText = hit.text;
          } else {
            app->hoverActive = false;
          }
          InvalidateRect(hwnd, nullptr, TRUE);
        }
      }
      return 0;
    case WM_LBUTTONUP:
      if (app && app->dragging) {
        app->dragging = false;
        ReleaseCapture();
      }
      return 0;
    case WM_MOUSELEAVE:
      if (app) {
        app->mouseLeaveArmed = false;
        app->hoverActive = false;
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
      DrawCharts(app, hdc, rc);
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
      const int leftW = 470;
      const int rightX = m + leftW + 12;
      const int rightW = 950 - rightX - m;
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
      SendMessageW(app->presetCombo, CB_SETCURSEL, 0, 0);
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
                                       m, y, leftW - 8, 280, hwnd, (HMENU)kIdSummary, nullptr, nullptr);
      SendMessageW(app->summaryText, WM_SETFONT, reinterpret_cast<WPARAM>(app->fontMono), TRUE);

      app->chartPanel = CreateWindowW(L"JNDBChartPanel", nullptr,
                                      WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP,
                                      rightX, 16, rightW, 560, hwnd, (HMENU)kIdChartPanel, nullptr,
                                      nullptr);
      SetWindowLongPtrW(app->chartPanel, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));

      const HWND controls[] = {app->runButton, app->statusText, app->historyEdit,
                               app->compareEdit, app->inputEdit, app->outputEdit, app->metricsEdit,
                               app->presetCombo};
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
          InvalidateRect(app->chartPanel, nullptr, TRUE);
          return 0;
        case kIdExportPng:
          ExportChartPanelPng(app);
          return 0;
        case kIdRun:
          StartDecode(app);
          return 0;
      }
      return 0;
    }
    case kMsgProgress:
      if (app) {
        SendMessageW(app->progressBar, PBM_SETPOS, static_cast<int>(wParam), 0);
        std::wstringstream ss;
        ss << L"Processing... " << static_cast<int>(wParam) << L"%";
        SetStatus(app, ss.str());
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
        app->chartZoom = std::clamp(app->chartZoom * factor, 1.0, 8.0);
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
                              CW_USEDEFAULT, CW_USEDEFAULT, 980, 640, nullptr, nullptr, instance,
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

#ifdef _WIN32

#include "gui_win32.hpp"

#include "ndb_decoder.hpp"
#include "wav.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "Comctl32.lib")

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

struct AppState {
  HWND hwnd = nullptr;
  HWND inputEdit = nullptr;
  HWND outputEdit = nullptr;
  HWND metricsEdit = nullptr;
  HWND runButton = nullptr;
  HWND progressBar = nullptr;
  HWND statusText = nullptr;
  HWND summaryText = nullptr;

  HFONT font = nullptr;
  HFONT fontBig = nullptr;

  std::atomic<bool> running{false};
  std::thread worker;
};

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

std::wstring ChooseOpenWav(HWND owner) {
  wchar_t fileName[MAX_PATH] = {};
  OPENFILENAMEW ofn = {};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = owner;
  ofn.lpstrFilter = L"WAV files (*.wav)\0*.wav\0All files (*.*)\0*.*\0";
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

  app->worker = std::thread([app, inputPath, outputPath, metricsPath]() {
    auto* result = new DecodeThreadResult();
    result->outputPath = outputPath;
    result->metricsPath = metricsPath;

    ndb::WavData wav;
    if (!ndb::ReadWavMono16(inputPath, &wav, &result->error)) {
      PostMessageW(app->hwnd, kMsgDone, 0, reinterpret_cast<LPARAM>(result));
      return;
    }

    ndb::DecoderConfig cfg;
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

      const int m = 18;
      const int lw = 120;
      const int bh = 34;
      const int eh = 30;
      const int bw = 120;
      int y = 20;

      HWND title = CreateWindowW(L"STATIC", L"JNDB Decoder", WS_CHILD | WS_VISIBLE, m, y, 400, 36,
                                 hwnd, nullptr, nullptr, nullptr);
      SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(app->fontBig), TRUE);
      y += 48;

      CreateWindowW(L"STATIC", L"Input WAV", WS_CHILD | WS_VISIBLE, m, y + 6, lw, 24, hwnd, nullptr,
                    nullptr, nullptr);
      app->inputEdit = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                     m + lw, y, 430, eh, hwnd, (HMENU)kIdInputEdit, nullptr, nullptr);
      CreateWindowW(L"BUTTON", L"Browse", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, m + lw + 438, y,
                    bw, bh, hwnd, (HMENU)kIdInputBrowse, nullptr, nullptr);
      y += 46;

      CreateWindowW(L"STATIC", L"Output CSV", WS_CHILD | WS_VISIBLE, m, y + 6, lw, 24, hwnd, nullptr,
                    nullptr, nullptr);
      app->outputEdit = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                      m + lw, y, 430, eh, hwnd, (HMENU)kIdOutputEdit, nullptr, nullptr);
      CreateWindowW(L"BUTTON", L"Browse", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, m + lw + 438, y,
                    bw, bh, hwnd, (HMENU)kIdOutputBrowse, nullptr, nullptr);
      y += 46;

      CreateWindowW(L"STATIC", L"Metrics JSON", WS_CHILD | WS_VISIBLE, m, y + 6, lw, 24, hwnd,
                    nullptr, nullptr, nullptr);
      app->metricsEdit = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                       m + lw, y, 430, eh, hwnd, (HMENU)kIdMetricsEdit, nullptr,
                                       nullptr);
      CreateWindowW(L"BUTTON", L"Browse", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, m + lw + 438, y,
                    bw, bh, hwnd, (HMENU)kIdMetricsBrowse, nullptr, nullptr);
      y += 56;

      app->runButton = CreateWindowW(L"BUTTON", L"Start Decode", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                     m + lw, y, 180, 40, hwnd, (HMENU)kIdRun, nullptr, nullptr);
      y += 50;

      app->progressBar = CreateWindowW(PROGRESS_CLASSW, nullptr,
                                       WS_CHILD | WS_VISIBLE | PBS_SMOOTH, m, y, 690, 24, hwnd,
                                       (HMENU)kIdProgress, nullptr, nullptr);
      SendMessageW(app->progressBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
      y += 34;

      app->statusText = CreateWindowW(L"STATIC", L"Ready", WS_CHILD | WS_VISIBLE, m, y, 690, 24, hwnd,
                                      (HMENU)kIdStatus, nullptr, nullptr);
      y += 30;

      app->summaryText = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE |
                                                     ES_READONLY | WS_VSCROLL,
                                       m, y, 690, 150, hwnd, (HMENU)kIdSummary, nullptr, nullptr);

      const HWND controls[] = {app->inputEdit, app->outputEdit, app->metricsEdit, app->runButton,
                               app->statusText, app->summaryText};
      for (HWND c : controls) {
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(app->font), TRUE);
      }

      return 0;
    }
    case WM_COMMAND: {
      if (!app) {
        return 0;
      }
      switch (LOWORD(wParam)) {
        case kIdInputBrowse: {
          const std::wstring p = ChooseOpenWav(hwnd);
          if (!p.empty()) {
            SetText(app->inputEdit, p);
            if (GetText(app->outputEdit).empty()) {
              std::wstring csv = p;
              const auto pos = csv.find_last_of(L'.');
              if (pos != std::wstring::npos) {
                csv = csv.substr(0, pos);
              }
              csv += L"_out.csv";
              SetText(app->outputEdit, csv);
            }
            if (GetText(app->metricsEdit).empty()) {
              std::wstring js = p;
              const auto pos = js.find_last_of(L'.');
              if (pos != std::wstring::npos) {
                js = js.substr(0, pos);
              }
              js += L"_metrics.json";
              SetText(app->metricsEdit, js);
            }
          }
          return 0;
        }
        case kIdOutputBrowse: {
          const std::wstring p =
              ChooseSaveFile(hwnd, L"Save CSV", L"CSV files (*.csv)\0*.csv\0All files (*.*)\0*.*\0", L"csv");
          if (!p.empty()) {
            SetText(app->outputEdit, p);
          }
          return 0;
        }
        case kIdMetricsBrowse: {
          const std::wstring p = ChooseSaveFile(
              hwnd, L"Save metrics JSON",
              L"JSON files (*.json)\0*.json\0All files (*.*)\0*.*\0", L"json");
          if (!p.empty()) {
            SetText(app->metricsEdit, p);
          }
          return 0;
        }
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
      SetTextColor(hdc, RGB(30, 30, 30));
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

  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"JNDB - NDB Decoder", WS_OVERLAPPED | WS_CAPTION |
                                                   WS_SYSMENU | WS_MINIMIZEBOX,
                              CW_USEDEFAULT, CW_USEDEFAULT, 760, 560, nullptr, nullptr, instance,
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

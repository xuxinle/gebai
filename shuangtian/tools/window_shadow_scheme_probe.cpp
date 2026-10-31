// shadow_probe_v5 —— 目标：**只**要 DWM 外阴影，不要系统圆角、不要系统 1px 描边。
//
// 判据（每项都独立量，不许互相将就）：
//   - 阴影：窗外 1..20px 的变暗剖面 + 随距离单调回升；
//   - 圆角：窗口矩形四角 3x3 像素是否仍等于填充色（被裁 → 变成背板色）；
//   - 描边：窗内 1..2px 与中心的亮度差（系统描边会显著偏暗）；
//   - 客户区：是否仍 == 目标尺寸（吃掉像素就会让"画布==客户区"不成立 → DXGI 缩放发糊）。
//
// 候选来源：Win11 下把窗口当"有框窗口"由 DWM 画阴影；`DwmExtendFrameIntoClientArea`
// 只是其中一种触发方式，另有 `WS_CAPTION` 组合。副作用则可用
// `DWMWA_WINDOW_CORNER_PREFERENCE`（34=圆角偏好）与 `DWMWA_BORDER_COLOR`（34? 见下）关掉。
//
// 编译：g++ -std=c++17 -O2 shadow_probe_v5.cpp -o shadow_probe_v5.exe -ldwmapi -lgdi32 -luser32

#include <windows.h>
#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int kW = 340;
constexpr int kH = 240;
constexpr int kBand = 20;
constexpr int kPad = kBand + 24;
constexpr COLORREF kBack = RGB(120, 122, 126);
constexpr COLORREF kFill = RGB(246, 246, 248);

// Win11 私有属性号（未进 mingw 头，按 SDK 值写死）
constexpr DWORD kDwmwaWindowCornerPreference = 33;
constexpr DWORD kDwmwaBorderColor = 34;
constexpr DWORD kDwmwaCaptionColor = 35;
constexpr int kCornerDefault = 0, kCornerDoNotRound = 1, kCornerRound = 2, kCornerRoundSmall = 3;
constexpr DWORD kColorNone = 0xFFFFFFFE;  // DWMWA_COLOR_NONE

// 被测方案的开关。窗口过程没有参数可传，只能用进程级状态回填当前这组实验参数——
// 这是探针的固有形态（不是交付代码里的共享可变状态），已在 CONVENTIONS §8 登记。
struct ProbeState {
  COLORREF fill = kFill;
  bool nccalc_zero = true;
};
// lint-allow: L8 窗口过程只能靠进程级状态拿「当前这一组实验参数」
ProbeState g_state;

LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_ERASEBKGND || msg == WM_PAINT) {
    HDC dc;
    PAINTSTRUCT ps{};
    bool began = false;
    if (msg == WM_PAINT) { dc = ::BeginPaint(hwnd, &ps); began = true; }
    else dc = reinterpret_cast<HDC>(wp);
    RECT rc{};
    ::GetClientRect(hwnd, &rc);
    HBRUSH b = ::CreateSolidBrush(g_state.fill);
    ::FillRect(dc, &rc, b);
    ::DeleteObject(b);
    if (began) ::EndPaint(hwnd, &ps);
    return (msg == WM_PAINT) ? 0 : 1;
  }
  if (msg == WM_NCCALCSIZE && g_state.nccalc_zero && wp != 0) return 0;
  return ::DefWindowProcW(hwnd, msg, wp, lp);
}

void pump(DWORD ms) {
  const ULONGLONG end = ::GetTickCount64() + ms;
  while (::GetTickCount64() < end) {
    MSG m{};
    while (::PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { ::TranslateMessage(&m); ::DispatchMessageW(&m); }
    ::Sleep(4);
  }
}

struct Shot {
  int w = 0, h = 0;
  std::vector<unsigned char> bgra;
  const unsigned char* at(int x, int y) const {
    if (x < 0 || y < 0 || x >= w || y >= h) return nullptr;
    return &bgra[(static_cast<size_t>(y) * w + x) * 4];
  }
};

bool grab(int x, int y, int w, int h, Shot& s) {
  HDC screen = ::GetDC(nullptr);
  HDC mem = ::CreateCompatibleDC(screen);
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP dib = ::CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  bool ok = false;
  if (dib != nullptr && bits != nullptr) {
    HGDIOBJ old = ::SelectObject(mem, dib);
    if (::BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY | CAPTUREBLT) != 0) {
      s.w = w;
      s.h = h;
      s.bgra.assign(static_cast<unsigned char*>(bits),
                    static_cast<unsigned char*>(bits) + static_cast<size_t>(w) * h * 4);
      ok = true;
    }
    ::SelectObject(mem, old);
  }
  if (dib) ::DeleteObject(dib);
  ::DeleteDC(mem);
  ::ReleaseDC(nullptr, screen);
  return ok;
}

double lum(const unsigned char* p) { return 0.114 * p[0] + 0.587 * p[1] + 0.299 * p[2]; }

struct Variant {
  const char* name;
  DWORD style_extra;   // 叠加在 WS_POPUP|WS_VISIBLE|WS_CLIPCHILDREN 上
  DWORD exstyle;
  int extend;          // -1 = 不调用
  int corner;          // -1 = 不设置
  DWORD border;        // 0 = 不设置
};

struct Measure {
  double profile[kBand + 1] = {0};
  double base = 0.0;
  double peak = 0.0;
  int peak_d = 0;
  bool corners_equal_fill = false;
  double corner_delta[4] = {0};
  double inside_edge = 0.0;
  int client_w = 0, client_h = 0;
  bool content_ok = false;
};

void run(const Variant& v, int idx, std::string& report) {
  const std::wstring cls = L"v5_" + std::to_wstring(idx);
  const wchar_t* bcls_src = L"v5_back_fixed";
  (void)bcls_src;
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &proc;
  wc.hInstance = ::GetModuleHandleW(nullptr);
  wc.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));

  const int wx = 260, wy = 200;
  g_state.nccalc_zero = false;
  g_state.fill = kBack;
  wc.lpszClassName = L"v5_back_fixed";
  ::RegisterClassExW(&wc);
  HWND back = ::CreateWindowExW(0, L"v5_back_fixed", L"back", WS_POPUP | WS_VISIBLE, wx - kPad,
                                wy - kPad, kW + 2 * kPad, kH + 2 * kPad, nullptr, nullptr,
                                ::GetModuleHandleW(nullptr), nullptr);

  g_state.nccalc_zero = true;
  g_state.fill = kFill;
  wc.lpszClassName = cls.c_str();
  ::RegisterClassExW(&wc);
  const DWORD style = WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN | v.style_extra;
  HWND win = ::CreateWindowExW(v.exstyle, cls.c_str(), L"probe", style, wx, wy, kW, kH, nullptr,
                               nullptr, ::GetModuleHandleW(nullptr), nullptr);
  char line[512];
  if (back == nullptr || win == nullptr) {
    std::snprintf(line, sizeof(line), "%-34s CREATE_FAILED err=%lu\n", v.name, ::GetLastError());
    report += line;
    return;
  }
  if (v.extend >= 0) {
    MARGINS m{v.extend, v.extend, v.extend, v.extend};
    const HRESULT hr = ::DwmExtendFrameIntoClientArea(win, &m);
    if (hr != S_OK) {
      std::snprintf(line, sizeof(line), "  [%s] extend hr=0x%08lX\n", v.name,
                    static_cast<unsigned long>(hr));
      report += line;
    }
  }
  if (v.corner >= 0) {
    const int pref = v.corner;
    const HRESULT hr = ::DwmSetWindowAttribute(win, kDwmwaWindowCornerPreference, &pref, sizeof(pref));
    if (hr != S_OK) {
      std::snprintf(line, sizeof(line), "  [%s] corner hr=0x%08lX\n", v.name,
                    static_cast<unsigned long>(hr));
      report += line;
    }
  }
  if (v.border != 0) {
    const COLORREF c = v.border;
    const HRESULT hr = ::DwmSetWindowAttribute(win, kDwmwaBorderColor, &c, sizeof(c));
    if (hr != S_OK) {
      std::snprintf(line, sizeof(line), "  [%s] border hr=0x%08lX\n", v.name,
                    static_cast<unsigned long>(hr));
      report += line;
    }
  }
  ::SetWindowPos(back, HWND_TOPMOST, wx - kPad, wy - kPad, kW + 2 * kPad, kH + 2 * kPad,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
  pump(180);
  ::SetWindowPos(win, HWND_TOPMOST, wx, wy, kW, kH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
  ::UpdateWindow(win);
  ::BringWindowToTop(win);
  pump(700);

  RECT wr{}, cr{};
  ::GetWindowRect(win, &wr);
  ::GetClientRect(win, &cr);
  Shot s;
  Measure m;
  const bool ok = grab(wx - kPad, wy - kPad, kW + 2 * kPad, kH + 2 * kPad, s);
  if (!ok) {
    std::snprintf(line, sizeof(line), "%-34s GRAB_FAILED\n", v.name);
    report += line;
    ::DestroyWindow(win);
    ::DestroyWindow(back);
    return;
  }
  // 内容校验：中心必须是填充色（否则窗口没上屏）
  const unsigned char* center = s.at(wx + kW / 2 - (wx - kPad), wy + kH / 2 - (wy - kPad));
  const int fb = static_cast<int>(kFill & 0xFF), fg = static_cast<int>((kFill >> 8) & 0xFF),
            fr = static_cast<int>((kFill >> 16) & 0xFF);
  m.content_ok = center != nullptr && std::abs(center[0] - fb) <= 2 &&
                 std::abs(center[1] - fg) <= 2 && std::abs(center[2] - fr) <= 2;
  m.client_w = cr.right;
  m.client_h = cr.bottom;
  if (m.content_ok) {
    const int right_x = wr.right - 1;
    const int bottom_y = wr.bottom - 1;
    auto col_avg = [&](int d) {
      double sum = 0.0;
      int n = 0;
      for (int y = wr.top + 6; y < wr.bottom - 6; ++y) {
        const unsigned char* p = s.at(right_x + d - (wx - kPad), y - (wy - kPad));
        if (p) { sum += lum(p); ++n; }
      }
      return n ? sum / n : 0.0;
    };
    for (int d = 1; d <= kBand; ++d) m.profile[d] = col_avg(d);
    m.base = col_avg(kBand);  // 最远处当基线（背板本色）
    for (int d = 1; d <= kBand; ++d) {
      const double delta = m.base - m.profile[d];
      if (delta > m.peak) { m.peak = delta; m.peak_d = d; }
    }
    // 四角 3x3 平均 vs 填充色
    const int cxs[4] = {wr.left + 1, wr.right - 3, wr.left + 1, wr.right - 3};
    const int cys[4] = {wr.top + 1, wr.top + 1, wr.bottom - 3, wr.bottom - 3};
    double worst = 0.0;
    bool all_fill = true;
    for (int k = 0; k < 4; ++k) {
      double sum = 0.0;
      int n = 0;
      for (int dy = 0; dy < 3; ++dy) {
        for (int dx = 0; dx < 3; ++dx) {
          const unsigned char* p = s.at(cxs[k] + dx - (wx - kPad), cys[k] + dy - (wy - kPad));
          if (p) { sum += lum(p); ++n; }
        }
      }
      const double avg = n ? sum / n : 0.0;
      m.corner_delta[k] = lum(center) - avg;
      worst = std::max(worst, std::fabs(m.corner_delta[k]));
      if (std::fabs(m.corner_delta[k]) > 3.0) all_fill = false;
    }
    m.corners_equal_fill = all_fill;
    // 窗内 1..2px
    double edge = 0.0;
    int en = 0;
    for (int d = 1; d <= 2; ++d) {
      for (int y = wr.top + 8; y < wr.bottom - 8; ++y) {
        const unsigned char* p = s.at(wr.left + d - (wx - kPad), y - (wy - kPad));
        if (p) { edge += lum(p); ++en; }
      }
      for (int x = wr.left + 8; x < wr.right - 8; ++x) {
        const unsigned char* p = s.at(x - (wx - kPad), wr.top + d - (wy - kPad));
        if (p) { edge += lum(p); ++en; }
      }
    }
    m.inside_edge = en ? lum(center) - edge / en : 0.0;
  }

  std::snprintf(line, sizeof(line),
                "%-34s client=%dx%d shadow_peak=%5.1f@%dpx corners_ok=%d inside_edge=%5.1f "
                "onscreen=%d\n",
                v.name, m.client_w, m.client_h, m.peak, m.peak_d, m.corners_equal_fill ? 1 : 0,
                m.inside_edge, m.content_ok ? 1 : 0);
  report += line;
  if (m.content_ok) {
    std::string prof = "  阴影剖面:";
    char cell[24];
    for (int d = 1; d <= 12; ++d) {
      std::snprintf(cell, sizeof(cell), " %5.1f", m.base - m.profile[d]);
      prof += cell;
    }
    std::snprintf(cell, sizeof(cell), "   base=%.1f\n", m.base);
    report += prof + cell;
    std::snprintf(line, sizeof(line), "  角点 Δ: %5.1f %5.1f %5.1f %5.1f\n", m.corner_delta[0],
                  m.corner_delta[1], m.corner_delta[2], m.corner_delta[3]);
    report += line;
  }
  ::DestroyWindow(win);
  ::DestroyWindow(back);
  ::UnregisterClassW(cls.c_str(), ::GetModuleHandleW(nullptr));
  pump(120);
}

}  // namespace

int main() {
  if (auto f = reinterpret_cast<BOOL(WINAPI*)(intptr_t)>(
          ::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))) {
    f(-4);
  }
  const std::vector<Variant> variants = {
      {"A_现状(无任何处理)", WS_THICKFRAME | WS_MINIMIZEBOX, 0, -1, -1, 0},
      {"B_extend1", WS_THICKFRAME | WS_MINIMIZEBOX, 0, 1, -1, 0},
      {"C_extend1+corner_noround", WS_THICKFRAME | WS_MINIMIZEBOX, 0, 1, kCornerDoNotRound, 0},
      {"D_extend1+corner_noround+no_border", WS_THICKFRAME | WS_MINIMIZEBOX, 0, 1,
       kCornerDoNotRound, kColorNone},
      {"E_caption+thick(无extend)", WS_THICKFRAME | WS_MINIMIZEBOX | WS_CAPTION, 0, -1,
       kCornerDoNotRound, kColorNone},
      {"F_caption+extend1+noround+noborder", WS_THICKFRAME | WS_MINIMIZEBOX | WS_CAPTION, 0, 1,
       kCornerDoNotRound, kColorNone},
  };
  std::string report;
  int idx = 0;
  for (const auto& v : variants) run(v, idx++, report);
  std::fputs(report.c_str(), stdout);
  if (FILE* f = std::fopen("shadow_probe_v5_report.txt", "wb")) {
    std::fwrite(report.data(), 1, report.size(), f);
    std::fclose(f);
  }
  return 0;
}

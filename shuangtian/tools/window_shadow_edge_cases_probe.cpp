// shadow_probe_v7 —— 边界条件：① 非可缩放窗口（无 WS_THICKFRAME）；② 最大化后的窗口。
// 这两条最容易在"只测了普通窗口"之后炸：前者可能不触发 DWM 阴影，后者可能多出 1px 边。
//
// 编译：g++ -std=c++17 -O2 shadow_probe_v7.cpp -o shadow_probe_v7.exe -ldwmapi -lgdi32 -luser32

#include <windows.h>
#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
constexpr int kW = 300, kH = 200, kBand = 20, kPad = 50;
constexpr COLORREF kBack = RGB(120, 122, 126);
constexpr COLORREF kFill = RGB(246, 246, 248);
constexpr DWORD kDwmwaCorner = 33;
constexpr DWORD kDwmwaBorder = 34;
constexpr DWORD kColorNone = 0xFFFFFFFE;

COLORREF g_fill = kFill;
bool g_nccalc = true;

LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_ERASEBKGND || m == WM_PAINT) {
    HDC dc;
    PAINTSTRUCT ps{};
    bool began = false;
    if (m == WM_PAINT) { dc = ::BeginPaint(h, &ps); began = true; } else dc = reinterpret_cast<HDC>(w);
    RECT rc{};
    ::GetClientRect(h, &rc);
    HBRUSH b = ::CreateSolidBrush(g_fill);
    ::FillRect(dc, &rc, b);
    ::DeleteObject(b);
    if (began) ::EndPaint(h, &ps);
    return (m == WM_PAINT) ? 0 : 1;
  }
  if (m == WM_NCCALCSIZE && g_nccalc && w != 0) return 0;
  return ::DefWindowProcW(h, m, w, l);
}

void pump(DWORD ms) {
  const ULONGLONG end = ::GetTickCount64() + ms;
  while (::GetTickCount64() < end) {
    MSG msg{};
    while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { ::TranslateMessage(&msg); ::DispatchMessageW(&msg); }
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
  if (dib && bits) {
    HGDIOBJ old = ::SelectObject(mem, dib);
    if (::BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY | CAPTUREBLT)) {
      s.w = w; s.h = h;
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

void apply_modern(HWND w) {
  MARGINS m{1, 1, 1, 1};
  const HRESULT hr1 = ::DwmExtendFrameIntoClientArea(w, &m);
  const int pref = 1;
  const HRESULT hr2 = ::DwmSetWindowAttribute(w, kDwmwaCorner, &pref, sizeof(pref));
  const COLORREF none = kColorNone;
  const HRESULT hr3 = ::DwmSetWindowAttribute(w, kDwmwaBorder, &none, sizeof(none));
  std::printf("   DwmExtend=0x%08lX corner=0x%08lX border=0x%08lX\n",
              static_cast<unsigned long>(hr1), static_cast<unsigned long>(hr2),
              static_cast<unsigned long>(hr3));
}

// 量：窗外右/下侧 1..kBand 的变暗量（相对最远处基线）
void report_shadow(const Shot& s, int wx, int wy, int ww, int wh, int ox, int oy, const char* tag) {
  const unsigned char* center = s.at(wx + ww / 2 - ox, wy + wh / 2 - oy);
  if (!center) { std::printf("   %s: 中心出界\n", tag); return; }
  auto col = [&](int d) {
    double sum = 0.0; int n = 0;
    for (int y = wy + 6; y < wy + wh - 6; ++y) {
      const unsigned char* p = s.at(wx + ww - 1 + d - ox, y - oy);
      if (p) { sum += lum(p); ++n; }
    }
    return n ? sum / n : 0.0;
  };
  auto row = [&](int d) {
    double sum = 0.0; int n = 0;
    for (int x = wx + 6; x < wx + ww - 6; ++x) {
      const unsigned char* p = s.at(x - ox, wy + wh - 1 + d - oy);
      if (p) { sum += lum(p); ++n; }
    }
    return n ? sum / n : 0.0;
  };
  const double base = col(kBand);
  std::printf("   %s right:", tag);
  for (int d = 1; d <= 8; ++d) std::printf(" %5.1f", base - col(d));
  std::printf("   bottom:");
  const double base2 = row(kBand);
  for (int d = 1; d <= 8; ++d) std::printf(" %5.1f", base2 - row(d));
  std::printf("   (中心=%d,%d,%d)\n", center[0], center[1], center[2]);
}

}  // namespace

int main() {
  if (auto f = reinterpret_cast<BOOL(WINAPI*)(intptr_t)>(
          ::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))) {
    f(-4);
  }
  RECT work{};
  ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &proc;
  wc.hInstance = ::GetModuleHandleW(nullptr);
  wc.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));

  const int ox = 200, oy = 180;
  const int tw = kW + 2 * kPad, th = kH + 2 * kPad;
  g_nccalc = false;
  g_fill = kBack;
  wc.lpszClassName = L"v7back";
  ::RegisterClassExW(&wc);
  HWND back = ::CreateWindowExW(0, L"v7back", L"b", WS_POPUP | WS_VISIBLE, ox, oy, tw, th, nullptr,
                                nullptr, ::GetModuleHandleW(nullptr), nullptr);

  g_nccalc = true;
  g_fill = kFill;

  // ── ① 非可缩放窗口（无 WS_THICKFRAME）──
  wc.lpszClassName = L"v7fixed";
  ::RegisterClassExW(&wc);
  HWND w1 = ::CreateWindowExW(0, L"v7fixed", L"fixed", WS_POPUP | WS_VISIBLE, ox + kPad, oy + kPad,
                              kW, kH, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
  std::printf("[1] 无 WS_THICKFRAME（不可缩放）:\n");
  apply_modern(w1);
  ::SetWindowPos(back, HWND_TOPMOST, ox, oy, tw, th, SWP_NOACTIVATE | SWP_SHOWWINDOW);
  ::SetWindowPos(w1, HWND_TOPMOST, ox + kPad, oy + kPad, kW, kH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
  ::UpdateWindow(w1);
  ::BringWindowToTop(w1);
  pump(700);
  RECT r1{}, c1{};
  ::GetWindowRect(w1, &r1);
  ::GetClientRect(w1, &c1);
  std::printf("   client=%ldx%ld win=%ldx%ld\n", c1.right, c1.bottom, r1.right - r1.left,
              r1.bottom - r1.top);
  {
    Shot s;
    if (grab(ox, oy, tw, th, s)) report_shadow(s, r1.left, r1.top, r1.right - r1.left, r1.bottom - r1.top, ox, oy, "fixed");
  }
  ::DestroyWindow(w1);

  // ── ② 最大化 ──
  wc.lpszClassName = L"v7max";
  ::RegisterClassExW(&wc);
  HWND w2 = ::CreateWindowExW(0, L"v7max", L"max", WS_POPUP | WS_VISIBLE | WS_THICKFRAME, ox + kPad,
                              oy + kPad, kW, kH, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
  std::printf("[2] 最大化（工作区 %ldx%ld @ %ld,%ld）:\n", work.right - work.left,
              work.bottom - work.top, work.left, work.top);
  apply_modern(w2);
  ::SetWindowPos(w2, HWND_TOPMOST, work.left, work.top, work.right - work.left,
                 work.bottom - work.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
  ::UpdateWindow(w2);
  pump(800);
  RECT r2{}, c2{};
  ::GetWindowRect(w2, &r2);
  ::GetClientRect(w2, &c2);
  std::printf("   win=(%ld,%ld) %ldx%ld client=%ldx%ld  差(工作区-窗口)=%ld,%ld\n", r2.left, r2.top,
              r2.right - r2.left, r2.bottom - r2.top, c2.right, c2.bottom,
              (work.right - work.left) - (r2.right - r2.left),
              (work.bottom - work.top) - (r2.bottom - r2.top));
  // 最大化时检查左边/上边 1..3 px 是否有系统描边（与内容色比较）
  {
    HDC screen = ::GetDC(nullptr);
    int diffs = 0;
    for (int d = 0; d < 3; ++d) {
      const COLORREF c = ::GetPixel(screen, r2.left + d, r2.top + 300);
      const int r = static_cast<int>(GetRValue(c)), g = static_cast<int>(GetGValue(c)),
                b = static_cast<int>(GetBValue(c));
      std::printf("   左边距 %dpx: RGB(%d,%d,%d)\n", d, r, g, b);
      if (std::abs(r - 246) > 3 || std::abs(g - 246) > 3 || std::abs(b - 248) > 3) ++diffs;
    }
    for (int d = 0; d < 3; ++d) {
      const COLORREF c = ::GetPixel(screen, r2.left + 400, r2.top + d);
      const int r = static_cast<int>(GetRValue(c)), g = static_cast<int>(GetGValue(c)),
                b = static_cast<int>(GetBValue(c));
      std::printf("   上边距 %dpx: RGB(%d,%d,%d)\n", d, r, g, b);
      if (std::abs(r - 246) > 3 || std::abs(g - 246) > 3 || std::abs(b - 248) > 3) ++diffs;
    }
    ::ReleaseDC(nullptr, screen);
    std::printf("   边缘异常像素(超出3)=%d\n", diffs);
  }
  ::DestroyWindow(w2);
  ::DestroyWindow(back);
  return 0;
}

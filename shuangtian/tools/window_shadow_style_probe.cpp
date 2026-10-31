// shadow_probe_v8 —— 不可缩放窗口的阴影：DWM 到底认哪个"有框"标志？
//
// 背景：v7 实测 —— 没有 WS_THICKFRAME 的 WS_POPUP 窗口，即使 extend_frame + corner + border
// 全设了，窗外阴影仍是 0.0。而"不可缩放"是 API 里的一等公民（options.resizable = false），
// 不能要求应用为了阴影把窗口变成可缩放。
//
// 思路：找一个**只影响 DWM 判断、不影响交互**的标志。候选：
//   - WS_BORDER / WS_DLGFRAME / WS_CAPTION（薄框一族）
//   - WS_THICKFRAME + WM_GETMINMAXINFO 把 min == max（可缩放标志在、但拖不动）
// 判据：阴影有（窗外显著变暗且单调）+ 命中在边缘仍是 HTCLIENT（不会真的能拖边）。
//
// 编译：g++ -std=c++17 -O2 shadow_probe_v8.cpp -o shadow_probe_v8.exe -ldwmapi -lgdi32 -luser32

#include <windows.h>
#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {
constexpr int kW = 300, kH = 200, kBand = 20, kPad = 50;
constexpr COLORREF kBack = RGB(120, 122, 126);
constexpr COLORREF kFill = RGB(246, 246, 248);
constexpr DWORD kDwmwaCorner = 33;
constexpr DWORD kDwmwaBorder = 34;
constexpr DWORD kColorNone = 0xFFFFFFFE;

// 被测方案的开关。窗口过程没有参数可传，只能用进程级状态回填当前这组实验参数——
// 这是探针的固有形态（不是交付代码里的共享可变状态），已在 CONVENTIONS §8 登记。
struct ProbeState {
  COLORREF fill = kFill;
  bool nccalc = true;
  bool lock_size = false;
};
// lint-allow: L8 窗口过程只能靠进程级状态拿「当前这一组实验参数」
ProbeState g_state;

LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_ERASEBKGND || m == WM_PAINT) {
    HDC dc;
    PAINTSTRUCT ps{};
    bool began = false;
    if (m == WM_PAINT) { dc = ::BeginPaint(h, &ps); began = true; } else dc = reinterpret_cast<HDC>(w);
    RECT rc{};
    ::GetClientRect(h, &rc);
    HBRUSH b = ::CreateSolidBrush(g_state.fill);
    ::FillRect(dc, &rc, b);
    ::DeleteObject(b);
    if (began) ::EndPaint(h, &ps);
    return (m == WM_PAINT) ? 0 : 1;
  }
  if (m == WM_NCCALCSIZE && g_state.nccalc && w != 0) return 0;
  if (m == WM_GETMINMAXINFO && g_state.lock_size) {
    auto* info = reinterpret_cast<MINMAXINFO*>(l);
    RECT rc{0, 0, kW, kH};
    if (info != nullptr) {
      info->ptMinTrackSize.x = kW;
      info->ptMinTrackSize.y = kH;
      info->ptMaxTrackSize.x = kW;
      info->ptMaxTrackSize.y = kH;
    }
    return 0;
  }
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

void test(const char* name, DWORD style, bool lock_size, int idx) {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &proc;
  wc.hInstance = ::GetModuleHandleW(nullptr);
  wc.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  wc.lpszClassName = (L"v8_" + std::to_wstring(idx)).c_str();
  ::RegisterClassExW(&wc);
  g_state.lock_size = lock_size;
  const int ox = 220, oy = 190;
  const int tw = kW + 2 * kPad, th = kH + 2 * kPad;

  g_state.nccalc = false;
  g_state.fill = kBack;
  wc.lpszClassName = L"v8_back";
  ::RegisterClassExW(&wc);
  HWND back = ::CreateWindowExW(0, L"v8_back", L"b", WS_POPUP | WS_VISIBLE, ox, oy, tw, th, nullptr,
                                nullptr, ::GetModuleHandleW(nullptr), nullptr);
  g_state.nccalc = true;
  g_state.fill = kFill;
  wc.lpszClassName = (L"v8_" + std::to_wstring(idx)).c_str();
  ::RegisterClassExW(&wc);
  HWND win = ::CreateWindowExW(0, (L"v8_" + std::to_wstring(idx)).c_str(), L"p",
                               style | WS_VISIBLE, ox + kPad, oy + kPad, kW, kH, nullptr, nullptr,
                               ::GetModuleHandleW(nullptr), nullptr);
  if (!back || !win) { std::printf("%-42s CREATE_FAILED\n", name); return; }
  MARGINS m{1, 1, 1, 1};
  ::DwmExtendFrameIntoClientArea(win, &m);
  const int pref = 1;
  ::DwmSetWindowAttribute(win, kDwmwaCorner, &pref, sizeof(pref));
  const COLORREF none = kColorNone;
  ::DwmSetWindowAttribute(win, kDwmwaBorder, &none, sizeof(none));

  ::SetWindowPos(back, HWND_TOPMOST, ox, oy, tw, th, SWP_NOACTIVATE | SWP_SHOWWINDOW);
  pump(150);
  ::SetWindowPos(win, HWND_TOPMOST, ox + kPad, oy + kPad, kW, kH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
  ::UpdateWindow(win);
  ::BringWindowToTop(win);
  pump(700);

  RECT wr{}, cr{};
  ::GetWindowRect(win, &wr);
  ::GetClientRect(win, &cr);
  Shot s;
  bool ok = grab(ox, oy, tw, th, s);
  double peak = 0.0;
  int peak_d = 0;
  std::string prof;
  double base = 0.0;
  if (ok) {
    const unsigned char* center = s.at(wr.left + (wr.right - wr.left) / 2 - ox,
                                       wr.top + (wr.bottom - wr.top) / 2 - oy);
    auto col = [&](int d) {
      double sum = 0.0; int n = 0;
      for (int y = wr.top + 6; y < wr.bottom - 6; ++y) {
        const unsigned char* p = s.at(wr.right - 1 + d - ox, y - oy);
        if (p) { sum += lum(p); ++n; }
      }
      return n ? sum / n : 0.0;
    };
    base = col(kBand);
    char c[16];
    for (int d = 1; d <= kBand; ++d) {
      const double delta = base - col(d);
      if (delta > peak) { peak = delta; peak_d = d; }
      if (d <= 8) { std::snprintf(c, sizeof(c), " %5.1f", delta); prof += c; }
    }
    if (center == nullptr) ok = false;
  }
  // 边缘命中：给窗口发一个"屏幕边缘点"，看它自己回的码（不能真拖，只看返回）
  const LPARAM lp = MAKELPARAM(wr.left + 1, wr.top + kH / 2);
  const LRESULT hit = ::SendMessageW(win, WM_NCHITTEST, 0, lp);
  std::printf("%-42s 阴影峰=%5.1f@%dpx client=%ldx%ld 边缘命中=%lld(%s) 剖面:%s\n", name, peak,
              peak_d, cr.right, cr.bottom, static_cast<long long>(hit),
              hit == HTCLIENT ? "HTCLIENT(不可缩放)" : "可拖边", prof.c_str());
  ::DestroyWindow(win);
  ::DestroyWindow(back);
  pump(100);
}

}  // namespace

int main() {
  if (auto f = reinterpret_cast<BOOL(WINAPI*)(intptr_t)>(
          ::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))) {
    f(-4);
  }
  int i = 0;
  const DWORD popup = WS_POPUP | WS_CLIPCHILDREN;
  test("① popup 仅", popup | WS_MINIMIZEBOX, false, i++);
  test("② popup+BORDER", popup | WS_MINIMIZEBOX | WS_BORDER, false, i++);
  test("③ popup+DLGFRAME", popup | WS_MINIMIZEBOX | WS_DLGFRAME, false, i++);
  test("④ popup+CAPTION", popup | WS_MINIMIZEBOX | WS_CAPTION, false, i++);
  test("⑤ popup+THICKFRAME(锁死尺寸)", popup | WS_MINIMIZEBOX | WS_THICKFRAME, true, i++);
  test("⑥ popup+CAPTION+THICKFRAME(锁死)", popup | WS_MINIMIZEBOX | WS_CAPTION | WS_THICKFRAME, true, i++);
  return 0;
}

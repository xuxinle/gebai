// shadow_live_check2.cpp —— 成品验收：把**真实的霜天窗口**移到干净背景上，量窗外阴影。
//
// 与 v1 的差别：v1 直接在窗口当前所在处截图，而窗口常贴在屏幕角上（左侧/上侧没有窗外空间），
// 且身后可能是歌白自己的深色界面（背景纹理掩盖阴影）。本版两步：
//   1. 先铺一块纯灰背板窗口（填 RGB(120,122,126)，无阴影）；
//   2. 用 SetWindowPos 把目标窗口挪到背板上，再采样窗外。
// 这样"窗外应该是什么颜色"是已知常数，阴影量 = 背板本色 − 实测，读数干净。
//
// 判据（全部要过）：
//   A. 窗口真的上屏了（中心像素 == WindowFromPoint 命中目标）；
//   B. 窗外右/下侧 1..14px 有阴影剖面：近处显著变暗且随距离回升；
//   C. 窗内 1..2px 没有多余的"系统描边"（与中心像素的差 ≤ 4）。
//
// 编译：g++ -std=c++17 -O2 shadow_live_check2.cpp -o shadow_live_check2.exe -lgdi32 -luser32
// 用法：shadow_live_check2.exe [类名]   默认 ShuangtianWindow

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr COLORREF kBackdrop = RGB(120, 122, 126);
constexpr int kPad = 34;

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
  if (screen == nullptr) return false;
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
  if (dib != nullptr) ::DeleteObject(dib);
  ::DeleteDC(mem);
  ::ReleaseDC(nullptr, screen);
  return ok;
}

double lum(const unsigned char* p) { return 0.114 * p[0] + 0.587 * p[1] + 0.299 * p[2]; }

void pump(DWORD ms) {
  const ULONGLONG end = ::GetTickCount64() + ms;
  while (::GetTickCount64() < end) {
    MSG m{};
    while (::PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE) != 0) {
      ::TranslateMessage(&m);
      ::DispatchMessageW(&m);
    }
    ::Sleep(4);
  }
}

COLORREF g_fill = kBackdrop;

LRESULT CALLBACK backdrop_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_ERASEBKGND || msg == WM_PAINT) {
    HDC dc;
    PAINTSTRUCT ps{};
    bool began = false;
    if (msg == WM_PAINT) { dc = ::BeginPaint(hwnd, &ps); began = true; }
    else dc = reinterpret_cast<HDC>(wp);
    RECT rc{};
    ::GetClientRect(hwnd, &rc);
    HBRUSH b = ::CreateSolidBrush(g_fill);
    ::FillRect(dc, &rc, b);
    ::DeleteObject(b);
    if (began) ::EndPaint(hwnd, &ps);
    return (msg == WM_PAINT) ? 0 : 1;
  }
  return ::DefWindowProcW(hwnd, msg, wp, lp);
}

struct Target {
  HWND hwnd = nullptr;
  std::wstring title;
  std::wstring cls;
};

BOOL CALLBACK enum_proc(HWND hwnd, LPARAM param) {
  auto* out = reinterpret_cast<std::vector<Target>*>(param);
  if (::IsWindowVisible(hwnd) == 0) return TRUE;
  if (::GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
  wchar_t cls[256] = {};
  ::GetClassNameW(hwnd, cls, 256);
  wchar_t title[512] = {};
  ::GetWindowTextW(hwnd, title, 512);
  RECT rc{};
  if (::GetWindowRect(hwnd, &rc) == 0) return TRUE;
  out->push_back(Target{hwnd, title, cls});
  return TRUE;
}

}  // namespace

int main(int argc, char** argv) {
  if (auto f = reinterpret_cast<BOOL(WINAPI*)(intptr_t)>(
          ::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))) {
    f(-4);
  }
  const char* wanted = "ShuangtianWindow";

  std::vector<Target> all;
  ::EnumWindows(&enum_proc, reinterpret_cast<LPARAM>(&all));
  HWND target = nullptr;
  // 参数支持两种写法：类名（默认 `ShuangtianWindow`）/ 标题子串。
  // `--smallest`：在候选中取**窗口面积最小**的那个——同时开着 gbcode（铺满屏）与
  // 相期（小卡片）时，需要测的是小窗口（它才有“窗外”可量）。
  bool smallest = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--smallest") == 0) smallest = true;
    else wanted = argv[i];
  }
  LONG best_area = 0;
  for (const auto& t : all) {
    char cls_u8[256] = {};
    char title_u8[512] = {};
    ::WideCharToMultiByte(CP_UTF8, 0, t.cls.c_str(), -1, cls_u8, 256, nullptr, nullptr);
    ::WideCharToMultiByte(CP_UTF8, 0, t.title.c_str(), -1, title_u8, 512, nullptr, nullptr);
    if (std::strcmp(cls_u8, wanted) != 0 && std::strstr(title_u8, wanted) == nullptr) continue;
    RECT rc{};
    ::GetWindowRect(t.hwnd, &rc);
    const LONG area = (rc.right - rc.left) * (rc.bottom - rc.top);
    std::printf("候选: %p class=%s title=%s %ldx%ld\n", t.hwnd, cls_u8, title_u8, rc.right - rc.left,
                rc.bottom - rc.top);
    if (!smallest || target == nullptr || area < best_area) {
      target = t.hwnd;
      best_area = area;
    }
  }
  if (target == nullptr) {
    std::printf("没有找到匹配 \"%s\" 的窗口\n", wanted);
    return 2;
  }

  // 目标原尺寸（只挪位置，不改尺寸——改尺寸会触发它自己的重建逻辑）
  RECT before{};
  ::GetWindowRect(target, &before);
  const int ww = before.right - before.left;
  const int wh = before.bottom - before.top;
  const LONG_PTR style = ::GetWindowLongPtrW(target, GWL_STYLE);
  std::printf("目标 %p 原始 rect=(%ld,%ld,%ldx%ld) style=0x%08lx THICKFRAME=%d\n", target,
              before.left, before.top, ww, wh, static_cast<unsigned long>(style),
              (style & WS_THICKFRAME) ? 1 : 0);

  // 屏幕工作区中央放背板
  RECT work{};
  ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  const int bx = std::max<int>(work.left, (work.right - work.left - (ww + 2 * kPad)) / 2);
  const int by = std::max<int>(work.top, (work.bottom - work.top - (wh + 2 * kPad)) / 2);

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &backdrop_proc;
  wc.hInstance = ::GetModuleHandleW(nullptr);
  wc.lpszClassName = L"shadow_live_backdrop";
  ::RegisterClassExW(&wc);
  HWND back = ::CreateWindowExW(0, L"shadow_live_backdrop", L"backdrop", WS_POPUP | WS_VISIBLE, bx,
                                by, ww + 2 * kPad, wh + 2 * kPad, nullptr, nullptr,
                                ::GetModuleHandleW(nullptr), nullptr);
  if (back == nullptr) {
    std::printf("背板窗口创建失败 %lu\n", ::GetLastError());
    return 3;
  }
  ::SetWindowPos(back, HWND_TOPMOST, bx, by, ww + 2 * kPad, wh + 2 * kPad,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
  pump(250);
  // 把目标挪到背板中央并置顶（不激活，避免抢焦点改变它自己的状态）
  ::SetWindowPos(target, HWND_TOPMOST, bx + kPad, by + kPad, ww, wh,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
  pump(900);

  RECT wr{}, cr{};
  ::GetWindowRect(target, &wr);
  ::GetClientRect(target, &cr);
  std::printf("挪后 rect=(%ld,%ld,%ldx%ld) client=%ldx%ld\n", wr.left, wr.top, wr.right - wr.left,
              wr.bottom - wr.top, cr.right, cr.bottom);

  Shot s;
  if (!grab(bx, by, ww + 2 * kPad, wh + 2 * kPad, s)) {
    std::printf("抓屏失败\n");
    return 3;
  }
  const unsigned char* center =
      s.at(wr.left + (wr.right - wr.left) / 2 - bx, wr.top + (wr.bottom - wr.top) / 2 - by);
  HWND under = ::WindowFromPoint(
      POINT{wr.left + (wr.right - wr.left) / 2, wr.top + (wr.bottom - wr.top) / 2});
  const bool onscreen = under == target && center != nullptr;
  std::printf("上屏校验: WindowFromPoint==target %d, 中心 BGR=(%d,%d,%d)\n", under == target ? 1 : 0,
              center ? center[0] : -1, center ? center[1] : -1, center ? center[2] : -1);

  auto col = [&](int d) {
    double sum = 0.0;
    int n = 0;
    for (int y = wr.top + 12; y < wr.bottom - 12; ++y) {
      const unsigned char* p = s.at(wr.right - 1 + d - bx, y - by);
      if (p != nullptr) { sum += lum(p); ++n; }
    }
    return n > 0 ? sum / n : 0.0;
  };
  auto row = [&](int d) {
    double sum = 0.0;
    int n = 0;
    for (int x = wr.left + 12; x < wr.right - 12; ++x) {
      const unsigned char* p = s.at(x - bx, wr.bottom - 1 + d - by);
      if (p != nullptr) { sum += lum(p); ++n; }
    }
    return n > 0 ? sum / n : 0.0;
  };
  // 基线：背板本色（最外一圈）
  const double base_r = col(kPad - 3);
  const double base_b = row(kPad - 3);
  std::printf("\n基线（背板）: right=%.1f bottom=%.1f  （背板本色亮度应≈128）\n", base_r, base_b);
  std::printf("距离:    ");
  for (int d = 1; d <= 14; ++d) std::printf("%6d", d);
  std::printf("\n右侧阴影:");
  double peak_r = 0.0;
  for (int d = 1; d <= 14; ++d) {
    const double delta = base_r - col(d);
    std::printf("%6.1f", delta);
    peak_r = std::max(peak_r, delta);
  }
  std::printf("\n下侧阴影:");
  double peak_b = 0.0;
  for (int d = 1; d <= 14; ++d) {
    const double delta = base_b - row(d);
    std::printf("%6.1f", delta);
    peak_b = std::max(peak_b, delta);
  }
  // 单调性：从峰值处往外应逐级回升（阴影的指纹）
  auto monotonic_after_peak = [&]() {
    const int prof_len = 14;
    double prof[14];
    for (int d = 1; d <= prof_len; ++d) prof[d - 1] = base_r - col(d);
    int at = 0;
    for (int i = 1; i < prof_len; ++i) if (prof[i] > prof[at]) at = i;
    // 峰值之后允许小幅抖动（±2），整体应下降
    return at < prof_len - 3 && prof[prof_len - 1] < prof[at] - 8.0;
  };

  // 窗内边缘（判断有没有系统描边）
  double edge = 0.0;
  int en = 0;
  for (int d = 1; d <= 2; ++d) {
    for (int y = wr.top + 12; y < wr.bottom - 12; ++y) {
      const unsigned char* p = s.at(wr.left + d - bx, y - by);
      if (p != nullptr) { edge += lum(p); ++en; }
    }
    for (int x = wr.left + 12; x < wr.right - 12; ++x) {
      const unsigned char* p = s.at(x - bx, wr.top + d - by);
      if (p != nullptr) { edge += lum(p); ++en; }
    }
  }
  const double inside_delta = (en > 0 && center != nullptr) ? lum(center) - edge / en : 0.0;

  std::printf("\n右侧峰值=%.1f  下侧峰值=%.1f  -> 阴影%s\n", peak_r, peak_b,
              (peak_r > 5.0 || peak_b > 5.0) ? "有 ✓" : "无 ✗");
  std::printf("窗内 1..2px 与中心差=%.1f（|Δ|>8 说明有系统描边）\n", inside_delta);
  std::printf("右下角剖面单调回升=%d\n", monotonic_after_peak() ? 1 : 0);

  const bool pass = onscreen && (peak_r > 5.0 || peak_b > 5.0) && std::fabs(inside_delta) < 8.0;
  std::printf("\n结论: %s\n", pass ? "通过 ✓" : "不通过 ✗");

  // 留证
  if (FILE* f = std::fopen("shadow_live_shot.bmp", "wb")) {
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = s.w;
    ih.biHeight = -s.h;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + static_cast<DWORD>(s.w) * s.h * 4;
    std::fwrite(&fh, sizeof(fh), 1, f);
    std::fwrite(&ih, sizeof(ih), 1, f);
    std::fwrite(s.bgra.data(), 1, s.bgra.size(), f);
    std::fclose(f);
    std::printf("留证：shadow_live_shot.bmp\n");
  }
  // 把窗口挪回原处（不改它的尺寸）
  ::SetWindowPos(target, HWND_NOTOPMOST, before.left, before.top, ww, wh,
                 SWP_NOACTIVATE | SWP_NOZORDER);
  ::DestroyWindow(back);
  ::UnregisterClassW(L"shadow_live_backdrop", ::GetModuleHandleW(nullptr));
  return pass ? 0 : 1;
}

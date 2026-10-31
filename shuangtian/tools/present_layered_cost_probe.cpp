// present_cost_probe.cpp —— 量 Win32 后端两条上屏通道的每帧成本。
//
// 为什么需要它：讨论"窗口圆角"要不要做时，所有方案都要在一次全窗口合成里
// 带 alpha（四角半透明）。而霜天现有上屏是：
//   软件腿：逐像素重排通道 → DIB → BitBlt
//   GPU 腿：D3D11 swapchain Present
// 圆角一上，软件腿的 BitBlt 必须换 `UpdateLayeredWindow`（窗口要 `WS_EX_LAYERED`），
// GPU 腿要换 DirectComposition 或回读。**这次量的是现状的底数**，
// 用来判断"新通道多出来的成本"到底占一帧多大比例。
//
// 三个量：
//   A. 逐像素重排（RRGGBBAA → 内存序 B,G,R,A）—— 现有软件腿的纯 CPU 段
//   B. DIB BitBlt 上屏（现通道）
//   C. UpdateLayeredWindow 整窗上屏（候选新通道，需 WS_EX_LAYERED）
//
// 编译：g++ -std=c++17 -O2 present_cost_probe.cpp -o present_cost_probe.exe -lgdi32 -luser32

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

struct Stats {
  double min_ms = 1e9;
  double med_ms = 0.0;
  double max_ms = 0.0;
};

Stats summarize(std::vector<double> samples) {
  Stats s;
  if (samples.empty()) return s;
  std::sort(samples.begin(), samples.end());
  s.min_ms = samples.front();
  s.med_ms = samples[samples.size() / 2];
  s.max_ms = samples.back();
  return s;
}

void pump(DWORD ms) {
  const ULONGLONG end = ::GetTickCount64() + ms;
  while (::GetTickCount64() < end) {
    MSG m{};
    while (::PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE) != 0) {
      ::TranslateMessage(&m);
      ::DispatchMessageW(&m);
    }
    ::Sleep(2);
  }
}

LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_ERASEBKGND) return 1;
  if (msg == WM_PAINT) {
    PAINTSTRUCT ps{};
    ::BeginPaint(hwnd, &ps);
    ::EndPaint(hwnd, &ps);
    return 0;
  }
  return ::DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int main(int argc, char** argv) {
  if (auto f = reinterpret_cast<BOOL(WINAPI*)(intptr_t)>(
          ::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))) {
    f(-4);
  }
  // 尺寸默认取本机实际窗口大小（DPI 缩放下是物理像素，就是要这个量级）
  int width = 3533;
  int height = 1921;
  if (argc > 2) {
    width = std::atoi(argv[1]);
    height = std::atoi(argv[2]);
  }
  const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  std::printf("画布 %dx%d（%.1f Mpx）\n\n", width, height, count / 1e6);

  // 源：模拟画布内部格式 0xRRGGBBAA（预乘），填一个渐变，避免全同值被优化掉
  std::vector<std::uint32_t> source(count);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const std::uint32_t r = static_cast<std::uint32_t>(x * 255 / width);
      const std::uint32_t g = static_cast<std::uint32_t>(y * 255 / height);
      source[static_cast<std::size_t>(y) * width + x] = (r << 24U) | (g << 16U) | (0x80U << 8U) | 0xFFU;
    }
  }

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &proc;
  wc.hInstance = ::GetModuleHandleW(nullptr);
  wc.lpszClassName = L"present_cost_probe";
  wc.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  ::RegisterClassExW(&wc);

  // 两个窗口：一个普通（BitBlt），一个 WS_EX_LAYERED（UpdateLayeredWindow）
  HWND plain = ::CreateWindowExW(0, L"present_cost_probe", L"plain", WS_POPUP | WS_VISIBLE, 40, 40,
                                 width / 4, height / 4, nullptr, nullptr,
                                 ::GetModuleHandleW(nullptr), nullptr);
  HWND layered = ::CreateWindowExW(WS_EX_LAYERED, L"present_cost_probe", L"layered", WS_POPUP, 80,
                                   80, width / 4, height / 4, nullptr, nullptr,
                                   ::GetModuleHandleW(nullptr), nullptr);
  if (plain == nullptr || layered == nullptr) {
    std::printf("窗口创建失败 %lu\n", ::GetLastError());
    return 1;
  }

  HDC screen = ::GetDC(nullptr);
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = width;
  bi.bmiHeader.biHeight = -height;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP dib = ::CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (dib == nullptr || bits == nullptr) {
    std::printf("CreateDIBSection 失败\n");
    return 1;
  }
  auto* target = static_cast<std::uint32_t*>(bits);
  HDC mem = ::CreateCompatibleDC(screen);
  HGDIOBJ old = ::SelectObject(mem, dib);

  constexpr int kIterations = 40;

  // A. 逐像素通道重排（现有软件腿的 CPU 段）
  std::vector<double> reorder;
  for (int i = 0; i < kIterations; ++i) {
    const auto t0 = Clock::now();
    for (std::size_t index = 0; index < count; ++index) {
      const std::uint32_t pixel = source[index];
      const std::uint32_t r = (pixel >> 24U) & 0xFFU;
      const std::uint32_t g = (pixel >> 16U) & 0xFFU;
      const std::uint32_t b = (pixel >> 8U) & 0xFFU;
      const std::uint32_t a = pixel & 0xFFU;
      target[index] = (a << 24U) | (r << 16U) | (g << 8U) | b;
    }
    reorder.push_back(ms_since(t0));
  }
  const Stats sA = summarize(reorder);

  // B. BitBlt 上屏（现通道）
  std::vector<double> blit;
  HDC window_dc = ::GetDC(plain);
  for (int i = 0; i < kIterations; ++i) {
    const auto t0 = Clock::now();
    ::BitBlt(window_dc, 0, 0, width / 4, height / 4, mem, 0, 0, SRCCOPY);
    blit.push_back(ms_since(t0));
  }
  ::ReleaseDC(plain, window_dc);
  const Stats sB = summarize(blit);

  // C. UpdateLayeredWindow 整窗上屏（候选新通道）
  std::vector<double> ulw;
  POINT src{0, 0};
  SIZE size{width, height};
  POINT dst{100, 100};
  BLENDFUNCTION blend{};
  blend.BlendOp = AC_SRC_OVER;
  blend.SourceConstantAlpha = 255;
  blend.AlphaFormat = AC_SRC_ALPHA;
  for (int i = 0; i < kIterations; ++i) {
    const auto t0 = Clock::now();
    ::UpdateLayeredWindow(layered, screen, &dst, &size, mem, &src, 0, &blend, ULW_ALPHA);
    ulw.push_back(ms_since(t0));
  }
  const Stats sC = summarize(ulw);

  std::printf("%-34s min %7.3f  median %7.3f  max %7.3f  ms\n", "A 逐像素重排（现有 CPU 段）",
              sA.min_ms, sA.med_ms, sA.max_ms);
  std::printf("%-34s min %7.3f  median %7.3f  max %7.3f  ms\n", "B BitBlt 上屏（现通道）",
              sB.min_ms, sB.med_ms, sB.max_ms);
  std::printf("%-34s min %7.3f  median %7.3f  max %7.3f  ms\n", "C UpdateLayeredWindow（候选）",
              sC.min_ms, sC.med_ms, sC.max_ms);
  std::printf("\n相对现通道（A+B）的倍率：\n");
  const double current = sA.med_ms + sB.med_ms;
  std::printf("  现通道 A+B      = %.3f ms\n", current);
  std::printf("  换 C 后 A+C     = %.3f ms（%.2fx）\n", sA.med_ms + sC.med_ms,
              (sA.med_ms + sC.med_ms) / current);
  std::printf("  若 A 也用 SIMD/原地换到 C 之前 = 未知，本次只量区间\n");

  ::SelectObject(mem, old);
  ::DeleteObject(dib);
  ::DeleteDC(mem);
  ::ReleaseDC(nullptr, screen);
  ::DestroyWindow(layered);
  ::DestroyWindow(plain);
  ::UnregisterClassW(L"present_cost_probe", ::GetModuleHandleW(nullptr));
  return 0;
}

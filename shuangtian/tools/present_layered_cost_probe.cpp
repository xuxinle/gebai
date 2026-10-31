// present_layered_cost_probe.cpp —— 上屏通道成本对照（**同面积、同口径**）。
//
// 用途：评估"给窗口做圆角"要付多少上屏代价。圆角要求四角真透明，而霜天现有
// 上屏通道都不带 alpha：
//   软件腿：逐像素重排 → DIB → BitBlt
//   GPU 腿：D3D11 swapchain（DXGI_SWAP_EFFECT_DISCARD，不含成）
// 圆角一上，软件腿必须换 `UpdateLayeredWindow`（窗口加 `WS_EX_LAYERED`）。
//
// ⚠ **第一版量具是错的**（2026-11-01 实测发现）：它用 `BitBlt` 只搬 1/16 面积
// （`width/4 × height/4`）却与 `UpdateLayeredWindow` 的**全量**对比，于是得出
// "2 倍"这个被面积比污染的数字。本版关键约束：
//   ① 两条通道搬**同一块全量像素**；
//   ② 各跑相同轮次，报 min/median/max；
//   ③ 另给一组"仅 API 开销"（1×1 像素）作为基线，把固定开销与逐像素成本分开——
//      否则无法回答"多出来的到底是什么"。
//
// 编译：g++ -std=c++17 -O2 present_layered_cost_probe.cpp -o present_layered_cost_probe.exe -lgdi32 -luser32

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

struct Stats {
  double min_ms = 0.0;
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
  int width = 3533;
  int height = 1921;
  if (argc > 2) {
    width = std::atoi(argv[1]);
    height = std::atoi(argv[2]);
  }
  const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  std::printf("画布 %dx%d（%.1f Mpx）· 所有通道都搬同一块全量像素\n\n", width, height, count / 1e6);

  // 源：模拟画布内部格式 0xRRGGBBAA（预乘），渐变避免全同值被优化
  std::vector<std::uint32_t> source(count);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const std::uint32_t r = static_cast<std::uint32_t>(x * 255 / width);
      const std::uint32_t g = static_cast<std::uint32_t>(y * 255 / height);
      source[static_cast<std::size_t>(y) * width + x] =
          (r << 24U) | (g << 16U) | (0x80U << 8U) | 0xFFU;
    }
  }

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &proc;
  wc.hInstance = ::GetModuleHandleW(nullptr);
  wc.lpszClassName = L"present_cost_probe";
  wc.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  ::RegisterClassExW(&wc);

  // 两个窗口，**尺寸相同**（避免"窗口比位图小 ⇒ GDI 少搬"这类隐形差异）。
  // 屏幕只有 2560x1440，3533x1921 的窗口放不下；两窗口都用屏幕内尺寸，
  // 但**搬运的像素量由 UpdateLayeredWindow/BitBlt 的参数决定**，与窗口大小无关。
  const int win_w = std::min(width, 1200);
  const int win_h = std::min(height, 700);
  HWND plain = ::CreateWindowExW(0, L"present_cost_probe", L"plain", WS_POPUP | WS_VISIBLE, 20, 20,
                                 win_w, win_h, nullptr, nullptr, ::GetModuleHandleW(nullptr),
                                 nullptr);
  HWND layered = ::CreateWindowExW(WS_EX_LAYERED, L"present_cost_probe", L"layered", WS_POPUP, 40,
                                   40, win_w, win_h, nullptr, nullptr,
                                   ::GetModuleHandleW(nullptr), nullptr);
  if (plain == nullptr || layered == nullptr) {
    std::printf("窗口创建失败 %lu\n", ::GetLastError());
    return 1;
  }
  pump(300);

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

  constexpr int kIterations = 60;

  // ── A. 逐像素通道重排（软件腿的 CPU 段）──
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

  // ── A0. 对照：**不重排**、纯全量拷贝进 DIB ──
  // 意义：现有 A 是「重排 + 写」，本项是「只写」。两者之差就回答了
  // “能不能把重排与上屏合成一趟”这个问题——即画布若原生就是 BGRA 能省多少。
  std::vector<double> copy_only;
  for (int i = 0; i < kIterations; ++i) {
    const auto t0 = Clock::now();
    std::memcpy(target, source.data(), count * sizeof(std::uint32_t));
    copy_only.push_back(ms_since(t0));
  }
  const Stats sA0 = summarize(copy_only);

  // ── B_mem. 纯 GDI 内存→内存 全量拷贝（不经窗口）──
  // 意义：B 量的是「贴到窗口」，会被窗口可见区裁切，且可异步；本项量的是
  // GDI 真正的拷贝吞吐。两者差多少，就说明“B 的便宜”有多少是假的。
  BITMAPINFO bi2 = bi;
  void* bits2 = nullptr;
  HBITMAP dib2 = ::CreateDIBSection(screen, &bi2, DIB_RGB_COLORS, &bits2, nullptr, 0);
  std::vector<double> mem_blit;
  if (dib2 != nullptr && bits2 != nullptr) {
    HDC mem2 = ::CreateCompatibleDC(screen);
    HGDIOBJ old2 = ::SelectObject(mem2, dib2);
    for (int i = 0; i < kIterations; ++i) {
      const auto t0 = Clock::now();
      ::BitBlt(mem2, 0, 0, width, height, mem, 0, 0, SRCCOPY);
      mem_blit.push_back(ms_since(t0));
    }
    ::SelectObject(mem2, old2);
    ::DeleteDC(mem2);
  }
  const Stats sBmem = summarize(mem_blit);

  // ── B 全量上屏（现通道）──
  std::vector<double> blit;
  HDC window_dc = ::GetDC(plain);
  for (int i = 0; i < kIterations; ++i) {
    const auto t0 = Clock::now();
    ::BitBlt(window_dc, 0, 0, width, height, mem, 0, 0, SRCCOPY);
    blit.push_back(ms_since(t0));
  }
  ::ReleaseDC(plain, window_dc);
  const Stats sB = summarize(blit);

  // ── C. UpdateLayeredWindow **全量**上屏（候选新通道）──
  // 另给一组 1x1 作为"纯 API 固定开销"，用于把固定开销与逐像素成本分开。
  std::vector<double> ulw;
  std::vector<double> ulw_tiny;
  POINT src{0, 0};
  POINT dst{100, 100};
  BLENDFUNCTION blend{};
  blend.BlendOp = AC_SRC_OVER;
  blend.SourceConstantAlpha = 255;
  blend.AlphaFormat = AC_SRC_ALPHA;
  SIZE full{width, height};
  SIZE tiny{1, 1};
  for (int i = 0; i < kIterations; ++i) {
    const auto t0 = Clock::now();
    ::UpdateLayeredWindow(layered, screen, &dst, &full, mem, &src, 0, &blend, ULW_ALPHA);
    ulw.push_back(ms_since(t0));
  }
  for (int i = 0; i < kIterations; ++i) {
    const auto t0 = Clock::now();
    ::UpdateLayeredWindow(layered, screen, &dst, &tiny, mem, &src, 0, &blend, ULW_ALPHA);
    ulw_tiny.push_back(ms_since(t0));
  }
  const Stats sC = summarize(ulw);
  const Stats sC0 = summarize(ulw_tiny);

  auto line = [](const char* name, const Stats& s) {
    std::printf("  %-38s min %7.3f  median %7.3f  max %7.3f\n", name, s.min_ms, s.med_ms, s.max_ms);
  };
  std::printf("单项耗时（ms）：\n");
  line("A  逐像素通道重排 + 写（CPU 段）", sA);
  line("A0 仅全量 memcpy（不重排）", sA0);
  line("B0 BitBlt 内存→内存 全量（GDI 真拷贝）", sBmem);
  line("B  BitBlt 全量上屏（到窗口）", sB);
  line("C  UpdateLayeredWindow 全量上屏", sC);
  line("C0 UpdateLayeredWindow 1x1（固定开销）", sC0);

  const double pixels = static_cast<double>(count);
  std::printf("\n逐像素部分（扣除 1x1 固定开销后）：\n");
  const double c_px = sC.med_ms - sC0.med_ms;
  std::printf("  C 逐像素 = %.3f − %.3f = %.3f ms  → %.2f ns/px\n", sC.med_ms, sC0.med_ms, c_px,
              c_px * 1e6 / pixels);
  std::printf("  A 逐像素 = %.3f ms                    → %.2f ns/px\n", sA.med_ms,
              sA.med_ms * 1e6 / pixels);
  std::printf("  B 逐像素 = %.3f ms                    → %.2f ns/px\n", sB.med_ms,
              sB.med_ms * 1e6 / pixels);

  std::printf("\n对照（同为全量）：\n");
  std::printf("  现通道 A+B          = %.3f ms\n", sA.med_ms + sB.med_ms);
  std::printf("  换分层后 A+C        = %.3f ms（%.2fx）\n", sA.med_ms + sC.med_ms,
              (sA.med_ms + sC.med_ms) / (sA.med_ms + sB.med_ms));
  std::printf("  多出                 = %.3f ms（其中固定开销 %.3f ms，逐像素 %.3f ms）\n",
              sC.med_ms - sB.med_ms, sC0.med_ms, c_px);

  ::SelectObject(mem, old);
  ::DeleteObject(dib);
  ::DeleteDC(mem);
  ::ReleaseDC(nullptr, screen);
  ::DestroyWindow(layered);
  ::DestroyWindow(plain);
  ::UnregisterClassW(L"present_cost_probe", ::GetModuleHandleW(nullptr));
  return 0;
}

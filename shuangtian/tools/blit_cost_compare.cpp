// blit_cost_compare.cpp —— 真实 `blit()` 路径的端到端对照（旧写法 vs 新写法）。
//
// 为什么单独做这一个：`channel_swap_probe.cpp` 只量了「通道重排」这一段的
// 微基准（新写法比旧写法快 51%），但真实 `blit()` 里这段之后紧跟 `BitBlt`，
// 端到端收益会被它稀释。本探针把 `blit()` 的两段原样复刻（同一份 DIB、同一块
// 目标窗口），分别量「旧写法 + BitBlt」与「新写法 + BitBlt」，给出真实每帧差。
//
// 编译：g++ -std=c++17 -O2 blit_cost_compare.cpp -o blit_cost_compare.exe -lgdi32 -luser32

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
};

Stats summarize(std::vector<double> samples) {
  Stats s;
  if (samples.empty()) return s;
  std::sort(samples.begin(), samples.end());
  s.min_ms = samples.front();
  s.med_ms = samples[samples.size() / 2];
  return s;
}

// 旧写法：逐通道拆掩码再拼（改动前 `platform_win32.cpp::blit()` 的原样）
void swap_old(const std::uint32_t* src, std::uint32_t* dst, std::size_t count) {
  for (std::size_t index = 0; index < count; ++index) {
    const std::uint32_t pixel = src[index];
    const std::uint32_t red = (pixel >> 24U) & 0xFFU;
    const std::uint32_t green = (pixel >> 16U) & 0xFFU;
    const std::uint32_t blue = (pixel >> 8U) & 0xFFU;
    const std::uint32_t alpha = pixel & 0xFFU;
    dst[index] = (alpha << 24U) | (red << 16U) | (green << 8U) | blue;
  }
}

// 新写法：与改动后 `platform_win32.cpp::blit()` 逐字相同
void swap_new(const std::uint32_t* src, std::uint32_t* dst, std::size_t count) {
  constexpr std::uint64_t kLo = 0x00000000FFFFFFFFULL;
  constexpr std::uint64_t kHi = 0xFFFFFFFF00000000ULL;
  const auto rotate = [](std::uint64_t two) {
    const std::uint64_t lo = ((two & kLo) >> 8U) | ((two & kLo) << 24U);
    const std::uint64_t hi = ((two & kHi) >> 8U) | ((two & kHi) << 24U);
    return (lo & kLo) | (hi & kHi);
  };
  std::size_t index = 0;
  constexpr std::size_t kUnroll = 8;
  for (; index + kUnroll <= count; index += kUnroll) {
    std::uint64_t a = 0;
    std::uint64_t b = 0;
    std::uint64_t c = 0;
    std::uint64_t d = 0;
    std::memcpy(&a, src + index, sizeof(a));
    std::memcpy(&b, src + index + 2, sizeof(b));
    std::memcpy(&c, src + index + 4, sizeof(c));
    std::memcpy(&d, src + index + 6, sizeof(d));
    a = rotate(a);
    b = rotate(b);
    c = rotate(c);
    d = rotate(d);
    std::memcpy(dst + index, &a, sizeof(a));
    std::memcpy(dst + index + 2, &b, sizeof(b));
    std::memcpy(dst + index + 4, &c, sizeof(c));
    std::memcpy(dst + index + 6, &d, sizeof(d));
  }
  for (; index < count; ++index) {
    const std::uint32_t pixel = src[index];
    dst[index] = (pixel >> 8U) | (pixel << 24U);
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
  std::printf("画布 %dx%d（%.1f Mpx）· 复刻真实 blit() 的两段\n\n", width, height, count / 1e6);

  HDC screen = ::GetDC(nullptr);
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP dib = ::CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (dib == nullptr || bits == nullptr) {
    std::printf("DIB 创建失败\n");
    return 1;
  }
  auto* target = static_cast<std::uint32_t*>(bits);

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &proc;
  wc.hInstance = ::GetModuleHandleW(nullptr);
  wc.lpszClassName = L"blit_cost_compare";
  ::RegisterClassExW(&wc);
  const int win_w = std::min(width, 1400);
  const int win_h = std::min(height, 800);
  HWND win = ::CreateWindowExW(0, L"blit_cost_compare", L"t", WS_POPUP | WS_VISIBLE, 20, 20, win_w,
                               win_h, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
  if (win == nullptr) return 1;
  MSG msg{};
  for (int i = 0; i < 40; ++i) {
    while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      ::TranslateMessage(&msg);
      ::DispatchMessageW(&msg);
    }
    ::Sleep(5);
  }

  // 源：真实界面那种内容（大块浅色 + 文字边缘），不用伪随机——
  // 伪随机的缓存预取行为与界面差异大，会把"内存带宽"测得偏高。
  std::vector<std::uint32_t> source(count);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const std::uint32_t noise = static_cast<std::uint32_t>((x * 7 + y * 13) & 0x0F);
      const std::uint32_t base = (x % 700 < 4 || y % 40 < 2) ? 0x30U : 0xF6U;  // 文字/分隔线 vs 底色
      const std::uint32_t v = base - noise;
      source[static_cast<std::size_t>(y) * width + x] =
          (v << 24U) | (v << 16U) | (v << 8U) | 0xFFU;
    }
  }

  HDC mem = ::CreateCompatibleDC(screen);
  HGDIOBJ old = ::SelectObject(mem, dib);
  HDC window_dc = ::GetDC(win);
  constexpr int kIters = 50;
  std::vector<double> old_t, new_t, old_swap_only, new_swap_only;

  for (int i = 0; i < kIters; ++i) {
    auto t0 = Clock::now();
    swap_old(source.data(), target, count);
    old_swap_only.push_back(ms_since(t0));

    t0 = Clock::now();
    swap_new(source.data(), target, count);
    new_swap_only.push_back(ms_since(t0));

    t0 = Clock::now();
    swap_old(source.data(), target, count);
    ::BitBlt(window_dc, 0, 0, win_w, win_h, mem, 0, 0, SRCCOPY);
    old_t.push_back(ms_since(t0));

    t0 = Clock::now();
    swap_new(source.data(), target, count);
    ::BitBlt(window_dc, 0, 0, win_w, win_h, mem, 0, 0, SRCCOPY);
    new_t.push_back(ms_since(t0));
  }
  ::ReleaseDC(win, window_dc);
  ::SelectObject(mem, old);
  ::DeleteDC(mem);

  const Stats sOldSwap = summarize(old_swap_only);
  const Stats sNewSwap = summarize(new_swap_only);
  const Stats sOld = summarize(old_t);
  const Stats sNew = summarize(new_t);

  std::printf("[只权重排段]（median，ms）\n");
  std::printf("  旧写法 %6.3f    新写法 %6.3f    省 %.3f（%.0f%%）\n", sOldSwap.med_ms,
              sNewSwap.med_ms, sOldSwap.med_ms - sNewSwap.med_ms,
              (sOldSwap.med_ms - sNewSwap.med_ms) / sOldSwap.med_ms * 100.0);
  std::printf("\n[端到端：重排 + 真 BitBlt]\n");
  std::printf("  旧写法 %6.3f    新写法 %6.3f    省 %.3f（%.0f%%）\n", sOld.med_ms, sNew.med_ms,
              sOld.med_ms - sNew.med_ms, (sOld.med_ms - sNew.med_ms) / sOld.med_ms * 100.0);

  ::DestroyWindow(win);
  ::UnregisterClassW(L"blit_cost_compare", ::GetModuleHandleW(nullptr));
  ::DeleteObject(dib);
  ::ReleaseDC(nullptr, screen);
  return 0;
}

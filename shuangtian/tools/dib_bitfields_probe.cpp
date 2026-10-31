// dib_bitfields_probe.cpp —— 能否让 GDI 按「画布的通道顺序」解读像素，免掉逐像素重排？
//
// 背景（2026-11-01 实测）：软件腿每帧上屏前有一段逐像素通道重排
// （画布 `0xRRGGBBAA` → DIB `BI_RGB` 的 `0xAARRGGBB`），3533×1921 上花 **~3.1 ms**，
// 而同一块数据的纯 `memcpy` 只要 **~1.5 ms**。这一半时间是纯浪费在逐像素拆位上的。
//
// ⚠ **字节序要自己算清楚，别被"看起来对"骗过去**（第一版本探针就在这里错了）：
//   画布 uint32 = `R<<24 | G<<16 | B<<8 | A`，小端落盘后**内存字节**是 A,B,G,R；
//   DIB  BI_RGB 的 32bpp 内存字节是 B,G,R,A（即 uint32 = `A<<24|R<<16|G<<8|B`）。
//   所以"直接复用画布内存"要求 GDI 把
//     byte[3] 当 R、byte[2] 当 G、byte[1] 当 B
//   即掩码 R=0xFF000000 / G=0x00FF0000 / B=0x0000FF00（第一版写反成 R=0x00FF0000，读回全错）。
//
// 本探针回答三件事，缺一不可：
//   1. `BI_BITFIELDS` 能不能建起来；
//   2. **读回的通道对不对**（写入已知色 → GetPixel 取回 → 比对；含不对称色排除巧合）；
//   3. 端到端省多少 —— 必须量 **真 BitBlt 到窗口**，不能只量 memcpy：
//      `BI_BITFIELDS` 可能让 GDI 在 blit 内部逐像素重打包，把省下的时间又吃回去。
//
// 编译：g++ -std=c++17 -O2 dib_bitfields_probe.cpp -o dib_bitfields_probe.exe -lgdi32 -luser32

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

HBITMAP make_dib(HDC screen, int width, int height, DWORD compression, void** bits) {
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = compression;
  if (compression == BI_BITFIELDS) {
    auto* masks = reinterpret_cast<DWORD*>(info.bmiColors);
    masks[0] = 0xFF000000U;  // R 在 byte[3]
    masks[1] = 0x00FF0000U;  // G 在 byte[2]
    masks[2] = 0x0000FF00U;  // B 在 byte[1]
  }
  return ::CreateDIBSection(screen, &info, DIB_RGB_COLORS, bits, nullptr, 0);
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
  std::printf("画布 %dx%d（%.1f Mpx）\n\n", width, height, count / 1e6);

  HDC screen = ::GetDC(nullptr);
  void* bits_plain = nullptr;
  void* bits_bf = nullptr;
  HBITMAP dib_plain = make_dib(screen, width, height, BI_RGB, &bits_plain);
  HBITMAP dib_bf = make_dib(screen, width, height, BI_BITFIELDS, &bits_bf);
  std::printf("[1] BI_RGB      建立: %s\n", (dib_plain && bits_plain) ? "成功" : "失败");
  std::printf("[2] BI_BITFIELDS 建立: %s", (dib_bf && bits_bf) ? "成功" : "失败");
  if (dib_bf == nullptr) std::printf("（GetLastError=%lu）", ::GetLastError());
  std::printf("\n");
  if (dib_plain == nullptr || bits_plain == nullptr) return 1;

  // ── 通道校验 ──
  bool channels_ok = false;
  if (bits_bf != nullptr) {
    auto* px = static_cast<std::uint32_t*>(bits_bf);
    HDC memc = ::CreateCompatibleDC(screen);
    HGDIOBJ oldb = ::SelectObject(memc, dib_bf);
    px[0] = 0xFF000080U;  // R=0xFF,G=0,B=0,A=0x80
    const COLORREF c1 = ::GetPixel(memc, 0, 0);
    px[0] = 0x12345600U;  // R=0x12,G=0x34,B=0x56,A=0
    const COLORREF c2 = ::GetPixel(memc, 0, 0);
    ::SelectObject(memc, oldb);
    ::DeleteDC(memc);
    const int r1 = static_cast<int>(GetRValue(c1));
    const int g1 = static_cast<int>(GetGValue(c1));
    const int b1 = static_cast<int>(GetBValue(c1));
    const int r2 = static_cast<int>(GetRValue(c2));
    const int g2 = static_cast<int>(GetGValue(c2));
    const int b2 = static_cast<int>(GetBValue(c2));
    std::printf("[3] 通道校验 0xFF000080 → RGB(%d,%d,%d) 期望(255,0,0) %s\n", r1, g1, b1,
                (r1 == 255 && g1 == 0 && b1 == 0) ? "✓" : "✗");
    std::printf("              0x12345600 → RGB(%d,%d,%d) 期望(18,52,86) %s\n", r2, g2, b2,
                (r2 == 0x12 && g2 == 0x34 && b2 == 0x56) ? "✓" : "✗");
    channels_ok = (r1 == 255 && g1 == 0 && b1 == 0) && (r2 == 0x12 && g2 == 0x34 && b2 == 0x56);
  }

  // ── 端到端计时：重排/拷贝 + 真 BitBlt 到窗口 ──
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &proc;
  wc.hInstance = ::GetModuleHandleW(nullptr);
  wc.lpszClassName = L"dib_bitfields_probe";
  ::RegisterClassExW(&wc);
  const int win_w = std::min(width, 1000);
  const int win_h = std::min(height, 600);
  HWND win = ::CreateWindowExW(0, L"dib_bitfields_probe", L"t", WS_POPUP | WS_VISIBLE, 30, 30, win_w,
                               win_h, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
  if (win == nullptr) {
    std::printf("窗口创建失败\n");
    return 1;
  }
  MSG msg{};
  for (int i = 0; i < 40; ++i) {
    while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      ::TranslateMessage(&msg);
      ::DispatchMessageW(&msg);
    }
    ::Sleep(5);
  }

  std::vector<std::uint32_t> source(count);
  for (std::size_t i = 0; i < count; ++i) {
    const std::uint32_t v = static_cast<std::uint32_t>(i * 2654435761U);
    source[i] = (v & 0xFFFFFF00U) | 0x000000FFU;
  }
  auto* target_plain = static_cast<std::uint32_t*>(bits_plain);
  auto* target_bf = static_cast<std::uint32_t*>(bits_bf);

  constexpr int kIterations = 50;
  std::vector<double> path_current;  // 重排 + BI_RGB + BitBlt
  std::vector<double> path_candidate;  // memcpy + BI_BITFIELDS + BitBlt
  std::vector<double> reorder_only;
  std::vector<double> copy_only;

  HDC window_dc = ::GetDC(win);
  HDC mem_plain = ::CreateCompatibleDC(screen);
  HGDIOBJ old_plain = ::SelectObject(mem_plain, dib_plain);
  HDC mem_bf = ::CreateCompatibleDC(screen);
  HGDIOBJ old_bf = (dib_bf != nullptr) ? ::SelectObject(mem_bf, dib_bf) : nullptr;

  for (int i = 0; i < kIterations; ++i) {
    auto t0 = Clock::now();
    for (std::size_t index = 0; index < count; ++index) {
      const std::uint32_t pixel = source[index];
      const std::uint32_t r = (pixel >> 24U) & 0xFFU;
      const std::uint32_t g = (pixel >> 16U) & 0xFFU;
      const std::uint32_t b = (pixel >> 8U) & 0xFFU;
      const std::uint32_t a = pixel & 0xFFU;
      target_plain[index] = (a << 24U) | (r << 16U) | (g << 8U) | b;
    }
    reorder_only.push_back(ms_since(t0));

    t0 = Clock::now();
    ::BitBlt(window_dc, 0, 0, win_w, win_h, mem_plain, 0, 0, SRCCOPY);
    path_current.push_back(ms_since(t0));
  }
  if (dib_bf != nullptr) {
    for (int i = 0; i < kIterations; ++i) {
      auto t0 = Clock::now();
      std::memcpy(target_bf, source.data(), count * sizeof(std::uint32_t));
      copy_only.push_back(ms_since(t0));

      t0 = Clock::now();
      ::BitBlt(window_dc, 0, 0, win_w, win_h, mem_bf, 0, 0, SRCCOPY);
      path_candidate.push_back(ms_since(t0));
    }
  }

  ::SelectObject(mem_plain, old_plain);
  if (old_bf != nullptr) ::SelectObject(mem_bf, old_bf);
  ::DeleteDC(mem_plain);
  ::DeleteDC(mem_bf);
  ::ReleaseDC(win, window_dc);

  const Stats sCur = summarize(path_current);
  const Stats sCand = summarize(path_candidate);
  const Stats sReorder = summarize(reorder_only);
  const Stats sCopy = summarize(copy_only);

  std::printf("\n耗时（ms，median，含真 BitBlt 到窗口）：\n");
  std::printf("  现路径 重排 + BI_RGB + BitBlt        %7.3f\n", sCur.med_ms);
  std::printf("    其中纯重排                        %7.3f\n", sReorder.med_ms);
  if (!path_candidate.empty()) {
    std::printf("  候选  memcpy + BI_BITFIELDS + BitBlt %7.3f\n", sCand.med_ms);
    std::printf("    其中纯 memcpy                     %7.3f\n", sCopy.med_ms);
    const double saved = sCur.med_ms - sCand.med_ms;
    std::printf("\n  端到端可省 = %.3f ms（%.1f%%）\n", saved, saved / sCur.med_ms * 100.0);
  }

  std::printf("\n结论: ");
  if (!channels_ok) {
    std::printf("BI_BITFIELDS 通道读错 → 此方案不可用\n");
  } else if (!path_candidate.empty() && sCand.med_ms < sCur.med_ms) {
    std::printf("可用（通道正确、端到端更快）\n");
  } else {
    std::printf("通道正确但端到端没更快 → 不值得改\n");
  }

  ::DestroyWindow(win);
  ::UnregisterClassW(L"dib_bitfields_probe", ::GetModuleHandleW(nullptr));
  if (dib_bf != nullptr) ::DeleteObject(dib_bf);
  ::DeleteObject(dib_plain);
  ::ReleaseDC(nullptr, screen);
  return channels_ok ? 0 : 2;
}

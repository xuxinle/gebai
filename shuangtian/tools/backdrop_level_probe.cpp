/// 启动期窗口背景的**透明度实验**：三种档位各抓一次屏，看哪种是"只有卡悬浮"。
///
/// ## 为什么要做这个实验（用户要求）
///
/// 用户要的是「动画窗口也要做背景透明，只看到悬浮的动画」。
/// 而当前实现（Acrylic 磨砂 + 分层窗口）会**让整个窗口显形**：
/// Acrylic 是整窗材质，即使画布 alpha=0，那层玻璃照样铺满窗口——
/// 用户看到的是"一块 447×403 的方块，中间有个卡"。
///
/// 三条可能的通路，必须先量清楚再动代码：
///
/// | 档位 | 做法 | 预期 | 代价 |
/// |---|---|---|---|
/// | A | 保留 Acrylic（现状） | 整窗磨砂方块 | 无 |
/// | B | 关 Acrylic，纯分层 ULW | **只有卡**，卡内半透明但**无背景模糊** | 失去磨砂质感 |
/// | C | 关 Acrylic，分层 + 卡内自己画"假玻璃" | 只有卡，卡内是均匀半透明 | 同上 |
///
/// ## 怎么跑
///
/// 它自己建一个最小 Win32 分层窗口（不依赖霜天），三种档位各显示 1.2 秒、
/// 从屏幕抓图并报告：窗口四角像素、窗口中心像素、以及与桌面底色的差异。
///
/// 用法：`backdrop_level_probe [输出目录]`（缺省 `build/backdrop-levels`）。

#include <windows.h>
#include <dwmapi.h>

#include <cstdio>
#include <string>
#include <vector>

#include "st/codec/png.hpp"
#include "st/core/fs.hpp"

namespace {

constexpr int kWidth = 447;
constexpr int kHeight = 403;
constexpr wchar_t kClass[] = L"StBackdropLevelProbe";

struct AccentPolicy {
  int accent_state;
  int accent_flags;
  std::uint32_t gradient_color;
  int animation_id;
};
struct WindowCompositionAttributeData {
  int attribute;
  void* data;
  std::size_t size;
};

constexpr int kAccentDisabled = 0;
constexpr int kAccentEnableAcrylicBlurBehind = 4;
constexpr int kWcaAccentPolicy = 19;

auto set_accent(HWND hwnd, int state, std::uint32_t tint) -> bool {
  using Fn = int (*)(HWND, WindowCompositionAttributeData*);
  static const auto fn = [] {
    const HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    if (user32 == nullptr) return Fn{nullptr};
    return reinterpret_cast<Fn>(
        reinterpret_cast<void*>(::GetProcAddress(user32, "SetWindowCompositionAttribute")));
  }();
  if (fn == nullptr) return false;
  AccentPolicy policy{};
  policy.accent_state = state;
  policy.accent_flags = 0;
  policy.gradient_color = tint;
  WindowCompositionAttributeData data{};
  data.attribute = kWcaAccentPolicy;
  data.data = &policy;
  data.size = sizeof(policy);
  return fn(hwnd, &data) != 0;
}

/// 抓屏 → RGBA
auto grab(int x, int y, int w, int h) -> std::vector<std::uint8_t> {
  HDC screen = ::GetDC(nullptr);
  HDC mem = ::CreateCompatibleDC(screen);
  HBITMAP bmp = ::CreateCompatibleBitmap(screen, w, h);
  HGDIOBJ old_bitmap = ::SelectObject(mem, bmp);
  ::BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY);

  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = w;
  info.bmiHeader.biHeight = -h;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  std::vector<std::uint8_t> buffer(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U);
  ::GetDIBits(mem, bmp, 0, static_cast<UINT>(h), buffer.data(), &info, DIB_RGB_COLORS);

  ::SelectObject(mem, old_bitmap);
  ::DeleteObject(bmp);
  ::DeleteDC(mem);
  ::ReleaseDC(nullptr, screen);
  return buffer;   // BGRA
}

void save_png(const std::string& path, const std::vector<std::uint8_t>& bgra, int w, int h) {
  st::codec::PngImage image{};
  image.width = static_cast<std::uint32_t>(w);
  image.height = static_cast<std::uint32_t>(h);
  image.rgba.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U);
  for (std::size_t index = 0; index < image.rgba.size() / 4U; ++index) {
    image.rgba[index * 4U + 0U] = bgra[index * 4U + 2U];
    image.rgba[index * 4U + 1U] = bgra[index * 4U + 1U];
    image.rgba[index * 4U + 2U] = bgra[index * 4U + 0U];
    image.rgba[index * 4U + 3U] = 255U;
  }
  (void)st::codec::png_write_file(path, image);
}

/// 画布：中心一块圆角"卡"（半透明白），四周 alpha=0。
auto make_card_pixels(int w, int h) -> std::vector<std::uint32_t> {
  std::vector<std::uint32_t> pixels(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0U);
  const int card_w = static_cast<int>(static_cast<float>(w) * 0.56f);
  const int card_h = static_cast<int>(static_cast<float>(h) * 0.56f);
  const int x0 = (w - card_w) / 2;
  const int y0 = (h - card_h) / 2;
  const int radius = 26;
  for (int y = y0; y < y0 + card_h; ++y) {
    for (int x = x0; x < x0 + card_w; ++x) {
      // 圆角判定
      const int cx = x < x0 + radius ? x0 + radius : (x >= x0 + card_w - radius ? x0 + card_w - radius - 1 : x);
      const int cy = y < y0 + radius ? y0 + radius : (y >= y0 + card_h - radius ? y0 + card_h - radius - 1 : y);
      const int dx = x - cx;
      const int dy = y - cy;
      if (dx * dx + dy * dy > radius * radius) continue;
      // 预乘 alpha：白色、alpha=0.22（与启动卡 card_opacity 同档）
      const std::uint32_t alpha = 56U;
      const std::uint32_t channel = 56U;   // 预乘：255 * 0.22
      pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
             static_cast<std::size_t>(x)] = (channel << 24U) | (channel << 16U) | (channel << 8U) | alpha;
    }
  }
  return pixels;
}

/// 把预乘 RGBA 画布送进分层窗口（ULW）。
void present_layered(HWND hwnd, const std::vector<std::uint32_t>& pixels, int w, int h) {
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = w;
  info.bmiHeader.biHeight = -h;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HDC screen = ::GetDC(nullptr);
  HDC mem = ::CreateCompatibleDC(screen);
  HBITMAP dib = ::CreateDIBSection(mem, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  HGDIOBJ old = ::SelectObject(mem, dib);
  auto* dst = static_cast<std::uint32_t*>(bits);
  for (std::size_t index = 0; index < pixels.size(); ++index) {
    const std::uint32_t value = pixels[index];
    // 画布 0xRRGGBBAA（预乘）→ DIB 内存序 BGRA（预乘）= 0xAARRGGBB
    dst[index] = (value >> 8U) | ((value & 0xFFU) << 24U);
  }
  RECT rect{};
  ::GetWindowRect(hwnd, &rect);
  POINT destination{rect.left, rect.top};
  SIZE size{w, h};
  POINT source{0, 0};
  BLENDFUNCTION blend{};
  blend.BlendOp = AC_SRC_OVER;
  blend.SourceConstantAlpha = 255;
  blend.AlphaFormat = AC_SRC_ALPHA;
  ::UpdateLayeredWindow(hwnd, nullptr, &destination, &size, mem, &source, 0, &blend, ULW_ALPHA);
  ::SelectObject(mem, old);
  ::DeleteObject(dib);
  ::DeleteDC(mem);
  ::ReleaseDC(nullptr, screen);
}

}  // namespace

int main(int argc, char** argv) {
  const std::string out_dir = argc > 1 ? argv[1] : "build/backdrop-levels";
  (void)st::fs::create_directories(out_dir);
  ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  WNDCLASSEXW cls{};
  cls.cbSize = sizeof(cls);
  cls.lpfnWndProc = ::DefWindowProcW;
  cls.hInstance = ::GetModuleHandleW(nullptr);
  cls.lpszClassName = kClass;
  cls.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  if (::RegisterClassExW(&cls) == 0) {
    std::printf("RegisterClassExW 失败\n");
    return 1;
  }

  const int screen_w = ::GetSystemMetrics(SM_CXSCREEN);
  const int screen_h = ::GetSystemMetrics(SM_CYSCREEN);
  const int left = (screen_w - kWidth) / 2;
  const int top = (screen_h - kHeight) / 2;

  HWND hwnd = ::CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, kClass, L"probe", WS_POPUP, left, top,
                                kWidth, kHeight, nullptr, nullptr, cls.hInstance, nullptr);
  if (hwnd == nullptr) {
    std::printf("CreateWindowExW 失败\n");
    return 1;
  }
  const auto pixels = make_card_pixels(kWidth, kHeight);

  struct Level {
    const char* name;
    bool acrylic;
    bool draw_card;
  };
  const Level levels[] = {
      {"A-acrylic", true, true},
      {"B-layered-only", false, true},
  };

  std::printf("窗口 %dx%d @(%d,%d)，屏幕 %dx%d\n", kWidth, kHeight, left, top, screen_w, screen_h);
  for (const auto& level : levels) {
    const BOOL dark = TRUE;
    (void)::DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    if (level.acrylic) {
      const MARGINS margins{-1, -1, -1, -1};
      (void)::DwmExtendFrameIntoClientArea(hwnd, &margins);
      (void)set_accent(hwnd, kAccentEnableAcrylicBlurBehind, 0x99181818U);
    } else {
      (void)set_accent(hwnd, kAccentDisabled, 0);
    }
    if (level.draw_card) present_layered(hwnd, pixels, kWidth, kHeight);
    ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    ::SetWindowPos(hwnd, HWND_TOPMOST, left, top, kWidth, kHeight, SWP_NOACTIVATE);
    // 让合成器稳定
    for (int tick = 0; tick < 40; ++tick) {
      ::Sleep(16);
      MSG message{};
      while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
      }
    }
    // 抓窗口 + 周围（看窗口边界是否可见）
    const int pad = 40;
    const auto shot = grab(left - pad, top - pad, kWidth + pad * 2, kHeight + pad * 2);
    const std::string path = st::fs::join(out_dir, std::string(level.name) + ".png");
    save_png(path, shot, kWidth + pad * 2, kHeight + pad * 2);

    const int shot_w = kWidth + pad * 2;
    const auto at = [&](int x, int y) {
      const std::size_t offset =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(shot_w) + static_cast<std::size_t>(x)) * 4U;
      if (offset + 3 >= shot.size()) return 0U;
      std::uint32_t value = 0U;
      value |= static_cast<std::uint32_t>(shot[offset + 2]) << 16U;
      value |= static_cast<std::uint32_t>(shot[offset + 1]) << 8U;
      value |= static_cast<std::uint32_t>(shot[offset + 0]);
      return value;
    };
    const std::uint32_t desktop_corner = at(4, 4);
    const std::uint32_t window_corner = at(pad + 6, pad + 6);
    const std::uint32_t card_center = at(pad + kWidth / 2, pad + kHeight / 2);
    std::printf("\n[%s]\n", level.name);
    std::printf("  桌面底色 #%06X · 窗口角 #%06X · 卡中心 #%06X\n", desktop_corner, window_corner,
                card_center);
    const bool corner_matches_desktop = (window_corner == desktop_corner);
    std::printf("  窗口角与桌面一致（=只有卡可见）: %s\n", corner_matches_desktop ? "是" : "否");
  }

  ::DestroyWindow(hwnd);
  ::UnregisterClassW(kClass, cls.hInstance);
  std::printf("\n产物目录：%s\n", out_dir.c_str());
  return 0;
}

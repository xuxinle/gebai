// window_frame_nc_probe —— 去装饰窗口「失焦时的系统灰边」：是谁画的、怎么消掉。
//
// ## 问题（用户报）
//
// 霜天桌面窗口在**失焦**（点别的程序、Alt+Tab）后，四周内侧冒出一圈约 10px 的灰白边框，
// 把应用画的外圈像素盖掉；激活后自己消失。用户的原话是「未激活的灰色边框」。
//
// ## 本探针一次性给出的结论（Win11 26200）
//
// | 变体 | 失焦时的带 |
// |---|---|
// | 默认（`WM_NCACTIVATE` 交给 `DefWindowProc`） | **10px** |
// | `WM_NCACTIVATE` 里 `lParam = -1` 调 `DefWindowProc` | **0px** |
// | `WM_NCPAINT` 里 `return 0`（不画非客户区） | 10px（拦不住） |
// | `DWMWA_NCRENDERING_POLICY = DWMNCRP_DISABLED` | 10px（拦不住） |
// | `WS_EX_LAYERED` / 改 DWM 圆角与描边属性 | 10px（全都无关） |
// | `WM_NCCALCSIZE` 不归零（系统保留自己的 NC 区） | 0px，但客户区被吃掉一圈（等于放弃自绘窗框） |
//
// ⇒ 真因是 `DefWindowProc(WM_NCACTIVATE)` 按 `WS_THICKFRAME` 把「未激活边框」画进了
// 我们声明的**整窗客户区**（`WM_NCCALCSIZE` 返回 0 的代价）。修法见
// `src/shell/platform_win32.cpp` 的 `WM_NCACTIVATE` 分支：`lParam = -1` 只更新激活状态、
// 不重画非客户区。
//
// ## 复跑方式
//
//   g++ -std=c++20 -O2 window_frame_nc_probe.cpp -o nc_probe.exe -ldwmapi -lgdi32 -luser32
//   nc_probe.exe
//
// 判据只有一条：失焦带 == 0。

#define NOMINMAX 1
#include <windows.h>
#include <dwmapi.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr int kW = 400, kH = 300;
constexpr int kScan = 16;
constexpr COLORREF kFill = RGB(30, 90, 200);   // 鲜明蓝：外来像素一眼可辨

constexpr DWORD kDwmwaNcRenderingPolicy = 2;
constexpr DWORD kDwmwaCorner = 33;
constexpr DWORD kDwmwaBorder = 34;
constexpr int kDwmNcRenderingDisabled = 1;
constexpr COLORREF kColorNone = 0xFFFFFFFE;

/// `WM_NCACTIVATE` 的处理方式（本探针的被测变量）。
enum class NcActivateMode { kDefault, kPassMinusOne, kReturnTrue, kReturnFalse };

struct PerWindow {
  NcActivateMode mode{NcActivateMode::kDefault};
  bool swallow_ncpaint{false};
  bool nccalc_zero{true};
};

LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  auto* st = reinterpret_cast<PerWindow*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
  if (m == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
    if (cs != nullptr) {
      ::SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
      st = static_cast<PerWindow*>(cs->lpCreateParams);
    }
  }
  if (m == WM_NCCALCSIZE && st != nullptr && st->nccalc_zero && w != 0) {
    return 0;   // 客户区 = 整窗（自绘窗框的前提）
  }
  if (m == WM_NCACTIVATE && st != nullptr) {
    switch (st->mode) {
      case NcActivateMode::kPassMinusOne:
        // 只更新激活状态、**不重画**非客户区——这就是修法。
        (void)::DefWindowProcW(h, m, w, -1);
        return TRUE;
      case NcActivateMode::kReturnTrue:
        return TRUE;
      case NcActivateMode::kReturnFalse:
        return FALSE;
      case NcActivateMode::kDefault:
        break;
    }
  }
  if (m == WM_NCPAINT && st != nullptr && st->swallow_ncpaint) return 0;
  if (m == WM_ERASEBKGND || m == WM_PAINT) {
    HDC dc = nullptr;
    PAINTSTRUCT ps{};
    bool began = false;
    if (m == WM_PAINT) {
      dc = ::BeginPaint(h, &ps);
      began = true;
    } else {
      dc = reinterpret_cast<HDC>(w);
    }
    RECT rc{};
    ::GetClientRect(h, &rc);
    HBRUSH b = ::CreateSolidBrush(kFill);
    ::FillRect(dc, &rc, b);
    ::DeleteObject(b);
    if (began) ::EndPaint(h, &ps);
    return (m == WM_PAINT) ? 0 : 1;
  }
  return ::DefWindowProcW(h, m, w, l);
}

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

/// 从窗口 DC 左缘起数「连续不是填充色」的像素数——就是那条灰带的宽度。
int band_width(HWND h, int y) {
  HDC dc = ::GetDC(h);
  if (dc == nullptr) return -1;
  RECT client{};
  ::GetClientRect(h, &client);
  const int fb = GetBValue(kFill), fg = GetGValue(kFill), fr = GetRValue(kFill);
  int width = 0;
  for (int x = 0; x < client.right; ++x) {
    const COLORREF c = ::GetPixel(dc, x, y);
    if (c == CLR_INVALID) break;
    const int dr = std::abs(static_cast<int>(GetRValue(c)) - fr);
    const int dg = std::abs(static_cast<int>(GetGValue(c)) - fg);
    const int db = std::abs(static_cast<int>(GetBValue(c)) - fb);
    if (dr > 6 || dg > 6 || db > 6) {
      ++width;
    } else {
      break;
    }
  }
  ::ReleaseDC(h, dc);
  return width;
}

struct Variant {
  const char* name;
  NcActivateMode mode;
  bool swallow_ncpaint;
  bool disable_nc_rendering;
  DWORD exstyle;
  bool nccalc_zero;
};

void run(const Variant& v, int idx, HWND other) {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &proc;
  wc.hInstance = ::GetModuleHandleW(nullptr);
  wc.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  const std::wstring cls = L"ncprobe_" + std::to_wstring(idx);
  static PerWindow states[32];
  PerWindow& st = states[idx];
  st = PerWindow{};
  st.mode = v.mode;
  st.swallow_ncpaint = v.swallow_ncpaint;
  st.nccalc_zero = v.nccalc_zero;
  wc.lpszClassName = cls.c_str();
  ::RegisterClassExW(&wc);
  const DWORD style = WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
  HWND win = ::CreateWindowExW(v.exstyle, cls.c_str(), L"ncprobe", style, 340, 260, kW, kH, nullptr,
                               nullptr, ::GetModuleHandleW(nullptr), &st);
  if (win == nullptr) {
    std::printf("\n[%s] CREATE_FAILED %lu\n", v.name, ::GetLastError());
    return;
  }
  if (v.disable_nc_rendering) {
    const int policy = kDwmNcRenderingDisabled;
    (void)::DwmSetWindowAttribute(win, kDwmwaNcRenderingPolicy, &policy, sizeof(policy));
  }
  // 一直给着的三项 DWM 属性（现状实现）：触发外阴影 + 关圆角 + 关系统描边。
  {
    MARGINS margins{1, 1, 1, 1};
    (void)::DwmExtendFrameIntoClientArea(win, &margins);
    const int corner = 1;   // DWMWCP_DONOTROUND
    (void)::DwmSetWindowAttribute(win, kDwmwaCorner, &corner, sizeof(corner));
    const COLORREF none = kColorNone;
    (void)::DwmSetWindowAttribute(win, kDwmwaBorder, &none, sizeof(none));
  }
  ::SetWindowPos(win, HWND_TOPMOST, 340, 260, kW, kH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
  ::ShowWindow(win, SW_SHOW);
  ::SetForegroundWindow(win);
  pump(320);
  const int active = band_width(win, kH / 2);

  ::SetForegroundWindow(other);
  pump(450);
  const bool inactive = ::GetForegroundWindow() != win;
  const int inactive_band = band_width(win, kH / 2);

  RECT wr{}, cr{};
  ::GetWindowRect(win, &wr);
  ::GetClientRect(win, &cr);
  std::printf("\n[%s]\n", v.name);
  std::printf("  激活带=%dpx  失焦带=%dpx  真的失焦=%d  客户区 %ldx%ld / 窗口 %ldx%ld\n", active,
              inactive_band, inactive ? 1 : 0, cr.right, cr.bottom, wr.right - wr.left,
              wr.bottom - wr.top);
  HDC dc = ::GetDC(win);
  if (dc != nullptr) {
    std::printf("  失焦 DC 左缘 x=0..%d:", kScan - 1);
    for (int x = 0; x < kScan; ++x) {
      const COLORREF c = ::GetPixel(dc, x, kH / 2);
      std::printf(" %02x%02x%02x", GetRValue(c), GetGValue(c), GetBValue(c));
    }
    std::printf("\n");
    ::ReleaseDC(win, dc);
  }
  ::DestroyWindow(win);
}

}  // namespace

int main() {
  ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &proc;
  wc.hInstance = ::GetModuleHandleW(nullptr);
  wc.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  wc.lpszClassName = L"ncprobe_back";
  ::RegisterClassExW(&wc);
  HWND back = ::CreateWindowExW(WS_EX_TOOLWINDOW, L"ncprobe_back", L"back", WS_POPUP | WS_VISIBLE,
                                0, 0, 1600, 1200, nullptr, nullptr, ::GetModuleHandleW(nullptr),
                                nullptr);
  // 抢焦点用的小窗：放在左上角，不会遮住被测窗（被测窗在 340,260）
  HWND other = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"ncprobe_back", L"other",
                                 WS_POPUP | WS_VISIBLE, 5, 5, 120, 60, nullptr, nullptr,
                                 ::GetModuleHandleW(nullptr), nullptr);
  (void)back;
  pump(220);
  std::printf("==== 失焦灰带：是谁画的（填充 = 1e5ac8，带 = 非填充色的连续像素数）====\n");
  const std::vector<Variant> variants = {
      {"A 现状：WM_NCACTIVATE 交给 DefWindowProc", NcActivateMode::kDefault, false, false, 0, true},
      {"B 修法：DefWindowProc(WM_NCACTIVATE, lParam=-1) 后返回 TRUE",
       NcActivateMode::kPassMinusOne, false, false, 0, true},
      {"C 对照：直接返回 TRUE（不调默认实现）", NcActivateMode::kReturnTrue, false, false, 0, true},
      {"D 对照：直接返回 FALSE", NcActivateMode::kReturnFalse, false, false, 0, true},
      {"E 对照：WM_NCPAINT 里 return 0（拦非客户区绘制）", NcActivateMode::kDefault, true, false, 0,
       true},
      {"F 对照：DWMWA_NCRENDERING_POLICY=DISABLED", NcActivateMode::kDefault, false, true, 0, true},
      {"G 对照：WS_EX_LAYERED", NcActivateMode::kDefault, false, false, WS_EX_LAYERED, true},
      {"H 对照：不归零 WM_NCCALCSIZE（系统保留自己的 NC 区）", NcActivateMode::kDefault, false, false,
       0, false},
  };
  for (size_t i = 0; i < variants.size(); ++i) run(variants[i], static_cast<int>(i), other);
  ::DestroyWindow(other);
  ::DestroyWindow(back);
  return 0;
}

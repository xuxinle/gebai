// 平台边界：本文件集中封装 Win32 窗口与输入 API（CONVENTIONS §10：系统头/平台宏/char* 边界只在此处）。
//
// Win32 后端的职责：把框架的"逻辑坐标 UI + 物理像素帧缓冲"映射到一个真实窗口上。
//   - 帧缓冲仍是软件光栅器（唯一真相源），本文件只负责**把像素搬到窗口**；
//   - 输入从窗口消息翻译成框架的 `ui::Event`，坐标换算逻辑像素（与协议、脚本同一口径）；
//   - 无窗口系统时（如服务会话）由上层回退 headless，本文件不做降级判断。

// 本文件只在 Windows 目标编译：`src/shell/*.cpp` 的 glob 是所有平台共用的，
// 若不加守卫，Linux/macOS 构建会去包含 `<windows.h>` 直接失败。
// （对比：X11/Wayland 用运行时 dlopen，所以不需要这个守卫。）
#if defined(_WIN32)

// `windows.h` 默认把 `min`/`max` 定义成宏：一旦带上，`std::min`/`std::max` 的调用点
// 会以“C2589 非法标记”报错，与真正原因相隔很远（实测本文件 8 处报错都源于此）。
#define NOMINMAX 1
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"
#include "st/core/log.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/gpu.hpp"
#include "st/shell/shell.hpp"

namespace st::shell {
namespace {

constexpr wchar_t kWindowClass[] = L"ShuangtianWindow";

[[nodiscard]] auto to_wide(std::string_view utf8) -> std::wstring {
  if (utf8.empty()) return {};
  const int needed =
      ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (needed <= 0) return {};
  std::wstring out(static_cast<std::size_t>(needed), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
  return out;
}

[[nodiscard]] auto to_utf8(std::wstring_view wide) -> std::string {
  if (wide.empty()) return {};
  const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
  if (needed <= 0) return {};
  std::string out(static_cast<std::size_t>(needed), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), needed,
                        nullptr, nullptr);
  return out;
}

/// 虚拟键 → 框架键名（与 X11 后端 / 控制通道使用同一套名字，UI 逻辑跨平台一致）。
[[nodiscard]] auto key_name(WPARAM key) -> std::string {
  if (key >= 'A' && key <= 'Z') return std::string(1, static_cast<char>('a' + (key - 'A')));
  if (key >= '0' && key <= '9') return std::string(1, static_cast<char>(key));
  switch (key) {
    case VK_RETURN: return "Enter";
    case VK_ESCAPE: return "Escape";
    case VK_TAB: return "Tab";
    case VK_BACK: return "Backspace";
    case VK_DELETE: return "Delete";
    case VK_SPACE: return "Space";
    case VK_LEFT: return "ArrowLeft";
    case VK_RIGHT: return "ArrowRight";
    case VK_UP: return "ArrowUp";
    case VK_DOWN: return "ArrowDown";
    case VK_HOME: return "Home";
    case VK_END: return "End";
    case VK_PRIOR: return "PageUp";
    case VK_NEXT: return "PageDown";
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT: return "Shift";
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL: return "Control";
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU: return "Alt";
    default: break;
  }
  return {};
}

/// UTF-16 码元 → UTF-8（`WM_CHAR` 可能给到代理对的一半，需按窗口状态成对处理）。
void append_utf8(std::string& out, char32_t code_point) {
  if (code_point <= 0x7F) {
    out.push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else if (code_point <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  }
}

class Win32Backend final : public Backend {
 public:
  Win32Backend() = default;
  ~Win32Backend() override {
    if (dib_ != nullptr) ::DeleteObject(dib_);
    if (memory_dc_ != nullptr) ::DeleteDC(memory_dc_);
    if (window_ != nullptr) ::DestroyWindow(window_);
    if (class_registered_) ::UnregisterClassW(kWindowClass, ::GetModuleHandleW(nullptr));
  }

  [[nodiscard]] auto name() const noexcept -> std::string_view override { return "win32"; }
  [[nodiscard]] auto headless() const noexcept -> bool override { return false; }
  [[nodiscard]] auto close_requested() const noexcept -> bool override { return close_requested_; }

  auto create_window(const WindowOptions& options) -> Status override {
    // DPI 感知：不声明的话系统会把我们的窗口位图**再拉伸一次**（模糊 + 坐标错位）。
    // 目标为 Win10 1703+（`SetProcessDpiAwarenessContext`）；更老的系统上该导入不存在，
    // 因此这里用**弱加载**语义：只在探测到导入可用时调用，否则退回 Vista 的 `SetProcessDPIAware`。
    if (::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) == 0) {
      (void)::SetProcessDPIAware();
    }

    logical_width_ = options.width > 0 ? options.width : 1280;
    logical_height_ = options.height > 0 ? options.height : 720;
    scale_ = options.scale > 0.0f ? options.scale : 1.0f;
    // 显式给了 `--scale` 就**不再被窗口 DPI 覆盖**：它是测试/复现用的覆盖值。
    // 关键在于"窗口尺寸"与"缓冲尺寸"必须用**同一个** scale——早先窗口按请求值建、
    // 缓冲按查询值建，两者不一致时 GPU 呈现只能让 DXGI 缩放 → 整屏发糊。
    const bool explicit_scale = options.scale > 0.0f;
  renderer_ = options.renderer.empty() ? "auto" : options.renderer;

    if (!class_registered_) {
      WNDCLASSEXW window_class{};
      window_class.cbSize = sizeof(window_class);
      // `CS_DBLCLKS`：双击/三击由系统识别（否则要自己算时间窗，与其它平台行为不一致）
      window_class.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
      window_class.lpfnWndProc = &Win32Backend::window_proc;
      window_class.hInstance = ::GetModuleHandleW(nullptr);
      // 32512 = 标准箭头光标（`IDC_ARROW` 是 ANSI 资源宏，类型与宽字符 API 不匹配，故用数值）
      window_class.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
      window_class.lpszClassName = kWindowClass;
      if (::RegisterClassExW(&window_class) == 0) {
        return unexpected(ErrorCode::Unsupported, "RegisterClassExW 失败（无窗口系统会话？）");
      }
      class_registered_ = true;
    }

    // 客户区要正好等于逻辑尺寸 × 缩放：先算"含边框的整体尺寸"再创建
    const int physical_width = static_cast<int>(std::lround(static_cast<float>(logical_width_) * scale_));
    const int physical_height = static_cast<int>(std::lround(static_cast<float>(logical_height_) * scale_));
    RECT rect{0, 0, physical_width, physical_height};
    const DWORD style = WS_OVERLAPPEDWINDOW;
    ::AdjustWindowRectEx(&rect, style, FALSE, 0);
    const std::wstring title = to_wide(options.title);

    window_ = ::CreateWindowExW(0, kWindowClass, title.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
                                rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                                ::GetModuleHandleW(nullptr), this);
    if (window_ == nullptr) {
      return unexpected(ErrorCode::Unsupported,
                        std::format("CreateWindowExW 失败（错误码 {}）", ::GetLastError()));
    }
    ::ShowWindow(window_, SW_SHOW);
    ::UpdateWindow(window_);

    // 窗口实际 DPI 可能与请求不同（多显示器/系统缩放）。
    // 未显式指定时以窗口为准；**但必须连窗口尺寸一起改**——
    // 上面是按"请求的 scale"建的窗口，若实际 DPI 不同就会出现
    // "1920×1200 的画布塞进 1280×800 的窗口"：GPU 呈现器只能让 DXGI 缩放，
    // 结果是**整屏发糊**（软件路径则表现为裁切）。这曾是一个真实缺陷，实测确认。
    const float requested_scale = scale_;
    if (!explicit_scale) scale_ = query_window_scale(window_, requested_scale);
    if (auto status = resize_window_to_scale(); !status) return status;
    if (!explicit_scale && scale_ != requested_scale) {
      log::info("win32 窗口 DPI 与请求不同：请求 {:.2f} → 实际 {:.2f}（窗口已按实际缩放调整）",
                static_cast<double>(requested_scale), static_cast<double>(scale_));
    }
    // 以**客户区为准**分配：窗口尺寸可能被系统调整过，逻辑尺寸反过来迁就它
    if (auto status = sync_buffers_to_client(scale_); !status) return status;
    log::info("win32 窗口已创建（逻辑 {}x{} · 缩放 {:.2f} · 物理 {}x{}）", logical_width_,
              logical_height_, static_cast<double>(scale_), surface_->physical_width(),
              surface_->physical_height());
    // 画布物理尺寸必须**等于**客户区，否则 DXGI 会缩放整块纹理（整屏发糊）。
    // 这条不变量在此显式核对一次：文件日志里看得见，比事发后再猜省事。
    if (window_ != nullptr) {
      RECT client{};
      if (::GetClientRect(window_, &client) != 0 &&
          (client.right != surface_->physical_width() || client.bottom != surface_->physical_height())) {
        log::warn("画布与客户区不一致：画布 {}x{}，客户区 {}x{}（GPU 呈现会被 DXGI 缩放）",
                  surface_->physical_width(), surface_->physical_height(), client.right,
                  client.bottom);
      }
    }
    return ok();
  }

  /// 按**当前客户区**分配绘制面，保证 `画布物理尺寸 == 客户区尺寸`。
  ///
  /// 所有会影响尺寸的路径（创建 / 改 DPI / WM_SIZE）都必须走这里，
  /// 否则就会重新引入"画布与客户区差 1px → DXGI 缩放 → 整屏发糊"。
  [[nodiscard]] auto sync_buffers_to_client(float scale) -> Status {
    if (window_ == nullptr) return ok();
    RECT client{};
    if (::GetClientRect(window_, &client) == 0) {
      return unexpected(ErrorCode::Io, "GetClientRect 失败");
    }
    // 客户区**原样**作为画布物理尺寸：不再做任何"逻辑尺寸 ↔ 物理尺寸"的往返换算，
    // 因此不可能出现 1 像素的偏差（那个偏差会让 DXGI 拉伸整块纹理 → 整屏发糊）。
    return allocate_buffers(std::max(1, static_cast<int>(client.right)),
                            std::max(1, static_cast<int>(client.bottom)), scale);
  }

  /// 把窗口**客户区**调整到 "逻辑尺寸 × 当前 scale"（保持左上角不动）。
  ///
  /// 为什么必须有它：`CreateWindowExW` 用的是创建时的 scale，而窗口落地在哪个显示器、
  /// 那个显示器是什么 DPI，只有创建之后才知道。两者不一致就会出现画布与窗口尺寸不符，
  /// 而 GPU 呈现器遇到这种情况会**静默缩放**（不报错，只是糊）。
  [[nodiscard]] auto resize_window_to_scale() -> Status {
    if (window_ == nullptr) return ok();
    const int client_width = std::max(1, static_cast<int>(std::lround(
        static_cast<double>(logical_width_) * static_cast<double>(scale_))));
    const int client_height = std::max(1, static_cast<int>(std::lround(
        static_cast<double>(logical_height_) * static_cast<double>(scale_))));
    RECT rect{0, 0, client_width, client_height};
    const DWORD style = static_cast<DWORD>(::GetWindowLongPtrW(window_, GWL_STYLE));
    if (::AdjustWindowRectEx(&rect, style, FALSE, 0) == 0) {
      return unexpected(ErrorCode::Io, "AdjustWindowRectEx 失败");
    }
    RECT current{};
    if (::GetWindowRect(window_, &current) == 0) return ok();
    const int wanted_width = rect.right - rect.left;
    const int wanted_height = rect.bottom - rect.top;
    if (current.right - current.left == wanted_width &&
        current.bottom - current.top == wanted_height) {
      return ok();   // 已经一致：不做无意义的 SetWindowPos（会引起闪烁）
    }
    if (::SetWindowPos(window_, nullptr, current.left, current.top, wanted_width, wanted_height,
                       SWP_NOZORDER | SWP_NOACTIVATE) == 0) {
      return unexpected(ErrorCode::Io, "SetWindowPos 失败");
    }
    return ok();
  }

  void present() override {
    pump_messages();
    if (surface_ == nullptr || window_ == nullptr) return;
    // GPU 画布优先走 swapchain（零 CPU 拷贝）；软件画布或未就绪时落回 GDI blit。
    if (presenter_ != nullptr) {
      if (auto presented = presenter_->present(*surface_, surface_->physical_width(),
                                               surface_->physical_height());
          presented) {
        ++present_frames_;
        return;
      } else if (!present_fallback_logged_) {
        // 只报一次：每帧刷屏会把日志淹掉，而"回退了"这个事实报一次就够
        log::warn("swapchain 呈现失败（{}），回退 GDI blit", presented.error().message);
        present_fallback_logged_ = true;
      }
    }
    if (dib_ == nullptr) return;
    blit();
    ++frames_;
  }

  [[nodiscard]] auto poll_event() -> std::optional<ui::Event> override {
    pump_messages();
    if (events_.empty()) return std::nullopt;
    ui::Event event = std::move(events_.front());
    events_.pop_front();
    return event;
  }

  void set_title(std::string_view title) override {
    if (window_ != nullptr) ::SetWindowTextW(window_, to_wide(title).c_str());
    title_ = std::string(title);
  }

  [[nodiscard]] auto clipboard_text() -> Result<std::string> override {
    if (!::OpenClipboard(window_)) {
      return unexpected(ErrorCode::Io, "打开剪贴板失败（可能被其它进程占用）");
    }
    std::string text;
    if (HANDLE handle = ::GetClipboardData(CF_UNICODETEXT); handle != nullptr) {
      if (const auto* wide = static_cast<const wchar_t*>(::GlobalLock(handle)); wide != nullptr) {
        text = to_utf8(wide);
        ::GlobalUnlock(handle);
      }
    }
    ::CloseClipboard();
    return text;
  }

  [[nodiscard]] auto set_clipboard_text(std::string_view text) -> Status override {
    const std::wstring wide = to_wide(text);
    if (!::OpenClipboard(window_)) {
      return unexpected(ErrorCode::Io, "打开剪贴板失败（可能被其它进程占用）");
    }
    (void)::EmptyClipboard();
    const std::size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes); memory != nullptr) {
      if (void* target = ::GlobalLock(memory); target != nullptr) {
        std::memcpy(target, wide.c_str(), bytes);
        ::GlobalUnlock(memory);
        ::SetClipboardData(CF_UNICODETEXT, memory);
      }
    }
    ::CloseClipboard();
    return ok();
  }

  [[nodiscard]] auto framebuffer() -> raster::Surface& override { return *surface_; }
  [[nodiscard]] auto renderer_name() const noexcept -> std::string_view override {
    return renderer_name_;
  }
  [[nodiscard]] auto renderer_note() const -> std::string override { return renderer_note_; }
  [[nodiscard]] auto frame_count() const noexcept -> std::uint64_t override { return frames_; }
  [[nodiscard]] auto device_scale() const noexcept -> float override { return scale_; }
  [[nodiscard]] auto logical_size() const noexcept -> math::Size override {
    // 用**精确**值（物理 / scale）而不是取整后的整数：布局按它排布，
    // 取整会让右侧/底部留下 1px 的空隙或溢出，与画布实际可绘区域不符。
    return math::Size{logical_width_exact_, logical_height_exact_};
  }

  auto set_device_scale(float scale) -> Status override {
    if (scale <= 0.0f) return unexpected(ErrorCode::Invalid, "DPI 缩放必须为正数");
    scale_ = scale;
    // 改 DPI 要动两处，缺一就糊：
    // ① 窗口客户区必须跟着变成「逻辑 × 新 scale」（只改缓冲会导致画布与客户区不符）；
    // ② 缓冲必须**以实际客户区为准**分配——`round(round(client/scale)*scale)` 在分数缩放下
    //    可能差 1 像素，而 1 像素就足以让 DXGI 拉伸整块纹理（整屏发糊，见
    //    `logical_for_client` 的说明）。两步都由下面两个函数保证。
    if (auto status = resize_window_to_scale(); !status) return status;
    return sync_buffers_to_client(scale_);
  }

 private:
  /// 窗口实际 DPI 缩放（优先 `GetDpiForWindow`，Win10 以下退回屏幕 DC 的 LOGPIXELSX）。
  [[nodiscard]] static auto query_window_scale(HWND window, float fallback) -> float {
    const UINT window_dpi = ::GetDpiForWindow(window);
    if (window_dpi > 0) return static_cast<float>(window_dpi) / 96.0f;
    if (HDC dc = ::GetDC(nullptr); dc != nullptr) {
      const int screen_dpi = ::GetDeviceCaps(dc, LOGPIXELSX);
      ::ReleaseDC(nullptr, dc);
      if (screen_dpi > 0) return static_cast<float>(screen_dpi) / 96.0f;
    }
    return fallback;
  }

  /// 按逻辑尺寸 + 缩放重建帧缓冲与 DIB（32bpp BGRA，正好是 GDI 能直接搬的格式）。
  /// 按**物理像素**尺寸分配绘制面（逻辑尺寸由 `物理 / scale` 折算）。
  ///
  /// 为什么参数是物理而不是逻辑：逻辑尺寸是整数，而 scale 常是 1.5 / 1.25 这类分数，
  /// `round(逻辑 × scale)` **取不到所有整数**——例如 1.5x 下奇数宽度根本不可达
  /// （`741×1.5 = 1111.5` 只能取到 1112）。于是画布与窗口客户区差 1 像素，
  /// DXGI 就会把整块纹理拉伸到客户区：**整屏发糊**（用户实测："切 DPI 后模糊、
  /// 重新缩放一下才好"）。把物理尺寸作为输入，这个偏差从根上消失。
  auto allocate_buffers(int physical_width, int physical_height, float scale) -> Status {
    scale_ = scale > 0.0f ? scale : 1.0f;
    const int clamped_width = std::max(1, physical_width);
    const int clamped_height = std::max(1, physical_height);
    // 逻辑尺寸只用于**报告**（`logical_size()`）与"改窗口尺寸"时的换算，取最接近的整数即可
    logical_width_ = std::max(1, static_cast<int>(std::lround(
        static_cast<double>(clamped_width) / static_cast<double>(scale_))));
    logical_height_ = std::max(1, static_cast<int>(std::lround(
        static_cast<double>(clamped_height) / static_cast<double>(scale_))));
    // 长宽比与 scale 无关；这里顺带记下精确值（`logical_size_exact_`），
    // 供 `logical_size()` 汇报——避免"整数折算"再被下游当成精确值使用
    logical_width_exact_ = static_cast<float>(clamped_width) / scale_;
    logical_height_exact_ = static_cast<float>(clamped_height) / scale_;
        SurfaceChoice choice = create_surface_for(clamped_width, clamped_height, scale_, renderer_);
    if (choice.surface == nullptr) return unexpected(ErrorCode::Io, "创建绘制面失败");
    surface_ = std::move(choice.surface);
    renderer_name_ = std::move(choice.name);
    renderer_note_ = std::move(choice.note);
    surface_->clear(math::Color{0, 0, 0, 0});

    // GPU 画布 + 真实窗口 → 建 swapchain 呈现器（这一步是"零拷贝上屏"的全部前提）
    presenter_.reset();
    if (renderer_name_ == "gpu" && window_ != nullptr) {
      if (auto created = raster::gpu::create_presenter(window_, surface_->physical_width(),
                                                       surface_->physical_height());
          created.has_value()) {
        presenter_ = std::move(*created);
        renderer_note_ += std::format(" · {}", presenter_->note());
      } else {
        renderer_note_ += std::format(" · 呈现回退 GDI（{}）", created.error().message);
      }
    }

    if (dib_ != nullptr) {
      ::DeleteObject(dib_);
      dib_ = nullptr;
    }
    if (memory_dc_ != nullptr) {
      ::DeleteDC(memory_dc_);
      memory_dc_ = nullptr;
    }
    HDC screen = ::GetDC(window_ != nullptr ? window_ : nullptr);
    memory_dc_ = ::CreateCompatibleDC(screen);
    if (window_ != nullptr) {
      ::ReleaseDC(window_, screen);
    } else {
      ::ReleaseDC(nullptr, screen);
    }
    if (memory_dc_ == nullptr) return unexpected(ErrorCode::Io, "CreateCompatibleDC 失败");

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = surface_->physical_width();
    // 负高度 = 自上而下：与画布的像素顺序（第一行在顶）一致，省一次翻转
    info.bmiHeader.biHeight = -surface_->physical_height();
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    dib_ = ::CreateDIBSection(memory_dc_, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (dib_ == nullptr || pixels == nullptr) return unexpected(ErrorCode::Io, "CreateDIBSection 失败");
    dib_pixels_ = static_cast<std::uint32_t*>(pixels);
    (void)::SelectObject(memory_dc_, dib_);
    return ok();
  }

  /// 建绘制面：与 headless 后端同一套语义（`auto` = 实测选更快的那条）。
  ///
  /// 为什么窗口后端的 `auto` 不直接信任 GPU：真实窗口下 `present()` 目前仍是
  /// "逐像素重排 + GDI BitBlt"（1.5x/1600x1000 实测约 1.25ms/帧）。GPU 画得快，
  /// 但这笔 CPU 拷贝会原样保留——所以判据仍应是**实测**，而不是想当然。
  struct SurfaceChoice {
    std::unique_ptr<raster::Surface> surface{};
    std::string name{"software"};
    std::string note{};
  };

  [[nodiscard]] static auto create_surface_for(int physical_width, int physical_height, float scale,
                                               const std::string& renderer) -> SurfaceChoice {
    SurfaceChoice choice;
    // 直接按物理尺寸建面：逻辑尺寸由 `物理 / scale` 折算（`Canvas` 已有这个契约）。
    // 反过来说"先定逻辑尺寸再乘 scale"就必然在某些尺寸上取不到目标物理值。
    const auto make_software = [&]() {
      return std::make_unique<raster::Canvas>(physical_width, physical_height, scale);
    };
    if (renderer == "software") {
      choice.note = "显式指定软件光栅器";
      choice.surface = make_software();
      return choice;
    }
    if (!raster::gpu::available()) {
      if (renderer == "gpu") {
        const auto probe = raster::gpu::probe();
        choice.note = std::format("GPU 不可用（{}），已回退软件", probe.error().message);
      } else {
        choice.note = "auto：GPU 不可用，选软件";
      }
      choice.surface = make_software();
      return choice;
    }
    auto gpu = raster::gpu::create_canvas(physical_width, physical_height, scale, {});
    if (!gpu.has_value()) {
      choice.note = std::format("GPU 画布创建失败（{}），已回退软件", gpu.error().message);
      choice.surface = make_software();
      return choice;
    }
    const auto info = raster::gpu::probe();
    const std::string gpu_label = info.has_value() ? info->adapter : std::string("D3D11");
    if (renderer == "gpu") {
      choice.surface = std::move(*gpu);
      choice.name = "gpu";
      choice.note = std::format("D3D11 · {} · {}", gpu_label, info.has_value() ? info->feature_level
                                                                               : std::string("-"));
      return choice;
    }
    // `auto` = **实测**：两条都建出来跑同一负载（与 headless 同一份判据）。
    // 注意这个数字**不含 present()**：GPU 画布走 DXGI swapchain（送显 ~0.03ms），
    // 软件画布走 GDI blit（~1.4ms）——两者差异由 renderer_note 报出，不混进基准。
    auto software = make_software();
    const double software_ms = shell::benchmark_surface(*software, 3);
    const double gpu_ms = shell::benchmark_surface(**gpu, 3);
    const bool pick_gpu = gpu_ms > 0.0 && gpu_ms < software_ms;
    choice.surface = pick_gpu ? std::move(*gpu) : std::move(software);
    choice.name = pick_gpu ? "gpu" : "software";
    choice.note = std::format("auto 实测（合成负载，不含呈现）：软件 {:.2f} ms vs GPU {} {:.2f} ms → 选{}",
                              software_ms, gpu_label, gpu_ms, pick_gpu ? "GPU" : "软件");
    return choice;
  }

  /// 画布像素 → DIB 像素并交给窗口。
  ///
  /// 画布内部是 `0xRRGGBBAA`（预乘），而 GDI 的 32bpp DIB 是内存序 B,G,R,A
  /// （即小端 `0xAARRGGBB`）——**必须逐像素重排**，否则红蓝互换（截图看着像"色调不对"）。
  void blit() {
    const std::span<const std::uint32_t> source = surface_->pixels();
    const std::size_t count =
        std::min(source.size(), static_cast<std::size_t>(surface_->physical_width()) *
                                    static_cast<std::size_t>(surface_->physical_height()));
    for (std::size_t index = 0; index < count; ++index) {
      const std::uint32_t pixel = source[index];
      const std::uint32_t red = (pixel >> 24U) & 0xFFU;
      const std::uint32_t green = (pixel >> 16U) & 0xFFU;
      const std::uint32_t blue = (pixel >> 8U) & 0xFFU;
      const std::uint32_t alpha = pixel & 0xFFU;
      dib_pixels_[index] = (alpha << 24U) | (red << 16U) | (green << 8U) | blue;
    }
    HDC window_dc = ::GetDC(window_);
    if (window_dc == nullptr) return;
    ::BitBlt(window_dc, 0, 0, surface_->physical_width(), surface_->physical_height(), memory_dc_, 0, 0,
             SRCCOPY);
    ::ReleaseDC(window_, window_dc);
  }

  /// 取空窗口消息并入队（`WM_PAINT` 等由系统消息驱动；应用循环调用本函数推进）。
  void pump_messages() {
    MSG message{};
    while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
      if (message.message == WM_QUIT) {
        close_requested_ = true;
        continue;
      }
      ::TranslateMessage(&message);
      ::DispatchMessageW(&message);
    }
  }

  /// 物理像素 → 逻辑像素（协议/脚本/UI 一律逻辑坐标）。
  [[nodiscard]] auto to_logical(LPARAM lparam) const -> math::Point {
    const auto physical_x = static_cast<short>(LOWORD(lparam));
    const auto physical_y = static_cast<short>(HIWORD(lparam));
    return math::Point{static_cast<float>(static_cast<double>(physical_x) / static_cast<double>(scale_)),
                       static_cast<float>(static_cast<double>(physical_y) / static_cast<double>(scale_))};
  }

  void push_mouse(ui::EventKind kind, LPARAM lparam, int button, int clicks) {
    ui::Event event;
    event.kind = kind;
    event.position = to_logical(lparam);
    event.button = button;
    event.click_count = clicks;
    events_.push_back(std::move(event));
  }

  void push_key(ui::EventKind kind) {
    ui::Event event;
    event.kind = kind;
    event.key = key_name(pending_key_);
    event.ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
    event.shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
    event.alt = (::GetKeyState(VK_MENU) & 0x8000) != 0;
    events_.push_back(std::move(event));
  }

  static auto CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
      -> LRESULT {
    Win32Backend* self = nullptr;
    if (message == WM_NCCREATE) {
      const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
      self = static_cast<Win32Backend*>(create->lpCreateParams);
      ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
      self->window_ = window;
    } else {
      self = reinterpret_cast<Win32Backend*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    if (self == nullptr) return ::DefWindowProcW(window, message, wparam, lparam);
    return self->handle_message(window, message, wparam, lparam);
  }

  auto handle_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam) -> LRESULT {
    switch (message) {
      case WM_CLOSE:
        // 关窗即请求退出：交由应用主循环收尾（进程内还有控制通道等资源要停）
        close_requested_ = true;
        return 0;
      case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;

      case WM_PAINT: {
        // 系统要求重绘：把离屏帧缓冲重新贴上去（我们不能只依赖 present 的节奏）
        PAINTSTRUCT paint{};
        ::BeginPaint(window, &paint);
        ::EndPaint(window, &paint);
        // 系统要求重绘：GPU 画布用 swapchain 重呈（blit 会触发 GPU→CPU 回读，白花几毫秒），
        // 软件画布才走 GDI blit。
        if (presenter_ != nullptr) {
          (void)presenter_->present(*surface_, surface_->physical_width(),
                                    surface_->physical_height());
        } else if (surface_ != nullptr && dib_ != nullptr) {
          blit();
        }
        return 0;
      }

      case WM_SIZE: {
        if (surface_ == nullptr) return 0;
        RECT client{};
        if (::GetClientRect(window, &client) == 0) return 0;
        // 尺寸变化由应用在下一帧读取 `logical_size()` 并同步视口（见 Application::tick）。
        // 一律走 `sync_buffers_to_client`：它保证画布物理尺寸**精确等于**客户区
        // （手写 `client / scale` 取整会差 1px → DXGI 缩放 → 糊）。
        (void)sync_buffers_to_client(scale_);
        return 0;
      }

      case WM_DPICHANGED: {
        const float new_scale = static_cast<float>(LOWORD(wparam)) / 96.0f;
        const auto* suggested = reinterpret_cast<const RECT*>(lparam);
        if (suggested != nullptr) {
          ::SetWindowPos(window, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        // 与 `set_device_scale` 同一口径：**客户区是唯一真源**。
        // 手写 `client / scale` 取整再回乘可能差 1px，那会让 DXGI 拉伸整块纹理（整屏发糊）。
        (void)sync_buffers_to_client(new_scale);
        return 0;
      }

      case WM_MOUSEMOVE:
        push_mouse(ui::EventKind::MouseMove, lparam, 0, 1);
        return 0;
      case WM_LBUTTONDOWN:
        ::SetCapture(window);
        push_mouse(ui::EventKind::MouseDown, lparam, 1, 1);
        return 0;
      case WM_LBUTTONUP:
        ::ReleaseCapture();
        // 先 MouseUp，再补一个 **Click**：按钮的激活发生在 `Element::activate()`，
        // 而它只在 `UiRoot::dispatch` 的 `Click` 分支里被调用——只发 Down/Up 的话
        // 按钮会"有焦点、有按压效果，但点了没反应"（实测就是这么发现的）。
        // 与协议 `input.mouse{kind:"click"}` 的事件序列保持一致（Down → Up → Click）。
        push_mouse(ui::EventKind::MouseUp, lparam, 1, 1);
        push_mouse(ui::EventKind::Click, lparam, 1, 1);
        return 0;
      case WM_LBUTTONDBLCLK:
        // 双击由系统识别（窗口类带 `CS_DBLCLKS`）：语义上第二次按下的 Click 计数为 2
        push_mouse(ui::EventKind::DoubleClick, lparam, 1, 2);
        push_mouse(ui::EventKind::Click, lparam, 1, 2);
        return 0;
      case WM_RBUTTONDOWN:
        push_mouse(ui::EventKind::MouseDown, lparam, 2, 1);
        return 0;
      case WM_RBUTTONUP:
        push_mouse(ui::EventKind::MouseUp, lparam, 2, 1);
        return 0;
      case WM_MBUTTONDOWN:
        push_mouse(ui::EventKind::MouseDown, lparam, 3, 1);
        return 0;
      case WM_MBUTTONUP:
        push_mouse(ui::EventKind::MouseUp, lparam, 3, 1);
        return 0;
      case WM_MOUSEWHEEL: {
        ui::Event event;
        event.kind = ui::EventKind::Wheel;
        // 滚轮消息的坐标是**屏幕**坐标，要转到客户区
        POINT point{static_cast<LONG>(static_cast<short>(LOWORD(lparam))),
                    static_cast<LONG>(static_cast<short>(HIWORD(lparam)))};
        ::ScreenToClient(window, &point);
        event.position = math::Point{static_cast<float>(static_cast<double>(point.x) / static_cast<double>(scale_)),
                                     static_cast<float>(static_cast<double>(point.y) / static_cast<double>(scale_))};
        // 一格 = WHEEL_DELTA(120) → 1.0；与 X11 后端的"行"口径一致
        event.wheel_delta = static_cast<float>(static_cast<short>(HIWORD(wparam))) / 120.0f;
        events_.push_back(std::move(event));
        return 0;
      }

      case WM_KEYDOWN:
      case WM_SYSKEYDOWN:
        pending_key_ = wparam;
        push_key(ui::EventKind::KeyDown);
        return message == WM_SYSKEYDOWN ? ::DefWindowProcW(window, message, wparam, lparam) : 0;
      case WM_KEYUP:
      case WM_SYSKEYUP:
        pending_key_ = wparam;
        push_key(ui::EventKind::KeyUp);
        return 0;
      case WM_CHAR: {
        ui::Event event;
        event.kind = ui::EventKind::TextInput;
        // 控制字符（\r \t \b 等）不是"文本输入"，由键事件表达
        if (wparam >= 32 && wparam != 127) {
          append_utf8(event.text, static_cast<char32_t>(wparam));
          events_.push_back(std::move(event));
        }
        return 0;
      }

      case WM_KILLFOCUS:
        events_.push_back(ui::Event{.kind = ui::EventKind::FocusOut});
        return 0;
      case WM_SETFOCUS:
        events_.push_back(ui::Event{.kind = ui::EventKind::FocusIn});
        return 0;

      case WM_ERASEBKGND:
        return 1;  // 我们整屏重绘，抑制系统擦背景（否则闪白）

      default: break;
    }
    return ::DefWindowProcW(window, message, wparam, lparam);
  }

  HWND window_{nullptr};
  HDC memory_dc_{nullptr};
  HBITMAP dib_{nullptr};
  std::uint32_t* dib_pixels_{nullptr};
  WPARAM pending_key_{0};
  /// 绘制面：软件或 GPU（由 `--renderer` / `auto` 决定）。
  /// 之前这里是具体 `Canvas` 且完全忽略 `options.renderer`——真实窗口下**永远走软件**，
  /// `--renderer gpu` 静默失效（实测绘制耗时 22.95ms vs 无头 GPU 的 1.31ms 才发现）。
  std::unique_ptr<raster::Surface> surface_{};
  /// DXGI 呈现器（仅 GPU 画布 + 真实窗口时存在）；为空时走 GDI blit。
  std::unique_ptr<raster::gpu::Presenter> presenter_{};
  std::uint64_t present_frames_{0};
  bool present_fallback_logged_{false};
  /// 请求的渲染器（来自 `--renderer`）。
  std::string renderer_{"auto"};
  std::string renderer_name_{"software"};
  std::string renderer_note_{};
  std::deque<ui::Event> events_{};
  std::string title_{};
  int logical_width_{1280};   ///< 逻辑尺寸（取整；用于"改窗口尺寸"时的换算与日志）
  int logical_height_{720};
  /// 精确逻辑尺寸 = 物理 / scale（供 `logical_size()` 汇报；布局按它排布）
  float logical_width_exact_{1280.0f};
  float logical_height_exact_{720.0f};
  float scale_{1.0f};
  std::uint64_t frames_{0};
  bool class_registered_{false};
  bool close_requested_{false};
};

}  // namespace

auto create_win32_backend() -> Result<std::unique_ptr<Backend>> {
  return std::unique_ptr<Backend>(std::make_unique<Win32Backend>());
}

}  // namespace st::shell

#endif  // defined(_WIN32)

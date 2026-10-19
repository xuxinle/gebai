#include "st/shell/shell.hpp"

#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <format>

#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/core/process.hpp"
#include "st/core/string.hpp"
#include "st/core/time.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/gpu.hpp"
#include "backend_util.hpp"

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

namespace st::shell {
namespace {

/// 离屏后端：无窗口、无显示服务依赖，像素结果与窗口模式一致。
class HeadlessBackend final : public Backend {
 public:
  [[nodiscard]] auto name() const noexcept -> std::string_view override { return "headless"; }
  [[nodiscard]] auto headless() const noexcept -> bool override { return true; }

  auto create_window(const WindowOptions& options) -> Status override {
    width_ = options.width > 0 ? options.width : 1280;
    height_ = options.height > 0 ? options.height : 720;
    // 工作区覆盖：只有无头需要它（真后端自己查显示器），见 `WindowOptions`。
    work_area_w_ = options.work_area_width;
    work_area_h_ = options.work_area_height;
    // 未显式指定 `scale` 时**跟随系统显示缩放**：内置通道（无头）与桌面窗口默认
    // 同一像素密度——这是“内置截图与实际运行效果一致”在 DPI 维度上的前提。
    // 显式值（`--scale`/`ST_SCALE`）完全接管，用于回归与复现。
    scale_ = options.scale > 0.0f ? options.scale : system_display_scale();
    title_ = options.title;
    // 把**解析后的** scale 传下去：`create_surface` 里的物理尺寸要从它算，
    // 继续传原始的 0 会让画布按 1x 建、而 device_scale 报 1.5（尺寸与声明不一致）。
    WindowOptions resolved = options;
    resolved.scale = scale_;
    // **无头缺省软件渲染器**（2026-10-14）：无头路径（CI / 截图 / 自动化）不需要 GPU，
    // 而 `auto` 的探测会真的建一个 D3D11 设备——实测代价 +53MB 进程内存、+55 线程、
    // +4.6GB 虚拟地址（NVIDIA 驱动进程内常驻），**任何像素产生之前**就付掉了。
    //
    // 为何不是改 `WindowOptions::renderer` 的声明缺省：那个字段是**共享**的，
    // 窗口后端（win32）缺省 `auto` 是**对的**（有窗口时“谁画得更快”确实得测）。
    // 因此按后端分叉：窗口后端不碰，无头后端只把**空串**（未指定）解析为 `software`；
    // 显式 `gpu` / `auto` 照旧探测（机器无 GPU 时 `auto` 会如实回退软件并说明原因）。
    //
    // ⚠ **判据必须是“空串”而不是“空串或 auto”**（2026-10-14 修）：
    // `AppOptions::renderer` 的声明缺省本就是 `auto`，于是“没传 --renderer”与
    // “显式传 --renderer auto”在字符串上**完全不可区分**——旧条件把两者一起改写为
    // `software`，直接违背了本段注释与下面 note 文案的承诺（“显式 auto 照旧探测”）。
    // 后果不是“少了点功能”而是**量错了机器**：`--renderer auto` 的无头 GPU 基准
    // 会静默跑在软件路径上（实测 1.44 ms → 11.49 ms），差点被当成 8.8 倍性能回归。
    // 修法：`AppOptions::renderer` 的缺省改为**空串**（= 未指定），于是三态可区分：
    // 空 = 无头缺省软件；`auto` = 照旧探测；`gpu`/`software` = 按名。
    const std::string requested_renderer =
        options.renderer.empty() ? std::string("software") : options.renderer;
    const bool defaulted_to_software = requested_renderer != options.renderer;
    detail::SurfaceChoice choice = detail::create_surface(resolved, requested_renderer);
    if (choice.surface == nullptr) return unexpected(ErrorCode::Unsupported, "创建绘制面失败");
    if (defaulted_to_software) {
      // 理由要**如实**说：这是无头缺省，不是实测选出来的（与 `auto` 的实测理由
      // 混为一谈会让"为什么无头没探测 GPU"变成谜案——排查的人会去翻微基准）。
      choice.note = std::format("无头缺省软件光栅器（不探测 GPU；显式 --renderer gpu/auto 才探测）：{}",
                                choice.note);
    }
    surface_ = std::move(choice.surface);
    renderer_name_ = std::move(choice.name);
    renderer_note_ = std::move(choice.note);
    surface_->clear(math::Color{0, 0, 0, 0});
    return ok();
  }

  void present() override { ++frames_; }

  [[nodiscard]] auto poll_event() -> std::optional<ui::Event> override { return std::nullopt; }

  void set_title(std::string_view title) override { title_ = std::string(title); }

  [[nodiscard]] auto clipboard_text() -> Result<std::string> override {
    // 进程内剪贴板（2026-10-12 补齐读侧）：`set_clipboard_text` 一直在存 `clipboard_`，
    // 但读侧报 Unsupported——于是 headless 下「复制→粘贴」永远不通（写进去的
    // 东西读不出来）。与 CodeEditor 的进程内剪贴板同姿态：无头与有窗口行为一致。
    return clipboard_;
  }

  [[nodiscard]] auto set_clipboard_text(std::string_view text) -> Status override {
    clipboard_ = std::string(text);
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
    return math::Size{static_cast<float>(width_), static_cast<float>(height_)};
  }
  /// 无头没有真显示器；但为了能**复现“小屏 + 高 DPI”**的场景，这里把逻辑尺寸
  /// 当作工作区（于是 `work_area_override` 能驱动真正的夹取逻辑），
  /// 而 `fit_window_to_work_area` 按需重建面大小。
  [[nodiscard]] auto work_area_logical() const noexcept -> math::Size override {
    // 未设覆盖 ⇒ “窗口就是全部可用空间”（不改变既有行为）。
    if (work_area_w_ <= 0 || work_area_h_ <= 0) return logical_size();
    return math::Size{static_cast<float>(work_area_w_), static_cast<float>(work_area_h_)};
  }
  [[nodiscard]] auto min_logical_size() const noexcept -> math::Size override {
    return math::Size{640.0f, 400.0f};
  }
  auto fit_window_to_work_area() -> Status override {
    const math::Size work = work_area_logical();
    const math::Size min_size = min_logical_size();
    if (work.width <= 0.0f || work.height <= 0.0f) {
      return unexpected(ErrorCode::Unsupported, "未设置工作区");
    }
    const float limit_w = std::max(min_size.width, work.width * 0.92f);
    const float limit_h = std::max(min_size.height, work.height * 0.92f);
    const int target_w = static_cast<int>(std::lround(std::min(static_cast<float>(width_), limit_w)));
    const int target_h = static_cast<int>(std::lround(std::min(static_cast<float>(height_), limit_h)));
    if (target_w == width_ && target_h == height_) return ok();
    width_ = std::max(1, target_w);
    height_ = std::max(1, target_h);
    // 重建面（与 set_device_scale 同一条路径）。
    WindowOptions options;
    options.width = width_;
    options.height = height_;
    options.scale = scale_;
    options.renderer = renderer_name_;
    detail::SurfaceChoice choice = detail::create_surface(options, renderer_name_);
    if (choice.surface == nullptr) return unexpected(ErrorCode::Unsupported, "重建绘制面失败");
    surface_ = std::move(choice.surface);
    return ok();
  }
  /// 工作区尺寸（由 Application 通过 WindowOptions 传入；0 = 等同逻辑尺寸）。
  void set_work_area(int w, int h) noexcept {
    work_area_w_ = w > 0 ? w : width_;
    work_area_h_ = h > 0 ? h : height_;
  }

  auto set_device_scale(float scale) -> Status override {
    if (scale <= 0.0f) return unexpected(ErrorCode::Invalid, "DPI 缩放必须为正数");
    scale_ = scale;
    if (surface_ == nullptr) return ok();
    // 换 DPI 就是换整块面（尺寸变了）；渲染器选择保持不变。
    WindowOptions options;
    options.width = width_;
    options.height = height_;
    options.scale = scale_;
    options.renderer = renderer_name_;
    detail::SurfaceChoice choice = detail::create_surface(options, renderer_name_);
    if (choice.surface == nullptr) return unexpected(ErrorCode::Unsupported, "重建绘制面失败");
    surface_ = std::move(choice.surface);
    renderer_note_ = std::move(choice.note);
    surface_->clear(math::Color{0, 0, 0, 0});
    return ok();
  }

 private:
  int width_{1280};
  int height_{720};
  /// 工作区尺寸（无头模拟“屏幕多大”；0 表未设）——见 `work_area_logical`。
  int work_area_w_{0};
  int work_area_h_{0};
  float scale_{1.0f};
  std::string title_{};
  std::string clipboard_{};
  std::uint64_t frames_{0};
  std::unique_ptr<raster::Surface> surface_{};
  std::string renderer_name_{"software"};
  std::string renderer_note_{};
};

}  // namespace

namespace detail {

/// 运行时库探测（**不产生链接期依赖**）：候选名用 `:` 分隔（如
/// `libX11.so.6:libX11.so`），返回第一个可加载的名字；都不行返回空串。
///
/// 先试带版本号的 soname（发行版安装的形态），再试无版本的开发名。
/// 两者都探不到就如实返回空串，调用方据此报"未探测到"，而不是链接期炸掉。
///
/// 落点说明：本文件是 L14 的**已登记豁免点**（`CONVENTIONS.md` §8：「需 `dlopen`
/// 做运行时后端探测——而这正是 §10 第 1 条『平台后端一律运行时探测（不产生链接期依赖）』
/// 的实现处」）。四个后端共用这一份，避免"探测口径分家"（两处对同一台机器给出
/// 不同答案是最难查的一类不一致）。
[[nodiscard]] auto library_available(std::string_view candidates) -> std::string {
#if defined(_WIN32)
  // Windows 平台库由系统保证存在，用系统目录下的存在性判断即可
  const std::string name(candidates.substr(0, candidates.find(':')));
  const std::string path = fs::join("C:/Windows/System32", name);
  return fs::exists(path) ? name : std::string{};
#else
  for (const auto candidate : st::split(candidates, ':')) {
    if (candidate.empty()) continue;
    void* handle = ::dlopen(std::string(candidate).c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (handle != nullptr) {
      ::dlclose(handle);
      return std::string(candidate);
    }
  }
  return {};
#endif
}

}  // namespace detail

auto has_display() noexcept -> bool {
#if defined(_WIN32)
  // Windows 有窗口系统是常态（无 DISPLAY 概念）；真正的"无头"由 `--headless` 显式指定。
  return true;
#else
  const auto check = [](const char* name) {
    const auto value = fs::read_env(name);
    return value.has_value() && !value->empty();
  };
  return check("DISPLAY") || check("WAYLAND_DISPLAY");
#endif
}

auto probe_backend() -> std::string {
  if (!has_display()) return "headless";
#if defined(_WIN32)
  return "win32";
#else
  if (!detail::library_available("libX11.so.6:libX11.so").empty()) return "x11";
  if (!detail::library_available("libwayland-client.so.0").empty()) return "wayland";
  return "headless";
#endif
}

auto system_display_scale() noexcept -> float {
#if defined(_WIN32)
  return win32_display_scale();
#else
  // 无“系统显示缩放”概念：1.0（与窗口后端在无 DPI 概念平台上的默认一致）。
  return 1.0f;
#endif
}

auto resolve_present_area(math::IntRect region, float scale, int width, int height) noexcept
    -> math::IntRect {
  const auto clamp_axis = [](int value, int limit) { return std::clamp(value, 0, limit); };
  if (width <= 0 || height <= 0) return math::IntRect{};
  if (region.is_empty()) return math::IntRect{0, 0, width, height};
  const double factor = scale > 0.0f ? static_cast<double>(scale) : 1.0;
  // 左/上向下取整、右/下向上取整：**向外取整**——宁可多发一列也不能漏一条边缘
  // （`round` 会把 100.4 的右缘收成 100，漏掉半像素，表现为“动一下边缘就缺一条”）。
  const int left = clamp_axis(static_cast<int>(std::floor(static_cast<double>(region.x) * factor)),
                              width);
  const int top = clamp_axis(static_cast<int>(std::floor(static_cast<double>(region.y) * factor)),
                             height);
  const int right = clamp_axis(
      static_cast<int>(std::ceil(static_cast<double>(region.x + region.width) * factor)), width);
  const int bottom = clamp_axis(
      static_cast<int>(std::ceil(static_cast<double>(region.y + region.height) * factor)), height);
  if (right <= left || bottom <= top) return math::IntRect{};
  return math::IntRect{left, top, right - left, bottom - top};
}

auto create_backend(std::string_view name) -> Result<std::unique_ptr<Backend>> {
  const std::string requested = name.empty() ? probe_backend() : std::string(name);
  if (requested == "headless") {
    return std::unique_ptr<Backend>(std::make_unique<HeadlessBackend>());
  }
  if (requested == "x11") {
#if defined(_WIN32)
    return unexpected(ErrorCode::Unsupported, "x11 后端仅在 POSIX 宿主可用");
#else
    if (!has_display()) {
      return unexpected(ErrorCode::Unsupported, "存在 libX11 但无 DISPLAY 环境变量（无桌面会话）");
    }
    return create_x11_backend();
#endif
  }
  if (requested == "wayland") {
#if defined(_WIN32)
    return unexpected(ErrorCode::Unsupported, "wayland 后端仅在 POSIX 宿主可用");
#else
    return create_wayland_backend();
#endif
  }
  if (requested == "win32") {
#if defined(_WIN32)
    return create_win32_backend();
#else
    return unexpected(ErrorCode::Unsupported, "win32 后端仅在 Windows 宿主可用");
#endif
  }
  return unexpected(ErrorCode::Invalid, std::format("未知后端: {}", requested));
}

/// 公共入口（声明见 `shell.hpp`）：四个后端共用的"更快"判据。
/// 实现住 `backend_util.cpp`（单一真源），这里只做转发——于是"更快怎么算"
/// 只定义一处，窗口与离屏、四个后端不会得出不同结论。
[[nodiscard]] auto benchmark_surface(raster::Surface& target, int runs) -> double {
  return detail::benchmark_surface_impl(target, runs);
}

}  // namespace st::shell

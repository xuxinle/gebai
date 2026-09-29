#include "st/shell/shell.hpp"

#include <cstdlib>
#include <format>

#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/core/process.hpp"
#include "st/core/string.hpp"

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

namespace st::shell {
namespace {

/// 离屏后端：无窗口、无显示服务依赖，像素结果与窗口模式一致（软件光栅器是唯一真相源）。
class HeadlessBackend final : public Backend {
 public:
  [[nodiscard]] auto name() const noexcept -> std::string_view override { return "headless"; }
  [[nodiscard]] auto headless() const noexcept -> bool override { return true; }

  auto create_window(const WindowOptions& options) -> Status override {
    width_ = options.width > 0 ? options.width : 1280;
    height_ = options.height > 0 ? options.height : 720;
    scale_ = options.scale > 0.0f ? options.scale : 1.0f;
    title_ = options.title;
    canvas_ = std::make_unique<raster::Canvas>(
        raster::Canvas::for_logical_size(width_, height_, scale_));
    canvas_->clear(math::Color{0, 0, 0, 0});
    return ok();
  }

  void present() override { ++frames_; }

  [[nodiscard]] auto poll_event() -> std::optional<ui::Event> override { return std::nullopt; }

  void set_title(std::string_view title) override { title_ = std::string(title); }

  [[nodiscard]] auto clipboard_text() -> Result<std::string> override {
    return unexpected(ErrorCode::Unsupported, "headless 后端无系统剪贴板，请经控制通道 input.text 注入文本");
  }

  [[nodiscard]] auto set_clipboard_text(std::string_view text) -> Status override {
    clipboard_ = std::string(text);
    return ok();
  }

  [[nodiscard]] auto framebuffer() -> raster::Canvas& override { return *canvas_; }
  [[nodiscard]] auto frame_count() const noexcept -> std::uint64_t override { return frames_; }
  [[nodiscard]] auto device_scale() const noexcept -> float override { return scale_; }
  [[nodiscard]] auto logical_size() const noexcept -> math::Size override {
    return math::Size{static_cast<float>(width_), static_cast<float>(height_)};
  }

  auto set_device_scale(float scale) -> Status override {
    if (scale <= 0.0f) return unexpected(ErrorCode::Invalid, "DPI 缩放必须为正数");
    if (canvas_ == nullptr) {
      scale_ = scale;
      return ok();
    }
    scale_ = scale;
    *canvas_ = raster::Canvas::for_logical_size(width_, height_, scale_);
    canvas_->clear(math::Color{0, 0, 0, 0});
    return ok();
  }

 private:
  int width_{1280};
  int height_{720};
  float scale_{1.0f};
  std::string title_{};
  std::string clipboard_{};
  std::uint64_t frames_{0};
  std::unique_ptr<raster::Canvas> canvas_{};
};

/// 运行时库探测（不产生链接期依赖）。
[[nodiscard]] auto library_available(std::string_view candidates) -> std::string {
#if defined(_WIN32)
  // Windows 平台库由系统保证存在，直接用 LOAD_LIBRARY_SEARCH_SYSTEM32 语义判断
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

/// 平台后端桩：诚实报告"已探测到/未探测到 + 本期实现范围"。
class PlatformStubBackend final : public Backend {
 public:
  PlatformStubBackend(std::string name, std::string library) noexcept
      : name_(std::move(name)), library_(std::move(library)) {}

  [[nodiscard]] auto name() const noexcept -> std::string_view override { return name_; }
  [[nodiscard]] auto headless() const noexcept -> bool override { return false; }

  auto create_window(const WindowOptions& options) -> Status override {
    (void)options;
    return unexpected(ErrorCode::Unsupported,
                      std::format("{} 后端（已探测到 {}）的窗口实现规划在 v0.2；"
                                  "当前请使用 headless 模式：控制通道可完成全部开发与验证",
                                  name_, library_));
  }

  void present() override {}
  [[nodiscard]] auto poll_event() -> std::optional<ui::Event> override { return std::nullopt; }
  void set_title(std::string_view title) override { (void)title; }
  [[nodiscard]] auto clipboard_text() -> Result<std::string> override {
    return unexpected(ErrorCode::Unsupported, "平台后端未实现");
  }
  [[nodiscard]] auto set_clipboard_text(std::string_view text) -> Status override {
    (void)text;
    return unexpected(ErrorCode::Unsupported, "平台后端未实现");
  }
  [[nodiscard]] auto framebuffer() -> raster::Canvas& override { return canvas_; }
  [[nodiscard]] auto frame_count() const noexcept -> std::uint64_t override { return 0; }
  [[nodiscard]] auto device_scale() const noexcept -> float override { return 1.0f; }
  [[nodiscard]] auto logical_size() const noexcept -> math::Size override {
    return math::Size{1.0f, 1.0f};
  }
  auto set_device_scale(float scale) -> Status override {
    (void)scale;
    return unexpected(ErrorCode::Unsupported, "平台后端未实现（v0.2）");
  }

 private:
  std::string name_{};
  std::string library_{};
  raster::Canvas canvas_{1, 1};
};

}  // namespace

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
  if (!library_available("libX11.so.6:libX11.so").empty()) return "x11";
  if (!library_available("libwayland-client.so.0").empty()) return "wayland";
  return "headless";
#endif
}

auto create_backend(std::string_view name) -> Result<std::unique_ptr<Backend>> {
  const std::string requested = name.empty() ? probe_backend() : std::string(name);
  if (requested == "headless") {
    return std::unique_ptr<Backend>(std::make_unique<HeadlessBackend>());
  }
  if (requested == "x11") {
    const std::string library = library_available("libX11.so.6:libX11.so");
    if (library.empty()) {
      return unexpected(ErrorCode::Unsupported, "未探测到 libX11（无 X 显示服务）");
    }
    if (!has_display()) {
      return unexpected(ErrorCode::Unsupported, "存在 libX11 但无 DISPLAY 环境变量（无桌面会话）");
    }
    auto backend = std::unique_ptr<Backend>(std::make_unique<PlatformStubBackend>("x11", library));
    return backend;
  }
  if (requested == "wayland") {
    const std::string library = library_available("libwayland-client.so.0");
    if (library.empty()) {
      return unexpected(ErrorCode::Unsupported, "未探测到 libwayland-client");
    }
    return std::unique_ptr<Backend>(std::make_unique<PlatformStubBackend>("wayland", library));
  }
  if (requested == "win32") {
#if defined(_WIN32)
    return std::unique_ptr<Backend>(std::make_unique<PlatformStubBackend>("win32", "user32.dll"));
#else
    return unexpected(ErrorCode::Unsupported, "win32 后端仅在 Windows 宿主可用");
#endif
  }
  return unexpected(ErrorCode::Invalid, std::format("未知后端: {}", requested));
}

}  // namespace st::shell

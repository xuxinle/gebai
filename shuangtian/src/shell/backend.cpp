#include "st/shell/shell.hpp"

#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <format>

#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/core/process.hpp"
#include "st/core/string.hpp"
#include "st/core/time.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/gpu.hpp"

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

namespace st::shell {
namespace {

/// 建绘制面：按 `renderer` 选择软件或 GPU，失败**如实回退**并记下原因。
///
/// 为什么把选择放在后端而不是应用：面（surface）由后端持有，应用只拿 `Surface&`；
/// 让后端决定它是什么类型，应用与控制通道都不需要知道这一帧是谁画的。
///
/// `auto` 的语义是"**取更快的那条**"，但渲染器的优劣与机器强相关（GPU 弱、驱动差、
/// 或呈现路径仍需 CPU 拷贝时，软件反而更快），所以这里不预设答案：
/// 由调用方（`Application`）在能测到真实帧耗时时做实测选择，测不到才用下面的保守默认。
/// 合成负载微基准（定义见下：`create_surface` 的 `auto` 分支要用它）。
[[nodiscard]] auto benchmark_surface_impl(raster::Surface& target, int runs) -> double;

struct SurfaceChoice {
  std::unique_ptr<raster::Surface> surface{};
  std::string name{"software"};
  std::string note{};
};

[[nodiscard]] auto create_surface(const WindowOptions& options, std::string_view renderer)
    -> SurfaceChoice {
  SurfaceChoice choice;
  const int physical_width = static_cast<int>(std::lround(
      static_cast<double>(options.width > 0 ? options.width : 1280) *
      static_cast<double>(options.scale > 0.0f ? options.scale : 1.0f)));
  const int physical_height = static_cast<int>(std::lround(
      static_cast<double>(options.height > 0 ? options.height : 720) *
      static_cast<double>(options.scale > 0.0f ? options.scale : 1.0f)));
  const float scale = options.scale > 0.0f ? options.scale : 1.0f;

  // `auto` = 实测：两条都建出来，各跑一遍同样的负载，取更快的那条。
  // 不做"GPU 优先"的硬编码——GPU 弱、驱动差、或呈现仍需 CPU 拷贝时，软件反而更快。
  if (renderer == "auto") {
    auto software = std::make_unique<raster::Canvas>(
        raster::Canvas::for_logical_size(options.width > 0 ? options.width : 1280,
                                         options.height > 0 ? options.height : 720, scale));
    const double software_ms = benchmark_surface_impl(*software, 3);
    std::unique_ptr<raster::Surface> gpu{};
    std::string gpu_label{"不可用"};
    if (raster::gpu::available()) {
      if (auto created = raster::gpu::create_canvas(physical_width, physical_height, scale, {});
          created.has_value()) {
        gpu = std::move(*created);
        const auto info = raster::gpu::probe();
        gpu_label = info.has_value() ? info->adapter : std::string("D3D11");
      }
    }
    const double gpu_ms = gpu != nullptr ? benchmark_surface_impl(*gpu, 3) : 0.0;
    const bool pick_gpu = gpu != nullptr && gpu_ms > 0.0 && gpu_ms < software_ms;
    // ⚠ 必须在 move **之前**记下"有没有 GPU"：`std::move(gpu)` 之后那个指针必然为空，
    // 拿它去做判断会得到相反的分支（曾因此出现"名字说 gpu、理由说 GPU 不可用"的自相矛盾）。
    const bool had_gpu = gpu != nullptr;
    choice.surface = pick_gpu ? std::move(gpu) : std::move(software);
    choice.name = pick_gpu ? "gpu" : "software";
    choice.note = !had_gpu
                      ? std::format("auto：GPU 不可用，选软件（实测软件 {:.2f} ms）", software_ms)
                      : std::format("auto 实测（合成负载，不含呈现）：软件 {:.2f} ms vs GPU {} {:.2f} ms → 选{}",
                                    software_ms, gpu_label, gpu_ms, pick_gpu ? "GPU" : "软件");
    return choice;
  }

  const bool want_gpu = renderer == "gpu";
  if (want_gpu) {
    if (!raster::gpu::available()) {
      const auto probe = raster::gpu::probe();
      choice.note = std::format("GPU 不可用（{}），已回退软件光栅器",
                                probe.error().message);
    } else {
      auto created = raster::gpu::create_canvas(physical_width, physical_height, scale, {});
      if (created.has_value()) {
        choice.surface = std::move(*created);
        choice.name = "gpu";
        const auto info = raster::gpu::probe();
        choice.note = info.has_value()
                          ? std::format("D3D11 · {} · {}", info->adapter, info->feature_level)
                          : std::string("D3D11");
        return choice;
      }
      choice.note = std::format("GPU 画布创建失败（{}），已回退软件光栅器",
                                created.error().message);
    }
  } else if (renderer == "software") {
    choice.note = "显式指定软件光栅器";
  }
  choice.surface = std::make_unique<raster::Canvas>(
      raster::Canvas::for_logical_size(options.width > 0 ? options.width : 1280,
                                       options.height > 0 ? options.height : 720, scale));
  return choice;
}

/// 合成负载：真实画布尺寸上跑一组有代表性的原语（圆角矩形 / 渐变 / 覆盖率遮罩），
/// 取中位数。公共入口 `benchmark_surface` 在文件末尾转发到这里。
[[nodiscard]] auto benchmark_surface_impl(raster::Surface& target, int runs) -> double {
  const float width = static_cast<float>(target.width());
  const float height = static_cast<float>(target.height());
  if (width < 32.0f || height < 32.0f) return 0.0;
  const raster::Paint solid = raster::Paint::solid(math::Color{0x40, 0x80, 0xFF, 0xFF});
  const raster::Paint gradient = raster::Paint::with_gradient(raster::Gradient::linear(
      math::Point{0.0f, 0.0f}, math::Point{width, 0.0f},
      {{0.0f, math::Color{0xFF, 0x00, 0x80, 0xFF}}, {1.0f, math::Color{0x00, 0x80, 0xFF, 0xFF}}}));
  // 小覆盖率位图（模拟字形遮罩：上传 + 采样，这是文字路径的成本特征）
  std::vector<float> mask(16U * 16U, 0.5f);

  std::vector<double> samples;
  samples.reserve(static_cast<std::size_t>(runs));
  for (int run = 0; run < runs; ++run) {
    const std::int64_t start = st::time::now_ns();
    target.clear(math::Color{0x10, 0x10, 0x18, 0xFF});
    for (int index = 0; index < 400; ++index) {
      const float x = static_cast<float>(index % 20) * (width / 20.0f);
      const float y = static_cast<float>(index / 20) * (height / 20.0f);
      target.fill_rect(math::Rect{x + 1.0f, y + 1.0f, width / 25.0f, height / 25.0f}, solid, 4.0f);
    }
    for (int index = 0; index < 40; ++index) {
      target.fill_rect(math::Rect{0.0f, static_cast<float>(index) * (height / 40.0f), width,
                                  height / 40.0f},
                       gradient);
    }
    for (int index = 0; index < 200; ++index) {
      target.blend_coverage_bitmap(index % 20 * 17, index / 20 * 17, mask, 16, 16, solid, 0.8f,
                                   raster::BlendMode::SrcOver);
    }
    samples.push_back(static_cast<double>(st::time::now_ns() - start) / 1'000'000.0);
  }
  std::sort(samples.begin(), samples.end());
  return samples[samples.size() / 2U];  // 中位数：忽略冷启动与偶发抖动
}

/// 离屏后端：无窗口、无显示服务依赖，像素结果与窗口模式一致。
class HeadlessBackend final : public Backend {
 public:
  [[nodiscard]] auto name() const noexcept -> std::string_view override { return "headless"; }
  [[nodiscard]] auto headless() const noexcept -> bool override { return true; }

  auto create_window(const WindowOptions& options) -> Status override {
    width_ = options.width > 0 ? options.width : 1280;
    height_ = options.height > 0 ? options.height : 720;
    scale_ = options.scale > 0.0f ? options.scale : 1.0f;
    title_ = options.title;
    SurfaceChoice choice = create_surface(options, options.renderer);
    if (choice.surface == nullptr) return unexpected(ErrorCode::Unsupported, "创建绘制面失败");
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
    return unexpected(ErrorCode::Unsupported, "headless 后端无系统剪贴板，请经控制通道 input.text 注入文本");
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
    SurfaceChoice choice = create_surface(options, renderer_name_);
    if (choice.surface == nullptr) return unexpected(ErrorCode::Unsupported, "重建绘制面失败");
    surface_ = std::move(choice.surface);
    renderer_note_ = std::move(choice.note);
    surface_->clear(math::Color{0, 0, 0, 0});
    return ok();
  }

 private:
  int width_{1280};
  int height_{720};
  float scale_{1.0f};
  std::string title_{};
  std::string clipboard_{};
  std::uint64_t frames_{0};
  std::unique_ptr<raster::Surface> surface_{};
  std::string renderer_name_{"software"};
  std::string renderer_note_{};
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
                      std::format("{} 后端（已探测到 {}）的窗口实现尚未提供；"
                                  "请使用 headless 模式：控制通道可完成全部开发与验证",
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
  [[nodiscard]] auto framebuffer() -> raster::Surface& override { return canvas_; }
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
    return create_win32_backend();
#else
    return unexpected(ErrorCode::Unsupported, "win32 后端仅在 Windows 宿主可用");
#endif
  }
  return unexpected(ErrorCode::Invalid, std::format("未知后端: {}", requested));
}

/// 公共入口（声明见 shell.hpp）：两个后端共用的"更快"判据。
/// 实现留在匿名命名空间里（`benchmark_surface_impl`），这里只做转发——
/// 于是"更快怎么算"只定义一处，窗口与离屏不会得出不同结论。
[[nodiscard]] auto benchmark_surface(raster::Surface& target, int runs) -> double {
  return benchmark_surface_impl(target, runs);
}

}  // namespace st::shell

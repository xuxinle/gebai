#include "st/app/app.hpp"

#include "st/app/text_port.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <thread>

#include "st/codec/png.hpp"
#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/core/process.hpp"
#include "st/core/string.hpp"
#include "st/core/time.hpp"
#include "st/text/text.hpp"

namespace st::app {
namespace {

}  // namespace

struct Application::Impl {
  shell::Backend* backend{nullptr};
  std::unique_ptr<shell::Backend> backend_holder{};
  std::unique_ptr<st::text::FontStack> fonts{};
  std::unique_ptr<st::text::TextRenderer> renderer{};
  std::unique_ptr<RendererTextPort> text_port{};
  std::unique_ptr<control::Server> server{};
  /// 脚本宿主（仅 `AppOptions::enable_script` 时创建；协议层通过 `Application::script()` 取用）
  std::unique_ptr<ui::ScriptHost> script{};
  std::vector<double> frame_times{};
  std::vector<std::string> log_lines{};
  std::uint64_t frames{0};
  float device_scale{1.0f};
  double last_frame_ms{0.0};
  std::int64_t started_ms{0};
  bool quit{false};
  bool repaint{true};
};

Application::Application(std::string name, std::string version, AppOptions options)
    : impl_(std::make_unique<Impl>()), name_(std::move(name)), version_(std::move(version)),
      options_(std::move(options)) {
  log::set_level(log::level_from_name(options_.log_level));
  root_.set_theme(ui::Theme::by_mode(options_.theme));
  if (!options_.screenshot_dir.empty()) {
    (void)fs::create_directories(options_.screenshot_dir);
  }
  log::add_listener([this](log::Level level, std::string_view message) {
    impl_->log_lines.push_back(std::format("[{}] {}", to_string(level), message));
    if (impl_->log_lines.size() > 512) impl_->log_lines.erase(impl_->log_lines.begin());
  });
}

auto Application::backend_name() const -> std::string_view {
  return impl_->backend != nullptr ? impl_->backend->name() : std::string_view{"none"};
}

auto Application::headless() const -> bool {
  return impl_->backend != nullptr ? impl_->backend->headless() : options_.headless;
}

auto Application::script() -> ui::ScriptHost* { return impl_->script.get(); }

auto Application::control_port() const noexcept -> std::uint16_t {
  return impl_->server != nullptr ? impl_->server->port() : 0;
}

void Application::request_quit() { impl_->quit = true; }

void Application::request_repaint() {
  impl_->repaint = true;
  root_.mark_dirty_all();
}

void Application::set_theme_mode(ui::ThemeMode mode) {
  root_.set_theme(ui::Theme::by_mode(mode));
  options_.theme = mode;
}

auto Application::quit_requested() const noexcept -> bool { return impl_->quit; }

auto Application::log_lines(std::size_t limit) const -> std::vector<std::string> {
  std::vector<std::string> out;
  const std::size_t count = std::min(limit, impl_->log_lines.size());
  const std::size_t begin = impl_->log_lines.size() - count;
  for (std::size_t index = begin; index < impl_->log_lines.size(); ++index) {
    out.push_back(impl_->log_lines[index]);
  }
  return out;
}

auto Application::metrics() const -> control::Metrics {
  control::Metrics metrics;
  metrics.frames = impl_->frames;
  metrics.last_frame_ms = impl_->last_frame_ms;
  if (!impl_->frame_times.empty()) {
    std::vector<double> sorted = impl_->frame_times;
    std::ranges::sort(sorted);
    const auto percentile = [&sorted](double ratio) -> double {
      const auto index = static_cast<std::size_t>(
          std::min(static_cast<double>(sorted.size() - 1), ratio * static_cast<double>(sorted.size() - 1)));
      return sorted[index];
    };
    metrics.frame_p50_ms = percentile(0.5);
    metrics.frame_p95_ms = percentile(0.95);
  }
  metrics.uptime_ms = time::now_ms() - impl_->started_ms;
  metrics.backend = std::string(backend_name());
  metrics.headless = headless();
  metrics.device_scale = impl_->device_scale;
  if (impl_->backend != nullptr) {
    metrics.physical_width = impl_->backend->framebuffer().physical_width();
    metrics.physical_height = impl_->backend->framebuffer().physical_height();
  }
  const auto count_nodes = [](auto&& self, const ui::VisualNode& node) -> std::size_t {
    std::size_t total = 1;
    for (const auto& child : node.children) total += self(self, child);
    return total;
  };
  metrics.nodes = count_nodes(count_nodes, root_.visual_tree());
  metrics.requests = 0;
  return metrics;
}

auto Application::device_scale() const -> float { return impl_->device_scale; }

auto Application::set_device_scale(float scale) -> Status {
  if (impl_->backend == nullptr) return unexpected(ErrorCode::Invalid, "应用未启动");
  if (scale <= 0.0f) return unexpected(ErrorCode::Invalid, "DPI 缩放必须为正数");
  if (auto status = impl_->backend->set_device_scale(scale); !status) {
    return forward_error(status.error());
  }
  impl_->device_scale = scale;
  root_.mark_dirty_all();
  log::info("DPI 缩放已切换为 {}（物理 {}×{}）", scale,
            impl_->backend->framebuffer().physical_width(),
            impl_->backend->framebuffer().physical_height());
  return ok();
}

auto Application::capture_png(math::IntRect region) -> Result<std::vector<std::uint8_t>> {
  if (impl_->backend == nullptr) {
    return unexpected(ErrorCode::Invalid, "应用未启动（无帧缓冲）");
  }
  raster::Canvas& canvas = impl_->backend->framebuffer();
  // **截图是物理像素口径**（`DESIGN.md` §6：capture 按物理分辨率出图）：
  // - 默认区域 = 整个物理缓冲（不是逻辑尺寸——按逻辑取会把 2x 屏截成左上 1/4）；
  // - 显式 region 是**逻辑坐标**（与协议里其它坐标一致），这里换算到物理像素后取像素。
  const math::IntRect physical_bounds{0, 0, canvas.physical_width(), canvas.physical_height()};
  math::IntRect area = physical_bounds;
  if (!region.is_empty()) {
    area = canvas.to_physical(math::Rect{static_cast<float>(region.x), static_cast<float>(region.y),
                                         static_cast<float>(region.width),
                                         static_cast<float>(region.height)});
  }
  area = area.intersect(physical_bounds);
  if (area.is_empty()) return unexpected(ErrorCode::Invalid, "截图区域为空或超出画布");

  codec::PngImage image;
  image.width = static_cast<std::uint32_t>(area.width);
  image.height = static_cast<std::uint32_t>(area.height);
  image.rgba.resize(static_cast<std::size_t>(area.width) * static_cast<std::size_t>(area.height) * 4U);
  std::size_t index = 0;
  for (int y = 0; y < area.height; ++y) {
    for (int x = 0; x < area.width; ++x) {
      const math::Color color = canvas.pixel_at(area.x + x, area.y + y);
      image.rgba[index] = color.r;
      image.rgba[index + 1] = color.g;
      image.rgba[index + 2] = color.b;
      image.rgba[index + 3] = color.a;
      index += 4;
    }
  }
  auto encoded = codec::png_encode(image, 6);
  if (!encoded) return forward_error(encoded.error());
  return encoded;
}

auto Application::capture_to_file(std::string_view path, math::IntRect region)
    -> Result<std::string> {
  auto png = capture_png(region);
  if (!png) return forward_error(png.error());
  std::string target(path);
  if (target.empty()) {
    const std::string directory = options_.screenshot_dir.empty()
                                      ? fs::join(fs::temp_dir(), "shuangtian-shots")
                                      : options_.screenshot_dir;
    if (auto status = fs::create_directories(directory); !status) return forward_error(status.error());
    target = fs::join(directory, std::format("shot-{}-{:03d}.png", time::unix_ms(),
                                             impl_->frames % 1000));
  }
  if (auto status = fs::write_bytes(target, std::span<const std::uint8_t>(*png)); !status) {
    return forward_error(status.error());
  }
  log::info("截图已保存 {}", target);
  return target;
}

auto Application::start() -> Status {
  if (started_) return ok();

  auto backend = shell::create_backend(options_.backend);
  if (!backend) {
    const std::string hint = backend.error().message;
    if (!options_.backend.empty()) return forward_error(backend.error());
    log::warn("平台后端不可用（{}），回退 headless", hint);
    auto fallback = shell::create_backend("headless");
    if (!fallback) return forward_error(fallback.error());
    impl_->backend_holder = std::move(*fallback);
  } else {
    impl_->backend_holder = std::move(*backend);
  }
  impl_->backend = impl_->backend_holder.get();

  // DPI 解析：显式选项 > ST_SCALE 环境变量 > 1.0（无头默认 1x；有显示时由平台后端提供）
  if (options_.scale <= 0.0f) {
    if (const auto env_scale = fs::read_env("ST_SCALE"); env_scale.has_value()) {
      if (const auto parsed = parse_f64(*env_scale); parsed.has_value() && *parsed > 0.0) {
        options_.scale = static_cast<float>(*parsed);
      }
    }
  }
  if (options_.scale <= 0.0f) options_.scale = 1.0f;

  shell::WindowOptions window;
  window.width = options_.width;
  window.height = options_.height;
  window.scale = options_.scale;
  window.title = options_.title;
  window.headless = impl_->backend->headless();
  if (auto status = impl_->backend->create_window(window); !status) {
    return forward_error(status.error());
  }

  // 字体：系统回退链（缺失时退化为 NullTextPort，UI 仍可运行）
  auto stack = st::text::FontStack::system_default();
  if (stack) {
    impl_->fonts = std::make_unique<st::text::FontStack>(std::move(*stack));
    impl_->renderer = std::make_unique<st::text::TextRenderer>(*impl_->fonts, options_.scale);
    impl_->text_port = std::make_unique<RendererTextPort>(*impl_->renderer);
    root_.set_text_port(impl_->text_port.get());
    log::info("字体已加载：{} 个 face", impl_->fonts->faces().size());
  } else {
    log::warn("未找到可用字体（{}），文本将不渲染", stack.error().message);
  }

  root_.set_viewport(math::Size{static_cast<float>(options_.width),
                                static_cast<float>(options_.height)});

  if (options_.enable_script) {
    impl_->script = std::make_unique<ui::ScriptHost>(root_, options_.script_limits);
    if (!impl_->script->valid()) {
      impl_->script.reset();
      return unexpected(ErrorCode::Unsupported,
                        "脚本宿主初始化失败（QuickJS 运行时或 JS 前置加载异常）");
    }
    // 事件桥由 `ScriptHost` 自己在构造时接上（见其构造函数注释）
    log::info("脚本能力已开启（内存上限 {} MiB / 超时 {} ms）",
              options_.script_limits.memory_bytes / (1024U * 1024U),
              options_.script_limits.timeout.count());
  }

  control::ServerOptions server_options;
  server_options.bind = options_.control_bind;
  server_options.port = options_.control_port;
  server_options.control_file = options_.control_file;
  server_options.enable_script = options_.enable_script;
  server_options.script_limits = options_.script_limits;
  impl_->server = std::make_unique<control::Server>(*this);
  auto port = impl_->server->start(server_options);
  if (!port) return forward_error(port.error());
  log::info("[gebai] shuangtian app '{}' listening control on {}:{} (backend={}, headless={})",
            name_, options_.control_bind, *port, backend_name(), headless());

  impl_->device_scale = impl_->backend->device_scale();
  impl_->started_ms = time::now_ms();
  started_ = true;
  render_frame();
  return ok();
}

void Application::render_frame() {
  if (impl_->backend == nullptr) return;
  const std::int64_t start_ns = time::now_ns();
  raster::Canvas& canvas = impl_->backend->framebuffer();
  root_.layout();
  const ui::Theme& theme = root_.theme();
  canvas.clear(theme.colors().bg);
  root_.paint(canvas);
  impl_->backend->present();
  ++impl_->frames;
  impl_->last_frame_ms = static_cast<double>(time::now_ns() - start_ns) / 1'000'000.0;
  impl_->frame_times.push_back(impl_->last_frame_ms);
  if (impl_->frame_times.size() > 240) impl_->frame_times.erase(impl_->frame_times.begin());
  impl_->repaint = false;
  root_.clear_dirty();
}

void Application::tick() {
  if (impl_->backend == nullptr) return;
  // 窗口系统请求关闭（用户点 X）：走与应用内 request_quit 相同的收尾路径
  if (impl_->backend->close_requested()) impl_->quit = true;
  // 窗口尺寸变化（用户拖拽边框）：视口跟随，否则界面只画在左上角旧尺寸区域
  const math::Size window_size = impl_->backend->logical_size();
  if (window_size.width > 0.0f && window_size.height > 0.0f &&
      (window_size.width != root_.viewport().width ||
       window_size.height != root_.viewport().height)) {
    root_.set_viewport(window_size);
    impl_->repaint = true;
  }
  if (impl_->repaint || root_.dirty()) render_frame();
  // 脚本定时器与"高频事件合并"的补发：按帧推进，不额外起线程
  if (impl_->script != nullptr) (void)impl_->script->tick(0.0);
  if (impl_->script != nullptr && impl_->script->take_repaint_request()) impl_->repaint = true;
  if (impl_->server != nullptr) impl_->server->poll();
  while (true) {
    auto event = impl_->backend->poll_event();
    if (!event.has_value()) break;
    (void)root_.dispatch(*event);
    impl_->repaint = true;
  }
}

Application::~Application() {
  // 脚本宿主（若启用）在其析构里摘掉事件观察者；此处只需保证它先于 `root_` 销毁
  impl_->script.reset();
}

void Application::set_content(std::unique_ptr<ui::Element> content) {
  root_.set_content(std::move(content));
}

auto Application::run(std::unique_ptr<ui::Element> content) -> Result<int> {
  set_content(std::move(content));
  if (auto status = start(); !status) return forward_error(status.error());
  if (options_.exit_on_ready) return 0;

  const auto frame_interval = std::chrono::duration<double, std::milli>(
      std::max(1.0, options_.frame_budget_ms));
  while (!impl_->quit) {
    const std::int64_t frame_start = time::now_ns();
    tick();
    if (options_.max_frames > 0 && impl_->frames >= options_.max_frames) break;
    if (impl_->repaint || root_.dirty()) {
      const std::int64_t elapsed = time::now_ns() - frame_start;
      const auto elapsed_ms = static_cast<double>(elapsed) / 1'000'000.0;
      const double remaining = options_.frame_budget_ms - elapsed_ms;
      if (remaining > 0.5) {
        std::this_thread::sleep_for(
            std::chrono::duration<double, std::milli>(remaining));
      }
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
  }
  log::info("应用退出：共 {} 帧，最后一帧 {:.2f}ms", impl_->frames, impl_->last_frame_ms);
  return 0;
}

}  // namespace st::app

#include "st/app/app.hpp"

#include "st/app/text_port.hpp"

#include <algorithm>
#include <array>
#include <format>

#include "st/codec/png.hpp"
#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/core/process.hpp"
#include "st/core/string.hpp"
#include "st/core/time.hpp"
#include "st/core/wait.hpp"
#include "st/text/text.hpp"

namespace st::app {
namespace {

/// 字符串 → 布尔（`ST_TEXT_LCD` 等环境变量的取值解析）。
[[nodiscard]] auto parse_bool_word(std::string_view value) -> std::optional<bool> {
  if (value == "1" || value == "on" || value == "true") return true;
  if (value == "0" || value == "off" || value == "false") return false;
  return std::nullopt;
}

}  // namespace

auto resolve_text_lcd(std::string_view mode) -> bool {
  if (const auto parsed = parse_bool_word(mode); parsed.has_value()) return *parsed;
  if (const auto value = fs::read_env("ST_TEXT_LCD"); value.has_value() && !value->empty()) {
    if (const auto parsed = parse_bool_word(*value); parsed.has_value()) return *parsed;
  }
  return true;
}

auto resolve_text_fit(std::string_view mode) -> st::text::GridFitMode {
  const auto from_word = [](std::string_view value) -> std::optional<st::text::GridFitMode> {
    if (value == "off" || value == "0" || value == "false") return st::text::GridFitMode::Off;
    if (value == "light") return st::text::GridFitMode::Light;
    if (value == "normal" || value == "on" || value == "1" || value == "true") {
      return st::text::GridFitMode::Normal;
    }
    return std::nullopt;
  };
  if (mode != "auto") {
    if (const auto parsed = from_word(mode); parsed.has_value()) return *parsed;
  }
  if (const auto value = fs::read_env("ST_TEXT_FIT"); value.has_value() && !value->empty()) {
    if (const auto parsed = from_word(*value); parsed.has_value()) return *parsed;
  }
  return st::text::GridFitMode::Normal;
}

auto resolve_text_gamma(std::string_view mode) -> float {
  const auto from_word = [](std::string_view value) -> std::optional<float> {
    if (value.empty() || value == "auto") return std::nullopt;
    if (value == "off" || value == "none" || value == "0" || value == "1") return 1.0f;
    // `std::stof` 对 "1.4abc" 也会成功（只读前缀）——但那个容错只惠及拼写错误，
    // 而参数值是要写进日志与复现步骤的，所以宁可**整串严格**：读到尾部才算数。
    try {
      std::size_t consumed = 0;
      const float parsed = std::stof(std::string(value), &consumed);
      if (consumed != value.size()) return std::nullopt;
      return parsed;
    } catch (const std::exception&) {
      return std::nullopt;
    }
  };
  if (const auto parsed = from_word(mode); parsed.has_value()) {
    return st::text::TextRenderer::sanitize_coverage_gamma(*parsed);
  }
  if (const auto value = fs::read_env("ST_TEXT_GAMMA"); value.has_value() && !value->empty()) {
    if (const auto parsed = from_word(*value); parsed.has_value()) {
      return st::text::TextRenderer::sanitize_coverage_gamma(*parsed);
    }
  }
  return st::text::TextRenderer::kDefaultCoverageGamma;
}

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
  /// 最后一帧的分阶段耗时（排版/绘制/送显），用于定位"帧耗时高"到底花在哪里。
  double layout_ms{0.0};
  double paint_ms{0.0};
  double present_ms{0.0};
  std::int64_t started_ms{0};
  /// 应用启动时刻（单调）：动画时间轴的零点。
  std::int64_t started_ns{0};
  bool quit{false};
  bool repaint{true};
  bool pacing{false};   ///< `pace_loop` 已进入空闲节拍（= “接下来空等一段”）
  /// 启动完成回调（`on_ready`）：只跑一次。
  std::function<void()> on_ready{};
  /// 绘制剖析器（`ST_PAINT_PROFILE=1` 时才挂到帧缓冲画布上）。
  raster::PaintProfiler profiler{};
  bool profiling{false};
};

Application::Application(std::string name, std::string version, AppOptions options)
    : impl_(std::make_unique<Impl>()), name_(std::move(name)), version_(std::move(version)),
      options_(std::move(options)) {
  log::set_level(log::level_from_name(options_.log_level));
  root_.set_theme(ui::Theme::by_mode(options_.theme));
  // 绘制剖析默认关闭：挂了才计时（代价是每次绘制两次时钟读）。
  if (const auto flag = fs::read_env("ST_PAINT_PROFILE"); flag.has_value() && !flag->empty() &&
      *flag != "0") {
    impl_->profiling = true;
  }
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

// —— ui::WindowControl：窗框动作 → 后端（**只做转发**，平台差异全在 `platform_*`）——
//
// 为什么不在这里分平台：`CONVENTIONS.md` §10 第 1 条要求平台差异只能出现在 `platform_*`；
// 而 `Backend` 已经把"去装饰建窗、拖拽、最小化/最大化"封装成平台中立的接口。
// 把 `Status` 折成 `bool` 也是有意为之：窗口控制端口的契约是"这一下生效了吗"。
// 具体错误经 `log` 如实落盘（静默吞错误会让"点了没反应"变成谜案）。
[[nodiscard]] auto Application::window_control_available() const -> bool {
  return impl_->backend != nullptr && impl_->backend->supports_window_control();
}

[[nodiscard]] auto Application::window_minimize() -> bool {
  if (impl_->backend == nullptr) return false;
  const auto status = impl_->backend->minimize();
  if (!status) log::warn("最小化窗口失败：{}", status.error().message);
  return status.has_value();
}

[[nodiscard]] auto Application::window_toggle_maximize() -> bool {
  if (impl_->backend == nullptr) return false;
  const auto status = impl_->backend->toggle_maximize();
  if (!status) log::warn("最大化/还原窗口失败：{}", status.error().message);
  return status.has_value();
}

[[nodiscard]] auto Application::window_request_close() -> bool {
  if (impl_->backend == nullptr) return false;
  const auto status = impl_->backend->request_close();
  if (!status) log::warn("请求关闭窗口失败：{}", status.error().message);
  return status.has_value();
}

[[nodiscard]] auto Application::window_begin_move() -> bool {
  if (impl_->backend == nullptr) return false;
  const auto status = impl_->backend->begin_move();
  // 拖动失败**不报 warn**：无头/桩后端下这是常态（没窗口可拖），
  // 而它每次按下都会发生——刷屏会把真正值得看的日志淹掉。
  return status.has_value();
}

[[nodiscard]] auto Application::window_begin_resize(ui::WindowEdge edge) -> bool {
  if (impl_->backend == nullptr) return false;
  const auto status = impl_->backend->begin_resize(edge);
  return status.has_value();
}

[[nodiscard]] auto Application::window_maximized() const -> bool {
  return impl_->backend != nullptr && impl_->backend->maximized();
}

auto Application::control_port() const noexcept -> std::uint16_t {
  return impl_->server != nullptr ? impl_->server->port() : 0;
}

auto Application::paint_profile() const -> const raster::PaintProfiler* {
  return impl_->profiling ? &impl_->profiler : nullptr;
}

void Application::request_quit() { impl_->quit = true; }

void Application::request_repaint() {
  // 只请求"下一帧"；具体重绘范围交给**损坏区机制**（增量重绘）：
  //
  // 旧实现在这里调 `mark_dirty_all()`——把"有东西变了"当成"所有东西都变了"，
  // 于是每次控制通道 set/invoke/输入都要整树重排 + 整帧重画。现在：
  // - 元素级状态变更在 setter 里 `mark_dirty()` → 上报损坏区（局部重绘）；
  // - 需要重排的变更（文本变长等）置 `layout_dirty` → 下一帧重排 + 整帧（保守）；
  // - 什么都没标（如空悬停移动）→ 损坏区为空 → `paint_frame` 回落整帧（安全兼底）。
  impl_->repaint = true;
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
  metrics.layout_ms = impl_->layout_ms;
  metrics.paint_ms = impl_->paint_ms;
  metrics.present_ms = impl_->present_ms;
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
  metrics.renderer = std::string(impl_->backend->renderer_name());
  metrics.renderer_note = impl_->backend->renderer_note();
  if (impl_->renderer != nullptr) {
    metrics.text_renderer = impl_->renderer->subpixel() ? "lcd" : "grayscale";
    // 把拟合模式一并上报：它不是“开关”而是三档（off/light/normal），
    // 只说“开了”不足以复现一个渲染结果。
      metrics.text_fit = "";
  switch (impl_->renderer->grid_fit()) {
    case st::text::GridFitMode::Off: metrics.text_fit = "off"; break;
    case st::text::GridFitMode::Light: metrics.text_fit = "light"; break;
    case st::text::GridFitMode::Normal: metrics.text_fit = "normal"; break;
  }
  // 覆盖率 gamma 同样如实上报：它是“字看着多重”的直接决定量，
  // 不报就无法从一个现场截图复现同一种字。
  metrics.text_gamma = impl_->renderer->coverage_gamma();
  }
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

auto Application::capture_pixels(math::IntRect region) -> Result<control::PixelView> {
  if (impl_->backend == nullptr) {
    return unexpected(ErrorCode::Invalid, "应用未启动（无帧缓冲）");
  }
  raster::Surface& canvas = impl_->backend->framebuffer();
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

  control::PixelView view;
  view.width = area.width;
  view.height = area.height;
  view.rgba.resize(static_cast<std::size_t>(area.width) * static_cast<std::size_t>(area.height) * 4U);
  std::size_t index = 0;
  for (int y = 0; y < area.height; ++y) {
    for (int x = 0; x < area.width; ++x) {
      const math::Color color = canvas.pixel_at(area.x + x, area.y + y);
      view.rgba[index] = color.r;
      view.rgba[index + 1] = color.g;
      view.rgba[index + 2] = color.b;
      view.rgba[index + 3] = color.a;
      index += 4;
    }
  }
  return view;
}

auto Application::capture_png(math::IntRect region) -> Result<std::vector<std::uint8_t>> {
  auto view = capture_pixels(region);
  if (!view) return forward_error(view.error());
  codec::PngImage image;
  image.width = static_cast<std::uint32_t>(view->width);
  image.height = static_cast<std::uint32_t>(view->height);
  image.rgba = std::move(view->rgba);
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

  // 后端选择：`backend` 显式指定 > `headless=true`（等价于指定 headless）> 自动探测。
  //
  // `headless` 字段**必须真的参与选择**：它原先只在"后端为空时报告用"，于是
  // `options.headless = true` 是个静默无效的开关——生成的工程写着 headless 却仍去开窗口，
  // 在没有可用显示服务的机器上直接启动失败（实测踩到）。
  const std::string requested =
      options_.backend.empty() ? (options_.headless ? std::string("headless") : std::string{})
                               : options_.backend;
  auto backend = shell::create_backend(requested);
  if (!backend) {
    const std::string hint = backend.error().message;
    if (!requested.empty()) return forward_error(backend.error());
    log::warn("平台后端不可用（{}），回退 headless", hint);
    auto fallback = shell::create_backend("headless");
    if (!fallback) return forward_error(fallback.error());
    impl_->backend_holder = std::move(*fallback);
  } else {
    impl_->backend_holder = std::move(*backend);
  }
  impl_->backend = impl_->backend_holder.get();

  // DPI 解析：显式选项 > ST_SCALE 环境变量 > **后端默认**（不要在这里填 1.0）。
  //
  // ⚠ 这里原先的 `if (options_.scale <= 0) options_.scale = 1.0f;` 是个真缺陷：
  // win32 后端靠 `options.scale > 0` 区分"用户显式指定"与"用系统 DPI"，
  // 被 app 层预先填成 1.0 后**每个窗口都被当成显式 1x**，
  // `query_window_scale`（GetDpiForWindow → 系统缩放）永远不会被走到——
  // 实测：系统 1.5x（144 DPI）下窗口仍按 1.0x 建，内容全部偏小。
  // 语义应为：0 = "未指定"，交给后端（win32 查窗口 DPI；headless 无显示器，缺省 1x）。
  if (options_.scale <= 0.0f) {
    if (const auto env_scale = fs::read_env("ST_SCALE"); env_scale.has_value()) {
      if (const auto parsed = parse_f64(*env_scale); parsed.has_value() && *parsed > 0.0) {
        options_.scale = static_cast<float>(*parsed);
      }
    }
  }
  // 未指定（仍为 0）就传 0 给后端，由后端决定（win32 = 查系统 DPI）

  shell::WindowOptions window;
  window.width = options_.width;
  window.height = options_.height;
  window.scale = options_.scale;
  window.title = options_.title;
  window.renderer = options_.renderer;
  // 窗框一律自绘（`CONVENTIONS.md` §10 第 7 条）：把开关如实交给后端。
  window.decorations = options_.decorations;
  window.resizable = options_.resizable;
  window.headless = impl_->backend->headless();
  if (auto status = impl_->backend->create_window(window); !status) {
    // 自动选择的后端开不出窗口（例如探测到 libX11 但没有可用显示服务）→ 按约定回退 headless，
    // 而不是直接失败：无头模式下控制通道能完成全部开发与验证，比"起不来"有用得多。
    if (requested.empty()) {
      log::warn("窗口创建失败（{}），回退 headless", status.error().message);
      auto fallback = shell::create_backend("headless");
      if (!fallback) return forward_error(fallback.error());
      impl_->backend_holder = std::move(*fallback);
      impl_->backend = impl_->backend_holder.get();
      window.headless = true;
      if (auto retry = impl_->backend->create_window(window); !retry) {
        return forward_error(retry.error());
      }
    } else {
      return forward_error(status.error());
    }
  }

  // 字体：系统回退链（缺失时退化为 NullTextPort，UI 仍可运行）
  auto stack = st::text::FontStack::system_default();
  if (stack) {
    impl_->fonts = std::make_unique<st::text::FontStack>(std::move(*stack));
    impl_->renderer = std::make_unique<st::text::TextRenderer>(*impl_->fonts, options_.scale);
    // 文字形态：命令行 > 环境变量 > 默认（两侧同源，见 resolve_text_* 的说明）。
    impl_->renderer->set_subpixel(resolve_text_lcd(options_.text_lcd));
    impl_->renderer->set_grid_fit(resolve_text_fit(options_.text_fit));
    impl_->renderer->set_coverage_gamma(resolve_text_gamma(options_.text_gamma));
  // **Skia 式逐颜色校正**（见 `docs/SKIA_TEXT_RENDERING_STUDY.md`）：默认仍走 Gamma 模式，
  // 因为它是已验证过的现网观感；这条曲线留作对照与深色主题的候选。
  if (impl_->renderer->coverage_gamma() == 1.0f) {
    // `--text-gamma off` 语义就是“不校正”——不要被 Skia 模式覆盖。
  } else if (std::getenv("ST_TEXT_SKIA_LUT") != nullptr) {
    impl_->renderer->set_coverage_correct(st::text::TextRenderer::CoverageCorrect::Skia);
  }
    // 如实说清这一帧的字是怎么画的：“字看着糊”的第一个分歧点就在这里。
    const char* fit_name = impl_->renderer->grid_fit() == st::text::GridFitMode::Normal
                               ? "normal"
                               : (impl_->renderer->grid_fit() == st::text::GridFitMode::Light
                                      ? "light"
                                      : "关");
    log::info("文字渲染：{} · 网格拟合 {} · 覆盖率 gamma {}（中文字形为 CFF：只做几何拟合，不依赖字体自带指令）",
              impl_->renderer->subpixel() ? "LCD 亚像素（每像素 R/G/B 三重覆盖率）" : "灰度抗锯齿",
              fit_name,
              impl_->renderer->coverage_gamma() == 1.0f
                  ? std::string("关（1.0，不校正）")
                  : std::format("{}", impl_->renderer->coverage_gamma()));
    impl_->text_port = std::make_unique<RendererTextPort>(*impl_->renderer);
    root_.set_text_port(impl_->text_port.get());
    // 逐 face 记录**路径 / 序号 / 名称**。
    // 只记数量在排查"字变了"这类问题时毫无用处：字形由哪个 face 提供，
    // 决定了该怀疑哪份字体数据（TTC 的多 face、CID-keyed CFF 的 FDSelect 都在这一层）。
    const auto& loaded_faces = impl_->fonts->faces();
    log::info("字体已加载：{} 个 face + {} 个等宽 face", loaded_faces.size(),
              impl_->fonts->monospace_faces().size());
    // 正文档**按优先级顺序**列出：栈里靠前的先被 `find_face` 选中，
    // 所以这一行顺序本身就是"某个字最终由谁画"的答案。
    for (std::size_t index = 0; index < loaded_faces.size(); ++index) {
      const auto& face = loaded_faces[index];
      log::info("  [{}] index={} name={} path={}", index, face.face_index(), face.name(),
                face.path());
    }
    for (std::size_t index = 0; index < impl_->fonts->monospace_faces().size(); ++index) {
      const auto& face = impl_->fonts->monospace_faces()[index];
      log::info("  mono[{}] index={} name={} path={}", index, face.face_index(), face.name(),
                face.path());
    }
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
  impl_->started_ns = time::now_ns();  // 动画时间轴零点（与 started_ms 同源）
  started_ = true;
  render_frame();
  // 首帧已画完 → 窗口可以露面了。
  //
  // 窗口后端建窗时**故意不显**（见 `Backend::show_when_ready` 声明处）：否则
  // 窗口会先露出“系统预备的白底”，而画布分配（含渲染器实测基准，本机 ~570ms）
  // 与 DPI 尺寸调整还没做完——多出来的区域没有任何东西可贴，就是**启动时
  // 右下/底部一大块黑**（实测 0.6s 后自愈）。放在首帧之后：用户看到的第一眼
  // 就是一个完整界面，没有中间态。
  impl_->backend->show_when_ready();
  // 显示之后**再画一帧**（而不是只靠显示前那一帧）。
  //
  // 必要性：`ShowWindow` 之前做的 `Present` 发生在窗口被合成器映射之前，
  // DXGI 不会把它送上屏——于是“露面”的那一瞬间用户看到的是未初始化的客户区
  // （实测黑 89%，约 100ms 后自行补上）。这里补一帧：先把窗口显出来，
  // 再重画一次，保证**用户看到的第一帧就是完整界面**。
  render_frame();
  return ok();
}

void Application::render_frame() {
  if (impl_->backend == nullptr) return;
  const std::int64_t start_ns = time::now_ns();
  raster::Surface& canvas = impl_->backend->framebuffer();
  // 分阶段计时：没有分段数据就无法判断"帧慢"该改哪里（排版/光栅化/送显三条路完全不同）。
  const std::int64_t layout_start = time::now_ns();
  if (impl_->profiling) {
    impl_->profiler.clear();  // 分解看的是**最后一帧**（与其余阶段指标同一口径）
    canvas.set_profiler(&impl_->profiler);
  }
  root_.layout();
  // 时间轴推进：动画（开关/悬浮过渡/3D 旋转）都靠它。
  // 用**应用启动以来的秒数**而不是系统时间：前者单调、与帧序号同源，
  // 便于复现（同一帧序列 → 同一动画进度）。
  root_.set_time(static_cast<double>(start_ns - impl_->started_ns) / 1'000'000'000.0);

  const std::int64_t paint_start = time::now_ns();
  // 绘制一帧：`paint_frame` 自动选择全量/局部（损坏区驱动；软件画布走局部，
  // GPU 画布与全局变化恒走全量——语义与旧路径完全一致，只少了无效的整帧重画）。
  root_.paint_frame(canvas);
  const std::int64_t present_start = time::now_ns();
  impl_->backend->present();
  const std::int64_t end_ns = time::now_ns();
  const auto to_ms = [](std::int64_t value) { return static_cast<double>(value) / 1'000'000.0; };
  impl_->layout_ms = to_ms(paint_start - layout_start);
  impl_->paint_ms = to_ms(present_start - paint_start);
  impl_->present_ms = to_ms(end_ns - present_start);
  ++impl_->frames;
  impl_->last_frame_ms = to_ms(end_ns - start_ns);
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

void Application::on_ready(std::function<void()> callback) {
  impl_->on_ready = std::move(callback);
}

auto Application::run() -> Result<int> {
  if (auto status = start(); !status) return forward_error(status.error());
  if (impl_->on_ready) {
    impl_->on_ready();
    impl_->on_ready = nullptr;  // 只跑一次
  }
  return run_loop();
}

auto Application::run(std::unique_ptr<ui::Element> content) -> Result<int> {
  set_content(std::move(content));
  return run();
}

auto Application::run_loop() -> Result<int> {
  if (options_.exit_on_ready) return 0;

  while (!impl_->quit) {
    const std::int64_t frame_start = time::now_ns();
    tick();
    if (options_.max_frames > 0 && impl_->frames >= options_.max_frames) break;
    pace_loop(frame_start / 1'000'000);
  }
  log::info("应用退出：共 {} 帧，最后一帧 {:.2f}ms（排版 {:.2f} / 绘制 {:.2f} / 送显 {:.2f}）",
            impl_->frames, impl_->last_frame_ms, impl_->layout_ms, impl_->paint_ms,
            impl_->present_ms);
  return 0;
}

void Application::pace_loop(std::int64_t tick_start_ms) const {
  // 两种节拍都**可被控制通道打断**：
  //
  // 为什么“有活干”那一路也必须可打断：本轮 `tick()` 的次序是
  // `render_frame()` → `server->poll()` → `pace_loop()`，而 `poll()` 收到命令会置
  // `repaint`——于是**刚刚处理完一条命令**的那一轮就会进入帧预算睡眠（~16ms），
  // 下一条命令只好等到它睡醒。实测延迟分布 = **帧节拍 − 距上次命令的间隔**：
  //   间隔 0ms → 15.7ms；2ms → 13.8ms；8ms → 8.5ms；16ms → 3.8ms；100ms → 2.4ms。
  // 即“连续操作”的命令几乎总是白等一拍（每次 ~12ms）——AI 驱动开发的常见形态
  // （改状态→看图→再改）恰好就是连续操作。
  //
  // 现在：把控制通道的监听句柄交给内核等，**新请求一到就醒**（微秒级），
  // 同时保留原来的时长作为上限——帧计时（动画/悬浮过渡）一分不少。
  const auto interruptible_wait = [this](int timeout_ms) {
    if (impl_->server == nullptr) {
      platform::sleep_ms(static_cast<double>(timeout_ms));
      return;
    }
    // 句柄每次现取：客户端集合会变（接入/断开），缓存列表会失效。
    const std::vector<std::intptr_t> handles = impl_->server->wait_handles();
    if (handles.empty()) {
      platform::sleep_ms(static_cast<double>(timeout_ms));
      return;
    }
    impl_->pacing = true;
    (void)platform::wait_any_readable(handles, timeout_ms);
    impl_->pacing = false;
  };
  if (impl_->repaint || root_.dirty()) {
    const double elapsed_ms = static_cast<double>(time::now_ms() - tick_start_ms);
    const double remaining = options_.frame_budget_ms - elapsed_ms;
    if (remaining > 0.5) interruptible_wait(static_cast<int>(remaining));
    return;
  }
  // 空闲：等控制通道可读（或 4ms 超时），而不是盲睡固定拍
  interruptible_wait(4);
}

void Application::wake_control() {
  // 控制通道有数据到达：若正处于等待中，立即结束它（下一轮 `tick()` 就能处理）。
  // 非等待期不动——已经在干活，等完这一段自然会去 poll。
  if (!impl_->pacing || impl_->server == nullptr) return;
  const std::vector<std::intptr_t> handles = impl_->server->wait_handles();
  if (!handles.empty()) (void)platform::wait_any_readable(handles, 0);
}

}  // namespace st::app

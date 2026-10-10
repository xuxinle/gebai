#include "st/app/app.hpp"

#include "st/app/text_port.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>

#include "st/codec/png.hpp"
#include "st/core/fs.hpp"
#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/core/process.hpp"
#include "st/core/string.hpp"
#include "st/core/time.hpp"
#include "st/core/wait.hpp"
#include "st/ext/json.hpp"
#include "st/text/text.hpp"
#include "st/ui/theme_io.hpp"

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
  // **默认 `off`**（2026-10-06 定，以真窗口浏览器为基准逐轴标定）。
  //
  // 下面这段 light 的推导仍然成立，但它是**单指标、无参照**的推理——"比不拟合更锐/更均匀"
  // 成立，却回答不了"该有多锐"。补上真窗口浏览器这个参照后：**参照的锐度就在不拟合那一档**
  // （边缘柔度：不拟合 3.27~6.66、拟合 light 2.81~4.86、浏览器 3.22~8.74），
  // 即拟合偏锐；覆盖离散上拟合也更大。三个正交轴都指向 `off`，详见 `docs/BACKLOG.md` P1。
  //
  // 用户线索（同一菜单栏）：同一字号字重下「运行」0.684 而「文件」0.511，**差 34%**。
  // 实测（gbcode 菜单栏，同一构建）：
  //
  // | 配置 | 墨量极差 |
  // |---|---|
  // | fit=off | 18% |
  // | normal | **34%** |
  // | normal + 墨量补偿 | **40%**（更差） |
  // | **light + 墨量补偿** | **18%**（追平不拟合） |
  //
  // 即：normal 的吸附幅度大、对字间墨量的扰乱已无法用补偿救回，而 light
  //（吸附幅度更小）+ 墨量补偿能同时拿到“锐度基本无损（过渡带 0.435 vs 不拟合 0.439）”
  // 与“字间均匀度追平不拟合”。这正是“全面优化、不要顾此失彼”的解。
  return st::text::GridFitMode::Off;
}

/// 拟合墨量补偿是否应开启（与 `resolve_text_fit` 成对，测试与实现走同一入口）。
///
/// **只有拟合开着时才需要它**：补偿解决的是「拟合对每个字的墨量改变幅度不一致」
/// （实测 normal 下逐字变化率极差 14.0%、light 11.4%），拟合关掉时逐字墨量
/// 恒为 1.000（无扰乱），补偿无事可做。
///
/// 为何要抽成函数：应用层实测（gbcode 菜单栏逐项墨量极差，同一构建）
/// `fit=off 18% / normal 34% / normal+补偿 40%（更差） / light+补偿 18%（追平不拟合）`
/// ——“light+补偿”那行是**成对**的结论（吸附幅度小的拟合配补偿才划算）。
/// 默认档后来改为 `Off`（浏览器基准），补偿也随之无事可做；
/// 状态当时只改了该函数的返回值，而 `app.cpp` 仍无条件 `set_ink_compensation(true)`、
/// 测试也自己写死 `Normal`——三处各自写一份默认值，必然错位。
[[nodiscard]] auto resolve_ink_compensation(st::text::GridFitMode fit) -> bool {
  return fit != st::text::GridFitMode::Off;
}

/// 解析覆盖率 gamma：命令行 > 环境变量 > `fallback`。
///
/// `fallback` 由调用方按**当前主题**给（见 `default_gamma_for`）——浅底/深底的正确值
/// 相差近 2 倍，写死一个必然错一个主题。
auto resolve_text_gamma(std::string_view mode, float fallback) -> float {
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
  return fallback;
}

auto resolve_ui_font_scale(std::string_view mode) -> float {
  const auto from_word = [](std::string_view value) -> std::optional<float> {
    if (value.empty() || value == "auto") return std::nullopt;
    try {
      std::size_t consumed = 0;
      const float parsed = std::stof(std::string(value), &consumed);
      if (consumed != value.size()) return std::nullopt;
      return parsed;
    } catch (const std::exception&) {
      return std::nullopt;
    }
  };
  // 范围限定：字号缩放超出 [0.5, 3] 只会把界面变得不可用（文字挤出控件或小到看不清），
  // 那是拼写错误而不是意图——夹取而不是静默接受。
  const auto clamp = [](float value) { return std::clamp(value, 0.5f, 3.0f); };
  if (const auto parsed = from_word(mode); parsed.has_value()) return clamp(*parsed);
  if (const auto value = fs::read_env("ST_UI_FONT_SCALE"); value.has_value() && !value->empty()) {
    if (const auto parsed = from_word(*value); parsed.has_value()) return clamp(*parsed);
  }
  return 1.0f;
}

/// 构造主题并按 `factor` 缩放整条字号阶梯（`factor == 1` 时等同 `Theme::by_mode`）。
/// 该主题模式下的**默认覆盖率 gamma**。
///
/// 两个主题的默认值必须不同：预校正的方向按**黑字白底**推导，白字黑底的观感由反方向
/// 的对比决定。真窗口实测（`tools/calibrate_text_gamma.py measure --theme dark`）深底理想 γ≈**0.57~0.77**，
/// 而浅底是 **1.05~1.6**——相差近 2 倍，单一默认值必然错一个主题。
[[nodiscard]] auto default_gamma_for(ui::ThemeMode mode) noexcept -> float {
  return mode == ui::ThemeMode::Dark ? st::text::TextRenderer::kDefaultCoverageGammaOnDark
                                     : st::text::TextRenderer::kDefaultCoverageGamma;
}

[[nodiscard]] auto scaled_theme(ui::ThemeMode mode, float factor) -> ui::Theme {
  ui::Theme theme = ui::Theme::by_mode(mode);
  theme.metrics().scale_fonts(factor);
  return theme;
}

/// 解析生效的自定义主题文件（命令行 > `ST_THEME_FILE`）。
[[nodiscard]] auto resolve_theme_file(std::string_view configured) -> std::string {
  if (!configured.empty()) return std::string(configured);
  const auto from_env = fs::read_env("ST_THEME_FILE");
  return from_env.has_value() ? *from_env : std::string{};
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
  /// 界面字号缩放（构造时解析一次，切主题时复用）——见 `Metrics::scale_fonts`。
  float font_scale{1.0f};
  /// 主题文件的**原文**（含 `base`；合成时跳过 `base`，它由 `apply_theme_file` 消费）。
  /// 住 `Impl` 而不是 `Application` 成员：`Json` 在 `app.hpp` 里只有前向声明
  /// （该头刻意不拉入 nlohmann 的 25,526 行），而**成员必须是完整类型**。
  Json theme_spec{Json::object()};
  /// 运行期 `theme.set` 累加的覆盖（优先级最高）。
  ///
  /// 必须**累加**而不是替换：控制通道分多次发 `colors` / `metrics` 时，
  /// 后一笔不能把前一笔抹掉。
  Json runtime_overlay{Json::object()};
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
  /// 日志订阅 id（构造时注册、析构时注销）——见 `Application::Application` 的说明。
  std::uint64_t log_listener_id{0};
};

Application::Application(std::string name, std::string version, AppOptions options)
    : impl_(std::make_unique<Impl>()), name_(std::move(name)), version_(std::move(version)),
      options_(std::move(options)) {
  log::set_level(log::level_from_name(options_.log_level));
  impl_->font_scale = resolve_ui_font_scale(options_.ui_font_scale);
  root_.set_theme(scaled_theme(options_.theme, impl_->font_scale));
  // 绘制剖析默认关闭：挂了才计时（代价是每次绘制两次时钟读）。
  if (const auto flag = fs::read_env("ST_PAINT_PROFILE"); flag.has_value() && !flag->empty() &&
      *flag != "0") {
    impl_->profiling = true;
  }
  if (!options_.screenshot_dir.empty()) {
    (void)fs::create_directories(options_.screenshot_dir);
  }
  // 订阅日志：控制通道的 `logs` 读的就是这里。
  //
  // **返回的 id 必须存下来、析构时注销**：监听器列表是进程级全局的，而
  // `Application` 可能是临时对象（测试里逐个构造销毁、工具里一个进程可建多个）。
  // 不注销就是悬垂监听器——对象死后只要**任何线程**打一条日志
  // （典型：`st::pkg::build` 的编译 worker）就会踩已释放的 `this`。
  impl_->log_listener_id = log::add_listener([this](log::Level level, std::string_view message) {
    impl_->log_lines.push_back(std::format("[{}] {}", to_string(level), message));
    if (impl_->log_lines.size() > 512) impl_->log_lines.erase(impl_->log_lines.begin());
  });
  // **自定义主题在日志监听器之后加载**：加载失败要写告警，而告警必须能被
  // 控制通道的 `logs` 读到（否则"主题没生效"在自动化流程里完全不可见）。
  apply_theme_file();
}

/// 装载 `--theme-file` / `ST_THEME_FILE` 指定的自定义主题（无配置时不动）。
///
/// 失败**不阻止启动**：主题是外观配置而非运行前提。但也不能静默——
/// 用户改了文件却没生效，是本类问题里最难查的一种（与"JSON 未知键要报错"同一个理由）。
void Application::apply_theme_file() {
  const std::string path = resolve_theme_file(options_.theme_file);
  if (path.empty()) return;
  options_.theme_file = path;
  // **先只读出配置本身**（不构造成品主题）：切模式时要拿它重新叠。
  const auto file_json = ui::load_theme_json_file(path);
  if (!file_json) {
    ST_LOG_WARN("自定义主题加载失败，已退回内置主题：{}", file_json.error().message);
    return;
  }
  // ⚠ `base` 是**基准选择器**，不是覆盖项：它决定用哪个内置主题做底。
  // 所以要在同一处、一次就从 JSON 里消费掉——留着它会让后面每次叠覆盖都报
  // "base 与当前模式不符"（实测踩过：文件里写着 `base: "dark"`，于是切到亮色后
  // **整份文件覆盖都被拒绝**，用户看到的是"我的配色全没了"）。
  impl_->theme_spec = file_json->is_object() ? *file_json : Json::object();
  ui::ThemeMode base_mode = options_.theme;
  if (const auto* base_value = json_find(impl_->theme_spec, "base"); base_value != nullptr) {
    const std::string base = json_as_string(*base_value, "");
    const auto parsed = ui::theme_mode_from_name(base);
    if (!parsed) {
      ST_LOG_WARN("自定义主题加载失败，已退回内置主题：未知基准主题 base=\"{}\"", base);
      impl_->theme_spec = Json::object();
      return;
    }
    base_mode = *parsed;
  }
  // **先校验后落地**：在拷贝上走一遍完整合成，失败则回退内置且不改任何状态。
  const auto composed = compose_theme_with(base_mode, impl_->runtime_overlay);
  if (!composed) {
    ST_LOG_WARN("自定义主题加载失败，已退回内置主题：{}", composed.error().message);
    impl_->theme_spec = Json::object();
    return;
  }
  custom_theme_ = *composed;
  options_.theme = base_mode;
  root_.set_theme(custom_theme_);
}

/// 重新构造生效主题（内置基准 + 文件覆盖 + 运行期覆盖）。
///
/// 一切会改变生效主题的动作（切模式、控制通道 `theme.set`）都走这里，
/// 而不是各自拼一份——否则"三层来源的优先级"就会在多个地方各实现一遍，
/// 迟早不一致（实测已踩：切模式时直接重读文件，把运行期改色静默抹掉）。
///
/// 优先级（后者覆盖前者）：内置基准 → 主题文件 → 运行期 `theme.set`。
[[nodiscard]] auto Application::compose_theme(ui::ThemeMode mode) const -> Result<ui::Theme> {
  return compose_theme_with(mode, impl_->runtime_overlay);
}

/// `compose_theme` 的显式变体：用指定的运行期覆盖（供"先校验后落地"用）。
///
/// `base` 在这里被**跳过**：它是基准选择器（由 `apply_theme_file` 消费），
/// 不是覆盖项。留着它会让每次叠覆盖都报"与当前模式不符"。
[[nodiscard]] auto Application::compose_theme_with(ui::ThemeMode mode,
                                                   const Json& runtime) const
    -> Result<ui::Theme> {
  ui::Theme theme = scaled_theme(mode, impl_->font_scale);
  if (impl_->theme_spec.is_object() && !impl_->theme_spec.empty()) {
    Json spec = impl_->theme_spec;
    spec.erase("base");
    const auto applied = ui::apply_theme_overrides(theme, spec);
    if (!applied) return forward_error(applied.error());
  }
  if (runtime.is_object() && !runtime.empty()) {
    const auto applied = ui::apply_theme_overrides(theme, runtime);
    if (!applied) return forward_error(applied.error());
  }
  return theme;
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
  if (!status) ST_LOG_WARN("最小化窗口失败：{}", status.error().message);
  return status.has_value();
}

[[nodiscard]] auto Application::window_toggle_maximize() -> bool {
  if (impl_->backend == nullptr) return false;
  const auto status = impl_->backend->toggle_maximize();
  if (!status) ST_LOG_WARN("最大化/还原窗口失败：{}", status.error().message);
  return status.has_value();
}

[[nodiscard]] auto Application::window_request_close() -> bool {
  if (impl_->backend == nullptr) return false;
  const auto status = impl_->backend->request_close();
  if (!status) ST_LOG_WARN("请求关闭窗口失败：{}", status.error().message);
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
  // 三种来源（内置基准 / 主题文件 / 运行期改色）的合成统一走 `compose_theme`：
  // 早先是"切模式时直接重读文件"，于是运行期用 `theme.set` 改过的色
  // 会在切一次亮暗后被**静默抹掉**（用户看到"我设的颜色过一会儿就没了"）。
  const auto composed = compose_theme(mode);
  if (!composed) {
    ST_LOG_WARN("切换主题失败，保持当前主题：{}", composed.error().message);
    return;
  }
  custom_theme_ = *composed;
  root_.set_theme(custom_theme_);
  options_.theme = mode;
  // **gamma 跟着主题走**：不切的话深色会拿到浅色标定的值（偏亮）、浅色拿到深色的（偏暗）。
  // 只在用户没显式配置时覆盖——显式值是他自己要的，不该被主题悄悄改掉。
  if (impl_->renderer != nullptr && options_.text_gamma.empty()) {
    impl_->renderer->set_coverage_gamma(default_gamma_for(mode));
  }
}

auto Application::apply_theme_overrides(const st::Json& overrides) -> Status {
  // 先**校验并并合**：失败时 `impl_->runtime_overlay` 与界面都不变。
  Json merged = impl_->runtime_overlay.is_object() ? impl_->runtime_overlay : Json::object();
  const auto combined = ui::merge_overrides(merged, overrides);
  if (!combined) return forward_error(combined.error());
  const auto composed = compose_theme_with(options_.theme, *combined);
  if (!composed) return forward_error(composed.error());
  impl_->runtime_overlay = *combined;
  custom_theme_ = *composed;
  root_.set_theme(custom_theme_);
  return {};
}

auto Application::theme_snapshot() const -> st::Json { return ui::theme_to_json(root_.theme()); }

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
  // **字形超采样必须跟着 DPI 走**（与构造时同一个入口）。
  //
  // `TextRenderer` 的超采样在构造时由解析出的 DPI 决定（`make_unique<TextRenderer>(fonts,
  // resolved_scale)`），而运行期切换 DPI 只改了缓冲尺寸——于是“启动即 2.0”
  // （超采样 2）与“启动 1.0 再切到 2.0”（超采样停在 1）**渲染结果不同**：
  // 后者每像素只有一个采样点，大字号的斜向/弧形边缘出现可见阶梯（实测同一区域
  // 1524 个像素差 >32）。`set_supersample` 会同时清字形缓存，所以不会混用旧密度的位图。
  if (impl_->renderer != nullptr) impl_->renderer->set_supersample(scale);
  root_.mark_dirty_all();
  ST_LOG_INFO("DPI 缩放已切换为 {}（物理 {}×{}）", scale,
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

auto Application::capture_png(math::IntRect region) -> Result<control::Host::PngView> {
  auto view = capture_pixels(region);
  if (!view) return forward_error(view.error());
  codec::PngImage image;
  image.width = static_cast<std::uint32_t>(view->width);
  image.height = static_cast<std::uint32_t>(view->height);
  // 尺寸必须在 `rgba` 被 move 走之**前**抄下来。
  const int width = view->width;
  const int height = view->height;
  image.rgba = std::move(view->rgba);
  auto encoded = codec::png_encode(image, 6);
  if (!encoded) return forward_error(encoded.error());
  return control::Host::PngView{std::move(*encoded), width, height};
}

auto Application::capture_to_file(std::string_view path, math::IntRect region)
    -> Result<control::Host::SavedShot> {
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
  // 尺寸取自 `capture_png` 的返回（= 已夹取的**实际**值），**不重算**：
  // 拿 `region` 重算会在区域被夹取时谎报（见 `control::Host::capture_to_file` 注释）。
  const int width = png->width;
  const int height = png->height;
  if (auto status = fs::write_bytes(target, std::span<const std::uint8_t>(png->png)); !status) {
    return forward_error(status.error());
  }
  ST_LOG_INFO("截图已保存 {}", target);
  return control::Host::SavedShot{std::move(target), width, height};
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
    ST_LOG_WARN("平台后端不可用（{}），回退 headless", hint);
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
      ST_LOG_WARN("窗口创建失败（{}），回退 headless", status.error().message);
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

  // 窗口/任务栏图标（PNG 字节，编译期嵌入）：**建窗之后**才能设（要 `HWND`）。
  //
  // 为何不失败就退出：图标是外观资源，不是运行前提。后端不支持（无头）或数据不合法时
  // 只写一条告警日志——与 `--theme-file` 加载失败同一姿态（配置/资源写错不该让进程起不来）。
  if (!options_.window_icon_png.empty()) {
    if (auto status = impl_->backend->set_window_icon(options_.window_icon_png); !status) {
      ST_LOG_WARN("窗口图标未设置：{}", status.error().message);
    }
  }

  auto stack = st::text::FontStack::system_default();
  if (stack) {
    impl_->fonts = std::make_unique<st::text::FontStack>(std::move(*stack));
    // **超采样用后端解析出的真实 DPI，而不是 `options_.scale`**（真缺陷，2026-10-04）。
    //
    // 为什么：`options_.scale` 在“自动”模式下到这一步**仍是 0**（见上面的 DPI 解析：
    // 只有显式指定或 ST_SCALE 才赋值，否则留给后端决定）。而超采样是
    // `max(1, lround(scale))` ⇒ **真窗口 1.5x 下超采样塌成 1**，
    // 每轴只有 2 级覆盖率、边缘出现可见阶梯 —— 用户反馈的「不够光滑锐利」正是它。
    //
    // 而**测试/截图都显式传了 `--scale 1.5`**，于是超采样是 2、看起来比用户实际更光滑：
    // “感觉没差别”的根源就在这里（量与用不是同一条链路）。
    //
    // 后端在 `create_window` 之后已解析出 scale（win32 查窗口 DPI / headless 查系统缩放），
    // 所以这里读 `backend->device_scale()`；它对显式指定同样返回该值（不改变既有语义）。
    const float resolved_scale =
        impl_->backend != nullptr && impl_->backend->device_scale() > 0.0f
            ? impl_->backend->device_scale()
            : (options_.scale > 0.0f ? options_.scale : 1.0f);
    impl_->renderer = std::make_unique<st::text::TextRenderer>(*impl_->fonts, resolved_scale);
    // 文字形态：命令行 > 环境变量 > 默认（两侧同源，见 resolve_text_* 的说明）。
    impl_->renderer->set_subpixel(resolve_text_lcd(options_.text_lcd));
    const st::text::GridFitMode fit = resolve_text_fit(options_.text_fit);
    impl_->renderer->set_grid_fit(fit);
    impl_->renderer->set_coverage_gamma(
      resolve_text_gamma(options_.text_gamma, default_gamma_for(options_.theme)));
    // **小字号分档**（见 `AppOptions::text_gamma_small`）：单档 gamma 消不掉“小字比正文
    // 偏重”的落差，小字号单独压一档。未配置就不动（行为与以前完全一致）。
    if (!options_.text_gamma_small.empty()) {
      const float small =
          resolve_text_gamma(options_.text_gamma_small, default_gamma_for(options_.theme));
      impl_->renderer->set_fitted_gamma(options_.text_gamma_small_max, small);
    }
    // **拟合墨量补偿**：拟合对每个字的墨量改变幅度不一致，是“有的字清晰、有的字发灰”
    // 的来源；补偿把它归一化回不拟合基准（只改墨色、不动几何）。
    //
    // 它与拟合档位是**一对**（应用层实测：fit=off 18% / normal 34% / normal+补偿 40%
    // / **light+补偿 18%**），所以开关跟着上面解析出的 `fit` 走（`resolve_ink_compensation`）
    // ——不再无条件 `true`：默认档已改为 `Off`，那里拟合不再扰乱墨量、补偿无事可做。
    impl_->renderer->set_ink_compensation(resolve_ink_compensation(fit));
  // **按字形类的覆盖率分档**（`TextRenderer::set_class_gamma`）。
  //
  // 为什么要分档：各字类的偏差**方向与幅度不同**，全局 γ 只能整体压黑，
  // 做不到"只补偏轻的那一类"。三档都由**逐字形总量比**扫描定
  // （量具：`tools/text_ab_allglyphs_page.html` 数字/字母、`tools/text_ab_han_page.html` 汉字）：
  //
  // | 类 | 不覆盖（基准，逐字号） | 采用档 | 采用后 |
  // |---|---|---|---|
  // | 数字 | 0.880 / 0.917 / 0.908 / 0.921（10~13px） | **0.92** | 0.957~0.983 |
  // | 字母 | 大写 0.975/0.950/0.943/0.952；小写 0.989/0.974/0.941/0.943 | **0.98** | 0.969~1.021 |
  // | 汉字 | 0.929 / 0.933 / 0.944 / 0.940 / 0.949 / 0.938（10/11/12/13/15/20px） | **0.93** | 0.992~1.002（20px 0.974） |
  //
  // 汉字档的取法说明：不覆盖时偏轻 5~7% 且**跨字号符号一致**；压到 0.93 后 10~15px
  // 落在 0.992~1.002，20px 仍略低（0.974）——那是"字号越大越接近"的自然趋势，
  // 不做过度补偿（再压一档会让小字号过冲）。
  //
  // 三档各自可用环境变量覆盖（诊断/复标用）：
  // `ST_TEXT_DIGIT_GAMMA` / `ST_TEXT_LETTER_GAMMA` / `ST_TEXT_HAN_GAMMA`。
  {
    // **类 gamma 用内置默认档**（`ClassGammas`：亮底 0.84/0.84/0.88、暗底 0.64/0.62/0.75）。
    // 它与 `--text-fit`（默认 off）共同构成"以真窗口浏览器三轴标定"的默认观感；
    // 两者是**正交参数**，各自可单独覆盖，不做"打包成一个档位名"的设计——
    // 打包出来的代号（A/B/C 之类）对使用者毫无意义，且把两个可独立调的旋钮捆成一个，
    // 想只改其中一个就不得不整体换档。
    st::app::ClassGammas gammas{};
    // 暗色主题换一组（见 `DarkClassGammas`：覆盖率→码值的映射在亮/暗底下不对称，
    // 沿用亮底 γ 会让暗底系统性偏轻 5~6%，真窗口实测）。
    if (impl_->renderer->coverage_gamma() < 1.0f) {
      const st::app::DarkClassGammas dark{};
      gammas.digit = dark.digit;
      gammas.letter = dark.letter;
      gammas.han = dark.han;
    }
    // 逐类覆盖：命令行 > 环境变量 > 内置默认。
    // 三类的 γ 本就是**独立旋钮**（各字形的度量特性不同），命令行因此也按类给。
    const auto class_gamma = [&](std::string_view cli_value, std::string_view env_name,
                                 float fallback) {
      if (!cli_value.empty() && cli_value != "auto") {
        try {
          return std::stof(std::string(cli_value));
        } catch (const std::exception&) {
          // 解析失败落到环境变量/默认——不静默用一个错值（`resolve_text_gamma` 同一姿态）。
        }
      }
      if (const auto value = fs::read_env(std::string(env_name));
          value.has_value() && !value->empty()) {
        try {
          return std::stof(*value);
        } catch (const std::exception&) {
          return fallback;
        }
      }
      return fallback;
    };
    gammas.digit = class_gamma(options_.text_digit_gamma, "ST_TEXT_DIGIT_GAMMA", gammas.digit);
    gammas.letter = class_gamma(options_.text_letter_gamma, "ST_TEXT_LETTER_GAMMA", gammas.letter);
    gammas.han = class_gamma(options_.text_han_gamma, "ST_TEXT_HAN_GAMMA", gammas.han);
    // 拟合的适用字号上限（`0` = 不限）：大字号上拟合两轴都无收益（见 BACKLOG P1），
    // 需要"只在小字号拟合"时用它，而不必整体关掉拟合。
    float fit_max_size = 0.0f;
    if (!options_.text_fit_max_size.empty() && options_.text_fit_max_size != "auto") {
      try {
        fit_max_size = std::stof(options_.text_fit_max_size);
      } catch (const std::exception&) {
        fit_max_size = 0.0f;
      }
    } else if (const auto value = fs::read_env("ST_TEXT_FIT_MAX_SIZE");
               value.has_value() && !value->empty()) {
      try {
        fit_max_size = std::stof(*value);
      } catch (const std::exception&) {
        fit_max_size = 0.0f;
      }
    }
    if (fit_max_size > 0.0f) impl_->renderer->set_grid_fit_max_size(fit_max_size);
    apply_class_gammas(*impl_->renderer, gammas);
    // 生效值自检（`ST_TEXT_DEBUG=1`）：测"参数是否真的生效"的**唯一**可靠手段——
    // 实测踩到两次：补丁被回退后两份渲染出几乎一样的图、应用自带解析器把新参数静默吃掉。
    if (fs::read_env("ST_TEXT_DEBUG").has_value()) {
      st::print("[text] theme_gamma={:.2f} fit={} fit_max_size={} class_gamma={}/{}/{}\n",
                impl_->renderer->coverage_gamma(),
                static_cast<int>(impl_->renderer->grid_fit()), fit_max_size, gammas.digit,
                gammas.letter, gammas.han);
    }
  }
  // **笔画加墨（stem darkening）**：与浏览器逐带对照后（`docs/TEXT_AB_REPORT.md`）确认，
  // 汉字的差距是"笔画没到满黑"（实心像素比 0.87~0.90）而非几何——FreeType 在这条路上
  // 做的是**笔画加墨**（CFF 引擎默认开、随 ppem 衰减），霜天原先没有。
  // 开启后汉字过渡像素比 0.874 → 0.960（更接近浏览器），而**拉丁/等宽逐位不变**
  // （实现里按轮廓类型只对 CFF 生效：真型字体本来就比浏览器重 1.09，加墨会过粗）。
  // 可用 `ST_TEXT_DARKEN=0` 关掉做对照。
  {
    const auto flag = fs::read_env("ST_TEXT_DARKEN");
    const bool enabled = !(flag.has_value() &&
                           (*flag == "0" || *flag == "false" || *flag == "off"));
    impl_->renderer->set_stem_darkening(enabled);
  }
  // **Skia 式逐颜色校正**（见 `docs/SKIA_TEXT_RENDERING_STUDY.md`）：默认仍走 Gamma 模式，
  // 因为它是已验证过的现网观感；这条曲线留作对照与深色主题的候选。
  if (impl_->renderer->coverage_gamma() == 1.0f) {
    // `--text-gamma off` 语义就是“不校正”——不要被 Skia 模式覆盖。
  } else if (std::getenv("ST_TEXT_SKIA_LUT") != nullptr) {
    impl_->renderer->set_coverage_correct(st::text::TextRenderer::CoverageCorrect::Skia);
  }
  // **扁平化容差的对照实验**：中文是 CFF 立方曲线轮廓，笔画侧边大量是微弯曲线，
  // 扁平化成折线后近似位置会随字形漂移。开关在 `rasterizer.cpp` 里直接读
  // `ST_TEXT_FLATTEN`（诊断项不进公共 API）。实测结论：**容差不是瓶颈**（见该处注释）。
    // 如实说清这一帧的字是怎么画的：“字看着糊”的第一个分歧点就在这里。
    // `fit_name` 只服务这条日志，所以**不单独声明**（裁日志时会变成未使用变量，
    // 而 `-Werror` 直接断构建）——直接把三元表达式写进实参里。
    ST_LOG_INFO(
        "文字渲染：{} · 网格拟合 {} · 覆盖率 gamma {}（中文字形为 CFF：只做几何拟合，不依赖字体自带指令）",
        impl_->renderer->subpixel() ? "LCD 亚像素（每像素 R/G/B 三重覆盖率）" : "灰度抗锯齿",
        impl_->renderer->grid_fit() == st::text::GridFitMode::Normal
            ? "normal"
            : (impl_->renderer->grid_fit() == st::text::GridFitMode::Light ? "light" : "关"),
        impl_->renderer->coverage_gamma() == 1.0f
            ? std::string("关（1.0，不校正）")
            : std::format("{}", impl_->renderer->coverage_gamma()));
    impl_->text_port = std::make_unique<RendererTextPort>(*impl_->renderer);
    root_.set_text_port(impl_->text_port.get());
    // 逐 face 记录**路径 / 序号 / 名称**。
    // 只记数量在排查"字变了"这类问题时毫无用处：字形由哪个 face 提供，
    // 决定了该怀疑哪份字体数据（TTC 的多 face、CID-keyed CFF 的 FDSelect 都在这一层）。
        const auto& loaded_faces = impl_->fonts->faces();
    ST_LOG_INFO("字体已加载：{} 个 face + {} 个等宽 face", loaded_faces.size(),
                impl_->fonts->monospace_faces().size());
    (void)loaded_faces;   // 日志被裁时上面那行不展开（见下段 `#if`）
    (void)loaded_faces;   // 日志被裁时上面那行不展开（见下段 `#if`）
    // 正文档**按优先级顺序**列出：栈里靠前的先被 `find_face` 选中，
    // 所以这一行顺序本身就是"某个字最终由谁画"的答案。
    //
    // 整段只在“日志编进来”时存在：这两个循环**只为日志而写**，
    // 裁掉日志时留着它们会变成“未使用的循环变量”（`-Werror` 直接断构建），
    // 而写 `(void)face;` 去哄编译器只是掩住了“这段本就不该在”这个事实。
#if !defined(ST_LOG_DISABLED)
    for (std::size_t index = 0; index < loaded_faces.size(); ++index) {
      const auto& face = loaded_faces[index];
      ST_LOG_INFO("  [{}] index={} name={} path={}", index, face.face_index(), face.name(),
                  face.path());
    }
    for (std::size_t index = 0; index < impl_->fonts->monospace_faces().size(); ++index) {
      const auto& face = impl_->fonts->monospace_faces()[index];
      ST_LOG_INFO("  mono[{}] index={} name={} path={}", index, face.face_index(), face.name(),
                  face.path());
    }
#endif
  } else {
    ST_LOG_WARN("未找到可用字体（{}），文本将不渲染", stack.error().message);
  }

  root_.set_viewport(math::Size{static_cast<float>(options_.width),
                                static_cast<float>(options_.height)});

  // 剪贴板：backend 已有平台实现（win32 真剪贴板、headless 进程内模拟），
  // 经 `UiRoot` 的 provider 接到组件层（终端 `Ctrl+Shift+V` 等）。读失败降级为
  // 空串——粘贴是“锦上添花”，不该因平台差异启动失败。
  if (impl_->backend != nullptr) {
    shell::Backend* backend_ptr = impl_->backend;
    root_.set_clipboard_provider([backend_ptr]() -> std::string {
      const auto text = backend_ptr->clipboard_text();
      return text.has_value() ? *text : std::string{};
    });
  }

  if (options_.enable_script) {
    impl_->script = std::make_unique<ui::ScriptHost>(root_, options_.script_limits);
    if (!impl_->script->valid()) {
      impl_->script.reset();
      return unexpected(ErrorCode::Unsupported,
                        "脚本宿主初始化失败（QuickJS 运行时或 JS 前置加载异常）");
    }
    // 事件桥由 `ScriptHost` 自己在构造时接上（见其构造函数注释）
    ST_LOG_INFO("脚本能力已开启（内存上限 {} MiB / 超时 {} ms）",
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
  ST_LOG_INFO("[gebai] shuangtian app '{}' listening control on {}:{} (backend={}, headless={})",
            name_, options_.control_bind, *port, backend_name(), headless());

  impl_->device_scale = impl_->backend->device_scale();
  impl_->started_ms = time::now_ms();
  impl_->started_ns = time::now_ns();  // 动画时间轴零点（与 started_ms 同源）
  // **拖动缩放时逐帧跟上**（见 `Backend::set_resize_repaint` 的契约）：
  // 用户拖边框走的是窗口系统模态循环，它**阻塞应用主循环**——不在那里驱动渲染，
  // 屏幕就只能停在旧尺寸那一帧上（DXGI 再把它非等比拉到新客户区 → 用户看到的
  // "拖动中拉伸扭曲、松手才重绘"）。这里把"重画一帧"交给后端去调。
  impl_->backend->set_resize_repaint([this] { render_frame(); });
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
  // **布局视口必须在这里对齐后端尺寸**，而不是只靠 `tick()` 那一条。
  //
  // 为什么：拖动缩放期间主循环不转，帧是由窗口过程（`WM_SIZE`）直接驱动的；
  // 那时 `tick()` 根本没机会把新视口同步给 UI 树——布局会按**旧视口**排，
  // 画进新尺寸的画布，表现为"右边/下边一片空"（实测拖动时能看到）。
  // 放在 render_frame 入口还有第二个好处：重建与重绘在**同一帧内**使用一致的尺寸，
  // 不会出现"画在旧画布、present 新画布"的错位。
  const math::Size backend_size = impl_->backend->logical_size();
  if (backend_size.width > 0.0f && backend_size.height > 0.0f) {
    root_.set_viewport(backend_size);
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
  // 拖放重绘走过的尺寸同步：`WM_SIZE` 里已经立即重画过一帧（因为模态循环期间主循环
  // 不转），这里只负责把**布局视口**改到同一条线上——两边不一致的话，下一帧仍会
  // 按旧视口排布（表现为"松手后内容才重新排列"）。
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
  // 先退订日志（再动 `impl_`）：回调抓的是 `this`，注销后便不再有人调用它。
  if (impl_ != nullptr && impl_->log_listener_id != 0) {
    log::remove_listener(impl_->log_listener_id);
    impl_->log_listener_id = 0;
  }
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
  ST_LOG_INFO("应用退出：共 {} 帧，最后一帧 {:.2f}ms（排版 {:.2f} / 绘制 {:.2f} / 送显 {:.2f}）",
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

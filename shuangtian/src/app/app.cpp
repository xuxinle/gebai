#include "st/app/app.hpp"

#include "st/app/text_port.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <string>

#include "st/codec/png.hpp"
#include "st/core/fs.hpp"
#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/core/process.hpp"
#include "st/core/string.hpp"
#include "st/core/time.hpp"
#include "st/core/wait.hpp"
#include "st/core/json.hpp"
#include "st/perf/frame.hpp"
#include "st/perf/startup.hpp"
#include "st/perf/report.hpp"
#include "st/text/text.hpp"
#include "st/ui/builtin_themes.hpp"
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

namespace {

/// 启动卡自适应的**跨启动记忆**文件路径。
///
/// ## 为何要落盘（2026-11-08）
///
/// “首帧要算多久”只有算完才知道，而下次启动需要它——所以必然跨进程。
/// 只存在成员里等于每次都是 0（永远拿不到历史）。
///
/// ## 为何放临时目录而不放应用数据目录
///
/// 这是个**纯启发值**（用来决定动画播多久），丢了最坏后果是下次按冷启动估计，
/// 而应用数据目录里放它需要先有一整套目录约定。先用最小机制：
/// `%TEMP%/st-splash-<app>.txt`（Linux/macOS 对应 `$TMPDIR`/`/tmp`）。
[[nodiscard]] auto splash_memory_path(std::string_view app_name) -> std::string {
  // 应用名进文件名：多个应用各记各的（裁云慢不代表 gallery 也慢）。
  // 非法字符换成 '-'：应用名来自构造参数，不该让它把路径弄坏。
  std::string safe(app_name);
  for (char& ch : safe) {
    const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                    (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
    if (!ok) ch = '-';
  }
  if (safe.empty()) safe = "app";
  std::string base = fs::temp_dir();
  if (base.empty()) base = ".";   // 拿不到临时目录时的退路（宁可写在当前目录，也不要丢掉整个机制）
  return fs::join(base, "st-splash-" + safe + ".txt");
}

/// 读上次记下的首帧耗时（毫秒；读不到/解析失败 → 0）。
[[nodiscard]] auto read_splash_memory(std::string_view app_name) -> double {
  const auto text = fs::read_text(splash_memory_path(app_name));
  if (!text.has_value()) return 0.0;
  const auto value = parse_f64(*text);
  if (!value.has_value() || *value <= 0.0) return 0.0;
  // 上界夹取：文件可能被手改/损坏，离谱值会让卡播到天荒地老。
  return std::min(*value, 30000.0);
}

/// 写回本次的首帧耗时（**失败静默**：这是启发值，不是契约）。
void write_splash_memory(std::string_view app_name, double ms) {
  if (ms <= 0.0) return;
  (void)fs::write_text(splash_memory_path(app_name), std::format("{:.1f}", ms));
}

}  // namespace

auto resolve_text_lcd(std::string_view mode) -> bool {
  if (const auto parsed = parse_bool_word(mode); parsed.has_value()) return *parsed;
  if (const auto value = fs::read_env("ST_TEXT_LCD"); value.has_value() && !value->empty()) {
    if (const auto parsed = parse_bool_word(*value); parsed.has_value()) return *parsed;
  }
  // **默认亚像素**（见 `resolve_text_lcd` 头注的实测依据）。
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

/// 同 `scaled_theme`，但以**给定主题**为底（用于内置主题库）。
///
/// 字号缩放必须与内置两档走**同一条路**：`scale_fonts` 是等比缩放，
/// 漏掉它会让"用内置主题时 UI 字号缩放不生效"——而那个现象很难一眼看出。
[[nodiscard]] auto scaled_theme_from(ui::Theme base, float factor) -> ui::Theme {
  base.metrics().scale_fonts(factor);
  return base;
}

/// 把名字列表拼成 `a / b / c`（错误消息用）。
[[nodiscard]] auto join_names(const std::vector<std::string_view>& names) -> std::string {
  std::string out;
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (i != 0) out += " / ";
    out += names[i];
  }
  return out;
}

/// 解析生效的自定义主题文件（命令行 > `ST_THEME_FILE`）。
[[nodiscard]] auto resolve_theme_file(std::string_view configured) -> std::string {
  if (!configured.empty()) return std::string(configured);
  const auto from_env = fs::read_env("ST_THEME_FILE");
  return from_env.has_value() ? *from_env : std::string{};
}

/// 定长**环形缓冲**（帧时序 / 日志尾部的替代，2026-10-14）。
///
/// 为何不用 `std::vector` + `erase(begin())`：那是一次 O(n) `memmove`，
/// 而它在**每帧**都要做（`wall_times` / `frame_times` 各一次）——量小但纯浪费，
/// 而且它给热路径引入了与窗口大小相关的隐藏成本。
/// 环形缓冲把"丢弃最旧"变成"覆盖一个下标"（O(1)、无搬动）。
///
/// 读取时用 `for_each_oldest_first`（而非返回容器）：调用方都是"读一遍做统计/输出"，
/// 而暴露内部布局会让两种状态（未满 / 已回绕）外溢到每个调用点。
///
/// 为何不用 `std::deque`：它每元素一次分散分配，帧环路径上反而更贵（本仓已有
/// “热路径不用 deque”的先例：终端回滚缓冲）。
struct RingBuffer {
  std::vector<double> items{};
  std::size_t next{0};      ///< 下一个写入位置
  std::size_t filled{0};    ///< 已写入数量（<= capacity）

  void set_capacity(std::size_t capacity) { items.resize(capacity); }

  void push(double value) {
    if (items.empty()) return;      // 容量未设：静默忽略（测试/极简路径）
    items[next] = value;
    next = (next + 1) % items.size();
    if (filled < items.size()) ++filled;
  }

  /// 按**从旧到新**顺序调用 `visit`（与旧的 `vector` 顺序一致，调用方无需区分回绕）。
  template <class Fn>
  void for_each_oldest_first(Fn&& visit) const {
    const std::size_t begin = (filled == items.size()) ? next : 0;
    for (std::size_t offset = 0; offset < filled; ++offset) {
      visit(items[(begin + offset) % (items.empty() ? 1 : items.size())]);
    }
  }
};

/// 字符串版环形缓冲（日志尾部）：同 `RingBuffer`，但元素不可拷贝构造成本低。
struct LogRing {
  std::vector<std::string> items{};
  std::size_t next{0};
  std::size_t filled{0};

  void set_capacity(std::size_t capacity) {
    if (capacity > 0) items.assign(capacity, std::string{});
  }

  void push(std::string value) {
    if (items.empty()) return;
    items[next] = std::move(value);
    next = (next + 1) % items.size();
    if (filled < items.size()) ++filled;
  }

  /// 取最近 `limit` 条（从旧到新）。
  [[nodiscard]] auto tail(std::size_t limit) const -> std::vector<std::string> {
    const std::size_t count = std::min(limit, filled);
    const std::size_t begin_index = filled - count;
    const std::size_t start = (filled == items.size() ? next : 0) + begin_index;
    std::vector<std::string> out;
    out.reserve(count);
    for (std::size_t offset = 0; offset < count; ++offset) {
      out.push_back(items[(start + offset) % (items.empty() ? 1 : items.size())]);
    }
    return out;
  }
};

/// 帧时序样本上限（p50/p95 用它算）。
inline constexpr std::size_t kFrameSampleCapacity = 240;
/// 日志尾部保留行数（控制通道 `logs` 用）。
inline constexpr std::size_t kLogLineCapacity = 512;

struct Application::Impl {
  shell::Backend* backend{nullptr};
  std::unique_ptr<shell::Backend> backend_holder{};
  /// 托盘是否真启用（后端不支持时保持 false——如实反映，不假装）。
  bool tray_ready{false};
  std::unique_ptr<st::text::FontStack> fonts{};
  std::unique_ptr<st::text::TextRenderer> renderer{};
  std::unique_ptr<RendererTextPort> text_port{};
  std::unique_ptr<control::Server> server{};
  /// 脚本宿主（仅 `AppOptions::enable_script` 时创建；协议层通过 `Application::script()` 取用）。
  ///
  /// `ST_NO_SCRIPT` 时降为裸指针（恒空）：unique_ptr<完整类型> 的删除器会在 Impl 的
  /// 构造/析构里实例化 `delete` → 引用 `ScriptHost::~` → 整条 quickjs 链被拉回产物
  ///（实测 318KB，按需链接的拆链就被这一个成员破坏）。裸指针不拥有，也无析构引用。
#ifdef ST_NO_SCRIPT
  ui::ScriptHost* script{nullptr};
#else
  std::unique_ptr<ui::ScriptHost> script{};
#endif
  RingBuffer frame_times{};
  /// 相邻帧开始时刻之差（墙钟间隔；与 `frame_times` 的帧内耗时对照看）。
  RingBuffer wall_times{};
  std::int64_t last_frame_start_ns{0};
  LogRing log_lines{};
  std::uint64_t frames{0};
  float device_scale{1.0f};
  /// 工作区逻辑尺寸覆盖（`AppOptions::work_area_override`；0 = 问后端）。
  math::Size work_area_override{0.0f, 0.0f};
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
  /// 启动期动画的**泵点**：由 `render_frame` 在布局阶段之后调。
  ///
  /// ## 为何要从 `render_frame` 内部泵（2026-11-08，用户“动画没了”）
  ///
  /// 首帧的 ~330ms 花在 `layout()` + `paint_frame()` 里（字体首次光栅化 +
  /// 图元缓存建表）。如果只在 `render_frame` **前后**泵动画帧，实测结果是：
  ///
  /// | 时刻 | 屏上 |
  /// |---|---|
  /// | 162ms | 卡（第 1 帧） |
  /// | 234~370ms | **diff=0，完全静止**（主线程在画首帧） |
  /// | 709ms | 已恢复真实尺寸 |
  ///
  /// 即动画只动了一下就定住——用户看到的就是一张静态图。
  /// 而 `layout` → `paint` 的边界是天然泵点：此时画布上还是**卡**（界面还没开始画），
  /// 泵一帧卡不会与任何东西冲突。
  ///
  /// ⚠ **`paint` 之后不能泵**：那时画布上已经是界面内容，重画卡会把界面盖掉，
  /// 而 `paint_frame` 的局部上屏假设也会被打破（“画布已被整块改写”）。
  std::function<void()> startup_frame_pump{};
  /// 启动卡的磨砂探测结果（`start()` 里算一次，`paint_splash_once` 直接用）。
  shell::BackdropSupport splash_backdrop_{shell::BackdropSupport::Unsupported};
  /// 启动期是否真的开了**分层窗口**（逐像素 alpha 上屏）。
  ///
  /// 决定 `SplashState::transparent_outside`：分层 → 卡外真透明（只有卡悬浮）；
  /// 不开 → 填主题底色（不透明卡，退路）。
  bool splash_layered_{false};
  /// **上一轮的首帧耗时**（毫秒；0 = 还没量到）。
  ///
  /// ## 为何要记它（2026-11-08，启动卡自适应）
  ///
  /// 启动卡该播多久，取决于“首帧要算多久”——而那个数**只有算完才知道**。
  /// 所以它必然是一个**跨启动的记忆**：本次用它决定本次播多久，本次算完后写回。
  ///
  /// 为何不改用“首次启动就慢、后续快”那套静态假设：用户报的场景正是
  /// “首次慢、之后快”，而卡住看不清动画的是首次——固定值一定会在其中一种下错。
  ///
  /// 存的是**首帧本身**（`first_frame` 里程碑），不含启动卡与建窗时间：
  /// 那些与本决策无关（启动卡自己就是被决定的对象，不能把自己算进预期）。
  double last_first_frame_ms{0.0};
  /// **启动卡从显示到主界面就绪的真实时长**（毫秒；0 = 还没量到）。
  ///
  /// ## 为何是这一对时刻（2026-11-08，第三次修正口径）
  ///
  /// 自适应要回答的是“用户在卡上看多久”，那就必须量**用户真在等的那段**：
  ///
  /// | 候选口径 | 量到 | 为何不对 |
  /// |---|---|---|
  /// | `now - started_ns` | 883→1277ms 递增 | 把卡自己播的时间也算进去 → 正反馈恶化 |
  /// | `render_frame()` 耗时 | **5ms** | 那是“画一帧”，不是“等多久”——差两个数量级 |
  /// | **`show_early → show_when_ready`** | ~700~900ms | ✅ 就是用户看卡的那段 |
  ///
  /// 前两个都是我先选错的口径，实测才发现——所以这里把三个值都记下来，
  /// 免得下次又拿错一个数。
  std::int64_t splash_shown_ns{0};
  /// **本轮启动卡动画自己占用的时长**（毫秒）。
  ///
  /// 用途：从“卡在屏时长”里扣掉它，剩下的才是“界面准备耗时”
  ///（见 `show_when_ready` 之后那段注释：不扣就会正反馈累加）。
  double splash_anim_ms{0.0};
  /// 启动卡是否真的会发生（判据集中在这一处，实现与日志/测试同源）。
  ///
  /// 条件（任一不成立就不画）：
  /// - `AppOptions::splash` 为真（应用没关掉）；
  /// - 有真窗口（无头没有要盖的那块空白）；
  /// - 用自绘窗框（`decorations` 为假）——系统窗框下客户区只是窗口的一部分，
  ///   把整张卡铺满客户区会与系统标题栏打架。
  [[nodiscard]] auto splash_enabled(const AppOptions& options) const noexcept -> bool {
    return options.splash && backend != nullptr && !backend->headless() && !options.decorations;
  }
  /// 绘制剖析器（`ST_PAINT_PROFILE=1` 时才挂到帧缓冲画布上）。
  raster::PaintProfiler profiler{};
  bool profiling{false};
  /// 帧剖析器（`--perf-frames N` / `ST_PERF=1` 时创建；同时会把自己设为
  /// 进程级"当前剖析器"，供 `Element::paint` 取用）。
  std::unique_ptr<perf::FrameProfiler> frame_profiler{};
  /// 日志订阅 id（构造时注册、析构时注销）——见 `Application::Application` 的说明。
  std::uint64_t log_listener_id{0};
  /// 应用异步任务系统（`tasks()` 的落点）。住 `Impl`：
  /// `Tasks` 不可移动且持有线程——必须比 `root_` 后析构（任务可能还在投递 UI 回调，
  /// 而 UI 队列的 pump 依赖 root 活着——析构顺序 `impl_` 先于 `root_` 声明序即可）。
  app::Tasks tasks{};
};

Application::Application(std::string name, std::string version, AppOptions options)
    : impl_(std::make_unique<Impl>()), name_(std::move(name)), version_(std::move(version)),
      options_(std::move(options)) {
  log::set_level(log::level_from_name(options_.log_level));
  // 帧时序与日志尾部改用环形缓冲：容量一次给定（旧实现是"超了 erase(begin())"）
  impl_->frame_times.set_capacity(kFrameSampleCapacity);
  impl_->wall_times.set_capacity(kFrameSampleCapacity);
  impl_->log_lines.set_capacity(kLogLineCapacity);
  impl_->font_scale = resolve_ui_font_scale(options_.ui_font_scale);
  impl_->work_area_override = options_.work_area_override;
  // 初始主题必须走 **`compose_theme`**（内置主题名 → 文件覆盖 → 运行期覆盖）：
  // 直接 `scaled_theme(options_.theme, …)` 会把 `--theme cyberpunk` 这类
  // 内置主题名全部忽略掉，而症状是“界面能开、主题不生效”——
  // 难查是因为不报错（实测：`theme` 命令返回 name="dark"）。
  {
    const auto initial = compose_theme(options_.theme);
    if (!initial) {
      ST_LOG_WARN("初始主题构建失败，已退回内置基准：{}", initial.error().message);
      root_.set_theme(scaled_theme(options_.theme, impl_->font_scale));
    } else {
      root_.set_theme(*initial);
    }
  }
  // 主题切换回调：组件（如标题栏 ThemePicker）经 `host_apply_theme` 触发时，
  // 走与控制通道/JS 同一条 `set_theme_name` 管线（合成/覆盖叠加/换调色都一致）。
  // 接在初始化后：回调里调的 `set_theme_name` 依赖 root_/renderer 已就绪。
  root_.theme_applier = [this](std::string_view theme_name) {
    return set_theme_name(theme_name).has_value();
  };
  // 绘制剖析默认关闭：挂了才计时（代价是每次绘制两次时钟读）。
  if (const auto flag = fs::read_env("ST_PAINT_PROFILE"); flag.has_value() && !flag->empty() &&
      *flag != "0") {
    impl_->profiling = true;
  }
  // 帧剖析：两个开法殊途同归（都归到一个剖析器），因为使用者是两种人：
  // ① `ST_PERF=1`：环境变量，适合"我怀疑这个应用慢"的排查现场；
  // ② `--perf-frames N`：命令行显式声明要抓多少帧，适合自动化/报告产物。
  if (options_.profile_frames > 0 || perf::enabled()) {
    impl_->frame_profiler = std::make_unique<perf::FrameProfiler>(
        options_.profile_frames > 0 ? options_.profile_frames : std::size_t{600});
    perf::set_active_frame_profiler(impl_->frame_profiler.get());
    // 剖析必然要读时钟：环境变量只给 `--perf-frames` 时也要真正开启。
    if (!perf::enabled()) perf::set_enabled(true);
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
    impl_->log_lines.push(std::format("[{}] {}", to_string(level), message));
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
  // 优先级：内置主题的调色板 → 主题文件稀疏覆盖 → 运行期 `theme.set`。
  //
  // 为何内置主题要在这里而不是当成一份“完整的 theme_spec”：
  // `theme_name` 选定的是**基准调色板**（包含“不是 token 的派生量”，如字体 gamma），
  // 而 `theme_spec` 是稀疏覆盖。两者叠在一起才是“选一套主题再微调”。
  ui::Theme theme = scaled_theme(mode, impl_->font_scale);
  if (!options_.theme_name.empty()) {
    auto builtin = ui::builtin_theme(options_.theme_name);
    if (!builtin.has_value()) {
      return unexpected(ErrorCode::Invalid,
                        std::format("未知内置主题：{}（可用：{}）", options_.theme_name,
                                    join_names(ui::builtin_theme_names())));
    }
    theme = scaled_theme_from(*builtin, impl_->font_scale);
    ST_LOG_INFO("主题：已应用内置主题 {}（bg={:08X} surface={:08X}）", options_.theme_name,
                theme.colors().bg.to_hex(), theme.colors().surface.to_hex());
  }
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

auto Application::script() -> ui::ScriptHost* {
#ifdef ST_NO_SCRIPT
  return nullptr;
#else
  return impl_->script.get();
#endif
}

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

  [[nodiscard]] auto Application::window_restore() -> bool {
    if (impl_->backend == nullptr) return false;
    const auto status = impl_->backend->restore();
    // 措辞上的区分很重要（实测踩过）：后端失败的形态有多种——
    // 「已置顶但没拿到键盘焦点」（后台弹卡被前台锁拒的典型形态，**窗口已经在人眼前了**）
    // 与「置顶也失败」（真异常）不是一回事。前者对“叫人看一眼”这个目的已经够了，
    // 只是写不进前景焦点；不当成错误，只记 info，免得每次弹卡都响一条 warn 噪声。
    if (!status) {
      const std::string msg = status.error().message;
      if (msg.find("已置到最上层") != std::string::npos) {
        ST_LOG_INFO("恢复窗口：{}", msg);
      } else {
        ST_LOG_WARN("恢复窗口：{}", msg);
      }
    }
    // 恢复后**清除 UI 焦点与悬停残留**：
    // - 焦点：关窗时焦点停留在关闭按钮上（用户点它关的），直接恢复会把焦点环带回；
    // - 悬停：关窗时鼠标在关闭按钮上，窗口隐藏期间没有新的 MouseMove，`hovered_`
    //   一直指向关闭按钮——恢复后按钮继续显示 hover 红色高亮，看起来像「聚焦/选中」
    //   了它（实测现象：鼠标没到窗口、关闭按钮红色）。两者都不是「窗口回来了」
    //   该有的样子，清掉后等鼠标真的移动再重新 hover。
    (void)root_.set_focus(nullptr);
    root_.clear_hover();
    return status.has_value();
  }

auto Application::set_window_size(int width_logical, int height_logical) -> Status {
  if (impl_->backend == nullptr) return unexpected(ErrorCode::Unsupported, "后端未就绪");
  if (auto status = impl_->backend->set_size(width_logical, height_logical); !status) {
    return status;
  }
  // 后端改的是物理窗口，逻辑视口也要跟着改——否则布局仍按旧尺寸算，
  // 现象是“窗口变小了但内容还是老宽高”（裁掉一大半）。
  root_.set_viewport(
      math::Size{static_cast<float>(width_logical), static_cast<float>(height_logical)});
  root_.mark_dirty_all();
  return ok();
}

auto Application::control_port() const noexcept -> std::uint16_t {
  return impl_->server != nullptr ? impl_->server->port() : 0;
}

auto Application::paint_profile() const -> const raster::PaintProfiler* {
  return impl_->profiling ? &impl_->profiler : nullptr;
}

auto Application::perf_report() const -> std::optional<perf::Report> {
  if (impl_->frame_profiler == nullptr) return std::nullopt;
  perf::MeasurementContext context{};
  context.app = app_name();
  context.app_version = app_version();
  context.backend = std::string(backend_name());
  // 构建档：`ST_BUILD_PROFILE` 由 `st` 在编译期注入；没有时按 `NDEBUG` 猜——
  // 猜错会让报告把 debug 档的数字当成 release（这正是报告 `caveats` 要防的误读）。
  if (const auto profile = fs::read_env("ST_BUILD_PROFILE"); profile.has_value()) {
    context.profile = *profile;
  }
  if (context.profile.empty()) {
#if defined(NDEBUG)
    context.profile = "release";
#else
    context.profile = "dev";
#endif
  }
  const control::Metrics snapshot = metrics();
  context.renderer = snapshot.renderer;
  context.headless = snapshot.headless;
  context.device_scale = snapshot.device_scale;
  context.width = static_cast<int>(root_.viewport().width);
  context.height = static_cast<int>(root_.viewport().height);
  return impl_->frame_profiler->report(context);
}

auto Application::perf_frames_csv() const -> std::string {
  if (impl_->frame_profiler == nullptr) return {};
  return impl_->frame_profiler->frames_csv();
}

void Application::perf_reset() {
  if (impl_->frame_profiler != nullptr) impl_->frame_profiler->reset();
}

auto Application::perf_active() const noexcept -> bool {
  return impl_->frame_profiler != nullptr;
}

auto Application::perf_report_json(std::uint32_t capacity) -> std::string {
  if (capacity > 0) {
    // “我要采更多帧”：容量变了就重建剖析器（旧样本随之丢弃，但这也是调用方
    // 的意图——它刚要求一个新的采样窗口）。
    if (impl_->frame_profiler == nullptr || impl_->frame_profiler->capacity() != capacity) {
      impl_->frame_profiler = std::make_unique<perf::FrameProfiler>(capacity);
      perf::set_active_frame_profiler(impl_->frame_profiler.get());
      perf::set_enabled(true);
    }
  }
  const auto report = perf_report();
  if (!report.has_value()) {
    // 未开剖析时返回一份**说清楚为什么没有数据**的报告，而不是空串：
    // 调用方（模型/脚本）看到空串只能猜，看到 caveat 就知道下一步该干什么。
    perf::Report shell{};
    shell.kind = "frame";
    shell.context.app = app_name();
    shell.context.app_version = app_version();
    shell.context.backend = std::string(backend_name());
    perf::Caveat caveat{};
    caveat.kind = "not_profiling";
    caveat.ok = false;
    caveat.detail = "帧剖析未开启（无样本）";
    caveat.fix = "控制通道 perf action=start（或启动时 --perf-frames N）";
    shell.caveats.push_back(std::move(caveat));
    return perf::to_json(shell);
  }
  return perf::to_json(*report);
}

auto Application::perf_set_active(bool active, std::uint32_t capacity) -> bool {
  if (active) {
    if (capacity > 0 && (impl_->frame_profiler == nullptr ||
                         impl_->frame_profiler->capacity() != capacity)) {
      impl_->frame_profiler = std::make_unique<perf::FrameProfiler>(capacity);
    }
    perf_start(capacity);
  } else {
    perf_stop();
  }
  return true;
}

void Application::perf_reset_samples() { perf_reset(); }

void Application::perf_start(std::uint32_t capacity) {
  if (impl_->frame_profiler == nullptr) {
    impl_->frame_profiler =
        std::make_unique<perf::FrameProfiler>(capacity > 0 ? capacity : std::uint32_t{600});
  }
  perf::set_active_frame_profiler(impl_->frame_profiler.get());
  // 剖析本身要读时钟：`ST_PERF` 没开时也要把它打开（“按需开”的语义就在这里）。
  perf::set_enabled(true);
}

void Application::perf_stop() {
  // 只摘掉“当前剖析器”并关闭探针，**不销毁**剖析器：停采样后仍要能取报告
  // （“跑了 30 秒，现在停下来看看结果”是最常见的用法）。
  perf::set_active_frame_profiler(nullptr);
  perf::set_enabled(false);
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

auto Application::set_theme_name(std::string_view name) -> Status {
  // 名字必须在内置库里才生效（不猜、不静默回退：选错主题要让调用方看见）。
  if (!ui::builtin_theme(std::string(name)).has_value()) {
    return unexpected(ErrorCode::Invalid,
                      std::format("未知内置主题：{}（可用：{}）", name,
                                  join_names(ui::builtin_theme_names())));
  }
  // 与 `set_theme_mode` 同一管线：`theme_name` 是基准调色板，`theme_spec`/
  // `runtime_overlay` 继续在其上叠加（换主题不丢调色微调）。
  const std::string previous = options_.theme_name;
  options_.theme_name = std::string(name);   // 先换名字再合成：compose 读的正是它
  const auto composed = compose_theme_with(options_.theme, impl_->runtime_overlay);
  if (!composed) {
    options_.theme_name = previous;   // 合成失败：还原名字（“先校验后落地”）
    return forward_error(composed.error());
  }
  custom_theme_ = *composed;
  root_.set_theme(custom_theme_);
  if (impl_->renderer != nullptr && options_.text_gamma.empty()) {
    impl_->renderer->set_coverage_gamma(default_gamma_for(options_.theme));
  }
  return ok();
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

auto Application::tray_ready() const noexcept -> bool { return impl_->tray_ready; }

auto Application::take_tray_clicks() noexcept -> std::uint32_t {
  if (!impl_->tray_ready || impl_->backend == nullptr) return 0;
  return impl_->backend->take_tray_clicks();
}

auto Application::set_tray_tooltip(std::string_view tooltip) -> Status {
  if (!impl_->tray_ready || impl_->backend == nullptr) {
    return unexpected(ErrorCode::Unsupported, "托盘未启用（后端不支持，或启动时 tray_enabled 为 false）");
  }
  // 空串 = 用窗口标题（与启动时的口径一致）
  const std::string text = tooltip.empty() ? options_.title : std::string(tooltip);
  return impl_->backend->set_tray_icon({}, text);
}

auto Application::log_lines(std::size_t limit) const -> std::vector<std::string> {
  return impl_->log_lines.tail(limit);
}

auto Application::metrics() const -> control::Metrics {
  control::Metrics metrics;
  metrics.frames = impl_->frames;
  // 字体加载方式（映射 / 回退整读）：内存改造后最重要的可观测项——
  // “为什么这台机器多占 80MB”的答案就在这里。
  {
    const auto font_stats = st::text::font_cache_stats();
    metrics.font_faces_loaded = font_stats.face_loads;
    metrics.font_mapped_loads = font_stats.mapped_loads;
    metrics.font_read_fallbacks = font_stats.read_fallbacks;
  }
  metrics.last_frame_ms = impl_->last_frame_ms;
  // 几何 sprite 缓存命中率（软件光栅器专属；GPU 画布不实现，保持 0）。
  if (auto* software = dynamic_cast<raster::Canvas*>(&impl_->backend->framebuffer())) {
    const raster::Canvas::GeometryStats stats = software->geometry_stats();
    metrics.sprite_hits = stats.hits;
    metrics.sprite_misses = stats.misses;
    metrics.sprite_rejected = stats.rejected;
    metrics.sprite_entries = static_cast<std::uint64_t>(stats.entries);
  }
  metrics.layout_ms = impl_->layout_ms;
  metrics.paint_ms = impl_->paint_ms;
  metrics.present_ms = impl_->present_ms;
  // “调了 present 但没送到屏幕”的帧数：软渲染路径曾有“早退后 DIB 永不重建”的缺陷，
  // 而旧指标里它与“每帧都正常上屏”完全不可区分（`present_ms` 照样有值）。
  metrics.present_skipped = impl_->backend->present_skipped();
  // p50/p95 从环形缓冲取副本后排序（两个 percentile 块合并成一处：
  // 原先两份代码除容器外逐字相同——那种重复在改口径时必偏一半）。
  const auto percentile_of = [](const std::vector<double>& sorted, double ratio) -> double {
    const auto index = static_cast<std::size_t>(std::min(
        static_cast<double>(sorted.size() - 1), ratio * static_cast<double>(sorted.size() - 1)));
    return sorted[index];
  };
  const auto sorted_samples = [](const RingBuffer& ring) {
    std::vector<double> samples;
    samples.reserve(kFrameSampleCapacity);
    ring.for_each_oldest_first([&samples](double value) { samples.push_back(value); });
    std::ranges::sort(samples);
    return samples;
  };
  if (impl_->frame_times.filled > 0) {
    const std::vector<double> sorted = sorted_samples(impl_->frame_times);
    metrics.frame_p50_ms = percentile_of(sorted, 0.5);
    metrics.frame_p95_ms = percentile_of(sorted, 0.95);
  }
  if (impl_->wall_times.filled > 0) {
    const std::vector<double> sorted = sorted_samples(impl_->wall_times);
    metrics.wall_p50_ms = percentile_of(sorted, 0.5);
    metrics.wall_p95_ms = percentile_of(sorted, 0.95);
  }
  metrics.uptime_ms = time::now_ms() - impl_->started_ms;
  metrics.backend = std::string(backend_name());
  metrics.renderer = std::string(impl_->backend->renderer_name());
  metrics.renderer_note = impl_->backend->renderer_note();
  // 上屏路径（如 X11 的 MIT-SHM vs XPutImage）：它**取决于环境**（扩展库/服务端/
  // 远程转发），回退了得一跟可见——与 `renderer` 同一条理由。
  metrics.present_note = impl_->backend->present_note();
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

auto Application::font_stack() const -> const st::text::FontStack* {
  // 字体在 `start()` 里装载（那里才拿得到真实 DPI 与后端），**构造后、启动前为空**。
  // 返回指针而不是引用：让“还没准备好”成为一个**能被调用方看见**的状态，
  // 而不是在解引用时变成一句断言失败（那是进程直接退出，没有任何上下文）。
  return impl_->fonts.get();
}

auto Application::device_scale() const -> float { return impl_->device_scale; }

auto Application::work_area_logical() const -> math::Size {
  // 覆盖值优先（见 `AppOptions::work_area_override`）：让“高 DPI + 小屏”可复现。
  if (impl_->work_area_override.width > 0.0f && impl_->work_area_override.height > 0.0f) {
    return impl_->work_area_override;
  }
  if (impl_->backend == nullptr) return root_.viewport();
  return impl_->backend->work_area_logical();
}

auto Application::min_logical_size() const -> math::Size {
  if (impl_->backend == nullptr) return math::Size{0.0f, 0.0f};
  return impl_->backend->min_logical_size();
}

auto Application::child_view_supported() const -> bool {
  // 未启动 = 无后端 = 不可能支持（与 `font_stack()` 同一个道理：如实报“还没有”）。
  if (impl_->backend == nullptr) return false;
  return impl_->backend->supports_child_view();
}

auto Application::child_view_probe() const -> shell::ChildViewProbe {
  if (impl_->backend == nullptr) return {};
  return impl_->backend->child_view_probe();
}

auto Application::find_window_by_process(std::uint64_t process_id) -> void* {
  if (impl_->backend == nullptr || process_id == 0) return nullptr;
  return impl_->backend->find_window_by_process(process_id);
}

void Application::dismiss_child_view(void* child) {
  if (impl_->backend == nullptr || child == nullptr) return;
  impl_->backend->dismiss_child_view(child);
}

auto Application::set_device_scale(float scale) -> Status {
  if (impl_->backend == nullptr) return unexpected(ErrorCode::Invalid, "应用未启动");
  if (scale <= 0.0f) return unexpected(ErrorCode::Invalid, "DPI 缩放必须为正数");
  if (auto status = impl_->backend->set_device_scale(scale); !status) {
    return forward_error(status.error());
  }
  impl_->device_scale = impl_->backend->device_scale();
  // **超采样也按实际值**：请求 2.0 但实际只给到 1.5 时，按 2.0 设超采样会把
  // 字形位图画得比实际需要更密（白白多花内存与时间，且与实际观感不符）。
  const float actual_scale = impl_->device_scale;
  // **字形超采样必须跟着 DPI 走**（与构造时同一个入口）。
  //
  // `TextRenderer` 的超采样在构造时由解析出的 DPI 决定（`make_unique<TextRenderer>(fonts,
  // resolved_scale)`），而运行期切换 DPI 只改了缓冲尺寸——于是“启动即 2.0”
  // （超采样 2）与“启动 1.0 再切到 2.0”（超采样停在 1）**渲染结果不同**：
  // 后者每像素只有一个采样点，大字号的斜向/弧形边缘出现可见阶梯（实测同一区域
  // 1524 个像素差 >32）。`set_supersample` 会同时清字形缓存，所以不会混用旧密度的位图。
  if (impl_->renderer != nullptr) impl_->renderer->set_supersample(actual_scale);
  root_.mark_dirty_all();
  if (std::abs(actual_scale - scale) > 0.01f) {
    ST_LOG_WARN("DPI 缩放请求 {} 未能完全生效，实际 {}（物理 {}×{}）",
                static_cast<double>(scale), static_cast<double>(actual_scale),
                impl_->backend->framebuffer().physical_width(),
                impl_->backend->framebuffer().physical_height());
  } else {
    ST_LOG_INFO("DPI 缩放已切换为 {}（物理 {}×{}）", static_cast<double>(actual_scale),
                impl_->backend->framebuffer().physical_width(),
                impl_->backend->framebuffer().physical_height());
  }
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

  // **启动分解的里程碑**：`ST_PERF_STARTUP=1` 时开启（关时每个 mark 只有一次
  // `enabled()` 读，不读时钟）。它们回答的是“首帧那 200ms 花在哪”——
  // 那段不属于任何一帧的阶段（帧剖析里三段加起来才 2ms）。
  perf::startup_mark("app.start");

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
  perf::startup_mark("backend.create");

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
  // ## 建窗即铺到工作区（2026-11-08，建窗一次到位）
  //
  // 开了它，后端会**在建窗前**算出工作区尺寸——省掉“先按请求尺寸分配 1 次画布、
  // 再铺开时分配第 2 次”的那一次 27MB 重分配（实测启动期共 4 次）。
  //
  // ⚠ 仅当应用要铺开（`fill_work_area`）时开：相期那类常驻面板的尺寸是它自己的
  // 语义（“一张 460×620 的卡片”），铺开就毁了。
  window.fill_work_area_on_create = options_.fill_work_area;
  window.work_area_width = static_cast<int>(options_.work_area_override.width);
  window.work_area_height = static_cast<int>(options_.work_area_override.height);
  window.scale = options_.scale;
  window.title = options_.title;
  window.renderer = options_.renderer;
  // 窗框一律自绘（`CONVENTIONS.md` §10 第 7 条）：把开关如实交给后端。
      window.decorations = options_.decorations;
    window.resizable = options_.resizable;
    // 窗口外阴影：自绘窗框丢了系统那圈阴影，默认由后端给回来（见 `WindowOptions::window_shadow`）。
    window.window_shadow = options_.window_shadow;
    // 只在“未显式指定渲染器”时才启用延迟升级（显式 gpu/software 要保持可复现语义）。
    // 判据走 `defer_gpu_effective()` 而不是在这里再写一遍：实现与测试用同一个入口。
    window.defer_gpu = options_.defer_gpu_effective();
    // 关窗→托盘常驻：与托盘同开关（托盘没建起来时关窗仍走退出，避免“窗口没了进程还在”的孤儿）
    window.tray_minimize_on_close = options_.tray_enabled && options_.tray_minimize_on_close;
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
  perf::startup_mark("window.create");

  // ## 启动期间在慢阶段边界**泵一次消息**
  //
  // 现象（用户报）：gbcode 启动时鼠标短暂变成旋转圆圈。
  //
  // 实测（`SendMessageTimeout(WM_NULL)` 探主线程响应性，见
  // `tools/startup_responsiveness_probe.py`）：从建窗到首帧完成这一段，
  // 主线程有 **1008 ms** 不回消息（探测超时、`ok=0`），而窗口此时已创建。
  // Windows 的判定是：进程长时间不响应基本消息 ⇒ 标 Ghost，光标变等待圈。
  //
  // 为何阻塞：这一段要做字体首次光栅化、画布分配、GPU 设备后台就绪
  // （本机实测 GPU 就绪 ~400ms）——全在主线程。
  //
  // 修法：在慢阶段的边界上泵消息。**只泵不处理业务**：
  // 这里 `started_` 还是 false，任何输入事件都不应生效（界面尚未就位），
  // 所以只泵不派——`pump_events()` 只做 `pump_messages()`，不取事件。
  //
  // ⚠ **实测结论：单靠边界泵消息不足以消除等待圈**。首帧内部
  // （字体光栅化 + 图元缓存建表，实测 ~1.4s）是一整块不可分割的工作，
  // 泵点只能在它之前/之后生效。真正的修法是让首帧变快或提前显窗，
  // 见 `show_early` 与 `docs/BACKLOG.md` 里“启动慢”的条目。
  impl_->backend->pump_events();

  // **软件渲染（WARP）帧预算自适应**（2026-10-29，后收敛为仅日志）：
  //
  // 背景：WARP（CPU 模拟 GPU）下曾因「每事件整窗重绘 + 整纹理 CopyResource」
  // 导致鼠标一动 CPU 拉满（实测 382% 单核）。第一阶段曾用「16ms→33ms 降帧」
  // 压制症状，但那是钝刀：hover 过渡被限到 15fps，观感变差。
  //
  // 现在根治手段已落地——win32 局部上屏（`set_present_region` →
  // `CopySubresourceRegion`，与 X11 同能力）：hover 只拷 32×32 的损坏区，
  // 整窗 6.7MP 的 CPU 拷贝消失。降帧预算不再需要，恢复 60fps；只留这条
  // 日志让“这台机器在用 WARP”成为可观察事实（排障时不用猜）。
  if (options_.frame_budget_ms <= 16.0) {
    const std::string note = impl_->backend->renderer_note();
    const bool software_raster =
        note.find("WARP") != std::string::npos ||
        note.find("Basic Render Driver") != std::string::npos;
    if (software_raster) {
      ST_LOG_INFO("软件渲染（{}）：已启用局部上屏，帧预算维持 16ms", note);
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

  // 系统托盘（可选）：常驻类应用的核心形态。
  //
  // 为何不失败就退出：与窗口图标同理——托盘是形态能力，不是运行前提。
  // 后端不支持（无头/未实现的平台）时只告警，应用照常跑（降级为普通窗口）。
  if (options_.tray_enabled) {
    const auto icon = options_.tray_icon_png.empty() ? options_.window_icon_png
                                                     : options_.tray_icon_png;
    const std::string tooltip =
        options_.tray_tooltip.empty() ? options_.title : options_.tray_tooltip;
    if (icon.empty()) {
      ST_LOG_WARN("托盘未启用：缺少图标 PNG（tray_icon_png / window_icon_png 都为空）");
    } else if (auto status = impl_->backend->set_tray_icon(icon, tooltip); !status) {
      // 如实告知降级：应用照常跑（当普通窗口用），但“为什么没图标”要能找到原因
      ST_LOG_WARN("系统托盘未启用（降级为普通窗口）：{}", status.error().message);
    } else {
      impl_->tray_ready = true;
    }
  }

  // **把窗口夹到屏幕工作区内**（DPI 适配的必需品）。
  //
  // 为何在建窗之后：工作区尺寸（含任务栏扣除）与“窗口落在哪块屏”只有建窗之后
  // 才拿得到（多显示器下每块屏的工作区不同）。
  //
  // 为何必须夹：窗口尺寸若写死逻辑值，在高 DPI 的小屏上会**超屏**——
  // 13 寸 1080p 开 200% 缩放时工作区只有 ≈960×540 逻辑像素，而应用要 1280×800，
  // 窗口被系统裁切/挤压，界面看着“错乱且不可用”。这里把尺寸收敛到
  // `[最小可用, 工作区 × 0.92]`：留一点边距让窗口不贴死屏幕边。
  //
  // 两条路径：后端能改窗口就走后端（它才知道客户区/边框/DPI 换算）；
  // 无头/拿不到屏幕时用 **work_area_override** 自己夹（让“小屏高 DPI”可复现）。
  // **把窗口开到屏幕工作区**（DPI 适配的主路径）。
  //
  // 为何在建窗之后：工作区尺寸（含任务栏扣除）与“窗口落在哪块屏”只有建窗之后
  // 才拿得到（多显示器下每块屏的工作区不同）。
  //
  // 为何是“铺开”而不是“超屏才缩”：实测用户现场——屏幕 3840×2160、DPI 1.5
  // ⇒ 工作区 2560×1392 逻辑，而应用期望 1440×900。旧实现只夹上限，于是窗口
  // 就停在 1440×900（**占屏 56%×65%**），右边与下方全是桌面，用户描述为
  // “页面混乱、不可用”。桌面应用该像 IDEA/VSCode 那样按可用区域铺开。
  //
  // 无条件调用（不再判断“是否超屏”）：尺寸收敛由后端负责
  // `[min_logical_size, 工作区 × 0.92]`，这里不重复算口径。
  //
  // `fill_work_area = false` 的应用**跳过这一步**：常驻面板/轻提示的尺寸是
  // 它自己的语义（“一张 460×620 的卡片”），铺开到工作区会直接毁掉它。
  if (options_.fill_work_area) {
    if (const auto fitted = impl_->backend->fit_window_to_work_area(); !fitted) {
      // 无头/拿不到屏幕不是错误：退回 `work_area_override` 的语义。
      const math::Size work = work_area_logical();
      const math::Size current = impl_->backend->logical_size();
      if (work.width > 0.0f && work.height > 0.0f) {
        ST_LOG_WARN("窗口未自适应（{}），当前 {}×{} 工作区 {}×{}", fitted.error().message,
                    current.width, current.height, work.width, work.height);
      }
    }
  } else {
    // 不铺开，但**仍要夹上限**：请求尺寸超过屏幕时必须缩下来（否则窗口大部分在屏外）。
    // 只缩不放大：工作区取不到（无头/覆盖值）时直接跳过。
    const math::Size work = work_area_logical();
    const math::Size current = impl_->backend->logical_size();
    if (work.width > 0.0f && work.height > 0.0f &&
        (current.width > work.width || current.height > work.height)) {
      const int capped_w = static_cast<int>(std::lround(
          current.width > work.width ? work.width : current.width));
      const int capped_h = static_cast<int>(std::lround(
          current.height > work.height ? work.height : current.height));
      (void)impl_->backend->set_size(capped_w, capped_h);
      ST_LOG_INFO("窗口超屏，已夹到工作区：{}×{}（请求 {}×{}）", capped_w, capped_h,
                  current.width, current.height);
    }
  }

  // **新窗口一律居中**（与上面那个分支无关：铺不铺开都要居中）。
  //
  // 为何在“尺寸定下来之后”：居中要按窗口的**最终尺寸**算，先移后改尺寸会让
  // 窗口偏到一边（长宽各差一半）。所以两条尺寸路径（铺开 / 只夹上限）都做完，
  // 这里再一次性定位置。
  //
  // 为何不让各后端在建窗时自己居中：三个后端的做法不同（Win32 只能在
  // `WM_*` 里改、X11 发一个请求、Wayland 根本没得选），而“居中”是**产品口径**，
  // 不是平台细节。实测旧状态就是分家的：只有 Win32 在 `fit_window_to_work_area`
  // 里顺手居中，于是 `fill_work_area = false` 的常驻面板与所有 X11 应用开局
  // 都落在屏幕左上角（(0,0)）。
  if (const auto centered = impl_->backend->center_window(); !centered) {
    // 无头（没有屏幕）/Wayland（位置归合成器）不是错误，只记一句供排障。
    ST_LOG_INFO("窗口未居中（{}）", centered.error().message);
  }

  // 字体栈首次装配（实测 63.5 ms：要读多个 TTC 的表目录）——泵一次消息。
  impl_->backend->pump_events();
  auto stack = st::text::FontStack::system_default();
  if (stack) {
    // 字体栈就绪后再泵一次：后面是 TextRenderer 构造 + 首帧字形光栅化，
    // 那一段是启动里最大的单笔阻塞。
    impl_->backend->pump_events();
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
    // ── 下面三项此前**只接到探针上**，应用层漏接——实测后果：
    //    gbcode 截图的彩边均值 89.6，而探针同条件只有 ~40（差一倍多），
    //    即“探针调好的参数，真实应用根本没生效”。
    // ──
    // **亚像素强度**（向灰度收缩）：默认 0.5，复刻 Chromium 的 ClearTypeLevel。
    // 环境变量 `ST_TEXT_SUBPIXEL_STRENGTH` 可覆盖（诊断用）。
    {
      float strength = st::text::TextRenderer::subpixel_strength_default();
      if (const auto value = fs::read_env("ST_TEXT_SUBPIXEL_STRENGTH");
          value.has_value() && !value->empty()) {
        try {
          strength = std::stof(*value);
        } catch (const std::exception&) {
          strength = st::text::TextRenderer::subpixel_strength_default();
        }
      }
      impl_->renderer->set_subpixel_strength(strength);
    }
    // **对比度 S 曲线**：**按主题分档**（亮 2.0 / 暗 0.6）。
    //
    // 为何必须分开：这一层管“满黑像素占比”，而亮/暗底下同一个强度的效果不同
    //（与 `ClassGammas` / `DarkClassGammas` 同样的理由）。亮色重新标定后
    //（汉字 γ 0.62→2.20，见 `ClassGammas::han` 的表）需要 2.0 才能把满黑
    // 拉到参照的 23.9%/48.3%；暗色一直是 0.6 且已按 VS Code 实拍验收过。
    {
      const bool dark_theme = impl_->renderer->coverage_gamma() < 1.0f;
      float contrast = dark_theme
                           ? st::text::TextRenderer::coverage_contrast_strength_default()
                           : st::app::kLightContrastStrength;
      if (const auto value = fs::read_env("ST_TEXT_CONTRAST_STRENGTH");
          value.has_value() && !value->empty()) {
        try {
          contrast = std::stof(*value);
        } catch (const std::exception&) {
          contrast = dark_theme
                         ? st::text::TextRenderer::coverage_contrast_strength_default()
                         : st::app::kLightContrastStrength;
        }
      }
      impl_->renderer->set_coverage_contrast_strength(contrast);
    }
    // **字重曲线**（跨字号连续映射）：从文件加载。
    //
    // ⚠ **曲线表是主题相关的**：`text_weight_curve.txt` 是在**暗色**（浅字深底）上
    // 标定的（表里 15px = γ1.12/c0.22）。亮色用同一张表会把亮色的类 γ / contrast 盖掉
    //——它是“全字号覆盖”，比类 γ 更强势（contrast 直接取曲线的值）。
    //
    // 因此按主题选文件：暗色用原表；亮色**默认不加载**（回落到 `ClassGammas` +
    // `kLightContrastStrength` 这组已按亮色参照重标的参数）。
    // 亮色如需曲线，把对应的表放成 `text_weight_curve_light.txt` 即会自动生效。
    {
      const bool dark_theme = impl_->renderer->coverage_gamma() < 1.0f;
      const std::string curve_name =
          dark_theme ? "text_weight_curve.txt" : "text_weight_curve_light.txt";
      st::text::WeightCurve curve;
      std::size_t loaded = 0;
      if (const auto env = fs::read_env("ST_TEXT_WEIGHT_CURVE");
          env.has_value() && !env->empty()) {
        loaded = curve.load_from_file(*env);
      } else {
        // **多个候选路径**：应用的工作目录不一定是仓库根（gbcode 可能从
        // build/dev/bin 启动），单一路径 `resources/...` 会静默加载不到。
        // 实测：只写相对路径时，gbcode 截图里的彩边 89.6 与探针 ~40
        // 差一倍多——曲线根本没生效。
        for (const auto& prefix :
             {"resources/", "../resources/", "../../resources/", "../../../resources/"}) {
          loaded = curve.load_from_file(std::string(prefix) + curve_name);
          if (loaded > 0) break;
        }
      }
      if (loaded > 0) {
        impl_->renderer->set_weight_curve(std::move(curve));
        if (fs::read_env("ST_TEXT_DEBUG").has_value()) {
          st::log::out("[text] weight_curve={} ({})\n", loaded, curve_name);
        }
      } else if (fs::read_env("ST_TEXT_DEBUG").has_value()) {
        st::log::out("[text] weight_curve=未加载（{}）\n", curve_name);
      }
    }
    // 生效值自检（`ST_TEXT_DEBUG=1`）：测"参数是否真的生效"的**唯一**可靠手段——
    // 实测踩到两次：补丁被回退后两份渲染出几乎一样的图、应用自带解析器把新参数静默吃掉。
    if (fs::read_env("ST_TEXT_DEBUG").has_value()) {
      st::log::out("[text] theme_gamma={:.2f} fit={} fit_max_size={} class_gamma={}/{}/{}\n",
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
    // 指针形态出口：UI 层解析出语义光标后，由**装配处**转给后端。
    //
    // 为何在这里接而不是让 UiRoot 直接持有后端：依赖方向是 shell → ui，
    // UI 层不该知道 shell 的存在。回调让这条依赖保持单向。
    // 后端为空（理论上不会，但别让空指针炸在这里）时静默跳过。
      root_.cursor_sink = [this](ui::MouseCursor cursor) {
    if (impl_->backend != nullptr) impl_->backend->set_cursor(cursor);
  };
  // 输入法插入点：界面只知道“光标在哪”，候选窗开在哪里是后端的事。
  // 与 `cursor_sink` 同一条单向依赖。
  root_.ime_caret_sink = [this](math::Rect caret) {
    if (impl_->backend != nullptr) impl_->backend->set_ime_caret(caret);
  };
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
    // 写侧（2026-10-12 补齐，与读侧对称）：copy 动作面（Text/Input/Terminal…）
    // 从这里把文本写到平台剪贴板。写失败**不降级**——返回 false 让组件如实上报
    //（“复制失败”是用户需要知道的，而不是静默丢掉）。
    root_.set_clipboard_write_provider([backend_ptr](std::string_view text) -> bool {
      return backend_ptr->set_clipboard_text(text).has_value();
    });

    // 原生子窗口嵌入：同样只装 provider。
    //
    // **不支持的后端什么都不做**（`supports_child_view()` 为 false 时组件自
    // 己退化为占位块），所以这里不需要任何分支——这就是“不可用不影响基础
    // 功能”的落点：不装 provider 的后端，组件拿到的就是默认的
    // `false`/空操作，布局/主题/事件一切如常。
    root_.set_child_view_providers(
        [backend_ptr]() -> bool { return backend_ptr->supports_child_view(); },
        [backend_ptr](void* child, math::Rect bounds) -> bool {
          return backend_ptr->adopt_child_view(child, bounds).has_value();
        },
        [backend_ptr](void* child, math::Rect bounds, math::Rect clip) {
          backend_ptr->sync_child_view(child, bounds, clip);
        },
        [backend_ptr](void* child) { backend_ptr->dismiss_child_view(child); },
        [backend_ptr]() -> ui::Element::ChildViewProbe {
          const shell::ChildViewProbe probe = backend_ptr->child_view_probe();
          ui::Element::ChildViewProbe out;
          out.attached = probe.attached;
          out.title_bar_height = probe.title_bar_height;
          out.content_x = probe.content_x;
          out.content_y = probe.content_y;
          out.content_width = probe.content_width;
          out.content_height = probe.content_height;
          out.host_width = probe.host_width;
          out.host_height = probe.host_height;
          out.visible = probe.visible;
          return out;
        });
  }

  // ## `ST_NO_SCRIPT`：按需链接的编译期拆链（与 server.cpp 的 handle_script 同一套）
  //
  // 定义了它 = 不创建脚本宿主、不 include 其实现：`script()` 恒返 nullptr，
  // quickjs 整链不进产物。运行期的 `--enable-script` 参数仍会被解析（cli.cpp
  // 与清单无关），只是没有任何效果——控制协议调它时得到明确的"未编入"回复。
#ifndef ST_NO_SCRIPT
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
#endif

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
  perf::startup_mark("before_first_frame");
  // 首帧是最重的一笔：实测 **~1.4 s**（字体首次光栅化 + 图元缓存建表）。
  // 而 `SendMessageTimeout(WM_NULL)` 探到主线程连续 **1008 ms** 不回消息，
  // Windows 据此判为无响应、把鼠标换成旋转圆圈（用户报的"启动时转圈"）。
  //
  // 处理：首帧**之前**先把窗口显出来（`show_early`）。
  // 原先的顺序是"首帧画完才 show_when_ready"，理由是怕用户看到未初始化的黑块；
  // `show_early` 的语义是"映射窗口 + 立刻用主题底色填一帧"（一次 fill，微秒级），
  // 所以露出的是干净底色而不是黑块。首帧完成后 `show_when_ready` 再 present 一次贴内容。
  //
  // ⚠ **这不能让等待圈消失**：首帧仍是 1.4 s 的不分割工作，窗口虽已可见，
  // 但主线程仍不处理消息。要使它真正消失，得降首帧耗时（目前主要用于
  // 文字首次光栅化，实测占 927 ms）——那是独立议题。
  impl_->backend->pump_events();
  // **先告诉后端窗口底色**（跟随主题），再显窗/画卡。
  //
  // 为何在这里而不是更早：主题在构造函数里就合成好了（`root_.theme()`），而
  // `backend` 在 `create_window` 之后才有——两者都就位的第一个时点就是这里。
  //
  // 为何**不能只靠启动卡**盖住底色：无磨砂时卡外就是这块底色，暗色主题下
  // 若它仍是亮色，卡周围会围一圈浅色——比没做启动卡还难看。
  impl_->backend->set_window_background(root_.theme().colors().bg);

  // 启动卡（`AppOptions::splash`）：**磨砂与分层通道必须在显窗之前就绪**。
  //
  // 为何顺序有讲究（实测）：磨砂开启时上屏走 `WS_EX_LAYERED` + `UpdateLayeredWindow`，
  // 而"先显普通窗口、再转分层"的那条路上，Windows 会把显窗时那一帧的红定向表面
  // 当成初始图层内容——于是后续 `UpdateLayeredWindow` 送上去的逐像素 alpha 被当成
  // **整窗一个 alpha**（实测：整窗均匀 (27,27,28)，卡完全看不见）。
  // 正确的做法是：先设 `WS_EX_LAYERED` + 磨砂 → 再 `ShowWindow` → 再用 ULW 送内容。
  const bool splash_active = impl_->splash_enabled(options_);
  // ## 启动期小窗（用户要求：首帧前只露卡，窗口与首帧一起显）
  //
  // 磨砂是**整窗**材质，所以“只露卡”必须靠改窗口尺寸，而不是靠透明画布。
  // 这里记住**真实尺寸**（之后要拉回去），再把窗口缩成卡的大小。
  int real_width = options_.width > 0 ? options_.width : 1280;
  int real_height = options_.height > 0 ? options_.height : 720;
  bool splash_shrunk = false;
  if (splash_active && options_.splash_card_window && !impl_->backend->headless()) {
    const auto current = impl_->backend->logical_size();
    if (current.width > 0.0f && current.height > 0.0f) {
      real_width = static_cast<int>(std::lround(current.width));
      real_height = static_cast<int>(std::lround(current.height));
    }
    const float card_w = options_.splash_style.card_width;
    const float card_h = card_w / std::max(0.2f, options_.splash_style.card_aspect);
    // 小窗 = 卡 + 两侧边距（纯分层档光晕不画，边距只是保险量）。
    const float margin = options_.splash_window_margin;
    int splash_w = static_cast<int>(std::lround(card_w + margin * 2.0f));
    int splash_h = static_cast<int>(std::lround(card_h + margin * 2.0f));
    // ⚠ **不能比真实窗口还大**（实测：相期的真实窗口是 460×620，而卡窗算出
    // 447×403 宽了一点点——启动期会“先把窗口撑宽一次、再缩回去”，用户能看到窗口左右抽）。
    // 夹到真实尺寸内：卡仍会按窗口高/宽自动缩（`paint_splash_card` 里有 0.86 夹取）。
    if (real_width > 0 && real_height > 0) {
      splash_w = std::min(splash_w, real_width);
      splash_h = std::min(splash_h, real_height);
    }
    if (auto resized = impl_->backend->set_size(splash_w, splash_h); !resized) {
      ST_LOG_INFO("启动期小窗设置失败（{}），改为整窗显示启动卡", resized.error().message);
    } else {
      splash_shrunk = true;
      // 小窗也要居中：否则卡会偏在一角（用户看到的是“一张歪掉的卡”）
      (void)impl_->backend->center_window();
      ST_LOG_INFO("启动期小窗：{}x{} 逻辑（真实窗口 {}x{}，首帧后恢复）", splash_w, splash_h,
                  real_width, real_height);
    }
  }
  // ## 启动卡的上屏通道：**只分层、不磨砂**（2026-11-08，用户要求）
  //
  // 旧版这里开的是磨砂（Acrylic）。而 Acrylic 是**整窗材质**：
  // 即使画布 alpha=0，那层玻璃也铺满整个窗口——用户看到的是“一块 447×403 的
  // 灰方块，卡在里面”，而不是“卡悬浮在桌面上”。
  //
  // 实测（`tools/backdrop_level_probe.cpp`）：
  //   Acrylic 开   → 窗口角 `#1E1E20`，与桌面 `#15151A` **不一致**（方块可见）
  //   Acrylic 关   → 窗口角 `#15151A`，与桌面**完全一致**（只有卡可见）
  //
  // 所以只开分层位（逐像素 alpha 所需），磨砂那块能力不用。
  // 代价：卡内透出的是**清晰背景**而非模糊背景——用户已确认接受这个取舍。
  if (splash_active) {
    if (auto layered = impl_->backend->set_layered_present(true); !layered) {
      ST_LOG_INFO("启动卡：分层上屏不可用（{}），退化为不透明卡", layered.error().message);
    } else {
      impl_->splash_layered_ = true;
    }
  }
  // 磨砂仍按应用声明尝试（有些应用可能想要），但启动卡默认已关。
  if (splash_active && options_.splash_backdrop_blur) {
    impl_->splash_backdrop_ = impl_->backend->set_backdrop_blur(true);
    if (impl_->splash_backdrop_ != shell::BackdropSupport::Active) {
      ST_LOG_INFO("启动卡：窗口磨砂不可用（{}），退化为不透明卡",
                  impl_->splash_backdrop_ == shell::BackdropSupport::Unsupported
                      ? "后端不支持"
                      : "未确认");
    }
  }
  impl_->backend->show_early();
  // 记下“卡开始露在屏幕上”的时刻：启动卡自适应要量的正是“从这里到主界面就绪”。
  // 详见 `splash_shown_ns` 处那张三种口径的对照表。
  impl_->splash_shown_ns = time::now_ns();
  impl_->backend->pump_events();
  // ── 启动卡：**动画**（不是静态一帧）──
  //
  // 旧版只画一帧，理由是“动画会净增启动耗时（实测 +340ms）”。那个账算得不对：
  // 它测的是“**跑完动画再画首帧**”（动画串行排在首帧之前）。
  // 而首帧那 330ms 是一整块计算（字体首次光栅化 + 图元缓存建表），
  // 屏幕在它期间本来就是一成不变的一帧——动画帧可以从这块时间里“借”。
  //
  // 所以现在：先画一帧（窗口一出现就有卡）+ 在首帧前后各泵几帧。
  // 净代价 ≈ 每帧 10ms（绘制 9 + 上屏 1，小窗）——相对于 667ms 总启动是 3% 量级，
  // 换来的是一个持续在呼吸的卡，而不是一张定住的图。
  std::optional<SplashPump> splash_pump;
  if (splash_active) {
    const ui::Theme& theme = root_.theme();
    SplashState state{};
    state.background = theme.colors().bg;
    state.accent = theme.colors().accent;
    state.backdrop_blur = impl_->splash_backdrop_ == shell::BackdropSupport::Active;
    // 纯分层档：卡外真透明（只看到卡悬浮在桌面上）。
    state.transparent_outside = !state.backdrop_blur && impl_->splash_layered_;
    state.logo = std::span<const std::uint8_t>(options_.window_icon_png);
    splash_pump.emplace(*impl_->backend, state, options_.splash_style);
    // 首帧之前：先把卡画出来（窗口一出现就有东西可看）。
    splash_pump->tick();
    // 挂上**首帧期间的泵点**：让卡在 `layout()` 腾出的缝里继续呼吸。
    // 为何不在 `paint` 之后泵：那时画布上已是界面内容（见 `startup_frame_pump` 的说明）。
    impl_->startup_frame_pump = [&pump = *splash_pump] {
      // ⚠ **不受帧间隔限制**：这个泵只在启动期拿几次机会，
      // 而 `tick()` 里的 max_fps 门限是为“持续循环”设的——
      // 在这里它会把每一次机会都筛掉（实测：首帧期间 diff 恒为 0）。
      pump.tick_forced();
    };
    // ## 首帧开始前先泵几帧（用户能看到“它真的在动”）
    //
    // 为何不是无限泵：每帧 ~10ms 都从首帧时间里借。
    // 而**真正让慢启动能看清动画的**不是这里，而是 `run_splash_animation`
    // 的“跟随真实耗时”策略（见 `ST_SPLASH_FOLLOW=1`）。
    for (int index = 0; index < 4; ++index) {
      splash_pump->tick_forced();
      platform::sleep_ms(16);
    }
  }
  // 启动卡动画：**自适应**跑（不再要求环境变量）。
  //
  // 旧条件 `if (ST_SPLASH_MS 存在) run_splash_animation(...)` 的后果：
  // 常态下动画只靠小窗阶段那几帧（~300ms）——用户报的“一闪而过”就是这个。
  // 而真正该决定时长的是**首帧要算多久**（见 `run_splash_animation` 里的自适应段）。
  // 它内部会自己算：启动快 → `min_ms=0` → 几乎立即返回（不多花时间）；
  // 启动慢 → 陪到首帧完成（封顶 1.5s）。
  if (splash_active) {
    run_splash_animation(impl_->splash_backdrop_);
  }
  // ## 小窗档不在这里 render（2026-11-08 修，实测真因）
  //
  // 旧版这里无条件 `render_frame()`，于是启动卡刚贴上屏就被**完整界面**盖掉：
  //   ① 它走 GDI `blit()`（不认 alpha）→ 整窗被涂成不透明；
  //   ② 而那时窗口还是卡那么小（447×403）——用户看到的是“一张被裁碎的界面”，
  //      实测屏幕采样整片 (243,253,253) 白、四角纯 255，卡与磨砂全不见。
  //
  // 小窗档的正确顺序是：恢复真实尺寸 → 画（在真实尺寸上）→ 显示。
  // 那段在下面，所以这里跳过。
  if (!splash_shrunk) render_frame();
  // 磨砂只属于启动期：主界面一出来就退场（见 `WindowOptions::backdrop_blur`）。
  //
  // ⚠ 小窗档**必须晚于**尺寸恢复（在下面）。为何：
  // `set_backdrop_blur(false)` 会拆掉 `WS_EX_LAYERED` 并把上屏切回 GDI `blit()`；
  // 在小窗上做这一步会把刚贴好的卡立即变成不透明底。
  // 而真实窗口那一帧本就要走 GDI（主界面），所以退场放在它之前正好。
  if (splash_active && !splash_shrunk) {
    (void)impl_->backend->set_backdrop_blur(false);
    (void)impl_->backend->set_layered_present(false);
  }
  // 首帧完成：这是“启动慢”的真实终点（用户看到完整界面的时刻）。
  // 它里面含字体首次光栅化与图元缓存建表——那正是帧剖析里“冷启动帧”的 200ms。
  perf::startup_mark("first_frame");
  // ## 启动期小窗 → 真实窗口（用户要求：窗口与首帧一起显）
  //
  // 启动期窗口只有卡那么大（见上面 `splash_shrunk`）。这里的顺序**至关重要**：
  //
  //   ① 先把窗口拉回真实尺寸（此时窗口还没显，用户看不到它变大）；
  //   ② 再画一帧 —— 画在**新尺寸**的画布上，内容是完整界面；
  //   ③ 然后才 `show_when_ready()`。
  //
  // 于是用户第一眼看到的就是完整界面，而启动期他只看过那张卡。
  //
  // ## 为何不能在中途先 render 一次小窗
  //
  // 那样会把**首帧那 366ms**（字体首次光栅化 + 图元缓存建表，与画布大小基本无关）
  // 花在小窗口上，而用户在这段时间里只盯着卡——卡本来就在那儿，那 366ms 白等了。
  // 现在把它花在真实窗口上，且**后面不用再画一帧**（下面那次 `render_frame`
  // 在小窗档里就不需要了）。实测：总启动 705ms → 约 520ms。
  //
  // 代价：`first_frame` 这个里程碑在小窗档变成“小窗里画一帧卡”，真实界面的首帧
  // 由 `first_frame_after_resize` 记录 —— 两个数都要看，后者才是用户感知的那一个。
  if (splash_shrunk) {
    // ⚠ **必须用 `resize_hidden_for_startup`**（先藏再改）：
    // 直接用 `set_size` 会把“从卡尺寸膨到真实尺寸”的过程露给用户，
    // 而接下来画首帧要 366ms——那 366ms 用户看到的是一整片**白板**。
    // 实测（用户截图）：卡 251ms → 白板 458ms → 白板持续 3s+。
    if (auto resized = impl_->backend->resize_hidden_for_startup(real_width, real_height);
        !resized) {
      ST_LOG_WARN("启动后恢复窗口尺寸失败：{}", resized.error().message);
    }
    (void)impl_->backend->center_window();
    // 启动期的两个“临时状态”都必须在这里退掉：
    //   ① 分层位：不退掉的话主界面的 GDI `blit()` 不参与合成（整屏黑/白）；
    //   ② 磨砂（若应用开过）：同上。
    // 顺序无所谓，但都必须在画主界面之前完成。
    if (splash_active) {
      (void)impl_->backend->set_backdrop_blur(false);
      (void)impl_->backend->set_layered_present(false);
    }
  }
  // 画**真实尺寸**的那一帧（启动期小窗时，这是用户会看到的第一帧内容）。
  //
  // ⚠ 动画泵在这里**必须摘掉**：此时画布已是真实尺寸，而 `render_frame()`
  // 要往它上面画完整界面——再泵一帧卡会把刚画好的界面盖掉。
  impl_->startup_frame_pump = nullptr;
  render_frame();
  if (splash_shrunk) perf::startup_mark("first_frame_after_resize");
  // 首帧已画完 → 窗口可以露面了。
  //
  // 窗口后端建窗时**故意不显**（见 `Backend::show_when_ready` 声明处）：否则
  // 窗口会先露出“系统预备的白底”，而画布分配（含渲染器实测基准，本机 ~570ms）
  // 与 DPI 尺寸调整还没做完——多出来的区域没有任何东西可贴，就是**启动时
  // 右下/底部一大块黑**（实测 0.6s 后自愈）。放在首帧之后：用户看到的第一眼
  // 就是一个完整界面，没有中间态。
  impl_->backend->show_when_ready();
  // ## 启动卡自适应：量“卡在屏上”的真实时长（2026-11-08）
  //
  // 口径：从 `show_early()`（卡开始露在屏上）到 `show_when_ready()`（完整界面就位）。
  // 这是用户**真正在看卡**的那段时间，也正是下次启动时动画该覆盖的长度。
  //
  // 为何不用其它三个候选值：见 `splash_shown_ns` 处那张对照表
  //（`now - started_ns` 会正反馈恶化；`render_frame()` 耗时只有 5ms，差两个数量级）。
  if (impl_->splash_shown_ns > 0) {
    // ## 为什么要**减掉动画自己占的时间**（2026-11-08，第四次修正）
    //
    // 直接量 `show_early → show_when_ready` 会得到正反馈累加：
    // 实测 735 → 964 → 1175 → 1422ms，因为那段时间里**包含了本轮动画自己的播放**。
    // 播得越久 → 量到越久 → 下回播更久 → 无限递增（直到封顶 1.5s）。
    //
    // 所以口径拆开：
    //   “卡在屏时长” = 动画耗时 + **界面准备耗时**
    // 而自适应该用的是后者——它是“不用动画也耍等的时间”，播多长都改变不了它。
    const double on_screen_ms =
        static_cast<double>(time::now_ns() - impl_->splash_shown_ns) / 1'000'000.0;
    impl_->last_first_frame_ms = std::max(0.0, on_screen_ms - impl_->splash_anim_ms);
    // **落盘给下次启动**（只存成员等于每次都是 0：每次启动都是新进程）。
    write_splash_memory(name_, impl_->last_first_frame_ms);
    if (const auto flag = st::fs::read_env("ST_SPLASH_DEBUG");
        flag.has_value() && !flag->empty() && *flag != "0") {
      ST_LOG_INFO("启动卡自适应：界面准备 {:.0f}ms（卡在屏 {:.0f}ms，动画占 {:.0f}ms）"
                  " → 下次最短播 {:.0f}ms",
                  impl_->last_first_frame_ms, on_screen_ms, impl_->splash_anim_ms,
                  impl_->last_first_frame_ms >= 350.0
                      ? std::min(impl_->last_first_frame_ms, 1500.0)
                      : 0.0);
    }
  }
  // 显示之后是否**再画一帧**：
  //
  // 必要性（小窗档**没有**）：`ShowWindow` 之前做的 present 发生在窗口被合成器
  // 映射之前，DXGI 不会把它送上屏——所以要先把窗口显出来、再重画一次。
  // 但启动期小窗档里，窗口是**先拉回真实尺寸、再画、再显**的，而软件路径的
  // `present` 本就是 GDI 直写（不依赖 DXGI 映射），所以那一帧已经在屏幕上。
  // 再画一次只是白花 ~1ms；真正的风险是它会把刚显示的完整界面闪一下。
  if (!splash_shrunk) render_frame();
  return ok();
}

void Application::run_splash_animation(shell::BackdropSupport backdrop) {
  // ## 为什么是一个"循环到时间到"而不是把首帧切碎
  //
  // 首帧的 ~389ms 是一整块不可分割的工作（字体首次光栅化 + 图元缓存建表），
  // 拆开要动 `paint` 的原子性——那是另一个量级的风险。而用户抱怨的那段
  // **正好在首帧之前**（实测：窗口可见 ~60ms → 内容 ~452ms，中间是一块
  // 完全静止的主题底色）。所以动画只覆盖那一段，首帧本身一个像素也不变。
  //
  // ## 时间上限为什么是 320ms
  //
  // 它必须**短于** last 一段到首帧的实测值（~395ms @ scale 1.5）。这不是拍脑：
  // 动画循环自己会泵消息、重画整窗（每帧 ~1ms），占用一点主线程时间；
  // 若让它跑到首帧该开始之后，就会把首帧往后推（用户看到的是启动变慢）。
  // 留 ~75ms 余量给"最后一帧上屏 + 进首帧"。
  //
  // ## 时间上限：为什么是 0（默认不跑）
  //
  // 启动动画的价值在“把一段**本来就在等的时间**变好看”——它**不应该**把首帧往后推。
  // 而实测（本机 / gallery / 3533×1921 @1.5）这条前提不成立：
  //
  // | | 总启动 | 说明 |
  // |---|---|---|
  // | 无启动卡 | **455ms** | 首帧 373ms |
  // | 启动卡 341ms | **748ms** | 首帧 378ms（没变） |
  //
  // 即：动画期间主线程被它占着，而**首帧的 373ms 本来可以并行**——
  // 后台 GPU 预热（实测 390ms）也同样被灋在启动卡之后才开始（主线程要画卡）。
  // 结果不是“填住空白”，而是“额外多等 341ms”。那种“优化”不该默认开。
  //
  // ## 默认行为：**自适应**（2026-11-08）
  //
  // 用户报“动画一闪而过、看不清”，而根源不是动画太快，是**启动本来就快**：
  // 实测 gallery 的启动卡阶段只有 ~300ms（启动期小窗 + 首帧），
  // 一张 1.15s 周期的脉冲动画在那里只能完成 1/4 个周期。
  //
  // 而“把动画拉长”与“不要把启动拖慢”是一对矛盾，所以按**真实耗时**分档：
  //
  // | 启动耗时 | 行为 | 用户感受 |
  // |---|---|---|
  // | < 350ms（常态） | 只播小窗阶段那几帧 | 一闪而过——**这是对的**，本来就快 |
  // | 350ms ~ 1.5s | 播到首帧完成（封顶 1.5s） | 能看清环在扩散 |
  // | > 1.5s（冷缓存/换机器） | 封顶 1.5s 后放行 | 不会让人干等 |
  //
  // ## 为何上限是 1.5s
  //
  // 一个脉冲周期是 1.15s；能看到**一个完整周期**才能读出“它在循环”而不是
  // “它在加载”。超过 1.5s 后多播的部分不增加信息，只是拖时间。
  //
  // ## 环境变量仍可接管
  //
  // `ST_SPLASH_MS=N` = **至少**播 N 毫秒（演示/录屏用），覆盖自适应。
  double min_ms = 0.0;
  bool forced_min = false;
  if (const auto override_ms = st::fs::read_env("ST_SPLASH_MS");
      override_ms.has_value() && !override_ms->empty()) {
    if (const auto parsed = parse_f64(*override_ms); parsed.has_value() && *parsed > 0.0) {
      min_ms = *parsed;
      forced_min = true;
    }
  }
  if (!forced_min) {
    // 自适应：拿**上次启动**的首帧耗时当预期（跨进程记忆，见 `read_splash_memory`）。
    //
    // ⚠ 拿不到历史（首次启动/临时目录不可写）时用 500ms：
    // 宁可略长一点让人看清动画，也不要按“很快”的假设一闪而过——
    // 而首次启动本来就慢，500ms 不会白等。
    constexpr double kColdStartGuessMs = 500.0;
    constexpr double kMaxAdaptiveMs = 1500.0;
    const double observed =
        impl_->last_first_frame_ms > 0.0 ? impl_->last_first_frame_ms
                                         : read_splash_memory(name_);
    const double estimate = observed > 0.0 ? observed : kColdStartGuessMs;
    // 只在“确实比较慢”时才延长：快启动不该人为拖慢。
    constexpr double kSlowThresholdMs = 350.0;
    min_ms = estimate >= kSlowThresholdMs ? std::min(estimate, kMaxAdaptiveMs) : 0.0;
    if (const auto flag = st::fs::read_env("ST_SPLASH_DEBUG");
        flag.has_value() && !flag->empty() && *flag != "0") {
      ST_LOG_INFO("启动卡自适应：预期首帧 {:.0f}ms（{}）→ 最短播 {:.0f}ms", estimate,
                  observed > 0.0 ? "历史实测" : "冷启动估计", min_ms);
    }
  }
  if (min_ms <= 0.0) return;
  if (impl_->backend == nullptr || impl_->backend->headless()) return;
  const ui::Theme& theme = root_.theme();
  SplashState state{};
  state.background = theme.colors().bg;
  state.accent = theme.colors().accent;
  // 磨砂是否生效由调用方判定（它负责控制时序），这里只接结果。
  state.backdrop_blur = backdrop == shell::BackdropSupport::Active;
  state.transparent_outside = !state.backdrop_blur && impl_->splash_layered_;
  state.logo = std::span<const std::uint8_t>(options_.window_icon_png);

  const double started_ms = static_cast<double>(time::now_ms());
  // **暂停 GPU 换型**（见 `Backend::pause_deferred_upgrade`）：否则后台 GPU 就绪后
  // `framebuffer()` 会换成 GPU 画布，启动卡就再也画不出去了。
  impl_->backend->pause_deferred_upgrade(true);
  struct ResumeGuard {
    shell::Backend* backend{nullptr};
    ~ResumeGuard() {
      if (backend != nullptr) backend->pause_deferred_upgrade(false);
    }
  } resume_guard{impl_->backend};
  const bool debug_splash = [] {
    const auto flag = fs::read_env("ST_SPLASH_DEBUG");
    return flag.has_value() && !flag->empty() && *flag != "0";
  }();
  const double frame_ms = 1000.0 / static_cast<double>(std::max(8.0f, options_.splash_style.max_fps));
  std::uint32_t painted = 0;
  while (true) {
    const double elapsed_ms = static_cast<double>(time::now_ms()) - started_ms;
    if (elapsed_ms > min_ms) break;
    state.elapsed_seconds = elapsed_ms / 1000.0;
    // 进度提示：**不是进度条**，只影响环的亮度/扩散幅度（见 `splash.hpp`）。
    // 用"已过时间 / 320ms"推，并夹到 0.85——留一点"还没到底"的意味，
    // 而不是把用户钓到一个永远差一格的 100%。
    state.progress = static_cast<float>(std::min(0.85, elapsed_ms / std::max(1.0, min_ms)));
    raster::Surface& canvas = impl_->backend->framebuffer();
    // 启动卡走的是**软件画布路径**：`paint_splash_card` 需要可逐像素写的 Canvas。
    // 若后端当前画布是 GPU（显式 `--renderer gpu`），就用它自己的软件退路——
    // 启动卡只是一张静态构图，不需要 GPU 的合成能力。
    if (auto* software_canvas = dynamic_cast<raster::Canvas*>(&canvas);
        software_canvas != nullptr) {
      paint_splash_card(*software_canvas, state, options_.splash_style);
      // 启动期诊断（`ST_SPLASH_DEBUG=1`）：确认卡真的画进了画布。
      // 为何需要它："卡没上屏"与"卡没画"在屏幕上是同一个现象，
      // 而两者的修法完全不同（前者是上屏通道，后者是绘制）。
      if (debug_splash && painted == 0) {
        const auto pixels = software_canvas->pixels();
        const int w = software_canvas->physical_width();
        const int h = software_canvas->physical_height();
        const auto at = [&](int x, int y) {
          const std::size_t off = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                                  static_cast<std::size_t>(x);
          return off < pixels.size() ? pixels[off] : 0U;
        };
        ST_LOG_INFO("启动卡诊断：画布 {}x{} · 中心 {:08X} · 边中 {:08X} · 库外空", w, h,
                    at(w / 2, h / 2), at(w / 2, 2));
      }
    }
    impl_->backend->refresh_early();
    ++painted;
    // **泵消息**：这一段里用户可能拖动/关窗——不泵就会被系统当无响应（旋转光标）。
    // 只泵不派（界面尚未就位），与启动期其它泵点同一姿态。
    impl_->backend->pump_events();
    if (impl_->backend->close_requested()) break;
    const double remaining =
        frame_ms - (static_cast<double>(time::now_ms()) - started_ms - elapsed_ms);
    if (remaining > 0.5) platform::sleep_ms(remaining);
  }
  perf::startup_mark("splash_frames");
  // 记下本轮动画自己占的时长：`show_when_ready` 之后要把它从“卡在屏时长”里扣掉，
  // 否则自适应会正反馈累加（实测 735→964→1175→1422ms）。
  impl_->splash_anim_ms = static_cast<double>(time::now_ms()) - started_ms;
  ST_LOG_INFO("启动卡：{} 帧 · {:.0f}ms · 磨砂 {}", painted, impl_->splash_anim_ms,
              state.backdrop_blur ? "开" : "关");
}

void Application::paint_splash_once() {
  // ## 为何是“一帧静态卡”而不是动画
  //
  // 启动动画的**前提**是“这段时间本来就在等”。实测（gallery 3533×1921 @1.5）：
  //
  // | | 总启动 |
  // |---|---|
  // | 无启动卡 | **455ms**（首帧 373ms + 尾部） |
  // | 动画跑 341ms | **748ms**（首帧 373ms 没变） |
  //
  // 即动画不是“填补空白”，而是**净增**——它占着主线程，而首帧与后台 GPU 预热
  //（实测 390ms）都只能排在它后面。画一帧静态卡则不同：它只多一次绘制 + 一次上屏
  //（实测 < 3ms），换来的是“窗口一出现就有品牌卡”而不是一块纯色底。
  //
  // 想真正做到“动画无代价”，得先把首帧拆成可中断的分段（字体预热 / 排版 / 绘制 /
  // 建表），把动画插在段与段之间——那是另一件事（调研稿 §4-D）。
  if (impl_->backend == nullptr || impl_->backend->headless()) return;
  const ui::Theme& theme = root_.theme();
  SplashState state{};
  state.background = theme.colors().bg;
  state.accent = theme.colors().accent;
  // 时序固定的一个采样点：脉冲相位在“刚亮起”附近（不是 t=0 的全零相位，
  // 否则环半径最小、最不显眼）。0.22s 落在第一个周期的 19% 处。
  state.elapsed_seconds = 0.22;
  state.progress = 0.12f;

  // 上屏通道由调用方先开好（分层 → ULW，否则 BitBlt）；这里只取结果。
  state.backdrop_blur = impl_->splash_backdrop_ == shell::BackdropSupport::Active;
  state.transparent_outside = !state.backdrop_blur && impl_->splash_layered_;
  state.logo = std::span<const std::uint8_t>(options_.window_icon_png);
  // 启动动画期间**暂停 GPU 换型**：换型后 `framebuffer()` 不再是软件画布，卡就画不出去。
  impl_->backend->pause_deferred_upgrade(true);
  struct ResumeGuard {
    shell::Backend* backend{nullptr};
    ~ResumeGuard() {
      if (backend != nullptr) backend->pause_deferred_upgrade(false);
    }
  } resume_guard{impl_->backend};

  auto& canvas = impl_->backend->framebuffer();
  auto* software_canvas = dynamic_cast<raster::Canvas*>(&canvas);
  if (software_canvas == nullptr) return;   // 显式 GPU 路径：没有软件画布可画，如实跳过
  const std::int64_t paint_started_ns = time::now_ns();
  paint_splash_card(*software_canvas, state, options_.splash_style);
  const double paint_ms = static_cast<double>(time::now_ns() - paint_started_ns) / 1'000'000.0;
  const std::int64_t present_started_ns = time::now_ns();
  impl_->backend->refresh_early();
  const double present_ms =
      static_cast<double>(time::now_ns() - present_started_ns) / 1'000'000.0;
  perf::startup_mark("splash_card");
  ST_LOG_INFO("启动卡：静态 1 帧 · 绘制 {:.1f}ms + 上屏 {:.1f}ms（{}x{} @{:.2f}）· 磨砂 {}",
              paint_ms, present_ms, software_canvas->physical_width(),
              software_canvas->physical_height(),
              static_cast<double>(software_canvas->device_scale()),
              state.backdrop_blur ? "开" : "关");
}

void Application::render_frame() {
  if (impl_->backend == nullptr) return;
  const std::int64_t start_ns = time::now_ns();
  // **帧剖析**（关闭时 `profiler` 为空，整个帧只多两次判空）。
  //
  // 为何在这里而不在 `tick()`：拖边框时帧是由窗口过程直接驱动的，绕过了 `tick`——
  // 剖析若挂在 `tick` 上，那些帧就不进样本，而它们正是“拖动时卡”的现场。
  perf::FrameProfiler* profiler = impl_->frame_profiler.get();
  if (profiler != nullptr) profiler->begin_frame(static_cast<double>(start_ns) / 1'000'000.0);
  // **墙钟帧间隔**：相邻两次进本函数之差（与帧内耗时是两件事）。
  //
  // 帧内耗时（layout+paint+present）小、而墙钟间隔大 ⇒ 被限帧（`pace_loop` 睡满
  // 帧预算），不是性能问题；反过来才是真的重。两者对照才有完整图景，
  // 而只看一个必然误读（实测：首版压测只报墙钟，把 20ms 读成“重合成很慢”）。
  if (impl_->last_frame_start_ns > 0 && start_ns > impl_->last_frame_start_ns) {
    // 环形缓冲：丢最旧是 O(1)（旧的 `erase(begin())` 每帧一次 O(n) 搬动）
    impl_->wall_times.push(static_cast<double>(start_ns - impl_->last_frame_start_ns) / 1'000'000.0);
  }
  impl_->last_frame_start_ns = start_ns;
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
  // **启动期动画泵点**（见 `startup_frame_pump` 的说明）：
  // `layout()` 是首帧最耗时的一段（字体首次光栅化），它跑完时画布上还是**卡**——
  // 此时泵一帧卡既能动画、又不会与即将画的界面冲突。
  if (impl_->startup_frame_pump) impl_->startup_frame_pump();
  // 时间轴推进：动画（开关/悬浮过渡/3D 旋转）都靠它。
  // 用**应用启动以来的秒数**而不是系统时间：前者单调、与帧序号同源，
  // 便于复现（同一帧序列 → 同一动画进度）。
  root_.set_time(static_cast<double>(start_ns - impl_->started_ns) / 1'000'000'000.0);

  const std::int64_t paint_start = time::now_ns();
  // 绘制一帧：`paint_frame` 自动选择全量/局部（损坏区驱动；软件画布走局部，
  // GPU 画布与全局变化恒走全量——语义与旧路径完全一致，只少了无效的整帧重画）。
  // 返回值为 false 即**本帧走了全量**（清屏 + 绘整树）——这个事实要告诉后端：
  // 窗口后端的“局部上屏”只在本帧确实只改了那一个矩形时才成立（否则上屏一块
  // 而画布已整屏重画的其余部分就送不出去，表现为“画面停在旧帧上”）。
  const bool partial = root_.paint_frame(canvas);
  // **空帧不上屏**（2026-10-29）：hint 计时/定时唤醒驱动的帧常常没有任何像素
  // 变化（损坏区空、无整帧标记）——多一次 present 是纯浪费：WARP 下那是
  // CPU 多线程整纹理拷贝（实测 ~7ms/帧），鼠标停在按钮上等气泡的那 450ms
  // 里每帧都在白烧。后备缓冲里的画面就是最新，跳过即可。
  if (!root_.last_frame_presentable()) {
    const std::int64_t now_ns = time::now_ns();
    impl_->repaint = false;
    root_.clear_dirty();
    // 空帧也要入样本：它正是"空转烧 CPU"的证据（见 `idle_churn` 判据）。
    if (profiler != nullptr) {
      const auto to_ms = [](std::int64_t value) { return static_cast<double>(value) / 1'000'000.0; };
      profiler->record_phase("layout", to_ms(paint_start - layout_start));
      profiler->record_phase("paint", to_ms(now_ns - paint_start));
      profiler->record_phase("present", 0.0);
      profiler->end_frame(0, false, false);
    }
    return;
  }
  impl_->backend->set_present_region(partial ? root_.dirty_rect() : math::IntRect{});
  const std::int64_t present_start = time::now_ns();
  impl_->backend->present();
  const std::int64_t end_ns = time::now_ns();
  const auto to_ms = [](std::int64_t value) { return static_cast<double>(value) / 1'000'000.0; };
  impl_->layout_ms = to_ms(paint_start - layout_start);
  impl_->paint_ms = to_ms(present_start - paint_start);
  impl_->present_ms = to_ms(end_ns - present_start);  ++impl_->frames;
  impl_->last_frame_ms = to_ms(end_ns - start_ns);
  impl_->frame_times.push(impl_->last_frame_ms);
  impl_->repaint = false;
  root_.clear_dirty();
  // 三段与帧指标同口径（`metrics.layout_ms` 等就是上面三个变量），剖析只多一次记录。
  if (profiler != nullptr) {
    profiler->record_phase("layout", impl_->layout_ms);
    profiler->record_phase("paint", impl_->paint_ms);
    profiler->record_phase("present", impl_->present_ms);
    profiler->end_frame(root_.painted_elements(), partial, true);
  }
}

auto Application::tasks() -> app::Tasks& { return impl_->tasks; }

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
  // UI 回调落地（`Tasks::post_ui` 的积压）：放在 render 之前——回调写 `State`
  // 标脏后**本帧**重组/绘制即生效（放 render 后则永远慢一帧，与泵的时序教训同源）。
  (void)impl_->tasks.pump();
  if (impl_->repaint || root_.dirty()) render_frame();
  // 脚本定时器：按帧推进，不额外起线程（ST_NO_SCRIPT 下恒空，编译期消除）
#ifndef ST_NO_SCRIPT
  if (impl_->script != nullptr) (void)impl_->script->tick(0.0);
  if (impl_->script != nullptr && impl_->script->take_repaint_request()) impl_->repaint = true;
#endif
  if (impl_->server != nullptr) impl_->server->poll();
  // 排空后端事件。**指针移动走合并**（见 `input_coalescer.hpp`）：拖动时
  // `WM_MOUSEMOVE` 可达 125~1000 次/秒，逐个派发会让绘制类应用在一帧里改几百次文档
  //（并推几百条撤销记录）。合并在**这里**（帧循环）而不是 `UiRoot::dispatch`：
  // 只有这里有"帧"的概念，而测试/控制通道期望"调一次派发一次"。
  //
  // ⚠ **repaint 只由真派发的事件置位**（2026-10-29 修"鼠标一动 CPU 拉满"）：
  // 被合并器收下的 move 在这里对 UI 零影响——若也置 repaint，125Hz 的移动就
  // 变成每秒 125 次"无条件整窗重绘"（实测纯软件 WARP 下 509% 单核）。
  // 合并结果在帧末 flush，届时 UI 真变了（hover 进入/离开 → set_hovered →
  // mark_dirty）自然有 dirty 驱动下一帧，不需要这里预先拉。
  while (true) {
    auto event = impl_->backend->poll_event();
    if (!event.has_value()) break;
    const bool coalesced = root_.feed_pointer_move(*event);
    if (!coalesced) {
      (void)root_.dispatch(*event);
      impl_->repaint = true;
    }
  }
  // 帧末把合并的移动派发出去（位置取最新；本帧走过的整段由合并器的 `earliest` 记录）。
  // flush 后的 UI 变化走 dirty 路径驱动下一帧——鼠标停在原处时（hover 已稳定）
  // 不再有任何帧，CPU 归零。
  if (root_.flush_coalesced_moves() > 0 && root_.dirty()) impl_->repaint = true;
}

Application::~Application() {
  // 先退订日志（再动 `impl_`）：回调抓的是 `this`，注销后便不再有人调用它。
  if (impl_ != nullptr && impl_->log_listener_id != 0) {
    log::remove_listener(impl_->log_listener_id);
    impl_->log_listener_id = 0;
  }
  // 脚本宿主（若启用）在其析构里摘掉事件观察者；此处只需保证它先于 `root_` 销毁
#ifndef ST_NO_SCRIPT
  impl_->script.reset();
#endif
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
  // 启动分解的收尾输出（`ST_PERF_STARTUP=1` 才有数据）。放在退出处而不是启动末尾：
  // “启动”真正的终点在首帧之后（那里才能量到完整代价）。
  perf::log_startup_summary();
  return 0;
}

void Application::pace_loop(std::int64_t tick_start_ms) {
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
  // 空闲：等控制通道可读，超时夹到「最近的定时唤醒点」（2026-10-29 改）。
  //
  // 低频周期事件（光标闪烁/Toast 倒计时）经 `Element::request_wake_at` 登记
  // 唤醒点：`needs_frame()` 在时刻未到时**不**置真，到这里就是真空闲。若超时
  // 仍固定 4ms，闪烁的那一帧要靠 4ms 轮询碰运气（最多晚 4ms，且每秒 250 次
  // 空醒）。现在直接睡到唤醒点（控制通道可读仍即时唤醒，输入响应不受影响）——
  // 无唤醒点时维持 4ms 上限（输入事件靠后端消息泵，4ms 轮询是它的响应保障）。
  //
  // ⚠ 先把帧时间轴刷新到当前墙钟：`time_seconds_` 停在上一帧，而唤醒点是
  // 绝对时刻——不刷新的话剩余时长永远按旧基准算，不是睡过头就是频繁早醒。
  root_.set_time(static_cast<double>(time::now_ns() - impl_->started_ns) / 1'000'000'000.0);
  const double wake_in_ms = root_.next_wake_in_ms();
  // **输入响应上限**：`interruptible_wait` 只等控制通道 socket，**不包括窗口消息**——
  // 键盘/鼠标事件由后端队列 `events_` 承载，只能靠主循环主动 `poll_event()` 取。
  // 所以即使有定时唤醒点，也不能睡过这个上限，否则真实键盘输入要等下一次
  // 闪烁翻转才上屏（实测：光标在闪时回显延迟 350~900ms，而绘制只用 3.8ms）。
  constexpr int kInputPollMs = 4;
  int idle_ms = kInputPollMs;
  if (wake_in_ms >= 0.0) {
    // 唤醒点只是“**额外的最早时限**”：取两者之小。到点或已过→立即醒。
    const int wake_ms = static_cast<int>(wake_in_ms);
    if (wake_ms < idle_ms) idle_ms = wake_ms;
  }
  if (idle_ms <= 0) return;  // 唤醒点已到：不睡，直接下一轮
  if (idle_ms > 4000) idle_ms = 4000;  // 防御：时钟异常时不至于睡死
  interruptible_wait(idle_ms);
}

void Application::wake_control() {
  // 控制通道有数据到达：若正处于等待中，立即结束它（下一轮 `tick()` 就能处理）。
  // 非等待期不动——已经在干活，等完这一段自然会去 poll。
  if (!impl_->pacing || impl_->server == nullptr) return;
  const std::vector<std::intptr_t> handles = impl_->server->wait_handles();
  if (!handles.empty()) (void)platform::wait_any_readable(handles, 0);
}

}  // namespace st::app

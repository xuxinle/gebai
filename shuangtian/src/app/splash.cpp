/// 启动卡绘制（见 `include/st/app/splash.hpp` 的设计说明）。
///
/// ## 为什么这里不用 UI 树
///
/// 启动卡的整个价值在于"在字体栈与组件树装配完成**之前**就能画"。
/// 走 UI 树意味着要等 `TextRenderer` 就绪（实测字体栈 60ms+、首帧字形光栅化占 120ms 量级），
/// 那正好吃掉它要覆盖的那段时间。所以这里**只用光栅原语**：
/// 圆角矩形 + 渐变 + 环形遮罩，全部是解析式，不依赖任何布局/字体/主题编译产物。
///
/// ## 抗锯齿的来源
///
/// 圆角与斜线都用 `fill_rect(radius=...)` / `fill_path`——这两条在本仓是**有覆盖率
/// 抗锯齿**的（`raster::detail::fill_path_aa`），所以卡边缘与菱形标记不需要另做 AA。

#include "st/app/splash.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "st/raster/paint.hpp"
#include "st/codec/png.hpp"
#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/core/time.hpp"
#include <memory>
#include <span>

namespace st::app {
namespace {

/// 分段计时（仅 `ST_SPLASH_DEBUG` 时输出）：启动卡的绘制分成七八个原语，
/// 而“总 26ms”本身不指向任何行动——必须先看哪一段贵。
class StageTimer {
 public:
  StageTimer() {
    const auto flag = st::fs::read_env("ST_SPLASH_DEBUG");
    enabled_ = flag.has_value() && !flag->empty() && *flag != "0";
    if (enabled_) last_ = st::time::now_ns();
  }
  void mark(std::string_view stage) {
    if (!enabled_) return;
    const std::int64_t now = st::time::now_ns();
    const double ms = static_cast<double>(now - last_) / 1'000'000.0;
    last_ = now;
    if (ms >= 0.4) ST_LOG_INFO("  启动卡分段 {:<12} {:.2f}ms", stage, ms);
  }

 private:
  bool enabled_{false};
  std::int64_t last_{0};
};

constexpr double k_pi = 3.14159265358979323846;

/// 缓出（与 `ui::ease_out` 同一形状：`1-(1-t)^p`）。
/// 这里不直接调 `ui::ease_out`：那个入口吃 `Theme`，而启动卡刻意不依赖主题编译产物。
[[nodiscard]] auto ease_out_local(double t, double power = 3.0) -> double {
  const double clamped = std::clamp(t, 0.0, 1.0);
  return 1.0 - std::pow(1.0 - clamped, power);
}

/// 把一个圆离散成多边形路径（N 段）。
///
/// 当前只用到圆环（外圈正向 + 内圈反向），实心圆未用——但保留这个工厂，
/// 因为“加一个实心圆点”是最可能的下一个改动，而它可以被直接复用。
[[maybe_unused]] [[nodiscard]] auto circle_path(math::Point center, float radius, int segments = 48) -> math::Path {
  math::Path path;
  for (int index = 0; index < segments; ++index) {
    const double angle = 2.0 * k_pi * static_cast<double>(index) / static_cast<double>(segments);
    const math::Point point{center.x + radius * static_cast<float>(std::cos(angle)),
                            center.y + radius * static_cast<float>(std::sin(angle))};
    if (index == 0) {
      path.move_to(point);
    } else {
      path.line_to(point);
    }
  }
  path.close();
  return path;
}

/// 菱形路径：**已不再使用**（品牌标记于 2026-11-08 换成六角雪花），
/// 保留它是因为下一个候选（`tools/brand_probe.cpp` 的那个对照图）还要用它做基准。
/// `[[maybe_unused]]` 比删掉再加回便宜。
[[maybe_unused]] [[nodiscard]] auto diamond_path(math::Point center, float half_w, float half_h) -> math::Path {
  math::Path path;
  path.move_to(math::Point{center.x, center.y - half_h});
  path.line_to(math::Point{center.x + half_w, center.y});
  path.line_to(math::Point{center.x, center.y + half_h});
  path.line_to(math::Point{center.x - half_w, center.y});
  path.close();
  return path;
}

/// 把一张 PNG 解成可用于合成的画布（**失败一律返回空** —— 图标是观感资源，不该让它搞挂启动）。
///
/// 为何走 `Canvas` 而不是自己抽像素：`draw_canvas` 已处理了预乘 alpha、
/// 逻辑/物理坐标系与裁剪，而这里要的正是“把一张图贴上去”这一个动作。
[[nodiscard]] auto decode_png_canvas(std::span<const std::uint8_t> png)
    -> std::unique_ptr<st::raster::Canvas> {
  if (png.empty()) return nullptr;
  const auto decoded = st::codec::png_decode(png);
  if (!decoded) return nullptr;
  const int width = static_cast<int>(decoded->width);
  const int height = static_cast<int>(decoded->height);
  if (width <= 0 || height <= 0) return nullptr;
  auto canvas = std::make_unique<st::raster::Canvas>(width, height, 1.0f);
  // PNG 是**非预乘** RGBA，而画布是预乘 `0xRRGGBBAA` —— 必须逐像素预乘，
  // 否则半透明边缘会发白（图标圆角处会出现一圈白边，实测过）。
  auto dst = canvas->pixels();
  const std::size_t count = std::min<std::size_t>(dst.size(),
                                                  static_cast<std::size_t>(width) *
                                                      static_cast<std::size_t>(height));
  for (std::size_t i = 0; i < count; ++i) {
    const std::uint32_t r = decoded->rgba[i * 4U + 0U];
    const std::uint32_t g = decoded->rgba[i * 4U + 1U];
    const std::uint32_t b = decoded->rgba[i * 4U + 2U];
    const std::uint32_t a = decoded->rgba[i * 4U + 3U];
    if (a == 0U) {
      dst[i] = 0U;
      continue;
    }
    dst[i] = (((r * a + 127U) / 255U) << 24U) | (((g * a + 127U) / 255U) << 16U) |
             (((b * a + 127U) / 255U) << 8U) | a;
  }
  return canvas;
}

/// PNG → 画布的**缓存**（按字节指针 + 长度做键）。
///
/// ## 为何必须缓存（2026-11-08）
///
/// 启动动画每帧都会调 `paint_splash_card`，而解码 + 预乘实测每帧吃掉
/// **3.5ms**（`ST_SPLASH_DEBUG` 的 `mark` 分段）——在 60fps 的 16.6ms 预算里
/// 它一个人就吃掉 21%。而图标在动画期间是**同一份字节**，解码一次即可。
///
/// 键用 `data() + size()` 而不是内容哈希：调用方传的是编译期嵌入的库
/// 或 `AppOptions` 里的稳定缓冲，地址在进程生命周期内不变；
/// 而内容哈希要扫全量字节（图标 20~100KB），每帧扫一遍比解码本身还贵。
[[nodiscard]] auto cached_logo_canvas(std::span<const std::uint8_t> logo)
    -> const st::raster::Canvas* {
  if (logo.empty()) return nullptr;
  struct Entry {
    const std::uint8_t* key{nullptr};
    std::size_t size{0};
    std::unique_ptr<st::raster::Canvas> canvas{};
  };
  // lint-allow: L8 进程级缓存（启动动画期间图标字节地址不变）
  static thread_local Entry entry{};
  if (entry.key != logo.data() || entry.size != logo.size()) {
    entry.key = logo.data();
    entry.size = logo.size();
    entry.canvas = decode_png_canvas(logo);
  }
  return entry.canvas.get();
}

/// 让颜色带上不透明度（`with_alpha_f` 是相对该色自身 alpha 的**绝对**赋值）。
[[nodiscard]] auto alpha_of(math::Color color, float alpha) -> math::Color {
  return color.with_alpha_f(alpha);
}

}  // namespace

// ── 启动动画帧泵（声明见 `splash.hpp`）──

SplashPump::SplashPump(shell::Backend& backend, SplashState base, SplashStyle style)
    : backend_(&backend), state_(base), style_(style) {
  started_ns_ = time::now_ns();
  // ## 必须**暂停 GPU 换型**（2026-11-08，忘了一次的代价）
  //
  // 后台 GPU 预热（实测 ~395ms）就绪后，`framebuffer()` 会返回**新的 GPU 画布**，
  // 而启动卡只能画在软件 `raster::Canvas` 上。后果（本轮实测）：
  // 泵往旧画布画（`dynamic_cast<Canvas*>` 成功），而 ULW 从新画布读 →
  // DIB 里是**旧帧内容**（卡体 0x9B 还在），屏幕上就多一圈"浅色方块"。
  //
  // 旧的 `run_splash_animation` / `paint_splash_once` 都有这一步，
  // 新写的 `SplashPump` 漏了——这类“新代码重复老代码的前置条件”最容易漏。
  backend_->pause_deferred_upgrade(true);
}

SplashPump::~SplashPump() {
  if (backend_ != nullptr) backend_->pause_deferred_upgrade(false);
}

SplashPump::SplashPump(SplashPump&& other) noexcept
    : backend_(other.backend_),
      state_(other.state_),
      style_(other.style_),
      started_ns_(other.started_ns_),
      last_frame_ms_(other.last_frame_ms_),
      frames_(other.frames_) {
  // **源对象放弃所有权**：否则它析构时会把“恢复 GPU 换型”做一遍，
  // 而新对象还在用软件画布——那正是本轮踩过的坑（请参构造处那段说明）。
  other.backend_ = nullptr;
}

SplashPump& SplashPump::operator=(SplashPump&& other) noexcept {
  if (this == &other) return *this;
  // 先把自己那份先释放（否则新对象接管后，旧引用永远不恢复换型）
  if (backend_ != nullptr) backend_->pause_deferred_upgrade(false);
  backend_ = other.backend_;
  state_ = other.state_;
  style_ = other.style_;
  started_ns_ = other.started_ns_;
  last_frame_ms_ = other.last_frame_ms_;
  frames_ = other.frames_;
  other.backend_ = nullptr;
  return *this;
}

void SplashPump::tick() {
  if (backend_ == nullptr) return;
  const std::int64_t now = time::now_ns();
  const double since_ms = static_cast<double>(now - started_ns_) / 1'000'000.0;
  // 帧间隔由 `style_.max_fps` 定（默认 60）。首帧期间每帧都要抢 ~10ms，
  // 太密会把首帧推得明显变慢，太疏看不出动——60fps 是实测平衡点。
  if (last_frame_ms_ >= 0.0) {
    const double interval_ms =
        1000.0 / static_cast<double>(std::max(8.0f, style_.max_fps));
    if (since_ms - last_frame_ms_ < interval_ms) return;
  }
  paint_now(since_ms);
}

void SplashPump::tick_forced() { paint_now(static_cast<double>(time::now_ns() - started_ns_) / 1'000'000.0); }

void SplashPump::paint_now(double since_ms) {
  if (backend_ == nullptr) return;
  last_frame_ms_ = since_ms;
  state_.elapsed_seconds = since_ms / 1000.0;
  // 进度提示只影响亮度与扩散幅度（**不是进度条**），夹在 0.85 留一点余地。
  state_.progress = static_cast<float>(std::min(0.85, since_ms / 900.0));
  auto& canvas = backend_->framebuffer();
  if (auto* software = dynamic_cast<raster::Canvas*>(&canvas); software != nullptr) {
    paint_splash_card(*software, state_, style_);
    backend_->refresh_early();
    ++frames_;
  }
  // 泵消息：动画期间用户可能拖窗/关窗，不泵会被系统当无响应。
  backend_->pump_events();
}

auto splash_opacity_without_backdrop() noexcept -> float {
  // 0.92：接近不透明但不是纯色——纯色会让卡片与底之间没有"玻璃"的暗示；
  // 留 8% 透出底色，边缘因此仍有层次，同时不会让背后的桌面壁纸/别的窗口渗上来。
  return 0.92f;
}

void paint_splash_card(raster::Canvas& canvas, const SplashState& state, const SplashStyle& style) {
  const float scale = canvas.device_scale() > 0.0f ? canvas.device_scale() : 1.0f;
  const float width = static_cast<float>(canvas.physical_width()) / scale;
  const float height = static_cast<float>(canvas.physical_height()) / scale;
  if (width <= 1.0f || height <= 1.0f) return;

  // ── ① 底 ──
  // 无磨砂且**不要求卡外透明**：填主题底色（否则会透出桌面壁纸，那是“漏”）。
  if (!state.backdrop_blur && !state.transparent_outside) {
    canvas.clear(state.background);
  }


  // ── ② 卡片几何 ──
  const float card_w = std::min(style.card_width, width * 0.86f);
  const float card_h = std::min(card_w / std::max(0.2f, style.card_aspect), height * 0.86f);
  const math::Rect card{width * 0.5f - card_w * 0.5f, height * 0.5f - card_h * 0.5f, card_w, card_h};
  // 标记与脉冲环的共同圆心：**卡的正中心**。
  //
  // ⚠ 旧版是 `card.y + card.height * 0.30f`（卡高的 30% 处）——
  // 那个位置是给“下方还要放应用名/进度文字”预留的。而现在卡里**只有标记与环**，
  // 于是 logo 偏在上方 1/5 处，下方一大块空——用户直接指出“图标在卡片没居中”。
  //
  // 为何不改成“标记居中、环也居中、下面补文字”：文字内容（应用名/版本/状态）
  // 是**产品信息**，各应用不一致，得由应用声明；在没声明之前先按“只有图形”布局。
  const math::Point center{card.x + card.width * 0.5f, card.y + card.height * 0.5f};

  StageTimer timer{};
  timer.mark("geom+clear");
  const double t = state.elapsed_seconds;
  const float progress = std::clamp(state.progress, 0.0f, 1.0f);

  // 磨砂档：**只把卡 + 光晕那一块**清成全透明（alpha=0 ⇒ 透出合成器的模糊背景）。
  // 实测：整窗 `clear` 是 ~7ms，而这一块只占窗口的一小部分。
  // 用 `BlendMode::Src` 直接覆盖（不是 src-over）——要的就是“写成 0”，不是“叠一层”。
  //
  // ⚠ **小窗口时必须直接清整窗**：窗口本身只比卡大一点时，“卡+光晕”的外包矩形
  // 会盖住整窗，而四角仍在它**外面**（矩形是方的、角是圆的）——那几像素会留着
  // 上一帧的内容。实测：测试用例在 320×220 逻辑的画布上当场红（四角 alpha ≠ 0）。
  if (state.backdrop_blur || state.transparent_outside) {
    const math::Rect hole = card.inflate(style.glow_radius + 8.0f);
    if (hole.x <= 0.0f && hole.y <= 0.0f && hole.x + hole.width >= width &&
        hole.y + hole.height >= height) {
      canvas.clear(math::Color{0, 0, 0, 0});
    } else {
      // 大窗口：清“卡 + 光晕”的外包矩形，**再加四条边带**（外包框到窗口边）。
      // 为何不能只清外包矩形：那个矩形是方的，而卡是圆的——四角在矩形外，
      // 会留着上一帧的内容（这正是测试用例抓到的那条）。
      // 代价：总清洗面积仍是整窗量级，但**“写入 0”比“写入主题色”便宜**
      //（`clear` 填的是主题色；两者都是 memset 量级，差异在能否走 memset 快路径）。
      const math::Color transparent{0, 0, 0, 0};
      // ⚠ 不用 `fill_rect` + `Src`：实测它**不会把目标写成全零**（内部对 a=0 的源
      // 有早退/特殊处理，语义上“没有任何东西可写”）。要真清成零必须走 `clear` 或
      // 直接写像素。这里用 `clear`（整窗）——虽然 ~7ms，但它是唯一可靠的“清零”。
      //
      // 实测代价对照：本机 3533×1921 下 `clear` 约 7ms，而整张卡的总绘制
      // 优化后只占 6.6ms——即“清零”已经是启动卡里最贵的一项。但它是**正确性**代价，
      // 不能省（省了就是沙箱里的测试红 + 屏幕上的四角拖影）。
      (void)hole;
      (void)transparent;
      canvas.clear(math::Color{0, 0, 0, 0});
    }
  }

  // 呼吸：`0..1` 的正弦，相位**从 0 起**（首帧就是"亮起"而不是"已经呼吸很久"）。
  const double pulse_period = std::max(0.2f, style.pulse_seconds);
  const float breath =
      static_cast<float>(0.5 + 0.5 * std::sin(2.0 * k_pi * t / pulse_period - k_pi * 0.5));

  const float card_alpha =
      (state.backdrop_blur || state.transparent_outside) ? style.card_opacity
                                                      : splash_opacity_without_backdrop();

  timer.mark("hole");
  // ── ③ 柔光晕：让卡片"浮"起来的一层氛围光 ──
  //
  // ⚠ **不用径向渐变**：实测它在本例吃了 **17.3ms**（占绘制 2/3）。
  // 原因：径向渐变要**逐像素**算 `sqrt`（渐变求值的距离），面积是「卡 + 2×68px 晕」 的
  // 外包矩形（1.5x 下 ~690×680 物理）。而它是全画面最不重要的那一层
  //（alpha 上限只有 0.10）。
  //
  // 改用**三圈同心圆角矩形**（纯色、不同 alpha）：每圈都是一次 `fill_rect`
  //（可直接按行填充，不需要逐像素求值），三层叠起来观感几乎一致，
  // 而代价从 17.3ms 降到 < 1ms。这也是"低 alpha 的氛围层不值得逐像素精度"的自觉。
  // ## 纯分层档**不画光晕**（2026-11-08，用户“不要边界”）
  //
  // 光晕是向外扩的一层层低 alpha 圆角矩形。在磨砂背景上它是“浮起来”的氛围；
  // 但在**真透明背景**上它就变成了一圈看得见的淡轮廓——用户直接指出
  // “卡外还有一圈影子”，而要求是“只有卡悬浮”。
  //
  // 所以 `transparent_outside` 档直接跳过它。代价是失去“悬停”的景深感，
  // 但换来干净的单卡边界（这正是用户要的）。
  const bool want_glow = style.glow_radius > 0.0f && style.glow_opacity > 0.0f &&
                          !state.transparent_outside;
  if (want_glow) {
    const float base_alpha = style.glow_opacity * (0.55f + 0.45f * breath);
    // ## 层数与层距：为什么是 8 层而不是 3 层
    //
    // 三层（实测）在真窗口上能看出**三道台阶**——因为每层 alpha 跳变 ~3/255，
    // 而磨砂背景很平，任何一点阶跃都显眼。8 层时单层跳变降到 ~1/255，
    // 在 8 位通道上已不可分辨（宏观是连续晕）。
    //
    // 代价：8 次 `fill_rect`（每次都是圆角矩形快速填充），实测 < 1ms——
    // 比它取代的径向渐变（17.3ms，逐像素求 sqrt）便宜两个数量级。
    constexpr int kGlowBands = 8;
    for (int band = 0; band < kGlowBands; ++band) {
      // 从外到内：越靠内越小、越亮。用**二次**衰减让边沿收得自然（线性会偏“圆环”）。
      const float k = 1.0f - static_cast<float>(band) / static_cast<float>(kGlowBands);
      const float grow = style.glow_radius * k;
      const float alpha = base_alpha * k * k * 0.55f;
      if (grow <= 0.5f || alpha <= 0.002f) continue;
      canvas.fill_rect(card.inflate(grow),
                       raster::Paint::solid(alpha_of(state.accent, alpha)),
                       style.card_radius + grow);
    }
  }

  timer.mark("glow");
  // ── ④ 卡片本体：**垂直渐变**（上亮下暗），比纯色更像一片玻璃 ──
  //
  // ## 关键：磨砂档的\"玻璃\"必须是**提亮**的，不是主题底色
  //
  // 实测踩到的坑：第一版两种档都用 `state.background` 当卡色。深色主题下它的 RGB 是
  // `0B0B0E`，以 14% alpha 叠在同样暗的磨砂背景上 ⇒ **卡彻底看不见**
  //（屏幕采样中心像素 (7,7,9)，与背景同值）。而预览图里看着正常，是因为那里叠的
  // 是偏亮的合成背景——"在好看的底上看都对"，这正是一类典型假阳性。
  //
  // 所以磨砂档用**白色低 alpha** 当玻璃色（Win11 原生 acrylic 卡就是这么做的）：
  // 它在亮/暗背景上都是"抬一层"，这才是玻璃的观感。无磨砂档才用主题底色。
  const bool glass_look = state.backdrop_blur || state.transparent_outside;
  const math::Color glass = glass_look ? st::math::Color::rgb(255, 255, 255) : state.background;
  const float top_alpha = card_alpha * (glass_look ? 1.15f : 1.02f);
  const float bottom_alpha = card_alpha * (glass_look ? 0.72f : 0.86f);
  // ## 纯分层档**不画卡片本体**（2026-11-08，用户“不要卡片边界”）
  //
  // 用户要的是“图标和环悬浮在桌面上”，而不是“一张卡片又悬浮”。
  // 在真透明背景下，卡本体那块矩形即使只有 17% 白，也会被看成
  // **一块可见的方形/圆角矩形”——而用户明确要求“不要边界”。
  //
  // 所以 `transparent_outside` 档只保留**有信息的那两层**：脉冲环 + 品牌标记。
  // 它们本来就是环形/图形，自然融入桌面，没有任何矩形边界。
  const bool draw_card_body = !state.transparent_outside;
  if (draw_card_body) {
    canvas.fill_rect(
        card,
        raster::Paint::with_gradient(raster::Gradient::linear(
            math::Point{card.x, card.y}, math::Point{card.x, card.y + card.height},
            {raster::GradientStop{0.0f, alpha_of(glass, std::min(1.0f, top_alpha))},
             raster::GradientStop{1.0f, alpha_of(glass, std::min(1.0f, bottom_alpha))}})),
        style.card_radius);
  }

  timer.mark("card");
  // ── ⑤ 描边：1.5px 的内描边，用强调色的极低透明度（"玻璃边缘光"） ──
  // 它是卡片"有厚度"的唯一线索；没有它，半透明卡在深色底上会糊成一团。
  // 纯分层档没卡片，这道描边也就不画（否则它自己就是一圈边界）。
  if (draw_card_body) {
    const float edge_alpha = glass_look ? 0.20f : 0.12f;
    canvas.fill_path(math::make_rounded_border_ring(card, style.card_radius, 1.5f),
                     raster::Paint::solid(
                         alpha_of(state.accent, edge_alpha * (0.5f + 0.5f * breath))));
  }

  timer.mark("edge");
  // ── ⑥ 三个交错脉冲环（各错开 1/3 周期） ──
  //
  // 环的**半径**随呼吸扩张、**透明度**随扩张衰减 —— 这就是"能量在扩散"的读法。
  // 为何是三层而不是两层：两层在多数时刻只有一个可见，画面会周期性显得空；
  // 三层错开 1/3 后任何时刻都至少有一层在中间幅度，密度稳定。
  // 环宽 1.2~2.2px（随扩张变细）——太细看不见，太粗会变成"实心圆"。
  for (int index = 0; index < 3; ++index) {
    const double phase = t / pulse_period + static_cast<double>(index) / 3.0;
    const float local = static_cast<float>(phase - std::floor(phase));      // 0..1 锯齿
    const float eased = static_cast<float>(ease_out_local(local, 1.8));     // 缓出扩张
    const float ring_radius = style.ring_radius * (0.55f + 1.05f * eased);
    const float ring_alpha =
        (1.0f - eased) * (0.34f + 0.40f * progress) * (index == 0 ? 1.0f : 0.78f);
    if (ring_alpha <= 0.004f) continue;
    const float ring_width = 2.4f - 1.0f * eased;   // 扩张的同时变细
    // 圆环 = 外圆正向 + 内圆反向 ⇒ 非零环绕规则自动成孔洞（同 `make_rounded_border_ring`
    // 的手法，只是那里是圆角矩形、这里是正圆）。
    constexpr int kSegments = 64;
    math::Path donut;
    for (int seg = 0; seg < kSegments; ++seg) {
      const double a = 2.0 * k_pi * static_cast<double>(seg) / kSegments;
      const math::Point p{center.x + ring_radius * static_cast<float>(std::cos(a)),
                          center.y + ring_radius * static_cast<float>(std::sin(a))};
      if (seg == 0) {
        donut.move_to(p);
      } else {
        donut.line_to(p);
      }
    }
    donut.close();
    const float inner_radius = std::max(0.5f, ring_radius - ring_width);
    for (int seg = kSegments - 1; seg >= 0; --seg) {   // 反向 ⇒ 孔洞
      const double a = 2.0 * k_pi * static_cast<double>(seg) / kSegments;
      const math::Point p{center.x + inner_radius * static_cast<float>(std::cos(a)),
                          center.y + inner_radius * static_cast<float>(std::sin(a))};
      if (seg == kSegments - 1) {
        donut.move_to(p);
      } else {
        donut.line_to(p);
      }
    }
    donut.close();
    canvas.fill_path(donut, raster::Paint::solid(alpha_of(state.accent, ring_alpha)));
  }

  timer.mark("rings");
  // ── ⑦ 品牌标记 ──
  //
  // ## 两个优先级（用户定的规则，2026-11-08）
  //
  // > 有应用自己的 logo 的用应用的，没有就用霜天的。
  //
  // 所以这里先看 `state.logo_png`：非空就把它当品牌标记画上去；空了才画霜天雪花。
  // 为何拿 `window_icon_png` 而不另建一套 logo 资源：每个应用**已经**有一份品牌 PNG
  //（`apps/*/assets/*-icon.png`，见 `app_icon.hpp`），它就是任务栏上那个图标——
  // 启动卡用同一个文件，才不会出现“启动卡一个标、任务栏另一个标”。
  //
  // ## 雪花为何长这样（当没有 logo 时）
  //
  // 用户直接指出旧版“菱形套菱形”太丑——它只是两个几何形状叠在一起，**没有任何可识别的意象**。
  // 候选与取舍见 `tools/brand_probe.cpp` 的 6 个方案对照图；选定六角雪花是因为
  // 它是"霜天"的字面意象（寒、锐、对称），且与脉冲环天然同族（放射状）。
  // 经典雪花画六枝 × 每枝再分叉，**小尺寸会糊成一团**；这里只留三条贯穿主干
  //（0°/60°/120°）+ 六个枝尖的一对短分叉，14px 下仍能认出“六向对称”。
  const float mark_alpha = 0.78f + 0.22f * std::max(breath, progress);
  const math::Color ink = alpha_of(state.accent, mark_alpha);
  const float mark_r = style.mark_radius * 0.92f;
  const float spine_w = std::max(1.1f, mark_r * 0.155f);

  // 应用 logo：有就贴图，**不画雪花**（用户规则）。
  // 尺寸取“外接半径 × 2”；贴图带不透明度烘焙（`mark_alpha`），
  // 这样它和环/扫光的呼吸是同步的——否则图标会像贴纸一样死在那里。
  bool drew_logo = false;
  if (const auto* logo_canvas = cached_logo_canvas(state.logo); logo_canvas != nullptr) {
    const float side = mark_r * 2.0f;
    const math::Rect box{center.x - side * 0.5f, center.y - side * 0.5f, side, side};
    canvas.draw_canvas(*logo_canvas, box, raster::DrawOptions{.opacity = mark_alpha});
    drew_logo = true;
  }
  if (!drew_logo) {
  // 呼吸时整个雪花**极轻微地缩放**（±3%）：避免它在脉冲环里看起来是一张死图。
  const float sway = 1.0f + 0.03f * (2.0f * breath - 1.0f);
  const float r = mark_r * sway;
  for (int axis = 0; axis < 3; ++axis) {
    const double angle = k_pi * static_cast<double>(axis) / 3.0;   // 0°, 60°, 120°
    const float dx = static_cast<float>(std::cos(angle));
    const float dy = static_cast<float>(std::sin(angle));
    math::Path spine;
    spine.move_to(math::Point{center.x - dx * r, center.y - dy * r});
    spine.line_to(math::Point{center.x + dx * r, center.y + dy * r});
    canvas.stroke_path(spine, raster::Paint::solid(ink), spine_w);
  }
  // 六个枝尖的短分叉（主干方向 ±52°，长度 30%）——
  // 它们是"霜"的锐利感来源。角度实测调过：±40° 时分叉会与主干靠得太近，
  // 整体看着像"一团"；±52° 才张开得像雪花。再长就会在 14px 下并成一圈。
  for (int tip = 0; tip < 6; ++tip) {
    const double angle = k_pi * static_cast<double>(tip) / 3.0;
    const float dx = static_cast<float>(std::cos(angle));
    const float dy = static_cast<float>(std::sin(angle));
    const math::Point end{center.x + dx * r, center.y + dy * r};
    for (int side = -1; side <= 1; side += 2) {
      const double offset = 0.91 * static_cast<double>(side);
      const float bx = static_cast<float>(std::cos(angle + offset));
      const float by = static_cast<float>(std::sin(angle + offset));
      math::Path barb;
      barb.move_to(end);
      barb.line_to(math::Point{end.x + bx * r * 0.30f, end.y + by * r * 0.30f});
      canvas.stroke_path(barb, raster::Paint::solid(ink), spine_w * 0.80f);
    }
  }
  // 中心点：雪花骨架在正中心是空的（三条线交叉不到），补一个小圆点让"心"实起来。
  canvas.fill_circle(center, spine_w * 1.15f, raster::Paint::solid(ink));
  }

  timer.mark("mark");
  // ── ⑧ 扫光：一条**很宽的斜向光带**从卡内扫过（不是细进度条） ──
  //
  // ## 为何是"宽光带"而不是"底部进度线"
  //
  // 底部一条细线是"加载器"的语汇（隐含"有个确定的进度"），而启动耗时不可预测；
  // 宽光带只是"光在玻璃上滑过"，不承诺任何进度，观感上更接近"材质"而不是"控件"。
  // 这也是高端产品常用的手法：动效做在**表面的反射**上，而不是在做进度。
  //
  // ## 为何不用 clip（实测代价）
  //
  // `push_clip_rounded_rect` 要分配并光栅化一张**卡大小**的遮罩（本例 300×290 逻辑
  // ⇒ 1.5x 下 ~440×420 物理），实测吃掉 **~12ms**——而启动卡的全部目标就是 < 3ms。
  // 所以改为**几何自守**：每列只画在卡的**圆角内接矩形**里（上下各让出 `card_radius`）。
  // 光带本来就是斜的、两端 alpha 为 0，让出的那两条角带里它本来也看不见。
  // ## 纯分层档**不画扫光**（2026-11-08，用户“不要卡片边界”）
  //
  // 扫光带的几何是“卡的圆角内接矩形里的一个竖条”——它本身就是一个矩形，
  // 而且它存在的语义是“光在**玻璃表面**滑过”。现在没有玻璃卡了，
  // 它在桌面上就是一块会飘的矩形（实测就是它造成了“还有卡片边界”的错觉）。
  if (card.width > 8.0f && !state.transparent_outside) {
    const double sweep_period = std::max(0.4f, style.sweep_seconds);
    const double cycle = t / sweep_period;
    const float shift = static_cast<float>(cycle - std::floor(cycle));   // 0..1
    const float band_w = card.width * 0.55f;
    const float travel = card.width + band_w * 2.0f;
    const float band_left = card.x - band_w + travel * shift;
    const float slant = card.height * 0.32f;     // 斜向幅度（列越靠右，光带越靠上）
    const float peak_alpha = 0.10f * (0.6f + 0.4f * breath);
    const float safe_top = card.y + style.card_radius;
    const float safe_bottom = card.y + card.height - style.card_radius;
    // **一条横向渐变矩形**就够，不用逐列。
    //
    // 为何改：逐列 `fill_rect` 在本例是 **138 次调用 / 6.2ms**（每次一次圆角矩形填充）。
    // 而光带本来就是一个“中间亮两端透明”的横向分布——`Gradient::linear` 一次到位，
    // 代价从 6.2ms 降到 < 0.3ms。
    //
    // 代价：不再是**斜**的（斜向原本靠逐列上下错位实现）。但光带是全高竖带、
    // 只活 320ms 且 alpha 上限 0.10——“斜 18°”在这个亮度下看不出来（截图对照过）。
    // 若将来真要斜的，正确做法是旋转整个渐变坐标系（`Gradient` 支持任意两点），
    // 而不是回到逐列。
    const math::Rect band{band_left, safe_top, band_w, safe_bottom - safe_top};
    if (band.width > 0.5f && band.height > 0.5f) {
      (void)slant;
      canvas.fill_rect(
          band,
          raster::Paint::with_gradient(raster::Gradient::linear(
              math::Point{band.x, band.y}, math::Point{band.x + band.width, band.y},
              {raster::GradientStop{0.0f, alpha_of(state.accent, 0.0f)},
               raster::GradientStop{0.5f, alpha_of(state.accent, peak_alpha)},
               raster::GradientStop{1.0f, alpha_of(state.accent, 0.0f)}})),
          0.0f);
    }
  }
  timer.mark("sweep");
}

}  // namespace st::app


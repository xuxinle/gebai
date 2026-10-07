/// 按钮的**几何居中**：图标 + 文字合成后，整块内容必须落在按钮框中心。
///
/// ## 为什么需要这组用例
///
/// 用户报「按钮文本没有居中」。真实应用里量到的现象有两条，性质完全不同：
///
/// | 现象 | 实测 |
/// |---|---|
/// | 纵向系统性偏低 | 17 个按钮中位 **+1.23px**（下偏），填充按钮达 +1.77~+2.02px |
/// | `btn-refresh` 横向偏右 12px | 图标名 `activity` 不在内置表（73 个里没有它）→ **预留了图标空位却没画** |
///
/// 第二条不只是画廊写错名字：**预留空间与"画不画得出来"必须是同一个判据**，
/// 否则任何未知名/缺资源都会静默地把内容推偏。本组用例把它钉住。
///
/// ## 桩为什么必须真画
///
/// 只报宽度、不画墨迹的桩**量不出横向居中**（`measure_width` 对了就永远"居中"），
/// 那是典型的假护栏。这里让桩在 `draw` 时按 `measure_width` 铺一条实心墨带、
/// 并按 `ink_metrics` 铺出确定的墨迹高度——于是"内容包围盒"是图上可量的真东西。
///
/// ## 排除了一个假口径
///
/// 量过一版「按区域 `capture` 截图再量偏移」，同一按钮用两种取样方式差了 10px
/// （区域截图 +0.75 vs 全帧裁剪 +10.50）——那是**量尺自身的缺陷**，不是被测对象。
/// 本组用例不经过截图，直接渲染到画布量像素，没有这条误差。

#include "st/test/test.hpp"

#include <cmath>
#include <string>
#include <vector>

#include "st/raster/canvas.hpp"
#include "st/core/print.hpp"
#include "st/core/string.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/ui_root.hpp"
#include "st/ui/line_layout.hpp"

namespace {

using st::math::Color;
using st::math::Rect;
using st::raster::Canvas;
using st::ui::Button;
using st::ui::RenderContext;
using st::ui::TextPort;
using st::ui::UiRoot;

constexpr int kWidth = 260;
constexpr int kHeight = 120;

/// 会**真的画墨**的文本桩：宽度 = `factor × size`，墨迹高度 = `ink_above`。
///
/// 故意让「行盒」远高于「墨迹」——按行盒居中的实现在这里必然偏。
class DrawingStubPort final : public TextPort {
 public:
  /// 每个字符折算的宽度（em 的倍数）——用来验证宽度确实参与了居中。
  float factor{1.0f};
  /// 墨迹高度（em 的倍数）：模拟“字面墨迹”（真实汉字约 0.85em）。
  float ink_above{0.85f};
  /// 基线**下方**的墨迹（em 的倍数）。
  ///
  /// ⚠ 这一项**不能是 0**：`centered_line_top` 的正确公式是
  /// `shaped_ascent + (below - above)/2`，而 `below == 0` 时它与错误写法
  /// `shaped_ascent - (above + below)/2` **恰好相等**——桩会把差异抹平，
  /// 于是“回退修复后测试仍绿”（逆向验证实测拓到，`CONVENTIONS` §7.2 的同一类坑）。
  /// 取真实量级：DejaVu Sans 汉字回退面 `below ≈ 0.136em`（@14px 即 1.91px）。
  float ink_below{0.136f};
  /// 行盒（em 的倍数）：真实字体约 1.16em，**明显大于墨迹**。
  float box_em{1.164f};
  /// 排版实际基线位（em 的倍数）：与 `ascent` 不同，模拟跨 run 的最大 ascender。
  float shaped_em{1.0f};

  [[nodiscard]] auto measure(std::string_view text, float size) const -> st::math::Size override {
    return st::math::Size{measure_width(text, size, st::text::FontRole::Proportional),
                          line_height(size)};
  }
  [[nodiscard]] auto measure_width(std::string_view text, float size, st::text::FontRole) const
      -> float override {
    return static_cast<float>(st::utf8_length(text)) * size * factor;
  }
  [[nodiscard]] auto line_height(float size) const -> float override { return size * box_em; }
  [[nodiscard]] auto ascent(float size) const -> float override { return size * 0.928f; }
  [[nodiscard]] auto descent(float size) const -> float override { return size * 0.236f; }
  [[nodiscard]] auto shaped_ascent(std::string_view, float size, st::text::FontRole) const
      -> float override {
    return size * shaped_em;
  }
  [[nodiscard]] auto ink_metrics(std::string_view, float size, st::text::FontRole) const
      -> std::optional<InkMetrics> override {
    return InkMetrics{size * ink_above, size * ink_below};
  }
  /// 画一条与 `measure_width` 同宽的实心墨带，墨迹高度按 `ink_above`。
  ///
  /// ⚠ `origin.y` 是**行盒顶部**，不是基线：端口自己把基线放在
  /// `origin.y + shaped_ascent`（与 `st::ui::centered_line_top` 的推导同一口径，
  /// 也是真实端口（`TextRenderer::draw`）的语义）。
  /// 把 `origin.y` 当基线会让墨迹整体上移一个 `shaped_ascent`——
  /// 实测踩到：那样量出来是 -14px 的“巨大偏差”，而实际居中是对的。
  void draw(st::raster::Surface& surface, std::string_view text, st::math::Point origin, float size,
            Color color, st::text::FontRole role, float, bool) const override {
    (void)role;
    const float width = measure_width(text, size, st::text::FontRole::Proportional);
    const float baseline = origin.y + size * shaped_em;
    const float above = size * ink_above;
    const float below = size * ink_below;
    surface.fill_rect(Rect{origin.x, baseline - above, width, above + below},
                      st::raster::Paint::solid(color));
  }
  [[nodiscard]] auto ellipsize(std::string_view utf8, float, float) const -> std::string override {
    return std::string{utf8};
  }
  [[nodiscard]] auto wrap(std::string_view utf8, float, float) const
      -> std::vector<std::string_view> override {
    return {utf8};
  }
  [[nodiscard]] auto wrap_limited(std::string_view utf8, float, float, std::size_t) const
      -> std::vector<std::string> override {
    return {std::string{utf8}};
  }
};

/// 墨迹包围盒（逻辑 px）。`background` 之外的像素都算墨。
struct InkBox {
  bool any{false};
  float x0{0.0f};
  float x1{0.0f};
  float y0{0.0f};
  float y1{0.0f};
  [[nodiscard]] auto center_x() const -> float { return (x0 + x1) * 0.5f; }
  [[nodiscard]] auto center_y() const -> float { return (y0 + y1) * 0.5f; }
};

[[nodiscard]] auto ink_box(const Canvas& canvas, Color background) -> InkBox {
  InkBox box;
  int min_x = canvas.physical_width();
  int min_y = canvas.physical_height();
  int max_x = -1;
  int max_y = -1;
  for (int y = 0; y < canvas.physical_height(); ++y) {
    for (int x = 0; x < canvas.physical_width(); ++x) {
      const Color pixel = canvas.pixel_at(x, y);
      if (pixel == background) continue;
      min_x = std::min(min_x, x);
      max_x = std::max(max_x, x);
      min_y = std::min(min_y, y);
      max_y = std::max(max_y, y);
    }
  }
  if (max_x < 0) return box;
  box.any = true;
  box.x0 = static_cast<float>(min_x);
  box.x1 = static_cast<float>(max_x + 1);
  box.y0 = static_cast<float>(min_y);
  box.y1 = static_cast<float>(max_y + 1);
  return box;
}

/// 把按钮摆到固定位置并渲染，返回墨迹包围盒（**排除按钮自身底色**）。
///
/// 取墨迹的办法：先用透明底色渲染**只画内容**的样子——`Button::paint_content`
/// 只画图标与文字（底色/边框在基类 `paint_box`），所以直接调它即可，
/// 不必从背景色里"挑出"文字（那种挑法在填充按钮上会把底也算成墨）。
[[nodiscard]] auto render_content(const Button& button, const RenderContext& context,
                                  const DrawingStubPort& port) -> InkBox {
  Canvas canvas{kWidth, kHeight, 1.0f};
  const Color background = Color{0, 0, 0, 0};
  canvas.clear(background);
  (void)port;
  button.paint_content(context, canvas);
  return ink_box(canvas, background);
}

void make_root(UiRoot& root, st::ui::Theme& theme) {
  root.set_theme(theme);
  root.set_viewport(st::math::Size{static_cast<float>(kWidth), static_cast<float>(kHeight)});
}

}  // namespace

/// 文字按钮：整块内容（此处只有文字）的墨迹中心必须落在框中心。
ST_TEST(button_content_is_centered_without_icon) {
  DrawingStubPort port{};
  st::ui::Theme theme{st::ui::Theme::light()};
  UiRoot root;
  make_root(root, theme);

  auto owned = std::make_unique<Button>("主要操作");
  Button* button = owned.get();
  button->set_id("btn");
  root.set_content(std::move(owned));

  const Rect box{20.0f, 20.0f, 140.0f, 40.0f};
  const RenderContext context{theme, &port, 0.0};
  button->measure(context, st::ui::Constraints{});
  button->arrange(context, box);

  const InkBox ink = render_content(*button, context, port);
  ST_REQUIRE(ink.any);
  const float dx = ink.center_x() - box.center().x;
  const float dy = ink.center_y() - box.center().y;
  st::print("[btn-center] 无图标 内容中心偏移 dx={:+.3f} dy={:+.3f}（墨迹 {:.1f}×{:.1f}）\n", dx,
            dy, ink.x1 - ink.x0, ink.y1 - ink.y0);
  ST_CHECK_NEAR(dx, 0.0f, 0.5f);
  ST_CHECK_NEAR(dy, 0.0f, 0.5f);
}

/// 图标 + 文字：**两者合成的整块**居中，而不是"文字居中、图标另算"。
///
/// 这条正是 `btn-refresh` 那个缺陷的护栏：图标占位参与了居中计算，
/// 若图标该画却没画，内容重心就会偏向文字一侧（实测右偏 12px）。
ST_TEST(button_icon_and_text_together_are_centered) {
  DrawingStubPort port{};
  st::ui::Theme theme{st::ui::Theme::light()};
  UiRoot root;
  make_root(root, theme);

  auto owned = std::make_unique<Button>("确定");
  Button* button = owned.get();
  root.set_content(std::move(owned));
  button->set_icon("refresh");

  const Rect box{20.0f, 20.0f, 160.0f, 40.0f};
  const RenderContext context{theme, &port, 0.0};
  button->measure(context, st::ui::Constraints{});
  button->arrange(context, box);

  const InkBox ink = render_content(*button, context, port);
  ST_REQUIRE(ink.any);
  const float dx = ink.center_x() - box.center().x;
  const float dy = ink.center_y() - box.center().y;
  st::print("[btn-center] 图标+文字 内容中心偏移 dx={:+.3f} dy={:+.3f}（墨迹 {:.1f}×{:.1f}）\n", dx,
            dy, ink.x1 - ink.x0, ink.y1 - ink.y0);
  // 图标与文字的总宽度应当**真的用上了**：太窄说明图标没画/没算进宽度
  ST_CHECK(ink.x1 - ink.x0 > 40.0f);
  ST_CHECK_NEAR(dx, 0.0f, 0.5f);
  ST_CHECK_NEAR(dy, 0.0f, 0.5f);
}

/// **反向护栏**：图标名不在资源表里时，内容仍必须居中。
///
/// 现状（缺陷）：`measure` 与 `paint_content` 各自判"要不要算图标"，
/// `measure` 按 `icon_` 非空算宽度，`paint_content` 按 `Icon::draw` 画不画得出——
/// 名字无效时前者算、后者不画，文字被空位推向一侧。
/// 修法必须是**同一个判据**（先问资源表里有没有这个图标，再决定算不算宽度）。
ST_TEST(button_with_unknown_icon_stays_centered) {
  DrawingStubPort port{};
  st::ui::Theme theme{st::ui::Theme::light()};
  UiRoot root;
  make_root(root, theme);

  auto owned = std::make_unique<Button>("刷新指标");
  Button* button = owned.get();
  root.set_content(std::move(owned));
  button->set_icon("activity");   // 内置 73 个图标里**没有**这个名字

  const Rect box{20.0f, 20.0f, 160.0f, 28.0f};
  const RenderContext context{theme, &port, 0.0};
  button->measure(context, st::ui::Constraints{});
  button->arrange(context, box);

  const InkBox ink = render_content(*button, context, port);
  ST_REQUIRE(ink.any);
  const float dx = ink.center_x() - box.center().x;
  const float dy = ink.center_y() - box.center().y;
  st::print("[btn-center] 未知图标 内容中心偏移 dx={:+.3f} dy={:+.3f}（墨迹 {:.1f}×{:.1f}）\n", dx,
            dy, ink.x1 - ink.x0, ink.y1 - ink.y0);
  ST_CHECK_NEAR(dx, 0.0f, 0.5f);
  ST_CHECK_NEAR(dy, 0.0f, 0.5f);
}

/// 悬浮时**整块按钮（框体 + 内容）都不移动**——这是用户直接看到的那件事。
///
/// 与上一条的区别：上一条只量**内容**，而 `HoverEffect::lift` 移的是**框体**
/// （背景/描边/阴影）——内容不动、框体上移，用户看到的就是「按钮浮起来了」。
/// 所以必须量**整块的墨迹包围盒**（含框体），否则这条护栏抓不到它。
ST_TEST(button_hover_does_not_shift_the_whole_button) {
  DrawingStubPort port{};
  st::ui::Theme theme{st::ui::Theme::light()};
  UiRoot root;
  make_root(root, theme);

  // 用 Secondary（有实心底 + 描边）：框体必须可见，否则量不到它的位移
  auto owned = std::make_unique<Button>("确定", Button::Variant::Secondary);
  Button* button = owned.get();
  root.set_content(std::move(owned));

  const Rect box{20.0f, 20.0f, 120.0f, 40.0f};
  const auto render = [&](double time) -> InkBox {
    const RenderContext context{theme, &port, time};
    button->measure(context, st::ui::Constraints{});
    button->arrange(context, box);
    Canvas canvas{kWidth, kHeight, 1.0f};
    canvas.clear(theme.colors().surface);
    // ⚠ 直接绘**该元素**，不走 `root.paint`：后者会先自己 `layout()`，
    // 把 `set_content(单个按钮)` 铺满整个视口——于是上面的手动 `arrange` 被覆盖，
    // 框体变成整个画布（实测：静止态量出 y[0,120] = 整个视口）。
    button->paint(context, canvas);
    // 底色就地取（`UiRoot`/主题几层混合后的真实值），不从主题变量推。
    return ink_box(canvas, canvas.pixel_at(3, 3));
  };

  const InkBox rest = render(0.0);
  ST_REQUIRE(rest.any);

  st::ui::Event move;
  move.kind = st::ui::EventKind::MouseMove;
  move.position = st::math::Point{box.center().x, box.center().y};
  (void)root.dispatch(move);
  ST_REQUIRE(button->hovered());

  InkBox hovered = rest;
  for (int i = 0; i < 40; ++i) hovered = render(0.1 * (i + 1));
  ST_REQUIRE(hovered.any);

  const float dy = hovered.center_y() - rest.center_y();
  const float dx = hovered.center_x() - rest.center_x();
  st::print("[btn-shift] 静止 y[{:.2f},{:.2f}] · 悬浮 y[{:.2f},{:.2f}] · 位移 ({:+.3f},{:+.3f})\n",
            static_cast<double>(rest.y0), static_cast<double>(rest.y1),
            static_cast<double>(hovered.y0), static_cast<double>(hovered.y1),
            static_cast<double>(dx), static_cast<double>(dy));
  // 用户报的「按钮悬浮时不要上移」：整块不得位移。
  ST_CHECK_NEAR(dy, 0.0f, 0.25f);
  ST_CHECK_NEAR(dx, 0.0f, 0.25f);
}

/// 悬浮**应当**有可见反馈（不能为了“不动”把反馈一并删掉）。
///
/// 这条是上一条的对偶：只断言“不动”很容易用“什么都不做”蒙混过关。
ST_TEST(button_hover_still_has_visual_feedback) {
  DrawingStubPort port{};
  st::ui::Theme theme{st::ui::Theme::light()};
  UiRoot root;
  make_root(root, theme);

  auto owned = std::make_unique<Button>("确定", Button::Variant::Secondary);
  Button* button = owned.get();
  root.set_content(std::move(owned));
  const Rect box{20.0f, 20.0f, 120.0f, 40.0f};
  const auto sample = [&](double time) -> Color {
    const RenderContext context{theme, &port, time};
    button->measure(context, st::ui::Constraints{});
    button->arrange(context, box);
    Canvas canvas{kWidth, kHeight, 1.0f};
    canvas.clear(theme.colors().surface);
    button->paint(context, canvas);
    return canvas.pixel_at(static_cast<int>(box.x) + 6, static_cast<int>(box.center().y));
  };
  const Color rest = sample(0.0);
  st::ui::Event move;
  move.kind = st::ui::EventKind::MouseMove;
  move.position = st::math::Point{box.center().x, box.center().y};
  (void)root.dispatch(move);
  Color hovered = rest;
  for (int i = 0; i < 40; ++i) hovered = sample(0.1 * (i + 1));
  st::print("[btn-feedback] 静止 #{:02X}{:02X}{:02X} · 悬浮 #{:02X}{:02X}{:02X}\n",
            static_cast<unsigned>(rest.r), static_cast<unsigned>(rest.g),
            static_cast<unsigned>(rest.b), static_cast<unsigned>(hovered.r),
            static_cast<unsigned>(hovered.g), static_cast<unsigned>(hovered.b));
  ST_CHECK(!(rest == hovered));
}

/// 焦点环与控件自己的描边是**同一道边框**，不是“框外套框”。
///
/// 用户报「按钮聚焦不要再嵌套一层边框」。旧实现在按钮**内部**缩进 2px 画环，
/// 于是 `Secondary`（自带 1px 描边）上能看到**三层**：外描边 / 2px 空底 / 内环；
/// 而共用助手的其它控件（`Input`/`Select`/`Checkbox`）是环从外沿 **往外** 画：
/// 它们的描边占 `[R-bw, R]`、环从 `R` 开始，两道**相邻不同色**的带子并排。
/// 统一后的契约：**环完整盖住控件自己的描边**（环从 `R-bw` 起画），
/// 不管是哪一种，最终都只有**一道环**的宽度。
///
/// 判据（必须用像素，不能用“有没有变化”）：
/// 1. 未聚焦时控件边框已占满 `[R-bw, R]`（原本就是实色）；
/// 2. 聚焦后**边框带外**（`x < R - bw`）的像素不得改变——环没往外长；
/// 3. 聚焦后**整个边框带**必须变成环色——环完整盖住了描边；
/// 4. 环覆盖总宽度（沿水平中线的改变像素数）不超过 `focus_width` 向上取整——
///    超过就是“描边 + 环”两道并排，正是用户看到的那种。
ST_TEST(button_focus_ring_covers_its_own_border) {
  DrawingStubPort port{};
  st::ui::Theme theme{st::ui::Theme::light()};
  UiRoot root;
  make_root(root, theme);

  const auto& metrics = theme.metrics();
  const float focus = std::max(metrics.focus_width, metrics.border_width);
  const Color ring = theme.colors().focus_ring;

  // 只取**紧贴显示器顶边**的按钮：它的四条边都不会被视口裁掉。
  const Rect box{20.0f, 0.0f, 120.0f, 40.0f};
  const int mid_y = static_cast<int>(box.center().y);

  auto render = [&](bool focused) {  // NOLINT(readability-identifier-length)
    Canvas canvas{kWidth, kHeight, 1.0f};
    canvas.clear(theme.colors().bg);
    root.set_content(nullptr);
    auto owned = std::make_unique<Button>("确定", Button::Variant::Secondary);
    Button* button = owned.get();
    root.set_content(std::move(owned));
    const RenderContext context{theme, &port, 0.0};
    button->measure(context, st::ui::Constraints{});
    button->arrange(context, box);
    if (focused) root.set_focus(button);
    button->paint(context, canvas);
    return canvas;
  };

  const Canvas plain = render(false);
  const Canvas focused = render(true);
  const auto composite = [&ring](Color under) -> Color {
    const float alpha = static_cast<float>(ring.a) / 255.0f;
    const auto mix = [alpha](std::uint8_t top, std::uint8_t bottom) -> std::uint8_t {
      return static_cast<std::uint8_t>(std::lround(
          static_cast<float>(top) * alpha + static_cast<float>(bottom) * (1.0f - alpha)));
    };
    return Color{mix(ring.r, under.r), mix(ring.g, under.g), mix(ring.b, under.b), 255U};
  };
  const auto near = [](Color lhs, Color rhs) -> bool {
    const int dr = static_cast<int>(lhs.r) - static_cast<int>(rhs.r);
    const int dg = static_cast<int>(lhs.g) - static_cast<int>(rhs.g);
    const int db = static_cast<int>(lhs.b) - static_cast<int>(rhs.b);
    return dr * dr + dg * dg + db * db <= 144;  // 12/通道（抗锯齿）
  };

  // **实测环带**（不假设）：沿水平中线扫一遍，把“相对于未聚焦帧发生变化的像素”
  // 按连通段切开，取包含控件左边的那一段。
  const auto bands_along_midline = [&](int mid) {
    std::vector<std::pair<int, int>> bands;
    int start = -1;
    for (int x = 0; x < kWidth; ++x) {
      const bool changed = !(focused.pixel_at(x, mid) == plain.pixel_at(x, mid));
      if (changed && start < 0) start = x;
      if (!changed && start >= 0) {
        bands.emplace_back(start, x - 1);
        start = -1;
      }
    }
    if (start >= 0) bands.emplace_back(start, kWidth - 1);
    return bands;
  };
  const std::vector<std::pair<int, int>> bands = bands_along_midline(mid_y);
  ST_REQUIRE(!bands.empty());
  const std::pair<int, int> left_band = bands.front();
  const int left_x = static_cast<int>(std::floor(box.x));
  const int band_width = left_band.second - left_band.first + 1;
  st::print("[btn-focus] 左环带 x[{},{}] 宽 {}px（控件左边 {}，上限 ceil(max(focus,border))={}）\n",
            left_band.first, left_band.second, band_width, left_x,
            static_cast<int>(std::ceil(focus)));
  // ① 环带**不得向外多长**：最左像素不早于 `R - ceil(width)`（环从 `R-bw` 起画，
  //    描边居中后可能被像素吸附到左一像素，但不会再多）。
  const int allowed_first = left_x - static_cast<int>(std::ceil(focus));
  ST_CHECK(left_band.first >= allowed_first);
  // ② 环带**不得比一道环更宽**（宽了就是“描边 + 环”两道并排）。
  ST_CHECK(band_width <= static_cast<int>(std::ceil(focus)) + 1);
  // ③ 环带必须盖到控件外沿 `R`（而不是停在 `R-bw` 处、又另起一道）。
  ST_CHECK(left_band.second >= left_x);
  // ④ 环带**更外侧**逐像素不变。
  for (int x = 0; x < allowed_first; ++x) {
    ST_CHECK(plain.pixel_at(x, mid_y) == focused.pixel_at(x, mid_y));
  }
  // ⑤ 对偶：环真的画出来了，且颜色是环色合成（不是把边框擦掉）。
  ST_CHECK(near(focused.pixel_at(left_x, mid_y), composite(plain.pixel_at(left_x, mid_y))));
  ST_CHECK(!(focused.pixel_at(left_x, mid_y) == plain.pixel_at(left_x, mid_y)));
}

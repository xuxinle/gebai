/// 开关类控件单测：`Checkbox` / `Radio` / `Switch`（状态、语义、四态渲染、键盘可达、焦点环）。
///
/// 宿主约束：ui 层不依赖 text 层，本测试作为宿主把 `text::TextRenderer` 适配成 `ui::TextPort`
/// 注入 `UiRoot`（系统字体缺失时退化为 `NullTextPort`，布局仍可运行）。

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/test/test.hpp"

#include "st/text/font.hpp"
#include "st/text/text.hpp"
#include "st/ui/components/toggle.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::math::Color;
using st::math::Point;
using st::math::Rect;

// ————————————————— 测试夹具：真实字体端口（可选） —————————————————

/// 把 `text::TextRenderer` 适配成 `ui::TextPort`（ui 层不依赖 text 层，测试作为宿主注入）。
class FontPort final : public st::ui::TextPort {
 public:
  explicit FontPort(const st::text::FontStack& stack) : renderer_(stack) {}

  [[nodiscard]] auto measure(std::string_view utf8, float size) const -> st::math::Size override {
    return renderer_.measure(utf8, size);
  }
  [[nodiscard]] auto measure_width(std::string_view utf8, float size) const -> float override {
    return renderer_.measure_width(utf8, size);
  }
  [[nodiscard]] auto line_height(float size) const -> float override {
    return renderer_.line_height(size);
  }
  void draw(st::raster::Canvas& canvas, std::string_view utf8, st::math::Point origin, float size,
            st::math::Color color) const override {
    (void)renderer_.draw(canvas, utf8, origin, size, color);
  }
  [[nodiscard]] auto ellipsize(std::string_view utf8, float size, float max_width) const
      -> std::string override {
    return renderer_.ellipsize(utf8, size, max_width);
  }
  [[nodiscard]] auto wrap(std::string_view utf8, float size, float max_width) const
      -> std::vector<std::string_view> override {
    return renderer_.wrap(utf8, size, max_width);
  }
  [[nodiscard]] auto wrap_limited(std::string_view utf8, float size, float max_width,
                                  std::size_t max_lines) const -> std::vector<std::string> override {
    std::vector<std::string> lines;
    for (const auto& line : renderer_.wrap(utf8, size, max_width)) {
      if (lines.size() >= max_lines) break;
      lines.emplace_back(line);
    }
    return lines;
  }

 private:
  st::text::TextRenderer renderer_;
};

struct Harness {
  std::optional<st::text::FontStack> stack{};
  std::unique_ptr<FontPort> port{};
  st::ui::UiRoot root{};

  Harness() {
    const auto loaded = st::text::FontStack::system_default();
    if (!loaded.has_value()) {
      std::cout << "[ui_toggle_test] 未找到系统字体，改用 NullTextPort（文本宽度按 0 计）\n";
      return;
    }
    stack.emplace(std::move(*loaded));
    port = std::make_unique<FontPort>(*stack);
    root.set_text_port(port.get());
  }

  /// 叠加层先摘、内容后销毁：`~Select` 可能回调 `overlay_remove`，需保证根容器仍存活。
  ~Harness() {
    root.clear_overlays();
    root.set_content(nullptr);
  }

  Harness(const Harness&) = delete;
  auto operator=(const Harness&) -> Harness& = delete;
};

// ————————————————— 交互与像素助手 —————————————————

auto dispatch_kind(st::ui::UiRoot& root, st::ui::EventKind kind, Point point) -> bool {
  st::ui::Event event;
  event.kind = kind;
  event.position = point;
  return root.dispatch(event);
}

/// 完整点击（down → up → click）：验证同一按压周期内只切换一次。
auto click_at(st::ui::UiRoot& root, Point point) -> bool {
  const bool handled = dispatch_kind(root, st::ui::EventKind::MouseDown, point);
  (void)dispatch_kind(root, st::ui::EventKind::MouseUp, point);
  (void)dispatch_kind(root, st::ui::EventKind::Click, point);
  return handled;
}

auto press_key(st::ui::UiRoot& root, std::string_view key) -> bool {
  st::ui::Event event;
  event.kind = st::ui::EventKind::KeyDown;
  event.key = std::string(key);
  return root.dispatch(event);
}

/// 焦点环落屏色：`focus_ring` 自带 alpha，与各浅色底色 token 合成后即为可见像素色。
auto ring_composites(const st::ui::Theme& theme) -> std::array<Color, 4> {
  const Color ring = theme.colors().focus_ring;
  const float alpha = static_cast<float>(ring.a) / 255.0f;
  const auto channel = [alpha](std::uint8_t base, std::uint8_t top) -> std::uint8_t {
    const float value =
        static_cast<float>(base) * (1.0f - alpha) + static_cast<float>(top) * alpha;
    return static_cast<std::uint8_t>(std::lround(value));
  };
  std::array<Color, 4> out{};
  const std::array<Color, 4> backdrops{theme.colors().bg, theme.colors().surface,
                                       theme.colors().surface_alt,
                                       theme.colors().surface_sunken};
  for (std::size_t index = 0; index < backdrops.size(); ++index) {
    out[index] = Color{channel(backdrops[index].r, ring.r), channel(backdrops[index].g, ring.g),
                       channel(backdrops[index].b, ring.b), 255};
  }
  return out;
}

[[nodiscard]] auto near_ring(Color pixel, const std::array<Color, 4>& expected) -> bool {
  for (const auto& candidate : expected) {
    const int dr = static_cast<int>(pixel.r) - static_cast<int>(candidate.r);
    const int dg = static_cast<int>(pixel.g) - static_cast<int>(candidate.g);
    const int db = static_cast<int>(pixel.b) - static_cast<int>(candidate.b);
    if (dr * dr + dg * dg + db * db <= 100) return true;  // 10/通道容差
  }
  return false;
}

[[nodiscard]] auto region_pixels(Rect region) -> std::vector<Point> {
  std::vector<Point> points;
  const int x0 = static_cast<int>(std::floor(region.x));
  const int y0 = static_cast<int>(std::floor(region.y));
  const int x1 = static_cast<int>(std::ceil(region.right()));
  const int y1 = static_cast<int>(std::ceil(region.bottom()));
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      points.push_back(Point{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f});
    }
  }
  return points;
}

/// `region` 内「相对未聚焦帧发生变化、且其颜色落在『原底色 → 焦点环满覆盖合成色』线段 ≥35% 处」
/// 的像素数。焦点环自带 alpha，且在圆弧/圆角处存在抗锯齿，故按投影比例判定而不比对精确色值。
auto focus_ring_pixels(const st::raster::Canvas& focused, const st::raster::Canvas& plain,
                       Rect region, const st::ui::Theme& theme) -> int {
  const auto expected = ring_composites(theme);
  const std::array<Color, 4> backdrops{theme.colors().bg, theme.colors().surface,
                                       theme.colors().surface_alt,
                                       theme.colors().surface_sunken};
  const int x0 = static_cast<int>(std::floor(region.x));
  const int y0 = static_cast<int>(std::floor(region.y));
  const int x1 = static_cast<int>(std::ceil(region.right()));
  const int y1 = static_cast<int>(std::ceil(region.bottom()));
  int count = 0;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      const Point point{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f};
      const Color before = plain.pixel_at_point(point);
      const Color after = focused.pixel_at_point(point);
      if (before == after) continue;
      float best = 0.0f;
      for (std::size_t index = 0; index < backdrops.size(); ++index) {
        if (!(before == backdrops[index])) continue;  // 只在纯底色像素上判定，避免其它元素抗锯齿噪声
        const Color full = expected[index];
        const float dr = static_cast<float>(full.r) - static_cast<float>(before.r);
        const float dg = static_cast<float>(full.g) - static_cast<float>(before.g);
        const float db = static_cast<float>(full.b) - static_cast<float>(before.b);
        const float denominator = dr * dr + dg * dg + db * db;
        if (denominator <= 0.0f) continue;
        const float ar = static_cast<float>(after.r) - static_cast<float>(before.r);
        const float ag = static_cast<float>(after.g) - static_cast<float>(before.g);
        const float ab = static_cast<float>(after.b) - static_cast<float>(before.b);
        const float ratio = (ar * dr + ag * dg + ab * db) / denominator;
        if (ratio > best) best = ratio;
      }
      if (best >= 0.35f) ++count;
    }
  }
  return count;
}

/// 单帧内的焦点环合成色像素数（用于「未聚焦时不应出现焦点环」的反向断言）。
auto ring_pixels_in(const st::raster::Canvas& canvas, Rect region,
                    const st::ui::Theme& theme) -> int {
  const auto expected = ring_composites(theme);
  int count = 0;
  for (const auto point : region_pixels(region)) {
    if (near_ring(canvas.pixel_at_point(point), expected)) ++count;
  }
  return count;
}

auto any_pixel_differs(const st::raster::Canvas& left, const st::raster::Canvas& right) -> bool {
  for (int y = 0; y < left.physical_height(); ++y) {
    for (int x = 0; x < left.physical_width(); ++x) {
      if (left.pixel_at(x, y) != right.pixel_at(x, y)) return true;
    }
  }
  return false;
}

/// 同一控件分别以「未聚焦 / 聚焦」渲染一次（清底为 `bg`，便于比对焦点环合成色）。
struct FocusRender {
  st::raster::Canvas plain;
  st::raster::Canvas focused;
  st::ui::Theme theme{};
};

auto render_focus_pair(st::ui::UiRoot& root, st::ui::Element* target) -> FocusRender {
  const int width = static_cast<int>(root.viewport().width);
  const int height = static_cast<int>(root.viewport().height);
  root.layout(true);

  st::raster::Canvas plain(width, height);
  plain.clear(root.theme().colors().bg);
  root.paint(plain);

  root.set_focus(target);
  st::raster::Canvas focused(width, height);
  focused.clear(root.theme().colors().bg);
  root.paint(focused);
  root.set_focus(nullptr);
  return FocusRender{std::move(plain), std::move(focused), root.theme()};
}

[[nodiscard]] auto has_property(const st::ui::Element& element, std::string_view name) -> bool {
  const auto names = element.property_names();
  return std::ranges::find(names, name) != names.end();
}

[[nodiscard]] auto find_role(const st::ui::SemanticsNode& node, st::ui::Role role)
    -> const st::ui::SemanticsNode* {
  if (node.role == role) return &node;
  for (const auto& child : node.children) {
    if (const auto* hit = find_role(child, role); hit != nullptr) return hit;
  }
  return nullptr;
}

/// 三态控件共用夹具：列布局 + 内边距（给焦点环留位）。
struct TrioFixture {
  Harness harness{};
  st::ui::Checkbox* box{nullptr};
  st::ui::Radio* radio{nullptr};
  st::ui::Switch* toggle{nullptr};

  TrioFixture() {
    harness.root.set_viewport(st::math::Size{360.0f, 200.0f});
    auto content = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
    content->style().padding = st::math::Insets::all(12.0f);
    content->style().gap = 10.0f;
    box = static_cast<st::ui::Checkbox*>(
        content->add_child(std::make_unique<st::ui::Checkbox>("启用通知")));
    radio =
        static_cast<st::ui::Radio*>(content->add_child(std::make_unique<st::ui::Radio>("自动模式")));
    toggle = static_cast<st::ui::Switch*>(
        content->add_child(std::make_unique<st::ui::Switch>("深色主题")));
    harness.root.set_content(std::move(content));
    harness.root.layout(true);
  }
};

}  // namespace

// ————————————————— Checkbox —————————————————

ST_TEST(ui_checkbox_state_roundtrip) {
  st::ui::Checkbox box("启用通知");
  ST_CHECK(box.role() == st::ui::Role::Checkbox);
  ST_CHECK_EQ(std::string(box.type()), std::string("Checkbox"));
  ST_CHECK(!box.checked());
  ST_CHECK_EQ(box.semantics_value(), std::string("false"));
  ST_CHECK_EQ(box.semantics_text(), std::string("启用通知"));
  ST_CHECK(has_property(box, "checked"));
  ST_CHECK(has_property(box, "label"));

  int changes = 0;
  bool reported = false;
  box.on_change = [&](bool value) {
    ++changes;
    reported = value;
  };

  box.activate();
  ST_CHECK(box.checked());
  ST_CHECK_EQ(changes, 1);
  ST_CHECK(reported);
  ST_CHECK_EQ(box.semantics_value(), std::string("true"));
  ST_CHECK(box.semantics_flags().checked);
  const auto property = box.get_property("checked");
  ST_REQUIRE(property.has_value());
  ST_CHECK_EQ(box.semantics_flags().checked, *property == std::string_view("true"));

  // 控制通道写入不触发 on_change（避免回环），非法取值被拒。
  ST_CHECK(box.set_property("checked", "false"));
  ST_CHECK(!box.checked());
  ST_CHECK_EQ(changes, 1);
  ST_CHECK(!box.set_property("checked", "maybe"));
  ST_CHECK(box.set_property("label", "新标签"));
  ST_CHECK_EQ(box.label(), std::string("新标签"));
  ST_CHECK(box.set_property("value", "yes"));
  ST_CHECK(box.checked());

  // 禁用仅拦交互，控制通道仍可读写。
  box.set_enabled(false);
  box.activate();
  ST_CHECK(box.checked());
  ST_CHECK_EQ(changes, 1);
  ST_CHECK(box.set_property("checked", std::string_view("false")));
  ST_CHECK(!box.checked());
}

ST_TEST(ui_checkbox_click_dedup_and_keyboard) {
  TrioFixture fixture;
  auto& root = fixture.harness.root;
  auto* box = fixture.box;
  ST_REQUIRE(box != nullptr);
  ST_REQUIRE(!box->bounds().is_empty());

  int changes = 0;
  box->on_change = [&](bool) { ++changes; };

  // down+up+click 三帧只切换一次。
  ST_CHECK(click_at(root, box->bounds().center()));
  ST_CHECK(box->checked());
  ST_CHECK_EQ(changes, 1);
  const auto property = box->get_property("checked");
  ST_REQUIRE(property.has_value());
  ST_CHECK_EQ(box->semantics_flags().checked, *property == std::string_view("true"));

  ST_CHECK(click_at(root, box->bounds().center()));
  ST_CHECK(!box->checked());
  ST_CHECK_EQ(changes, 2);

  // 焦点态下 Space/Enter 切换。
  root.set_focus(box);
  ST_CHECK(press_key(root, " "));
  ST_CHECK(box->checked());
  ST_CHECK(press_key(root, "Enter"));
  ST_CHECK(!box->checked());
  ST_CHECK_EQ(changes, 4);

  // 禁用后点击无效（UiRoot 的 click 兜底 activate 也被 enabled 拦住）。
  box->set_enabled(false);
  (void)click_at(root, box->bounds().center());
  ST_CHECK(!box->checked());
  ST_CHECK_EQ(changes, 4);
}

// ————————————————— Radio —————————————————

ST_TEST(ui_radio_selection_is_idempotent) {
  st::ui::Radio radio("手动模式");
  ST_CHECK(radio.role() == st::ui::Role::Radio);
  ST_CHECK(!radio.checked());
  ST_CHECK(!radio.semantics_flags().checked);

  int changes = 0;
  radio.on_change = [&](bool value) {
    if (value) ++changes;
  };
  radio.activate();
  ST_CHECK(radio.checked());
  ST_CHECK(radio.semantics_flags().checked);
  ST_CHECK_EQ(changes, 1);

  // 单选语义：再次点击不取消选中，也不重复通知。
  radio.activate();
  ST_CHECK(radio.checked());
  ST_CHECK_EQ(changes, 1);

  // 程序化写入可由调用方完成分组互斥。
  radio.set_checked(false);
  ST_CHECK(!radio.checked());
  const auto property = radio.get_property("checked");
  ST_REQUIRE(property.has_value());
  ST_CHECK_EQ(radio.semantics_flags().checked, *property == std::string_view("true"));
}

// ————————————————— Switch —————————————————

ST_TEST(ui_switch_toggle_roundtrip) {
  st::ui::Switch toggle("深色主题");
  ST_CHECK(toggle.role() == st::ui::Role::Switch);
  ST_CHECK(!toggle.checked());
  ST_CHECK(has_property(toggle, "checked"));

  int changes = 0;
  bool reported = false;
  toggle.on_change = [&](bool value) {
    ++changes;
    reported = value;
  };
  toggle.toggle();
  ST_CHECK(toggle.checked());
  ST_CHECK(reported);
  ST_CHECK_EQ(changes, 1);
  const auto property = toggle.get_property("checked");
  ST_REQUIRE(property.has_value());
  ST_CHECK_EQ(toggle.semantics_flags().checked, *property == std::string_view("true"));
  ST_CHECK(toggle.set_property("value", "false"));
  ST_CHECK(!toggle.checked());
  ST_CHECK_EQ(toggle.semantics_value(), std::string("false"));
  ST_CHECK_EQ(changes, 1);
}

ST_TEST(ui_switch_click_and_keyboard) {
  TrioFixture fixture;
  auto& root = fixture.harness.root;
  auto* toggle = fixture.toggle;
  ST_REQUIRE(toggle != nullptr);
  ST_REQUIRE(!toggle->bounds().is_empty());

  // 点击轨道（40×22 胶囊位于控件左侧）。
  const st::math::Point track{toggle->bounds().x + 10.0f, toggle->bounds().center().y};  ST_CHECK(click_at(root, track));
  ST_CHECK(toggle->checked());
  const auto property = toggle->get_property("checked");
  ST_REQUIRE(property.has_value());
  ST_CHECK_EQ(toggle->semantics_flags().checked, *property == std::string_view("true"));

  root.set_focus(toggle);
  ST_CHECK(press_key(root, "Space"));
  ST_CHECK(!toggle->checked());
  ST_CHECK(press_key(root, " "));
  ST_CHECK(toggle->checked());
}

ST_TEST(ui_switch_animation_follows_time_axis) {
  const st::ui::Theme theme = st::ui::Theme::light();
  const st::ui::RenderContext idle{theme, nullptr, 0.0};
  st::ui::Switch toggle("动效");
  toggle.measure(idle, st::ui::Constraints{});
  toggle.arrange(idle, st::math::Rect{0.0f, 0.0f, 120.0f, 26.0f});

  // 静态帧先落定（记录时间轴），随后的切换才有动画可推演。
  st::raster::Canvas settled(140, 30);
  settled.clear(theme.colors().bg);
  toggle.paint_content(idle, settled);

  toggle.set_checked(true);
  st::raster::Canvas start(140, 30);
  start.clear(theme.colors().bg);
  toggle.paint_content(st::ui::RenderContext{theme, nullptr, 0.05}, start);

  st::raster::Canvas middle(140, 30);
  middle.clear(theme.colors().bg);
  toggle.paint_content(st::ui::RenderContext{theme, nullptr, 0.14}, middle);

  st::raster::Canvas done(140, 30);
  done.clear(theme.colors().bg);
  toggle.paint_content(st::ui::RenderContext{theme, nullptr, 0.60}, done);

  st::raster::Canvas stable(140, 30);
  stable.clear(theme.colors().bg);
  toggle.paint_content(st::ui::RenderContext{theme, nullptr, 1.20}, stable);

  ST_CHECK(any_pixel_differs(start, middle));  // 180ms 中段：滑块位置随钟推进
  ST_CHECK(any_pixel_differs(middle, done));   // 动画结束：落到位
  ST_CHECK(!any_pixel_differs(done, stable));  // 结束后画面稳定（不回退）
}

// ————————————————— 焦点环 / 语义 / 视觉树 —————————————————

ST_TEST(ui_toggle_focus_ring_pixels) {
  TrioFixture fixture;
  auto& root = fixture.harness.root;
  ST_REQUIRE(fixture.box != nullptr && fixture.radio != nullptr && fixture.toggle != nullptr);
  ST_REQUIRE(!fixture.box->bounds().is_empty());

  const auto box_ring = render_focus_pair(root, fixture.box);
  ST_CHECK(focus_ring_pixels(box_ring.focused, box_ring.plain, fixture.box->bounds().inflate(4.0f),
                             box_ring.theme) >= 20);
  ST_CHECK_EQ(ring_pixels_in(box_ring.plain, fixture.box->bounds().inflate(4.0f), box_ring.theme),
              0);
  // 精确到单像素：指示器左侧环带中点必须是焦点环合成色，未聚焦帧则是底色。
  const Point probe{fixture.box->bounds().x + 3.0f, fixture.box->bounds().center().y};
  ST_CHECK(near_ring(box_ring.focused.pixel_at_point(probe), ring_composites(box_ring.theme)));
  ST_CHECK(box_ring.focused.pixel_at_point(probe) != box_ring.plain.pixel_at_point(probe));

  const auto radio_ring = render_focus_pair(root, fixture.radio);
  ST_CHECK(focus_ring_pixels(radio_ring.focused, radio_ring.plain,
                             fixture.radio->bounds().inflate(4.0f), radio_ring.theme) >= 20);
  ST_CHECK_EQ(
      ring_pixels_in(radio_ring.plain, fixture.radio->bounds().inflate(4.0f), radio_ring.theme),
      0);

  const auto switch_ring = render_focus_pair(root, fixture.toggle);
  ST_CHECK(focus_ring_pixels(switch_ring.focused, switch_ring.plain,
                             fixture.toggle->bounds().inflate(4.0f), switch_ring.theme) >= 20);
  ST_CHECK_EQ(ring_pixels_in(switch_ring.plain, fixture.toggle->bounds().inflate(4.0f),
                             switch_ring.theme),
              0);
}

ST_TEST(ui_toggle_visual_tree_and_semantics) {
  TrioFixture fixture;
  auto& root = fixture.harness.root;
  fixture.box->set_checked(true);
  fixture.toggle->set_checked(true);
  root.layout(true);

  const st::ui::VisualNode visual = root.visual_tree();
  ST_REQUIRE(visual.children.size() == 1U);
  const st::ui::VisualNode& content = visual.children[0];
  ST_REQUIRE(content.children.size() == 3U);
  for (const auto& node : content.children) {
    ST_CHECK(!node.bounds.is_empty());
    ST_CHECK(node.hit_target);
  }

  const st::ui::SemanticsNode tree = root.semantics(0);
  const auto* box_node = find_role(tree, st::ui::Role::Checkbox);
  const auto* radio_node = find_role(tree, st::ui::Role::Radio);
  const auto* switch_node = find_role(tree, st::ui::Role::Switch);
  ST_REQUIRE(box_node != nullptr);
  ST_REQUIRE(radio_node != nullptr);
  ST_REQUIRE(switch_node != nullptr);
  ST_CHECK(box_node->flags.checked);
  ST_CHECK(box_node->flags.enabled);
  ST_CHECK(!radio_node->flags.checked);
  ST_CHECK(switch_node->flags.checked);
  ST_CHECK_EQ(switch_node->type, std::string("Switch"));
  ST_CHECK(!box_node->bounds.is_empty());

  root.set_focus(fixture.radio);
  const st::ui::SemanticsNode focused_tree = root.semantics(0);
  const auto* focused_radio = find_role(focused_tree, st::ui::Role::Radio);
  ST_REQUIRE(focused_radio != nullptr);
  ST_CHECK(focused_radio->flags.focused);
  ST_CHECK(fixture.radio->focused());
  ST_CHECK(!fixture.box->focused());
}

ST_TEST(ui_toggle_tab_focus_order) {
  TrioFixture fixture;
  auto& root = fixture.harness.root;
  root.set_focus(fixture.toggle);
  ST_CHECK(press_key(root, "Tab"));
  ST_CHECK(root.focused() == fixture.box);
  ST_CHECK(press_key(root, "Tab"));
  ST_CHECK(root.focused() == fixture.radio);

  st::ui::Event back;
  back.kind = st::ui::EventKind::KeyDown;
  back.key = "Tab";
  back.shift = true;
  ST_CHECK(root.dispatch(back));
  ST_CHECK(root.focused() == fixture.box);
}

/// 滑杆单测：`Slider`（点击/拖拽取值、夹取、键盘步进、属性面、焦点环、语义与视觉树）。
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
#include <utility>
#include <vector>

#include "st/core/string.hpp"
#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/test/test.hpp"
#include "st/text/font.hpp"
#include "st/text/text.hpp"
#include "st/ui/components/slider.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::math::Color;
using st::math::Point;
using st::math::Rect;

/// 把 `text::TextRenderer` 适配成 `ui::TextPort`（ui 层不依赖 text 层，测试作为宿主注入）。
class FontPort final : public st::ui::TextPort {
 public:
  explicit FontPort(const st::text::FontStack& stack) : renderer_(stack) {}

  [[nodiscard]] auto measure(std::string_view utf8, float size) const -> st::math::Size override {
    return renderer_.measure(utf8, size);
  }
  [[nodiscard]] auto measure_width(std::string_view utf8, float size,
                     st::text::FontRole role = st::text::FontRole::Proportional) const
      -> float override {
      (void)role;  // 桩：等宽与比例同宽，无需区分
    return renderer_.measure_width(utf8, size, role);
  }
  [[nodiscard]] auto line_height(float size) const -> float override {
    return renderer_.line_height(size);
  }
  void draw(st::raster::Surface& canvas, std::string_view utf8, st::math::Point origin, float size,
            st::math::Color color,
            st::text::FontRole role = st::text::FontRole::Proportional,
            float embolden = 0.0f, bool = false) const override {
    (void)role;      // 桩
    (void)embolden;  // 桩
    (void)renderer_.draw(canvas, utf8, origin, size, color, role, embolden);
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

/// 滑块半径（与组件文档一致：16px 滑块 → 半径 8；控件压矮时随高度收缩）。
constexpr float k_knob_radius = 8.0f;

struct Harness {
  std::optional<st::text::FontStack> stack{};
  std::unique_ptr<FontPort> port{};
  st::ui::UiRoot root{};

  Harness() {
    const auto loaded = st::text::FontStack::system_default();
    if (!loaded.has_value()) {
      std::cout << "[ui_slider_test] 未找到系统字体，改用 NullTextPort（文本宽度按 0 计）\n";
      return;
    }
    stack.emplace(std::move(*loaded));
    port = std::make_unique<FontPort>(*stack);
    root.set_text_port(port.get());
  }

  ~Harness() {
    root.clear_overlays();
    root.set_content(nullptr);
  }

  Harness(const Harness&) = delete;
  auto operator=(const Harness&) -> Harness& = delete;
};

auto dispatch_kind(st::ui::UiRoot& root, st::ui::EventKind kind, Point point) -> bool {
  st::ui::Event event;
  event.kind = kind;
  event.position = point;
  return root.dispatch(event);
}

auto press_key(st::ui::UiRoot& root, std::string_view key) -> bool {
  st::ui::Event event;
  event.kind = st::ui::EventKind::KeyDown;
  event.key = std::string(key);
  return root.dispatch(event);
}

/// 滑杆几何：取值 0/1 对应滑块中心的内缘位置（`[x + r, right - r]`）。
[[nodiscard]] auto value_at(const st::ui::Slider& slider, float x) -> float {
  const Rect bounds = slider.bounds();
  const float travel = bounds.width - k_knob_radius * 2.0f;
  if (travel <= 0.0f) return slider.value();
  return std::clamp((x - (bounds.x + k_knob_radius)) / travel, 0.0f, 1.0f);
}

/// 把 `top`（带 alpha）叠加到不透明底色 `base` 上，得到落屏色。
[[nodiscard]] auto blend_over(Color base, Color top) -> Color {
  const float alpha = static_cast<float>(top.a) / 255.0f;
  const auto channel = [alpha](std::uint8_t under, std::uint8_t over) -> std::uint8_t {
    const float value =
        static_cast<float>(under) * (1.0f - alpha) + static_cast<float>(over) * alpha;
    return static_cast<std::uint8_t>(std::lround(value));
  };
  return Color{channel(base.r, top.r), channel(base.g, top.g), channel(base.b, top.b), 255};
}



auto focus_ring_pixels(const st::raster::Canvas& focused, const st::raster::Canvas& plain,
                       Rect region, const st::ui::Theme& theme) -> int {
  // 焦点环自带 alpha，且在圆弧/圆角处存在抗锯齿，故按「原底色 → 满覆盖合成色」的投影比例判定。
  // 滑杆的环压在滑块阴影上（底色不是纯 token 色），因此以该像素自身原色为基准做投影。
  const Color ring = theme.colors().focus_ring;
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
      const Color full = blend_over(before, ring);
      const float dr = static_cast<float>(full.r) - static_cast<float>(before.r);
      const float dg = static_cast<float>(full.g) - static_cast<float>(before.g);
      const float db = static_cast<float>(full.b) - static_cast<float>(before.b);
      const float denominator = dr * dr + dg * dg + db * db;
      if (denominator <= 0.0f) continue;
      const float ar = static_cast<float>(after.r) - static_cast<float>(before.r);
      const float ag = static_cast<float>(after.g) - static_cast<float>(before.g);
      const float ab = static_cast<float>(after.b) - static_cast<float>(before.b);
      if ((ar * dr + ag * dg + ab * db) / denominator >= 0.35f) ++count;
    }
  }
  return count;
}

/// 夹具：列布局 + 内边距，滑杆被拉伸到内容宽度（焦点环不外溢画布）。
struct SliderFixture {
  Harness harness{};
  st::ui::Slider* slider{nullptr};

  explicit SliderFixture(float value) {
    harness.root.set_viewport(st::math::Size{260.0f, 140.0f});
    auto content = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
    content->style().padding = st::math::Insets::all(12.0f);
    slider = static_cast<st::ui::Slider*>(
        content->add_child(std::make_unique<st::ui::Slider>(value)));
    harness.root.set_content(std::move(content));
    harness.root.layout(true);
  }
};

}  // namespace

ST_TEST(ui_slider_click_drag_and_clamp) {
  SliderFixture fixture(0.25f);
  auto& root = fixture.harness.root;
  auto* slider = fixture.slider;
  ST_REQUIRE(slider != nullptr);
  ST_REQUIRE(!slider->bounds().is_empty());
  ST_CHECK_NEAR(slider->value(), 0.25f, 0.0001f);

  int changes = 0;
  float last = -1.0f;
  slider->on_change = [&](float value) {
    ++changes;
    last = value;
  };

  const Rect bounds = slider->bounds();
  const float mid_y = bounds.center().y;

  // ① 按下即跳到该位置（滑块中心映射）。
  const float center_x = bounds.center().x;
  ST_CHECK(dispatch_kind(root, st::ui::EventKind::MouseDown, Point{center_x, mid_y}));
  ST_CHECK_NEAR(slider->value(), value_at(*slider, center_x), 0.002f);
  ST_CHECK_NEAR(slider->value(), 0.5f, 0.02f);
  ST_CHECK(changes >= 1);
  ST_CHECK_NEAR(last, slider->value(), 0.0001f);

  // ② 按住拖动（MouseMove）继续跟随。
  const float drag_x = bounds.x + bounds.width * 0.8f;
  ST_CHECK(dispatch_kind(root, st::ui::EventKind::MouseMove, Point{drag_x, mid_y}));
  ST_CHECK_NEAR(slider->value(), value_at(*slider, drag_x), 0.002f);
  ST_CHECK(slider->value() > 0.6f);

  // ③ 越界夹取到 [0,1]。
  (void)dispatch_kind(root, st::ui::EventKind::MouseMove, Point{bounds.right() - 0.5f, mid_y});
  ST_CHECK_NEAR(slider->value(), 1.0f, 0.0001f);
  (void)dispatch_kind(root, st::ui::EventKind::MouseMove, Point{bounds.x + 0.25f, mid_y});
  ST_CHECK_NEAR(slider->value(), 0.0f, 0.0001f);
  // 指针移出控件后不再命中，取值保持（拖拽需指针留在控件内）。
  (void)dispatch_kind(root, st::ui::EventKind::MouseMove, Point{bounds.x - 40.0f, mid_y});
  ST_CHECK_NEAR(slider->value(), 0.0f, 0.0001f);

  // ④ 松开后移动不再改变取值（拖拽需按压）。
  (void)dispatch_kind(root, st::ui::EventKind::MouseUp, Point{bounds.x + 0.25f, mid_y});
  const float frozen = slider->value();
  (void)dispatch_kind(root, st::ui::EventKind::MouseMove, Point{drag_x, mid_y});
  ST_CHECK_NEAR(slider->value(), frozen, 0.0001f);
}

ST_TEST(ui_slider_keyboard_and_properties) {
  SliderFixture fixture(0.0f);
  auto& root = fixture.harness.root;
  auto* slider = fixture.slider;
  ST_REQUIRE(slider != nullptr);
  ST_CHECK_EQ(std::string(slider->type()), std::string("Slider"));
  ST_CHECK(slider->role() == st::ui::Role::Slider);

  root.set_focus(slider);
  ST_CHECK(slider->semantics_flags().focused);
  ST_CHECK(press_key(root, "ArrowRight"));
  ST_CHECK_NEAR(slider->value(), 0.05f, 0.001f);
  ST_CHECK(press_key(root, "ArrowRight"));
  ST_CHECK_NEAR(slider->value(), 0.10f, 0.001f);
  ST_CHECK(press_key(root, "ArrowLeft"));
  ST_CHECK_NEAR(slider->value(), 0.05f, 0.001f);
  ST_CHECK(press_key(root, "End"));
  ST_CHECK_NEAR(slider->value(), 1.0f, 0.0001f);
  ST_CHECK(press_key(root, "ArrowUp"));
  ST_CHECK_NEAR(slider->value(), 1.0f, 0.0001f);  // 端点饱和
  ST_CHECK(press_key(root, "Home"));
  ST_CHECK_NEAR(slider->value(), 0.0f, 0.0001f);

  // 属性面：写入即夹取到 [0,1]，非法值被拒。
  ST_CHECK(slider->set_property("value", "0.5"));
  ST_CHECK_NEAR(slider->value(), 0.5f, 0.0001f);
  ST_CHECK(slider->set_property("value", "2.5"));
  ST_CHECK_NEAR(slider->value(), 1.0f, 0.0001f);
  ST_CHECK(slider->set_property("value", "-3"));
  ST_CHECK_NEAR(slider->value(), 0.0f, 0.0001f);
  ST_CHECK(!slider->set_property("value", "abc"));
  ST_CHECK(slider->set_property("label", "音量"));
  ST_CHECK_EQ(slider->label(), std::string("音量"));
  ST_CHECK_EQ(slider->semantics_text(), std::string("音量"));

  ST_CHECK(slider->set_property("value", "0.5"));
  const auto property = slider->get_property("value");
  ST_REQUIRE(property.has_value());
  const auto parsed = st::parse_f64(*property);
  ST_REQUIRE(parsed.has_value());
  ST_CHECK_NEAR(static_cast<float>(*parsed), slider->value(), 0.0001f);
  ST_CHECK_EQ(slider->semantics_value(), std::string("0.50"));

  ST_CHECK(slider->invoke_action("increment", {}));
  ST_CHECK_NEAR(slider->value(), 0.55f, 0.001f);
  ST_CHECK(slider->invoke_action("decrement", {}));
  ST_CHECK_NEAR(slider->value(), 0.50f, 0.001f);
  ST_CHECK(slider->set_property("step", "0.25"));
  ST_CHECK_NEAR(slider->step(), 0.25f, 0.0001f);

  // 禁用态拦下所有交互。
  slider->set_enabled(false);
  const float frozen = slider->value();
  (void)dispatch_kind(root, st::ui::EventKind::MouseDown, slider->bounds().center());
  ST_CHECK_NEAR(slider->value(), frozen, 0.0001f);
  ST_CHECK(!press_key(root, "End"));
  ST_CHECK_NEAR(slider->value(), frozen, 0.0001f);
}

ST_TEST(ui_slider_focus_ring_and_visual_tree) {
  SliderFixture fixture(0.35f);
  auto& root = fixture.harness.root;
  auto* slider = fixture.slider;
  ST_REQUIRE(slider != nullptr);
  ST_REQUIRE(!slider->bounds().is_empty());

  root.layout(true);
  const Rect region = slider->bounds().inflate(3.0f);
  st::raster::Canvas plain(static_cast<int>(root.viewport().width),
                           static_cast<int>(root.viewport().height));
  plain.clear(root.theme().colors().bg);
  root.paint(plain);

  root.set_focus(slider);
  st::raster::Canvas focused(static_cast<int>(root.viewport().width),
                             static_cast<int>(root.viewport().height));
  focused.clear(root.theme().colors().bg);
  root.paint(focused);

  // 滑杆已选段本身为 primary，其抗锯齿过渡色与焦点环合成色接近，
  // 故此处只做正向断言（焦点态确实画出焦点环像素），不做「未聚焦为 0」的反向断言以免误报。
  ST_CHECK(focus_ring_pixels(focused, plain, region, root.theme()) >= 20);

  const st::ui::VisualNode visual = root.visual_tree();
  ST_REQUIRE(visual.children.size() == 1U);
  ST_REQUIRE(visual.children[0].children.size() == 1U);
  ST_CHECK(!visual.children[0].children[0].bounds.is_empty());
  ST_CHECK(visual.children[0].children[0].hit_target);

  const st::ui::SemanticsNode tree = root.semantics(0);
  ST_REQUIRE(!tree.children.empty());
  ST_CHECK_EQ(tree.children[0].children.size(), 1U);
  ST_CHECK(tree.children[0].children[0].role == st::ui::Role::Slider);
  ST_CHECK_EQ(tree.children[0].children[0].value, std::string("0.35"));
  ST_CHECK(tree.children[0].children[0].flags.focused);
}

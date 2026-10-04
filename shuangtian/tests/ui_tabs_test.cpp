/// 标签条与下拉单选单测：`Tabs`（切换/回调/键盘/指示条/属性面）+ `Select`（叠加层面板/选中/属性面）。
///
/// `Select` 与 `Tabs` 同属「选项 + 活动项」的单选模型（下拉 vs 平铺），故合并在本文件验证；
/// ui 层不依赖 text 层，本测试作为宿主把 `text::TextRenderer` 适配成 `ui::TextPort` 注入 `UiRoot`。

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

#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/test/test.hpp"
#include "st/text/font.hpp"
#include "st/text/text.hpp"
#include "st/ui/components/select.hpp"
#include "st/ui/components/tabs.hpp"
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
            float embolden = 0.0f) const override {
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

struct Harness {
  std::optional<st::text::FontStack> stack{};
  std::unique_ptr<FontPort> port{};
  st::ui::UiRoot root{};

  Harness() {
    const auto loaded = st::text::FontStack::system_default();
    if (!loaded.has_value()) {
      std::cout << "[ui_tabs_test] 未找到系统字体，改用 NullTextPort（文本宽度按 0 计）\n";
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

auto dispatch_kind(st::ui::UiRoot& root, st::ui::EventKind kind, Point point) -> bool {
  st::ui::Event event;
  event.kind = kind;
  event.position = point;
  return root.dispatch(event);
}

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
    if (dr * dr + dg * dg + db * db <= 100) return true;
  }
  return false;
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

/// 单帧内焦点环合成色像素数（未聚焦时的反向断言）。
auto ring_pixels_in(const st::raster::Canvas& canvas, Rect region, const st::ui::Theme& theme)
    -> int {
  const auto expected = ring_composites(theme);
  const int x0 = static_cast<int>(std::floor(region.x));
  const int y0 = static_cast<int>(std::floor(region.y));
  const int x1 = static_cast<int>(std::ceil(region.right()));
  const int y1 = static_cast<int>(std::ceil(region.bottom()));
  int count = 0;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      const Point point{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f};
      if (near_ring(canvas.pixel_at_point(point), expected)) ++count;
    }
  }
  return count;
}

[[nodiscard]] auto find_role(const st::ui::SemanticsNode& node, st::ui::Role role)
    -> const st::ui::SemanticsNode* {
  if (node.role == role) return &node;
  for (const auto& child : node.children) {
    if (const auto* hit = find_role(child, role); hit != nullptr) return hit;
  }
  return nullptr;
}

/// 标签条夹具：父面板显式宽度 > 标签总宽，标签条右侧留出空白区（空白点击不改变活动项）。
struct TabsFixture {
  Harness harness{};
  st::ui::Tabs* tabs{nullptr};

  explicit TabsFixture(std::vector<std::string> labels) {
    harness.root.set_viewport(st::math::Size{400.0f, 160.0f});
    auto content = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
    content->style().width = 360.0f;
    content->style().padding = st::math::Insets::all(12.0f);
    tabs = static_cast<st::ui::Tabs*>(content->add_child(std::make_unique<st::ui::Tabs>()));
    tabs->set_tabs(std::move(labels));
    harness.root.set_content(std::move(content));
    harness.root.layout(true);
  }
};

/// 下拉夹具：`overlay_host` 捕获面板（默认不挂载），`overlay_remove` 从根摘除。
struct SelectFixture {
  Harness harness{};
  st::ui::Select* select{nullptr};
  int opens{0};
  std::unique_ptr<st::ui::Element> pending{};
  std::vector<std::string> changes{};

  SelectFixture() {
    harness.root.set_viewport(st::math::Size{360.0f, 260.0f});
    auto content = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
    content->style().padding = st::math::Insets::all(12.0f);
    select = static_cast<st::ui::Select*>(content->add_child(std::make_unique<st::ui::Select>()));
    select->add_option("alpha", "Alpha");
    select->add_option("beta", "Beta");
    select->add_option("gamma", "Gamma");
    select->set_placeholder("请选择");
    select->overlay_host = [this](std::unique_ptr<st::ui::Element> panel) {
      ++opens;
      pending = std::move(panel);
    };
    select->overlay_remove = [this](st::ui::Element* panel) { harness.root.remove_overlay(panel); };
    select->on_change = [this](std::string_view value) { changes.emplace_back(value); };
    harness.root.set_content(std::move(content));
    harness.root.layout(true);
  }
};

}  // namespace

// ————————————————— Tabs —————————————————

ST_TEST(ui_tabs_switch_and_callback) {
  TabsFixture fixture({"概览", "指标", "日志"});
  auto& root = fixture.harness.root;
  auto* tabs = fixture.tabs;
  ST_REQUIRE(tabs != nullptr);
  ST_REQUIRE(!tabs->bounds().is_empty());
  ST_CHECK_EQ(std::string(tabs->type()), std::string("Tabs"));
  ST_CHECK(tabs->role() == st::ui::Role::Tab);
  ST_REQUIRE(tabs->tab_count() == 3U);
  ST_CHECK_EQ(tabs->active_index(), std::size_t{0});
  ST_CHECK_EQ(tabs->active_label(), std::string_view("概览"));

  std::vector<std::size_t> changes;
  tabs->on_change = [&](std::size_t index) { changes.push_back(index); };

  const Rect second = tabs->tab_rect(1);
  ST_REQUIRE(!second.is_empty());
  ST_CHECK(click_at(root, second.center()));
  ST_CHECK_EQ(tabs->active_index(), std::size_t{1});
  ST_REQUIRE(changes.size() == 1U);
  ST_CHECK_EQ(changes[0], std::size_t{1});
  ST_CHECK_EQ(tabs->active_label(), std::string_view("指标"));
  ST_CHECK_EQ(tabs->get_property("active").value_or(""), std::string("1"));

  // 重复点击当前项不重复通知。
  ST_CHECK(click_at(root, tabs->tab_rect(1).center()));
  ST_CHECK_EQ(changes.size(), 1U);

  // 点击标签条空白处（右侧留白）不改变活动项。
  const Point blank{tabs->bounds().right() - 4.0f, tabs->bounds().center().y};
  ST_CHECK(click_at(root, blank));
  ST_CHECK_EQ(tabs->active_index(), std::size_t{1});
  ST_CHECK_EQ(changes.size(), 1U);
}

ST_TEST(ui_tabs_keyboard_navigation) {
  TabsFixture fixture({"A", "B", "C"});
  auto& root = fixture.harness.root;
  auto* tabs = fixture.tabs;
  ST_REQUIRE(tabs != nullptr);
  int confirmations = 0;
  tabs->on_change = [&](std::size_t) { ++confirmations; };

  root.set_focus(tabs);
  ST_CHECK(tabs->semantics_flags().focused);
  ST_CHECK(press_key(root, "ArrowRight"));
  ST_CHECK_EQ(tabs->active_index(), std::size_t{1});
  ST_CHECK(press_key(root, "ArrowRight"));
  ST_CHECK_EQ(tabs->active_index(), std::size_t{2});
  ST_CHECK(press_key(root, "ArrowRight"));
  ST_CHECK_EQ(tabs->active_index(), std::size_t{0});  // 循环
  ST_CHECK(press_key(root, "ArrowLeft"));
  ST_CHECK_EQ(tabs->active_index(), std::size_t{2});  // 反向循环
  ST_CHECK(press_key(root, "Home"));
  ST_CHECK_EQ(tabs->active_index(), std::size_t{0});
  ST_CHECK(press_key(root, "End"));
  ST_CHECK_EQ(tabs->active_index(), std::size_t{2});
  ST_CHECK_EQ(confirmations, 6);

  // Enter/Space 重新确认当前项。
  ST_CHECK(press_key(root, "Enter"));
  ST_CHECK_EQ(confirmations, 7);
  ST_CHECK(press_key(root, " "));
  ST_CHECK_EQ(confirmations, 8);
  ST_CHECK_EQ(tabs->active_index(), std::size_t{2});
}

ST_TEST(ui_tabs_properties_and_metrics) {
  TabsFixture fixture({"A", "B", "C"});
  auto* tabs = fixture.tabs;
  ST_REQUIRE(tabs != nullptr);

  ST_CHECK(tabs->set_property("active", "2"));
  ST_CHECK_EQ(tabs->active_index(), std::size_t{2});
  ST_CHECK(tabs->set_property("active", "B"));  // 亦接受标签名
  ST_CHECK_EQ(tabs->active_index(), std::size_t{1});
  ST_CHECK(!tabs->set_property("active", "不存在"));
  ST_CHECK_EQ(tabs->get_property("label").value_or(""), std::string("B"));
  ST_CHECK_EQ(tabs->get_property("options").value_or(""), std::string("A|B|C"));

  ST_CHECK(tabs->set_property("options", "x|y|z|w"));
  ST_CHECK_EQ(tabs->tab_count(), std::size_t{4});
  ST_CHECK_EQ(tabs->get_property("options").value_or(""), std::string("x|y|z|w"));
  ST_CHECK_EQ(tabs->active_index(), std::size_t{1});  // 活动项保持合法

  ST_CHECK(tabs->invoke_action("next", {}));
  ST_CHECK_EQ(tabs->active_index(), std::size_t{2});
  ST_CHECK(tabs->invoke_action("last", {}));
  ST_CHECK_EQ(tabs->active_index(), std::size_t{3});
  ST_CHECK(tabs->invoke_action("select", "1"));
  ST_CHECK_EQ(tabs->active_index(), std::size_t{1});

  // 标签项几何：自左向右排布、等高、总宽为正。
  fixture.harness.root.layout(true);
  ST_CHECK(tabs->tabs_width() > 0.0f);
  for (std::size_t index = 0; index + 1 < tabs->tab_count(); ++index) {
    const Rect left = tabs->tab_rect(index);
    const Rect right = tabs->tab_rect(index + 1);
    ST_CHECK(!left.is_empty());
    ST_CHECK(left.right() <= right.x);
    ST_CHECK_NEAR(left.height, tabs->bounds().height, 0.001f);
  }
  ST_CHECK(tabs->tab_rect(tabs->tab_count()).is_empty());  // 越界为空
}

ST_TEST(ui_tabs_focus_ring_and_visual_tree) {
  TabsFixture fixture({"A", "B", "C"});
  auto& root = fixture.harness.root;
  auto* tabs = fixture.tabs;
  ST_REQUIRE(tabs != nullptr);
  root.layout(true);

  const int width = static_cast<int>(root.viewport().width);
  const int height = static_cast<int>(root.viewport().height);
  st::raster::Canvas plain(width, height);
  plain.clear(root.theme().colors().bg);
  root.paint(plain);

  root.set_focus(tabs);
  st::raster::Canvas focused(width, height);
  focused.clear(root.theme().colors().bg);
  root.paint(focused);

  const Rect bounds = tabs->bounds();
  ST_CHECK(focus_ring_pixels(focused, plain, bounds.inflate(4.0f), root.theme()) >= 20);
  // 未聚焦时的反向断言只看环带（标签条上沿外侧）：指示条本体为 primary，
  // 其抗锯齿过渡色与焦点环合成色落在同一色域，整框比对会误报。
  const Rect top_band{bounds.x - 3.0f, bounds.y - 3.0f, bounds.width + 6.0f, 3.0f};
  ST_CHECK_EQ(ring_pixels_in(plain, top_band, root.theme()), 0);
  ST_CHECK(focus_ring_pixels(focused, plain, top_band, root.theme()) >= 20);

  const st::ui::VisualNode visual = root.visual_tree();
  ST_REQUIRE(visual.children.size() == 1U);
  ST_REQUIRE(visual.children[0].children.size() == 1U);
  ST_CHECK(!visual.children[0].children[0].bounds.is_empty());
  ST_CHECK(visual.children[0].children[0].hit_target);

  const st::ui::SemanticsNode tree = root.semantics(0);
  const auto* tab_node = find_role(tree, st::ui::Role::Tab);
  ST_REQUIRE(tab_node != nullptr);
  ST_CHECK(tab_node->flags.selected);
  ST_CHECK(tab_node->flags.focused);
  ST_CHECK_EQ(tab_node->value, std::string("0"));
  ST_CHECK_EQ(tab_node->text, std::string("A"));
  ST_CHECK(!tab_node->bounds.is_empty());
}

// ————————————————— Select —————————————————

ST_TEST(ui_select_opens_via_overlay_host) {
  SelectFixture fixture;
  auto& root = fixture.harness.root;
  auto* select = fixture.select;
  ST_REQUIRE(select != nullptr);
  ST_REQUIRE(!select->bounds().is_empty());
  ST_CHECK_EQ(std::string(select->type()), std::string("Select"));
  ST_CHECK(select->role() == st::ui::Role::Select);
  ST_CHECK_EQ(select->option_count(), std::size_t{3});
  ST_CHECK(!select->open());
  ST_CHECK(!select->selected_index().has_value());
  ST_CHECK_EQ(fixture.opens, 0);

  // ① 展开：面板经 overlay_host 交给调用方（lambda 捕获计数）。
  ST_CHECK(click_at(root, select->bounds().center()));
  ST_CHECK_EQ(fixture.opens, 1);
  ST_CHECK(select->open());
  ST_REQUIRE(fixture.pending != nullptr);
  auto* panel = static_cast<st::ui::SelectPanel*>(fixture.pending.get());
  ST_CHECK_EQ(panel->option_count(), std::size_t{3});
  ST_CHECK(panel->role() == st::ui::Role::List);

  // ② 调用方把面板挂到 UiRoot：面板按锚点贴控件下沿（而非叠加层默认槽位）。
  root.add_overlay(std::move(fixture.pending));
  root.layout(true);
  auto* overlay = root.overlay_at(0);
  ST_REQUIRE(overlay != nullptr);
  ST_CHECK_EQ(root.overlay_count(), std::size_t{1});
  ST_CHECK_NEAR(overlay->bounds().x, select->bounds().x, 0.5);
  ST_CHECK_NEAR(overlay->bounds().y,
                select->bounds().bottom() + root.theme().metrics().space_xs, 0.5);
  ST_CHECK_NEAR(overlay->bounds().width, select->bounds().width, 0.5);

  // ③ 选中第 3 行：值变更 + on_change + 收起 + 面板立即隐藏。
  const Rect row = panel->option_rect(2);
  ST_REQUIRE(!row.is_empty());
  ST_CHECK(click_at(root, row.center()));
  ST_REQUIRE(select->selected_index().has_value());
  ST_CHECK_EQ(*select->selected_index(), std::size_t{2});
  ST_CHECK_EQ(select->selected_value(), std::string_view("gamma"));
  ST_CHECK_EQ(select->selected_label(), std::string_view("Gamma"));
  ST_REQUIRE(fixture.changes.size() == 1U);
  ST_CHECK_EQ(fixture.changes[0], std::string("gamma"));
  ST_CHECK(!select->open());
  ST_CHECK(!overlay->visible());

  // ④ 摘除在布局期执行（不在事件分发内析构正在处理事件的面板）。
  root.layout(true);
  ST_CHECK_EQ(root.overlay_count(), std::size_t{0});
  ST_CHECK_EQ(select->get_property("value").value_or(""), std::string("gamma"));
  ST_CHECK_EQ(select->get_property("active").value_or(""), std::string("2"));

  // 再次展开仍可用（延迟摘除不会卡住面板创建）。
  ST_CHECK(click_at(root, select->bounds().center()));
  ST_CHECK_EQ(fixture.opens, 2);
  ST_CHECK(select->open());
  ST_CHECK(fixture.pending != nullptr);
}

ST_TEST(ui_select_keyboard_and_close) {
  SelectFixture fixture;
  auto& root = fixture.harness.root;
  auto* select = fixture.select;
  ST_REQUIRE(select != nullptr);

  root.set_focus(select);
  ST_CHECK(press_key(root, "Enter"));  // 键盘展开
  ST_CHECK(select->open());
  ST_CHECK_EQ(fixture.opens, 1);
  ST_REQUIRE(fixture.pending != nullptr);
  root.add_overlay(std::move(fixture.pending));
  root.layout(true);
  ST_CHECK_EQ(root.overlay_count(), std::size_t{1});

  ST_CHECK(press_key(root, "Escape"));  // 收起
  ST_CHECK(!select->open());
  root.layout(true);
  ST_CHECK_EQ(root.overlay_count(), std::size_t{0});

  // 方向键改选中项（未展开时同样可用），空选中态用占位符。
  ST_CHECK_EQ(select->semantics_text(), std::string("请选择"));
  ST_CHECK(press_key(root, "ArrowDown"));
  ST_REQUIRE(select->selected_index().has_value());
  ST_CHECK_EQ(*select->selected_index(), std::size_t{0});
  ST_CHECK(press_key(root, "ArrowDown"));
  ST_CHECK_EQ(*select->selected_index(), std::size_t{1});
  ST_CHECK(press_key(root, "ArrowUp"));
  ST_CHECK_EQ(*select->selected_index(), std::size_t{0});
  ST_CHECK(press_key(root, "End"));
  ST_CHECK_EQ(*select->selected_index(), std::size_t{2});
  ST_CHECK(press_key(root, "Home"));
  ST_CHECK_EQ(*select->selected_index(), std::size_t{0});
  // 变更次数：ArrowDown(空选→0)/ArrowDown(0→1)/ArrowUp(1→0)/End(0→2)/Home(2→0)
  ST_CHECK_EQ(fixture.changes.size(), std::size_t{5});
}

ST_TEST(ui_select_properties_and_option_geometry) {
  SelectFixture fixture;
  auto& root = fixture.harness.root;
  auto* select = fixture.select;
  ST_REQUIRE(select != nullptr);

  ST_CHECK(select->set_property("value", "beta"));
  ST_CHECK_EQ(select->selected_value(), std::string_view("beta"));
  ST_CHECK_EQ(select->get_property("label").value_or(""), std::string("Beta"));
  ST_CHECK_EQ(select->get_property("active").value_or(""), std::string("1"));
  ST_CHECK(!select->set_property("value", "不存在"));
  ST_CHECK_EQ(select->get_property("options").value_or(""), std::string("Alpha|Beta|Gamma"));

  ST_CHECK(select->set_property("active", "none"));  // 清空选中
  ST_CHECK(!select->selected_index().has_value());
  ST_CHECK_EQ(select->get_property("value").value_or("x"), std::string(""));
  ST_CHECK_EQ(select->get_property("active").value_or(""), std::string("none"));

  ST_CHECK(select->set_property("options", "one|two|three"));
  ST_CHECK_EQ(select->option_count(), std::size_t{3});
  ST_CHECK(select->set_property("active", "two"));
  ST_CHECK_EQ(select->get_property("value").value_or(""), std::string("two"));

  // 选项行几何：行高 32、自上而下等距。
  ST_CHECK(select->set_property("open", "true"));
  ST_CHECK(select->open());
  ST_REQUIRE(fixture.pending != nullptr);
  auto* panel = static_cast<st::ui::SelectPanel*>(fixture.pending.get());
  root.add_overlay(std::move(fixture.pending));
  root.layout(true);
  const Rect first = panel->option_rect(0);
  const Rect second = panel->option_rect(1);
  ST_REQUIRE(!first.is_empty());
  ST_CHECK_NEAR(first.height, 32.0f, 0.001f);
  ST_CHECK_NEAR(second.y - first.y, 32.0f, 0.001f);
  ST_CHECK(panel->option_rect(panel->option_count()).is_empty());
  ST_CHECK(panel->bounds().contains(first.center()));

  ST_CHECK(select->invoke_action("close", {}));
  ST_CHECK(!select->open());
  root.layout(true);
  ST_CHECK_EQ(root.overlay_count(), std::size_t{0});
}

ST_TEST(ui_select_focus_ring_pixels) {
  SelectFixture fixture;
  auto& root = fixture.harness.root;
  auto* select = fixture.select;
  ST_REQUIRE(select != nullptr);
  root.layout(true);

  const int width = static_cast<int>(root.viewport().width);
  const int height = static_cast<int>(root.viewport().height);
  st::raster::Canvas plain(width, height);
  plain.clear(root.theme().colors().bg);
  root.paint(plain);

  root.set_focus(select);
  st::raster::Canvas focused(width, height);
  focused.clear(root.theme().colors().bg);
  root.paint(focused);

  const Rect region = select->bounds().inflate(4.0f);
  ST_CHECK(focus_ring_pixels(focused, plain, region, root.theme()) >= 20);
  // 控件盒上沿外侧一圈只有焦点环（未展开时控件本身不含 primary 填充）。
  const Rect top_band{region.x, region.y, region.width, 4.0f};
  ST_CHECK_EQ(ring_pixels_in(plain, top_band, root.theme()), 0);
  ST_CHECK(focus_ring_pixels(focused, plain, top_band, root.theme()) >= 20);
}

// ————————————————— Tabs 升级（关闭×/修改点/溢出滚动/业务 key）—————————————————

namespace {

/// 结构化标签夹具：父面板显式宽度，标签条可溢出（`narrow` 时 120 宽装不下多个标签）。
struct MetaTabsFixture {
  Harness harness{};
  st::ui::Tabs* tabs{nullptr};

  explicit MetaTabsFixture(bool narrow = false) {
    harness.root.set_viewport(st::math::Size{400.0f, 160.0f});
    auto content = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
    content->style().padding = st::math::Insets::all(12.0f);
    tabs = static_cast<st::ui::Tabs*>(content->add_child(std::make_unique<st::ui::Tabs>()));
    // 窄形态给标签条设显式宽 + 退出交叉轴拉伸（Column 面板默认 Stretch 会把子项拉满面板宽，
    // 显式宽被覆盖——溢出场景就造不出来了）。
    tabs->style().width = narrow ? 120.0f : 336.0f;
    tabs->style().align_self = narrow ? st::ui::Align::Start : st::ui::Align::Stretch;
    harness.root.set_content(std::move(content));
  }

  void sync(std::vector<st::ui::Tabs::Tab> items) {
    tabs->sync_tabs(items);
    harness.root.layout(true);
  }
};

[[nodiscard]] auto make_tabs(std::vector<std::pair<std::string, std::string>> key_labels)
    -> std::vector<st::ui::Tabs::Tab> {
  std::vector<st::ui::Tabs::Tab> out;
  out.reserve(key_labels.size());
  for (auto& [key, label] : key_labels) {
    out.push_back(st::ui::Tabs::Tab{.key = key, .label = label});
  }
  return out;
}

}  // namespace

ST_TEST(ui_tabs_sync_keeps_active_by_key) {
  MetaTabsFixture fixture;
  auto* tabs = fixture.tabs;
  ST_REQUIRE(tabs != nullptr);

  fixture.sync(make_tabs({{"a.md", "Alpha"}, {"b.md", "Beta"}, {"c.md", "Gamma"}}));
  ST_CHECK_EQ(tabs->tab_count(), std::size_t{3});
  tabs->set_active(1, false);
  ST_CHECK_EQ(tabs->active_key(), std::string_view("b.md"));

  // 数据刷新（顺序打乱 + 文案更新）：次序随新数据，活动态跟 key 走（不跟索引）。
  std::vector<st::ui::Tabs::Tab> next;
  next.push_back(st::ui::Tabs::Tab{.key = "c.md", .label = "Gamma*"});
  next.push_back(st::ui::Tabs::Tab{.key = "a.md", .label = "Alpha*"});
  next.push_back(st::ui::Tabs::Tab{.key = "b.md", .label = "Beta*"});
  fixture.sync(next);
  ST_CHECK_EQ(tabs->tab_count(), std::size_t{3});
  ST_CHECK_EQ(tabs->active_key(), std::string_view("b.md"));
  ST_CHECK_EQ(tabs->active_label(), std::string_view("Beta*"));
  // 次序以新数据为准：c a b
  ST_CHECK_EQ(*tabs->index_of_key("c.md"), std::size_t{0});
  ST_CHECK_EQ(*tabs->index_of_key("a.md"), std::size_t{1});
  ST_CHECK_EQ(*tabs->index_of_key("b.md"), std::size_t{2});
  ST_CHECK_EQ(tabs->active_index(), std::size_t{2});  // 活动态跟 key 到新位置
}

ST_TEST(ui_tabs_sync_add_remove_follows_active_key) {
  MetaTabsFixture fixture;
  auto* tabs = fixture.tabs;
  ST_REQUIRE(tabs != nullptr);

  fixture.sync(make_tabs({{"a", "A"}, {"b", "B"}, {"c", "C"}}));
  tabs->set_active(2, false);  // 活动项是 c

  // 移除活动项 c：活动态落到夹取位置，不派发回调。
  std::size_t notified = 0;
  tabs->on_change = [&notified](std::size_t) { ++notified; };
  fixture.sync(make_tabs({{"a", "A"}, {"b", "B"}}));
  ST_CHECK_EQ(tabs->tab_count(), std::size_t{2});
  ST_CHECK(tabs->active_index() < std::size_t{2});
  ST_CHECK_EQ(notified, std::size_t{0});

  // 新增 key 按数据次序就位：b a d。
  fixture.sync(make_tabs({{"b", "B"}, {"a", "A"}, {"d", "D"}}));
  ST_CHECK_EQ(tabs->tab_count(), std::size_t{3});
  ST_CHECK_EQ(*tabs->index_of_key("b"), std::size_t{0});
  ST_CHECK_EQ(*tabs->index_of_key("a"), std::size_t{1});
  ST_CHECK_EQ(*tabs->index_of_key("d"), std::size_t{2});
}

ST_TEST(ui_tabs_meta_flags_and_close_geometry) {
  MetaTabsFixture fixture;
  auto* tabs = fixture.tabs;
  ST_REQUIRE(tabs != nullptr);

  fixture.sync(make_tabs({{"a", "Alpha"}, {"b", "Beta"}}));
  tabs->set_tab_meta(0, true, true);
  ST_CHECK(tabs->tab_modified(0));
  ST_CHECK(tabs->tab_closable(0));
  ST_CHECK(!tabs->tab_modified(1));
  ST_CHECK(!tabs->tab_closable(1));
  tabs->set_tab_meta(9, true, true);  // 越界忽略
  ST_CHECK(!tabs->tab_modified(9));

  fixture.harness.root.layout(true);
  // modified/closable 占宽：带 meta 的标签比同文案的裸标签宽。
  ST_CHECK(tabs->tabs_width() > 0.0f);
  const Rect with_meta = tabs->tab_rect(0);
  ST_REQUIRE(!with_meta.is_empty());
  ST_CHECK(with_meta.width > 36.0f);

  // × 命中区：closable 项有，非 closable 项为空。
  const Rect close = tabs->close_rect(0);
  ST_REQUIRE(!close.is_empty());
  ST_CHECK(close.right() <= with_meta.right() + 0.01f);
  ST_CHECK(tabs->close_rect(1).is_empty());
}

ST_TEST(ui_tabs_close_click_fires_callback_not_switch) {
  MetaTabsFixture fixture;
  auto* tabs = fixture.tabs;
  auto& root = fixture.harness.root;
  ST_REQUIRE(tabs != nullptr);

  fixture.sync(make_tabs({{"a", "Alpha"}, {"b", "Beta"}}));
  tabs->set_tab_meta(0, false, true);
  root.layout(true);

  std::vector<std::size_t> closed;
  std::size_t changed = 0;
  tabs->on_close = [&closed](std::size_t index) { closed.push_back(index); };
  tabs->on_change = [&changed](std::size_t) { ++changed; };

  // 点 ×：触发 on_close，不切换活动项。
  const Rect close = tabs->close_rect(0);
  ST_REQUIRE(!close.is_empty());
  ST_CHECK(click_at(root, close.center()));
  ST_REQUIRE(closed.size() == 1U);
  ST_CHECK_EQ(closed[0], std::size_t{0});
  ST_CHECK_EQ(tabs->active_index(), std::size_t{0});
  ST_CHECK_EQ(changed, std::size_t{0});
  // 标签仍在（组件不删除，由调用方决定）。
  ST_CHECK_EQ(tabs->tab_count(), std::size_t{2});
}

ST_TEST(ui_tabs_overflow_scroll_clamps) {
  MetaTabsFixture fixture(/*narrow=*/true);
  auto* tabs = fixture.tabs;
  auto& root = fixture.harness.root;
  ST_REQUIRE(tabs != nullptr);

  // 窄容器装 5 个标签：必然溢出。
  fixture.sync(make_tabs({{"1", "One"}, {"2", "Two"}, {"3", "Three"}, {"4", "Four"}, {"5", "Five"}}));
  ST_CHECK(tabs->max_scroll() > 0.0f);
  ST_CHECK_EQ(tabs->scroll_offset(), 0.0f);

  // 写入负值：夹取到 0。
  tabs->set_scroll_offset(-50.0f);
  ST_CHECK_EQ(tabs->scroll_offset(), 0.0f);

  // tab_rect 反映滚动偏移：滚到最右后首标签左移（位移量 = 滚动偏移）。
  const Rect first_at_zero = tabs->tab_rect(0);
  ST_CHECK_NEAR(first_at_zero.x, tabs->bounds().x, 0.01f);
  tabs->set_scroll_offset(1e6f);  // 超大值 → 夹到 max
  ST_CHECK_NEAR(tabs->scroll_offset(), tabs->max_scroll(), 0.001f);
  const Rect first_at_max = tabs->tab_rect(0);
  ST_CHECK(first_at_max.x < first_at_zero.x);
  ST_CHECK_NEAR(first_at_zero.x - first_at_max.x, tabs->scroll_offset(), 0.01f);

  // 滚到最右后把活动项设回 0：自动滚回可见（active 项尽量可见）。
  tabs->set_active(0, false);
  ST_CHECK(tabs->tab_rect(0).x >= tabs->bounds().x - 0.01f);
  (void)root;
}

ST_TEST(ui_tabs_wheel_scrolls_horizontally) {
  MetaTabsFixture fixture(/*narrow=*/true);
  auto* tabs = fixture.tabs;
  auto& root = fixture.harness.root;
  ST_REQUIRE(tabs != nullptr);

  fixture.sync(make_tabs({{"1", "One"}, {"2", "Two"}, {"3", "Three"}, {"4", "Four"}, {"5", "Five"}}));
  root.set_focus(tabs);
  ST_REQUIRE(tabs->max_scroll() > 0.0f);

  // 滚轮向下（delta 负）= 向右滚动（看后面的标签）。
  st::ui::Event wheel;
  wheel.kind = st::ui::EventKind::Wheel;
  wheel.position = tabs->bounds().center();
  wheel.wheel_delta = -1.0f;
  ST_CHECK(root.dispatch(wheel));
  ST_CHECK(tabs->scroll_offset() > 0.0f);

  // 滚轮向上（delta 正）= 向左回滚；回到顶后夹在 0。
  wheel.wheel_delta = 1.0f;
  for (int i = 0; i < 10; ++i) (void)root.dispatch(wheel);
  ST_CHECK_EQ(tabs->scroll_offset(), 0.0f);

  // 属性面：scroll 可读写。
  ST_CHECK(tabs->set_property("scroll", "12.5"));
  ST_CHECK_NEAR(tabs->scroll_offset(), 12.5f, 0.001f);
  const auto value = tabs->get_property("scroll").value_or("0");
  ST_CHECK_NEAR(std::stof(value), 12.5f, 0.01f);
}

ST_TEST(ui_tabs_overflow_arrow_click_scrolls) {
  MetaTabsFixture fixture(/*narrow=*/true);
  auto* tabs = fixture.tabs;
  auto& root = fixture.harness.root;
  ST_REQUIRE(tabs != nullptr);

  fixture.sync(make_tabs({{"1", "One"}, {"2", "Two"}, {"3", "Three"}, {"4", "Four"}, {"5", "Five"}}));
  ST_REQUIRE(tabs->max_scroll() > 0.0f);

  // 箭头命中区：溢出时两侧都有。
  const Rect right_arrow = tabs->overflow_arrow_rect(/*right=*/true);
  const Rect left_arrow = tabs->overflow_arrow_rect(/*right=*/false);
  ST_REQUIRE(!right_arrow.is_empty());
  ST_REQUIRE(!left_arrow.is_empty());

  // 点右箭头：向右滚动。
  const float before = tabs->scroll_offset();
  ST_CHECK(click_at(root, right_arrow.center()));
  ST_CHECK(tabs->scroll_offset() > before);

  // 点左箭头：向左回滚（夹到 0）。
  ST_CHECK(click_at(root, left_arrow.center()));
  ST_CHECK_EQ(tabs->scroll_offset(), 0.0f);

  // 箭头点击不改变活动项（不是标签命中）。
  ST_CHECK_EQ(tabs->active_index(), std::size_t{0});

  // 不溢出时箭头区为空。
  fixture.sync(make_tabs({{"1", "One"}}));
  ST_CHECK(tabs->max_scroll() <= 0.0f);
  ST_CHECK(tabs->overflow_arrow_rect(true).is_empty());
  ST_CHECK(tabs->overflow_arrow_rect(false).is_empty());
}

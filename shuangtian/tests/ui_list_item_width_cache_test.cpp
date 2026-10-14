/// `ListItem` 文本宽度缓存的**一致性面**护栏。
///
/// ## 这组用例守的是什么
///
/// `List` 不覆写 `measure`，走 `Element` 基类逐子元素递归——于是 N 项列表每轮 measure
/// 要调 `TextPort::measure_width` **2N 次**（label + subtitle）。而文本层的宽度缓存只有
/// 4096 条且满了**整表清空**：2048 项时正好 4096 次调用"自己把自己的缓存冲掉"，
/// 命中率≈0 → **断崖**。实测（`tools/ui_list_items_bench.cpp`）：
///
/// | 项数 | 改前 measure | 改后 |
/// |---|---|---|
/// | 2000 | 0.192 ms | 0.028 ms |
/// | **2100** | **10.923 ms** | **0.029 ms** |
/// | 5000 | 25.927 ms | 0.081 ms |
///
/// 修法是把文本宽度缓存在**项自己身上**（文案没变就不重量）。
///
/// ## 为什么断言"调用次数"而不是耗时
///
/// 同 `ui_tree_label_width_cache_test.cpp`：耗时机器相关；而缓存的真风险是
/// **"改了文案却读回旧宽度"**——那是正确性问题，表现为"列表项宽度不对"，
/// 截图里也难一眼看出。数 `measure_width` 的调用次数可以同时证明
/// "缓存生效"与"失效正确"。

#include "st/test/test.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "st/ui/components/list.hpp"
#include "st/ui/theme.hpp"

#include "tests/support/text_port_fixtures.hpp"

namespace {

using st::test::ProportionalTextPort;
using st::ui::Constraints;
using st::ui::RenderContext;
using st::ui::Theme;

/// 包装比例端口，**计数 `measure_width` 调用次数**（其余转发）。
///
/// 内层选 `ProportionalTextPort`（宽度 = `size × 0.55 × 字符数`，**依赖 size**），
/// 这样"字号变了要重新量"才测得住（定宽桩的宽度与 size 无关，测不出——见
/// `ui_tree_label_width_cache_test.cpp` 的注释里记的那个实测踩坑）。
class CountingTextPort final : public st::ui::TextPort {
 public:
  ProportionalTextPort inner{};
  mutable std::size_t width_calls{0};

  [[nodiscard]] auto measure(std::string_view utf8, float size) const -> st::math::Size override {
    return inner.measure(utf8, size);
  }
  [[nodiscard]] auto measure_width(
      std::string_view utf8, float size,
      st::text::FontRole role = st::text::FontRole::Proportional) const -> float override {
    ++width_calls;
    return inner.measure_width(utf8, size, role);
  }
  [[nodiscard]] auto line_height(float size) const -> float override {
    return inner.line_height(size);
  }
  void draw(st::raster::Surface& canvas, std::string_view utf8, st::math::Point origin, float size,
            st::math::Color color, st::text::FontRole role = st::text::FontRole::Proportional,
            float embolden = 0.0f, bool bold = false) const override {
    inner.draw(canvas, utf8, origin, size, color, role, embolden, bold);
  }
  [[nodiscard]] auto ellipsize(std::string_view utf8, float size, float max_width) const
      -> std::string override {
    return inner.ellipsize(utf8, size, max_width);
  }
  [[nodiscard]] auto wrap(std::string_view utf8, float size, float max_width) const
      -> std::vector<std::string_view> override {
    return inner.wrap(utf8, size, max_width);
  }
  [[nodiscard]] auto wrap_limited(std::string_view utf8, float size, float max_width,
                                  std::size_t max_lines) const
      -> std::vector<std::string> override {
    return inner.wrap_limited(utf8, size, max_width, max_lines);
  }
};

struct Fixture {
  Theme theme{Theme::light()};
  CountingTextPort port{};

  [[nodiscard]] auto context() -> RenderContext { return RenderContext{theme, &port, 0.0}; }
};

/// 量一次并返回本次新增的 `measure_width` 调用数。
[[nodiscard]] auto measure_calls(Fixture& fixture, st::ui::List& list) -> std::size_t {
  const std::size_t before = fixture.port.width_calls;
  list.measure(fixture.context(), Constraints{});
  return fixture.port.width_calls - before;
}

}  // namespace

/// 数据没变时重复 measure **不再重复量**（缓存生效）。
ST_TEST(list_item_width_cache_avoids_repeat_measurement) {
  Fixture fixture;
  st::ui::List list;
  list.add_item("alpha", "sub a");
  list.add_item("beta", "");
  list.add_item("gamma", "sub c");

  // 首轮：3 项 → alpha 量 2 次（label+subtitle）、beta 量 1 次（无副行）、gamma 量 2 次 = 5
  ST_CHECK_EQ(measure_calls(fixture, list), std::size_t{5});
  // 次轮：全命中缓存
  ST_CHECK_EQ(measure_calls(fixture, list), std::size_t{0});
}

/// `set_label` 后只重量**那一段**文案（不是整项、更不是全表）。
ST_TEST(list_item_width_cache_invalidates_on_label_change) {
  Fixture fixture;
  st::ui::List list;
  auto* item = list.add_item("short", "sub");
  (void)measure_calls(fixture, list);

  item->set_label("a_much_longer_label");
  ST_CHECK_EQ(measure_calls(fixture, list), std::size_t{1});  // 只重量 label（副行仍命中）
}

/// `set_subtitle` 同理；清空副行则那一份宽度不再参与（也不该被量）。
ST_TEST(list_item_width_cache_invalidates_on_subtitle_change) {
  Fixture fixture;
  st::ui::List list;
  auto* item = list.add_item("label", "sub");
  (void)measure_calls(fixture, list);

  item->set_subtitle("a_much_longer_subtitle");
  ST_CHECK_EQ(measure_calls(fixture, list), std::size_t{1});  // 只重量 subtitle

  item->set_subtitle("");
  ST_CHECK_EQ(measure_calls(fixture, list), std::size_t{0});  // 空副行不参与度量
}

/// 宽度结果**不被缓存改变**：宽度确实随文案变长而变大。
ST_TEST(list_item_width_cache_preserves_result) {
  Fixture fixture;
  st::ui::List list;
  auto* item = list.add_item("a", "");
  list.measure(fixture.context(), Constraints{});
  const float narrow = item->measured_size().width;

  item->set_label("a_far_longer_label_than_before");
  list.measure(fixture.context(), Constraints{});
  ST_CHECK(item->measured_size().width > narrow);
}

/// **字号变了**要重算（宽度依赖字号；不失效会"字号调大、列表宽度没跟上"）。
ST_TEST(list_item_width_cache_follows_font_size) {
  Fixture fixture;
  st::ui::List list;
  auto* item = list.add_item("label", "");
  list.measure(fixture.context(), Constraints{});
  const float small = item->measured_size().width;

  fixture.theme.metrics().scale_fonts(2.0F);
  const std::size_t recomputed = measure_calls(fixture, list);
  ST_CHECK(recomputed > 0U);  // 字号变了必须重量
  ST_CHECK(item->measured_size().width > small);
}

/// 大项数下缓存仍生效——断崖修复的直接断言（调用数不随轮数涨，而非"变快"）。
ST_TEST(list_item_width_cache_holds_for_many_items) {
  Fixture fixture;
  st::ui::List list;
  constexpr std::size_t kItems = 3000;
  for (std::size_t index = 0; index < kItems; ++index) {
    list.add_item("file_" + std::to_string(index) + ".cpp", "modified");
  }
  ST_CHECK_EQ(measure_calls(fixture, list), kItems * 2U);  // 首轮：每项 2 次
  ST_CHECK_EQ(measure_calls(fixture, list), std::size_t{0});  // 次轮：零调用（改前这里是 6000）
}

/// `sync_items` 走"复用同 key 元素"路径时不改变宽度语义（文案更新照样重新量）。
ST_TEST(list_item_width_cache_survives_sync_items) {
  Fixture fixture;
  st::ui::List list;
  std::vector<st::ui::List::Entry> first;
  first.push_back(st::ui::List::Entry{.key = "k1", .label = "short", .subtitle = ""});
  list.sync_items(first);
  (void)measure_calls(fixture, list);
  const float narrow = list.item(0)->measured_size().width;

  std::vector<st::ui::List::Entry> second;
  second.push_back(
      st::ui::List::Entry{.key = "k1", .label = "a_much_longer_label", .subtitle = ""});
  list.sync_items(second);  // 同 key → 复用同一个元素，但文案变了
  ST_CHECK(measure_calls(fixture, list) > 0U);  // 必须重量
  ST_CHECK(list.item(0)->measured_size().width > narrow);
}

/// 容器把**自身 `max_width`** 纳入给子元素的可用宽（自适应内容的宽度链）。
///
/// ## 缺陷现场（小白气泡宽度不随内容走）
///
/// 气泡是 `Row` → `Card(max_width=520)` → 内层容器 → `MarkdownView`。
/// `Element::measure` 算「可用宽」时只取了父级给的 `constraints.max_width`，
/// **没把本元素自己声明的 `max_width` 算进去**：
///
/// ```text
/// Row     max_w=1150  avail_w=1150
/// Card     max_w=1e9  avail_w=1150   ← Row 主轴按 flex 约定给无界上界
/// 内层     max_w=1e9  avail_w=1e9    ← 无界一路传下去（真实宽已丢失）
/// ```
///
/// 后果：下游按 1106 排版（行少、内容矮），而 `arrange` 只拿到真实的 488
/// （行多、内容高）——同一份内容两个口径。表现为气泡宽度不随内容走
/// （实测：161 字符的推理块只有 80px 宽），长正文末行被裁，且多轮重排不收敛。
///
/// ## 修法
///
/// `available_width` 由 `fill_width(constraints, …)` 求出后，再用本元素的
/// `std::clamp(x, min_width, max_width)` 夹一刀——与全仓其它夹取同口径
///（显式声明优先于父级建议）。
///
/// ## 判据为什么放在**子元素**上
///
/// 被测的是「约束有没有传给子元素」，不是「容器自己多宽」——
/// 缺陷版里容器自己也会被夹住（`arrange` 时），但**已经传给子元素的宽是错的**。
/// 只在子元素身上量得出这个差错。
#include <memory>
#include <string>

#include "st/test/test.hpp"

#include "st/ui/dsl.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"

#include "tests/support/text_port_fixtures.hpp"

namespace {

using st::ui::Constraints;
using st::ui::Element;
using st::ui::FlexDirection;
using st::ui::RenderContext;
using st::ui::Theme;

constexpr float kContainerMaxWidth = 320.0f;

struct Fixture {
  Theme theme{Theme::light()};
  st::test::FixedAdvanceTextPort port{};
  [[nodiscard]] auto context() -> RenderContext { return RenderContext{theme, &port, 0.0}; }
};

/// 造一个“声明了 `max_width` 的列容器”，内含一个**不收缩**的探针元素。
///
/// ⚠ 探针必须是不自我收缩的普通元素：用 `MarkdownView`（`shrink_to_fit`）
/// 当探针时，它会把宽自己缩到内容宽，**掩盖“拿到的可用宽是无界”这件事**——
/// 回退修复后测试依然全绿（逆向验证抓出来的）。
[[nodiscard]] auto make_clamped_column(Element** out_child) -> std::unique_ptr<Element> {
  auto column = st::ui::dsl::make_element("Panel");
  ST_CHECK(column != nullptr);
  column->style().direction = FlexDirection::Column;
  column->style().max_width = kContainerMaxWidth;

  // `grow` 的子项**必须填满可用宽** ⇒ 它量出的宽就是它拿到的可用宽。
  // 这是唯一能直接观测“可用宽”的探针：内容自适应的元素（Text/MarkdownView）
  // 只按内容长，拿到的约束再大也看不出来（用它当探针时逆向验证全绿，白测）。
  auto child = st::ui::dsl::make_element("Panel");
  ST_CHECK(child != nullptr);
  if (child != nullptr) {
    child->style().grow = true;
  }
  *out_child = child.get();
  column->add_child(std::move(child));
  return column;
}

}  // namespace

ST_TEST(measure_passes_own_max_width_to_children) {
  // **核心回归**：容器声明 `max_width=320`，子元素量到的可用宽必须被它夹住。
  //
  // 父级给的是**全无界约束**（Row 主轴给子节点的真实粗口径）——
  // 缺陷版会把这个无界一路传给子元素（子元素于是按兜底宽 640 排版），
  // 修复版把它夹到 320。
  Fixture fixture;
  Element* child = nullptr;
  auto column = make_clamped_column(&child);

  Constraints parent;   // 全无界：max/available 都是 kUnbounded
  column->measure(fixture.context(), parent);
  // `grow` 子项的宽要到 arrange 才分配 ⇒ 必须排一次才量得到。
  column->arrange(fixture.context(), st::math::Rect{0.0f, 0.0f, 1200.0f, 400.0f});

  ST_CHECK(child != nullptr);
  if (child != nullptr) {
    // 判据：`grow` 子项**填满可用宽** ⇒ 它的实际宽就是它拿到的可用宽。
    // 缺陷版：容器把父级的 1200 原样传给子项 → 子项宽 1200（远大于 320）。
    ST_CHECK(child->bounds().width <= kContainerMaxWidth + 1.0f);
  }
}

ST_TEST(measure_max_width_reaches_grandchild) {
  // 中间再套一层**不设上限**的容器：`max_width` 必须穿透过去，
  // 不能在中间层被无界覆盖掉（那正是缺陷的传播方式）。
  Fixture fixture;
  Element* child = nullptr;
  auto column = make_clamped_column(&child);

  // 外层再加一个不设上限的列容器。
  auto outer = st::ui::dsl::make_element("Panel");
  ST_CHECK(outer != nullptr);
  outer->style().direction = FlexDirection::Column;
  outer->add_child(std::move(column));

  Constraints parent;
  outer->measure(fixture.context(), parent);
  // `grow` 子项的宽要到 arrange 才分配（与第一条同口径）。
  outer->arrange(fixture.context(), st::math::Rect{0.0f, 0.0f, 1200.0f, 400.0f});

  ST_CHECK(child != nullptr);
  if (child != nullptr) {
    // 中间层不设上限，但**内层声明了 320**——`grow` 子项填满的是内层可用宽，
    // 所以它必须 ≤320。缺陷版会把 1200 一路传下去 ⇒ 子项宽 1200。
    ST_CHECK(child->bounds().width <= kContainerMaxWidth + 1.0f);
  }
}

/// **绝对定位的子元素不参与父的测量**（`Style::absolute` 的文档承诺）。
///
/// ## 守的是什么
///
/// `Style::absolute` 的文档写着：
///
/// > 语义与 CSS `position:absolute` 一致：不参与父元素的 flex 分配（**不占位、不影向
/// > 兄弟元素的位置与父的 `measure`**）
///
/// 而 `Element::layout_children` 确实跳过了它们（`src/ui/element.cpp:986`），
/// **`Element::measure` 却没有跳**（同一文件 :880 的循环把所有可见子元素都量进去）。
/// 两者不一致的后果非常具体：
///
/// - 一个 `absolute` 的覆盖层（连线层、缩略图、拖拽浮标）会**把父容器量得更大**；
/// - 于是兄弟元素的位置、页面高度、滚动条范围全都跟着变——而**代码看起来是对的**
///   （样式里明明写了 `absolute`，`get` 回来也是 `true`）。
///
/// ## 判据为何是"父子两组对照"
///
/// 只量"有绝对子元素时父高 > 0"是不够的：任何子元素都会让它变大。必须**同尺寸的
/// 兄弟元素分别用 absolute / 普通流**两种摆法，然后断言两者的父尺寸**不同**——
/// 那才说明 absolute 真的没被算进测量。
///
/// ⚠ 这条用例必须走真实的 `Element::measure`（经 `UiRoot::layout` 或直接调
/// `measure`），不能自己算——自己算测的是"我有没有写这段逻辑"，不是框架行为。

#include "st/test/test.hpp"

#include <string>

#include "st/core/log.hpp"
#include "st/core/print.hpp"
#include "st/math/geometry.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::math::Size;
using st::ui::Constraints;
using st::ui::Element;
using st::ui::RenderContext;
using st::ui::Theme;

/// 造一个定尺寸的普通容器。
[[nodiscard]] auto make_sized(float width, float height) -> std::unique_ptr<Element> {
  auto element = std::make_unique<Element>();
  element->style().width = width;
  element->style().height = height;
  return element;
}

/// 一个纵向容器 + 一个 (200×40) 的**普通流**子元素。
[[nodiscard]] auto measure_with_flow_child() -> Size {
  const Theme theme = Theme::dark();
  const RenderContext context{theme, nullptr, 0.0};
  Element parent;
  parent.style().direction = st::ui::FlexDirection::Column;
  parent.add_child(make_sized(200.0f, 40.0f));
  Constraints constraints;
  parent.measure(context, constraints);
  return parent.measured_size();
}

/// 同一个容器 + 一个 (200×40) 的**绝对定位**子元素。
[[nodiscard]] auto measure_with_absolute_child() -> Size {
  const Theme theme = Theme::dark();
  const RenderContext context{theme, nullptr, 0.0};
  Element parent;
  parent.style().direction = st::ui::FlexDirection::Column;
  auto child = make_sized(200.0f, 40.0f);
  child->style().absolute = true;
  child->style().left = 0.0f;
  child->style().top = 0.0f;
  parent.add_child(std::move(child));
  Constraints constraints;
  parent.measure(context, constraints);
  return parent.measured_size();
}

/// `wrap` 那一支（另一条独立的测量路径，同样必须跳过绝对子元素）。
[[nodiscard]] auto measure_wrapped(bool absolute) -> Size {
  const Theme theme = Theme::dark();
  const RenderContext context{theme, nullptr, 0.0};
  Element parent;
  parent.style().direction = st::ui::FlexDirection::Row;
  parent.style().wrap = true;
  auto child = make_sized(600.0f, 40.0f);
  if (absolute) {
    child->style().absolute = true;
    child->style().left = 0.0f;
    child->style().top = 0.0f;
  }
  parent.add_child(std::move(child));
  Constraints constraints;
  constraints.max_width = 300.0f;   // 迫使换行分支生效
  constraints.available_width = 300.0f;
  parent.measure(context, constraints);
  return parent.measured_size();
}

}  // namespace

/// 主轴方向：绝对定位子元素不得把父的**宽或高**撑大。
ST_TEST(layout_absolute_child_does_not_affect_parent_measure) {
  const Size flow = measure_with_flow_child();
  const Size absolute = measure_with_absolute_child();
  st::log::out("[absolute] 普通流 子元素 → 父 {}×{}；绝对定位 子元素 → 父 {}×{}\n",
               static_cast<double>(flow.width), static_cast<double>(flow.height),
               static_cast<double>(absolute.width), static_cast<double>(absolute.height));

  // 普通流：父必须容纳子元素。
  ST_CHECK(flow.height >= 40.0f);
  ST_CHECK(flow.width >= 200.0f);
  // 绝对定位：父**不应**被这个子元素撑大（尺寸为 0）。
  ST_CHECK(absolute.height < 0.01f);
  ST_CHECK(absolute.width < 0.01f);
}

/// 换行分支（`wrap`）是另一条测量路径，同样必须跳过绝对子元素。
ST_TEST(layout_absolute_child_does_not_affect_wrapped_parent_measure) {
  const Size flow = measure_wrapped(/*absolute=*/false);
  const Size absolute = measure_wrapped(/*absolute=*/true);
  st::log::out("[absolute] wrap 普通流 → {}×{}；绝对定位 → {}×{}\n",
               static_cast<double>(flow.width), static_cast<double>(flow.height),
               static_cast<double>(absolute.width), static_cast<double>(absolute.height));
  ST_CHECK(flow.height >= 40.0f);
  ST_CHECK(absolute.height < 0.01f);
}

/// 绝对定位子元素**仍然要测量**（它自己的 `measure` 会被调到），否则
/// `place_absolute` 拿到的 `measured_size` 是 0×0 —— 而 `width/height` 为 `kAuto`
/// 的绝对定位元素（常见：内部件自己定尺寸）会缩成什么都没有。
ST_TEST(layout_absolute_child_is_still_measured_for_its_own_size) {
  const Theme theme = Theme::dark();
  const RenderContext context{theme, nullptr, 0.0};
  Element parent;
  auto child = make_sized(120.0f, 30.0f);
  child->style().absolute = true;
  child->style().left = 5.0f;
  child->style().top = 7.0f;
  Element* child_ptr = child.get();
  parent.add_child(std::move(child));

  Constraints constraints;
  parent.measure(context, constraints);
  // 自己的量出来的尺寸必须还在（否则 `place_absolute` 会摆出 0×0）。
  ST_CHECK(std::abs(child_ptr->measured_size().width - 120.0f) < 0.01f);
  ST_CHECK(std::abs(child_ptr->measured_size().height - 30.0f) < 0.01f);

  parent.arrange(context, st::math::Rect{0.0f, 0.0f, 400.0f, 300.0f});
  // 摆放位置按 left/top（父内容盒原点 + 偏移）。
  ST_CHECK(std::abs(child_ptr->bounds().x - 5.0f) < 0.01f);
  ST_CHECK(std::abs(child_ptr->bounds().y - 7.0f) < 0.01f);
  ST_CHECK(std::abs(child_ptr->bounds().width - 120.0f) < 0.01f);
}

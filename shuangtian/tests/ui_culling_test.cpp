/// 视口剔除：屏幕外的元素不该被绘制。
///
/// 为什么单独一个文件：这是**性能修复的正确性护栏**。剔除错了的表现是"某个东西不见了"，
/// 而这类问题在截图里很难发现（本应用跨运行的像素不确定，实测同一实例连拍两次仍有
/// 上千像素差异——所以逐像素 A/B 在本项目**不能当验证手段**）。因此这里直接断言
/// **哪些元素被绘制了**：`RenderContext::painted_elements` 计数是可复现的结构性证据。
///
/// 覆盖四类必须成立的情形（每一类都是剔除可能"剔错"的地方）：
/// ① 视口内 → 必须画；
/// ② 视口外（超出安全外扩）→ 不该画；
/// ③ 视口外但在**阴影/发光外扩范围内** → 仍要画（否则卡片边缘会被切）；
/// ④ 父容器在视口外、**子元素在视口内** → 子元素要画（框架允许子元素排到父之外）。

#include "st/test/test.hpp"

#include <memory>
#include <vector>

#include "st/raster/canvas.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::math::Rect;
using st::raster::Canvas;
using st::ui::Button;
using st::ui::Element;
using st::ui::Panel;
using st::ui::RenderContext;
using st::ui::Theme;

inline constexpr int kCanvasWidth = 320;
inline constexpr int kCanvasHeight = 200;

/// 画一次并返回**实际被绘制的元素数**。
struct PaintRun {
  Theme theme{Theme::light()};
  Canvas canvas{kCanvasWidth, kCanvasHeight};

  [[nodiscard]] auto count(Element& element) -> std::uint64_t {
    canvas.clear(st::math::Color{0x20, 0x20, 0x28, 0xFF});
    RenderContext context{theme, nullptr, 1.0};
    std::uint64_t painted = 0;
    context.painted_elements = &painted;
    element.paint(context, canvas);
    return painted;
  }
};

/// 造一个给定矩形的叶子元素。
[[nodiscard]] auto make_at(Rect rect) -> std::unique_ptr<Button> {
  auto button = std::make_unique<Button>("x");
  RenderContext context{Theme::light(), nullptr, 0.0};
  button->measure(context, st::ui::Constraints{});
  button->arrange(context, rect);
  return button;
}

}  // namespace

ST_TEST(cull_paints_element_inside_viewport) {
  PaintRun run;
  auto element = make_at(Rect{10.0f, 10.0f, 80.0f, 30.0f});
  ST_CHECK_EQ(static_cast<int>(run.count(*element)), 1);
}

ST_TEST(cull_skips_element_far_outside_viewport) {
  PaintRun run;
  // 远在视口之外（右侧）：不该画。
  auto element = make_at(Rect{4000.0f, 10.0f, 80.0f, 30.0f});
  ST_CHECK_EQ(static_cast<int>(run.count(*element)), 0);
  // 远在视口下方：同上（这正是"滚动到视野外的三维视图/长列表尾部"的情形）
  auto below = make_at(Rect{10.0f, 4000.0f, 80.0f, 30.0f});
  ST_CHECK_EQ(static_cast<int>(run.count(*below)), 0);
}

ST_TEST(cull_keeps_element_whose_shadow_reaches_viewport) {
  PaintRun run;
  // 紧贴右边缘**之外**：它自己不可见，但阴影/发光会溢进视口——
  // 按元素矩形剔除会把这块阴影切掉（表现为"卡片边缘少一截"），
  // 所以剔除必须按主题实际外扩量放宽。
  auto element = make_at(Rect{static_cast<float>(kCanvasWidth) + 20.0f, 40.0f, 80.0f, 30.0f});
  ST_CHECK_EQ(static_cast<int>(run.count(*element)), 1);
}

ST_TEST(cull_keeps_child_outside_parent_but_inside_viewport) {
  PaintRun run;
  // 父容器整个在视口外，子元素却在视口内（框架允许这种排布）：
  // 只看父矩形就会把可见的子元素一起剔掉。
  auto parent = std::make_unique<Panel>(st::ui::FlexDirection::Column);
  RenderContext context{Theme::light(), nullptr, 0.0};
  parent->measure(context, st::ui::Constraints{});
  parent->arrange(context, Rect{4000.0f, 0.0f, 10.0f, 10.0f});
  parent->add_child(make_at(Rect{20.0f, 50.0f, 80.0f, 30.0f}));
  // 父自己 + 可见的子 = 2
  ST_CHECK_EQ(static_cast<int>(run.count(*parent)), 2);
}

ST_TEST(cull_skips_whole_subtree_when_all_outside) {
  PaintRun run;
  auto parent = std::make_unique<Panel>(st::ui::FlexDirection::Column);
  RenderContext context{Theme::light(), nullptr, 0.0};
  parent->measure(context, st::ui::Constraints{});
  parent->arrange(context, Rect{4000.0f, 0.0f, 100.0f, 100.0f});
  parent->add_child(make_at(Rect{4010.0f, 10.0f, 80.0f, 30.0f}));
  // 父子都在视口外 → 一个都不画（这是省下开销的关键情形）
  ST_CHECK_EQ(static_cast<int>(run.count(*parent)), 0);
}

ST_TEST(cull_respects_narrowed_clip) {
  PaintRun run;
  auto element = make_at(Rect{200.0f, 100.0f, 80.0f, 30.0f});
  ST_CHECK_EQ(static_cast<int>(run.count(*element)), 1);   // 默认裁剪=整画布 → 在视口内
  // 把裁剪缩到左上角后，同一元素落在裁剪之外 → 不该画
  run.canvas.push_clip_rect(Rect{0.0f, 0.0f, 100.0f, 100.0f});
  ST_CHECK_EQ(static_cast<int>(run.count(*element)), 0);
  run.canvas.pop_clip();
}

ST_TEST(cull_never_skips_invisible_flag_handling) {
  PaintRun run;
  // 与 `visible=false` 不冲突：不可见元素本来就不画（既有行为不能被剔除逻辑破坏）
  auto element = make_at(Rect{10.0f, 10.0f, 80.0f, 30.0f});
  element->set_visible(false);
  ST_CHECK_EQ(static_cast<int>(run.count(*element)), 0);
  element->set_visible(true);
  ST_CHECK_EQ(static_cast<int>(run.count(*element)), 1);
}

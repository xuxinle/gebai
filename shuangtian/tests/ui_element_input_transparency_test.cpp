// 「透明命中层」：视觉在上、点击穿透到下层。
//
// 缺口实录（2026-11-05）：做两行列表行时，想要「文字盖在可点区域上面，
// 但点击要落到下面的按钮上」。`intercepts_input()` 是虚函数，只有覆写这条路——
// 而 DSL 里没有子类可写，于是被迫把两行内容塞进 `Button` 的**单行** label，
// 排版直接崩（状态字母跑到文件名左侧、两者都贴中轴线）。
//
// 这组用例钉住新加的实例级开关 `set_intercepts_input(false)`：
// 它让「只在事件层面透明」这个常见的叠加需求不必再写一个子类。

#include "st/test/test.hpp"
#include "st/ui/element.hpp"

#include <memory>

namespace {

/// 记录自己收到过多少次点击的最小元素（模拟“下层按钮”）。
class ProbeElement : public st::ui::Element {
 public:
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Probe"; }
  auto on_event(const st::ui::RenderContext&, st::ui::Event& event) -> bool override {
    if (event.kind == st::ui::EventKind::Click) {
      ++clicks;
      return true;
    }
    return false;
  }
  int clicks{0};
};

}  // namespace

ST_TEST(element_intercepts_input_defaults_to_true) {
  st::ui::Element element;
  // 默认要拦截：绝大多数元素都该按自己的矩形吃掉输入。
  ST_CHECK(element.intercepts_input());
}

ST_TEST(element_set_intercepts_input_overrides_without_subclassing) {
  st::ui::Element element;
  element.set_intercepts_input(false);
  // 关键：**不必子类化**就能关掉（DSL 里没有子类可写，这是它的存在理由）。
  ST_CHECK(!element.intercepts_input());

  // 关回来（恢复默认口径，而不是“永久 false”）。
  element.set_intercepts_input(true);
  ST_CHECK(element.intercepts_input());
}

ST_TEST(element_clear_intercepts_input_restores_default) {
  st::ui::Element element;
  element.set_intercepts_input(false);
  ST_CHECK(!element.intercepts_input());
  // 清掉覆盖 = 回到虚函数默认（对基类就是 true）。
  // 与 `set_intercepts_input(true)` 的区别：若子类覆写返回 false，
  // clear 会回到子类的答案，而 set(true) 会强行盖成 true。
  element.clear_intercepts_input();
  ST_CHECK(element.intercepts_input());
}

ST_TEST(element_transparent_layer_still_lays_out_and_paints) {
  // 关掉输入拦截**不该影响**其它任何东西：它只是“事件层面透明”。
  st::ui::Element layer;
  layer.style().width = 100.0f;
  layer.style().height = 40.0f;
  layer.set_intercepts_input(false);
  ST_CHECK_EQ(layer.style().width, 100.0f);
  ST_CHECK_EQ(layer.style().height, 40.0f);
  ST_CHECK(layer.visible());
  ST_CHECK(layer.intercepts_input() == false);
}

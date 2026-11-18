/// `Input::caret_rect()`：**还没画过也要给得出插入点位置**。
///
/// ## 缺陷现场（中文输入法选词弹窗没对准光标）
///
/// 输入法候选窗的位置来自 `UiRoot::publish_ime_caret()` → `focused_->caret_rect()`。
/// 而 `Input` 原本只把位置缓存进 `last_caret_rect_`，**且只在 `paint_content` 里写**：
///
/// - 聚焦发生时就要上报一次（`set_focused_element` 的收尾），
/// - 但那一刻新元素**还没画过**，缓存是空矩形；
/// - 上报出去 `(0,0,0,0)` ⇒ 系统把候选窗摆到屏幕上角。
///
/// 实测印证：`set_ime_caret` 收到了 `(38, 970, 2, 20)` 之类的正常值**之后**
/// 才会更新位置，但首次组合（`WM_IME_STARTCOMPOSITION`）常常跑在那之前。
///
/// ## 修法
///
/// `caret_rect()` 在缓存无效时按当前 `bounds_` 推出“首行行首”的位置。
/// 它拿不到 `RenderContext`（不知道字号/字宽），但**纵向一定对**——
/// 而候选窗贴不贴光标行主要由纵坐标决定；下一帧画过之后缓存接管，横向也准。
#include <string>

#include "st/test/test.hpp"

#include "st/ui/components/input_box.hpp"
#include "st/ui/theme.hpp"

#include "tests/support/text_port_fixtures.hpp"

namespace {

using st::math::Rect;
using st::ui::Constraints;
using st::ui::Input;
using st::ui::RenderContext;
using st::ui::Theme;

struct Fixture {
  Theme theme{Theme::light()};
  st::test::FixedAdvanceTextPort port{};
  [[nodiscard]] auto context() -> RenderContext { return RenderContext{theme, &port, 0.0}; }
};

[[nodiscard]] auto constraints() -> Constraints {
  Constraints value;
  value.max_width = 400.0f;
  value.max_height = 200.0f;
  value.available_width = 400.0f;
  value.available_height = 200.0f;
  return value;
}

}  // namespace

ST_TEST(input_caret_rect_non_empty_before_first_paint) {
  // **核心回归**：摆好位置但**一次都没画**时，`caret_rect()` 也必须给出有效矩形。
  //
  // 这正是“聚焦就上报插入点”那一刻的真实状态：`measure`/`arrange` 已经跑过
  //（bounds 有了），而 `paint_content` 还没来。
  Fixture fixture;
  Input input;
  input.measure(fixture.context(), constraints());
  input.arrange(fixture.context(), Rect{20.0f, 40.0f, 400.0f, 34.0f});

  const Rect caret = input.caret_rect();
  ST_CHECK(!caret.is_empty());
  ST_CHECK(caret.width > 0.0f);
  ST_CHECK(caret.height > 0.0f);
}

ST_TEST(input_caret_rect_sits_inside_input_bounds) {
  // 兜底值必须落在输入框内（否则候选窗仍然会偏到别处）。
  Fixture fixture;
  Input input;
  input.measure(fixture.context(), constraints());
  input.arrange(fixture.context(), Rect{20.0f, 40.0f, 400.0f, 34.0f});

  const Rect caret = input.caret_rect();
  const Rect bounds = input.bounds();
  ST_CHECK(caret.x >= bounds.x - 0.5f);
  ST_CHECK(caret.x <= bounds.right());
  ST_CHECK(caret.y >= bounds.y - 0.5f);
  ST_CHECK(caret.bottom() <= bounds.bottom() + 0.5f);
}

ST_TEST(input_caret_rect_follows_bounds) {
  // 换位置之后兜底值必须跟着走（不是常量）。
  Fixture fixture;
  Input first;
  first.measure(fixture.context(), constraints());
  first.arrange(fixture.context(), Rect{20.0f, 40.0f, 400.0f, 34.0f});
  const Rect a = first.caret_rect();

  Input second;
  second.measure(fixture.context(), constraints());
  second.arrange(fixture.context(), Rect{20.0f, 300.0f, 400.0f, 34.0f});
  const Rect b = second.caret_rect();

  ST_CHECK(b.y > a.y + 100.0f);
}

ST_TEST(input_caret_rect_empty_when_not_laid_out) {
  // 反例护栏：没有几何时不该凭空给位置（否则候选窗会贴到 (0,0) 附近）。
  Fixture fixture;
  Input input;
  ST_CHECK(input.caret_rect().is_empty());
}

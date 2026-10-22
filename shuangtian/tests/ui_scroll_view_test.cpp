/// `ScrollView` 的**写入**通道：属性面 `offset` 与动作面 `scroll_*`。
///
/// ## 为什么有这份用例
///
/// `examples/xiangqi` 实测：`offset` 一直只在 `get_property` 里，控制通道 `set`
/// 与脚本 `$('#x').set({offset: …})` 都**静默无效**——设了不动、也不报错。
/// 应用要“新内容到了自动滚到底”只能去合成滚轮事件，那不是应用该干的事。
///
/// ## 断言口径
///
/// 1. `set offset` 真的改变偏移，且夹取到 `[0, max_scroll]`
/// 2. 非法值**如实返回 false**（不是静默当 0）
/// 3. 动作面 `scroll_to` / `scroll_by` / `scroll_to_end` / `scroll_to_top` 生效
/// 4. `scroll_to_end` 的**两帧语义**：内容刚变长时第一帧受旧上限夹取，
///    下一帧才到真底部（这是既有契约，见 scroll.hpp 的注释）

#include "st/test/test.hpp"

#include <memory>
#include <string>

#include "st/raster/paint.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/scroll.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::ui::Panel;
using st::ui::ScrollView;
using st::ui::UiRoot;

/// 造一个「视口 200 高、内容 1000 高」的滚动容器：能滚，且量明确。
struct ScrollFixture {
  UiRoot root{};
  ScrollView* view{nullptr};

  ScrollFixture() {
    root.set_viewport(st::math::Size{300.0f, 200.0f});
    root.set_theme(st::ui::Theme::light());
    auto page = std::make_unique<Panel>(st::ui::FlexDirection::Column);
    auto scroller = std::make_unique<ScrollView>();
    scroller->set_id("scroller");
    // ⚠ **必须 grow**：不 grow 时列布局按内容高给它排（实测 view_h=1000 = 内容高），
    // 于是 `max_scroll = content - view = 0`——容器永远“刚好装得下”，滚不起来。
    // 真实应用里 ScrollView 几乎总是要占满剩余空间，这就是那个语义。
    scroller->style().grow = true;
    view = scroller.get();
    // 内容：一个 1000 高的固定面板（比视口高 → max_scroll > 0）
    auto content = std::make_unique<Panel>();
    content->set_id("content");
    content->style().height = 1000.0f;
    content->style().min_height = 1000.0f;
    view->add_child(std::move(content));
    page->add_child(std::move(scroller));
    root.set_content(std::move(page));
    root.layout(true);
  }

  /// 再跑一帧布局（`scroll_to_end` 需要“下一帧”才拿到新上限）。
  void layout_again() { root.layout(true); }
};

}  // namespace

ST_TEST(scroll_offset_property_is_writable) {
  ScrollFixture fx;
  ST_REQUIRE(fx.view != nullptr);
  ST_REQUIRE(fx.view->max_scroll() > 0.0f);   // 前提：内容确实超出

  // 初始在顶部
  ST_CHECK_NEAR(fx.view->scroll_offset(), 0.0f, 1e-3f);

  // 写 offset → 真的动
  ST_CHECK(fx.view->set_property("offset", "120"));
  ST_CHECK_NEAR(fx.view->scroll_offset(), 120.0f, 1e-3f);

  // `scroll` 是同义词（get_property 两个名字都认，写也应当对称）
  ST_CHECK(fx.view->set_property("scroll", "40"));
  ST_CHECK_NEAR(fx.view->scroll_offset(), 40.0f, 1e-3f);
}

ST_TEST(scroll_offset_is_clamped_not_rejected) {
  ScrollFixture fx;
  ST_REQUIRE(fx.view != nullptr);
  const float max_scroll = fx.view->max_scroll();
  ST_REQUIRE(max_scroll > 0.0f);

  // 超过上限 → 夹到底（这是“滚到底”的合理表达方式，`scroll_to_end()` 就是 1e9）
  ST_CHECK(fx.view->set_property("offset", "1e9"));
  ST_CHECK_NEAR(fx.view->scroll_offset(), max_scroll, 1e-2f);

  // 负值 → 夹到 0
  ST_CHECK(fx.view->set_property("offset", "-500"));
  ST_CHECK_NEAR(fx.view->scroll_offset(), 0.0f, 1e-3f);
}

ST_TEST(scroll_offset_rejects_garbage) {
  ScrollFixture fx;
  ST_REQUIRE(fx.view != nullptr);
  const float before = fx.view->scroll_offset();

  // 非数字 / 带单位 / 空串 → **如实 false**，且不改偏移
  ST_CHECK(!fx.view->set_property("offset", "abc"));
  ST_CHECK(!fx.view->set_property("offset", "12px"));   // 只看 prefix 就会错收成 12
  ST_CHECK(!fx.view->set_property("offset", ""));
  ST_CHECK(!fx.view->set_property("offset", "nan"));
  ST_CHECK_NEAR(fx.view->scroll_offset(), before, 1e-6f);
}

ST_TEST(scroll_actions_move_the_viewport) {
  ScrollFixture fx;
  ST_REQUIRE(fx.view != nullptr);
  const float max_scroll = fx.view->max_scroll();
  ST_REQUIRE(max_scroll > 100.0f);

  // 绝对定位
  ST_CHECK(fx.view->invoke_action("scroll_to", "80"));
  ST_CHECK_NEAR(fx.view->scroll_offset(), 80.0f, 1e-3f);

  // 相对量（属性面表达不了的那种）
  ST_CHECK(fx.view->invoke_action("scroll_by", "50"));
  ST_CHECK_NEAR(fx.view->scroll_offset(), 130.0f, 1e-3f);

  ST_CHECK(fx.view->invoke_action("scroll_by", "-30"));
  ST_CHECK_NEAR(fx.view->scroll_offset(), 100.0f, 1e-3f);

  // 到底 / 到顶
  ST_CHECK(fx.view->invoke_action("scroll_to_end", ""));
  ST_CHECK_NEAR(fx.view->scroll_offset(), max_scroll, 1e-2f);
  ST_CHECK(fx.view->invoke_action("scroll_to_top", ""));
  ST_CHECK_NEAR(fx.view->scroll_offset(), 0.0f, 1e-3f);

  // 未知动作如实拒绝（不是静默 true）
  ST_CHECK(!fx.view->invoke_action("scroll_sideways", "10"));
}

ST_TEST(scroll_to_end_needs_next_frame_for_new_content) {
  ScrollFixture fx;
  ST_REQUIRE(fx.view != nullptr);

  const float before = fx.view->max_scroll();
  // 追加内容把它变长
  auto extra = std::make_unique<Panel>();
  extra->set_id("extra");
  extra->style().height = 500.0f;
  extra->style().min_height = 500.0f;
  fx.view->add_child(std::move(extra));

  // 第一帧：上限还是旧的 → `scroll_to_end` 只能到旧上限（这是既有契约）
  fx.view->scroll_to_end();
  ST_CHECK_NEAR(fx.view->scroll_offset(), before, 1e-2f);

  // 下一帧布局后：上限变了，再滚才到真底
  fx.layout_again();
  const float after = fx.view->max_scroll();
  ST_CHECK(after > before);
  fx.view->scroll_to_end();
  ST_CHECK_NEAR(fx.view->scroll_offset(), after, 1e-2f);
}

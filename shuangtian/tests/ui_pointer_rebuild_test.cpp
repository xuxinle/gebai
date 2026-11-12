/// 内容树**重建之后**的指针态：指针没动，悬浮/光标必须按当前位置重算。
///
/// ## 守的是什么
///
/// 悬浮态在框架里由**悬浮链**（`hover_chain_`）承载，而链是“指针上一次命中时
/// 算出来的答案”。内容树一旦重建，链上那些元素就是一批已销毁的实例——框架会在
/// `prune_stale_pointers` 里把它们摘掉（必须如此，否则解引用即 UB），
/// 但**摘掉之后如果不再重算**，新树上同一位置就永远不会进入悬浮态：
///
/// | 场景 | 修复前 | 应有的行为 |
/// |---|---|---|
/// | 指针停在卡片上，数据刷新（列表 `clear` + 逐项重建） | 悬浮丢失，直到鼠标动一下 | 按同一位置重算，悬浮照旧 |
///
/// 这与浏览器的 `:hover` 不同构：CSS 的 `:hover` 是“指针位置 × 当前布局”的
/// **持续**状态，元素重建不影响它。重建式渲染（本框架的主流写法）必须自己补这一跳。
///
/// ## 用例必须走**子容器重建**这条路
///
/// `UiRoot::set_content` 自己就会置 `pointer_state_dirty_`（整棵换树），走那条路
/// 测不出本缺陷——修复前后都绿（实测：写成 `set_content` 版本时回退修复仍然全绿）。
/// 应用里的重建是 `list.clear()` + 逐项 `append`，即 `Element::clear_children` /
/// `remove_child` + `add_child`——**这些路径拿不到 root**，所以它们才是缺陷现场。
///
/// ## 为什么不能只靠“几何变了”这个判据
///
/// `layout()` 末尾有一处几何签名比对（没挑动任何东西的重排不整帧重绘）。但重建式
/// 渲染**经常产出完全相同的几何**——实测相期的卡片刷新：每块矩形逐像素一致，
/// 元素却全换了。那种重建在几何签名上不可见，所以本用例刻意让重建前后**布局完全相同**。
///
/// ## 逆向验证
///
/// 把 `UiRoot::prune_stale_pointers` 里
/// `if (hover_chain_.size() != chain_before) pointer_state_dirty_ = true;`
/// 这一行去掉 → `hover_survives_list_rebuild` 的主断言立刻变红。

#include "st/test/test.hpp"

#include <format>
#include <memory>
#include <string>

#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::math::Point;
using st::ui::Button;
using st::ui::Element;
using st::ui::Event;
using st::ui::EventKind;
using st::ui::MouseCursor;
using st::ui::UiRoot;

/// 一枚按钮 + 悬浮回调计数（回调是组件“看见自己悬浮”的唯一入口）。
struct Probe {
  Button* element{nullptr};
  int enters{0};
  int leaves{0};

  void bind(Button* owned) {
    element = owned;
    element->set_on_hover([this](bool hovered) { (hovered ? enters : leaves) += 1; });
  }
};

/// 夹具：根下是一个稳定的 `list` 容器，容器里逐项重建按钮。
///
/// 结构特意与相期的卡片列表同形（`#cards` 清空后逐张重建）——那正是缺陷现场。
struct RebuildFixture {
  UiRoot root{};
  Element* list{nullptr};
  Probe first{};
  Probe second{};
  int generation{0};
  st::raster::Canvas canvas{320, 160};
  MouseCursor cursor_last{MouseCursor::Arrow};

  RebuildFixture() {
    root.set_theme(st::ui::Theme::light());
    auto column = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
    auto row = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Row);
    row->set_id("list");
    list = row.get();
    column->add_child(std::move(row));
    root.set_content(std::move(column));
    root.set_viewport(st::math::Size{320.0f, 160.0f});
    root.cursor_sink = [this](MouseCursor cursor) { cursor_last = cursor; };
    fill();
    root.layout(true);
  }

  /// 把 `list` 清空后重建两个按钮（几何与上一次完全相同）。
  ///
  /// 每轮给按钮写一个递增的 `hint` 序号：它既是“确实是新实例”的可靠标识
  ///（旧实例已销毁，读不回新序号），也不影响布局/命中。
  /// **不能拿指针不等当标识**：刚释放的地址常被新实例复用（实测踩到）。
  void fill() {
    ++generation;
    list->clear_children();
    auto left = std::make_unique<Button>("左");
    left->set_id("btn-a");
    left->set_cursor(MouseCursor::Hand);
    left->set_hint(std::format("gen-{}", generation));
    first.bind(left.get());
    auto right = std::make_unique<Button>("右");
    right->set_id("btn-b");
    right->set_hint(std::format("gen-{}", generation));
    second.bind(right.get());
    list->add_child(std::move(left));
    list->add_child(std::move(right));
  }

  /// 重建式刷新：清空 + 逐项重建（**不碰 root**——这正是缺陷现场）。
  void refresh() {
    fill();
    root.layout(true);
  }

  void move_to(Point point) {
    Event event;
    event.kind = EventKind::MouseMove;
    event.position = point;
    (void)root.dispatch(event);
  }

  [[nodiscard]] auto button(const std::string& id) -> Button* {
    Element* found = root.find(id);
    return found == nullptr ? nullptr : static_cast<Button*>(found);
  }
};

}  // namespace

/// **重建后悬浮必须还在**：指针不动、布局不变，只是元素换了实例。
///
/// 同时钉住两件事：
/// ① 新实例真的处于悬浮态（否则“悬停才显示”的控件永远不出现）；
/// ② 光标也重算（手型不能因为重建就退回 Arrow——那是“这块能点”的唯一提示）。
ST_TEST(hover_survives_list_rebuild) {
  RebuildFixture fixture;
  const Point spot = fixture.first.element->bounds().center();
  fixture.move_to(spot);
  ST_REQUIRE(fixture.first.element->hovered());   // 前置成立：悬浮确实在
  ST_CHECK_EQ(fixture.first.enters, 1);
  ST_CHECK(fixture.root.cursor() == MouseCursor::Hand);

  const int gen_before = fixture.generation;
  fixture.refresh();
  (void)fixture.root.paint_frame(fixture.canvas);

  Button* fresh = fixture.button("btn-a");
  ST_REQUIRE(fresh != nullptr);
  // 前置成立：确实是**新实例**（用序号证明，不用指针不等——地址会被复用）
  ST_REQUIRE(fixture.generation == gen_before + 1);
  ST_CHECK_EQ(fresh->hint(), std::format("gen-{}", fixture.generation));
  // 几何没变——否则本用例就不再证明“几何签名捕捉不到”那条判据
  ST_CHECK_NEAR(fresh->bounds().center().x, spot.x, 0.5f);
  ST_CHECK_NEAR(fresh->bounds().center().y, spot.y, 0.5f);

  ST_CHECK(fresh->hovered());                    // ← 修复前：false（悬浮丢失）
  ST_CHECK(fixture.root.cursor() == MouseCursor::Hand);
  // 不断言旧实例收到 HoverOut：它在 `clear_children` 时就被销毁了，而本修复是**又
  // 发现它不在树上才重算的——对已销毁对象派发事件就是 UB（`prune_stale_pointers`
  // 的整个前提就是这个）。所以“旧实例的离开通知”不在本用例的断言范围内。
}

/// 对偶：重建后指针位置**不再落在任何控件上**时，悬浮必须真的清掉
/// （不能因为“曾经悬停过”就留着）。
ST_TEST(hover_clears_when_rebuild_removes_the_content_under_pointer) {
  RebuildFixture fixture;
  fixture.move_to(fixture.first.element->bounds().center());
  ST_REQUIRE(fixture.first.element->hovered());
  ST_REQUIRE(fixture.root.cursor() == MouseCursor::Hand);

  // 只重建一枚远离原位置的窄按钮：指针原处变成空白。
  fixture.list->clear_children();
  auto far = std::make_unique<Button>("远");
  far->set_id("btn-far");
  far->style().width = 40.0f;
  far->style().height = 20.0f;
  far->style().margin.left = 250.0f;
  far->style().margin.top = 100.0f;
  fixture.list->add_child(std::move(far));
  fixture.root.layout(true);
  (void)fixture.root.paint_frame(fixture.canvas);

  const Button* now = fixture.button("btn-far");
  ST_REQUIRE(now != nullptr);
  ST_CHECK(!now->hovered());                             // 指针不在它上面
  ST_CHECK(fixture.root.cursor() == MouseCursor::Arrow);  // 手型必须退掉
}

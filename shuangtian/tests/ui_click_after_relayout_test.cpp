/// 一次按放**只归属按下时锁定的那个元素**——释放后补发的 `Click` 不得在新布局里
/// 二次命中另一个元素。
///
/// ## 需求的来源：一个真实缺陷（用户 2026-11-08）
///
/// 「gallery 点最大化，DPI 从 1.5x 变成 2x」。
///
/// 三件事叠在一起才出得来（都实测过）：
/// ① 后端在 `WM_LBUTTONUP` 之后**无条件补发一个 `Click`**（`platform_win32.cpp`：
///    Down → Up → Click，与协议 `input.mouse{kind:"click"}` 同一序列）；
/// ② 点最大化/还原会**当场改变窗口几何**——视口 2355 → 2560 逻辑，标题栏右端的
///    按钮整体右移（实测 `#dpi-toggle` 从 x=1988.6 挪到 x=2193.3）；
/// ③ `dispatch` 的 `Click` 分支**重新 `hit_test(event.position)`**：这颗 Click 于是
///    落在新布局里同一个屏幕点所对应的**另一个控件**上，把它 `activate()` 了。
///
/// 实测轨迹：鼠标点在逻辑 (2286.3, 17.0)（最大化按钮 = 标题栏右缘往左 1.5×46）。
/// 窗口化布局下那个点够不着任何按钮；窗口一放大，同一个逻辑点落进「DPI 1.5x」
/// （新 bounds `[2193.3, 2293.8]`），于是 DPI 跳到 2.0、状态栏出现
/// 「DPI 2.0x · 字形已按物理分辨率重栅格化」。窗口宽度不同则差几十像素，
/// 所以这个缺陷「时有时无」。
///
/// ## 口径
///
/// 指针手势的归属在 **MouseDown 那一刻**就已确定（与 `pressed_` 的拖拽归属同一条
/// 契约）。后续 Up/Click 是那次手势的**收尾**，不是新的一次命中。因此 `Click` 的目标
/// 是「本次按放锁定的元素」；只有**没有先导 down/up** 的 Click（控制通道
/// `input.mouse{kind:"click"}`、单测夹具）才回到按位置命中的老路——那条语义不能动，
/// 否则协议里的 click 就「点了没反应」。
///
/// ⚠ 断言分两半：**先证明前置成立**（B 确实被挪到了落点上），再断言「按放 A 不连累 B」。
/// 否则 B 本来就不在落点上，用例会假绿。

#include "st/test/test.hpp"

#include <functional>
#include <memory>

#include "st/math/geometry.hpp"
#include "st/ui/components/button.hpp"
#include "st/ui/components/panel.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::math::Point;
using st::math::Size;
using st::ui::Button;
using st::ui::Event;
using st::ui::EventKind;
using st::ui::Panel;
using st::ui::UiRoot;

/// 一枚会记账的按钮。
struct Probe {
  Button* element{nullptr};
  int clicks{0};
};

/// 夹具：A 在左、B 在右；**按下 A 的那一刻**可以把 B 挪到落点上
/// （等价于「点窗口控制按钮导致标题栏重排」）。
///
/// 几何一律**显式指定**（`place_b`），不用流式排版的副作用：本用例的命题是
/// 「几何变了以后后续事件的归属不变」，前提几何必须完全可控——流式排版会因为
/// 文字宽度把 B 放到意料之外的位置，断言就跟着假红（`ui_pointer_state_test.cpp`
/// 头部同一条 ⚠）。
struct RelayoutFixture {
  UiRoot root{};
  Probe a{};
  Probe b{};

  RelayoutFixture() {
    root.set_theme(st::ui::Theme::light());
    auto row = std::make_unique<Panel>(st::ui::FlexDirection::Row);
    auto a_owned = std::make_unique<Button>("A");
    a_owned->set_id("btn-a");
    a.element = a_owned.get();
    a_owned->on_click = [this] { ++a.clicks; };
    auto b_owned = std::make_unique<Button>("B");
    b_owned->set_id("btn-b");
    b.element = b_owned.get();
    b_owned->on_click = [this] { ++b.clicks; };
    row->add_child(std::move(a_owned));
    row->add_child(std::move(b_owned));
    root.set_content(std::move(row));
    root.set_viewport(Size{600.0f, 120.0f});
    root.layout(true);
  }

  void press(Point point) { emit(EventKind::MouseDown, point); }
  void release(Point point) { emit(EventKind::MouseUp, point); }
  void click(Point point) { emit(EventKind::Click, point); }

  void emit(EventKind kind, Point point) {
    Event event;
    event.kind = kind;
    event.position = point;
    event.button = 1;
    (void)root.dispatch(event);
  }

  /// 把 B 绝对定位到指定矩形（父内容盒坐标）。
  void place_b(float x, float y, float w, float h) {
    auto& style = b.element->style();
    style.absolute = true;
    style.left = x;
    style.top = y;
    style.width = w;
    style.height = h;
    root.layout(true);
  }
};

}  // namespace

// ───────────────── 主用例：按放 A，Click 不得连累被挪过来的 B ─────────────────

ST_TEST(click_after_relayout_stays_with_the_pressed_element) {
  RelayoutFixture fixture;
  fixture.place_b(480.0f, 0.0f, 120.0f, 40.0f);

  const st::math::Rect a_rect = fixture.a.element->bounds();
  // 落点取 A 的左半：真实缺陷里那个点原本谁都够不着，重排后才压到邻居身上。
  const Point landing{a_rect.x + a_rect.width * 0.25f, a_rect.y + a_rect.height * 0.5f};
  ST_CHECK(a_rect.contains(landing));
  ST_CHECK(!fixture.b.element->bounds().contains(landing));   // 前置①：此刻只有 A

  // 【1】按下 A（真实点击的第一颗事件）。
  fixture.press(landing);

  // 【2】按下的效果发生：窗口最大化 → 重排 → B 挪到落点之下。
  //     真实时序就是这样：置位 `size_dirty_`/`pending_*`，在本帧取画布时重建并重排，
  //     而补发的 `Click` 在同一帧后面的批次里派发——到达时几何已经是新的。
  fixture.place_b(landing.x - 30.0f, landing.y - 20.0f, 120.0f, 40.0f);
  // 前置②：B **确实**压在落点上了——否则本用例证明不了任何事。
  ST_CHECK(fixture.b.element->bounds().contains(landing));

  // 【3】释放 + 后端无条件补发的那一颗 Click。
  fixture.release(landing);
  fixture.click(landing);

  ST_CHECK_EQ(fixture.a.clicks, 1);   // A 照常被激活
  ST_CHECK_EQ(fixture.b.clicks, 0);   // **B 不得被连累**（缺陷正是这里）
}

// ───────── 反向护栏：普通点击照常生效（别把 Click 的语义修死） ─────────

ST_TEST(click_after_relayout_does_not_break_a_plain_click) {
  RelayoutFixture fixture;
  fixture.place_b(480.0f, 0.0f, 120.0f, 40.0f);
  const Point b_center = fixture.b.element->bounds().center();

  fixture.press(b_center);
  fixture.release(b_center);
  fixture.click(b_center);
  ST_CHECK_EQ(fixture.b.clicks, 1);

  // 重复按放仍逐次生效（没有「只认第一次」这类副作用）：
  // 末尾两颗 Click 里第一颗收尾、第二颗是协议 `input.mouse{kind:"click"}` 的形态。
  fixture.press(b_center);
  fixture.release(b_center);
  fixture.click(b_center);
  ST_CHECK_EQ(fixture.b.clicks, 2);
  fixture.click(b_center);
  ST_CHECK_EQ(fixture.b.clicks, 3);
}

// ───── 只有 Click、没有先导 down/up：仍按位置命中（协议语义不能动） ─────

ST_TEST(click_without_press_falls_back_to_hit_test) {
  RelayoutFixture fixture;
  fixture.place_b(480.0f, 0.0f, 120.0f, 40.0f);

  fixture.click(fixture.b.element->bounds().center());
  ST_CHECK_EQ(fixture.b.clicks, 1);

  // 无先导按放时，位置变了就该点到新的那个（`input.mouse{kind:"click"}` 的语义）。
  fixture.click(fixture.a.element->bounds().center());
  ST_CHECK_EQ(fixture.a.clicks, 1);
  ST_CHECK_EQ(fixture.b.clicks, 1);
}

// ───── 拖动过的松手仍不算点击（既有语义不许被这次改动破坏） ─────

ST_TEST(dragged_release_is_still_not_a_click) {
  RelayoutFixture fixture;
  fixture.place_b(480.0f, 0.0f, 120.0f, 40.0f);
  const Point a_center = fixture.a.element->bounds().center();

  fixture.press(a_center);
  const Point far{a_center.x + 40.0f, a_center.y};   // 位移 40 > 阈值 4
  fixture.release(far);
  fixture.click(far);
  ST_CHECK_EQ(fixture.a.clicks, 0);
  ST_CHECK_EQ(fixture.b.clicks, 0);
}

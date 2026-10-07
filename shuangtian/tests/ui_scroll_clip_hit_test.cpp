/// 滚动容器里的**命中裁剪**。
///
/// 背景（真实缺陷，2026-10-07 gbcode 的终端滚回）：滚动内容被排成一个
/// **远高于视口**的矩形（实测 `y=-119.3 / height=851.3`，横跨整个窗口）。
/// 绘制侧一直有裁剪（`ScrollView::paint` 里 `push_clip_rect(bounds_)`），
/// 而**命中侧没有**——`UiRoot::hit_test_subtree` 只逐层找“谁的 bounds 含点”。
/// 于是滚动之后点标题栏/菜单栏，命中报告给出的却是那个看不见的滚回文本：
/// 用户感受是“顶部那条点不动了、菜单打不开”。
///
/// 判定：这是框架缺陷，不是示例用法问题——“超出容器的那部分不存在”不能只对像素
/// 成立、对输入不成立（同一份裁剪两种口径）。`ScrollView` 本来就带 `clip_children`
/// 语义（它在 `paint` 里自己压裁剪），命中侧照着同一个开关裁即可。
///
/// 用例做过逆向验证（把命中裁剪去掉即断言变红）。

#include "st/test/test.hpp"

#include <memory>
#include <string>

#include "st/core/print.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/scroll.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::math::Point;
using st::ui::Button;
using st::ui::Panel;
using st::ui::ScrollView;
using st::ui::UiRoot;

/// 与真实场景同形：`[顶栏][滚动视口][底栏]`，视口里的内容**滚到视口之外**、
/// 且它的 bounds 仍然盖住了顶栏所在的区域（真案例：终端滚回文本盖住标题栏）。
///
/// ⚠ 夹具必须复现“越出的那部分盖住另一个兄弟元素”这个几何特征：若只验证
/// “视口内的点命中的是谁”，缺陷根本不会显形（那个点在视口内，命中内容本来就对）。
struct ClipFixture {
  UiRoot root{};
  Button* inside{nullptr};
  Button* topbar{nullptr};
  Button* outside{nullptr};

  ClipFixture() {
    root.set_viewport(st::math::Size{400.0f, 300.0f});
    auto column = std::make_unique<Panel>(st::ui::FlexDirection::Column);
    column->set_id("page");

    // 顶栏：模拟标题栏/菜单栏那一带（在滚动视口**之上**）。
    auto top = std::make_unique<Button>("顶栏");
    top->set_id("topbar");
    top->style().height = 40.0f;
    topbar = top.get();
    column->add_child(std::move(top));

    auto view = std::make_unique<ScrollView>();
    view->set_id("clipper");
    view->style().height = 100.0f;   // 视口高 100（y = 40..140）
    auto content = std::make_unique<Panel>(st::ui::FlexDirection::Column);
    content->set_id("tall");
    content->style().height = 600.0f;
    auto inner = std::make_unique<Button>("里面");
    inner->set_id("inside");
    inner->style().height = 560.0f;  // 滚到底后：y = -500..60（盖住了顶栏）
    inside = inner.get();
    content->add_child(std::move(inner));
    view->add_child(std::move(content));
    column->add_child(std::move(view));

    auto below = std::make_unique<Button>("外面");
    below->set_id("outside");
    outside = below.get();
    column->add_child(std::move(below));
    root.set_content(std::move(column));
    root.layout(true);
    if (auto* view_ptr = dynamic_cast<ScrollView*>(root.find("clipper")); view_ptr != nullptr) {
      view_ptr->scroll_to_end();
    }
    root.layout(true);
  }
};

}  // namespace

ST_TEST(scrolling_content_outside_the_viewport_is_not_hit) {
  ClipFixture fx;
  auto* view = dynamic_cast<ScrollView*>(fx.root.find("clipper"));
  ST_REQUIRE(view != nullptr);
  ST_REQUIRE(fx.inside != nullptr);
  st::print("[clip] 视口 y {:.1f}..{:.1f} · 被滚出的按钮 y {:.1f}..{:.1f} · 顶栏 y {:.1f}..{:.1f}\n",
            view->bounds().y, view->bounds().bottom(), fx.inside->bounds().y,
            fx.inside->bounds().bottom(), fx.topbar->bounds().y, fx.topbar->bounds().bottom());
  ST_REQUIRE(!fx.inside->bounds().is_empty());
  // 前提：它越出视口，且越出的那部分**盖住了顶栏**（缺陷能显形的最小几何）。
  ST_REQUIRE(fx.inside->bounds().y < view->bounds().y);
  ST_REQUIRE(fx.inside->bounds().contains(fx.topbar->bounds().center()));

  // ① 顶栏上的点：必须命中**顶栏**，而不是那个看不见的滚出内容。
  st::ui::Element* hit = fx.root.hit_test(fx.topbar->bounds().center());
  ST_CHECK(hit == static_cast<st::ui::Element*>(fx.topbar));

  // ② 视口内的点：命中的是滚动容器（或它可见的内容），但**不该**是“已滚出视口”
  //    那部分的坐标——这里用同一个元素在视口外的坐标做对照。
  const Point above_view{view->bounds().center().x, view->bounds().y - 10.0f};
  ST_CHECK(fx.root.hit_test(above_view) != static_cast<st::ui::Element*>(fx.inside));

  // ③ 没被裁的元素照旧可点（裁剪只影响越出容器的那一支）。
  ST_REQUIRE(fx.outside->bounds().height > 0.0f);
  ST_CHECK(fx.root.hit_test(fx.outside->bounds().center()) ==
            static_cast<st::ui::Element*>(fx.outside));
}

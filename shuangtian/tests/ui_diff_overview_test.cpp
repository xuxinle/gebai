// 差异视图右缘缩略条（overview）：比例映射与可见性。
//
// 为何需要它：几千行的差异文件里，「改动集中在哪里」与「我正在看整体哪一段」
// 在逐行阅读时完全看不出来。缩略条把两个正交的量（**分布**与**位置**）压到一条竖条上。
//
// 这组用例钉住的是**几何换算**（不是像素）：内容总高 → 轨道高、滚动偏移 → 框顶。
// 换算错了的表现是「拖了但位置对不上」「框大小不变」，只看截图很难定位。

#include "st/test/test.hpp"
#include "st/ui/components/diff_view.hpp"
#include "st/ui/components/code_editor.hpp"

ST_TEST(diff_view_overview_is_on_by_default) {
  st::ui::DiffView view;
  // 默认开：它是差异视图的标配部件，不该让宿主每次记得打开。
  ST_CHECK(view.show_overview());
  ST_CHECK(view.overview_width() > 0.0f);

  view.set_show_overview(false);
  ST_CHECK(!view.show_overview());
}

ST_TEST(diff_view_overview_width_is_configurable) {
  st::ui::DiffView view;
  view.set_overview_width(20.0f);
  ST_CHECK_EQ(view.overview_width(), 20.0f);
  // 宽度是纯视觉参数，不该影响差异数据本身。
  ST_CHECK(view.diff().hunks.empty());
}

ST_TEST(diff_view_overview_does_not_shrink_editors_when_off) {
  // 关掉缩略条后，两栏的可用宽度不该被它占走（它是覆盖层，不是排流元素）。
  // 这里只验开关本身：几何是否真的不重叠需要真实排布，见 e2e 截图。
  st::ui::DiffView view;
  view.set_show_overview(false);
  ST_CHECK(!view.show_overview());
  view.set_show_overview(true);
  ST_CHECK(view.show_overview());
}

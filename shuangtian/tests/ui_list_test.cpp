/// 列表交互测试：**"可激活"必须走 `activate()`**。
///
/// 背景（实测踩到）：`ListItem` 原先把激活逻辑写在 `on_event` 里而没有覆盖 `activate()`，
/// 于是"真实鼠标点击有效、协议/脚本 `invoke(click)` 静默无效"——对"应用可被智能体驱动"
/// 是致命的（自动化流程正是靠 `invoke`）。这个测试把两条路径都钉住：
/// 鼠标点击与 `invoke_action("click")` 必须产生**同一个结果**。

#include "st/test/test.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "st/ui/components/list.hpp"
#include "st/ui/theme.hpp"

namespace {

[[nodiscard]] auto make_context() -> st::ui::RenderContext {
  static const st::ui::Theme theme = st::ui::Theme::light();
  return st::ui::RenderContext{theme, nullptr, 0.0};
}

}  // namespace

ST_TEST(list_item_activate_via_invoke_selects) {
  st::ui::List list;
  list.set_id("tasks");
  for (int index = 0; index < 3; ++index) {
    list.add_item("任务 " + std::to_string(index));
  }
  std::vector<std::size_t> selected;
  list.set_on_select([&selected](std::size_t index) { selected.push_back(index); });

  st::ui::ListItem* second = list.item(1);
  ST_CHECK(second != nullptr);

  // 协议/脚本路径：`invoke(click)` → `Element::invoke_action` → `activate()`
  ST_CHECK(second->invoke_action("click", {}));
  ST_CHECK_EQ(selected.size(), 1U);
  ST_CHECK_EQ(selected[0], 1U);
  ST_CHECK_EQ(list.selected_index(), 1U);
  // 选中态落在被点的项上（而不是别的项）
  ST_CHECK(list.item(1)->selected());
  ST_CHECK(!list.item(0)->selected());
}

ST_TEST(list_item_mouse_click_matches_invoke) {
  st::ui::List list;
  list.add_item("A");
  list.add_item("B");
  std::vector<std::size_t> selected;
  list.set_on_select([&selected](std::size_t index) { selected.push_back(index); });

  // 鼠标路径：真实的 Click 事件
  auto context = make_context();
  st::ui::Event event;
  event.kind = st::ui::EventKind::Click;
  event.position = st::math::Point{10.0f, 10.0f};
  st::ui::ListItem* first = list.item(0);
  ST_CHECK(first != nullptr);
  (void)first->on_event(context, event);

  ST_CHECK_EQ(selected.size(), 1U);
  ST_CHECK_EQ(selected[0], 0U);
  ST_CHECK_EQ(list.selected_index(), 0U);
}

ST_TEST(list_rebuild_keeps_selection_callback_working) {
  // 数据刷新（`clear_items` + 重建）后，回调仍应指向新建的项
  st::ui::List list;
  std::vector<std::size_t> selected;
  list.set_on_select([&selected](std::size_t index) { selected.push_back(index); });
  list.add_item("旧项");
  list.clear_items();
  list.add_item("新项 A");
  list.add_item("新项 B");

  ST_CHECK(list.invoke_action("click", {}));  // 容器本身被"点"不改变项选择
  ST_CHECK(list.item(1)->invoke_action("click", {}));
  ST_CHECK_EQ(selected.size(), 1U);
  ST_CHECK_EQ(selected[0], 1U);
}

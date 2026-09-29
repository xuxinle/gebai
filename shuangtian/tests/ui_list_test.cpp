/// 列表交互与**稳定身份**测试。
///
/// 两条线索：
/// 1. "可激活"必须走 `activate()`（鼠标点击与 `invoke(click)` 等效）——背景：`ListItem`
///    原先把逻辑写在 `on_event` 里，导致真实点击有效、协议 `invoke(click)` 静默无效。
/// 2. **稳定 id**：自动 id 是路径式的，索引会随插入/删除整体位移，于是"刷新后原来看中的
///    那一项变成别的数据"。`key` + `sync_items` 让同一逻辑元素在重建/重排后仍是同一个 id。

#include "st/test/test.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "st/ui/components/basic.hpp"
#include "st/ui/components/list.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

[[nodiscard]] auto make_context() -> st::ui::RenderContext {
  static const st::ui::Theme theme = st::ui::Theme::light();
  return st::ui::RenderContext{theme, nullptr, 0.0};
}

/// 把 `sync_items` 的入参写法收敛一下（测试里只关心 key + 文案）。
[[nodiscard]] auto entries(const std::vector<std::pair<std::string, std::string>>& items)
    -> std::vector<st::ui::List::Entry> {
  std::vector<st::ui::List::Entry> out;
  out.reserve(items.size());
  for (const auto& [key, label] : items) {
    st::ui::List::Entry entry;
    entry.key = key;
    entry.label = label;
    out.push_back(std::move(entry));
  }
  return out;
}

[[nodiscard]] auto ids_of(const st::ui::List& list) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (std::size_t index = 0; index < list.item_count(); ++index) {
    out.push_back(list.item(index)->derived_id());
  }
  return out;
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

ST_TEST(list_sync_keeps_id_and_selection_across_reorder) {
  st::ui::List list;
  list.set_id("tasks");
  list.sync_items(entries({{"t1", "写文档"}, {"t2", "写代码"}, {"t3", "跑测试"}}));
  const std::vector<std::string> before = ids_of(list);
  ST_CHECK_EQ(before.size(), 3U);
  ST_CHECK(before[0].ends_with("ListItem@t1"));

  // 选中 t2（写代码）
  (void)list.item(1)->invoke_action("click", {});
  ST_CHECK_EQ(list.selected_index(), 1U);
  const std::string chosen_id = list.item(1)->derived_id();

  // 重排 + 增删：t2 挪到首位、t1 删除、t4 新增
  list.sync_items(entries({{"t2", "写代码"}, {"t3", "跑测试"}, {"t4", "发布"}}));

  // ① id 跟着 key 走，不跟索引走：同一个 id 仍然指向"写代码"
  ST_CHECK_EQ(list.item(0)->derived_id(), chosen_id);
  ST_CHECK(list.item(0)->derived_id().ends_with("ListItem@t2"));
  // ② 选中项跟随被选中的那个 key，而不是旧索引
  ST_CHECK_EQ(list.selected_index(), 0U);
  ST_CHECK(list.item(0)->selected());
  // ③ 元素被复用（同一个对象），而不是销毁重建
  ST_CHECK_EQ(list.item(0)->label(), std::string("写代码"));
  // ④ 消失的 key 不留在树里
  for (std::size_t index = 0; index < list.item_count(); ++index) {
    ST_CHECK(!list.item(index)->derived_id().ends_with("ListItem@t1"));
  }
}

ST_TEST(list_sync_updates_label_without_changing_id) {
  st::ui::List list;
  list.set_id("tasks");
  list.sync_items(entries({{"t1", "旧文案"}}));
  const std::string original = list.item(0)->derived_id();
  list.sync_items(entries({{"t1", "新文案"}}));
  ST_CHECK_EQ(list.item(0)->label(), std::string("新文案"));
  ST_CHECK_EQ(list.item(0)->derived_id(), original);  // 文案变了，身份不变
}

ST_TEST(list_sync_reports_when_selected_item_removed) {
  st::ui::List list;
  list.sync_items(entries({{"t1", "A"}, {"t2", "B"}}));
  int notifications = 0;
  std::size_t last = 0;
  list.set_on_select([&](std::size_t index) {
    ++notifications;
    last = index;
  });
  (void)list.item(0)->invoke_action("click", {});
  ST_CHECK_EQ(notifications, 1);
  ST_CHECK_EQ(last, 0U);

  // 被选中的项从数据里消失：如实告知（而不是把选中静默挪到别的数据上）
  list.sync_items(entries({{"t2", "B"}}));
  ST_CHECK_EQ(notifications, 2);
  ST_CHECK_EQ(last, st::ui::kNoSelection);
  ST_CHECK_EQ(list.selected_index(), st::ui::kNoSelection);
}

ST_TEST(list_key_makes_ids_selectable_by_id_term) {
  // key 里含空格/点在 id 里会被替换成 '-'：id 会出现在选择器里，
  // 而选择器在 `#`/`.`/`:`/`[`/空白 处切词——不转就会"看不见这个元素"。
  st::ui::List list;
  list.set_id("tasks");
  list.sync_items(entries({{"task 42.v2", "带特殊字符的 key"}}));
  const std::string id = list.item(0)->derived_id();
  ST_CHECK(id.find(' ') == std::string::npos);
  ST_CHECK(id.find('.') == std::string::npos);
  ST_CHECK(id.ends_with("ListItem@task-42-v2"));
}

ST_TEST(element_key_survives_container_rebuild) {
  // 通用路径（不只 List）：任何元素只要设了 key，自动 id 就用 key 而不是索引
  st::ui::Panel page;
  page.set_id("page");
  auto first = std::make_unique<st::ui::Button>("A");
  first->set_key("alpha");
  auto second = std::make_unique<st::ui::Button>("B");
  second->set_key("beta");
  st::ui::Button* first_ptr = first.get();
  (void)page.add_child(std::move(first));
  (void)page.add_child(std::move(second));
  ST_CHECK_EQ(first_ptr->derived_id(), std::string("page/Button@alpha"));

  // 在前面插入一个新的兄弟：带 key 的 id 不受索引位移影响
  auto inserted = std::make_unique<st::ui::Button>("C");
  inserted->set_key("gamma");
  (void)page.insert_child(0, std::move(inserted));
  ST_CHECK_EQ(first_ptr->derived_id(), std::string("page/Button@alpha"));
}

/// 元素标识（`derived_id`）测试：**内存安全 + 可用性**。
///
/// 为什么单独立一个测试文件：`derived_id` 原先没有任何覆盖，而它里面藏着一个悬垂
/// `string_view`（把 `std::format` 的临时字符串存进 `vector<string_view>`）——
/// 结果是自动生成的 id 里出现垃圾字节与 NUL、相邻兄弟甚至拿到同一个 id，
/// 选择器/协议/脚本因此都定位不到元素。这类问题在"随便点一下界面"时很容易被忽略，
/// 但对"应用可被智能体驱动"是致命的，因此用测试钉死。

#include "st/test/test.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "st/ui/components/basic.hpp"
#include "st/ui/components/list.hpp"
#include "st/ui/ui_root.hpp"

namespace {

/// id 里不允许出现的字符：控制字符（含 NUL）会让选择器、JSON 消费方、
/// 脚本层各自用不同方式"坏掉"，且排查时很难看出问题在哪。
[[nodiscard]] auto has_control_character(const std::string& text) -> bool {
  for (const unsigned char value : text) {
    if (value < 0x20 || value == 0x7F) return true;
  }
  return false;
}

}  // namespace

ST_TEST(element_id_explicit_wins) {
  st::ui::Panel panel;
  panel.set_id("mine");
  ST_CHECK_EQ(panel.derived_id(), std::string("mine"));
}

ST_TEST(element_id_derived_is_path_like_and_clean) {
  st::ui::Panel page;
  page.set_id("page");
  auto first = std::make_unique<st::ui::Button>("A");
  auto second = std::make_unique<st::ui::Button>("B");
  st::ui::Button* first_ptr = first.get();
  st::ui::Button* second_ptr = second.get();
  (void)page.add_child(std::move(first));
  (void)page.add_child(std::move(second));

  const std::string first_id = first_ptr->derived_id();
  const std::string second_id = second_ptr->derived_id();

  ST_CHECK(!has_control_character(first_id));
  ST_CHECK(!has_control_character(second_id));
  ST_CHECK(first_id.starts_with("page/"));
  ST_CHECK(first_id.find("Button") != std::string::npos);
  // 兄弟必须拿到不同的 id（否则选择器只能命中一个，另一个永远不可达）
  ST_CHECK(first_id != second_id);
  // 同一元素重复取 id 必须一致（协议/脚本会反复取用）
  ST_CHECK_EQ(first_ptr->derived_id(), first_id);
}

ST_TEST(element_id_after_list_rebuild_is_unique_and_clean) {
  // 实测场景：清空列表再按新数据重建后，列表项 id 必须仍然合法且互不相同
  st::ui::List list;
  list.set_id("tasks");
  for (int index = 0; index < 3; ++index) {
    list.add_item("任务 " + std::to_string(index));
  }
  std::vector<std::string> ids;
  for (std::size_t index = 0; index < list.item_count(); ++index) {
    const std::string id = list.item(index)->derived_id();
    ST_CHECK(!has_control_character(id));
    ids.push_back(id);
  }
  ST_CHECK_EQ(ids.size(), 3U);
  ST_CHECK(ids[0] != ids[1]);
  ST_CHECK(ids[1] != ids[2]);
  ST_CHECK(ids[0] != ids[2]);

  // 重建后仍成立（`clear_items` + `add_item` 是刷新数据的标准做法）
  list.clear_items();
  ST_CHECK_EQ(list.item_count(), 0U);
  list.add_item("只剩一项");
  ST_CHECK_EQ(list.item_count(), 1U);
  ST_CHECK(!has_control_character(list.item(0)->derived_id()));
}

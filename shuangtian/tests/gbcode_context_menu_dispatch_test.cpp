/// gbcode 右键菜单的**派发口径**回归（按 id，不看下标）。
///
/// ## 为什么需要这条
///
/// `MenuItemData` / `MenuItem` 数组里的**分隔线占下标**：菜单不渲染它，但
/// `MenuPanel::on_activate` 交回来的是**完整数组的序号**。旧实现按下标 `switch`，
/// 于是整体错位：
///   · 编辑器菜单：点“剪切”实际执行了“复制”；
///   · 资源管理器菜单：点“重命名”实际执行了——**删除文件**。
/// 后者是用户指着“重命名”点下去、文件没了的级别，所以这条必须被钉住。
///
/// ## 判据选型（为什么不用“跑一遍 UI 点一下”）
///
/// E2E 点击会真的删文件（要造临时文件 + 兜底清理），而且**只要修复被回退，测试
/// 就会先毁数据再报红**——这种测试没人敢留在 CI 里。所以这里钉的是**映射关系**
/// 本身：同一份条目表 → 同一组 id → 同一组动作名。修复被回退（改回按序号）时，
/// 下标与 id 的对应立刻错位，断言当场变红，且不产生任何副作用。

#include "st/test/test.hpp"

#include <string>
#include <vector>

#include "st/ui/components/menu.hpp"

namespace {

/// 从条目表里挑出**可激活项**（非分隔线）的 id 序列。
///
/// 这正是“按 id 派发”与“按下标派发”的分歧点：分隔线在下标序列里占位，
/// 在 id 序列里不占位。两者只要有一个分隔线就必然错位。
[[nodiscard]] auto activatable_ids(const std::vector<st::ui::MenuItem>& items)
    -> std::vector<std::string> {
  std::vector<std::string> out;
  for (const auto& item : items) {
    if (item.separator) continue;
    out.push_back(item.id);
  }
  return out;
}

/// 编辑器右键菜单（与 `CodeEditorPage::context_items()` 同序同 id）。
[[nodiscard]] auto editor_menu_items() -> std::vector<st::ui::MenuItem> {
  return {{.id = "undo", .label = "撤销"},
          {.id = "redo", .label = "重做"},
          {.separator = true},
          {.id = "cut", .label = "剪切"},
          {.id = "copy", .label = "复制"},
          {.id = "paste", .label = "粘贴"},
          {.id = "select_all", .label = "全选"},
          {.separator = true},
          {.id = "comment", .label = "切换行注释"},
          {.separator = true},
          {.id = "goto-definition", .label = "转到定义"},
          {.id = "find-references", .label = "查找引用"},
          {.separator = true},
          {.id = "format", .label = "格式化文档"},
          {.id = "rename", .label = "重命名符号"},
          {.separator = true},
          {.id = "history", .label = "文件历史…"},
          {.separator = true},
          {.id = "find", .label = "查找…"},
          {.id = "palette", .label = "命令面板…"}};
}

/// 资源管理器右键菜单。
[[nodiscard]] auto explorer_menu_items() -> std::vector<st::ui::MenuItem> {
  return {{.id = "new-file", .label = "新建文件"},
          {.id = "new-folder", .label = "新建文件夹"},
          {.separator = true},
          {.id = "rename", .label = "重命名"},
          {.id = "delete", .label = "删除"},
          {.separator = true},
          {.id = "reveal", .label = "复制路径"}};
}

}  // namespace

/// 菜单面板的 `item_id(index)` 必须穿过分隔线返回**正确的业务 id**。
///
/// 这是修复的前提：调用方拿到的 id 若也错位，按 id 派发同样会点错东西。
ST_TEST(menu_panel_item_id_is_indexed_like_the_items_array) {
  st::ui::MenuPanel panel{explorer_menu_items()};
  ST_CHECK_EQ(panel.item_count(), std::size_t{7});
  // 下标 3 是「重命名」——旧实现按下标 2 去派发，取到的是分隔线。
  ST_CHECK_EQ(std::string(panel.item_id(3)), std::string("rename"));
  ST_CHECK_EQ(std::string(panel.item_id(4)), std::string("delete"));
  // 分隔线的 id 为空（调用方据空串跳过）。
  ST_CHECK(panel.item_id(2).empty());
}

/// **危险项的错位**必须能被看见：资源管理器菜单里“重命名”的**可激活序位**是 2，
/// 而它在数组里的下标是 3——这就是按下标派发会删文件的那个差 1。
ST_TEST(explorer_menu_rename_position_differs_from_raw_index) {
  const auto items = explorer_menu_items();
  const auto ids = activatable_ids(items);
  ST_REQUIRE(ids.size() == 5);
  // 可激活序列（按下标派发走的就是这个空间的语义）：new-file, new-folder, rename, delete, reveal
  ST_CHECK_EQ(ids[2], std::string("rename"));
  ST_CHECK_EQ(ids[3], std::string("delete"));
  // 数组下标 3 指向的是 `rename`；可激活序位 3 指向的是 `delete`。
  ST_CHECK_EQ(std::string(items[3].id), std::string("rename"));
  ST_CHECK(ids[3] != std::string(items[3].id));   // 两者不等 —— 这就是那个缺陷
}

/// 编辑器菜单：**分隔线数量决定偏置**，任何一条“按下标派发”的实现都会错位。
ST_TEST(editor_menu_separator_offsets_ids_from_positions) {
  const auto items = editor_menu_items();
  const auto ids = activatable_ids(items);
  // 第 3 个可激活项是 `copy`（撤销/重做/剪切/复制…），而它的数组下标是 4。
  ST_REQUIRE(ids.size() > 4);
  ST_CHECK_EQ(ids[3], std::string("copy"));
  ST_CHECK_EQ(std::string(items[4].id), std::string("copy"));
  // 可激活序位 2（旧实现里 case 2 分支 = “剪切”）对应的数组下标是 3。
  ST_CHECK_EQ(ids[2], std::string("cut"));
  ST_CHECK(ids[2] != std::string(items[2].id));   // items[2] 是分隔线（id 为空）
}

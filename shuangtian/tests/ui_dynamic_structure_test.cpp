/// 动态结构的压力回归：**clear + 大量 append 不得崩溃**。
///
/// ## 为什么有这份用例
///
/// `examples/xiangqi` 实测踩到：对同一容器 `clear()` 后同帧追加十几个子元素，
/// 宿主进程直接 `std::bad_alloc` 崩掉，**没有任何 JS 异常**——日志停在上一条
/// 业务日志上，极易误判成业务代码的问题。
///
/// 这条路径是**框架自己的缺陷**（`ui.build` 先挂根内容再挪走 + `ui.clear` 摘除即销毁），
/// 不该由应用层用"槽位复用"绕过。本用例把它钉住：只要再犯，构建即红。
///
/// ## 断言口径
///
/// 1. 反复 clear+append 多轮不崩（内存不无界增长）
/// 2. 清空后 `find` 不到旧子元素（真的释放，不是泄漏）
/// 3. 重建后 id 能被正确寻址（没有残留同 id 幽灵）

#include <cstddef>
#include <string>
#include <vector>

#include "st/test/test.hpp"
#include "st/ui/dsl.hpp"
#include "st/ui/element.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::ui::Element;
using st::ui::Panel;
using st::ui::UiRoot;

/// 就地建一棵「可清空的容器」树：root → content → Column(id=host)。
///
/// 为什么就地建而不是返回：`UiRoot` 禁拷贝（独占内容树），返回会要求移动构造，
/// 而它持有的是 unique_ptr 树——就地建最直接，也不会误引入移动语义的细节。
auto make_root_in_place(UiRoot& root, Panel*& host) -> void {
  auto content = st::ui::dsl::make_element("Panel");
  auto* content_raw = content.get();
  root.set_content(std::move(content));
  auto column = st::ui::dsl::make_element("Panel");
  if (auto* panel = dynamic_cast<Panel*>(column.get()); panel != nullptr) {
    panel->style().direction = st::ui::FlexDirection::Column;
  }
  column->set_id("host");
  host = dynamic_cast<Panel*>(content_raw->add_child(std::move(column)));
}

}  // namespace

ST_TEST(ui_clear_then_append_does_not_crash) {
  UiRoot root;
  Panel* host = nullptr;
  make_root_in_place(root, host);
  ST_REQUIRE(host != nullptr);

  // 连续多轮：每轮清空 + 建 24 个子元素（含嵌套）。
  // 24 这个数字远超界面实际规模（卡片是个位数量级），
  // 故意放大以让"每轮泄漏一点"的缺陷在少数轮内就暴露。
  for (int round = 0; round < 12; ++round) {
    while (host->child_count() > 0) {
      (void)host->remove_child(host->child_at(0));
    }
    for (int i = 0; i < 24; ++i) {
      auto row = st::ui::dsl::make_element("Panel");
      row->set_id(std::string("row-") + std::to_string(i));
      auto* row_raw = host->add_child(std::move(row));
      // 每行再挂两个子元素（模拟"一张卡里还有按钮/文本"）
      for (int j = 0; j < 2; ++j) {
        auto cell = st::ui::dsl::make_element("Text");
        cell->set_id(std::string("cell-") + std::to_string(i) + "-" + std::to_string(j));
        (void)row_raw->add_child(std::move(cell));
      }
    }
    ST_CHECK(host->child_count() == 24);
  }

  // 最后一轮留下的应是完好的一批（不是半死状态）
  ST_CHECK(host->child_count() == 24);
  ST_CHECK(root.find("row-0") != nullptr);
  ST_CHECK(root.find("cell-23-1") != nullptr);
}

ST_TEST(ui_clear_releases_children_completely) {
  UiRoot root;
  Panel* host = nullptr;
  make_root_in_place(root, host);
  ST_REQUIRE(host != nullptr);

  for (int i = 0; i < 8; ++i) {
    auto child = st::ui::dsl::make_element("Text");
    child->set_id(std::string("gone-") + std::to_string(i));
    (void)host->add_child(std::move(child));
  }
  ST_CHECK(host->child_count() == 8);
  ST_CHECK(root.find("gone-0") != nullptr);

  while (host->child_count() > 0) {
    (void)host->remove_child(host->child_at(0));
  }
  ST_CHECK(host->child_count() == 0);
  // 关键：清空后旧 id 必须真的不可寻址（否则是"看着没了其实还在"，
  // 后续同 id 新建会撞名 → find 拿到幽灵元素）。
  for (int i = 0; i < 8; ++i) {
    ST_CHECK(root.find(std::string("gone-") + std::to_string(i)) == nullptr);
  }

  // 同 id 复用必须干净（无幽灵残留）
  auto again = st::ui::dsl::make_element("Text");
  again->set_id("gone-0");
  auto* back = host->add_child(std::move(again));
  ST_REQUIRE(back != nullptr);
  ST_CHECK(root.find("gone-0") == back);
  ST_CHECK(dynamic_cast<Panel*>(back) == nullptr);   // 确实是个文本节点，不是残留的旧 Panel
}

ST_TEST(ui_repeated_rebuild_keeps_structure_consistent) {
  UiRoot root;
  Panel* host = nullptr;
  make_root_in_place(root, host);
  ST_REQUIRE(host != nullptr);

  // 模拟"每次数据变化就整片重建"（交互面板的常见形态）。
  // 重点是**每轮都换一批 id**——这正是真实场景（卡片被答复掉、新卡进来）。
  for (int round = 0; round < 10; ++round) {
    while (host->child_count() > 0) {
      (void)host->remove_child(host->child_at(0));
    }
    for (int i = 0; i < 6; ++i) {
      auto card = st::ui::dsl::make_element("Panel");
      card->set_id(std::string("r") + std::to_string(round) + "-card-" + std::to_string(i));
      (void)host->add_child(std::move(card));
    }
    ST_CHECK(host->child_count() == 6);
    // 上一轮的 id 不该还活着
    if (round > 0) {
      ST_CHECK(root.find(std::string("r") + std::to_string(round - 1) + "-card-0") == nullptr);
    }
  }
}

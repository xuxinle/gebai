/// 列表的**容器高度契约**与**图标搬运**回归。
///
/// 两条都来自 gbcode 大纲的实测现场（2026-11-16）：
/// 1. `List` 的显式 `height` 是**单行高**，容器高必须由行数累加——基类
///    `Element::measure` 会把它当容器总高，于是 164 个符号只排 1 行（
///    `visible_rows = 1`、滚动宿主 `content_height = 22`、滚动条不出现）。
/// 2. `sync_items` 的**新建分支**必须搬 `icon`：旧实现只在复用分支里调
///    `set_icon`，于是数据首次到达时图标全部丢失（要等下一次 sync 才补上）。

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

/// 造一份 N 项的列表数据（可带图标）。
[[nodiscard]] auto make_entries(std::size_t count, const std::string& icon)
    -> std::vector<st::ui::List::Entry> {
  std::vector<st::ui::List::Entry> out;
  out.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    out.push_back(st::ui::List::Entry{.key = "row-" + std::to_string(index),
                                      .label = "行 " + std::to_string(index),
                                      .icon = icon});
  }
  return out;
}

}  // namespace

/// 显式 `height` = **单行高**：容器高 = 行数 × 行高（+ 间距），不是单行高。
ST_TEST(list_explicit_height_is_row_height_not_container_height) {
  st::ui::List list;
  list.set_id("probe");
  list.sync_items(make_entries(20, {}));
  // 行高 22、gap 0（与 gbcode 大纲同一组参数）。
  list.set_item_gap(0.0f);
  list.style().height = 22.0f;

  const auto context = make_context();
  st::ui::Constraints constraints;
  constraints.max_width = 240.0f;
  constraints.max_height = st::ui::kUnbounded;
  constraints.available_width = 240.0f;
  constraints.available_height = st::ui::kUnbounded;
  list.measure(context, constraints);

  // 20 行 × 22 = 440。容器若被当“单行高”，这里会是 22。
  ST_CHECK_NEAR(list.measured_size().height, 440.0f, 0.5f);

  // 行高确实下放到每一项（否则上面那个数对了、画面仍是一行挤 40px）。
  const st::ui::ListItem* first = list.item(0);
  ST_REQUIRE(first != nullptr);
  ST_CHECK_NEAR(first->style().height, 22.0f, 0.01f);

  // **反向护栏**：`visible_item_count` 按真实行高算（511px 视口 / 22 = 23），
  // 用固定常量 `ListItem::kHeight` 会少报一半（旧实现报 12）。
  list.arrange(context, st::math::Rect{0.0f, 0.0f, 240.0f, 511.0f});
  ST_CHECK_EQ(list.visible_item_count(), std::size_t{23});
}

/// `sync_items` 的**新建路径**也要搬图标（旧实现只有复用路径搬）。
ST_TEST(list_sync_items_carries_icon_on_create_path) {
  st::ui::List list;
  list.set_id("probe");
  list.sync_items(make_entries(3, "sym-class"));
  for (std::size_t index = 0; index < 3; ++index) {
    const st::ui::ListItem* item = list.item(index);
    ST_REQUIRE(item != nullptr);
    ST_CHECK_EQ(item->icon(), std::string("sym-class"));
  }

  // 复用路径同样要跟得上（数据里图标变了，元素复用但图标更新）。
  list.sync_items(make_entries(3, "sym-function"));
  for (std::size_t index = 0; index < 3; ++index) {
    const st::ui::ListItem* item = list.item(index);
    ST_REQUIRE(item != nullptr);
    ST_CHECK_EQ(item->icon(), std::string("sym-function"));
  }
}

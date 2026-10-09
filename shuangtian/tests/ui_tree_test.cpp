/// `Tree` 组件测试：扁平数组语义、按 key 的稳定身份、目录展开/懒加载、键盘导航。
///
/// 树是**自绘**组件（无子 Element）：行身份靠 `sync_nodes` 的 key 对齐维护，
/// 选中态跟 key 走（与 `List::sync_items` 同一套设计理念）。

#include "st/test/test.hpp"

#include <memory>
#include <string>
#include <vector>

#include "st/ui/components/tree.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::ui::TreeNode;
using st::ui::Tree;
using st::ui::UiRoot;

[[nodiscard]] auto make_context() -> st::ui::RenderContext {
  static const st::ui::Theme theme = st::ui::Theme::light();
  return st::ui::RenderContext{theme, nullptr, 0.0};
}

/// 两层小树：src/（展开时含 main.cpp、util/（收起））、README.md。
/// **扁平化由调用方做**：src 收起时子行不进数组。
[[nodiscard]] auto sample_nodes(bool src_expanded = true) -> std::vector<TreeNode> {
  std::vector<TreeNode> nodes{
      TreeNode{.key = "src", .label = "src", .expanded = src_expanded, .is_dir = true, .depth = 0},
  };
  if (src_expanded) {
    nodes.push_back(
        TreeNode{.key = "src/main.cpp", .label = "main.cpp", .is_dir = false, .depth = 1});
    nodes.push_back(
        TreeNode{.key = "src/util", .label = "util", .expanded = false, .is_dir = true, .depth = 1});
  }
  nodes.push_back(TreeNode{.key = "README.md", .label = "README.md", .is_dir = false, .depth = 0});
  return nodes;
}

/// 布局一棵树（固定 240×400 的盒子，行几何可预测）。
void layout_tree(Tree& tree, const std::vector<TreeNode>& nodes) {
  tree.sync_nodes(nodes);
  tree.arrange(make_context(), st::math::Rect{0.0f, 0.0f, 240.0f, 400.0f});
}

[[nodiscard]] auto click(st::ui::EventKind kind, float x, float y) -> st::ui::Event {
  st::ui::Event event;
  event.kind = kind;
  event.position = st::math::Point{x, y};
  return event;
}

}  // namespace

ST_TEST(tree_sync_reuses_identity_by_key) {
  Tree tree;
  tree.set_id("files");
  layout_tree(tree, sample_nodes());
  ST_CHECK_EQ(tree.node_count(), std::size_t{4});

  // 数据刷新（中间插入一行 + 顺序不变）：行身份按 key 对齐
  std::vector<TreeNode> next = sample_nodes();
  next.insert(next.begin() + 2,
              TreeNode{.key = "src/extra.hpp", .label = "extra.hpp", .depth = 1});
  tree.sync_nodes(next);
  ST_CHECK_EQ(tree.node_count(), std::size_t{5});
  ST_CHECK(tree.index_of_key("src/extra.hpp").has_value());
  ST_CHECK(tree.index_of_key("src/main.cpp").has_value());

  // 消失的 key 被移除
  tree.sync_nodes({TreeNode{.key = "README.md", .label = "README.md"}});
  ST_CHECK_EQ(tree.node_count(), std::size_t{1});
  ST_CHECK(!tree.index_of_key("src").has_value());
}

ST_TEST(tree_selected_key_survives_sync) {
  Tree tree;
  layout_tree(tree, sample_nodes());
  tree.select_key("src/main.cpp");
  ST_CHECK_EQ(std::string(tree.selected_key()), std::string("src/main.cpp"));

  // 同 key 仍在 → 选中保持（顺序/文案变化不影响）
  std::vector<TreeNode> next = sample_nodes();
  next[1].label = "main.cpp (改名)";
  tree.sync_nodes(next);
  ST_CHECK_EQ(std::string(tree.selected_key()), std::string("src/main.cpp"));

  // 选中的 key 消失 → 如实清空（不静默挪到别的行）
  tree.sync_nodes({TreeNode{.key = "README.md", .label = "README.md"}});
  ST_CHECK_EQ(std::string(tree.selected_key()), std::string(""));
  ST_CHECK_EQ(tree.selected_index(), Tree::kNoSelection);
}

ST_TEST(tree_collapse_shrinks_visible_rows) {
  Tree tree;
  // src 收起：子树不进数组（扁平化由调用方做），行数从 4 → 2
  layout_tree(tree, sample_nodes(/*src_expanded=*/false));
  ST_CHECK_EQ(tree.node_count(), std::size_t{2});
  ST_CHECK(tree.index_of_key("src").has_value());
  ST_CHECK(tree.index_of_key("README.md").has_value());
  ST_CHECK(!tree.index_of_key("src/main.cpp").has_value());
}

ST_TEST(tree_directory_click_toggles_with_callback) {
  Tree tree;
  layout_tree(tree, sample_nodes(/*src_expanded=*/false));

  std::string toggled_key;
  bool toggled_to = false;
  tree.on_toggle = [&](std::string_view key, bool expanded) {
    toggled_key = std::string(key);
    toggled_to = expanded;
  };

  // 点击 src 行（第 0 行，y=20 在行内）
  st::ui::Event event = click(st::ui::EventKind::Click, 30.0f, 20.0f);
  const st::ui::RenderContext context = make_context();
  ST_CHECK(tree.on_event(context, event));
  ST_CHECK_EQ(toggled_key, std::string("src"));
  ST_CHECK(toggled_to);  // 收起 → 展开

  // 展开后的同一行再点：目标状态为收起
  tree.sync_nodes(sample_nodes(/*src_expanded=*/true));
  tree.arrange(context, st::math::Rect{0.0f, 0.0f, 240.0f, 400.0f});
  st::ui::Event again = click(st::ui::EventKind::Click, 30.0f, 20.0f);
  ST_CHECK(tree.on_event(context, again));
  ST_CHECK(!toggled_to);
}

ST_TEST(tree_file_click_selects_and_notifies) {
  Tree tree;
  layout_tree(tree, sample_nodes());

  std::string picked;
  tree.on_select = [&](std::string_view key) { picked = std::string(key); };

  const st::ui::RenderContext context = make_context();
  // src/main.cpp 是第 1 行——用 `row_rect` 取行中心（不硬编码像素，
  // 否则行高一变这个用例就误报，实测踩到）。
  const auto row = tree.row_rect(1);
  st::ui::Event event =
      click(st::ui::EventKind::Click, row.x + 60.0f, row.y + row.height * 0.5f);
  ST_CHECK(tree.on_event(context, event));
  ST_CHECK_EQ(std::string(tree.selected_key()), std::string("src/main.cpp"));
  ST_CHECK_EQ(picked, std::string("src/main.cpp"));

  // select_key(notify=false) 不派发回调
  picked.clear();
  tree.select_key("README.md");
  ST_CHECK(picked.empty());
  ST_CHECK_EQ(std::string(tree.selected_key()), std::string("README.md"));
}

ST_TEST(tree_keyboard_navigation_across_visible_rows) {
  Tree tree;
  layout_tree(tree, sample_nodes());
  const st::ui::RenderContext context = make_context();

  auto key = [&](const std::string& name) {
    st::ui::Event event;
    event.kind = st::ui::EventKind::KeyDown;
    event.key = name;
    return tree.on_event(context, event);
  };

  ST_CHECK(key("ArrowDown"));  // 0 → 1（src/main.cpp）
  ST_CHECK_EQ(std::string(tree.selected_key()), std::string("src/main.cpp"));
  ST_CHECK(key("ArrowDown"));  // → 2（src/util，目录行）
  ST_CHECK_EQ(std::string(tree.selected_key()), std::string("src/util"));
  ST_CHECK(key("ArrowDown"));  // → 3（README.md，末行不再前进）
  ST_CHECK_EQ(std::string(tree.selected_key()), std::string("README.md"));
  ST_CHECK(key("ArrowDown"));
  ST_CHECK_EQ(std::string(tree.selected_key()), std::string("README.md"));
  ST_CHECK(key("ArrowUp"));  // 回到 2
  ST_CHECK_EQ(std::string(tree.selected_key()), std::string("src/util"));
  ST_CHECK(key("Home"));
  ST_CHECK_EQ(std::string(tree.selected_key()), std::string("src"));
  ST_CHECK(key("End"));
  ST_CHECK_EQ(std::string(tree.selected_key()), std::string("README.md"));

  // ←/→ 只作用于目录行；文件行（README.md）不消费
  ST_CHECK(!key("ArrowRight"));
  // 目录行 src：先回到收起基线，再验证 → 展开 / ← 收起
  ST_CHECK(key("Home"));
  tree.sync_nodes(sample_nodes(/*src_expanded=*/false));  // 选中态跟 key：仍是 src
  ST_CHECK_EQ(std::string(tree.selected_key()), std::string("src"));
  std::string toggled;
  tree.on_toggle = [&](std::string_view k, bool expanded) {
    toggled = std::format("{}:{}", k, expanded ? "open" : "close");
  };
  ST_CHECK(key("ArrowRight"));  // 展开 src
  ST_CHECK_EQ(toggled, std::string("src:open"));
  tree.sync_nodes(sample_nodes(/*src_expanded=*/true));
  ST_CHECK(key("ArrowLeft"));  // 已展开 → 收起
  ST_CHECK_EQ(toggled, std::string("src:close"));
}

ST_TEST(tree_row_rects_follow_indent_and_layout) {
  Tree tree;
  layout_tree(tree, sample_nodes());

  const st::math::Rect root_row = tree.node_rect("src");
  ST_CHECK_EQ(root_row.y, 0.0f);
  ST_CHECK_EQ(root_row.height, Tree::kRowHeight);
  const st::math::Rect child_row = tree.node_rect("src/main.cpp");
  ST_CHECK_EQ(child_row.y, Tree::kRowHeight);
  // 缩进断言：depth=1 比 depth=0 深 16px
  ST_CHECK_EQ(tree.node_indent("src"), 0.0f);
  ST_CHECK_EQ(tree.node_indent("src/main.cpp"), Tree::kIndentStep);
  ST_CHECK_EQ(tree.node_indent("src/util"), Tree::kIndentStep);
  ST_CHECK_EQ(tree.node_indent("README.md"), 0.0f);
  // 不存在的 key
  ST_CHECK(tree.node_rect("nope").is_empty());
  ST_CHECK_EQ(tree.node_indent("nope"), 0.0f);

  // 绘制不崩溃（无字体环境退化为 no-op 端口）
  st::raster::Canvas canvas(240, 400);
  tree.paint(make_context(), canvas);
  ST_CHECK(true);
}

ST_TEST(tree_lazy_children_appended_on_toggle) {
  Tree tree;
  layout_tree(tree, sample_nodes(/*src_expanded=*/false));
  const st::ui::RenderContext context = make_context();

  // 懒加载：展开时调用方才补子节点
  tree.on_toggle = [&](std::string_view key, bool expanded) {
    if (key != "src" || !expanded) return;
    std::vector<TreeNode> next = sample_nodes(/*src_expanded=*/false);
    next.insert(next.begin() + 1,
                TreeNode{.key = "src/main.cpp", .label = "main.cpp", .depth = 1});
    next.insert(next.begin() + 2,
                TreeNode{.key = "src/util", .label = "util", .is_dir = true, .depth = 1});
    tree.sync_nodes(next);
    tree.arrange(context, st::math::Rect{0.0f, 0.0f, 240.0f, 400.0f});
  };

  st::ui::Event event = click(st::ui::EventKind::Click, 30.0f, 20.0f);
  ST_CHECK(tree.on_event(context, event));
  ST_CHECK_EQ(tree.node_count(), std::size_t{4});  // 子节点在回调里才进来
  ST_CHECK(tree.index_of_key("src/main.cpp").has_value());
}

ST_TEST(tree_measure_grows_with_rows_and_indent) {
  Tree tree;
  const st::ui::RenderContext context = make_context();

  tree.sync_nodes(sample_nodes());
  st::ui::Constraints constraints;
  constraints.max_width = 240.0f;
  constraints.max_height = 400.0f;
  tree.measure(context, constraints);
  ST_CHECK_EQ(tree.measured_size().height, 4.0F * Tree::kRowHeight);
  ST_CHECK(tree.measured_size().width > 0.0f);

  // 深树测量更高、无字体环境宽度仍有底线（80px）
  tree.sync_nodes({TreeNode{.key = "deep", .label = "deep", .depth = 5}});
  tree.measure(context, constraints);
  ST_CHECK_EQ(tree.measured_size().height, Tree::kRowHeight);
  ST_CHECK(tree.measured_size().width >= 80.0f);
}

ST_TEST(tree_row_height_is_configurable) {
  // 密度是**宿主场景的属性**：资源管理器要密、设置清单可松。默认 26（旧 40 太松）。
  st::ui::Tree tree;
  ST_CHECK_EQ(tree.row_height(), Tree::kDefaultRowHeight);
  ST_CHECK_EQ(Tree::kDefaultRowHeight, 26.0F);

  st::ui::Tree dense;
  dense.set_row_height(24.0F);
  ST_CHECK_EQ(dense.row_height(), 24.0F);
  // 属性面可读写（自动化调密度做视觉验收）。
  ST_CHECK_EQ(dense.get_property("row_height").value_or(""), std::string("24"));
  ST_CHECK(dense.set_property("row_height", "32"));
  ST_CHECK_EQ(dense.row_height(), 32.0F);
  // 非法值如实拒绝且不改状态。
  ST_CHECK(!dense.set_property("row_height", "0"));
  ST_CHECK(!dense.set_property("row_height", "-5"));
  ST_CHECK(!dense.set_property("row_height", "abc"));
  ST_CHECK_EQ(dense.row_height(), 32.0F);

  // 行高影响行几何与测量高。
  st::ui::Tree sized;
  sized.sync_nodes({{.key = "a", .label = "a"}, {.key = "b", .label = "b"},
                    {.key = "c", .label = "c"}});
  st::ui::UiRoot root;
  root.set_viewport(st::math::Size{300.0F, 400.0F});
  auto host = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
  auto* raw = static_cast<st::ui::Tree*>(host->add_child(std::make_unique<st::ui::Tree>()));
  raw->sync_nodes({{.key = "a", .label = "a"}, {.key = "b", .label = "b"},
                   {.key = "c", .label = "c"}});
  root.set_content(std::move(host));
  root.layout(true);
  ST_CHECK_EQ(raw->measured_size().height, 3.0F * Tree::kDefaultRowHeight);
  raw->set_row_height(20.0F);
  root.layout(true);
  ST_CHECK_EQ(raw->measured_size().height, 3.0F * 20.0F);
  ST_CHECK_EQ(raw->row_rect(2).height, 20.0F);
  ST_CHECK_EQ(raw->row_rect(2).y - raw->row_rect(0).y, 40.0F);   // 2 行 × 20px
}

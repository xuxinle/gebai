/// `MindMapView` 与 `st::md::mindmap` 的回归。
///
/// ## 判据分成四组（对应四类会真的坏掉的东西）
///
/// | 组 | 判据 | 坏掉时的现象 |
/// |---|---|---|
/// | 序列化 | `.mm` 往返保真、未知属性不丢、二次往返逐字节一致、坏输入如实报错 | **静默丢数据**（最难发现：用户下次打开少了一个分支） |
/// | 编辑 | 增/删/改动/改父级/上移下移 + 结构不变量（父子双向一致、无环、根唯一） | 树坏掉后布局无限递归 / 存盘写出自引用 |
/// | 布局 | 确定性、同层不重叠、父在子带中央、连线两端贴框边 | "图看着别扭"：节点压在一起、边穿框 |
/// | 视图 | 属性面自洽、fit/zoom 分离、命中/拖拽/键盘、像素冒烟 | 用户按了没反应；或属性全对但画面是错的 |
///
/// ## 为什么布局断言写成"不变量"而不是"期望坐标"
///
/// 坐标会随字号、字体栈、内边距一起变，写死数值的测试改一次参数就要全线重写，
/// 而这种"每次都要改的测试"最后一定会被改成恒绿。所以断言的是**关系**：
/// 同层不重叠、父节点在自己带的中央、连线端点落在框边上。
///
/// ## 为什么序列化测试要拿"真实 FreeMind 文件形态"当输入
///
/// 自己写出去的格式自己一定读得回来——那种测试对**别人的文件**毫无保证。
/// 下面 `kRealFreeMind` 是 FreeMind 真实产物的形态：版本头、注释、`ID`、
/// `FOLDED`、`COLOR`、`LINK`、`richcontent TYPE="NOTE"`，以及我们不认识的
/// `STYLE`/`ICON`/`CREATED`/`MODIFIED`/`POSITION`。**"不认识的属性必须原样留住"**
/// 是本组最重要的一条：丢掉它们等于替用户删数据。
#include "st/test/test.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <string>
#include <vector>

#include "st/edit/undo.hpp"
#include "st/math/geometry.hpp"
#include "st/md/mindmap.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/components/all.hpp"
#include "st/ui/theme.hpp"

#include "tests/support/text_port_fixtures.hpp"

namespace {

using st::md::mindmap::Document;
using st::md::mindmap::LayoutOptions;
using st::md::mindmap::Node;
using st::ui::MindMapView;

constexpr std::size_t kNoNode = Node::kNoNode;

/// 定宽度量：每个码点 8px。几何断言只关心"关系"，用桩可让数字可复算。
auto stub_measure(float per_char = 8.0f) {
  return [per_char](std::string_view text, float /*font_size*/) {
    return per_char * static_cast<float>(st::utf8_length(text));
  };
}

auto overlaps(st::math::Rect a, st::math::Rect b) -> bool {
  return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
}

/// **真实 FreeMind 文件形态**（不是我们自己写出去的形态）。
/// 里面同时含：认得的属性（TEXT/ID/FOLDED/COLOR/LINK/NOTE）与**不认得的**
/// （STYLE/ICON/CREATED/MODIFIED/POSITION）。
constexpr std::string_view kRealFreeMind =
    "<map version=\"1.0.1\">\n"
    "<!-- To view this file, download free mind mapping software FreeMind from "
    "http://freemind.sourceforge.net -->\n"
    "<node CREATED=\"1700000000000\" ID=\"ID_1\" MODIFIED=\"1700000000001\" "
    "TEXT=\"产品路线图\">\n"
    "  <richcontent TYPE=\"NOTE\"><html><body><p>第一段</p><p>第二段</p></body></html></richcontent>\n"
    "  <node CREATED=\"1700000000002\" FOLDED=\"true\" ID=\"ID_2\" POSITION=\"right\" "
    "STYLE=\"bubble\" TEXT=\"2026 Q1\">\n"
    "    <node ID=\"ID_3\" TEXT=\"框架内核\"/>\n"
    "    <node COLOR=\"#ff8800\" ID=\"ID_4\" TEXT=\"组件库\">\n"
    "      <node ID=\"ID_5\" LINK=\"https://example.com/mm\" TEXT=\"思维导图\"/>\n"
    "    </node>\n"
    "  </node>\n"
    "  <node ID=\"ID_6\" POSITION=\"left\" TEXT=\"2026 Q2\"/>\n"
    "</node>\n"
    "</map>\n";

/// 造一棵有层次的示例树（8 个一级分支，每个 3 个叶子）。
auto build_sample(int branches = 4) -> Document {
  Document doc{std::string("中心主题")};
  for (int i = 0; i < branches; ++i) {
    const auto branch = doc.add_child(0, std::format("分支 {}", i + 1));
    for (int j = 0; j < 3; ++j) doc.add_child(branch, std::format("叶子 {}-{}", i + 1, j + 1));
  }
  return doc;
}

struct Fixture {
  st::ui::Theme theme = st::ui::Theme::light();
  st::test::FixedAdvanceTextPort port{};
  st::ui::RenderContext context{theme, &port, 0.0};

  void layout(st::ui::Element& element, float width, float height) {
    element.apply_theme(theme);
    st::ui::Constraints constraints{};
    constraints.max_width = width;
    constraints.max_height = height;
    constraints.available_width = width;
    constraints.available_height = height;
    element.measure(context, constraints);
    element.arrange(context, st::math::Rect{0.0f, 0.0f, width, height});
  }

  void paint(const st::ui::Element& element, st::raster::Canvas& canvas) {
    const_cast<st::ui::Element&>(element).paint(context, canvas);
  }
};

/// 结构不变量：任一节点都能从根走到；父子双向一致；无环。
void check_structure(const Document& doc) {
  ST_REQUIRE(doc.valid());
  std::vector<bool> reached(doc.node_count(), false);
  std::vector<std::size_t> stack{doc.root()};
  std::size_t visited = 0;
  while (!stack.empty()) {
    const std::size_t index = stack.back();
    stack.pop_back();
    ST_REQUIRE(index < doc.node_count());
    ST_CHECK(!reached[index]);   // 重复到达 = 有环或重复挂接
    if (reached[index]) continue;
    reached[index] = true;
    ++visited;
    const Node* node = doc.node(index);
    ST_REQUIRE(node != nullptr);
    for (const std::size_t child : node->children) {
      const Node* child_node = doc.node(child);
      ST_REQUIRE(child_node != nullptr);
      // 双向一致：子的 parent 必须指回来。
      ST_CHECK_EQ(child_node->parent, index);
      stack.push_back(child);
    }
  }
  ST_CHECK_EQ(visited, doc.node_count());
}

}  // namespace

// ————————————————————————————————————————————————————————————————————————————
// 序列化：FreeMind .mm
// ————————————————————————————————————————————————————————————————————————————

/// 读**真实 FreeMind 文件**：认得的字段全部到位。
ST_TEST(mindmap_freemind_reads_real_file) {
  const auto doc = Document::from_freemind(kRealFreeMind);
  ST_REQUIRE(doc.has_value());
  ST_CHECK_EQ(doc->node_count(), 6U);
  ST_CHECK_EQ(doc->meta().version, 1);   // `<map version="1.0.1">` 的主版本

  // 备注：`richcontent TYPE="NOTE"` 的两个 `<p>` 要变成两行（单段落里的 `\n`
  // 会被 HTML 空白折叠吃掉，所以用 `<p>` 分段）。
  const Node* root = doc->node(doc->root());
  ST_REQUIRE(root != nullptr);
  ST_CHECK_EQ(root->text, std::string("产品路线图"));
  ST_CHECK_EQ(root->id, std::string("ID_1"));
  ST_CHECK(root->note.find("第一段") != std::string::npos);
  ST_CHECK(root->note.find("第二段") != std::string::npos);
  ST_CHECK(root->note.find('\n') != std::string::npos);

  // FOLDED → collapsed
  const Node* q1 = doc->node(1);
  ST_REQUIRE(q1 != nullptr);
  ST_CHECK_EQ(q1->text, std::string("2026 Q1"));
  ST_CHECK(q1->collapsed);

  // COLOR → argb（`#rrggbb` → 不透明 ARGB）
  bool found_color = false;
  bool found_link = false;
  for (std::size_t index = 0; index < doc->node_count(); ++index) {
    const Node* node = doc->node(index);
    if (node->text == "组件库") {
      found_color = true;
      ST_CHECK_EQ(node->argb, 0xFFFF8800U);
    }
    if (node->text == "思维导图") {
      found_link = true;
      ST_CHECK_EQ(node->link, std::string("https://example.com/mm"));
    }
  }
  ST_CHECK(found_color);
  ST_CHECK(found_link);
  check_structure(*doc);
}

/// **不认识的属性必须原样留住**（丢它们等于替用户删数据）。
ST_TEST(mindmap_freemind_preserves_unknown_attributes) {
  const auto doc = Document::from_freemind(kRealFreeMind);
  ST_REQUIRE(doc.has_value());

  const auto has_attr = [&](std::size_t index, std::string_view key) {
    const Node* node = doc->node(index);
    if (node == nullptr) return false;
    return std::any_of(node->extra_attributes.begin(), node->extra_attributes.end(),
                       [&](const auto& pair) { return pair.first == key; });
  };
  ST_CHECK(has_attr(doc->root(), "CREATED"));
  ST_CHECK(has_attr(doc->root(), "MODIFIED"));
  ST_CHECK(has_attr(1, "STYLE"));      // 字体样式
  ST_CHECK(has_attr(1, "POSITION"));   // 左右侧提示

  // 写回去还在（值也要对）。
  const std::string saved = doc->to_freemind(true);
  ST_CHECK(saved.find("CREATED=\"1700000000000\"") != std::string::npos);
  ST_CHECK(saved.find("STYLE=\"bubble\"") != std::string::npos);
  ST_CHECK(saved.find("POSITION=\"left\"") != std::string::npos);
}

/// `richcontent TYPE="NODE"`（节点内嵌富文本）**不当备注吃掉、也不丢掉**。
///
/// 为何单独一条：它与 `TYPE="NOTE"` 是同一个元素名的两种语义。把 `NODE`
/// 错当备注会让用户看到“节点文字突然多了一段 HTML”；直接丢弃则是替用户删数据。
/// 实测过：这条一开始没写用例，回退实现后整套仍全绿——那就是测不到的路径。
ST_TEST(mindmap_freemind_keeps_richcontent_node) {
  constexpr std::string_view kWithNodeRich =
      "<map version=\"1.0.1\">\n"
      "  <node TEXT=\"根\">\n"
      "    <richcontent TYPE=\"NODE\"><html><body><p>节点内嵌富文本</p></body></html>"
      "</richcontent>\n"
      "  </node>\n"
      "</map>\n";
  const auto doc = Document::from_freemind(kWithNodeRich);
  ST_REQUIRE(doc.has_value());
  const Node* root = doc->node(doc->root());
  ST_REQUIRE(root != nullptr);

  // ① 不能当备注
  ST_CHECK(root->note.empty());
  // ② 也不能丢：留在 extra 里
  const bool kept = std::any_of(root->extra_attributes.begin(), root->extra_attributes.end(),
                                [](const auto& pair) { return pair.first == "richcontent"; });
  ST_CHECK(kept);
}

/// 二次往返**逐字节一致**：存取一轮不该在版本库里制造满屏 diff。
ST_TEST(mindmap_freemind_roundtrip_is_byte_stable) {
  const auto first = Document::from_freemind(kRealFreeMind);
  ST_REQUIRE(first.has_value());
  const std::string once = first->to_freemind(true);
  const auto second = Document::from_freemind(once);
  ST_REQUIRE(second.has_value());
  const std::string twice = second->to_freemind(true);
  ST_CHECK_EQ(once, twice);
}

/// 特殊字符保真：XML 元字符 / 引号 / 中文 / emoji / 制表符。
ST_TEST(mindmap_freemind_escapes_special_characters) {
  Document doc{std::string("A & B <c> \"d\" 'e'")};
  const auto child = doc.add_child(0, "含 <尖括号> & 与 \"引号\" 的节点");
  doc.add_child(child, "emoji 🎯 与制表\t符");
  (void)doc.set_note(0, "备注里也有 <tag> 和 & 和 \"q\"\n第二行");

  const std::string saved = doc.to_freemind(true);
  // 写出去必须是**转义后**的（未转义的 `<c>` 会让 FreeMind 读坏）。
  ST_CHECK(saved.find("&amp;") != std::string::npos);
  ST_CHECK(saved.find("&lt;") != std::string::npos);
  ST_CHECK(saved.find("TEXT=\"A & B") == std::string::npos);

  const auto back = Document::from_freemind(saved);
  ST_REQUIRE(back.has_value());
  ST_CHECK_EQ(back->text_of(0), doc.text_of(0));
  ST_CHECK_EQ(back->text_of(1), doc.text_of(1));
  ST_CHECK_EQ(back->text_of(2), doc.text_of(2));
  ST_CHECK_EQ(back->node(0)->note, doc.node(0)->note);
}

/// 坏输入**如实报错**（不许产出一张空图当成功）。
ST_TEST(mindmap_freemind_rejects_bad_input) {
  const auto wrong_root = Document::from_freemind("<html><body>x</body></html>");
  ST_CHECK(!wrong_root.has_value());
  ST_CHECK(wrong_root.error().message.find("<map>") != std::string::npos);

  const auto empty_map = Document::from_freemind("<map></map>");
  ST_CHECK(!empty_map.has_value());
  ST_CHECK(empty_map.error().message.find("node") != std::string::npos);
}

/// 深嵌套不爆栈（`.mm` 的深度由用户决定，递归实现会在这里崩）。
ST_TEST(mindmap_freemind_handles_deep_nesting) {
  Document deep{std::string("L0")};
  std::size_t cursor = 0;
  for (int i = 1; i <= 200; ++i) cursor = deep.add_child(cursor, std::format("L{}", i));
  ST_REQUIRE(deep.node_count() == 201U);
  ST_CHECK_EQ(deep.max_depth(), 200U);

  const std::string xml = deep.to_freemind(false);
  const auto back = Document::from_freemind(xml);
  ST_REQUIRE(back.has_value());
  ST_CHECK_EQ(back->node_count(), 201U);
  ST_CHECK_EQ(back->to_freemind(false), xml);
}

// ————————————————————————————————————————————————————————————————————————————
// 编辑：结构不变量
// ————————————————————————————————————————————————————————————————————————————

/// 加/删/改都维持结构不变量，且删除会**搬移下标**（所有引用跟着改）。
ST_TEST(mindmap_edit_keeps_structure_invariants) {
  Document doc = build_sample(3);
  check_structure(doc);
  const std::size_t before = doc.node_count();

  // 删掉第一个分支（连带 3 个叶子）
  const std::size_t branch = doc.node(0)->children.front();
  const std::size_t next = doc.remove_subtree(branch);
  ST_CHECK_EQ(doc.node_count(), before - 4U);
  check_structure(doc);
  // 删完必须选中一个**仍然合法**的节点（否则下一次编辑会改到别处）。
  ST_CHECK(next == kNoNode || next < doc.node_count());
}

/// 改父级：合法时成功，成环时**拒绝**（成环的后果是布局无限递归）。
ST_TEST(mindmap_edit_reparent_rejects_cycles) {
  Document doc = build_sample(2);
  const std::size_t branch = doc.node(0)->children[0];
  const std::size_t leaf = doc.node(branch)->children[0];
  const std::size_t other = doc.node(0)->children[1];

  // 非法：把分支挂到**自己的后代**下（成环）——必须在把 leaf 移走之前测，
  // 否则 leaf 已不是它的后代，这条就变成了合法操作（写错时这条会恒绿）。
  ST_CHECK(!doc.reparent(branch, leaf));
  check_structure(doc);

  // 合法：叶子挂到另一分支
  ST_CHECK(doc.reparent(leaf, other));
  check_structure(doc);
  // 非法：根没有父
  ST_CHECK(!doc.reparent(doc.root(), branch));
  // 非法：原地不动（应返回 false，不产生空历史条目）
  ST_CHECK(!doc.reparent(leaf, other));
  check_structure(doc);
}

/// 同级排序：上移/下移换位置，边界如实拒绝。
ST_TEST(mindmap_edit_reorders_siblings) {
  Document doc{std::string("root")};
  const auto a = doc.add_child(0, "a");
  const auto b = doc.add_child(0, "b");
  ST_CHECK(doc.node(0)->children[0] == a);

  ST_CHECK(doc.move_down(a));
  ST_CHECK(doc.node(0)->children[0] == b);
  ST_CHECK(!doc.move_down(a));    // 已在末尾
  ST_CHECK(doc.move_up(a));
  ST_CHECK(doc.node(0)->children[0] == a);
  ST_CHECK(!doc.move_up(a));      // 已在开头
  check_structure(doc);
}

/// 加同级紧挨自己之后（不是追加到末尾）——"我刚点的这一支旁边加一个"。
ST_TEST(mindmap_edit_add_sibling_inserts_after_self) {
  Document doc{std::string("root")};
  const auto a = doc.add_child(0, "a");
  const auto c = doc.add_child(0, "c");
  const auto inserted = doc.add_sibling(a, "b");
  const auto& children = doc.node(0)->children;
  ST_REQUIRE(children.size() == 3U);
  ST_CHECK_EQ(children[0], a);
  ST_CHECK_EQ(children[1], inserted);
  ST_CHECK_EQ(children[2], c);
  check_structure(doc);
}

/// 折叠只影响可见性，**不截断子树**（`subtree_size` 不看折叠）。
ST_TEST(mindmap_edit_collapse_affects_visibility_only) {
  Document doc = build_sample(2);
  const std::size_t branch = doc.node(0)->children[0];
  ST_CHECK_EQ(doc.visible_count(), doc.node_count());
  ST_CHECK(doc.set_collapsed(branch, true));
  ST_CHECK_EQ(doc.subtree_size(branch), 4U);              // 自己 + 3 叶子
  ST_CHECK(doc.visible_count() < doc.node_count());       // 但看不见了
  // 叶子折叠无意义 → 拒绝（不产生"点了没反应"的历史条目）
  const std::size_t leaf = doc.node(branch)->children[0];
  ST_CHECK(!doc.toggle_collapsed(leaf));
  ST_CHECK(doc.toggle_collapsed(branch));
  check_structure(doc);
}

/// 加子节点会**自动展开父级**（否则新节点立刻落在折叠区里 = "点了没反应"）。
ST_TEST(mindmap_edit_add_child_expands_collapsed_parent) {
  Document doc{std::string("root")};
  const auto child = doc.add_child(0, "child");
  ST_CHECK(doc.set_collapsed(0, true));
  ST_CHECK(doc.node(0)->collapsed);
  const auto grand = doc.add_child(0, "grand");
  ST_CHECK(grand != kNoNode);
  ST_CHECK(!doc.node(0)->collapsed);   // 自动展开
  ST_CHECK_EQ(doc.node(child)->children.size(), 0U);
}

// ————————————————————————————————————————————————————————————————————————————
// 布局
// ————————————————————————————————————————————————————————————————————————————

/// 确定性：同输入必得同一几何（否则截图回归会飘、缺陷无法复现）。
ST_TEST(mindmap_layout_is_deterministic) {
  const Document doc = build_sample(4);
  const auto measure = stub_measure();
  const LayoutOptions options;
  const auto first = st::md::mindmap::layout(doc, options, measure);
  const auto second = st::md::mindmap::layout(doc, options, measure);
  ST_REQUIRE(first.boxes.size() == second.boxes.size());
  for (std::size_t index = 0; index < first.boxes.size(); ++index) {
    ST_CHECK_EQ(first.boxes[index].rect.x, second.boxes[index].rect.x);
    ST_CHECK_EQ(first.boxes[index].rect.y, second.boxes[index].rect.y);
    ST_CHECK_EQ(first.boxes[index].rect.width, second.boxes[index].rect.width);
  }
  ST_CHECK_EQ(first.size.width, second.size.width);
}

/// 节点框**两两不重叠**（tidy-tree 的核心承诺；重叠就是"图看着乱"）。
ST_TEST(mindmap_layout_boxes_do_not_overlap) {
  const Document doc = build_sample(5);
  const auto result = st::md::mindmap::layout(doc, LayoutOptions{}, stub_measure());
  for (std::size_t i = 0; i < result.boxes.size(); ++i) {
    if (result.boxes[i].rect.is_empty()) continue;
    for (std::size_t j = i + 1; j < result.boxes.size(); ++j) {
      if (result.boxes[j].rect.is_empty()) continue;
      if (overlaps(result.boxes[i].rect, result.boxes[j].rect)) {
        ST_FAIL(std::format("节点 {} 与 {} 的框重叠", i, j));
      }
    }
  }
}

/// 内容包围盒**包住**所有节点（否则会有节点画到视口外/被裁掉）。
ST_TEST(mindmap_layout_size_covers_all_nodes) {
  const Document doc = build_sample(4);
  const auto result = st::md::mindmap::layout(doc, LayoutOptions{}, stub_measure());
  ST_REQUIRE(result.size.width > 0.0f);
  ST_REQUIRE(result.size.height > 0.0f);
  for (const auto& box : result.boxes) {
    if (box.rect.is_empty()) continue;
    ST_CHECK(box.rect.x >= 0.0f);
    ST_CHECK(box.rect.y >= 0.0f);
    ST_CHECK(box.rect.right() <= result.size.width + 0.01f);
    ST_CHECK(box.rect.bottom() <= result.size.height + 0.01f);
  }
}

/// 父节点在自己子树的**纵向中央**（思维导图的心智模型：父在两子之间）。
ST_TEST(mindmap_layout_parent_centers_on_children) {
  Document doc{std::string("root")};
  const auto a = doc.add_child(0, "a");
  const auto b = doc.add_child(0, "b");
  const auto result = st::md::mindmap::layout(doc, LayoutOptions{}, stub_measure());
  const float root_y = result.boxes[doc.root()].rect.center().y;
  const float a_y = result.boxes[a].rect.center().y;
  const float b_y = result.boxes[b].rect.center().y;
  // 根应当落在两个子节点的纵向中点附近（容差 2px 覆盖行高取整）
  ST_CHECK(std::abs(root_y - (a_y + b_y) * 0.5f) <= 2.0f);
}

/// 折叠会让子树**退出布局**（框为空），而不是被画在其他节点上面。
ST_TEST(mindmap_layout_collapsed_subtree_leaves_no_boxes) {
  Document doc = build_sample(2);
  const std::size_t branch = doc.node(0)->children[0];
  ST_CHECK(doc.set_collapsed(branch, true));
  const auto result = st::md::mindmap::layout(doc, LayoutOptions{}, stub_measure());
  // 分支自己还在
  ST_CHECK(!result.boxes[branch].rect.is_empty());
  ST_CHECK(result.boxes[branch].has_hidden_children);
  ST_CHECK_EQ(result.boxes[branch].hidden_children, 3U);
  // 但它的叶子不在布局里，也没有连线
  for (const std::size_t leaf : doc.node(branch)->children) {
    ST_CHECK(result.boxes[leaf].rect.is_empty());
  }
  for (const auto& link : result.links) {
    ST_CHECK(link.parent != branch);
  }
}

/// 两种方向都产出合法布局（`Both` 会用到负坐标，必须整体平移进留白内）。
ST_TEST(mindmap_layout_both_direction) {
  const Document doc = build_sample(4);
  LayoutOptions options;
  options.direction = st::md::mindmap::LayoutDirection::Both;
  const auto result = st::md::mindmap::layout(doc, options, stub_measure());
  ST_REQUIRE(!result.boxes.empty());
  for (const auto& box : result.boxes) {
    if (box.rect.is_empty()) continue;
    ST_CHECK(box.rect.x >= 0.0f);
    ST_CHECK(box.rect.right() <= result.size.width + 0.01f);
  }
  // 一级分支必须落在**两侧**（否则 `Both` 与 `Right` 没区别）。
  const Node* root = doc.node(doc.root());
  ST_REQUIRE(root != nullptr);
  bool has_left = false;
  bool has_right = false;
  for (const std::size_t child : root->children) {
    if (result.boxes[child].side < 0) has_left = true;
    if (result.boxes[child].side > 0) has_right = true;
  }
  ST_CHECK(has_left);
  ST_CHECK(has_right);
}

/// 命中测试：框内命中、框外不命中。
ST_TEST(mindmap_layout_hit_test) {
  const Document doc = build_sample(2);
  const auto result = st::md::mindmap::layout(doc, LayoutOptions{}, stub_measure());
  const std::size_t target = doc.node(0)->children[0];
  ST_CHECK_EQ(st::md::mindmap::hit_test(result, result.boxes[target].rect.center()), target);
  // 远离所有节点的地方
  ST_CHECK_EQ(st::md::mindmap::hit_test(result, st::math::Point{-999.0f, -999.0f}), kNoNode);
}

// ————————————————————————————————————————————————————————————————————————————
// 组件
// ————————————————————————————————————————————————————————————————————————————

/// `set_source` / `source` 往返：缩进 → 层级。
ST_TEST(mindmap_view_source_roundtrip) {
  MindMapView view;
  view.set_source("根\n  一级 A\n    二级 A1\n  一级 B\n");
  ST_CHECK_EQ(view.document().node_count(), 4U);
  ST_CHECK_EQ(view.document().max_depth(), 2U);
  // 文字投影应当保留层级缩进
  const std::string projected = view.source();
  ST_CHECK(projected.find("根") != std::string::npos);
  ST_CHECK(projected.find("  一级 A") != std::string::npos);
  ST_CHECK(projected.find("    二级 A1") != std::string::npos);
}

/// 编辑动作都进撤销栈；撤销/重做把树换回去。
ST_TEST(mindmap_view_undo_redo) {
  MindMapView view;
  view.set_source("根\n  A\n  B\n");
  const std::size_t before = view.document().node_count();
  ST_CHECK(!view.can_undo());

  view.set_selected(view.document().node(0)->children[0]);
  ST_CHECK(view.invoke_action("add_child", ""));
  ST_CHECK_EQ(view.document().node_count(), before + 1U);
  ST_CHECK(view.can_undo());
  ST_CHECK(view.dirty());

  ST_CHECK(view.undo());
  ST_CHECK_EQ(view.document().node_count(), before);
  ST_CHECK(view.can_redo());
  ST_CHECK(view.redo());
  ST_CHECK_EQ(view.document().node_count(), before + 1U);
}

/// 改名：动作面传文字直接改；只读时拒绝。
ST_TEST(mindmap_view_rename_and_readonly) {
  MindMapView view;
  view.set_source("根\n  A\n");
  view.set_selected(1);
  ST_CHECK(view.invoke_action("rename", "改过的名字"));
  ST_CHECK_EQ(view.document().text_of(1), std::string("改过的名字"));
  ST_CHECK_EQ(view.get_property("selected_text").value_or(""), std::string("改过的名字"));

  view.set_editable(false);
  ST_CHECK(!view.invoke_action("rename", "再改一次"));
  ST_CHECK_EQ(view.document().text_of(1), std::string("改过的名字"));
  ST_CHECK(!view.invoke_action("add_child", ""));
  ST_CHECK(!view.begin_rename(1));
}

/// 属性面自洽：声明的可写名都读得动；只读名写不进去；未知名两头都拒绝。
ST_TEST(mindmap_view_property_surface) {
  MindMapView view;
  view.set_source("根\n  A\n  B\n");
  for (const std::string_view name : view.property_names()) {
    (void)view.get_property(name);   // 不崩即可
  }
  // 只读名必须**拒绝写**、且读得动。
  //
  // ⚠ 不断言"一定有值"：基类有几个只读诊断名（`animation`/`animation_opacity`/
  // `transition`）在未设置时按设计返回 `nullopt`（"此刻无值"本身是信息）。
  // 本组件自己的只读项则恒有值——那才是"已接通"的证据。
  const std::vector<std::string_view> own_readonly{
      "node_count", "visible_count", "depth", "selected_text", "error",
      "content_width", "content_height", "scale", "dragging", "editing",
      "can_undo", "can_redo", "dirty"};
  for (const std::string_view name : view.readonly_property_names()) {
    ST_CHECK(!view.set_property(name, "1"));
    (void)view.get_property(name);
  }
  for (const std::string_view name : own_readonly) {
    ST_CHECK(view.get_property(name).has_value());
  }
  ST_CHECK(!view.set_property("__no_such_property__", "x"));
  ST_CHECK(!view.get_property("__no_such_property__").has_value());
  ST_CHECK(!view.invoke_action("__no_such_action__", ""));

  // 可写项真的生效（不是"声明了但落不上"）
  ST_CHECK(view.set_property("direction", "both"));
  ST_CHECK(view.direction() == st::md::mindmap::LayoutDirection::Both);
  ST_CHECK(!view.set_property("direction", "diagonal"));   // 非法值如实拒绝
  ST_CHECK(view.set_property("selected", "1"));
  ST_CHECK_EQ(view.selected(), 1U);
  ST_CHECK(view.set_property("show_collapse_badges", "false"));
  ST_CHECK(!view.show_collapse_badges());
}

/// 键盘：Tab 遍历可见节点、方向键按几何走、Space 折叠、Delete 删子树。
ST_TEST(mindmap_view_keyboard) {
  Fixture fixture;
  MindMapView view;
  view.set_source("根\n  A\n    A1\n  B\n");
  fixture.layout(view, 600.0f, 400.0f);
  view.set_selected(view.document().root());

  // Tab → 下一个可见节点（先序：根 → A）
  st::ui::Event tab{};
  tab.kind = st::ui::EventKind::KeyDown;
  tab.key = "Tab";
  ST_CHECK(view.on_event(fixture.context, tab));
  ST_CHECK_EQ(view.selected(), 1U);

  // 方向键向右 → 它的第一个子节点
  st::ui::Event right{};
  right.kind = st::ui::EventKind::KeyDown;
  right.key = "ArrowRight";
  ST_CHECK(view.on_event(fixture.context, right));
  ST_CHECK_EQ(view.selected(), 2U);   // A1

  // 方向键向左 → 回到 A（父节点在左）
  st::ui::Event left{};
  left.kind = st::ui::EventKind::KeyDown;
  left.key = "ArrowLeft";
  ST_CHECK(view.on_event(fixture.context, left));
  ST_CHECK_EQ(view.selected(), 1U);   // A

  // Space → 折叠 A（它有子节点）
  st::ui::Event space{};
  space.kind = st::ui::EventKind::KeyDown;
  space.key = " ";
  ST_CHECK(view.on_event(fixture.context, space));
  ST_CHECK(view.document().node(1)->collapsed);

  // Ctrl+Z → 撤销折叠
  st::ui::Event undo_key{};
  undo_key.kind = st::ui::EventKind::KeyDown;
  undo_key.key = "z";
  undo_key.ctrl = true;
  ST_CHECK(view.on_event(fixture.context, undo_key));
  ST_CHECK(!view.document().node(1)->collapsed);
}

/// 鼠标：点选中、点徽标折叠、双击进改名、拖到另一节点改父级。
ST_TEST(mindmap_view_mouse) {
  Fixture fixture;
  MindMapView view;
  view.set_source("根\n  A\n  B\n");
  fixture.layout(view, 600.0f, 400.0f);

  const std::size_t branch_a = 1;
  const std::size_t branch_b = 2;
  const st::math::Rect a_rect = view.node_rect(branch_a);
  ST_REQUIRE(!a_rect.is_empty());

  // 单击选中
  st::ui::Event down{};
  down.kind = st::ui::EventKind::MouseDown;
  down.position = a_rect.center();
  down.button = 1;
  ST_CHECK(view.on_event(fixture.context, down));
  ST_CHECK_EQ(view.selected(), branch_a);
  st::ui::Event up{};
  up.kind = st::ui::EventKind::MouseUp;
  up.position = a_rect.center();
  up.button = 1;
  (void)view.on_event(fixture.context, up);

  // 双击 → 进改名（内嵌输入框被挂上）
  st::ui::Event dbl{};
  dbl.kind = st::ui::EventKind::DoubleClick;
  dbl.position = a_rect.center();
  dbl.button = 1;
  ST_CHECK(view.on_event(fixture.context, dbl));
  ST_CHECK(view.is_renaming());
  ST_CHECK_EQ(view.renaming_index(), branch_a);
  // 提交
  ST_CHECK(view.commit_rename(std::string("改过了")));
  ST_CHECK(!view.is_renaming());
  ST_CHECK_EQ(view.document().text_of(branch_a), std::string("改过了"));

  // 拖 A 到 B → 改父级
  fixture.layout(view, 600.0f, 400.0f);
  const st::math::Rect a2 = view.node_rect(branch_a);
  const st::math::Rect b2 = view.node_rect(branch_b);
  ST_REQUIRE(!a2.is_empty());
  ST_REQUIRE(!b2.is_empty());
  st::ui::Event drag_down{};
  drag_down.kind = st::ui::EventKind::MouseDown;
  drag_down.position = a2.center();
  drag_down.button = 1;
  (void)view.on_event(fixture.context, drag_down);
  st::ui::Event drag_move{};
  drag_move.kind = st::ui::EventKind::MouseMove;
  drag_move.position = b2.center();
  (void)view.on_event(fixture.context, drag_move);
  ST_CHECK(view.dragging());
  st::ui::Event drag_up{};
  drag_up.kind = st::ui::EventKind::MouseUp;
  drag_up.position = b2.center();
  drag_up.button = 1;
  (void)view.on_event(fixture.context, drag_up);
  ST_CHECK_EQ(view.document().node(branch_a)->parent, branch_b);
  ST_CHECK(view.can_undo());
}

/// 像素冒烟：真的画出东西（不是一片背景），且内容随「换文档」变化。
ST_TEST(mindmap_view_paints_pixels) {
  Fixture fixture;
  MindMapView view;
  view.set_source("中心\n  分支一\n  分支二\n");
  fixture.layout(view, 500.0f, 300.0f);

  const auto background = fixture.theme.colors().bg;
  st::raster::Canvas canvas(500, 300);
  canvas.clear(background);
  fixture.paint(view, canvas);

  // 与**背景色差**归一量：纯背景是 0。为何不直接判非零像素：背景本身不是纯黑，
  // 判 `!= 0` 会把整张背景算成"画了东西"（恒绿的假断言）。
  const auto inked = [&](st::raster::Canvas& target) {
    std::size_t count = 0;
    for (int y = 0; y < 300; ++y) {
      for (int x = 0; x < 500; ++x) {
        const auto pixel = target.pixel_at(x, y);
        const int delta = std::abs(static_cast<int>(pixel.r) - static_cast<int>(background.r)) +
                          std::abs(static_cast<int>(pixel.g) - static_cast<int>(background.g)) +
                          std::abs(static_cast<int>(pixel.b) - static_cast<int>(background.b));
        if (delta > 24) ++count;
      }
    }
    return count;
  };
  // 节点框 + 连线 + 文字的量级；画不出来会是个位数。
  ST_CHECK(inked(canvas) > 1500U);

  // 换文档 → 画面必须变（不是"改了数据但没重画"）。
  view.set_source("完全不同的主题\n  只有一个分支\n");
  fixture.layout(view, 500.0f, 300.0f);
  st::raster::Canvas canvas2(500, 300);
  canvas2.clear(background);
  fixture.paint(view, canvas2);
  std::size_t differing = 0;
  for (int y = 0; y < 300; ++y) {
    for (int x = 0; x < 500; ++x) {
      const auto a = canvas.pixel_at(x, y);
      const auto b = canvas2.pixel_at(x, y);
      if (a.r != b.r || a.g != b.g || a.b != b.b) ++differing;
    }
  }
  ST_CHECK(differing > 500U);
}

/// 分支着色：**不同一级分支必须不同色**（按深度着色会让同层全同色）。
///
/// 为何值得一条像素用例：这个缺陷实测出现过——按深度着色时截图里"框架内核"与
/// "应用生态"都是绿色，属性面全对、截图**能看出来**，但没有断言就只能靠人看。
/// 颜色是思维导图里表达"哪些节点同一支"的唯一手段，不容退化。
ST_TEST(mindmap_view_branch_colors_differ_per_branch) {
  Fixture fixture;
  MindMapView view;
  view.set_source("根\n  分支一\n  分支二\n  分支三\n");
  fixture.layout(view, 700.0f, 400.0f);

  const auto background = fixture.theme.colors().bg;
  st::raster::Canvas canvas(700, 400);
  canvas.clear(background);
  fixture.paint(view, canvas);

  // 取三个一级分支**左边框上**的像素（一级分支用强调色描边，颜色差异最明显）。
  const auto border_color = [&](std::size_t index) {
    const st::math::Rect rect = view.node_rect(index);
    if (rect.is_empty()) return st::math::Color{0, 0, 0, 0};
    const int x = static_cast<int>(rect.x + 1.0f);
    const int y = static_cast<int>(rect.center().y);
    return canvas.pixel_at(x, y);
  };
  const auto b1 = border_color(1);
  const auto b2 = border_color(2);
  const auto b3 = border_color(3);

  // 为了不被"三个都是背景色"这种退化情形蒙混过关：先确认描边真的画了（与背景不同）。
  const auto differs_from_bg = [&](st::math::Color c) {
    return c.r != background.r || c.g != background.g || c.b != background.b;
  };
  ST_CHECK(differs_from_bg(b1));
  ST_CHECK(differs_from_bg(b2));
  ST_CHECK(differs_from_bg(b3));

  // 三个分支必须**两两不同色**（同一支下的后代同色，那是另一回事）。
  ST_CHECK(b1.r != b2.r || b1.g != b2.g || b1.b != b2.b);
  ST_CHECK(b2.r != b3.r || b2.g != b3.g || b2.b != b3.b);
  ST_CHECK(b1.r != b3.r || b1.g != b3.g || b1.b != b3.b);
}

/// 经 `on_submit` 提交改名不能把正在派发的输入框当场销毁。
///
/// ## 为何必须单独一条（这条对应一次真崩溃）
///
/// 上面的生命周期用例是**直接调** `commit_rename`，不在派发栈里——所以它全程是绿的。
/// 而真实路径是：`Input::activate()` → `on_submit` → `commit_rename` → 摘除输入框。
/// 若摘除后立刻 `reset()`，`activate()` 就在返回途中用已释放的 `this`；
/// 实测表现是从控制通道 `invoke submit` 后**整个应用退出**（无异常信息）。
///
/// 本用例模拟那条路径：拿 `Input*`、从**它自己的动作入口**提交，然后**继续访问本组件**——
/// 若实现当场销毁它，这里就是 use-after-free（debug 档 libstdc++ 会直接 abort）。
ST_TEST(mindmap_view_submit_from_input_is_safe) {
  Fixture fixture;
  MindMapView view;
  view.set_source("根\n  A\n");
  fixture.layout(view, 500.0f, 300.0f);

  ST_REQUIRE(view.begin_rename(1));
  ST_REQUIRE(view.child_count() == 1U);
  ST_CHECK_EQ(view.retired_input_count(), 0U);

  st::ui::Element* field = view.child_at(0);
  ST_REQUIRE(field != nullptr);
  ST_CHECK(field->set_property("value", "经 submit 改的名"));
  ST_CHECK(field->invoke_action("submit", ""));

  // 提交后：树改了、改名态结束、子元素摘干。
  ST_CHECK_EQ(view.document().text_of(1), std::string("经 submit 改的名"));
  ST_CHECK(!view.is_renaming());
  ST_CHECK_EQ(view.child_count(), 0U);

  // **关键时序**：提交那一瞬它必须是「已摘下、还押着」的。
  //
  // 这一条是这条用例真正的价值所在："不要当场销毁自己"没法用指针有无来测
  //（debug 档下读悬垂指针不一定崩——实测回退成立即 reset 后本用例仍绿）。
  // 但时序是确定的：如果实现当场 reset，这里就是 0，而 0 就是那条崩溃的写法。
  ST_CHECK_EQ(view.retired_input_count(), 1U);

  // 下一次事件把它释放掉（否则会一直押着）。
  st::ui::Event tick{};
  tick.kind = st::ui::EventKind::MouseMove;
  tick.position = st::math::Point{-100.0f, -100.0f};
  (void)view.on_event(fixture.context, tick);
  ST_CHECK_EQ(view.retired_input_count(), 0U);

  // 且组件仍可正常使用——这条同时是对“没把自己搞坏”的确认。
  ST_CHECK(view.invoke_action("add_child", ""));
  ST_CHECK(view.node_at(st::math::Point{-999.0f, -999.0f}) == kNoNode);
}

/// fit 与 zoom 是**两个量**：窗口变化自动适配，用户放大不被清掉。
ST_TEST(mindmap_view_fit_and_zoom_are_separate) {
  Fixture fixture;
  MindMapView view;
  view.set_source("中心\n  分支一\n  分支二\n");
  fixture.layout(view, 400.0f, 300.0f);
  const float scale_small = view.effective_scale();

  // 窗口变大 → fit 自动跟上（只缩不放：小图不会被放大）
  fixture.layout(view, 1600.0f, 1200.0f);
  const float scale_big = view.effective_scale();
  ST_CHECK(scale_big >= scale_small);

  // 手动放大后窗口再变，用户倍率仍在（不该被窗口变化清掉）
  view.set_zoom(2.0f);
  ST_CHECK_EQ(view.zoom(), 2.0f);
  fixture.layout(view, 800.0f, 600.0f);
  ST_CHECK_EQ(view.zoom(), 2.0f);
}

/// 内嵌改名框：进改名时挂上、取消/提交时摘干净（不留悬垂子元素）。
ST_TEST(mindmap_view_rename_input_lifecycle) {
  Fixture fixture;
  MindMapView view;
  view.set_source("根\n  A\n");
  fixture.layout(view, 500.0f, 300.0f);

  ST_CHECK(view.begin_rename(1));
  ST_CHECK_EQ(view.child_count(), 1U);
  ST_CHECK(view.is_renaming());

  view.cancel_rename();
  ST_CHECK_EQ(view.child_count(), 0U);
  ST_CHECK(!view.is_renaming());

  // 提交路径也要摘掉
  ST_CHECK(view.begin_rename(1));
  ST_CHECK_EQ(view.child_count(), 1U);
  (void)view.commit_rename(std::string("改名了"));
  ST_CHECK_EQ(view.child_count(), 0U);
  ST_CHECK_EQ(view.document().text_of(1), std::string("改名了"));
}

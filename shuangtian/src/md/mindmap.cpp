#include "st/md/mindmap.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <numeric>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "st/core/json.hpp"
#include "st/core/string.hpp"
#include "st/xml/xml.hpp"

namespace st::md::mindmap {
namespace {

constexpr std::size_t kNoNode = Node::kNoNode;

/// 折行的最后一行省略号（与 `TextPort::ellipsize` 同一字符，避免两套"看起来像省略"）。
constexpr std::string_view kEllipsis{"…"};

}  // namespace

auto to_string(LayoutDirection direction) noexcept -> std::string_view {
  switch (direction) {
    case LayoutDirection::Right: return "right";
    case LayoutDirection::Both: return "both";
  }
  return "right";
}

auto parse_direction(std::string_view name) noexcept -> std::optional<LayoutDirection> {
  if (name == "right" || name == "logical") return LayoutDirection::Right;
  if (name == "both" || name == "mindmap") return LayoutDirection::Both;
  return std::nullopt;
}

// ————————————————————————————————————————————————————————————————————————————
// 树模型
// ————————————————————————————————————————————————————————————————————————————

Document::Document(std::string root_text) {
  root_ = append_node(kNoNode, std::move(root_text));
}

auto Document::empty() -> Document { return Document{}; }

auto Document::append_node(std::size_t parent, std::string text) -> std::size_t {
  Node node;
  node.id = std::format("n{}", nodes_.size());
  node.text = std::move(text);
  node.parent = parent;
  nodes_.push_back(std::move(node));
  const std::size_t index = nodes_.size() - 1U;
  if (parent != kNoNode && parent < nodes_.size()) {
    nodes_[parent].children.push_back(index);
  }
  return index;
}

void Document::detach_child(std::size_t index, std::size_t child) {
  if (index >= nodes_.size()) return;
  auto& children = nodes_[index].children;
  const auto at = std::find(children.begin(), children.end(), child);
  if (at != children.end()) children.erase(at);
}

auto Document::node(std::size_t index) const noexcept -> const Node* {
  return index < nodes_.size() ? &nodes_[index] : nullptr;
}

auto Document::text_of(std::size_t index) const -> std::string {
  const Node* found = node(index);
  return found != nullptr ? found->text : std::string{};
}

auto Document::depth_of(std::size_t index) const noexcept -> std::size_t {
  std::size_t depth = 0;
  std::size_t cursor = index;
  // 上界保护：结构不变量保证无环，但**越界/坏下标**必须能安全退出（本函数 noexcept）。
  std::size_t guard = nodes_.size() + 1U;
  while (cursor != kNoNode && cursor < nodes_.size() && guard > 0U) {
    --guard;
    const std::size_t parent = nodes_[cursor].parent;
    if (parent == kNoNode) break;
    ++depth;
    cursor = parent;
  }
  return depth;
}

auto Document::path_of(std::size_t index) const -> std::vector<std::size_t> {
  std::vector<std::size_t> path;
  std::size_t cursor = index;
  std::size_t guard = nodes_.size() + 1U;
  while (cursor != kNoNode && cursor < nodes_.size() && guard > 0U) {
    --guard;
    path.push_back(cursor);
    cursor = nodes_[cursor].parent;
  }
  std::reverse(path.begin(), path.end());
  return path;
}

auto Document::is_descendant_of(std::size_t index, std::size_t ancestor) const noexcept -> bool {
  std::size_t cursor = index;
  std::size_t guard = nodes_.size() + 1U;
  while (cursor != kNoNode && cursor < nodes_.size() && guard > 0U) {
    --guard;
    if (cursor == ancestor) return true;
    cursor = nodes_[cursor].parent;
  }
  return false;
}

auto Document::subtree_size(std::size_t index) const noexcept -> std::size_t {
  if (index >= nodes_.size()) return 0U;
  std::size_t total = 0;
  // 显式栈（递归深度受用户折叠层级影响，不能假定很浅）。
  std::vector<std::size_t> stack{index};
  while (!stack.empty()) {
    const std::size_t current = stack.back();
    stack.pop_back();
    if (current >= nodes_.size()) continue;
    ++total;
    for (const std::size_t child : nodes_[current].children) stack.push_back(child);
  }
  return total;
}

auto Document::max_depth() const noexcept -> std::size_t {
  std::size_t deepest = 0;
  for (std::size_t index = 0; index < nodes_.size(); ++index) {
    deepest = std::max(deepest, depth_of(index));
  }
  return deepest;
}

auto Document::visible_count() const noexcept -> std::size_t {
  if (root_ == kNoNode || root_ >= nodes_.size()) return 0U;
  std::size_t total = 0;
  std::vector<std::size_t> stack{root_};
  while (!stack.empty()) {
    const std::size_t current = stack.back();
    stack.pop_back();
    if (current >= nodes_.size()) continue;
    ++total;
    if (nodes_[current].collapsed) continue;
    for (const std::size_t child : nodes_[current].children) stack.push_back(child);
  }
  return total;
}

void Document::set_root_text(std::string text) {
  if (root_ == kNoNode || root_ >= nodes_.size()) return;
  nodes_[root_].text = std::move(text);
}

auto Document::set_text(std::size_t index, std::string text) -> bool {
  if (index >= nodes_.size()) return false;
  if (nodes_[index].text == text) return false;
  nodes_[index].text = std::move(text);
  return true;
}

auto Document::set_note(std::size_t index, std::string note) -> bool {
  if (index >= nodes_.size()) return false;
  if (nodes_[index].note == note) return false;
  nodes_[index].note = std::move(note);
  return true;
}

auto Document::set_link(std::size_t index, std::string link) -> bool {
  if (index >= nodes_.size()) return false;
  if (nodes_[index].link == link) return false;
  nodes_[index].link = std::move(link);
  return true;
}

auto Document::restore_id(std::size_t index, std::string id) -> bool {
  if (index >= nodes_.size()) return false;
  nodes_[index].id = std::move(id);
  return true;
}

auto Document::set_color(std::size_t index, std::uint32_t argb) -> bool {
  if (index >= nodes_.size()) return false;
  if (nodes_[index].argb == argb) return false;
  nodes_[index].argb = argb;
  return true;
}

auto Document::set_collapsed(std::size_t index, bool collapsed) -> bool {
  if (index >= nodes_.size()) return false;
  if (nodes_[index].collapsed == collapsed) return false;
  nodes_[index].collapsed = collapsed;
  return true;
}

auto Document::toggle_collapsed(std::size_t index) -> bool {
  if (index >= nodes_.size()) return false;
  if (nodes_[index].children.empty()) return false;  // 没子节点时折叠无意义（不产生空历史条目）
  return set_collapsed(index, !nodes_[index].collapsed);
}

auto Document::add_child(std::size_t parent, std::string text) -> std::size_t {
  if (parent >= nodes_.size()) return kNoNode;
  // 加子节点前先展开父级：否则新节点立刻落在折叠区里，用户看到的是"点了没反应"
  // （这是编辑类组件最常见的一类静默失败）。
  nodes_[parent].collapsed = false;
  return append_node(parent, std::move(text));
}

auto Document::add_sibling(std::size_t index, std::string text) -> std::size_t {
  if (index >= nodes_.size()) return kNoNode;
  const std::size_t parent = nodes_[index].parent;
  if (parent == kNoNode) return add_child(index, std::move(text));
  nodes_[parent].collapsed = false;
  const std::size_t created = append_node(parent, std::move(text));
  // `append_node` 追加到末尾；同级应当紧挨着自己之后——搬一次位置。
  auto& children = nodes_[parent].children;
  const auto at = std::find(children.begin(), children.end(), created);
  if (at != children.end()) {
    children.erase(at);
    const auto self = std::find(children.begin(), children.end(), index);
    children.insert(self == children.end() ? children.end() : self + 1, created);
  }
  return created;
}

auto Document::remove_subtree(std::size_t index) -> std::size_t {
  if (index >= nodes_.size() || index == root_) return kNoNode;

  // ① 收集要删的下标集合（子树）。
  std::vector<std::size_t> doomed;
  std::vector<std::size_t> stack{index};
  while (!stack.empty()) {
    const std::size_t current = stack.back();
    stack.pop_back();
    if (current >= nodes_.size()) continue;
    doomed.push_back(current);
    for (const std::size_t child : nodes_[current].children) stack.push_back(child);
  }
  std::sort(doomed.begin(), doomed.end());

  // ② 先算好"删完该选中谁"：下一个同级，否则父级。
  const std::size_t parent = nodes_[index].parent;
  std::size_t successor = parent;
  if (parent != kNoNode && parent < nodes_.size()) {
    const auto& siblings = nodes_[parent].children;
    const auto self = std::find(siblings.begin(), siblings.end(), index);
    if (self != siblings.end() && self + 1 != siblings.end()) {
      // 下一位是同级（也可能在本子树之外——`self+1` 必然不是本子树成员，
      // 因为子树成员都是 `index` 的后代，不会与它同级）。
      successor = *(self + 1);
    }
  }

  // ③ 从父级的 children 里摘掉自己。
  if (parent != kNoNode && parent < nodes_.size()) detach_child(parent, index);

  // ④ 重建数组：保留未删的节点，同时把下标重映射（父子引用一起改）。
  std::vector<std::size_t> remap(nodes_.size(), kNoNode);
  std::vector<Node> kept;
  kept.reserve(nodes_.size() - doomed.size());
  for (std::size_t old = 0; old < nodes_.size(); ++old) {
    if (std::binary_search(doomed.begin(), doomed.end(), old)) continue;
    remap[old] = kept.size();
    kept.push_back(std::move(nodes_[old]));
  }
  for (Node& node : kept) {
    node.parent = node.parent < remap.size() ? remap[node.parent] : kNoNode;
    std::vector<std::size_t> children;
    children.reserve(node.children.size());
    for (const std::size_t child : node.children) {
      // 被删的后代已从父子链上摘掉（且其父不在集合里时也不会出现在 children 中），
      // 这里再过滤一次是把"结构不变量"变成"删完必然成立"，而不是依赖调用顺序。
      if (child < remap.size() && remap[child] != kNoNode) children.push_back(remap[child]);
    }
    node.children = std::move(children);
  }
  nodes_ = std::move(kept);
  root_ = root_ < remap.size() ? remap[root_] : kNoNode;

  if (successor >= remap.size()) return root_ == kNoNode ? kNoNode : root_;
  const std::size_t mapped = remap[successor];
  return mapped == kNoNode ? root_ : mapped;
}

auto Document::reparent(std::size_t index, std::size_t new_parent) -> bool {
  if (index >= nodes_.size() || new_parent >= nodes_.size()) return false;
  if (index == root_) return false;                       // 根不能有父
  if (nodes_[index].parent == new_parent) return false;   // 原地不动（不产生空历史条目）
  // 成环检查：目标是自己的后代时，挂上去会得到一个与根脱钩的环——
  // 而环的后果比"报错"严重得多（布局会无限递归、序列化会写出自引用）。
  if (is_descendant_of(new_parent, index)) return false;

  const std::size_t old_parent = nodes_[index].parent;
  if (old_parent != kNoNode) detach_child(old_parent, index);
  nodes_[new_parent].collapsed = false;
  nodes_[index].parent = new_parent;
  nodes_[new_parent].children.push_back(index);
  return true;
}

auto Document::move_up(std::size_t index) -> bool {
  if (index >= nodes_.size()) return false;
  const std::size_t parent = nodes_[index].parent;
  if (parent == kNoNode) return false;
  auto& children = nodes_[parent].children;
  const auto self = std::find(children.begin(), children.end(), index);
  if (self == children.end() || self == children.begin()) return false;
  std::iter_swap(self, self - 1);
  return true;
}

auto Document::move_down(std::size_t index) -> bool {
  if (index >= nodes_.size()) return false;
  const std::size_t parent = nodes_[index].parent;
  if (parent == kNoNode) return false;
  auto& children = nodes_[parent].children;
  const auto self = std::find(children.begin(), children.end(), index);
  if (self == children.end() || self + 1 == children.end()) return false;
  std::iter_swap(self, self + 1);
  return true;
}

void Document::reset(Document document) { *this = std::move(document); }

void Document::clear() {
  nodes_.clear();
  root_ = kNoNode;
  meta_ = Meta{};
}

// ————————————————————————————————————————————————————————————————————————————
// 序列化：FreeMind `.mm`
// ————————————————————————————————————————————————————————————————————————————
//
// 文件形状（FreeMind 0.9 原生）：
//
// ```xml
// <map version="1.0.1">
//   <node TEXT="产品路线图" FOLDED="true" COLOR="#ff8800">
//     <richcontent TYPE="NOTE"><html><body><p>备注第一行</p></body></html></richcontent>
//     <node TEXT="Q1">…</node>
//   </node>
// </map>
// ```
//
// 四个容易写错的点（都是 FreeMind 实际文件的形态）：
// ① **备注是 `richcontent` 子元素**，不是一个属性；且可以是 `TYPE="NOTE"`（HTML）
//    或 `TYPE="NODE"`（节点内嵌富文本）。本层只识别 `NOTE`，`NODE` 原样留在
//    `extra`——它是**显示层**的东西，而本组件的节点文字是纯文本。
// ② **折叠是 `FOLDED="true"`**（属性名不是 `COLLAPSED`）；值只认 `true`。
// ③ **颜色是 `#rrggbb`**（六位小写十六进制），而模型存 ARGB。丢掉 alpha 是有意的：
//    `.mm` 没有 alpha 概念，硬塞会得到一个别的工具读不懂的串。
// ④ **`<map>` 下只有一个顶层 `<node>`**（根）。有多个时取第一个并记进诊断，
//    不静默拼成一棵树（那会把两个独立导图错接成一个）。

namespace {

/// 十六进制两位数（小写）。
void append_hex_byte(std::string& out, unsigned value) {
  constexpr std::string_view kDigits{"0123456789abcdef"};
  out.push_back(kDigits[(value >> 4U) & 0x0FU]);
  out.push_back(kDigits[value & 0x0FU]);
}

/// ARGB → `#rrggbb`（丢弃 alpha：`.mm` 没有 alpha 概念）。
[[nodiscard]] auto argb_to_css(std::uint32_t argb) -> std::string {
  std::string out;
  out.reserve(7);
  out.push_back('#');
  append_hex_byte(out, (argb >> 16U) & 0xFFU);
  append_hex_byte(out, (argb >> 8U) & 0xFFU);
  append_hex_byte(out, argb & 0xFFU);
  return out;
}

[[nodiscard]] auto hex_digit(char c) -> int {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/// `#rrggbb` / `#rgb` → ARGB（不透明）。认不出返回 0（= 用默认色，不报错）。
[[nodiscard]] auto css_to_argb(std::string_view text) -> std::uint32_t {
  if (text.empty() || text.front() != '#') return 0U;
  text.remove_prefix(1);
  // `#rgb` 缩写：FreeMind 老文件里会出现，展开成 `#rrggbb`。
  std::string expanded;
  if (text.size() == 3U) {
    for (const char c : text) {
      expanded.push_back(c);
      expanded.push_back(c);
    }
    text = expanded;
  }
  if (text.size() != 6U) return 0U;
  std::uint32_t value = 0U;
  for (const char c : text) {
    const int digit = hex_digit(c);
    if (digit < 0) return 0U;
    value = (value << 4U) | static_cast<std::uint32_t>(digit);
  }
  return 0xFF000000U | value;
}

/// 备注的编码：FreeMind 用 `<richcontent TYPE="NOTE"><html><body><p>…</p>…</body></html></richcontent>`。
///
/// 折行写多个 `<p>`：FreeMind 把每个 `<p>` 当一个段落，而单段落里的 `\n` 会被浏览器
/// 空白折叠吃掉（实测：多行备注读回来变成一行）。
void append_note(std::string& out, std::string_view note, std::string_view indent) {
  if (note.empty()) return;
  out += indent;
  out += "<richcontent TYPE=\"NOTE\"><html><body>";
  std::string_view rest = note;
  while (true) {
    const auto at = rest.find('\n');
    const std::string_view line = at == std::string_view::npos ? rest : rest.substr(0, at);
    out += "<p>";
    out += xml::escape(line);
    out += "</p>";
    if (at == std::string_view::npos) break;
    rest = rest.substr(at + 1);
  }
  out += "</body></html></richcontent>\n";
}

/// 从 `<richcontent TYPE="NOTE">` 里取回备注文本（每个 `<p>` 一段）。
[[nodiscard]] auto note_from_richcontent(const xml::Node& richcontent) -> std::string {
  std::string note;
  // 段落顺序要紧，所以按**文档顺序**递归收集（`children_named` 会丢层级顺序）。
  const auto collect = [&](auto&& self, const xml::Node& node, std::string& out) -> void {
    for (const xml::Node& child : node.children) {
      if (child.name == "p" || child.name == "div") {
        if (!out.empty()) out.push_back('\n');
        out += child.text;
      }
      self(self, child, out);
    }
  };
  collect(collect, richcontent, note);
  if (note.empty()) note = richcontent.text;
  return note;
}

/// 一个节点及其子树的 XML（`indent` 是当前行的前导空白）。
void append_node_xml(const Document& document, std::size_t index, std::string& out,
                     std::string indent, bool pretty) {
  const Node* node = document.node(index);
  if (node == nullptr) return;
  const std::string inner = pretty ? indent + "  " : indent;
  out += indent;
  out += "<node TEXT=\"";
  out += xml::escape(node->text);
  out += "\"";
  if (!node->id.empty()) {
    out += " ID=\"";
    out += xml::escape(node->id);
    out += "\"";
  }
  if (node->collapsed) out += " FOLDED=\"true\"";
  if (node->argb != 0U) {
    out += " COLOR=\"";
    out += argb_to_css(node->argb);
    out += "\"";
  }
  if (!node->link.empty()) {
    out += " LINK=\"";
    out += xml::escape(node->link);
    out += "\"";
  }
  // 其余属性（STYLE / ICON / CREATED / MODIFIED / POSITION…）原样写回：
  // 它们是别人的数据，我们看不懂不等于可以丢。
  for (const auto& [key, value] : node->extra_attributes) {
    if (key == "TEXT" || key == "ID" || key == "FOLDED" || key == "COLOR" || key == "LINK") {
      continue;  // 上面已按模型值写过（模型是这些字段的真值）
    }
    out += " ";
    out += xml::escape(key);
    out += "=\"";
    out += xml::escape(value);
    out += "\"";
  }
  const bool has_body = !node->note.empty() || !node->children.empty();
  if (!has_body) {
    out += "/>\n";
    return;
  }
  out += ">\n";
  append_note(out, node->note, inner);
  for (const std::size_t child : node->children) {
    append_node_xml(document, child, out, inner, pretty);
  }
  out += indent;
  out += "</node>\n";
}

/// 把 `<node>` 的属性表里"模型不认识的"收进 `extra_attributes`。
void collect_extra_attributes(const xml::Node& source, Node& target) {
  for (const auto& [key, value] : source.attributes) {
    if (key == "TEXT" || key == "ID" || key == "FOLDED" || key == "COLOR" || key == "LINK") {
      continue;
    }
    target.extra_attributes.emplace_back(key, value);
  }
}

}  // namespace

auto Document::to_freemind(bool pretty) const -> std::string {
  std::string out;
  out.reserve(256U + nodes_.size() * 64U);
  if (valid() && meta_.version > 0) {
    out += "<map version=\"";
    out += std::to_string(meta_.version);
    out += "\">\n";
  } else {
    // 不写版本头是合法的（FreeMind 自己也是这么读老文件的）。
    out += "<map>\n";
  }
  if (!valid()) {
    out += "</map>\n";
    return out;
  }
  append_node_xml(*this, root_, out, pretty ? "  " : "", pretty);
  out += "</map>\n";
  return out;
}

auto Document::from_freemind(std::string_view text) -> Result<Document> {
  xml::Error xml_error;
  const auto parsed = xml::parse(text, &xml_error);
  if (!parsed.has_value()) {
    return st::unexpected(ErrorCode::Parse,
                          std::format("第 {} 行：{}", xml_error.line, xml_error.message));
  }
  const xml::Node& map = parsed->root;
  if (map.name != "map") {
    return st::unexpected(ErrorCode::Parse,
                          std::format("根元素应为 <map>，实际是 <{}>", map.name));
  }
  const xml::Node* root_node = map.child("node");
  if (root_node == nullptr) return st::unexpected(ErrorCode::Parse, "<map> 下没有 <node>");

  Document document = Document::empty();
  if (const auto version = map.attr("version"); version.has_value()) {
    // `1.0.1` 这类两段式版本号：只取主版本（本层不需要精确复刻它）。
    const std::string_view major = version->substr(0, version->find('.'));
    document.meta_.version = static_cast<std::int32_t>(std::strtol(std::string(major).c_str(),
                                                                  nullptr, 10));
  }

  // 显式栈：`.mm` 的嵌套深度由用户决定，不能假定很浅（递归有爆栈风险）。
  struct Frame {
    const xml::Node* source{nullptr};
    std::size_t created{Node::kNoNode};
  };
  document.root_ = document.append_node(Node::kNoNode,
                                         std::string(root_node->attr_or("TEXT", "")));
  std::vector<Frame> stack;
  stack.push_back(Frame{root_node, document.root_});

  while (!stack.empty()) {
    const Frame frame = stack.back();
    stack.pop_back();
    Node* node = frame.created < document.nodes_.size() ? &document.nodes_[frame.created]
                                                        : nullptr;
    if (node == nullptr || frame.source == nullptr) continue;
    const xml::Node& source = *frame.source;
    (void)document.restore_id(frame.created, std::string(source.attr_or("ID", "")));
    if (const auto link = source.attr("LINK"); link.has_value()) {
      (void)document.set_link(frame.created, std::string(*link));
    }
    node->collapsed = source.attr_bool("FOLDED", false);
    if (const auto color = source.attr("COLOR"); color.has_value()) {
      node->argb = css_to_argb(*color);
    }
    collect_extra_attributes(source, *node);

    for (const xml::Node& child : source.children) {
      if (child.name == "richcontent") {
        // 只有 `TYPE="NOTE"` 是节点备注；`TYPE="NODE"` 是内嵌富文本，
        // 本组件不画它（节点文字是纯文本），所以原样留在 extra 里而不是当备注吃掉。
        if (child.attr_or("TYPE", "") == "NOTE") {
          (void)document.set_note(frame.created, note_from_richcontent(child));
        } else {
          node->extra_attributes.emplace_back("richcontent", std::string(child.text));
        }
        continue;
      }
      if (child.name != "node") continue;
      const std::string child_text(child.attr_or("TEXT", ""));
      const std::size_t created = document.append_node(frame.created, child_text);
      // 注意：`append_node` 把子节点追加到 `node->children` —— 而 `node` 是本轮的指针，
      // `append_node` 内部的 `push_back` 可能**让 `nodes_` 重分配**，于是 `node` 与
      // `children` 引用会一起悬空。所以这里不再使用 `node`/`frame.created` 的指针，
      // 只把它当作下标交给下一帧。
      stack.push_back(Frame{&child, created});
    }
  }
  return document;
}

// ————————————————————————————————————————————————————————————————————————————
// 布局
// ————————————————————————————————————————————————————————————————————————————

auto wrap_text(std::string_view text, float font_size, float max_width, std::size_t max_lines,
               const MeasureText& measure) -> std::vector<std::string> {
  std::vector<std::string> lines;
  if (max_lines == 0U) return lines;
  // 显式换行优先（用户就是要把它们分两行的）。
  std::vector<std::string_view> paragraphs;
  std::string_view rest = text;
  while (true) {
    const auto at = rest.find('\n');
    if (at == std::string_view::npos) {
      paragraphs.push_back(rest);
      break;
    }
    paragraphs.push_back(rest.substr(0, at));
    rest = rest.substr(at + 1);
  }

  const auto width_of = [&](std::string_view candidate) {
    if (measure) return measure(candidate, font_size);
    return font_size * 0.6f * static_cast<float>(st::utf8_length(candidate));
  };

  for (const std::string_view paragraph : paragraphs) {
    if (lines.size() >= max_lines) break;
    if (paragraph.empty()) {
      lines.emplace_back();
      continue;
    }
    std::size_t begin = 0;  // 码点下标
    const std::size_t total = st::utf8_length(paragraph);
    while (begin < total && lines.size() < max_lines) {
      // 贪心：从 `begin` 起尽量多塞，直到超宽；然后回退到最近的空白（若有）。
      std::size_t count = 0;
      std::size_t last_space = 0;
      bool saw_space = false;
      while (begin + count < total) {
        const std::string_view candidate = st::utf8_slice(paragraph, begin, count + 1);
        if (width_of(candidate) > max_width && count > 0) break;
        ++count;
        const std::string_view head = st::utf8_slice(paragraph, begin + count - 1, 1);
        if (head == " " || head == "\t") {
          last_space = count;
          saw_space = true;
        }
      }
      if (count == 0) count = 1;  // 单字超宽也必须前进，否则死循环
      std::size_t take = count;
      if (begin + count < total && saw_space && last_space > 0) take = last_space;
      std::string_view line = st::utf8_slice(paragraph, begin, take);
      // 去掉行尾空白（英文折行会留下一个空格，居中/左对齐都看得见）
      while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
        line.remove_suffix(1);
      }
      lines.emplace_back(line);
      begin += take;
      while (begin < total) {
        const std::string_view head = st::utf8_slice(paragraph, begin, 1);
        if (head != " " && head != "\t") break;
        ++begin;
      }
    }
  }

  if (lines.empty()) lines.emplace_back();
  // 超出上限：末行加省略号。**按宽度继续砍**，别让省略号自己把行撑出去。
  if (lines.size() > max_lines) lines.resize(max_lines);
  const bool truncated = [&] {
    std::size_t consumed = 0;
    for (const std::string& line : lines) consumed += st::utf8_length(line);
    return consumed < st::utf8_length(text);
  }();
  if (truncated) {
    std::string& last = lines.back();
    std::string with_dots = last + std::string(kEllipsis);
    while (!with_dots.empty() && width_of(with_dots) > max_width &&
           st::utf8_length(with_dots) > 1U) {
      const std::size_t keep = st::utf8_length(with_dots) - 2U;
      with_dots = std::string(st::utf8_slice(with_dots, 0, keep)) + std::string(kEllipsis);
    }
    last = std::move(with_dots);
  }
  return lines;
}

namespace {

/// 布局期的中间状态（每个可见节点一份，与 `Document::nodes()` 同序）。
struct Slot {
  bool visible{false};
  float width{0.0f};
  float height{0.0f};
  std::size_t depth{0};
  int side{0};
  float band{0.0f};   ///< 该子树占的纵向高度
  float center_y{0.0f};
  float x{0.0f};
};

auto layout_right(const Document& document, const LayoutOptions& options,
                  const MeasureText& measure, const std::vector<bool>& visible,
                  const std::vector<std::size_t>& depth, std::vector<Slot>& slots,
                  float& total_width, float& total_height) -> void {
  (void)document;
  (void)measure;
  const std::size_t count = slots.size();
  // ① 每层最宽（列对齐的依据）
  std::vector<float> level_width;
  for (std::size_t index = 0; index < count; ++index) {
    if (!visible[index]) continue;
    if (depth[index] >= level_width.size()) level_width.resize(depth[index] + 1U, 0.0f);
    level_width[depth[index]] = std::max(level_width[depth[index]], slots[index].width);
  }
  // ② 每层的左缘
  std::vector<float> level_x(level_width.size(), 0.0f);
  float cursor_x = 0.0f;
  for (std::size_t level = 0; level < level_width.size(); ++level) {
    level_x[level] = cursor_x;
    cursor_x += level_width[level];
    if (level + 1U < level_width.size()) cursor_x += options.level_gap;
  }
  total_width = cursor_x;

  // ③ 纵向：先算每棵子树的"带高"，再按带居中排（父节点在自己带的中心）。
  const auto band_of = [&](auto&& self, std::size_t index) -> float {
    const Node* node = document.node(index);
    if (node == nullptr) return 0.0f;
    float children_total = 0.0f;
    std::size_t visible_children = 0;
    if (!node->collapsed) {
      for (const std::size_t child : node->children) {
        if (!visible[child]) continue;
        children_total += self(self, child);
        ++visible_children;
      }
      if (visible_children > 1U) {
        children_total += options.sibling_gap * static_cast<float>(visible_children - 1U);
      }
    }
    return std::max(slots[index].height, children_total);
  };

  const auto place = [&](auto&& self, std::size_t index, float top) -> void {
    const Node* node = document.node(index);
    if (node == nullptr) return;
    const float band = band_of(band_of, index);
    slots[index].band = band;
    slots[index].x = level_x[depth[index]];
    // 自己在该带里**垂直居中**：父节点就该在两个子节点中间（思维导图的心智模型）。
    slots[index].center_y = top + band * 0.5f;

    float children_total = 0.0f;
    std::size_t visible_children = 0;
    if (!node->collapsed) {
      for (const std::size_t child : node->children) {
        if (!visible[child]) continue;
        children_total += band_of(band_of, child);
        ++visible_children;
      }
      if (visible_children > 1U) {
        children_total += options.sibling_gap * static_cast<float>(visible_children - 1U);
      }
    }
    if (visible_children == 0U) return;
    float child_top = top + (band - children_total) * 0.5f;
    for (const std::size_t child : node->children) {
      if (!visible[child]) continue;
      self(self, child, child_top);
      child_top += band_of(band_of, child) + options.sibling_gap;
    }
  };

  if (!slots.empty() && document.root() < slots.size()) {
    place(place, document.root(), 0.0f);
    total_height = band_of(band_of, document.root());
  } else {
    total_height = 0.0f;
  }
}

auto layout_both(const Document& document, const LayoutOptions& options,
                 const MeasureText& measure, const std::vector<bool>& visible,
                 const std::vector<std::size_t>& seq_depth, std::vector<Slot>& slots,
                 float& total_width, float& total_height) -> void {
  (void)measure;
  const std::size_t count = slots.size();
  // 左右各自的"第几列"（左右是两套列宽：左边那条分支的第一列不该被右边最宽的节点撑开）。
  std::vector<std::size_t> column(count, 0);
  std::vector<float> right_width, left_width;
  const auto ensure = [](std::vector<float>& table, std::size_t level) {
    if (level >= table.size()) table.resize(level + 1U, 0.0f);
  };
  // 根（side=0）单独算；其余按 side 分列。
  float root_width = 0.0f;
  for (std::size_t index = 0; index < count; ++index) {
    if (!visible[index]) continue;
    const int side = slots[index].side;
    if (side == 0) {
      root_width = std::max(root_width, slots[index].width);
      continue;
    }
    const std::size_t level = seq_depth[index] == 0U ? 0U : seq_depth[index] - 1U;
    column[index] = level;
    if (side > 0) {
      ensure(right_width, level);
      right_width[level] = std::max(right_width[level], slots[index].width);
    } else {
      ensure(left_width, level);
      left_width[level] = std::max(left_width[level], slots[index].width);
    }
  }

  float left_total = 0.0f;
  for (const float width : left_width) left_total += width;
  if (!left_width.empty()) left_total += options.level_gap * static_cast<float>(left_width.size());
  float right_total = 0.0f;
  for (const float width : right_width) right_total += width;
  if (!right_width.empty()) {
    right_total += options.level_gap * static_cast<float>(right_width.size());
  }

  // 左右侧各自的列左缘（以"根的中心"为 0 参考，最后统一平移）。
  std::vector<float> right_x(right_width.size(), 0.0f);
  float cursor = root_width * 0.5f + options.level_gap;
  for (std::size_t level = 0; level < right_width.size(); ++level) {
    right_x[level] = cursor;
    cursor += right_width[level] + options.level_gap;
  }
  std::vector<float> left_right_edge(left_width.size(), 0.0f);
  cursor = -root_width * 0.5f - options.level_gap;
  for (std::size_t level = 0; level < left_width.size(); ++level) {
    left_right_edge[level] = cursor;
    cursor -= left_width[level] + options.level_gap;
  }

  for (std::size_t index = 0; index < count; ++index) {
    if (!visible[index]) continue;
    const int side = slots[index].side;
    if (side == 0) {
      slots[index].x = -root_width * 0.5f;
    } else if (side > 0) {
      slots[index].x = right_x[column[index]];
    } else {
      slots[index].x = left_right_edge[column[index]] - slots[index].width;
    }
  }
  total_width = left_total + root_width + right_total;

  // 纵向：左右各排一次，再以根为中心对齐。
  const auto band_of = [&](auto&& self, std::size_t index) -> float {
    const Node* node = document.node(index);
    if (node == nullptr) return 0.0f;
    float children_total = 0.0f;
    std::size_t visible_children = 0;
    if (!node->collapsed) {
      for (const std::size_t child : node->children) {
        // **只算同一侧的兄弟**：左右侧各自成带，混在一起算会把根撑得极高。
        if (!visible[child] || slots[child].side != slots[index].side) continue;
        children_total += self(self, child);
        ++visible_children;
      }
      if (visible_children > 1U) {
        children_total += options.sibling_gap * static_cast<float>(visible_children - 1U);
      }
    }
    return std::max(slots[index].height, children_total);
  };

  const auto place = [&](auto&& self, std::size_t index, float top) -> void {
    const Node* node = document.node(index);
    if (node == nullptr) return;
    const float band = band_of(band_of, index);
    slots[index].band = band;
    slots[index].center_y = top + band * 0.5f;
    float children_total = 0.0f;
    std::size_t visible_children = 0;
    if (!node->collapsed) {
      for (const std::size_t child : node->children) {
        if (!visible[child] || slots[child].side != slots[index].side) continue;
        children_total += band_of(band_of, child);
        ++visible_children;
      }
      if (visible_children > 1U) {
        children_total += options.sibling_gap * static_cast<float>(visible_children - 1U);
      }
    }
    if (visible_children == 0U) return;
    float child_top = top + (band - children_total) * 0.5f;
    for (const std::size_t child : node->children) {
      if (!visible[child] || slots[child].side != slots[index].side) continue;
      self(self, child, child_top);
      child_top += band_of(band_of, child) + options.sibling_gap;
    }
  };

  if (!slots.empty() && document.root() < slots.size()) {
    const std::size_t root = document.root();
    // 根的两侧各排一次（各自从 0 起算），再把整条带以 0 为中心对齐。
    float right_band = 0.0f;
    float left_band = 0.0f;
    std::size_t right_children = 0;
    std::size_t left_children = 0;
    const Node* root_node = document.node(root);
    if (root_node != nullptr && !root_node->collapsed) {
      for (const std::size_t child : root_node->children) {
        if (!visible[child]) continue;
        const float band = band_of(band_of, child);
        if (slots[child].side > 0) {
          right_band += band;
          ++right_children;
        } else {
          left_band += band;
          ++left_children;
        }
      }
      if (right_children > 1U) {
        right_band += options.sibling_gap * static_cast<float>(right_children - 1U);
      }
      if (left_children > 1U) {
        left_band += options.sibling_gap * static_cast<float>(left_children - 1U);
      }
    }
    const float band = std::max({slots[root].height, right_band, left_band});
    slots[root].x = -root_width * 0.5f;
    slots[root].center_y = band * 0.5f;
    slots[root].band = band;

    // 右侧：从带的中心往下排；左侧同理。
    if (root_node != nullptr && !root_node->collapsed) {
      float right_top = (band - right_band) * 0.5f;
      float left_top = (band - left_band) * 0.5f;
      for (const std::size_t child : root_node->children) {
        if (!visible[child]) continue;
        const float child_band = band_of(band_of, child);
        if (slots[child].side > 0) {
          place(place, child, right_top);
          right_top += child_band + options.sibling_gap;
        } else {
          place(place, child, left_top);
          left_top += child_band + options.sibling_gap;
        }
      }
    }
    total_height = band;
  } else {
    total_height = 0.0f;
  }
}

}  // namespace

auto layout(const Document& document, const LayoutOptions& options,
            const MeasureText& measure) -> Layout {
  Layout out;
  if (!document.valid()) {
    out.size = math::Size{0.0f, 0.0f};
    return out;
  }

  const std::size_t count = document.node_count();
  out.boxes.resize(count);
  std::vector<bool> visible(count, false);
  std::vector<std::size_t> depth(count, 0);
  std::vector<Slot> slots(count);

  const float line_height = options.font_size * options.line_gap;
  const float text_max_width = std::max(options.max_node_width - options.pad_x * 2.0f, 24.0f);

  // ① 遍历一次：可见性、深度、绝对层级（`Both` 下的"第几列"。左右两侧各从 1 起）、
  //    节点尺寸与折行。
  struct Frame {
    std::size_t index{0};
    std::size_t depth{0};
    std::size_t seq{0};  ///< `Both` 布局下的"列序号"（根 = 0，一级子节点 = 1）
    int side{0};
    std::size_t child_ordinal{0};
  };
  std::vector<Frame> stack;
  stack.push_back(Frame{document.root(), 0U, 0U, 0, 0U});
  std::vector<std::size_t> seq_depth(count, 0);

  while (!stack.empty()) {
    const Frame frame = stack.back();
    stack.pop_back();
    const Node* node = document.node(frame.index);
    if (node == nullptr) continue;
    visible[frame.index] = true;
    depth[frame.index] = frame.depth;
    seq_depth[frame.index] = frame.seq;
    slots[frame.index].visible = true;
    slots[frame.index].depth = frame.depth;
    slots[frame.index].side = frame.side;

    const std::vector<std::string> lines =
        wrap_text(node->text, options.font_size, text_max_width, 6U, measure);
    float widest = 0.0f;
    for (const std::string& line : lines) {
      const float width = measure ? measure(line, options.font_size)
                                  : options.font_size * 0.6f * static_cast<float>(st::utf8_length(line));
      widest = std::max(widest, width);
    }
    const float box_width = std::clamp(
        widest + options.pad_x * 2.0f, std::min(options.min_node_width, options.max_node_width),
        std::max(options.min_node_width, options.max_node_width));
    const float box_height = line_height * static_cast<float>(lines.size()) + options.pad_y * 2.0f;

    out.boxes[frame.index].lines = lines;
    out.boxes[frame.index].depth = frame.depth;
    out.boxes[frame.index].side = frame.side;
    slots[frame.index].width = box_width;
    slots[frame.index].height = box_height;

    if (node->collapsed && !node->children.empty()) {
      out.boxes[frame.index].has_hidden_children = true;
      out.boxes[frame.index].hidden_children = node->children.size();
    }

    if (node->collapsed) continue;
    // 逆序压栈，保证弹出顺序 = 声明顺序。
    std::size_t ordinal = node->children.size();
    for (auto it = node->children.rbegin(); it != node->children.rend(); ++it) {
      --ordinal;
      const std::size_t child = *it;
      const int side = options.direction == LayoutDirection::Both
                           ? (frame.depth == 0U ? ((ordinal % 2U) == 0U ? 1 : -1) : frame.side)
                           : 1;
      const std::size_t seq = options.direction == LayoutDirection::Both
                                  ? (frame.depth == 0U ? 1U : frame.seq + 1U)
                                  : frame.depth + 1U;
      stack.push_back(Frame{child, frame.depth + 1U, seq, side, ordinal});
    }
  }

  // ② 排位。
  float natural_width = 0.0f;
  float natural_height = 0.0f;
  if (options.direction == LayoutDirection::Both) {
    layout_both(document, options, measure, visible, seq_depth, slots, natural_width,
                natural_height);
  } else {
    layout_right(document, options, measure, visible, depth, slots, natural_width,
                 natural_height);
  }

  // ③ 落框 + 求包围盒（`Both` 会有负数坐标，统一平移到留白内）。
  float min_x = 0.0f;
  float min_y = 0.0f;
  float max_x = 0.0f;
  float max_y = 0.0f;
  bool any = false;
  for (std::size_t index = 0; index < count; ++index) {
    if (!visible[index]) continue;
    const math::Rect rect{slots[index].x, slots[index].center_y - slots[index].height * 0.5f,
                          slots[index].width, slots[index].height};
    out.boxes[index].rect = rect;
    out.boxes[index].text_rect = rect.inset(math::Insets{options.pad_y, options.pad_x,
                                                         options.pad_y, options.pad_x});
    if (!any) {
      min_x = rect.x;
      min_y = rect.y;
      max_x = rect.right();
      max_y = rect.bottom();
      any = true;
    } else {
      min_x = std::min(min_x, rect.x);
      min_y = std::min(min_y, rect.y);
      max_x = std::max(max_x, rect.right());
      max_y = std::max(max_y, rect.bottom());
    }
  }
  if (!any) {
    out.size = math::Size{0.0f, 0.0f};
    return out;
  }
  const float shift_x = options.margin - min_x;
  const float shift_y = options.margin - min_y;
  for (std::size_t index = 0; index < count; ++index) {
    if (!visible[index]) continue;
    out.boxes[index].rect.x += shift_x;
    out.boxes[index].rect.y += shift_y;
    out.boxes[index].text_rect.x += shift_x;
    out.boxes[index].text_rect.y += shift_y;
  }
  out.size = math::Size{(max_x - min_x) + options.margin * 2.0f,
                        (max_y - min_y) + options.margin * 2.0f};

  // ④ 连线：父框朝向子节点那一侧的边中点 → 子框对侧边中点，控制点沿水平方向拉出。
  for (std::size_t index = 0; index < count; ++index) {
    if (!visible[index]) continue;
    const Node* node = document.node(index);
    if (node == nullptr || node->collapsed) continue;
    const math::Rect parent_rect = out.boxes[index].rect;
    for (const std::size_t child : node->children) {
      if (!visible[child]) continue;
      const math::Rect child_rect = out.boxes[child].rect;
      const bool to_right = child_rect.center().x >= parent_rect.center().x;
      LinkGeometry link;
      link.parent = index;
      link.child = child;
      link.from = math::Point{to_right ? parent_rect.right() : parent_rect.x,
                              parent_rect.center().y};
      link.to = math::Point{to_right ? child_rect.x : child_rect.right(), child_rect.center().y};
      const float dx = link.to.x - link.from.x;
      // 水平拉出 45%：太小会贴着框走成直角、太大则两条线在中间拧成一团。
      link.control1 = math::Point{link.from.x + dx * 0.45f, link.from.y};
      link.control2 = math::Point{link.to.x - dx * 0.45f, link.to.y};
      out.links.push_back(link);
    }
  }
  (void)natural_width;
  (void)natural_height;
  return out;
}

auto hit_test(const Layout& layout_value, math::Point point) -> std::size_t {
  for (std::size_t index = 0; index < layout_value.boxes.size(); ++index) {
    const math::Rect& rect = layout_value.boxes[index].rect;
    if (rect.is_empty()) continue;
    if (rect.contains(point)) return index;
  }
  return Node::kNoNode;
}

}  // namespace st::md::mindmap

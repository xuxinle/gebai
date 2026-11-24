#include "st/md/mermaid.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "st/core/string.hpp"

namespace st::md::mermaid {
namespace {

constexpr double kPi = std::numbers::pi_v<double>;

/// 诊断去重：同一句不重复刷屏（一张图里几十个 `subgraph` 只报一次）。
void note(std::vector<std::string>& diagnostics, std::string message) {
  if (std::ranges::find(diagnostics, message) != diagnostics.end()) return;
  diagnostics.push_back(std::move(message));
}

[[nodiscard]] auto is_word_char(char c) noexcept -> bool {
  const auto byte = static_cast<unsigned char>(c);
  return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'z') ||
         (byte >= 'A' && byte <= 'Z') || byte == '_';
}

[[nodiscard]] auto starts_with(std::string_view text, std::string_view prefix) noexcept -> bool {
  return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

/// 去掉包裹的引号（mermaid 允许 `A["文字"]`，引号不是内容）。
[[nodiscard]] auto strip_quotes(std::string_view text) -> std::string {
  if (text.size() >= 2U && text.front() == '"' && text.back() == '"') {
    return std::string(text.substr(1, text.size() - 2U));
  }
  return std::string(text);
}

// ── 节点 / 连线的词法 ────────────────────────────────────────────────────────

/// 形状括号表：**两字符开括号必须排在单字符之前**（`((` 先于 `(`，`([` 先于 `(`）。
struct ShapeSyntax {
  std::string_view open{};
  std::string_view close{};
  NodeShape shape{NodeShape::Rect};
  /// 语法认得、但 v1 不区分的形状（子程序/圆柱/六边形…）：按矩形画 + 上报一次。
  bool supported{true};
};

constexpr ShapeSyntax kShapeSyntax[] = {
    {"((", "))", NodeShape::Circle, true},
    {"([", "])", NodeShape::Stadium, true},
    {"[[", "]]", NodeShape::Rect, false},
    {"[(", ")]", NodeShape::Rect, false},
    {"{{", "}}", NodeShape::Rect, false},
    {"[/", "/]", NodeShape::Rect, false},
    {"[\\", "\\]", NodeShape::Rect, false},
    {"[", "]", NodeShape::Rect, true},
    {"(", ")", NodeShape::Rounded, true},
    {"{", "}", NodeShape::Diamond, true},
};

struct ParsedNode {
  std::string id{};
  std::string label{};
  NodeShape shape{NodeShape::Rect};
  bool has_label{false};
  std::string_view unsupported_shape{};
  std::size_t consumed{0};
};

/// 从 `pos` 起读一个节点引用（`id` + 可选形状）。读不出来返回 `nullopt`。
[[nodiscard]] auto parse_node(std::string_view line, std::size_t pos) -> std::optional<ParsedNode> {
  std::size_t cursor = pos;
  while (cursor < line.size() && line[cursor] == ' ') ++cursor;
  const std::size_t id_begin = cursor;
  while (cursor < line.size() && is_word_char(line[cursor])) ++cursor;
  if (cursor == id_begin) return std::nullopt;

  ParsedNode node;
  node.id = std::string(line.substr(id_begin, cursor - id_begin));
  node.label = node.id;
  node.consumed = cursor - pos;

  auto handle_shape = [&](const ShapeSyntax& syntax) {
    const std::string_view rest = line.substr(cursor);
    if (!starts_with(rest, syntax.open)) return false;
    const std::size_t text_begin = cursor + syntax.open.size();
    const std::size_t close = line.find(syntax.close, text_begin);
    if (close == std::string_view::npos) {
      // 未闭合：把剩下全当标签（与 Markdown 解析的宽容口径一致）。
      node.label = strip_quotes(st::trim(line.substr(text_begin)));
      node.consumed = line.size() - pos;
    } else {
      node.label = strip_quotes(st::trim(line.substr(text_begin, close - text_begin)));
      node.consumed = close + syntax.close.size() - pos;
    }
    node.shape = syntax.shape;
    node.has_label = true;
    if (!syntax.supported) node.unsupported_shape = syntax.open;
    return true;
  };

  for (const ShapeSyntax& syntax : kShapeSyntax) {
    if (handle_shape(syntax)) break;
  }

  // `:::className` 样式后缀：v1 不实现样式类，但**必须吃掉**，否则它会被当成节点 id。
  if (line.substr(pos + node.consumed, 3) == ":::") {
    std::size_t cursor2 = pos + node.consumed + 3;
    while (cursor2 < line.size() && is_word_char(line[cursor2])) ++cursor2;
    node.consumed = cursor2 - pos;
  }
  return node;
}

/// 一次连线匹配（线型 + 箭头 + 中缀标签）。
struct LinkMatch {
  EdgeStyle style{EdgeStyle::Solid};
  bool arrow{true};
  std::string label{};
  std::size_t length{0};
};

constexpr struct {
  std::string_view token;
  EdgeStyle style;
  bool arrow;
} kDirectLinks[] = {
    {"-.->", EdgeStyle::Dashed, true},
    {"-.-", EdgeStyle::Dashed, false},
    {"-->", EdgeStyle::Solid, true},
    {"--o", EdgeStyle::Solid, true},
    {"--x", EdgeStyle::Solid, true},
    {"---", EdgeStyle::Solid, false},
    {"==>", EdgeStyle::Thick, true},
    {"===", EdgeStyle::Thick, false},
};

constexpr struct {
  std::string_view open;
  std::string_view close;
  EdgeStyle style;
} kMidLabelLinks[] = {
    {"--", "-->", EdgeStyle::Solid},
    {"==", "==>", EdgeStyle::Thick},
    {"-.", ".->", EdgeStyle::Dashed},
};

/// 在 `pos` 处匹配连线记号。顺序很要紧：`-.->` 必须先于 `-.-`（否则后者吃掉前者）。
[[nodiscard]] auto match_link(std::string_view line, std::size_t pos) -> std::optional<LinkMatch> {
  const std::string_view rest = line.substr(pos);
  for (const auto& direct : kDirectLinks) {
    if (!starts_with(rest, direct.token)) continue;
    LinkMatch match;
    match.style = direct.style;
    match.arrow = direct.arrow;
    match.length = direct.token.size();
    return match;
  }
  // 中缀标签写法：`A -- 文本 --> B` / `A == 文本 ==> B` / `A -. 文本 .-> B`。
  for (const auto& mid : kMidLabelLinks) {
    if (!starts_with(rest, mid.open)) continue;
    const std::size_t close = rest.find(mid.close, mid.open.size());
    if (close == std::string_view::npos) continue;
    LinkMatch match;
    match.style = mid.style;
    match.arrow = true;
    match.label = std::string(st::trim(rest.substr(mid.open.size(), close - mid.open.size())));
    match.length = close + mid.close.size();
    return match;
  }
  return std::nullopt;
}

// ── 流程图语句 ────────────────────────────────────────────────────────────────

[[nodiscard]] auto ensure_node(Flowchart& chart, const ParsedNode& parsed) -> std::size_t {
  for (std::size_t index = 0; index < chart.nodes.size(); ++index) {
    if (chart.nodes[index].id != parsed.id) continue;
    // 后出现的显式形状/标签补上（`A --> B[结果]` 这种"先引用后定义"的写法）。
    if (parsed.has_label) {
      chart.nodes[index].label = parsed.label;
      chart.nodes[index].shape = parsed.shape;
    }
    return index;
  }
  FlowNode node;
  node.id = parsed.id;
  node.label = parsed.label;
  node.shape = parsed.shape;
  chart.nodes.push_back(std::move(node));
  return chart.nodes.size() - 1U;
}

/// 解析一行流程图语句（可含链式 `A --> B --> C`）。返回 false = 这一行没法解析。
auto parse_flow_statement(std::string_view line, Flowchart& chart,
                          std::vector<std::string>& diagnostics) -> bool {
  const auto first = parse_node(line, 0);
  if (!first.has_value()) {
    note(diagnostics, "无法解析的语句：" + std::string(st::trim(line)));
    return false;
  }
  if (!first->unsupported_shape.empty()) {
    note(diagnostics, "形状按矩形绘制：`" + std::string(first->unsupported_shape) + "`");
  }
  std::size_t current = ensure_node(chart, *first);
  std::size_t pos = first->consumed;

  while (true) {
    while (pos < line.size() && line[pos] == ' ') ++pos;
    if (pos >= line.size()) return true;
    if (line[pos] == '&') {
      note(diagnostics, "暂不支持并列节点语法 `&`");
      return true;
    }
    const auto link = match_link(line, pos);
    if (!link.has_value()) {
      note(diagnostics, "无法解析的连接符：" + std::string(st::trim(line.substr(pos))));
      return true;
    }
    pos += link->length;
    std::string label = link->label;
    while (pos < line.size() && line[pos] == ' ') ++pos;
    if (pos < line.size() && line[pos] == '|') {
      const std::size_t close = line.find('|', pos + 1U);
      if (close == std::string_view::npos) {
        note(diagnostics, "边标签未闭合：" + std::string(st::trim(line)));
        return true;
      }
      label = std::string(st::trim(line.substr(pos + 1U, close - pos - 1U)));
      pos = close + 1U;
      while (pos < line.size() && line[pos] == ' ') ++pos;
    }
    const auto next = parse_node(line, pos);
    if (!next.has_value()) {
      note(diagnostics, "连接符后缺少节点：" + std::string(st::trim(line)));
      return true;
    }
    if (!next->unsupported_shape.empty()) {
      note(diagnostics, "形状按矩形绘制：`" + std::string(next->unsupported_shape) + "`");
    }
    pos += next->consumed;
    const std::size_t target = ensure_node(chart, *next);
    FlowEdge edge;
    edge.from = current;
    edge.to = target;
    edge.label = std::move(label);
    edge.style = link->style;
    edge.arrow = link->arrow;
    edge.self = current == target;
    chart.edges.push_back(std::move(edge));
    current = target;
  }
}

// ── 三类图的解析 ──────────────────────────────────────────────────────────────

[[nodiscard]] auto parse_direction(std::string_view token) -> std::optional<Direction> {
  if (token == "TD" || token == "TB") return Direction::TopDown;
  if (token == "LR") return Direction::LeftRight;
  if (token == "RL") return Direction::RightLeft;
  if (token == "BT") return Direction::BottomUp;
  return std::nullopt;
}

void parse_flowchart_lines(const std::vector<std::string_view>& lines, std::size_t first,
                           Document& document) {
  Flowchart& chart = document.flowchart;
  for (std::size_t index = first; index < lines.size(); ++index) {
    const std::string_view line = st::trim(lines[index]);
    if (line.empty()) continue;
    if (starts_with(line, "%%")) continue;
    if (starts_with(line, "subgraph")) {
      note(document.diagnostics, "暂不支持子图 `subgraph`（内容按普通节点画）");
      continue;
    }
    if (line == "end") continue;
    if (starts_with(line, "style ") || starts_with(line, "classDef ") ||
        starts_with(line, "class ") || starts_with(line, "linkStyle ") ||
        starts_with(line, "click ") || starts_with(line, "direction ")) {
      note(document.diagnostics, "暂不支持样式/跳转指令：`" + std::string(st::split_whitespace(line).front()) + "`");
      continue;
    }
    (void)parse_flow_statement(line, chart, document.diagnostics);
  }
  document.ok = !chart.nodes.empty();
}

/// 时序图的箭头记号（长优先：`-->>` 必须先于 `-->`）。
constexpr struct {
  std::string_view token;
  bool dashed;
  bool arrow;
} kSequenceArrows[] = {
    {"-->>", true, true}, {"->>", false, true}, {"-->", true, true},
    {"-)", false, true},  {"--)", true, true},  {"--x", true, true},
    {"->", false, true},  {"-x", false, true},
};

/// 找最靠前的箭头记号（同位置取最长的那个）。
[[nodiscard]] auto find_sequence_arrow(std::string_view line)
    -> std::optional<std::pair<std::size_t, std::size_t>> {
  std::size_t best_pos = std::string_view::npos;
  std::size_t best_index = 0;
  for (std::size_t index = 0; index < std::size(kSequenceArrows); ++index) {
    const std::size_t at = line.find(kSequenceArrows[index].token);
    if (at == std::string_view::npos) continue;
    if (best_pos == std::string_view::npos || at < best_pos ||
        (at == best_pos && kSequenceArrows[index].token.size() >
                               kSequenceArrows[best_index].token.size())) {
      best_pos = at;
      best_index = index;
    }
  }
  if (best_pos == std::string_view::npos) return std::nullopt;
  return std::pair<std::size_t, std::size_t>{best_pos, best_index};
}

[[nodiscard]] auto ensure_participant(Sequence& sequence, std::string_view id) -> std::size_t {
  for (std::size_t index = 0; index < sequence.participants.size(); ++index) {
    if (sequence.participants[index].id == id) return index;
  }
  SequenceParticipant participant;
  participant.id = std::string(id);
  participant.label = std::string(id);
  sequence.participants.push_back(std::move(participant));
  return sequence.participants.size() - 1U;
}

void parse_sequence_lines(const std::vector<std::string_view>& lines, std::size_t first,
                          Document& document) {
  Sequence& sequence = document.sequence;
  constexpr std::string_view kBlockKeywords[] = {"loop", "alt", "opt", "else", "par", "and",
                                                 "rect", "critical", "break", "end", "activate",
                                                 "deactivate", "Note", "note"};
  for (std::size_t index = first; index < lines.size(); ++index) {
    const std::string_view line = st::trim(lines[index]);
    if (line.empty() || starts_with(line, "%%")) continue;
    if (line == "autonumber") continue;  // 本组件恒显示序号，声明与否等价
    if (starts_with(line, "participant ") || starts_with(line, "actor ")) {
      const bool actor = starts_with(line, "actor ");
      const std::string_view rest = st::trim(line.substr(actor ? 5U : 11U));
      const std::size_t as = rest.find(" as ");
      const std::string_view id = as == std::string_view::npos ? rest : rest.substr(0, as);
      const std::size_t at = ensure_participant(sequence, st::trim(id));
      if (as != std::string_view::npos) {
        sequence.participants[at].label = std::string(st::trim(rest.substr(as + 4U)));
      }
      sequence.participants[at].actor = actor;
      continue;
    }
    bool block = false;
    for (const std::string_view keyword : kBlockKeywords) {
      if (!starts_with(line, keyword)) continue;
      if (line.size() > keyword.size() && line[keyword.size()] != ' ') continue;
      block = true;
      break;
    }
    if (block) {
      note(document.diagnostics, "暂不支持时序图块语句（loop/alt/activate/Note 等）");
      continue;
    }
    const auto arrow = find_sequence_arrow(line);
    if (!arrow.has_value()) {
      note(document.diagnostics, "无法解析的时序语句：" + std::string(line));
      continue;
    }
    const auto& syntax = kSequenceArrows[arrow->second];
    const std::string_view head = line.substr(0, arrow->first);
    const std::string_view tail = line.substr(arrow->first + syntax.token.size());
    const std::size_t colon = tail.find(':');
    const std::string_view to_id = st::trim(colon == std::string_view::npos ? tail
                                                                          : tail.substr(0, colon));
    const std::string_view text = colon == std::string_view::npos ? std::string_view{}
                                                                 : st::trim(tail.substr(colon + 1U));
    if (st::trim(head).empty() || to_id.empty()) {
      note(document.diagnostics, "时序语句缺少参与者：" + std::string(line));
      continue;
    }
    SequenceMessage message;
    message.from = ensure_participant(sequence, st::trim(head));
    message.to = ensure_participant(sequence, to_id);
    message.text = std::string(text);
    message.dashed = syntax.dashed;
    message.arrow = syntax.arrow;
    sequence.messages.push_back(std::move(message));
  }
  document.ok = !sequence.participants.empty();
}

void parse_pie_lines(const std::vector<std::string_view>& lines, std::size_t first,
                     Document& document) {
  Pie& pie = document.pie;
  // 首行的 `title …` / `showData` 可能与前缀同行，也可能在下一行。
  for (std::size_t index = first; index < lines.size(); ++index) {
    const std::string_view line = st::trim(lines[index]);
    if (line.empty() || starts_with(line, "%%")) continue;
    if (starts_with(line, "title")) {
      pie.title = std::string(st::trim(line.substr(5U)));
      continue;
    }
    if (line == "showData" || line == "showData ") continue;
    const std::size_t colon = line.rfind(':');
    if (colon == std::string_view::npos) {
      note(document.diagnostics, "无法解析的饼图行：" + std::string(line));
      continue;
    }
    PieSlice slice;
    slice.label = strip_quotes(st::trim(line.substr(0, colon)));
    const std::string_view value_text = st::trim(line.substr(colon + 1U));
    const auto value = st::parse_f64(value_text);
    if (!value.has_value()) {
      note(document.diagnostics, "饼图数值不是数字：" + std::string(value_text));
      continue;
    }
    slice.value = *value;
    pie.slices.push_back(std::move(slice));
  }
  document.ok = !pie.slices.empty();
}

}  // namespace

auto to_string(DiagramKind kind) noexcept -> std::string_view {
  switch (kind) {
    case DiagramKind::Unknown: return "unknown";
    case DiagramKind::Flowchart: return "flowchart";
    case DiagramKind::Sequence: return "sequence";
    case DiagramKind::Pie: return "pie";
  }
  return "unknown";
}

auto to_string(Direction direction) noexcept -> std::string_view {
  switch (direction) {
    case Direction::TopDown: return "TD";
    case Direction::LeftRight: return "LR";
    case Direction::RightLeft: return "RL";
    case Direction::BottomUp: return "BT";
  }
  return "TD";
}

auto to_string(NodeShape shape) noexcept -> std::string_view {
  switch (shape) {
    case NodeShape::Rect: return "rect";
    case NodeShape::Rounded: return "rounded";
    case NodeShape::Stadium: return "stadium";
    case NodeShape::Diamond: return "diamond";
    case NodeShape::Circle: return "circle";
  }
  return "rect";
}

auto to_string(EdgeStyle style) noexcept -> std::string_view {
  switch (style) {
    case EdgeStyle::Solid: return "solid";
    case EdgeStyle::Dashed: return "dashed";
    case EdgeStyle::Thick: return "thick";
  }
  return "solid";
}

auto split_label_lines(std::string_view label) -> std::vector<std::string> {
  std::vector<std::string> lines;
  std::string current;
  std::size_t cursor = 0;
  auto flush = [&]() {
    lines.push_back(std::string(st::trim(current)));
    current.clear();
  };
  while (cursor < label.size()) {
    const std::string_view rest = label.substr(cursor);
    // 三种换行写法都认（`<br/>` 是 mermaid 的，`\n` 是人手写文本时最自然的）。
    if (starts_with(rest, "<br/>")) {
      flush();
      cursor += 5U;
      continue;
    }
    if (starts_with(rest, "<br>")) {
      flush();
      cursor += 4U;
      continue;
    }
    if (label[cursor] == '\n') {
      flush();
      ++cursor;
      continue;
    }
    current.push_back(label[cursor]);
    ++cursor;
  }
  flush();
  if (lines.size() > 1U && lines.back().empty()) lines.pop_back();
  return lines;
}

auto parse(std::string_view text) -> Document {
  Document document;
  // 统一行尾：`\r\n` 在 Windows 文本里随处可见，不去掉的话每个标签尾部都多一个 `\r`
  //（而 `\r` 会被当成字形画出来，症状是"每个节点右边多一个小方块"）。
  std::string normalized;
  normalized.reserve(text.size());
  for (const char raw : text) {
    if (raw == '\r') continue;
    normalized.push_back(raw);
  }
  std::vector<std::string_view> lines;
  for (const std::string_view piece : st::split(normalized, '\n')) lines.push_back(piece);

  std::size_t first = 0;
  while (first < lines.size()) {
    const std::string_view line = st::trim(lines[first]);
    if (!line.empty() && !starts_with(line, "%%")) break;
    ++first;
  }
  if (first >= lines.size()) {
    note(document.diagnostics, "内容为空");
    return document;
  }

  const std::string_view header = st::trim(lines[first]);
  if (starts_with(header, "flowchart") || starts_with(header, "graph")) {
    document.kind = DiagramKind::Flowchart;
    const auto tokens = st::split_whitespace(header);
    if (tokens.size() >= 2U) {
      if (const auto direction = parse_direction(tokens[1]); direction.has_value()) {
        document.flowchart.direction = *direction;
      } else {
        note(document.diagnostics, "未知流向：" + std::string(tokens[1]));
      }
    }
    parse_flowchart_lines(lines, first + 1U, document);
    return document;
  }
  if (starts_with(header, "sequenceDiagram")) {
    document.kind = DiagramKind::Sequence;
    parse_sequence_lines(lines, first + 1U, document);
    return document;
  }
  if (starts_with(header, "pie")) {
    document.kind = DiagramKind::Pie;
    // 首行残留（`pie title X` / `pie showData`）也要喂给解析：把首个 token 之后的
    // 内容拼回一行，避免"标题写在首行就丢了"。
    std::vector<std::string_view> shifted;
    shifted.reserve(lines.size());
    for (std::size_t index = 0; index < first; ++index) shifted.push_back(lines[index]);
    shifted.emplace_back(st::trim(header.substr(3U)));
    for (std::size_t index = first + 1U; index < lines.size(); ++index) {
      shifted.push_back(lines[index]);
    }
    parse_pie_lines(shifted, first, document);
    return document;
  }

  note(document.diagnostics, "暂不支持的图类型：" + std::string(st::split_whitespace(header).front()));
  return document;
}

// ── 布局 ──────────────────────────────────────────────────────────────────────
//
// 三类图共用一条纪律：**先在"层空间"里算，最后统一映射到屏幕**。
// 层空间只有一个方向（`l` = 层增长方向、`c` = 同层内排列方向），于是四种流向
// （TD/BT/LR/RL）只是同一个算法的四次坐标映射——而不是四份布局代码。
// 四个方向各写一份是本类需求最常见的腐烂方式：改一处、漏三处。

namespace {

/// 图边距（内容坐标里四周留白）。
constexpr float kMargin = 12.0f;
/// 自环回环的手臂长度（层空间 `c` 方向）。
constexpr float kSelfLoopArm = 26.0f;
/// 回边（目标在上游）走的外侧车道，相对全部节点的最左缘再外推的距离。
constexpr float kSideLane = 30.0f;

/// 层空间中的一个节点盒（`l`/`c` 都是"层增长方向/层内方向"上的量）。
struct LayerBox {
  float l{0.0f};
  float c{0.0f};
  float l_size{0.0f};
  float c_size{0.0f};
};

/// 节点形状给标签留的**可用比例**（`x`/`y` 各占外接框的比例，以中心对齐）。
///
/// 同一对数字同时用于两处：**算盒子尺寸**（`text / ratio + padding`）与**给文字区**
/// （`rect 尺寸 × ratio`）。分成两处各算一套必然对不上——实测症状就是
/// “菱形里的字横向溢出到尖角外”，而盒子尺寸与属性面全都对。
///
/// 数值来自几何关系：菱形内接矩形满足 `a/A + b/B <= 1`（各 0.5 恰好相切）；
/// 圆内接正方形边长 = 直径/√2；体育场两端半圆不排字（各让 10%）。
struct TextRatio {
  float x{1.0f};
  float y{1.0f};
};

[[nodiscard]] constexpr auto text_ratio(NodeShape shape) noexcept -> TextRatio {
  switch (shape) {
    case NodeShape::Diamond: return TextRatio{0.5f, 0.5f};
    case NodeShape::Circle: return TextRatio{0.7071f, 0.7071f};
    case NodeShape::Stadium: return TextRatio{0.8f, 1.0f};
    case NodeShape::Rect:
    case NodeShape::Rounded: return TextRatio{1.0f, 1.0f};
  }
  return TextRatio{};
}

/// 一趟重心排序：按"相邻层邻居的平均位置"重排同一层内的顺序（减少交叉）。
///
/// 用 `stable_sort` + 声明序做次键：布局必须确定性，而重心相同的节点总有若干个。
void order_layer(std::vector<std::size_t>& layer, const std::vector<std::size_t>& rank_of,
                 const std::vector<std::vector<std::size_t>>& neighbours) {
  std::vector<std::pair<float, std::size_t>> keyed;
  keyed.reserve(layer.size());
  for (const std::size_t node : layer) {
    float sum = 0.0f;
    std::size_t used = 0;
    for (const std::size_t other : neighbours[node]) {
      if (rank_of[other] == rank_of[node]) continue;
      sum += static_cast<float>(other);  // 声明序号作为"位置"的廉价代理
      ++used;
    }
    const float barycenter = used == 0U ? static_cast<float>(node)
                                        : sum / static_cast<float>(used);
    keyed.emplace_back(barycenter, node);
  }
  std::ranges::stable_sort(keyed, [](const auto& lhs, const auto& rhs) {
    return lhs.first < rhs.first;
  });
  for (std::size_t index = 0; index < layer.size(); ++index) layer[index] = keyed[index].second;
}

}  // namespace

auto layout_flowchart(const Flowchart& chart, const LayoutOptions& options,
                      const MeasureText& measure) -> FlowLayout {
  FlowLayout out;
  const std::size_t count = chart.nodes.size();
  if (count == 0U) return out;
  const float font = std::max(options.font_size, 1.0f);
  const float line_h = font * options.line_gap;
  const bool vertical = chart.direction == Direction::TopDown || chart.direction == Direction::BottomUp;

  // ── ① 标签分行与节点盒尺寸 ──
  std::vector<std::vector<std::string>> label_lines(count);
  std::vector<LayerBox> boxes(count);
  for (std::size_t index = 0; index < count; ++index) {
    label_lines[index] = split_label_lines(chart.nodes[index].label);
    float text_width = 0.0f;
    for (const std::string& line : label_lines[index]) {
      text_width = std::max(text_width, measure(line, font));
    }
    const float text_height = line_h * static_cast<float>(label_lines[index].size());
    // 盒子尺寸 = 文字需求 / 形状可用比例 + 内边距：与下面的 `text_rect` 同一对数字，
    // 所以“文字恰好放进盒子”是构造出来的，不是调出来的。
    const TextRatio ratio = text_ratio(chart.nodes[index].shape);
    float box_width =
        std::max(text_width / std::max(ratio.x, 0.01f) + options.pad_x * 2.0f,
                 options.min_node_width);
    float box_height = text_height / std::max(ratio.y, 0.01f) + options.pad_y * 2.0f;
    if (chart.nodes[index].shape == NodeShape::Circle) box_height = box_width;
    boxes[index].l_size = vertical ? box_height : box_width;
    boxes[index].c_size = vertical ? box_width : box_height;
  }

  // ── ② 去掉回边（DFS 三色）──
  // 为什么必须去：分层布局在**有环图**上无解（rank 会互相推高、永不收敛），
  // 而 mermaid 的流程图允许环（那是常见画法）。回边不参与分层，但仍然画出来。
  std::vector<std::vector<std::size_t>> adjacency(count);
  std::vector<std::vector<std::size_t>> neighbours(count);
  auto add_pair = [&](std::size_t from, std::size_t to) {
    if (from >= count || to >= count || from == to) return;
    adjacency[from].push_back(to);
    neighbours[from].push_back(to);
    neighbours[to].push_back(from);
  };
  for (const FlowEdge& edge : chart.edges) add_pair(edge.from, edge.to);

  std::vector<std::uint8_t> color(count, 0);
  std::vector<std::vector<std::size_t>> back_edges(count);
  std::vector<std::pair<std::size_t, std::size_t>> stack;
  for (std::size_t root = 0; root < count; ++root) {
    if (color[root] != 0U) continue;
    stack.clear();
    stack.emplace_back(root, 0);
    color[root] = 1;
    while (!stack.empty()) {
      const std::size_t node = stack.back().first;
      const std::size_t next = stack.back().second;
      if (next >= adjacency[node].size()) {
        color[node] = 2;
        stack.pop_back();
        continue;
      }
      stack.back().second = next + 1U;
      const std::size_t target = adjacency[node][next];
      if (color[target] == 1U) {
        back_edges[node].push_back(target);
        continue;
      }
      if (color[target] == 0U) {
        color[target] = 1;
        stack.emplace_back(target, 0);
      }
    }
  }

  // ── ③ 最长路径分层（Kahn 拓扑序；回边不参与）──
  std::vector<std::size_t> indegree(count, 0);
  std::vector<std::vector<std::size_t>> forward(count);
  for (const FlowEdge& edge : chart.edges) {
    if (edge.self || edge.from >= count || edge.to >= count) continue;
    if (std::ranges::find(back_edges[edge.from], edge.to) != back_edges[edge.from].end()) continue;
    forward[edge.from].push_back(edge.to);
    ++indegree[edge.to];
  }
  std::vector<std::size_t> rank(count, 0);
  std::vector<std::size_t> queue;
  queue.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    if (indegree[index] == 0U) queue.push_back(index);
  }
  std::vector<std::size_t> order;
  order.reserve(count);
  for (std::size_t head = 0; head < queue.size(); ++head) {
    const std::size_t node = queue[head];
    order.push_back(node);
    for (const std::size_t target : forward[node]) {
      rank[target] = std::max(rank[target], rank[node] + 1U);
      if (--indegree[target] == 0U) queue.push_back(target);
    }
  }
  if (order.size() < count) {
    // 兜底：多重回边组合出的残余环。未入列的节点统一排到最后一层——
    // 布局是派生结论，坏输入不该变成"空白"或异常。
    std::size_t deepest = 0;
    for (const std::size_t each : rank) deepest = std::max(deepest, each);
    for (std::size_t index = 0; index < count; ++index) {
      if (indegree[index] != 0U) rank[index] = deepest;
    }
  }

  // ── ④ 分组与层内排序 ──
  std::size_t rank_count = 0;
  for (const std::size_t each : rank) rank_count = std::max(rank_count, each + 1U);
  std::vector<std::vector<std::size_t>> layers(rank_count);
  for (std::size_t index = 0; index < count; ++index) layers[rank[index]].push_back(index);
  for (auto& layer : layers) order_layer(layer, rank, neighbours);

  // ── ⑤ 层空间坐标 ──
  float total_l = 0.0f;
  std::vector<float> layer_l(layers.size(), 0.0f);
  std::vector<float> layer_l_size(layers.size(), 0.0f);
  for (std::size_t index = 0; index < layers.size(); ++index) {
    float thickness = 0.0f;
    for (const std::size_t node : layers[index]) thickness = std::max(thickness, boxes[node].l_size);
    layer_l_size[index] = thickness;
    layer_l[index] = total_l;
    total_l += thickness + options.rank_gap;
  }
  if (!layers.empty()) total_l -= options.rank_gap;
  total_l = std::max(total_l, 0.0f);

  float widest = 0.0f;
  for (const auto& layer : layers) {
    float span = 0.0f;
    for (const std::size_t node : layer) span += boxes[node].c_size + options.sibling_gap;
    if (!layer.empty()) span -= options.sibling_gap;
    widest = std::max(widest, span);
  }
  for (std::size_t index = 0; index < layers.size(); ++index) {
    float span = 0.0f;
    for (const std::size_t node : layers[index]) span += boxes[node].c_size + options.sibling_gap;
    if (!layers[index].empty()) span -= options.sibling_gap;
    float cursor = (widest - span) * 0.5f;
    for (const std::size_t node : layers[index]) {
      boxes[node].c = cursor;
      boxes[node].l = layer_l[index] + (layer_l_size[index] - boxes[node].l_size) * 0.5f;
      cursor += boxes[node].c_size + options.sibling_gap;
    }
  }

  // ── ⑥ 映射到屏幕 ──
  const Direction direction = chart.direction;
  const auto to_screen = [&](float c, float l) -> math::Point {
    switch (direction) {
      case Direction::TopDown: return math::Point{c, l};
      case Direction::BottomUp: return math::Point{c, total_l - l};
      case Direction::LeftRight: return math::Point{l, c};
      case Direction::RightLeft: return math::Point{total_l - l, c};
    }
    return math::Point{c, l};
  };
  const auto rect_to_screen = [&](float c, float l, float c_size, float l_size) -> math::Rect {
    const math::Point a = to_screen(c, l);
    const math::Point b = to_screen(c + c_size, l + l_size);
    return math::Rect::from_ltrb(std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x),
                                 std::max(a.y, b.y));
  };

  out.nodes.resize(count);
  for (std::size_t index = 0; index < count; ++index) {
    out.nodes[index].rect =
        rect_to_screen(boxes[index].c, boxes[index].l, boxes[index].c_size, boxes[index].l_size);
    out.nodes[index].lines = label_lines[index];
    // 标签可排区域：形状决定可用内接范围。集中在这里算（而不是绘制端），
    // 因为它是几何结论；且用与盒子尺寸**同一对比例**（见 `text_ratio`）。
    const math::Rect& rect = out.nodes[index].rect;
    const TextRatio ratio = text_ratio(chart.nodes[index].shape);
    const float inner_width = rect.width * ratio.x;
    const float inner_height = rect.height * ratio.y;
    out.nodes[index].text_rect =
        math::Rect{rect.center().x - inner_width * 0.5f, rect.center().y - inner_height * 0.5f,
                   inner_width, inner_height};
  }

  // ── ⑦ 连线：从起点朝目标一侧出线、到中点拐弯、水平进入目标 ──
  //
  // 为什么不用"中心到中心 + 形状裁切"：那条线在多层跨度的边上会斜穿整张图，
  // 且与同类图（drawio/PlantUML）的观感差很远。正交三段既不穿框、也可断言。
  out.edges.reserve(chart.edges.size());
  for (std::size_t index = 0; index < chart.edges.size(); ++index) {
    const FlowEdge& edge = chart.edges[index];
    if (edge.from >= count || edge.to >= count) continue;
    EdgePath path;
    path.edge = index;
    path.style = edge.style;
    path.arrow = edge.arrow;
    path.self = edge.self || edge.from == edge.to;
    const LayerBox& source = boxes[edge.from];
    const LayerBox& target = boxes[edge.to];
    if (path.self) {
      const float c_center = source.c + source.c_size * 0.5f;
      const float l_end = source.l + source.l_size;
      const float l_mid = source.l + source.l_size * 0.5f;
      const float c_arm = source.c + source.c_size + kSelfLoopArm;
      path.points = {to_screen(c_center, l_end), to_screen(c_arm, l_end),
                     to_screen(c_arm, l_mid), to_screen(source.c + source.c_size, l_mid)};
      if (!edge.label.empty()) {
        path.has_label = true;
        path.label_lines = split_label_lines(edge.label);
        const float width = path.label_lines.empty() ? 0.0f : measure(path.label_lines.front(), font);
        const math::Point anchor = to_screen(c_arm + 4.0f, l_mid - line_h * 0.5f);
        path.label = math::Rect{anchor.x, anchor.y, width + 6.0f,
                                line_h * static_cast<float>(path.label_lines.size()) + 4.0f};
      }
      out.edges.push_back(std::move(path));
      continue;
    }

    const float from_c = source.c + source.c_size * 0.5f;
    const float from_l = source.l + source.l_size;
    const float to_c = target.c + target.c_size * 0.5f;
    const float to_l = target.l;
    const math::Point start = to_screen(from_c, from_l);
    const math::Point end = to_screen(to_c, to_l);

    // **回边要绕外侧**：目标层在上游（target 在上/在左）时，若照正向路由算
    // （起点取源层下缘、终点取目标层上缘），折线会从下往上直接穿过源节点自身。
    // 实测症状：`D --> A` 这条回边在截图里整条消失（被节点盖住）。
    // 做法：从源节点的侧边出、沿图外侧的车道往上、再横进入目标节点的侧边。
    const bool backward = target.l + target.l_size <= source.l + 0.5f;
    if (backward) {
      float lane_c = source.c;
      for (const LayerBox& box : boxes) lane_c = std::min(lane_c, box.c);
      lane_c -= kSideLane;
      const float src_mid_l = source.l + source.l_size * 0.5f;
      const float dst_mid_l = target.l + target.l_size * 0.5f;
      path.points = {to_screen(source.c, src_mid_l), to_screen(lane_c, src_mid_l),
                     to_screen(lane_c, dst_mid_l), to_screen(target.c, dst_mid_l)};
      if (!edge.label.empty()) {
        path.has_label = true;
        path.label_lines = split_label_lines(edge.label);
        float width = 0.0f;
        for (const std::string& line : path.label_lines) width = std::max(width, measure(line, font));
        const math::Point anchor = to_screen(lane_c, (src_mid_l + dst_mid_l) * 0.5f);
        path.label = math::Rect{anchor.x - width * 0.5f - 3.0f, anchor.y - line_h * 0.5f,
                                width + 6.0f,
                                line_h * static_cast<float>(path.label_lines.size()) + 4.0f};
      }
      out.edges.push_back(std::move(path));
      continue;
    }

    // 正向：拐弯点放在**源层下方的空隙里**（而不是两层的正中）——跳层连线的横向
    // 段就不会穿进中间那层的节点框（中层节点的平面坐标区间正落在正中附近）。
    const float lane_l = from_l + options.rank_gap * 0.5f;
    if (std::abs(from_c - to_c) < 0.5f) {
      path.points = {start, end};
    } else {
      path.points = {start, to_screen(from_c, lane_l), to_screen(to_c, lane_l), end};
    }
    if (!edge.label.empty()) {
      path.has_label = true;
      path.label_lines = split_label_lines(edge.label);
      float width = 0.0f;
      for (const std::string& line : path.label_lines) width = std::max(width, measure(line, font));
      const math::Point anchor = to_screen((from_c + to_c) * 0.5f, lane_l);
      const float label_height = line_h * static_cast<float>(path.label_lines.size()) + 4.0f;
      path.label = math::Rect{anchor.x - width * 0.5f - 3.0f, anchor.y - line_h * 0.5f,
                              width + 6.0f, label_height};
    }
    out.edges.push_back(std::move(path));
  }

  // ── ⑧ 归一化到正象限（内容坐标原点 = 图左上角，绘制方只加视图偏移）──
  float min_x = 0.0f;
  float min_y = 0.0f;
  float max_x = 0.0f;
  float max_y = 0.0f;
  bool seeded = false;
  const auto include_rect = [&](const math::Rect& rect) {
    if (!seeded) {
      min_x = rect.x;
      min_y = rect.y;
      max_x = rect.right();
      max_y = rect.bottom();
      seeded = true;
      return;
    }
    min_x = std::min(min_x, rect.x);
    min_y = std::min(min_y, rect.y);
    max_x = std::max(max_x, rect.right());
    max_y = std::max(max_y, rect.bottom());
  };
  for (const NodeBox& node : out.nodes) include_rect(node.rect);
  for (const EdgePath& path : out.edges) {
    if (path.has_label) include_rect(path.label);
    for (const math::Point point : path.points) include_rect(math::Rect{point.x, point.y, 0.0f, 0.0f});
  }
  if (!seeded) {
    out.size = math::Size{kMargin * 2.0f, kMargin * 2.0f};
    return out;
  }
  const float shift_x = kMargin - min_x;
  const float shift_y = kMargin - min_y;
  for (NodeBox& node : out.nodes) {
    node.rect = node.rect.offset(shift_x, shift_y);
    // ⚠ `text_rect` 必须一起平移：它是 `rect` 的派生量，漏平移就会跑到框外面（实测）。
    node.text_rect = node.text_rect.offset(shift_x, shift_y);
  }
  for (EdgePath& path : out.edges) {
    for (math::Point& point : path.points) {
      point.x += shift_x;
      point.y += shift_y;
    }
    if (path.has_label) path.label = path.label.offset(shift_x, shift_y);
  }
  out.size = math::Size{max_x - min_x + kMargin * 2.0f, max_y - min_y + kMargin * 2.0f};
  return out;
}

auto layout_sequence(const Sequence& sequence, const LayoutOptions& options,
                     const MeasureText& measure) -> SequenceLayout {
  SequenceLayout out;
  if (sequence.participants.empty()) return out;
  const float font = std::max(options.font_size, 1.0f);
  const float line_h = font * options.line_gap;
  /// 参与者头之间的最小空隙：生命线要能各自落位，且消息文字不能压到旁边那条。
  constexpr float kColumnGap = 44.0f;
  constexpr float kMessageGap = 14.0f;
  constexpr float kHeadRadius = 8.0f;

  std::vector<std::vector<std::string>> head_lines(sequence.participants.size());
  std::vector<float> widths(sequence.participants.size(), 0.0f);
  float head_height = 0.0f;
  for (std::size_t index = 0; index < sequence.participants.size(); ++index) {
    const SequenceParticipant& participant = sequence.participants[index];
    head_lines[index] = split_label_lines(participant.label.empty() ? participant.id
                                                                   : participant.label);
    float width = 0.0f;
    for (const std::string& line : head_lines[index]) width = std::max(width, measure(line, font));
    widths[index] = std::max(width + options.pad_x * 2.0f, 84.0f);
    head_height = std::max(head_height,
                           line_h * static_cast<float>(head_lines[index].size()) +
                               options.pad_y * 2.0f);
  }

  float cursor = kMargin;
  out.participants.resize(sequence.participants.size());
  out.lifelines.resize(sequence.participants.size(), 0.0f);
  for (std::size_t index = 0; index < sequence.participants.size(); ++index) {
    out.participants[index].rect = math::Rect{cursor, kMargin, widths[index], head_height};
    out.participants[index].lines = head_lines[index];
    out.lifelines[index] = cursor + widths[index] * 0.5f;
    cursor += widths[index] + kColumnGap;
  }
  const float diagram_width = cursor - kColumnGap + kMargin;
  out.lifeline_top = kMargin + head_height + kHeadRadius;

  float y = out.lifeline_top + 22.0f;
  out.messages.reserve(sequence.messages.size());
  for (std::size_t index = 0; index < sequence.messages.size(); ++index) {
    const SequenceMessage& message = sequence.messages[index];
    if (message.from >= out.lifelines.size() || message.to >= out.lifelines.size()) continue;
    MessageGeometry geometry;
    geometry.index = index;
    geometry.dashed = message.dashed;
    geometry.arrow = message.arrow;
    geometry.self = message.from == message.to;
    geometry.lines = split_label_lines(message.text);
    float label_width = 0.0f;
    for (const std::string& line : geometry.lines) label_width = std::max(label_width, measure(line, font));
    const float label_height =
        geometry.lines.empty() ? 0.0f : line_h * static_cast<float>(geometry.lines.size());
    const float row_height = std::max(label_height, 22.0f) + kMessageGap;
    const float center_y = y + row_height * 0.5f;
    geometry.y = center_y;
    geometry.x_from = out.lifelines[message.from];
    geometry.x_to = out.lifelines[message.to];
    if (geometry.self) {
      // 自发消息：箭头在原处拐个弯，文字画在右侧（与 UML 的通行画法一致）。
      geometry.label =
          math::Rect{geometry.x_from + 18.0f, center_y - label_height * 0.5f, label_width + 6.0f,
                     label_height + 4.0f};
    } else {
      // 文字放在**线的上方**（mermaid/UML 的通行做法）：压在线上即使有底板，
      // 也会被读成"线被打断了"。
      geometry.label = math::Rect{
          (geometry.x_from + geometry.x_to) * 0.5f - label_width * 0.5f - 3.0f,
          center_y - label_height - 4.0f, label_width + 6.0f, label_height + 4.0f};
    }
    y += row_height;
    out.messages.push_back(std::move(geometry));
  }
  out.lifeline_bottom = y + 6.0f;
  out.size = math::Size{diagram_width, out.lifeline_bottom + kMargin};
  return out;
}

auto layout_pie(const Pie& pie, const LayoutOptions& options, const MeasureText& measure)
    -> PieLayout {
  PieLayout out;
  const float font = std::max(options.font_size, 1.0f);
  const float line_h = font * options.line_gap;
  // 半径按字号缩放（与布局参数同一套纪律：放大查看时比例一致）。
  const float radius = std::max(font * 6.4f, 56.0f);

  out.title_lines = pie.title.empty() ? std::vector<std::string>{}
                                      : split_label_lines(pie.title);
  const float title_height =
      out.title_lines.empty() ? 0.0f : line_h * static_cast<float>(out.title_lines.size()) + 8.0f;
  out.title_box = math::Rect{kMargin, kMargin, 0.0f, title_height};

  out.center = math::Point{kMargin + radius, kMargin + title_height + radius};
  out.radius = radius;

  double total = 0.0;
  for (const PieSlice& slice : pie.slices) {
    if (slice.value > 0.0) total += slice.value;
  }
  const float legend_x = out.center.x + radius + 26.0f;
  float legend_y = out.center.y - radius;
  float legend_width = 0.0f;
  double accumulated = 0.0;
  out.slices.reserve(pie.slices.size());
  for (const PieSlice& slice : pie.slices) {
    PieSliceGeometry geometry;
    if (total > 0.0 && slice.value > 0.0) {
      geometry.percent = slice.value / total * 100.0;
      geometry.start_angle = accumulated / total * 2.0 * kPi;
      geometry.sweep = slice.value / total * 2.0 * kPi;
      accumulated += slice.value;
    }
    // 图例一行就是最终显示文本（`标签  百分比`）：
    // **宽度必须按最终文本算**——布局按 `标签 值` 算、绘制再追加百分比，
    // 就会横向溢出并压到左侧色块上（实测）。
    const std::string text =
        std::format("{}  {:.1f}%", slice.label, geometry.percent);
    geometry.lines = {text};
    const float width = measure(text, font);
    legend_width = std::max(legend_width, width);
    geometry.legend = math::Rect{legend_x, legend_y, width + 18.0f, line_h + 6.0f};
    legend_y += line_h + 8.0f;
    out.slices.push_back(std::move(geometry));
  }

  out.size = math::Size{std::max(legend_x + legend_width + kMargin, out.center.x + radius + kMargin),
                        std::max(legend_y, out.center.y + radius) + kMargin};
  out.title_box.width = out.size.width - kMargin * 2.0f;
  return out;
}

}  // namespace st::md::mermaid

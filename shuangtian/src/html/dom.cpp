#include "st/html/dom.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace st::html {

namespace {

[[nodiscard]] auto is_space(char ch) noexcept -> bool {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f';
}

[[nodiscard]] auto lower_ascii(std::string_view text) -> std::string {
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
    out.push_back(ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch);
  }
  return out;
}

[[nodiscard]] auto equals_ignore_case(std::string_view left, std::string_view right) noexcept
    -> bool {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    char a = left[index];
    char b = right[index];
    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

// ————————————————————————————————————————————————————————————————————————————
// 实体解码
// ————————————————————————————————————————————————————————————————————————————

/// 追加 UTF-8 编码的码点。
void append_codepoint(std::string& out, std::uint32_t value) {
  if (value <= 0x7FU) {
    out.push_back(static_cast<char>(value));
  } else if (value <= 0x7FFU) {
    out.push_back(static_cast<char>(0xC0U | (value >> 6U)));
    out.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
  } else if (value <= 0xFFFFU) {
    out.push_back(static_cast<char>(0xE0U | (value >> 12U)));
    out.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
  } else {
    out.push_back(static_cast<char>(0xF0U | (value >> 18U)));
    out.push_back(static_cast<char>(0x80U | ((value >> 12U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
  }
}

/// 常见命名实体（HTML 里网页实际会用到的那一批；其余原样保留）。
struct NamedEntity {
  std::string_view name;
  std::uint32_t value;
};
constexpr NamedEntity kNamedEntities[] = {
    {"amp", 0x26},    {"lt", 0x3C},     {"gt", 0x3E},     {"quot", 0x22},   {"apos", 0x27},
    {"nbsp", 0xA0},   {"copy", 0xA9},   {"reg", 0xAE},    {"trade", 0x2122}, {"hellip", 0x2026},
    {"mdash", 0x2014}, {"ndash", 0x2013}, {"lsquo", 0x2018}, {"rsquo", 0x2019},
    {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"bull", 0x2022}, {"middot", 0xB7},
    {"times", 0xD7},  {"divide", 0xF7}, {"plusmn", 0xB1}, {"deg", 0xB0},
    {"euro", 0x20AC}, {"pound", 0xA3}, {"yen", 0xA5},   {"cent", 0xA2},
    {"sect", 0xA7},   {"para", 0xB6},  {"laquo", 0xAB},  {"raquo", 0xBB},
    {"le", 0x2264},   {"ge", 0x2265},  {"ne", 0x2260},   {"asymp", 0x2248},
    {"infin", 0x221E}, {"sum", 0x2211}, {"radic", 0x221A}, {"rarr", 0x2192},
    {"larr", 0x2190}, {"uarr", 0x2191}, {"darr", 0x2193}, {"check", 0x2713},
};

/// 解码 `&…;`（数字引用与上表里的命名实体；认不出的原样保留）。
void append_decoded(std::string& out, std::string_view raw) {
  out.reserve(out.size() + raw.size());
  for (std::size_t index = 0; index < raw.size();) {
    if (raw[index] != '&') {
      out.push_back(raw[index]);
      ++index;
      continue;
    }
    const std::size_t semi = raw.find(';', index + 1);
    // 实体名不会很长；找不到 `;` 或跨度过大就当普通字符（避免把整篇当实体扫描）。
    if (semi == std::string_view::npos || semi - index > 12) {
      out.push_back(raw[index]);
      ++index;
      continue;
    }
    const std::string_view body = raw.substr(index + 1, semi - index - 1);
    bool decoded = false;
    if (!body.empty() && body[0] == '#') {
      std::string_view digits = body.substr(1);
      std::uint32_t base = 10;
      if (!digits.empty() && (digits[0] == 'x' || digits[0] == 'X')) {
        base = 16;
        digits.remove_prefix(1);
      }
      std::uint32_t value = 0;
      bool ok = !digits.empty();
      for (const char ch : digits) {
        std::uint32_t digit = 0;
        if (ch >= '0' && ch <= '9') {
          digit = static_cast<std::uint32_t>(ch - '0');
        } else if (base == 16 && ch >= 'a' && ch <= 'f') {
          digit = static_cast<std::uint32_t>(ch - 'a') + 10U;
        } else if (base == 16 && ch >= 'A' && ch <= 'F') {
          digit = static_cast<std::uint32_t>(ch - 'A') + 10U;
        } else {
          ok = false;
          break;
        }
        value = value * base + digit;
      }
      if (ok && value > 0U && value <= 0x10FFFFU) {
        append_codepoint(out, value);
        decoded = true;
      }
    } else {
      for (const NamedEntity& entity : kNamedEntities) {
        if (equals_ignore_case(body, entity.name)) {
          append_codepoint(out, entity.value);
          decoded = true;
          break;
        }
      }
    }
    if (decoded) {
      index = semi + 1;
    } else {
      // 认不出：原样保留（连 `&` 一起），不猜语义。
      out.push_back(raw[index]);
      ++index;
    }
  }
}

/// 折叠空白（连续空白 → 单个空格；首尾去掉）后追加。
void append_collapsed(std::string& out, std::string_view raw) {
  std::string decoded;
  append_decoded(decoded, raw);
  for (const char ch : decoded) {
    if (is_space(ch)) {
      if (!out.empty() && out.back() != ' ') out.push_back(' ');
    } else {
      out.push_back(ch);
    }
  }
}

// ————————————————————————————————————————————————————————————————————————————
// 隐式闭合规则
// ————————————————————————————————————————————————————————————————————————————

/// 「遇到 `open` 时要自动闭合栈顶的哪些标签」（HTML 的隐式闭合最小集）。
///
/// 为何必须有：真实网页大量使用 `<li>a<li>b`、`<p>x<p>y`（不写闭合标签）。
/// 若不折叠，整篇会变成**层层嵌套**——排版上表现为「每段越缩越深」，
/// 而这种畸形在数据层看起来完全合法（树就是深的）。
[[nodiscard]] auto auto_closes(std::string_view open, std::string_view top) noexcept -> bool {
  if (open == "li") return top == "li";
  if (open == "p") {
    return top == "p" || top == "div" || top == "section" || top == "article" ||
           top == "blockquote" || top == "ul" || top == "ol" || top == "table";
  }
  if (open == "dt" || open == "dd") return top == "dt" || top == "dd";
  if (open == "tr") return top == "tr" || top == "td" || top == "th";
  if (open == "td" || open == "th") return top == "td" || top == "th";
  if (open == "option") return top == "option";
  if (open == "thead" || open == "tbody") return top == "thead" || top == "tbody" || top == "tr";
  return false;
}

}  // namespace

// ————————————————————————————————————————————————————————————————————————————
// Node
// ————————————————————————————————————————————————————————————————————————————

auto Node::attr(std::string_view key) const -> std::optional<std::string_view> {
  for (const auto& [name, value] : attributes) {
    if (equals_ignore_case(name, key)) return std::string_view(value);
  }
  return std::nullopt;
}

auto Node::attr_number(std::string_view key) const -> std::optional<float> {
  const auto raw = attr(key);
  if (!raw.has_value()) return std::nullopt;
  // 允许 `"12px"` / `"50%"` 这类带单位的写法：只取前面的数字部分。
  std::string_view digits = *raw;
  std::size_t length = 0;
  while (length < digits.size() &&
         ((digits[length] >= '0' && digits[length] <= '9') || digits[length] == '.' ||
          digits[length] == '-' || digits[length] == '+')) {
    ++length;
  }
  if (length == 0U) return std::nullopt;
  try {
    return std::stof(std::string(digits.substr(0, length)));
  } catch (...) {
    // `std::stof` 在极端输入上会抛（无异常边界：这里吞掉并按"没给"处理）。
    return std::nullopt;
  }
}

auto Node::child(std::string_view child_tag) const -> const Node* {
  for (const Node& node : children) {
    if (node.kind == NodeKind::Element && node.tag == child_tag) return &node;
  }
  return nullptr;
}

auto Node::has_class(std::string_view name) const -> bool {
  const auto value = attr("class");
  if (!value.has_value()) return false;
  std::size_t index = 0;
  while (index < value->size()) {
    while (index < value->size() && is_space((*value)[index])) ++index;
    const std::size_t begin = index;
    while (index < value->size() && !is_space((*value)[index])) ++index;
    if (index > begin && equals_ignore_case(value->substr(begin, index - begin), name)) {
      return true;
    }
  }
  return false;
}

// ————————————————————————————————————————————————————————————————————————————
// 空元素
// ————————————————————————————————————————————————————————————————————————————

auto is_void_element(std::string_view tag) noexcept -> bool {
  static constexpr std::string_view kVoid[] = {"area", "base",   "br",    "col",   "embed",
                                              "hr",   "img",    "input", "link",  "meta",
                                              "param", "source", "track", "wbr"};
  return std::ranges::find(kVoid, tag) != std::end(kVoid);
}

// ————————————————————————————————————————————————————————————————————————————
// 解析
// ————————————————————————————————————————————————————————————————————————————

namespace {

/// 解析器状态（全部是局部变量——`parse` 是纯函数，可重入、可并行）。
struct Parser {
  std::string_view source;
  std::size_t pos{0};
  std::uint32_t line{1};
  Document doc{};
  std::vector<Node*> stack{};   ///< 当前父链（栈底是合成根）
  std::string pending_text{};   ///< 待提交的文本（折叠后才入树）
  bool preserve_ws{false};      ///< 是否在 `<pre>`/`<textarea>` 内（保留空白）
  bool in_raw_text{false};      ///< 是否在 `<script>`/`<style>` 内（内容不当标签解析）
  std::string raw_text{};       ///< raw text 元素的累计内容
  std::string raw_tag{};

  void advance_to(std::size_t target) {
    for (std::size_t index = pos; index < target && index < source.size(); ++index) {
      if (source[index] == '\n') ++line;
    }
  }

  void flush_text() {
    if (pending_text.empty()) return;
    Node node{};
    node.kind = NodeKind::Text;
    node.text = std::move(pending_text);
    node.line = line;
    pending_text.clear();
    stack.back()->children.push_back(std::move(node));
  }

  void append_text(std::string_view raw, bool preserve) {
    if (preserve) {
      // 保留空白：只解实体，不折叠（`<pre>` 的缩进与换行是内容）。
      std::string decoded;
      append_decoded(decoded, raw);
      pending_text += decoded;
    } else {
      append_collapsed(pending_text, raw);
    }
  }

  void close_top() {
    if (stack.size() <= 1U) return;
    stack.pop_back();
    // 退出 `<style>`/`<pre>` 后恢复上下文。
    preserve_ws = false;
    for (std::size_t index = 1; index < stack.size(); ++index) {
      if (stack[index]->tag == "pre" || stack[index]->tag == "textarea") preserve_ws = true;
    }
  }

  /// 按隐式闭合规则收口（可能连续收口多层）。
  void auto_close(std::string_view tag) {
    while (stack.size() > 1U) {
      const std::string& top = stack.back()->tag;
      if (!auto_closes(tag, top)) break;
      flush_text();
      close_top();
    }
  }

  /// 解析属性串（`index` 指向标签名之后）。
  auto parse_attributes(std::string_view tag_text, std::size_t& index, Node& node) -> void {
    while (index < tag_text.size()) {
      while (index < tag_text.size() && is_space(tag_text[index])) ++index;
      if (index >= tag_text.size() || tag_text[index] == '>' || tag_text[index] == '/') break;
      const std::size_t name_begin = index;
      while (index < tag_text.size() && !is_space(tag_text[index]) && tag_text[index] != '=' &&
             tag_text[index] != '>' && tag_text[index] != '/') {
        ++index;
      }
      const std::string name = lower_ascii(tag_text.substr(name_begin, index - name_begin));
      while (index < tag_text.size() && is_space(tag_text[index])) ++index;
      std::string value;
      if (index < tag_text.size() && tag_text[index] == '=') {
        ++index;
        while (index < tag_text.size() && is_space(tag_text[index])) ++index;
        if (index < tag_text.size() && (tag_text[index] == '"' || tag_text[index] == '\'')) {
          const char quote = tag_text[index];
          ++index;
          const std::size_t value_begin = index;
          while (index < tag_text.size() && tag_text[index] != quote) ++index;
          value = lower_ascii(tag_text.substr(value_begin, index - value_begin));
          if (index < tag_text.size()) ++index;
        } else {
          const std::size_t value_begin = index;
          while (index < tag_text.size() && !is_space(tag_text[index]) && tag_text[index] != '>') {
            ++index;
          }
          value = lower_ascii(tag_text.substr(value_begin, index - value_begin));
        }
        std::string decoded;
        append_decoded(decoded, value);
        value = std::move(decoded);
      }
      if (name.empty()) continue;
      // 同名属性以第一个为准（与 `st::xml` 同口径）。
      const bool exists = std::ranges::any_of(node.attributes, [&name](const auto& pair) {
        return pair.first == name;
      });
      if (!exists) node.attributes.emplace_back(name, std::move(value));
    }
  }

  /// 找标签结束的 `>`（跳过引号内的 `>`——属性值里出现 `>` 是合法的）。
  auto find_tag_end(std::size_t from) const -> std::size_t {
    char quote = 0;
    for (std::size_t index = from; index < source.size(); ++index) {
      const char ch = source[index];
      if (quote != 0) {
        if (ch == quote) quote = 0;
        continue;
      }
      if (ch == '"' || ch == '\'') {
        quote = ch;
        continue;
      }
      if (ch == '>') return index;
    }
    return std::string_view::npos;
  }

  /// 跳过注释/`<!DOCTYPE>`/`<?xml?>`/CDATA 等声明；返回是否跳过了一段。
  auto skip_declaration() -> bool {
    const auto lower_prefix = [this](std::string_view needle) {
      return source.size() - pos >= needle.size() &&
             equals_ignore_case(source.substr(pos, needle.size()), needle);
    };
    if (lower_prefix("<!--")) {
      const std::size_t end = source.find("-->", pos + 4);
      const std::size_t stop = end == std::string_view::npos ? source.size() : end + 3;
      advance_to(stop);
      pos = stop;
      return true;
    }
    if (lower_prefix("<![cdata[")) {
      const std::size_t start = pos + 9;
      const std::size_t end = source.find("]]>", start);
      const std::size_t stop = end == std::string_view::npos ? source.size() : end;
      append_text(source.substr(start, stop - start), preserve_ws);
      advance_to(stop);
      pos = end == std::string_view::npos ? source.size() : end + 3;
      return true;
    }
    if (lower_prefix("<!") || lower_prefix("<?")) {
      const std::size_t end = source.find('>', pos);
      const std::size_t stop = end == std::string_view::npos ? source.size() : end + 1;
      advance_to(stop);
      pos = stop;
      return true;
    }
    return false;
  }

  void run() {
    stack.push_back(&doc.root);
    while (pos < source.size()) {
      // —— raw text（`<script>`/`<style>`）：内容原样收到对应关闭标签 ——
      if (in_raw_text) {
        const std::string closing = "</" + raw_tag;
        const std::size_t end = lower_prefix_at(pos, closing);
        if (end == std::string_view::npos) {
          raw_text.append(source.substr(pos));
          advance_to(source.size());
          pos = source.size();
          break;
        }
        raw_text.append(source.substr(pos, end - pos));
        // 内容入库（`<style>` 收进 `style_text`，其余作为文本子节点）。
        if (raw_tag == "style") {
          if (!doc.style_text.empty()) doc.style_text += '\n';
          doc.style_text += raw_text;
        } else if (raw_tag != "script") {
          Node node{};
          node.kind = NodeKind::Text;
          node.text = std::move(raw_text);
          node.line = line;
          stack.back()->children.push_back(std::move(node));
        }
        raw_text.clear();
        advance_to(end);
        pos = end;
        in_raw_text = false;
      }

      const std::size_t lt = source.find('<', pos);
      if (lt == std::string_view::npos) {
        append_text(source.substr(pos), preserve_ws);
        advance_to(source.size());
        pos = source.size();
        break;
      }
      if (lt > pos) {
        append_text(source.substr(pos, lt - pos), preserve_ws);
        advance_to(lt);
        pos = lt;
      }
      if (skip_declaration()) continue;

      const std::size_t close = find_tag_end(pos + 1);
      if (close == std::string_view::npos) {
        // 没有 `>`：把剩下的当文本（畸形输入不该让整篇消失）。
        append_text(source.substr(pos), preserve_ws);
        advance_to(source.size());
        pos = source.size();
        break;
      }
      const std::string_view tag_text = source.substr(pos, close - pos + 1);
      const std::uint32_t tag_line = line;

      if (tag_text.size() >= 2 && tag_text[1] == '/') {
        // —— 结束标签 ——
        std::size_t index = 2;
        const std::size_t name_begin = index;
        while (index < tag_text.size() && !is_space(tag_text[index]) && tag_text[index] != '>') {
          ++index;
        }
        const std::string name =
            lower_ascii(tag_text.substr(name_begin, index - name_begin));
        flush_text();
        // 沿父链找同名元素收口（找不到就**忽略**这个结束标签——多余闭合不该拆树）。
        for (std::size_t depth = stack.size(); depth-- > 1U;) {
          if (stack[depth]->tag == name) {
            while (stack.size() > depth) close_top();
            break;
          }
        }
        advance_to(close + 1);
        pos = close + 1;
        continue;
      }

      // —— 开始标签 ——
      std::size_t index = 1;
      const std::size_t name_begin = index;
      while (index < tag_text.size() && !is_space(tag_text[index]) && tag_text[index] != '>' &&
             tag_text[index] != '/') {
        ++index;
      }
      const std::string name = lower_ascii(tag_text.substr(name_begin, index - name_begin));
      std::size_t last = tag_text.size();
      while (last > 0 && (tag_text[last - 1] == '>' || is_space(tag_text[last - 1]))) --last;
      const bool self_close = last > 0 && tag_text[last - 1] == '/';

      flush_text();
      auto_close(name);

      // **总是**挂在当前栈顶下（包括 `<html>`）：文档根固定是合成根 `#document`。
      // 早先把 `<html>` 提升成根，于是它一开栈顶就得换成一个不稳定的指针——
      // 而「根是谁」这种细节不该让排版层去猜（合成根让“任意 HTML 片段”都能跑）。
      Node node{};
      node.kind = NodeKind::Element;
      node.tag = name;
      node.line = tag_line;
      parse_attributes(tag_text, index, node);
      stack.back()->children.push_back(std::move(node));

      const bool void_tag = is_void_element(name) || self_close;
      if (!void_tag) {
        stack.push_back(&stack.back()->children.back());
        if (name == "pre" || name == "textarea") preserve_ws = true;
        if (name == "script" || name == "style") {
          in_raw_text = true;
          raw_tag = name;
          raw_text.clear();
        }
      }
      advance_to(close + 1);
      pos = close + 1;
    }
    flush_text();
  }

  /// 从 `from` 起找 needle（大小写不敏感）；返回位置或 npos。
  [[nodiscard]] auto lower_prefix_at(std::size_t from, std::string_view needle) const
      -> std::size_t {
    if (needle.size() > source.size() - from) return std::string_view::npos;
    for (std::size_t index = from; index + needle.size() <= source.size(); ++index) {
      if (equals_ignore_case(source.substr(index, needle.size()), needle)) return index;
    }
    return std::string_view::npos;
  }
};

}  // namespace

auto parse(std::string_view source, Error* out_error) -> Document {
  Parser parser{};
  parser.source = source;
  parser.doc.root.tag = "#document";
  parser.run();

  // `<title>` 抽出来（标题是元数据，不属于渲染树）。
  for (const Node& node : parser.doc.root.children) {
    if (node.kind == NodeKind::Element && node.tag == "title") {
      for (const Node& inner : node.children) {
        if (inner.kind == NodeKind::Text) parser.doc.title += inner.text;
      }
    }
  }
  parser.doc.style_text = clean_style_text(parser.doc.style_text);
  if (out_error != nullptr) {
    out_error->message.clear();
    out_error->line = 1;
  }
  return std::move(parser.doc);
}

auto clean_style_text(std::string_view source) -> std::string {
  std::string out;
  out.reserve(source.size());
  for (std::size_t index = 0; index < source.size();) {
    // `/* … */` 注释
    if (index + 1 < source.size() && source[index] == '/' && source[index + 1] == '*') {
      const std::size_t end = source.find("*/", index + 2);
      index = end == std::string_view::npos ? source.size() : end + 2;
      continue;
    }
    // HTML 注释包裹（老式写法：`<style><!-- … --></style>`）
    if (index + 3 < source.size() && source.compare(index, 4, "<!--") == 0) {
      index += 4;
      continue;
    }
    if (index + 2 < source.size() && source.compare(index, 3, "-->") == 0) {
      index += 3;
      continue;
    }
    out.push_back(source[index]);
    ++index;
  }
  return out;
}

}  // namespace st::html

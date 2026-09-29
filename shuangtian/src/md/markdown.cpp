#include "st/md/markdown.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "st/core/string.hpp"

namespace st::md {
namespace {

inline constexpr std::size_t npos = std::string_view::npos;

// ============================ 基础工具 ============================

[[nodiscard]] constexpr auto is_digit(char raw) noexcept -> bool { return raw >= '0' && raw <= '9'; }

[[nodiscard]] constexpr auto is_alpha(char raw) noexcept -> bool {
  return (raw >= 'a' && raw <= 'z') || (raw >= 'A' && raw <= 'Z');
}

[[nodiscard]] constexpr auto is_alnum(char raw) noexcept -> bool { return is_digit(raw) || is_alpha(raw); }

[[nodiscard]] constexpr auto is_blank_char(char raw) noexcept -> bool { return raw == ' ' || raw == '\t'; }

/// CommonMark 可转义字符（ASCII 标点全集）。
[[nodiscard]] constexpr auto is_escapable(char raw) noexcept -> bool {
  switch (raw) {
    case '!': case '"': case '#': case '$': case '%': case '&': case '\'': case '(': case ')': case '*':
    case '+': case ',': case '-': case '.': case '/': case ':': case ';': case '<': case '=': case '>':
    case '?': case '@': case '[': case '\\': case ']': case '^': case '_': case '`': case '{': case '|':
    case '}': case '~':
      return true;
    default:
      return false;
  }
}

/// 空行判定（仅含空格/制表符的算空行；UTF-8 全角空格不算，属于内容）。
[[nodiscard]] auto is_blank_text(std::string_view text) noexcept -> bool {
  for (const char raw : text) {
    if (!is_blank_char(raw)) return false;
  }
  return true;
}

/// 前导空格数（仅计 `' '`，缩进容器/围栏判定用；制表符不参与，避免列宽歧义）。
[[nodiscard]] auto leading_spaces(std::string_view text) noexcept -> std::size_t {
  std::size_t count = 0;
  while (count < text.size() && text[count] == ' ') ++count;
  return count;
}

/// 前导空白字符数（`' '` 与 `'\t'` 各计 1 个字符）。
[[nodiscard]] auto leading_blank_chars(std::string_view text) noexcept -> std::size_t {
  std::size_t count = 0;
  while (count < text.size() && is_blank_char(text[count])) ++count;
  return count;
}

/// 前导空白折算的列数（制表符按 4 列制表位展开）。
[[nodiscard]] auto indent_columns(std::string_view text) noexcept -> std::size_t {
  std::size_t columns = 0;
  for (const char raw : text) {
    if (raw == ' ') {
      ++columns;
    } else if (raw == '\t') {
      columns += 4 - (columns % 4);
    } else {
      break;
    }
  }
  return columns;
}

/// 按列数剥离行首前缀（空白按制表位计数，非空白字符各占 1 列——列表标记行用）。
[[nodiscard]] auto strip_columns(std::string_view text, std::size_t columns) -> std::string_view {
  std::size_t used = 0;
  std::size_t pos = 0;
  while (pos < text.size() && used < columns) {
    const char raw = text[pos];
    if (raw == ' ') {
      ++used;
    } else if (raw == '\t') {
      used += 4 - (used % 4);
    } else {
      ++used;
    }
    ++pos;
  }
  return text.substr(pos);
}

/// 中日文全角标点（句读/引号）：裸自动链接尾部裁剪用。
[[nodiscard]] constexpr auto is_cjk_trailing_punct(char32_t code) noexcept -> bool {
  switch (code) {
    case U'\u3001':
    case U'\u3002':
    case U'\u300b':
    case U'\u300d':
    case U'\u300f':
    case U'\u3011':
    case U'\uff01':
    case U'\uff09':
    case U'\uff0c':
    case U'\uff1a':
    case U'\uff1b':
    case U'\uff1f':
      return true;
    default:
      return false;
  }
}

/// 文本首词（围栏信息串取语言标注用）。
[[nodiscard]] auto first_token(std::string_view text) -> std::string {
  const std::string_view trimmed = st::trim(text);
  std::size_t stop = 0;
  while (stop < trimmed.size() && !is_blank_char(trimmed[stop])) ++stop;
  return std::string(trimmed.substr(0, stop));
}

/// 尾部是否为完整 UTF-8 序列（流式增量提交的前置条件：半个字符不算定型）。
[[nodiscard]] auto utf8_complete_at_end(std::string_view text) noexcept -> bool {
  std::size_t remaining = text.size();
  std::size_t steps = 0;
  while (remaining > 0 && steps < 4) {
    const auto byte = static_cast<unsigned char>(text[remaining - 1]);
    if ((byte & 0xC0U) != 0x80U) {
      std::size_t expected = 1;
      if (byte >= 0xF0U && byte <= 0xF7U) {
        expected = 4;
      } else if (byte >= 0xE0U && byte <= 0xEFU) {
        expected = 3;
      } else if (byte >= 0xC0U && byte <= 0xDFU) {
        expected = 2;
      }
      return steps + 1 >= expected;
    }
    --remaining;
    ++steps;
  }
  return true;
}

// ============================ 行模型 ============================

/// 源文本的一行；容器剥离后仍保留绝对行号与字节偏移。
struct Line {
  std::string_view text{};  ///< 行内容（`\r\n` 的 `\r` 已去除）
  std::size_t offset{0};    ///< 行首在本次解析文本中的字节偏移
  std::size_t number{0};    ///< 绝对行号（0 起）
  bool terminated{false};   ///< 是否带行结束符
};

/// 切分为行：`\n` 与 `\r\n` 均视为行结束符；末尾无换行的残行 terminated=false（流式未完成）。
[[nodiscard]] auto split_lines(std::string_view text, std::size_t base_line) -> std::vector<Line> {
  std::vector<Line> lines;
  std::size_t begin = 0;
  std::size_t number = base_line;
  while (begin < text.size()) {
    const std::size_t stop = text.find('\n', begin);
    if (stop == npos) {
      lines.push_back(Line{text.substr(begin), begin, number, false});
      break;
    }
    std::string_view content = text.substr(begin, stop - begin);
    if (!content.empty() && content.back() == '\r') content.remove_suffix(1);
    lines.push_back(Line{content, begin, number, true});
    begin = stop + 1;
    ++number;
  }
  return lines;
}

// ============================ 块级构造识别 ============================

struct HeadingInfo {
  std::uint32_t level{0};
  std::string_view content{};
};

/// ATX 标题：`#{1,6}` + 空白（或行尾），可选闭合序列（` ##`）。
[[nodiscard]] auto atx_heading(std::string_view text) -> std::optional<HeadingInfo> {
  const std::size_t indent = leading_spaces(text);
  if (indent > 3) return std::nullopt;
  const std::string_view rest = text.substr(indent);
  std::size_t level = 0;
  while (level < rest.size() && rest[level] == '#') ++level;
  if (level == 0 || level > 6) return std::nullopt;
  if (level < rest.size() && !is_blank_char(rest[level])) return std::nullopt;
  std::string_view content = st::trim(rest.substr(level));
  std::size_t cut = content.size();
  while (cut > 0 && content[cut - 1] == '#') --cut;
  if (cut < content.size()) {
    if (cut == 0) {
      content = std::string_view{};
    } else if (is_blank_char(content[cut - 1])) {
      content = st::trim_end(content.substr(0, cut));
    }
  }
  return HeadingInfo{static_cast<std::uint32_t>(level), content};
}

struct FenceInfo {
  char marker{0};
  std::size_t length{0};
  std::size_t indent{0};
  std::string_view info{};
};

/// 围栏开始行：``` 或 ~~~（≥3，缩进 ≤3；反引号信息串内不得含反引号）。
[[nodiscard]] auto fence_open(std::string_view text) -> std::optional<FenceInfo> {
  const std::size_t indent = leading_spaces(text);
  if (indent > 3) return std::nullopt;
  const std::string_view rest = text.substr(indent);
  if (rest.size() < 3) return std::nullopt;
  const char marker = rest[0];
  if (marker != '`' && marker != '~') return std::nullopt;
  std::size_t length = 0;
  while (length < rest.size() && rest[length] == marker) ++length;
  if (length < 3) return std::nullopt;
  const std::string_view info = rest.substr(length);
  if (marker == '`' && info.find('`') != npos) return std::nullopt;
  return FenceInfo{marker, length, indent, st::trim(info)};
}

/// 围栏结束行：同字符、长度 ≥ 开始行、其后仅空白。
[[nodiscard]] auto fence_close(std::string_view text, const FenceInfo& open) -> bool {
  const std::size_t indent = leading_spaces(text);
  if (indent > 3) return false;
  const std::string_view rest = text.substr(indent);
  std::size_t length = 0;
  while (length < rest.size() && rest[length] == open.marker) ++length;
  if (length < open.length) return false;
  return is_blank_text(rest.substr(length));
}

/// 水平分隔线：`---` / `***` / `___`（≥3，允许中间空白，缩进 ≤3）。
[[nodiscard]] auto is_divider(std::string_view text) -> bool {
  const std::size_t indent = leading_spaces(text);
  if (indent > 3) return false;
  const std::string_view rest = text.substr(indent);
  if (rest.empty()) return false;
  const char marker = rest[0];
  if (marker != '-' && marker != '*' && marker != '_') return false;
  std::size_t count = 0;
  for (const char raw : rest) {
    if (raw == marker) {
      ++count;
    } else if (!is_blank_char(raw)) {
      return false;
    }
  }
  return count >= 3;
}

/// 引用行（缩进 ≤3 且以 `>` 开头）。
[[nodiscard]] auto starts_quote(std::string_view text) -> bool {
  const std::size_t indent = leading_spaces(text);
  if (indent > 3) return false;
  const std::string_view rest = text.substr(indent);
  return !rest.empty() && rest[0] == '>';
}

struct ListMarker {
  bool ordered{false};
  char delimiter{0};           ///< 无序为项目符号字符，有序为 `.` / `)`
  std::size_t indent{0};       ///< 标记前缩进（列数）
  std::size_t width{0};        ///< 标记本身宽度（字符数）
  std::size_t content_indent{0};  ///< 内容缩进列数（标记缩进 + 宽度 + 后随空白，1..4）
};

/// 列表标记：`-`/`+`/`*` 与 `1.`/`1)`（缩进 ≤3）。
[[nodiscard]] auto list_marker(std::string_view text) -> std::optional<ListMarker> {
  const std::size_t indent = leading_spaces(text);
  if (indent > 3) return std::nullopt;
  const std::string_view rest = text.substr(indent);
  if (rest.empty()) return std::nullopt;

  const char first = rest[0];
  if (first == '-' || first == '+' || first == '*') {
    if (rest.size() > 1 && !is_blank_char(rest[1])) return std::nullopt;
    const bool has_gap = rest.size() > 1;
    const std::size_t gap = has_gap ? leading_blank_chars(rest.substr(1)) : 0;
    const std::size_t step = (has_gap && gap <= 4) ? (gap == 0 ? 1 : gap) : 1;
    return ListMarker{false, first, indent, 1, indent + 1 + step};
  }

  if (is_digit(first)) {
    std::size_t digits = 0;
    while (digits < rest.size() && is_digit(rest[digits])) ++digits;
    if (digits == 0 || digits > 9 || digits >= rest.size()) return std::nullopt;
    const char delimiter = rest[digits];
    if (delimiter != '.' && delimiter != ')') return std::nullopt;
    const std::size_t after = digits + 1;
    if (after < rest.size() && !is_blank_char(rest[after])) return std::nullopt;
    const bool has_gap = after < rest.size();
    const std::size_t gap = has_gap ? leading_blank_chars(rest.substr(after)) : 0;
    const std::size_t step = (has_gap && gap <= 4) ? (gap == 0 ? 1 : gap) : 1;
    return ListMarker{true, delimiter, indent, after, indent + after + step};
  }
  return std::nullopt;
}

/// HTML 块起始行：缩进 ≤3 且以 `<` + 字母 / `/` / `!` / `?` 开头。
[[nodiscard]] auto starts_html_block(std::string_view text) -> bool {
  const std::size_t indent = leading_spaces(text);
  if (indent > 3) return false;
  const std::string_view rest = text.substr(indent);
  if (rest.size() < 2 || rest[0] != '<') return false;
  const char next = rest[1];
  return is_alpha(next) || next == '/' || next == '!' || next == '?';
}

/// 该行是否开始一个新的块级构造（段落中断 / 惰性续行判定用）。
[[nodiscard]] auto starts_block_construct(std::string_view text) -> bool {
  return atx_heading(text).has_value() || fence_open(text).has_value() || is_divider(text) ||
         list_marker(text).has_value() || starts_quote(text) || starts_html_block(text);
}

// ============================ 表格 ============================

/// 按未转义的 `|` 切分表格行，去掉首尾因行首/行尾竖线产生的空单元格，单元格已 trim。
[[nodiscard]] auto split_table_cells(std::string_view text) -> std::vector<std::string> {
  std::vector<std::string> cells;
  std::string current;
  bool escaped = false;
  for (const char raw : text) {
    if (escaped) {
      if (raw == '|' || raw == '\\') {
        current.push_back(raw);
      } else {
        current.push_back('\\');
        current.push_back(raw);
      }
      escaped = false;
      continue;
    }
    if (raw == '\\') {
      escaped = true;
      continue;
    }
    if (raw == '|') {
      cells.push_back(std::string(st::trim(current)));
      current.clear();
      continue;
    }
    current.push_back(raw);
  }
  if (escaped) current.push_back('\\');
  cells.push_back(std::string(st::trim(current)));
  if (!cells.empty() && cells.front().empty()) cells.erase(cells.begin());
  if (!cells.empty() && cells.back().empty()) cells.pop_back();
  return cells;
}

/// GFM 对齐行：单元格须形如 `:?-+:?`，单元格数须与表头一致。
[[nodiscard]] auto table_alignment(std::string_view text, std::size_t expected_cells) -> bool {
  if (text.find('|') == npos) return false;
  const std::vector<std::string> cells = split_table_cells(text);
  if (cells.empty() || cells.size() != expected_cells) return false;
  for (const std::string& cell : cells) {
    const std::string_view view = st::trim(cell);
    if (view.empty()) return false;
    std::size_t index = 0;
    if (view[index] == ':') ++index;
    std::size_t dashes = 0;
    while (index < view.size() && view[index] == '-') {
      ++index;
      ++dashes;
    }
    if (dashes == 0) return false;
    if (index < view.size() && view[index] == ':') ++index;
    if (index != view.size()) return false;
  }
  return true;
}

/// 该行是否是"表头行 + 下一行对齐行"的表格起点。
[[nodiscard]] auto table_start(const std::vector<Line>& lines, std::size_t index, std::size_t last) -> bool {
  if (index + 1 >= last) return false;
  if (lines[index].text.find('|') == npos) return false;
  const std::vector<std::string> header = split_table_cells(lines[index].text);
  if (header.empty()) return false;
  return table_alignment(lines[index + 1].text, header.size());
}

// ============================ 行内解析 ============================

[[nodiscard]] auto parse_inlines(std::string_view text) -> std::vector<Inline>;

/// 把行内序列扁平为纯文本（表格单元格 / 强调内层 / 图片 alt 用）。
[[nodiscard]] auto flatten_inlines(const std::vector<Inline>& inlines) -> std::string {
  std::string out;
  for (const Inline& node : inlines) {
    switch (node.kind) {
      case InlineKind::SoftBreak:
      case InlineKind::HardBreak:
        out.push_back(' ');
        break;
      default:
        out.append(node.text);
        break;
    }
  }
  return out;
}

/// 解析一段 Markdown 行内文本并扁平为纯文本（递归用）。
[[nodiscard]] auto flatten_markdown(std::string_view text) -> std::string {
  return flatten_inlines(parse_inlines(text));
}

/// 行内词法扫描器：一趟线性扫描，遇到未闭合构造即退化为字面文本。
class InlineLexer {
 public:
  explicit InlineLexer(std::string_view text) noexcept : text_(text) {}

  [[nodiscard]] auto run() -> std::vector<Inline> {
    while (pos_ < text_.size()) {
      const char raw = text_[pos_];
      switch (raw) {
        case '\\':
          if (escape()) continue;
          break;
        case '\n':
          break_line();
          continue;
        case '`':
          if (code_span()) continue;
          break;
        case '*':
        case '_':
          if (emphasis(raw)) continue;
          break;
        case '~':
          if (strikethrough()) continue;
          break;
        case '!':
          if (text_.size() > pos_ + 1 && text_[pos_ + 1] == '[' && link_with(pos_ + 1, true)) continue;
          break;
        case '[':
          if (link_with(pos_, false)) continue;
          break;
        case '<':
          if (angle_autolink()) continue;
          break;
        case 'h':
        case 'w':
        case 'm':
          if (bare_autolink()) continue;
          break;
        default:
          break;
      }
      buffer_.push_back(raw);
      ++pos_;
    }
    flush();
    return std::move(out_);
  }

 private:
  [[nodiscard]] auto is_space_or_newline(char raw) const noexcept -> bool {
    return raw == ' ' || raw == '\t' || raw == '\n';
  }

  void flush() {
    if (buffer_.empty()) return;
    Inline node;
    node.kind = InlineKind::Text;
    node.text = std::move(buffer_);
    buffer_.clear();
    out_.push_back(std::move(node));
  }

  void emit(InlineKind kind, std::string text, std::string url = {}, std::string title = {}) {
    flush();
    Inline node;
    node.kind = kind;
    node.text = std::move(text);
    node.url = std::move(url);
    node.title = std::move(title);
    out_.push_back(std::move(node));
  }

  void emit_break(InlineKind kind) {
    flush();
    Inline node;
    node.kind = kind;
    out_.push_back(std::move(node));
  }

  /// 反斜杠转义 / 行尾反斜杠硬换行。
  [[nodiscard]] auto escape() -> bool {
    if (pos_ + 1 >= text_.size()) return false;
    const char next = text_[pos_ + 1];
    if (next == '\n') {
      emit_break(InlineKind::HardBreak);
      pos_ += 2;
      skip_line_indent();
      return true;
    }
    if (is_escapable(next)) {
      buffer_.push_back(next);
      pos_ += 2;
      return true;
    }
    return false;
  }

  /// 行内换行：行尾 ≥2 空格为硬换行，否则软换行（行尾空格与下一行前导空白丢弃）。
  void break_line() {
    std::size_t spaces = 0;
    while (spaces < buffer_.size() && buffer_[buffer_.size() - 1 - spaces] == ' ') ++spaces;
    const bool hard = spaces >= 2;
    buffer_.resize(buffer_.size() - spaces);
    emit_break(hard ? InlineKind::HardBreak : InlineKind::SoftBreak);
    ++pos_;
    skip_line_indent();
  }

  void skip_line_indent() {
    while (pos_ < text_.size() && is_blank_char(text_[pos_])) ++pos_;
  }

  /// 行内代码；未找到等长反引号串则退化为字面文本。
  [[nodiscard]] auto code_span() -> bool {
    std::size_t run = 0;
    while (pos_ + run < text_.size() && text_[pos_ + run] == '`') ++run;
    const std::size_t content_begin = pos_ + run;
    std::size_t cursor = content_begin;
    while (cursor < text_.size()) {
      if (text_[cursor] != '`') {
        ++cursor;
        continue;
      }
      std::size_t close_run = 0;
      while (cursor + close_run < text_.size() && text_[cursor + close_run] == '`') ++close_run;
      if (close_run == run) {
        std::string_view content = text_.substr(content_begin, cursor - content_begin);
        if (content.size() >= 2 && content.front() == ' ' && content.back() == ' ' && !is_blank_text(content)) {
          content = content.substr(1, content.size() - 2);
        }
        emit(InlineKind::Code, std::string(content));
        pos_ = cursor + close_run;
        return true;
      }
      cursor += close_run;
    }
    return false;
  }

  /// 查找闭合分隔符：内容非空、首字符非空白、闭合符前一字符非空白；`_` 另需词边界。
  [[nodiscard]] auto find_closing(std::size_t content_begin, char marker, std::size_t width) const -> std::size_t {
    if (content_begin >= text_.size() || is_space_or_newline(text_[content_begin])) return npos;
    std::size_t cursor = content_begin;
    while (cursor < text_.size()) {
      if (text_[cursor] != marker) {
        ++cursor;
        continue;
      }
      std::size_t run = 0;
      while (cursor + run < text_.size() && text_[cursor + run] == marker) ++run;
      const bool flanking = cursor > content_begin && !is_space_or_newline(text_[cursor - 1]);
      const bool word_boundary =
          marker != '_' || cursor + run >= text_.size() || (!is_alnum(text_[cursor + run]) && text_[cursor + run] != '_');
      if (run >= width && flanking && word_boundary) return cursor;
      cursor += run;
    }
    return npos;
  }

  /// 强调 / 加粗：优先两字符（Strong），否则单字符（Emphasis）。
  [[nodiscard]] auto emphasis(char marker) -> bool {
    std::size_t run = 0;
    while (pos_ + run < text_.size() && text_[pos_ + run] == marker) ++run;
    if (marker == '_') {
      if (pos_ > 0 && (is_alnum(text_[pos_ - 1]) || text_[pos_ - 1] == '_')) return false;
      if (pos_ + run < text_.size() && (is_alnum(text_[pos_ + run]) || text_[pos_ + run] == '_')) return false;
    }
    if (run >= 2) {
      const std::size_t close = find_closing(pos_ + 2, marker, 2);
      if (close != npos) {
        emit(InlineKind::Strong, flatten_markdown(text_.substr(pos_ + 2, close - (pos_ + 2))));
        pos_ = close + 2;
        return true;
      }
    }
    const std::size_t close = find_closing(pos_ + 1, marker, 1);
    if (close != npos) {
      emit(InlineKind::Emphasis, flatten_markdown(text_.substr(pos_ + 1, close - (pos_ + 1))));
      pos_ = close + 1;
      return true;
    }
    return false;
  }

  /// 删除线（GFM `~~x~~`）。
  [[nodiscard]] auto strikethrough() -> bool {
    if (pos_ + 1 >= text_.size() || text_[pos_ + 1] != '~') return false;
    const std::size_t close = find_closing(pos_ + 2, '~', 2);
    if (close == npos) return false;
    emit(InlineKind::Strikethrough, flatten_markdown(text_.substr(pos_ + 2, close - (pos_ + 2))));
    pos_ = close + 2;
    return true;
  }

  /// `[label](target "title")` 的闭合 `]`（支持一层嵌套方括号与转义）。
  [[nodiscard]] auto find_matching_bracket(std::size_t open) const -> std::size_t {
    std::size_t depth = 0;
    std::size_t cursor = open;
    while (cursor < text_.size()) {
      const char raw = text_[cursor];
      if (raw == '\\') {
        cursor += 2;
        continue;
      }
      if (raw == '[') {
        ++depth;
      } else if (raw == ']') {
        if (depth == 0) return npos;
        --depth;
        if (depth == 0) return cursor;
      }
      ++cursor;
    }
    return npos;
  }

  /// `(target "title")` 的闭合 `)`（支持括号嵌套与转义）。
  [[nodiscard]] auto find_matching_paren(std::size_t open) const -> std::size_t {
    std::size_t depth = 0;
    std::size_t cursor = open;
    while (cursor < text_.size()) {
      const char raw = text_[cursor];
      if (raw == '\\') {
        cursor += 2;
        continue;
      }
      if (raw == '\n') return npos;
      if (raw == '(') {
        ++depth;
      } else if (raw == ')') {
        if (depth == 0) return npos;
        --depth;
        if (depth == 0) return cursor;
      }
      ++cursor;
    }
    return npos;
  }

  /// 拆解链接目标：`url` 或 `url "title"`，尖括号包裹的 URL 去壳。
  static void split_target(std::string_view inner, std::string& url, std::string& title) {
    std::string_view view = st::trim(inner);
    url.clear();
    title.clear();
    if (view.empty()) return;
    const char last = view.back();
    if (last == '"' || last == '\'') {
      const std::size_t open_quote = view.rfind(last, view.size() >= 2 ? view.size() - 2 : 0);
      if (open_quote != npos && open_quote > 0 && is_blank_char(view[open_quote - 1])) {
        title.assign(view.substr(open_quote + 1, view.size() - open_quote - 2));
        view = st::trim_end(view.substr(0, open_quote));
      }
    }
    if (view.size() >= 2 && view.front() == '<' && view.back() == '>') {
      view = view.substr(1, view.size() - 2);
    }
    url.assign(view);
  }

  /// 链接 / 图片（`open` 指向 `[`）。
  [[nodiscard]] auto link_with(std::size_t open, bool is_image) -> bool {
    const std::size_t close = find_matching_bracket(open);
    if (close == npos) return false;
    if (close + 1 >= text_.size() || text_[close + 1] != '(') return false;
    const std::size_t paren = find_matching_paren(close + 1);
    if (paren == npos) return false;
    std::string url;
    std::string title;
    split_target(text_.substr(close + 2, paren - (close + 2)), url, title);
    emit(is_image ? InlineKind::Image : InlineKind::Link,
         flatten_markdown(text_.substr(open + 1, close - (open + 1))), std::move(url), std::move(title));
    pos_ = paren + 1;
    return true;
  }

  [[nodiscard]] static auto looks_like_url(std::string_view text) -> bool {
    const std::size_t colon = text.find(':');
    if (colon == npos || colon == 0 || colon + 1 >= text.size()) return false;
    if (!is_alpha(text[0])) return false;
    for (std::size_t index = 1; index < colon; ++index) {
      const char raw = text[index];
      if (!is_alnum(raw) && raw != '+' && raw != '-' && raw != '.') return false;
    }
    return true;
  }

  [[nodiscard]] static auto looks_like_email(std::string_view text) -> bool {
    const std::size_t at = text.find('@');
    if (at == npos || at == 0 || at + 1 >= text.size()) return false;
    const std::size_t dot = text.find('.', at + 1);
    return dot != npos && dot + 1 < text.size();
  }

  /// `<http://…>` / `<mailto:…>` / `<a@b.c>` 自动链接；行内 HTML 标签退化为文本。
  [[nodiscard]] auto angle_autolink() -> bool {
    const std::size_t stop = text_.find('>', pos_ + 1);
    if (stop == npos) return false;
    const std::string_view inner = text_.substr(pos_ + 1, stop - (pos_ + 1));
    if (inner.empty() || inner.find(' ') != npos || inner.find('\n') != npos || inner.find('<') != npos) return false;
    if (looks_like_url(inner)) {
      emit(InlineKind::Link, std::string(inner), std::string(inner));
      pos_ = stop + 1;
      return true;
    }
    if (looks_like_email(inner)) {
      emit(InlineKind::Link, std::string(inner), "mailto:" + std::string(inner));
      pos_ = stop + 1;
      return true;
    }
    return false;
  }

  /// 裸自动链接（GFM）：`http://` / `https://` / `www.` 起始，尾部标点不入链接。
  [[nodiscard]] auto bare_autolink() -> bool {
    if (pos_ > 0) {
      const char previous = text_[pos_ - 1];
      if (is_alnum(previous) || previous == '_' || previous == '/' || previous == ':') return false;
    }
    std::string_view prefix{};
    for (const std::string_view candidate : kBarePrefixes) {
      if (text_.size() - pos_ >= candidate.size() && st::ascii_starts_with_ci(text_.substr(pos_), candidate)) {
        prefix = candidate;
        break;
      }
    }
    if (prefix.empty()) return false;
    std::size_t stop = pos_;
    while (stop < text_.size()) {
      const char raw = text_[stop];
      if (is_blank_char(raw) || raw == '\n' || raw == '<' || raw == '>' || raw == '"') break;
      ++stop;
    }
    while (stop > pos_ && is_trailing_punct(text_[stop - 1])) --stop;
    stop = trim_url_tail(pos_, stop);
    if (stop <= pos_ + prefix.size()) return false;
    const std::string literal(text_.substr(pos_, stop - pos_));
    std::string url = literal;
    if (st::ascii_iequals(prefix, "www.")) url = "http://" + literal;
    emit(InlineKind::Link, literal, std::move(url));
    pos_ = stop;
    return true;
  }

  /// 裁剪 URL 尾部标点（ASCII 标点 + 中日文全角标点），返回新的结束位置。
  [[nodiscard]] auto trim_url_tail(std::size_t begin, std::size_t stop) const -> std::size_t {
    while (stop > begin) {
      const char last = text_[stop - 1];
      if (static_cast<unsigned char>(last) < 0x80U) {
        if (!is_trailing_punct(last)) break;
        --stop;
        continue;
      }
      std::size_t start = stop - 1;
      while (start > begin && (static_cast<unsigned char>(text_[start]) & 0xC0U) == 0x80U) --start;
      std::size_t index = start;
      const st::Codepoint code = st::decode_utf8(text_, index);
      if (code.bytes == 0 || !is_cjk_trailing_punct(code.value)) break;
      stop = start;
    }
    return stop;
  }

  [[nodiscard]] static auto is_trailing_punct(char raw) noexcept -> bool {
    switch (raw) {
      case '.': case ',': case ';': case ':': case '!': case '?': case ')': case ']': case '}':
      case '"': case '\'': case '*': case '_': case '~':
        return true;
      default:
        return false;
    }
  }

  inline static constexpr std::array<std::string_view, 3> kBarePrefixes{"http://", "https://", "www."};

  std::string_view text_{};
  std::size_t pos_{0};
  std::string buffer_{};
  std::vector<Inline> out_{};
};

[[nodiscard]] auto parse_inlines(std::string_view text) -> std::vector<Inline> {
  InlineLexer lexer(text);
  return lexer.run();
}

// ============================ 块级解析 ============================

/// 顶层解析状态：记录"最后一块在输入末尾是否仍未定型"及起始偏移（流式增量提交用）。
struct ParseState {
  bool last_open{false};
  std::size_t last_offset{0};
};

/// 非 owning 出参：仅顶层调用传入，容器递归传 `nullptr`（局部不变量，无所有权）。
void record_block(ParseState* top, const Line& line, bool open) {
  if (top == nullptr) return;
  top->last_open = open;
  top->last_offset = line.offset;
}

[[nodiscard]] auto parse_blocks(const std::vector<Line>& lines, std::size_t first, std::size_t last,
                                std::uint32_t depth, ParseState* top) -> std::vector<Block>;

struct TaskMarker {
  bool found{false};
  bool checked{false};
};

/// 就地剥离 GFM 任务标记（`[ ]` / `[x]`），返回是否命中与勾选态。
[[nodiscard]] auto strip_task_marker(std::vector<Line>& lines) -> TaskMarker {
  for (Line& line : lines) {
    if (is_blank_text(line.text)) continue;
    const std::string_view text = line.text;
    if (text.size() < 3 || text[0] != '[' || text[2] != ']') return TaskMarker{};
    const char mark = text[1];
    if (mark != ' ' && mark != 'x' && mark != 'X') return TaskMarker{};
    if (text.size() > 3 && !is_blank_char(text[3])) return TaskMarker{};
    std::size_t removed = 3;
    while (removed < text.size() && is_blank_char(text[removed])) ++removed;
    line.offset += removed;
    line.text = text.substr(removed);
    return TaskMarker{true, mark != ' '};
  }
  return TaskMarker{};
}

/// 列表项复用 `BlockKind::Paragraph` 承载行内序列，其余块（含嵌套列表）挂在 `children`。
[[nodiscard]] auto make_list_item(std::vector<Line>& item_lines, std::size_t start_line, std::uint32_t depth) -> Block {
  const TaskMarker task = strip_task_marker(item_lines);
  Block item;
  item.kind = BlockKind::Paragraph;
  item.level = depth;
  item.task_item = task.found;
  item.checked = task.checked;
  item.start_line = static_cast<std::uint64_t>(start_line);
  std::vector<Block> children = parse_blocks(item_lines, 0, item_lines.size(), depth + 1, nullptr);
  if (!children.empty() && children.front().kind == BlockKind::Paragraph) {
    item.inlines = std::move(children.front().inlines);
    children.erase(children.begin());
  }
  item.children = std::move(children);
  return item;
}

[[nodiscard]] auto parse_blocks(const std::vector<Line>& lines, std::size_t first, std::size_t last,
                                std::uint32_t depth, ParseState* top) -> std::vector<Block> {
  std::vector<Block> blocks;
  std::size_t index = first;
  while (index < last) {
    const Line& line = lines[index];
    if (is_blank_text(line.text)) {
      ++index;
      continue;
    }

    // —— ATX 标题 ——
    if (const auto heading = atx_heading(line.text); heading.has_value()) {
      Block block;
      block.kind = BlockKind::Heading;
      block.level = heading->level;
      block.inlines = parse_inlines(heading->content);
      block.start_line = static_cast<std::uint64_t>(line.number);
      blocks.push_back(std::move(block));
      record_block(top, line, false);
      ++index;
      continue;
    }

    // —— 围栏代码块（``` / ~~~，未闭合则内容到输入末尾） ——
    if (const auto fence = fence_open(line.text); fence.has_value()) {
      std::size_t cursor = index + 1;
      bool closed = false;
      while (cursor < last) {
        if (fence_close(lines[cursor].text, *fence)) {
          closed = true;
          break;
        }
        ++cursor;
      }
      Block block;
      block.kind = BlockKind::CodeBlock;
      block.language = first_token(fence->info);
      block.start_line = static_cast<std::uint64_t>(line.number);
      for (std::size_t inner = index + 1; inner < cursor; ++inner) {
        block.code.append(strip_columns(lines[inner].text, fence->indent));
        if (lines[inner].terminated) block.code.push_back('\n');
      }
      blocks.push_back(std::move(block));
      record_block(top, line, !closed);
      index = closed ? cursor + 1 : cursor;
      continue;
    }

    // —— 水平分隔线 ——
    if (is_divider(line.text)) {
      Block block;
      block.kind = BlockKind::Divider;
      block.start_line = static_cast<std::uint64_t>(line.number);
      blocks.push_back(std::move(block));
      record_block(top, line, false);
      ++index;
      continue;
    }

    // —— 引用（支持嵌套与段落惰性续行） ——
    if (starts_quote(line.text)) {
      std::vector<Line> inner;
      std::size_t cursor = index;
      while (cursor < last) {
        const Line& current = lines[cursor];
        const std::size_t indent = leading_spaces(current.text);
        const std::string_view rest = current.text.substr(indent);
        if (indent <= 3 && !rest.empty() && rest[0] == '>') {
          std::string_view content = rest.substr(1);
          if (!content.empty() && is_blank_char(content[0])) content.remove_prefix(1);
          const std::size_t shift = current.text.size() - content.size();
          inner.push_back(Line{content, current.offset + shift, current.number, current.terminated});
          ++cursor;
          continue;
        }
        if (is_blank_text(current.text)) {
          std::size_t look = cursor + 1;
          while (look < last && is_blank_text(lines[look].text)) ++look;
          if (look < last && starts_quote(lines[look].text)) {
            inner.push_back(Line{std::string_view{}, current.offset, current.number, current.terminated});
            ++cursor;
            continue;
          }
          break;
        }
        if (starts_block_construct(current.text)) break;
        inner.push_back(Line{current.text, current.offset, current.number, current.terminated});
        ++cursor;
      }
      Block block;
      block.kind = BlockKind::Quote;
      block.start_line = static_cast<std::uint64_t>(line.number);
      block.children = parse_blocks(inner, 0, inner.size(), depth, nullptr);
      blocks.push_back(std::move(block));
      record_block(top, line, cursor >= last);
      index = cursor;
      continue;
    }

    // —— GFM 表格 ——
    if (table_start(lines, index, last)) {
      std::vector<std::string> header = split_table_cells(line.text);
      std::vector<std::vector<std::string>> raw_rows;
      std::size_t cursor = index + 2;
      while (cursor < last && !is_blank_text(lines[cursor].text) && lines[cursor].text.find('|') != npos) {
        std::vector<std::string> cells = split_table_cells(lines[cursor].text);
        cells.resize(header.size());
        raw_rows.push_back(std::move(cells));
        ++cursor;
      }
      Block block;
      block.kind = BlockKind::Table;
      block.start_line = static_cast<std::uint64_t>(line.number);
      for (const std::string& cell : header) block.header.push_back(flatten_markdown(cell));
      for (const std::vector<std::string>& row : raw_rows) {
        std::vector<std::string> flat;
        flat.reserve(row.size());
        for (const std::string& cell : row) flat.push_back(flatten_markdown(cell));
        block.rows.push_back(std::move(flat));
      }
      blocks.push_back(std::move(block));
      record_block(top, line, cursor >= last);
      index = cursor;
      continue;
    }

    // —— 列表（可嵌套、可含多段、支持任务项） ——
    if (const auto marker = list_marker(line.text); marker.has_value()) {
      const bool ordered = marker->ordered;
      const char delimiter = marker->delimiter;
      const std::size_t list_indent = marker->indent;  // 本层列表标记的基础缩进（更深的标记属于项内容）
      Block list;
      list.kind = BlockKind::List;
      list.ordered = ordered;
      list.level = depth;
      list.start_line = static_cast<std::uint64_t>(line.number);

      std::vector<Line> item_lines;
      std::size_t item_start = line.number;
      std::size_t item_indent = marker->content_indent;
      std::size_t cursor = index;
      while (cursor < last) {
        const Line& current = lines[cursor];
        if (is_blank_text(current.text)) {
          std::size_t look = cursor + 1;
          while (look < last && is_blank_text(lines[look].text)) ++look;
          if (look >= last) break;
          const auto next_marker = list_marker(lines[look].text);
          const bool sibling = next_marker.has_value() && next_marker->ordered == ordered &&
                               (ordered || next_marker->delimiter == delimiter) &&
                               next_marker->indent <= list_indent;
          if (sibling || indent_columns(lines[look].text) >= item_indent) {
            item_lines.push_back(Line{std::string_view{}, current.offset, current.number, current.terminated});
            ++cursor;
            continue;
          }
          break;
        }
        const auto current_marker = list_marker(current.text);
        const bool sibling = current_marker.has_value() && current_marker->ordered == ordered &&
                             (ordered || current_marker->delimiter == delimiter) &&
                             current_marker->indent <= list_indent;
        if (sibling) {
          if (!item_lines.empty()) {
            list.children.push_back(make_list_item(item_lines, item_start, depth));
            item_lines.clear();
          }
          item_start = current.number;
          item_indent = current_marker->content_indent;
          const std::string_view remainder = strip_columns(current.text, item_indent);
          item_lines.push_back(Line{remainder, current.offset + (current.text.size() - remainder.size()),
                                    current.number, current.terminated});
          ++cursor;
          continue;
        }
        if (!item_lines.empty() && indent_columns(current.text) >= item_indent) {
          const std::string_view remainder = strip_columns(current.text, item_indent);
          item_lines.push_back(Line{remainder, current.offset + (current.text.size() - remainder.size()),
                                    current.number, current.terminated});
          ++cursor;
          continue;
        }
        if (!item_lines.empty() && !starts_block_construct(current.text)) {
          item_lines.push_back(Line{current.text, current.offset, current.number, current.terminated});
          ++cursor;
          continue;
        }
        break;
      }
      if (!item_lines.empty()) list.children.push_back(make_list_item(item_lines, item_start, depth));
      blocks.push_back(std::move(list));
      record_block(top, line, true);
      index = cursor;
      continue;
    }

    // —— 缩进代码块 ——
    if (indent_columns(line.text) >= 4) {
      std::string code;
      std::size_t cursor = index;
      while (cursor < last) {
        const Line& current = lines[cursor];
        if (is_blank_text(current.text)) {
          std::size_t look = cursor + 1;
          while (look < last && is_blank_text(lines[look].text)) ++look;
          if (look < last && indent_columns(lines[look].text) >= 4) {
            code.push_back('\n');
            cursor = look;
            continue;
          }
          break;
        }
        if (indent_columns(current.text) < 4) break;
        code.append(strip_columns(current.text, 4));
        code.push_back('\n');
        ++cursor;
      }
      Block block;
      block.kind = BlockKind::CodeBlock;
      block.code = std::move(code);
      block.start_line = static_cast<std::uint64_t>(line.number);
      blocks.push_back(std::move(block));
      record_block(top, line, cursor >= last);
      index = cursor;
      continue;
    }

    // —— HTML 块（原样保留到空行） ——
    if (starts_html_block(line.text)) {
      std::string fragment;
      std::size_t cursor = index;
      while (cursor < last && !is_blank_text(lines[cursor].text)) {
        fragment.append(lines[cursor].text);
        if (lines[cursor].terminated) fragment.push_back('\n');
        ++cursor;
      }
      Block block;
      block.kind = BlockKind::HtmlBlock;
      block.code = std::move(fragment);
      block.start_line = static_cast<std::uint64_t>(line.number);
      blocks.push_back(std::move(block));
      record_block(top, line, cursor >= last);
      index = cursor;
      continue;
    }

    // —— 段落（被空行或块级构造中断；GFM 表格可中断段落） ——
    {
      std::string text;
      std::size_t cursor = index;
      while (cursor < last) {
        const Line& current = lines[cursor];
        if (is_blank_text(current.text)) break;
        if (cursor > index && (starts_block_construct(current.text) || table_start(lines, cursor, last))) break;
        text.append(current.text);
        text.push_back('\n');
        ++cursor;
      }
      if (!text.empty() && text.back() == '\n') text.pop_back();
      Block block;
      block.kind = BlockKind::Paragraph;
      block.inlines = parse_inlines(text);
      block.start_line = static_cast<std::uint64_t>(line.number);
      blocks.push_back(std::move(block));
      record_block(top, line, cursor >= last);
      index = cursor;
    }
  }
  return blocks;
}

}  // namespace

// ============================ 对外入口 ============================

auto detail::parse_incremental(std::string_view markdown, std::size_t base_line) -> detail::ParseResult {
  detail::ParseResult result;
  const std::vector<Line> lines = split_lines(markdown, base_line);
  ParseState state;
  result.blocks = parse_blocks(lines, 0, lines.size(), 0, &state);

  // 定型判定：末尾行完整（含 UTF-8 完整）+ 最后一块已闭合 + 不是可跨空行续接的容器
  // （列表/引用保留重解析：`- a\n\n- b` 与 `> a\n\n> b` 的续接判定依赖后续行）。
  bool sealed = false;
  if (!result.blocks.empty()) {
    const bool complete = !lines.empty() && lines.back().terminated && utf8_complete_at_end(markdown);
    const BlockKind kind = result.blocks.back().kind;
    const bool container = kind == BlockKind::List || kind == BlockKind::Quote;
    sealed = complete && !state.last_open && !container;
  }

  if (sealed) {
    result.sealed_blocks = result.blocks.size();
    result.sealed_bytes = markdown.size();
  } else if (result.blocks.empty()) {
    // 纯空行：可将已完结的行提交，但**末尾残行不得跨过**——
    // 它可能是下一个块（如缩进代码块）的开头，切在行中会把缩进丢掉。
    result.sealed_blocks = 0;
    result.sealed_bytes = (lines.empty() || lines.back().terminated) ? markdown.size() : lines.back().offset;
  } else {
    result.sealed_blocks = result.blocks.size() - 1;
    result.sealed_bytes = std::min(state.last_offset, markdown.size());
  }
  return result;
}

auto parse(std::string_view markdown) -> std::vector<Block> {
  return detail::parse_incremental(markdown, 0).blocks;
}

}  // namespace st::md

#include "st/text/highlight.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <initializer_list>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "st/core/string.hpp"

namespace st::text {
namespace {

inline constexpr std::size_t npos = std::string_view::npos;

// ============================ 字符分类 ============================

[[nodiscard]] constexpr auto is_digit(char raw) noexcept -> bool {
  return raw >= '0' && raw <= '9';
}

[[nodiscard]] constexpr auto is_hex_digit(char raw) noexcept -> bool {
  return is_digit(raw) || (raw >= 'a' && raw <= 'f') || (raw >= 'A' && raw <= 'F');
}

[[nodiscard]] constexpr auto is_ident_start(char raw) noexcept -> bool {
  return (raw >= 'a' && raw <= 'z') || (raw >= 'A' && raw <= 'Z') || raw == '_' ||
         static_cast<unsigned char>(raw) >= 0x80U;
}

[[nodiscard]] constexpr auto is_ident_continue(char raw) noexcept -> bool {
  return is_ident_start(raw) || is_digit(raw);
}

[[nodiscard]] constexpr auto is_operator_char(char raw) noexcept -> bool {
  switch (raw) {
    case '+': case '-': case '*': case '/': case '%': case '=': case '<': case '>':
    case '!': case '&': case '|': case '^': case '~': case '?': case ':':
      return true;
    default:
      return false;
  }
}

[[nodiscard]] constexpr auto is_punct_char(char raw) noexcept -> bool {
  switch (raw) {
    case '(': case ')': case '[': case ']': case '{': case '}': case ',': case ';': case '.':
      return true;
    default:
      return false;
  }
}

/// 忽略大小写比较（`case_insensitive` 语言用；ASCII 范围）。
[[nodiscard]] auto equal_fold(std::string_view a, std::string_view b) noexcept -> bool {
  if (a.size() != b.size()) return false;
  for (std::size_t index = 0; index < a.size(); ++index) {
    const char lhs = a[index];
    const char rhs = b[index];
    const char lowered_lhs = (lhs >= 'A' && lhs <= 'Z') ? static_cast<char>(lhs + 32) : lhs;
    const char lowered_rhs = (rhs >= 'A' && rhs <= 'Z') ? static_cast<char>(rhs + 32) : rhs;
    if (lowered_lhs != lowered_rhs) return false;
  }
  return true;
}

/// 词表命中判定：表按字典序排好 → 二分；`fold` 时忽略大小写。
[[nodiscard]] auto in_table(std::span<const std::string> table, std::string_view word, bool fold)
    -> bool {
  if (table.empty() || word.empty()) return false;
  if (!fold) {
    return std::ranges::binary_search(table, word) ||
           std::ranges::find(table, word) != table.end();
  }
  return std::ranges::any_of(table, [word](const std::string& item) {
    return equal_fold(item, word);
  });
}

// ============================ 词法扫描器 ============================

/// 单趟线性扫描：输出**升序、互不重叠**的 token 序列（未着色字节为 `Plain`）。
///
/// 与"语法分析"无关，只做词法；因此对半截代码、混合语言内嵌片段（HTML 里的 CSS/JS）也不会失败——
/// 这正是编辑器能"边打字边高亮"的前提。
class Lexer {
 public:
  Lexer(std::string_view code, const LanguageSpec& spec) noexcept : code_(code), spec_(spec) {}

  [[nodiscard]] auto run() -> std::vector<Token> {
    if (spec_.diff_like) return run_diff();
    if (spec_.markdown_like) return run_markdown();
    while (pos_ < code_.size()) {
      if (!step()) break;
    }
    flush_plain(pos_);
    return std::move(out_);
  }

 private:
  // —— 通用扫描 ——

  [[nodiscard]] auto matches(std::string_view text) const -> bool {
    return code_.size() - pos_ >= text.size() && code_.compare(pos_, text.size(), text) == 0;
  }

  [[nodiscard]] auto at_line_start() const -> bool {
    std::size_t index = pos_;
    while (index > 0) {
      const char raw = code_[index - 1];
      if (raw == '\n') return true;
      if (raw != ' ' && raw != '\t') return false;
      --index;
    }
    return true;
  }

  void flush_plain(std::size_t stop) {
    if (stop > plain_begin_) push(TokenKind::Plain, plain_begin_, stop);
    plain_begin_ = stop;
  }

  void push(TokenKind kind, std::size_t begin, std::size_t end) {
    if (end <= begin) return;
    if (!out_.empty() && out_.back().kind == kind && out_.back().end == begin) {
      out_.back().end = end;
      return;
    }
    out_.push_back(Token{kind, begin, end});
  }

  void emit(TokenKind kind, std::size_t begin, std::size_t end) {
    flush_plain(begin);
    push(kind, begin, end);
    plain_begin_ = end;
  }

  /// 取一个字符（不消费）。
  [[nodiscard]] auto peek(std::size_t offset) const -> char {
    return pos_ + offset < code_.size() ? code_[pos_ + offset] : '\0';
  }

  [[nodiscard]] auto has(std::string_view text) const noexcept -> bool {
    return !text.empty();
  }

  /// 一次词法判定；返回 false 表示已到末尾（提前结束）。
  auto step() -> bool {
    const char raw = code_[pos_];
    if (raw == ' ' || raw == '\t' || raw == '\n' || raw == '\r' || raw == '\f' || raw == '\v') {
      ++pos_;
      return true;
    }
    // 注释
    if (has(spec_.line_comment) && matches(spec_.line_comment)) {
      line_comment();
      return true;
    }
    if (has(spec_.line_comment_alt) && matches(spec_.line_comment_alt)) {
      line_comment();
      return true;
    }
    if (has(spec_.block_comment_begin) && has(spec_.block_comment_end) &&
        matches(spec_.block_comment_begin)) {
      block_comment();
      return true;
    }
    // 预处理
    if (spec_.preprocessor && raw == '#' && at_line_start()) {
      line_of(TokenKind::Preprocessor, true);
      return true;
    }
    // 标记语言标签
    if (has(spec_.tag_begin) && matches(spec_.tag_begin)) {
      tag();
      return true;
    }
    // 带前缀的原始字符串（`R"(`、`r#"`、`f"`、`@"`…）
    for (const std::string& prefix : spec_.string_prefixes) {
      if (!has(prefix) || !matches(prefix)) continue;
      prefixed_string(prefix);
      return true;
    }
    // 字符串
    if (spec_.triple_quote && (matches("\"\"\"") || matches("'''"))) {
      delimited_string(matches("\"\"\"") ? "\"\"\"" : "'''");
      return true;
    }
    if (raw == '"' && spec_.double_quote) {
      const std::size_t begin = pos_;
      quoted_string('"');
      reclassify_string_key(begin);
      return true;
    }
    if (raw == '\'' && spec_.single_quote) {
      const std::size_t begin = pos_;
      quoted_string('\'');
      reclassify_string_key(begin);
      return true;
    }
    if (raw == '`' && spec_.backtick_string) {
      quoted_string('`');
      return true;
    }
    // 属性/装饰器（`@Override`、`@media`）
    if (has(spec_.attribute_prefix) && matches(spec_.attribute_prefix)) {
      attribute();
      return true;
    }
    // 数字
    if (is_digit(raw) || (raw == '.' && is_digit(peek(1)))) {
      number();
      return true;
    }
    // 标识符
    if (is_ident_start(raw) || (spec_.css_like && (raw == '#' || raw == '.' || raw == '-') &&
                                is_ident_start(peek(1)))) {
      identifier();
      return true;
    }
    if (is_operator_char(raw)) {
      run_of(TokenKind::Operator, is_operator_char);
      return true;
    }
    if (is_punct_char(raw)) {
      run_of(TokenKind::Punctuation, is_punct_char);
      return true;
    }
    ++pos_;
    return true;
  }

  // —— 各类词法单元 ——

  /// 从当前位置吃到行尾（可选：吞掉行尾续行符 `\` 连起的下一行）。
  void line_of(TokenKind kind, bool join_continuations) {
    const std::size_t begin = pos_;
    while (pos_ < code_.size() && code_[pos_] != '\n') ++pos_;
    if (join_continuations) {
      while (pos_ > begin && code_[pos_ - 1] == '\\' && pos_ < code_.size() &&
             code_[pos_] == '\n') {
        ++pos_;
        while (pos_ < code_.size() && code_[pos_] != '\n') ++pos_;
      }
    }
    emit(kind, begin, pos_);
  }

  void line_comment() { line_of(TokenKind::Comment, false); }

  void block_comment() {
    const std::size_t begin = pos_;
    pos_ += spec_.block_comment_begin.size();
    const std::size_t stop = code_.find(spec_.block_comment_end, pos_);
    pos_ = stop == npos ? code_.size() : stop + spec_.block_comment_end.size();
    emit(TokenKind::Comment, begin, pos_);
  }

  /// 定界字符串（三引号 / 原始串）：可跨行，未闭合则到文末。
  void delimited_string(std::string_view delimiter) {
    const std::size_t begin = pos_;
    pos_ += delimiter.size();
    const std::size_t stop = code_.find(delimiter, pos_);
    pos_ = stop == npos ? code_.size() : stop + delimiter.size();
    emit(TokenKind::String, begin, pos_);
  }

  /// 带前缀的原始字符串：`前缀` 形如 `R"(` / `r#"` / `f"` / `@"`。
  /// 收尾定界按「引号前 `#` 的逆序 + 引号」推导（`R"(` → `)"`；`r#"` → `"#`；`f"` → `"`）。
  void prefixed_string(const std::string& prefix) {
    const std::size_t begin = pos_;
    std::size_t quote_index = prefix.size();
    while (quote_index > 0 && prefix[quote_index - 1] != '"' && prefix[quote_index - 1] != '\'') {
      --quote_index;
    }
    if (quote_index == 0) {
      // 前缀里没有引号（配置错误）→ 按普通标识符处理，避免吞掉整份代码
      ++pos_;
      return;
    }
    const char quote = prefix[quote_index - 1];
    std::size_t hashes = 0;
    while (quote_index >= 1 + hashes + 1 && prefix[quote_index - 1 - 1 - hashes] == '#') ++hashes;
    std::string closing;
    closing.append(hashes, '#');
    closing.push_back(quote);
    pos_ += prefix.size();
    const std::size_t stop = code_.find(closing, pos_);
    pos_ = stop == npos ? code_.size() : stop + closing.size();
    emit(TokenKind::String, begin, pos_);
  }

  /// 引号串；`multiline_quoted` 为真时可跨行（模板串），否则未闭合止于行尾。
  void quoted_string(char quote) {
    const std::size_t begin = pos_;
    const bool escapes = quote == '\'' ? spec_.escape_in_single : true;
    ++pos_;
    while (pos_ < code_.size()) {
      const char raw = code_[pos_];
      if (escapes && raw == '\\') {
        pos_ += 2;
        continue;
      }
      if (raw == quote) {
        ++pos_;
        break;
      }
      if (raw == '\n' && !spec_.multiline_quoted) break;
      ++pos_;
    }
    if (pos_ > code_.size()) pos_ = code_.size();
    emit(TokenKind::String, begin, pos_);
  }

  /// 标记语言标签：`<name attr="v">` —— 名字按 `Tag`、属性名按 `Attribute`、值按 `String`。
  void tag() {
    const std::size_t begin = pos_;
    emit(TokenKind::Punctuation, begin, begin + spec_.tag_begin.size());
    pos_ += spec_.tag_begin.size();
    // `/` 与 `!`/`?`（注释、声明）前缀
    while (pos_ < code_.size() && (code_[pos_] == '/' || code_[pos_] == '!' || code_[pos_] == '?')) {
      ++pos_;
    }
    // 标签名
    const std::size_t name_begin = pos_;
    while (pos_ < code_.size() && (is_ident_continue(code_[pos_]) || code_[pos_] == '-' ||
                                   code_[pos_] == ':')) {
      ++pos_;
    }
    if (pos_ > name_begin) emit(TokenKind::Tag, name_begin, pos_);
    // 标签内部：属性名 / 字符串值 / `=`
    while (pos_ < code_.size() && !matches(spec_.tag_end)) {
      const char raw = code_[pos_];
      if (raw == '"' || raw == '\'') {
        quoted_string(raw);
        continue;
      }
      if (is_ident_start(raw)) {
        const std::size_t attr_begin = pos_;
        while (pos_ < code_.size() && (is_ident_continue(code_[pos_]) || code_[pos_] == '-' ||
                                       code_[pos_] == ':')) {
          ++pos_;
        }
        emit(TokenKind::Attribute, attr_begin, pos_);
        continue;
      }
      if (raw == '=') {
        ++pos_;
        emit(TokenKind::Operator, pos_ - 1, pos_);
        continue;
      }
      if (raw == ' ' || raw == '\t' || raw == '\n' || raw == '\r') {
        ++pos_;
        continue;
      }
      ++pos_;
    }
    if (pos_ < code_.size() && matches(spec_.tag_end)) {
      const std::size_t mark = pos_;
      pos_ += spec_.tag_end.size();
      emit(TokenKind::Punctuation, mark, pos_);
    }
  }

  /// 属性/装饰器：前缀符（`@`）起，其后标识符按 `Attribute` 着色。
  void attribute() {
    const std::size_t begin = pos_;
    pos_ += spec_.attribute_prefix.size();
    // 属性名允许带命名空间点与连字符（`@kotlin.jvm.JvmStatic`、`@data-test`）
    const std::size_t name_begin = pos_;
    while (pos_ < code_.size() && (is_ident_continue(code_[pos_]) || code_[pos_] == '.' ||
                                   code_[pos_] == '-')) {
      ++pos_;
    }
    if (pos_ == name_begin) {
      // 前缀后不是名字（如 Rust 的 `#[derive]`）：只把前缀按运算符着色，别把后面的结构吞掉
      emit(TokenKind::Operator, begin, pos_);
      return;
    }
    emit(TokenKind::Attribute, begin, pos_);
  }

  /// 引号串后若紧跟键分隔符（`:` / `=`），把它从 String 改写为 Key（JSON/YAML/TOML 的对象键）。
  void reclassify_string_key(std::size_t begin) {
    if (!spec_.key_value_keys) return;
    if (!next_is_key_separator()) return;
    if (out_.empty() || out_.back().begin != begin) return;
    out_.back().kind = TokenKind::Key;
  }

  void number() {
    const std::size_t begin = pos_;
    const bool radix_prefix =
        code_[pos_] == '0' && pos_ + 1 < code_.size() &&
        (code_[pos_ + 1] == 'x' || code_[pos_ + 1] == 'X' || code_[pos_ + 1] == 'b' ||
         code_[pos_ + 1] == 'B' || code_[pos_ + 1] == 'o' || code_[pos_ + 1] == 'O');
    if (radix_prefix) {
      pos_ += 2;
      while (pos_ < code_.size() && (is_hex_digit(code_[pos_]) || code_[pos_] == '_')) ++pos_;
    } else {
      while (pos_ < code_.size() && (is_digit(code_[pos_]) || code_[pos_] == '_')) ++pos_;
      if (pos_ + 1 < code_.size() && code_[pos_] == '.' && is_digit(code_[pos_ + 1])) {
        ++pos_;
        while (pos_ < code_.size() && (is_digit(code_[pos_]) || code_[pos_] == '_')) ++pos_;
      }
      if (pos_ < code_.size() && (code_[pos_] == 'e' || code_[pos_] == 'E')) {
        const std::size_t exponent_begin = pos_;
        ++pos_;
        if (pos_ < code_.size() && (code_[pos_] == '+' || code_[pos_] == '-')) ++pos_;
        if (pos_ < code_.size() && is_digit(code_[pos_])) {
          while (pos_ < code_.size() && is_digit(code_[pos_])) ++pos_;
        } else {
          pos_ = exponent_begin;
        }
      }
    }
    // 数值后缀（`f`/`u`/`l`/`i32u8`）与 `#hex` 颜色一并吞掉，避免半截着色
    while (pos_ < code_.size() && (is_ident_continue(code_[pos_]) || code_[pos_] == '.')) {
      if (code_[pos_] == '.' && !(pos_ + 1 < code_.size() && is_digit(code_[pos_ + 1]))) break;
      ++pos_;
    }
    emit(TokenKind::Number, begin, pos_);
  }

  /// 标识符 → 关键字/类型/内建/属性/键/函数名；都不匹配则保持 `Plain`。
  void identifier() {
    const std::size_t begin = pos_;
    // CSS 风格的选择器：`#id` / `.class` 前导符号
    if (spec_.css_like && (code_[pos_] == '#' || code_[pos_] == '.')) ++pos_;
    while (pos_ < code_.size() && (is_ident_continue(code_[pos_]) ||
                                   (spec_.css_like && code_[pos_] == '-'))) {
      ++pos_;
    }
    const std::string_view word = code_.substr(begin, pos_ - begin);
    const bool fold = spec_.case_insensitive;

    if (in_table(spec_.keywords, word, fold) || in_table(spec_.definition_keywords, word, fold)) {
      // 定义关键字（`fn`/`def`/`class`）即使在关键字表里缺席也按关键字着色：
      // 它同时是"其后标识符是函数/类型名"的判定依据（见 `follows_definition_keyword`）。
      emit(TokenKind::Keyword, begin, pos_);
      last_keyword_ = word;
      last_keyword_end_ = pos_;
      return;
    }
    last_keyword_ = {};
    if (in_table(spec_.types, word, fold)) {
      emit(TokenKind::Type, begin, pos_);
      return;
    }
    if (in_table(spec_.builtins, word, fold)) {
      emit(TokenKind::Builtin, begin, pos_);
      return;
    }
    if (spec_.key_value_keys && next_is_key_separator()) {
      emit(TokenKind::Key, begin, pos_);
      return;
    }
    if (spec_.function_call && (next_is_paren() || follows_definition_keyword(begin))) {
      emit(TokenKind::Function, begin, pos_);
      return;
    }
  }

  [[nodiscard]] auto next_is_paren() const -> bool {
    std::size_t index = pos_;
    while (index < code_.size() && (code_[index] == ' ' || code_[index] == '\t')) ++index;
    return index < code_.size() && code_[index] == '(';
  }

  /// 下一个非空白字符是否为键分隔符（`:` / `=`）——JSON/YAML/TOML/INI 的键判定。
  [[nodiscard]] auto next_is_key_separator() const -> bool {
    std::size_t index = pos_;
    while (index < code_.size() && (code_[index] == ' ' || code_[index] == '\t')) ++index;
    if (index >= code_.size()) return false;
    return code_[index] == ':' || code_[index] == '=';
  }

  [[nodiscard]] auto follows_definition_keyword(std::size_t begin) const -> bool {
    if (last_keyword_.empty() || last_keyword_end_ > begin) return false;
    for (std::size_t index = last_keyword_end_; index < begin; ++index) {
      if (code_[index] != ' ' && code_[index] != '\t') return false;
    }
    return in_table(spec_.definition_keywords, last_keyword_, spec_.case_insensitive);
  }

  void run_of(TokenKind kind, auto predicate) {
    const std::size_t begin = pos_;
    while (pos_ < code_.size() && predicate(code_[pos_])) ++pos_;
    emit(kind, begin, pos_);
  }

  // —— Markdown 风格（行级标记 + 行内强调）——

  [[nodiscard]] auto run_markdown() -> std::vector<Token> {
    while (pos_ < code_.size()) {
      const std::size_t line_begin = pos_;
      const std::size_t line_end = [this]() {
        const std::size_t stop = code_.find('\n', pos_);
        return stop == npos ? code_.size() : stop;
      }();
      const std::string_view line = code_.substr(line_begin, line_end - line_begin);
      const std::string_view trimmed = [line]() {
        std::size_t index = 0;
        while (index < line.size() && (line[index] == ' ' || line[index] == '\t')) ++index;
        return line.substr(index);
      }();
      const bool fence = trimmed.starts_with("```") || trimmed.starts_with("~~~");
      if (fence) {
        pos_ = line_end;
        emit(TokenKind::Keyword, line_begin, line_end);
      } else if (trimmed.starts_with("#")) {
        pos_ = line_end;
        emit(TokenKind::Keyword, line_begin, line_end);
      } else if (trimmed.starts_with(">")) {
        pos_ = line_end;
        emit(TokenKind::Comment, line_begin, line_end);
      } else if (trimmed.starts_with("- ") || trimmed.starts_with("* ") ||
                 trimmed.starts_with("+ ") || trimmed.starts_with("- [") ||
                 trimmed.starts_with("* [")) {
        pos_ = line_end;
        emit(TokenKind::Builtin, line_begin, line_end);
      } else if (trimmed.starts_with("---") || trimmed.starts_with("***")) {
        pos_ = line_end;
        emit(TokenKind::Punctuation, line_begin, line_end);
      } else {
        inline_markdown(line_end);
      }
      if (pos_ < code_.size() && code_[pos_] == '\n') ++pos_;
    }
    flush_plain(pos_);
    return std::move(out_);
  }

  void inline_markdown(std::size_t line_end) {
    while (pos_ < line_end) {
      const char raw = code_[pos_];
      if (raw == '`') {
        const std::size_t begin = pos_;
        const std::size_t stop = code_.find('`', pos_ + 1);
        pos_ = (stop == npos || stop > line_end) ? line_end : stop + 1;
        emit(TokenKind::String, begin, pos_);
        continue;
      }
      if (raw == '*' || raw == '_') {
        std::size_t count = 0;
        while (pos_ + count < line_end && code_[pos_ + count] == raw) ++count;
        const std::string_view marker = code_.substr(pos_, count);
        const std::size_t stop = code_.find(marker, pos_ + count);
        if (stop != npos && stop < line_end) {
          const std::size_t begin = pos_;
          pos_ = stop + count;
          emit(TokenKind::Type, begin, pos_);
          continue;
        }
      }
      if (raw == '[') {
        const std::size_t stop = code_.find("](", pos_);
        if (stop != npos && stop < line_end) {
          const std::size_t begin = pos_;
          const std::size_t close = code_.find(')', stop + 2);
          pos_ = close == npos || close > line_end ? line_end : close + 1;
          emit(TokenKind::Function, begin, pos_);
          continue;
        }
      }
      ++pos_;
    }
  }

  // —— Diff 风格（整行着色）——

  [[nodiscard]] auto run_diff() -> std::vector<Token> {
    while (pos_ < code_.size()) {
      const std::size_t line_begin = pos_;
      const std::size_t stop = code_.find('\n', pos_);
      const std::size_t line_end = stop == npos ? code_.size() : stop;
      const std::string_view line = code_.substr(line_begin, line_end - line_begin);
      TokenKind kind = TokenKind::Plain;
      if (line.starts_with("+++") || line.starts_with("---") || line.starts_with("@@") ||
          line.starts_with("diff ") || line.starts_with("index ")) {
        kind = TokenKind::Keyword;
      } else if (line.starts_with("+")) {
        kind = TokenKind::Inserted;
      } else if (line.starts_with("-")) {
        kind = TokenKind::Deleted;
      }
      if (kind != TokenKind::Plain) emit(kind, line_begin, line_end);
      pos_ = line_end;
      if (pos_ < code_.size() && code_[pos_] == '\n') ++pos_;
    }
    flush_plain(pos_);
    return std::move(out_);
  }

  std::string_view code_{};
  const LanguageSpec& spec_;
  std::size_t pos_{0};
  std::size_t plain_begin_{0};
  std::string_view last_keyword_{};
  std::size_t last_keyword_end_{0};
  std::vector<Token> out_{};
};

/// 规范名比较（注册表用）。
[[nodiscard]] auto same_name(std::string_view a, std::string_view b) noexcept -> bool {
  return equal_fold(st::trim(a), st::trim(b));
}

/// 词表规范化：去空、去重、按字典序排序（供二分查找）；`fold` 时统一小写。
void normalize_table(std::vector<std::string>& table, bool fold) {
  std::erase_if(table, [](const std::string& item) { return item.empty(); });
  if (fold) {
    for (auto& item : table) item = st::ascii_lower(item);
  }
  std::ranges::sort(table);
  table.erase(std::unique(table.begin(), table.end()), table.end());
}

void normalize_spec(LanguageSpec& spec) {
  spec.name = st::ascii_lower(st::trim(spec.name));
  for (auto& alias : spec.aliases) alias = st::ascii_lower(st::trim(alias));
  std::erase_if(spec.aliases, [](const std::string& item) { return item.empty(); });
  std::ranges::sort(spec.aliases);
  spec.aliases.erase(std::unique(spec.aliases.begin(), spec.aliases.end()), spec.aliases.end());

  const bool fold = spec.case_insensitive;
  normalize_table(spec.keywords, fold);
  normalize_table(spec.types, fold);
  normalize_table(spec.builtins, fold);
  normalize_table(spec.definition_keywords, fold);
  std::ranges::sort(spec.string_prefixes);
}

[[nodiscard]] auto spec_matches(const LanguageSpec& spec, std::string_view query) -> bool {
  if (same_name(spec.name, query)) return true;
  return std::ranges::any_of(spec.aliases, [query](const std::string& alias) {
    return same_name(alias, query);
  });
}

}  // namespace

// ============================ 注册表 ============================

struct LanguageRegistry::Impl {
  mutable std::shared_mutex mutex{};
  std::vector<std::shared_ptr<const LanguageSpec>> specs{};
};

LanguageRegistry::LanguageRegistry() : impl_(std::make_shared<Impl>()) {}

auto LanguageRegistry::register_language(LanguageSpec spec, bool replace) -> bool {
  normalize_spec(spec);
  if (spec.name.empty()) return false;
  auto stored = std::make_shared<const LanguageSpec>(std::move(spec));
  const std::scoped_lock lock(impl_->mutex);
  const auto existing = std::ranges::find_if(impl_->specs, [&stored](const auto& item) {
    return spec_matches(*item, stored->name);
  });
  if (existing != impl_->specs.end()) {
    if (!replace) return false;
    *existing = std::move(stored);
    // 覆盖后可能与其他语言的别名冲突：清掉那些以同一名字命名的旧条目（保留最新注册者）
    return true;
  }
  impl_->specs.push_back(std::move(stored));
  return true;
}

auto LanguageRegistry::unregister(std::string_view name) -> bool {
  const std::scoped_lock lock(impl_->mutex);
  const auto removed = std::erase_if(impl_->specs, [name](const auto& item) {
    return spec_matches(*item, name);
  });
  return removed > 0;
}

auto LanguageRegistry::find(std::string_view name_or_alias) const
    -> std::shared_ptr<const LanguageSpec> {
  if (st::trim(name_or_alias).empty()) return {};
  const std::shared_lock lock(impl_->mutex);
  for (const auto& spec : impl_->specs) {
    if (spec_matches(*spec, name_or_alias)) return spec;
  }
  return {};
}

auto LanguageRegistry::names() const -> std::vector<std::string> {
  const std::shared_lock lock(impl_->mutex);
  std::vector<std::string> out;
  out.reserve(impl_->specs.size());
  for (const auto& spec : impl_->specs) out.push_back(spec->name);
  std::ranges::sort(out);
  return out;
}

auto LanguageRegistry::specs() const -> std::vector<std::shared_ptr<const LanguageSpec>> {
  const std::shared_lock lock(impl_->mutex);
  std::vector<std::shared_ptr<const LanguageSpec>> out = impl_->specs;
  std::ranges::sort(out, [](const auto& a, const auto& b) { return a->name < b->name; });
  return out;
}

auto LanguageRegistry::size() const -> std::size_t {
  const std::shared_lock lock(impl_->mutex);
  return impl_->specs.size();
}

void LanguageRegistry::clear() {
  const std::scoped_lock lock(impl_->mutex);
  impl_->specs.clear();
}

void LanguageRegistry::reset_to_builtin() {
  std::vector<std::shared_ptr<const LanguageSpec>> fresh;
  for (auto& spec : builtin_languages()) {
    normalize_spec(spec);
    fresh.push_back(std::make_shared<const LanguageSpec>(std::move(spec)));
  }
  const std::scoped_lock lock(impl_->mutex);
  impl_->specs = std::move(fresh);
}

auto global_languages() -> LanguageRegistry& {
  static LanguageRegistry registry = [] {
    LanguageRegistry instance;
    instance.reset_to_builtin();
    return instance;
  }();
  return registry;
}

// ============================ 顶层 API ============================

auto highlight_with(std::string_view code, const LanguageSpec& spec) -> std::vector<Token> {
  if (code.empty()) return std::vector<Token>{Token{TokenKind::Plain, 0, 0}};
  Lexer lexer(code, spec);
  return lexer.run();
}

auto highlight(std::string_view code, std::string_view language) -> std::vector<Token> {
  const auto spec = global_languages().find(language);
  if (!spec) return std::vector<Token>{Token{TokenKind::Plain, 0, code.size()}};
  return highlight_with(code, *spec);
}

auto language_from_path(std::string_view path) -> std::optional<std::string> {
  const std::string_view trimmed = st::trim(path);
  if (trimmed.empty()) return std::nullopt;
  const std::size_t slash = trimmed.find_last_of("/\\");
  const std::string_view base = slash == npos ? trimmed : trimmed.substr(slash + 1);
  // 无扩展名的常见文件名（优先按全名匹配）
  static constexpr std::array kByBaseName{
      std::pair{"makefile", "makefile"},      std::pair{"gnumakefile", "makefile"},
      std::pair{"dockerfile", "dockerfile"},  std::pair{"containerfile", "dockerfile"},
      std::pair{"cmakelists.txt", "cmake"},   std::pair{"cargo.toml", "toml"},
      std::pair{".gitignore", "ini"},         std::pair{".env", "ini"},
      std::pair{"changelog.md", "markdown"},
  };
  const std::string lowered_base = st::ascii_lower(base);
  for (const auto& [key, language] : kByBaseName) {
    if (lowered_base == key) {
      auto spec = global_languages().find(language);
      if (spec) return spec->name;
    }
  }
  const std::size_t dot = base.find_last_of('.');
  if (dot == npos || dot + 1 >= base.size()) return std::nullopt;
  const std::string extension = st::ascii_lower(base.substr(dot + 1));
  if (const auto spec = global_languages().find(extension); spec) return spec->name;
  return std::nullopt;
}

auto supported_languages() -> std::vector<std::string> {
  std::vector<std::string> names;
  for (const auto& spec : global_languages().specs()) {
    names.push_back(spec->name);
    for (const auto& alias : spec->aliases) names.push_back(alias);
  }
  std::ranges::sort(names);
  names.erase(std::unique(names.begin(), names.end()), names.end());
  return names;
}

// ============================ 便捷构造 ============================

auto make_language(std::string name) -> LanguageSpec {
  LanguageSpec spec;
  spec.name = std::move(name);
  return spec;
}

namespace {

auto append_words(std::vector<std::string>& target, std::initializer_list<std::string_view> words)
    -> void {
  for (const std::string_view word : words) target.emplace_back(word);
}

}  // namespace

auto with_keywords(LanguageSpec& spec, std::initializer_list<std::string_view> words) -> LanguageSpec& {
  append_words(spec.keywords, words);
  return spec;
}

auto with_types(LanguageSpec& spec, std::initializer_list<std::string_view> words) -> LanguageSpec& {
  append_words(spec.types, words);
  return spec;
}

auto with_builtins(LanguageSpec& spec, std::initializer_list<std::string_view> words) -> LanguageSpec& {
  append_words(spec.builtins, words);
  return spec;
}

auto with_definition_keywords(LanguageSpec& spec, std::initializer_list<std::string_view> words)
    -> LanguageSpec& {
  append_words(spec.definition_keywords, words);
  return spec;
}

auto with_aliases(LanguageSpec& spec, std::initializer_list<std::string_view> names) -> LanguageSpec& {
  for (const std::string_view name : names) spec.aliases.emplace_back(name);
  return spec;
}

auto with_c_comments(LanguageSpec& spec) -> LanguageSpec& {
  spec.line_comment = "//";
  spec.block_comment_begin = "/*";
  spec.block_comment_end = "*/";
  return spec;
}

auto with_line_comment(LanguageSpec& spec, std::string_view marker) -> LanguageSpec& {
  spec.line_comment = std::string(marker);
  return spec;
}

auto with_block_comment(LanguageSpec& spec, std::string_view begin, std::string_view end)
    -> LanguageSpec& {
  spec.block_comment_begin = std::string(begin);
  spec.block_comment_end = std::string(end);
  return spec;
}

auto with_tags(LanguageSpec& spec, std::string_view begin, std::string_view end) -> LanguageSpec& {
  spec.tag_begin = std::string(begin);
  spec.tag_end = std::string(end);
  return spec;
}

}  // namespace st::text

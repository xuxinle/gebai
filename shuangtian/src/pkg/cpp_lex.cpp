/// `st::pkg::lex` 的实现：C++ 词法净化（去注释 / 去字符串内容）。
///
/// 状态机的逐条规则见 `include/st/pkg/cpp_lex.hpp` 的 `lex_line` 注释。
/// 这里只放实现，不重复解释——两份说明必然漂移。

#include "st/pkg/cpp_lex.hpp"

namespace st::pkg::lex {

namespace {

/// 裸串终结判定：当前字符起是否正好是 `)` + 定界符 + `"`（无定界符时就是 `)"`）。
[[nodiscard]] auto raw_terminator_at(std::string_view line, const RawStringState& raw,
                                     std::size_t position) -> bool {
  if (line[position] != ')') return false;
  const std::size_t after_paren = position + 1;
  const std::size_t quote_at = after_paren + raw.delimiter.size();
  if (quote_at >= line.size()) return false;  // 本行放不下终结符（继续当内容）
  if (line.compare(after_paren, raw.delimiter.size(), raw.delimiter) != 0) return false;
  return line[quote_at] == '"';
}

}  // namespace

auto lex_line(std::string_view line, State& state) -> LexedLine {
  LexedLine out;
  out.uncommented.reserve(line.size());
  out.code.reserve(line.size());
  bool in_string = false;
  bool in_char = false;

  for (std::size_t index = 0; index < line.size(); ++index) {
    const char current = line[index];
    const char next = index + 1 < line.size() ? line[index + 1] : '\0';

    // ① 块注释中
    if (state.in_block_comment) {
      if (current == '*' && next == '/') {
        state.in_block_comment = false;
        ++index;
      }
      continue;
    }

    // ② 裸字符串中：内容只进 `uncommented`（`//` 在此**不是**行注释）
    if (state.raw.active) {
      if (raw_terminator_at(line, state.raw, index)) {
        const std::string terminator = ")" + state.raw.delimiter + "\"";
        out.uncommented += terminator;
        out.code += terminator;
        index += state.raw.delimiter.size() + 1;
        state.raw.active = false;
        state.raw.delimiter.clear();
        continue;
      }
      out.uncommented.push_back(current);
      continue;
    }

    // ③ 普通字符串 / 字符中
    if (in_string || in_char) {
      out.uncommented.push_back(current);
      if (current == '\\' && next != '\0') {
        out.uncommented.push_back(next);
        out.code.append("  ");
        ++index;
        continue;
      }
      if ((in_string && current == '"') || (in_char && current == '\'')) {
        in_string = false;
        in_char = false;
        out.code.push_back(current);
        continue;
      }
      out.code.push_back(' ');
      continue;
    }

    // ④ 行注释：本行剩余部分丢弃
    if (current == '/' && next == '/') break;
    if (current == '/' && next == '*') {
      state.in_block_comment = true;
      ++index;
      continue;
    }

    // ⑤ 裸字符串开头：`R"<定界符>(`。定界符最长 16 字符（C++ 标准上限），
    //    且不得含空格/括号/反斜杠——遇到这些字符说明不是裸串开头。
    if (current == 'R' && next == '"') {
      const std::size_t delimiter_begin = index + 2;
      std::size_t probe = delimiter_begin;
      while (probe < line.size() && probe - delimiter_begin <= 16U && line[probe] != '(' &&
             line[probe] != ' ' && line[probe] != ')' && line[probe] != '\\') {
        ++probe;
      }
      if (probe < line.size() && line[probe] == '(') {
        state.raw.active = true;
        state.raw.delimiter = std::string(line.substr(delimiter_begin, probe - delimiter_begin));
        const std::string opener = "R\"" + state.raw.delimiter + "(";
        out.uncommented += opener;
        out.code += opener;
        index = probe;
        continue;
      }
      // 非法定界符：退回普通字符串起头（那行编译时会报错，这里只保证状态不乱）
      in_string = true;
      out.uncommented.append("R\"");
      out.code.append("R\"");
      ++index;
      continue;
    }

    if (current == '"') {
      in_string = true;
      out.uncommented.push_back(current);
      out.code.push_back(current);
      continue;
    }
    if (current == '\'') {
      in_char = true;
      out.uncommented.push_back(current);
      out.code.push_back(current);
      continue;
    }

    // ⑥ 普通代码字符
    out.uncommented.push_back(current);
    out.code.push_back(current);
  }
  return out;
}

auto strip_to_code(std::string_view source) -> std::string {
  std::string out;
  out.reserve(source.size());
  State state;
  std::size_t begin = 0;
  while (begin <= source.size()) {
    const std::size_t end = source.find('\n', begin);
    const std::string_view line =
        source.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
    out += lex_line(line, state).code;
    if (end == std::string_view::npos) break;
    out.push_back('\n');
    begin = end + 1;
  }
  return out;
}

}  // namespace st::pkg::lex

#include "st/pkg/lint.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <regex>
#include <string>

#include "st/core/fs.hpp"
#include "st/core/string.hpp"

namespace st::pkg {
namespace {

struct RuleSpec {
  std::string_view id;
  std::string_view description;
  std::string_view pattern;
  bool advisory{false};
};

/// 规则表（与 `CONVENTIONS.md` §8 一一对应）。
constexpr std::array<RuleSpec, 12> kRules{{
    {"L1", "禁止裸 new/delete/malloc/free（用 unique_ptr/RAII/容器）",
     R"(\bnew\s|\bdelete\s|\bmalloc\s*\(|\bfree\s*\(|\brealloc\s*\()"},
    {"L2", "禁止 C 风格强制转换（用 static_cast/bit_cast）",
     // 前置负向约束用「前一个字符」表达（ECMAScript 正则无 lookbehind）：
     // 只有出现在 行首/空白/（=,;{( 之后的 `(类型)` 才是 C 转换；
     // `std::function<void(std::size_t)>` 这类模板实参、`foo(size_t)` 这类调用实参不得误报。
     R"((?:^|[\s=(,;{!&|?:])\(\s*(?:unsigned\s+|signed\s+)?(?:int|char|short|long|float|double|size_t|bool)\s*\*?\s*\))"},
    {"L3", "禁止函数式宏（用 constexpr/consteval/if constexpr/concepts）",
     R"(^\s*#\s*define\s+\w+\s*\()"},
    {"L4", "禁止 goto/setjmp/longjmp（用结构化控制流）", R"(\bgoto\b|\bsetjmp\b|\blongjmp\b)"},
    {"L5", "禁止 throw 作业务错误（用 Result/unexpected）", R"(\bthrow\b)"},
    {"L6", "禁止 reinterpret_cast（平台/SIMD 边界单点封装除外）", R"(\breinterpret_cast\b)"},
    {"L7", "禁止 printf/sprintf（用 std::format）", R"(\bprintf\s*\(|\bsprintf\s*\()"},
        // L8 需要作用域判断（只看单行会把函数内局部变量全部误报），
    // 由 `check_mutable_globals()` 单独实现；此处仅登记元数据，`pattern` 留空表示"不走正则"。
    {"L8", "禁止可变的进程级全局状态（显式注入 Context）", ""},
    {"L9", "禁止裸 mutex 操作与线程 detach（用 scoped_lock/jthread）",
     R"(\.detach\s*\(|\.lock\s*\(|pthread_detach)"},
    {"L10", "单参构造应显式 explicit（提示级）",
     R"(^\s{2}(?!explicit)(?!~)[A-Z]\w*\s*\(\s*(?:const\s+)?[\w:]+(?:<[^>]*>)?\s*[&\w]*\s*\)\s*(?::|=\s*default|\{))",
     true},
    {"L11", "禁止 std::endl（用 '\\n'，避免无谓 flush）", R"(\bstd::endl\b)"},
    // 为什么单独立一条：`.at()` 是**抛异常**的取值接口（Json 键缺失、std 容器越界都会抛），
    // 与"错误经 Result 返回"的约定冲突。它极易被误用成"看起来更安全的下标"——
    // nlohmann 的 `at()` 在键可选时直接终止进程（本框架踩过：鼠标事件的修饰键缺席，整进程挂掉）。
    //
    // 规则只收 `.at(`：`.value()` 虽然是 nlohmann 的同类抛异常接口，但 `value()` 是**极常见的
    // 自有 getter 名**（本框架 `Slider::value()` 等），文本级规则无法区分接收者类型，
    // 收了就会天天误报。Json 上的 `.value()` 由 `st/ext/json.hpp` 的文档与评审把关。
    {"L12", "禁止 .at() 取值（键缺失即抛异常；用 json_at/find 或显式检查）", R"(\.at\s*\()"},
}};

/// 去掉行注释与块注释状态（保留字符串内容，简单启发式）。
[[nodiscard]] auto strip_comments(std::string_view line, bool& in_block_comment) -> std::string {
  std::string out;
  out.reserve(line.size());
  bool in_string = false;
  for (std::size_t index = 0; index < line.size(); ++index) {
    const char current = line[index];
    const char next = index + 1 < line.size() ? line[index + 1] : '\0';
    if (in_block_comment) {
      if (current == '*' && next == '/') {
        in_block_comment = false;
        ++index;
      }
      continue;
    }
    if (in_string) {
      out.push_back(current);
      if (current == '\\' && next != '\0') {
        out.push_back(next);
        ++index;
        continue;
      }
      if (current == '"') in_string = false;
      continue;
    }
    if (current == '/' && next == '/') break;
    if (current == '/' && next == '*') {
      in_block_comment = true;
      ++index;
      continue;
    }
    if (current == '"') in_string = true;
    out.push_back(current);
  }
  return out;
}

/// 文件级豁免（均需在 `CONVENTIONS.md` §8 登记）：
/// - **L6**：平台/指令集边界（`platform_*`、`simd*`）——系统 API 与 SIMD intrinsics 的
///   位级重解释只能在这里发生，且必须单点封装。
/// - **L3**：测试框架 `test.hpp` 的断言宏——这是"函数式宏"唯一被认可的用途
///   （需要在断言里拿到调用点文件/行号与表达式原文）。
[[nodiscard]] auto rule_exempt(std::string_view file_name, std::string_view rule) -> bool {
  if (rule == "L6") {
    return file_name.find("platform_") != std::string_view::npos ||
           file_name.find("simd") != std::string_view::npos;
  }
  if (rule == "L3") {
    return file_name == "test.hpp";
  }
  return false;
}

[[nodiscard]] auto has_allow_comment(std::string_view raw_line, std::string_view rule) -> bool {
  const std::size_t position = raw_line.find("lint-allow:");
  if (position == std::string_view::npos) return false;
  const std::string_view tail = raw_line.substr(position);
  return tail.find(rule) != std::string_view::npos;
}

/// 命名空间作用域下的**可变全局**声明判定（L8）。
///
/// 只看单行必然误报（局部变量、函数定义都会被卷进来，实测误报近千条），因此这里做两件事：
/// ① **跟踪花括号深度**，只在深度 0（命名空间作用域）判定；
/// ② 只在形如 `类型 名字 = …;` / `类型 名字{…};` / `类型 名字;` 且**不含函数调用/定义特征**时命中。
/// 允许的前缀：`const`/`constexpr`/`consteval`/`inline`/`using`/`typedef`/`extern`/`template` 等。
[[nodiscard]] auto check_mutable_globals(const std::vector<std::string_view>& lines,
                                         std::string_view path, std::size_t& suppressed)
    -> std::vector<LintViolation> {
  std::vector<LintViolation> violations;
  static const std::regex declaration(
      R"(^[A-Za-z_][\w:]*\s*(?:<[^;{}]*>)?\s*[\*&]?\s*[a-z_][\w]*\s*(?:=[^=]|\{|;\s*$))");
  static const std::regex allowed_prefix(
      R"(^(?:const|constexpr|consteval|inline|using|typedef|extern|template|namespace|class|struct|enum|union|return|if|else|for|while|switch|do|case|break|continue|public|private|protected|static_assert|friend|virtual|explicit|operator)\b)");
  bool in_block_comment = false;
  int depth = 0;
  for (std::size_t index = 0; index < lines.size(); ++index) {
    const std::string_view raw = lines[index];
    const std::string stripped = strip_comments(raw, in_block_comment);
    const int line_depth = depth;
    // 更新深度（判断发生在行首深度上：命名空间作用域的声明深度为 0）
    for (const char glyph : stripped) {
      if (glyph == '{') ++depth;
      else if (glyph == '}') depth = depth > 0 ? depth - 1 : 0;
    }
    if (line_depth != 0) continue;
    const std::string body = std::string(trim(stripped));
    if (body.empty()) continue;
    // 预处理指令、注释、宏续行等不在本规则范围
    if (body.front() == '#') continue;
    if (std::regex_search(body, allowed_prefix)) continue;
    // 含 '(' 且不含 '=' → 更像函数声明/定义/调用，跳过（保守优先，宁可漏报不误报）
    const bool has_paren = body.find('(') != std::string::npos;
    const bool has_assign = body.find('=') != std::string::npos;
    if (has_paren && !has_assign) continue;
    if (!std::regex_search(body, declaration)) continue;
    const std::string file_name = fs::file_name(path);
    if (rule_exempt(file_name, "L8")) continue;
    if (has_allow_comment(raw, "L8")) {
      ++suppressed;
      continue;
    }
    LintViolation violation;
    violation.file = std::string(path);
    violation.line = static_cast<int>(index) + 1;
    violation.rule = "L8";
    violation.text = body;
    violations.push_back(std::move(violation));
  }
  return violations;
}

/// 把字符串字面量的**内容**替换为空格（保留引号与转义结构），返回"仅代码"视图。
///
/// 为什么必须做：`src/md/highlight.cpp` 里有关键字表（"goto"/"throw"/"reinterpret_cast"），
/// `src/pkg/lint.cpp` 里有本规则表自己的正则字符串——按原文匹配会让 linter 对着自己的
/// 关键词表报几十条"违规"。字符串内容不是代码，不应参与构造类规则（L4/L5/L6/L9/L11）匹配。
[[nodiscard]] auto strip_string_literals(std::string_view line, bool& in_raw_string) -> std::string {
  std::string out;
  out.reserve(line.size());
  bool in_string = false;
  bool in_char = false;
  for (std::size_t index = 0; index < line.size(); ++index) {
    const char current = line[index];
    if (in_raw_string) {
      if (current == ')' && index + 1 < line.size() && line[index + 1] == '"') {
        out.append(")\"");
        ++index;
        in_raw_string = false;
        continue;
      }
      continue;
    }
    if (in_string || in_char) {
      if (current == '\\' && index + 1 < line.size()) {
        out.append("  ");
        ++index;
        continue;
      }
      if ((in_string && current == '"') || (in_char && current == '\'')) {
        out.push_back(current);
        in_string = false;
        in_char = false;
        continue;
      }
      out.push_back(' ');
      continue;
    }
    if (current == 'R' && index + 1 < line.size() && line[index + 1] == '"') {
      out.append("R\"");
      ++index;
      // 跳过分隔符直到 '('
      std::size_t probe = index + 1;
      while (probe < line.size() && line[probe] != '(') ++probe;
      if (probe >= line.size()) {
        in_raw_string = true;
        continue;
      }
      index = probe;
      in_raw_string = true;
      continue;
    }
    if (current == '"') {
      in_string = true;
      out.push_back(current);
      continue;
    }
    if (current == '\'') {
      in_char = true;
      out.push_back(current);
      continue;
    }
    out.push_back(current);
  }
  return out;
}

[[nodiscard]] auto scan_text(std::string_view path, std::string_view text,
                             std::size_t& suppressed) -> std::vector<LintViolation> {
  const auto lines = split(text, '\n');
  const std::string file_name = fs::file_name(path);
  std::vector<LintViolation> violations;
  bool in_block_comment = false;
  bool in_raw_string = false;

  std::vector<std::regex> patterns;
  patterns.reserve(kRules.size());
  for (const auto& rule : kRules) {
    if (rule.pattern.empty()) {
      patterns.emplace_back("(?!)");  // 永不匹配：该规则由专用检查实现
      continue;
    }
    patterns.emplace_back(std::string(rule.pattern), std::regex::ECMAScript);
  }

  for (std::size_t index = 0; index < lines.size(); ++index) {
    const std::string_view raw = lines[index];
    // 两层净化：① 注释（不参与任何规则）② 字符串字面量内容（不参与任何规则）
    const std::string uncommented = strip_comments(raw, in_block_comment);
    const std::string stripped = strip_string_literals(uncommented, in_raw_string);
    if (trim(stripped).empty()) continue;
    for (std::size_t rule_index = 0; rule_index < kRules.size(); ++rule_index) {
      const RuleSpec& rule = kRules[rule_index];
      if (rule_exempt(file_name, rule.id)) continue;
      if (!std::regex_search(stripped, patterns[rule_index])) continue;
      if (has_allow_comment(raw, rule.id)) {
        ++suppressed;
        continue;
      }
      LintViolation violation;
      violation.file = std::string(path);
      violation.line = static_cast<int>(index) + 1;
      violation.rule = std::string(rule.id);
      violation.text = std::string(trim(raw));
      violation.advisory = rule.advisory;
      violations.push_back(std::move(violation));
    }
  }
  for (auto& violation : check_mutable_globals(lines, path, suppressed)) {
    violations.push_back(std::move(violation));
  }
  return violations;
}

}  // namespace

auto lint_file(std::string_view path) -> Result<std::vector<LintViolation>> {
  auto text = fs::read_text(path);
  if (!text) return forward_error(text.error());
  std::size_t suppressed = 0;
  return scan_text(path, *text, suppressed);
}

auto lint_project(std::string_view root) -> Result<LintReport> {
  LintReport report;
  const std::array<std::string_view, 5> roots{"include", "src", "tests", "examples", "tools"};
  for (const auto& directory : roots) {
    const std::string base = fs::join(root, directory);
    if (!fs::is_directory(base)) continue;
    auto entries = fs::walk(base);
    if (!entries) return forward_error(entries.error());
    for (const auto& entry : *entries) {
      if (entry.is_dir) continue;
      const std::string extension = fs::extension(entry.path);
      if (extension != ".cpp" && extension != ".hpp") continue;
      const std::string full = fs::join(base, entry.path);
      auto text = fs::read_text(full);
      if (!text) return forward_error(text.error());
      ++report.files_scanned;
      auto violations = scan_text(full, *text, report.suppressed);
      for (auto& violation : violations) report.violations.push_back(std::move(violation));
    }
  }
  std::ranges::sort(report.violations, [](const LintViolation& a, const LintViolation& b) {
    if (a.file != b.file) return a.file < b.file;
    return a.line < b.line;
  });
  return report;
}

auto explain_rule(std::string_view rule) -> std::string {
  for (const auto& spec : kRules) {
    if (spec.id != rule) continue;
    return std::format("{}: {}{}\n  正则: {}", spec.id, spec.description,
                       spec.advisory ? "（提示级，不导致失败）" : "", spec.pattern);
  }
  return std::format("未知规则: {}", rule);
}

auto known_rules() -> std::vector<std::pair<std::string, std::string>> {
  std::vector<std::pair<std::string, std::string>> out;
  out.reserve(kRules.size());
  for (const auto& spec : kRules) {
    out.emplace_back(std::string(spec.id), std::string(spec.description));
  }
  return out;
}

}  // namespace st::pkg

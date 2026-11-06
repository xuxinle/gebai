/// `st lint` 的**词法层**测试（`src/pkg/lint.cpp` 的 `lex_line`）。
///
/// ## 为什么单独一个文件（2026-11-06，来自一次"0 违反"被信过的代价）
///
/// 那天的现场：`st lint` 报 51 项违反，其中 9 条是 L8「可变全局」，指向
/// `src/raster/three_runtime.cpp` 里一串**函数内局部变量**（`auto begin = …`、
/// `const bool needs_return = …`）。看着像代码违规，其实是**扫描器自己的词法错**：
///
/// `strip_string_literals` 只认 `)"` 作裸字符串终结符，而该文件里是 `R"JS( … )JS"`。
/// 于是内容里第一处 `)"` 就被当成终结，状态机整段错位——
/// ① 第 381–709 行被当成字符串，**一条规则都没检查过**（静默漏扫）；
/// ② 第 728–806 行的花括号算错，栈里多了一层 `'O'`，于是"全局作用域"判定成立，
///    函数内局部变量被当成进程级全局。
///
/// 这类缺陷的可怕之处：**输出看起来像代码问题**。人第一反应是去改代码，
/// 而真正该改的是 linter。所以本文件把三件事钉住：
/// 1. 带定界符的裸串必须按**自己的定界符**终结（内容里的 `)"` 不算）；
/// 2. 裸串内容里的 `//` **不是**行注释（不能截断那一行——否则终结符若写在同一行后面就丢了）；
/// 3. 裸串结束后，后文的代码必须被正常扫描（L8 在函数体内**不得**报）。

#include "st/test/test.hpp"

#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/fs.hpp"
#include "st/pkg/lint.hpp"

namespace {

/// 把源码写进临时文件并扫描（与 `pkg_lint_test.cpp` 同一手法）。
[[nodiscard]] auto lint_source(std::string_view name, std::string_view source)
    -> std::vector<st::pkg::LintViolation> {
  const std::string path = st::fs::join(st::fs::temp_dir(), std::string(name));
  {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << source;
  }
  auto result = st::pkg::lint_file(path);
  (void)st::fs::remove_file(path);
  if (!result) return {};
  return *result;
}

[[nodiscard]] auto count_rule(const std::vector<st::pkg::LintViolation>& violations,
                             std::string_view rule) -> std::size_t {
  std::size_t count = 0;
  for (const auto& violation : violations) {
    if (violation.rule == rule) ++count;
  }
  return count;
}

}  // namespace

// ————————————————————————————————————————————————————————————————————————————
// 裸字符串定界符
// ————————————————————————————————————————————————————————————————————————————

/// 核心回归：**带定界符**的裸串里的 `)"` 不得提前终结它。
///
/// 用真实缺陷形态：裸串内容里有 `f()"` 这样的片段（JS 里遍地都是），
/// 而终结符 `)JS"` 在下面几行。若终结判据写死成 `)"`，那个 `f()"` 就会被当成串尾，
/// 之后整段 C++ 状态错位。
///
/// ⚠ **用例自身的陷阱**（实测踩过一次）：内容里绝不能写出完整的结束定界符
/// （连注释文字都不行）——写进去就等于真的提前终结了裸串，用例会变成
/// “断言器说得好：linter 没错，是用例错了”。这里只用 `)"` 与 `JS"` 分开的描述。
///
/// 断言写法（为何看 L1 不看 L8）：旧口径下提前“终结”让**裸串尾部**变成代码，
/// 而下文的 `new` 可能恰好落在被误判为字符串的区间里——两个错误互相掩盖。
/// 所以把测点放在可确定性判定的地方：内容里写 `free(`，
/// 若扫描器真的把内容当代码（提前终结），L1 必然命中。
ST_TEST(lint_lex_raw_string_with_delimiter_ignores_inner_paren_quote) {
  const std::string source = R"CPP(
namespace probe {
void run() {
  const char* script = R"JS(
    function f() { return "x"; }
    var s = f()";      // 内容里的右括号+引号：不是终结符
    free(ptr);         // 内容里的关键字：被当成代码就会报 L1
)JS";
  (void)script;
}
}
)CPP";
  const auto violations = lint_source("lex_raw_delim.cpp", source);
  ST_CHECK_EQ(count_rule(violations, "L1"), 0U);   // 裸串内容不参与规则
  ST_CHECK_EQ(count_rule(violations, "L8"), 0U);   // 函数体内的局部量不是全局
}

/// 裸串内容里的 `//` **不是**行注释——不能把那一行截断。
///
/// 实测形态：终结符写在与 `//` 同行时（`… )JS";  // 收尾`），
/// 若先把 `//` 之后整段砍掉，终结符就永远看不见，状态机卡在裸串里到文件末尾，
/// 后面的代码全部漏扫（且花括号栈永久偏移）。
ST_TEST(lint_lex_line_comment_inside_raw_string_does_not_truncate) {
  const std::string source = R"CPP(
namespace probe {
const char* k = R"JS(
  // JS 里的注释，不是 C++ 注释
  var url = "http://x"; )JS";
// 裸串**之后**的 C++ 代码：这里故意写一个真全局，必须被抓到
int real_global_counter = 0;
}
)CPP";
  const auto violations = lint_source("lex_raw_comment.cpp", source);
  // 裸串结束之后的真全局必须报到（证明状态机真的退出了裸串、后续行被正常扫描）
  ST_CHECK_EQ(count_rule(violations, "L8"), 1U);
}
/// 反例：**不带定界符**的裸串仍然按 `)"` 终结（这条不能因为上面两条被改坏）。
ST_TEST(lint_lex_plain_raw_string_still_terminates_on_paren_quote) {
  const std::string source = R"CPP(
namespace probe {
const char* k = R"(一层 shell: $(echo hi))";
int after_plain = 0;
}
)CPP";
  const auto violations = lint_source("lex_raw_plain.cpp", source);
  ST_CHECK_EQ(count_rule(violations, "L8"), 1U);
}

/// 反例：普通字符串里的 `//` 与 `{`/`}` 不是注释也不是花括号。
///
/// 这条守的是"净代码视图"本身：字符串内容必须对 L8 的花括号计数**不可见**，
/// 否则 `"{"` 会让栈永久偏移（本仓 markdown 解析器里满是大括号字面量）。
ST_TEST(lint_lex_string_literals_do_not_shift_brace_scope) {
  const std::string source = R"CPP(
namespace probe {
void f() {
  const char* open = "{";
  const char* close = "}";
  const char* comment = "// 不是注释";
  (void)open; (void)close; (void)comment;
}
int real_decl = 0;
}
)CPP";
  const auto violations = lint_source("lex_string_braces.cpp", source);
  ST_CHECK_EQ(count_rule(violations, "L8"), 1U);   // 只有 real_decl，花括号没被字面量带偏
}

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
constexpr std::array<RuleSpec, 14> kRules{{
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
    // L13 需要作用域判断（「当前类是否继承 Element」「这行是否在 semantics_flags 覆写
    // 体内」都表达不成单行正则），由 `check_element_state()` 单独实现。
    {"L13", "组件不得遮蔽 Element 保护成员、semantics_flags 覆写不得重建标志", ""},
    // L14：系统头单点封装。
    //
    // 为什么需要它：`CONVENTIONS.md` §10 第 1 条要求「平台差异只能出现在 platform_* 里」，
    // 而这条约束原先**只在文档里**——§8 写着「lint 亦检查禁用 include」，实现里却
    // 没有这道检查。于是 `src/core/entry.cpp` 长期直接 `#include <windows.h>` 无人发现。
    //
    // 判据（两类，都只看代码行）：
    //   ① 系统头 include：`#include <windows.h>` 等；
    //   ② 直接用 `dlopen`/`dlsym` 家族（应当经 `st::process` 单点封装）。
    //
    // 白名单（`rule_exempt(file_name, "L14")`）：`platform_*` 单点封装文件，**外加
    // 清单 `lint.exempt.L14` 登记的具名文件**（如 `core/entry.cpp`——`ST_MAIN` 需在
    // 调用点正规化 Windows 的 ANSI `argv`，而它的职责不属于任何 `platform_*` 横切层）。
    // 两条通道都各自计数，不允许静默。
    {"L14", "系统头/平台 API 只能出现在 platform_* 单点封装（见 §10 第 1 条）",
     R"(#\s*include\s*<(windows|unistd|dlfcn|shellapi|winsock2|arpa/inet|netinet/in|sys/socket|poll|fcntl)\.h?>|\bdlopen\s*\(|\bdlsym\s*\()"},
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
/// - **L14**：同样以 `platform_*` 为边界（系统头的唯一合法落脚点）；`backend.cpp`
///   额外获准——它需要 `dlopen` 做**运行时后端探测**，而这正是 §10 第 1 条
///   「平台后端一律运行时探测（不产生链接期依赖）」的实现处：探测这一层本身就是
///   平台差异的**唯一**判定点，放进 `platform_*` 只会让"有没有这个后端"这件事被拆散。
[[nodiscard]] auto rule_exempt(std::string_view file_name, std::string_view rule) -> bool {
  if (rule == "L6") {
    return file_name.find("platform_") != std::string_view::npos ||
           file_name.find("simd") != std::string_view::npos;
  }
  if (rule == "L3") {
    return file_name == "test.hpp";
  }
  if (rule == "L14") {
    return file_name.find("platform_") != std::string_view::npos || file_name == "backend.cpp";
  }
  return false;
}

/// 清单配置的规则豁免（`st.pkg` 的 `lint.exempt`）：`{ "规则": ["路径 glob", …] }`。
///
/// ## 为何需要它（2026-10-05，来自实战）
///
/// `st lint` 把 dev-tools 应用 `llm.cpp` 里 9 处 `throw std::runtime_error` 判成 L5 违规，
/// 而那些 throw 是 **worker 线程向主线程传播错误**的合理用法——跨线程边界上，
/// 异常是唯一能完整携带"错误在子线程里发生在哪一步"的机制（`Result` 只能在线程内传递）。
/// 但 lint 是**文本级**检查，看不到"这行在哪个线程上下文里"。
///
/// 两个选择：① 让业务层改用 `Result`（把异常硬掰成返回值，跨线程时丢失调用栈与
/// 错误的来源层次，为迁就工具而降低代码质量）；② **让工具承认自己看不出来的地方**
/// （把边界显式写进清单，审计留痕、可复查）。选 ②——工具服务于约束，不是反过来。
///
/// ## 与行内豁免（`// lint-allow: L5 原因`）的分工
///
/// - 行内：**单点**、就地说明（"这一行为什么破例"），适合零星例外；
/// - 清单：**成片**、集中的边界（"这一层与框架的约定不同"），且需在
///   `CONVENTIONS.md` §8 登记（"哪些边界可以不同"变成可复查的清单，而不是散落各处的注释）。
///
/// 两者都不允许静默：清单里的 glob 命中即计入 `LintReport::suppressed`，
/// `st lint` 的汇总行会如实报出"豁免 N 处"。
[[nodiscard]] auto manifest_exempt(std::string_view path, std::string_view rule,
                                  const LintExemptions& exemptions, std::size_t& suppressed) -> bool {
  const auto rule_it = exemptions.find(std::string(rule));
  if (rule_it == exemptions.end()) return false;
  for (const std::string& pattern : rule_it->second) {
    if (!st::fs::match_glob(pattern, path)) continue;
    ++suppressed;
    return true;
  }
  return false;
}

[[nodiscard]] auto has_allow_comment(std::string_view raw_line, std::string_view rule) -> bool {
  const std::size_t position = raw_line.find("lint-allow:");
  if (position == std::string_view::npos) return false;
  const std::string_view tail = raw_line.substr(position);
  return tail.find(rule) != std::string_view::npos;
}

/// 把字符串字面量的**内容**替换为空格（定义在本文件后部；此处前向声明——
/// L8 的花括号计数必须在“净代码”上做，见 `check_mutable_globals` 的误报防线）。
[[nodiscard]] auto strip_string_literals(std::string_view line, bool& in_raw_string) -> std::string;

/// 命名空间作用域下的**可变全局**声明判定（L8）。
///
/// ## 历史缺陷（本轮修复）
///
/// 旧实现只判 `line_depth == 0`（花括号深度为 0）。但本工程几乎每个文件的辅助全局都
/// 包在**匿名命名空间**里（`namespace { ... }`），深度恒 ≥1——于是可变全局
/// **全部漏网**，L8 实际近乎失效（还掩盖了一个真实缺陷：`on_state_write` 里无锁遍历
/// 那个漏网的容器）。
///
/// ## 现行判据
///
/// ① **全局作用域**：花括号标签栈只由 `N`（namespace 体）组成——含文件级与嵌套
///    匿名/具名 namespace。这才是 L8 的原意（进程级全局）。
/// ② **函数内局部 `static`**：同样是“一次初始化、进程生存期共享”的可变状态，
///    只是藏在函数里，一并判（类体内的 `static` 是静态成员，同属进程级状态，也判）。
///
/// ## 误报防线（三条，均由实测误报反推）
///
/// - **净代码视图**：花括号计数必须在**剔除字符串字面量**后的文本上做。markdown 解析器
///   里满是大括号字面量（`"{"`/`"}"`），按原文计数会让栈严重错位（实测把类成员
///   判成了全局）。
/// - **签名续行**：函数声明的续行（`float radius = 0.0f) {`）看着就像全局声明。用
///   “本行以 `)`/`{`/`,` 结尾”与“括号未平衡”排除——两条件对**声明行**都不成立。
/// - **前置作用域豁免**：`// lint-allow: L8 <原因>` 写在命名空间开括号**之前**是既有惯例
///   （`log.cpp`/`test_runner.cpp`）——往后 50 行内生效，让整块登记一次即可。
[[nodiscard]] auto check_mutable_globals(const std::vector<std::string_view>& lines,
                                         std::string_view path, std::size_t& suppressed)
    -> std::vector<LintViolation> {
  std::vector<LintViolation> violations;
  static const std::regex declaration(
      R"(^[A-Za-z_][\w:]*\s*(?:<[^;{}]*>)?\s*[\*&]?\s*[a-z_][\w]*\s*(?:=[^=]|\{|;\s*$))");
  static const std::regex allowed_prefix(
      R"(^(?:const|constexpr|consteval|inline|using|typedef|extern|template|namespace|class|struct|enum|union|return|if|else|for|while|switch|do|case|break|continue|public|private|protected|static_assert|friend|virtual|explicit|operator)\b)");
  static const std::regex namespace_open(R"(^\s*namespace\b)");
  static const std::regex class_open(R"(^\s*(?:class|struct|union)\b)");
  static const std::regex static_word(R"(\bstatic\b)");
  static const std::regex immutable_word(R"(\b(?:const|constexpr|consteval)\b)");
  // `thread_local` 是**每线程**一份，不是“进程级共享”——L8 针对的恰恰是共享可变状态。
  // 线程局部状态反而是一种**避免**共享的手段，不应被本条惩罚（本框架的
  // `tls_composer` 就是重组线程的“当前 Composer”，正是正确用法）。
  static const std::regex thread_local_word(R"(\bthread_local\b)");
  static const std::regex declaration_tail(R"([),{]$)");
  // 前置存储说明符：`static std::string store;` 里的 `static` 会占住“类型”位，
  // 让后面的 `[a-z_][\w]*` 只能匹配到 `std`（限定名 `std::string` 解析不出）。
  // 先剥掉再匹配——剥掉后 `static` 的语义已由 `has_static` 捕获，不影响判定。
  static const std::regex storage_prefix(R"(^(?:static|inline|extern|thread_local)\s+)");

  const std::string file_name = fs::file_name(path);
  if (rule_exempt(file_name, "L8")) return violations;

  // 作用域标签栈：'N' = namespace 体、'C' = class/struct/union 体、'O' = 其他块。
  std::vector<char> scopes;
  bool in_block_comment = false;
  bool in_raw_string = false;
  int open_parens = 0;
  // 前置作用域豁免的剩余行数（`// lint-allow: L8 …` 写在命名空间开括号之前，见函数头）。
  int block_allow_remaining = 0;

  const auto at_global_scope = [&scopes] {
    return std::ranges::all_of(scopes, [](char tag) { return tag == 'N'; });
  };
  const auto inside_class = [&scopes] {
    return std::ranges::find(scopes, 'C') != scopes.end();
  };

  for (std::size_t index = 0; index < lines.size(); ++index) {
    const std::string_view raw = lines[index];
    const std::string uncommented = strip_comments(raw, in_block_comment);
    // 净代码视图：字符串内容换成空格（花括号计数必须在这上面做）。
    const std::string code = strip_string_literals(uncommented, in_raw_string);
    const std::string body = std::string(trim(code));

    const bool allow_here = has_allow_comment(raw, "L8");
    // **仅当该行是纯注释行**才开启作用域窗口：这才是前置登记（`// lint-allow: L8 …`
    // 单独一行、写在命名空间开括号之前）的形态。贴在声明行尾的豁免仍是单点语义
    // （否则下一行的真全局会被上一行的注释“顺带”豁免掉）。
    if (allow_here && std::string(trim(code)).empty()) block_allow_remaining = 50;

    // —— 先判定本行（用**行首**的作用域）——
    if (!body.empty() && body.front() != '#' && !inside_class()) {
      const bool has_static = std::regex_search(body, static_word);
      const bool immutable = std::regex_search(body, immutable_word);
      const bool per_thread = std::regex_search(body, thread_local_word);
      // 函数签名的续行（`... = 0.0f) {`）——签名本身不在此行起头，排除。
      const bool continuation = open_parens > 0 || std::regex_search(body, declaration_tail);
      const bool candidate = (at_global_scope() || has_static) && !immutable && !per_thread &&
                             !continuation && !std::regex_search(body, allowed_prefix);
      if (candidate) {
        // 含 '(' 且不含 '=' → 更像函数声明/定义/调用，跳过（保守优先，宁可漏报不误报）
        const bool has_paren = body.find('(') != std::string::npos;
        const bool has_assign = body.find('=') != std::string::npos;
        std::string declarator = body;
        std::smatch storage;
        if (std::regex_search(declarator, storage, storage_prefix)) declarator = storage.suffix();
        if (!(has_paren && !has_assign) && std::regex_search(declarator, declaration)) {
          if (allow_here || block_allow_remaining > 0) {
            ++suppressed;
          } else {
            LintViolation violation;
            violation.file = std::string(path);
            violation.line = static_cast<int>(index) + 1;
            violation.rule = "L8";
            violation.text = body;
            violations.push_back(std::move(violation));
          }
        }
      }
    }

    // —— 再更新作用域栈与括号平衡（用净代码视图）——
    const bool opens_namespace = std::regex_search(body, namespace_open);
    const bool opens_class = std::regex_search(body, class_open);
    for (const char glyph : code) {
      if (glyph == '(') {
        ++open_parens;
      } else if (glyph == ')') {
        if (open_parens > 0) --open_parens;
      } else if (glyph == '{') {
        scopes.push_back(opens_namespace ? 'N' : (opens_class ? 'C' : 'O'));
        // 同一行只吃第一个标签（`namespace X { namespace Y {` 极少见；
        // 漏掉一个内层只会让判定偏保守）。
        if (opens_namespace || opens_class) continue;
      } else if (glyph == '}') {
        if (!scopes.empty()) scopes.pop_back();
      }
    }
    if (block_allow_remaining > 0) --block_allow_remaining;
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

/// Element 保护成员名（L13 用）。
///
/// **与 `include/st/ui/element.hpp` 的成员表保持同步**：基类新增或改名保护成员时
/// 往这里补一笔，否则新成员被遮蔽时规则漏报。
///
/// ⚠ 这是一份**手工同步的重复真源**（当前与 `element.hpp` 逐项核对过）。它在 P4 的
/// `Element` 瘦身（`hover_t_` 等移入 `RenderExtras`）时**确实漏改过一次**——
/// 恰好漏掉的 `hover_t_` 正好是 A2 重构会动到的字段，正好说明了这类清单的风险。
/// 将来若要把 `Element` 的成员表也做成单一真源（如从 `element.hpp` 生成），
/// 这里是首要候选。
constexpr std::array<std::string_view, 19> kElementStateMembers{
    "style_",      "bounds_",        "measured_",       "id_",          "key_",
    "parent_",     "children_",      "visible_",        "enabled_",     "focusable_",
    "hovered_",    "pressed_",       "focused_",        "dirty_",       "layout_dirty_",
    "hover_effect_", "host_",       "animation_requested_", "extras_",
};

/// L13：组件不得**遮蔽** `Element` 的保护成员；`semantics_flags` 覆写不得重建标志。
///
/// 为何单独立一条：这两类缺陷**编译零警告**，且症状具有欺骗性——焦点写基类、
/// 读遮蔽副本（或语义标志被重建）时，直接调 `set_focused` 的组件级单测读写落在同一侧，
/// 只有真实应用（`UiRoot::set_focus`）才暴露。实测代价：`CodeEditor` 自带
/// `bool focused_{false}` → 光标永不绘制、括号高亮失效，而测试全绿。
///
/// 为何专用检查：需判断「当前类是否继承 `Element`」与「这行是否在 `semantics_flags`
/// 覆写体内」——单行正则表达不了。类继承状态按花括号深度跟踪（类头行记下当时的深度，
/// 深度回落即类结束）。
[[nodiscard]] auto check_element_state(const std::vector<std::string_view>& lines,
                                       std::string_view path, std::size_t& suppressed)
    -> std::vector<LintViolation> {
  static const std::regex class_header(R"(^\s*(?:class|struct)\s+\w+([^;{]*)\{)");
  // 覆写头的两种形态：类外定义 `Type::semantics_flags()` 与类内 inline 定义
  static const std::regex semantics_override(R"((?:::)?semantics_flags\s*\(\s*\)[^{;]*\{)");
  static const std::regex flags_rebuild(R"(\bSemanticsFlags\s+\w+\s*\{\s*\})");
  static const std::regex member_decl(
      R"(^\s*(?:mutable\s+)?(?:static\s+)?[\w:][\w:<>,\s\*&]*?\b(\w+_)\s*(?:\{[^;]*\})?;)");
  // 语句行（成员函数体内的 `return children_;` 之类）不是成员声明：不参与遮蔽判定
  static const std::regex statement_prefix(
      R"(^\s*(?:return|if|else|for|while|switch|do|case|break|continue|goto|throw|using|typedef|friend)\b)");

  std::vector<LintViolation> violations;
  bool in_block_comment = false;
  bool in_raw_string = false;
  bool class_is_element = false;
  int depth = 0;
  int class_depth = -1;      // 当前类开始时的深度（-1 = 不在类内）
  int override_remaining = 0;  // `semantics_flags` 覆写体内剩余扫描行数
  const auto report = [&](std::size_t index, std::string_view body) {
    violations.push_back(LintViolation{std::string(path), static_cast<int>(index) + 1, "L13",
                                       std::string(body), false});
  };
  for (std::size_t index = 0; index < lines.size(); ++index) {
    const std::string_view raw = lines[index];
    const std::string uncommented = strip_comments(raw, in_block_comment);
    const std::string stripped = strip_string_literals(uncommented, in_raw_string);
    const std::string body = std::string(trim(stripped));
    if (body.empty()) continue;
    // 类头（进入新类：**基类子句**里提到 Element 才算「Element 子类」——
    // 只看整行会连 `class Element {` 自己（类名含 Element）也当成子类）
    std::smatch header;
    if (std::regex_search(stripped, header, class_header)) {
      const std::string bases = header[1].str();
      const std::size_t colon = bases.find(':');
      class_is_element =
          colon != std::string::npos && bases.find("Element", colon) != std::string::npos;
      class_depth = depth;
    }
    bool hit = false;
    // ① 遮蔽：Element 子类里声明与基类同名的成员
    if (class_is_element && !std::regex_search(stripped, statement_prefix)) {
      std::smatch matched;
      if (std::regex_search(stripped, matched, member_decl)) {
        const std::string name = matched[1].str();
        if (std::find(kElementStateMembers.begin(), kElementStateMembers.end(), name) !=
            kElementStateMembers.end()) {
          hit = true;
        }
      }
    }
    // ② 语义标志重建：覆写体内以 `SemanticsFlags flags{}` 起手
    if (!hit && override_remaining > 0 && std::regex_search(stripped, flags_rebuild)) hit = true;
    if (hit) {
      if (has_allow_comment(raw, "L13")) {
        ++suppressed;
      } else {
        report(index, body);
      }
    }
    // semantics_flags 覆写开启（同一行的花括号即函数体开始；扫描到首个 `}` 行为止）
    if (std::regex_search(stripped, semantics_override)) override_remaining = 40;
    else if (override_remaining > 0) --override_remaining;
    if (override_remaining > 0 && body == "}") override_remaining = 0;
    // 深度跟踪（在行末更新：类头行开括号后深度才 +1）
    for (const char glyph : stripped) {
      if (glyph == '{') ++depth;
      else if (glyph == '}') depth = depth > 0 ? depth - 1 : 0;
    }
    if (class_depth >= 0 && depth <= class_depth) {
      class_is_element = false;
      class_depth = -1;
    }
  }
  return violations;
}

[[nodiscard]] auto compiled_patterns() -> const std::vector<std::regex>& {
  // 一次性编译（首次调用时）：原先在 scan_text 里逐文件重建 regex 对象，
  // 198 文件就是 2376 次 regex 构造——std::regex 构造是出名的贵（每条模式都要编译）。
  // C++11 起函数局部 static 的初始化线程安全（magic static），多文件并发扫描也安全。
  static const std::vector<std::regex> patterns = [] {
    std::vector<std::regex> compiled;
    compiled.reserve(kRules.size());
    for (const auto& rule : kRules) {
      if (rule.pattern.empty()) {
        compiled.emplace_back("(?!)");  // 永不匹配：该规则由专用检查实现
        continue;
      }
      compiled.emplace_back(std::string(rule.pattern), std::regex::ECMAScript);
    }
    return compiled;
  }();
  return patterns;
}

[[nodiscard]] auto scan_text(std::string_view path, std::string_view text, std::size_t& suppressed,
                             const LintExemptions& exemptions, std::size_t& manifest_suppressed)
    -> std::vector<LintViolation> {
  const auto lines = split(text, '\n');
  const std::string file_name = fs::file_name(path);
  std::vector<LintViolation> violations;
  bool in_block_comment = false;
  bool in_raw_string = false;

  const std::vector<std::regex>& patterns = compiled_patterns();

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
      // 两条豁免通道：行内（单点）与清单（成片边界）。两者都各自计数（不静默）。
      if (has_allow_comment(raw, rule.id)) {
        ++suppressed;
        continue;
      }
      if (manifest_exempt(path, rule.id, exemptions, manifest_suppressed)) continue;
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
  for (auto& violation : check_element_state(lines, path, suppressed)) {
    violations.push_back(std::move(violation));
  }
  return violations;
}

}  // namespace

auto lint_file(std::string_view path) -> Result<std::vector<LintViolation>> {
  auto text = fs::read_text(path);
  if (!text) return forward_error(text.error());
  std::size_t suppressed = 0;
  std::size_t manifest_suppressed = 0;
  // 单文件入口不接清单（调用方可能是编辑器里的一行一文件）——传空豁免表
  return scan_text(path, *text, suppressed, LintExemptions{}, manifest_suppressed);
}

auto lint_project(std::string_view root, const LintExemptions& exemptions) -> Result<LintReport> {
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
      // 清单豁免的 glob 相对**工程根**（不是相对 `include/` 那层）——
      // 写清单的人想的是"src/app/llm.cpp"，不是相对某个扫描子目录的路径。
      const std::string relative = fs::join(directory, entry.path);
      std::size_t manifest_hits = 0;
      auto violations =
          scan_text(relative, *text, report.suppressed, exemptions, manifest_hits);
      report.suppressed_by_manifest += manifest_hits;
      // 违规的 `file` 字段仍报**绝对路径**（用户要能直接点开/搜索到）——
      // 只在匹配 glob 时用相对路径。
      for (auto& violation : violations) violation.file = full;
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
    const std::string pattern =
        spec.pattern.empty() ? std::string{"由专用检查实现（见 src/pkg/lint.cpp）"}
                             : std::string(spec.pattern);
    return std::format("{}: {}{}\n  正则: {}", spec.id, spec.description,
                       spec.advisory ? "（提示级，不导致失败）" : "", pattern);
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

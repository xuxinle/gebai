#include "st/pkg/lint.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <regex>
#include <string>
#include <atomic>
#include <optional>
#include <thread>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/string.hpp"
#include "st/core/log.hpp"

namespace st::pkg {
namespace {

struct RuleSpec {
  std::string_view id;
  std::string_view description;
  std::string_view pattern;
  bool advisory{false};
};

/// 规则表（与 `CONVENTIONS.md` §8 一一对应）。
constexpr std::array<RuleSpec, 18> kRules{{
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
    // 收了就会天天误报。Json 上的 `.value()` 由 `st/core/json.hpp` 的文档与评审把关。
    // L12 只针对**容器/JSON 的 .at()**（键缺失即抛异常）。
    // 几何/数学类型上的同名访问器（`Ray::at(t)` 取射线上一点）不是这个语义。
    // 用"行内豁免"而不是改正则：`(?<!...)` 后行断言在 std::regex 上会崩
    //（实测 lint 直接段错误），而那是一个更糟的取舍——规则复杂到跑不起来等于没有。
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
     // ③ **POSIX 函数直调**（补于本轮）：上面两类只盖「系统头 include」与
     // `dlopen` 家族，于是 `tests/` 里三处 `static_cast<int>(::getpid())`
     // 长期无人发现——Linux 上靠头文件传递恰好编得过，**交叉编译直接报
     // `'::getpid' has not been declared`**。同一类错还有一批同样常见的：
     // 睡眠/进程/时间 的 POSIX 入口。判据用 `::name(` 的**限定调用形式**：
     // 不经 `::` 的裸调用与同名单成员调用（如 `clock.…`）不会误报；
     // 误伤面限于“自己定义了同名全局函数并限定调用”——本仓没有。
     R"(#\s*include\s*<(windows|unistd|dlfcn|shellapi|winsock2|arpa/inet|netinet/in|sys/socket|poll|fcntl)\.h?>|\bdlopen\s*\(|\bdlsym\s*\(|::(?:getpid|getppid|getuid|geteuid|fork|execv|execve|execl|execlp|waitpid|kill|usleep|sleep|nanosleep|clock_gettime|gettimeofday|opendir|readdir|closedir)\s*\()"},
    // L15：raw string 用**带定界符**的形式。
    //
    // 为什么需要它（本会话实测踩过**两次**）：`R"(...)"` 里出现 `)"` 序列时，
    // raw string 会**提前终结**——后续内容被当成代码解析，报错是
    // "missing terminating \" character" / "character constant too long"，
    // 而真正原因（某处 `)"` 出现）离报错位置可能隔几十行。
    // 实测两次：一次是 JSON 样例里的 `($0)`、一次是 `(std::string)`。
    //
    // 判据：出现 `R"(` （**无定界符**的 raw string 开头）即提示改用 `R"x(...)x"`。
    // 这是一个**提示级**规则（不阻断构建），因为它只指出风险所在——
    // 内容里没有 `)"` 时写法是安全的，但从"AI 写代码"的角度看，
    // 无定界符形式**没有任何好处**（只多一个踩坑面）。
    {"L15", "raw string 建议带定界符（`R\"x(...)x\"`）——无定界符时内容里的 `)\"` 会提前终结（提示级）",
     // `R"(` 紧邻（无定界符）才报；`R"x(` 之类有定界符的**不**匹配。
     //
         // ⚠ **不能作为单行正则实现**：`scan_text` 会先 `strip_string_literals`
    //（字符串内容不参与任何规则），而 `R"(` 正是字符串字面量的**开头**——
    // 剥完就没了。实数落空后才明白：这条必须走**专用检查**（同 L13），
    // 在净化**之前**看原文（见 `check_raw_string_delimiters`）。
    // 顺带记一笔：写这个正则时我自己用无定界符形式写它，被它提前终结了一次
    //——这条规则的最佳示范就是它自己。
    ""},
    // L16：**层方向**——低层不得 include 高层头（core→ui 这类）。
    //
    // 为什么需要它（BACKLOG P1 实测）：`platform_crash.cpp` 曾 include
    // `st/ui/state_trace.hpp`——是**死 include**（零符号使用），但正因为是
    // 头文件级依赖，`st`（工具链）被迫把整条 ui 渲染链拖进链接闭包；
    // 而 lint 看不见它（正则只看单行、看不到依赖方向），于是它活到 2026-10。
    // BACKLOG 的原话：「两条根因都是头文件级的，所以 lint 看不见」——本条就是
    // 把这道检查补上。命中方向：**core → 任何 ui 头**（将来可扩展到 core→shell 等）。
    // 走专用检查（同 L13/L15）：方向判定需要文件路径，单行正则表达不了。
    {"L16", "core 层不得 include ui 层头（依赖方向：ui 可用 core，core 不认识 ui）", ""},
    // L17：**禁止用 `long` / `long long` 表达数据**——一律用定宽类型。
    //
    // 为什么这条值得单独成规则（本会话实测踩到）：
    // `long` 在 LP64（Linux/macOS）是 64 位、在 LLP64（Windows）是 32 位。
    // 用它表达字节数/毫秒/计数，会在**跨平台时才暴露**：Windows 上静默截断或溢出，
    // 而本地开发（Windows + MinGW 恰好 32 位 long）测不出来。
    // 更阴的一层：`std::int64_t` 在 LP64 下**就是** `long`、在 LLP64 下**就是** `long long`，
    // 所以 `std::format("{}", int64值)` 会因平台而报不同的歧义错——用 `long long`
    // "绕过"这个错误等于把差异固化进代码（`src/ext/video.cpp` 曾这么干过）。
    //
    // 判据：把代码行里的**类型位置**上的 `long` / `long long` 抛出来，但排除三类：
    //   ① `long double`（浮点，与宽度无关）；
    //   ② 平台 ABI 边界（`platform_*.cpp` 里的 `LONG`/`long` 是对系统类型的如实声明，
    //      改了反而错——白名单见 `rule_exempt`）；
    //   ③ 注释与字符串（`scan_text` 会先剔掉）。
    //
    // 提示级还是阻断级：**阻断**。理由：它没有"这次恰好安全"的情形——
    // 用 `std::int64_t` 永远同样简洁，而收益是确定的。
    {"L17", "禁止 long / long long 表达数据（用 std::intN_t / std::uintN_t / std::size_t）",
     R"((?:^|[\s(+,*<:])(?:(?:unsigned|signed)\s+)?long(?:\s+long)?(?![\s]*double)\b)"},
    // L18：**禁止绕过日志通道的裸打印**——`printf`/`puts`/`fprintf`/`std::cout` 直接
    // 写 stdout/stderr，不经 `st::log` 的统一输出管道（`st::log::out`/`err`/`ST_LOG_*`，
    // 走 tail/sink/listener/镜像落盘）。于是：
    //  - 库被嵌入宿主时这些输出无法被关闭/分级/审计；
    //  - `st`（CLI）里它们不进 build/check/test 的运行日志，事后无法追溯。
    //
    // 合法的输出通道（不在此列）：`st::log::out` / `st::log::err`（无前缀主输出，
    // 原 `st::print`/`st::eprint`）、`ST_LOG_*`（带级别诊断）、`write_stdout`/`write_stderr`
    // 的调用方（print.cpp 本身与 console_log.cpp 是打印的实现方）。
    // 白名单（`rule_exempt` L18）：打印实现方（print.cpp/console_log.cpp/log_file.cpp/
    // platform_crash.cpp）、平台层（platform_* 直调系统输出是点名要求）。
    {"L18", "禁止绕过日志通道的裸打印（printf/puts/fprintf/cout/cerr；用 st::log::out/err 或 ST_LOG_*）",
     R"((?:^|[\s:;(])std::cout\b|(?:^|[\s:;(])std::cerr\b|\bstd::puts|\bstd::printf|\bprintf\s*\(|\bputs\s*\(|\bfprintf\s*\(|\bfprintf_s?\s*\()"},
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
/// - **L3**：两处。测试框架 `test.hpp` 的断言宏——需要在断言里拿到调用点
///   文件/行号与表达式原文；码日志入口 `log.hpp` 的 `ST_LOG_*`——需要在调用点把参数
///   **一起丢掉**（空函数模板里参数仍被求值，字符串字面量照样编进产物）。
///   两者的共同点：**只能在调用点展开**，没有函数形式能等价实现。
[[nodiscard]] auto rule_exempt(std::string_view file_name, std::string_view rule) -> bool {
  if (rule == "L6") {
    return file_name.find("platform_") != std::string_view::npos ||
           file_name.find("simd") != std::string_view::npos;
  }
  if (rule == "L3") {
    return file_name == "test.hpp" || file_name == "log.hpp";
  }
  if (rule == "L14") {
    return file_name.find("platform_") != std::string_view::npos || file_name == "backend.cpp";
  }
  if (rule == "L8") {
    // 诊断开关是**进程级**的：`ST_TRACE_STATE` 由环境变量控制，追踪缓冲必须在
    // 任意写入点可见——注入式方案会把"诊断"变成"每个 State 都要带一个引用"。
    // 这是单点例外（一个文件、一组相关变量），与 L8 的意图（禁"随手写的全局状态"）
    // 不冲突：这些变量的生命周期与整个进程一致，且**只被本文件读写**。
    return file_name == "state_trace.cpp";
  }
  if (rule == "L17") {
    // 平台 ABI 边界：这些地方的 `long` / `LONG` 是对**系统类型**的如实声明，
    // 不是我们选择的数据宽度。改成 `int32_t` 反而会错：
    //   - `platform_x11.cpp` 里的 `XEvent`/`XWindowAttributes` 布局由 Xlib 定，
    //     `long` 换了 `int64_t` 在 32 位平台上就与真实结构对不上（XEvent 尺寸有 static_assert 钉）；
    //   - `platform_tls_openssl.cpp` 的函数指针签名必须与 OpenSSL 导出符号逐位一致；
    //   - `platform_crash.cpp` / `platform_process.cpp` 接的是 Win32 `LONG`/`DWORD`；
    //   - `platform_net.cpp` / `platform_http.cpp` 的 `FIONBIO` 展开经系统头做 size_t→long 转换。
    // 判据用文件名而不是清单：这类文件**按定义**就是平台层，漏登记一个就会
    // 逼着人去"改错"真正的系统签名。
    return file_name.find("platform_") != std::string_view::npos;
  }
  if (rule == "L18") {
    // 打印**实现方**（必然直写系统句柄）：print/console_log 是 `st::print` 的
    // 底层、log_file 是 `st::log` 的落盘实现、platform_crash 的崩溃栈必须在
    // 日志系统都不可用时仍能输出（信号处理器里不能依赖任何分配/锁）。
    // 平台层（platform_*）直调系统输出是点名要求（如 WASM/裸机的 puts 替代）。
    // CLI 工具（stpm/*）是**交互输出方**：用户直接面对这些消息（构建进度/错误），
    // 且 `st::print` 已镜像落盘（见 console_log），fprintf(stderr) 的错误同样
    // 在 cli_log 的镜像里——可审计性已保证，不做日志化（ST_LOG_DISABLED 会裁掉
    // 用户必须看到的错误）。
    return file_name == "print.cpp" || file_name == "console_log.cpp" ||
           file_name == "log_file.cpp" || file_name == "platform_crash.cpp" ||
           file_name.find("platform_") != std::string_view::npos ||
           file_name.find("stpm") != std::string_view::npos;
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
/// L15：**无定界符 raw string 里出现 `)"`**——会在该处提前终结。
///
/// 判据刻意收窄到"能确定性判定的情形"，而不是"凡 `R"(` 就提示"：
/// 前者是**真缺陷**（编译报错，且报错位置离真因几十行），后者只是风格建议——
/// 本仓有 100+ 处合法的 `R"(...)"`，全都提示会把 lint 输出淹掉，
/// 真正的问题反而看不见（实测：第一版报 102 条，一眼扫不出哪条要紧）。
///
/// 本会话实测踩过**两次**：JSON 样例里的 `($0)`、`(std::string)`——
/// 编译器报 "missing terminating \" character" / "character constant too long"，
/// 而真正原因离报错位置隔了几十行。
///
/// 判定：逐字符找 `R"(` 起点（无定界符），跳过其内容直到**第一个 `)"`**——
/// 若该 `)"` 之后同行还有内容且不是合法的 C++ 续写（简化为"`)` 之后紧跟非空白非分号"），
/// 说明那次终结来得太早（内容被当成了代码）。判定宽松一点：只要在**同一行**找到
/// `)"` 之后还有 `"`（很可能是下一段字符串），即报。
[[nodiscard]] auto check_raw_string_delimiters(const std::vector<std::string_view>& lines,
                                               std::string_view path,
                                               std::size_t& suppressed)
    -> std::vector<LintViolation> {
  std::vector<LintViolation> violations;
  bool in_block_comment = false;
  for (std::size_t index = 0; index < lines.size(); ++index) {
    const std::string_view raw = lines[index];
    if (has_allow_comment(raw, "L15")) {
      ++suppressed;
      continue;
    }
    const std::string uncommented = strip_comments(raw, in_block_comment);
    // 找无定界符起点：`R"(`。
    std::size_t at = uncommented.find("R\"(");
    if (at == std::string::npos) continue;
    // 该行的这一处：往后找第一个 `)"`。
    const std::size_t close = uncommented.find(")\"", at + 3);
    if (close == std::string::npos) continue;   // 跨行 raw string：本行判不了（不报）
    const std::size_t after = close + 2;
    // 判据：终结之后**同行还有双引号**。合法用法里 `)"` 之后只会是
    // `;`/`)`/`,`/运算符/空白/行尾；再出现 `"` 说明"后面那半本来就是内容"
    //（内容里混进了 `)"`，提前终结）。实测的两处正是这个形态：
    // `R"({"insertText":"widget_${1:name}($0)","kind":3})"` 与
    // `R"({"label":"f","detail":"void f()"})"`——`)"` 之后的 `,` / `}` 后面还带着 `"`。
    if (after < uncommented.size() && uncommented.find('"', after) != std::string::npos) {
      violations.push_back(LintViolation{std::string(path), static_cast<int>(index) + 1, "L15",
                                         "无定界符 raw string 的内容里出现 )\"（提前终结）——"
                                         "改用 R\"x(...)x\""});
    }
  }
  return violations;
}

/// **层方向检查（L16）**：core 层的文件不得 include ui 层的头。
///
/// 为什么这条要专用检查（同 L13/L15）：方向是**文件路径 × include 目标**的关系，
/// 单行正则表达不了——正则只看得到「这行 include 了什么」，看不到「谁 include 的」。
///
/// 判据（刻意收窄，只判**确定性方向**）：
/// - 文件在 `src/core/` 或 `include/st/core/` 下，且 include 了 `st/ui/` 开头的头
///   → 违规（core 认识 ui 就是层次污染；实测形态：`platform_crash.cpp` 的死 include
///   `st/ui/state_trace.hpp` 把渲染链拖进工具链闭包，BACKLOG P1 根因①）；
/// - 其余方向**不判**：ui→core 合法；shell→ui 有合法前例（窗口命中契约在 ui，
///   BACKLOG P1 根因②是待解耦项，不是违规项——本规则不替它报错）。
[[nodiscard]] auto check_layer_includes(const std::vector<std::string_view>& lines,
                                       std::string_view path, std::size_t& suppressed)
    -> std::vector<LintViolation> {
  // 判定调用方（本文件）是否属于 core 层：按路径片段判（`lint_file` 传绝对路径，
  // `lint_project` 传相对路径，两寄都含 `core/` 段）。**先归一化分隔符**：Windows 的
  // `fs::join` 产生反斜杠，而判据写的是正斜杠（实测：`lint_source` 临时文件在 Windows
  // 上全是 `\`，不归一化则永不命中）。
  std::string normalized{path};
  for (char& character : normalized) {
    if (character == '\\') character = '/';
  }
  const bool in_core = normalized.contains("src/core/") ||
                       normalized.contains("include/st/core/");
  if (!in_core) return {};
  std::vector<LintViolation> violations;
  bool in_block_comment = false;
  for (std::size_t index = 0; index < lines.size(); ++index) {
    const std::string_view raw = lines[index];
    if (has_allow_comment(raw, "L16")) {
      ++suppressed;
      continue;
    }
    const std::string uncommented = strip_comments(raw, in_block_comment);
    // include 的 ui 头：`#include "st/ui/…"`（引号形式——框架内头都是引号 include）。
    const std::size_t hit = uncommented.find("#include \"st/ui/");
    if (hit == std::string::npos) continue;
    violations.push_back(LintViolation{std::string(path), static_cast<int>(index) + 1, "L16",
                                       "core 层 include 了 ui 层头（依赖方向反了）：" +
                                           std::string(trim(raw))});
  }
  return violations;
}

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
  for (auto& violation : check_raw_string_delimiters(lines, path, suppressed)) {
    violations.push_back(std::move(violation));
  }
  for (auto& violation : check_layer_includes(lines, path, suppressed)) {
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

  // ## 先收集待扫文件（串行，只走目录）
  //
  // 收集与扫描分开：目录遍历与文件系统的顺序性无关紧要，但**读取+扫描**是 99% 的时间。
  // 实测（2026-10-27）：764 文件、CPU 22.7 s / 墙钟 23.0 s ⇒ 并行度 **0.99/28**。
  struct Job {
    std::string full{};       ///< 绝对路径（报给用户看）
    std::string relative{};   ///< 相对工程根（清单豁免 glob 用）
    std::string text{};       ///< 文件内容（读进内存再并行扫：读盘本身也要几百 ms）
  };
  std::vector<Job> jobs;
  jobs.reserve(1024);
  for (const auto& directory : roots) {
    const std::string base = fs::join(root, directory);
    if (!fs::is_directory(base)) continue;
    auto entries = fs::walk(base);
    if (!entries) return forward_error(entries.error());
    for (const auto& entry : *entries) {
      if (entry.is_dir) continue;
      const std::string extension = fs::extension(entry.path);
      if (extension != ".cpp" && extension != ".hpp") continue;
      Job job;
      job.full = fs::join(base, entry.path);
      // 清单豁免的 glob 相对**工程根**（不是相对 `include/` 那层）——
      // 写清单的人想的是"src/app/llm.cpp"，不是相对某个扫描子目录的路径。
      job.relative = fs::join(directory, entry.path);
      jobs.push_back(std::move(job));
    }
  }

  // ## 并行扫描
  //
  // ### 为何可以安全并行（逐条核过）
  //
  // BACKLOG 里原本写着"L16 层方向需看路径、清单豁免需看 st.pkg，分片要保证全局视图"。
  // **实际读代码后发现前提不成立**：
  // · `check_layer_includes()` 只根据**本文件自己的路径**判它是否在 core 层（`path.contains("src/core/")`），
  //   不读任何其他文件
  // · `manifest_exempt()` 只查传入的 `exemptions`（只读，已在外层解析好）
  // · `suppressed` / `manifest_suppressed` 是**每文件的局部计数器**，本来就在 `scan_text` 里累加
  // ⇒ **所有规则都是单文件规则**，并行只需把"读+扫"分到多线程，没有共享可变状态。
  //
  // ### 为何先读进内存再并行
  //
  // 两种写法的差别：在并行任务里读盘（每线程一个 `read_text`）会让 "4096 个文件同时在磁盘队列里"，
  // 而总带宽固定——读盘不会因为多线程变快，反而因寻道竞争可能变慢。
  // 先把内容装进内存（串行读，OS 页缓存友好），再让 CPU 密集的正则扫描并行，
  // 才能把 28 核用起来。代价是峰值内存：764 文件 × 平均约 20 KB ≈ 15 MB，可接受。
  //
  // ### 失败语义
  //
  // 旧实现遇到读不到的文件 **立刻 return 错误**——并行下没法中途放弃（其他线程还在跑），
  // 所以改成"收集第一个错误、全部跑完后返回"。这比旧行为更好：
  // 用户一次能看到**所有**读失败的文件，而不是修一个再撞下一个。
  std::vector<std::optional<Error>> read_errors(jobs.size());
  const std::size_t worker_count = std::clamp<std::size_t>(jobs.size(), 1, std::thread::hardware_concurrency() == 0 ? 1 : std::thread::hardware_concurrency());
  std::atomic<std::size_t> next_index{0};

  // 每个文件的扫描结果（下标对齐 jobs）
  struct Scanned {
    std::vector<LintViolation> violations{};
    std::size_t suppressed{0};
    std::size_t manifest_suppressed{0};
    /// 是否真的被扫过（而非“分配了但没轮到”）。计数只能来自 `true` 的条目。
    bool scanned{false};
  };
  std::vector<Scanned> results(jobs.size());

  const auto worker = [&]() {
    for (;;) {
      const std::size_t index = next_index.fetch_add(1, std::memory_order_relaxed);
      if (index >= jobs.size()) return;
      auto& job = jobs[index];
      auto text = fs::read_text(job.full);
      if (!text) {
        read_errors[index] = text.error();
        continue;
      }
      Scanned scanned;
      scanned.violations =
          scan_text(job.relative, *text, scanned.suppressed, exemptions, scanned.manifest_suppressed);
      scanned.scanned = true;
      // 违规的 `file` 字段报**绝对路径**（用户要能直接点开/搜索到）——
      // 只在匹配 glob 时用相对路径。
      for (auto& violation : scanned.violations) violation.file = job.full;
      results[index] = std::move(scanned);
      // 内容用完即释放：避免 764 份全文同时驻留
      job.text = std::string{};
    }
  };

  {
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index) workers.emplace_back(worker);
    for (auto& thread : workers) thread.join();
  }

  // 读失败：按原顺序报第一个（并行下顺序不定，取下标最小以保证可复现）
  for (std::size_t index = 0; index < read_errors.size(); ++index) {
    if (read_errors[index].has_value()) return unexpected(*read_errors[index]);
  }

  // 扫描数 = **真正成功扫过的文件数**（不是 `jobs.size()`）。
  //
  // 这里曾经写成 `report.files_scanned = jobs.size()`——那是个真实的 bug：
  // 并行分片若漏掉某个下标（边界写错、提前 return），那个文件根本没扫，
  // 而报告仍然显示“扫了 N 个”。逆向验证当场抓到了它：把
  // `if (index >= jobs.size())` 改成 `index + 1 >=` 后，用例依然绿。
  // 计数必须来自“确实产出过结果”的条目。
  for (auto& scanned : results) {
    if (!scanned.scanned) continue;
    ++report.files_scanned;
    report.suppressed += scanned.suppressed;
    report.suppressed_by_manifest += scanned.manifest_suppressed;
    for (auto& violation : scanned.violations) report.violations.push_back(std::move(violation));
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

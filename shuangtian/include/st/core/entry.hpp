#pragma once

/// 程序入口适配：**跨平台的主函数启动器**。
///
/// 为什么需要它（而不是直接写 `int main(int argc, char** argv)`）：
///
/// | 平台 | `argv` 编码 | 后果 |
/// |---|---|---|
/// | Linux/macOS | UTF-8（与框架内部一致） | 无问题 |
/// | Windows | **本地 ANSI 代码页**（中文机器上是 GBK） | 中文命令行参数、中文路径参数**直接乱码/打不开文件** |
///
/// 框架内部一律 UTF-8（`CONVENTIONS` §3.7），因此 Windows 侧必须在入口处把
/// `GetCommandLineW()` 的宽字符参数转成 UTF-8 再交给业务代码；顺带把控制台输出设为 UTF-8，
/// 否则中文日志在 Windows 控制台也是乱码。
///
/// 用法（示例与工具程序统一这样写）：
///
/// ```cpp
/// #include "st/core/entry.hpp"
///
/// auto run_app(int argc, char** argv) -> int {   // 这里拿到的一定是 UTF-8 参数
///   ...
/// }
///
/// ST_MAIN(run_app)                               // 跨平台入口
/// ```

#include <string>
#include <vector>

namespace st {

/// 程序启动时的初始化（平台差异全部收在这里）。
///
/// 目前做两件事：① 解析出 **UTF-8 的** 命令行参数；② Windows 下把控制台输入/输出代码页设为 UTF-8。
/// 返回的参数向量与 `main(argc, argv)` 语义一致（`[0]` 是程序路径），但编码恒为 UTF-8。
[[nodiscard]] auto startup_arguments() -> std::vector<std::string>;

/// 仅做控制台编码设置（供自行解析参数的场景）。
void startup_configure_console();

/// **诊断落盘的两条自动接线**（由 `ST_MAIN` 调用；自行写 main 的应用可手动调）。
///
/// 为何放在这里而不是要求应用自己写：
/// “日志落盘 + 崩溃报告”是**出事之后才有价值**的能力，而 `main` 是应用**第一个**
/// 拿到命令行的地方——放在这儿，任何应用（包括以后新写的）默认就有，
/// 不必逐工程重复一遍。
///
/// 两条环境变量（命令行参数由 `parse_common_options` 处理，优先级更高）：
///
/// | 变量 | 含义 |
/// |---|---|
/// | `ST_LOG_FILE` | 日志文件路径（父目录自动建） |
/// | `ST_LOG_LEVEL` | `trace`/`debug`/`info`/`warn`/`error`/`off` |
/// | `ST_CRASH_DIR` | 崩溃报告落盘目录（文件名 `crash-<时间戳>-<pid>.log`） |
///
/// 为何也认命令行：智能体驱动无头应用时，**命令行是唯一稳定的注入点**
///（环境变量可能被上一层 shell 改掉）。而这份函数在参数解析之前跑，
/// 只做一次粗扫——真正的命令行解析仍归 `parse_common_options`。
void startup_configure_diagnostics(int argc, char** argv);

/// 正常退出时的收尾：flush 日志文件、删掉从未写过的崩溃报告。
void finalize_diagnostics();

/// 安装崩溃处理器（进程内一次性，幂等）：崩溃时把异常码/信号/地址+模块打到 stderr，
/// 不吞异常（交给系统默认处置）。无头应用崩溃没有控制台可看——这份记录是
/// 智能体/开发者事后定位的唯一线索（审视报告 P1-3）。入 ST_MAIN 时自动调用。
void install_crash_handler();

/// 注册"崩溃时追加输出"的提供者（可选；核心层不依赖上层诊断设施）。
///
/// 用途：`ST_TRACE_STATE=1` 的 State 写入追踪在 `ui` 层（`st/ui/state_trace.hpp`），
/// 而本文件在 `core` 层——**依赖方向是 core ← ui**，所以由上层在启动时注册
/// （见 `st::ui::trace::install_crash_hook()`），核心层只留一个函数指针。
/// 这样 `st` 构建器（只链 core）不会因 UI 层符号缺失而链接失败。
void set_crash_extra_provider(std::string (*provider)());

/// 设置崩溃报告文件路径（UTF-8）。**必须在 `install_crash_handler()` 之前调**——
/// 报告文件是启动时就打开的（见 `platform_crash.cpp` 里的理由：信号处理器只能调
/// 异步信号安全的函数，`fopen` 不在其中）。
///
/// 若已经开始崩溃采样，后续调用返回 false 且不生效（避免丢掉已写内容）。
[[nodiscard]] auto set_crash_report_path(std::string path) -> bool;

/// 当前崩溃报告文件路径（未开启时为空）。
[[nodiscard]] auto crash_report_path() -> std::string;

/// 崩溃报告**文件名**（`crash-<时间戳>-<pid>.log`）。
///
/// 单独抽出来是因为它有一条**只能在真机上看出来的契约**：文件名里不能有 `:`。
/// 时间戳用 `iso8601_now()`（形如 `2026-10-10T14:18:33.362Z`）时含 `:`，
/// 而 `:` 在 Windows 文件名里非法 ⇒ `open` 失败 ⇒ **崩溃报告永不生成**，
/// 且失败发生在启动时、用户看不到。抽成函数才能被回归测试直接钉住
/// （测“替换后能用”是假测试：那样测的是测试自己）。
///
/// 参数 `pid` 显式传入（而非内部取）——让测试能固定它。
[[nodiscard]] auto crash_report_file_name(std::int64_t unix_millis, unsigned long pid)
    -> std::string;

/// 正常退出时调用：删除从未被写过的崩溃报告文件。
///
/// 为何要删：报告文件是启动就建的（这样才能在崩溃瞬间写入）。若程序**正常**退出，
/// 这个文件里只有启动时的表头，留着会让事后看到它的人以为“曾经崩过”。
/// 返回是否确实删掉了一个文件。
[[nodiscard]] auto finalize_crash_report() -> bool;

}  // namespace st

/// 跨平台入口宏：把 `main` 的参数正规化成 UTF-8 后交给 `fn(argc, argv)`。
///
/// 宏而非函数：`main` 的签名与返回语义（进程退出码）只能由宏在调用点展开。
/// 这是 `CONVENTIONS` §8 L3「禁止函数式宏」的**登记豁免**：它不带函数式参数、
/// 只做入口样板展开，且这是唯一能在调用点生成 `main` 的手段。
#define ST_MAIN(fn)  /* lint-allow: L3 入口宏：唯一能在调用点生成 main 的手段 */ \
  auto main(int argc, char** argv) -> int {             \
    st::startup_configure_console();                    \
    st::startup_configure_diagnostics(argc, argv);      \
    st::install_crash_handler();                        \
    const std::vector<std::string> st_arguments = st::startup_arguments(); \
    (void)argc;                                         \
    (void)argv;                                         \
    std::vector<char*> st_argv;                         \
    st_argv.reserve(st_arguments.size() + 1);           \
    for (const auto& item : st_arguments) {             \
      st_argv.push_back(const_cast<char*>(item.c_str())); \
    }                                                   \
    st_argv.push_back(nullptr);                         \
    const int st_exit = fn(static_cast<int>(st_arguments.size()), st_argv.data()); \
    st::finalize_diagnostics();                         \
    return st_exit;                                     \
  }

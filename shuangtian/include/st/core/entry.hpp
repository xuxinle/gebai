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

}  // namespace st

/// 跨平台入口宏：把 `main` 的参数正规化成 UTF-8 后交给 `fn(argc, argv)`。
///
/// 宏而非函数：`main` 的签名与返回语义（进程退出码）只能由宏在调用点展开。
/// 这是 `CONVENTIONS` §8 L3「禁止函数式宏」的**登记豁免**：它不带函数式参数、
/// 只做入口样板展开，且这是唯一能在调用点生成 `main` 的手段。
#define ST_MAIN(fn)  /* lint-allow: L3 入口宏：唯一能在调用点生成 main 的手段 */ \
  auto main(int argc, char** argv) -> int {             \
    st::startup_configure_console();                    \
    const std::vector<std::string> st_arguments = st::startup_arguments(); \
    (void)argc;                                         \
    (void)argv;                                         \
    std::vector<char*> st_argv;                         \
    st_argv.reserve(st_arguments.size() + 1);           \
    for (const auto& item : st_arguments) {             \
      st_argv.push_back(const_cast<char*>(item.c_str())); \
    }                                                   \
    st_argv.push_back(nullptr);                         \
    return fn(static_cast<int>(st_arguments.size()), st_argv.data()); \
  }

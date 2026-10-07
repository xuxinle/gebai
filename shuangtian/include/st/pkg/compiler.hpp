#pragma once

/// 编译器族差异的**单点封装**（构建驱动用）。
///
/// 为什么要有这一层：清单（`st.pkg`）必须保持**跨平台写法**——同一份清单要能编
/// Linux/Windows/macOS，甚至同一个工程要同时支持"本机编译器"与"交叉 mingw"两套产物。
/// 因此差异不写进清单，而是集中在这里翻译：
///
/// - **标志拼法**：清单恒写 GCC 风格（`-O2`/`-Ifoo`/`-DNAME`），GCC 与 Clang 都认；
/// - **依赖产出**：`-MMD -MF` 直接产 `.d`（唯一路径）；
/// - **链接**：`-lfoo`；并丢掉不存在于该目标平台的库名（`pthread`/`dl`/`m` 在 Windows 上
///   不存在，照传就是链接失败）；
/// - **目标三元组**：`clang++` 的默认目标跟着**它自己的构建方式**走（LLVM 官方 Windows 包
///   是 MSVC 目标），要让 clang 不依赖 MSVC 就得显式给 `--target`——这是工具链清单里
///   `target_triple` 字段的用途，见 `MANIFEST` 的类型 `ToolchainSpec`。
///
/// ## 支持的编译器族：GCC 与 Clang
///
/// 两者都是"GCC 风格驱动 + libstdc++ 运行库"这一条口径：标志拼法相同、依赖产出相同、
/// 链接写法相同。Windows 上的 C++ 运行库由 **MinGW-w64 的 libstdc++** 提供
/// （g++ 自带；clang 用 `--target=x86_64-w64-windows-gnu` 吃同一套）。
///
/// **本层不内置 MSVC 支持**。理由不是"MSVC 不好"，而是它属于**另一条工具链口径**：
/// `cl.exe` 需要 `vcvars64.bat` 决定 `INCLUDE`/`LIB`/`PATH`，标志是 `/` 前缀、
/// 依赖产出是 `/sourceDependencies` 的 JSON、链接库要加 `.lib`、PCH 是 `/Yu`+`/FI`+`/Fp`
/// 三件套。把这些塞进主路径会让每个环节都多出一个"只在少数机器上生效"的分支
/// （而这正是它历史上多次静默失效的土壤：PCH 静默空转、依赖清单静默降级、
/// `/DEBUG` 放错位置导致 PDB 缺失——三处都是"看着没事、实际不对"）。
/// 真要支持它，应当在 `platform_*` 里新加一个族，而不是让默认路径背着它。

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"

namespace st::pkg {

/// 编译器族。
enum class CompilerKind : std::uint8_t { Gcc, Clang };

/// 源语言（C 与 C++ 的方言固定项不同）。
enum class SourceLanguage : std::uint8_t { Cxx, C };

/// 由编译器路径/名字判定族。名字含 `clang` → Clang；其余按 GCC 系。
///
/// **`clang-cl` 明确拒绝**：它是"MSVC 风味的 clang"（标志、环境、链接都按 MSVC 走），
/// 本层不支持 MSVC 口径，把它误判成 Clang 会给出 `-std=c++20` 而它期待 `/std:c++20`——
/// 报一堆看不懂的错。返回错误比静默误判省时间。
[[nodiscard]] auto compiler_kind_of(std::string_view compiler) -> Result<CompilerKind>;

[[nodiscard]] auto compiler_kind_name(CompilerKind kind) -> std::string_view;

/// 该族的**恒定**编译标志（方言固定项）。
///
/// GCC/Clang 在此返回空：`-std=c++20` 由清单的 `flags` 统一给出（两族拼法相同），
/// GCC 专有的档标（`-Wno-array-bounds` 等）由工具链的 `suppressions` 按族收敛
/// （见 `ToolchainSpec::suppressions`）。
[[nodiscard]] auto dialect_flags(CompilerKind kind, SourceLanguage language)
    -> std::vector<std::string>;

/// 预编译头**消费端**的追加标志（创建端在构建器里，见 `ensure_pch`）。
///
/// GCC/Clang 同一拼法：`-I<pch 目录>` + `-include <代理头>`。`.gch` 靠**同名规则**命中
/// （`prefix.hpp.gch` 与 `prefix.hpp` 同目录），无需显式指向；因此不像 MSVC 那样需要
/// 一个 `pch_file` 参数。
[[nodiscard]] auto pch_consume_args(std::string_view directory, std::string_view header)
    -> std::vector<std::string>;

/// 库名 → 该目标平台的链接参数（`-lfoo`）。
///
/// `platform` 是**目标**平台（不是宿主）：`pthread`/`dl`/`m` 在 `windows` 上要丢弃
/// ——mingw 根本没有这些库，照传就是 `cannot find -ldl`。
[[nodiscard]] auto link_library_arguments(std::string_view platform,
                                          const std::vector<std::string>& libraries)
    -> std::vector<std::string>;

/// 从编译标志里剥掉 sanitizer 相关项（第三方源用）。
///
/// vendored 的 `quickjs.c`/`sqlite3.c` 是上游代码：给它们插桩既改语义又拖慢构建，
/// 而它们自己的告警已被 `-w` 压掉。GCC 与 Clang 的 sanitizer 拼法相同，
/// 故本函数对两族行为一致——存在意义是"这个决定只写一处"。
[[nodiscard]] auto strip_sanitizers(const std::vector<std::string>& flags)
    -> std::vector<std::string>;

}  // namespace st::pkg

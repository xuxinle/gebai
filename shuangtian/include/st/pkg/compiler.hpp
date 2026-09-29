#pragma once

/// 编译器族差异的**单点封装**（构建驱动用）。
///
/// 为什么要有这一层：清单（`st.pkg`）必须保持**跨平台写法**——同一份清单要能编
/// Linux/Windows/macOS，甚至同一个工程要同时支持"本机 MSVC"与"交叉 mingw"两套产物。
/// 因此差异不写进清单，而是集中在这里翻译：
///
/// - **标志拼法**：`-O2` ↔ `/O2`、`-Ifoo` ↔ `/Ifoo`、`-DNAME` ↔ `/DNAME`；
/// - **依赖产出**：GCC 系 `-MMD -MF` 直接产 `.d`；MSVC 走 `/sourceDependencies` 的 JSON，
///   再由本层转成**同一种 `.d`**——下游的增量判新与共享对象缓存因此只有一套逻辑；
/// - **链接**：`-lfoo` ↔ `foo.lib`，并丢掉不存在于该目标平台的库名
///   （`pthread`/`dl`/`m` 在 Windows 上不存在，照传就是链接失败）；
/// - **运行环境**：MSVC 的 `cl.exe` 必须带上 `INCLUDE`/`LIB`/`PATH`（由 VS 的
///   `vcvars64.bat` 决定）。本层负责取回并注入子进程——**不手工拼 VS/SDK 目录布局**，
///   那套布局随 VS 版本变化，vcvars 才是官方口径。

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"

namespace st::pkg {

/// 编译器族。
enum class CompilerKind : std::uint8_t { Gcc, Clang, Msvc };

/// 源语言（C 与 C++ 的标志集不同：`/EHsc` 之类只对 C++ 有意义）。
enum class SourceLanguage : std::uint8_t { Cxx, C };

/// 由编译器路径/名字判定族（`cl`/`cl.exe` → MSVC；`clang-cl` 也是 MSVC 风味）。
[[nodiscard]] auto compiler_kind_of(std::string_view compiler) -> CompilerKind;
[[nodiscard]] auto compiler_kind_name(CompilerKind kind) -> std::string_view;

/// MSVC 工具集定位结果。
struct MsvcToolchain {
  std::string cl{};       ///< `cl.exe` 绝对路径
  std::string vcvars{};   ///< `vcvars64.bat` 绝对路径
  std::string version{};  ///< 工具集版本目录名（如 `14.51.36231`）
};

/// 定位 MSVC 工具集（Windows 上**首选**的编译器）。
///
/// 顺序：`ST_CL` / `ST_VCVARS` 环境变量（显式指定，离线/非常规安装用）
/// → `vswhere`（VS 官方查询器，能正确识别非默认安装路径）→ 常见安装根目录扫描。
/// 不用 `PATH` 查找：`cl.exe` 不在 `PATH` 里，必须先执行 `vcvars` 才可见——
/// 而"先执行 vcvars"这件事本身就需要先知道它在哪里。
[[nodiscard]] auto find_msvc_toolchain() -> Result<MsvcToolchain>;

/// 取编译器运行所需的环境变量（MSVC 需要；GCC 系返回空表）。
/// 实现是"跑一次 `vcvars64.bat` 再 `set`"，筛选出与编译/链接有关的键。
[[nodiscard]] auto compiler_environment(CompilerKind kind)
    -> Result<std::map<std::string, std::string>>;

/// 该族的**恒定**编译标志（编码/异常模型/依赖产出）。
/// 必须始终出现：缺 `/utf-8` 时 MSVC 按本地 ANSI 代码页读源码——本仓库的中文注释与
/// 界面文案会**静默乱码**（能编过，只是界面上全是乱字符）。
[[nodiscard]] auto dialect_flags(CompilerKind kind, SourceLanguage language)
    -> std::vector<std::string>;

/// 跨平台（GCC 风格）标志 → 该族等价标志。
/// 无法一一对应者被丢弃并记入 `dropped`（调用方可打印，避免"以为还在严格检查"）。
[[nodiscard]] auto translate_flags(CompilerKind kind, const std::vector<std::string>& flags,
                                   std::vector<std::string>* dropped = nullptr)
    -> std::vector<std::string>;

/// 库名 → 该族/该目标平台的链接参数。
/// `platform` 是**目标**平台（不是宿主）：`pthread`/`dl`/`m` 在 `windows` 上被丢弃。
[[nodiscard]] auto link_library_arguments(CompilerKind kind, std::string_view platform,
                                          const std::vector<std::string>& libraries)
    -> std::vector<std::string>;

/// MSVC `/sourceDependencies` 的 JSON → GCC 风格 `.d` 文本（供既有增量判定复用）。
/// 转义规则与 `parse_depfile` 对齐：空格写成 `\ `（否则 `C:\Program Files\...` 会被切成两个依赖）。
[[nodiscard]] auto depfile_from_source_dependencies(std::string_view json_text,
                                                    std::string_view object_path) -> Result<std::string>;

}  // namespace st::pkg

#pragma once

/// 独立工程对霜天框架的引用解析（`st.pkg` 的 `framework` 段）。
///
/// ## 语义：源码级依赖
///
/// 引用框架 = **把框架的库部分并入本工程的构建图**：
/// 包含目录、编译标志与宏、系统库、第三方 C 源（QuickJS）、编译期嵌入（脚本运行时前置）、
/// 以及框架源（**排除 `src/pkg/*`**——那是工具链自身，应用用不到）。
///
/// 不做预编译库 + 安装步骤：安装位置、ABI/编译器版本、交叉编译两套产物都是持续的麻烦；
/// 源码级引用**无需安装、始终同版本、离线可构建、交叉编译天然生效**。
/// 代价（每个工程首次要编框架源）由对象缓存摊掉（见 `pkg/cache.hpp`）。
///
/// ## 什么**不**并入
///
/// - 框架的 `targets`（那是框架自己的示例/工具，与引用方无关）
/// - 框架的 `tests`（引用方不跑框架的单测）
/// - `src/pkg/*`（工具链实现；框架清单里本就以 `exclude_sources` 排除）
/// - 框架的 `toolchains`（工具链属于**引用方**的构建环境，不该被依赖方覆盖）

#include <string>
#include <vector>

#include "st/core/error.hpp"
#include "st/pkg/manifest.hpp"

namespace st::pkg {

/// 解析后的框架贡献。
struct Framework {
  std::string directory{};  ///< 框架根（绝对）
  std::string name{};       ///< 框架工程名（标识符前缀、诊断用）
  std::string version{};    ///< 框架版本（诊断用）

  /// 供 `-I` 的包含目录（绝对）。
  std::vector<std::string> include_dirs{};
  /// 框架源（绝对路径，已排除工具链源）。
  std::vector<std::string> sources{};
  /// 框架的第三方 C 源（绝对路径；按第三方对待：放宽告警、不进 PCH、不插桩）。
  std::vector<std::string> third_party_sources{};
  /// 编译标志 / C 标志 / 宏 / 系统库（`inherit_flags=false` 时标志与宏为空）。
  std::vector<std::string> flags{};
  std::vector<std::string> c_flags{};
  std::vector<std::string> defines{};
  std::vector<std::string> system_libs{};
  /// 工程级嵌入 glob（相对框架根）。
  std::vector<std::string> embed{};
  /// 框架声明的交叉编译工具链。
  ///
  /// **要继承**：目标平台的系统库（`ws2_32`/`gdi32`/`user32`）与宏（`_WIN32_WINNT`）
  /// 是"框架与平台如何配合"的知识，不该要求引用方自己知道；引用方同名可覆盖。
  std::vector<ToolchainSpec> toolchains{};
  /// 嵌入模板与运行时所在目录（`<framework>/third_party/battery`）。
  std::string embed_support_directory{};
};

/// 解析框架引用。错误：`NotFound`（目录/清单缺失）、`Invalid`（路径不是有效的框架根）。
[[nodiscard]] auto load_framework(const FrameworkSpec& spec) -> Result<Framework>;

/// 把路径解析成绝对路径（相对路径按 `base` 解析）；不做存在性检查。
[[nodiscard]] auto resolve_spec_path(std::string_view base, std::string_view path) -> std::string;

}  // namespace st::pkg

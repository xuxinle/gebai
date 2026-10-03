#pragma once

/// 构建驱动：解析清单 → 生成编译命令 → **直接调用编译器**（不经 CMake/Make）→ 增量构建 → 链接。
/// 产物布局：`<root>/build/<profile>/{obj,bin}/…`。

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"
#include "st/pkg/manifest.hpp"

namespace st::pkg {

struct BuildOptions {
  std::string root{};                 ///< 工程根（空=清单目录）
  std::string profile{"debug"};       ///< debug / release / san
  std::string target{};               ///< 目标名（空=库目标：只编译不链接）
  std::size_t jobs{0};                ///< 并行编译单元数（0=按**内存预算**与硬件并发推导，见 `pkg/memory.hpp`）
  /// 超大翻译单元的并发上限（0=自动：1）。超大单元按源文件字节数判定。
  std::size_t jobs_large{0};
  /// 可用内存预算（MiB；0=自动探测 cgroup/系统上限）。推导并行度用。
  std::uint64_t max_memory_mb{0};
  /// 交叉编译工具链名（空 = 本机）。命中 `Manifest::toolchains` 中的一项：
  /// 决定编译器、目标系统库、产物后缀与平台宏，并把产物/中间目录与本地档**隔离**。
  std::string toolchain{};
  bool verbose{false};                ///< 打印每条编译命令
  bool force{false};                  ///< 忽略增量判定，全量重编
  bool use_pch{true};                 ///< 使用预编译头（`include/st/pch.hpp`）加速
  std::vector<std::string> extra_include_dirs{};
  std::vector<std::string> extra_flags{};
};

struct BuildStats {
  std::string artifact{};
  std::size_t units_total{0};
  std::size_t units_rebuilt{0};
  std::size_t units_cached{0};
  std::int64_t elapsed_ms{0};
  std::int64_t compile_ms{0};  ///< 编译阶段耗时（不含链接）
  /// 纯链接阶段耗时。**不等同于 `elapsed - compile`**：后者还包含目标单元（应用源码）的
  /// 编译——应用单元在库编译**之后**另起一轮，两者之间还夹着链接指纹比对，
  /// 把它算成"链接"会让耗时分解失真（改一个应用 .cpp 后"链接 9 s"其实是编译 8.5 s）。
  std::int64_t link_ms{0};
  std::size_t workers{0};      ///< 并行度（按内存预算推导而来，除非显式 `--jobs`）
  /// 超大翻译单元的并发上限（源文件 ≥ `kLargeUnitBytes` 者走这道窄闸门）。
  std::size_t workers_large{0};
  /// 采用的编译内存预算（MiB；0 = 不可知，并发退回硬件数）。
  std::uint64_t memory_budget_mb{0};
  /// 并发决策的理由（直接展示给用户："为什么是这个并发数"）。
  std::string concurrency_reason{};
  bool pch_used{false};        ///< 是否用到预编译头
  bool linked{false};          ///< 本次是否真的执行了链接（产物已最新则跳过）
  /// 产物能否在宿主直接执行：交叉档里**目标平台 == 宿主平台**的产物可跑
  /// （Windows 宿主上 mingw 交叉档产出本机 PE）；目标≠宿主（Linux 宿主编 mingw）
  /// 为 false——`st run`/`st test` 拒绝执行并提示在目标平台运行。
  bool runs_on_host{false};
};

/// profile → 编译/链接标志（debug: -O0 -g / release: -O2 -DNDEBUG / san: ASan+UBSan）。
[[nodiscard]] auto profile_flags(std::string_view profile) -> Result<std::vector<std::string>>;
/// 链接所需的系统库（按平台）。
[[nodiscard]] auto default_system_libs() -> std::vector<std::string>;

/// 检测编译器（`ST_CXX`/`CXX` 环境变量 → g++ → clang++ → c++）。
/// 探测 C++ 编译器（`ST_CXX`/`CXX` 环境变量优先，其次 g++/clang++/c++）。
/// `override_compiler` 非空时直接用它（交叉编译工具链走这条路）。
[[nodiscard]] auto detect_compiler(std::string_view override_compiler = {}) -> Result<std::string>;

/// 构建库对象与目标产物（`options.target` 为空时只编译库对象）。
[[nodiscard]] auto build(const Manifest& manifest, const BuildOptions& options) -> Result<BuildStats>;

/// 构建测试可执行文件（库源 + tests + 测试框架入口）并运行；返回退出码。
/// `list_only` = 只列出用例名（`st test --list`，仍可带 filter）；
/// `junit_path` 非空时给测试进程设 `ST_JUNIT_XML`，逐用例结果写成 JUnit XML（CI 消费）。
[[nodiscard]] auto run_tests(const Manifest& manifest, const BuildOptions& options,
                             std::string_view filter, bool list_only = false,
                             std::string_view junit_path = {}) -> Result<int>;

}  // namespace st::pkg

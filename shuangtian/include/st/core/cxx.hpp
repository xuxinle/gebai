#pragma once

/// 语言标准的**唯一守卫**：本仓源码按 C++23 编写，低于该标准必须立刻停下。
///
/// 为什么要有这一道：语言标准有两个入口——清单 `st.pkg` 的 `cxx_standard`（构建驱动据此
/// 生成 `-std=c++XX`），以及**绕过构建驱动**的各种手工路径（探针脚本、外部工程、IDE 的
/// 编译数据库、直接 `g++ foo.cpp`）。后者不会读清单，于是"标准被调低了"这件事没有任何
/// 征兆：`std::expected`/`std::mdspan` 之类会以**别的名字**报错（甚至只是行为不同），
/// 排查会从"我的代码哪写错了"开始，离真因很远。
///
/// 判据是 `__cplusplus`：C++23 下 GCC 与 Clang 都给 `202302L`（`/std:c++latest` 之外的
/// MSVC 给 `_MSVC_LANG`）。本仓不支持 MSVC 口径（见 `st/pkg/compiler.hpp`），故只看前者。
///
/// 同时导出**可选设施探测**函数：有些 C++23 设施在本仓的两个编译器族上并不齐（`<print>` /
/// `<stacktrace>` 的实现住在需要显式链接的 `libstdc++exp` 里、clang 目标下 `<generator>` 缺失），
/// 业务代码据此决定走哪条路，
/// 而不是各自写 `#ifdef`——`#ifdef` 散开之后，"到底有没有这个能力"就没人能回答了。
///
/// 包含位置：本头由 `st/core/error.hpp` 与预编译头 `st/pch.hpp` 引入，因此**任何**包含
/// 框架头的翻译单元都先过这道守卫，不需要每个 .cpp 各自记得。

#if defined(_MSVC_LANG) && !defined(__clang__)
#error "霜天不支持 MSVC 口径的工具链（请用 GCC 风格的 g++ 或 clang++，见 CONVENTIONS.md §10.1）"
#endif

#if !defined(__cplusplus) || __cplusplus < 202302L
#error "霜天源码要求 C++23（-std=c++23）。构建请经 `st build`（标准取自清单 st.pkg 的 cxx_standard）；手工编译请显式加 -std=c++23。"
#endif

#include <version>

namespace st {

/// 本仓要求的语言标准年号（与清单 `st.pkg` 的 `cxx_standard` 缺省值一致）。
///
/// 与 `cxx_standard_in_use()` 一样用函数而非 `inline constexpr` 变量：
/// lint L8 只看声明形态，`constexpr` 变量与可变全局在它眼里是同一类。
[[nodiscard]] consteval auto cxx_standard() noexcept -> long { return 23; }

/// `<print>` / `<println>`：**宏在 ≠ 能链接**。
///
/// 实测（MinGW-w64 GCC 16.2 + libstdc++ 16）：`#include <print>` 与 `std::print(...)` 都能
/// 编过，**默认链接**才报 `undefined reference to std::__open_terminal` /
/// `std::__write_to_terminal`。
///
/// ⚠ 这两个符号**不是没有实现**，而是实现在 **`libstdc++exp.a`** 里（`print.o`）：
/// 加 `-lstdc++exp` 后 g++ 与 clang（gnu 目标）都能正常跑，中文/emoji/重定向均正确。
/// 所以本探针只能回答“宏在不在”——**是否可用要连构建侧的链接能力一起判**，
/// 而本仓目前**不接**它（理由与代价见 `docs/BACKLOG.md` 的 P1 条，不是“不可用”）。
/// 遇到 `undefined reference` 时先 `nm -A <候选库>.a | grep <符号>`，别直接归因成平台不支持。
///
/// 为什么写成 `consteval` 函数而不是 `inline constexpr bool`：后者会被 lint L8
/// （禁止可变的进程级全局状态）判为全局变量——它只看声明形态、分不清 const 与否，
/// 所以「能力探测」这一类常量统一用函数形态，既保持编译期求值，也不引出一条豁免。
[[nodiscard]] consteval auto has_lib_print() noexcept -> bool {
#if defined(__cpp_lib_print)
  return true;
#else
  return false;
#endif
}

/// `<generator>`（协程生成器）：GCC 有、**clang 目标下缺**（libstdc++ 的
/// `__glibcxx_coroutine` 门控在 clang 上不成立）。跨族代码不得无条件依赖它。
[[nodiscard]] consteval auto has_lib_generator() noexcept -> bool {
#if defined(__cpp_lib_generator)
  return true;
#else
  return false;
#endif
}

/// `<stacktrace>`：与 `std::print` **同处 `libstdc++exp.a`**，但两件事结果不同——
/// 它加 `-lstdc++exp` 后能链接、`current()` 也回帧，**但所有帧同名**（符号化失效，
/// 实测 5 帧全叫 `register_frame_ctor`）。本仓**不依赖**它（崩溃回溯仍走自研 +
/// addr2line/DbgHelp）；判据与证据见 `tools/stacktrace_probe.cpp`。
/// 注意：**同一个库里一个能用一个不能用**，所以不能整库推广“加了就行”。
[[nodiscard]] consteval auto has_lib_stacktrace() noexcept -> bool {
#if defined(__cpp_lib_stacktrace)
  return true;
#else
  return false;
#endif
}

/// `<expected>`：本仓 `Result<T>` 的内芯依赖它（C++23 起可用）。
[[nodiscard]] consteval auto has_lib_expected() noexcept -> bool {
#if defined(__cpp_lib_expected)
  return true;
#else
  return false;
#endif
}

/// 本翻译单元实际使用的标准年号（`__cplusplus` 的标准化形态：202302L → 23）。
[[nodiscard]] constexpr auto cxx_standard_in_use() noexcept -> long {
  if constexpr (__cplusplus >= 202302L) return 23;
  if constexpr (__cplusplus >= 202002L) return 20;
  return 0;
}

}  // namespace st

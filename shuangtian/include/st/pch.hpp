#pragma once

/// 预编译头（PCH）：集中包含高频**标准库**头。
///
/// 为什么需要：霜天不用第三方依赖，但**标准库头部解析**占单文件编译时间的大头
/// （`<format>`/`<filesystem>`/`<thread>` 尤重）。把这些头一次性预编译，后续每个编译单元复用。
///
/// **刻意只放标准库**（不放 `st/core/*` 等项目头）：预编译头一旦过大，加载成本会超过收益——
/// 实测把项目头也塞进 PCH，单个 TU 反而从 ~3.5s 涨到 ~5.0s（项目头被不需要它的 TU 白白解析）。
/// 项目头由真正需要它们的 .cpp 自行包含，保持"按需解析"。
///
/// 使用方式（由 `stpm` 自动完成，无需手工调用）：
/// `st build` 会先编译本文件为 `.gch`，再以 `-include st.hpp` 让每个 .cpp 复用。
/// 手工编译时同样可以照此使用；不加 PCH 也能正常编译（本头**不引入任何语义**）。

// —— 语言标准守卫与可选设施探测 ——
// PCH 包含了全部标准库头，本就只有在 C++23 下才能成形；把守卫放在最前，
// 让任何走 PCH 的单元在报错时看到的是"标准低了"，而不是一大堆标准库内部错。
#include "st/core/cxx.hpp"

// —— C++23 设施 ——
// 只收**本仓真在用且两族都能用**的：`<expected>`（Result 内芯）、`<mdspan>`（像素视图）、
// `<flat_map>`（小表查找）。
//
// ⚠ `std::move_only_function` 的宏 `__cpp_lib_move_only_function` 在 libstdc++ 16 里**已定义**，
// 但**没有独立头** `<move_only_function>`（它住在 `<functional>` 里）——按宏去写
// `#include <move_only_function>` 会直接 `fatal error: No such file or directory`。
// 这就是“能力宏存在 ≠ 该这么包含”的典型：要查头的就查 `__has_include`。
//
// 刻意**不收**的三个（用 `st::has_lib_*` 判断，不进 PCH）：
// - `<print>` / `<stacktrace>`：实现住在 **`libstdc++exp.a`**（`print.o` / `stacktrace.o`），
//   后者主库里没有副本且**不自动链接**——加 `-lstdc++exp` 后 `std::print` 可用，
//   而 `std::stacktrace` 能链但符号化失效（逐设施验，不要整库推广）。
// - `<generator>`：clang 目标下缺失（libstdc++ 协程门控不成立）；
// - `<stacktrace>`：GCC 要额外链接 `libstdc++_libbacktrace`，发行包常不带。
#include <expected>
#include <flat_map>
#include <mdspan>

// —— 语言设施 ——
#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <queue>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#if defined(__cpp_lib_format)
#include <format>
#endif

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
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <queue>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

// —— 刻意**不收**并发组（`<thread>` / `<mutex>` / `<chrono>` / `<atomic>` / `<future>` / `<stop_token>`）——
//
// 实测（2026-10-26）：这组占 PCH 体积 **+24.6 MB**（143.0 → 167 之前累计的 17%），
// 而只用它们的单元是少数（147 个 .cpp 里直接包含 `<thread>` 的 6 个、`<future>` 的 1 个）。
// 移出后：PCH 143.9 → 119.1 MB（−17%），单单元（3 个代表取最小）18.27 → 16.61 s（−9.1%）。
//
// **前提是自包含**：移出前有 13 个文件“用并发类型却不 include”，靠 PCH 供头。
// 那些文件已补齐（`<thread>`/`<mutex>`/`<stop_token>`/`<chrono>`/`<atomic>`），
// 并逐个验证过“不带 PCH 也能单独编译”（12 个 .cpp 全过）。
//
// 若现在有人在新增代码里忘写 include，症状会是 `'thread' is not a member of 'std'`——
// 报错指向使用处，而根因是漏 include。这是有意保留的代价：它可发现，
// 而“PCH 养着漏 include 的文件”会在换构建系统/加新单元时突然集体爆发。
//
// `st pch` 不提供“临时加回来”的开关：那样两份配置会漂移，而 PCH 恰恰是
// “创建与消费必须标志一致”才有效的设施（见 `ensure_pch` 的三条关键细节）。

#if defined(__cpp_lib_format)
#include <format>
#endif

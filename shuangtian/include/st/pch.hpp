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

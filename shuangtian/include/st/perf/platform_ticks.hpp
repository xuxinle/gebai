#pragma once

/// @file platform_ticks.hpp
/// **平台单点**：读取 CPU 的高精度刻度（x86 上是 TSC，其他平台返回不可用）。
///
/// 为什么要单独一层：`__rdtsc` / `intrin.h` 是**平台差异**，按 `CONVENTIONS.md` §10
/// 只能出现在 `platform_*` 里。剖析的内部逻辑（标定、换算、开关）在 `clock.cpp`，
/// 它只通过这里的两个函数与平台打交道——于是"换平台要改哪里"是一个文件的答案。
///
/// 契约：
/// - `ticks_available()`：**纯查询**，无副作用、可随时调用；
/// - `read_ticks()`：单调递增（不保证连续）、无分配、开销约 5~20 个周期。
///   不可用时返回单调时钟纳秒（调用方须先查 `ticks_available()` 再换算）。

#include <cstdint>

namespace st::perf::platform {

/// 本平台是否有高精度刻度可用（x86/x64 为真）。
[[nodiscard]] auto ticks_available() noexcept -> bool;

/// 读一次刻度。**不做任何修正**（乱序/迁移的稳健性由上层标定与"只取差值"的用法保证）。
[[nodiscard]] auto read_ticks() noexcept -> std::int64_t;

}  // namespace st::perf::platform

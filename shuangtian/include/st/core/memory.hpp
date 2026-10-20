#pragma once

/// 进程内存统计（平台无关封装）。
///
/// 为什么需要：诊断"首帧慢"时，**缺页数**是区分"冷内存"与"纯 CPU 慢"的关键量
/// （见 `BACKLOG` 的「大 Markdown 预览首帧 58ms」条目与
/// `tools/cold_memory_probe.cpp`）。而平台 API 是 `GetProcessMemoryInfo`（Windows）
/// / `getrusage`（POSIX）——按 `CONVENTIONS` §10，系统头只能在 `platform_*` 里，
/// 所以这里给一个平台无关的口子，而不是让探针各写一份 `#if defined(_WIN32)`。

#include <cstdint>

namespace st::memory {

/// 进程自启动以来的**累计页错误数**（软 + 硬）。
///
/// 用途：在两次采样之间取差，就得到该段代码触发了多少次缺页。
/// 读不到时返回 0（调用方据此判断"本平台不支持"，不要当"零缺页"用）。
[[nodiscard]] auto page_fault_count() -> std::uint64_t;

/// 进程当前工作集（驻留内存）字节数。读不到返回 0。
[[nodiscard]] auto working_set_bytes() -> std::uint64_t;

/// 本平台是否支持上述查询（不支持的平台上两个函数恒返回 0）。
[[nodiscard]] auto has_stats() noexcept -> bool;

}  // namespace st::memory

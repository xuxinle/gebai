#pragma once

/// @file startup.hpp
/// **冷启动分解**：进程从入口到首屏，每一步花了多久。
///
/// ## 为什么单列一份
///
/// 稳态帧耗时（`FrameProfiler`）与启动耗时是**两件事**：实测同一程序的
/// 稳态帧是 3.9 ms，而首帧 203 ms——那 200 ms 全在"字体首次光栅化 + 图元缓存建表"
/// 上，它不属于任何一帧的阶段（`layout`/`paint`/`present` 三段加起来只有 2 ms）。
/// 只看帧剖析会把启动慢**完全藏起来**（而且冷启动帧还被刻意排除出分位，更看不见了）。
///
/// ## 用法
///
/// `ST_PERF_STARTUP=1` 开启（或 `--perf-startup`）。开后在进程退出时输出一份
/// `kind: "startup"` 的报告；也可以在运行期经 `st::perf::startup_report()` 取。
///
/// 埋点方式：在关键里程碑处调 `st::perf::startup_mark("字体加载")`（同名多次
/// 只累计、不重复建桶）。
///
/// ## 与 `--perf-frames` 的关系
///
/// 两者**独立**：启动分解不需要帧剖析，帧剖析也不包含启动。报告里都带
/// `schema` 与 `caveats`，可以并排读。

#include <cstdint>
#include <string>
#include <string_view>

#include "st/perf/report.hpp"

namespace st::perf {

/// 启动分解是否开启（`ST_PERF_STARTUP=1`）。
[[nodiscard]] auto startup_enabled() noexcept -> bool;
void set_startup_enabled(bool value) noexcept;

/// 进程**入口时刻**（纳秒，单调时钟）。所有里程碑的相对零点。
///
/// 由 `startup_mark("process.start")` 或第一次 `startup_mark` 自动建立。
[[nodiscard]] auto startup_origin_ns() noexcept -> std::int64_t;

/// 记一个里程碑。相对上一个里程碑的间隔进 `steps`，从入口起的累计进 `cumulative`。
///
/// 同名重复调用**累加**同名步骤耗时（例如"字体加载"发生 6 次），
/// 但报告的 `steps` 里仍然只有一行——读者要看的是"字体一共花了多少"。
void startup_mark(std::string_view name);

/// 启动阶段的耗时是否值得报告（有至少两个里程碑）。
[[nodiscard]] auto startup_has_data() noexcept -> bool;

/// 生成启动报告（`kind: "startup"`）。无数据时返回 `nullopt`。
///
/// 报告的 `sections.metrics` 里放 `total_ms`（首步到末步）与各步骤的 `*_ms`，
/// 于是 `st perf --compare` 能把"启动变慢"当成普通指标比较。
[[nodiscard]] auto startup_report(const MeasurementContext& context) -> Report;

/// 把启动报告打到日志（`ST_PERF_STARTUP=1` 时在应用退出时自动调用）。
///
/// 只打**最贵的几步 + 总时长**：启动日志最常见的用途是"这次比上次慢没有"，
/// 而不是逐个步骤核对。
void log_startup_summary();

/// 清空所有里程碑（测试用）。
void startup_reset();

}  // namespace st::perf

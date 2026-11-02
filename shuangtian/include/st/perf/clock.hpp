#pragma once

/// @file clock.hpp
/// 剖析专用的计时原语：**单调、零分配、可被关掉**。
///
/// ## 与 `st::time` 的分工
///
/// `st::time::now_ns()` 是通用时钟。剖析走的是**另一条路**：一次帧内剖析会在热路径上
/// 调用上万次，而一次 `steady_clock::now()` 在高并发环境下约几十纳秒——一个元素
/// 两次读表就是几十纳秒，而"画一个小方块"本身只要几百纳秒，**量尺会吃掉被测对象**。
///
/// 所以这里给两级时钟：
/// | 时钟 | 成本 | 用途 |
/// |---|---|---|
/// | `now_ns()` | 中 | 阶段级（一帧十几次） |
/// | `now_ticks()` | 低（x86 上是 TSC） | 元素级、原语级（一帧上万次） |
///
/// 刻度换算在**首次调用时标定一次**（用 `now_ns` 对时，取多次测量的最小值以免被
/// 调度抖动污染），之后是纯乘法。平台差异（读 TSC）在 `platform_ticks.cpp` 单点。
///
/// ## 单位
///
/// `now_ticks()` 返回**刻度**，只能求差后交给 `ticks_to_ns()`。在无高精度刻度的
/// 平台上它退化为纳秒，`ticks_to_ns` 也就退化为恒等——上层代码不必分平台写。
///
/// ## 关闭时零成本
///
/// `enabled()` 由 `ST_PERF` 决定（`0`/`off` 或空 = 关）。所有探针入口要么先问
/// `enabled()`，要么由调用方判 `if`——**关掉时不读时钟**。这是硬要求：剖析必须能
/// 常驻在代码里而不影响生产帧耗时。

#include <atomic>
#include <cstdint>

namespace st::perf {

/// 剖析总开关（`ST_PERF=1|on` 开；缺省关）。进程内取一次后缓存。
///
/// 为何缺省关：剖析的固定成本（每次读时钟 + 每帧一次快照）在 60 FPS 预算下
/// 不是"可以忽略"的量级——默认开着等于替用户做了性能决策。
[[nodiscard]] auto enabled() noexcept -> bool;

/// 强制开启/关闭（测试与"运行期按需采样"用）。
///
/// 覆盖环境变量的判定结果；传 `std::nullopt` 恢复环境口径。存在的理由是
/// **运行中的进程要能开始/停止采样**：`ST_PERF` 是启动期的决定，而"我这会儿
/// 觉得卡，抓 200 帧看看"是运行期的需求（控制通道的 `perf` 方法走这条）。
void set_enabled(bool value) noexcept;
void clear_forced_enabled() noexcept;

/// 单调纳秒（阶段级计时）。
[[nodiscard]] auto now_ns() noexcept -> std::int64_t;

/// 低开销刻度（元素级/原语级计时）。只能喂给 `ticks_to_ns` 求差。
[[nodiscard]] auto now_ticks() noexcept -> std::int64_t;

/// 刻度 → 纳秒（`delta_ticks <= 0` 返回 0）。
[[nodiscard]] auto ticks_to_ns(std::int64_t delta_ticks) noexcept -> std::int64_t;

/// 是否用上了高精度刻度（`false` = 退化为 `steady_clock`）。
///
/// 报告里要如实说明："元素级耗时来自 TSC 标定，误差 ≤ x%" vs
/// "退化为 chrono 时钟，元素级读数含读数开销"。**不许让读者猜**。
[[nodiscard]] auto high_resolution_clock() noexcept -> bool;

/// 刻度标定的相对误差上界（用于报告自述；无高精度刻度时为 1.0 = 不可信）。
[[nodiscard]] auto clock_uncertainty() noexcept -> double;

}  // namespace st::perf

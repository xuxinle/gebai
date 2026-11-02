#pragma once

/// @file histogram.hpp
/// 时间直方图（**增量分位**）：p50/p90/p95/p99/max/mean 边采边算，不留样本。
///
/// ## 为什么不用 `std::vector<double>` 排序
///
/// 帧路径上的度量必须满足两件事，排序法都做不到：
/// 1. **恒定内存**：留 600 帧样本 × 每帧几十个阶段 = 几万 double，还要在报告时
///    拷贝排序（一次几十微秒的分配，正好污染它在量的那段代码）；
/// 2. **任何时刻可读**：`metrics` 之类的查询可能出现在任意帧之后，而"攒够样本
///    再排序"意味着查询本身要为排序付钱。
///
/// 所以用**定桶对数直方图**：桶边界按 `2^(k/8)` 递增（每倍频 8 个桶，相对误差
/// 上限 ≈ 4.4%），总量固定 `kBins` 个 `uint32`，`add()` 是一次乘法 + 一次查表。
///
/// ## 分位是**桶上界**，不是精确值
///
/// `quantile(0.95)` 返回所在桶的**上界**：读出来的数**不会低于实测值**
/// （性能数据里宁可略微高估，也不要报出一个偏乐观的数）。误差上限写进
/// `max_relative_error()`，报告里一并带出去——**不许留下裸数字**。
///
/// ## 读数自带的"这支量尺今天可信吗"
///
/// `count()` 太小（性能噪声相对更大）时 `reliable()` 为假，报告里必须把这一点
/// 说出来，而不是照样给一个 p95。这是本仓反复出现的假绿形态（"零红没报分辨力"）。

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace st::perf {

/// 一个直方图最多能表示的**上界**（纳秒）：约 268 秒。
///
/// 为何要有上界而不是无界增长：桶索引是 `log` 出来的，无上界意味着
/// 一个异常大的样本（比如误把"进程启动到现在"记成一次耗时）会把整张表的
/// 分辨率挤掉。超出上界的样本计入 `overflow_count`（**不丢、不改数值口径**），
/// 报告里能看到"有样本溢出"这件事。
inline constexpr std::int64_t kHistogramCeilingNs = 268'435'456'000LL;

/// 桶数：`2^(k/8)` 覆盖 1 ns ~ 268 s（≈ 28 个倍频 × 8）。
inline constexpr std::size_t kHistogramBins = 232;

/// 每倍频的桶数（8 ⇒ 相对误差上限 ≈ 2^(1/8) − 1 ≈ 9.05%，
/// 取桶上界时读到的值 ≥ 真值，故实际"高估不超过 9.05%"，中位数误差约 4.4%）。
inline constexpr int kBinsPerOctave = 8;

/// 时间直方图（纳秒）。
///
/// 复制是廉价的（固定大小 POD 数组），可以在快照里整份带走。
struct Histogram {
  std::array<std::uint32_t, kHistogramBins> bins{};
  std::uint64_t count{0};      ///< 落入桶内的样本数（不含溢出）
  std::uint64_t overflow{0};   ///< 超出上界的样本数（>0 = 读数不可全信）
  std::int64_t sum_ns{0};
  std::int64_t min_ns{0};
  std::int64_t max_ns{0};

  /// 记一个样本。`ns <= 0`（时钟抖动/未计时）按 1 ns 记——**不丢样本**，
  /// 但也不让它拉低分位（0 与 1 在桶 0 里等价）。
  void add(std::int64_t ns) noexcept;

  /// 合并另一个直方图（逐桶相加 + 标量合并）。用于"多线程各记一份，末了汇总"。
  void merge(const Histogram& other) noexcept;

  [[nodiscard]] auto empty() const noexcept -> bool { return count == 0 && overflow == 0; }

  /// 分位（`ratio` ∈ [0,1]）。空直方图返回 0。返回**桶上界**（≥ 真值）。
  [[nodiscard]] auto quantile_ns(double ratio) const noexcept -> std::int64_t;
  /// 同上，转毫秒。
  [[nodiscard]] auto quantile_ms(double ratio) const noexcept -> double;
  [[nodiscard]] auto mean_ns() const noexcept -> double;
  [[nodiscard]] auto mean_ms() const noexcept -> double;
  [[nodiscard]] auto min_ms() const noexcept -> double;
  [[nodiscard]] auto max_ms() const noexcept -> double;
  /// 全部样本的耗时之和（毫秒）。**这才是"总成本"**：分位只回答"典型一次多贵"。
  [[nodiscard]] auto total_ms() const noexcept -> double;

  /// 分位读数的相对误差上限（= 桶上界 / 桶下界 − 1）。随桶号增大而稳定在 9.05%。
  [[nodiscard]] auto max_relative_error() const noexcept -> double;

  /// **样本量够不够支撑结论**。阈值取 30（性能统计里"少于 30 个样本谈分位没有意义"
  /// 的下限惯例）；不足时报告必须标注，而不是照样给 p95。
  [[nodiscard]] auto reliable() const noexcept -> bool { return count >= 30; }

  /// 一句话自述（`n=600 p50=3.20ms p95=4.10ms max=9.8ms err≤9.1%`）。
  /// 面向"直接把读数贴进报告"的场景——**统计量随数字一起走**。
  [[nodiscard]] auto describe() const -> std::string;
};

}  // namespace st::perf

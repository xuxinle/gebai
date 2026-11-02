/// 时间直方图的实现（定桶对数直方图，见头文件的口径说明）。
///
/// 为何把 `log` 查表而不是每次 `std::log2`：`add()` 在帧路径上被调用几千次/帧
///（每个元素、每个绘制原语），而 `std::log2` 是 ~20 ns 的函数调用；桶边界只有
/// 232 个，表在首次使用时构建一次即可（构建代价 ~232 次 `pow`，一次性）。

#include "st/perf/histogram.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

namespace st::perf {
namespace {

/// 桶 `k` 的**上界**（纳秒）：`2^(k/8)`，保证 `bins[k-1] < ns <= bins[k]` 落进 k。
///
/// 表在首次使用时构建（`inline` 的函数内静态 ⇒ C++11 起线程安全一次性初始化）。
/// 它**只含不可变数据**，不含业务状态。
///
/// lint-allow: L8 桶边界表是**不可变的查表常量**（构建一次、只读），
/// 与"随手写的可变全局状态"性质不同；它由 `Histogram::add` 的常数时间要求决定
/// （每帧上万次调用，逐次算 `pow` 会自己变成热点）。
struct BinTable {
  std::array<std::int64_t, kHistogramBins> upper{};
};

[[nodiscard]] auto bin_table() noexcept -> const BinTable& {
  // lint-allow: L8 见上方结构体说明（不可变查表常量）
  static const BinTable table = [] {
    BinTable built{};
    for (std::size_t index = 0; index < kHistogramBins; ++index) {
      const double exponent =
          static_cast<double>(index + 1) / static_cast<double>(kBinsPerOctave);
      const double value = std::pow(2.0, exponent);
      built.upper[index] = static_cast<std::int64_t>(std::ceil(value));
    }
    // 单调性护栏：浮点舍入不应让上界倒退（倒退会让 add 的二分/线性扫描出错）。
    for (std::size_t index = 1; index < kHistogramBins; ++index) {
      built.upper[index] = std::max(built.upper[index], built.upper[index - 1] + 1);
    }
    return built;
  }();
  return table;
}

/// 找到 `ns` 所属桶（第一个上界 ≥ ns）。越界返回 `kHistogramBins`。
[[nodiscard]] auto bin_of(std::int64_t ns) noexcept -> std::size_t {
  const BinTable& table = bin_table();
  const auto it = std::lower_bound(table.upper.begin(), table.upper.end(), ns);
  if (it == table.upper.end()) return kHistogramBins;
  return static_cast<std::size_t>(it - table.upper.begin());
}

}  // namespace

void Histogram::add(std::int64_t ns) noexcept {
  // 非正样本按 1 ns 记：时钟只保证单调不倒退，同一次调用两次取值的差可能是 0；
  // 把它当"没测到"丢掉会让极快的路径（一次纯色填充）在 n 上被系统性低估。
  const std::int64_t clamped = ns > 0 ? ns : 1;
  if (clamped > kHistogramCeilingNs) {
    ++overflow;
    // 溢出样本仍参与 sum/min/max——它们在"总成本"上是真的，只是分位表装不下。
    sum_ns += clamped;
    if (max_ns < clamped) max_ns = clamped;
    if (min_ns == 0 || clamped < min_ns) min_ns = clamped;
    return;
  }
  const std::size_t index = bin_of(clamped);
  ++bins[index];
  ++count;
  sum_ns += clamped;
  if (max_ns < clamped) max_ns = clamped;
  if (min_ns == 0 || clamped < min_ns) min_ns = clamped;
}

void Histogram::merge(const Histogram& other) noexcept {
  for (std::size_t index = 0; index < kHistogramBins; ++index) {
    bins[index] += other.bins[index];
  }
  count += other.count;
  overflow += other.overflow;
  sum_ns += other.sum_ns;
  if (other.min_ns != 0 && (min_ns == 0 || other.min_ns < min_ns)) min_ns = other.min_ns;
  if (other.max_ns > max_ns) max_ns = other.max_ns;
}

auto Histogram::quantile_ns(double ratio) const noexcept -> std::int64_t {
  if (count == 0) return 0;
  const double clamped = std::clamp(ratio, 0.0, 1.0);
  // 目标秩：`ratio=0.5` 取第 ⌈0.5·n⌉ 个（1 起）。
  const auto target = static_cast<std::uint64_t>(
      std::ceil(clamped * static_cast<double>(count)));
  const std::uint64_t rank = target == 0 ? 1 : target;
  std::uint64_t accumulated = 0;
  const BinTable& table = bin_table();
  for (std::size_t index = 0; index < kHistogramBins; ++index) {
    accumulated += bins[index];
    if (accumulated >= rank) return table.upper[index];
  }
  // 只在"分位落在溢出样本里"时走到这里：那时如实返回上界（不谎报成有限值）。
  return max_ns;
}

auto Histogram::quantile_ms(double ratio) const noexcept -> double {
  return static_cast<double>(quantile_ns(ratio)) / 1'000'000.0;
}

auto Histogram::mean_ns() const noexcept -> double {
  const std::uint64_t total = count + overflow;
  if (total == 0) return 0.0;
  return static_cast<double>(sum_ns) / static_cast<double>(total);
}

auto Histogram::mean_ms() const noexcept -> double { return mean_ns() / 1'000'000.0; }

auto Histogram::min_ms() const noexcept -> double {
  return static_cast<double>(min_ns) / 1'000'000.0;
}

auto Histogram::max_ms() const noexcept -> double {
  return static_cast<double>(max_ns) / 1'000'000.0;
}

auto Histogram::total_ms() const noexcept -> double {
  return static_cast<double>(sum_ns) / 1'000'000.0;
}

auto Histogram::max_relative_error() const noexcept -> double {
  // 桶宽比 = 2^(1/8)。桶上界读数的最大高估 = 该比值 − 1。
  const double ratio = std::pow(2.0, 1.0 / static_cast<double>(kBinsPerOctave));
  return ratio - 1.0;
}

auto Histogram::describe() const -> std::string {
  if (empty()) return "n=0（没有样本）";
  std::string text = std::format("n={} p50={:.2f}ms p95={:.2f}ms max={:.2f}ms err<={:.1f}%",
                                 count, quantile_ms(0.5), quantile_ms(0.95), max_ms(),
                                 max_relative_error() * 100.0);
  if (overflow != 0) {
    text += std::format(" 溢出={}（超出 {:.0f}s 上界）", overflow,
                        static_cast<double>(kHistogramCeilingNs) / 1e9);
  }
  if (!reliable()) {
    text += " ⚠样本不足30，分位不可作为结论";
  }
  return text;
}

}  // namespace st::perf

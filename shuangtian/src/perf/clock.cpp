/// 剖析时钟：开关、刻度标定、低开销读数（见 `include/st/perf/clock.hpp`）。
///
/// 进程级状态只有两样，且都是**不可变快照**（首次调用算出、之后只读）：
/// ① 环境开关的原值；② 刻度标定结果。唯一的可变项是"运行期强制开关"
/// （`set_enabled`），它用 `atomic` 承载——控制通道从服务线程写、帧线程读。
///
/// 平台差异（读 CPU 刻度）全在 `platform_ticks.cpp`，本文件不含任何平台头。

#include "st/perf/clock.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>

#include "st/core/fs.hpp"
#include "st/perf/platform_ticks.hpp"

namespace st::perf {
namespace {

[[nodiscard]] auto read_chrono_ns() noexcept -> std::int64_t {
  return static_cast<std::int64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()
                                                               .time_since_epoch())
          .count());
}

/// 刻度标定结果：`ns = ticks * ns_per_tick`。
struct Calibration {
  bool tsc{false};          ///< 是否用上了高精度刻度
  double ns_per_tick{1.0};
  double uncertainty{1.0};  ///< 相对误差上界（未标定时为 1，即"完全不可信"）
};

/// 标定一次（多次对时取**最小比值**）。
///
/// 为何取最小值：每次对时的实际间隔只会被调度抖动**拉长**，所以"同一段刻度对应
/// 多少个 ns"的最小比值最接近真值（与性能基准取最小值的理由同源）。
[[nodiscard]] auto calibrate() noexcept -> Calibration {
  Calibration result{};
  if (!platform::ticks_available()) {
    // 无高精度刻度：`now_ticks` 就是纳秒，换算恒等，误差上界 1（如实说"没有高精度"）。
    result.tsc = false;
    result.ns_per_tick = 1.0;
    result.uncertainty = 1.0;
    return result;
  }
  constexpr int kRounds = 8;
  constexpr std::int64_t kWindowNs = 2'000'000;  // 每轮对时约 2ms
  double best = 0.0;
  double worst = 0.0;
  bool any = false;
  for (int round = 0; round < kRounds; ++round) {
    const std::int64_t t0 = platform::read_ticks();
    const std::int64_t n0 = read_chrono_ns();
    std::int64_t n1 = n0;
    // 忙等而不是 sleep：sleep 的粒度会让"刻度 → ns"的比值毫无意义。
    while (n1 - n0 < kWindowNs) n1 = read_chrono_ns();
    const std::int64_t t1 = platform::read_ticks();
    const std::int64_t dticks = t1 - t0;
    const std::int64_t dns = n1 - n0;
    if (dticks <= 0 || dns <= 0) continue;
    const double ratio = static_cast<double>(dns) / static_cast<double>(dticks);
    if (!any || ratio < best) best = ratio;
    if (!any || ratio > worst) worst = ratio;
    any = true;
  }
  if (!any || best <= 0.0) {
    result.tsc = false;
    result.ns_per_tick = 1.0;
    result.uncertainty = 1.0;
    return result;
  }
  result.tsc = true;
  result.ns_per_tick = best;
  // 误差上界：最好与最坏之比 − 1（抖动有多大）。给一个 0.1% 下限，避免报告里出现 0。
  result.uncertainty = std::max(0.001, worst / best - 1.0);
  return result;
}

[[nodiscard]] auto calibration() noexcept -> const Calibration& {
  // lint-allow: L8 标定结果是不可变快照（首次调用算出、之后只读）
  static const Calibration snapshot = calibrate();
  return snapshot;
}

/// 环境变量的原值快照（`ST_PERF`）。不可变。
[[nodiscard]] auto env_enabled() noexcept -> bool {
  // lint-allow: L8 环境快照（进程生命周期内不变，见头文件说明）
  static const bool snapshot = [] {
    const auto value = fs::read_env("ST_PERF");
    if (!value.has_value() || value->empty()) return false;
    const std::string_view text{*value};
    return !(text == "0" || text == "off" || text == "false" || text == "no");
  }();
  return snapshot;
}

/// 运行期强制开关（控制通道 `perf start/stop` 用）。`-1` = 未强制。
[[nodiscard]] auto forced_flag() noexcept -> std::atomic<int>& {
  // lint-allow: L8 进程级诊断开关（跨线程可见，用 atomic 承载；语义就是"一个开关"）
  static std::atomic<int> flag{-1};
  return flag;
}

}  // namespace

auto enabled() noexcept -> bool {
  const int forced = forced_flag().load(std::memory_order_relaxed);
  if (forced >= 0) return forced != 0;
  return env_enabled();
}

void set_enabled(bool value) noexcept {
  forced_flag().store(value ? 1 : 0, std::memory_order_relaxed);
}

void clear_forced_enabled() noexcept { forced_flag().store(-1, std::memory_order_relaxed); }

auto now_ns() noexcept -> std::int64_t { return read_chrono_ns(); }

auto now_ticks() noexcept -> std::int64_t { return platform::read_ticks(); }

auto ticks_to_ns(std::int64_t delta_ticks) noexcept -> std::int64_t {
  if (delta_ticks <= 0) return 0;
  const Calibration& cal = calibration();
  if (!cal.tsc) return delta_ticks;  // 已是纳秒
  return static_cast<std::int64_t>(static_cast<double>(delta_ticks) * cal.ns_per_tick);
}

auto high_resolution_clock() noexcept -> bool { return calibration().tsc; }

auto clock_uncertainty() noexcept -> double { return calibration().uncertainty; }

}  // namespace st::perf

#pragma once

/// 时间与计时（`std::chrono` 封装）：单调时钟用于测量，系统时钟用于时间戳。

#include <chrono>
#include <cstdint>
#include <string>

namespace st::time {

using SteadyClock = std::chrono::steady_clock;
using SystemClock = std::chrono::system_clock;

/// 单调时钟当前值（纳秒，进程内相对基准）。
[[nodiscard]] auto now_ns() noexcept -> std::int64_t;
[[nodiscard]] inline auto now_ms() noexcept -> std::int64_t { return now_ns() / 1'000'000; }
/// Unix 纪元毫秒（日志/协议时间戳）。
[[nodiscard]] auto unix_ms() noexcept -> std::int64_t;

/// ISO-8601 UTC 时间串（`2026-01-31T04:05:06.789Z`）。
[[nodiscard]] auto iso8601_utc(std::int64_t unix_millis) -> std::string;
[[nodiscard]] auto iso8601_now() -> std::string;

/// 人类可读时长（`12.3ms` / `1.20s` / `1m02s`）。
[[nodiscard]] auto format_duration_ns(std::int64_t nanos) -> std::string;

/// 作用域计时器（析构时回调耗时；用于性能埋点）。
class Stopwatch {
 public:
  Stopwatch() noexcept : start_ns_(now_ns()) {}
  [[nodiscard]] auto elapsed_ns() const noexcept -> std::int64_t { return now_ns() - start_ns_; }
  [[nodiscard]] auto elapsed_ms() const noexcept -> double {
    return static_cast<double>(elapsed_ns()) / 1'000'000.0;
  }
  void reset() noexcept { start_ns_ = now_ns(); }

 private:
  std::int64_t start_ns_{0};
};

}  // namespace st::time

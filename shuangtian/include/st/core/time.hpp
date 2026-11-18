#pragma once

/// 时间与计时（`std::chrono` 封装）：单调时钟用于测量，系统时钟用于时间戳。

#include <chrono>
#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>

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

/// Unix 秒 → 本地时区的字段分解（时区跟随系统）。
/// 平台实现集中在 `src/core/platform_time.cpp`（POSIX `localtime_r`；Windows 的
/// MSVC/MinGW 无 `localtime_r`，用 `localtime_s`）；失败时返回全零字段（不抛异常）。
[[nodiscard]] auto local_fields(std::time_t unix_seconds) -> std::tm;

/// Unix 毫秒 → **指定固定时区偏移**下的 `HH:MM`（与运行环境时区无关）。
///
/// 为何不用 `local_fields`：部署环境可能是任何时区（CI、容器常是 UTC），
/// 而“北京时间”是用户要看的口径——时区必须固定成常量，不能跟随宿主机。
/// `offset_seconds` 为东八区就传 `8 * 3600`。
///
/// 负值（1970 之前 / 时钟回拨）已归一化到当天，不会出现负号或越界时分。
[[nodiscard]] auto hhmm_at_offset(std::int64_t unix_millis, std::int64_t offset_seconds)
    -> std::string;

/// 当前时间按简单占位模板格式化：`YYYY-MM-DD HH:MM:SS` 与 `HH:MM:SS` 两个预设
/// （`kFormatDateTime` / `kFormatTime`），模板里 `Y m d H M S` 逐字替换
/// （其余字符原样）。状态栏/日志前缀够用，不做完整 strftime。
inline constexpr std::string_view kFormatDateTime{"{Y}-{m}-{d} {H}:{M}:{S}"};
inline constexpr std::string_view kFormatTime{"{H}:{M}:{S}"};
[[nodiscard]] auto format_now(std::string_view format) -> std::string;

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

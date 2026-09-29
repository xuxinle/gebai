#include "st/core/time.hpp"

#include <array>

namespace st::time {
namespace {

/// 天数 → 公历年月日（Howard Hinnant 的 civil_from_days，全部整数运算，无平台时区依赖）。
struct CivilDate {
  std::int64_t year{1970};
  unsigned month{1};
  unsigned day{1};
};

[[nodiscard]] constexpr auto civil_from_days(std::int64_t days) noexcept -> CivilDate {
  const std::int64_t shifted = days + 719468;
  const std::int64_t era = (shifted >= 0 ? shifted : shifted - 146096) / 146097;
  const auto doe = static_cast<unsigned>(shifted - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t year = static_cast<std::int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned day = doy - (153 * mp + 2) / 5 + 1;
  const unsigned month = mp < 10 ? mp + 3 : mp - 9;
  return CivilDate{year + (month <= 2 ? 1 : 0), month, day};
}

[[nodiscard]] constexpr auto floor_div(std::int64_t value, std::int64_t divisor) noexcept
    -> std::int64_t {
  const std::int64_t quotient = value / divisor;
  const std::int64_t remainder = value % divisor;
  return (remainder != 0 && ((remainder < 0) != (divisor < 0))) ? quotient - 1 : quotient;
}

}  // namespace

auto now_ns() noexcept -> std::int64_t {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(SteadyClock::now().time_since_epoch())
      .count();
}

auto unix_ms() noexcept -> std::int64_t {
  return std::chrono::duration_cast<std::chrono::milliseconds>(SystemClock::now().time_since_epoch())
      .count();
}

auto iso8601_utc(std::int64_t unix_millis) -> std::string {
  const std::int64_t seconds = floor_div(unix_millis, 1000);
  std::int64_t millis = unix_millis - seconds * 1000;
  if (millis < 0) millis += 1000;
  const std::int64_t days = floor_div(seconds, 86400);
  const std::int64_t second_of_day = seconds - days * 86400;
  const CivilDate date = civil_from_days(days);
  const auto hour = static_cast<unsigned>(second_of_day / 3600);
  const auto minute = static_cast<unsigned>((second_of_day % 3600) / 60);
  const auto second = static_cast<unsigned>(second_of_day % 60);
  return std::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}.{:03d}Z", date.year, date.month,
                     date.day, hour, minute, second, static_cast<unsigned>(millis));
}

auto iso8601_now() -> std::string { return iso8601_utc(unix_ms()); }

auto format_duration_ns(std::int64_t nanos) -> std::string {
  const double value = static_cast<double>(nanos);
  if (nanos < 1'000) return std::format("{}ns", nanos);
  if (nanos < 1'000'000) return std::format("{:.2f}us", value / 1'000.0);
  if (nanos < 1'000'000'000) return std::format("{:.2f}ms", value / 1'000'000.0);
  if (nanos < 60'000'000'000) return std::format("{:.2f}s", value / 1'000'000'000.0);
  const auto total_seconds = static_cast<std::int64_t>(value / 1'000'000'000.0);
  return std::format("{}m{:02d}s", total_seconds / 60, total_seconds % 60);
}

}  // namespace st::time

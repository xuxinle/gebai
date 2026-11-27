#pragma once

/// 日历与时钟的**纯算法**：无 UI、无时区查询、无系统调用。
///
/// ## 为什么单独一个头（不塞进 `st/core/time.hpp`）
///
/// `time.hpp` 被 **29 个 TU** 直接包含（见它文件头的编译成本说明）。而日历网格这套
/// 算法只有日期/时间选择器与少数业务页需要——放进去就是给 29 个 TU 各加一份。
///
/// ## 为什么全是 `constexpr` 自由函数
///
/// 月界、闰年、周首、网格排布这些东西**错了很难从界面看出来**（少一天、错一格、
/// 闰年 2 月只有 28 天都可能被当成"就是这个月"）。做成纯函数就能穷举断言：
/// `tests/date_time_test.cpp` 直接对着真实历法数据打表核对。
///
/// 约定：月份/日 **从 1 开始**（与用户心智一致，不做 C 语言那套 0 基）；
/// 星期 **0 = 周一**（与网格列对齐，不是 `std::tm` 的 0 = 周日）。
///
/// `std::tm` 那套（0 基月、0 = 周日）只在**边界**出现：与 `st::time::local_fields`
/// 互转时就地换一次，不让它渗进来。

#include <cstdint>
#include <string>
#include <string_view>

namespace st::ui::calendar {

/// 年/月/日（**均从 1 起**）。
struct Date {
  int year{1970};
  int month{1};
  int day{1};

  [[nodiscard]] friend constexpr auto operator==(const Date&, const Date&) -> bool = default;
  /// 字典序即时序（用于"早于 min / 晚于 max"这类比较）。
  [[nodiscard]] friend constexpr auto operator<=>(const Date&, const Date&) = default;
};

/// 时/分（24 小时制，分钟按 `minute` 原值存，不吸附到步进）。
struct Clock {
  int hour{0};
  int minute{0};

  [[nodiscard]] friend constexpr auto operator==(const Clock&, const Clock&) -> bool = default;
};

/// 闰年：能被 4 整除，但世纪年须能被 400 整除（1900 不是闰年、2000 是）。
[[nodiscard]] constexpr auto is_leap_year(int year) noexcept -> bool {
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

/// 该年该月的天数（1~12 月；非法月份返回 0——调用方应保证合法，这里不抛）。
[[nodiscard]] constexpr auto days_in_month(int year, int month) noexcept -> int {
  if (month < 1 || month > 12) return 0;
  constexpr int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2 && is_leap_year(year)) return 29;
  return kDays[month - 1];
}

/// 星期几（**0 = 周一** …… 6 = 周日）。
///
/// 算法用 Sakamoto 公式（对公历成立）；先按 `std::tm` 的 0=周日算再平移，
/// 免得自己再推一遍世纪年偏移。
[[nodiscard]] constexpr auto weekday_of(Date date) noexcept -> int {
  // 以 1970-01-01 为基准（那天是周四 = 周序号 3，0=周一口径）。
  // 不引 `<chrono>`：这里只算日期差，逐月累加足够且可 constexpr。
  std::int64_t days = 0;
  const auto year = static_cast<std::int64_t>(date.year);
  const auto month = static_cast<std::int64_t>(date.month);
  const auto day = static_cast<std::int64_t>(date.day);
  // 整年：每年 365 + 闰日
  const auto leap_count_upto = [](std::int64_t y) constexpr {
    // 闰年个数（小于 y 的年份里），y 可为负但本项目不用负年，按正年处理。
    return (y - 1) / 4 - (y - 1) / 100 + (y - 1) / 400;
  };
  days += (year - 1970) * 365 + (leap_count_upto(year) - leap_count_upto(1970));
  constexpr int kCumulative[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  days += kCumulative[month - 1];
  if (month > 2 && is_leap_year(static_cast<int>(year))) ++days;
  days += day - 1;
  // 1970-01-01 = 周四 → 0=周一口径下是 3
  std::int64_t index = (days + 3) % 7;
  if (index < 0) index += 7;
  return static_cast<int>(index);
}

/// 把日期夹到合法范围（日超出当月天数时收到月末；月份/年份不夹）。
///
/// 用途：从 `31 日` 切到 `2 月` 时，选中日必须变成 28/29 而不是留一个不存在的日期。
[[nodiscard]] constexpr auto clamp_day(Date date) noexcept -> Date {
  const int limit = days_in_month(date.year, date.month);
  if (limit > 0 && date.day > limit) date.day = limit;
  if (date.day < 1) date.day = 1;
  return date;
}

/// 6 行 × 7 列 = 42（`fill_month_grid` 的缓冲长度）。
inline constexpr int kGridRows = 6;
inline constexpr int kGridColumns = 7;
inline constexpr int kGridCells = kGridRows * kGridColumns;

/// 月视图网格：**6 行 × 7 列**（固定 42 格，避免切换月份时面板高度跳动）。
///
/// 每格是该日**在本月的日号**；0 表示“不属于本月”（上月末尾/下月开头，画成弱化空位）。
///
/// 为什么不返回“上月/下月的真实日期”：网格是给人点当月的，邻月格子要么不可点、
/// 要么点了会意外跳到别的月。留 0 让调用方明确地画成空位。
///
/// 为何由调用方传缓冲（而不是返回内部静态数组）：静态数组在多线程/多实例下会
/// 互相覆盖，且渲染线程与后台线程同时算日历时出现“画着画着变了”——而本组件
/// 同时有这两条使用场景。42 字节的栈缓冲，没有分配代价。
/// 返回实际写入的格数（恒为 `kGridCells`，供链式使用）。
auto fill_month_grid(int year, int month, std::int8_t* out) -> int;

/// 加/减若干月（跨年正确）。`day` 会按目标月夹取（1/31 加一月 → 2/28 或 2/29）。
[[nodiscard]] constexpr auto add_months(Date date, int delta) noexcept -> Date {
  int total = (date.year * 12 + (date.month - 1)) + delta;
  // 用 floor 除法处理负 delta（C++ 整除对负数是向零截断）
  int year = total / 12;
  int month = total % 12;
  if (month < 0) {
    month += 12;
    --year;
  }
  date.year = year;
  date.month = month + 1;
  return clamp_day(date);
}

/// `YYYY-MM-DD`（定宽，前导零）。
[[nodiscard]] auto format_date(Date date) -> std::string;

/// 解析 `YYYY-MM-DD`（也接受 `YYYY/MM/DD`）。非法/越界返回 `std::nullopt` 语义的空值。
struct DateParse {
  Date value{};
  bool ok{false};
};
[[nodiscard]] auto parse_date(std::string_view text) -> DateParse;

/// `HH:MM`（定宽）。
[[nodiscard]] auto format_clock(Clock clock) -> std::string;

/// 解析 `HH:MM`（也接受 `HH:MM:SS`，秒被丢弃——本组件是分钟级）。
struct ClockParse {
  Clock value{};
  bool ok{false};
};
[[nodiscard]] auto parse_clock(std::string_view text) -> ClockParse;

/// 把分钟吸附到步进的整数倍（向下取整；用于"5 分钟一档"的选择列）。
[[nodiscard]] constexpr auto snap_minute(int minute, int step) noexcept -> int {
  if (step <= 1) return minute < 0 ? 0 : (minute > 59 ? 59 : minute);
  int snapped = (minute / step) * step;
  if (snapped < 0) snapped = 0;
  if (snapped > 59) snapped = 59;
  return snapped;
}

}  // namespace st::ui::calendar

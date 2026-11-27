/// 日历与时钟纯算法（`st/ui/calendar.hpp`）。
///
/// ## 为什么这批断言值得写
///
/// 这些函数错了**很难从界面看出来**：少一天、错一格、闰年 2 月只 28 天，
/// 都可能被当成"这个月就是这样"。所以对着**真实历法数据打表核对**，
/// 而不是"看着对"。
///
/// 三条最容易错的：
/// - 世纪闰年（1900 不闰、2000 闰）—— 只写 `year % 4 == 0` 必错；
/// - 周首（本库约定 **0 = 周一**，不是 `std::tm` 的 0 = 周日）；
/// - `add_months` 跨年与负 delta（C++ 整除对负数是向零截断，直接 `%12` 会错）。
#include "st/test/test.hpp"

#include <cstdint>
#include <string>

#include "st/ui/calendar.hpp"

namespace {

using st::ui::calendar::add_months;
using st::ui::calendar::clamp_day;
using st::ui::calendar::Clock;
using st::ui::calendar::Date;
using st::ui::calendar::days_in_month;
using st::ui::calendar::format_clock;
using st::ui::calendar::format_date;
using st::ui::calendar::is_leap_year;
using st::ui::calendar::parse_clock;
using st::ui::calendar::parse_date;
using st::ui::calendar::snap_minute;
using st::ui::calendar::weekday_of;

}  // namespace

ST_TEST(calendar_leap_year_follows_gregorian_rules) {
  // 非世纪闰年
  ST_CHECK(is_leap_year(2024));
  ST_CHECK(is_leap_year(2028));
  ST_CHECK(!is_leap_year(2023));
  // **世纪年**：1900 不是闰年、2000 是——只写 `%4` 会在这两个上翻车
  ST_CHECK(!is_leap_year(1900));
  ST_CHECK(is_leap_year(2000));
  ST_CHECK(!is_leap_year(2100));
  // 2 月天数要跟着走
  ST_CHECK_EQ(days_in_month(2024, 2), 29);
  ST_CHECK_EQ(days_in_month(2023, 2), 28);
  ST_CHECK_EQ(days_in_month(2000, 2), 29);
  ST_CHECK_EQ(days_in_month(1900, 2), 28);
}

ST_TEST(calendar_days_in_month_covers_all_twelve) {
  constexpr int kExpected[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  for (int month = 1; month <= 12; ++month) {
    ST_CHECK_EQ(days_in_month(2025, month), kExpected[month - 1]);
  }
  // 非法月份如实返回 0（不崩、不绕回）
  ST_CHECK_EQ(days_in_month(2025, 0), 0);
  ST_CHECK_EQ(days_in_month(2025, 13), 0);
}

ST_TEST(calendar_weekday_is_zero_on_monday) {
  // 2026-11-27 是周五（本仓运行日常），0=周一口径下应为 4
  ST_CHECK_EQ(weekday_of(Date{2026, 11, 27}), 4);
  // 2026-11-23 是周一 → 0
  ST_CHECK_EQ(weekday_of(Date{2026, 11, 23}), 0);
  // 2026-11-29 是周日 → 6
  ST_CHECK_EQ(weekday_of(Date{2026, 11, 29}), 6);
  // 1970-01-01 是周四 → 3（纪元基准，公式的地基）
  ST_CHECK_EQ(weekday_of(Date{1970, 1, 1}), 3);
  // 跨世纪锚点：2000-01-01 是周六 → 5
  ST_CHECK_EQ(weekday_of(Date{2000, 1, 1}), 5);
  // 连续 7 天必须遍历 0..6 各一次（防"某天算重/算漏"）
  bool seen[7] = {};
  for (int day = 23; day <= 29; ++day) seen[weekday_of(Date{2026, 11, day})] = true;
  for (bool hit : seen) ST_CHECK(hit);
}

ST_TEST(calendar_clamp_day_keeps_date_valid) {
  // 1/31 切到 2 月 → 2/28（平年）或 2/29（闰年），不能留 2/31
  ST_CHECK(clamp_day(Date{2025, 2, 31}) == (Date{2025, 2, 28}));
  ST_CHECK(clamp_day(Date{2024, 2, 31}) == (Date{2024, 2, 29}));
  // 落在范围内的不动
  ST_CHECK(clamp_day(Date{2025, 2, 15}) == (Date{2025, 2, 15}));
  // 下界也要收
  ST_CHECK(clamp_day(Date{2025, 3, 0}) == (Date{2025, 3, 1}));
}

ST_TEST(calendar_add_months_crosses_years_and_negative_delta) {
  // 正向跨年
  ST_CHECK(add_months(Date{2026, 11, 15}, 2) == (Date{2027, 1, 15}));
  // 负向跨年（C++ 的 `%` 对负数是向零截断，直接取模会得到 -1 月）
  ST_CHECK(add_months(Date{2026, 1, 15}, -2) == (Date{2025, 11, 15}));
  ST_CHECK(add_months(Date{2026, 1, 31}, -1) == (Date{2025, 12, 31}));
  // 月末夹取：1/31 + 1 月
  ST_CHECK(add_months(Date{2026, 1, 31}, 1) == (Date{2026, 2, 28}));
  ST_CHECK(add_months(Date{2024, 1, 31}, 1) == (Date{2024, 2, 29}));
  // 加 12 个月 = 同月次年
  ST_CHECK(add_months(Date{2026, 6, 10}, 12) == (Date{2027, 6, 10}));
}

ST_TEST(calendar_month_grid_aligns_first_day_to_correct_column) {
  // 2026-11：1 日是周日 → 0=周一口径下在第 6 列；前 6 格空（0）
  std::int8_t grid[st::ui::calendar::kGridCells]{};
  st::ui::calendar::fill_month_grid(2026, 11, grid);
  const int lead = weekday_of(Date{2026, 11, 1});
  ST_CHECK_EQ(lead, 6);
  for (int index = 0; index < lead; ++index) ST_CHECK_EQ(static_cast<int>(grid[index]), 0);
  ST_CHECK_EQ(static_cast<int>(grid[lead]), 1);       // 1 号落在正确的列
  ST_CHECK_EQ(static_cast<int>(grid[lead + 1]), 2);
  // 30 天全部在网格里，之后全是 0
  ST_CHECK_EQ(static_cast<int>(grid[lead + 29]), 30);
  for (int index = lead + 30; index < st::ui::calendar::kGridCells; ++index) {
    ST_CHECK_EQ(static_cast<int>(grid[index]), 0);
  }
}

ST_TEST(calendar_month_grid_fits_worst_case_in_forty_two_cells) {
  // 最坏情形：31 天的月份且 1 日是周日（lead=6）→ 占 6+31=37 格，仍 ≤ 42
  // 2027-08：1 日是周日
  std::int8_t grid[st::ui::calendar::kGridCells]{};
  st::ui::calendar::fill_month_grid(2027, 8, grid);
  const int lead = weekday_of(Date{2027, 8, 1});
  ST_CHECK_EQ(lead, 6);
  ST_CHECK_EQ(static_cast<int>(grid[lead + 30]), 31);   // 31 号在网格内
  // 且不越界：最后一个非零格之后全 0
  int last_nonzero = -1;
  for (int index = 0; index < st::ui::calendar::kGridCells; ++index) {
    if (grid[index] != 0) last_nonzero = index;
  }
  ST_CHECK(last_nonzero < st::ui::calendar::kGridCells);
}

ST_TEST(calendar_date_round_trips_through_text) {
  ST_CHECK_EQ(format_date(Date{2026, 1, 5}), std::string("2026-01-05"));
  const auto parsed = parse_date("2026-01-05");
  ST_CHECK(parsed.ok);
  ST_CHECK(parsed.value == (Date{2026, 1, 5}));
  // 斜杠也认（用户手输常见）
  const auto slashed = parse_date("2026/01/05");
  ST_CHECK(slashed.ok);
  ST_CHECK(slashed.value == (Date{2026, 1, 5}));
  // 非法输入如实失败（不是静默给个默认日期）
  ST_CHECK(!parse_date("").ok);
  ST_CHECK(!parse_date("2026-13-01").ok);   // 月份越界
  ST_CHECK(!parse_date("2026-02-30").ok);   // 该月没这一天
  ST_CHECK(!parse_date("abc").ok);
  ST_CHECK(!parse_date("2026-1").ok);       // 缺日
}

ST_TEST(calendar_clock_round_trips_and_snaps) {
  ST_CHECK_EQ(format_clock(Clock{9, 5}), std::string("09:05"));
  const auto parsed = parse_clock("09:05");
  ST_CHECK(parsed.ok);
  ST_CHECK(parsed.value == (Clock{9, 5}));
  // 带秒也认（秒丢弃）
  const auto with_seconds = parse_clock("09:05:33");
  ST_CHECK(with_seconds.ok);
  ST_CHECK(with_seconds.value == (Clock{9, 5}));
  // 越界拒绝
  ST_CHECK(!parse_clock("24:00").ok);
  ST_CHECK(!parse_clock("09:60").ok);
  ST_CHECK(!parse_clock("").ok);

  // 步进吸附：向下取整，不改上界语义
  ST_CHECK_EQ(snap_minute(0, 5), 0);
  ST_CHECK_EQ(snap_minute(7, 5), 5);
  ST_CHECK_EQ(snap_minute(9, 5), 5);
  ST_CHECK_EQ(snap_minute(10, 5), 10);
  ST_CHECK_EQ(snap_minute(59, 5), 55);
  ST_CHECK_EQ(snap_minute(59, 1), 59);
}

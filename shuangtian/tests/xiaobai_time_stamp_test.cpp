/// 消息时间戳固定为**北京时间（UTC+8）**。
///
/// ## 缺陷现场
///
/// 旧实现是：
/// ```cpp
/// const std::int64_t seconds = st::time::now_ms() / 1000;
/// return std::format("{:02}:{:02}", (seconds / 3600) % 24, (seconds / 60) % 60);
/// ```
/// `now_ms()` 是**单调时钟**（进程内相对基准，从 0 附近起算），
/// 拿它除 3600 得到的“小时”与真实时刻无关——消息上那个时间恒显示
/// 一个固定的假值（实测 `04:41`，其实是进程启动至今的小时数）。
///
/// ## 本文件钉什么
///
/// 时间戳口径 = `unix_ms`（真实时刻）+ **固定 +8 小时**（与宿主机时区无关），
/// 由 `st::time::hhmm_at_offset` 实现——**应用与这里共用同一个入口**，
/// 不复制算式（复制出来的断言会让“实现改了、测试没改”无声通过）。
#include <cstdint>
#include <string>

#include "st/core/time.hpp"
#include "st/test/test.hpp"

namespace {
constexpr std::int64_t kBeijingOffsetSeconds = 8 * 3600;

/// 2026-11-18 00:00:00 UTC。
constexpr std::int64_t kUtcMidnight = 1794960000LL;
}  // namespace

ST_TEST(beijing_stamp_adds_eight_hours) {
  // UTC 零点 ⇒ 北京 08:00。
  ST_CHECK_EQ(st::time::hhmm_at_offset(kUtcMidnight * 1000, kBeijingOffsetSeconds),
              std::string("08:00"));
  // UTC 12:34 ⇒ 北京 20:34。
  ST_CHECK_EQ(st::time::hhmm_at_offset((kUtcMidnight + 12 * 3600 + 34 * 60) * 1000,
                                       kBeijingOffsetSeconds),
              std::string("20:34"));
}

ST_TEST(beijing_stamp_wraps_across_midnight) {
  // UTC 20:30 ⇒ 北京时间**次日** 04:30（不回绕成 20:30）。
  ST_CHECK_EQ(st::time::hhmm_at_offset((kUtcMidnight + 20 * 3600 + 30 * 60) * 1000,
                                       kBeijingOffsetSeconds),
              std::string("04:30"));
}

ST_TEST(beijing_stamp_is_never_negative) {
  // 纪元起点 ⇒ 北京 08:00；再往前（UTC 1969-12-31 23:59）也不许出现负号。
  ST_CHECK_EQ(st::time::hhmm_at_offset(0, kBeijingOffsetSeconds), std::string("08:00"));
  const std::string before_epoch = st::time::hhmm_at_offset(-60LL * 1000, kBeijingOffsetSeconds);
  ST_CHECK_EQ(before_epoch, std::string("07:59"));
  ST_CHECK(before_epoch.find('-') == std::string::npos);
}

ST_TEST(beijing_stamp_is_stable_within_a_minute) {
  // 同一分钟内的任意秒给出同样的时分（不会因精度闪烁），跨分钟才变。
  ST_CHECK_EQ(st::time::hhmm_at_offset(kUtcMidnight * 1000 + 59 * 1000, kBeijingOffsetSeconds),
              std::string("08:00"));
  ST_CHECK_EQ(st::time::hhmm_at_offset(kUtcMidnight * 1000 + 60 * 1000, kBeijingOffsetSeconds),
              std::string("08:01"));
}

ST_TEST(beijing_stamp_ignores_host_timezone) {
  // **关键语义**：结果只取决于 `offset_seconds` 参数，与宿主机时区无关——
  // 同一时刻传 0 得到 UTC 的时分，传 +8 得到北京的时分。
  const std::int64_t moment = (kUtcMidnight + 3 * 3600) * 1000;   // UTC 03:00
  ST_CHECK_EQ(st::time::hhmm_at_offset(moment, 0), std::string("03:00"));
  ST_CHECK_EQ(st::time::hhmm_at_offset(moment, kBeijingOffsetSeconds), std::string("11:00"));
}

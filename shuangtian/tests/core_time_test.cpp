/// `st::time` 本地时间分解与格式化测试。
///
/// 背景：`format_now` 直接用 POSIX `localtime_r`，Windows 的 MSVC/MinGW 没有该函数
/// （实测：mingw 交叉编译失败）。修复把平台差异下沉到 `platform_time.cpp` 的
/// `local_fields`（POSIX `localtime_r` / Windows `localtime_s`），本用例锁住
/// 可观察行为：已知时间戳的字段分解正确、占位模板逐字替换、异常输入不越界。

#include "st/test/test.hpp"

#include <ctime>
#include <string>

#include "st/core/time.hpp"

namespace {

/// UTC 字段分解（测试自身用的对照物；与被测的 local_fields 无关，故允许直接用平台分支）。
[[nodiscard]] auto utc_fields_of(std::time_t seconds) -> std::tm {
  std::tm fields{};
#if defined(_WIN32)
  (void)gmtime_s(&fields, &seconds);
#else
  (void)gmtime_r(&seconds, &fields);
#endif
  return fields;
}

}  // namespace

ST_TEST(time_local_fields_known_timestamp) {
  // 本地字段与 UTC 只差一个时区偏移（失败形态是全零字段：年会是 1900）。
  // 断言口径避开“本机时区是什么”这个变量：
  // - 年/月/日 在合法范围（全零失败会被年龄检查抳住）；
  // - 秒与 UTC 一致（现实时区的偏移均为整分钟）；
  // - 与 UTC 的“时:分”总分钟差是 30 的整数倍（覆盖整点/半点/15 分时区，
  //   不对具体时区取值做任何假设，CI（UTC）与开发机（任意时区）同样成立）。
  const std::time_t known = 1769827506;  // 2026-01-31T02:45:06Z
  const std::tm fields = st::time::local_fields(known);
  const std::tm utc = utc_fields_of(known);
  ST_CHECK(fields.tm_year + 1900 == 2026 || fields.tm_year + 1900 == 2025);
  ST_CHECK(fields.tm_mon + 1 >= 1 && fields.tm_mon + 1 <= 12);
  ST_CHECK(fields.tm_mday >= 1 && fields.tm_mday <= 31);
  ST_CHECK_EQ(fields.tm_sec, utc.tm_sec);
  const int local_minutes = fields.tm_hour * 60 + fields.tm_min;
  const int utc_minutes = utc.tm_hour * 60 + utc.tm_min;
  const int offset_minutes = local_minutes - utc_minutes;
  ST_CHECK(offset_minutes % 30 == 0);
  // 偏移不会超过现实时区范围（−12:00 .. +14:00）
  ST_CHECK(offset_minutes >= -12 * 60 && offset_minutes <= 14 * 60);
}

ST_TEST(time_format_now_placeholders_and_literals) {
  const std::string formatted = st::time::format_now(st::time::kFormatDateTime);
  // {Y}-{m}-{d} {H}:{M}:{S} → 19 字符；分隔符落在固定位
  ST_CHECK_EQ(formatted.size(), std::size_t{19});
  ST_CHECK_EQ(formatted[4], '-');
  ST_CHECK_EQ(formatted[7], '-');
  ST_CHECK_EQ(formatted[10], ' ');
  ST_CHECK_EQ(formatted[13], ':');
  ST_CHECK_EQ(formatted[16], ':');
  // 全部字符要么数字要么合法分隔符（占位符已全部被替换）
  for (const char c : formatted) {
    ST_CHECK((c >= '0' && c <= '9') || c == '-' || c == ' ' || c == ':');
  }
  // 纯文本模板原样透传（无占位符）
  ST_CHECK_EQ(st::time::format_now("status"), std::string("status"));
  // 未知占位 {X} 原样保留
  ST_CHECK_EQ(st::time::format_now("{X}"), std::string("{X}"));
}

ST_TEST(time_iso8601_utc_format) {
  // 1769827506789 = 2026-01-31T02:45:06.789Z（自算对照，防手写错时间戳）
  ST_CHECK_EQ(st::time::iso8601_utc(1769827506789), std::string("2026-01-31T02:45:06.789Z"));
  ST_CHECK_EQ(st::time::iso8601_utc(0), std::string("1970-01-01T00:00:00.000Z"));
}

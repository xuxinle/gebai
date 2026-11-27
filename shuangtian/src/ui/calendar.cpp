#include "st/ui/calendar.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

namespace st::ui::calendar {

namespace {

/// 定宽两位（前导零）。不引 `<format>`：本 TU 在 UI 层，编译成本值得省。
void append_two(std::string& out, int value) {
  out.push_back(static_cast<char>('0' + (value / 10) % 10));
  out.push_back(static_cast<char>('0' + value % 10));
}

/// 从 `text` 的 `pos` 起读 1~2 位十进制（带前导零或单位都可以）。
/// 成功时 `pos` 越过读到的数字，返回数值；失败返回空。
auto read_number(std::string_view text, std::size_t& pos, int min_digits, int max_digits)
    -> std::optional<int> {
  int value = 0;
  int digits = 0;
  while (pos < text.size() && digits < max_digits && text[pos] >= '0' && text[pos] <= '9') {
    value = value * 10 + (text[pos] - '0');
    ++pos;
    ++digits;
  }
  if (digits < min_digits) return std::nullopt;
  return value;
}

/// 跳过分隔符（`-` 或 `/`）。
auto skip_separator(std::string_view text, std::size_t& pos) -> bool {
  if (pos >= text.size()) return false;
  if (text[pos] != '-' && text[pos] != '/') return false;
  ++pos;
  return true;
}

}  // namespace

auto fill_month_grid(int year, int month, std::int8_t* out) -> int {
  if (out == nullptr) return 0;
  for (int index = 0; index < kGridCells; ++index) out[index] = 0;
  const int total_days = days_in_month(year, month);
  if (total_days <= 0) return kGridCells;
  // 1 号落在第几列（0 = 周一 …… 6 = 周日）
  const int lead = weekday_of(Date{year, month, 1});
  for (int day = 1; day <= total_days; ++day) {
    const int cell = lead + day - 1;
    if (cell < 0 || cell >= kGridCells) break;   // 理论上不会（最坏 6+31=37）
    out[cell] = static_cast<std::int8_t>(day);
  }
  return kGridCells;
}

auto format_date(Date date) -> std::string {
  std::string out;
  out.reserve(10);
  // 年份按 4 位定宽（不足补零：公元 999 年写成 0999，与本库其它时间串一致）
  const int year = date.year;
  out.push_back(static_cast<char>('0' + (year / 1000) % 10));
  out.push_back(static_cast<char>('0' + (year / 100) % 10));
  out.push_back(static_cast<char>('0' + (year / 10) % 10));
  out.push_back(static_cast<char>('0' + year % 10));
  out.push_back('-');
  append_two(out, date.month);
  out.push_back('-');
  append_two(out, date.day);
  return out;
}

auto parse_date(std::string_view text) -> DateParse {
  DateParse result;
  // 容忍前后空白（从剪贴板/输入框粘贴常见）
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  if (text.empty()) return result;

  std::size_t pos = 0;
  // 年：**至少 4 位**（避免 "26-1-5" 被理解成公元 26 年）
  const auto year = read_number(text, pos, 4, 4);
  if (!year) return result;
  if (!skip_separator(text, pos)) return result;
  const auto month = read_number(text, pos, 1, 2);
  if (!month) return result;
  if (!skip_separator(text, pos)) return result;
  const auto day = read_number(text, pos, 1, 2);
  if (!day) return result;
  if (pos != text.size()) return result;   // 尾巴还有东西 → 不认识

  if (*month < 1 || *month > 12) return result;
  const int limit = days_in_month(*year, *month);
  if (*day < 1 || *day > limit) return result;

  result.value = Date{*year, *month, *day};
  result.ok = true;
  return result;
}

auto format_clock(Clock clock) -> std::string {
  std::string out;
  out.reserve(5);
  append_two(out, clock.hour);
  out.push_back(':');
  append_two(out, clock.minute);
  return out;
}

auto parse_clock(std::string_view text) -> ClockParse {
  ClockParse result;
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  if (text.empty()) return result;

  std::size_t pos = 0;
  const auto hour = read_number(text, pos, 1, 2);
  if (!hour) return result;
  if (pos >= text.size() || text[pos] != ':') return result;
  ++pos;
  const auto minute = read_number(text, pos, 1, 2);
  if (!minute) return result;
  // 允许 `HH:MM:SS`（秒直接丢弃——本组件是分钟级）
  if (pos < text.size()) {
    if (text[pos] != ':') return result;
    ++pos;
    const auto second = read_number(text, pos, 1, 2);
    if (!second) return result;
    if (pos != text.size()) return result;
  }
  if (*hour < 0 || *hour > 23) return result;
  if (*minute < 0 || *minute > 59) return result;

  result.value = Clock{*hour, *minute};
  result.ok = true;
  return result;
}

}  // namespace st::ui::calendar

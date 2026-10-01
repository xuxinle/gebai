// 平台边界：Unix 秒 → 本地时区字段分解（CONVENTIONS §10.1 平台差异只进平台层）。
// POSIX 提供 `localtime_r`（可重入）；Windows（MSVC 与 MinGW）没有该函数，
// 对应物是参数顺序相反的 `localtime_s`。两侧失败都退化为全零字段（不抛异常——
// 调用方是状态栏/日志前缀这类纯展示路径，时间分解失败不值得中断进程）。
#include "st/core/time.hpp"

#include <ctime>

namespace st::time {

auto local_fields(std::time_t unix_seconds) -> std::tm {
  std::tm fields{};
#if defined(_WIN32)
  (void)localtime_s(&fields, &unix_seconds);
#else
  (void)localtime_r(&unix_seconds, &fields);
#endif
  return fields;
}

}  // namespace st::time

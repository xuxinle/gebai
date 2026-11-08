/// 路径长名展开的平台实现（单点封装：系统头只在本文出现）。
///
/// ## 为什么需要这一层
///
/// `std::filesystem::weakly_canonical` 在 **Windows 上不展开 8.3 短名**：
/// 实测 `C:/Users/ADMINI~1/AppData/Local/Temp` 原样返回，而
/// `GetLongPathNameW` 给的是 `C:/Users/Administrator/AppData/Local/Temp`。
///
/// 两者是**同一个目录**，但任何字符串比较都看不出来。代价（2026-11-08 实测，
/// 册府图谱 e2e）：控制通道 `capture` 的落盘白名单存的是 `fs::temp_dir()` 给到的
/// 短名，而客户端传长名 → 合法路径全被拒，报错只说"不在白名单内"，
/// 看上去像安全策略太紧，实际是路径身份没归一。
///
/// ## 接口约定
///
/// 返回 `nullopt` = 无法展开（路径不存在 / 非 Windows / 系统调用失败）。
/// **不报错**：调用方的语义是"能拿到长名就用它"，拿不到退回原串即可——
/// 为一个可选的归一化步骤制造一个新的失败点，只会让调用方多一条无从处理的错误分支。
#include "st/core/fs.hpp"

#include <filesystem>
#include <optional>
#include <string>

#if defined(_WIN32)
// `windows.h` 默认把 `min`/`max` 定义成宏，会**静默破坏**所有 `std::min`/`std::max`
// 调用点——必须在包含前压住（本仓其它 `platform_*` 同此口径）。
#define NOMINMAX 1
#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>
#endif

namespace st::fs {

#if defined(_WIN32)

auto platform_long_path_name(const std::string& path) -> std::optional<std::string> {
  // UTF-8 → 宽字符：Windows 的路径 API 全是宽字符接口。
  //
  // 不用 `std::filesystem::u8path`：它在 C++20 起已 deprecated（`-Werror` 直接报），
  // 而 `path(std::u8string)` 才是现行接口。
  if (path.empty()) return std::nullopt;
  const std::u8string utf8(path.begin(), path.end());
  const std::wstring widened = std::filesystem::path(utf8).wstring();
  // 先问长度（第二个参数为 0 时不写入，只返回所需长度）。
  const DWORD needed = ::GetLongPathNameW(widened.c_str(), nullptr, 0);
  if (needed == 0) return std::nullopt;
  std::wstring buffer(needed, L'\0');
  const DWORD written = ::GetLongPathNameW(widened.c_str(), buffer.data(), needed);
  if (written == 0 || written >= needed) return std::nullopt;
  buffer.resize(written);
  const std::u8string result = std::filesystem::path(buffer).u8string();
  return std::string(result.begin(), result.end());
}

#else

auto platform_long_path_name(const std::string& path) -> std::optional<std::string> {
  // POSIX 没有短名别名这回事：长名就是路径本身，交回 `nullopt` 让调用方用原串。
  (void)path;
  return std::nullopt;
}

#endif

}  // namespace st::fs

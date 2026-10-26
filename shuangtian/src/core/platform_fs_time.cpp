/// 文件时间戳的平台实现（单点封装：系统头只在本文出现）。
///
/// ## 为什么要专门做这一层
///
/// `std::filesystem::last_write_time` 在 MinGW 下把 Win32 的 100ns 时间戳
/// **截断到整秒**——实测同一秒内先后写两个文件，两边返回值 `% 1e9` 都是 0，
/// 连"谁先写的"都分不出来。
///
/// 代价不在这一个函数上：**所有按 mtime 做的增量判断粒度都退到秒**。
/// 同一秒内改源码再构建会被判成"没变"，于是拿旧产物当新结果——
/// 这正是本仓库反复踩的"改了没生效"的一支。
///
/// 所以 Windows 上直接走 `GetFileTime`（原生 100ns），POSIX 上标准库本就是
/// 纳秒（`st_mtim`），无需绕路。
///
/// ## 接口约定
///
/// 返回**自 Unix 纪元起的纳秒数**。两条平台分支必须同一时基——否则两种来源的
/// 时间戳不能互相比较（Windows 的 FILETIME 自 1601 起，直接返回会差 369 年）。
#include "st/core/fs_time.hpp"

#if defined(_WIN32)
// `windows.h` 默认把 `min`/`max` 定义成宏，会**静默破坏**所有
// `std::min`/`std::max`/`std::numeric_limits<T>::max()` 调用点——
// 必须在包含前压住（本仓其它 `platform_*` 同此口径）。
#define NOMINMAX 1
#define WIN32_LEAN_AND_MEAN 1
#include <chrono>
#include <windows.h>
#endif

#include <cstdint>
#include <string>

#include "st/core/error.hpp"

namespace st::fs {

#if defined(_WIN32)

auto platform_modified_ns(const std::string& path) -> Result<std::int64_t> {
  // UTF-8 → UTF-16（Windows 的真 Unicode 接口；框架内部一律 UTF-8）
  const int needed = ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(),
                                           static_cast<int>(path.size()), nullptr, 0);
  if (needed <= 0) {
    return unexpected(ErrorCode::Invalid, "路径不是合法 UTF-8");
  }
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  (void)::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()),
                              wide.data(), needed);

  // 只要读属性即可；`FILE_FLAG_BACKUP_SEMANTICS` 让目录也能取时间。
  HANDLE handle = ::CreateFileW(wide.c_str(), FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return unexpected(ErrorCode::NotFound, "无法打开文件取时间戳: " + path);
  }
  FILETIME write_time{};
  const BOOL ok = ::GetFileTime(handle, nullptr, nullptr, &write_time);
  ::CloseHandle(handle);
  if (ok == FALSE) {
    return unexpected(ErrorCode::NotFound, "无法读取文件时间戳: " + path);
  }

  // FILETIME = 自 1601-01-01 起的 100ns 数（高低两段），换算到 Unix 纪元。
  const std::uint64_t ticks =
      (static_cast<std::uint64_t>(write_time.dwHighDateTime) << 32) | write_time.dwLowDateTime;
  constexpr std::uint64_t kUnixEpochTicks = 116444736000000000ULL;   // 1601→1970 的 100ns 数
  const std::int64_t since_unix_ticks = static_cast<std::int64_t>(ticks - kUnixEpochTicks);
  return since_unix_ticks * 100;   // 100ns → ns
}

#else

auto platform_modified_ns(const std::string& path) -> Result<std::int64_t> {
  // POSIX：标准库的实现基于 `st_mtim`，本就是纳秒，不需要绕路。
  std::error_code ec;
  const auto stamp = std::filesystem::last_write_time(std::filesystem::path(path), ec);
  if (ec) return unexpected(ErrorCode::NotFound, "无法读取文件时间戳: " + path);
  return std::chrono::duration_cast<std::chrono::nanoseconds>(stamp.time_since_epoch()).count();
}

#endif

}  // namespace st::fs

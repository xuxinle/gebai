/// 只读内存映射的平台实现（单点封装：系统头只在本文件出现，见 `fs_map.hpp` 的说明）。
///
/// 为什么需要映射（2026-10-14 内存评估 P0）：字体链 15 个 face 的原始文件字节
/// 实测 ~86MB **常驻且永不释放**——`read_bytes` 拿到的是私有堆副本，只能靠换页；
/// 映射拿到的是文件页的**共享副本**，内存压力下系统可直接回收。

#include "st/core/fs_map.hpp"

#if defined(_WIN32)
// `windows.h` 默认把 `min`/`max` 定义成宏，会**静默破坏**所有 `std::min`/`std::max`/
// `std::numeric_limits<T>::max()` 调用点——必须在包含前压住（口径同本仓其它 platform_*）。
#define NOMINMAX 1
#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <string>

#include "st/core/error.hpp"

namespace st::fs {

#if defined(_WIN32)

/// UTF-8 → UTF-16（Windows 的真 Unicode 接口；框架内部一律 UTF-8）。
///
/// 为何不调 `fs.cpp` 的 `to_path`：它在匿名命名空间里（本文件不该为了一个转换
/// 把通用层的内部实现揪出来）。本仓 `platform_*` 层本来就有同名小工具
/// （`platform_process.cpp` / `platform_shell.cpp` / `platform_win32.cpp` 各一份）——
/// 这是**刻意的重复**：它只有 6 行，而“为了去重把平台边界打穿”代价更高。
///
/// 与 `fs::to_path` 同口径：**不得抛异常**。非法 UTF-8 退化为空串 →
/// `CreateFileW` 失败 → 返回 `NotFound`（“找不到那个文件”），而不是进程崩掉。
[[nodiscard]] auto to_wide(std::string_view utf8) -> std::wstring {
  if (utf8.empty()) return {};
  const int needed =
      ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (needed <= 0) return {};
  std::wstring out(static_cast<std::size_t>(needed), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
  return out;
}

auto open_file_mapping(std::string_view path) -> Result<MappedView> {
  const std::wstring wide = to_wide(path);
  // **`FILE_SHARE_DELETE` 必须有**：不加它，任何已映射的字体文件在映射存活期间
  // 无法被删除/改名——用户重装字体、覆盖安装会看到“文件被占用”。
  // 加上它，映射仍能读到旧内容（这是映射的既有语义），但不再锁住路径。
  HANDLE file = ::CreateFileW(wide.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return unexpected(ErrorCode::NotFound, std::string("open failed '").append(path).append("'"));
  }
  LARGE_INTEGER size{};
  if (::GetFileSizeEx(file, &size) == 0 || size.QuadPart <= 0) {
    ::CloseHandle(file);
    return unexpected(ErrorCode::Io, std::string("size failed '").append(path).append("'"));
  }
  HANDLE mapping = ::CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (mapping == nullptr) {
    ::CloseHandle(file);
    return unexpected(ErrorCode::Io, std::string("mapping failed '").append(path).append("'"));
  }
  const void* view = ::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
  if (view == nullptr) {
    ::CloseHandle(mapping);
    ::CloseHandle(file);
    return unexpected(ErrorCode::Io, std::string("map view failed '").append(path).append("'"));
  }
  // 两个句柄分别存：`handle` = 映射对象、`handle_extra` = 文件。
  // 为何不用堆上打包结构：`MappedView` 是**跨平台值类型**，为 Windows 多引一次分配/释放
  // 只会给失败路径添一个新失败点（而 L1 也禁裸 `new`——那条规则在这里是对的）。
  MappedView result;
  result.data = static_cast<const std::uint8_t*>(view);
  result.size = static_cast<std::size_t>(size.QuadPart);
  result.handle = mapping;
  result.handle_extra = file;
  return result;
}

void close_file_mapping(MappedView& view) noexcept {
  if (view.data != nullptr) ::UnmapViewOfFile(view.data);
  if (view.handle != nullptr) ::CloseHandle(static_cast<HANDLE>(view.handle));
  if (view.handle_extra != nullptr) ::CloseHandle(static_cast<HANDLE>(view.handle_extra));
  view.data = nullptr;
  view.size = 0;
  view.handle = nullptr;
  view.handle_extra = nullptr;
}

#else  // POSIX

auto open_file_mapping(std::string_view path) -> Result<MappedView> {
  const std::string name(path);
  const int fd = ::open(name.c_str(), O_RDONLY);
  if (fd < 0) {
    return unexpected(ErrorCode::NotFound, std::string("open failed '").append(path).append("'"));
  }
  struct stat info {};
  if (::fstat(fd, &info) != 0 || info.st_size <= 0) {
    ::close(fd);
    return unexpected(ErrorCode::Io, std::string("size failed '").append(path).append("'"));
  }
  // ⚠ 目录也能 `open` 成功——而 `mmap` 一个目录报的是 `ENODEV`（“设备不支持”），
  // 看着像硬件问题。显式挡掉，给出可行动的理由。
  if (S_ISDIR(info.st_mode)) {
    ::close(fd);
    return unexpected(ErrorCode::Invalid, std::string("是目录，不是文件 '").append(path).append("'"));
  }
  void* view = ::mmap(nullptr, static_cast<std::size_t>(info.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
  // **映射建立后 fd 即可关闭**（POSIX 语义：映射独立于 fd）——早关一个是一个，
  // 也省得调用方为句柄生命周期操心。
  ::close(fd);
  if (view == MAP_FAILED) {
    return unexpected(ErrorCode::Io, std::string("mmap failed '").append(path).append("'"));
  }
  MappedView result;
  result.data = static_cast<const std::uint8_t*>(view);
  result.size = static_cast<std::size_t>(info.st_size);
  result.handle = nullptr;
  return result;
}

void close_file_mapping(MappedView& view) noexcept {
  if (view.data != nullptr) ::munmap(const_cast<std::uint8_t*>(view.data), view.size);
  view.data = nullptr;
  view.size = 0;
  view.handle = nullptr;
  view.handle_extra = nullptr;
}

#endif

}  // namespace st::fs

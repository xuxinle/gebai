#include "st/core/log_file.hpp"

#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/core/time.hpp"

// lint-allow: L8 日志为进程级基础设施，文件 sink 与尾部缓冲按设计为进程级配置（见 CONVENTIONS §3.6 例外登记）
namespace st::log {
namespace {

/// 常驻的环形缓冲：崩溃时从这里取尾部（不去读文件——那时文件可能还没 flush）。
///
/// 已挪到 `st::log::tail`（**不依赖是否开落盘**，见那里的说明）；这里只保留转发。

/// 文件落盘状态。
std::mutex file_mutex{};
struct FileState {
  std::FILE* handle{nullptr};
  std::string path{};
  std::size_t written{0};      ///< 当前文件已写字节数（判滚动）
  std::size_t max_bytes{0};
  std::size_t keep_files{0};
  bool timestamps{true};
  bool reported_failure{false};   ///< 写失败只报一次（磁盘满时别刷爆日志）
};
FileState file{};
std::uint64_t listener_id{0};

/// 把一条日志推进环形缓冲（超容量丢最老的）。
///
/// 已由 `st::log::write` 直接完成（见 `log.cpp`）——缓冲属于日志系统本身，
/// 不应依赖文件 sink 是否开启。此处不保留副本。

/// 滚动：`app.log` → `app.1.log` → `app.2.log`，最老的丢弃。
///
/// 为什么不按时间命名：日志是"出事后才看"的东西，而按序号滚动意味着
/// **最新的一档路径恒定**（`app.log`）——脚本与智能体不必去猜文件名。
void rotate() {
  if (file.handle != nullptr) {
    std::fclose(file.handle);
    file.handle = nullptr;
  }
  if (file.keep_files == 0) {
    (void)fs::remove_file(file.path);
    return;
  }
  // 从最老一档往新一档挪：`app.<n>.log` 先删，然后逐级后移。
  const auto suffix = [](const std::string& base, std::size_t index) -> std::string {
    if (index == 0) return base;
    // `dir/app.log` → `dir/app.1.log`（放在扩展名之前，便于按扩展名过滤）。
    const std::size_t dot = base.find_last_of('.');
    const std::size_t slash = base.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
      return base + "." + std::to_string(index);
    }
    return base.substr(0, dot) + "." + std::to_string(index) + base.substr(dot);
  };
  (void)fs::remove_file(suffix(file.path, file.keep_files));
  for (std::size_t index = file.keep_files; index > 1; --index) {
    (void)fs::rename(suffix(file.path, index - 1), suffix(file.path, index));
  }
  (void)fs::rename(file.path, suffix(file.path, 1));
}

/// 真正写一条（调用方持锁）。
void write_line(const std::string& line) {
  if (file.handle == nullptr) return;
  const std::size_t length = std::fwrite(line.data(), 1, line.size(), file.handle);
  if (length != line.size()) {
    if (!file.reported_failure) {
      file.reported_failure = true;
      // 用 stderr 直写（**不能再走 log::warn**——那会递归回本函数）。
      std::fprintf(stderr, "[st-log] 日志写盘失败（后续只写 stderr）：%s\n", file.path.c_str());
    }
    std::fclose(file.handle);
    file.handle = nullptr;
    return;
  }
  file.written += length;
  if (file.max_bytes > 0 && file.written >= file.max_bytes) {
    rotate();
    file.handle = std::fopen(file.path.c_str(), "wb");
    file.written = 0;
  }
}

/// 监听器：把每条日志镜像到文件。
///
/// 尾部缓冲不在这里做——由 `st::log::write` 直接推进（与是否开落盘无关）。
void on_log(Level level, std::string_view message) {
  const std::string line =
      file.timestamps ? std::format("[{}] {:<5} {}\n", time::iso8601_now(), to_string(level), message)
                      : std::format("{:<5} {}\n", to_string(level), message);
  const std::scoped_lock lock(file_mutex);
  write_line(line);
}

}  // namespace

auto open_file(const FileOptions& options) -> bool {
  if (options.path.empty()) return false;
  {
    const std::scoped_lock lock(file_mutex);
    if (file.handle != nullptr) {
      std::fclose(file.handle);
      file.handle = nullptr;
    }
    file.path = options.path;
    file.max_bytes = options.max_bytes;
    file.keep_files = options.keep_files;
    file.timestamps = options.timestamps;
    file.written = 0;
    file.reported_failure = false;
    // 父目录不存在时建出来（应用不必自己先 mkdir）。
    if (const std::size_t slash = file.path.find_last_of("/\\"); slash != std::string::npos) {
      (void)fs::create_directories(file.path.substr(0, slash));
    }
    file.handle = std::fopen(file.path.c_str(), "ab");
    if (file.handle == nullptr) {
      std::fprintf(stderr, "[st-log] 日志文件打不开（只写 stderr）：%s\n", file.path.c_str());
      file.path.clear();
      return false;
    }
    // 续写已有文件时把当前大小算上，滚动阈值才准。
    if (const auto size = fs::file_size(file.path); size.has_value()) file.written = *size;
  }
  if (listener_id == 0) listener_id = add_listener(on_log);
  return true;
}

void close_file() {
  {
    const std::scoped_lock lock(file_mutex);
    if (file.handle != nullptr) {
      std::fflush(file.handle);
      std::fclose(file.handle);
      file.handle = nullptr;
    }
    file.path.clear();
  }
  if (listener_id != 0) {
    remove_listener(listener_id);
    listener_id = 0;
  }
}

auto file_active() -> bool {
  const std::scoped_lock lock(file_mutex);
  return file.handle != nullptr;
}

auto file_path() -> std::string {
  const std::scoped_lock lock(file_mutex);
  return file.path;
}

auto recent_tail(std::size_t max_lines) -> std::string {
  return tail::recent(max_lines);
}

void set_tail_capacity(std::size_t lines) { tail::set_capacity(lines); }

namespace detail {

auto try_recent_tail(std::size_t max_lines) -> std::string { return tail::try_recent(max_lines); }

}  // namespace detail

}  // namespace st::log

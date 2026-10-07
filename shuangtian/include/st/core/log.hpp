#pragma once

/// 日志：级别过滤 + 可替换 sink + `std::format` 风格 API。
/// 默认 sink 写 stderr（无文件句柄、无隐式落盘）；控制通道订阅时经 `set_sink` 转发。

#include <cstdint>
#include <format>
#include <functional>
#include <string_view>
#include <utility>

namespace st::log {

enum class Level : std::uint8_t { Trace, Debug, Info, Warn, Error, Off };

[[nodiscard]] constexpr auto to_string(Level level) noexcept -> std::string_view {
  switch (level) {
    case Level::Trace: return "trace";
    case Level::Debug: return "debug";
    case Level::Info: return "info";
    case Level::Warn: return "warn";
    case Level::Error: return "error";
    case Level::Off: return "off";
  }
  return "?";
}

void set_level(Level level) noexcept;
[[nodiscard]] auto level() noexcept -> Level;
[[nodiscard]] auto level_from_name(std::string_view name) noexcept -> Level;

/// 替换输出目标（默认 stderr）；传入空函数恢复默认。
void set_sink(std::function<void(Level, std::string_view)> sink);

/// 底层写入（已格式化文本，含级别过滤与时间戳前缀）。
void write(Level level, std::string_view message);

/// 事件回调式订阅（控制通道 `events` 用）：每个 sink 收到原始消息（不含时间戳）。
/// 返回值是**订阅 id**，用 `remove_listener` 注销。
///
/// 为何要有注销：列表是**进程级全局**的，而订阅者常是**临时对象**（如 `Application`：
/// 测试里逐个构造销毁、示例里一个进程可能有多个）。不注销就是**悬垂监听器**——
/// 对象死了回调还在，之后**任何线程**（包括 `pkg::build` 的编译 worker）打一条日志
/// 就会踩到已释放的 `this`（实测：`st test` 随机分片 SIGSEGV 在
/// `app.cpp` 的 `impl_->log_lines.push_back`）。
[[nodiscard]] auto add_listener(std::function<void(Level, std::string_view)> listener)
    -> std::uint64_t;

/// 注销 `add_listener` 返回的订阅（已注销的 id 再传一次是无害的空操作）。
void remove_listener(std::uint64_t id) noexcept;

template <class... Args>
void trace(std::format_string<Args...> fmt, Args&&... args) {
  if (level() <= Level::Trace) write(Level::Trace, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
  if (level() <= Level::Debug) write(Level::Debug, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
  if (level() <= Level::Info) write(Level::Info, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
  if (level() <= Level::Warn) write(Level::Warn, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
  if (level() <= Level::Error) write(Level::Error, std::format(fmt, std::forward<Args>(args)...));
}

/// 作用域计时（析构时按级别输出耗时）。
class ScopedTimer {
 public:
  ScopedTimer(Level level, std::string label);
  ~ScopedTimer();
  ScopedTimer(const ScopedTimer&) = delete;
  auto operator=(const ScopedTimer&) -> ScopedTimer& = delete;
  ScopedTimer(ScopedTimer&&) = delete;
  auto operator=(ScopedTimer&&) -> ScopedTimer& = delete;

 private:
  Level level_{Level::Debug};
  std::string label_{};
  std::int64_t start_ns_{0};
};

}  // namespace st::log

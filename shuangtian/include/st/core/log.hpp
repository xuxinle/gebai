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

/// **进程内日志尾部**（最近 N 条，固定容量环形缓冲）。
///
/// 为何在 `log` 层而不是 `log_file`：崩溃现场的“死之前发生了什么”**不应依赖于
/// 是否开了文件落盘**——实测踩到：缓冲曾经挂在文件 sink 的监听器上，于是
/// 没开 `--log-file` 时尾部恒为空，崩溃报告里那一栏永远是空的
/// （而那恰恰是最需要看到日志的场景）。由 `log_file_test` 报红才现形。
///
/// 开销：每条日志额外一次 string 拷贝 + 入队；容量有界（默认 512 行），
/// 相对于日志本身的格式化开销可忽略。
namespace tail {

/// 设环形缓冲容量（行）。0 视为 1。
void set_capacity(std::size_t lines);

/// 取最近 `max_lines` 行（0 = 全部）。普通锁——**不要在信号处理器里调**。
[[nodiscard]] auto recent(std::size_t max_lines) -> std::string;

/// **信号安全**版：`try_lock`，拿不到锁立即返回空。崩溃处理器专用。
[[nodiscard]] auto try_recent(std::size_t max_lines) -> std::string;

}  // namespace tail

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

/// **编译期裁剪**：定义 `ST_LOG_DISABLED` 后所有日志入口成 no-op。
///
/// 为何要它：release 产物里日志字符串与 `std::format` 实例化是**实打实的体积**
/// （实测 gbcode：裁前 33 357 312 → 裁后 33 240 064，**减 117 KB**），
/// 而发布版通常靠崩溃报告就够。需要时用构建开关裁掉，**零开销**。
///
/// 用法：`st.pkg` 里目标或工程加 `"log": false`（见 `pkg::manifest`），
/// 或自己 `-DST_LOG_DISABLED=1`。
///
/// ## 为何是宏而不是空函数模板
///
/// 空函数模板里参数**仍会被求值**，于是字符串字面量照样编进产物——
/// 裁掉的是调用开销，不是体积。宏才能把参数一起丢掉。
/// （实测：用空函数时 `"listening control on"` 这类字面量仍在二进制里，
/// 换成宏之后才消失。）
///
/// ⚠ 五个宏**各自写全**，不做“参数名 + 拼接”（`ST_LOG_LEVEL(info, …)` 那种）：
/// 宏参数名会与函数名撞（参数叫 `level` 而体里写 `st::log::level`，
/// 预处理器会把两者一起换掉）——实测报 `expected unqualified-id before '(' token`，
/// 而报错位置在头文件里、离真正原因很远。
#if defined(ST_LOG_DISABLED)

#define ST_LOG_TRACE(...) ((void)0)
#define ST_LOG_DEBUG(...) ((void)0)
#define ST_LOG_INFO(...) ((void)0)
#define ST_LOG_WARN(...) ((void)0)
#define ST_LOG_ERROR(...) ((void)0)

#else

/// 日志入口：**推荐写法**（裁得掉开销）。
/// `ST_LOG_INFO("x={}", x)` 展开成 `st::log::info("x={}", x)`；
/// 开了 `ST_LOG_DISABLED` 时整个（含参数）被丢掉。
///
/// ⚠ 展开目标走**间接层**（`ST_LOG_DETAIL_*`）而不是直接写 `st::log::info`：
/// `st::log::info(__VA_ARGS__)` 里的 `info` 会**再被同名宏替换一次**，
/// 展开成 `st::log::ST_LOG_INFO(__VA_ARGS__)`（而宏名在 `st::log::` 作用域里不存在）——
/// 报 `expected unqualified-id before '(' token`，位置在头文件里、离真正原因很远。
#define ST_LOG_DETAIL_TRACE(...) st::log::trace(__VA_ARGS__)
#define ST_LOG_DETAIL_DEBUG(...) st::log::debug(__VA_ARGS__)
#define ST_LOG_DETAIL_INFO(...) st::log::info(__VA_ARGS__)
#define ST_LOG_DETAIL_WARN(...) st::log::warn(__VA_ARGS__)
#define ST_LOG_DETAIL_ERROR(...) st::log::error(__VA_ARGS__)

#define ST_LOG_TRACE(...) ST_LOG_DETAIL_TRACE(__VA_ARGS__)
#define ST_LOG_DEBUG(...) ST_LOG_DETAIL_DEBUG(__VA_ARGS__)
#define ST_LOG_INFO(...) ST_LOG_DETAIL_INFO(__VA_ARGS__)
#define ST_LOG_WARN(...) ST_LOG_DETAIL_WARN(__VA_ARGS__)
#define ST_LOG_ERROR(...) ST_LOG_DETAIL_ERROR(__VA_ARGS__)

#endif  // ST_LOG_DISABLED

#if !defined(ST_LOG_DISABLED)
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

#endif  // ST_LOG_DISABLED

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

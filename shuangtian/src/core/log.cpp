#include "st/core/log.hpp"

#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "st/core/time.hpp"

// lint-allow: L8 日志为进程级基础设施，级别/sink/listener 按设计为进程级配置（见 CONVENTIONS §3.6 例外登记）
namespace st::log {
namespace {

std::atomic<Level> level_state{Level::Info};
std::mutex sink_mutex{};
std::function<void(Level, std::string_view)> sink_state{};
/// 订阅项 = 回调 + 稳定 id（id 用于注销，见 `add_listener` 的说明）。
struct Listener {
  std::uint64_t id{0};
  std::function<void(Level, std::string_view)> callback{};
};
std::vector<Listener> listeners{};
std::uint64_t next_listener_id{1};

/// 日志尾部环形缓冲（见 `st::log::tail` 的说明）。
std::mutex tail_mutex{};
std::deque<std::string> tail_lines{};
std::size_t tail_capacity{512};

/// 把一行推进环形缓冲（超容量丢最老的）。调用方不得持 `sink_mutex`。
void push_tail(std::string line) {
  const std::scoped_lock lock(tail_mutex);
  tail_lines.push_back(std::move(line));
  while (tail_lines.size() > tail_capacity) tail_lines.pop_front();
}

/// 共用的“取最近 N 行”实现；`millis` = 0 表示无限等锁（普通路径）。
[[nodiscard]] auto collect_tail(std::size_t max_lines, bool try_only) -> std::string {
  // 两种取锁形态共用一个实现：`std::scoped_lock` 不能“尝试取”，
  // 而 `unique_lock(try_to_lock)` 又多一处构造分支——用 defer + 显式取锁最直白。
  // lint-allow: L9 崩溃尾部采样需要在“普通锁”与“try_lock”两种形态间切换（见上方说明）
  std::unique_lock<std::mutex> lock(tail_mutex, std::defer_lock);
  if (try_only) {
    // `try_lock` 带 `[[nodiscard]]`：拿不到锁就是“采样失败”，而不是可忽略的返回值。
    if (!lock.try_lock()) return {};
  } else {
    lock.lock();  // lint-allow: L9 见上方说明
  }
  if (tail_lines.empty()) return {};
  const std::size_t count =
      (max_lines == 0 || max_lines >= tail_lines.size()) ? tail_lines.size() : max_lines;
  std::string out;
  out.reserve(count * 96U);
  for (std::size_t index = tail_lines.size() - count; index < tail_lines.size(); ++index) {
    out += tail_lines[index];
  }
  return out;
}

}  // namespace

void set_level(Level level) noexcept { level_state.store(level, std::memory_order_relaxed); }
auto level() noexcept -> Level { return level_state.load(std::memory_order_relaxed); }

auto level_from_name(std::string_view name) noexcept -> Level {
  if (name == "trace") return Level::Trace;
  if (name == "debug") return Level::Debug;
  if (name == "info") return Level::Info;
  if (name == "warn" || name == "warning") return Level::Warn;
  if (name == "error") return Level::Error;
  if (name == "off" || name == "none") return Level::Off;
  return Level::Info;
}

void set_sink(std::function<void(Level, std::string_view)> sink) {
  const std::scoped_lock lock(sink_mutex);
  sink_state = std::move(sink);
}

auto add_listener(std::function<void(Level, std::string_view)> listener) -> std::uint64_t {
  const std::scoped_lock lock(sink_mutex);
  const std::uint64_t id = next_listener_id++;
  listeners.push_back(Listener{id, std::move(listener)});
  return id;
}

void remove_listener(std::uint64_t id) noexcept {
  const std::scoped_lock lock(sink_mutex);
  std::erase_if(listeners, [id](const Listener& item) { return item.id == id; });
}

void write(Level msg_level, std::string_view message) {
  std::function<void(Level, std::string_view)> sink;
  std::vector<std::function<void(Level, std::string_view)>> current_listeners;
  {
    const std::scoped_lock lock(sink_mutex);
    sink = sink_state;
    // 拷一份**回调**（不是订阅项）：`write` 之后不再碰 `listeners`——
    // 回调里若又调 `remove_listener`（很常见：收到某条日志后自行退订），
    // 拿着引用遍历就会踩到被擦除的元素。
    current_listeners.reserve(listeners.size());
    for (const auto& item : listeners) current_listeners.push_back(item.callback);
  }
  // 尾部缓冲**先于** sink：不依赖排版形态、不依赖是否开了文件落盘（见 `tail` 说明）。
  push_tail(std::format("[{}] {:<5} {}\n", time::iso8601_now(), to_string(msg_level), message));
  if (sink) {
    sink(msg_level, message);
  } else {
    const std::string line =
        std::format("[{}] {:<5} {}\n", time::iso8601_now(), to_string(msg_level), message);
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
  }
  for (const auto& listener : current_listeners) listener(msg_level, message);
}

namespace tail {

void set_capacity(std::size_t lines) {
  const std::scoped_lock lock(tail_mutex);
  tail_capacity = lines == 0 ? 1 : lines;
  while (tail_lines.size() > tail_capacity) tail_lines.pop_front();
}

auto recent(std::size_t max_lines) -> std::string { return collect_tail(max_lines, false); }

auto try_recent(std::size_t max_lines) -> std::string { return collect_tail(max_lines, true); }

}  // namespace tail

ScopedTimer::ScopedTimer(Level level, std::string label)    : level_(level), label_(std::move(label)), start_ns_(time::now_ns()) {}

ScopedTimer::~ScopedTimer() {
  if (level() > level_) return;
  write(level_,
        std::format("{} took {}", label_, time::format_duration_ns(time::now_ns() - start_ns_)));
}

}  // namespace st::log

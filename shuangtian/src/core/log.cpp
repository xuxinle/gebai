#include "st/core/log.hpp"

#include <atomic>
#include <cstdio>
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

ScopedTimer::ScopedTimer(Level level, std::string label)
    : level_(level), label_(std::move(label)), start_ns_(time::now_ns()) {}

ScopedTimer::~ScopedTimer() {
  if (level() > level_) return;
  write(level_,
        std::format("{} took {}", label_, time::format_duration_ns(time::now_ns() - start_ns_)));
}

}  // namespace st::log

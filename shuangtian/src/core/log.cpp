#include "st/core/console_log.hpp"
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

/**
 * 统一输出核心：级别消息（带时间戳前缀）与纯文本输出（无前缀，`st::print`/`st::eprint`）
 * 走同一条管道——tail 环形缓冲、可替换 sink、订阅 listener、console_log 镜像落盘。
 *
 * ## 为什么 print 也走这里（统一入口的意义）
 *
 * 所有对外输出（CLI 进度、测试结果、业务日志、错误提示）**只有一个出口**：
 * 改格式/加订阅/查尾部/落盘都在一处。避免三套输出各写各的（print 直接写 FD、
 * ST_LOG 写 sink、测试框架又一份），认知与维护都省。
 *
 * 差异只保留两个：
 * - 前缀：级别消息带 `[时间戳] LEVEL`，纯文本不带（CLI 可读性）；
 * - 裁剪：`ST_LOG_*` 宏被 `ST_LOG_DISABLED` 裁（release 裁事件日志），
 *   `st::print`/`st::eprint` 是函数**永远在**（CLI 输出不可裁）。
 */
void write_impl(Level msg_level, std::string_view message, bool prefixed, bool to_stderr) {
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
  const std::string line =
      prefixed ? std::format("[{}] {:<5} {}\n", time::iso8601_now(), to_string(msg_level),
                             message)
               : std::string(message);
  // 尾部缓冲**先于** sink：不依赖排版形态、不依赖是否开了文件落盘（见 `tail` 说明）。
  push_tail(line);
  if (sink) {
    sink(msg_level, message);
  } else {
    // 无 sink（默认直写终端）：print 走 stdout、级别日志走 stderr——
    // 与调用方的语义一致（进度给人看、事件进 stderr 可管道）。
    std::fwrite(line.data(), 1, line.size(), to_stderr ? stderr : stdout);
    std::fflush(to_stderr ? stderr : stdout);
    // 镜像落盘：`st::print` 与 `ST_LOG_*` 都进 `.st/logs/*.log`——
    // 构建日志（print 镜像）与事件日志（ST_LOG）同一份文件，事后一文件看全。
    console_log::mirror(line);
  }
  for (const auto& listener : current_listeners) listener(msg_level, message);
}

void write(Level msg_level, std::string_view message) {
  write_impl(msg_level, message, /*prefixed=*/true, /*to_stderr=*/true);
}

/// 纯文本输出（无前缀；`st::print`/`st::eprint` 的底层）。
/// `to_stderr` = true 时写标准错误（`st::eprint`），否则写标准输出（`st::print`）。
void write_plain(std::string_view message, bool to_stderr) {
  write_impl(Level::Info, message, /*prefixed=*/false, to_stderr);
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

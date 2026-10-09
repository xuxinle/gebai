/// State 写入追踪（诊断基建）：`ST_TRACE_STATE=1` 时把每次 State 写入记进环形缓冲，
/// 崩溃时随调用栈一起输出。
///
/// ## 为什么需要
///
/// "数据进了、界面不动"是本框架最难查的一类问题——本会话实测三次：
/// ① `git_log_` 读到了、列表却是空的（缺标脏）；
/// ② Tabs 属性面写入不通知宿主（`active` 读回来变了、视图不切）；
/// ③ Input 属性面写入不通知（提交框 `set text` 成功、提交时报"空"）。
/// 三次的**共同点**：写入发生了（`get` 能读到新值），但**没人知道要重画**。
///
/// 排查这类问题时，"写入到底有没有发生、从哪来"是第一问。有了追踪，这一问
/// 直接看日志；没有时只能在各处插 print（本会话就是这么做的，每次都重来一遍）。
///
/// ## 用法
///
/// ```
/// ST_TRACE_STATE=1 ./build/dev/bin/gbcode --headless ...
/// # 崩溃时自动附带最近 64 次写入；也可在日志里看到每次写入的序号与宿主
/// ```
///
/// ## 设计取舍
///
/// - **默认关闭**：追踪有开销（每次写入一次记录 + 短栈采集），且正常开发不需要。
///   环境变量开关是"用的时候打开"的最小代价形态。
/// - **环形缓冲（固定 64 条）**：崩溃时的现场在**最近若干次操作**里，不需要全量历史；
///   固定容量避免"跑久了内存涨"。这与"崩溃栈只要最近几十帧"同一个判断。
/// - **只记地址不记值**：值可能是任意类型（没有统一 to_string），而地址 + 序号
///   已足够回答"写没写、写了几个、是不是同一个 State"——配合调用点日志定位。

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/entry.hpp"
#include "st/ui/state_trace.hpp"

namespace st::ui::trace {

namespace {

/// 环形缓冲容量。
///
/// 64 的理由：崩溃现场在"刚刚发生的那几次操作"里。太少（如 8）会在一次批量更新
/// （`for_each` 里写 30 个元素）里被冲掉；太多则崩溃输出被淹没。
constexpr std::size_t kCapacity = 64;

struct Entry {
  std::uint64_t sequence{0};      ///< 全局写入序号（判断"两次崩溃之间写了多少"）
  const void* state{nullptr};     ///< State 对象地址
  const char* site{nullptr};      ///< 静态描述（编译期字符串，不持有所有权）
  bool during_build{false};       ///< 是否发生在构建期（②那条诊断的补充）
};

std::array<Entry, kCapacity> g_entries{};
std::atomic<std::size_t> g_count{0};      ///< 总写入次数（用于序号与"够不够满"）
std::atomic<bool> g_enabled{false};
std::atomic<bool> g_initialized{false};

/// 惰性读环境变量（首次写入时读一次——不放静态初始化，避免影响启动顺序）。
auto enabled() -> bool {
  if (!g_initialized.load(std::memory_order_relaxed)) {
    const char* value = std::getenv("ST_TRACE_STATE");
    g_enabled.store(value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0,
                    std::memory_order_relaxed);
    g_initialized.store(true, std::memory_order_relaxed);
  }
  return g_enabled.load(std::memory_order_relaxed);
}

}  // namespace

auto is_enabled() -> bool { return enabled(); }

void record_write(const void* state, const char* site, bool during_build) {
  if (!enabled() || state == nullptr) return;
  const std::size_t index = g_count.fetch_add(1, std::memory_order_relaxed);
  g_entries[index % kCapacity] = Entry{index + 1, state, site, during_build};
}

auto snapshot() -> std::vector<std::string> {
  std::vector<std::string> out;
  if (!enabled()) return out;
  const std::size_t total = g_count.load(std::memory_order_relaxed);
  const std::size_t count = total < kCapacity ? total : kCapacity;
  if (count == 0) return out;
  out.reserve(count);
  // 从最旧到最新（溢出处跳过未被写过的槽）。
  const std::size_t first = total > kCapacity ? total - kCapacity : 0;
  for (std::size_t index = first; index < total; ++index) {
    const Entry& entry = g_entries[index % kCapacity];
    out.push_back(std::format("#{} State@{} {}{}", entry.sequence,
                              static_cast<const void*>(entry.state),
                              entry.site == nullptr ? "?" : entry.site,
                              entry.during_build ? "（构建期）" : ""));
  }
  return out;
}

auto has_records() -> bool {
  return enabled() && g_count.load(std::memory_order_relaxed) > 0;
}

auto format_for_crash() -> std::string {
  const auto entries = snapshot();
  if (entries.empty()) return {};
  std::string out = std::format("  最近 {} 次 State 写入（最旧在前；只有 ST_TRACE_STATE=1 时记录）:\n",
                                entries.size());
  for (const auto& entry : entries) {
    out += "    ";
    out += entry;
    out += "\n";
  }
  return out;
}

void install_crash_hook() {
  // 只挂一次；重复调用无害（幂等）。
  st::set_crash_extra_provider([]() -> std::string {
    if (!has_records()) return {};
    return format_for_crash();
  });
}

}  // namespace st::ui::trace

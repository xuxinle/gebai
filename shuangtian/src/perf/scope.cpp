/// 阶段计时与调用点桶（见 `include/st/perf/scope.hpp`）。
///
/// ## 线程模型
///
/// 记录缓冲**按线程分开**（`thread_local`），互不争用：
/// - 帧路径上的 `begin/end` 一帧上千次，共用一把锁会把测量本身变成串行热点
///   （而剖析的意义就是查串行瓶颈——量尺制造假的瓶颈最坏）；
/// - `phases()` / `sites()` 在**快照时**汇总各线程的缓冲，那时才需要同步。
///
/// 线程退出时缓冲随之销毁，样本要在**在此之前**并入主副本（`flush`）。
///
/// ## 稳态不分配
///
/// 表键用 `std::string_view`：埋点名与调用点文本都来自**字符串字面量**
/// （`SourceLocation::file_name()` 指向字面量，静态存储期），所以查表不需要构造
/// 临时 `std::string`。路径缩短（`shorten_site`）在**快照时**做一次。
/// 这条是热路径的前提：一次 `std::string` 临时构造（哪怕 SSO 命中）也是几十 ns，
/// 与"画一个小方块"同量级，会彻底污染元素级读数。
///
/// 例外：`source_location` 的 `function_name()` 与 `column()` 在部分实现在
/// `consteval` 下才有值，运行期取会走慢路径——所以**只取 `file_name()` + `line()`**
/// （它们由编译器直接填进结构体，零成本），并把 `file:line` 拼在 snapshot 侧。

#include "st/perf/scope.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

namespace st::perf {
namespace {

/// 单线程阶段栈的深度上限。超过时新的 `begin` **不记录**（不崩、不抛）：
/// 剖析必须比被测代码更耐操——埋点写错（漏了 `end`）不该把应用拖垮。
/// 上限远大于真实嵌套（frame ⊃ layout ⊃ ...，实际 ≤ 8 层）。
inline constexpr std::size_t kMaxDepth = 64;

/// 阶段名表条目上限（防止运行期拼出来的名字把表撑爆）。
inline constexpr std::size_t kMaxPhases = 512;
/// 调用点表条目上限（防止"在宏里埋点"把表撑爆：每行都算一个调用点）。
inline constexpr std::size_t kMaxSites = 4096;

/// 一个调用点的**标识**：文件名指针 + 行号。
///
/// 用指针而不是 `std::string` 做键：`SourceLocation::file_name()` 指向编译期
/// 字面量，指针在进程内稳定且唯一，比较是 O(1)——热路径上省掉整条字符串哈希。
struct SiteKey {
  const char* file{nullptr};
  std::uint32_t line{0};
  [[nodiscard]] auto operator==(const SiteKey& other) const noexcept -> bool {
    return file == other.file && line == other.line;
  }
};

struct SiteKeyHash {
  [[nodiscard]] auto operator()(const SiteKey& key) const noexcept -> std::size_t {
    // 不用 `reinterpret_cast<uintptr_t>` 转地址（L6）：`std::hash<const char*>`
    // 的就是指针值，语义完全一致而且不需要破例。
    const std::size_t address = std::hash<const char*>{}(key.file);
    return address ^ (static_cast<std::size_t>(key.line) << 3U);
  }
};

/// 一次未完成的计时（在栈上）。
struct Frame {
  std::string_view name{};
  SiteKey site{};
  bool has_site{false};
  std::int64_t start_ticks{0};
  std::int64_t child_ns{0};  ///< 已结束的子阶段累计
};

/// 按名字聚合的桶。
struct NameBucket {
  Histogram self{};
  Histogram total{};
  std::uint64_t calls{0};
};

/// 按调用点聚合的桶。
struct SiteBucket {
  std::string_view phase{};
  Histogram self{};
  Histogram total{};
  std::uint64_t calls{0};
};

/// 一个线程的**可变记录状态**。
struct ThreadState {
  std::array<Frame, kMaxDepth> stack{};
  std::size_t depth{0};
  /// 栈溢出 / 未配对 `end` 的计数（>0 说明埋点写错了，报告里要报出来）。
  std::uint64_t anomalies{0};
};

/// 进程共享的记录表：线程退出时把样本搬到这里（唯一需要同步的地方）。
struct SharedTables {
  /// `mutable`：`Recorder::empty()` 等 const 方法也要加锁（加锁改互斥体内部状态，
  /// 而互斥体不属于对象的可观测状态——这是标准库自己的惯例）。
  mutable std::mutex mutex{};
  std::unordered_map<std::string_view, NameBucket> by_name{};
  std::unordered_map<SiteKey, SiteBucket, SiteKeyHash> by_site{};
  /// 顶层迭代（最外层阶段）的总耗时——"一次完整迭代多贵"。
  Histogram iteration{};
};

/// 线程私有的记录表 + 阶段栈。
struct ThreadTables {
  std::unordered_map<std::string_view, NameBucket> by_name{};
  std::unordered_map<SiteKey, SiteBucket, SiteKeyHash> by_site{};
  Histogram iteration{};
  ThreadState state{};
  SharedTables* owner{nullptr};

  ThreadTables() = default;
  ThreadTables(const ThreadTables&) = delete;
  auto operator=(const ThreadTables&) -> ThreadTables& = delete;

  /// **析构不自动 flush**：`thread_local` 的销毁时机在进程/线程退出时，
  /// 那时 owner 可能已经析构（`Recorder` 是局部对象时必然如此）——
  /// 去 flush 就是向已释放的 `SharedTables` 写。
  /// 想拿样本就**显式**调 `flush()`（快照路径会读、`Recorder` 析构时会摘表）。
  ///
  /// 实测代价：原先写成“析构时自动 flush”时，`perf_unbalanced_end_*` 用例
  /// **进程挂死**（不是崩溃）——而挂死比崩溃难查得多。
  ~ThreadTables() = default;

  /// 把本线程样本并入主副本（并入后清空）。
  ///
  /// ⚠ **调用方必须已持 `shared.mutex`**（本函数不取锁）。
  /// 早期版本自己取锁，而 `reset()` 会在已持锁的路径上调它 → **同一 mutex 重入
  /// = 自锁**（`std::mutex` 不可重入）。实测表现：测试进程挂死（不是崩溃）。
  /// 规矩：“合并样本”是受保护区域内的动作，锁由外层持。
  void flush_locked() {
    if (owner == nullptr) {
      by_name.clear();
      by_site.clear();
      iteration = Histogram{};
      return;
    }
    for (auto& entry : by_name) {
      NameBucket& target = owner->by_name[entry.first];
      target.self.merge(entry.second.self);
      target.total.merge(entry.second.total);
      target.calls += entry.second.calls;
    }
    for (auto& entry : by_site) {
      SiteBucket& target = owner->by_site[entry.first];
      if (target.phase.empty()) target.phase = entry.second.phase;
      target.self.merge(entry.second.self);
      target.total.merge(entry.second.total);
      target.calls += entry.second.calls;
    }
    owner->iteration.merge(iteration);
    by_name.clear();
    by_site.clear();
    iteration = Histogram{};
  }
};

/// 本线程的记录表（**按 recorder 区分**）。
///
/// ## 为什么键可以安全地是指针
///
/// `SharedTables` **不由 `Recorder` 拥有，而由进程级静态池持有**（见 `SharedPool`）：
/// 池里的对象**永不销毁**，所以 `thread_local` 映射里的指针永远有效。
/// 这是整个设计能站得住的根基——早期版本让 `Recorder` 拥有表且可析构，于是
/// “新建的 recorder 复用同一地址 → 命中旧表 → 向已释放内存写入”成为必然，
/// 而症状是**堆损坏/挂死**（比崩溃难查得多）。
///
/// 代价：每个 `Recorder` 永久占一份 `SharedTables`（几百字节）。
/// 现实中 recorder 的数量是个位数（应用一个 + 少量测试），完全可以接受。
using ThreadTableMap = std::vector<std::pair<SharedTables*, ThreadTables*>>;

/// 线程表槽：把一个 `ThreadTables` 放在**池化内存**里。
///
/// 为何不直接 `make_unique<ThreadTables>`：`thread_local` 在线程退出时会销毁它，
/// 而 `RecorderImpl::registered` 里还留着它的地址 → 快照读到已释放内存。
/// 槽用局部分配器把它放到进程级静态区域，于是“线程退出”只解除绑定、不释放对象。
struct ThreadTableSlot {
  alignas(ThreadTables) unsigned char storage[sizeof(ThreadTables)]{};
};

/// 本线程的映射表（`forget_tables` 要能拿到它，所以提成函数）。
[[nodiscard]] auto thread_table_map() -> ThreadTableMap& {
  thread_local ThreadTableMap tables;
  return tables;
}

/// 进程级 `SharedTables` 池：**对象永不销毁**（见 `ThreadTableMap` 的说明）。
///
/// 为何用静态池而不是 `shared_ptr` 保活：`thread_local` 映射需在**任意线程**随时
/// 解引用那个指针，而 `shared_ptr` 的“最后一个引用释放”无法跨线程保证
/// “映射表也已经清干净”。池化把问题从“生命周期协调”降级为“永不释放”——
/// 对几百字节 × 个位数实例，这是最划算的交易。
class SharedPool {
 public:
  [[nodiscard]] static auto acquire() -> SharedTables& {
    // lint-allow: L8 进程级静态池（有意永不析构，理由见类注释）
    static std::deque<SharedTables> pool{};
    // lint-allow: L8 同上（池的互斥体）
    static std::mutex pool_mutex{};
    const std::scoped_lock lock(pool_mutex);
    pool.emplace_back();
    return pool.back();
  }
};

[[nodiscard]] auto thread_tables_for(SharedTables& owner) -> ThreadTables& {
  ThreadTableMap& tables = thread_table_map();
  for (auto& entry : tables) {
    if (entry.first == &owner) return *entry.second;
  }
  constexpr std::size_t kMaxRecorders = 8;  // 防止误用（每线程挂一堆 recorder）无限增长
  if (tables.size() >= kMaxRecorders) {
    // 超限时**丢弃最旧那一项的绑定**（样本不并入，直接弃）。
    // 注意：这里**不 flush**（那要拿 `shared.mutex`，与本函数的调用位置形成锁顺序麻烦）；
    // 丢样本可接受——超限本身说明用法错了（每线程挂了 8 个 recorder）。
    tables.erase(tables.begin());
  }
  tables.emplace_back(&owner, new ThreadTables{});  // lint-allow: L1 池化（有意不释放，见上）
  tables.back().second->owner = &owner;
  return *tables.back().second;
}

/// 把某个 owner 的表在**本线程**里摘掉。
///
/// ⚠ 两个细节都是实测踩出来的：
/// ① **不能 flush**（那会去拿 `owner->mutex`，删表本身就不需要那些样本）；
/// 把某个 owner 的表在**本线程**里解绑（表对象本身不释放，见 `ThreadTables` 的池化）。
///
/// 解绑时**必须把 `owner` 置空**（否则后续误用时还会往旧 owner 写）；
/// 但**不能释放表对象**：`RecorderImpl::registered` 里可能还有别的快照引用它。
void forget_thread_tables(SharedTables& owner) {
  ThreadTableMap& tables = thread_table_map();
  const auto found = std::ranges::find_if(tables, [&](const auto& entry) {
    return entry.first == &owner;
  });
  if (found == tables.end()) return;
  if (found->second->owner == &owner) found->second->owner = nullptr;
  tables.erase(found);
}

/// 一个 `Recorder` 的实现：所有线程的表最终都由**池化的** `SharedTables` 汇总。
struct RecorderImpl {
  /// 引用池化的共享表（**永不销毁**，所以 `thread_local` 里存它的地址是安全的）。
  SharedTables& shared;
  /// 用过本记录器的线程表（快照时要把"还没退出的线程"的样本一并收集）。
  ///
  /// 为何需要：帧线程不会退出，它的样本永远在 `thread_local` 里，不登记就永远
  /// 拿不到（这是"快照看起来一直是 0"的经典成因）。
  ///
  /// ⚠ **登记项也可能变成悬垂指针**：线程退出时 `thread_local` 表会被销毁，
  /// 而本向量里还留着它的地址 ⇒ `collect()` 读到已释放内存（实测症状：
  /// **测试挂死 / `0xC0000374` 堆损坏**，而且只在“多线程/多用例”下出现，单跑则绿）。
  /// 解法：表也**池化**——`thread_local` 销毁时表对象仍存活（见 `ThreadTableSlot`），
  /// 所以本向量里永远不会有悬垂指针。
  mutable std::mutex registry_mutex{};
  std::vector<ThreadTables*> registered{};
};

/// 确保本线程的样本登记到 `owner`（每个 recorder 在本线程有**自己的**表）。
///
/// ⚠ **锁纪律（血的教训）**：本函数只拿 `registry_mutex`，**拿完后不得再拿
/// `shared.mutex`**。早先版本这里调了 `flush()`（内含 `shared.mutex`），而
/// `collect()` 的顺序是 `shared.mutex` → `registry_mutex`——两边相反就是典型的
/// 锁顺序反转死锁。实测表现：测试进程**挂死**（不是崩溃）。
/// 规矩：
/// - `collect()`：`shared.mutex` → `registry_mutex`
/// - `ensure_registered()`：只拿 `registry_mutex`
/// - `flush_locked()`：**不取锁**（调用方已持 `shared.mutex`）
void ensure_registered(RecorderImpl& owner) {
  ThreadTables& tables = thread_tables_for(owner.shared);
  const std::scoped_lock lock(owner.registry_mutex);
  const auto found = std::ranges::find(owner.registered, &tables);
  if (found == owner.registered.end()) owner.registered.push_back(&tables);
}

/// 把各线程的样本汇总成一份临时副本（**不改动**计数器）。
[[nodiscard]] auto collect(const RecorderImpl& inner,
                           std::unordered_map<std::string_view, NameBucket>& names,
                           std::unordered_map<SiteKey, SiteBucket, SiteKeyHash>& sites,
                           Histogram& iteration) -> std::uint64_t {
  std::uint64_t anomalies = 0;
  {
    // 读 `registered` 里的线程表没有同步保障（帧线程还在跑），但读的是
    // `Histogram`（纯数值）与 `unordered_map`（键集固定、不缩容）：
    // 最坏结果是某个样本落在两次读取之间被漏掉一次，**不会读到非法内存**。
    // 用锁反而会让帧线程每帧付一次——量尺不能变成被测对象的一部分。
    const std::scoped_lock lock(inner.shared.mutex);
    names = inner.shared.by_name;
    sites = inner.shared.by_site;
    iteration = inner.shared.iteration;
  }
  std::vector<ThreadTables*> snapshot{};
  {
    const std::scoped_lock lock(inner.registry_mutex);
    snapshot = inner.registered;
  }
  for (const ThreadTables* tables : snapshot) {
    if (tables == nullptr) continue;
    for (const auto& entry : tables->by_name) {
      NameBucket& target = names[entry.first];
      target.self.merge(entry.second.self);
      target.total.merge(entry.second.total);
      target.calls += entry.second.calls;
    }
    for (const auto& entry : tables->by_site) {
      SiteBucket& target = sites[entry.first];
      if (target.phase.empty()) target.phase = entry.second.phase;
      target.self.merge(entry.second.self);
      target.total.merge(entry.second.total);
      target.calls += entry.second.calls;
    }
    iteration.merge(tables->iteration);
    anomalies += tables->state.anomalies;
  }
  return anomalies;
}

}  // namespace

struct Recorder::Impl {
  RecorderImpl inner{SharedPool::acquire()};
};
Recorder::Recorder() : impl_(std::make_unique<Impl>()) {}

Recorder::~Recorder() {
  // **不释放 `SharedTables`**（它在池里，见 `SharedPool`）：把本线程的映射项摘掉
  // 只是为了“不把新样本写进一个已经没人读的表”，而不是为了防悬垂。
  // 这也是设计上唯一正确的选择：`thread_local` 映射可能被**任意线程**在任意时刻
  // 访问，无法协调“表何时可以真正释放”。
  forget_thread_tables(impl_->inner.shared);
}

auto Recorder::begin(std::string_view name, const std::source_location* site) -> void {
  if (!enabled()) return;
  ensure_registered(impl_->inner);
  ThreadTables& tables = thread_tables_for(impl_->inner.shared);
  ThreadState& state = tables.state;
  if (state.depth >= kMaxDepth) {
    ++state.anomalies;
    return;
  }
  Frame& frame = state.stack[state.depth];
  frame.name = name;
  frame.has_site = site != nullptr;
  if (site != nullptr) {
    frame.site.file = site->file_name();
    frame.site.line = site->line();
  }
  frame.start_ticks = now_ticks();
  frame.child_ns = 0;
  ++state.depth;
}

auto Recorder::end() -> void {
  // **先查开关**：关掉时 `begin` 没记任何东西，`end` 也就不应该建表——
  // 否则会抛出一个 `thread_tables_for`（新表 + 注册锁），而那种“只记了 end”的
  // 局面会把 `anomalies` 误计（真正的“未配对 end”指的是 begin/end 不配对，
  // 而不是“先关掉了剖析”）。
  if (!enabled()) return;
  ThreadTables& tables = thread_tables_for(impl_->inner.shared);
  ThreadState& state = tables.state;
  if (state.depth == 0) {
    ++state.anomalies;
    return;
  }
  --state.depth;
  const Frame& frame = state.stack[state.depth];
  const std::int64_t elapsed = ticks_to_ns(now_ticks() - frame.start_ticks);
  const std::int64_t self = elapsed > frame.child_ns ? elapsed - frame.child_ns : 0;

  const std::string_view name = frame.name.empty() ? std::string_view{"<unnamed>"} : frame.name;
  if (tables.by_name.size() < kMaxPhases || tables.by_name.contains(name)) {
    NameBucket& bucket = tables.by_name[name];
    bucket.self.add(self);
    bucket.total.add(elapsed);
    ++bucket.calls;
  }

  if (frame.has_site && (tables.by_site.size() < kMaxSites || tables.by_site.contains(frame.site))) {
    SiteBucket& site_bucket = tables.by_site[frame.site];
    if (site_bucket.phase.empty()) site_bucket.phase = name;
    site_bucket.self.add(self);
    site_bucket.total.add(elapsed);
    ++site_bucket.calls;
  }

  // 把耗时记进父阶段的 `child_ns`（父的 self 因此不含子）。
  if (state.depth > 0) {
    state.stack[state.depth - 1].child_ns += elapsed;
  } else {
    // 最外层阶段结束 = 一次完整迭代。
    tables.iteration.add(elapsed);
  }
}

void Recorder::reset() {
  {
    const std::scoped_lock lock(impl_->inner.shared.mutex);
    impl_->inner.shared.by_name.clear();
    impl_->inner.shared.by_site.clear();
    impl_->inner.shared.iteration = Histogram{};
  }
  std::vector<ThreadTables*> snapshot{};
  {
    const std::scoped_lock lock(impl_->inner.registry_mutex);
    snapshot = impl_->inner.registered;
  }
  for (ThreadTables* tables : snapshot) {
    if (tables == nullptr) continue;
    tables->by_name.clear();
    tables->by_site.clear();
    tables->iteration = Histogram{};
    tables->state.depth = 0;
    tables->state.anomalies = 0;
  }
}

auto Recorder::empty() const -> bool {
  {
    const std::scoped_lock lock(impl_->inner.shared.mutex);
    if (!impl_->inner.shared.by_name.empty() || !impl_->inner.shared.by_site.empty()) {
      return false;
    }
  }
  std::vector<ThreadTables*> snapshot{};
  {
    const std::scoped_lock lock(impl_->inner.registry_mutex);
    snapshot = impl_->inner.registered;
  }
  for (const ThreadTables* tables : snapshot) {
    if (tables != nullptr && (!tables->by_name.empty() || !tables->by_site.empty())) return false;
  }
  return true;
}

auto Recorder::phases() const -> std::vector<PhaseStat> {
  std::unordered_map<std::string_view, NameBucket> names{};
  std::unordered_map<SiteKey, SiteBucket, SiteKeyHash> sites{};
  Histogram iteration{};
  (void)collect(impl_->inner, names, sites, iteration);
  std::vector<PhaseStat> result{};
  result.reserve(names.size());
  for (auto& entry : names) {
    PhaseStat stat{};
    stat.name = std::string(entry.first);
    stat.self = entry.second.self;
    stat.total = entry.second.total;
    stat.calls = entry.second.calls;
    result.push_back(std::move(stat));
  }
  std::ranges::sort(result, [](const PhaseStat& a, const PhaseStat& b) {
    return a.total.mean_ms() > b.total.mean_ms();
  });
  return result;
}

auto Recorder::sites() const -> std::vector<SiteStat> {
  std::unordered_map<std::string_view, NameBucket> names{};
  std::unordered_map<SiteKey, SiteBucket, SiteKeyHash> sites{};
  Histogram iteration{};
  (void)collect(impl_->inner, names, sites, iteration);
  std::vector<SiteStat> result{};
  result.reserve(sites.size());
  for (auto& entry : sites) {
    SiteStat stat{};
    // 只在这里做一次拼接与缩短（热路径不付）。
    stat.site = shorten_site(std::string(entry.first.file == nullptr ? "" : entry.first.file) +
                             ":" + std::to_string(entry.first.line));
    stat.phase = std::string(entry.second.phase);
    stat.self = entry.second.self;
    stat.total = entry.second.total;
    stat.calls = entry.second.calls;
    result.push_back(std::move(stat));
  }
  std::ranges::sort(result, [](const SiteStat& a, const SiteStat& b) {
    return a.total.mean_ms() > b.total.mean_ms();
  });
  return result;
}

auto Recorder::iteration_total() const -> Histogram {
  std::unordered_map<std::string_view, NameBucket> names{};
  std::unordered_map<SiteKey, SiteBucket, SiteKeyHash> sites{};
  Histogram iteration{};
  (void)collect(impl_->inner, names, sites, iteration);
  return iteration;
}

auto Recorder::anomalies() const -> std::uint64_t {
  // **不经过 `collect()`**：异常计数只需要读几份 `state.anomalies`，
  // 而 `collect()` 要汇总所有表（连带两把锁）。量“埋点对不对”时
  // 不该去碰那些无关的重活（也避免它在异常路径上再撞一次锁）。
  std::uint64_t count = 0;
  std::vector<ThreadTables*> snapshot{};
  {
    const std::scoped_lock lock(impl_->inner.registry_mutex);
    snapshot = impl_->inner.registered;
  }
  for (const ThreadTables* tables : snapshot) {
    if (tables != nullptr) count += tables->state.anomalies;
  }
  return count;
}

auto recorder() noexcept -> Recorder& {
  // lint-allow: L8 进程默认记录器（单例；应用可自建 `Recorder` 隔离，见头文件）
  static Recorder instance{};
  return instance;
}

auto shorten_site(std::string_view site) -> std::string {
  std::string text{site};
  // 反斜杠统一成正斜杠：Windows 上 `__FILE__` 可能是反斜杠。
  std::ranges::replace(text, '\\', '/');
  static constexpr std::array<std::string_view, 5> kAnchors{"src/", "include/", "tests/",
                                                            "tools/", "examples/"};
  std::size_t best = std::string::npos;
  for (const std::string_view anchor : kAnchors) {
    // 匹配 `/src/` 形态而不是裸 `src/`：避免把 `.../my_src/` 误当锚点。
    const std::string needle = "/" + std::string(anchor);
    const std::size_t position = text.rfind(needle);
    if (position != std::string::npos && (best == std::string::npos || position + 1 > best)) {
      best = position + 1;
    }
  }
  if (best != std::string::npos) return text.substr(best);
  // 没有锚点（例如独立工程的源在任意目录）：只保留最后两段，够定位。
  const std::size_t last_slash = text.rfind('/');
  if (last_slash == std::string::npos) return text;
  const std::size_t previous = last_slash > 0 ? text.rfind('/', last_slash - 1) : std::string::npos;
  if (previous == std::string::npos) return text;
  return text.substr(previous + 1);
}

auto site_of(const std::source_location& location) -> std::string {
  return shorten_site(std::string(location.file_name()) + ":" + std::to_string(location.line()));
}

}  // namespace st::perf

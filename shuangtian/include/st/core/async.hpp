#pragma once

/// 应用级异步任务系统（`DESIGN.md` §8）：**主线程只渲染**契约的一等支撑。
///
/// ## 线程契约（成文于 CONVENTIONS §11，这里是实现）
///
/// | 线程 | 允许 | 禁止 |
/// |---|---|---|
/// | 主（UI）线程 | layout / paint / 声明式重组 / 事件派发 / `post_ui` 回调 | 业务计算、阻塞 IO、长循环 |
/// | 工作线程 | 计算、IO、解析——任何不碰 UI 树的东西 | 写 `State`、碰元素、调组件接口 |
///
/// 工作线程要把结果交回 UI：**只能** `post_ui(fn)`（下一帧 `pump()` 有序执行）。
/// 直接从工作线程写 `State` 是数据竞争（重组是单线程的，正在遍历的树与并发写互斥不了）。
///
/// ## 两种提交方式（怎么选）
///
/// - `run_async(fn)`：**独立线程**跑这一次任务（`std::jthread`，RAII 自动 join）。
///   适合：长任务（构建/索引/大文件解析）、IO 密集且要独立失效的任务——
///   不占池、不被别的任务排队阻塞、生命周期与 Future 绑定。
/// - `submit(fn)`：**共享线程池**排队执行。适合：短任务、可并行分解的批量工作
///   （`parallel_for`）、吞吐优先的场景——池有上限（不打满系统），排队公平。
///
/// 两者都返回 `std::future`（`run_async` 的含异常转发；`submit` 的异常进 future）。
///
/// ## 取消
///
/// `run_async` 返回的句柄可 `cancel()`：置 `stop_token` 协作取消——任务在耗时点
/// 检查 `is_cancelled()` 提前退出（与 `dsl::AsyncCancel` 同一协作模式）。
/// 线程池任务不提供单任务取消（排队语义：要么跑完要么在池析构时丢弃）；
/// 需要取消的长任务用 `run_async`。

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace st::app {

/// 独立线程任务的控制块（`AsyncHandle` 持共享引用；`Tasks` 析构时 join 线程）。
struct AsyncBlock {
  std::stop_source source{};
  std::jthread thread{};
  std::atomic<bool> finished{false};
};

/// 独立线程任务的句柄：等待完成 + 协作取消。
class AsyncHandle {
 public:
  AsyncHandle() = default;
  /// 等待任务结束（阻塞；任务通常应已通过 future 取结果，这里是兜底）。
  void wait();
  /// 请求取消（协作式：任务在检查点自愿退出；不抢占）。
  void cancel() noexcept;
  /// 任务是否已被请求取消。
  [[nodiscard]] auto is_cancelled() const noexcept -> bool;
  [[nodiscard]] auto valid() const noexcept { return block_ != nullptr; }

 private:
  friend class Tasks;
  explicit AsyncHandle(std::shared_ptr<AsyncBlock> block) : block_(std::move(block)) {}
  std::shared_ptr<AsyncBlock> block_{};
};

/// 应用异步任务系统：`run_async`（独立线程）+ `submit`（线程池）+ `post_ui`（回主线程）。
///
/// 生命周期：通常由 `Application` 持有（`Application::tasks()`），随应用析构而收尾——
/// 析构时：UI 队列清空、池停止并 join、独立线程全部 join（不 detach，见 CONVENTIONS §3.5）。
class Tasks {
 public:
  /// `pool_threads == 0` 取 `hardware_concurrency`（夹取 [1, 8]）。
  explicit Tasks(std::size_t pool_threads = 0);
  ~Tasks();
  Tasks(const Tasks&) = delete;
  auto operator=(const Tasks&) -> Tasks& = delete;
  Tasks(Tasks&&) = delete;
  auto operator=(Tasks&&) = delete;

  /// —— 独立线程（长任务/IO 密集，与其它任务互不排队）——
  ///
  /// `fn` 签名 `(std::stop_token)`（可协作取消）或 `()`（无参，兼容简单任务）。
  /// 返回 future（取结果/异常）+ 句柄（取消）。
  template <class Fn>
  auto run_async(Fn&& fn)
      -> std::pair<std::future<std::invoke_result_t<Fn, std::stop_token>>, AsyncHandle> {
    using R = std::invoke_result_t<Fn, std::stop_token>;
    auto block = std::make_shared<AsyncBlock>();
    auto task = std::make_shared<std::packaged_task<R()>>(
        [fn = std::forward<Fn>(fn), block]() { return fn(block->source.get_token()); });
    auto future = task->get_future();
    {
      std::lock_guard<std::mutex> lock(async_mutex_);
      // jthread 的线程体持有 block 的共享引用：先起线程再存句柄是安全的
      //（block 总是活到线程退出——引用计数保证）。
      block->thread = std::jthread([task, block] {
        (*task)();
        block->finished.store(true, std::memory_order_release);
      });
      asyncs_.push_back(block);   // 登记以便析构时统一 join（不 detach）
    }
    return {std::move(future), AsyncHandle(block)};
  }

  /// —— 线程池（短任务/并行分解；排队共享并发额度）——
  template <class Fn>
  auto submit(Fn&& fn) -> std::future<std::invoke_result_t<Fn>> {
    using R = std::invoke_result_t<Fn>;
    auto task = std::make_shared<std::packaged_task<R()>>(std::forward<Fn>(fn));
    auto future = task->get_future();
    enqueue_pool([task] { (*task)(); });
    return future;
  }

  /// 并行执行 `[0, count)`（按池线程数分块；异常不允许逃逸——任务内自行处理）。
  template <class Fn>
  void parallel_for(std::size_t count, Fn&& fn) {
    if (count == 0) return;
    const std::size_t workers = pool_size() < count ? pool_size() : count;
    if (workers <= 1) {
      for (std::size_t i = 0; i < count; ++i) fn(i);
      return;
    }
    std::vector<std::future<void>> futures;
    futures.reserve(workers);
    const std::size_t chunk = (count + workers - 1) / workers;
    for (std::size_t w = 0; w < workers; ++w) {
      const std::size_t begin = w * chunk;
      const std::size_t end = begin + chunk < count ? begin + chunk : count;
      if (begin >= end) break;
      futures.push_back(submit([&fn, begin, end] {
        for (std::size_t i = begin; i < end; ++i) fn(i);
      }));
    }
    for (auto& future : futures) future.get();
  }

  /// —— 回主线程（下一帧 `pump()` 有序执行）——
  ///
  /// 线程安全；调用方通常是工作线程的收尾（把结果写 UI 的唯一合法路径）。
  /// `pump()` 在 `Application::tick()` 内被调用（每帧一次，执行全部积压）。
  void post_ui(std::function<void()> fn);
  /// 执行全部积压的 UI 回调（主线程调用；`Application::tick` 调）。
  auto pump() -> std::size_t;

  [[nodiscard]] auto pool_size() const noexcept -> std::size_t { return workers_.size(); }
  /// 池队列长度（诊断/背压观测；不含正在执行的）。
  [[nodiscard]] auto queued() const noexcept -> std::size_t;

 private:
  friend class AsyncHandle;
  void enqueue_pool(std::function<void()> job);
  void pool_loop(std::stop_token token);

  /// 活跃独立线程的登记（析构时统一 join——jthread 本身 RAII，这里显式管理顺序：
  /// 先请求停止（stop_source 在 block 里，由句持有者决定），再等池，最后 join 独立线程）。
  mutable std::mutex async_mutex_{};
  std::vector<std::shared_ptr<AsyncBlock>> asyncs_{};

  mutable std::mutex queue_mutex_{};
  std::condition_variable_any queue_cv_{};
  std::deque<std::function<void()>> jobs_{};
  std::vector<std::jthread> workers_{};
  bool pool_stopping_{false};

  mutable std::mutex ui_mutex_{};
  std::vector<std::function<void()>> ui_inbox_{};
};

}  // namespace st::app

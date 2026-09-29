#pragma once

/// 线程池（`std::jthread` + 协作式停止）。用于构建并行化与运行时后台任务。
/// 约定（`CONVENTIONS.md` §3.5）：线程一律 `std::jthread`（自动 join + `stop_token`），
/// 互斥一律 RAII（`std::scoped_lock`/`lock_guard`），禁手动 lock/unlock 与 detach。

#include <algorithm>
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

namespace st {

class ThreadPool {
 public:
  /// `threads == 0` 时取硬件并发数（至少 1）。
  explicit ThreadPool(std::size_t threads = 0);
  ~ThreadPool();

  ThreadPool(const ThreadPool&) = delete;
  auto operator=(const ThreadPool&) -> ThreadPool& = delete;
  ThreadPool(ThreadPool&&) = delete;
  auto operator=(ThreadPool&&) -> ThreadPool& = delete;

  /// 提交任务，返回 future（`Result` 语义的任务由调用方自行承载错误）。
  template <class Fn>
  auto submit(Fn&& fn) -> std::future<std::invoke_result_t<Fn>> {
    using R = std::invoke_result_t<Fn>;
    auto task = std::make_shared<std::packaged_task<R()>>(std::forward<Fn>(fn));
    auto future = task->get_future();
    enqueue([task]() { (*task)(); });
    return future;
  }

  /// 并行执行 [0, count) 区间（按线程数分块）；返回各自结果（异常不允许逃逸：任务内需自行处理错误）。
  template <class Fn>
  void parallel_for(std::size_t count, Fn&& fn) {
    if (count == 0) return;
    const std::size_t workers = std::min(thread_count(), count);
    if (workers <= 1) {
      for (std::size_t i = 0; i < count; ++i) fn(i);
      return;
    }
    std::vector<std::future<void>> futures;
    futures.reserve(workers);
    const std::size_t chunk = (count + workers - 1) / workers;
    for (std::size_t w = 0; w < workers; ++w) {
      const std::size_t begin = w * chunk;
      const std::size_t end = std::min(begin + chunk, count);
      if (begin >= end) break;
      futures.push_back(submit([&fn, begin, end]() {
        for (std::size_t i = begin; i < end; ++i) fn(i);
      }));
    }
    for (auto& future : futures) future.get();
  }

  [[nodiscard]] auto thread_count() const noexcept -> std::size_t { return workers_.size(); }
  /// 等待队列清空（不保证任务已完全结束，仅表示已分发完毕）。
  void wait_idle();

 private:
  void enqueue(std::function<void()> job);
  void worker_loop(std::stop_token token);

  std::mutex mutex_{};
  std::condition_variable_any cv_{};
  std::condition_variable idle_cv_{};
  std::queue<std::function<void()>> jobs_{};
  std::size_t active_{0};
  std::vector<std::jthread> workers_{};
};

/// 硬件并发数（检测失败返回 4）。
[[nodiscard]] auto hardware_concurrency() noexcept -> std::size_t;

}  // namespace st

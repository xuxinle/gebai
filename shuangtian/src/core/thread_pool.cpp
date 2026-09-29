#include "st/core/thread_pool.hpp"

namespace st {

auto hardware_concurrency() noexcept -> std::size_t {
  const unsigned detected = std::thread::hardware_concurrency();
  return detected == 0U ? 4U : static_cast<std::size_t>(detected);
}

ThreadPool::ThreadPool(std::size_t threads) {
  if (threads == 0) threads = hardware_concurrency();
  workers_.reserve(threads);
  for (std::size_t index = 0; index < threads; ++index) {
    workers_.emplace_back([this](std::stop_token token) { worker_loop(token); });
  }
}

ThreadPool::~ThreadPool() {
  for (auto& worker : workers_) worker.request_stop();
  cv_.notify_all();
}

void ThreadPool::enqueue(std::function<void()> job) {
  {
    const std::scoped_lock lock(mutex_);
    jobs_.push(std::move(job));
  }
  cv_.notify_one();
}

void ThreadPool::worker_loop(std::stop_token token) {
  while (true) {
    std::function<void()> job;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, token, [this] { return !jobs_.empty(); });
      if (jobs_.empty()) return;  // 已请求停止且无待处理任务
      job = std::move(jobs_.front());
      jobs_.pop();
      ++active_;
    }
    job();
    {
      const std::scoped_lock lock(mutex_);
      --active_;
      if (jobs_.empty() && active_ == 0) idle_cv_.notify_all();
    }
  }
}

void ThreadPool::wait_idle() {
  std::unique_lock lock(mutex_);
  idle_cv_.wait(lock, [this] { return jobs_.empty() && active_ == 0; });
}

}  // namespace st

/// 应用级异步任务系统实现（`st/app/async.hpp`；DESIGN §8「主线程只渲染」契约）。

#include "st/app/async.hpp"

#include <algorithm>
#include <utility>

namespace st::app {

// —— AsyncHandle ——

void AsyncHandle::wait() {
  if (block_ == nullptr) return;
  if (block_->thread.joinable()) block_->thread.join();
}

void AsyncHandle::cancel() noexcept {
  if (block_ != nullptr) block_->source.request_stop();
}

auto AsyncHandle::is_cancelled() const noexcept -> bool {
  return block_ != nullptr && block_->source.stop_requested();
}

// —— Tasks ——

Tasks::Tasks(std::size_t pool_threads) {
  std::size_t count = pool_threads;
  if (count == 0) {
    const unsigned hardware = std::thread::hardware_concurrency();
    count = hardware == 0 ? 2U : static_cast<std::size_t>(hardware);
  }
  // 上限 8：池面向「短任务吞吐」，超过核数收益递减而调度开销上升
  //（与原 dsl 私有池同口径——那是实测结论，不是拍脑袋）。
  count = std::clamp(count, std::size_t{1}, std::size_t{8});
  workers_.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    workers_.emplace_back([this](std::stop_token token) { pool_loop(token); });
  }
}

Tasks::~Tasks() {
  // 收尾顺序（成文契约）：① 停池（队列排空后退出）→ ② join 池 →
  // ③ 独立线程 join（它们的任务可能还在往 UI 队列投递，投完为止）→ ④ 清 UI 队列。
  // 不做「强取消」：任务没有义务响应停止请求，强杀线程是 UB；等它们自然结束
  //（协作取消由持有句柄的一方决定——析构方不知道哪些任务可弃）。
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    pool_stopping_ = true;
  }
  queue_cv_.notify_all();
  for (auto& worker : workers_) {
    if (worker.joinable()) worker.join();
  }
  {
    std::lock_guard<std::mutex> lock(async_mutex_);
    for (auto& block : asyncs_) {
      if (block->thread.joinable()) block->thread.join();
    }
    asyncs_.clear();
  }
}

void Tasks::enqueue_pool(std::function<void()> job) {
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    jobs_.push_back(std::move(job));
  }
  queue_cv_.notify_one();
}

void Tasks::pool_loop(std::stop_token token) {
  for (;;) {
    std::function<void()> job;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait(lock, token, [this] { return pool_stopping_ || !jobs_.empty(); });
      if ((pool_stopping_ && jobs_.empty()) || (token.stop_requested() && jobs_.empty())) return;
      if (jobs_.empty()) continue;
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    if (job) {
      // 任务异常不得杀工作线程（后续任务会饿死）——packaged_task 已把异常
      // 转进 future，这里的 catch 是二道保险（与 dsl 旧池同款）。
      try {
        job();
      } catch (...) {
      }
    }
  }
}

void Tasks::post_ui(std::function<void()> fn) {
  if (!fn) return;
  std::lock_guard<std::mutex> lock(ui_mutex_);
  ui_inbox_.push_back(std::move(fn));
}

auto Tasks::pump() -> std::size_t {
  std::vector<std::function<void()>> batch;
  {
    std::lock_guard<std::mutex> lock(ui_mutex_);
    // 整批换出：执行期间新投递的进下一批（不在持锁下执行——回调可能再投递）。
    batch.swap(ui_inbox_);
  }
  for (auto& fn : batch) {
    try {
      fn();
    } catch (...) {
      // UI 回调异常不吞掉整批：逐个隔离（一个坏回调不该饿死同批其它结果落地）。
    }
  }
  return batch.size();
}

auto Tasks::queued() const noexcept -> std::size_t {
  const std::lock_guard<std::mutex> lock(queue_mutex_);
  return jobs_.size();
}

}  // namespace st::app

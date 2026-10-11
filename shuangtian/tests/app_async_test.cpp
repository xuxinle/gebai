/// `st::app::Tasks` 单测：两种提交、回主线程、取消、并行分解、析构收尾。
/// 时序敏感处用「事件而非睡死」断言（future + 条件变量），避免共享机器上的抖动红灯
///（CONVENTIONS §7：环境相关的耗时门禁要标 SLOW，这里全部是确定性时序）。

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "st/app/async.hpp"
#include "st/test/test.hpp"

namespace {

using st::app::Tasks;

/// 泵到 inbox 出现目标数量（带超时上限，防死等挂测试）。
auto pump_until(Tasks& tasks, const std::atomic<int>& counter, int expected, int spins = 2000)
    -> bool {
  for (int i = 0; i < spins; ++i) {
    (void)tasks.pump();
    if (counter.load() >= expected) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return counter.load() >= expected;
}

}  // namespace

ST_TEST(app_tasks_pool_runs_and_returns) {
  Tasks tasks(2);
  auto future = tasks.submit([] { return std::string("pool-result"); });
  ST_CHECK_EQ(future.get(), std::string("pool-result"));
}

ST_TEST(app_tasks_run_async_independent_thread) {
  Tasks tasks(1);
  const auto main_id = std::this_thread::get_id();
  std::thread::id worker_id{};
  auto [future, handle] = tasks.run_async(
      [&worker_id](std::stop_token) {
        worker_id = std::this_thread::get_id();
        return 42;
      });
  ST_CHECK_EQ(future.get(), 42);
  ST_CHECK(worker_id != main_id);   // 确实不在主线程
  ST_CHECK(handle.valid());
  handle.wait();
}

ST_TEST(app_tasks_post_ui_pump_ordering) {
  Tasks tasks(1);
  std::atomic<int> done{0};
  std::vector<int> order{};
  std::mutex order_mutex{};

  // 从工作线程连续投递 3 个：pump 必须按提交顺序执行（有序性是契约——
  // 依赖顺序落地的状态更新不能被重排）。
  auto future = tasks.submit([&] {
    for (int i = 0; i < 3; ++i) {
      tasks.post_ui([&, i] {
        std::lock_guard<std::mutex> lock(order_mutex);
        order.push_back(i);
        done.fetch_add(1);
      });
    }
  });
  future.get();
  ST_CHECK(pump_until(tasks, done, 3));
  std::lock_guard<std::mutex> lock(order_mutex);
  ST_CHECK_EQ(order.size(), std::size_t{3});
  ST_CHECK_EQ(order[0], 0);
  ST_CHECK_EQ(order[1], 1);
  ST_CHECK_EQ(order[2], 2);
}

ST_TEST(app_tasks_cancel_is_cooperative) {
  Tasks tasks(1);
  std::atomic<bool> saw_cancel{false};
  std::atomic<bool> finished{false};
  auto [future, handle] = tasks.run_async([&](std::stop_token token) {
    // 忙等取消信号（协作点）：真实任务这里是分片检查
    while (!token.stop_requested()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    saw_cancel.store(true);
    return 7;
  });
  handle.cancel();
  ST_CHECK_EQ(future.get(), 7);         // 取消后任务在检查点退出并正常返回
  ST_CHECK(saw_cancel.load());
  finished.store(true);
  (void)finished;
}

ST_TEST(app_tasks_parallel_for_covers_all) {
  Tasks tasks(4);
  std::vector<int> hits(100, 0);
  tasks.parallel_for(100, [&](std::size_t i) { hits[i] = 1; });
  int total = 0;
  for (int hit : hits) total += hit;
  ST_CHECK_EQ(static_cast<std::size_t>(total), std::size_t{100});   // 无遗漏
}

ST_TEST(app_tasks_destructor_joins_without_detach) {
  std::atomic<bool> ran{false};
  {
    Tasks tasks(2);
    (void)tasks.submit([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      ran.store(true);
    });
    (void)tasks.run_async([](std::stop_token) { return 1; });
  }   // 析构：池任务排空、独立线程 join
  ST_CHECK(ran.load());
}

ST_TEST(app_tasks_exception_forwarded_via_future) {
  Tasks tasks(1);
  auto future = tasks.submit([]() -> int {
    throw std::runtime_error("boom");   // lint-allow: L5 单行 lambda 里 throw 难以展开成多行（用例本身就是一行抛）
  });
  bool caught = false;
  try {
    (void)future.get();
  } catch (const std::runtime_error& error) {
    caught = std::string(error.what()) == "boom";
  }
  ST_CHECK(caught);   // 池线程不死、异常进 future（契约）
}

ST_TEST(app_tasks_post_ui_from_async_thread) {
  Tasks tasks(1);
  std::atomic<int> landed{0};
  auto [future, handle] = tasks.run_async([&](std::stop_token) {
    // 独立线程也能投 UI 队列（跨线程收尾的唯一合法路径）
    tasks.post_ui([&] { landed.fetch_add(1); });
    return 0;
  });
  future.get();
  handle.wait();
  ST_CHECK(pump_until(tasks, landed, 1));
  ST_CHECK_EQ(landed.load(), 1);
}

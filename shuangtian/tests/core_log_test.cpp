/// 日志订阅的生命周期：**订阅者死了，回调必须跟着消失**。
///
/// 背景（真实缺陷，2026-10-07）：`log::add_listener` 原先**没有注销入口**，
/// 而监听器列表是**进程级全局**的。`Application` 构造时注册了一个抓 `this` 的回调，
/// 析构却不摘——于是**任何**临时 `Application` 都会留下一个悬垂监听器：
/// 对象死了、回调还在，之后**任何线程**打一条日志就踩已释放的 `this`。
///
/// 实测症状：`st_tests --shard 1/4` 稳定 SIGSEGV（0xC0000005），栈为
/// `st::log::write → Application 的 lambda → impl_->log_lines.push_back`，
/// 触发者是 `st::pkg::build` 的编译 worker（`compile_units` 里那条
/// “并行编译 N 路”的 info），调用链上还有 `tests/pkg_check_test.cpp` 的
/// `check_only_writes_no_artifacts`——它在测试进程里真调 `st::pkg::build`。
/// 这解释了“崩的是哪片会漂”：哪一片先构造过 `Application`，哪一片就带着悬垂监听器。
///
/// 两条断言（都做过逆向验证）：
/// ① 注销后不再收到消息；
/// ② **在另一个线程**发日志也收不到（缺陷正是跨线程踩出来的）。

#include "st/test/test.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "st/core/log.hpp"

namespace {

/// 把全局日志级别临时调低以便产生消息，离开作用域复原。
class LogLevelScope {
 public:
  explicit LogLevelScope(st::log::Level level) : previous_(st::log::level()) {
    st::log::set_level(level);
  }
  ~LogLevelScope() { st::log::set_level(previous_); }
  LogLevelScope(const LogLevelScope&) = delete;
  auto operator=(const LogLevelScope&) -> LogLevelScope& = delete;

 private:
  st::log::Level previous_;
};

}  // namespace

ST_TEST(log_listener_removed_after_remove_listener) {
  LogLevelScope scope(st::log::Level::Info);
  std::atomic<int> received{0};
  const std::uint64_t id =
      st::log::add_listener([&received](st::log::Level, std::string_view) { ++received; });

  st::log::info("订阅期间的消息");
  const int during = received.load();
  // 前提：订阅确实生效（否则这条用例什么都验证不到）。
  ST_REQUIRE(during >= 1);

  st::log::remove_listener(id);
  st::log::info("注销之后的消息");
  ST_CHECK_EQ(received.load(), during);
}

ST_TEST(log_listener_removed_before_other_thread_logs) {
  // 这条是缺陷的直接形态：订阅者（`Application` 那种临时对象）先“死”，
  // 再由**别的线程**打日志。若注销没有生效，这里就会踩到已释放的对象
  // （在真实布局上表现为 SIGSEGV；测试里至少能看到“回调仍被调用”）。
  LogLevelScope scope(st::log::Level::Info);
  auto flag = std::make_shared<std::atomic<bool>>(false);
  const std::uint64_t id =
      st::log::add_listener([flag](st::log::Level, std::string_view) { flag->store(true); });

  st::log::info("第一次：订阅者还活着");
  ST_REQUIRE(flag->load());
  flag->store(false);

  st::log::remove_listener(id);   // 等价于“订阅者析构”

  std::thread worker([] { st::log::info("来自另一个线程的日志"); });
  worker.join();
  ST_CHECK(!flag->load());
}

ST_TEST(log_remove_listener_is_idempotent_and_survives_reentrant_removal) {
  LogLevelScope scope(st::log::Level::Info);
  // ① 重复注销同一 id 是无害空操作（析构路径可能被走两次而不该出事）。
  std::atomic<int> received{0};
  const std::uint64_t id =
      st::log::add_listener([&received](st::log::Level, std::string_view) { ++received; });
  st::log::remove_listener(id);
  st::log::remove_listener(id);
  st::log::info("重复注销之后");
  ST_CHECK_EQ(received.load(), 0);

  // ② **回调里自行退订**：`write` 必须对订阅表做快照，否则边遍历边擦除
  //    （拿着引用遍历被 erase 过的 vector）就是 UB。这里钉住它不崩。
  std::uint64_t self = 0;
  std::atomic<int> hits{0};
  self = st::log::add_listener([&self, &hits](st::log::Level, std::string_view) {
    ++hits;
    st::log::remove_listener(self);
  });
  st::log::info("触发一次自退订");
  st::log::info("再触发一次（此时已退订）");
  ST_CHECK_EQ(hits.load(), 1);
}

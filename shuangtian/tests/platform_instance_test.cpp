/// 单实例守门：**同名第二次 acquire 必须失败**、**激活信号必须送达**。
///
/// ## 为什么必须用真进程/真句柄，而不是桩
///
/// 这段代码的全部价值在"内核对象语义"（同名即同一份、进程死则释放）。
/// 用桩把 `acquire` 换成返回固定值，测的就只是"if 分支写得对不对"，
/// 真实的"两个进程能不能都拿到锁"永远测不到——而那才是会伤到用户的错。
///
/// 所以这里：
/// - 同一进程内两次 `acquire` 同名 → 第二次必须 `Busy`（内核对象是进程无关的，
///   同进程同名同样互斥）；
/// - 不同名 → 都能拿到（不能"一律只许一个"）；
/// - `notify_existing` 的计数能被 `take_activate_requests` 取走且**取后归零**
///   （否则主循环每帧都会恢复窗口一次，用户会看到窗口反复抢焦点）。
#include "st/test/test.hpp"

#include <chrono>
#include <string>
#include <thread>

#include "st/platform/instance.hpp"

namespace {

using st::ErrorCode;
using st::platform::SingleInstance;

/// 用例间唯一的 key 后缀计数。
///
/// 放在命名空间作用域（不用函数内 `static`）：本仓 lint L8 禁止函数内静态可变状态，
/// 且语义上也一致——它就是该 TU 的单一递增值。
int g_key_counter = 0;  // lint-allow: L8 用例级计数器：作用域限于本翻译单元（不跨用例泄漏），与 tests/ui_dsl_test.cpp 的用例计数器同类

/// 每次用不同的 key：用例之间不能互相干扰（前一个用例的锁若未释放，
/// 后一个会莫名其妙 `Busy`——这类串扰最费时间）。
auto unique_key(const char* suffix) -> std::string {
  ++g_key_counter;
  return std::string("st-test-") + suffix + "-" + std::to_string(g_key_counter);
}

}  // namespace

/// 第一个拿到，第二个必须 `Busy`。
ST_TEST(single_instance_second_acquire_reports_busy) {
  const std::string key = unique_key("busy");
  auto first = SingleInstance::acquire(key);
  ST_REQUIRE(first.has_value());
  ST_CHECK(first->valid());

  auto second = SingleInstance::acquire(key);
  ST_REQUIRE(!second.has_value());
  ST_CHECK(second.error().code == ErrorCode::Busy);

  // 第一个仍然有效（失败的那个不能把前一个搞坏）
  ST_CHECK(first->valid());
}

/// 不同 key 互不影响（否则"只允许一个应用"就退化成"只允许一个进程"）。
ST_TEST(single_instance_distinct_keys_do_not_collide) {
  auto a = SingleInstance::acquire(unique_key("keya"));
  auto b = SingleInstance::acquire(unique_key("keyb"));
  ST_REQUIRE(a.has_value());
  ST_REQUIRE(b.has_value());
  ST_CHECK(a->valid());
  ST_CHECK(b->valid());
}

/// 锁释放后**可以再拿到**（"正常退出后立刻再启动"必须能起来）。
ST_TEST(single_instance_lock_is_released_on_destroy) {
  const std::string key = unique_key("release");
  {
    auto held = SingleInstance::acquire(key);
    ST_REQUIRE(held.has_value());
  }
  auto again = SingleInstance::acquire(key);
  ST_REQUIRE(again.has_value());
  ST_CHECK(again->valid());
}

/// 激活信号：投递 → 被等到的计数取走 → 取后归零。
ST_TEST(single_instance_activate_signal_reaches_holder) {
  const std::string key = unique_key("activate");
  auto holder = SingleInstance::acquire(key);
  ST_REQUIRE(holder.has_value());

  // 等待线程是 detached 的、启动有一小段延迟；先给一点时间，避免"信号发出时
  // 线程还没进 WaitForSingleObject"而把这条用例判成不稳定。
  std::this_thread::sleep_for(std::chrono::milliseconds(60));

  ST_CHECK(SingleInstance::notify_existing(key));

  // 轮询等计数（信号跨线程，不能假设"下一行就到"）
  std::uint32_t observed = 0;
  for (int attempt = 0; attempt < 100 && observed == 0; ++attempt) {
    observed = holder->take_activate_requests();
    if (observed == 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ST_CHECK(observed >= 1U);
  // **取后归零**：主循环每帧调用，不能每帧都报告"有请求"
  ST_CHECK_EQ(holder->take_activate_requests(), 0U);
}

/// 没有实例时投递**如实失败**（不能假装成功——那会让第二个进程以为自己叫醒了窗口）。
ST_TEST(single_instance_notify_without_holder_returns_false) {
  const std::string key = unique_key("nobody");
  ST_CHECK(!SingleInstance::notify_existing(key));
}

/// 默认构造的实例**无效**（没拿过锁就不能当守门用）。
ST_TEST(single_instance_default_is_invalid) {
  SingleInstance empty;
  ST_CHECK(!empty.valid());
  // 无效实例取计数不应崩，且恒为 0
  ST_CHECK_EQ(empty.take_activate_requests(), 0U);
}

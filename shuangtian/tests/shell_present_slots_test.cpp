/// 上屏槽环（`st::shell::PresentSlotRing`）与降级判据的行为契约。
///
/// 这三条判定的错法都不会崩、也不会让截图变红，而是"画面缺一块/停在旧帧/偶发撕裂"——
/// 只有真 X 服务器 + 局部上屏才复现（本机无头跑一万遍也看不出来）。因此把判定抽成
/// 纯逻辑并在这里钉住；真实服务器那一半由 `tools/x11_win_check.py` 与
/// `tools/present_bench.py` 覆盖。
///
/// ⚠ 每条断言都做过**逆向验证**（`tools/present_reverse_verify.py`）：
/// 把实现回退成缺陷版，这里必须变红。

#include "st/test/test.hpp"

#include "st/math/geometry.hpp"
#include "st/shell/present_slots.hpp"

namespace {

using st::shell::decide_after_sync_wait;
using st::shell::kPresentSlotLimit;
using st::shell::PresentSlotRing;
using st::shell::SyncVerdict;

}  // namespace

ST_TEST(present_slot_ring_alternates_between_buffers) {
  // 两块槽的**全部意义**就是交替：写一块、写另一块、再回到第一块。
  // 退回单块时（比如选槽总返回 0）表现是"每帧都在等上一次的完成事件"——
  // 不崩、不错，只是把双缓冲的收益原样丢回去，正是最容易被放过的那类缺陷。
  PresentSlotRing ring{kPresentSlotLimit};
  ST_CHECK_EQ(ring.slot_count(), 2);

  const int first = ring.pick();
  ST_CHECK_EQ(first, 0);  // 初始没有 active：从 0 起
  ring.commit(first);
  ring.mark_in_flight(first);

  const int second = ring.pick();
  ST_CHECK_EQ(second, 1);  // 0 在途 ⇒ 必须换另一块（不是等 0 回来）
  ring.commit(second);
  ring.mark_in_flight(second);

  // 两块都在途：没有可写的槽（调用方必须先排空/降级，**不能**硬写）
  ST_CHECK_EQ(ring.pick(), -1);
  ST_CHECK_EQ(ring.in_flight_count(), 2);

  // 第一块被服务端释放后，轮转回到它（0 是第二块的下一个）
  ring.mark_idle(first);
  ST_CHECK_EQ(ring.in_flight_count(), 1);
  ST_CHECK_EQ(ring.pick(), 0);  // (1 + 1) % 2 == 0
  ring.commit(0);
  ring.mark_in_flight(0);
  ring.mark_idle(second);
  ST_CHECK_EQ(ring.pick(), 1);
}

ST_TEST(present_slot_ring_single_buffer_blocks_without_completion) {
  // 降级为单块之后，**没有第二块可退**：在途期间必须如实报"无槽可用"，
  // 让调用方去排空（而不是悄悄覆写服务端正读的那块 = 撕裂）。
  PresentSlotRing ring{1};
  ST_CHECK_EQ(ring.slot_count(), 1);
  ST_CHECK_EQ(ring.pick(), 0);
  ring.commit(0);
  ring.mark_in_flight(0);
  ST_CHECK_EQ(ring.pick(), -1);
  ring.mark_all_idle();
  ST_CHECK_EQ(ring.pick(), 0);
  ST_CHECK_EQ(ring.in_flight_count(), 0);
}

ST_TEST(present_slot_ring_mark_idle_is_idempotent) {
  // 完成事件重复到达（或 `XSync` 已统一放开标记后再来一枚）不得把计数减成负数：
  // 负数会让"还有段在被读"的判据永远为假 → 无保护覆写（撕裂）。
  PresentSlotRing ring{2};
  ring.mark_in_flight(0);
  ring.mark_idle(0);
  ST_CHECK_EQ(ring.in_flight_count(), 0);
  ring.mark_idle(0);  // 重复
  ring.mark_idle(0);
  ST_CHECK_EQ(ring.in_flight_count(), 0);
  // 计数没被减坏：两块都空闲，照常交替（先写 0，在途期间换 1）
  const int first = ring.pick();
  ST_CHECK_EQ(first, 0);
  ring.commit(first);
  ring.mark_in_flight(first);
  ST_CHECK_EQ(ring.pick(), 1);
  ST_CHECK_EQ(ring.in_flight_count(), 1);
}

ST_TEST(present_slot_ring_degrade_to_single_keeps_one_slot) {
  PresentSlotRing ring{2};
  ring.mark_in_flight(0);
  ring.mark_in_flight(1);
  ring.degrade_to_single();
  ST_CHECK_EQ(ring.slot_count(), 1);
  ST_CHECK_EQ(ring.in_flight_count(), 0);
  ST_CHECK_EQ(ring.pick(), 0);  // 单块只有 0
}

ST_TEST(present_sync_verdict_degrades_only_when_never_seen) {
  // 判据的两半都要钉住（少钉一半就会交付"恒降级"或"永不降级"的假护栏）：
  // ① **从未见过**完成事件（超时也没收到）⇒ 这环境不回报它 ⇒ 必须降级；
  // ② 见过（哪怕这次没收到）⇒ 只是慢，继续用双缓冲。
  ST_CHECK(decide_after_sync_wait(0, false) == SyncVerdict::Degrade);
  ST_CHECK(decide_after_sync_wait(1, false) == SyncVerdict::Continue);
  ST_CHECK(decide_after_sync_wait(7, false) == SyncVerdict::Continue);
  // 这次收到了就没什么可降的（哪怕之前一个都没见过：第一次等待就等到了）
  ST_CHECK(decide_after_sync_wait(0, true) == SyncVerdict::Continue);
  ST_CHECK(decide_after_sync_wait(9, true) == SyncVerdict::Continue);
}

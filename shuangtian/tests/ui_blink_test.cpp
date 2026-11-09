#include <cmath>
#include <string>

#include "st/test/test.hpp"
#include "st/ui/blink.hpp"

namespace {

/// 假的宿主元素：只记录 `request_wake_at` 被调了几次、最后一次是哪个时刻。
/// 这样能直接断言“闪烁协议**登记了**唤醒点”这个契约——
/// 漏登记就是光标冻住那个缺陷，而它不会有任何像素差异。
struct FakeOwner {
  mutable int wake_calls{0};
  mutable double last_wake{-1.0};
  void request_wake_at(double t) const noexcept {
    ++wake_calls;
    last_wake = t;
  }
};

}  // namespace

ST_TEST(blink_tick_always_registers_wake) {
  // 契约（缺失就是“光标一直亮着但不闪”）：**只要 tick 被调用，就必须登记下一翻转点**。
  // 静止时帧循环已收敛，没有唤醒点就永远拿不到下一帧。
  st::ui::BlinkCursor blink;
  FakeOwner owner;
  const st::ui::BlinkState state = blink.tick(0.0, owner);
  ST_CHECK_EQ(owner.wake_calls, 1);
  // 0.0 处相位为 0（可见期起点）⇒ 下一翻转点应是“可见期结束”那个时刻。
  ST_CHECK(std::abs(owner.last_wake - 0.5) < 1e-9);
  ST_CHECK(state.visible);
  // 再 tick 一次（同一时刻）仍然要登记（调用方可能在任意帧调用，不能假设顺序）。
  (void)blink.tick(0.0, owner);
  ST_CHECK_EQ(owner.wake_calls, 2);
}

ST_TEST(blink_phase_matches_definition) {
  // 相位口径：period 1.0s、duty 0.5 ⇒ [0,0.5) 可见、[0.5,1.0) 不可见。
  st::ui::BlinkCursor blink;
  FakeOwner owner;
  ST_CHECK(blink.tick(0.0, owner).visible);
  ST_CHECK(blink.tick(0.25, owner).visible);
  ST_CHECK(!blink.tick(0.5, owner).visible);
  ST_CHECK(!blink.tick(0.75, owner).visible);
  ST_CHECK(blink.tick(1.0, owner).visible);  // 回到起点
  ST_CHECK(blink.tick(1.25, owner).visible);
}

ST_TEST(blink_reports_damage_only_on_flip) {
  // 只在**相位真翻转**那一帧上报损坏区：每帧报会与帧间损坏判据互相触发成续帧循环。
  st::ui::BlinkCursor blink;
  FakeOwner owner;
  // 首次 tick：上次相位未知 ⇒ 判为翻转（必须上报，否则光标重新出现时画不出来）。
  ST_CHECK(blink.tick(0.0, owner).needs_damage());
  // 同相位内：不上报。
  ST_CHECK(!blink.tick(0.1, owner).needs_damage());
  ST_CHECK(!blink.tick(0.2, owner).needs_damage());
  // 跨到不可见相位：上报一次。
  ST_CHECK(blink.tick(0.5, owner).needs_damage());
  ST_CHECK(!blink.tick(0.6, owner).needs_damage());
  // 跨回可见：再上报一次。
  ST_CHECK(blink.tick(1.0, owner).needs_damage());
}

ST_TEST(blink_next_flip_never_stalls) {
  // 唤醒点必须**严格向前**：若算出来等于当前时刻，帧循环会原地空转成 60fps。
  st::ui::BlinkCursor blink;
  FakeOwner owner;
  for (int i = 0; i < 500; ++i) {
    const double now = static_cast<double>(i) * 0.037;  // 非整除的步长
    (void)blink.tick(now, owner);
    ST_CHECK(owner.last_wake > now);
    ST_CHECK(owner.last_wake - now <= 1.0 + 1e-9);
  }
}

ST_TEST(blink_reset_makes_next_tick_report_damage) {
  // 失焦再聚焦：必须重新上报一次损坏区（光标要立刻画出来，不能等下一个整秒）。
  st::ui::BlinkCursor blink;
  FakeOwner owner;
  (void)blink.tick(0.0, owner);
  (void)blink.tick(0.1, owner);
  ST_CHECK_EQ(blink.last_visible(), 1);
  blink.reset();
  ST_CHECK_EQ(blink.last_visible(), -1);
  const st::ui::BlinkState after = blink.tick(0.2, owner);
  ST_CHECK(after.needs_damage());
  ST_CHECK(after.visible);
}

ST_TEST(blink_custom_duty_and_period) {
  st::ui::BlinkCursor blink{st::ui::BlinkSpec{.period_seconds = 2.0, .duty = 0.25}};
  FakeOwner owner;
  // 2.0s 周期、25% 可见 ⇒ [0,0.5) 可见。
  ST_CHECK(blink.tick(0.0, owner).visible);
  ST_CHECK(blink.tick(0.4, owner).visible);
  ST_CHECK(!blink.tick(0.5, owner).visible);
  ST_CHECK(!blink.tick(1.9, owner).visible);
  ST_CHECK(blink.tick(2.0, owner).visible);
  // 可见期结束时唤醒点 = 0.5。
  (void)blink.tick(0.0, owner);
  ST_CHECK(std::abs(owner.last_wake - 0.5) < 1e-9);
}

/// `st::ui::InputCoalescer` 高频指针移动的帧内合并测试。
///
/// 这一层的价值是**三条可量化的收益**（见头文件）：重绘次数、笔迹质量、撤销粒度。
/// 前两条不好在单测里直接量，但"一帧 N 次移动 → 1 次派发"这个**倍率**可以精确断言，
/// 而它正是那三条的共同来源。所以本文件的中心是倍率与"首尾都保留"。

#include <cstddef>
#include <memory>

#include "st/test/test.hpp"
#include "st/ui/input_coalescer.hpp"

namespace {

using st::math::Point;
using st::ui::InputCoalescer;

}  // namespace

ST_TEST(input_coalescer_merges_frame_moves_into_one) {
  InputCoalescer coalescer;
  ST_CHECK(!coalescer.pending());
  ST_CHECK(!coalescer.take().has_value());   // 没移动就不派发

  // 模拟一次 1000Hz 鼠标拖过 8ms 的一帧：8 次移动。
  for (int step = 0; step < 8; ++step) {
    coalescer.feed(Point{static_cast<float>(step) * 3.0f, 10.0f});
  }
  ST_CHECK(coalescer.pending());
  ST_CHECK_EQ(coalescer.merged_count(), std::size_t{7});   // 8 次里合并掉 7 次

  const auto move = coalescer.take();
  ST_REQUIRE(move.has_value());
  ST_CHECK_EQ(move->merged, std::size_t{7});
  // **首尾都要保留**：只给最新位置会丢掉本帧走过的整段路（快速划动时笔迹断成一段段）。
  ST_CHECK(move->earliest.x == 0.0f);
  ST_CHECK(move->latest.x == 21.0f);
  ST_CHECK(move->moved());
  // 取走后清空：下一帧不会重复派发同一批。
  ST_CHECK(!coalescer.pending());
  ST_CHECK(!coalescer.take().has_value());
  ST_CHECK_EQ(coalescer.merged_count(), std::size_t{0});
}

ST_TEST(input_coalescer_single_move_is_not_a_merge) {
  InputCoalescer coalescer;
  coalescer.feed(Point{5.0f, 6.0f});
  const auto move = coalescer.take();
  ST_REQUIRE(move.has_value());
  ST_CHECK_EQ(move->merged, std::size_t{0});   // 只有一次，不算合并
  ST_CHECK(move->earliest.x == 5.0f && move->earliest.y == 6.0f);
  ST_CHECK(move->latest.x == 5.0f && move->latest.y == 6.0f);
  // 原地不动的一次"移动"：`moved()` 为假，调用方据此避免无谓重绘。
  ST_CHECK(!move->moved());
}

ST_TEST(input_coalescer_keeps_latest_modifiers) {
  // 修饰键保留**最新**的：按住 Ctrl 是"当前状态"，保留最早那次会表现为
  // "我按了 Ctrl 但它没生效"（按下时刻比最后一次移动早）。
  InputCoalescer coalescer;
  coalescer.feed(Point{1.0f, 1.0f}, /*ctrl=*/false, /*shift=*/false);
  coalescer.feed(Point{2.0f, 2.0f}, /*ctrl=*/true, /*shift=*/false);
  coalescer.feed(Point{3.0f, 3.0f}, /*ctrl=*/true, /*shift=*/true);
  const auto move = coalescer.take();
  ST_REQUIRE(move.has_value());
  ST_CHECK(move->ctrl);
  ST_CHECK(move->shift);
  ST_CHECK(!move->alt);
  ST_CHECK(!move->meta);
}

ST_TEST(input_coalescer_reports_leave) {
  InputCoalescer coalescer;
  coalescer.feed_leave();
  // 只有离开、没有移动：仍要给出结果（调用方据此发 `HoverOut`），
  // 但 `moved()` 为假——不能被当成"指针动了"。
  const auto move = coalescer.take();
  ST_REQUIRE(move.has_value());
  ST_CHECK(move->left);
  ST_CHECK(!move->moved());
  ST_CHECK_EQ(move->merged, std::size_t{0});
  ST_CHECK(!coalescer.pending());
}

ST_TEST(input_coalescer_reset_drops_pending) {
  // 窗口失活/焦点丢失：那一帧的移动不该再派发（否则会"突然跳到某处"）。
  InputCoalescer coalescer;
  coalescer.feed(Point{9.0f, 9.0f});
  coalescer.feed(Point{10.0f, 10.0f});
  coalescer.reset();
  ST_CHECK(!coalescer.pending());
  ST_CHECK(!coalescer.take().has_value());
  ST_CHECK_EQ(coalescer.merged_count(), std::size_t{0});
}

ST_TEST(input_coalescer_quantifies_the_saving) {
  // 把收益**量出来**（头文件里那三条的公共来源）。
  //
  // 一次 2 秒的长笔画：普通鼠标 125Hz + 60fps 渲染 ⇒ 250 次移动 / 120 帧。
  // 不合并 = 250 次派发（且 250 次改文档 + 推撤销命令）；
  // 合并后 = 至多 120 次派发，而实际"每帧有移动"的帧约 120 帧。
  InputCoalescer coalescer;
  std::size_t dispatched = 0;
  std::size_t fed = 0;
  constexpr int kFrames = 120;
  constexpr int kMovesPerFrame = 2;   // 125Hz / 60fps ≈ 2
  for (int frame = 0; frame < kFrames; ++frame) {
    for (int step = 0; step < kMovesPerFrame; ++step) {
      coalescer.feed(Point{static_cast<float>(fed) * 1.0f, 0.0f});
      ++fed;
    }
    if (coalescer.take().has_value()) ++dispatched;
  }
  ST_CHECK_EQ(fed, std::size_t{240});
  ST_CHECK_EQ(dispatched, std::size_t{120});
  ST_CHECK(dispatched < fed);
  // 合并倍率 = 2（这正是"撤销一次退 1px → 退一帧"的来源）。
  ST_CHECK_EQ(fed / dispatched, std::size_t{2});
}

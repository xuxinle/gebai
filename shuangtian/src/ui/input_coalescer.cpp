#include "st/ui/input_coalescer.hpp"

namespace st::ui {

void InputCoalescer::feed(math::Point position, bool ctrl, bool shift, bool alt, bool meta) {
  if (!has_any_) {
    // 本帧第一次移动：earliest 与 latest 都从这里起。
    earliest_ = position;
    has_any_ = true;
  } else {
    ++merged_;
  }
  latest_ = position;
  // 修饰键只保留**最新**的：按住 Ctrl 是"当前状态"，而不是某个历史瞬间的状态。
  // 若保留最早那次，会出现"我按了 Ctrl 但它没生效"（按下时刻比最后一次移动早）。
  ctrl_ = ctrl;
  shift_ = shift;
  alt_ = alt;
  meta_ = meta;
}

void InputCoalescer::feed_leave() { left_ = true; }

auto InputCoalescer::take() -> std::optional<CoalescedMove> {
  if (!has_any_ && !left_) return std::nullopt;
  CoalescedMove out;
  out.earliest = earliest_;
  out.latest = latest_;
  out.merged = merged_;
  out.left = left_;
  out.ctrl = ctrl_;
  out.shift = shift_;
  out.alt = alt_;
  out.meta = meta_;
  // `left_` 单独存在（指针离开但本帧没有 move）时：`earliest`/`latest` 是上帧的值，
  // 而调用方靠 `left` 判断"该发 MouseLeave 了"——那种情况下不该把它当成"移动了"。
  reset();
  return out;
}

void InputCoalescer::reset() noexcept {
  has_any_ = false;
  left_ = false;
  merged_ = 0;
  earliest_ = {};
  latest_ = {};
  ctrl_ = false;
  shift_ = false;
  alt_ = false;
  meta_ = false;
}

}  // namespace st::ui

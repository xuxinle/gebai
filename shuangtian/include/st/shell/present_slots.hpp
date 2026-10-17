#pragma once

/// 上屏缓冲槽的**状态机**（与平台无关）。
///
/// ## 它解决什么问题
///
/// 窗口后端的"上屏"是把像素**交出去**。X11 的 MIT-SHM 让服务端直接读客户端的共享段，
/// 因此"客户端下一帧覆写该段"与"服务端正在读它"会撞在一起（症状是撕裂：画面上下半
/// 来自不同帧）。单段的解法是每帧等一次往返（`XSync`），代价见 `DESIGN.md` §4.6.2。
///
/// 多段交替 + 完成事件能把这次等待去掉。判定只有三条，但**错一条的后果都很隐蔽**
/// （"画面缺一块""停在旧帧""偶发撕裂"），且只有真 X 服务器才复现——所以判定抽在这里，
/// 用纯逻辑钉住：
///
/// 1. **选哪块**：轮转的下一个；它还在途则退而选任一空闲块（比等它更快且同样正确）。
/// 2. **可能覆写一块吗**：只有在收到它的完成事件（服务端已读完）之后。
/// 3. **什么时候必须放弃双缓冲**：从来没收到过完成事件 = 这个环境不回报它，
///    继续交替就是无保护覆写（撕裂）→ 如实降级为单段 + 每帧排空。
///
/// ## 一个**不**需要的条件（避免为它白付代价）
///
/// 初版设计里每块槽都维护"自上次写入以来变过的区域"，复用前先补齐，理由是
/// "交替提交时整屏复用会把两帧前的像素刷上去"。对 X11 这个理由是**不成立**的：
/// `XShmPutImage` 的语义是服务端在**处理该请求时**把段内容拷进自己的可绘制区，
/// 完成事件正是"数据已被读走"的回执（`shmstr.h` 原文：*after the image data has been
/// read from the shared memory segment*）；此后服务端不再读该段。而屏幕上看到的是
/// "历次送出去的区域叠加"，我们每帧送的恰好就是本帧写入的区域——所以槽里区域之外的
/// 旧像素**永远不会被送到服务端**。补齐机制在整屏重绘时多写一整块画布、在局部上屏时
/// 每次多写上一次的损坏区（正是局部上屏要省掉的那份开销），纯属自伤。
///
/// （Wayland 后端相反：`wl_buffer` 会被合成器**持续持有**，每块缓冲都必须是完整帧——
/// 那是 `platform_wayland.cpp` 的 `commit_frame` 注释记录的另一个事实。）

#include <array>

#include "st/math/geometry.hpp"

namespace st::shell {

/// 槽数上限：两块即可实现交替（更多块只是延长"允许在途的帧数"，收益递减而内存翻倍）。
inline constexpr int kPresentSlotLimit{2};

/// 在途槽被复用时，排空等待之后的处置。
enum class SyncVerdict {
  Continue,  ///< 完成事件本来会来（只是慢）——继续用双缓冲
  Degrade,   ///< 一次都没见过完成事件——这个环境不回报它，退化为单缓冲
};

/// 纯函数：排空等待后该继续还是退化。
///
/// 判据为什么是"**累计**是否见过完成事件"而不是"这次见过"：一次等待没等到，
/// 可能只是事件还在管线里；而**从来没收到过**任何一个完成事件，就说明这个
/// 服务端（或这条传输）不回报它——继续用双缓冲就是无保护地覆写在途段（撕裂）。
[[nodiscard]] constexpr auto decide_after_sync_wait(int completions_seen_total,
                                                    bool completion_seen_now) noexcept
    -> SyncVerdict {
  if (completion_seen_now) return SyncVerdict::Continue;
  return completions_seen_total == 0 ? SyncVerdict::Degrade : SyncVerdict::Continue;
}

/// 上屏槽环：选槽 / 记录在途 / 降级。
class PresentSlotRing final {
 public:
  /// @param slot_count 实际可用的槽数（1 = 单缓冲，2 = 双缓冲）
  explicit PresentSlotRing(int slot_count = kPresentSlotLimit) noexcept { reset(slot_count); }

  /// 重建全部状态（换尺寸、降级都走它）：所有槽空闲、在途计数归零。
  void reset(int slot_count) noexcept {
    count_ = slot_count < 0 ? 0 : (slot_count > kPresentSlotLimit ? kPresentSlotLimit : slot_count);
    active_ = -1;
    in_flight_ = 0;
    for (auto& slot : slots_) slot.in_flight = false;
  }

  [[nodiscard]] auto slot_count() const noexcept -> int { return count_; }

  /// 本次要写入的槽；-1 = 所有槽都在途（调用方须排空或退化，**不能**直接写）。
  ///
  /// 优先"轮转的下一个"，它不可用时退而选任一空闲槽：两块槽下这就是"交替"，
  /// 而在某块还没被服务端释放时，用另一块比等它更快且同样正确。
  [[nodiscard]] auto pick() const noexcept -> int {
    if (count_ <= 0) return -1;
    const int preferred = (active_ + 1) % count_;
    if (!slots_[static_cast<std::size_t>(preferred)].in_flight) return preferred;
    for (int index = 0; index < count_; ++index) {
      if (!slots_[static_cast<std::size_t>(index)].in_flight) return index;
    }
    return -1;
  }

  /// 记录"已把本帧送出去的那一次写入落在槽 `slot`"：下一帧从它的下一块起轮转。
  void commit(int slot) noexcept {
    if (slot < 0 || slot >= count_) return;
    active_ = slot;
  }

  void mark_in_flight(int slot) noexcept {
    if (slot < 0 || slot >= count_) return;
    auto& item = slots_[static_cast<std::size_t>(slot)];
    if (item.in_flight) return;  // 重复标记不得把计数加两次
    item.in_flight = true;
    ++in_flight_;
  }

  void mark_idle(int slot) noexcept {
    if (slot < 0 || slot >= count_) return;
    if (!slots_[static_cast<std::size_t>(slot)].in_flight) return;
    slots_[static_cast<std::size_t>(slot)].in_flight = false;
    if (in_flight_ > 0) --in_flight_;
  }

  /// 全部放开在途标记（`XSync` 回来 = 服务端已处理完所有请求，段都不在被读）。
  void mark_all_idle() noexcept {
    for (auto& slot : slots_) slot.in_flight = false;
    in_flight_ = 0;
  }

  [[nodiscard]] auto in_flight(int slot) const noexcept -> bool {
    if (slot < 0 || slot >= count_) return false;
    return slots_[static_cast<std::size_t>(slot)].in_flight;
  }

  /// 在途槽数（0 = 没有段在被服务端读）。
  [[nodiscard]] auto in_flight_count() const noexcept -> int { return in_flight_; }

  /// 最近一次写入的槽（诊断用；-1 = 还没写过）。
  [[nodiscard]] auto active() const noexcept -> int { return active_; }

  /// 退化为单缓冲：只留第一块。
  /// 调用方必须先保证服务端不再读别的段（排空等待之后）。
  void degrade_to_single() noexcept { reset(1); }

 private:
  struct Slot {
    bool in_flight{false};
  };

  std::array<Slot, kPresentSlotLimit> slots_{};
  int count_{0};
  int active_{-1};
  int in_flight_{0};
};

}  // namespace st::shell

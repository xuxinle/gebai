#pragma once

/// 格斗示例 · 电脑对手。
///
/// 设计要点（也是这个示例想演示的一件事）：**AI 与玩家走同一个输入面**——
/// 它每帧产出的是一个 `Intent`，和键盘折算出来的那个结构完全相同。
/// 于是"AI 能做的事玩家都能做"是结构保证；也不会出现"AI 用了玩家没有的
/// 隐藏机制"这种调不平的手感问题。
///
/// 随机源是**自带的确定性 LCG**（不是 `std::rand`）：同一个种子跑出同一场对局，
/// e2e 才能断言"电脑在第 N 帧出了什么"。全局可变状态是被禁的（`CONVENTIONS` R11），
/// 所以状态住在对象里、由调用方持有。

#include <cstdint>

#include "fight.hpp"

namespace gedou {

/// 难度档（决定反应延迟、进攻欲望、防御率）。
enum class Difficulty : std::uint8_t { Easy, Normal, Hard };

[[nodiscard]] auto difficulty_name(Difficulty difficulty) -> const char*;
[[nodiscard]] auto difficulty_from_name(const char* name) -> Difficulty;

class AiController {
 public:
  AiController(std::uint32_t seed = 0x5EED1234U, Difficulty difficulty = Difficulty::Normal);

  void set_difficulty(Difficulty difficulty) { difficulty_ = difficulty; }
  [[nodiscard]] auto difficulty() const noexcept -> Difficulty { return difficulty_; }
  void reseed(std::uint32_t seed) noexcept { rng_state_ = seed | 1U; }
  /// 让 AI 停下（结算态/调试时用）；停下后每帧返回空意图。
  void set_enabled(bool enabled) { enabled_ = enabled; }

  /// 产出**这一帧**的意图。`match` 只读（AI 不许改战斗状态）。
  [[nodiscard]] auto think(const Match& match, int side) -> Intent;

  /// 属性面（调试/断言用）。
  [[nodiscard]] auto last_decision() const -> const char* { return decision_; }

  /// **进攻距离上限（世界像素）**——与 `think()` 内部用的是**同一个口径**。
  ///
  /// 为何把它暴露出来：单位类错误（局部 vs 世界）不会让 AI 失效，只让它
  /// “出手距离偏小 40%”，从画面上看不出来。而单测只要把这个值与
  /// “招式表触及距离 + 半身体宽”直接对拍，就能把单位错钉死——
  /// 前提是这个值来自 `think()` 的同一份计算，而不是测试自己重算一遗。
  [[nodiscard]] auto aggression_range() const noexcept -> float;

 private:
  [[nodiscard]] auto next_random() noexcept -> float;   ///< [0,1)
  [[nodiscard]] auto chance(float probability) noexcept -> bool;

  std::uint32_t rng_state_{1U};
  Difficulty difficulty_{Difficulty::Normal};
  bool enabled_{true};
  /// 距离上次"重新决策"的帧数：AI 不是每帧都变主意（那是人类做不到的操作量）。
  int reaction_timer_{0};
  /// 当前决策的持续帧数（一个进攻动作要按几帧）。
  int hold_timer_{0};
  Intent committed_{};
  const char* decision_{"idle"};
};

}  // namespace gedou

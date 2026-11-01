#include "ai.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace gedou {

namespace {

/// 双方脚下距离（世界像素）。
///
/// ⚠ 它必须与招式表的 `reach_*` 在**同一个单位空间**里比较：招式表是局部单位，
/// 世界距离 = 局部 × `kBodyScale`。忘了乘的后果是 AI 的“出手距离”比真实触及距离
/// **短近 40%**：它会一直贴到几乎重合才出手，看起来像“反应迟钟、不会走位”，
/// 而从现象无法反推是单位问题。
///
/// 注：它**不会**让 AI 永不命中（继续靠近总能打到），所以“AI 打得到人”那类
/// 一般性断言抓不住这个错——必须把距离**钉死**再验（见 `gedou_ai_has_no_unit_mismatch`）。
[[nodiscard]] auto gap_between(const Match& match, int side) -> float {
  const FighterState& mine = match.state(side);
  const FighterState& other = match.state(1 - side);
  return std::abs(other.x - mine.x);
}

/// 招式表里的距离→世界像素。
[[nodiscard]] constexpr auto world_reach(float local) noexcept -> float {
  return local * kBodyScale;
}

/// 各难度的行为参数。
struct Tuning {
  int reaction_frames;   ///< 多久重新决策一次（越小越"神反应"）
  float aggression;      ///< 进攻欲望（每次决策攻击的概率）
  float guard_rate;      ///< 看到对手出招时选择防御的概率
  float special_rate;    ///< 有槽时优先放必杀的倾向
};

[[nodiscard]] auto tuning_for(Difficulty difficulty) -> Tuning {
  switch (difficulty) {
    // 三档的差别在"反应速度 + 进攻欲望"，而**抢招能力**决定强弱档位：
    // 反应帧 > 轻拳起手（14）时抢不了招（只能被动防），< 14 才能反打。
    // 实测（对同档自己）：easy/easy 50%，normal/normal ~30%，hard/hard ~50%。
    case Difficulty::Easy: return Tuning{30, 0.30f, 0.15f, 0.25f};
    case Difficulty::Normal: return Tuning{13, 0.55f, 0.40f, 0.50f};
    case Difficulty::Hard: return Tuning{6, 0.72f, 0.60f, 0.70f};
  }
  return Tuning{14, 0.6f, 0.45f, 0.5f};
}

}  // namespace

auto difficulty_name(Difficulty difficulty) -> const char* {
  switch (difficulty) {
    case Difficulty::Easy: return "easy";
    case Difficulty::Normal: return "normal";
    case Difficulty::Hard: return "hard";
  }
  return "normal";
}

auto difficulty_from_name(const char* name) -> Difficulty {
  const std::string_view value{name != nullptr ? name : ""};
  if (value == "easy") return Difficulty::Easy;
  if (value == "hard") return Difficulty::Hard;
  return Difficulty::Normal;
}

AiController::AiController(std::uint32_t seed, Difficulty difficulty)
    : rng_state_(seed | 1U), difficulty_(difficulty) {}

auto AiController::next_random() noexcept -> float {
  // 线性同余（Numerical Recipes 常数）：确定性、够乱、无分配。
  rng_state_ = rng_state_ * 1664525U + 1013904223U;
  return static_cast<float>(rng_state_ >> 8U) / 16777216.0f;
}

auto AiController::chance(float probability) noexcept -> bool {
  return next_random() < probability;
}

auto AiController::think(const Match& match, int side) -> Intent {
  Intent intent{};
  if (!enabled_ || match.phase() != RoundPhase::Fight) {
    decision_ = "off";
    return intent;
  }
  const FighterState& mine = match.state(side);
  const FighterState& other = match.state(1 - side);
  if (!mine.can_act()) {
    decision_ = "busy";
    return intent;
  }
  const Tuning tuning = tuning_for(difficulty_);

  // ① 处理"已承诺的动作"：一旦决定进攻就按完，中途乱改会让 AI 看起来抽风。
  //
  // 但**对手起手时不能硬撑**：那会把 AI 锁在上一轮决策里、永远慢一步——
  // 这正是之前被“无脑连招”压着打的直接原因。
  const bool danger = is_attack(other.move) && other.move_frame < other.timing.startup;
  if (hold_timer_ > 0 && !danger) {
    --hold_timer_;
    return committed_;
  }

  // ② 反应延迟：两次决策之间保持同一状态（人不会每帧换主意）。
  // ①b **起手段不受反应延迟约束**：
  //
  // 反应延迟是为了“人不会每帧换主意”，但起手/受击这种**必须立即响应**的
  // 事件不能被它拖住——否则看到对手出招时还在计时，等到能决策时已经吃了。
  // 实测代价：反应帧 14（= 轻拳起手时长）时，抢招逻辑形同不存在，
  // 手写 AI 对“无脑按重拳”胜率 0%。
  const Tuning quick = tuning_for(difficulty_);
  const bool urgent = is_attack(other.move) &&
                      other.move_frame <= other.timing.startup + other.timing.active &&
                      gap_between(match, side) < world_reach(attack_spec(MoveId::Heavy).reach_far) +
                                                  30.0f * kBodyScale;
  if (reaction_timer_ > 0 && !urgent) {
    --reaction_timer_;
    return committed_;
  }
  reaction_timer_ = urgent ? 1 : quick.reaction_frames;

  // 距离与招式表**必须先换到同一个单位**（见 `world_reach` 的说明）。
  const float gap = gap_between(match, side);
  const float light_far = world_reach(attack_spec(MoveId::Light).reach_far);
  const float light_near = world_reach(attack_spec(MoveId::Light).reach_near);
  const float heavy_far = world_reach(attack_spec(MoveId::Heavy).reach_far);
  const float kick_far = world_reach(attack_spec(MoveId::Kick).reach_far);
  const float special_far = world_reach(attack_spec(MoveId::Special).reach_far);
  // 招式命中还需要“对手的身体宽度”参与：脚下间距 ≤ 触及距离 + 半个身体，
  // 否则会对着“指尖刚碰到”的距离反复出空拳。
  constexpr float kBodyHalfWidth = 30.0f * kBodyScale;

  // ③ 对手正在出招且离得近 → 有概率防御（这是"看得到对手动作"的唯一表现点）。
  const bool opponent_attacking = is_attack(other.move) &&
                                  other.move_frame <= other.timing.startup + other.timing.active;
  // ③a **抢招**：对手起手段还没进入判定时，用更快的招把它打掉。
  //
  // 为何必需（实测代价）：早期实现只会“后退”或“防御”，遇到**一直在出招**的
  // 对手时永远只能反应、不能反打——实测手写 AI 对“无脑按重拳”胜率 0%、
  // 而对方 100%。格斗里“抢帧”是基本功：对手起手比我的判定更早到，我就该躲；
  // 反之就该用**更快**的招抢掉它。
  if (opponent_attacking && gap < heavy_far + kBodyHalfWidth &&
      other.move_frame < other.timing.startup) {
    // 对手还在起手：轻拳起动最快（14 帧起手），适合抢。
    const int other_remaining = other.timing.startup - other.move_frame;
    if (other_remaining >= gedou::timing_for(MoveId::Light).startup &&
        gap <= light_far + kBodyHalfWidth && chance(tuning.aggression)) {
      committed_ = Intent{0.0f, false, MoveId::Light};
      hold_timer_ = 3;
      decision_ = "interrupt";
      return committed_;
    }
  }
  if (opponent_attacking && gap < heavy_far + kBodyHalfWidth && chance(tuning.guard_rate)) {
    committed_ = Intent{0.0f, true, std::nullopt};
    // 防御不要"按住很久"：对手收招后必须立即夺回节奏，
    // 长 hold 会让 AI 在对手下一轮起手时还在抱头。
    hold_timer_ = 4;
    decision_ = "guard";
    return committed_;
  }

  // ③b **惩罚"出招但不出手"**：旧实现只在 `gap <= heavy_far + 半身` 才考虑进攻，
  // 而 AI 自己的“接近”目标就是这个距离——两者叠在一起时，AI 会**站在
  // 出招距离之外反复走位、但永不攻击**（实测：对“一直按蹬腿”的人工策略，
  // 它全程只在 80~170px 来回走，从不反击，故对方 100% 胜）。
  // 修法：进入任意招式的触及范围就**按概率出手**，而不是先要求“更近”。
  if (gap <= kick_far + kBodyHalfWidth && gap >= light_near - kBodyHalfWidth &&
      chance(tuning.aggression * 0.75f)) {
    const MoveId pick = gap <= light_far + kBodyHalfWidth && chance(0.55f) ? MoveId::Light
                                                                          : MoveId::Kick;
    committed_ = Intent{0.0f, false, pick};
    hold_timer_ = 3;
    decision_ = pick == MoveId::Light ? "light" : "kick";
    return committed_;
  }

  // ④ 有必杀槽：距离合适就放（必杀是破防的，AI 用它收尾）。
  if (mine.meter >= kMeterCost && gap < special_far + kBodyHalfWidth &&
      chance(tuning.special_rate)) {
    committed_ = Intent{0.0f, false, MoveId::Special};
    hold_timer_ = 6;
    decision_ = "special";
    return committed_;
  }

  // ⑤ 进攻：进入重拳距离就打重拳；正好在手边打轻拳；腿长的距离用蹬腿。
  if (gap <= heavy_far + kBodyHalfWidth && gap >= light_near - kBodyHalfWidth) {
    if (gap <= light_far + kBodyHalfWidth && chance(tuning.aggression * 0.5f)) {
      committed_ = Intent{0.0f, false, MoveId::Light};
      hold_timer_ = 4;
      decision_ = "light";
      return committed_;
    }
    if (chance(tuning.aggression)) {
      const MoveId pick = kick_far > heavy_far && chance(0.5f) ? MoveId::Kick : MoveId::Heavy;
      committed_ = Intent{0.0f, false, pick};
      hold_timer_ = 6;
      decision_ = pick == MoveId::Kick ? "kick" : "heavy";
      return committed_;
    }
  }

  // ⑥ 走位：远则接近；进入出招距离后**小步调整并保持压迫**。
  if (gap > heavy_far + kBodyHalfWidth) {
    committed_ = Intent{1.0f, false, std::nullopt};
    hold_timer_ = 6;
    decision_ = "approach";
    return committed_;
  }
  committed_ = Intent{-1.0f, false, std::nullopt};
  hold_timer_ = 8;
  decision_ = "retreat";
  return committed_;
}

auto AiController::aggression_range() const noexcept -> float {
  // —— 与 `think()` **同一个口径**：这是它能被断言的前提 ——
  // 两处各写一份时，“单测验的是阈值”与“AI 实际用的阈值”会静默分岔。
  return world_reach(attack_spec(MoveId::Heavy).reach_far) + 30.0f * kBodyScale;
}

}  // namespace gedou

#include "fight.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

#include "st/core/json.hpp"

namespace gedou {

namespace {

constexpr int kComboWindowFrames = 40;
/// 判定的**帧内推进**：动作第 `startup` 帧起算为第一个判定帧，
/// 持续 `active` 帧（与 `pose_for` 的出招段严格对齐）。
[[nodiscard]] auto in_active_window(const FighterState& fighter) noexcept -> bool {
  if (!is_attack(fighter.move)) return false;
  return fighter.move_frame >= fighter.timing.startup &&
         fighter.move_frame < fighter.timing.startup + fighter.timing.active;
}

/// 转身判定：对手从背后打过来时应当转身面对（格斗游戏的"自动面向"）。
/// 只在**能行动**时转身——硬直中转身会让"被背刺"变得无从发生。
void face_opponent(FighterState& fighter, const FighterState& opponent) {
  if (!fighter.can_act()) return;
  const float delta = opponent.x - fighter.x;
  if (std::abs(delta) < 1.0f) return;
  fighter.facing_right = delta > 0.0f;
}

/// 关节到关节的距离（命中盒可视化的判据之一，也是"伸没伸出去"的直接量）。
[[nodiscard]] auto reach_of(const Pose& pose) noexcept -> float {
  return std::max(pose.hand_front.x, pose.foot_front.x);
}

}  // namespace
Match::Match(std::size_t left_profile, std::size_t right_profile, Arena arena)
    : arena_(arena) {
  fighters_[0].profile = left_profile;
  fighters_[1].profile = right_profile;
  reset();
}

void Match::reset() {
  for (int side = 0; side < 2; ++side) {
    FighterState& fighter = fighters_[static_cast<std::size_t>(side)];
    const FighterProfile& profile = fighter_profile(fighter.profile);
    fighter.x = arena_.center() + (side == 0 ? -arena_.start_gap : arena_.start_gap);
    fighter.facing_right = side == 0;
    fighter.max_hp = profile.max_hp;
    fighter.hp = profile.max_hp;
    fighter.meter = 0.0f;
    fighter.move = MoveId::Idle;
    fighter.move_frame = 0;
    fighter.timing = MoveTiming{};
    fighter.hitstun = 0;
    fighter.connected = false;
    fighter.push_velocity = 0.0f;
    fighter.walk_phase = 0.0f;
    fighter.pose_lock = 0;
    fighter.combo = 0;
    fighter.comboTimer = 0;
    clamp_to_arena(fighter);
  }
  phase_ = RoundPhase::Fight;
  frame_index_ = 0;
  end_timer_ = 0;
}

auto Match::state(int side) const -> const FighterState& {
  return fighters_[static_cast<std::size_t>(std::clamp(side, 0, 1))];
}

auto Match::winner() const -> std::optional<int> {
  if (phase_ != RoundPhase::Over) return std::nullopt;
  const bool left_alive = fighters_[0].alive();
  const bool right_alive = fighters_[1].alive();
  if (left_alive && !right_alive) return 0;
  if (right_alive && !left_alive) return 1;
  return std::nullopt;
}

void Match::begin_move(FighterState& fighter, MoveId move) {
  const FighterProfile& profile = fighter_profile(fighter.profile);
  fighter.move = move;
  // 帧计数从 0 开始：`in_active_window` 用 `move_frame >= startup`，
  // 而“已经过了一帧”的状态就是 `move_frame == 1`。两者保持一致（差 1 帧的手感差异
  // 在起手时看得出来：旧写法把首帧白白丢掉了）。
  fighter.move_frame = 0;
  fighter.connected = false;
  fighter.timing = scaled_timing(move, profile.speed);
  if (move == MoveId::Special) fighter.meter = std::max(0.0f, fighter.meter - kMeterCost);
}

void Match::update_fighter(FighterState& fighter, int side, const Intent& intent,
                           FrameEvents& events) {
  const FighterProfile& profile = fighter_profile(fighter.profile);
  FighterState& opponent = fighters_[static_cast<std::size_t>(1 - side)];

  // ⓪ 结算态（胜利/败北/倒地）：**只推进帧**，不受意图影响。
  //
  // 为何要单独一条：这几个姿态不是“持续的动作”，而是**终态**。
  // 旧写法让它们落到下面“空闲 → 切回 Idle”的分支里，于是胜者的胜利姿态
  // 只维持一帧就变回待机（屏幕上两个人都站着，看不出谁赢了）。
  if (phase_ == RoundPhase::Over &&
      (fighter.move == MoveId::Victory || fighter.move == MoveId::Defeat ||
       fighter.move == MoveId::KnockDown)) {
    if (fighter.pose_lock > 0) --fighter.pose_lock;
    ++fighter.move_frame;
    return;
  }

  // ① 退场态（倒地/胜利/败北）优先：计时到了就切到对应的稳定姿态。
  if (fighter.pose_lock > 0) {
    --fighter.pose_lock;
    ++fighter.move_frame;
    if (fighter.pose_lock == 0) {
      // 姿态收场：**血量为零的就是败者**（先看自己，再看对手）——
      // 写成“winner() == 1 - side”会把胜者也判成败北（两边都是“非我即他”）。
      if (fighter.hp <= 0.0f) {
        fighter.move = MoveId::Defeat;
      } else if (opponent.hp <= 0.0f && phase_ == RoundPhase::Over) {
        fighter.move = MoveId::Victory;
      } else {
        fighter.move = MoveId::Idle;
      }
      fighter.move_frame = 0;
    }
    return;
  }

  // ② 硬直：倒计时结束后回到可行动状态。
  if (fighter.hitstun > 0) {
    --fighter.hitstun;
    fighter.push_velocity *= 0.82f;
    fighter.x += fighter.push_velocity;
    clamp_to_arena(fighter);
    if (fighter.comboTimer > 0) {
      --fighter.comboTimer;
      if (fighter.comboTimer == 0) fighter.combo = 0;
    }
    ++fighter.move_frame;
    if (fighter.hitstun == 0) {
      fighter.move = MoveId::Idle;
      fighter.move_frame = 0;
      fighter.timing = MoveTiming{};
    }
    return;
  }

  face_opponent(fighter, opponent);

  // ③ 正在出招：推进帧；招式走完回待机。
  if (is_attack(fighter.move)) {
    ++fighter.move_frame;
    if (fighter.move_frame >= fighter.timing.total()) {
      fighter.move = MoveId::Idle;
      fighter.move_frame = 0;
      fighter.timing = MoveTiming{};
    }
  } else if (fighter.move == MoveId::Hurt) {
    ++fighter.move_frame;
    if (fighter.move_frame >= fighter.timing.total() + 1) {
      fighter.move = MoveId::Idle;
      fighter.move_frame = 0;
    }
  } else {
    // ④ 空闲/移动/防御：按本帧意图决定。
    const bool wants_attack = intent.attack.has_value();
    if (wants_attack) {
      const MoveId move = *intent.attack;
      const bool needs_meter = move == MoveId::Special;
      if (!needs_meter || fighter.meter >= kMeterCost) {
        begin_move(fighter, move);
        if (move == MoveId::Special) events.special_fired = true;
        // 本帧就是第 0 帧（生成的那一帧不算已过一帧）：不额外推进，
        // 下一帧由上面的 `is_attack` 分支 ++ 到 1。
        return;
      }
    }
    if (intent.guard) {
      if (fighter.move != MoveId::Guard) {
        fighter.move = MoveId::Guard;
        fighter.move_frame = 0;
      } else {
        ++fighter.move_frame;
      }
      return;
    }
    const float direction = intent.move;
    if (std::abs(direction) > 0.01f) {
      const float sign = fighter.facing_right ? 1.0f : -1.0f;
      // `move > 0` = 朝自己正面**前进**（不是"向右"）：朝向翻转后同一个键仍是前进。
      const float delta = direction * sign;
      fighter.x += delta * profile.walk_speed;
      fighter.walk_phase = std::fmod(fighter.walk_phase + 0.09f, 1.0f);
      fighter.move = MoveId::Walk;
      ++fighter.move_frame;
      clamp_to_arena(fighter);
      return;
    }
    if (fighter.move != MoveId::Idle) {
      fighter.move = MoveId::Idle;
      fighter.move_frame = 0;
    } else {
      ++fighter.move_frame;
    }
    clamp_to_arena(fighter);
  }
}

void Match::resolve_attack(FighterState& attacker, FighterState& defender, int attacker_side,
                           FrameEvents& events) {
  if (attacker.connected || !in_active_window(attacker)) return;
  // 若本招已命中，不得再算（`connected` 由 `apply_hit` 置位）。
  const math::Rect box = active_hitbox(attacker_side);
  if (box.is_empty()) return;
  const math::Rect body = hurtbox(1 - attacker_side);
  if (box.intersect(body).is_empty()) return;
  apply_hit(attacker, defender, attacker_side, attacker.move, events);
}

void Match::apply_hit(FighterState& attacker, FighterState& defender, int attacker_side, MoveId move,
                      FrameEvents& events) {
  const AttackSpec& spec = attack_spec(move);
  const FighterProfile& defender_profile = fighter_profile(defender.profile);
  attacker.connected = true;

  // 防御：只在**正面**且处于 Guard 时成立（背刺破防；必杀 `unblockable` 破防）。
  const bool facing_attacker = defender.facing_right != attacker.facing_right;
  bool blocking = defender.move == MoveId::Guard && facing_attacker && !spec.unblockable;
  float damage = spec.damage;
  if (blocking) {
    damage *= spec.chip_ratio;
    // —— 削韧破防 ——
    //
    // 为何必需（实测代价）：只要防御**没有代价**，“一直按同一招”就是必胜策略——
    // 对方只能一直挡、永远不能反击（实测：无条件按蹬腿对三档难度均 100% 胜率）。
    // 这套规则让防御有成本：累积削韧超过阈值就破防，进入**不受控的**长硬直。
    defender.guard_break = std::min(kGuardBreakMax, defender.guard_break + spec.guard_damage);
    if (defender.guard_break >= kGuardBreakMax) {
      blocking = false;
      defender.guard_break = 0.0f;
      // 破防：本次招式**不再削减**（全额命中），并给一段长硬直。
      events.broken_guard.push_back(1 - attacker_side);
    }
  } else {
    // 没在防御：削韧随时间回落（主动进攻不需要“回韧”操作）
    defender.guard_break = std::max(0.0f, defender.guard_break - spec.guard_damage * 0.5f);
  }

  defender.hp = std::max(0.0f, defender.hp - damage);
  attacker.meter = std::min(kMeterMax, attacker.meter + spec.meter_gain);
  defender.meter = std::min(kMeterMax, defender.meter + kMeterOnHurt);

  // 连击：上次命中在窗口内 → 累加，否则重置为 1。
  attacker.combo = attacker.comboTimer > 0 ? attacker.combo + 1 : 1;
  attacker.comboTimer = kComboWindowFrames;

  const float sign = attacker.facing_right ? 1.0f : -1.0f;
  const float knockback = spec.knockback / std::max(0.2f, defender_profile.weight);
  defender.push_velocity = sign * knockback * (blocking ? 0.55f : 1.0f);

  const math::Point contact{(attacker.x + defender.x) * 0.5f, arena_.ground - 130.0f};
  events.hits.push_back(HitEvent{1 - attacker_side, move, damage, blocking, spec.knockdown,
                                 attacker.combo, contact});
  if (move == MoveId::Special || (spec.knockdown && !blocking)) events.shake = 7.0f;
  else if (!blocking) events.shake = std::max(events.shake, 2.5f);

  if (defender.hp <= 0.0f) {
    // 血条清零：切倒地，回合在本帧结束。
    defender.move = MoveId::KnockDown;
    defender.move_frame = 0;
    defender.timing = MoveTiming{};
    defender.pose_lock = timing_for(MoveId::KnockDown).recovery;
    defender.hitstun = 0;
    events.knocked_out.push_back(1 - attacker_side);
    events.shake = 9.0f;
    phase_ = RoundPhase::Over;
    end_timer_ = 120;
  } else if (spec.knockdown && !blocking) {
    defender.move = MoveId::KnockDown;
    defender.move_frame = 0;
    defender.timing = MoveTiming{};
    defender.pose_lock = timing_for(MoveId::KnockDown).recovery;
    defender.hitstun = 0;
  } else {
    // 普通命中：硬直按招式表；破防命中则给一段**更长的**硬直
    // （“挡也不是、不挡也不是”才是防御该有的压力）。
    const bool was_broken =
        std::find(events.broken_guard.begin(), events.broken_guard.end(), 1 - attacker_side) !=
        events.broken_guard.end();
    defender.hitstun = blocking ? std::max(4, spec.hitstun / 2)
                                : (was_broken ? spec.hitstun * 2 : spec.hitstun);
    defender.move = MoveId::Hurt;
    defender.move_frame = 0;
    defender.timing = timing_for(MoveId::Hurt);
  }
  defender.x += sign * knockback * 0.5f;
  clamp_to_arena(defender);
}

auto Match::advance(const Intent& left, const Intent& right) -> FrameEvents {
  FrameEvents events{};
  ++frame_index_;

  if (phase_ == RoundPhase::Fight) {
    update_fighter(fighters_[0], 0, left, events);
    update_fighter(fighters_[1], 1, right, events);
    resolve_attack(fighters_[0], fighters_[1], 0, events);
    resolve_attack(fighters_[1], fighters_[0], 1, events);
    // 身体互不穿透（谁也不能站到对方身体里）：按"谁在动"分摊推开量。
    const float min_gap = 76.0f;
    const float delta = fighters_[1].x - fighters_[0].x;
    if (std::abs(delta) < min_gap) {
      const float push = (min_gap - std::abs(delta)) * 0.5f;
      const float sign = delta >= 0.0f ? 1.0f : -1.0f;
      fighters_[0].x -= sign * push;
      fighters_[1].x += sign * push;
      clamp_to_arena(fighters_[0]);
      clamp_to_arena(fighters_[1]);
    }
    if (fighters_[0].comboTimer > 0) --fighters_[0].comboTimer;
    if (fighters_[1].comboTimer > 0) --fighters_[1].comboTimer;
  } else {
    // 结算：继续推进姿态（倒地/胜利动画要播），但不再受理输入。
    static const Intent kIdle{};
    update_fighter(fighters_[0], 0, kIdle, events);
    update_fighter(fighters_[1], 1, kIdle, events);
    // 胜方进胜利姿态（它没有 `pose_lock`，所以必须在这里显式切）——
    // 否则屏幕上是“两个人都在待机，只有一个人血空了”。
    for (int side = 0; side < 2; ++side) {
      FighterState& me = fighters_[static_cast<std::size_t>(side)];
      const FighterState& other = fighters_[static_cast<std::size_t>(1 - side)];
      if (me.hp > 0.0f && other.hp <= 0.0f && me.pose_lock == 0 &&
          me.move != MoveId::Victory && me.move != MoveId::KnockDown) {
        me.move = MoveId::Victory;
        me.move_frame = 0;
        me.timing = timing_for(MoveId::Victory);
      }
      // 败者同理：倒地姿态计时到了就定在败北（同样不能被后面的“空闲分支”冲掉）。
      if (me.hp <= 0.0f && me.pose_lock == 0 && me.move != MoveId::Defeat &&
          me.move != MoveId::KnockDown) {
        me.move = MoveId::Defeat;
        me.move_frame = 0;
        me.timing = timing_for(MoveId::Defeat);
      }
    }
    if (end_timer_ > 0) --end_timer_;
  }
  return events;
}

void Match::clamp_to_arena(FighterState& fighter) const {
  // 夹取要留出**半个身体宽**：只用 `arena.left/right` 时，角色中心到了边界，
  // 而肩/手还在边界外——截图里就是“人被挤出舞台”（实测被训练后的机器人
  // 逼到墙角时看得很清楚，而战况数据完全正常，所以只能从画面上发现）。
  constexpr float kBodyRadius = 30.0f * kBodyScale;
  const float lo = arena_.left + kBodyRadius;
  const float hi = arena_.right - kBodyRadius;
  fighter.x = lo <= hi ? std::clamp(fighter.x, lo, hi) : (lo + hi) * 0.5f;
}

void Match::set_arena(const Arena& arena) {
  arena_ = arena;
  for (FighterState& fighter : fighters_) clamp_to_arena(fighter);
}

void Match::place(int side, float x) {
  FighterState& fighter = fighters_[static_cast<std::size_t>(std::clamp(side, 0, 1))];
  fighter.x = x;
  clamp_to_arena(fighter);
}

void Match::charge_meter(int side) {
  fighters_[static_cast<std::size_t>(std::clamp(side, 0, 1))].meter = kMeterMax;
}

auto Match::world_pose(int side) const -> Pose {
  const FighterState& fighter = state(side);
  return pose_for(fighter.move, fighter.move_frame, fighter.walk_phase)
      .to_world(fighter.x, arena_.ground, fighter.facing_right);
}

auto Match::hurtbox(int side) const -> math::Rect {
  const FighterState& fighter = state(side);
  const Pose pose = world_pose(side);
  // 判定盒不需要精确到肢体（那是格斗游戏里刻意粗化的部分），
  // 但必须**与骨架同口径**：头/胸的高度直接取姿态点，宽度按 `kBodyScale` 缩。
  const float top = std::min(pose.head.y - pose.head_radius, pose.chest.y) -
                    6.0f * kBodyScale;
  const float bottom = arena_.ground;
  const float half_width = fighter.move == MoveId::KnockDown ? 56.0f * kBodyScale
                                                            : 30.0f * kBodyScale;
  const float center = fighter.move == MoveId::KnockDown
                           ? fighter.x + (fighter.facing_right ? -22.0f : 22.0f) * kBodyScale
                           : fighter.x;
  return math::Rect::from_ltrb(center - half_width, top, center + half_width, bottom);
}

auto Match::active_hitbox(int side) const -> math::Rect {
  const FighterState& fighter = state(side);
  if (!in_active_window(fighter)) return math::Rect{};
  const AttackSpec& spec = attack_spec(fighter.move);
  // 招式表用**局部单位**：与骨架同一套坐标系，乘 `kBodyScale` 才是世界像素。
  const float sign = fighter.facing_right ? 1.0f : -1.0f;
  const float near_edge = fighter.x + sign * spec.reach_near * kBodyScale;
  const float far_edge = fighter.x + sign * spec.reach_far * kBodyScale;
  const float left = std::min(near_edge, far_edge);
  const float width = std::abs(far_edge - near_edge);
  const float bottom = arena_.ground - spec.height_low * kBodyScale;
  const float top = arena_.ground - spec.height_high * kBodyScale;
  return math::Rect::from_ltrb(left, top, left + width, bottom);
}

auto Match::force_move(int side, MoveId move) -> bool {
  FighterState& fighter = fighters_[static_cast<std::size_t>(std::clamp(side, 0, 1))];
  if (!is_attack(move)) {
    // 非攻击动作（待机/防御…）随时可切：给 e2e 摆姿态用。
    fighter.move = move;
    fighter.move_frame = 0;
    fighter.timing = timing_for(move);
    fighter.pose_lock = 0;
    fighter.hitstun = 0;
    return true;
  }
  if (!fighter.can_act()) return false;
  if (move == MoveId::Special && fighter.meter < kMeterCost) return false;
  begin_move(fighter, move);
  return true;
}

auto Match::apply_damage(int side, float damage) -> bool {
  FighterState& fighter = fighters_[static_cast<std::size_t>(std::clamp(side, 0, 1))];
  if (!fighter.alive() || damage <= 0.0f) return false;
  fighter.hp = std::max(0.0f, fighter.hp - damage);
  // 血空必须**走同一套 KO 处理**（倒地 + 回合结束），不能只改数字：
  // 否则“血量 0 但还在打”这种状态会成为一条与真实命中分岔的旁路
  // （e2e 用它造残血场景时就会测到一个不存在的状态）。
  if (fighter.hp <= 0.0f) {
    fighter.move = MoveId::KnockDown;
    fighter.move_frame = 0;
    fighter.timing = MoveTiming{};
    fighter.pose_lock = timing_for(MoveId::KnockDown).recovery;
    fighter.hitstun = 0;
    phase_ = RoundPhase::Over;
    end_timer_ = 120;
  }
  return true;
}

auto Match::snapshot() const -> std::string {
  using st::Json;
  Json root = Json::object();
  root["phase"] = phase_ == RoundPhase::Fight ? "fight" : "over";
  root["frame"] = frame_index_;
  root["arena"] = Json{{"left", arena_.left},
                       {"right", arena_.right},
                       {"ground", arena_.ground},
                       {"center", arena_.center()}};
  if (const auto who = winner(); who.has_value()) root["winner"] = *who;
  Json sides = Json::array();
  for (int side = 0; side < 2; ++side) {
    const FighterState& fighter = state(side);
    const FighterProfile& profile = fighter_profile(fighter.profile);
    Json item = Json::object();
    item["side"] = side;
    item["profile"] = std::string(profile.key);
    item["name"] = std::string(profile.name);
    item["x"] = fighter.x;
    item["facing"] = fighter.facing_right ? "right" : "left";
    item["hp"] = fighter.hp;
    item["max_hp"] = fighter.max_hp;
    item["meter"] = fighter.meter;
    item["move"] = std::string(move_name(fighter.move));
    item["move_frame"] = fighter.move_frame;
    item["hitstun"] = fighter.hitstun;
    // 出招时的"手/脚伸到多远"（相对脚下的距离）——绘制层不用它，
    // 但它是**骨架真的伸出去了**的直接量：e2e 可以断言"判定帧的手比待机帧远"，
    // 而不必去猜像素。
    const Pose local = pose_for(fighter.move, fighter.move_frame, fighter.walk_phase);
    item["reach"] = reach_of(local) * kBodyScale;
    item["combo"] = fighter.combo;
    item["alive"] = fighter.alive();
    const math::Rect box = active_hitbox(side);
    item["hitbox"] = Json{{"x", box.x}, {"y", box.y}, {"width", box.width},
                          {"height", box.height}};
    sides.push_back(std::move(item));
  }
  root["fighters"] = std::move(sides);
    // 招式表也导出：e2e 的期望值从**运行中的进程**取，而不是脚本里再抄一份。
    // 距离类字段已乘 `kBodyScale`（世界像素）：与截图/判定盒同口径。
  Json moves = Json::object();
  for (std::uint8_t index = 0; index < static_cast<std::uint8_t>(MoveId::Count); ++index) {
    const auto move = static_cast<MoveId>(index);
    const AttackSpec& spec = attack_spec(move);
    if (!is_attack(move)) continue;
    moves[std::string(move_name(move))] =
        Json{{"damage", spec.damage},
             {"reach_near", spec.reach_near * kBodyScale},
             {"reach_far", spec.reach_far * kBodyScale},
             {"hitstun", spec.hitstun},
             {"knockdown", spec.knockdown},
             {"unblockable", spec.unblockable}};
  }
  root["moves"] = std::move(moves);
  return st::json_dump(root);
}

}  // namespace gedou

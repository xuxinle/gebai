#include "rl_env.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

#include "st/core/json.hpp"

namespace gedou {

namespace {

/// 观测分量表。**顺序即向量下标**：改顺序就是改契约（权重全部失效），
/// 所以每一项都带名字与含义，schema 导出与文档都从这里取。
///
/// 归一化口径：所有量都压到 `[-1, 1]` 或 `[0, 1]`。**不做逐样本标准化**
/// （mean/std 归一化）——那会让"训练时常量"进到权重里，而游戏内推理是单帧、
/// 拿不到训练集统计量，两边必然分岔。
const std::vector<ObservationField> kFields{
    {"self_hp", "己方血量 / 上限（1 = 满血）", 0.0f, 1.0f},
    {"opp_hp", "对手血量 / 上限", 0.0f, 1.0f},
    {"self_meter", "己方必杀槽 / 上限（1 = 可放必杀）", 0.0f, 1.0f},
    {"opp_meter", "对手必杀槽 / 上限", 0.0f, 1.0f},
    {"gap", "双方脚下距离（已按 300px 归一，1 = 远离）", 0.0f, 1.0f},
    {"gap_near", "距离是否在轻拳触及内（1/0）", 0.0f, 1.0f},
    {"gap_mid", "距离是否在重拳/蹬腿触及内（1/0）", 0.0f, 1.0f},
    {"facing", "对手是否在己方正面（1 = 正面，-1 = 背后）", -1.0f, 1.0f},
    {"self_ready", "己方可行动（1）/ 硬直或退场（0）", 0.0f, 1.0f},
    {"opp_ready", "对手可行动（1）/ 硬直或退场（0）", 0.0f, 1.0f},
    {"opp_attacking", "对手正在出招（1/0）", 0.0f, 1.0f},
    {"opp_move_active", "对手处于判定帧（1/0）——最危险的瞬间", 0.0f, 1.0f},
    {"self_phase_startup", "己方出招的起手段（1/0）", 0.0f, 1.0f},
    {"self_phase_active", "己方出招的判定段（1/0）", 0.0f, 1.0f},
    {"self_phase_recovery", "己方出招的收招段（1/0）", 0.0f, 1.0f},
    {"opp_guard", "对手处于防御姿态（1/0）", 0.0f, 1.0f},
    {"opp_combo", "对手当前连击数（按 4 归一）", 0.0f, 1.0f},
    {"self_cornered", "己方是否被逼到墙角（1/0）——被压制状态", 0.0f, 1.0f},
    {"opp_cornered", "对手是否被逼到墙角（1/0）——压制机会", 0.0f, 1.0f},
    {"time_left", "回合剩余时间比（1 = 刚开局，0 = 到时）", 0.0f, 1.0f},
};

[[nodiscard]] auto clamp_unit(float value) -> float {
  return std::clamp(value, -1.0f, 1.0f);
}

/// 招式在某一帧处于哪个阶段（供观测；与 `Match` 的判定窗口同口径）。
void phase_flags(const FighterState& fighter, bool& startup, bool& active,
                 bool& recovery) {
  startup = false;
  active = false;
  recovery = false;
  if (!is_attack(fighter.move)) return;
  const int frame = fighter.move_frame;
  startup = frame < fighter.timing.startup;
  active = frame >= fighter.timing.startup &&
           frame < fighter.timing.startup + fighter.timing.active;
  recovery = !startup && !active;
}

}  // namespace

auto rl_action_name(RlAction action) -> std::string_view {
  switch (action) {
    case RlAction::Idle: return "idle";
    case RlAction::Forward: return "forward";
    case RlAction::Backward: return "backward";
    case RlAction::Guard: return "guard";
    case RlAction::Light: return "light";
    case RlAction::Heavy: return "heavy";
    case RlAction::Kick: return "kick";
    case RlAction::Special: return "special";
    case RlAction::Count: break;
  }
  return "idle";
}

auto rl_action_from_index(int index) -> RlAction {
  // 越界**不静默回退到 Idle**：策略输出越界一定是契约出了问题，
  // 而"悄悄当待机"会让这种问题藏在一场场对局里看不见。
  if (index < 0 || index >= static_cast<int>(kRlActionCount)) return RlAction::Idle;
  return static_cast<RlAction>(index);
}

auto observation_fields() -> const std::vector<ObservationField>& { return kFields; }

auto observation_dim() -> std::size_t { return kFields.size(); }

auto observe(const Match& match, int side) -> Observation {
  const FighterState& self = match.state(side);
  const FighterState& opp = match.state(1 - side);
  const Arena& arena = match.arena();
  const FighterProfile& self_profile = fighter_profile(self.profile);
  const FighterProfile& opp_profile = fighter_profile(opp.profile);

  const float gap = std::abs(opp.x - self.x);
  // 距离特征按**角色自己的招式**算（不是硬编码的轻/重/腿）：
  // 否则技能型角色（射程 240 的扇形、几乎只能贴身的摸腰）
  // 会在观测里被描述成一个它们根本没有的距离画像——策略学到的东西与真能用的招脱节。
  const float light_far = attack_spec(self_profile.moves[0]).reach_far * kBodyScale;
  const float heavy_far = attack_spec(self_profile.moves[1]).reach_far * kBodyScale;
  const float kick_far = attack_spec(self_profile.moves[0]).reach_far * kBodyScale;
  const float mid_far = std::max(heavy_far, kick_far);
  const float body_half = 30.0f * kBodyScale;
  // 对手是否在己方面前：正负号由朝向决定（自我中心，不含世界坐标）。
  const float sign = self.facing_right ? 1.0f : -1.0f;
  const float signed_gap = (opp.x - self.x) * sign;
  const float facing = signed_gap >= 0.0f ? 1.0f : -1.0f;

  bool self_startup = false;
  bool self_active = false;
  bool self_recovery = false;
  phase_flags(self, self_startup, self_active, self_recovery);
  bool opp_startup = false;
  bool opp_active = false;
  bool opp_recovery = false;
  phase_flags(opp, opp_startup, opp_active, opp_recovery);
  (void)opp_startup;
  (void)opp_recovery;

  // 墙角：离场地边界的距离小于"一步半"。
  //
  // ⚠ 这里用**相对场地宽度的比例**而不是绝对像素：观测必须**自我中心**
  // （同一场对局平移到另一个场地，观测应完全相同）。早期用 `<= 40px` 时，
  // 换到更宽的场地就把“靠墙”判断成了“不靠墙”——策略会学到与窗口尺寸
  // 绑定的东西（单测 `rl_observation_is_egocentric` 就是用来钉这条的）。
  const float edge_band = arena.width() * 0.04f;
  const float self_edge = std::min(self.x - arena.left, arena.right - self.x);
  const float opp_edge = std::min(opp.x - arena.left, arena.right - opp.x);

  const int max_frames = std::max(1, 60 * 40);
  const float time_left = std::clamp(
      1.0f - static_cast<float>(match.frame_count()) / static_cast<float>(max_frames), 0.0f, 1.0f);

  Observation out;
  out.reserve(kFields.size());
  out.push_back(clamp_unit(self.hp / std::max(1.0f, self.max_hp)));
  out.push_back(clamp_unit(opp.hp / std::max(1.0f, opp.max_hp)));
  out.push_back(clamp_unit(self.meter / kMeterMax));
  out.push_back(clamp_unit(opp.meter / kMeterMax));
  out.push_back(clamp_unit(gap / 300.0f));  out.push_back(gap <= light_far + body_half ? 1.0f : 0.0f);
  out.push_back(gap <= mid_far + body_half ? 1.0f : 0.0f);
  out.push_back(facing);
  out.push_back(self.can_act() ? 1.0f : 0.0f);
  out.push_back(opp.can_act() ? 1.0f : 0.0f);
  out.push_back(is_attack(opp.move) ? 1.0f : 0.0f);
  out.push_back(opp_active ? 1.0f : 0.0f);
  out.push_back(self_startup ? 1.0f : 0.0f);
  out.push_back(self_active ? 1.0f : 0.0f);
  out.push_back(self_recovery ? 1.0f : 0.0f);
  out.push_back(opp.move == MoveId::Guard ? 1.0f : 0.0f);
  out.push_back(clamp_unit(static_cast<float>(opp.combo) / 4.0f));
  out.push_back(self_edge <= edge_band ? 1.0f : 0.0f);
  out.push_back(opp_edge <= edge_band ? 1.0f : 0.0f);
  out.push_back(time_left);
  (void)self_profile;
  (void)opp_profile;
  return out;
}

/// 把 RL 的抽象动作映射到**该角色实际拥有的招式**。
///
/// 为何要这层：RL 的观测/动作空间是固定维度的（轻/重/腿/必杀四招），
/// 这样训练好的策略与具体角色解耦。而技能型角色（甜甜圈/户圣）
/// 手里没有“轻拳”——它们的三个技能位是自己那三招。
///
/// 映射语义与手写 AI 一致（`ai.cpp` 的 `to_profile_move`）：
/// Light/Kick（快招）→ 技能位 0；Heavy → 技能位 1；Special → 技能位 2。
/// 两处必须同一口径，否则“人机对打时双方招式容量不同”——那是隐形的难度偏移。
[[nodiscard]] auto profile_move(const FighterState& me, MoveId abstract_move) -> MoveId {
  if (abstract_move == MoveId::Guard || abstract_move == MoveId::Idle ||
      abstract_move == MoveId::Walk) {
    return abstract_move;
  }
  const FighterProfile& profile = fighter_profile(me.profile);
  if (abstract_move == MoveId::Light || abstract_move == MoveId::Kick) return profile.moves[0];
  if (abstract_move == MoveId::Heavy) return profile.moves[1];
  if (abstract_move == MoveId::Special) return profile.moves[2];
  return abstract_move;
}

auto action_intent(RlAction action) -> Intent {
  Intent intent{};
  switch (action) {
    case RlAction::Idle: break;
    case RlAction::Forward: intent.move = 1.0f; break;
    case RlAction::Backward: intent.move = -1.0f; break;
    case RlAction::Guard: intent.guard = true; break;
    case RlAction::Light: intent.attack = MoveId::Light; break;
    case RlAction::Heavy: intent.attack = MoveId::Heavy; break;
    case RlAction::Kick: intent.attack = MoveId::Kick; break;
    case RlAction::Special: intent.attack = MoveId::Special; break;
    case RlAction::Count: break;
  }
  return intent;
}

RlEnv::RlEnv(Config config) : config_(config) {
  match_ = Match(config_.self_profile, config_.opp_profile, Arena{});
  // 场地固定成"标准宽度"：训练/评测/游戏内三处的观测口径必须一致，
  // 而真实窗口尺寸会变（那会让 `gap/300` 的分布随窗口漂移）。
  Arena arena{};
  arena.left = 70.0f;
  arena.right = 1110.0f;
  arena.ground = 552.0f;
  arena.start_gap = 200.0f;
  match_.set_arena(arena);
  opponent_ai_ = AiController(config_.seed, config_.opponent);
  rng_ = static_cast<std::uint64_t>(config_.seed) | 1U;
}

auto RlEnv::next_random() noexcept -> float {
  rng_ = rng_ * 6364136223846793005ULL + 1442695040888963407ULL;
  return static_cast<float>((rng_ >> 40U) & 0xFFFFFFU) / 16777216.0f;
}

auto RlEnv::reset() -> Observation {
  match_.reset();
  // 对手每局**换一个种子**（用局计数推）：
  // 若每局都用同一个种子，对手的随机序列完全相同——策略会学到
  // “对某套固定招式序列的最优应对”（过拟合单一对手），而非真正的格斗能力。
  // `config_.seed` 固定 ⇒ 整个评测/训练仍可复现。
  ++episode_index_;
  const std::uint32_t opponent_seed = config_.seed + episode_index_ * 2654435761U;
  opponent_ai_ = AiController(opponent_seed, config_.opponent);
  // 一部分回合让对手**打桩不动**（见 Config::idle_opponent_ratio）：
  // 否则策略学不到“对手不主动时该怎么打”，进游戏遇到不动的玩家就会卡住。
  opponent_idle_ = next_random() < config_.idle_opponent_ratio;
  // 开局间距**随机化**（见 Config::start_gap_min 的实测说明）：
  // 固定间距会让策略只在一种距离上学到“怎么打”，换个窗口就失效。
  {
    Arena arena = match_.arena();
    const float span = std::max(0.0f, config_.start_gap_max - config_.start_gap_min);
    arena.start_gap = config_.start_gap_min + next_random() * span;
    match_.set_arena(arena);
  }
  frames_ = 0;
  episode_return_ = 0.0f;
  held_action_ = RlAction::Idle;
  attack_cooldown_left_ = 0;
  last_self_hp_ = match_.state(kSelfSide).hp;
  last_opp_hp_ = match_.state(1 - kSelfSide).hp;
  return observation();
}

auto RlEnv::observation() const -> Observation { return observe(match_, kSelfSide); }

auto RlEnv::step(RlAction action) -> StepResult {
  StepResult result{};
  if (match_.phase() == RoundPhase::Over) {
    // 已经结束：返回 done，且不给奖励（避免"死亡后继续刷分"）。
    result.done = true;
    return result;
  }

  // ——— 动作节流（见类注释：没有它训练必然退化）———
  //
  // `frame_skip` 决定策略“每几帧被问一次”；未被问到的帧**复用上次动作**
  // （等价于玩家按住某个方向键）。这样“一直按进攻”不再是免费连击，
  // 因为攻击还要过下一道 `attack_cooldown`。
  const int frame_skip = std::max(1, config_.frame_skip);
  const bool asking = frames_ % frame_skip == 0;
  RlAction effective = asking ? action : held_action_;
  if (asking) held_action_ = action;

  if (attack_cooldown_left_ > 0) {
    --attack_cooldown_left_;
    // 冷却期间**攻击类动作一律降级为待机**（方向/防御不受影响）。
    switch (effective) {
      case RlAction::Light:
      case RlAction::Heavy:
      case RlAction::Kick:
      case RlAction::Special:
        effective = RlAction::Idle;
        break;
      default: break;
    }
  }
  const FighterState& self_before = match_.state(kSelfSide);
  const float self_hp_before = self_before.hp;
  const float opp_hp_before = match_.state(1 - kSelfSide).hp;
  const float gap_before = std::abs(match_.state(1 - kSelfSide).x - self_before.x);

  Intent self_intent = action_intent(effective);
  // 把抽象招式翻译成**这个角色实际拥有的**招式（见 `profile_move`）。
  if (self_intent.attack.has_value()) {
    self_intent.attack = profile_move(match_.state(kSelfSide), *self_intent.attack);
  }
  Intent opp_intent = opponent_idle_ ? Intent{} : opponent_ai_.think(match_, 1 - kSelfSide);
  // 冷却按**招式自身的完整帧数**计（见 Config::attack_cooldown 的实测说明）：
  // 出招期间本就无法再次出招，冷却再压一层是防止“硬直一解就无缝接同一招”。
  const bool triggers_attack = self_intent.attack.has_value();
  const MoveId move = triggers_attack ? *self_intent.attack : MoveId::Idle;
  const FrameEvents events = match_.advance(self_intent, opp_intent);
  if (triggers_attack) {
    const FighterProfile& profile = fighter_profile(match_.state(kSelfSide).profile);
    const int move_frames = scaled_timing(move, profile.speed).total();
    attack_cooldown_left_ = config_.cooldown_from_move
                                ? std::max(config_.attack_cooldown,
                                           static_cast<int>(static_cast<float>(move_frames) *
                                                            config_.cooldown_ratio))
                                : config_.attack_cooldown;
  }
  ++frames_;

  const FighterState& self_after = match_.state(kSelfSide);
  const FighterState& opp_after = match_.state(1 - kSelfSide);
  const float self_hp_after = self_after.hp;
  const float opp_hp_after = opp_after.hp;

  const float dealt = std::max(0.0f, opp_hp_before - opp_hp_after);
  const float taken = std::max(0.0f, self_hp_before - self_hp_after);
  result.damage_dealt = dealt;
  result.damage_taken = taken;

  const float self_max = std::max(1.0f, self_after.max_hp);
  const float opp_max = std::max(1.0f, opp_after.max_hp);
  float reward = config_.reward.damage_dealt * (dealt / opp_max) -
                 config_.reward.damage_taken * (taken / self_max);

  for (const HitEvent& hit : events.hits) {
    const bool on_opponent = hit.victim == (1 - kSelfSide);
    if (on_opponent) {
      ++result.hits;
      if (hit.blocked) {
        ++result.blocked;
        reward += config_.reward.blocked_bonus;
      } else {
        reward += config_.reward.hit_bonus;
      }
    }
  }

  // 贴脸惩罚：只在"双方都还能动"时算（被打进硬直时贴脸是被迫的）。
  if (gap_before < config_.reward.crowding_distance && self_after.can_act()) {
    reward += config_.reward.crowding_penalty;
  }
  // 龟缩惩罚：按防御或后退且**本帧没挨打** —— 只在"无代价的保守"时计。
  //   真正用它挡下攻击（`taken > 0`）是正当策略，不该受罚。
  const bool passive = effective == RlAction::Guard || effective == RlAction::Backward;
  if (passive && taken <= 0.0f) reward += config_.reward.passive_penalty;
  // 空挥惩罚：**出招时距离已确定够不到**（用出招前的距离算）。
  //   为何用出招前的距离：招式判定是后续帧的事，但“要不要出这一招”
  //   是在出招瞬间决定的——奖励必须对应到那个决策。
  if (move != MoveId::Idle) {
    const float reach =
        attack_spec(move).reach_far * kBodyScale + 30.0f * kBodyScale;   // 触及 + 半身体
    if (gap_before > reach) reward += config_.reward.whiff_penalty;
  }
  // 消极惩罚：**所有招都够不到、而自己又没往前压** ⇒ 扣分。
  //   判据用“最长触及 + 半身”（而非逐招），因为这里的语义是
  //   “这一帧完全没有任何进攻可能”。
  if (effective != RlAction::Forward) {
    const float longest = attack_spec(MoveId::Special).reach_far * kBodyScale +
                          30.0f * kBodyScale;
    if (gap_before > longest * 1.15f) reward += config_.reward.passive_range_penalty;
  } else {
    // 反向：**已经贴到能打**却还在往前推 —— 既无收益又把帧浪费在起手前。
    const float lightest = attack_spec(MoveId::Light).reach_far * kBodyScale +
                           30.0f * kBodyScale;
    if (gap_before <= lightest) reward += config_.reward.push_penalty;
  }
  reward += config_.reward.step_penalty;

  // 终局判定：血量清零，或到时间按血量比。
  const bool ko = opp_after.hp <= 0.0f || self_after.hp <= 0.0f;
  const bool timeout = frames_ >= config_.max_frames;
  if (ko || timeout) {
    result.done = true;
    if (opp_after.hp <= 0.0f && self_after.hp > 0.0f) {
      result.won = true;
      reward += config_.reward.win_bonus;
    } else if (self_after.hp <= 0.0f && opp_after.hp > 0.0f) {
      result.lost = true;
      reward += config_.reward.lose_penalty;
    } else {
      // 平局（同归于尽或时间到且血量差 < 1%）。
      const float ratio_self = self_after.hp / self_max;
      const float ratio_opp = opp_after.hp / opp_max;
      if (std::abs(ratio_self - ratio_opp) < 0.01f) {
        result.draw = true;
        reward += config_.reward.draw_bonus;
      } else if (ratio_opp < ratio_self) {
        result.won = true;
        reward += config_.reward.win_bonus;
      } else {
        result.lost = true;
        reward += config_.reward.lose_penalty;
      }
    }
  }

  result.reward = reward;
  episode_return_ += reward;
  last_self_hp_ = self_hp_after;
  last_opp_hp_ = opp_hp_after;
  return result;
}

auto rl_schema_json() -> std::string {
  using st::Json;
  Json root = Json::object();
  root["version"] = 1;
  root["observation_dim"] = observation_dim();
  Json fields = Json::array();
  for (const ObservationField& field : kFields) {
    fields.push_back(Json{{"name", std::string(field.name)},
                          {"meaning", std::string(field.meaning)},
                          {"low", field.low},
                          {"high", field.high}});
  }
  root["observation"] = std::move(fields);
  Json actions = Json::array();
  for (int index = 0; index < static_cast<int>(kRlActionCount); ++index) {
    actions.push_back(Json{std::string(rl_action_name(rl_action_from_index(index)))});
  }
  root["actions"] = std::move(actions);
  root["action_count"] = kRlActionCount;
  root["reward"] = Json{{"damage_dealt", RewardConfig{}.damage_dealt},
                        {"damage_taken", RewardConfig{}.damage_taken},
                        {"hit_bonus", RewardConfig{}.hit_bonus},
                        {"win_bonus", RewardConfig{}.win_bonus},
                        {"lose_penalty", RewardConfig{}.lose_penalty},
                        {"step_penalty", RewardConfig{}.step_penalty}};
  root["max_frames"] = 60 * 40;
  return st::json_dump(root);
}

}  // namespace gedou

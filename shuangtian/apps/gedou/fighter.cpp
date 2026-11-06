#include "fighter.hpp"

#include <algorithm>
#include <array>

namespace gedou {

namespace {

/// 五套档案：速度型 / 均衡型 / 力量型 / 两个技能型。
///
/// 前三个共用一套招式骨架（差异只在数值与配色）；后两个各带**专属招式**。
/// 这正是这个示例想说明的两件事：同一套骨架能表达不同手感，
/// 而机制不同的角色也能跑在同一台状态机上。
constexpr std::array<FighterProfile, kFighterCount> kProfiles{{
    // 甜甜圈：血少、走得快，靠技能拉开距离而不是对拳。
    // 技能位：轻=真好吃（抛甜甜圈） 重=泡芙（扇喷） 必杀=摸腰（冻结+半血）
    {"donut", "甜甜圈", "甜到你发愣 · 技能型", math::Color{0xD9, 0x2B, 0x3A, 0xFF},
     math::Color{0xFF, 0xE1, 0x8A, 0xFF}, math::Color{0xF4, 0xC6, 0xA8, 0xFF}, 900.0f, 4.2f,
     0.92f, 0.94f,
     {MoveId::DonutToss, MoveId::PuffSpray, MoveId::WaistGrab},
     {"真好吃", "泡芙", "摸腰"}},
    // 户圣：慢一点、血厚一点，三个技能都不靠对拳。
    // 技能位：轻=苹果手机（天降） 重=苹果小区（护盾反击） 必杀=苹果人生（变身回血）
    {"husheng", "户圣", "苹安喜乐 · 忍耐型", math::Color{0x9A, 0x9E, 0xA6, 0xFF},
     math::Color{0x4E, 0x9A, 0x4A, 0xFF}, math::Color{0xE8, 0xC0, 0x9C, 0xFF}, 1050.0f, 3.4f,
     1.05f, 1.02f,
     {MoveId::AppleDrop, MoveId::AppleGuard, MoveId::AppleLife},
     {"苹果手机", "苹果小区", "苹果人生"}},
}};

/// 招式表。下标 = `MoveId`（`static_cast` 后取；非攻击项全 0）。
///
/// ## 距离字段为何不手填
///
/// `reach_*` 必须与**骨架真的伸到哪里**一致，否则会出现两种反向的假命中：
/// 盒比手远（空挥也命中）或手比盒远（看着打中了却没判上）。手填数字时
/// 改一个姿态就得记得同步改表（实测忘过一次，四个招式全偏）。
/// 所以这里由 `reach_of()` 从**命中帧的姿态**反算，手填的只是“起手距离”。
/// ## 为何用**命名初始化**而不是位置初始化
///
/// 这张表的字段会随玩法增加（最近一次加 `guard_damage` 时，位置初始化全部错位，
/// 编译器报的是“float 缩窄成 bool”而不是“缺一个字段”）。改用指定成员初始化后，
/// 新增字段只需在需要它的招式上写一行，其余自动取默认值。
constexpr AttackSpec kNoAttack{};

constexpr auto make_attacks() {
  std::array<AttackSpec, static_cast<std::size_t>(MoveId::Count)> table{};
  table[static_cast<std::size_t>(MoveId::Light)] = AttackSpec{
      .damage = 42.0f, .reach_near = 18.0f, .height_low = 58.0f, .height_high = 96.0f,
      .hitstun = 12, .knockback = 12.0f, .chip_ratio = 0.04f, .guard_damage = 18.0f,
      .meter_gain = 8.0f};
  table[static_cast<std::size_t>(MoveId::Heavy)] = AttackSpec{
      .damage = 96.0f, .reach_near = 16.0f, .height_low = 50.0f, .height_high = 100.0f,
      // 硬直必须**短于**自己的出招时长，否则“打中→对手硬直→自己已就绪”构成
      // 最优锁：任何对手都只能挨打（实测：玩家 900 血全程未损、无条件按踢 100% 胜）。
      .hitstun = 14, .knockback = 46.0f, .chip_ratio = 0.16f, .guard_damage = 10.0f,
      .meter_gain = 14.0f};
  table[static_cast<std::size_t>(MoveId::Kick)] = AttackSpec{
      .damage = 84.0f, .reach_near = 24.0f, .height_low = 24.0f, .height_high = 74.0f,
      .hitstun = 12, .knockback = 52.0f, .chip_ratio = 0.14f, .guard_damage = 40.0f,
      .meter_gain = 14.0f};
  table[static_cast<std::size_t>(MoveId::Special)] = AttackSpec{
      .damage = 170.0f, .reach_near = 16.0f, .height_low = 20.0f, .height_high = 104.0f,
      .hitstun = 26, .knockback = 52.0f, .knockdown = true, .chip_ratio = 0.30f,
      .unblockable = true, .guard_damage = 24.0f, .meter_gain = 0.0f};

  // ══════════ 甜甜圈 ══════════

  // 真好吃（抛甜甜圈）：本体不伤人，伤害全在投射物身上。
  // `reach_*` 很小——手里只有那个甜甜圈，看上去是“抛”，不是“挥”。
  table[static_cast<std::size_t>(MoveId::DonutToss)] = AttackSpec{
      .damage = 0.0f, .reach_near = 10.0f, .height_low = 90.0f, .height_high = 118.0f,
      .hitstun = 10, .knockback = 8.0f, .chip_ratio = 0.06f, .guard_damage = 12.0f,
      .meter_gain = 10.0f,
      .mechanic = MoveMechanic::Projectile,
      .projectile_speed = 9.0f, .projectile_life = 90};

  // 泡芙（扇形喷射）：扇形覆盖“中近距”，但**伤害偏低**——
  // 它的价值是多段判定与压制，不是一发打死人。
  // 扇形用多个判定帧采样（每帧一小段），所以单帧伤害小、总伤靠命中次数。
  table[static_cast<std::size_t>(MoveId::PuffSpray)] = AttackSpec{
      .damage = 26.0f, .reach_near = 20.0f, .height_low = 18.0f, .height_high = 86.0f,
      .hitstun = 8, .knockback = 6.0f, .chip_ratio = 0.12f, .guard_damage = 14.0f,
      .meter_gain = 8.0f,
      .mechanic = MoveMechanic::FanSpray,
      .fan_half_angle = 30.0f, .fan_range = 150.0f};

  // 摸腰（冻结 + 扣半血）：全场最强单招 —— 所以前摇/收招最长（见 `timing_for`），
  // 且判定距离**很短**（必须贴脸才能抓到）。半血是 `damage_ratio`（按对手最大血）。
  //
  // 为何不给 `unblockable`：能挡是它不成为“无脑必胜”的关键——
  // 挡下后只有削韧代价，而挠不到就得吃满 22 帧前摇的反击。
  table[static_cast<std::size_t>(MoveId::WaistGrab)] = AttackSpec{
      .damage = 40.0f, .damage_ratio = 0.5f, .reach_near = 14.0f, .height_low = 70.0f,
      .height_high = 104.0f,
      .hitstun = 20, .knockback = 10.0f, .chip_ratio = 0.0f, .guard_damage = 30.0f,
      .meter_gain = 0.0f,
      .mechanic = MoveMechanic::GripFreeze,
      .status = StatusKind::Frozen, .status_frames = 120};   // 2 秒 @60Hz

  // ══════════ 户圣 ══════════

  // 苹果手机（天降苹果）：先预警后落下——落点可躲（让对手有反应窗口）。
  // 伤害不高、但**击倒**：这是一个“制造距离/打断进攻”的招。
  table[static_cast<std::size_t>(MoveId::AppleDrop)] = AttackSpec{
      .damage = 58.0f, .reach_near = 0.0f, .height_low = 0.0f, .height_high = 200.0f,
      .hitstun = 16, .knockback = 20.0f, .knockdown = true, .chip_ratio = 0.20f,
      .guard_damage = 16.0f, .meter_gain = 12.0f,
      .mechanic = MoveMechanic::SkyDrop,
      .sky_warn_frames = 24, .sky_fall_speed = 22.0f};

  // 苹果小区（环绕护盾）：本身不伤人，给**自己**挂护盾——
  // 格挡并反弹。`damage` 设 0 是因为它不是攻击招，而是防御招。
  // `reflect_ratio` 取 0.6：反弹不过分强，否则“护盾一直开”就是无敌。
  table[static_cast<std::size_t>(MoveId::AppleGuard)] = AttackSpec{
      .damage = 0.0f, .reach_near = 0.0f, .height_low = 0.0f, .height_high = 0.0f,
      .hitstun = 0, .knockback = 0.0f, .chip_ratio = 0.0f, .guard_damage = 0.0f,
      .meter_gain = 6.0f,
      .mechanic = MoveMechanic::OrbitShield,
      .status = StatusKind::Shielded, .status_frames = 150,
      .shield_frames = 150, .reflect_ratio = 0.6f};

  // 苹果人生（变身）：3 秒回血 + 减伤 60%，但**完全不能动**——
  // 这是拿“3 秒的主动权”换血量，对手可以直接走过来打（虽然打得慢）。
  // 回血总量 = 1.4 × 180 帧 ≈ 252（约四分之一条血），不算少。
  table[static_cast<std::size_t>(MoveId::AppleLife)] = AttackSpec{
      .damage = 0.0f, .reach_near = 0.0f, .height_low = 0.0f, .height_high = 0.0f,
      .hitstun = 0, .knockback = 0.0f, .chip_ratio = 0.0f, .guard_damage = 0.0f,
      .meter_gain = 0.0f,
      .mechanic = MoveMechanic::Transform,
      .status = StatusKind::AppleForm, .status_frames = 180,   // 3 秒
      .transform_frames = 180, .regen_per_frame = 1.4f, .damage_reduction = 0.6f};

  return table;
}

constexpr auto kAttacks = make_attacks();

/// 命中帧的“最前点”（局部 x；取四肢最靠前者）——判定盒远端的**唯一真源**。
[[nodiscard]] auto strike_reach(MoveId move) -> float {
  const auto keys = keyframes_for(move);
  if (!keys.has_value()) return 0.0f;
  const Pose& pose = keys->strike;
  return std::max({pose.hand_front.x, pose.hand_back.x, pose.foot_front.x, pose.foot_back.x});
}

}  // namespace

auto fighter_profile(std::size_t index) -> const FighterProfile& {
  return kProfiles[std::min(index, kProfiles.size() - 1)];
}

auto fighter_move(std::size_t profile_index, std::size_t slot) -> MoveId {
  const FighterProfile& profile = fighter_profile(profile_index);
  return profile.moves[std::min(slot, profile.moves.size() - 1)];
}

auto fighter_move_label(std::size_t profile_index, std::size_t slot) -> std::string_view {
  const FighterProfile& profile = fighter_profile(profile_index);
  const std::size_t at = std::min(slot, profile.move_labels.size() - 1);
  if (!profile.move_labels[at].empty()) return profile.move_labels[at];
  return move_label(fighter_move(profile_index, slot));
}

auto attack_spec(MoveId move) -> AttackSpec {
  const auto index = static_cast<std::size_t>(move);
  if (index >= kAttacks.size()) return AttackSpec{};
  AttackSpec spec = kAttacks[index];
  // 距离字段由**姿态反算**（见 kAttacks 的注释）：非攻击招式没有命中帧，
  // `strike_reach` 返 0，而它们的 `reach_*` 本来就是 0，不受影响。
  if (is_attack(move)) spec.reach_far = strike_reach(move);
  return spec;
}

auto scaled_timing(MoveId move, float speed) -> MoveTiming {
  const MoveTiming base = timing_for(move);
  if (speed <= 0.0f) return base;
  const auto scale = [speed](int frames) {
    if (frames <= 0) return frames;
    const float scaled = static_cast<float>(frames) / speed;
    const int rounded = static_cast<int>(scaled + 0.5f);
    return rounded < 1 ? 1 : rounded;
  };
  return MoveTiming{scale(base.startup), scale(base.active), scale(base.recovery)};
}

}  // namespace gedou

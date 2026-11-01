#include "fighter.hpp"

#include <algorithm>
#include <array>

namespace gedou {

namespace {

/// 三套档案：速度型 / 均衡型 / 力量型。差异只体现在数值与配色上——
/// 姿态与招式骨架共用一套（同一个角色骨架系统能表达三种手感，是这个示例想说的）。
constexpr std::array<FighterProfile, kFighterCount> kProfiles{{
    {"swift", "凌云", "疾风步 · 速度型", math::Color{0x2E, 0x6F, 0xB5, 0xFF},
     math::Color{0xE8, 0xC7, 0x4C, 0xFF}, math::Color{0xE6, 0xB9, 0x8C, 0xFF}, 900.0f, 4.6f, 0.9f,
     0.82f},
    {"balanced", "磐石", "均衡 · 攻守兼备", math::Color{0x2F, 0x8F, 0x6A, 0xFF},
     math::Color{0xE0, 0xE0, 0xE0, 0xFF}, math::Color{0xD9, 0xA0, 0x74, 0xFF}, 1000.0f, 3.6f,
     1.0f, 1.0f},
    {"power", "赤霄", "重击 · 一击必倒", math::Color{0x9C, 0x35, 0x2F, 0xFF},
     math::Color{0x2A, 0x2A, 0x2E, 0xFF}, math::Color{0xC9, 0x93, 0x6E, 0xFF}, 1100.0f, 3.0f, 1.25f,
     1.18f},
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

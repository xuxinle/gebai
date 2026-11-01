#pragma once

/// 格斗示例 · 角色与招式**数据表**（数值全部集中在这里，改手感不必进逻辑）。
///
/// 为什么数值要单列成表：格斗游戏的"手感"几乎全由这些数（攻击距离 / 前摇 /
/// 伤害 / 受击硬直）决定，把它们散在状态机里时，调一次手感要读三处代码；
/// 而**测试也拿同一份表**（`tools/gedou_e2e.py` 的期望值来自这里的导出），
/// 于是"改了数值、测试还绿"这种漂移不会发生。

#include <array>
#include <cstdint>
#include <string_view>

#include "st/math/color.hpp"
#include "pose.hpp"

namespace gedou {

/// 招式的**机制**（除了“打一下”之外还做什么）。
///
/// 为何要做成标记而不是另写一套状态机：多数招式仍然是“一段判定 + 伤害”，
/// 只是其中少数额外带一种行为。用标记把它们挂到同一张表上，
/// 好处是帧数/硬直/削韧/槽这些通用字段仍然只维护一份。
enum class MoveMechanic : std::uint8_t {
  None = 0,
  /// 抛出一个**直线飞行的投射物**（甜甜圈）。命中/超时/格挡后消失。
  Projectile,
  /// **扇形喷射**：以角色为顶点的锥形多段判定（泡芙）。
  FanSpray,
  /// 强制拉近对方、冻结并扣**百分比**血（摸腰）。
  GripFreeze,
  /// 天上落苹果：先出现落点预警，延迟后命中（苹果手机）。
  SkyDrop,
  /// 召唤环绕护盾，格挡并**反弹**来袭伤害（苹果小区）。
  OrbitShield,
  /// 原地变形：持续回血 + 减伤，但完全不能动（苹果人生）。
  Transform,
};

/// 招式造成的**状态异常**（挂在受击方身上，按帧倒数）。
enum class StatusKind : std::uint8_t {
  None = 0,
  /// 冻结：完全不能行动（摸腰）。
  Frozen,
  /// 护盾：格挡全部正面/投掷伤害，并把其中一部分反弹给攻击方。
  Shielded,
  /// 苹果形态：每帧回血、受伤减免，但完全不能动。
  AppleForm,
};

/// 单个招式的战斗数值。
///
/// **单位是局部单位**（与 `pose.*` 的骨架同一套），使用时乘 `kBodyScale`——
/// 这样“拳伸到 x=52”“判定盒到 52”永远是同一个数，改角色大小时不必追改招式表。
struct AttackSpec {
  /// 伤害（血条刻度 = 角色 `max_hp`，缺省 1000）。
  float damage{0.0f};
  /// **百分比伤害**：按对手最大血量的比例扣（摸腰 50%）。
  /// 与 `damage` **相加**：两者都为 0 时就是纯控制招。
  float damage_ratio{0.0f};
  /// 判定盒在**朝向方向**上的起止距离（相对脚下的局部 x）。
  /// `reach_far` 由命中帧姿态反算（`strike_reach`），手填无效。
  float reach_near{0.0f};
  float reach_far{0.0f};
  /// 判定盒相对地面的高度带（局部 y：下沿 / 上沿）——高踢打不到蹲下的对手这类差异靠它。
  float height_low{0.0f};
  float height_high{0.0f};
  /// 命中后对方硬直帧数（越大越能接后续连段）。
  int hitstun{14};
  /// 击退距离（沿攻击方向）。
  float knockback{18.0f};
  /// 命中是否直接击倒（必杀/重击为真）。
  bool knockdown{false};
  /// 对手**防守时**仍受的伤害比例（0 = 完全防住）。
  float chip_ratio{0.0f};
  /// 强制破防（必杀：`unblockable`）——这类招式不受削韧影响。
  bool unblockable{false};
  /// 命中防守方时给对方的**削韧量**。
  ///
  /// 为何必需（实测代价）：只要 `chip_ratio == 0`，“一直按同一招”就因为
  /// 对方**永远只能挡、不能反击**而变成必胜策略（实测：无条件按蹬腿对
  /// easy/normal/hard 全为 **100% 胜率**，打满 1000 伤害）。
  /// 防御必须有代价——挨打会累积削韧，满了就破防。
  float guard_damage{0.0f};
  /// 命中时给攻击方回的必杀槽。
  float meter_gain{0.0f};

  // —— 机制扩展（默认都是“无”，不影响既有招式）——

  /// 本招带什么额外行为。
  MoveMechanic mechanic{MoveMechanic::None};
  /// 命中后给对手挂的异常状态与持续帧数。
  StatusKind status{StatusKind::None};
  int status_frames{0};
  /// 投射物飞行速度（局部单位/帧；仅 `Projectile` 用）。
  float projectile_speed{0.0f};
  /// 投射物最大存活帧数（超时消失）。
  int projectile_life{0};
  /// 扇形喷射的**半角**（度）与射程（仅 `FanSpray` 用）。
  float fan_half_angle{0.0f};
  float fan_range{0.0f};
  /// 天空落苹果的**预警帧数**（先显落点、后落下）与下落速度（仅 `SkyDrop` 用）。
  int sky_warn_frames{0};
  float sky_fall_speed{0.0f};
  /// 护盾持续帧数与**反弹比例**（仅 `OrbitShield` 用）。
  int shield_frames{0};
  float reflect_ratio{0.0f};
  /// 变身持续帧数、每帧回血、减伤比例（仅 `Transform` 用）。
  int transform_frames{0};
  float regen_per_frame{0.0f};
  float damage_reduction{0.0f};
};

/// 角色档案。
struct FighterProfile {
  std::string_view key;    ///< 稳定短名（属性面/测试用）
  std::string_view name;   ///< 显示名
  std::string_view title;  ///< 称号（选人条与胜利语用）
  math::Color gi;          ///< 道服主色
  math::Color accent;      ///< 配色（腰带/护腕）
  math::Color skin;
  float max_hp{1000.0f};
  /// 每帧移动速度（逻辑 px @60fps）。
  float walk_speed{3.6f};
  /// 被击退的倍率（重角色更难被推开）。
  float weight{1.0f};
  /// 起手/收招帧的整体倍率（<1 更快）。
  float speed{1.0f};

  /// 本角色的**三个攻击位**各自对应哪个招式。
  ///
  /// 为何每个角色自带一列而不是全局共用：新角色带来的是**新招式**
  /// （抛甜甜圈/冻结/护盾反弹），而不是“同一招改个数值”。共用一张表时
  /// 要么把通用三招全改掉（旧角色手感就变了），要么把它们硬塞进 `Special`
  /// 一个槽里（选人时就分不出招）。按角色列出来，旧角色一行不改。
  ///
  /// 顺序 = 界面上的三个技能位（轻/重/必杀键）。
  std::array<MoveId, 3> moves{MoveId::Light, MoveId::Heavy, MoveId::Special};

  /// 技能显示名（与 `moves` 一一对应；空 = 用招式本身的通用中文名）。
  std::array<std::string_view, 3> move_labels{};
};

/// 角色数量（选人与 `match` 的属性面据此枚举）。
inline constexpr std::size_t kFighterCount = 2;

[[nodiscard]] auto fighter_profile(std::size_t index) -> const FighterProfile&;
/// 招式数值（**按值返回**：距离字段由命中帧的姿态反算，见实现）。
[[nodiscard]] auto attack_spec(MoveId move) -> AttackSpec;
/// 某角色某个技能位（0/1/2）对应的招式。
[[nodiscard]] auto fighter_move(std::size_t profile_index, std::size_t slot) -> MoveId;
/// 某角色某个技能位的显示名（角色专属名，没有则回退到通用中文名）。
[[nodiscard]] auto fighter_move_label(std::size_t profile_index, std::size_t slot)
    -> std::string_view;

/// 招式各阶段帧数（已按角色 `speed` 折算）。
[[nodiscard]] auto scaled_timing(MoveId move, float speed) -> MoveTiming;

/// 必杀槽满值（攒满才能放必杀）。
inline constexpr float kMeterMax = 100.0f;
/// 每次必杀消耗。
inline constexpr float kMeterCost = 100.0f;
/// 受击回槽（攒必杀的另一半来源：打得越惨越能翻盘）。
inline constexpr float kMeterOnHurt = 6.0f;

}  // namespace gedou

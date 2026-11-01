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

/// 单个招式的战斗数值。
///
/// **单位是局部单位**（与 `pose.*` 的骨架同一套），使用时乘 `kBodyScale`——
/// 这样“拳伸到 x=52”“判定盒到 52”永远是同一个数，调大角色时不必追改招式表。
struct AttackSpec {
  /// 伤害（血条刻度 = 角色 `max_hp`，缺省 1000）。
  float damage{0.0f};
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
};

/// 角色数量（选人与 `match` 的属性面据此枚举）。
inline constexpr std::size_t kFighterCount = 3;

[[nodiscard]] auto fighter_profile(std::size_t index) -> const FighterProfile&;
/// 招式数值（**按值返回**：距离字段由命中帧的姿态反算，见实现）。
[[nodiscard]] auto attack_spec(MoveId move) -> AttackSpec;

/// 招式各阶段帧数（已按角色 `speed` 折算）。
[[nodiscard]] auto scaled_timing(MoveId move, float speed) -> MoveTiming;

/// 必杀槽满值（攒满才能放必杀）。
inline constexpr float kMeterMax = 100.0f;
/// 每次必杀消耗。
inline constexpr float kMeterCost = 100.0f;
/// 受击回槽（攒必杀的另一半来源：打得越惨越能翻盘）。
inline constexpr float kMeterOnHurt = 6.0f;

}  // namespace gedou

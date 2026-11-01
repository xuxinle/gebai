#pragma once

/// 格斗示例 · 姿态求解（纯函数，可被单元测试直接引用）。
///
/// 为什么姿态单独成一层：战斗状态机（`fight.hpp`）只该回答"谁在做什么招、
/// 已经过了几帧"，而"这一帧手脚该在哪"是**纯几何**——两者混在一起时，
/// 想断言"这一招真的伸出去了"就只能截图看，而截图不告诉你差在哪个关节。
///
/// 坐标约定（**局部空间**）：
/// - 原点在角色**双脚之间的地面点**，`x` 指向角色**正面**、`y` 向上；
/// - 因此左右朝向只影响"局部 → 世界"的那一步（`Pose::to_world`），
///   `pose_for` 永远只算一个朝向——镜像对称为真（测试直接断言它）。
/// - 世界坐标 `y` 向下（与画布一致），由 `to_world` 翻转。

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

#include "st/math/geometry.hpp"

namespace gedou {

/// `st::math` 的本地别名（本示例大量用 Point/Rect/Color；不写长前缀）。
namespace math = st::math;

/// 招式/状态标识。`Idle`..`Special` 是**主动作**，其余是受击与收场。
enum class MoveId : std::uint8_t {
  Idle = 0,
  Walk,
  Guard,
  Light,
  Heavy,
  Kick,
  Special,
  Hurt,
  KnockDown,
  Victory,
  Defeat,
  Count,
};

/// 稳定短名（控制通道属性面/日志/测试断言用；不要拿枚举序号，那会随枚举顺序变）。
[[nodiscard]] auto move_name(MoveId move) -> std::string_view;
/// 中文名（界面提示用）。
[[nodiscard]] auto move_label(MoveId move) -> std::string_view;
/// 短名 → 招式（认不出返回 `nullopt`；不静默回退，调用方要能报错）。
[[nodiscard]] auto move_from_name(std::string_view name) -> std::optional<MoveId>;
/// 主动作（能被玩家/AI 发起的那些）。
[[nodiscard]] constexpr auto is_attack(MoveId move) noexcept -> bool {
  return move == MoveId::Light || move == MoveId::Heavy || move == MoveId::Kick ||
         move == MoveId::Special;
}

/// 骨架坐标的**全局缩放**（局部单位 → 世界像素）。
///
/// 为何单独一个常量：招式表（`fighter.*`）用**同一套局部单位**写攻击距离/高度，
/// 世界坐标 = 局部值 × 本常量。改角色大小时只需改这一处，
/// 而“手臂伸到 74 局部单位”与“判定盒到 118 世界像素”也不必各自维护一份。
inline constexpr float kBodyScale = 1.6f;

/// 骨架关节（局部空间；见文件头坐标约定）。
struct Pose {
  math::Point hip{};
  math::Point chest{};
  math::Point head{};
  /// 肩点（前/后）。**手背必须从肩出发**，不能从躯干中线——
  /// 两条手臂同源时视觉上会糊成一坠（实测截图里“手臂不见了”）。
  math::Point shoulder_front{};
  math::Point shoulder_back{};
  /// 前臂：肩点 → 肘 → 手。
  math::Point elbow_front{};
  math::Point hand_front{};
  /// 后臂。
  math::Point elbow_back{};
  math::Point hand_back{};
  /// 前腿：髋 → 膝 → 足。
  math::Point knee_front{};
  math::Point foot_front{};
  /// 后腿。
  math::Point knee_back{};
  math::Point foot_back{};
  float head_radius{19.0f};
  /// 躯干倾斜（弧度，正 = 前倾）：只影响绘制时的护甲/腰带这类贴图件，
  /// 骨架点本身已经包含倾斜，所以它不是"必填"字段。
  float lean{0.0f};

  /// 局部 → 世界（`facing_right` 决定 `x` 镜像；`ground_y` 是世界坐标的地面线）。
  /// 同时施加 `kBodyScale`（骨架大小）。
  [[nodiscard]] auto to_world(float feet_x, float ground_y, bool facing_right) const -> Pose;};

/// 关节插值（逐点线性）；`t` 由调用方夹到 [0,1]。
[[nodiscard]] auto lerp_pose(const Pose& from, const Pose& to, float t) -> Pose;

/// 求某一帧的姿态。
///
/// `frame`：**当前动作已过的帧数**（0 = 动作首帧）。
/// `move_progress`：仅 `Walk` 用（步态相位，0~1 循环）。
[[nodiscard]] auto pose_for(MoveId move, int frame, float walk_phase = 0.0f) -> Pose;

/// 招式关键帧（起手 / 出招）；`Startup`/`Active`/`Recovery` 之外的招式返回 `nullopt`。
struct Keyframes {
  Pose wind{};
  Pose strike{};
};
[[nodiscard]] auto keyframes_for(MoveId move) -> std::optional<Keyframes>;

/// 招式各阶段帧数（起手 / 判定 / 收招）；非主动作返回全 0。
struct MoveTiming {
  int startup{0};
  int active{0};
  int recovery{0};
  [[nodiscard]] constexpr auto total() const noexcept -> int { return startup + active + recovery; }
};
[[nodiscard]] auto timing_for(MoveId move) -> MoveTiming;

}  // namespace gedou

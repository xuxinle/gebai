#pragma once

/// 格斗示例 · **骨骼命名映射表**（把"语义关节"翻到具体模型的关节名）。
///
/// ## 为什么需要这一层
///
/// 每个 3D 模型给骨骼起的名字都不一样，而游戏逻辑只关心 14 个语义位置
/// （髋 / 膝 / 踝 / 腰 / 胸 / 肩 / 肘 / 腕 / 颈 / 头）。换一个模型就要改一遍名字。
///
/// 把它抽成一张表，好处是：**换模型只改这张表，驱动代码一个字不动**。
/// 早先名字硬编码在 `fighter_rig.cpp` 的骨骼循环里，换模型要先读懂那段
/// 反射式代码才敢动——那是"能跑但改不动"的形态。
///
/// ## 已支持的命名风格
///
/// | 风格 | 例子 | 典型来源 |
/// |---|---|---|
/// | CesiumMan | `Skeleton_arm_joint_L__4_` | Cesium 示例资产 |
/// | Quaternius | `LeftArm` / `RightForeArm` | quaternius.com（CC0） |
/// | Mixamo | `mixamorig:LeftArm` | Adobe Mixamo |
/// | Ready Player Me | `LeftArm` / `LeftHand` | readyplayer.me |
/// | 通用（Blender 默认） | `upper_arm.L` / `forearm.R` | Blender Rigify |
///
/// ## 匹配规则（宽松但有优先级）
///
/// 每条映射给一组**候选名**，按顺序找第一个存在的。这样同一个语义
/// （比如"左大臂"）能同时兼容 `LeftArm` / `mixamorig:LeftArm` / `upper_arm.L`，
/// 而不必为每种风格各写一套表。

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace gedou {

/// 语义关节（游戏逻辑关心的那些）。顺序即驱动顺序。
enum class Joint : std::uint8_t {
  Hip = 0,        ///< 骨盆 / 髋（躯干根）
  Spine,          ///< 脊柱中段（腰→胸的中间）
  Chest,          ///< 胸 / 肩线
  Neck,           ///< 颈
  Head,           ///< 头
  ShoulderL,      ///< 左肩
  ElbowL,         ///< 左肘
  HandL,          ///< 左腕 / 手
  ShoulderR,
  ElbowR,
  HandR,
  HipL,           ///< 左髋（腿根）
  KneeL,
  FootL,
  HipR,
  KneeR,
  FootR,
  Count,
};

/// 一个语义关节的候选名（按优先级）。
struct JointAlias {
  Joint joint{Joint::Hip};
  /// 候选关节名（大小写不敏感匹配；`mixamorig:` 前缀会被剥掉再比）。
  std::vector<std::string_view> names{};
};

/// 骨骼映射表：一组别名 + 该模型"人形有多高"的参考值。
struct SkeletonMap {
  std::vector<JointAlias> aliases{};
  /// 模型的髋→颈跨度（米）。用于把 2D 骨架的动作幅度换算到模型尺度。
  ///
  /// 为什么必须逐模型给：不同模型的身高差很大（CesiumMan 1.24m、
  /// Quaternius 约 1.8m）。用同一个常量会让动作幅度失真
  /// （实测踩过：CesiumMan 上正确的值套到别的模型上，角色被拉长 1.5 倍）。
  float hip_to_neck{0.46f};

  /// 按语义查实际关节名；找不到返回空串。
  [[nodiscard]] auto find(Joint joint, const std::vector<std::string>& available) const
      -> std::string;

  /// 从"模型实际有的关节名"里推断映射（用于未知模型）。
  ///
  /// 推断规则：按常见命名习惯匹配（`left`/`l` 前缀或后缀、`arm`/`leg`/`spine` 等词根）。
  /// 推断失败时返回的映射里该关节为空串——调用方应把它当作"该关节不驱动"，
  /// 而不是硬塞一个错的（错关节会让整条骨骼链歪掉，比不驱动更难查）。
  [[nodiscard]] static auto infer(const std::vector<std::string>& available) -> SkeletonMap;
};

/// 内置映射：CesiumMan（当前用的示例资产）。
[[nodiscard]] auto cesium_man_map() -> const SkeletonMap&;

/// 内置映射：Quaternius / Mixamo / Ready Player Me 这一类"标准人形命名"。
///
/// 这三家的关节名高度一致（都是 `Hips/Spine/Chest/Neck/Head/LeftArm/...`），
/// 用同一张表就能覆盖，不必各写一份。
[[nodiscard]] auto standard_humanoid_map() -> const SkeletonMap&;

}  // namespace gedou

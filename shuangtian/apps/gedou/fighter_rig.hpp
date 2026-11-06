#pragma once

/// 格斗示例 · **高质量角色装配**（glTF 真人本体 + 程序化服装）。
///
/// ## 为什么是"本体 + 服装"两段
///
/// 真人身体（皮肤、脸、手、体态）是**建模资产**——代码写不出来，
/// 所以用 glTF（`assets/models/gltf/CesiumMan.glb`，3273 顶点 / 19 关节 / 带贴图）。
/// 而两个角色的**识别特征**（美国国旗棒球帽、红色运动夹克、亮片眼镜、灰 polo）
/// 是几何与配色，可以程序化生成并**挂到本体骨骼上**。
///
/// ## 姿态怎么来（这是本文件的核心问题）
///
/// 格斗逻辑用的是 2D 骨架（`pose.cpp`，判定盒从它算）。而 glTF 本体要的是
/// **每根骨骼的 3D 旋转**。两者之间本文件做一次映射：
///
/// `2D 关节世界坐标` → `每根骨骼的方向向量` → `相对绑定姿势的旋转`
///
/// 也就是说：**不使用 glTF 自带的动画**，而是把现有的战斗姿态"翻译"成骨骼旋转。
/// 这样动画与判定**永远同源**——拳伸到哪里、判定盒就到哪里，不会出现
/// "看着打中了、判定没中"（反过来也一样）。
///
/// ## 关节名（CesiumMan 的实际命名，大小写与下划线都不可改）
///
/// | 语义 | glTF 关节名 | 绑定位置 (x,y,z) |
/// |---|---|---|
/// | 髋（右/左） | `leg_joint_R_1` / `leg_joint_L_1` | ±0.073, 0.614 |
/// | 膝 | `leg_joint_*_2` | ±0.078, 0.352 |
/// | 踝 | `leg_joint_*_3` | ±0.084, 0.086 |
/// | 腰 | `Skeleton_torso_joint_1` | 0.005, 0.679 |
/// | 胸 | `Skeleton_torso_joint_2` | 0.005, 0.824 |
/// | 肩线 | `torso_joint_3` | 0.005, 1.074 |
/// | 肩（右/左） | `Skeleton_arm_joint_R` / `_L__4_` | ±0.09, 1.074 |
/// | 肘 | `Skeleton_arm_joint_*__2_` / `__3_` | ±0.30, 0.965 |
/// | 腕 | `Skeleton_arm_joint_*__3_` / `__2_` | ±0.45, 0.875 |
/// | 颈 | `Skeleton_neck_joint_1` | 0.005, 1.138 |
/// | 头 | `Skeleton_neck_joint_2` | 0.005, 1.190 |

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "fighter.hpp"
#include "pose.hpp"
#include "procedural_texture.hpp"
#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/math/matrix.hpp"
#include "st/raster/gltf.hpp"
#include "st/raster/mesh.hpp"

namespace st::raster {
class Scene3D;
}

namespace gedou {

/// 骨架像素 → 3D 米的比例。
///
/// 160：一个身高约 300 世界像素的角色刚好 1.87 米。
/// 实际换算时纵向用**骨骼跨度归一**（见 `draw` 的 ⓪），
/// 这个常量只用于横向（x）位置。
inline constexpr float kPixelPerMeter = 160.0f;

/// 角色的**外观定义**（服装配色 + 特征件）。数据驱动，加角色只加一个常量。
struct FighterLook {
  math::Color skin{0xE8, 0xB4, 0x92, 0xFF};
  math::Color torso{0xD9, 0x2B, 0x3A, 0xFF};          ///< 上衣主色
  math::Color torso_accent{0xFA, 0xFA, 0xFA, 0xFF};   ///< 上衣点缀（条纹/门襟）
  math::Color legs{0x2E, 0x34, 0x42, 0xFF};           ///< 裤子
  math::Color shoes{0xF2, 0xF2, 0xF2, 0xFF};
  math::Color hair{0x24, 0x1E, 0x1A, 0xFF};
  /// 头部特征件：`has_cap` = 棒球帽（甜甜圈），`has_glasses` = 亮片眼镜（户圣）。
  bool has_cap{false};
  math::Color cap_base{0x1F, 0x3A, 0x93, 0xFF};
  math::Color cap_stripe{0xBF, 0x0A, 0x30, 0xFF};
  bool has_glasses{false};
  math::Color glasses_frame{0xC8, 0xA8, 0x50, 0xFF};
  /// 袖子长度：true = 长袖（夹克），false = 短袖（polo）。
  bool long_sleeves{true};
};

/// 姿态像素 → 模型米的换算比例（**全局唯一**）。
///
/// 这个比例必须同时用于：骨骼位置、服装挂点、相机取景。
/// 分开算过一次，结果是"相机按一套尺度、角色按另一套"——
/// 表现为角色偏离自己该在的位置、甚至被拉变形。
///
/// 取 `0.459 / 300`：模型髋→颈 0.459 米，2D 姿态同跨度约 300 像素。
constexpr float kPoseToModelScale = 0.459f / 300.0f;

/// 角色的外观（按档案顺序；与 `fighter_profile` 一一对应）。
[[nodiscard]] auto fighter_look(std::size_t profile_index) -> const FighterLook&;

/// 装配好的角色：glTF 本体 + 程序化服装，共用一套骨骼姿态。
class FighterRig {
 public:
  /// 从 glTF 文件建装配（两个角色各一份；服装配色由 `look` 决定）。
  ///
  /// 失败返回 `nullptr`（模型文件缺失/损坏）——调用方应退化成"纯程序化模型"，
  /// 而不是崩掉：没有美术资产不该让游戏不能玩。
  [[nodiscard]] static auto create(std::string_view gltf_path, const FighterLook& look)
      -> std::shared_ptr<const FighterRig>;

  /// 按这一帧的 2D 姿态画到场景里。
  ///
  /// `origin_x` / `ground_y` 是角色脚下的世界像素坐标；`facing_right` 决定镜像。
  /// 按这一帧的 2D 姿态画到场景里。
  ///
  /// `pixels_per_meter` 必须与相机用的**同一个值**——相机与角色各用一套尺度时，
  /// 角色会偏离它该在的位置（表现为"人站在场地外面"或"两个人叠在一起"）。
  void draw(st::raster::Scene3D& scene, const Pose& pose, float origin_x, float ground_y,
            bool facing_right, float pixels_per_meter) const;

  [[nodiscard]] auto valid() const noexcept -> bool { return valid_; }
  /// 本体的关节数（测试/诊断用）。
  [[nodiscard]] auto joint_count() const noexcept -> std::size_t { return skin_.joints.size(); }

 private:
  struct GarmentPiece {
    st::raster::Mesh mesh;
    /// 绑到哪根骨骼（用关节名，避免下标随模型改动而错位）。
    std::string joint{};
    /// 相对该骨骼的偏移（骨骼空间，米）。
    math::Vec3 offset{};
    /// 附加旋转（弧度，绕 Z）：用于帽檐前伸、镜腿横放这类朝向。
    float yaw{0.0f};
  };

  bool valid_{false};
  st::raster::GltfScene body_{};

  // —— 程序化贴图（替换 glTF 自带的品牌色贴图）——
  //
  // 为什么必须换：CesiumMan 自带的是**蓝绿品牌图案**（Cesium 的标志色），
  // 直接用它角色像穿了迷彩服。生成一次就缓存（生成是逐像素的，每帧做会掉帧）。
  TextureBitmap skin_texture_{};
  TextureBitmap cloth_texture_{};
  TextureBitmap cap_texture_{};
  TextureBitmap lens_texture_{};

  st::raster::Skeleton skin_{};
  std::vector<GarmentPiece> garments_{};
  FighterLook look_{};
};

}  // namespace gedou

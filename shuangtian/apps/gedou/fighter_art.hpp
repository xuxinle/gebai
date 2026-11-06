#pragma once

/// 格斗示例 · **2D 矢量角色**（取代之前的 3D 低模）。
///
/// ## 为什么是 2D
///
/// 3D 路线试过：glTF 蒙皮管线打通了，但资产是 CesiumMan（3273 顶点、无 UV 分区、
/// 贴图是品牌图案），怎么改都是"色块人"。**质量上限在资产，不在渲染**。
///
/// 2D 矢量正好相反：轮廓、色块、描边、体积层次**全在我手里**，
/// 能做到真正的精致——而且格斗游戏本来就是 2D 的传统形态
///（街霸/拳皇/罪恶装备都是）。判定的 2D 骨架也是现成的，视觉与判定天然同源。
///
/// ## 质感从哪来（这几样缺一样就"扁平"）
///
/// | 手法 | 作用 | 缺了会怎样 |
/// |---|---|---|
/// | **外描边**（暗色粗线） | 把角色从背景里"拔"出来 | 与背景糊在一起，尤其夜景 |
/// | **双段肢体**（上宽下窄 + 关节球） | 手臂像手臂而不是两根棍 | 像火柴人 |
/// | **明暗两色**（受光面亮、背光面暗） | 体积感 | 像剪纸 |
/// | **衣褶线** | 布料的"呼吸" | 像硬塑料 |
/// | **落地阴影**（随跳跃缩放） | 与地面的关系 | 像贴纸浮空 |
///
/// ## 坐标
///
/// 全部输入是**世界坐标**（`Pose::to_world` 之后），与判定盒同一套空间。
/// 内部不做任何缩放——`kBodyScale` 已经由 `to_world` 施加过。

#include <cstdint>
#include <string_view>

#include "pose.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/surface.hpp"

namespace gedou {

/// 角色配色（2D 版；与招式特效的颜色区分开）。
struct FighterPalette {
  math::Color skin{0xF2, 0xC8, 0xA8, 0xFF};
  math::Color skin_shadow{0xD4, 0xA2, 0x82, 0xFF};   ///< 受光面的暗部（体积感来源）
  math::Color cloth{0xD9, 0x2B, 0x3A, 0xFF};         ///< 上衣主色
  math::Color cloth_shadow{0xA8, 0x1C, 0x2C, 0xFF};  ///< 上衣暗部
  math::Color cloth_accent{0xFA, 0xFA, 0xFA, 0xFF};  ///< 条纹/门襟
  math::Color pants{0x2E, 0x34, 0x42, 0xFF};
  math::Color pants_shadow{0x1C, 0x20, 0x2A, 0xFF};
  math::Color shoes{0xF2, 0xF2, 0xF2, 0xFF};
  math::Color hair{0x24, 0x1E, 0x1A, 0xFF};
  /// 外描边色（一般是主色的极暗版；夜景里要够深才"拔得出来"）。
  math::Color outline{0x14, 0x10, 0x18, 0xFF};

  bool has_cap{false};
  math::Color cap_base{0x1F, 0x3A, 0x93, 0xFF};
  math::Color cap_stripe{0xBF, 0x0A, 0x30, 0xFF};
  bool has_glasses{false};
  math::Color glasses_frame{0xC8, 0xA8, 0x50, 0xFF};
  bool long_sleeves{true};

  /// 角色整体标志色（血条/卡片/特效染色共用）。
  math::Color signature{0xD9, 0x2B, 0x3A, 0xFF};
};

/// 取角色的 2D 配色（按档案顺序；与 `fighter_profile` 一一对应）。
[[nodiscard]] auto fighter_palette(std::size_t profile_index) -> const FighterPalette&;

/// 绘制选项（每帧由 `GameView` 提供）。
struct FighterDrawOptions {
  /// 受击闪白强度（0 = 正常，1 = 全白）。
  float flash{0.0f};
  /// 落地阴影不透明度（跳跃时变淡变小）。
  float shadow_alpha{0.35f};
  float shadow_scale{1.0f};
  /// 整体透明度（隐身/复活无敌等效果可用）。
  float alpha{1.0f};
  /// 是否朝向右侧（决定侧脸、发丝、衣褶的偏置方向）。
  bool facing_right{true};
};

/// 画一个角色（含阴影）。`canvas` 已是世界坐标（`GameView` 把相机变换设好）。
void draw_fighter(st::raster::Surface& canvas, const Pose& pose, const FighterPalette& palette,
                  const FighterDrawOptions& options);

}  // namespace gedou

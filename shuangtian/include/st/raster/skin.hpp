#pragma once

/// @file skin.hpp
/// @brief **骨骼蒙皮**：骨架、逆绑定矩阵、逐顶点线性混合蒙皮（LBS）。
///
/// ## 为什么需要它（刚性父子不够用）
///
/// `scene_graph` 的父子链是**刚体**继承：子节点整体跟着父节点走。
/// 对机械臂（两段硬棍 + 关节球）这是对的，因为关节处本来就该有缝。
/// 但**角色**不一样：手臂弯曲时肩部皮肤要**连续变形**，不能出现一条裂缝。
///
/// 蒙皮的原理只有一句话：
///
/// ```text
/// 顶点位置 = Σ wᵢ · (骨骼ᵢ的世界矩阵 · 逆绑定矩阵ᵢ) · 顶点位置
/// ```
///
/// - `逆绑定矩阵`（inverse bind matrix）把顶点从**模型空间**搬到骨骼的局部空间；
/// - `骨骼世界矩阵` 再把它搬回当前姿态下的模型空间；
/// - 两者合起来就是"这根骨骼当前的形变"，多个骨骼按权重加权平均。
///
/// ## 与 glTF / Blender 的对应
///
/// | 本模块 | glTF 2.0 | Blender |
/// |---|---|---|
/// | `Skeleton::joints` | `skin.joints[]` | Armature 的 Bones |
/// | `Skeleton::inverse_bind` | `skin.inverseBindMatrices` | `bone.matrix_local` 的逆 |
/// | `Mesh::skins` | `JOINTS_0` / `WEIGHTS_0` | 顶点组的权重 |
/// | `skin_mesh()` | 播放器逐帧做 | Armature Modifier |
///
/// ## 一条必须说清的限制
///
/// 本实现是 **LBS（线性混合蒙皮）**：两个骨骼影响同一个顶点时按权重线性加权。
/// 它在关节**大幅扭转**时会出现"糖纸效应"（体积塌陷、边缘捏扁）。
/// 双四元数（DQS）能解决，但成本翻倍且 glTF 不表达它——
/// 所以这里选 LBS（与所有引擎的实际行为一致），并把它写进文档而不是假装没有。
///
/// ## 坐标系约定
///
/// - 顶点位置在**模型空间**（= 网格被创建时的坐标）；
/// - `inverse_bind` 把模型空间 → 骨骼空间；
/// - 关节的世界矩阵来自 `scene_graph::resolve`（已含父链与动画采样）；
/// - 骨骼名必须与场景节点名对应（解析时校验）。

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"
#include "st/math/matrix.hpp"
#include "st/raster/mesh.hpp"
#include "st/raster/scene_graph.hpp"

namespace st::raster {

/// 一副骨架：关节名 + 逆绑定矩阵。
///
/// ## 为什么存名字而不是直接存矩阵
///
/// 逆绑定矩阵在**绑定那一刻**就固定了（模型做好时的姿态），而世界矩阵每帧都变
/// （`scene_graph::resolve` 算出来）。所以骨架只存"不变的那一半"，并在求值时
/// 按名字去当前的世界矩阵表里查——这样同一副骨架能被任意多份姿态复用。
struct Skeleton {
  /// 关节名（与场景节点名对应；索引 = `Mesh::skins` 里的下标）。
  std::vector<std::string> joints{};
  /// 逆绑定矩阵（与 `joints` 一一对应）：模型空间 → 该关节的绑定空间。
  ///
  /// 全零/空时按**单位阵**处理（即"顶点已在关节空间里"，用于手搓测试）。
  std::vector<math::Mat4> inverse_bind{};

  [[nodiscard]] auto joint_count() const noexcept -> std::size_t { return joints.size(); }
  /// 第 `index` 个关节的逆绑定矩阵；越界或缺失返回单位阵。
  [[nodiscard]] auto bind_matrix_at(std::size_t index) const noexcept -> math::Mat4;
  /// 名字 → 关节下标；找不到返回 -1。
  [[nodiscard]] auto index_of(std::string_view name) const noexcept -> int;
};

/// 绑定姿态辅助：由"当前世界矩阵"反推逆绑定矩阵。
///
/// ## 什么时候用
///
/// 建模时网格就在**绑定姿态**（通常是 T-pose / A-pose）下生成，
/// 此时每个关节的世界矩阵就是它的绑定矩阵，逆绑定矩阵 = 它的逆。
/// 这个函数把"取逆"集中成一处——`Mat4` 没有通用求逆（只有平移/旋转/缩放构成的可逆性），
/// 而绑定矩阵恰好是这三者的乘积，所以逆也只需要这三次。
///
/// 退化（行列式为 0）时返回单位阵：宁可"不形变"，也不要 NaN 扩散。
[[nodiscard]] auto inverse_bind_from_world(const math::Mat4& world) noexcept -> math::Mat4;

/// 求值结果：每个关节的**蒙皮矩阵** = `世界矩阵 · 逆绑定矩阵`。
struct SkinMatrices {
  std::vector<math::Mat4> matrices{};
  /// 关节名（与 `matrices` 同序），用于报错与调试。
  std::vector<std::string> joint_names{};
};

/// 由骨架 + 当前世界矩阵表算出所有蒙皮矩阵。
///
/// `world_of` 是一个"查名字拿世界矩阵"的回调——用回调而不是 map：
/// 调用方手里往往已经有 `ResolvedTree`（按名字查），再建一份 map 是白抄一遍。
///
/// 失败：骨架里的关节名在场景里找不到（`NotFound`，消息里带**哪个关节**）——
/// 这条必须报错而不是静默用单位阵，否则症状是"模型整个塌到原点"，很难定位。
[[nodiscard]] auto compute_skin_matrices(
    const Skeleton& skeleton, const ResolvedTree& tree) -> Result<SkinMatrices>;

/// 用蒙皮矩阵把一个网格**形变**成新网格（逐顶点 LBS）。
///
/// - 没有 `skins` 的网格原样返回（静态网格）；
/// - 法线用同一蒙皮矩阵的 3×3 部分变换后重新归一化（**不**重新算面法线：
///   那会抹掉原有的软硬边区分）；
/// - UV 与顶点色原样保留。
///
/// 返回**新网格**而不是就地改：网格常被多份姿态共享（同一角色多帧），
/// 就地改会让"上一帧的形变"累积到下一帧。代价是一次顶点拷贝（几千顶点 ≈ 几十 KB）。
[[nodiscard]] auto skin_mesh(const Mesh& mesh, const SkinMatrices& matrices) -> Mesh;

/// 把每顶点影响**限制到 4 个**并按权重归一化。
///
/// ## 为什么必须归到 4 个
///
/// `Mesh::skins` 的布局是固定 4 槽（GPU 顶点着色器不能吃可变长数组，glTF 的
/// `JOINTS_0` 也是 4 个）。超过 4 个影响时取权重最大的四个——那些被丢掉的影响
/// 通常只有 1~2% 的权重，视觉上看不出差别（这也是行业惯例）。
///
/// `epsilon` 以下的影响直接丢弃并重新归一化，避免"四个极小权重"这种退化情形。
/// 全零权重的顶点保持全零（视为不受骨骼影响）。
[[nodiscard]] auto limit_to_four_influences(const std::vector<std::pair<std::uint32_t, float>>& influences,
                                           float epsilon = 1.0e-4f)
    -> std::array<float, 8>;

/// 生成**测试/示例用**的骨架权重：按顶点到关节的接近程度自动分配。
///
/// ## 这不是"自动绑定"（别当生产工具用）
///
/// 真正的自动绑定（heat diffusion / bone glow）是一整个课题，输出质量取决于
/// 网格拓扑与关节摆放。这个函数只做一件朴素的事：**离哪个关节近就给谁权重**，
/// 并在两个关节之间做平滑过渡。它足够让示例动起来、让测试有可验证的输入，
/// 但角色的真实绑定应当在 Blender 里刷权重。
///
/// `falloff` 是过渡带宽度（世界单位）；0 时退化成"硬分配"（每个顶点只属于最近关节）。
[[nodiscard]] auto auto_weights(const Mesh& mesh, const Skeleton& skeleton,
                                const ResolvedTree& tree, float falloff = 0.12f) -> Mesh;

}  // namespace st::raster

#pragma once

/// @file pick.hpp
/// @brief **场景级拾取与碰撞**：把 `collision` 的原语接到组合模型上。
///
/// ## 解决的问题
///
/// `math::collision` 只给原语（射线×盒、射线×三角形）。真实用法是：
///
/// - 「鼠标点在屏幕某处，命中了哪个零件？」（需要：屏幕 → 世界射线 → 逐物体求交）
/// - 「这两个模型是否穿插？」（需要：变换到同一空间 + AABB + 三角形）
/// - 「我的视线被谁挡住了？」（需要：取最近命中）
///
/// 这些都需要**知道节点树的当前姿态**（世界矩阵）与每个节点的几何。
/// 本模块把三者接起来，并保证：命中结果**带节点路径**，能直接拿去改。
///
/// ## 宽相 / 窄相的边界
///
/// | 阶段 | 判据 | 何时使用 |
/// |---|---|---|
/// | 宽相 | 射线 × **世界空间 AABB** | 永远先做（一次 6 次比较 vs 上千次三角形） |
/// | 窄相 | 射线 × **三角形**（在模型空间） | 宽相通过后 |
///
/// 窄相在**模型空间**做（把射线用逆矩阵搬进去），而不是把三角形搬到世界空间：
/// 后者每个三角形要 12 次乘法，前者每个物体只有一次矩阵求逆。

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "st/math/collision.hpp"
#include "st/math/matrix.hpp"
#include "st/raster/mesh_ops.hpp"
#include "st/raster/scene_graph.hpp"

namespace st::raster {

/// 一个可被拾取的物体（几何 + 当前世界矩阵）。
///
/// 为什么是"扁平列表 + 自带矩阵"而不是直接吃 `SceneNode`：
/// 网格不在节点里（节点只管空间），而且同一个网格常被多个节点引用
/// （实例化）。让调用方自己组装这个列表，比让本模块去反推"哪个网格配哪个节点"
/// 可靠得多——那个反推在本仓的调用面里根本不存在（`scene3d_script` 是现算网格）。
struct PickObject {
  /// 节点名（命中结果里带出去，便于"点到的是哪个零件"）。
  std::string name{};
  /// 当前姿态的世界矩阵（来自 `SceneNode` 的 resolve 结果）。
  math::Mat4 world{math::Mat4::identity()};
  /// 几何（**绑定/模型空间**：顶点未经 `world` 变换）。
  const Mesh* mesh{nullptr};
  /// 世界空间 AABB（宽相用）。空盒（`min > max`）= 跳过。
  Bounds world_bounds{};
};

/// 一次命中的结果。
struct PickHit {
  /// 命中的物体名。
  std::string name{};
  /// 沿射线的参数（**以输入射线的方向长度为单位**；射线已归一化时即距离）。
  float distance{0.0f};
  /// 世界空间命中点。
  math::Vec3 point{};
  /// 世界空间命中面法线（已归一化；**朝射线来向**，即与 `ray.direction` 点积 < 0）。
  ///
  /// 为什么要翻转：渲染的法线朝向取决于顶点法线与绕序，而拾取时调用方
  /// 总是想"把物体推到射线这一侧"。不统一朝向的话，同一个物体会时正时反。
  math::Vec3 normal{};
  /// 命中三角形的顶点下标（可用于查 UV / 顶点色 / 关节权重）。
  std::uint32_t triangle{0U};
  /// 重心坐标（`u`、`v`；第三个权重是 `1-u-v`，对应三角形的三个顶点）。
  float bary_u{0.0f};
  float bary_v{0.0f};
};

/// 射线与一组物体求交，返回**最近**的命中。
///
/// ## 为什么必须有 `max_distance`
///
/// 无上限时，一条朝地平线打出去的射线会命中最远的那个物体——
/// 而"点到很远的东西"在交互里几乎总是错的（用户点的是眼前那个）。
/// 默认给一个很大的值（而不是 0），保持"不传就一直打到底"的语义。
[[nodiscard]] auto raycast(const std::vector<PickObject>& objects, const math::Ray& ray,
                           float max_distance = 3.4e38f) -> std::optional<PickHit>;

/// 射线与一组物体求交，返回**全部**命中（按距离升序）。
///
/// 用途：透明物体（穿透拾取）、统计"一条视线穿过几层"。
[[nodiscard]] auto raycast_all(const std::vector<PickObject>& objects, const math::Ray& ray,
                               float max_distance = 3.4e38f) -> std::vector<PickHit>;

/// 两个物体是否可能相交（宽相：世界 AABB 重叠）。
///
/// 这是**保守**判断：AABB 重叠不代表真交（对角摆放的两个细杆会误报）。
/// 但对"每帧要筛几百对"的场景，宽相本来就是为了快速排除。
[[nodiscard]] auto overlaps(const PickObject& a, const PickObject& b) -> bool;

/// 两物体的最近距离下界（世界 AABB 间距）；重叠时返回 0。
///
/// 用途：排序"最接近的一对"、或者做"接近就通知"这类交互反馈。
[[nodiscard]] auto bounds_gap(const PickObject& a, const PickObject& b) -> float;

/// 由网格 + 世界矩阵造一个 `PickObject`（自动算世界 AABB）。
///
/// 世界 AABB 用**变换后的 8 个角**取并，而不是只变换 `min`/`max`——
/// 后者在旋转后严重低估（一个绕对角线转 45° 的长方体，变换两个角得到的盒子
/// 会比实际小一半），症状是"物体明明在盒里却拾取不到"。
[[nodiscard]] auto make_pick_object(std::string name, const Mesh& mesh,
                                    const math::Mat4& world) -> PickObject;

}  // namespace st::raster

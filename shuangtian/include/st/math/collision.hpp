#pragma once

/// @file collision.hpp
/// @brief **碰撞与拾取**：射线求交、AABB 重叠、球体、三角形。
///
/// ## 为什么分成"宽相 / 窄相"两级（这不是过度设计）
///
/// 一个 5000 三角形的模型，逐三角形做射线求交要算 5000 次（每次含一次除法与
/// 三个叉积）。而**先测它的 AABB** 只要 6 次比较——绝大多数射线连盒子都碰不到。
/// 实测差距在 2~3 个数量级，所以凡是"场景里可能有多个物体"的场合，
/// 先宽相后窄相不是优化、是**能不能用**的区别。
///
/// 本模块提供两级原语，由调用方组合（本模块不建加速结构，
/// 因为"什么算一组"取决于调用方：场景物体、单个网格的三角形、还是 UI 命中区）。
///
/// ## 数值约定
///
/// - 射线：`origin + direction * t`，`t ∈ [0, t_max]`。**direction 不必归一化**，
///   但强烈建议归一化——否则 `t` 不是距离，"最远 10 米"这类参数会失控。
/// - 所有求交函数都返回 `t`（沿射线的参数）或不存在。
/// - 退化情形（零长方向、退化三角形、零尺寸盒）一律**返回"不相交"**而不是 NaN：
///   NaN 会静默扩散（`if (t < best)` 对 NaN 恒假，于是"永远打不中"，
///   而"永远打不中"看起来像"程序没实现"）。
///
/// ## 与物理引擎的关系
///
/// 这里只做**几何查询**（raycast / overlap），不做刚体动力学（质量、摩擦、冲量）。
/// 那是另一个量级的工作量，而"点选一个物体""判断两个模型是否穿插""视线遮挡"
/// 这些三维应用里最常要的事，几何查询就够。

#include <cstdint>
#include <optional>

#include "st/math/geometry.hpp"
#include "st/math/matrix.hpp"

namespace st::math {

/// 射线。
struct Ray {
  Vec3 origin{};
  /// 方向（**建议归一化**：否则 `t` 不是距离）。
  Vec3 direction{0.0f, 0.0f, -1.0f};

  /// 取射线上 `t` 处的点。
  [[nodiscard]] auto at(float t) const noexcept -> Vec3 {
    return Vec3{origin.x + direction.x * t, origin.y + direction.y * t,
                origin.z + direction.z * t};
  }
};

/// 射线与 AABB 求交（**slab 法**）。
///
/// ## slab 法为什么比"逐面 6 次求交"好
///
/// 逐面求交要算 6 个平面交点、再各自判断"是否落在面的范围内"——既慢又容易
/// 在边界上出错。slab 法把盒子看成三对平行平面，射线在每对平面之间划出一个
/// 参数区间 `[t_near, t_far]`，三个区间的交集非空即相交。只有 3 次除法 +
/// 若干比较，且天生处理"射线起点在盒内"（此时 `t_near < 0 < t_far`，
/// 返回 `t_far` 或 0，取决于 `allow_inside`）。
///
/// ## 平行射线的处理（0/0 = NaN 的经典坑）
///
/// 方向分量为 0 且起点在该轴 slab 之外时，除法会得到 `±inf` 或 `NaN`。
/// 这里**显式判断**而不是依赖 IEEE 的 inf 比较（那种代码看起来能跑，
/// 但在不同编译器/优化级别下行为不一致）。
///
/// @param t_min 起点偏移（用于"不要打到自己"的自遮挡，通常给个 1e-3）
/// @param allow_inside 起点在盒内时是否算命中（返回 `t_min`）
[[nodiscard]] auto ray_intersects_box(const Ray& ray, const Vec3& box_min, const Vec3& box_max,
                                      float t_min = 0.0f, float t_max = 3.4e38f,
                                      bool allow_inside = true) -> std::optional<float>;

/// 射线与三角形求交（**Möller–Trumbore**）。
///
/// ## 为什么用 Möller–Trumbore 而不是先求平面再判内外
///
/// 后者要 3 次"点与三角形位置关系"判断（每次 3 个叉积），而 Möller–Trumbore
/// 用重心坐标一次算完：`u`、`v` 直接给出"是否在三角形内"（`u>=0, v>=0, u+v<=1`），
/// 顺带还能得到插值权重（贴图/法线插值要用）。
///
/// ## 双面还是单面
///
/// 默认**双面**（`cull_back_face=false`）：拾取时经常要"点到背面也算"。
/// 渲染的背面剔除是显示策略，与几何查询无关——混用会让"看得见的点不到"。
///
/// @return 命中时返回 `(t, u, v)`：`t` 是射线参数，`u`/`v` 是重心坐标
///         （第三个权重是 `1-u-v`，对应三角形的顶点 0/1/2 顺序）。
struct RayTriangleHit {
  float t{0.0f};
  float u{0.0f};
  float v{0.0f};
};
[[nodiscard]] auto ray_intersects_triangle(const Ray& ray, const Vec3& a, const Vec3& b,
                                           const Vec3& c, bool cull_back_face = false,
                                           float t_min = 1.0e-5f, float t_max = 3.4e38f)
    -> std::optional<RayTriangleHit>;

/// 射线与球求交。
///
/// @return 命中时返回 `(t_near, t_far)`：两个交点参数（球在射线前方时 `t_near > 0`）。
struct RaySphereHit {
  float t_near{0.0f};
  float t_far{0.0f};
};
[[nodiscard]] auto ray_intersects_sphere(const Ray& ray, const Vec3& center, float radius,
                                         float t_min = 0.0f, float t_max = 3.4e38f)
    -> std::optional<RaySphereHit>;

/// 两个 AABB 是否重叠（**闭区间**：贴合算重叠）。
///
/// 为什么贴合算重叠：`union_with` 造出来的盒会精确相接，
/// 若把贴合判为不重叠，"相邻的两块地形"会漏掉彼此的接触。
[[nodiscard]] auto boxes_overlap(const Vec3& a_min, const Vec3& a_max, const Vec3& b_min,
                                 const Vec3& b_max) -> bool;

/// 球与 AABB 的最近距离平方（球心在盒内时为 0）。
///
/// 用来做"球体碰撞"的宽相：`distance_squared <= radius²` 即相交。
/// 比"6 个面的距离"稳，因为它天然处理"球心在盒内"与"最近点在棱/角上"。
[[nodiscard]] auto squared_distance_point_box(const Vec3& point, const Vec3& box_min,
                                              const Vec3& box_max) -> float;

/// 球与 AABB 是否相交。
[[nodiscard]] auto sphere_intersects_box(const Vec3& center, float radius, const Vec3& box_min,
                                         const Vec3& box_max) -> bool;

/// 按矩阵变换射线（模型空间求交时用）。
///
/// ## 为什么方向要**只乘 3×3 部分**
///
/// 平移不该作用于方向。踩过的经典坑：方向和点用同一套变换，得到的方向被
/// 平移量污染——症状是"射线在物体附近莫名其妙地偏"，
/// 而偏的方向恰好等于物体的位置。
///
/// 返回的 `direction` **未归一化**（长度被矩阵缩放），所以 `t` 与输入射线的 `t`
/// 处在不同尺度上。要比较距离时请先归一化返回值，或按 `1/|direction|` 换算。
[[nodiscard]] auto transform_ray(const Mat4& matrix, const Ray& ray) -> Ray;

}  // namespace st::math

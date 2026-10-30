#include "st/math/collision.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace st::math {

namespace {

constexpr float kInfinity = 3.4e38f;

[[nodiscard]] auto is_finite_value(float value) noexcept -> bool {
  return std::isfinite(value);
}

}  // namespace

auto ray_intersects_box(const Ray& ray, const Vec3& box_min, const Vec3& box_max, float t_min,
                        float t_max, bool allow_inside) -> std::optional<float> {
  // ## slab 法：三对平行平面围出的参数区间求交
  //
  // 对每个轴：`t1 = (min - o) / d`、`t2 = (max - o) / d`，
  // 取 `t_near = min(t1,t2)`、`t_far = max(t1,t2)`，然后整体收缩。
  // 三个轴都收缩完，若 `t_near <= t_far` 且区间与 `[t_min, t_max]` 有交集即命中。
  float near_t = -kInfinity;
  float far_t = kInfinity;
  const float origin[3] = {ray.origin.x, ray.origin.y, ray.origin.z};
  const float direction[3] = {ray.direction.x, ray.direction.y, ray.direction.z};
  const float lo[3] = {box_min.x, box_min.y, box_min.z};
  const float hi[3] = {box_max.x, box_max.y, box_max.z};

  for (int axis = 0; axis < 3; ++axis) {
    if (std::abs(direction[axis]) < 1.0e-12f) {
      // ## 射线与该轴平行：只能靠"起点是否在这一对平面之间"决定
      //
      // 这里**显式**判断，不依赖 0/0 产生 NaN 或 ±inf 再比较。
      // 依赖 IEEE 特殊值的写法在不同优化级别下行为不一致（实测开 -ffast-math
      // 时 NaN 比较会被优化掉），而那种 bug 只在发布版出现。
      if (origin[axis] < lo[axis] || origin[axis] > hi[axis]) return std::nullopt;
      continue;
    }
    const float inverse = 1.0f / direction[axis];
    float t1 = (lo[axis] - origin[axis]) * inverse;
    float t2 = (hi[axis] - origin[axis]) * inverse;
    if (t1 > t2) std::swap(t1, t2);
    near_t = std::max(near_t, t1);
    far_t = std::min(far_t, t2);
    if (near_t > far_t) return std::nullopt;
  }

  // 区间与 [t_min, t_max] 求交
  const float enter = std::max(near_t, t_min);
  const float exit_t = std::min(far_t, t_max);
  if (enter > exit_t) return std::nullopt;
  // 起点在盒内（near_t < 0 < far_t）
  if (near_t < 0.0f) {
    if (!allow_inside) return std::nullopt;
    return std::max(0.0f, t_min);
  }
  return enter;
}

auto ray_intersects_triangle(const Ray& ray, const Vec3& a, const Vec3& b, const Vec3& c,
                             bool cull_back_face, float t_min, float t_max)
    -> std::optional<RayTriangleHit> {
  // Möller–Trumbore。命名沿用原文：`e1/e2` 是两条边、`pvec` 是 `d × e2`。
  const Vec3 e1 = b - a;
  const Vec3 e2 = c - a;
  const Vec3 pvec = ray.direction.cross(e2);
  const float det = e1.dot(pvec);

  // ## 行列式接近 0 时不求交（而不是"给个容差硬算"）
  //
  // `|det|` 小意味着射线与三角形所在平面**几乎平行**：几何上没有稳定交点，
  // 硬算会得到极大或极小的 `t`（数值噪声）。这种"结果取决于浮点噪声"的情况
  // 会让拾取在掠射角下随机命中，比"打不中"更难查。
  constexpr float kEpsilon = 1.0e-8f;
  if (cull_back_face) {
    if (det < kEpsilon) return std::nullopt;   // 背面剔除：只接受正面命中
  } else if (std::abs(det) < kEpsilon) {
    return std::nullopt;
  }

  const float inverse_det = 1.0f / det;
  const Vec3 tvec = ray.origin - a;
  const float u = tvec.dot(pvec) * inverse_det;
  if (u < 0.0f || u > 1.0f) return std::nullopt;
  const Vec3 qvec = tvec.cross(e1);
  const float v = ray.direction.dot(qvec) * inverse_det;
  if (v < 0.0f || u + v > 1.0f) return std::nullopt;
  const float t = e2.dot(qvec) * inverse_det;
  if (!is_finite_value(t) || t < t_min || t > t_max) return std::nullopt;
  return RayTriangleHit{t, u, v};
}

auto ray_intersects_sphere(const Ray& ray, const Vec3& center, float radius, float t_min,
                           float t_max) -> std::optional<RaySphereHit> {
  // 解 `|o + t·d - c|² = r²`：`(d·d)t² + 2(d·(o-c))t + (|o-c|²-r²) = 0`
  const Vec3 to_center = ray.origin - center;
  const float a = ray.direction.dot(ray.direction);
  if (a < 1.0e-12f) return std::nullopt;   // 零长方向：不是射线
  const float b = 2.0f * to_center.dot(ray.direction);
  const float c = to_center.dot(to_center) - radius * radius;
  const float discriminant = b * b - 4.0f * a * c;
  if (discriminant < 0.0f) return std::nullopt;
  const float root = std::sqrt(discriminant);
  const float near_t = (-b - root) / (2.0f * a);
  const float far_t = (-b + root) / (2.0f * a);
  // 区间与 [t_min,t_max] 求交（球在身后时两个 t 都可能是负的）
  if (far_t < t_min || near_t > t_max) return std::nullopt;
  return RaySphereHit{std::max(near_t, t_min), std::min(far_t, t_max)};
}

auto boxes_overlap(const Vec3& a_min, const Vec3& a_max, const Vec3& b_min, const Vec3& b_max)
    -> bool {
  // 逐轴分离测试（SAT 在一维上的特例）：只要有一个轴分离就不重叠
  if (a_max.x < b_min.x || a_min.x > b_max.x) return false;
  if (a_max.y < b_min.y || a_min.y > b_max.y) return false;
  if (a_max.z < b_min.z || a_min.z > b_max.z) return false;
  return true;
}

auto squared_distance_point_box(const Vec3& point, const Vec3& box_min, const Vec3& box_max)
    -> float {
  // 逐轴取"到区间的距离"（在区间内为 0），再平方和。
  // 这个写法自动处理"最近点在面上/棱上/角上"三种情形，不必分类讨论。
  const auto axis_distance = [](float value, float lo, float hi) {
    if (value < lo) return lo - value;
    if (value > hi) return value - hi;
    return 0.0f;
  };
  const float dx = axis_distance(point.x, box_min.x, box_max.x);
  const float dy = axis_distance(point.y, box_min.y, box_max.y);
  const float dz = axis_distance(point.z, box_min.z, box_max.z);
  return dx * dx + dy * dy + dz * dz;
}

auto sphere_intersects_box(const Vec3& center, float radius, const Vec3& box_min,
                           const Vec3& box_max) -> bool {
  const float distance_squared = squared_distance_point_box(center, box_min, box_max);
  return distance_squared <= radius * radius;
}

auto transform_ray(const Mat4& matrix, const Ray& ray) -> Ray {
  Ray out{};
  out.origin = Vec3{matrix.value(0, 0) * ray.origin.x + matrix.value(0, 1) * ray.origin.y +
                       matrix.value(0, 2) * ray.origin.z + matrix.value(0, 3),
                   matrix.value(1, 0) * ray.origin.x + matrix.value(1, 1) * ray.origin.y +
                       matrix.value(1, 2) * ray.origin.z + matrix.value(1, 3),
                   matrix.value(2, 0) * ray.origin.x + matrix.value(2, 1) * ray.origin.y +
                       matrix.value(2, 2) * ray.origin.z + matrix.value(2, 3)};
  // 方向只乘 3×3（不含平移）——见头文件里的说明
  out.direction = Vec3{matrix.value(0, 0) * ray.direction.x + matrix.value(0, 1) * ray.direction.y +
                           matrix.value(0, 2) * ray.direction.z,
                       matrix.value(1, 0) * ray.direction.x + matrix.value(1, 1) * ray.direction.y +
                           matrix.value(1, 2) * ray.direction.z,
                       matrix.value(2, 0) * ray.direction.x + matrix.value(2, 1) * ray.direction.y +
                           matrix.value(2, 2) * ray.direction.z};
  return out;
}

}  // namespace st::math

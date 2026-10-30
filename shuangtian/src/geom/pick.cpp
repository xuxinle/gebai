#include "st/raster/pick.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace st::raster {

namespace {

using math::Mat4;
using math::Vec3;

[[nodiscard]] auto vertex_at(const Mesh& mesh, std::size_t index) noexcept -> Vec3 {
  const std::size_t at = index * Mesh::kVertexFloats;
  if (at + 2U >= mesh.vertices.size()) return Vec3{};
  return Vec3{mesh.vertices[at], mesh.vertices[at + 1U], mesh.vertices[at + 2U]};
}

/// 4×4 通用求逆（高斯-约当，带主元）。
///
/// ## 为什么不复用 `inverse_bind_from_world`
///
/// 那个函数假设矩阵是 `T·R·S` 的乘积（骨骼绑定矩阵的形态），用解析式更快。
/// 而拾取要处理**任意**世界矩阵——包括带错切、非均匀缩放、镜像的。
/// 那种矩阵用解析式会算错，而"算错"的表现是"某些物体拾取不到"，
/// 排查时很难想到是求逆的问题。
///
/// 退化（奇异）时返回 `std::nullopt`，调用方跳过该物体（而不是用单位阵硬算——
/// 那会让射线在模型空间里乱走，命中一堆不相关的位置）。
[[nodiscard]] auto invert_matrix(const Mat4& matrix) -> std::optional<Mat4> {
  // 拓广矩阵 [A | I]，对 A 做行变换
  double a[4][8] = {};
  for (int row = 0; row < 4; ++row) {
    for (int column = 0; column < 4; ++column) {
      a[row][column] = static_cast<double>(matrix.value(row, column));
    }
    a[row][4 + row] = 1.0;
  }
  for (int column = 0; column < 4; ++column) {
    // 选主元（部分主元法：取该列绝对值最大的行，避免小主元放大误差）
    int pivot = column;
    double best = std::abs(a[column][column]);
    for (int row = column + 1; row < 4; ++row) {
      if (const double value = std::abs(a[row][column]); value > best) {
        best = value;
        pivot = row;
      }
    }
    if (best < 1.0e-12) return std::nullopt;   // 奇异
    if (pivot != column) {
      for (int k = 0; k < 8; ++k) std::swap(a[column][k], a[pivot][k]);
    }
    const double scale = 1.0 / a[column][column];
    for (int k = 0; k < 8; ++k) a[column][k] *= scale;
    for (int row = 0; row < 4; ++row) {
      if (row == column) continue;
      const double factor = a[row][column];
      if (factor == 0.0) continue;
      for (int k = 0; k < 8; ++k) a[row][k] -= factor * a[column][k];
    }
  }
  Mat4 out = Mat4::identity();
  for (int row = 0; row < 4; ++row) {
    for (int column = 0; column < 4; ++column) {
      out.set(row, column, static_cast<float>(a[row][4 + column]));
    }
  }
  return out;
}

/// 三角形面法线（世界空间）。
[[nodiscard]] auto face_normal(const Vec3& a, const Vec3& b, const Vec3& c) -> Vec3 {
  const Vec3 normal = (b - a).cross(c - a);
  const float length = normal.length();
  if (length < 1.0e-12f) return Vec3{0.0f, 1.0f, 0.0f};
  return normal * (1.0f / length);
}

}  // namespace

auto make_pick_object(std::string name, const Mesh& mesh, const Mat4& world) -> PickObject {
  PickObject object{};
  object.name = std::move(name);
  object.world = world;
  object.mesh = &mesh;
  // ## 世界 AABB 要变换**8 个角**再取并
  //
  // 只变换 `min`/`max` 两点在旋转后严重低估：绕对角线转 45° 的长方体，
  // 两个对角点变换后仍是两个点，它们围出的盒子比实际小一半。
  // 症状是"物体明明在盒里却拾取不到"——而宽相就在这一步把它排除了。
  const Bounds local = bounds(mesh);
  if (local.is_empty()) {
    object.world_bounds = Bounds::empty();
    return object;
  }
  Bounds world_box = Bounds::empty();
  for (int mask = 0; mask < 8; ++mask) {
    const Vec3 corner{(mask & 1) != 0 ? local.max.x : local.min.x,
                      (mask & 2) != 0 ? local.max.y : local.min.y,
                      (mask & 4) != 0 ? local.max.z : local.min.z};
    const Vec3 moved{
        world.value(0, 0) * corner.x + world.value(0, 1) * corner.y +
            world.value(0, 2) * corner.z + world.value(0, 3),
        world.value(1, 0) * corner.x + world.value(1, 1) * corner.y +
            world.value(1, 2) * corner.z + world.value(1, 3),
        world.value(2, 0) * corner.x + world.value(2, 1) * corner.y +
            world.value(2, 2) * corner.z + world.value(2, 3)};
    world_box = world_box.union_with(moved);
  }
  object.world_bounds = world_box;
  return object;
}

auto raycast(const std::vector<PickObject>& objects, const math::Ray& ray, float max_distance)
    -> std::optional<PickHit> {
  PickHit best{};
  float best_distance = max_distance;
  bool found = false;
  for (const PickObject& object : objects) {
    if (object.mesh == nullptr || object.world_bounds.is_empty()) continue;
    // ── 宽相：世界 AABB ──
    if (!math::ray_intersects_box(ray, object.world_bounds.min, object.world_bounds.max, 0.0f,
                                  best_distance)
             .has_value()) {
      continue;
    }
    // ── 窄相：把射线搬进模型空间，逐三角形求交 ──
    const auto inverse = invert_matrix(object.world);
    if (!inverse.has_value()) continue;
    const math::Ray local_ray = math::transform_ray(*inverse, ray);
    const Mesh& mesh = *object.mesh;
    const std::size_t triangles = mesh.indices.size() / 3U;
    float nearest = best_distance;
    std::size_t nearest_triangle = 0;
    float nearest_u = 0.0f;
    float nearest_v = 0.0f;
    bool hit_any = false;
    for (std::size_t triangle = 0; triangle < triangles; ++triangle) {
      const std::uint32_t i0 = mesh.indices[triangle * 3U];
      const std::uint32_t i1 = mesh.indices[triangle * 3U + 1U];
      const std::uint32_t i2 = mesh.indices[triangle * 3U + 2U];
      // ## 模型空间的 t 要换算回世界空间
      //
      // 逆矩阵把方向也变了长度（非均匀缩放下每个方向的比例不同），
      // 所以模型空间的 `t` 不与世界空间的 `t` 相等。
      // 这里用"命中点在模型空间 → 变换到世界 → 与射线起点算距离"来避免换算错误：
      // 换算公式依赖方向长度，容易在非均匀缩放下写错（而写错后"近处物体被远处物体盖住"）。
      const auto hit = math::ray_intersects_triangle(local_ray, vertex_at(mesh, i0),
                                                     vertex_at(mesh, i1), vertex_at(mesh, i2),
                                                     false, 1.0e-6f, 3.4e38f);
      if (!hit.has_value()) continue;
      const Vec3 local_point = local_ray.at(hit->t);   // lint-allow: L12 Ray::at 取射线上一点，非容器取值
      const Vec3 world_point{
          object.world.value(0, 0) * local_point.x + object.world.value(0, 1) * local_point.y +
              object.world.value(0, 2) * local_point.z + object.world.value(0, 3),
          object.world.value(1, 0) * local_point.x + object.world.value(1, 1) * local_point.y +
              object.world.value(1, 2) * local_point.z + object.world.value(1, 3),
          object.world.value(2, 0) * local_point.x + object.world.value(2, 1) * local_point.y +
              object.world.value(2, 2) * local_point.z + object.world.value(2, 3)};
      const Vec3 delta = world_point - ray.origin;
      // 距离用"沿射线方向的投影"而不是欧氏长度：对未归一化的射线也能得到正确的参数
      const float along = delta.dot(ray.direction) / ray.direction.dot(ray.direction);
      if (along < 0.0f || along >= nearest) continue;
      nearest = along;
      nearest_triangle = triangle;
      nearest_u = hit->u;
      nearest_v = hit->v;
      hit_any = true;
    }
    if (!hit_any) continue;

    best.name = object.name;
    best.distance = nearest;
    best.point = ray.at(nearest);   // lint-allow: L12 同上
    best.triangle = static_cast<std::uint32_t>(nearest_triangle);
    best.bary_u = nearest_u;
    best.bary_v = nearest_v;
    // 法线：用世界空间顶点算（而非模型空间法线变换），与绕序无关、与射线来向对齐
    const std::uint32_t i0 = mesh.indices[nearest_triangle * 3U];
    const std::uint32_t i1 = mesh.indices[nearest_triangle * 3U + 1U];
    const std::uint32_t i2 = mesh.indices[nearest_triangle * 3U + 2U];
    const auto to_world = [&](const Vec3& local) {
      return Vec3{object.world.value(0, 0) * local.x + object.world.value(0, 1) * local.y +
                      object.world.value(0, 2) * local.z + object.world.value(0, 3),
                  object.world.value(1, 0) * local.x + object.world.value(1, 1) * local.y +
                      object.world.value(1, 2) * local.z + object.world.value(1, 3),
                  object.world.value(2, 0) * local.x + object.world.value(2, 1) * local.y +
                      object.world.value(2, 2) * local.z + object.world.value(2, 3)};
    };
    Vec3 normal = face_normal(to_world(vertex_at(mesh, i0)), to_world(vertex_at(mesh, i1)),
                              to_world(vertex_at(mesh, i2)));
    // 统一朝射线来向（调用方总想"往自己这边推"）
    if (normal.dot(ray.direction) > 0.0f) normal = normal * -1.0f;
    best.normal = normal;
    best_distance = nearest;
    found = true;
  }
  if (!found) return std::nullopt;
  return best;
}

auto raycast_all(const std::vector<PickObject>& objects, const math::Ray& ray, float max_distance)
    -> std::vector<PickHit> {
  // ## 为什么逐物体"取最近"再收集，而不是反复调 `raycast` 往前推进
  //
  // 起初的实现是循环调用 `raycast`、每次把上限压到"刚命中点再往前一点点"
  //（`nextafter`）。它有真 bug：`raycast` 返回的是**世界空间**距离，而三角形求交
  // 在**模型空间**做——两个尺度的 `t` 通过"命中点 → 世界 → 沿射线投影"换算，
  // 浮点误差会让同一个三角形在下一轮又被命中（差几个 ulp），
  // 于是 `raycast_all` 反复返回**同一个物体**，而更远的物体永远不出现。
  //
  // 正确做法：每个物体**只求一次**它的最近命中（这是它自己的极小值），
  // 收集起来再统一排序。语义也更清楚——"每个物体给我一个最近点"。
  std::vector<PickHit> hits;
  for (const PickObject& object : objects) {
    const std::vector<PickObject> single{object};
    if (const auto hit = raycast(single, ray, max_distance); hit.has_value()) {
      hits.push_back(*hit);
    }
  }
  std::sort(hits.begin(), hits.end(),
            [](const PickHit& lhs, const PickHit& rhs) { return lhs.distance < rhs.distance; });
  return hits;
}

auto overlaps(const PickObject& a, const PickObject& b) -> bool {
  if (a.world_bounds.is_empty() || b.world_bounds.is_empty()) return false;
  return math::boxes_overlap(a.world_bounds.min, a.world_bounds.max, b.world_bounds.min,
                             b.world_bounds.max);
}

auto bounds_gap(const PickObject& a, const PickObject& b) -> float {
  if (a.world_bounds.is_empty() || b.world_bounds.is_empty()) {
    return std::numeric_limits<float>::max();
  }
  if (overlaps(a, b)) return 0.0f;
  // 两盒之间的最短距离：逐轴算"区间间距"，再取欧氏长度
  const auto axis_gap = [](float a_min, float a_max, float b_min, float b_max) {
    if (a_max < b_min) return b_min - a_max;
    if (b_max < a_min) return a_min - b_max;
    return 0.0f;
  };
  const float dx = axis_gap(a.world_bounds.min.x, a.world_bounds.max.x, b.world_bounds.min.x,
                            b.world_bounds.max.x);
  const float dy = axis_gap(a.world_bounds.min.y, a.world_bounds.max.y, b.world_bounds.min.y,
                            b.world_bounds.max.y);
  const float dz = axis_gap(a.world_bounds.min.z, a.world_bounds.max.z, b.world_bounds.min.z,
                            b.world_bounds.max.z);
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}  // namespace st::raster

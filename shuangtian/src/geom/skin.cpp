#include "st/raster/skin.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <map>
#include <set>

#include "st/raster/mesh_ops.hpp"

namespace st::raster {

namespace {

using math::Mat4;
using math::Vec3;

[[nodiscard]] auto vertex_position(const Mesh& mesh, std::size_t index) noexcept -> Vec3 {
  const std::size_t at = index * Mesh::kVertexFloats;
  if (at + 2U >= mesh.vertices.size()) return Vec3{};
  return Vec3{mesh.vertices[at], mesh.vertices[at + 1U], mesh.vertices[at + 2U]};
}

[[nodiscard]] auto vertex_normal(const Mesh& mesh, std::size_t index) noexcept -> Vec3 {
  const std::size_t at = index * Mesh::kVertexFloats + 3U;
  if (at + 2U >= mesh.vertices.size()) return Vec3{0.0f, 1.0f, 0.0f};
  return Vec3{mesh.vertices[at], mesh.vertices[at + 1U], mesh.vertices[at + 2U]};
}

/// 点过矩阵（含平移）。
[[nodiscard]] auto transform_point(const Mat4& matrix, Vec3 point) noexcept -> Vec3 {
  return Vec3{matrix.value(0, 0) * point.x + matrix.value(0, 1) * point.y +
                  matrix.value(0, 2) * point.z + matrix.value(0, 3),
              matrix.value(1, 0) * point.x + matrix.value(1, 1) * point.y +
                  matrix.value(1, 2) * point.z + matrix.value(1, 3),
              matrix.value(2, 0) * point.x + matrix.value(2, 1) * point.y +
                  matrix.value(2, 2) * point.z + matrix.value(2, 3)};
}

/// 向量过矩阵（只取 3×3 部分，不含平移）。
///
/// 法线严格来说应该用逆转置，但蒙皮矩阵的 3×3 部分通常是**正交**的
/// （旋转 + 均匀缩放），此时逆等于转置、转置等于自身——用 3×3 直接变换
/// 与用逆转置的结果只差一个缩放因子，而末尾会重新归一化。
/// 非均匀缩放的蒙皮在实践里不出现（骨骼不缩放网格）。
[[nodiscard]] auto transform_vector(const Mat4& matrix, Vec3 vector) noexcept -> Vec3 {
  return Vec3{matrix.value(0, 0) * vector.x + matrix.value(0, 1) * vector.y +
                  matrix.value(0, 2) * vector.z,
              matrix.value(1, 0) * vector.x + matrix.value(1, 1) * vector.y +
                  matrix.value(1, 2) * vector.z,
              matrix.value(2, 0) * vector.x + matrix.value(2, 1) * vector.y +
                  matrix.value(2, 2) * vector.z};
}

/// 3×3 行列式（判断可逆性）。
[[nodiscard]] auto determinant3(const Mat4& matrix) noexcept -> float {
  const float a = matrix.value(0, 0);
  const float b = matrix.value(0, 1);
  const float c = matrix.value(0, 2);
  const float d = matrix.value(1, 0);
  const float e = matrix.value(1, 1);
  const float f = matrix.value(1, 2);
  const float g = matrix.value(2, 0);
  const float h = matrix.value(2, 1);
  const float i = matrix.value(2, 2);
  return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
}

}  // namespace

// ────────────────────────────────────────────────────────────────────────────
// Skeleton
// ────────────────────────────────────────────────────────────────────────────

auto Skeleton::bind_matrix_at(std::size_t index) const noexcept -> Mat4 {
  if (index >= inverse_bind.size()) return Mat4::identity();
  return inverse_bind[index];
}

auto Skeleton::index_of(std::string_view name) const noexcept -> int {
  for (std::size_t i = 0; i < joints.size(); ++i) {
    if (joints[i] == name) return static_cast<int>(i);
  }
  return -1;
}

auto inverse_bind_from_world(const Mat4& world) noexcept -> Mat4 {
  // ## 只处理"平移 + 旋转 + 缩放"构成的可逆矩阵
  //
  // 骨骼的绑定矩阵永远是这三者的乘积（`T · R · S`），所以逆可以解析地写成
  // `S⁻¹ · R⁻¹ · T⁻¹`，不必写通用 4×4 求逆（那两行代码错了也照样算得出数，
  // 而那种错表现为"角色整体缩成一点"或"绕错轴"，极难定位）。
  const float det = determinant3(world);
  if (std::abs(det) < 1.0e-9f) return Mat4::identity();   // 退化：不形变比 NaN 好
  // 3×3 逆转置的写法：cofactor / det，再转置回原位（与 `mesh_ops` 的 normal_matrix 同源）
  Mat4 out = Mat4::identity();
  const float inv = 1.0f / det;
  const float a = world.value(0, 0);
  const float b = world.value(0, 1);
  const float c = world.value(0, 2);
  const float d = world.value(1, 0);
  const float e = world.value(1, 1);
  const float f = world.value(1, 2);
  const float g = world.value(2, 0);
  const float h = world.value(2, 1);
  const float i = world.value(2, 2);
  out.set(0, 0, (e * i - f * h) * inv);
  out.set(0, 1, (c * h - b * i) * inv);
  out.set(0, 2, (b * f - c * e) * inv);
  out.set(1, 0, (f * g - d * i) * inv);
  out.set(1, 1, (a * i - c * g) * inv);
  out.set(1, 2, (c * d - a * f) * inv);
  out.set(2, 0, (d * h - e * g) * inv);
  out.set(2, 1, (b * g - a * h) * inv);
  out.set(2, 2, (a * e - b * d) * inv);
  // 平移部分：`-R⁻¹ · t`
  const float tx = world.value(0, 3);
  const float ty = world.value(1, 3);
  const float tz = world.value(2, 3);
  out.set(0, 3, -(out.value(0, 0) * tx + out.value(0, 1) * ty + out.value(0, 2) * tz));
  out.set(1, 3, -(out.value(1, 0) * tx + out.value(1, 1) * ty + out.value(1, 2) * tz));
  out.set(2, 3, -(out.value(2, 0) * tx + out.value(2, 1) * ty + out.value(2, 2) * tz));
  return out;
}

// ────────────────────────────────────────────────────────────────────────────
// 蒙皮矩阵
// ────────────────────────────────────────────────────────────────────────────

auto compute_skin_matrices(const Skeleton& skeleton, const ResolvedTree& tree)
    -> Result<SkinMatrices> {
  SkinMatrices out{};
  out.matrices.reserve(skeleton.joint_count());
  out.joint_names.reserve(skeleton.joint_count());
  for (std::size_t i = 0; i < skeleton.joint_count(); ++i) {
    const std::string& joint = skeleton.joints[i];
    const ResolvedNode* node = tree.find_by_name(joint);
    if (node == nullptr) {
      return st::unexpected(
          ErrorCode::NotFound,
          std::format("骨架的第 {} 个关节 \"{}\" 在场景里没有对应节点"
                      "（关节名必须与节点名一致）",
                      i, joint));
    }
    // 蒙皮矩阵 = 当前世界矩阵 · 逆绑定矩阵
    out.matrices.push_back(node->world * skeleton.bind_matrix_at(i));
    out.joint_names.push_back(joint);
  }
  return out;
}

// ────────────────────────────────────────────────────────────────────────────
// 形变
// ────────────────────────────────────────────────────────────────────────────

auto skin_mesh(const Mesh& mesh, const SkinMatrices& matrices) -> Mesh {
  if (!mesh.has_skins() || matrices.matrices.empty()) return mesh;

  Mesh out = mesh;
  const std::size_t count = mesh.vertex_count();
  for (std::size_t vertex = 0; vertex < count; ++vertex) {
    const Vec3 position = vertex_position(mesh, vertex);
    const Vec3 normal = vertex_normal(mesh, vertex);
    Vec3 blended_position{};
    Vec3 blended_normal{};
    float total_weight = 0.0f;
    for (std::size_t slot = 0; slot < 4U; ++slot) {
      const auto [joint, weight] = mesh.influence_at(vertex, slot);
      if (weight <= 0.0f) continue;
      if (static_cast<std::size_t>(joint) >= matrices.matrices.size()) continue;   // 坏索引当无影响
      const Mat4& matrix = matrices.matrices[joint];
      blended_position = blended_position + transform_point(matrix, position) * weight;
      blended_normal = blended_normal + transform_vector(matrix, normal) * weight;
      total_weight += weight;
    }
    const std::size_t at = vertex * Mesh::kVertexFloats;
    if (at + 5U >= out.vertices.size()) break;
    if (total_weight > 1.0e-6f) {
      // ## 除以总权重（而不是假设已归一化）
      //
      // 调用方"保证归一化"是约定，但实际数据经常差一点点（工具导出、手搓测试、
      // 权重被截断到 4 个之后）。不除的话症状是"角色整体缩小"——
      // 因为所有权重都小于 1，位置被拉向原点。除以总权重是零成本的保险。
      const Vec3 position_out = blended_position * (1.0f / total_weight);
      const Vec3 normal_out = blended_normal.normalized();
      out.vertices[at + 0U] = position_out.x;
      out.vertices[at + 1U] = position_out.y;
      out.vertices[at + 2U] = position_out.z;
      out.vertices[at + 3U] = normal_out.length() > 0.5f ? normal_out.x : normal.x;
      out.vertices[at + 4U] = normal_out.length() > 0.5f ? normal_out.y : normal.y;
      out.vertices[at + 5U] = normal_out.length() > 0.5f ? normal_out.z : normal.z;
    }
    // 权重全零：保持原位（不受骨骼影响 = 静态部分，比如角色的背包挂件）
  }
  return out;
}

auto limit_to_four_influences(const std::vector<std::pair<std::uint32_t, float>>& influences,
                              float epsilon) -> std::array<float, 8> {
  std::array<float, 8> out{};
  // 按权重降序取前四（稳定：权重相同时保持原顺序，结果可复现）
  std::vector<std::pair<std::uint32_t, float>> sorted;
  sorted.reserve(influences.size());
  for (const auto& [joint, weight] : influences) {
    if (weight > epsilon) sorted.emplace_back(joint, weight);
  }
  std::stable_sort(sorted.begin(), sorted.end(),
                   [](const auto& lhs, const auto& rhs) { return lhs.second > rhs.second; });
  if (sorted.size() > 4U) sorted.resize(4U);
  float total = 0.0f;
  for (const auto& entry : sorted) total += entry.second;
  if (total <= 1.0e-9f) return out;   // 全部被 epsilon 滤掉：视为不受影响
  for (std::size_t slot = 0; slot < sorted.size(); ++slot) {
    out[slot * 2U] = static_cast<float>(sorted[slot].first);
    out[slot * 2U + 1U] = sorted[slot].second / total;   // 归一化
  }
  return out;
}

// ────────────────────────────────────────────────────────────────────────────
// 自动权重（朴素版，供示例/测试）
// ────────────────────────────────────────────────────────────────────────────

auto auto_weights(const Mesh& mesh, const Skeleton& skeleton, const ResolvedTree& tree,
                  float falloff) -> Mesh {
  Mesh out = mesh;
  if (skeleton.joint_count() == 0U || mesh.is_empty()) return out;

  // 收集关节的**世界位置**（取世界矩阵的平移列）
  std::vector<Vec3> joint_positions;
  joint_positions.reserve(skeleton.joint_count());
  for (const std::string& joint : skeleton.joints) {
    const ResolvedNode* node = tree.find_by_name(joint);
    joint_positions.push_back(node != nullptr
                                 ? Vec3{node->world.value(0, 3), node->world.value(1, 3),
                                        node->world.value(2, 3)}
                                 : Vec3{});
  }

  out.skins.assign(mesh.vertex_count() * 8U, 0.0f);
  const float band = falloff > 0.0f ? falloff : 1.0e-4f;
  for (std::size_t vertex = 0; vertex < mesh.vertex_count(); ++vertex) {
    // 顶点位置也在世界空间里比（网格顶点是模型空间，而关节位置是世界空间）。
    // 这里假设网格未带额外变换（示例/测试场景都满足）——文档里已声明这是朴素实现。
    const Vec3 position = vertex_position(mesh, vertex);
    std::vector<std::pair<std::uint32_t, float>> influences;
    influences.reserve(joint_positions.size());
    for (std::size_t joint = 0; joint < joint_positions.size(); ++joint) {
      const float distance = (position - joint_positions[joint]).length();
      // 反距离权重 + 过渡带：`falloff` 内线性衰减到 0，之外不参与
      if (distance >= band * 8.0f) continue;
      const float weight = 1.0f / (distance + band);
      influences.emplace_back(static_cast<std::uint32_t>(joint), weight);
    }
    const auto limited = limit_to_four_influences(influences);
    for (std::size_t i = 0; i < 8U; ++i) out.skins[vertex * 8U + i] = limited[i];
  }
  return out;
}

}  // namespace st::raster

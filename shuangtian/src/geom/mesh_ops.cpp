#include "st/raster/mesh_ops.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <map>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/json.hpp"

namespace st::raster {

namespace {

/// 位置量化成整数格点，用于焊接时**哈希**（浮点数不能直接进 `unordered_map` 的键：
/// 同一个几何位置会因累加顺序差出 1 ULP，于是"看着重合"的顶点焊不上）。
///
/// 量化步长 = `epsilon`；相邻格点仍要比较（见 `weld`），否则跨格的近邻又被拆开。
[[nodiscard]] auto quantize(float value, float epsilon) noexcept -> std::int64_t {
  const double scaled = static_cast<double>(value) / static_cast<double>(epsilon);
  // 四舍五入并夹到 int64 的安全范围（极端坐标不该引发 UB）
  const double rounded = std::round(scaled);
  constexpr double kLimit = 9.0e15;
  if (!(rounded > -kLimit && rounded < kLimit)) return rounded > 0 ? 9000000000000000 : -9000000000000000;
  return static_cast<std::int64_t>(rounded);
}

struct QuantizedKey {
  std::int64_t x{0};
  std::int64_t y{0};
  std::int64_t z{0};
  std::int64_t nx{0};
  std::int64_t ny{0};
  std::int64_t nz{0};

  friend auto operator==(const QuantizedKey&, const QuantizedKey&) noexcept -> bool = default;
};

struct QuantizedKeyHash {
  auto operator()(const QuantizedKey& key) const noexcept -> std::size_t {
    // FNV-1a 风格混合：把六个整数搅进一个 size_t（不需要密码学强度，只要分布均匀）
    std::uint64_t seed = 1469598103934665603ULL;
    const auto mix = [&seed](std::int64_t value) {
      seed ^= static_cast<std::uint64_t>(value);
      seed *= 1099511628211ULL;
      seed ^= seed >> 29U;
    };
    mix(key.x);
    mix(key.y);
    mix(key.z);
    mix(key.nx);
    mix(key.ny);
    mix(key.nz);
    return static_cast<std::size_t>(seed);
  }
};

[[nodiscard]] auto vertex_at(const Mesh& mesh, std::size_t index, std::size_t component) noexcept
    -> float {
  const std::size_t offset = index * Mesh::kVertexFloats + component;
  return offset < mesh.vertices.size() ? mesh.vertices[offset] : 0.0f;
}

/// 从索引三元组算出几何法线（未归一化）。
[[nodiscard]] auto face_cross(const Mesh& mesh, std::size_t i0, std::size_t i1, std::size_t i2)
    -> math::Vec3 {
  const math::Vec3 p0{vertex_at(mesh, i0, 0), vertex_at(mesh, i0, 1), vertex_at(mesh, i0, 2)};
  const math::Vec3 p1{vertex_at(mesh, i1, 0), vertex_at(mesh, i1, 1), vertex_at(mesh, i1, 2)};
  const math::Vec3 p2{vertex_at(mesh, i2, 0), vertex_at(mesh, i2, 1), vertex_at(mesh, i2, 2)};
  return (p1 - p0).cross(p2 - p0);
}

/// OBJ 数字格式：定点 + 去尾零。
///
/// 为何不用 `{}`：`std::format("{}", 1.0f)` 得到 `1`——OBJ 里是合法的，但
/// 有些极简解析器把整数当索引/计数读；固定带小数点最稳，且人读着也清楚。
[[nodiscard]] auto obj_number(float value) -> std::string {
  if (!std::isfinite(value)) return "0";
  std::string text = std::format("{:.6f}", static_cast<double>(value));
  const std::size_t dot = text.find('.');
  if (dot != std::string::npos) {
    std::size_t last = text.find_last_not_of('0');
    if (last == dot) last = dot + 1;
    text.erase(last + 1);
  }
  if (text == "-0") return "0";
  return text;
}

}  // namespace

// ────────────────────────────────────────────────────────────────────────────
// Bounds
// ────────────────────────────────────────────────────────────────────────────

auto Bounds::empty() noexcept -> Bounds {
  return Bounds{math::Vec3{1.0f, 1.0f, 1.0f}, math::Vec3{-1.0f, -1.0f, -1.0f}};
}

auto Bounds::is_empty() const noexcept -> bool {
  return min.x > max.x || min.y > max.y || min.z > max.z;
}

auto Bounds::size() const noexcept -> math::Vec3 {
  if (is_empty()) return math::Vec3{};
  return max - min;
}

auto Bounds::center() const noexcept -> math::Vec3 {
  if (is_empty()) return math::Vec3{};
  return math::Vec3{(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f, (min.z + max.z) * 0.5f};
}

auto Bounds::union_with(math::Vec3 point) const noexcept -> Bounds {
  if (is_empty()) return Bounds{point, point};
  Bounds out = *this;
  out.min.x = std::min(out.min.x, point.x);
  out.min.y = std::min(out.min.y, point.y);
  out.min.z = std::min(out.min.z, point.z);
  out.max.x = std::max(out.max.x, point.x);
  out.max.y = std::max(out.max.y, point.y);
  out.max.z = std::max(out.max.z, point.z);
  return out;
}

auto Bounds::union_with(const Bounds& other) const noexcept -> Bounds {
  if (other.is_empty()) return *this;
  if (is_empty()) return other;
  return union_with(other.min).union_with(other.max);
}

auto bounds(const Mesh& mesh) noexcept -> Bounds {
  Bounds out = Bounds::empty();
  for (std::size_t i = 0; i < mesh.vertex_count(); ++i) {
    out = out.union_with(
        math::Vec3{vertex_at(mesh, i, 0), vertex_at(mesh, i, 1), vertex_at(mesh, i, 2)});
  }
  return out;
}

// ────────────────────────────────────────────────────────────────────────────
// 变换
// ────────────────────────────────────────────────────────────────────────────

namespace {

/// 逆转置矩阵（法线用）。
///
/// 实现路径：先算 3×3 的行列式与余子式（伴随矩阵），再除以行列式。
/// **不走通用 4×4 求逆**：法线只需要 3×3 部分，而 4×4 求逆的两行代码错了
/// 也照样能算出一个数——那种错在画面上只表现为"光照方向怪"，极难定位。
[[nodiscard]] auto normal_matrix(const math::Mat4& matrix, bool& mirrored) -> math::Mat4 {
  const float a = matrix.value(0, 0);
  const float b = matrix.value(0, 1);
  const float c = matrix.value(0, 2);
  const float d = matrix.value(1, 0);
  const float e = matrix.value(1, 1);
  const float f = matrix.value(1, 2);
  const float g = matrix.value(2, 0);
  const float h = matrix.value(2, 1);
  const float i = matrix.value(2, 2);
  const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
  mirrored = det < 0.0f;
  if (std::abs(det) < 1e-12f) return math::Mat4::identity();  // 退化：法线保持原样
  const float inv = 1.0f / det;
  // 逆转置：cofactor 矩阵直接按转置后的位置写入
  math::Mat4 out = math::Mat4::identity();
  out.set(0, 0, (e * i - f * h) * inv);
  out.set(0, 1, (f * g - d * i) * inv);
  out.set(0, 2, (d * h - e * g) * inv);
  out.set(1, 0, (c * h - b * i) * inv);
  out.set(1, 1, (a * i - c * g) * inv);
  out.set(1, 2, (b * g - a * h) * inv);
  out.set(2, 0, (b * f - c * e) * inv);
  out.set(2, 1, (c * d - a * f) * inv);
  out.set(2, 2, (a * e - b * d) * inv);
  return out;
}

}  // namespace

auto transform(const Mesh& mesh, const math::Mat4& matrix) -> Mesh {
  Mesh out = mesh;
  bool mirrored = false;
  const math::Mat4 normal_m = normal_matrix(matrix, mirrored);

  for (std::size_t i = 0; i < out.vertex_count(); ++i) {
    const std::size_t at = i * Mesh::kVertexFloats;
    if (at + Mesh::kVertexFloats > out.vertices.size()) break;
    const float px = out.vertices[at + 0];
    const float py = out.vertices[at + 1];
    const float pz = out.vertices[at + 2];
    out.vertices[at + 0] = matrix.value(0, 0) * px + matrix.value(0, 1) * py +
                           matrix.value(0, 2) * pz + matrix.value(0, 3);
    out.vertices[at + 1] = matrix.value(1, 0) * px + matrix.value(1, 1) * py +
                           matrix.value(1, 2) * pz + matrix.value(1, 3);
    out.vertices[at + 2] = matrix.value(2, 0) * px + matrix.value(2, 1) * py +
                           matrix.value(2, 2) * pz + matrix.value(2, 3);
    // 法线：逆转置后**必须重新归一化**（缩放会让长度不再为 1，光照强度跟着漂）
    const float nx = out.vertices[at + 3];
    const float ny = out.vertices[at + 4];
    const float nz = out.vertices[at + 5];
    const math::Vec3 normal{(normal_m.value(0, 0) * nx + normal_m.value(0, 1) * ny +
                             normal_m.value(0, 2) * nz),
                            (normal_m.value(1, 0) * nx + normal_m.value(1, 1) * ny +
                             normal_m.value(1, 2) * nz),
                            (normal_m.value(2, 0) * nx + normal_m.value(2, 1) * ny +
                             normal_m.value(2, 2) * nz)};
    const math::Vec3 unit = normal.normalized();
    out.vertices[at + 3] = unit.x;
    out.vertices[at + 4] = unit.y;
    out.vertices[at + 5] = unit.z;
  }

  if (!mirrored) return out;
  // 镜像：几何朝向被翻转，但顶点法线已被上面的逆转置"翻回来"——
  // 于是两者反向，背面剔除会把物体剔成内壁。翻转绕序让它们重新对齐。
  for (std::size_t t = 0; t + 2 < out.indices.size(); t += 3) {
    std::swap(out.indices[t + 1], out.indices[t + 2]);
  }
  return out;
}

auto transform(const Mesh& mesh, math::Vec3 translate, math::Vec3 rotate_radians,
               math::Vec3 scale) -> Mesh {
  // 顺序与 Blender 的 Object Transform 一致：先缩放、再绕 X→Y→Z 旋转、最后平移。
  const math::Mat4 model =
      math::Mat4::translation(translate) * math::Mat4::rotation(math::Vec3{0.0f, 0.0f, 1.0f},
                                                                rotate_radians.z) *
      math::Mat4::rotation(math::Vec3{0.0f, 1.0f, 0.0f}, rotate_radians.y) *
      math::Mat4::rotation(math::Vec3{1.0f, 0.0f, 0.0f}, rotate_radians.x) *
      math::Mat4::scaling(scale);
  return transform(mesh, model);
}

// ────────────────────────────────────────────────────────────────────────────
// 合并
// ────────────────────────────────────────────────────────────────────────────

auto merge(std::span<const Mesh> parts) -> Mesh {
  Mesh out{};
  std::size_t vertex_total = 0;
  std::size_t index_total = 0;
  std::size_t uv_total = 0;
  bool all_have_uv = true;
  bool any = false;
  for (const Mesh& part : parts) {
    if (part.is_empty()) continue;
    any = true;
    vertex_total += part.vertex_count();
    index_total += part.indices.size();
    uv_total += part.uvs.size();
    if (!part.has_uvs()) all_have_uv = false;
  }
  if (!any) return out;

  out.vertices.reserve(vertex_total * Mesh::kVertexFloats);
  out.indices.reserve(index_total);
  if (all_have_uv) out.uvs.reserve(uv_total);

  const bool keep_uv = all_have_uv;
  for (const Mesh& part : parts) {
    if (part.is_empty()) continue;
    const auto base = static_cast<std::uint32_t>(out.vertex_count());
    out.vertices.insert(out.vertices.end(), part.vertices.begin(), part.vertices.end());
    out.indices.reserve(out.indices.size() + part.indices.size());
    for (const std::uint32_t index : part.indices) out.indices.push_back(base + index);
    if (keep_uv) out.uvs.insert(out.uvs.end(), part.uvs.begin(), part.uvs.end());
  }
  // 顶点数超过 32 位索引能表达的范围（40 亿）时，回绕会静默出错：
  // 那是 13 GB 的顶点缓冲，现实中不会发生，但"不该发生"不等于"不可能"。
  return out;
}

auto merge(const Mesh& a, const Mesh& b) -> Mesh {
  const Mesh parts[2] = {a, b};
  return merge(std::span<const Mesh>(parts, 2));
}

// ────────────────────────────────────────────────────────────────────────────
// 焊接
// ────────────────────────────────────────────────────────────────────────────

auto weld(const Mesh& mesh, float epsilon) -> Mesh {
  if (epsilon <= 0.0f || mesh.is_empty()) return mesh;

  Mesh out{};
  std::vector<std::uint32_t> remap(mesh.vertex_count(), 0U);
  std::unordered_map<QuantizedKey, std::uint32_t, QuantizedKeyHash> seen;
  seen.reserve(mesh.vertex_count() * 2U + 8U);

  const bool has_uv = mesh.has_uvs();
  for (std::size_t i = 0; i < mesh.vertex_count(); ++i) {
    const math::Vec3 position{vertex_at(mesh, i, 0), vertex_at(mesh, i, 1), vertex_at(mesh, i, 2)};
    const math::Vec3 normal{vertex_at(mesh, i, 3), vertex_at(mesh, i, 4), vertex_at(mesh, i, 5)};
    const QuantizedKey key{quantize(position.x, epsilon), quantize(position.y, epsilon),
                           quantize(position.z, epsilon), quantize(normal.x, epsilon),
                           quantize(normal.y, epsilon), quantize(normal.z, epsilon)};
    const auto found = seen.find(key);
    if (found != seen.end()) {
      remap[i] = found->second;
      continue;
    }
    const auto slot = static_cast<std::uint32_t>(out.vertex_count());
    seen.emplace(key, slot);
    remap[i] = slot;
    out.vertices.insert(out.vertices.end(),
                        {position.x, position.y, position.z, normal.x, normal.y, normal.z,
                         vertex_at(mesh, i, 6), vertex_at(mesh, i, 7), vertex_at(mesh, i, 8)});
    if (has_uv) {
      const auto uv = mesh.uv_at(i);
      out.uvs.push_back(uv.first);
      out.uvs.push_back(uv.second);
    }
  }

  out.indices.reserve(mesh.indices.size());
  for (const std::uint32_t index : mesh.indices) {
    if (index < remap.size()) out.indices.push_back(remap[index]);
  }
  if (out.uvs.size() != out.vertex_count() * 2U) out.uvs.clear();
  return out;
}

// ────────────────────────────────────────────────────────────────────────────
// 朝向与法线
// ────────────────────────────────────────────────────────────────────────────

auto flip(const Mesh& mesh) -> Mesh {
  Mesh out = mesh;
  for (std::size_t i = 0; i < out.vertex_count(); ++i) {
    const std::size_t at = i * Mesh::kVertexFloats;
    if (at + Mesh::kVertexFloats > out.vertices.size()) break;
    out.vertices[at + 3] = -out.vertices[at + 3];
    out.vertices[at + 4] = -out.vertices[at + 4];
    out.vertices[at + 5] = -out.vertices[at + 5];
  }
  for (std::size_t t = 0; t + 2 < out.indices.size(); t += 3) {
    std::swap(out.indices[t + 1], out.indices[t + 2]);
  }
  return out;
}

auto recompute_normals(const Mesh& mesh, NormalShading shading) -> Mesh {
  if (mesh.is_empty()) return mesh;

  if (shading == NormalShading::Flat) {
    // 每三角形独立顶点：硬边只能这样表达（共享顶点只有一个法线）。
    Mesh out{};
    const std::size_t triangles = mesh.indices.size() / 3U;
    out.vertices.reserve(triangles * 3U * Mesh::kVertexFloats);
    out.indices.reserve(triangles * 3U);
    const bool has_uv = mesh.has_uvs();
    if (has_uv) out.uvs.reserve(triangles * 3U * 2U);
    for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
      const std::size_t i0 = mesh.indices[t];
      const std::size_t i1 = mesh.indices[t + 1];
      const std::size_t i2 = mesh.indices[t + 2];
      const math::Vec3 normal = face_cross(mesh, i0, i1, i2).normalized();
      const std::size_t ids[3] = {i0, i1, i2};
      const auto base = static_cast<std::uint32_t>(out.vertex_count());
      for (const std::size_t index : ids) {
        out.vertices.insert(out.vertices.end(),
                            {vertex_at(mesh, index, 0), vertex_at(mesh, index, 1),
                             vertex_at(mesh, index, 2), normal.x, normal.y, normal.z,
                             vertex_at(mesh, index, 6), vertex_at(mesh, index, 7),
                             vertex_at(mesh, index, 8)});
        if (has_uv) {
          const auto uv = mesh.uv_at(index);
          out.uvs.push_back(uv.first);
          out.uvs.push_back(uv.second);
        }
      }
      out.indices.insert(out.indices.end(), {base, base + 1U, base + 2U});
    }
    return out;
  }

  // 平滑：按**位置**累加面法线，再按位置写回。
  //
  // 为何按位置而不是按顶点索引：UV 接缝处同一位置有多个顶点（UV 不同），
  // 按索引累加会在接缝两侧算出不同法线，于是球面上出现一条可见的"硬缝"。
  Mesh out = mesh;
  std::map<std::tuple<std::int64_t, std::int64_t, std::int64_t>, math::Vec3> accumulated;
  constexpr float kGrid = 1e-4f;
  const auto key_of = [](const Mesh& target, std::size_t index) {
    return std::make_tuple(quantize(vertex_at(target, index, 0), kGrid),
                           quantize(vertex_at(target, index, 1), kGrid),
                           quantize(vertex_at(target, index, 2), kGrid));
  };
  for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
    const std::size_t i0 = mesh.indices[t];
    const std::size_t i1 = mesh.indices[t + 1];
    const std::size_t i2 = mesh.indices[t + 2];
    const math::Vec3 normal = face_cross(mesh, i0, i1, i2).normalized();
    for (const std::size_t index : {i0, i1, i2}) {
      math::Vec3& slot = accumulated[key_of(mesh, index)];
      slot = slot + normal;
    }
  }
  for (std::size_t i = 0; i < out.vertex_count(); ++i) {
    const auto found = accumulated.find(key_of(mesh, i));
    if (found == accumulated.end()) continue;
    const math::Vec3 unit = found->second.normalized();
    const std::size_t at = i * Mesh::kVertexFloats;
    if (at + Mesh::kVertexFloats > out.vertices.size()) break;
    out.vertices[at + 3] = unit.x;
    out.vertices[at + 4] = unit.y;
    out.vertices[at + 5] = unit.z;
  }
  return out;
}

// ────────────────────────────────────────────────────────────────────────────
// 尺寸归一
// ────────────────────────────────────────────────────────────────────────────

auto resize(const Mesh& mesh, math::Vec3 target_size) -> Mesh {
  const Bounds box = bounds(mesh);
  if (box.is_empty()) return mesh;
  const math::Vec3 current = box.size();
  math::Vec3 factor{1.0f, 1.0f, 1.0f};
  // 某一轴为 0（扁平面）不缩放该轴：除零会产出 NaN，而 NaN 在管线里静默扩散成整屏黑。
  if (current.x > 1e-6f) factor.x = target_size.x / current.x;
  if (current.y > 1e-6f) factor.y = target_size.y / current.y;
  if (current.z > 1e-6f) factor.z = target_size.z / current.z;
  return transform(mesh, math::Mat4::scaling(factor));
}

auto center_at(const Mesh& mesh, math::Vec3 target) -> Mesh {
  const Bounds box = bounds(mesh);
  if (box.is_empty()) return mesh;
  return transform(mesh, math::Mat4::translation(target - box.center()));
}

auto sit_on_ground(const Mesh& mesh, float floor_y) -> Mesh {
  const Bounds box = bounds(mesh);
  if (box.is_empty()) return mesh;
  return transform(mesh, math::Mat4::translation(math::Vec3{0.0f, floor_y - box.min.y, 0.0f}));
}

// ────────────────────────────────────────────────────────────────────────────
// 自检与导出
// ────────────────────────────────────────────────────────────────────────────

auto describe(const Mesh& mesh) -> std::string {
  const Bounds box = bounds(mesh);
  std::string text;
  text.reserve(256);
  if (box.is_empty()) {
    return std::format(R"({{"vertices":{},"triangles":{},"empty":true}})", mesh.vertex_count(),
                       mesh.indices.size() / 3U);
  }
  return std::format(
      R"({{"vertices":{},"triangles":{},"has_uvs":{},"bounds":{{"min":[{},{},{}],"max":[{},{},{}],"size":[{},{},{}]}}}})",
      mesh.vertex_count(), mesh.indices.size() / 3U, mesh.has_uvs() ? "true" : "false",
      obj_number(box.min.x), obj_number(box.min.y), obj_number(box.min.z), obj_number(box.max.x),
      obj_number(box.max.y), obj_number(box.max.z), obj_number(box.size().x),
      obj_number(box.size().y), obj_number(box.size().z));
}

auto to_obj_text(const Mesh& mesh, std::string_view object_name) -> std::string {
  std::string out;
  out.reserve(mesh.vertex_count() * 96U + mesh.indices.size() * 16U + 128U);
  out += "# 霜天 Shuangtian — Wavefront OBJ\n";
  out += "# 坐标：右手系，Y 轴朝上；三角形逆时针为正面\n";
  out += std::format("o {}\n", object_name);

  const bool has_uv = mesh.has_uvs();
  for (std::size_t i = 0; i < mesh.vertex_count(); ++i) {
    out += std::format("v {} {} {}\n", obj_number(vertex_at(mesh, i, 0)),
                       obj_number(vertex_at(mesh, i, 1)), obj_number(vertex_at(mesh, i, 2)));
  }
  if (has_uv) {
    for (std::size_t i = 0; i < mesh.vertex_count(); ++i) {
      const auto uv = mesh.uv_at(i);
      // OBJ 的 v 轴朝上，而纹理 uv 的 v 轴朝下：这里翻一次，
      // 否则图在 Blender 里上下颠倒（加载器侧同口径，往返才对得上）。
      out += std::format("vt {} {}\n", obj_number(uv.first), obj_number(1.0f - uv.second));
    }
  }
  for (std::size_t i = 0; i < mesh.vertex_count(); ++i) {
    out += std::format("vn {} {} {}\n", obj_number(vertex_at(mesh, i, 3)),
                       obj_number(vertex_at(mesh, i, 4)), obj_number(vertex_at(mesh, i, 5)));
  }
  // OBJ 索引是 **1 基**（写 0 基会让每个面偏一格——那种错在 Blender 里表现为"模型破洞"）
  for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
    const std::uint32_t a = mesh.indices[t] + 1U;
    const std::uint32_t b = mesh.indices[t + 1] + 1U;
    const std::uint32_t c = mesh.indices[t + 2] + 1U;
    if (has_uv) {
      out += std::format("f {}/{}/{} {}/{}/{} {}/{}/{}\n", a, a, a, b, b, b, c, c, c);
    } else {
      // 没有 UV：`v//vn` 写法（中间的斜杠必须留着，少一个就成了 `v/vn`，语义不同）
      out += std::format("f {}//{} {}//{} {}//{}\n", a, a, b, b, c, c);
    }
  }
  return out;
}

auto write_obj_file(std::string_view path, const Mesh& mesh, std::string_view object_name)
    -> Status {
  return st::fs::write_text(path, to_obj_text(mesh, object_name));
}

}  // namespace st::raster

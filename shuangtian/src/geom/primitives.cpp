#include "st/raster/primitives.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <map>
#include <tuple>
#include <vector>

#include "st/raster/color_pipeline.hpp"

namespace st::raster::primitives {

namespace {

using math::Color;
using math::Vec3;
using UV = std::pair<float, float>;

// ────────────────────────────────────────────────────────────────────────────
// Builder：造网格的唯一通道
// ────────────────────────────────────────────────────────────────────────────

/// 顶点累加器 + **自动定向**。
///
/// ## 为什么把"绕序"从生成器里拿掉（本模块最贵的一课）
///
/// 第一版每个生成器都手写三角形绕序，结果 22 个图元里有 6 个出错：
/// tube 内壁 20 个三角形全反、gear 侧壁 32 个反、torus_knot 与 helix 整体内外翻转、
/// wedge 少了两个端面、capsule 有 10 个零面积三角形。它们的共同点是：
/// **代码"看起来"是对的**（顶点、法线、面都在），只有几何法线与顶点法线反向——
/// 而这一件事只能靠算，不能靠看。
///
/// 现在：生成器只给「顶点位置 + 应有的法线 + UV」，**顺序随便写**；
/// `face()` 算一遍几何法线（叉积），与解析法线反向就把顶点顺序倒过来。
/// 于是"绕序与法线同向"从"每个生成器各自保证的不变式"变成**结构上不可能违反**。
///
/// 代价：每个面多一次叉积 + 一次点积（相对顶点写入可忽略）。
/// 收益：删掉了整整一类缺陷，而且后面每加一个图元都自动正确。
struct Builder {
  Mesh mesh{};
  bool with_uv{true};
  math::Vec3 color{1.0f, 1.0f, 1.0f};
  /// 量化格：位置/法线/UV 进哈希前的整数化步长（1e-6 远小于有意义的几何差异）。
  double grid{1.0e-5};

  [[nodiscard]] auto quantize(double value, double step) const noexcept -> std::int64_t {
    const double scaled = value / step;
    const double rounded = std::round(scaled);
    constexpr double kLimit = 9.0e15;
    if (!(rounded > -kLimit && rounded < kLimit)) return 0;
    return static_cast<std::int64_t>(rounded);
  }

  /// 取/建一个顶点。同位置 + 同法线 + 同 UV 的顶点复用（接缝与极点才能共享，
  /// 而"水密"才是可验证的事实，不是靠肉眼数面）。
  auto add(Vec3 position, Vec3 normal, UV uv) -> std::uint32_t {
    const std::int64_t px = quantize(position.x, grid);
    const std::int64_t py = quantize(position.y, grid);
    const std::int64_t pz = quantize(position.z, grid);
    const std::int64_t nx = quantize(normal.x, 1.0e-3);
    const std::int64_t ny = quantize(normal.y, 1.0e-3);
    const std::int64_t nz = quantize(normal.z, 1.0e-3);
    const std::int64_t uu = quantize(uv.first, 1.0e-4);
    const std::int64_t vv = quantize(uv.second, 1.0e-4);
    const auto key = std::make_tuple(px, py, pz, nx, ny, nz, uu, vv);
    if (const auto found = index_.find(key); found != index_.end()) return found->second;
    const auto slot = static_cast<std::uint32_t>(mesh.vertex_count());
    push_vertex(mesh.vertices, position, normal, color);
    if (with_uv) {
      mesh.uvs.push_back(uv.first);
      mesh.uvs.push_back(uv.second);
    }
    index_.emplace(key, slot);
    return slot;
  }

  /// 取/建一个顶点（无 UV 时自动退回 0,0）。
  auto add(Vec3 position, Vec3 normal) -> std::uint32_t { return add(position, normal, {0.0f, 0.0f}); }

  /// 一个面（3 个及以上顶点）：**自动定向**后再拆成三角形扇。
  ///
  /// `positions` / `normals` / `uvs` 长度相同；顺序任意（会按几何法线纠正）。
  void face(const std::vector<Vec3>& positions, const std::vector<Vec3>& normals,
            const std::vector<UV>& uvs) {
    const std::size_t count = std::min({positions.size(), normals.size(), uvs.size()});
    if (count < 3U) return;
    // 几何法线：取第一个面积非零的连续三元组
    Vec3 geo{};
    for (std::size_t i = 1; i + 1U < count; ++i) {
      geo = (positions[i] - positions[0]).cross(positions[i + 1U] - positions[0]);
      if (geo.length() > 1.0e-12f) break;
    }
    if (geo.length() <= 1.0e-12f) return;   // 退化面：不写（写了只会污染统计与法线）
    Vec3 mean{};
    for (std::size_t i = 0; i < count; ++i) mean = mean + normals[i];
    const bool flip = geo.dot(mean) < 0.0f;
    std::vector<std::uint32_t> slots;
    slots.reserve(count);
    for (std::size_t k = 0; k < count; ++k) {
      const std::size_t i = flip ? count - 1U - k : k;
      slots.push_back(add(positions[i], normals[i], uvs[i]));
    }
    for (std::size_t i = 1; i + 1U < count; ++i) {
      mesh.indices.insert(mesh.indices.end(), {slots[0], slots[i], slots[i + 1U]});
    }
  }

  void triangle(Vec3 p0, Vec3 p1, Vec3 p2, Vec3 n0, Vec3 n1, Vec3 n2, UV u0 = {}, UV u1 = {},
                UV u2 = {}) {
    face({p0, p1, p2}, {n0, n1, n2}, {u0, u1, u2});
  }

  void quad(Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, Vec3 n0, Vec3 n1, Vec3 n2, Vec3 n3, UV u0 = {},
            UV u1 = {}, UV u2 = {}, UV u3 = {}) {
    face({p0, p1, p2, p3}, {n0, n1, n2, n3}, {u0, u1, u2, u3});
  }

  /// 收尾：UV 不完整就整体丢掉（`has_uvs()` 的判据是"两倍顶点数"）。
  [[nodiscard]] auto finish() -> Mesh {
    if (mesh.uvs.size() != mesh.vertex_count() * 2U) mesh.uvs.clear();
    return std::move(mesh);
  }

 private:
  std::map<std::tuple<std::int64_t, std::int64_t, std::int64_t, std::int64_t, std::int64_t,
                      std::int64_t, std::int64_t, std::int64_t>,
           std::uint32_t>
      index_{};
};

// ────────────────────────────────────────────────────────────────────────────
// 小工具
// ────────────────────────────────────────────────────────────────────────────

[[nodiscard]] auto linear_color(Color color) noexcept -> Vec3 {
  return srgb_to_linear(Vec3{static_cast<float>(color.r) / 255.0f,
                             static_cast<float>(color.g) / 255.0f,
                             static_cast<float>(color.b) / 255.0f});
}

/// 绕 Y 轴一圈的环点。`theta` 增大 = 从 +Y 俯视**顺时针**（x = sin, z = cos）——
/// 这个方向本身不重要（`face()` 会自动定向），但**所有生成器要一致**，
/// 否则 UV 接缝会落在不同的位置。
[[nodiscard]] auto ring_point(float radius, float theta, float y) noexcept -> Vec3 {
  return Vec3{radius * std::sin(theta), y, radius * std::cos(theta)};
}

[[nodiscard]] auto angle_at(int index, int count) noexcept -> float {
  return 2.0f * kPi * static_cast<float>(index) / static_cast<float>(count);
}

/// 绕 Y 轴扭转（`twist` = 整段高度上的总扭转角，弧度）。
[[nodiscard]] auto twisted(Vec3 point, float y, float half_height, float twist) noexcept -> Vec3 {
  if (twist == 0.0f || half_height <= 1.0e-6f) return point;
  const float t = (y + half_height) / (2.0f * half_height);   // 0（底）→ 1（顶）
  const float angle = twist * t;
  const float c = std::cos(angle);
  const float s = std::sin(angle);
  return Vec3{point.x * c + point.z * s, point.y, -point.x * s + point.z * c};
}

}  // namespace

// ────────────────────────────────────────────────────────────────────────────
// 名字 ↔ 枚举
// ────────────────────────────────────────────────────────────────────────────

auto kind_name(PrimitiveKind kind) noexcept -> std::string_view {
  switch (kind) {
    case PrimitiveKind::Plane: return "plane";
    case PrimitiveKind::Cube: return "cube";
    case PrimitiveKind::UvSphere: return "uv_sphere";
    case PrimitiveKind::IcoSphere: return "ico_sphere";
    case PrimitiveKind::Cylinder: return "cylinder";
    case PrimitiveKind::Cone: return "cone";
    case PrimitiveKind::Capsule: return "capsule";
    case PrimitiveKind::Torus: return "torus";
    case PrimitiveKind::Disc: return "disc";
    case PrimitiveKind::Circle: return "circle";
    case PrimitiveKind::Grid: return "grid";
    case PrimitiveKind::Tube: return "tube";
    case PrimitiveKind::Prism: return "prism";
    case PrimitiveKind::Gear: return "gear";
    case PrimitiveKind::TorusKnot: return "torus_knot";
    case PrimitiveKind::Arrow: return "arrow";
    case PrimitiveKind::Wedge: return "wedge";
    case PrimitiveKind::Tetrahedron: return "tetrahedron";
    case PrimitiveKind::Octahedron: return "octahedron";
    case PrimitiveKind::Dodecahedron: return "dodecahedron";
    case PrimitiveKind::Icosahedron: return "icosahedron";
    case PrimitiveKind::Helix: return "helix";
  }
  return "unknown";
}

namespace {
constexpr std::array<PrimitiveKind, 22> kAllKinds = {
    PrimitiveKind::Plane,        PrimitiveKind::Cube,         PrimitiveKind::UvSphere,
    PrimitiveKind::IcoSphere,    PrimitiveKind::Cylinder,     PrimitiveKind::Cone,
    PrimitiveKind::Capsule,      PrimitiveKind::Torus,        PrimitiveKind::Disc,
    PrimitiveKind::Circle,       PrimitiveKind::Grid,         PrimitiveKind::Tube,
    PrimitiveKind::Prism,        PrimitiveKind::Gear,         PrimitiveKind::TorusKnot,
    PrimitiveKind::Arrow,        PrimitiveKind::Wedge,        PrimitiveKind::Tetrahedron,
    PrimitiveKind::Octahedron,   PrimitiveKind::Dodecahedron, PrimitiveKind::Icosahedron,
    PrimitiveKind::Helix};
}  // namespace

auto all_kind_names() -> std::string {
  std::string text;
  for (const PrimitiveKind kind : kAllKinds) {
    if (!text.empty()) text += ", ";
    text += kind_name(kind);
  }
  return text;
}

auto parse_kind(std::string_view name) -> std::optional<PrimitiveKind> {
  // 规范化：小写；`-` / 空格 / `.` → `_`（`uv-sphere`、`UV Sphere` 都收）
  std::string key;
  key.reserve(name.size());
  for (const char ch : name) {
    if (ch == '-' || ch == ' ' || ch == '.') {
      key.push_back('_');
    } else {
      key.push_back(static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch));
    }
  }
  while (!key.empty() && key.back() == '_') key.pop_back();

  struct Alias {
    const char* name;
    PrimitiveKind kind;
  };
  // 别名只收**真实存在过的叫法**（Blender / three.js / Unity / 日常英文对应）。
  // 不收生造词：别名越杂，"拼错也能过"越会掩盖真拼错。
  static constexpr Alias kAliases[] = {
      {"plane", PrimitiveKind::Plane},          {"quad", PrimitiveKind::Plane},
      {"cube", PrimitiveKind::Cube},            {"box", PrimitiveKind::Cube},
      {"uv_sphere", PrimitiveKind::UvSphere},   {"sphere", PrimitiveKind::UvSphere},
      {"ball", PrimitiveKind::UvSphere},        {"globe", PrimitiveKind::UvSphere},
      {"ico_sphere", PrimitiveKind::IcoSphere}, {"icosphere", PrimitiveKind::IcoSphere},
      {"cylinder", PrimitiveKind::Cylinder},    {"cone", PrimitiveKind::Cone},
      {"capsule", PrimitiveKind::Capsule},      {"pill", PrimitiveKind::Capsule},
      {"torus", PrimitiveKind::Torus},          {"donut", PrimitiveKind::Torus},
      {"doughnut", PrimitiveKind::Torus},       {"disc", PrimitiveKind::Disc},
      {"disk", PrimitiveKind::Disc},            {"circle", PrimitiveKind::Circle},
      {"ring", PrimitiveKind::Circle},          {"grid", PrimitiveKind::Grid},
      {"tube", PrimitiveKind::Tube},            {"open_tube", PrimitiveKind::Tube},
      {"pipe", PrimitiveKind::Tube},            {"prism", PrimitiveKind::Prism},
      {"ngon_prism", PrimitiveKind::Prism},     {"gear", PrimitiveKind::Gear},
      {"cog", PrimitiveKind::Gear},             {"torus_knot", PrimitiveKind::TorusKnot},
      {"knot", PrimitiveKind::TorusKnot},       {"arrow", PrimitiveKind::Arrow},
      {"wedge", PrimitiveKind::Wedge},          {"ramp", PrimitiveKind::Wedge},
      {"tetrahedron", PrimitiveKind::Tetrahedron},
      {"tetra", PrimitiveKind::Tetrahedron},    {"octahedron", PrimitiveKind::Octahedron},
      {"octa", PrimitiveKind::Octahedron},      {"dodecahedron", PrimitiveKind::Dodecahedron},
      {"dodeca", PrimitiveKind::Dodecahedron},  {"icosahedron", PrimitiveKind::Icosahedron},
      {"icosa", PrimitiveKind::Icosahedron},    {"helix", PrimitiveKind::Helix},
      {"spring", PrimitiveKind::Helix},         {"coil", PrimitiveKind::Helix},
  };
  for (const Alias& alias : kAliases) {
    if (key == alias.name) return alias.kind;
  }
  return std::nullopt;
}

// ────────────────────────────────────────────────────────────────────────────
// 参数归一
// ────────────────────────────────────────────────────────────────────────────

auto sanitized(PrimitiveParams params) -> PrimitiveParams {
  constexpr float kMinRadius = 1.0e-4f;
  if (!(params.radius > kMinRadius)) params.radius = kMinRadius;
  if (!(params.major_radius > kMinRadius)) params.major_radius = kMinRadius;
  if (!(params.minor_radius > kMinRadius)) params.minor_radius = kMinRadius;
  params.minor_radius = std::min(params.minor_radius, params.major_radius);
  if (!(params.depth > kMinRadius)) params.depth = kMinRadius;
  if (params.height < 0.0f) params.height = 0.0f;
  params.top_radius = std::clamp(params.top_radius, 0.0f, params.radius);

  // 3 是闭合环的下限（2 段会退化成一片"双面三角形"，法线无从定义）
  params.segments = std::max(params.segments, 3);
  params.major_segments = std::max(params.major_segments, 3);
  params.minor_segments = std::max(params.minor_segments, 3);
  params.rings = std::max(params.rings, 2);
  params.columns = std::max(params.columns, 1);
  params.rows = std::max(params.rows, 1);
  params.subdivisions = std::clamp(params.subdivisions, 0, 6);
  if (params.sides > 0) params.segments = std::max(params.sides, 3);
  params.teeth = std::max(params.teeth, 3);
  if (!(params.tooth_height >= 0.0f)) params.tooth_height = 0.0f;
  params.tooth_height = std::min(params.tooth_height, params.radius);
  params.tooth_width = std::clamp(params.tooth_width, 0.05f, 0.95f);
  params.knot_p = std::max(params.knot_p, 1);
  params.knot_q = std::max(params.knot_q, 1);
  if (!(params.turns >= 0.25f)) params.turns = 0.25f;
  if (!(params.taper >= 0.0f)) params.taper = 0.0f;
  params.taper = std::min(params.taper, 0.9f);
  return params;
}

// ────────────────────────────────────────────────────────────────────────────
// 平面 / 网格 / 盘 / 环带
// ────────────────────────────────────────────────────────────────────────────

namespace {

/// 单面矩形（`columns × rows` 分段）。`face_camera` = 朝 +Z（立面），否则朝 +Y（地面）。
auto make_rect(float width, float depth_length, int columns, int rows, bool face_camera,
               Vec3 color, bool with_uv) -> Mesh {
  Builder out{};
  out.with_uv = with_uv;
  out.color = color;
  const float half_w = width * 0.5f;
  const float half_d = depth_length * 0.5f;
  const Vec3 normal = face_camera ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{0.0f, 1.0f, 0.0f};

  const auto point_at = [&](int i, int j) {
    const float x = -half_w + width * static_cast<float>(i) / static_cast<float>(columns);
    const float z = -half_d + depth_length * static_cast<float>(j) / static_cast<float>(rows);
    if (face_camera) return Vec3{x, z, 0.0f};
    return Vec3{x, 0.0f, z};
  };
  for (int j = 0; j < rows; ++j) {
    for (int i = 0; i < columns; ++i) {
      const float u0 = static_cast<float>(i) / static_cast<float>(columns);
      const float u1 = static_cast<float>(i + 1) / static_cast<float>(columns);
      // UV 的 v 轴翻转：纹理原点在左上（与 `Mesh::textured_plane` 同口径）
      const float v0 = 1.0f - static_cast<float>(j) / static_cast<float>(rows);
      const float v1 = 1.0f - static_cast<float>(j + 1) / static_cast<float>(rows);
      out.quad(point_at(i, j), point_at(i + 1, j), point_at(i + 1, j + 1), point_at(i, j + 1),
               normal, normal, normal, normal, {u0, v0}, {u1, v0}, {u1, v1}, {u0, v1});
    }
  }
  return out.finish();
}

}  // namespace

auto make_plane(const PrimitiveParams& params) -> Mesh {
  const PrimitiveParams p = sanitized(params);
  return make_rect(p.radius * 2.0f, p.depth, 1, 1, p.face_camera, linear_color(p.color), p.with_uv);
}

auto make_grid(const PrimitiveParams& params) -> Mesh {
  const PrimitiveParams p = sanitized(params);
  return make_rect(p.radius * 2.0f, p.depth, p.columns, p.rows, p.face_camera,
                   linear_color(p.color), p.with_uv);
}

auto make_disc(const PrimitiveParams& params) -> Mesh {
  const PrimitiveParams p = sanitized(params);
  Builder out{};
  out.with_uv = p.with_uv;
  out.color = linear_color(p.color);
  const Vec3 normal = p.face_camera ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{0.0f, 1.0f, 0.0f};
  const auto place = [&](float radius, float theta) {
    return p.face_camera ? Vec3{radius * std::cos(theta), radius * std::sin(theta), 0.0f}
                         : ring_point(radius, theta, 0.0f);
  };
  for (int i = 0; i < p.segments; ++i) {
    const float a0 = angle_at(i, p.segments);
    const float a1 = angle_at(i + 1, p.segments);
    const UV uv0{0.5f + 0.5f * std::cos(a0), 0.5f + 0.5f * std::sin(a0)};
    const UV uv1{0.5f + 0.5f * std::cos(a1), 0.5f + 0.5f * std::sin(a1)};
    out.triangle(Vec3{0.0f, 0.0f, 0.0f}, place(p.radius, a0), place(p.radius, a1), normal, normal,
                 normal, {0.5f, 0.5f}, uv0, uv1);
  }
  return out.finish();
}

auto make_circle(const PrimitiveParams& params) -> Mesh {
  // 环带（不是"线"）：渲染器只画三角形，宽度为 0 的线在三维里不存在。
  // 一个细环带才是"光环 / 描边 / 轨道"的正确几何，而且它水密、可导出复用。
  PrimitiveParams p = sanitized(params);
  const float inner = p.radius * (1.0f - std::clamp(p.tooth_width, 0.02f, 0.9f));
  Builder out{};
  out.with_uv = p.with_uv;
  out.color = linear_color(p.color);
  const Vec3 normal = p.face_camera ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{0.0f, 1.0f, 0.0f};
  const auto place = [&](float radius, float theta) {
    return p.face_camera ? Vec3{radius * std::cos(theta), radius * std::sin(theta), 0.0f}
                         : ring_point(radius, theta, 0.0f);
  };
  for (int i = 0; i < p.segments; ++i) {
    const float a0 = angle_at(i, p.segments);
    const float a1 = angle_at(i + 1, p.segments);
    const float u0 = static_cast<float>(i) / static_cast<float>(p.segments);
    const float u1 = static_cast<float>(i + 1) / static_cast<float>(p.segments);
    out.quad(place(p.radius, a0), place(p.radius, a1), place(inner, a1), place(inner, a0), normal,
             normal, normal, normal, {u0, 1.0f}, {u1, 1.0f}, {u1, 0.0f}, {u0, 0.0f});
  }
  return out.finish();
}

// ────────────────────────────────────────────────────────────────────────────
// 立方体 / 楔形 / 正多面体
// ────────────────────────────────────────────────────────────────────────────

auto make_cube(const PrimitiveParams& params) -> Mesh {
  PrimitiveParams p = sanitized(params);
  // `radius` = 半边长（与球一致：所有图元的 `radius` 都是"到中心的距离"）。
  // `depth` 是 Y 向长度；`height`（>0 时）是 Z 向长度——于是同一入口能造长方体。
  const float hx = p.radius;
  const float hy = p.depth * 0.5f;
  const float hz = p.height > 0.0f ? p.height * 0.5f : p.radius;
  Builder out{};
  out.with_uv = p.with_uv;
  out.color = linear_color(p.color);
  const auto p000 = Vec3{-hx, -hy, -hz};
  const auto p100 = Vec3{hx, -hy, -hz};
  const auto p110 = Vec3{hx, hy, -hz};
  const auto p010 = Vec3{-hx, hy, -hz};
  const auto p001 = Vec3{-hx, -hy, hz};
  const auto p101 = Vec3{hx, -hy, hz};
  const auto p111 = Vec3{hx, hy, hz};
  const auto p011 = Vec3{-hx, hy, hz};
  struct Side {
    Vec3 a, b, c, d;
    Vec3 normal;
  };
  const Side sides[6] = {
      {p001, p101, p111, p011, {0, 0, 1}},    // +Z
      {p100, p000, p010, p110, {0, 0, -1}},   // -Z
      {p101, p100, p110, p111, {1, 0, 0}},    // +X
      {p000, p001, p011, p010, {-1, 0, 0}},   // -X
      {p011, p111, p110, p010, {0, 1, 0}},    // +Y
      {p000, p100, p101, p001, {0, -1, 0}},   // -Y
  };
  for (const Side& side : sides) {
    out.quad(side.a, side.b, side.c, side.d, side.normal, side.normal, side.normal, side.normal,
             {0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}, {0.0f, 0.0f});
  }
  // ## UV 按**每个面各自**归一化到 0..1（不是按世界尺寸、也不是全局归一化）
  //
  // 踩过两次，都是"看起来能贴、但图案是条纹"：
  //
  // 1. 六个面硬编码 0..1 → 非立方体时各面长宽比不同，方格被拉成长条；
  // 2. 按世界尺寸归一化（"1 米一格"或"最大面 = 整张图"）→ 方格比例对了，
  //    但**小面只覆盖 UV 的一部分**，`texture_tile` 一乘就露馅（图案被切一半再拉伸）。
  //
  // 正确做法与所有建模软件一致：**每个面独立映射到 0..1**。
  // 于是"一张图铺满每一个面"，`texture_tile: 2` 就是对每个面平铺 2×2——
  // 语义与用户预期一致，换物体尺寸也不会变。
  // 代价：面上图案的长宽比随面尺寸变化（宽面上方格会被拉宽），
  // 这是 UV 映射的固有取舍；要等比应当用世界单位 UV（那是另一个模式，不入默认）。
  if (p.with_uv) {
    auto& vertices = out.mesh.vertices;
    auto& uvs = out.mesh.uvs;
    uvs.clear();
    uvs.reserve((vertices.size() / Mesh::kVertexFloats) * 2U);
    for (std::size_t i = 0; i + Mesh::kVertexFloats <= vertices.size();
         i += Mesh::kVertexFloats) {
      const Vec3 position{vertices[i], vertices[i + 1U], vertices[i + 2U]};
      const Vec3 normal{vertices[i + 3U], vertices[i + 4U], vertices[i + 5U]};
      const float ax = std::abs(normal.x);
      const float ay = std::abs(normal.y);
      const float az = std::abs(normal.z);
      float u = 0.5f;
      float v = 0.5f;
      if (ax >= ay && ax >= az) {
        // ±X 面：切向 = (Z, Y)，按该面的实际半宽 hz / 半高 hy 归一化
        u = hz > 1.0e-6f ? position.z / (2.0f * hz) + 0.5f : 0.5f;
        v = hy > 1.0e-6f ? position.y / (2.0f * hy) + 0.5f : 0.5f;
      } else if (ay >= az) {
        // ±Y 面：切向 = (X, Z)
        u = hx > 1.0e-6f ? position.x / (2.0f * hx) + 0.5f : 0.5f;
        v = hz > 1.0e-6f ? position.z / (2.0f * hz) + 0.5f : 0.5f;
      } else {
        // ±Z 面：切向 = (X, Y)
        u = hx > 1.0e-6f ? position.x / (2.0f * hx) + 0.5f : 0.5f;
        v = hy > 1.0e-6f ? position.y / (2.0f * hy) + 0.5f : 0.5f;
      }
      uvs.push_back(u);
      uvs.push_back(v);
    }
  }
  return out.finish();
}

auto make_wedge(const PrimitiveParams& params) -> Mesh {
  // 楔形 = 底面矩形 + 一条顶棱（斜坡 / 屋顶 / 三角靠板）：5 个顶点、5 个面。
  //
  // 第一版手写了 6 个三角形，漏掉两个三角端面、底面绕序也反了
  // （测试读到 6 条边界边 + 2 条非流形边）。教训：**面表要按拓扑写全**，
  // 而不是想到哪写到哪；绕序交给 `Builder::face()` 自动定向。
  PrimitiveParams p = sanitized(params);
  const float hx = p.radius;
  const float hy = p.depth * 0.5f;
  const float hz = p.height > 0.0f ? p.height * 0.5f : p.radius;
  Builder out{};
  out.with_uv = p.with_uv;
  out.color = linear_color(p.color);

  const Vec3 a{-hx, -hy, hz};    // 前下左
  const Vec3 b{hx, -hy, hz};     // 前下右
  const Vec3 c{hx, -hy, -hz};    // 后下右
  const Vec3 d{-hx, -hy, -hz};   // 后下左
  const Vec3 e{-hx, hy, hz};     // 前上左（顶棱）
  const Vec3 f{-hx, hy, -hz};    // 后上左（顶棱）

  // 五个面（顺序无关；法线由几何自动定向，这里给的是**约定方向**便于阅读）
  const std::vector<Vec3> bottom = {a, b, c, d};
  const std::vector<Vec3> front = {a, b, e};        // +Z 斜三角
  const std::vector<Vec3> back = {d, f, c};         // -Z 斜三角
  const std::vector<Vec3> slope = {a, e, f, d};     // 朝左上（顶面）
  const std::vector<Vec3> right_side = {b, c, f, e};  // 朝右下（斜面）
  const std::vector<std::vector<Vec3>> faces = {bottom, front, back, slope, right_side};
  const auto face_normal = [](const std::vector<Vec3>& points) {
    Vec3 n = (points[1] - points[0]).cross(points[2] - points[0]);
    if (n.length() < 1.0e-9f) n = (points[2] - points[0]).cross(points[3] - points[0]);
    return n.normalized();
  };
  for (const std::vector<Vec3>& points : faces) {
    const Vec3 n = face_normal(points);
    std::vector<Vec3> normals(points.size(), n);
    std::vector<UV> uvs;
    uvs.reserve(points.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
      uvs.emplace_back(i == 1U || i == 2U ? 1.0f : 0.0f, i <= 1U ? 1.0f : 0.0f);
    }
    out.face(points, normals, uvs);
  }
  return out.finish();
}

namespace {

/// 正多面体的通用生成：`unit_points` 是外接球上的顶点（会被归一化后乘半径），
/// `triangles` 是面索引。**法线由面心方向确定**，绕序交给 `Builder::face()`。
auto polyhedron(float radius, const std::vector<Vec3>& unit_points,
                const std::vector<std::array<int, 3>>& triangles, Vec3 color, bool with_uv,
                bool flat) -> Mesh {
  Builder out{};
  out.with_uv = with_uv;
  out.color = color;
  std::vector<Vec3> points;
  points.reserve(unit_points.size());
  for (const Vec3& p : unit_points) points.push_back(p.normalized() * radius);

  for (const std::array<int, 3>& tri : triangles) {
    const Vec3& p0 = points[static_cast<std::size_t>(tri[0])];
    const Vec3& p1 = points[static_cast<std::size_t>(tri[1])];
    const Vec3& p2 = points[static_cast<std::size_t>(tri[2])];
    const Vec3 centroid = (p0 + p1 + p2) * (1.0f / 3.0f);
    // 朝外法线 = 从中心指向面心（对凸多面体永远成立，不依赖顶点表的绕序）
    const Vec3 face_normal = centroid.normalized();
    std::vector<Vec3> positions = {p0, p1, p2};
    std::vector<Vec3> normals;
    if (flat) {
      normals.assign(3, face_normal);
    } else {
      normals = {p0.normalized(), p1.normalized(), p2.normalized()};
    }
    std::vector<UV> uvs;
    uvs.reserve(3);
    for (const Vec3& p : positions) {
      // 球面 UV（正多面体没有天然 UV；球面投影足够贴纯色/渐变）
      const float u = 0.5f + std::atan2(p.z, p.x) / (2.0f * kPi);
      const float v = 0.5f - std::asin(std::clamp(p.y / std::max(radius, 1.0e-6f), -1.0f, 1.0f)) / kPi;
      uvs.emplace_back(u, v);
    }
    out.face(positions, normals, uvs);
  }
  return out.finish();
}

constexpr float kPhi = 1.6180339887498948f;
constexpr float kInvPhi = 0.6180339887498948f;

/// 正二十面体的顶点与面（后面 ico_sphere / 十二面体 / 二十面体共用）。
auto icosahedron_data() -> std::pair<std::vector<Vec3>, std::vector<std::array<int, 3>>> {
  const float a = 1.0f;
  const float b = kInvPhi;
  std::vector<Vec3> points = {
      {-a, b, 0}, {a, b, 0},   {-a, -b, 0}, {a, -b, 0},
      {0, -a, b}, {0, a, b},   {0, -a, -b}, {0, a, -b},
      {b, 0, -a}, {b, 0, a},   {-b, 0, -a}, {-b, 0, a},
  };
  std::vector<std::array<int, 3>> faces = {
      {0, 11, 5}, {0, 5, 1},   {0, 1, 7},   {0, 7, 10},  {0, 10, 11},
      {1, 5, 9},  {5, 11, 4},  {11, 10, 2}, {10, 7, 6},  {7, 1, 8},
      {3, 9, 4},  {3, 4, 2},   {3, 2, 6},   {3, 6, 8},   {3, 8, 9},
      {4, 9, 5},  {2, 4, 11},  {6, 2, 10},  {8, 6, 7},   {9, 8, 1}};
  return {points, faces};
}

}  // namespace

auto make_tetrahedron(const PrimitiveParams& params) -> Mesh {
  const PrimitiveParams p = sanitized(params);
  const std::vector<Vec3> points = {
      {1.0f, 1.0f, 1.0f}, {1.0f, -1.0f, -1.0f}, {-1.0f, 1.0f, -1.0f}, {-1.0f, -1.0f, 1.0f}};
  const std::vector<std::array<int, 3>> faces = {{0, 1, 2}, {0, 3, 1}, {0, 2, 3}, {1, 3, 2}};
  return polyhedron(p.radius, points, faces, linear_color(p.color), p.with_uv, true);
}

auto make_octahedron(const PrimitiveParams& params) -> Mesh {
  const PrimitiveParams p = sanitized(params);
  const std::vector<Vec3> points = {
      {1.0f, 0.0f, 0.0f},  {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
      {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f},  {0.0f, 0.0f, -1.0f},
  };
  const std::vector<std::array<int, 3>> faces = {
      {0, 2, 4}, {4, 2, 1}, {1, 2, 5}, {5, 2, 0},
      {0, 4, 3}, {4, 1, 3}, {1, 5, 3}, {5, 0, 3}};
  return polyhedron(p.radius, points, faces, linear_color(p.color), p.with_uv, true);
}

auto make_icosahedron(const PrimitiveParams& params) -> Mesh {
  const PrimitiveParams p = sanitized(params);
  const auto [points, faces] = icosahedron_data();
  return polyhedron(p.radius, points, faces, linear_color(p.color), p.with_uv, true);
}

auto make_dodecahedron(const PrimitiveParams& params) -> Mesh {
  // 正十二面体 = 正二十面体的**对偶**（这一条是本模块最省事的一步）。
  //
  // ## 第一版为什么错
  //
  // 手写 12 个五边形的索引表——36 个三角形里有 33 条边界边，面之间根本对不上。
  // 那张表没有任何可验证的约束，只能靠"看起来像"。
  //
  // ## 对偶构造（天然正确）
  //
  // 二十面体的 20 个**面心** = 十二面体的 20 个顶点；
  // 二十面体的 12 个**顶点周围的面** = 十二面体的 12 个五边形
  // （把每个顶点周围的面按绕该顶点的极角排序，就得到五边形的正确环序）。
  const PrimitiveParams p = sanitized(params);
  const auto [ico_points, ico_faces] = icosahedron_data();

  std::vector<Vec3> dual;
  dual.reserve(ico_faces.size());
  for (const std::array<int, 3>& face : ico_faces) {
    const Vec3 center = (ico_points[static_cast<std::size_t>(face[0])] +
                         ico_points[static_cast<std::size_t>(face[1])] +
                         ico_points[static_cast<std::size_t>(face[2])]) *
                        (1.0f / 3.0f);
    dual.push_back(center.normalized());
  }

  std::vector<std::vector<int>> around(ico_points.size());
  for (std::size_t f = 0; f < ico_faces.size(); ++f) {
    for (const int vertex : ico_faces[f]) around[static_cast<std::size_t>(vertex)].push_back(static_cast<int>(f));
  }

  std::vector<std::array<int, 3>> triangles;
  for (std::size_t v = 0; v < around.size(); ++v) {
    std::vector<int>& ring = around[v];
    if (ring.size() < 3U) continue;
    const Vec3 axis = ico_points[v].normalized();
    const Vec3 helper = std::abs(axis.y) < 0.9f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
    const Vec3 e1 = axis.cross(helper).normalized();
    const Vec3 e2 = axis.cross(e1).normalized();
    std::sort(ring.begin(), ring.end(), [&](int lhs, int rhs) {
      const Vec3 pl = dual[static_cast<std::size_t>(lhs)];
      const Vec3 pr = dual[static_cast<std::size_t>(rhs)];
      return std::atan2(pl.dot(e2), pl.dot(e1)) < std::atan2(pr.dot(e2), pr.dot(e1));
    });
    // 五边形是凸的，扇形三角化正确
    for (std::size_t k = 1; k + 1U < ring.size(); ++k) {
      triangles.push_back({ring[0], ring[k], ring[k + 1U]});
    }
  }
  return polyhedron(p.radius, dual, triangles, linear_color(p.color), p.with_uv, true);
}

// ────────────────────────────────────────────────────────────────────────────
// 球
// ────────────────────────────────────────────────────────────────────────────

auto make_uv_sphere(const PrimitiveParams& params) -> Mesh {
  PrimitiveParams p = sanitized(params);
  const int rings = p.rings;       // 纬度环数（从南极到北极的段数）
  const int segments = p.segments;
  Builder out{};
  out.with_uv = p.with_uv;
  out.color = linear_color(p.color);

  // 极点单独一个顶点：让一整圈顶点挤在同一点会产生 `segments` 个零面积三角形
  // （实测：球 32 段 → 64 个，会让 flat 法线出 NaN、导出 glTF 被验证器判为退化图元）
  const auto sphere_point = [&](float phi, float theta) {
    // phi: 0（南极，y=-R）→ π（北极，y=+R）
    const Vec3 normal{std::sin(phi) * std::sin(theta), -std::cos(phi),
                      std::sin(phi) * std::cos(theta)};
    return std::make_pair(normal * p.radius, normal);
  };
  const Vec3 south_normal{0.0f, -1.0f, 0.0f};
  const Vec3 south_position{0.0f, -p.radius, 0.0f};
  const Vec3 north_normal{0.0f, 1.0f, 0.0f};
  const Vec3 north_position{0.0f, p.radius, 0.0f};

  // 中间各环
  std::vector<std::vector<std::pair<Vec3, Vec3>>> rows;   // [ring][segment] = (position, normal)
  std::vector<float> row_v;
  for (int ring = 1; ring < rings; ++ring) {
    const float v = static_cast<float>(ring) / static_cast<float>(rings);
    const float phi = kPi * v;
    std::vector<std::pair<Vec3, Vec3>> row;
    row.reserve(static_cast<std::size_t>(segments));
    for (int i = 0; i < segments; ++i) {
      row.push_back(sphere_point(phi, angle_at(i, segments)));
    }
    rows.push_back(std::move(row));
    row_v.push_back(v);
  }

  // 南极扇
  for (int i = 0; i < segments; ++i) {
    const int j = (i + 1) % segments;
    const float u0 = static_cast<float>(i) / static_cast<float>(segments);
    const float u1 = static_cast<float>(i + 1) / static_cast<float>(segments);
    out.triangle(south_position, rows[0][static_cast<std::size_t>(i)].first,
                 rows[0][static_cast<std::size_t>(j)].first, south_normal,
                 rows[0][static_cast<std::size_t>(i)].second,
                 rows[0][static_cast<std::size_t>(j)].second, {0.5f, 1.0f}, {u0, row_v[0]},
                 {u1, row_v[0]});
  }
  // 中间带
  for (std::size_t ring = 0; ring + 1U < rows.size(); ++ring) {
    for (int i = 0; i < segments; ++i) {
      const int j = (i + 1) % segments;
      const float u0 = static_cast<float>(i) / static_cast<float>(segments);
      const float u1 = static_cast<float>(i + 1) / static_cast<float>(segments);
      out.quad(rows[ring][static_cast<std::size_t>(i)].first,
               rows[ring][static_cast<std::size_t>(j)].first,
               rows[ring + 1U][static_cast<std::size_t>(j)].first,
               rows[ring + 1U][static_cast<std::size_t>(i)].first,
               rows[ring][static_cast<std::size_t>(i)].second,
               rows[ring][static_cast<std::size_t>(j)].second,
               rows[ring + 1U][static_cast<std::size_t>(j)].second,
               rows[ring + 1U][static_cast<std::size_t>(i)].second, {u0, row_v[ring]},
               {u1, row_v[ring]}, {u1, row_v[ring + 1U]}, {u0, row_v[ring + 1U]});
    }
  }
  // 北极扇
  for (int i = 0; i < segments; ++i) {
    const int j = (i + 1) % segments;
    const float u0 = static_cast<float>(i) / static_cast<float>(segments);
    const float u1 = static_cast<float>(i + 1) / static_cast<float>(segments);
    out.triangle(rows.back()[static_cast<std::size_t>(i)].first,
                 rows.back()[static_cast<std::size_t>(j)].first, north_position,
                 rows.back()[static_cast<std::size_t>(i)].second,
                 rows.back()[static_cast<std::size_t>(j)].second, north_normal,
                 {u0, row_v.back()}, {u1, row_v.back()}, {0.5f, 0.0f});
  }
  return out.finish();
}

auto make_ico_sphere(const PrimitiveParams& params) -> Mesh {
  PrimitiveParams p = sanitized(params);
  auto [points, faces] = icosahedron_data();
  // 递归细分：每个三角形一分为四（边中点归一化到球面）
  for (int level = 0; level < p.subdivisions; ++level) {
    std::map<std::pair<int, int>, int> midpoints;
    std::vector<std::array<int, 3>> next;
    next.reserve(faces.size() * 4U);
    const auto midpoint = [&](int i0, int i1) {
      const std::pair<int, int> key{i0 < i1 ? i0 : i1, i0 < i1 ? i1 : i0};
      if (const auto found = midpoints.find(key); found != midpoints.end()) return found->second;
      const Vec3 mid = ((points[static_cast<std::size_t>(i0)] +
                         points[static_cast<std::size_t>(i1)]) * 0.5f)
                           .normalized();
      const int index = static_cast<int>(points.size());
      points.push_back(mid);
      midpoints.emplace(key, index);
      return index;
    };
    for (const std::array<int, 3>& tri : faces) {
      const int a = midpoint(tri[0], tri[1]);
      const int b = midpoint(tri[1], tri[2]);
      const int c = midpoint(tri[2], tri[0]);
      next.push_back({tri[0], a, c});
      next.push_back({tri[1], b, a});
      next.push_back({tri[2], c, b});
      next.push_back({a, b, c});
    }
    faces = std::move(next);
  }
  // 平滑法线（球面要圆；硬边会露出 20 个面的棱）
  return polyhedron(p.radius, points, faces, linear_color(p.color), p.with_uv, false);
}

// ────────────────────────────────────────────────────────────────────────────
// 旋转体：柱 / 锥 / 管 / 胶囊 / 箭头
// ────────────────────────────────────────────────────────────────────────────

namespace {

/// **旋转体通用生成**：`profile` 是一串 `(半径, y)`（自下而上），绕 Y 轴扫掠。
///
/// 这是本模块最重要的一个函数：柱、锥、胶囊、箭头都是"一条折线绕轴转一圈"，
/// 各写一遍必然漂移（法线朝里 / 绕序反 / UV 错位只出现在其中一处）。
///
/// - `cap_bottom` / `cap_top`：端点半径 > 0 时按圆盘封盖；
/// - 半径为零的层被当成**单个极点**（扇形连接），否则会产出 `segments` 个零面积三角形。
auto sweep_profile(const std::vector<std::pair<float, float>>& profile, int segments,
                   bool cap_bottom, bool cap_top, float twist, Vec3 color, bool with_uv,
                   bool smooth) -> Mesh {
  Builder out{};
  out.with_uv = with_uv;
  out.color = color;

  // 相邻重复点先去重：胶囊在 `depth < 2·radius` 时圆柱段长度夹到 0，
  // "下半球末端"与"上半球起点"是同一个环——直接扫掠会多出一层零厚度四边形。
  std::vector<std::pair<float, float>> shape;
  shape.reserve(profile.size());
  for (const auto& point : profile) {
    if (!shape.empty()) {
      const float dr = point.first - shape.back().first;
      const float dy = point.second - shape.back().second;
      if (dr * dr + dy * dy < 1.0e-12f) continue;
    }
    shape.push_back(point);
  }
  if (shape.size() < 2U || segments < 3) return out.finish();

  const float y_min = shape.front().second;
  const float y_max = shape.back().second;
  const float height = std::max(y_max - y_min, 1.0e-6f);
  const float half_height = (y_max - y_min) * 0.5f;
  constexpr float kZeroRadius = 1.0e-5f;

  // ## 每个**段**一个截面法线，而不是每个**层**一个
  //
  // 这是本函数的关键（踩过两次）：
  //
  // 箭头的"箭头底面"是一圈水平环带（同一 y、两个半径），它的正确法线是 ∓Y。
  // 若按"层的相邻点求平均"算，那个角的法线会变成斜的（上半段朝上、下半段朝下被抹平），
  // 于是环带 10 个三角形的几何法线（±Y）与顶点法线（斜）几乎垂直 ——
  // 自动定向判不出来（实测 5 个反向），而且着色也是错的。
  //
  // 按**段**求法线天然处理拐角：水平段得到 ±Y、竖直段得到径向、锥面得到斜向，
  // 每个顶点按"它属于哪一段"取法线。`smooth = true`（胶囊）时再对相邻两段取平均
  // —— 那是唯一需要软边的地方。
  const auto segment_normal = [&shape](std::size_t index) {
    const float dr = shape[index + 1U].first - shape[index].first;
    const float dy = shape[index + 1U].second - shape[index].second;
    // 切线 (dr, dy) 顺时针转 90° 得外法线 (dy, -dr)
    const Vec3 normal = Vec3{dy, -dr, 0.0f}.normalized();
    return normal.length() > 0.5f ? normal : Vec3{1.0f, 0.0f, 0.0f};
  };
  std::vector<Vec3> segment_normals;
  segment_normals.reserve(shape.size() - 1U);
  for (std::size_t i = 0; i + 1U < shape.size(); ++i) segment_normals.push_back(segment_normal(i));

  struct Ring {
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    float v{0.0f};
  };
  // `smooth = true`（胶囊）：环上法线取"上下两段平均值"，避免柱面与半球之间出现硬棱。
  const auto smoothed_section = [&](std::size_t level, const Vec3& fallback) {
    if (!smooth) return fallback;
    Vec3 sum{};
    std::size_t count = 0;
    if (level > 0U) {
      sum = sum + segment_normals[level - 1U];
      ++count;
    }
    if (level < segment_normals.size()) {
      sum = sum + segment_normals[level];
      ++count;
    }
    if (count == 0U) return fallback;
    const Vec3 average = sum * (1.0f / static_cast<float>(count));
    return average.length() > 1.0e-5f ? average.normalized() : fallback;
  };
  const auto build_ring = [&](std::size_t level, const Vec3& section) {
    Ring ring{};
    ring.v = (shape[level].second - y_min) / height;
    const float radius = shape[level].first;
    if (radius <= kZeroRadius) return ring;
    ring.positions.reserve(static_cast<std::size_t>(segments));
    ring.normals.reserve(static_cast<std::size_t>(segments));
    for (int i = 0; i < segments; ++i) {
      const float theta = angle_at(i, segments);
      const Vec3 base = ring_point(radius, theta, shape[level].second);
      ring.positions.push_back(twisted(base, shape[level].second, half_height, twist));
      ring.normals.push_back(
          Vec3{section.x * std::sin(theta), section.y, section.x * std::cos(theta)}.normalized());
    }
    return ring;
  };

  for (std::size_t level = 0; level + 1U < shape.size(); ++level) {
    const Vec3 section = segment_normals[level];
    const Ring lower = build_ring(level, smoothed_section(level, section));
    const Ring upper = build_ring(level + 1U, smoothed_section(level + 1U, section));
    const bool lower_apex = lower.positions.empty();
    const bool upper_apex = upper.positions.empty();

    if (lower_apex && upper_apex) continue;
    if (lower_apex || upper_apex) {
      const Ring& ring = lower_apex ? upper : lower;
      const Vec3 apex_position{0.0f,
                               shape[lower_apex ? level : level + 1U].second, 0.0f};
      const float apex_v = lower_apex ? lower.v : upper.v;
      // 极点法线 = 该段环上的法线平均（与这一面的几何法线同向，自动定向判得出）
      Vec3 average{};
      for (const Vec3& n : ring.normals) average = average + n;
      const Vec3 apex_normal = average.normalized();
      for (int i = 0; i < segments; ++i) {
        const int j = (i + 1) % segments;
        const float u0 = static_cast<float>(i) / static_cast<float>(segments);
        const float u1 = static_cast<float>(i + 1) / static_cast<float>(segments);
        const Vec3& n0 = ring.normals[static_cast<std::size_t>(i)];
        const Vec3& n1 = ring.normals[static_cast<std::size_t>(j)];
        if (upper_apex) {
          out.triangle(ring.positions[static_cast<std::size_t>(i)],
                       ring.positions[static_cast<std::size_t>(j)], apex_position, n0, n1,
                       apex_normal, {u0, ring.v}, {u1, ring.v}, {0.5f, apex_v});
        } else {
          out.triangle(apex_position, ring.positions[static_cast<std::size_t>(i)],
                       ring.positions[static_cast<std::size_t>(j)], apex_normal, n0, n1,
                       {0.5f, apex_v}, {u0, ring.v}, {u1, ring.v});
        }
      }
      continue;
    }
    for (int i = 0; i < segments; ++i) {
      const int j = (i + 1) % segments;
      const float u0 = static_cast<float>(i) / static_cast<float>(segments);
      const float u1 = static_cast<float>(i + 1) / static_cast<float>(segments);
      out.quad(lower.positions[static_cast<std::size_t>(i)],
               lower.positions[static_cast<std::size_t>(j)],
               upper.positions[static_cast<std::size_t>(j)],
               upper.positions[static_cast<std::size_t>(i)],
               lower.normals[static_cast<std::size_t>(i)],
               lower.normals[static_cast<std::size_t>(j)],
               upper.normals[static_cast<std::size_t>(j)],
               upper.normals[static_cast<std::size_t>(i)], {u0, lower.v}, {u1, lower.v},
               {u1, upper.v}, {u0, upper.v});
    }
  }

  // 端盖：法线 ±Y（竖直面的法线），与端盖所在层的环**同一半径、独立顶点**。
  const auto cap = [&](std::size_t level, bool bottom) {
    if (shape[level].first <= kZeroRadius) return;
    const Vec3 normal{0.0f, bottom ? -1.0f : 1.0f, 0.0f};
    const float y = shape[level].second;
    for (int i = 0; i < segments; ++i) {
      const int j = (i + 1) % segments;
      const float a0 = angle_at(i, segments);
      const float a1 = angle_at(j, segments);
      const float u0 = static_cast<float>(i) / static_cast<float>(segments);
      const float u1 = static_cast<float>(j) / static_cast<float>(segments);
      out.triangle(Vec3{0.0f, y, 0.0f}, twisted(ring_point(shape[level].first, a0, y), y, half_height, twist),
                   twisted(ring_point(shape[level].first, a1, y), y, half_height, twist), normal,
                   normal, normal, {0.5f, 0.5f}, {0.5f + 0.5f * u0, 0.5f + 0.5f * u0},
                   {0.5f + 0.5f * u1, 0.5f + 0.5f * u1});
    }
  };
  if (cap_bottom) cap(0U, true);
  if (cap_top) cap(shape.size() - 1U, false);
  return out.finish();
}

}  // namespace

auto make_cylinder(const PrimitiveParams& params) -> Mesh {
  const PrimitiveParams p = sanitized(params);
  const float hy = p.depth * 0.5f;
  const std::vector<std::pair<float, float>> profile = {{p.radius, -hy}, {p.radius, hy}};
  return sweep_profile(profile, p.segments, p.cap_bottom, p.cap_top, p.twist,
                       linear_color(p.color), p.with_uv, false);
}

auto make_cone(const PrimitiveParams& params) -> Mesh {
  const PrimitiveParams p = sanitized(params);
  const float hy = p.depth * 0.5f;
  // 顶面半径为 0 → 尖锥（sweep_profile 会把它当成单个极点，扇形收口）
  const std::vector<std::pair<float, float>> profile = {{p.radius, -hy}, {p.top_radius, hy}};
  return sweep_profile(profile, p.segments, true, p.top_radius > 0.0f, p.twist,
                       linear_color(p.color), p.with_uv, false);
}

auto make_capsule(const PrimitiveParams& params) -> Mesh {
  const PrimitiveParams p = sanitized(params);
  const float radius = p.radius;
  const float half = std::max(p.depth * 0.5f - radius, 0.0f);   // 圆柱段半长（`depth` 是总高）
  const int arc = std::max(p.rings, 4);
  std::vector<std::pair<float, float>> profile;
  profile.reserve(static_cast<std::size_t>(arc * 2 + 2));
  for (int i = 0; i <= arc; ++i) {   // 下半球：(0,-half-R) → (R,-half)
    const float phi = (kPi * 0.5f) * static_cast<float>(i) / static_cast<float>(arc);
    profile.emplace_back(radius * std::sin(phi), -half - radius * std::cos(phi));
  }
  for (int i = 0; i <= arc; ++i) {   // 上半球：(R,half) → (0,half+R)
    const float phi = (kPi * 0.5f) * static_cast<float>(i) / static_cast<float>(arc);
    profile.emplace_back(radius * std::cos(phi), half + radius * std::sin(phi));
  }
  return sweep_profile(profile, p.segments, false, false, 0.0f, linear_color(p.color), p.with_uv,
                       true);
}

auto make_arrow(const PrimitiveParams& params) -> Mesh {
  const PrimitiveParams p = sanitized(params);
  const float total = p.depth;
  const float head = total * 0.3f;            // 箭头头占 30%
  const float shaft = total - head;
  const float head_radius = p.radius;
  const float shaft_radius = p.radius * 0.35f;
  const float y0 = -total * 0.5f;
  const std::vector<std::pair<float, float>> profile = {
      {shaft_radius, y0}, {shaft_radius, y0 + shaft}, {head_radius, y0 + shaft}, {0.0f, y0 + total}};
  // 底面封盖（法线 -Y）；尖端是极点，不需要盖
  return sweep_profile(profile, p.segments, true, false, 0.0f, linear_color(p.color), p.with_uv,
                       false);
}

auto make_tube(const PrimitiveParams& params) -> Mesh {
  const PrimitiveParams p = sanitized(params);
  const float hy = p.depth * 0.5f;
  const float inner = p.radius * (1.0f - std::clamp(p.tooth_width, 0.05f, 0.95f));
  Builder out{};
  out.with_uv = p.with_uv;
  out.color = linear_color(p.color);

  // 四种环：外下 / 外上 / 内下 / 内上。
  //
  // ## 端面为什么必须用**独立的顶点组**
  //
  // 第一版让端面复用壁面顶点（法线是径向的），于是端面三角形的几何法线（±Y）
  // 与顶点法线（径向）**垂直**——点积恰为 0，既不算同向也不算反向，而实际效果是
  // 端面着色全错（它按径向法线取光，等于"没有朝上的面"）。
  // 顶点只能有一个法线，所以端面要有自己的一份。
  struct Ring {
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<UV> uvs;
  };
  const auto make_ring = [&](float radius, float y, bool outward, bool cap_normal) -> Ring {
    Ring ring{};
    const auto reserve = static_cast<std::size_t>(p.segments);
    ring.positions.reserve(reserve);
    ring.normals.reserve(reserve);
    ring.uvs.reserve(reserve);
    for (int i = 0; i < p.segments; ++i) {
      const float theta = angle_at(i, p.segments);
      ring.positions.push_back(twisted(ring_point(radius, theta, y), y, hy, p.twist));
      if (cap_normal) {
        ring.normals.emplace_back(0.0f, y >= 0.0f ? 1.0f : -1.0f, 0.0f);
      } else {
        const float sign = outward ? 1.0f : -1.0f;
        ring.normals.emplace_back(sign * std::sin(theta), 0.0f, sign * std::cos(theta));
      }
      ring.uvs.emplace_back(static_cast<float>(i) / static_cast<float>(p.segments),
                            radius > (p.radius + inner) * 0.5f ? 1.0f : 0.0f);
    }
    return ring;
  };
  const Ring wall_out_bottom = make_ring(p.radius, -hy, true, false);
  const Ring wall_out_top = make_ring(p.radius, hy, true, false);
  const Ring wall_in_bottom = make_ring(inner, -hy, false, false);
  const Ring wall_in_top = make_ring(inner, hy, false, false);
  const Ring rim_top_outer = make_ring(p.radius, hy, true, true);
  const Ring rim_top_inner = make_ring(inner, hy, false, true);
  const Ring rim_bottom_outer = make_ring(p.radius, -hy, true, true);
  const Ring rim_bottom_inner = make_ring(inner, -hy, false, true);

  // 取环上的点（自动回绕）。**顺序不用操心**：`Builder::face()` 按几何法线自动定向，
  // 所以这里只需要保证"取对了四个点、给了对应法线"。
  const auto at = [&](const Ring& ring, int index) {
    const auto i = static_cast<std::size_t>(index) % ring.positions.size();
    return std::make_tuple(ring.positions[i], ring.normals[i], ring.uvs[i]);
  };
  for (int i = 0; i < p.segments; ++i) {
    const auto [ob0, obn0, obu0] = at(wall_out_bottom, i);
    const auto [ob1, obn1, obu1] = at(wall_out_bottom, i + 1);
    const auto [ot0, otn0, otu0] = at(wall_out_top, i);
    const auto [ot1, otn1, otu1] = at(wall_out_top, i + 1);
    out.quad(ob0, ob1, ot1, ot0, obn0, obn1, otn1, otn0, obu0, obu1, otu1, otu0);

    const auto [ib0, ibn0, ibu0] = at(wall_in_bottom, i);
    const auto [ib1, ibn1, ibu1] = at(wall_in_bottom, i + 1);
    const auto [it0, itn0, itu0] = at(wall_in_top, i);
    const auto [it1, itn1, itu1] = at(wall_in_top, i + 1);
    out.quad(ib0, ib1, it1, it0, ibn0, ibn1, itn1, itn0, ibu0, ibu1, itu1, itu0);

    // 上端环带（法线 +Y）
    const auto [tco0, tcon0, _u0] = at(rim_top_outer, i);
    const auto [tco1, tcon1, _u1] = at(rim_top_outer, i + 1);
    const auto [tci0, tcin0, _u2] = at(rim_top_inner, i);
    const auto [tci1, tcin1, _u3] = at(rim_top_inner, i + 1);
    out.quad(tco0, tco1, tci1, tci0, tcon0, tcon1, tcin1, tcin0, {0.0f, 1.0f}, {1.0f, 1.0f},
             {1.0f, 0.0f}, {0.0f, 0.0f});

    // 下端环带（法线 -Y）
    const auto [bco0, bcon0, _v0] = at(rim_bottom_outer, i);
    const auto [bco1, bcon1, _v1] = at(rim_bottom_outer, i + 1);
    const auto [bci0, bcin0, _v2] = at(rim_bottom_inner, i);
    const auto [bci1, bcin1, _v3] = at(rim_bottom_inner, i + 1);
    out.quad(bco0, bco1, bci1, bci0, bcon0, bcon1, bcin1, bcin0, {0.0f, 0.0f}, {1.0f, 0.0f},
             {1.0f, 1.0f}, {0.0f, 1.0f});
  }
  return out.finish();
}

// ────────────────────────────────────────────────────────────────────────────
// 环 / 纽结 / 螺旋
// ────────────────────────────────────────────────────────────────────────────

auto make_torus(const PrimitiveParams& params) -> Mesh {
  const PrimitiveParams p = sanitized(params);
  const float R = p.major_radius;
  const float r = p.minor_radius;
  Builder out{};
  out.with_uv = p.with_uv;
  out.color = linear_color(p.color);
  for (int i = 0; i < p.major_segments; ++i) {
    const float theta0 = angle_at(i, p.major_segments);
    const float theta1 = angle_at(i + 1, p.major_segments);
    const float u0 = static_cast<float>(i) / static_cast<float>(p.major_segments);
    const float u1 = static_cast<float>(i + 1) / static_cast<float>(p.major_segments);
    for (int j = 0; j < p.minor_segments; ++j) {
      const float phi0 = angle_at(j, p.minor_segments);
      const float phi1 = angle_at(j + 1, p.minor_segments);
      const float v0 = static_cast<float>(j) / static_cast<float>(p.minor_segments);
      const float v1 = static_cast<float>(j + 1) / static_cast<float>(p.minor_segments);
      const auto point = [&](float theta, float phi) {
        const Vec3 normal{std::cos(theta) * std::cos(phi), std::sin(phi),
                          std::sin(theta) * std::cos(phi)};
        return std::make_pair(Vec3{(R + r * std::cos(phi)) * std::cos(theta), r * std::sin(phi),
                                   (R + r * std::cos(phi)) * std::sin(theta)},
                              normal);
      };
      const auto [p00, n00] = point(theta0, phi0);
      const auto [p10, n10] = point(theta1, phi0);
      const auto [p11, n11] = point(theta1, phi1);
      const auto [p01, n01] = point(theta0, phi1);
      out.quad(p00, p10, p11, p01, n00, n10, n11, n01, {u0, v0}, {u1, v0}, {u1, v1}, {u0, v1});
    }
  }
  return out.finish();
}

namespace {

/// 沿一条参数曲线扫掠圆管（纽结与螺旋共用）。
///
/// ## 标架手性（这一条踩过，必读）
///
/// 管面三角形的几何法线来自"环方向 × 路径方向"。要让它与我们写的径向法线
/// `right·cos + up·sin` 同向，标架必须满足 `right × up = tangent`，即
/// `up = tangent × right`。
///
/// 反过来写（`up = right × tangent`）会让**每个**三角形内外翻转：
/// 顶点法线是对的 → 光照看着正常，但背面剔除把整个物体剔成内壁。
/// 实测代价：`torus_knot` 与 `helix` 各有 768 个三角形反向，而"只是看着有点怪"。
auto sweep_curve(const std::vector<Vec3>& centers, float tube_radius, int around, Vec3 color,
                 bool with_uv, bool close_loop, bool cap_ends) -> Mesh {
  Builder out{};
  out.with_uv = with_uv;
  out.color = color;
  const std::size_t count = centers.size();
  if (count < 3U || around < 3) return out.finish();

  const auto tangent_at = [&](std::size_t i) {
    if (close_loop) {
      const Vec3 prev = centers[(i + count - 1U) % count];
      const Vec3 next = centers[(i + 1U) % count];
      return (next - prev).normalized();
    }
    const Vec3 prev = centers[i == 0U ? 0U : i - 1U];
    const Vec3 next = centers[i + 1U < count ? i + 1U : i];
    return (next - prev).normalized();
  };

  std::vector<std::vector<Vec3>> rings;
  std::vector<std::vector<Vec3>> normals;
  std::vector<Vec3> tangents;
  rings.reserve(count);
  normals.reserve(count);
  tangents.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const Vec3 tangent = tangent_at(i);
    Vec3 helper = std::abs(tangent.y) < 0.9f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
    Vec3 right = tangent.cross(helper);
    if (right.length() < 1.0e-5f) right = tangent.cross(Vec3{1.0f, 0.0f, 0.0f});
    right = right.normalized();
    const Vec3 up = tangent.cross(right).normalized();   // right × up = tangent
    std::vector<Vec3> ring_positions;
    std::vector<Vec3> ring_normals;
    ring_positions.reserve(static_cast<std::size_t>(around));
    ring_normals.reserve(static_cast<std::size_t>(around));
    for (int j = 0; j < around; ++j) {
      const float angle = angle_at(j, around);
      const Vec3 radial = right * std::cos(angle) + up * std::sin(angle);
      ring_positions.push_back(centers[i] + radial * tube_radius);
      ring_normals.push_back(radial);
    }
    rings.push_back(std::move(ring_positions));
    normals.push_back(std::move(ring_normals));
    tangents.push_back(tangent);
  }

  const std::size_t levels = close_loop ? count : count - 1U;
  for (std::size_t i = 0; i < levels; ++i) {
    const std::size_t i_next = (i + 1U) % count;
    const float v0 = static_cast<float>(i) / static_cast<float>(count);
    const float v1 = static_cast<float>(i + 1U) / static_cast<float>(count);
    for (int j = 0; j < around; ++j) {
      const std::size_t j0 = static_cast<std::size_t>(j);
      const std::size_t j1 = static_cast<std::size_t>((j + 1) % around);
      const float u0 = static_cast<float>(j) / static_cast<float>(around);
      const float u1 = static_cast<float>(j + 1) / static_cast<float>(around);
      out.quad(rings[i][j0], rings[i][j1], rings[i_next][j1], rings[i_next][j0], normals[i][j0],
               normals[i][j1], normals[i_next][j1], normals[i_next][j0], {u0, v0}, {u1, v0},
               {u1, v1}, {u0, v1});
    }
  }

  if (!close_loop && cap_ends) {
    // 两端封盖（法线 = ∓切线）
    for (const std::size_t end : {std::size_t{0}, count - 1U}) {
      const bool is_start = end == 0U;
      const Vec3 normal = is_start ? tangents[end] * -1.0f : tangents[end];
      const Vec3 center =
          [&] {
            Vec3 sum{};
            for (const Vec3& point : rings[end]) sum = sum + point;
            return sum * (1.0f / static_cast<float>(rings[end].size()));
          }();
      for (int j = 0; j < around; ++j) {
        const std::size_t j0 = static_cast<std::size_t>(j);
        const std::size_t j1 = static_cast<std::size_t>((j + 1) % around);
        out.triangle(center, rings[end][j0], rings[end][j1], normal, normal, normal,
                     {0.5f, 0.5f},
                     {static_cast<float>(j) / static_cast<float>(around), 0.0f},
                     {static_cast<float>(j + 1) / static_cast<float>(around), 1.0f});
      }
    }
  }
  return out.finish();
}

}  // namespace

auto make_torus_knot(const PrimitiveParams& params) -> Mesh {
  const PrimitiveParams p = sanitized(params);
  const int along = std::max(p.major_segments, 24) * std::max(p.knot_p, 1);
  const float scale = p.major_radius / 2.0f;
  // 中心曲线：(p, q) 纽结。Trefoil = (2, 3)。
  std::vector<Vec3> centers;
  centers.reserve(static_cast<std::size_t>(along));
  for (int i = 0; i < along; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(along);
    const float phi = t * 2.0f * kPi * static_cast<float>(p.knot_p);
    const float theta = t * 2.0f * kPi * static_cast<float>(p.knot_q);
    const float radius = 2.0f + std::cos(theta);
    centers.push_back(Vec3{radius * std::cos(phi), std::sin(theta), radius * std::sin(phi)} *
                      scale);
  }
  return sweep_curve(centers, p.minor_radius, p.minor_segments, linear_color(p.color), p.with_uv,
                     true, false);
}

auto make_helix(const PrimitiveParams& params) -> Mesh {
  const PrimitiveParams p = sanitized(params);
  const int along = std::max(static_cast<int>(p.turns * 24.0f), 8);
  std::vector<Vec3> centers;
  centers.reserve(static_cast<std::size_t>(along + 1));
  for (int i = 0; i <= along; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(along);
    const float angle = t * p.turns * 2.0f * kPi;
    const float radius = p.radius * (1.0f - p.taper * t);
    centers.push_back(
        Vec3{radius * std::cos(angle), -p.depth * 0.5f + p.depth * t, radius * std::sin(angle)});
  }
  return sweep_curve(centers, p.minor_radius, p.minor_segments, linear_color(p.color), p.with_uv,
                     false, true);
}

// ────────────────────────────────────────────────────────────────────────────
// 棱柱 / 齿轮
// ────────────────────────────────────────────────────────────────────────────

namespace {

/// 多边形（(x, z) 序列）沿 Y 挤出成闭合体。
///
/// ## 两件事必须做对，否则测试一定红
///
/// 1. **多边形绕向先归一化**：后续的耳切法要求"从 +Y 俯视逆时针"。
///    齿轮按角度生成时绕向取决于 x=sin/z=cos 的取法，不归一化就会得到"看起来有面、
///    其实封盖一个三角形都没生成"（实测齿轮 96 个三角形 = 只有侧壁，边界边 32 条）。
/// 2. **封盖用耳切法而不是扇形**：扇形只对凸多边形成立；齿轮的齿谷是凹角，
///    扇形会让三角形跑到轮廓外（观感像"齿被糊住"）。
auto extrude_polygon(std::vector<std::pair<float, float>> polygon, float half_height, float twist,
                     Vec3 color, bool with_uv) -> Mesh {
  Builder out{};
  out.with_uv = with_uv;
  out.color = color;
  const std::size_t count = polygon.size();
  if (count < 3U) return out.finish();

  // 有向面积（(x,z) 平面，正 = 从 +Y 俯视逆时针）
  const auto signed_area = [&polygon] {
    float area = 0.0f;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
      const std::size_t j = (i + 1U) % polygon.size();
      area += polygon[i].first * polygon[j].second - polygon[j].first * polygon[i].second;
    }
    return area * 0.5f;
  };
  if (signed_area() < 0.0f) std::reverse(polygon.begin(), polygon.end());

  std::vector<Vec3> bottom;
  std::vector<Vec3> top;
  bottom.reserve(count);
  top.reserve(count);
  for (const auto& [x, z] : polygon) {
    bottom.push_back(Vec3{x, -half_height, z});
    top.push_back(twisted(Vec3{x, half_height, z}, half_height, half_height, twist));
  }

  // 侧壁：法线取该边的外法线（u × e 对逆时针多边形指向外侧）
  for (std::size_t i = 0; i < count; ++i) {
    const std::size_t j = (i + 1U) % count;
    const float dx = polygon[j].first - polygon[i].first;
    const float dz = polygon[j].second - polygon[i].second;
    Vec3 normal = Vec3{dz, 0.0f, -dx}.normalized();
    if (normal.length() < 0.5f) normal = Vec3{1.0f, 0.0f, 0.0f};
    const float u0 = static_cast<float>(i) / static_cast<float>(count);
    const float u1 = static_cast<float>(i + 1U) / static_cast<float>(count);
    out.quad(bottom[i], bottom[j], top[j], top[i], normal, normal, normal, normal, {u0, 1.0f},
             {u1, 1.0f}, {u1, 0.0f}, {u0, 0.0f});
  }

  // 封盖：耳切法（对凹多边形正确）
  const auto cross2 = [&polygon](std::size_t a, std::size_t b, std::size_t c) {
    return (polygon[b].first - polygon[a].first) * (polygon[c].second - polygon[a].second) -
           (polygon[b].second - polygon[a].second) * (polygon[c].first - polygon[a].first);
  };
  const auto triangulate = [&](bool top_side) {
    const Vec3 normal{0.0f, top_side ? 1.0f : -1.0f, 0.0f};
    const std::vector<Vec3>& level = top_side ? top : bottom;
    std::vector<std::size_t> remaining(count);
    for (std::size_t i = 0; i < count; ++i) remaining[i] = i;
    const auto emit = [&](std::size_t a, std::size_t b, std::size_t c) {
      out.triangle(level[a], level[b], level[c], normal, normal, normal,
                   {0.5f + polygon[a].first * 0.5f, 0.5f + polygon[a].second * 0.5f},
                   {0.5f + polygon[b].first * 0.5f, 0.5f + polygon[b].second * 0.5f},
                   {0.5f + polygon[c].first * 0.5f, 0.5f + polygon[c].second * 0.5f});
    };
    std::size_t guard = 0;
    const std::size_t guard_limit = count * count + 32U;
    while (remaining.size() > 3U && guard < guard_limit) {
      ++guard;
      bool clipped = false;
      for (std::size_t k = 0; k < remaining.size(); ++k) {
        const std::size_t ia = remaining[(k + remaining.size() - 1U) % remaining.size()];
        const std::size_t ib = remaining[k];
        const std::size_t ic = remaining[(k + 1U) % remaining.size()];
        if (cross2(ia, ib, ic) <= 1.0e-12f) continue;   // 凹角，不是耳
        bool blocked = false;
        for (const std::size_t other : remaining) {
          if (other == ia || other == ib || other == ic) continue;
          if (cross2(ia, ib, other) >= 0.0f && cross2(ib, ic, other) >= 0.0f &&
              cross2(ic, ia, other) >= 0.0f) {
            blocked = true;
            break;
          }
        }
        if (blocked) continue;
        emit(ia, ib, ic);
        remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(k));
        clipped = true;
        break;
      }
      if (!clipped) break;
    }
    if (remaining.size() == 3U) emit(remaining[0], remaining[1], remaining[2]);
  };
  triangulate(true);
  triangulate(false);
  return out.finish();
}

}  // namespace

auto make_prism(const PrimitiveParams& params) -> Mesh {
  const PrimitiveParams p = sanitized(params);
  std::vector<std::pair<float, float>> polygon;
  polygon.reserve(static_cast<std::size_t>(p.segments));
  for (int i = 0; i < p.segments; ++i) {
    const float theta = angle_at(i, p.segments);
    polygon.emplace_back(p.radius * std::sin(theta), p.radius * std::cos(theta));
  }
  return extrude_polygon(std::move(polygon), p.depth * 0.5f, p.twist, linear_color(p.color),
                         p.with_uv);
}

auto make_gear(const PrimitiveParams& params) -> Mesh {
  const PrimitiveParams p = sanitized(params);
  const float outer = p.radius;
  const float root = std::max(p.radius - p.tooth_height, p.radius * 0.25f);
  const float pitch = 2.0f * kPi / static_cast<float>(p.teeth);
  const float half_tooth = pitch * p.tooth_width * 0.5f;
  std::vector<std::pair<float, float>> polygon;
  polygon.reserve(static_cast<std::size_t>(p.teeth) * 4U);
  const auto push = [&polygon](float radius, float angle) {
    polygon.emplace_back(radius * std::sin(angle), radius * std::cos(angle));
  };
  for (int i = 0; i < p.teeth; ++i) {
    const float center = pitch * static_cast<float>(i);
    // 一个齿 = **齿谷 → 齿顶 → 齿顶**（三点）。
    //
    // ## 为什么不是四点（第一版缺陷）
    //
    // 第一版写成"齿谷 → 齿顶 → 齿顶 → 齿谷"，其中末点落在 `center + pitch/2`、
    // 下一齿的首点落在 `center + pitch/2`——**同一个点**。两点重合产生零长边，
    // 于是耳切法在那里判退化、侧壁产出零面积三角形（实测 22 个退化 + 19 条边界边）。
    // 齿谷只写一次（它就是两齿之间的那个点）。
    push(root, center - pitch * 0.5f);
    push(outer, center - half_tooth);
    push(outer, center + half_tooth);
  }
  return extrude_polygon(std::move(polygon), p.depth * 0.5f, p.twist, linear_color(p.color),
                         p.with_uv);
}

// ────────────────────────────────────────────────────────────────────────────
// 分派
// ────────────────────────────────────────────────────────────────────────────

auto make(PrimitiveKind kind, const PrimitiveParams& params) -> Mesh {
  switch (kind) {
    case PrimitiveKind::Plane: return make_plane(params);
    case PrimitiveKind::Cube: return make_cube(params);
    case PrimitiveKind::UvSphere: return make_uv_sphere(params);
    case PrimitiveKind::IcoSphere: return make_ico_sphere(params);
    case PrimitiveKind::Cylinder: return make_cylinder(params);
    case PrimitiveKind::Cone: return make_cone(params);
    case PrimitiveKind::Capsule: return make_capsule(params);
    case PrimitiveKind::Torus: return make_torus(params);
    case PrimitiveKind::Disc: return make_disc(params);
    case PrimitiveKind::Circle: return make_circle(params);
    case PrimitiveKind::Grid: return make_grid(params);
    case PrimitiveKind::Tube: return make_tube(params);
    case PrimitiveKind::Prism: return make_prism(params);
    case PrimitiveKind::Gear: return make_gear(params);
    case PrimitiveKind::TorusKnot: return make_torus_knot(params);
    case PrimitiveKind::Arrow: return make_arrow(params);
    case PrimitiveKind::Wedge: return make_wedge(params);
    case PrimitiveKind::Tetrahedron: return make_tetrahedron(params);
    case PrimitiveKind::Octahedron: return make_octahedron(params);
    case PrimitiveKind::Dodecahedron: return make_dodecahedron(params);
    case PrimitiveKind::Icosahedron: return make_icosahedron(params);
    case PrimitiveKind::Helix: return make_helix(params);
  }
  return Mesh{};
}

auto make_from_name(std::string_view name, const PrimitiveParams& params) -> Result<Mesh> {
  const auto kind = parse_kind(name);
  if (!kind.has_value()) {
    return st::unexpected(ErrorCode::Parse,
                          std::format("不认识的图元名 \"{}\"；可用：{}", name, all_kind_names()));
  }
  return make(*kind, params);
}

}  // namespace st::raster::primitives

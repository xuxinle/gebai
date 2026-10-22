#include "st/raster/mesh.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "st/core/fs.hpp"
#include "st/core/string.hpp"

namespace st::raster {

namespace {

/// 解析一个 `f` 顶点引用：返回 (位置索引, 法线索引)，均为 **0 基**；法线缺失时 -1。
/// 支持 `v` / `v/vt` / `v//vn` / `v/vt/vn` 四种写法与负索引（相对当前顶点数）。
[[nodiscard]] auto parse_face_ref(std::string_view token, int position_count, int normal_count)
    -> std::optional<std::pair<int, int>> {
  const std::size_t slash = token.find('/');
  const std::string_view position_text = slash == std::string_view::npos
                                             ? token
                                             : token.substr(0, slash);
  if (position_text.empty()) return std::nullopt;
  const auto position = st::parse_i64(position_text);
  if (!position.has_value()) return std::nullopt;
  // OBJ 是 1 基；负数表示"从末尾往前数"（-1 = 最后一个）
  const int position_index = *position > 0
                                 ? static_cast<int>(*position) - 1
                                 : position_count + static_cast<int>(*position);
  if (position_index < 0 || position_index >= position_count) return std::nullopt;

  int normal_index = -1;
  if (slash != std::string_view::npos) {
    const std::string_view rest = token.substr(slash + 1);
    const std::size_t second_slash = rest.find('/');
    if (second_slash != std::string_view::npos) {
      const std::string_view normal_text = rest.substr(second_slash + 1);
      if (!normal_text.empty()) {
        const auto normal = st::parse_i64(normal_text);
        if (normal.has_value()) {
          normal_index = *normal > 0 ? static_cast<int>(*normal) - 1
                                     : normal_count + static_cast<int>(*normal);
          if (normal_index < 0 || normal_index >= normal_count) normal_index = -1;  // 越界当缺失
        }
      }
    }
  }
  return std::make_pair(position_index, normal_index);
}

}  // namespace

auto Mesh::load_obj(std::string_view text, math::Color base_color) -> Result<Mesh> {
  std::vector<math::Vec3> positions;
  std::vector<math::Vec3> normals;
  // 先攒面（每面记录引用），因为 OBJ 允许 `f` 出现在 `v`/`vn` 之前
  std::vector<std::pair<int, int>> face_corners;
  std::vector<std::size_t> face_sizes;

  for (const std::string_view raw_line : st::split(text, '\n')) {
    const std::string_view line = st::trim(raw_line);
    if (line.empty() || line[0] == '#') continue;
    const std::vector<std::string_view> parts = st::split_whitespace(line);
    if (parts.empty()) continue;
    const std::string_view kind = parts[0];

    if (kind == "v" && parts.size() >= 4) {
      const auto x = st::parse_f64(parts[1]);
      const auto y = st::parse_f64(parts[2]);
      const auto z = st::parse_f64(parts[3]);
      if (x.has_value() && y.has_value() && z.has_value()) {
        positions.push_back(math::Vec3{static_cast<float>(*x), static_cast<float>(*y),
                                       static_cast<float>(*z)});
      }
    } else if (kind == "vn" && parts.size() >= 4) {
      const auto x = st::parse_f64(parts[1]);
      const auto y = st::parse_f64(parts[2]);
      const auto z = st::parse_f64(parts[3]);
      if (x.has_value() && y.has_value() && z.has_value()) {
        normals.push_back(math::Vec3{static_cast<float>(*x), static_cast<float>(*y),
                                     static_cast<float>(*z)});
      }
    } else if (kind == "f" && parts.size() >= 4) {
      // 面引用是相对**当时已读到的**顶点数（负索引语义如此）
      const int position_count = static_cast<int>(positions.size());
      const int normal_count = static_cast<int>(normals.size());
      std::vector<std::pair<int, int>> corners;
      for (std::size_t index = 1; index < parts.size(); ++index) {
        if (auto ref = parse_face_ref(parts[index], position_count, normal_count);
            ref.has_value()) {
          corners.push_back(*ref);
        }
      }
      // 坏面只跳过（不整文件失败）：真实导出文件里常有一两个退化面
      if (corners.size() >= 3) {
        face_corners.insert(face_corners.end(), corners.begin(), corners.end());
        face_sizes.push_back(corners.size());
      }
    }
  }

  if (positions.empty() || face_corners.empty()) {
    return unexpected(ErrorCode::Invalid,
                      std::format("OBJ 里没有可用几何（顶点 {} 个，面 {} 组）", positions.size(),
                                  face_sizes.size()));
  }

  Mesh mesh;
  mesh.vertices.reserve(face_corners.size() * 9U);
  const math::Vec3 color{static_cast<float>(base_color.r) / 255.0f,
                         static_cast<float>(base_color.g) / 255.0f,
                         static_cast<float>(base_color.b) / 255.0f};

  std::size_t corner = 0;
  for (const std::size_t size : face_sizes) {
    // 扇形三角化：(0, i, i+1)——对凸/近似凸面正确；凹面会略有偏差，
    // 这是"够画出模型"的取舍，已在头文件写明不支持自由曲面。
    for (std::size_t index = 1; index + 1 < size; ++index) {
      const std::pair<int, int> a = face_corners[corner];
      const std::pair<int, int> b = face_corners[corner + index];
      const std::pair<int, int> c = face_corners[corner + index + 1];
      // 缺法线：按面算（几何法线），保证光照不会因为法线为 0 而全黑
      math::Vec3 face_normal = (positions[static_cast<std::size_t>(b.first)] -
                                positions[static_cast<std::size_t>(a.first)])
                                   .cross(positions[static_cast<std::size_t>(c.first)] -
                                          positions[static_cast<std::size_t>(a.first)])
                                   .normalized();
      if (face_normal.length() < 0.5f) face_normal = math::Vec3{0.0f, 1.0f, 0.0f};  // 退化面兜底
      const auto emit = [&](const std::pair<int, int>& ref) {
        const math::Vec3 position = positions[static_cast<std::size_t>(ref.first)];
        const math::Vec3 normal = ref.second >= 0
                                      ? normals[static_cast<std::size_t>(ref.second)].normalized()
                                      : face_normal;
        push_vertex(mesh.vertices, position,
                    normal.length() < 0.5f ? face_normal : normal, color);
      };
      const auto base = static_cast<std::uint32_t>(mesh.vertex_count());
      emit(a);
      emit(b);
      emit(c);
      mesh.indices.insert(mesh.indices.end(), {base, base + 1U, base + 2U});
    }
    corner += size;
  }
  if (mesh.is_empty()) {
    return unexpected(ErrorCode::Invalid, "OBJ 的面全部退化（没有可用三角形）");
  }
  return mesh;
}

auto Mesh::load_obj_file(std::string_view path, math::Color base_color) -> Result<Mesh> {
  auto text = st::fs::read_text(path);
  if (!text.has_value()) return forward_error(text.error());
  return load_obj(*text, base_color);
}



void push_vertex(std::vector<float>& out, math::Vec3 position, math::Vec3 normal,
                 math::Vec3 color) {
  out.insert(out.end(), {position.x, position.y, position.z, normal.x, normal.y, normal.z,
                         color.x, color.y, color.z});
}

auto Mesh::box(float x, float y, float z) -> Mesh {
  Mesh mesh;
  const float hx = x * 0.5f;
  const float hy = y * 0.5f;
  const float hz = z * 0.5f;
  // 六面各自不同色：**一眼能看出朝向**（单色立方体转过 90° 看不出来，
  // 于是"模型矩阵对不对"这种问题会被漏掉）
  struct Face {
    math::Vec3 normal;
    math::Vec3 color;
    math::Vec3 corners[4];
  };
  const math::Vec3 faces_color[6] = {{1.0f, 0.42f, 0.38f}, {0.42f, 0.78f, 1.0f},
                                     {1.0f, 0.85f, 0.36f}, {0.52f, 0.92f, 0.58f},
                                     {0.78f, 0.55f, 1.0f}, {1.0f, 0.62f, 0.85f}};
  const math::Vec3 normals[6] = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0},
                                 {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
  const math::Vec3 quad[6][4] = {
      {{-hx, -hy, hz}, {hx, -hy, hz}, {hx, hy, hz}, {-hx, hy, hz}},
      {{hx, -hy, -hz}, {-hx, -hy, -hz}, {-hx, hy, -hz}, {hx, hy, -hz}},
      {{hx, -hy, hz}, {hx, -hy, -hz}, {hx, hy, -hz}, {hx, hy, hz}},
      {{-hx, -hy, -hz}, {-hx, -hy, hz}, {-hx, hy, hz}, {-hx, hy, -hz}},
      {{-hx, hy, hz}, {hx, hy, hz}, {hx, hy, -hz}, {-hx, hy, -hz}},
      {{-hx, -hy, -hz}, {hx, -hy, -hz}, {hx, -hy, hz}, {-hx, -hy, hz}},
  };
  for (int face = 0; face < 6; ++face) {
    const auto base = static_cast<std::uint32_t>(face * 4);
    for (int corner = 0; corner < 4; ++corner) {
      push_vertex(mesh.vertices, quad[face][corner], normals[face], faces_color[face]);
    }
    mesh.indices.insert(mesh.indices.end(),
                        {base + 0U, base + 1U, base + 2U, base + 0U, base + 2U, base + 3U});
  }
  return mesh;
}

auto Mesh::cube(float size) -> Mesh { return box(size, size, size); }

namespace {

/// `math::Color` → 0~1 的线性色（光照计算的工作空间）。
[[nodiscard]] auto to_unit(math::Color color) noexcept -> math::Vec3 {
  return math::Vec3{static_cast<float>(color.r) / 255.0f, static_cast<float>(color.g) / 255.0f,
                    static_cast<float>(color.b) / 255.0f};
}

/// 按竖直位置（-1..1）做一个**很小的**亮度渐变。
///
/// 为何幅度小：统一色 + Lambert 光照在平面侧壁上会显得像“贴片”，加一点方向性
/// 渐变才有“实体”的观感；但幅度一大（>20%）就变成“上白下黑”的渐变色卡。
/// `cap(s)` 把线性系数限在 ±12%。
[[nodiscard]] auto vertical_shade(float normalized_y) noexcept -> float {
  const float clamped = normalized_y < -1.0f ? -1.0f : (normalized_y > 1.0f ? 1.0f : normalized_y);
  return 1.0f + clamped * 0.12f;
}

}  // namespace

auto Mesh::colored_box(float x, float y, float z, math::Color color, float bevel) -> Mesh {
  Mesh mesh = box(x, y, z);
  const math::Vec3 base = to_unit(color);
  const float half_y = y * 0.5f;
  for (std::size_t vertex = 0; vertex < mesh.vertex_count(); ++vertex) {
    const std::size_t offset = vertex * kVertexFloats;
    const float world_y = mesh.vertices[offset + 1];
    const float shade = vertical_shade(half_y > 1e-6f ? world_y / half_y : 0.0f);
    mesh.vertices[offset + 6] = base.x * shade;
    mesh.vertices[offset + 7] = base.y * shade;
    mesh.vertices[offset + 8] = base.z * shade;
  }
  // 倒角：曾经试过两种做法，都不行，现在**默认不做**（`bevel` 保留参数但设为 0）。
  //
  // 踩过的两个坑（都有 1080p 实测截图）：
  // ① “面内收缩 + 沿法线外推”：相邻面各自缩了 bevel、却沿各自法线推出，
  //    两面对不上，棱角处留一条**亮缝**；
  // ② “只沿法线外推”：面本身还在原处，但不同面的顶点各自偏移，
  //    面与面之间错开，同样出缝。
  //
  // 真倒角需要把盒子拆成 6 个主面 + 12 条边条 + 8 个角块（共 26 个面），
  // 并保证所有接缝处顶点**完全重合**——那是四倍顶点与一套新的生成逻辑。
  // 直角盒体在本渲染器里本来就干净（光照已经给出了面差），不值得为它付这个代价。
  (void)bevel;
  return mesh;
}

auto Mesh::colored_sphere(float radius, int segments, math::Color color) -> Mesh {
  Mesh mesh = sphere(radius, segments);
  const math::Vec3 base = to_unit(color);
  const float extent = std::max(radius, 1e-6f);
  for (std::size_t vertex = 0; vertex < mesh.vertex_count(); ++vertex) {
    const std::size_t offset = vertex * kVertexFloats;
    const float shade = vertical_shade(mesh.vertices[offset + 1] / extent);
    mesh.vertices[offset + 6] = base.x * shade;
    mesh.vertices[offset + 7] = base.y * shade;
    mesh.vertices[offset + 8] = base.z * shade;
  }
  return mesh;
}

auto Mesh::torus(float major_radius, float minor_radius, int major_segments, int minor_segments,
                 math::Color color) -> Mesh {
  Mesh mesh;
  const int major = std::max(major_segments, 3);
  const int minor = std::max(minor_segments, 3);
  const math::Vec3 base = to_unit(color);
  constexpr float kPi = 3.14159265358979f;
  for (int i = 0; i <= major; ++i) {
    const float theta = static_cast<float>(i) / static_cast<float>(major) * 2.0f * kPi;
    const float cos_t = std::cos(theta);
    const float sin_t = std::sin(theta);
    for (int j = 0; j <= minor; ++j) {
      const float phi = static_cast<float>(j) / static_cast<float>(minor) * 2.0f * kPi;
      const float cos_p = std::cos(phi);
      const float sin_p = std::sin(phi);
      // 环面参数方程：中心圆上的点 + 绕管截面的偏移
      const float ring = major_radius + minor_radius * cos_p;
      const math::Vec3 position{ring * cos_t, minor_radius * sin_p, ring * sin_t};
      const math::Vec3 normal{cos_p * cos_t, sin_p, cos_p * sin_t};
      const float shade = vertical_shade(minor_radius > 1e-6f ? position.y / minor_radius : 0.0f);
      push_vertex(mesh.vertices, position, normal.normalized(),
                  math::Vec3{base.x * shade, base.y * shade, base.z * shade});
    }
  }
  for (int i = 0; i < major; ++i) {
    for (int j = 0; j < minor; ++j) {
      const auto a = static_cast<std::uint32_t>(i * (minor + 1) + j);
      const auto b = static_cast<std::uint32_t>(a + static_cast<std::uint32_t>(minor) + 1U);
      mesh.indices.insert(mesh.indices.end(), {a, b, a + 1U, a + 1U, b, b + 1U});
    }
  }
  return mesh;
}

auto Mesh::cylinder(float radius, float height, int segments, math::Color color) -> Mesh {
  Mesh mesh;
  const int sides = std::max(segments, 3);
  const math::Vec3 base = to_unit(color);
  const float half_h = height * 0.5f;
  constexpr float kPi = 3.14159265358979f;
  // 侧壁：两圈顶点（上/下），法线沿径向
  for (int i = 0; i <= sides; ++i) {
    const float theta = static_cast<float>(i) / static_cast<float>(sides) * 2.0f * kPi;
    const math::Vec3 normal{std::cos(theta), 0.0f, std::sin(theta)};
    push_vertex(mesh.vertices, math::Vec3{normal.x * radius, -half_h, normal.z * radius}, normal,
                math::Vec3{base.x * vertical_shade(-1.0f), base.y * vertical_shade(-1.0f),
                           base.z * vertical_shade(-1.0f)});
    push_vertex(mesh.vertices, math::Vec3{normal.x * radius, half_h, normal.z * radius}, normal,
                math::Vec3{base.x * vertical_shade(1.0f), base.y * vertical_shade(1.0f),
                           base.z * vertical_shade(1.0f)});
  }
  for (int i = 0; i < sides; ++i) {
    const auto a = static_cast<std::uint32_t>(i * 2);
    mesh.indices.insert(mesh.indices.end(), {a, a + 2U, a + 1U, a + 1U, a + 2U, a + 3U});
  }
  // 顶盖与底盖：各自独立的顶点（法线朝上/朝下，才能与侧壁分开着色）
  for (int side = 0; side < 2; ++side) {
    const float y = side == 0 ? half_h : -half_h;
    const math::Vec3 normal{0.0f, side == 0 ? 1.0f : -1.0f, 0.0f};
    const auto center = static_cast<std::uint32_t>(mesh.vertex_count());
    const float shade = vertical_shade(side == 0 ? 1.0f : -1.0f);
    push_vertex(mesh.vertices, math::Vec3{0.0f, y, 0.0f}, normal,
                math::Vec3{base.x * shade, base.y * shade, base.z * shade});
    for (int i = 0; i <= sides; ++i) {
      const float theta = static_cast<float>(i) / static_cast<float>(sides) * 2.0f * kPi;
      push_vertex(mesh.vertices,
                  math::Vec3{std::cos(theta) * radius, y, std::sin(theta) * radius}, normal,
                  math::Vec3{base.x * shade, base.y * shade, base.z * shade});
    }
    for (int i = 0; i < sides; ++i) {
      const auto a = static_cast<std::uint32_t>(center + 1U + static_cast<std::uint32_t>(i));
      if (side == 0) {
        mesh.indices.insert(mesh.indices.end(), {center, a, a + 1U});
      } else {
        mesh.indices.insert(mesh.indices.end(), {center, a + 1U, a});
      }
    }
  }
  return mesh;
}

auto Mesh::plane(float width, float height, math::Color color) -> Mesh {
  Mesh mesh;
  const math::Vec3 base = to_unit(color);
  const float half_w = width * 0.5f;
  const float half_h = height * 0.5f;
  const math::Vec3 normal{0.0f, 0.0f, 1.0f};
  push_vertex(mesh.vertices, math::Vec3{-half_w, -half_h, 0.0f}, normal,
              math::Vec3{base.x * vertical_shade(-1.0f), base.y * vertical_shade(-1.0f),
                         base.z * vertical_shade(-1.0f)});
  push_vertex(mesh.vertices, math::Vec3{half_w, -half_h, 0.0f}, normal,
              math::Vec3{base.x * vertical_shade(-1.0f), base.y * vertical_shade(-1.0f),
                         base.z * vertical_shade(-1.0f)});
  push_vertex(mesh.vertices, math::Vec3{half_w, half_h, 0.0f}, normal,
              math::Vec3{base.x * vertical_shade(1.0f), base.y * vertical_shade(1.0f),
                         base.z * vertical_shade(1.0f)});
  push_vertex(mesh.vertices, math::Vec3{-half_w, half_h, 0.0f}, normal,
              math::Vec3{base.x * vertical_shade(1.0f), base.y * vertical_shade(1.0f),
                         base.z * vertical_shade(1.0f)});
  mesh.indices = {0, 1, 2, 0, 2, 3};
  return mesh;
}

auto Mesh::tinted(math::Color color) const -> Mesh {
  Mesh out = *this;
  const math::Vec3 base = to_unit(color);
  // 保留原有的**相对亮度关系**（而非一概平）：一个球原本上下有渐变，
  // 换色后应该还是那个渐变，只是换了一个色系。
  for (std::size_t vertex = 0; vertex < out.vertex_count(); ++vertex) {
    const std::size_t offset = vertex * kVertexFloats;
    const float original = (out.vertices[offset + 6] + out.vertices[offset + 7] +
                            out.vertices[offset + 8]) / 3.0f;
    // 原亮度归一到 1 附近（原生成器的基准色本身就在 0.35~1.0 之间）
    const float gain = original > 1e-4f ? original : 1.0f;
    out.vertices[offset + 6] = std::min(base.x * gain, 1.0f);
    out.vertices[offset + 7] = std::min(base.y * gain, 1.0f);
    out.vertices[offset + 8] = std::min(base.z * gain, 1.0f);
  }
  return out;
}

auto Mesh::sphere(float radius, int segments) -> Mesh {
  Mesh mesh;
  const int rings = std::max(segments / 2, 3);
  const int slices = std::max(segments, 3);
  for (int ring = 0; ring <= rings; ++ring) {
    const float v = static_cast<float>(ring) / static_cast<float>(rings);
    const float phi = v * 3.14159265358979f;         // 0..π（从北极到南极）
    for (int slice = 0; slice <= slices; ++slice) {
      const float u = static_cast<float>(slice) / static_cast<float>(slices);
      const float theta = u * 2.0f * 3.14159265358979f;
      const math::Vec3 normal{std::sin(phi) * std::cos(theta), std::cos(phi),
                              std::sin(phi) * std::sin(theta)};
      // 纬度渐变上色：让球面的曲率在明暗之外还有第二重线索
      const math::Vec3 color{0.35f + 0.55f * (1.0f - v), 0.55f + 0.35f * v, 0.95f - 0.35f * v};
      push_vertex(mesh.vertices, normal * radius, normal, color);
    }
  }
  for (int ring = 0; ring < rings; ++ring) {
    for (int slice = 0; slice < slices; ++slice) {
      const auto a = static_cast<std::uint32_t>(ring * (slices + 1) + slice);
      const auto b = static_cast<std::uint32_t>(a + static_cast<std::uint32_t>(slices) + 1U);
      mesh.indices.insert(mesh.indices.end(), {a, b, a + 1U, a + 1U, b, b + 1U});
    }
  }
  return mesh;
}

namespace {

/// 一张四边形的四个角：位置、法线、UV（按角顺序）。
struct QuadCorner {
  math::Vec3 position;
  float u;
  float v;
};

/// 把一组四边形组成 Mesh（带 UV）：每面 4 个独立顶点（法线/UV 才能不共用）。
void emit_quads(Mesh& mesh, const QuadCorner quads[][4], const math::Vec3 normals[], int count,
                math::Vec3 color) {
  for (int face = 0; face < count; ++face) {
    const auto base = static_cast<std::uint32_t>(mesh.vertex_count());
    for (int corner = 0; corner < 4; ++corner) {
      const QuadCorner& q = quads[face][corner];
      push_vertex(mesh.vertices, q.position, normals[face], color);
      mesh.uvs.push_back(q.u);
      mesh.uvs.push_back(q.v);
    }
    mesh.indices.insert(mesh.indices.end(),
                        {base + 0U, base + 1U, base + 2U, base + 0U, base + 2U, base + 3U});
  }
}

}  // namespace

auto Mesh::textured_plane(float width, float height, math::Color tint) -> Mesh {
  Mesh mesh;
  const float hw = width * 0.5f;
  const float hh = height * 0.5f;
  // UV 的 v 轴**翻转**：纹理原点在左上，而三维里 +Y 是上。
  // 不翻会让图标上下颠倒——那种错误看着像“图贴反了”，但很容易误判成模型朝向问题。
  const QuadCorner corners[1][4] = {{QuadCorner{{-hw, -hh, 0.0f}, 0.0f, 1.0f},
                                     QuadCorner{{hw, -hh, 0.0f}, 1.0f, 1.0f},
                                     QuadCorner{{hw, hh, 0.0f}, 1.0f, 0.0f},
                                     QuadCorner{{-hw, hh, 0.0f}, 0.0f, 0.0f}}};
  const math::Vec3 normals[1] = {{0.0f, 0.0f, 1.0f}};
  emit_quads(mesh, corners, normals, 1, to_unit(tint));
  return mesh;
}

auto Mesh::textured_box(float x, float y, float z, math::Color tint) -> Mesh {
  Mesh mesh;
  const float hx = x * 0.5f;
  const float hy = y * 0.5f;
  const float hz = z * 0.5f;
  // 六个面，每面 UV 都铺满 0..1（六面同一图案）。
  // 绕序与 `Mesh::box` 逐面向量严格一致（已用几何法线对过），否则背面会被当成正面。
  const QuadCorner corners[6][4] = {
      // +Z
      {{{-hx, -hy, hz}, 0.0f, 1.0f},
       {{hx, -hy, hz}, 1.0f, 1.0f},
       {{hx, hy, hz}, 1.0f, 0.0f},
       {{-hx, hy, hz}, 0.0f, 0.0f}},
      // -Z
      {{{hx, -hy, -hz}, 0.0f, 1.0f},
       {{-hx, -hy, -hz}, 1.0f, 1.0f},
       {{-hx, hy, -hz}, 1.0f, 0.0f},
       {{hx, hy, -hz}, 0.0f, 0.0f}},
      // +X
      {{{hx, -hy, hz}, 0.0f, 1.0f},
       {{hx, -hy, -hz}, 1.0f, 1.0f},
       {{hx, hy, -hz}, 1.0f, 0.0f},
       {{hx, hy, hz}, 0.0f, 0.0f}},
      // -X
      {{{-hx, -hy, -hz}, 0.0f, 1.0f},
       {{-hx, -hy, hz}, 1.0f, 1.0f},
       {{-hx, hy, hz}, 1.0f, 0.0f},
       {{-hx, hy, -hz}, 0.0f, 0.0f}},
      // +Y
      {{{-hx, hy, hz}, 0.0f, 0.0f},
       {{hx, hy, hz}, 1.0f, 0.0f},
       {{hx, hy, -hz}, 1.0f, 1.0f},
       {{-hx, hy, -hz}, 0.0f, 1.0f}},
      // -Y
      {{{-hx, -hy, -hz}, 0.0f, 0.0f},
       {{hx, -hy, -hz}, 1.0f, 0.0f},
       {{hx, -hy, hz}, 1.0f, 1.0f},
       {{-hx, -hy, hz}, 0.0f, 1.0f}},
  };
  const math::Vec3 normals[6] = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
  emit_quads(mesh, corners, normals, 6, to_unit(tint));
  return mesh;
}

auto Mesh::textured_slab(float width, float thickness, float depth, math::Color side_color) -> Mesh {
  Mesh mesh;
  const float hx = width * 0.5f;
  const float hy = thickness * 0.5f;
  const float hz = depth * 0.5f;
  const math::Vec3 side = to_unit(side_color);
  // 顶面用“表面色 = 白色”作顶点色：纹理原色不被污染（真正颜色来自贴图）。
  const math::Vec3 top_tint{1.0f, 1.0f, 1.0f};

  // —— 顶面（+Y）——
  // 俯视时：v 小的一端在 -Z（远端），v 大的一端在 +Z（近端）；u 小的一端在 -X。
  // ⚠ UV 从 `kTopInset` 开始而不是 0：贴图左上角埋了一块侧壁色的角带（供侧壁采样），
  // 顶面必须避开它，否则桌面画面会被那条色带压进 UV 范围（看起来像“被推向右下”）。
  // 绕序必须让几何法线 = +Y（右手系）：
  //   (-x,-z) → (-x,+z) → (+x,+z) → (+x,-z)
  {
    const auto base = static_cast<std::uint32_t>(mesh.vertex_count());
    const float u0 = kTopInset;
    const float v0 = kTopInset;
    const QuadCorner corners[4] = {QuadCorner{{-hx, hy, -hz}, u0, v0},
                                   QuadCorner{{-hx, hy, hz}, u0, 1.0f},
                                   QuadCorner{{hx, hy, hz}, 1.0f, 1.0f},
                                   QuadCorner{{hx, hy, -hz}, 1.0f, v0}};
    for (const QuadCorner& corner : corners) {
      push_vertex(mesh.vertices, corner.position, math::Vec3{0.0f, 1.0f, 0.0f}, top_tint);
      mesh.uvs.push_back(corner.u);
      mesh.uvs.push_back(corner.v);
    }
    mesh.indices.insert(mesh.indices.end(), {base + 0U, base + 1U, base + 2U, base + 0U,
                                             base + 2U, base + 3U});
  }

  // —— 底面 + 四个侧壁：纯色，UV 钉在“侧壁色角落” ——
  // 各面法线/绕序与 `textured_box` 完全同构（那里逐面对过几何法线）。
  const QuadCorner sides[5][4] = {
      // -Y（下）
      {{{-hx, -hy, -hz}, kSideU, kSideV},
       {{hx, -hy, -hz}, kSideU, kSideV},
       {{hx, -hy, hz}, kSideU, kSideV},
       {{-hx, -hy, hz}, kSideU, kSideV}},
      // +Z（近）
      {{{-hx, -hy, hz}, kSideU, kSideV},
       {{hx, -hy, hz}, kSideU, kSideV},
       {{hx, hy, hz}, kSideU, kSideV},
       {{-hx, hy, hz}, kSideU, kSideV}},
      // -Z（远）
      {{{hx, -hy, -hz}, kSideU, kSideV},
       {{-hx, -hy, -hz}, kSideU, kSideV},
       {{-hx, hy, -hz}, kSideU, kSideV},
       {{hx, hy, -hz}, kSideU, kSideV}},
      // +X（右）
      {{{hx, -hy, hz}, kSideU, kSideV},
       {{hx, -hy, -hz}, kSideU, kSideV},
       {{hx, hy, -hz}, kSideU, kSideV},
       {{hx, hy, hz}, kSideU, kSideV}},
      // -X（左）
      {{{-hx, -hy, -hz}, kSideU, kSideV},
       {{-hx, -hy, hz}, kSideU, kSideV},
       {{-hx, hy, hz}, kSideU, kSideV},
       {{-hx, hy, -hz}, kSideU, kSideV}},
  };
  const math::Vec3 side_normals[5] = {{0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}};
  for (int face = 0; face < 5; ++face) {
    const auto base = static_cast<std::uint32_t>(mesh.vertex_count());
    for (const QuadCorner& corner : sides[face]) {
      push_vertex(mesh.vertices, corner.position, side_normals[face], side);
      mesh.uvs.push_back(corner.u);
      mesh.uvs.push_back(corner.v);
    }
    mesh.indices.insert(mesh.indices.end(), {base + 0U, base + 1U, base + 2U, base + 0U,
                                             base + 2U, base + 3U});
  }
  return mesh;
}

auto Mesh::flattened(float offset_y, float expand) const -> Mesh {
  // —— 先找出“朝上、面积最大”的那组三角形（即顶面）——
  //
  // 为何按“法线向上 + 面积最大”选：对盒体/平板，顶面就是正确的投影轮廓；
  // 而直接把所有面压平会产生一堆共面重叠多边形，在 z-buffer 里互相争夺
  // （实测：影子画了但完全看不见，因为先画的赢了、其余全部丢弃）。
  float best_area = -1.0f;
  float best_y = 0.0f;
  for (std::size_t tri = 0; tri + 2 < indices.size(); tri += 3) {
    const std::size_t i0 = indices[tri];
    const std::size_t i1 = indices[tri + 1];
    const std::size_t i2 = indices[tri + 2];
    const float ny = vertices[i0 * kVertexFloats + 4];
    if (ny < 0.9f) continue;   // 只要法线朝上的面
    const float* a = &vertices[i0 * kVertexFloats];
    const float* b = &vertices[i1 * kVertexFloats];
    const float* c = &vertices[i2 * kVertexFloats];
    // XZ 平面上的三角形面积（叉积的模 / 2）
    const float e1x = b[0] - a[0];
    const float e1z = b[2] - a[2];
    const float e2x = c[0] - a[0];
    const float e2z = c[2] - a[2];
    const float area = std::abs(e1x * e2z - e1z * e2x) * 0.5f;
    if (area > best_area) {
      best_area = area;
      best_y = a[1];
    }
  }
  (void)best_y;
  if (best_area <= 0.0f) return Mesh{};   // 没有朝上的面（例如纯平面模型）→ 不画影子

  // —— 收集所有“朝上面”的三角形，压到平面上 ——
  //
  // 注意：这里收集**所有**朝上的面而不只是最大那一面。对盒体只有顶面（两个三角形），
  // 对圆柱/球则会拿到顶部的一片面——那正是它们投影到地面的真实轮廓近似。
  // 关键差别是：它们**不会重叠**（同一个物体的不同朝上面在 XZ 上通常分居不同区域），
  // 所以不会出现“压平后互相遮”的问题。
  Mesh out{};
  out.vertices.reserve(vertices.size());
  out.indices.reserve(indices.size());
  std::vector<std::size_t> remap(vertex_count(), static_cast<std::size_t>(-1));
  float min_x = 1e30f, max_x = -1e30f, min_z = 1e30f, max_z = -1e30f;
  // 第一道：登记需要保留的顶点并算副中心
  std::vector<std::size_t> used;
  used.reserve(indices.size());
  for (std::size_t tri = 0; tri + 2 < indices.size(); tri += 3) {
    const std::size_t i0 = indices[tri];
    if (vertices[i0 * kVertexFloats + 4] < 0.9f) continue;
    for (int k = 0; k < 3; ++k) {
      const std::size_t index = indices[tri + static_cast<std::size_t>(k)];
      if (remap[index] == static_cast<std::size_t>(-1)) {
        remap[index] = used.size();
        used.push_back(index);
        const float x = vertices[index * kVertexFloats];
        const float z = vertices[index * kVertexFloats + 2];
        min_x = std::min(min_x, x);
        max_x = std::max(max_x, x);
        min_z = std::min(min_z, z);
        max_z = std::max(max_z, z);
      }
    }
  }
  if (used.empty()) return Mesh{};
  const float center_x = (min_x + max_x) * 0.5f;
  const float center_z = (min_z + max_z) * 0.5f;
  const float safe_expand = std::clamp(expand, 1.0f, 3.0f);
  // 第二道：写压平后的顶点（Y 统一 offset_y、法线朝上、沿 X/Z 放大）
  out.vertices.resize(used.size() * kVertexFloats);
  for (std::size_t slot = 0; slot < used.size(); ++slot) {
    const std::size_t source = used[slot];
    const float x = vertices[source * kVertexFloats];
    const float z = vertices[source * kVertexFloats + 2];
    float* dst = &out.vertices[slot * kVertexFloats];
    dst[0] = center_x + (x - center_x) * safe_expand;
    dst[1] = offset_y;
    dst[2] = center_z + (z - center_z) * safe_expand;
    dst[3] = 0.0f;
    dst[4] = 1.0f;
    dst[5] = 0.0f;
    // 颜色沿用原顶点色（调用方随后会用 `tinted()` 覆盖）
    dst[6] = vertices[source * kVertexFloats + 6];
    dst[7] = vertices[source * kVertexFloats + 7];
    dst[8] = vertices[source * kVertexFloats + 8];
  }
  // 第三道：重建索引（只保留朝上面）
  for (std::size_t tri = 0; tri + 2 < indices.size(); tri += 3) {
    const std::size_t i0 = indices[tri];
    if (vertices[i0 * kVertexFloats + 4] < 0.9f) continue;
    for (int k = 0; k < 3; ++k) {
      out.indices.push_back(static_cast<std::uint32_t>(remap[indices[tri + static_cast<std::size_t>(k)]]));
    }
  }
  return out;
}

auto Mesh::textured_icon_box(float x, float y, float z, math::Color side_color) -> Mesh {
  Mesh mesh;
  const float hx = x * 0.5f;
  const float hy = y * 0.5f;
  const float hz = z * 0.5f;
  const math::Vec3 white{1.0f, 1.0f, 1.0f};

  // —— 正面（+Z）：铺满 0..1 UV，避开角落纯色带 ——
  {
    const auto base = static_cast<std::uint32_t>(mesh.vertex_count());
    const float u0 = kTopInset;
    const float v0 = kTopInset;
    const QuadCorner corners[4] = {QuadCorner{{-hx, -hy, hz}, u0, 1.0f},
                                   QuadCorner{{hx, -hy, hz}, 1.0f, 1.0f},
                                   QuadCorner{{hx, hy, hz}, 1.0f, v0},
                                   QuadCorner{{-hx, hy, hz}, u0, v0}};
    for (const QuadCorner& corner : corners) {
      push_vertex(mesh.vertices, corner.position, math::Vec3{0.0f, 0.0f, 1.0f}, white);
      mesh.uvs.push_back(corner.u);
      mesh.uvs.push_back(corner.v);
    }
    mesh.indices.insert(mesh.indices.end(), {base + 0U, base + 1U, base + 2U, base + 0U,
                                             base + 2U, base + 3U});
  }

  // —— 其余五面：纯侧面色（UV 钉在纯色角落）。绕序与 `textured_box` 同构。 ——
  const QuadCorner sides[5][4] = {
      // -Z（远）
      {{{-hx, -hy, -hz}, kSideU, kSideV},
       {{hx, -hy, -hz}, kSideU, kSideV},
       {{hx, hy, -hz}, kSideU, kSideV},
       {{-hx, hy, -hz}, kSideU, kSideV}},
      // +X（右）
      {{{hx, -hy, hz}, kSideU, kSideV},
       {{hx, -hy, -hz}, kSideU, kSideV},
       {{hx, hy, -hz}, kSideU, kSideV},
       {{hx, hy, hz}, kSideU, kSideV}},
      // -X（左）
      {{{-hx, -hy, -hz}, kSideU, kSideV},
       {{-hx, -hy, hz}, kSideU, kSideV},
       {{-hx, hy, hz}, kSideU, kSideV},
       {{-hx, hy, -hz}, kSideU, kSideV}},
      // +Y（上）
      {{{-hx, hy, hz}, kSideU, kSideV},
       {{hx, hy, hz}, kSideU, kSideV},
       {{hx, hy, -hz}, kSideU, kSideV},
       {{-hx, hy, -hz}, kSideU, kSideV}},
      // -Y（下）
      {{{-hx, -hy, -hz}, kSideU, kSideV},
       {{hx, -hy, -hz}, kSideU, kSideV},
       {{hx, -hy, hz}, kSideU, kSideV},
       {{-hx, -hy, hz}, kSideU, kSideV}},
  };
  const math::Vec3 side_normals[5] = {{0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
  const math::Vec3 side_tint{static_cast<float>(side_color.r) / 255.0f,
                             static_cast<float>(side_color.g) / 255.0f,
                             static_cast<float>(side_color.b) / 255.0f};
  for (int face = 0; face < 5; ++face) {
    const auto base = static_cast<std::uint32_t>(mesh.vertex_count());
    for (const QuadCorner& corner : sides[face]) {
      push_vertex(mesh.vertices, corner.position, side_normals[face], side_tint);
      mesh.uvs.push_back(corner.u);
      mesh.uvs.push_back(corner.v);
    }
    mesh.indices.insert(mesh.indices.end(), {base + 0U, base + 1U, base + 2U, base + 0U,
                                             base + 2U, base + 3U});
  }
  return mesh;
}

}  // namespace st::raster

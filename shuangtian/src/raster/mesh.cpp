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

}  // namespace st::raster

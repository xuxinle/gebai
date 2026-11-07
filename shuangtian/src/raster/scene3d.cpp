/// `Lighting` 的非内联成员：目前只有 IBL 探针的归一化。
///
/// ## 为什么这一段要单独一个文件
///
/// 它是**两条腿共用的语义**（软件光栅 / D3D11），而不是某一个后端的实现细节。
/// 放进任何一个后端文件里，另一个人就会在另一个后端里"顺手再写一遍"——
/// 本仓已经因为这种"各写一遍"出过四次两条腿不一致的缺陷。
///
/// 放在 `scene3d.cpp`（`Scene3D` 这一层的公共实现）里，语义归属是清楚的：
/// **它定义的是"环境是什么"，不是"怎么画"。**

#include "st/raster/scene3d.hpp"

#include "st/codec/image.hpp"
#include "st/core/log.hpp"
#include "st/raster/color_pipeline.hpp"

namespace st::raster {

auto EnvironmentMap::load_faces(std::string_view basis) -> bool {
  if (face_paths[0].empty()) return false;
  int common = 0;
  for (int i = 0; i < 6; ++i) {
    std::string path = face_paths[static_cast<std::size_t>(i)];
    // 相对路径按 `basis` 解析（与网格 `obj:` 的路径口径一致）
    const bool absolute = path.size() > 1 &&
                          (path[0] == '/' || path[0] == '\\' ||
                           (path.size() > 2 && path[1] == ':'));
    if (!absolute && !basis.empty()) {
      std::string joined{basis};
      if (!joined.empty() && joined.back() != '/' && joined.back() != '\\') joined.push_back('/');
      joined += path;
      path = std::move(joined);
    }
    const auto decoded = st::codec::decode_image_file(path);
    if (!decoded.has_value()) return false;
    const st::codec::DecodedImage& image = *decoded;
    if (image.width <= 0 || image.height <= 0) return false;
    // 六个面尺寸必须一致：不齐无法插值（而“看着有图但反光位置错”极难查）。
    if (i == 0) common = image.width;
    if (image.width != common || image.height != common) return false;
    const std::size_t count = static_cast<std::size_t>(image.width) *
                              static_cast<std::size_t>(image.height);
    auto& face = faces[static_cast<std::size_t>(i)];
    face.resize(count);
    // 解码结果是 **sRGB 编码的 8bit**，而 IBL 核需要**线性**值。
    // 不转会让环境光整体偏亮（与 AgX 那次同一类错）。
    for (std::size_t k = 0; k < count; ++k) {
      const std::size_t base = k * 4U;
      if (base + 2 >= image.rgba.size()) {
        face[k] = math::Vec3{0.0f, 0.0f, 0.0f};
        continue;
      }
      face[k] = st::raster::srgb_to_linear(
          math::Vec3{static_cast<float>(image.rgba[base]) / 255.0f,
                     static_cast<float>(image.rgba[base + 1]) / 255.0f,
                     static_cast<float>(image.rgba[base + 2]) / 255.0f});
    }
  }
  size = common;
  return true;
}

auto EnvironmentMap::sample_direction(math::Vec3 direction) const -> math::Vec3 {
  if (faces[0].empty() || size <= 0) return math::Vec3{0.0f, 0.0f, 0.0f};
  const math::Vec3 d = direction.normalized();
  // 确定主方向 → 面索引（+X, -X, +Y, -Y, +Z, -Z）
  const float ax = std::abs(d.x);
  const float ay = std::abs(d.y);
  const float az = std::abs(d.z);
  int face = 0;
  float u = 0.0f;
  float v = 0.0f;
  float major = 0.0f;
  if (ax >= ay && ax >= az) {
    major = ax;
    if (d.x >= 0.0f) {
      face = 0;
      u = -d.z / major;
      v = -d.y / major;
    } else {
      face = 1;
      u = d.z / major;
      v = -d.y / major;
    }
  } else if (ay >= az) {
    major = ay;
    if (d.y >= 0.0f) {
      face = 2;
      u = d.x / major;
      v = d.z / major;
    } else {
      face = 3;
      u = d.x / major;
      v = -d.z / major;
    }
  } else {
    major = az;
    if (d.z >= 0.0f) {
      face = 4;
      u = d.x / major;
      v = -d.y / major;
    } else {
      face = 5;
      u = -d.x / major;
      v = -d.y / major;
    }
  }
  // [-1,1] → 纹素坐标（双线性）
  const float fx = (u * 0.5f + 0.5f) * static_cast<float>(size) - 0.5f;
  const float fy = (v * 0.5f + 0.5f) * static_cast<float>(size) - 0.5f;
  const int x0 = static_cast<int>(std::floor(fx));
  const int y0 = static_cast<int>(std::floor(fy));
  const float tx = fx - static_cast<float>(x0);
  const float ty = fy - static_cast<float>(y0);
  const auto texel = [this, face](int x, int y) -> math::Vec3 {
    const int cx = std::clamp(x, 0, size - 1);
    const int cy = std::clamp(y, 0, size - 1);
    const std::size_t index = static_cast<std::size_t>(cy) * static_cast<std::size_t>(size) +
                              static_cast<std::size_t>(cx);
    return faces[static_cast<std::size_t>(face)][index];
  };
  const math::Vec3 c00 = texel(x0, y0);
  const math::Vec3 c10 = texel(x0 + 1, y0);
  const math::Vec3 c01 = texel(x0, y0 + 1);
  const math::Vec3 c11 = texel(x0 + 1, y0 + 1);
  const auto mix = [](float a, float b, float t) { return a + (b - a) * t; };
  return math::Vec3{mix(mix(c00.x, c10.x, tx), mix(c01.x, c11.x, tx), ty),
                    mix(mix(c00.y, c10.y, tx), mix(c01.y, c11.y, tx), ty),
                    mix(mix(c00.z, c10.z, tx), mix(c01.z, c11.z, tx), ty)};
}

auto EnvironmentMap::make_filtered_probes(const Lighting& lighting, math::Vec3* out_dirs,
                                          math::Vec3* out_colors, std::size_t capacity) const
    -> std::size_t {
  if (faces[0].empty() || size <= 0) return 0;
  if (out_dirs == nullptr || out_colors == nullptr || capacity == 0) return 0;
  // ## 为何是「预过滤的方向探针」而不是真 cubemap 采样
  //
  // 真 PMREM 会生成一条 mip 链，按粗糙度选层。本框架的 IBL 内核是
  // **方向探针 + 余弦/镜面加权**（`pbr_ibl_diffuse` / `pbr_ibl_specular`），
  // 没有贴图采样路径。
  //
  // 所以这里做的是：把环境贴图**预过滤成一组方向探针**——每个探针
  // 在它那个方向取一个**半球平均**（用 `roughness` 控制锥角）。
  // 这与真 PMREM 的差别：
  //
  // | 项 | 真 PMREM | 本实现 |
  // |---|---|---|
  // | 高光形状 | 连续（按 mip 层） | 探针间插值，有台阶 |
  // | 粗糙面模糊 | 精确的 GGX 卷积 | 近似（锥角平均）|
  // | 镜像反射 | ✅ | 只有在 sharpness 高时接近 |
  //
  // 所以它能做到“环境里有明暗结构、粗糙面高光变钝”，
  // 但**不会与 Cycles 的 PMREM 逐像素相同**。
  const std::size_t count = std::min<std::size_t>(capacity, probes);
  if (count == 0) return 0;
  // 黄金角螺旋分布：确定性（不用随机数，否则两次运行不同）
  for (std::size_t i = 0; i < count; ++i) {
    const float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(count);
    const float cos_theta = 1.0f - 2.0f * t;   // 均匀分布于球面
    const float sin_theta = std::sqrt(std::max(0.0f, 1.0f - cos_theta * cos_theta));
    const float phi = static_cast<float>(i) * 2.399963f;
    const math::Vec3 direction{sin_theta * std::cos(phi), cos_theta,
                               sin_theta * std::sin(phi)};
    out_dirs[i] = direction;
    // 预过滤：沿锥角取若干样本平均（锥角由 `filter_angle` 控制）
    const float angle = std::clamp(filter_angle, 0.0f, 1.5707963f);
    if (angle < 1e-4f) {
      out_colors[i] = sample_direction(direction) * exposure;
      continue;
    }
    // 锥内取 8 个方向（固定螺旋），做平均
    math::Vec3 sum{0.0f, 0.0f, 0.0f};
    constexpr int kTaps = 8;
    for (int k = 0; k < kTaps; ++k) {
      const float kt = (static_cast<float>(k) + 0.5f) / static_cast<float>(kTaps);
      const float kcos = 1.0f - kt * (1.0f - std::cos(angle));
      const float ksin = std::sqrt(std::max(0.0f, 1.0f - kcos * kcos));
      const float kphi = static_cast<float>(k) * 2.399963f;
      // 以 direction 为极轴构建局部坐标系
      const math::Vec3 up = std::abs(direction.y) > 0.99f ? math::Vec3{1.0f, 0.0f, 0.0f}
                                                         : math::Vec3{0.0f, 1.0f, 0.0f};
      const math::Vec3 tangent = up.cross(direction).normalized();
      const math::Vec3 bitangent = direction.cross(tangent);
      const math::Vec3 sample_dir{tangent.x * (ksin * std::cos(kphi)) +
                                      bitangent.x * (ksin * std::sin(kphi)) +
                                      direction.x * kcos,
                                  tangent.y * (ksin * std::cos(kphi)) +
                                      bitangent.y * (ksin * std::sin(kphi)) +
                                      direction.y * kcos,
                                  tangent.z * (ksin * std::cos(kphi)) +
                                      bitangent.z * (ksin * std::sin(kphi)) +
                                      direction.z * kcos};
      const math::Vec3 c = sample_direction(sample_dir);
      sum.x += c.x;
      sum.y += c.y;
      sum.z += c.z;
    }
    const float inv = 1.0f / static_cast<float>(kTaps);
    out_colors[i] = math::Vec3{sum.x * inv * exposure, sum.y * inv * exposure,
                               sum.z * inv * exposure};
  }
  (void)lighting;
  return count;
}

auto Lighting::make_ibl_probes(math::Vec3* out_dirs, math::Vec3* out_colors,
                               std::size_t capacity) const -> std::size_t {
  if (ibl_strength <= 0.0f || out_dirs == nullptr || out_colors == nullptr) return 0;

  // ① 有环境贴图（PMREM，2026-11-07）：预过滤成方向探针。
  //
  // 为何放在**显式探针之前**：环境贴图是更强的表达（它有真实的高频结构），
  // 而 `env_probes` 是手工填的几个方向——两者同时存在时，贴图应当赢。
  if (env_map.size > 0 && !env_map.faces[0].empty()) {
    const std::size_t count =
        env_map.make_filtered_probes(*this, out_dirs, out_colors, capacity);
    if (count > 0) return count;
  }

  // ① 有显式探针：原样给出（这是 three 的 `scene.environment` 的等价物，
  //    也是 Blender `world` 有方向结构时的表达方式）。
  if (env_probe_count > 0) {
    const std::size_t count = std::min(env_probe_count, capacity);
    for (std::size_t i = 0; i < count; ++i) {
      out_dirs[i] = env_probes[i].direction;
      out_colors[i] = env_probes[i].color;
    }
    return count;
  }

  // ② 没有显式探针：用天/地两色当两个方向上/下的探针。
  //
  // 这不是"旧模型那个假反射"。区别是**它走的是同一个 IBL 公式**：
  // 漫反射按余弦加权、高光按反射向量加点、强度乘 `ibl_strength`。
  // 旧模型是把背景色乘一个系数直接加到结果上，连 BRDF 都不经过。
  //
  // 这样调用方只设 `ibl_sky`/`ibl_ground` 也能得到合理的环境光照，
  // 而不必理解"探针"这个概念——这是 bpy 的 `world`（均匀发光）与
  // 剧本里"上亮下暗"两种常见需求的短路径。
  if (capacity < 2) return 0;
  out_dirs[0] = math::Vec3{0.0f, 1.0f, 0.0f};
  out_colors[0] = ibl_sky;
  out_dirs[1] = math::Vec3{0.0f, -1.0f, 0.0f};
  out_colors[1] = ibl_ground;
  return 2;
}

auto ShadowMap::make_view_projection(const Lighting& lighting) const -> math::Mat4 {
  if (strength <= 0.0f) return math::Mat4::identity();
  if (light_index < 0 || static_cast<std::size_t>(light_index) >= lighting.extra_lights.size()) {
    return math::Mat4::identity();
  }
  const Light& light = lighting.extra_lights[static_cast<std::size_t>(light_index)];
  if (light.intensity <= 0.0f) return math::Mat4::identity();
  // 点光需要立方体贴图（六面），单张正交图做不到——与软件腿一致地跳过。
  if (light.kind == LightKind::Point) return math::Mat4::identity();

  math::Vec3 direction = light.direction;
  const float length = direction.length();
  if (length <= 1e-6f) return math::Mat4::identity();
  direction = math::Vec3{direction.x / length, direction.y / length, direction.z / length};

  // 光源相机：从「逆向远处」看向原点（平行光无位置）。
  // 聚光有位置，但从它的位置看与从远处看（同方向）在小锥角下几乎一致——
  // 这个取舍的代价是：大锥角聚光的阴影会偏小。
  const float dist = std::max(extent * 2.0f, 4.0f);
  const bool spot = light.kind == LightKind::Spot;
  const math::Vec3 origin = spot ? light.position : math::Vec3{0.0f, 0.0f, 0.0f};
  const math::Vec3 eye{origin.x - direction.x * dist, origin.y - direction.y * dist,
                       origin.z - direction.z * dist};
  const math::Vec3 target{eye.x + direction.x, eye.y + direction.y, eye.z + direction.z};
  // up 不能与 direction 平行：|direction.y| 接近 1 时改用 X 轴
  const math::Vec3 up = std::abs(direction.y) > 0.99f ? math::Vec3{1.0f, 0.0f, 0.0f}
                                                     : math::Vec3{0.0f, 1.0f, 0.0f};
  const math::Mat4 view = math::Mat4::look_at(eye, target, up);
  const math::Mat4 proj = math::Mat4::orthographic(extent * 2.0f, 1.0f, 0.01f, dist * 2.0f);
  return proj * view;
}

}  // namespace st::raster

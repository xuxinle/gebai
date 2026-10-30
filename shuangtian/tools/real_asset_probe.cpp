// 真实素材渲染探针：把下载来的真实 glTF/GLB 渲成图片，**看效果**。
//
// ## 为什么必须真渲染（探针账目不够）
//
// "解码出 5 张贴图、15452 个三角形"只证明数据进了内存。它不能回答：
// 贴图**贴对位置**了吗？法线方向对吗？材质参数看着对吗？模型朝向合理吗？
// 那些只有看图才知道。
//
// 这个探针做的事：读素材 → 居中和缩放到取景框 → 渲染 → 存 PNG。
// 一次跑一批，产物并排输出，人眼过一遍就能判断"生态吸收"到底成没成。
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <string>
#include <vector>

#include "st/codec/png.hpp"
#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/gltf.hpp"
#include "st/raster/mesh_ops.hpp"
#include "st/raster/scene3d.hpp"

namespace {

using st::math::Color;
using st::math::Mat4;
using st::math::Vec3;

constexpr int kSize = 400;
constexpr float camera_fov = 40.0f;

auto canvas_to_rgba(const st::raster::Canvas& canvas) -> std::vector<std::uint8_t> {
  const int width = canvas.physical_width();
  const int height = canvas.physical_height();
  std::vector<std::uint8_t> out(
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const Color pixel = canvas.pixel_at(x, y);
      const std::size_t at =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
           static_cast<std::size_t>(x)) * 4U;
      const float alpha = static_cast<float>(pixel.a) / 255.0f;
      const auto unpremultiply = [alpha](std::uint8_t value) {
        if (alpha <= 0.0f) return static_cast<std::uint8_t>(0);
        const float scaled = static_cast<float>(value) / alpha;
        return static_cast<std::uint8_t>(std::lround(std::min(scaled, 255.0f)));
      };
      out[at + 0] = unpremultiply(pixel.r);
      out[at + 1] = unpremultiply(pixel.g);
      out[at + 2] = unpremultiply(pixel.b);
      out[at + 3] = pixel.a;
    }
  }
  return out;
}

/// 把整份 glTF 场景合成一个"居中的包围盒 + 合适的相机"。
struct Framing {
  Vec3 center{};
  float radius{1.0f};
  bool valid{false};
};

auto frame_scene(const st::raster::GltfScene& scene) -> Framing {
  st::raster::Bounds total = st::raster::Bounds::empty();
  for (const auto& node : scene.nodes) {
    for (const std::size_t mesh_index : node.meshes) {
      if (mesh_index >= scene.meshes.size()) continue;
      const auto local = st::raster::bounds(scene.meshes[mesh_index]);
      if (local.is_empty()) continue;
      const auto world = st::raster::transform(scene.meshes[mesh_index], node.transform);
      total = total.union_with(st::raster::bounds(world));
    }
  }
  if (total.is_empty()) return Framing{};
  Framing framing{};
  framing.center = total.center();
  const Vec3 size = total.size();
  framing.radius = std::max({size.x, size.y, size.z}) * 0.5f;
  if (framing.radius < 1.0e-4f) framing.radius = 1.0f;
  framing.valid = true;
  return framing;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string out_dir = argc > 1 ? argv[1] : "out/real";
  (void)st::fs::create_directories(out_dir);
  std::vector<std::string> targets;
  for (int i = 2; i < argc; ++i) targets.emplace_back(argv[i]);

  auto scene3d = st::raster::Scene3D::create(kSize, kSize);
  if (!scene3d.has_value()) {
    st::log::err("无法创建场景：{}\n", scene3d.error().message);
    return 1;
  }

  int rendered = 0;
  for (const std::string& path : targets) {
    st::log::out("\n=== {} ===\n", path);
    const auto scene = st::raster::gltf_read_file(path);
    if (!scene.has_value()) {
      st::log::out("  [拒绝] {}\n", scene.error().message);
      continue;
    }
    const Framing framing = frame_scene(*scene);
    if (!framing.valid) {
      st::log::out("  [空场景] 没有任何可画几何\n");
      continue;
    }
    st::log::out("  居中 ({:.2f},{:.2f},{:.2f}) 半径 {:.2f}\n", static_cast<double>(framing.center.x),
                 static_cast<double>(framing.center.y), static_cast<double>(framing.center.z),
                 static_cast<double>(framing.radius));

    // ## 相机距离按包围球算，而不是凭常数
    //
    // 初版用 `radius * 3.4`，对 helmet（半径 1.0）合适，但对 BoxTextured
    //（半径 0.5）就把模型顶到画框边缘了——因为透视下"物体占画面的比例"
    // 由 `2*atan(radius/distance)` 决定，而 fov 是固定的。
    // 正解：由 fov 反解距离，让包围球刚好占画面的 62%。
    constexpr float kFillRatio = 0.62f;
    const float half_fov = camera_fov * 0.5f * 3.14159265f / 180.0f;
    const float distance = framing.radius / (kFillRatio * std::tan(half_fov));
    st::raster::Camera camera{};
    camera.fov_y_degrees = camera_fov;
    // ## 近/远裁剪面必须跟着模型尺度走（真实素材暴露的缺陷）
    //
    // 默认 `near_z = 0.1` 对米级模型合适，但 Avocado / Corset 这类素材
    // 半径只有 0.03（3cm）：相机距离约 0.08，**整个模型落在近裁剪面之内**，
    // 渲染结果为 0 像素。而报错信息什么都没有——画面只是一片背景色。
    //
    // 这是"生态素材"的普遍问题：不同来源的模型单位差几个数量级
    //（珠宝 mm、建筑 km），任何固定的 near/far 都会在某个尺度上失效。
    camera.near_z = std::max(distance * 0.01f, 1.0e-5f);
    camera.far_z = distance * 10.0f;
    camera.eye = Vec3{framing.center.x + distance * 0.62f,
                      framing.center.y + distance * 0.45f,
                      framing.center.z + distance * 0.65f};
    camera.target = framing.center;
    camera.up = Vec3{0.0f, 1.0f, 0.0f};

    st::raster::Lighting lighting{};
    lighting.key_direction = Vec3{0.45f, 0.80f, 0.55f};
    lighting.ambient = 0.30f;
    lighting.diffuse = 0.85f;

    (*scene3d)->begin_frame(Color{26, 28, 34, 255});
    (*scene3d)->set_camera(camera);
    (*scene3d)->set_lighting(lighting);
    (*scene3d)->set_cull_mode(st::raster::CullMode::None);

    std::size_t drawn_triangles = 0;
    for (const auto& node : scene->nodes) {
      for (const std::size_t mesh_index : node.meshes) {
        if (mesh_index >= scene->meshes.size()) continue;
        const auto& mesh = scene->meshes[mesh_index];
        drawn_triangles += mesh.indices.size() / 3U;

        // 材质：从 glTF 槽位搬过来
        st::raster::Material material{};
        bool texture_bound = false;
        if (node.material >= 0 &&
            static_cast<std::size_t>(node.material) < scene->materials.size()) {
          const auto& slot = scene->materials[static_cast<std::size_t>(node.material)];
          material.base_color = Vec3{static_cast<float>(slot.base_color.r) / 255.0f,
                                     static_cast<float>(slot.base_color.g) / 255.0f,
                                     static_cast<float>(slot.base_color.b) / 255.0f};
          material.base_color_set = true;
          material.metallic = slot.metallic;
          material.roughness = slot.roughness;
          material.opacity = slot.opacity;
          material.specular = slot.metallic > 0.0f ? 1.0f : 0.0f;
          material.emission = slot.emissive;
          if (slot.base_color_texture >= 0 &&
              static_cast<std::size_t>(slot.base_color_texture) < scene->textures.size()) {
            const auto& texture = scene->textures[static_cast<std::size_t>(slot.base_color_texture)];
            if (texture.is_valid()) {
              (*scene3d)->set_texture(texture.rgba, texture.width, texture.height);
              (*scene3d)->set_texture_transform(slot.uv_scale_u, slot.uv_scale_v,
                                                slot.uv_offset_u, slot.uv_offset_v, false);
              texture_bound = true;
            }
          }
        }
        (*scene3d)->set_material(material);
        (*scene3d)->draw_mesh(mesh, node.transform);
        if (texture_bound) {
          (*scene3d)->set_texture({}, 0, 0);
          (*scene3d)->set_texture_transform(1.0f, 1.0f, 0.0f, 0.0f, false);
        }
      }
    }

    st::raster::Canvas canvas(kSize, kSize);
    (*scene3d)->end_frame(canvas, st::math::Rect{0.0f, 0.0f, static_cast<float>(kSize),
                                                 static_cast<float>(kSize)});
    (*scene3d)->flush();

    // 统计非背景像素（证明真的画出了东西）
    int inked = 0;
    const Color background = canvas.pixel_at(0, 0);
    for (int y = 0; y < kSize; ++y) {
      for (int x = 0; x < kSize; ++x) {
        const Color pixel = canvas.pixel_at(x, y);
        const int dr = static_cast<int>(pixel.r) - static_cast<int>(background.r);
        const int dg = static_cast<int>(pixel.g) - static_cast<int>(background.g);
        const int db = static_cast<int>(pixel.b) - static_cast<int>(background.b);
        if (dr * dr + dg * dg + db * db > 400) ++inked;
      }
    }

    // 文件名：取素材主名
    std::string base = path;
    for (const char* suffix : {".gltf", ".glb"}) {
      if (base.size() > std::strlen(suffix) &&
          base.compare(base.size() - std::strlen(suffix), std::strlen(suffix), suffix) == 0) {
        base.resize(base.size() - std::strlen(suffix));
      }
    }
    const auto slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);

    st::codec::PngImage image{};
    image.width = kSize;
    image.height = kSize;
    image.rgba = canvas_to_rgba(canvas);
    const std::string png_path = st::fs::join(out_dir, base + ".png");
    if (const auto written = st::codec::png_write_file(png_path, image); !written.has_value()) {
      st::log::out("  [写图失败] {}\n", written.error().message);
      continue;
    }
    st::log::out("  [渲染] {} 个三角形 · {} 个非背景像素 · → {}\n", drawn_triangles, inked,
                 png_path);
    ++rendered;
  }
  st::log::out("\n共渲染 {} 份素材\n", rendered);
  return 0;
}

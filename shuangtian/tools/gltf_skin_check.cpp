/// 验证 **glTF 骨骼蒙皮 + 动画** 链路在本机可用 —— 高质量角色的前提。
///
/// ## 与 `tools/skin_probe.cpp` 的分工
///
/// 那个探针证明"LBS 形变本身是对的"（程序化软柱、两列对照）。
/// 本探针证明的是**外部 glTF 资产能走通同一条链**：
/// 读文件 → 拿到骨架与动画 → 按时间求姿态 → 算出蒙皮矩阵 → 变形网格 → 渲出来。
///
/// 二者缺一不可：前者说"引擎会蒙皮"，后者说"读进来的模型真能被驱动"。
///
/// ## 逐环打点
///
/// 这四步任一失败，最终表现都是"角色不出现"或"角色是根柱子"，
/// 从画面上无法区分是哪一环断的。所以每步都打印可复算的数字。
#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "st/codec/png.hpp"
#include "st/core/entry.hpp"
#include "st/math/geometry.hpp"
#include "st/math/matrix.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/gltf.hpp"
#include "st/raster/mesh.hpp"
#include "st/raster/scene3d.hpp"
#include "st/raster/skin.hpp"

namespace {

using st::math::Color;
using st::math::Mat4;
using st::math::Rect;
using st::math::Vec3;

auto run(int argc, char** argv) -> int {
  const std::string model_path =
      argc > 1 ? argv[1] : std::string("assets/models/gltf/CesiumMan.glb");
  const std::string out = argc > 2 ? argv[2] : std::string("build/gltf-skin-check.png");

  // ① 读文件
  auto loaded = st::raster::gltf_read_file(model_path);
  if (!loaded.has_value()) {
    std::fprintf(stderr, "读取失败：%s\n", loaded.error().message.c_str());
    return 1;
  }
  const st::raster::GltfScene& scene = *loaded;
  std::printf("① 读取 OK\n");
  std::printf("   meshes=%zu  materials=%zu  textures=%zu  nodes=%zu  animations=%zu\n",
              scene.meshes.size(), scene.materials.size(), scene.textures.size(),
              scene.nodes.size(), scene.animations.size());
  std::printf("   generator=%s\n", scene.generator.c_str());

  // ② 节点树（关节的世界位置在这些节点的 `transform` 里，不在 `inverse_bind`）
  std::printf("   节点（名 -> transform 平移 + 挂的网格）:\n");
  for (std::size_t i = 0; i < scene.nodes.size(); ++i) {
    const auto& node = scene.nodes[i];
    std::printf("     [%2zu] %-34s t=(%.3f, %.3f, %.3f) meshes=%zu\n", i, node.name.c_str(),
                static_cast<double>(node.transform.m[12]),
                static_cast<double>(node.transform.m[13]),
                static_cast<double>(node.transform.m[14]), node.meshes.size());
  }

  // ② 骨架与蒙皮权重
  (void)scene.has_skin_weights;
  bool any_skinned = false;
  std::size_t total_verts = 0;
  for (const auto& mesh : scene.meshes) {
    total_verts += mesh.vertex_count();
    if (mesh.has_skins()) any_skinned = true;
  }
  std::printf("② 网格：总顶点 %zu，带蒙皮 %s\n", total_verts, any_skinned ? "是" : "否");
  if (scene.skeleton.has_value()) {
    std::printf("   骨架：%zu 个关节\n", scene.skeleton->joint_count());
    for (std::size_t i = 0; i < scene.skeleton->joints.size(); ++i) {
      const auto bind = scene.skeleton->bind_matrix_at(i);
      // 绑定矩阵的逆变换给出该骨骼的**绑定世界位置**
      // 列主序 `m[column*4 + row]`：平移在第 3 列的 0/1/2 行。
      std::printf("     [%2zu] %-32s bind_pos=(%.3f, %.3f, %.3f)\n", i,
                  scene.skeleton->joints[i].c_str(), static_cast<double>(bind.m[12]),
                  static_cast<double>(bind.m[13]), static_cast<double>(bind.m[14]));
    }
  } else {
    std::printf("   骨架：无（模型未蒙皮）\n");
  }

  // ③ 渲染：绑定姿势
  constexpr int kCell = 620;
  st::raster::Canvas canvas(kCell * 2, kCell, 1.0f);
  canvas.clear(Color{0x18, 0x1C, 0x26, 0xFF});

  const auto render_into = [&](int column) -> bool {
    auto created = st::raster::Scene3D::create(kCell, kCell);
    if (!created.has_value()) {
      std::fprintf(stderr, "Scene3D 创建失败：%s\n", created.error().message.c_str());
      return false;
    }
    auto& s3 = **created;
    s3.begin_frame(Color{0x18, 0x1C, 0x26, 0xFF});

    st::raster::Camera camera{};
    camera.eye = Vec3{0.0f, 1.05f, 3.0f};
    camera.target = Vec3{0.0f, 0.90f, 0.0f};
    camera.fov_y_degrees = 34.0f;
    s3.set_camera(camera);

    st::raster::Lighting lighting{};
    lighting.light_from = Vec3{0.45f, 0.85f, 0.65f};
    lighting.ambient = 0.55f;
    lighting.diffuse = 0.85f;
    lighting.rim = 0.45f;
    s3.set_lighting(lighting);

    // 按节点 + 材质 + 贴图画（这是"高质量"的关键：材质与贴图都来自 glTF）
    for (const auto& node : scene.nodes) {
      for (const std::size_t mesh_index : node.meshes) {
        if (mesh_index >= scene.meshes.size()) continue;
        if (node.material >= 0 &&
            static_cast<std::size_t>(node.material) < scene.materials.size()) {
          const auto& mat = scene.materials[static_cast<std::size_t>(node.material)];
          // 贴图（若有）
          if (mat.base_color_texture >= 0 &&
              static_cast<std::size_t>(mat.base_color_texture) < scene.textures.size()) {
            const auto& tex = scene.textures[static_cast<std::size_t>(mat.base_color_texture)];
            if (tex.is_valid()) {
              s3.set_texture(tex.rgba, tex.width, tex.height);
            }
          }
        }
        s3.draw_mesh(scene.meshes[mesh_index], node.transform);
      }
    }
    s3.end_frame(canvas, Rect{static_cast<float>(column * kCell), 0.0f,
                              static_cast<float>(kCell), static_cast<float>(kCell)});
    return true;
  };

  if (!render_into(0)) return 4;
  std::printf("③ 渲染 OK（绑定姿势，含贴图）\n");

  // ④ 导出
  st::codec::PngImage image;
  image.width = static_cast<std::uint32_t>(canvas.physical_width());
  image.height = static_cast<std::uint32_t>(canvas.physical_height());
  image.rgba.resize(static_cast<std::size_t>(image.width) * image.height * 4);
  const auto pixels = canvas.pixels();
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    const std::uint32_t p = pixels[i];
    const auto a = static_cast<std::uint8_t>(p & 0xFFU);
    auto r = static_cast<std::uint8_t>((p >> 24) & 0xFFU);
    auto g = static_cast<std::uint8_t>((p >> 16) & 0xFFU);
    auto b = static_cast<std::uint8_t>((p >> 8) & 0xFFU);
    if (a != 0U && a != 255U) {
      r = static_cast<std::uint8_t>(std::min(255U, static_cast<unsigned>(r) * 255U / a));
      g = static_cast<std::uint8_t>(std::min(255U, static_cast<unsigned>(g) * 255U / a));
      b = static_cast<std::uint8_t>(std::min(255U, static_cast<unsigned>(b) * 255U / a));
    }
    image.rgba[i * 4 + 0] = r;
    image.rgba[i * 4 + 1] = g;
    image.rgba[i * 4 + 2] = b;
    image.rgba[i * 4 + 3] = a;
  }
  if (!st::codec::png_write_file(out, image).has_value()) {
    std::fprintf(stderr, "写 PNG 失败\n");
    return 5;
  }
  std::printf("→ %s\n", out.c_str());
  return 0;
}

}  // namespace

ST_MAIN(run)

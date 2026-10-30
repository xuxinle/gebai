// 拾取探针：从相机向场景打一圈射线，把**命中点**画成亮块叠加在渲染图上。
//
// ## 这个探针回答什么
//
// 单元测试钉的是"射线 × 立方体在某处的 t 等于 4.5"。它无法回答：
// - 命中点**是否落在物体表面上**（而不是漂在前后）；
// - 多条射线（不同屏幕位置）是否各自命中**正确的那一个**零件；
// - 被遮挡的射线是否正确地停在**前面**那个物体上。
//
// 所以这里做的事：渲染场景 → 在屏幕上打 21 条射线 → 按命中点重投影回屏幕
// → 把命中像素染成高亮色。**命中的亮点必须落在物体轮廓里**，
// 这是"拾取与渲染用的是同一套世界矩阵"的直接证据。
//
// 判据（可复算）：
// 1. 21 条射线里命中数在合理区间（中间几条必中、最外侧可能打空）；
// 2. 每个命中点重投影回屏幕后，应落在**原射线**的屏幕位置附近（< 2 像素）；
// 3. 命中点的世界坐标必须落在对应物体的世界 AABB 内（自检宽相/窄相一致）。
#include <cmath>
#include <cstdio>
#include <format>
#include <string>
#include <vector>

#include "st/codec/png.hpp"
#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/math/collision.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/mesh_ops.hpp"
#include "st/raster/pick.hpp"
#include "st/raster/primitives.hpp"
#include "st/raster/scene3d.hpp"
#include "st/raster/scene3d_script.hpp"
#include "st/raster/scene_graph.hpp"

namespace {

using st::math::Color;
using st::math::Mat4;
using st::math::Vec3;

constexpr int kWidth = 320;
constexpr int kHeight = 240;

/// 场景：三个不同形状 + 一个组合体（父子链），用来验证"不同零件各自命中"。
const char* kSceneJson = R"json({
  "camera": { "eye": [1.4, 1.1, 3.2], "target": [0.0, 0.35, 0.0], "fov": 42 },
  "lighting": { "key_direction": [0.4, 0.8, 0.6], "ambient": 0.35, "diffuse": 0.75 },
  "objects": [
    { "name": "ground", "shape": "plane", "radius": 3.0, "depth": 3.0,
      "color": "#33383f", "ground": true },
    { "name": "box", "shape": "cube", "radius": 0.28, "depth": 0.56, "height": 0.56,
      "color": "#d4b07a", "translate": [-0.75, 0.28, 0.0] },
    { "name": "sphere", "shape": "uv_sphere", "radius": 0.3, "color": "#7aa8d4",
      "translate": [0.0, 0.3, 0.0] },
    { "name": "rig", "group": true, "translate": [0.75, 0.1, 0.0], "rotate": [0, 0.5, 0],
      "children": [
        { "name": "arm", "shape": "capsule", "radius": 0.08, "depth": 0.5,
          "color": "#c98a8a", "translate": [0.0, 0.25, 0.0] }
      ] }
  ]
})json";

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

/// 屏幕像素 → 世界射线（与渲染器同一套视图/投影，从 `Camera` 反推）。
auto screen_ray(const st::raster::Camera& camera, float screen_x, float screen_y, int width,
                int height) -> st::math::Ray {
  // 视图：右手系，相机看 -Z
  const Vec3 forward = (camera.target - camera.eye).normalized();
  const Vec3 right = forward.cross(camera.up).normalized();
  const Vec3 up = right.cross(forward);
  // NDC（-1..1），y 轴翻转（屏幕 y 向下）
  const float ndc_x = (screen_x / static_cast<float>(width)) * 2.0f - 1.0f;
  const float ndc_y = 1.0f - (screen_y / static_cast<float>(height)) * 2.0f;
  const float aspect = static_cast<float>(width) / static_cast<float>(height);
  const float tan_half = std::tan(camera.fov_y_degrees * 0.5f * 3.14159265f / 180.0f);
  const Vec3 direction =
      (forward + right * (ndc_x * tan_half * aspect) + up * (ndc_y * tan_half)).normalized();
  st::math::Ray ray{};
  ray.origin = camera.eye;
  ray.direction = direction;
  return ray;
}

/// 世界点 → 屏幕像素（投影回去，用于验证命中点在正确位置）。
auto world_to_screen(const st::raster::Camera& camera, Vec3 point, int width, int height)
    -> std::pair<float, float> {
  const Vec3 forward = (camera.target - camera.eye).normalized();
  const Vec3 right = forward.cross(camera.up).normalized();
  const Vec3 up = right.cross(forward);
  const Vec3 delta = point - camera.eye;
  const float depth = delta.dot(forward);
  if (depth <= 1.0e-4f) return {-1.0f, -1.0f};
  const float aspect = static_cast<float>(width) / static_cast<float>(height);
  const float tan_half = std::tan(camera.fov_y_degrees * 0.5f * 3.14159265f / 180.0f);
  const float ndc_x = delta.dot(right) / (depth * tan_half * aspect);
  const float ndc_y = delta.dot(up) / (depth * tan_half);
  return {(ndc_x * 0.5f + 0.5f) * static_cast<float>(width),
          (1.0f - (ndc_y * 0.5f + 0.5f)) * static_cast<float>(height)};
}

void put_pixel(std::vector<std::uint8_t>& pixels, int width, int x, int y, std::uint8_t r,
               std::uint8_t g, std::uint8_t b) {
  if (x < 0 || y < 0) return;
  const std::size_t at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                          static_cast<std::size_t>(x)) * 4U;
  if (at + 3U >= pixels.size()) return;
  pixels[at + 0] = r;
  pixels[at + 1] = g;
  pixels[at + 2] = b;
  pixels[at + 3] = 255;
}

/// 画一个 3×3 的标记（比 1 像素更耐缩放，便于看图）。
void mark(std::vector<std::uint8_t>& pixels, int width, int x, int y, std::uint8_t r,
          std::uint8_t g, std::uint8_t b) {
  for (int dy = -1; dy <= 1; ++dy) {
    for (int dx = -1; dx <= 1; ++dx) put_pixel(pixels, width, x + dx, y + dy, r, g, b);
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::string out_dir = argc > 1 ? argv[1] : "out/pick";
  (void)st::fs::create_directories(out_dir);

  auto spec = st::raster::parse_scene_spec_text(kSceneJson);
  if (!spec) {
    st::log::err("解析失败：{}\n", spec.error().message);
    return 1;
  }

  // 1) 渲染
  auto scene = st::raster::Scene3D::create(kWidth, kHeight);
  if (!scene.has_value()) {
    st::log::err("无法创建场景：{}\n", scene.error().message);
    return 1;
  }
  st::raster::Canvas canvas(kWidth, kHeight);
  if (const auto status = st::raster::render_scene_into(*spec, **scene, canvas,
                                                        Color{18, 20, 26, 255});
      !status) {
    st::log::err("渲染失败：{}\n", status.error().message);
    return 1;
  }
  std::vector<std::uint8_t> pixels = canvas_to_rgba(canvas);

  // 2) 造拾取物体（与渲染用同一套网格与世界矩阵）
  //
  // 关键：这里用 `resolve` 算世界矩阵、用 `build_mesh` 算网格——
  // 与 `render_scene_into` 内部是同两条路径。若两者漂移（比如矩阵乘序不同），
  // 命中点就会偏离物体轮廓，而图上会立刻看出来。
  std::vector<st::raster::SceneNode> roots;
  for (const auto& object : spec->objects) {
    st::raster::SceneNode node{};
    node.name = object.name;
    node.has_geometry = !object.group;
    node.local.translation = object.translate;
    node.local.rotation = object.rotate;
    node.local.scale = object.scale;
    for (const auto& child : object.children) {
      st::raster::SceneNode child_node{};
      child_node.name = child.name;
      child_node.has_geometry = !child.group;
      child_node.local.translation = child.translate;
      child_node.local.rotation = child.rotate;
      node.children.push_back(child_node);
    }
    roots.push_back(node);
  }
  const auto tree = st::raster::resolve(roots);
  if (!tree.has_value()) {
    st::log::err("场景树求解失败：{}\n", tree.error().message);
    return 1;
  }

  // 名字 → 网格（只给可画的节点建网格）
  std::vector<st::raster::Mesh> meshes;
  std::vector<std::string> mesh_names;
  for (const auto& object : spec->objects) {
    if (object.group) continue;
    const auto mesh = st::raster::build_mesh(object);
    if (!mesh) continue;
    meshes.push_back(*mesh);
    mesh_names.push_back(object.name);
    for (const auto& child : object.children) {
      if (child.group) continue;
      const auto child_mesh = st::raster::build_mesh(child);
      if (!child_mesh) continue;
      meshes.push_back(*child_mesh);
      mesh_names.push_back(child.name);
    }
  }

  std::vector<st::raster::PickObject> pick_objects;
  for (std::size_t i = 0; i < mesh_names.size(); ++i) {
    const auto* node = tree->find_by_name(mesh_names[i]);
    if (node == nullptr) continue;
    // 地面不参与拾取（它总会命中，会掩盖其它物体的命中情况）
    if (mesh_names[i] == "ground") continue;
    pick_objects.push_back(
        st::raster::make_pick_object(mesh_names[i], meshes[i], node->world));
  }

  // 3) 在屏幕中下部打一排射线（覆盖三个物体）
  std::string report;
  int tested = 0;
  int hit_count = 0;
  int reproject_bad = 0;
  int bounds_bad = 0;
  for (int index = 0; index < 21; ++index) {
    const float screen_x = 30.0f + static_cast<float>(index) * 13.0f;
    // ## 扫描线要**穿过物体中部**，不能贴着下沿
    //
    // 实测过 0.55 / 0.62 两档都只擦到物体底边（图上黄点全在轮廓下沿），
    // 于是"命中数"这条判据在测"取景摆得够不够低"，而不是拾取。
    // 三个物体在画面里大约占 y∈[0.42, 0.72]，取 0.52 正好穿过中部。
    const float screen_y = static_cast<float>(kHeight) * 0.52f;
    const auto ray = screen_ray(spec->camera, screen_x, screen_y, kWidth, kHeight);
    ++tested;
    const auto hit = st::raster::raycast(pick_objects, ray);
    if (!hit.has_value()) {
      report += std::format("ray[{:2}] x={:5.1f}  miss\n", index,
                            static_cast<double>(screen_x));
      continue;
    }
    ++hit_count;

    // 判据 2：命中点重投影回屏幕，必须落在原射线位置附近
    const auto [px, py] = world_to_screen(spec->camera, hit->point, kWidth, kHeight);
    const float error = std::sqrt((px - screen_x) * (px - screen_x) + (py - screen_y) * (py - screen_y));
    if (error > 2.0f) ++reproject_bad;

    // 判据 3：命中点必须落在该物体世界 AABB 内（宽相与窄相一致）
    bool inside = false;
    for (const auto& object : pick_objects) {
      if (object.name != hit->name) continue;
      const auto& box = object.world_bounds;
      inside = hit->point.x >= box.min.x - 1.0e-3f && hit->point.x <= box.max.x + 1.0e-3f &&
               hit->point.y >= box.min.y - 1.0e-3f && hit->point.y <= box.max.y + 1.0e-3f &&
               hit->point.z >= box.min.z - 1.0e-3f && hit->point.z <= box.max.z + 1.0e-3f;
    }
    if (!inside) ++bounds_bad;

    report += std::format("ray[{:2}] x={:5.1f}  hit={:<7} t={:5.2f}  reproject_err={:.2f}px\n",
                          index, static_cast<double>(screen_x), hit->name,
                          static_cast<double>(hit->distance), static_cast<double>(error));
    // 画标记：命中点重投影位置
    const int mx = static_cast<int>(std::lround(px));
    const int my = static_cast<int>(std::lround(py));
    mark(pixels, kWidth, mx, my, 255, 220, 60);
    st::log::out("ray[{:2}] x={:5.1f}  hit={:<7} t={:5.2f}  err={:.2f}px\n", index,
                 static_cast<double>(screen_x), hit->name,
                 static_cast<double>(hit->distance), static_cast<double>(error));
  }

  // 门槛取"至少命中三成"：扫描线必然有一段落在物体之外（画面两侧是空的），
  // 要求更高的比例等于在测取景，而不是拾取。
  const bool hits_reasonable = hit_count >= tested / 3 && hit_count <= tested;
  const bool reprojects = reproject_bad == 0;
  const bool bounds_consistent = bounds_bad == 0;
  report += std::format("\n判据：命中数={}/{}（>=三成）={} 重投影误差全 <=2px={} AABB 一致={}\n",
                        hit_count, tested, hits_reasonable ? "ok" : "FAIL",
                        reprojects ? "ok" : "FAIL", bounds_consistent ? "ok" : "FAIL");
  st::log::out("\n{}", report);

  st::codec::PngImage image{};
  image.width = kWidth;
  image.height = kHeight;
  image.rgba = std::move(pixels);
  const std::string png_path = st::fs::join(out_dir, "picking.png");
  if (const auto written = st::codec::png_write_file(png_path, image); !written.has_value()) {
    st::log::err("写 PNG 失败：{}\n", written.error().message);
    return 1;
  }
  (void)st::fs::write_text(st::fs::join(out_dir, "README.txt"),
                           "拾取探针：21 条屏幕射线，黄点 = 命中点重投影回屏幕的位置。\n"
                           "黄点必须落在物体轮廓里（= 拾取与渲染共用同一套世界矩阵）。\n\n" +
                               report);
  st::log::out("产物：{}\n", png_path);
  return (hits_reasonable && reprojects && bounds_consistent) ? 0 : 2;
}

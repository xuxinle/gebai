/// 真实素材回归测试：用 Khronos glTF 官方样例（CC0/CC-BY）验证生态兼容。
///
/// ## 为什么要有"真素材"测试（自造素材覆盖不到什么）
///
/// 自造素材是我理解中的 glTF——**我按自己的理解构造，也按自己的理解验证**，
/// 于是双方一致的错误永远不会被发现。真实素材是别人按规范导出的，
/// 会带上我没预料到的组合。这一轮就靠真素材抓到了两个缺陷：
///
/// 1. **近裁剪面固定 0.1**：Avocado / Corset 半径 0.03（3cm），整个模型落在
///    近裁剪面内，渲染出 **0 个像素**而没有任何报错。自造的米级模型永远碰不到。
/// 2. **相机距离凭常数**：`radius * 3.4` 对半径 1.0 合适，对 0.5 就把模型顶出画框。
///
/// ## 素材不存在时跳过（而不是失败）
///
/// 这些文件来自网络，不该成为 CI 的硬依赖（离线环境跑不了）。
/// 但**存在时就必须真的验证**——所以 `st skip`（显式跳过，报告里可见），
/// 而不是静默 `return`（那会让"没测"和"测过"看起来一样）。
#include <cmath>
#include <string>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/raster/mesh_ops.hpp"
#include "st/raster/gltf.hpp"
#include "st/test/test.hpp"

namespace {

/// 素材目录（由探测脚本下载到 repo 外的临时位置）。
auto asset_dir() -> std::string {
  if (const auto env = st::fs::read_env("ST_REAL_ASSETS"); env.has_value() && !env->empty()) {
    return *env;
  }
  return "tmp/real";
}

auto asset_path(const std::string& name) -> std::string {
  return st::fs::join(asset_dir(), name);
}

/// 收集一份 glTF 场景的总包围盒（世界空间）。
auto total_bounds(const st::raster::GltfScene& scene) -> st::raster::Bounds {
  st::raster::Bounds total = st::raster::Bounds::empty();
  for (const auto& node : scene.nodes) {
    for (const std::size_t mesh_index : node.meshes) {
      if (mesh_index >= scene.meshes.size()) continue;
      const auto world = st::raster::transform(scene.meshes[mesh_index], node.transform);
      const auto box = st::raster::bounds(world);
      if (!box.is_empty()) total = total.union_with(box);
    }
  }
  return total;
}

}  // namespace

ST_TEST(real_asset_box_without_texture_loads) {
  const std::string path = asset_path("box.glb");
  if (!st::fs::exists(path)) {
    ST_SKIP("真实素材不存在（设置 ST_REAL_ASSETS 或运行 tools/fetch_assets.ps1）");
  }
  const auto scene = st::raster::gltf_read_file(path);
  ST_REQUIRE(scene.has_value());
  ST_CHECK_EQ(scene->meshes.size(), 1U);
  ST_CHECK_EQ(scene->meshes.front().indices.size() / 3U, 12U);   // 立方体 = 12 三角形
  ST_CHECK(!scene->materials.empty());
}

ST_TEST(real_asset_box_textured_decodes_texture) {
  const std::string path = asset_path("boxtextured.glb");
  if (!st::fs::exists(path)) {
    ST_SKIP("真实素材不存在");
  }
  const auto scene = st::raster::gltf_read_file(path);
  ST_REQUIRE(scene.has_value());
  // 贴图必须真的解出像素（不是只记了个引用）
  ST_REQUIRE(!scene->textures.empty());
  const auto& texture = scene->textures.front();
  ST_CHECK(texture.is_valid());
  ST_CHECK(texture.width > 0);
  ST_CHECK(texture.height > 0);
  ST_CHECK_EQ(texture.rgba.size(),
              static_cast<std::size_t>(texture.width) * static_cast<std::size_t>(texture.height) *
                  4U);
  // 材质必须引用到它
  ST_REQUIRE(!scene->materials.empty());
  ST_CHECK(scene->materials.front().base_color_texture >= 0);
}

ST_TEST(real_asset_helmet_large_textured_model) {
  // DamagedHelmet：3.7MB、15452 三角形、**5 张贴图**。
  // 这是"能不能吃下真实生产级素材"的代表。
  const std::string path = asset_path("helmet.glb");
  if (!st::fs::exists(path)) {
    ST_SKIP("真实素材不存在");
  }
  const auto scene = st::raster::gltf_read_file(path);
  ST_REQUIRE(scene.has_value());
  ST_REQUIRE(!scene->meshes.empty());
  const std::size_t triangles = scene->meshes.front().indices.size() / 3U;
  st::log::out("[real] helmet 三角形={} 贴图={}\n", triangles, scene->textures.size());
  ST_CHECK(triangles > 15000U);
  ST_CHECK(scene->textures.size() >= 4U);   // albedo/normal/metallicRoughness/AO/emissive
  // 每张贴图都要解出有效像素
  int valid = 0;
  for (const auto& texture : scene->textures) {
    if (texture.is_valid()) ++valid;
  }
  ST_CHECK_EQ(valid, static_cast<int>(scene->textures.size()));
  // UV 必须存在（否则贴图贴不上）
  ST_CHECK(scene->meshes.front().has_uvs());
}

ST_TEST(real_asset_rigged_has_skeleton_and_animation) {
  // RiggedSimple：骨架 2 关节 + 1 段动画。验证上一轮的骨架/动画在真实素材上成立。
  const std::string path = asset_path("rigged.glb");
  if (!st::fs::exists(path)) {
    ST_SKIP("真实素材不存在");
  }
  const auto scene = st::raster::gltf_read_file(path);
  ST_REQUIRE(scene.has_value());
  ST_REQUIRE(scene->skeleton.has_value());
  ST_CHECK_EQ(scene->skeleton->joint_count(), 2U);
  ST_CHECK(!scene->animations.empty());
  // 蒙皮权重必须真的读到（不是只有骨架没权重）
  bool skinned = false;
  for (const auto& mesh : scene->meshes) {
    if (mesh.has_skins()) skinned = true;
  }
  ST_CHECK(skinned);
}

ST_TEST(real_asset_draco_is_rejected_with_actionable_message) {
  // ## 真实 Draco 素材必须被**拒绝并指路**
  //
  // 这是本轮最重要的行为：不装解码器时，静默读出空模型是最糟的失败形态。
  const std::string path = asset_path("Box.gltf");
  if (!st::fs::exists(path)) {
    ST_SKIP("真实素材不存在");
  }
  const auto scene = st::raster::gltf_read_file(path);
  ST_REQUIRE(!scene.has_value());
  ST_CHECK(scene.error().code == st::ErrorCode::Unsupported);
  const std::string message = scene.error().message;
  ST_CHECK(message.find("KHR_draco_mesh_compression") != std::string::npos);
  ST_CHECK(message.find("Blender") != std::string::npos);   // 解决动作
}

ST_TEST(real_asset_scales_span_orders_of_magnitude) {
  // ## 这条钉住"近裁剪面固定 0.1"那个缺陷
  //
  // 生态素材的单位差好几个数量级：
  //   Avocado / Corset 半径 ≈ 0.03（3cm，道具）
  //   AntiqueCamera 半径 ≈ 3.6（3.6m，含三脚架）
  //   RiggedSimple 半径 ≈ 4.6
  //
  // 固定的 near=0.1 会把 3cm 的模型整个裁掉（渲染 0 像素，且无任何报错）。
  // 所以调用方必须**按模型尺度自适应** near/far——这里验证前提成立：
  // 素材的尺度确实跨度极大，任何固定值都会在某一端失效。
  const std::string small_path = asset_path("Avocado.glb");
  const std::string large_path = asset_path("AntiqueCamera.glb");
  if (!st::fs::exists(small_path) || !st::fs::exists(large_path)) {
    ST_SKIP("真实素材不存在");
  }
  const auto small_scene = st::raster::gltf_read_file(small_path);
  const auto large_scene = st::raster::gltf_read_file(large_path);
  ST_REQUIRE(small_scene.has_value());
  ST_REQUIRE(large_scene.has_value());
  const auto small_box = total_bounds(*small_scene);
  const auto large_box = total_bounds(*large_scene);
  ST_REQUIRE(!small_box.is_empty());
  ST_REQUIRE(!large_box.is_empty());
  const float small_radius = std::max({small_box.size().x, small_box.size().y, small_box.size().z}) * 0.5f;
  const float large_radius = std::max({large_box.size().x, large_box.size().y, large_box.size().z}) * 0.5f;
  st::log::out("[real] 小模型半径 {:.4f} · 大模型半径 {:.3f} · 比值 {:.0f}x\n",
               static_cast<double>(small_radius), static_cast<double>(large_radius),
               static_cast<double>(large_radius / std::max(small_radius, 1.0e-6f)));
  // 跨度必须足够大，才证明"固定 near/far 不可行"这个论断成立
  ST_CHECK(large_radius / std::max(small_radius, 1.0e-6f) > 50.0f);
  // 小模型半径必须小于默认近裁剪面 0.1（= 固定 near 会裁掉它）
  ST_CHECK(small_radius < 0.1f);
}

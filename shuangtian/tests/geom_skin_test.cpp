/// 蒙皮（LBS）的回归测试：逆绑定矩阵、权重限制、形变、glTF 往返、端到端。
///
/// ## 判据为什么必须算得出来
///
/// 蒙皮的错误几乎全是**数值型**的，而它们画出来"还是个模型"：
/// - 逆绑定矩阵写错 → 整个模型缩成一点/飞到天边；
/// - 权重没归一化 → 角色整体缩小（朝向原点收缩）；
/// - 形变时没除总权重 → 同上，但只在权重和 < 1 的顶点上；
/// - 关节索引越界 → 那一块顶点"钉死不动"（因为坏索引被当无影响）。
///
/// 所以每一条都钉具体坐标或计数，不用"看起来动了吗"。
#include <cmath>
#include <string>
#include <vector>

#include "st/core/log.hpp"
#include "st/math/quaternion.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/gltf.hpp"
#include "st/raster/primitives.hpp"
#include "st/raster/scene3d_script.hpp"
#include "st/raster/scene_graph.hpp"
#include "st/raster/skin.hpp"
#include "st/test/test.hpp"

namespace {

using st::math::Mat4;
using st::math::Vec3;
using st::raster::Mesh;
using st::raster::SceneNode;
using st::raster::Skeleton;
namespace prim = st::raster::primitives;

auto inked(const st::raster::Canvas& canvas) -> int {
  const st::math::Color background = canvas.pixel_at(0, 0);
  int count = 0;
  for (int y = 0; y < canvas.physical_height(); ++y) {
    for (int x = 0; x < canvas.physical_width(); ++x) {
      const st::math::Color pixel = canvas.pixel_at(x, y);
      const int dr = static_cast<int>(pixel.r) - static_cast<int>(background.r);
      const int dg = static_cast<int>(pixel.g) - static_cast<int>(background.g);
      const int db = static_cast<int>(pixel.b) - static_cast<int>(background.b);
      if (dr * dr + dg * dg + db * db > 400) ++count;
    }
  }
  return count;
}

auto vertex_position(const Mesh& mesh, std::size_t index) -> Vec3 {
  const auto v = mesh[index];
  return Vec3{v[0], v[1], v[2]};
}

/// 造一个只有两个关节的骨架：`root`（原点）与 `tip`（沿 X 偏移）。
auto make_skeleton(float tip_x) -> Skeleton {
  Skeleton skeleton{};
  skeleton.joints = {"root", "tip"};
  // 绑定姿态：root 在原点、tip 在 (tip_x,0,0) → 逆绑定矩阵是各自世界矩阵的逆
  const Mat4 root_world = Mat4::identity();
  const Mat4 tip_world = Mat4::translation(Vec3{tip_x, 0.0f, 0.0f});
  skeleton.inverse_bind = {st::raster::inverse_bind_from_world(root_world),
                           st::raster::inverse_bind_from_world(tip_world)};
  return skeleton;
}

/// 造一副两节点的树（root + tip 子节点）。
auto make_tree(float tip_x) -> std::vector<SceneNode> {
  SceneNode root{};
  root.name = "root";
  root.has_geometry = false;
  SceneNode tip{};
  tip.name = "tip";
  tip.has_geometry = false;
  tip.local.translation = Vec3{tip_x, 0.0f, 0.0f};
  root.children.push_back(tip);
  return {root};
}

}  // namespace

ST_TEST(skin_inverse_bind_round_trips) {
  // 逆绑定矩阵 × 原矩阵 = 单位阵。这条是所有蒙皮计算的地基：
  // 写错一个符号，模型就会缩成一点或飞到天边，而"画面上确实有东西"。
  const Mat4 cases[] = {
      Mat4::translation(Vec3{1.0f, -2.0f, 3.5f}),
      Mat4::rotation(Vec3{0.0f, 1.0f, 0.0f}, 0.7f),
      Mat4::translation(Vec3{0.5f, 0.0f, -1.0f}) *
          Mat4::rotation(Vec3{0.0f, 0.0f, 1.0f}, 1.1f) * Mat4::scaling(Vec3{2.0f, 2.0f, 2.0f}),
  };
  for (const Mat4& world : cases) {
    const Mat4 inverse = st::raster::inverse_bind_from_world(world);
    const Mat4 identity = world * inverse;
    for (int row = 0; row < 4; ++row) {
      for (int column = 0; column < 4; ++column) {
        const float expected = row == column ? 1.0f : 0.0f;
        ST_CHECK_NEAR(identity.value(row, column), expected, 1e-4f);
      }
    }
  }
  // 退化矩阵（行列式为 0）必须返回单位阵，而不是 NaN
  Mat4 degenerate = Mat4::identity();
  degenerate.set(0, 0, 0.0f);
  degenerate.set(1, 0, 0.0f);
  degenerate.set(2, 0, 0.0f);
  degenerate.set(0, 1, 0.0f);
  degenerate.set(1, 1, 0.0f);
  degenerate.set(2, 1, 0.0f);
  const Mat4 fallback = st::raster::inverse_bind_from_world(degenerate);
  for (int i = 0; i < 16; ++i) {
    ST_CHECK(std::isfinite(fallback.m[i]));
  }
}

ST_TEST(skin_weights_are_limited_and_normalized) {
  // 超过 4 个影响时取最大的四个并归一化
  const std::vector<std::pair<std::uint32_t, float>> many = {
      {0U, 0.05f}, {1U, 0.6f}, {2U, 0.2f}, {3U, 0.1f}, {4U, 0.05f}};
  const auto limited = st::raster::limit_to_four_influences(many);
  float total = 0.0f;
  for (std::size_t slot = 0; slot < 4U; ++slot) total += limited[slot * 2U + 1U];
  ST_CHECK_NEAR(total, 1.0f, 1e-5f);   // 归一化
  // 权重最大的那个（1 号）排第一；其归一化权重 = 0.6 / (0.6+0.2+0.1+0.05) = 0.6/0.95
  ST_CHECK_EQ(static_cast<int>(limited[0]), 1);
  ST_CHECK_NEAR(limited[1], 0.6f / 0.95f, 1e-4f);
  // ## 被丢弃的是 0.05 那一档，但"4 号与 0 号权重相同"时谁留谁丢取决于原顺序
  //
  // 实现用 `stable_sort` 降序，权重相同则保持输入顺序 → 0 号（排在 4 号之前）留下。
  // 判据钉"恰好留了四个、且都不小于最小的那个权重"，而不是钉具体哪个被丢
  //（那是在测实现的排序细节，不是在测"限制到四个"这个契约）。
  int kept = 0;
  for (std::size_t slot = 0; slot < 4U; ++slot) {
    if (limited[slot * 2U + 1U] > 0.0f) ++kept;
  }
  ST_CHECK_EQ(kept, 4);

  // 全零权重：输出全零（不受骨骼影响）
  const auto zero = st::raster::limit_to_four_influences({{0U, 0.0f}, {1U, 0.0f}});
  float zero_total = 0.0f;
  for (std::size_t slot = 0; slot < 4U; ++slot) zero_total += zero[slot * 2U + 1U];
  ST_CHECK_NEAR(zero_total, 0.0f, 1e-9f);
}

ST_TEST(skin_rigid_weight_moves_whole_mesh) {
  // 单关节、权重 1 的极端情形 = **刚体变换**。
  // 判据：所有顶点整���平移 (0,1,0)，与直接用矩阵变换一致。
  // 这条把"权重计算"排除在外，只验形变循环本身。
  prim::PrimitiveParams params{};
  params.radius = 0.3f;
  params.depth = 0.6f;
  params.subdivisions = 0;
  Mesh mesh = prim::make_ico_sphere(params);
  const std::size_t count = mesh.vertex_count();
  mesh.skins.assign(count * 8U, 0.0f);
  for (std::size_t i = 0; i < count; ++i) {
    mesh.skins[i * 8U] = 0.0f;       // 关节 0
    mesh.skins[i * 8U + 1U] = 1.0f;  // 权重 1
  }

  st::raster::SkinMatrices matrices{};
  matrices.matrices = {Mat4::translation(Vec3{0.0f, 1.0f, 0.0f})};
  const Mesh skinned = st::raster::skin_mesh(mesh, matrices);
  ST_CHECK_EQ(skinned.vertex_count(), count);
  for (std::size_t i = 0; i < count; ++i) {
    const Vec3 before = vertex_position(mesh, i);
    const Vec3 after = vertex_position(skinned, i);
    ST_CHECK_NEAR(after.x, before.x, 1e-4f);
    ST_CHECK_NEAR(after.y, before.y + 1.0f, 1e-4f);
    ST_CHECK_NEAR(after.z, before.z, 1e-4f);
  }
}

ST_TEST(skin_two_joints_blend_between) {
  // 两个关节、各 0.5 权重：顶点必须落在两种变换的**中点**。
  // 这条钉"混合"本身（单关节刚体测不到它）。
  Mesh mesh{};
  st::raster::push_vertex(mesh.vertices, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f},
              Vec3{1.0f, 1.0f, 1.0f});
  mesh.indices = {0U};
  mesh.skins = {0.0f, 0.5f, 1.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f};

  st::raster::SkinMatrices matrices{};
  matrices.matrices = {Mat4::translation(Vec3{0.0f, 2.0f, 0.0f}),
                       Mat4::translation(Vec3{0.0f, -2.0f, 0.0f})};
  const Mesh skinned = st::raster::skin_mesh(mesh, matrices);
  const Vec3 after = vertex_position(skinned, 0);
  // 两个平移的加权平均：y = 0.5*2 + 0.5*(-2) = 0（原本也是 0）
  ST_CHECK_NEAR(after.x, 1.0f, 1e-4f);
  ST_CHECK_NEAR(after.y, 0.0f, 1e-4f);
  // 法线必须仍是单位长（变换是纯平移，法线不变）
  const auto v = skinned[0];
  ST_CHECK_NEAR(std::sqrt(v[3] * v[3] + v[4] * v[4] + v[5] * v[5]), 1.0f, 1e-4f);
}

ST_TEST(skin_unnormalized_weights_do_not_shrink) {
  // ## 这条钉住"模型缩小"这个经典症状
  //
  // 权重和 = 0.5（调用方少写了一半）时，若不除总权重，顶点会被拉向原点
  //（因为位置 = 0.5 × 变换后的位置），观感是"角色整体缩小"。
  // 正确行为：除以总权重，位置与"权重 1"时一致。
  Mesh mesh{};
  st::raster::push_vertex(mesh.vertices, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f},
              Vec3{1.0f, 1.0f, 1.0f});
  mesh.indices = {0U};
  mesh.skins = {0.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};   // 和 = 0.5

  st::raster::SkinMatrices matrices{};
  matrices.matrices = {Mat4::translation(Vec3{2.0f, 0.0f, 0.0f})};
  const Mesh skinned = st::raster::skin_mesh(mesh, matrices);
  const Vec3 after = vertex_position(skinned, 0);
  // 期望：完整的平移 (3,0,0)，而不是被权重缩放成 (2.5,0,0) 或 (1.5,0,0)
  ST_CHECK_NEAR(after.x, 3.0f, 1e-4f);
}

ST_TEST(skin_compute_matrices_reports_missing_joint) {
  // 关节名在场景里找不到时必须报错并**指到那个关节**
  //（静默用单位阵的症状是"模型塌到原点"，很难定位）
  Skeleton skeleton = make_skeleton(1.0f);
  skeleton.joints[1] = "ghost";
  const auto tree = st::raster::resolve(make_tree(1.0f));
  ST_REQUIRE(tree.has_value());
  const auto computed = st::raster::compute_skin_matrices(skeleton, *tree);
  ST_REQUIRE(!computed.has_value());
  ST_CHECK(computed.error().code == st::ErrorCode::NotFound);
  ST_CHECK(computed.error().message.find("ghost") != std::string::npos);
}

ST_TEST(skin_bind_pose_is_identity_deformation) {
  // **绑定姿态下形变应为恒等**：世界矩阵 == 绑定矩阵 → 蒙皮矩阵 = I。
  // 这是最强的一条自检：如果它不成立，任何动画都会立刻错位。
  const Skeleton skeleton = make_skeleton(1.0f);
  const auto tree = st::raster::resolve(make_tree(1.0f));
  ST_REQUIRE(tree.has_value());
  const auto matrices = st::raster::compute_skin_matrices(skeleton, *tree);
  ST_REQUIRE(matrices.has_value());
  for (const Mat4& matrix : matrices->matrices) {
    for (int row = 0; row < 4; ++row) {
      for (int column = 0; column < 4; ++column) {
        const float expected = row == column ? 1.0f : 0.0f;
        ST_CHECK_NEAR(matrix.value(row, column), expected, 1e-4f);
      }
    }
  }
}

ST_TEST(skin_script_end_to_end_deforms_mesh) {
  // 端到端：网格 + 骨架 + `skin: true` → 骨骼旋转后模型**真的形变**。
  //
  // 判据用"渲染像素不同"+"顶点位置真的变了"两层：
  // 只看像素会被"模型整体挪走"骗过，只看顶点会被"形变了但没画出来"骗过。
  const std::string json = R"({
    "camera": { "eye": [0.0, 0.6, 3.2], "target": [0.0, 0.6, 0.0], "fov": 42 },
    "lighting": { "ambient": 0.4, "diffuse": 0.8 },
    "skeleton": { "joints": ["root", "upper"] },
    "objects": [
      { "name": "root", "group": true, "translate": [0.0, 0.3, 0.0] },
      { "name": "upper", "parent": "root", "translate": [0.0, 0.6, 0.0] },
      { "name": "body", "parent": "upper", "shape": "capsule", "radius": 0.14, "depth": 0.9,
        "color": "#d4b07a", "translate": [0.0, 0.35, 0.0], "skin": true }
    ],
    "animation": { "tracks": [
      { "node": "upper", "kind": "rotate", "keys": [
        { "t": 0.0, "value": [0, 0, 0] },
        { "t": 1.0, "value": [0, 0, 1.1] } ] } ] }
  })";
  auto spec = st::raster::parse_scene_spec_text(json);
  ST_REQUIRE(spec.has_value());
  ST_REQUIRE(spec->skeleton.has_value());
  ST_CHECK_EQ(spec->skeleton->joint_count(), 2U);
  // 扁平写法 + parent 引用 → 建树后只剩 1 个根（root），其余成了它的子节点
  ST_CHECK_EQ(spec->objects.size(), 1U);
  ST_CHECK_EQ(spec->objects[0].name, std::string("root"));
  ST_REQUIRE(spec->objects[0].children.size() == 1U);
  ST_REQUIRE(spec->objects[0].children[0].children.size() == 1U);
  // 三层：root → upper → body
  ST_CHECK_EQ(spec->objects[0].children[0].name, std::string("upper"));
  ST_CHECK_EQ(spec->objects[0].children[0].children[0].name, std::string("body"));
  ST_CHECK(spec->objects[0].children[0].children[0].skin);

  const auto render_one = [&](float time) {
    st::raster::Canvas canvas(220, 220);
    const auto status =
        st::raster::render_scene_at(*spec, time, canvas, st::math::Color{14, 15, 19, 255});
    ST_CHECK(status.has_value());
    return inked(canvas);
  };
  const int ink_start = render_one(0.0f);
  const int ink_end = render_one(1.0f);
  st::log::out("[skin] ink t=0 {} t=1 {}\n", ink_start, ink_end);
  ST_CHECK(ink_start > 0);
  ST_CHECK(ink_end > 0);
  // 骨骼转动后蒙皮网格必须跟着形变 → 画面不同
  ST_CHECK(ink_start != ink_end);
}

ST_TEST(skin_gltf_round_trip_preserves_weights) {
  // glTF 往返：权重与骨架必须原样回来（JOINTS_0 是 u8、WEIGHTS_0 是 float）。
  prim::PrimitiveParams params{};
  params.radius = 0.3f;
  params.depth = 0.6f;
  params.subdivisions = 0;
  Mesh mesh = prim::make_ico_sphere(params);
  const std::size_t count = mesh.vertex_count();
  mesh.skins.assign(count * 8U, 0.0f);
  for (std::size_t i = 0; i < count; ++i) {
    // 一半绑 root、一半绑定到 upper（用 0.3/0.7 混合以验证权重本身）
    const float blend = static_cast<float>(i % 2U) * 0.4f + 0.3f;
    mesh.skins[i * 8U] = 0.0f;
    mesh.skins[i * 8U + 1U] = 1.0f - blend;
    mesh.skins[i * 8U + 2U] = 1.0f;
    mesh.skins[i * 8U + 3U] = blend;
  }

  st::raster::GltfObject object{};
  object.mesh = mesh;
  object.name = "upper";        // 必须与骨架关节名对应（写出时按名字找节点）
  object.skinned = true;

  st::raster::GltfWriteOptions options{};
  options.binary = true;
  options.skeleton = make_skeleton(1.0f);
  auto bytes = st::raster::gltf_write_binary({object}, options);
  ST_REQUIRE(bytes.has_value());

  auto scene = st::raster::gltf_read(*bytes);
  ST_REQUIRE(scene.has_value());
  ST_CHECK(scene->has_skin_weights);
  ST_REQUIRE(scene->skeleton.has_value());
  ST_REQUIRE(scene->meshes.size() == 1U);
  const Mesh& loaded = scene->meshes.front();
  ST_REQUIRE(loaded.has_skins());
  ST_CHECK_EQ(loaded.vertex_count(), count);
  // 逐顶点比权重（浮点在二进制里原样往返）
  int mismatched = 0;
  for (std::size_t i = 0; i < count; ++i) {
    for (std::size_t slot = 0; slot < 4U; ++slot) {
      const auto [joint_a, weight_a] = mesh.influence_at(i, slot);
      const auto [joint_b, weight_b] = loaded.influence_at(i, slot);
      if (joint_a != joint_b) ++mismatched;
      if (std::abs(weight_a - weight_b) > 1.0e-5f) ++mismatched;
    }
  }
  ST_CHECK_EQ(mismatched, 0);
  // 骨架：关节名与逆绑定矩阵都要回来
  ST_CHECK_EQ(scene->skeleton->joint_count(), 2U);
  ST_CHECK_EQ(scene->skeleton->joints[0], std::string("root"));
  ST_CHECK_EQ(scene->skeleton->joints[1], std::string("tip"));
  ST_CHECK(scene->skeleton->inverse_bind.size() == 2U);
}

ST_TEST(skin_auto_weights_are_normalized) {
  // 朴素自动权重：每顶点权重和必须为 1（或全零），且关节下标在范围内
  prim::PrimitiveParams params{};
  params.radius = 0.2f;
  params.depth = 0.8f;
  Mesh mesh = prim::make_cylinder(params);
  const Skeleton skeleton = make_skeleton(0.3f);
  const auto tree = st::raster::resolve(make_tree(0.3f));
  ST_REQUIRE(tree.has_value());
  const Mesh weighted = st::raster::auto_weights(mesh, skeleton, *tree, 0.2f);
  ST_REQUIRE(weighted.has_skins());
  int bad_total = 0;
  int bad_joint = 0;
  int zero_weight = 0;
  for (std::size_t i = 0; i < weighted.vertex_count(); ++i) {
    float total = 0.0f;
    for (std::size_t slot = 0; slot < 4U; ++slot) {
      const auto [joint, weight] = weighted.influence_at(i, slot);
      total += weight;
      if (weight > 0.0f && joint >= skeleton.joint_count()) ++bad_joint;
    }
    if (std::abs(total - 1.0f) > 1.0e-4f && total > 0.0f) ++bad_total;
    if (total <= 0.0f) ++zero_weight;
  }
  ST_CHECK_EQ(bad_total, 0);
  ST_CHECK_EQ(bad_joint, 0);
  st::log::out("[skin] auto_weights zero-weight vertices={}\n", zero_weight);
}

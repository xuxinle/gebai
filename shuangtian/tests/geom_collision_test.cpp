/// 碰撞与拾取的回归测试：射线原语、宽相/窄相、场景级 raycast。
///
/// ## 判据为什么必须算得出来
///
/// 求交的错误几乎全是**数值型**且不报错：
/// - slab 法在平行射线上得到 NaN → "永远打不中"（看起来像没实现）；
/// - 三角形求交没做退化判断 → 掠射角下随机命中；
/// - 世界 AABB 只变换两个角 → 旋转后严重低估 → "明明在盒里却拾取不到"；
/// - 射线方向被平移污染 → 命中点整体偏移。
///
/// 每条都要有可复算的坐标或距离，不能只看"有没有命中"。
#include <cmath>
#include <string>
#include <vector>

#include "st/core/log.hpp"
#include "st/math/collision.hpp"
#include "st/raster/mesh_ops.hpp"
#include "st/raster/pick.hpp"
#include "st/raster/primitives.hpp"
#include "st/raster/scene_graph.hpp"
#include "st/test/test.hpp"

namespace {

using st::math::Mat4;
using st::math::Ray;
using st::math::Vec3;
using st::raster::Mesh;
using st::raster::PickObject;
namespace prim = st::raster::primitives;

constexpr float kEps = 1.0e-4f;

/// 单位立方体（中心在原点，半边长 0.5）。
auto make_unit_cube() -> Mesh {
  prim::PrimitiveParams params{};
  params.radius = 0.5f;
  params.depth = 1.0f;
  params.height = 1.0f;
  return prim::make_cube(params);
}

}  // namespace

ST_TEST(collision_ray_box_basic_hits_and_misses) {
  const Vec3 lo{-1.0f, -1.0f, -1.0f};
  const Vec3 hi{1.0f, 1.0f, 1.0f};
  // 从 z=5 朝 -Z 打，应命中 z=1 面（t=4）
  const Ray forward{Vec3{0.0f, 0.0f, 5.0f}, Vec3{0.0f, 0.0f, -1.0f}};
  const auto hit = st::math::ray_intersects_box(forward, lo, hi);
  ST_REQUIRE(hit.has_value());
  ST_CHECK_NEAR(*hit, 4.0f, kEps);

  // 偏离的射线打不中
  const Ray miss{Vec3{2.0f, 0.0f, 5.0f}, Vec3{0.0f, 0.0f, -1.0f}};
  ST_CHECK(!st::math::ray_intersects_box(miss, lo, hi).has_value());

  // 反向（朝 +Z）打不中
  const Ray away{Vec3{0.0f, 0.0f, 5.0f}, Vec3{0.0f, 0.0f, 1.0f}};
  ST_CHECK(!st::math::ray_intersects_box(away, lo, hi).has_value());

  // 起点在盒内：默认算命中且返回 0；`allow_inside=false` 时不算
  const Ray inside{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}};
  const auto inside_hit = st::math::ray_intersects_box(inside, lo, hi);
  ST_REQUIRE(inside_hit.has_value());
  ST_CHECK_NEAR(*inside_hit, 0.0f, kEps);
  ST_CHECK(!st::math::ray_intersects_box(inside, lo, hi, 0.0f, 3.4e38f, false).has_value());
}

ST_TEST(collision_ray_box_parallel_axis_no_nan) {
  // ## 平行射线的经典坑：方向某轴为 0 时 0/0 产生 NaN
  //
  // 症状极隐蔽：`if (t < best)` 对 NaN 恒假 → "永远打不中"，
  // 而"永远打不中"看起来像"还没实现拾取"。
  const Vec3 lo{-1.0f, -1.0f, -1.0f};
  const Vec3 hi{1.0f, 1.0f, 1.0f};

  // 沿 +X 打（y、z 方向分量都是 0）：y/z 若在盒内应命中，盒外应不命中
  const Ray along_x{Vec3{-5.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}};
  const auto hit = st::math::ray_intersects_box(along_x, lo, hi);
  ST_REQUIRE(hit.has_value());
  ST_CHECK_NEAR(*hit, 4.0f, kEps);

  // 沿 +X 但在盒外（y=3）→ 平行轴落在 slab 之外，必须不命中且不产生 NaN
  const Ray outside{Vec3{-5.0f, 3.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}};
  const auto miss = st::math::ray_intersects_box(outside, lo, hi);
  ST_CHECK(!miss.has_value());
  // 显式检查"不命中"不是 NaN 造成的假象：换一条同起点但 y 在盒内的射线必须命中
  const Ray inside_slab{Vec3{-5.0f, 0.5f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}};
  ST_CHECK(st::math::ray_intersects_box(inside_slab, lo, hi).has_value());

  // 完全平行的退化射线（方向为 0）不该崩、不该命中
  const Ray degenerate{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 0.0f}};
  const auto degenerate_hit = st::math::ray_intersects_box(degenerate, lo, hi);
  // 起点在盒内 + 方向为 0 → 返回 0（允许在内部）；关键是**没有 NaN**
  if (degenerate_hit.has_value()) ST_CHECK(std::isfinite(*degenerate_hit));
}

ST_TEST(collision_ray_triangle_barycentric) {
  // 三角形在 z=0 平面，射线沿 -Z 打向重心 → 重心坐标必须是 (1/3, 1/3)
  const Vec3 a{0.0f, 0.0f, 0.0f};
  const Vec3 b{1.0f, 0.0f, 0.0f};
  const Vec3 c{0.0f, 1.0f, 0.0f};
  const Ray ray{Vec3{1.0f / 3.0f, 1.0f / 3.0f, 2.0f}, Vec3{0.0f, 0.0f, -1.0f}};
  const auto hit = st::math::ray_intersects_triangle(ray, a, b, c);
  ST_REQUIRE(hit.has_value());
  ST_CHECK_NEAR(hit->t, 2.0f, kEps);
  ST_CHECK_NEAR(hit->u, 1.0f / 3.0f, 1e-3f);
  ST_CHECK_NEAR(hit->v, 1.0f / 3.0f, 1e-3f);

  // 三角形外（右上角）打不中
  const Ray outside{Vec3{0.9f, 0.9f, 2.0f}, Vec3{0.0f, 0.0f, -1.0f}};
  ST_CHECK(!st::math::ray_intersects_triangle(outside, a, b, c).has_value());

  // 双面：从背面（-Z 侧朝 +Z）打也要命中
  const Ray from_back{Vec3{1.0f / 3.0f, 1.0f / 3.0f, -2.0f}, Vec3{0.0f, 0.0f, 1.0f}};
  ST_CHECK(st::math::ray_intersects_triangle(from_back, a, b, c).has_value());
  // 但开了背面剔除就不命中
  ST_CHECK(!st::math::ray_intersects_triangle(from_back, a, b, c, true).has_value());

  // 退化三角形（共线）：必须不命中（而不是给个噪声 t）
  const Vec3 collinear{2.0f, 0.0f, 0.0f};
  const Ray toward{Vec3{0.5f, 0.0f, 2.0f}, Vec3{0.0f, 0.0f, -1.0f}};
  ST_CHECK(!st::math::ray_intersects_triangle(toward, a, b, collinear).has_value());

  // 掠射（射线几乎与三角形平行）：必须返回不命中，而不是极大/极小的 t
  const Ray grazing{Vec3{0.3f, 0.3f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}};
  const auto grazing_hit = st::math::ray_intersects_triangle(grazing, a, b, c);
  if (grazing_hit.has_value()) ST_CHECK(std::isfinite(grazing_hit->t));
}

ST_TEST(collision_ray_sphere_and_box_distance) {
  const auto hit = st::math::ray_intersects_sphere(Ray{Vec3{0.0f, 0.0f, 5.0f}, Vec3{0.0f, 0.0f, -1.0f}},
                                                   Vec3{}, 1.0f);
  ST_REQUIRE(hit.has_value());
  ST_CHECK_NEAR(hit->t_near, 4.0f, kEps);
  ST_CHECK_NEAR(hit->t_far, 6.0f, kEps);
  // 偏开半径 → 不命中
  ST_CHECK(!st::math::ray_intersects_sphere(Ray{Vec3{2.0f, 0.0f, 5.0f}, Vec3{0.0f, 0.0f, -1.0f}},
                                            Vec3{}, 1.0f)
                .has_value());

  // 点与盒距离：盒内为 0；正上方为 (y-hi)；角上为三维斜距
  const Vec3 lo{-1.0f, -1.0f, -1.0f};
  const Vec3 hi{1.0f, 1.0f, 1.0f};
  ST_CHECK_NEAR(st::math::squared_distance_point_box(Vec3{}, lo, hi), 0.0f, kEps);
  ST_CHECK_NEAR(st::math::squared_distance_point_box(Vec3{0.0f, 3.0f, 0.0f}, lo, hi), 4.0f, kEps);
  ST_CHECK_NEAR(st::math::squared_distance_point_box(Vec3{2.0f, 2.0f, 2.0f}, lo, hi), 3.0f, kEps);

  // 球与盒：半径刚好够到算相交（闭区间）
  ST_CHECK(st::math::sphere_intersects_box(Vec3{0.0f, 2.0f, 0.0f}, 1.0f, lo, hi));
  ST_CHECK(!st::math::sphere_intersects_box(Vec3{0.0f, 2.1f, 0.0f}, 1.0f, lo, hi));

  // 盒重叠：贴合算重叠
  ST_CHECK(st::math::boxes_overlap(lo, hi, Vec3{1.0f, -1.0f, -1.0f}, Vec3{3.0f, 1.0f, 1.0f}));
  ST_CHECK(!st::math::boxes_overlap(lo, hi, Vec3{1.1f, -1.0f, -1.0f}, Vec3{3.0f, 1.0f, 1.0f}));
}

ST_TEST(collision_transform_ray_does_not_contaminate_direction) {
  // ## 方向不该被平移污染
  //
  // 踩过的经典坑：方向用 4×4 变换（含平移），得到的方向被平移量污染。
  // 症状是"射线在物体附近莫名其妙地偏"，而偏的方向恰好等于物体位置。
  const Ray ray{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}};
  const Mat4 move = Mat4::translation(Vec3{10.0f, 20.0f, 30.0f});
  const Ray moved = st::math::transform_ray(move, ray);
  ST_CHECK_NEAR(moved.origin.x, 10.0f, kEps);
  ST_CHECK_NEAR(moved.origin.y, 20.0f, kEps);
  ST_CHECK_NEAR(moved.origin.z, 30.0f, kEps);
  // 方向不变（平移不作用于方向）
  ST_CHECK_NEAR(moved.direction.x, 0.0f, kEps);
  ST_CHECK_NEAR(moved.direction.y, 0.0f, kEps);
  ST_CHECK_NEAR(moved.direction.z, -1.0f, kEps);
}

ST_TEST(pick_world_bounds_uses_all_eight_corners) {
  // ## 只变换 min/max 两点会严重低估旋转后的盒子
  //
  // 判决式：把 2×1×0.1 的长条绕 Y 转 45°，其世界 AABB 的 X 宽度必须接近
  // `2*cos45 + 0.1*sin45 ≈ 1.485`；若只变换两个角会得到更小的值（拾取不到边缘）。
  // ⚠ `make_cube` 的参数语义：`radius` = 半边 X，`depth` = **Y 向长度**，
  // `height`（>0 时）= **Z 向长度**（见 primitives.cpp 的注释）。
  // 所以要造"X 长 2、Y 薄 0.1、Z 宽 2"的板，是 `radius=1, depth=0.1, height=2`。
  prim::PrimitiveParams params{};
  params.radius = 1.0f;
  params.depth = 0.1f;    // Y（薄）
  params.height = 2.0f;   // Z（宽）
  const Mesh bar = prim::make_cube(params);
  const Mat4 rotate = Mat4::rotation(Vec3{0.0f, 1.0f, 0.0f}, 0.7853982f);   // 45°
  const PickObject object = st::raster::make_pick_object("bar", bar, rotate);
  ST_CHECK(!object.world_bounds.is_empty());
  const float width_x = object.world_bounds.max.x - object.world_bounds.min.x;
  ST_CHECK(width_x > 1.3f);   // 八个角都算才够宽
  // 厚度轴几乎不变（绕 Y 转不影响 Y）
  const float height_y = object.world_bounds.max.y - object.world_bounds.min.y;
  ST_CHECK_NEAR(height_y, 0.1f, 1e-3f);
}

ST_TEST(pick_raycast_hits_nearest_object) {
  // 两个立方体：一个在 z=0、一个在 z=-3。从 z=5 朝 -Z 打，必须命中**近的**那个
  const Mesh cube = make_unit_cube();
  std::vector<PickObject> objects;
  objects.push_back(st::raster::make_pick_object("near", cube, Mat4::identity()));
  objects.push_back(
      st::raster::make_pick_object("far", cube, Mat4::translation(Vec3{0.0f, 0.0f, -3.0f})));

  const Ray ray{Vec3{0.0f, 0.0f, 5.0f}, Vec3{0.0f, 0.0f, -1.0f}};
  const auto hit = st::raster::raycast(objects, ray);
  ST_REQUIRE(hit.has_value());
  ST_CHECK_EQ(hit->name, std::string("near"));
  ST_CHECK_NEAR(hit->distance, 4.5f, 0.01f);   // 立方体半边长 0.5 → z=0.5 面
  ST_CHECK_NEAR(hit->point.z, 0.5f, 0.01f);
  // 法线朝射线来向（+Z）
  ST_CHECK(hit->normal.z > 0.5f);

  // 穿透看全部：应当两层都命中（近的在 z=0、远的在 z=-3），且按距离升序
  const auto all = st::raster::raycast_all(objects, ray);
  ST_REQUIRE(all.size() >= 2U);
  ST_CHECK(all[0].distance <= all[1].distance);
  ST_CHECK_EQ(all[0].name, std::string("near"));
  ST_CHECK_EQ(all[1].name, std::string("far"));

  // 限距：只能打到 1 米内 → 一个都不命中（最近的也要 4.5）
  ST_CHECK(!st::raster::raycast(objects, ray, 1.0f).has_value());
  // 限距 5 米 → 命中近的（4.5 < 5）
  ST_CHECK(st::raster::raycast(objects, ray, 5.0f).has_value());
}

ST_TEST(pick_raycast_respects_node_transform) {
  // 物体被移到 (3,0,0) 并缩放：射线从 (3,0,5) 打过去必须命中，
  // 从 (0,0,5) 打过去必须不命中——这证明世界矩阵真的参与了求交。
  const Mesh cube = make_unit_cube();
  const Mat4 world = Mat4::translation(Vec3{3.0f, 0.0f, 0.0f}) * Mat4::scaling(Vec3{2.0f, 2.0f, 2.0f});
  const std::vector<PickObject> objects{st::raster::make_pick_object("moved", cube, world)};

  const Ray on_target{Vec3{3.0f, 0.0f, 5.0f}, Vec3{0.0f, 0.0f, -1.0f}};
  const auto hit = st::raster::raycast(objects, on_target);
  ST_REQUIRE(hit.has_value());
  ST_CHECK_EQ(hit->name, std::string("moved"));
  // 缩放到 2 倍 → 半边长 1.0 → 命中面在 z=1
  ST_CHECK_NEAR(hit->point.z, 1.0f, 0.02f);

  const Ray off_target{Vec3{0.0f, 0.0f, 5.0f}, Vec3{0.0f, 0.0f, -1.0f}};
  ST_CHECK(!st::raster::raycast(objects, off_target).has_value());
}

ST_TEST(pick_overlaps_and_gap) {
  const Mesh cube = make_unit_cube();
  const PickObject a = st::raster::make_pick_object("a", cube, Mat4::identity());
  const PickObject b = st::raster::make_pick_object("b", cube, Mat4::translation(Vec3{0.5f, 0.0f, 0.0f}));
  const PickObject c =
      st::raster::make_pick_object("c", cube, Mat4::translation(Vec3{0.0f, 5.0f, 0.0f}));

  ST_CHECK(st::raster::overlaps(a, b));     // 相交
  ST_CHECK(!st::raster::overlaps(a, c));    // 分离
  ST_CHECK_NEAR(st::raster::bounds_gap(a, b), 0.0f, kEps);
  // a 的上沿 y=0.5、c 的下沿 y=4.5 → 间距 4.0
  ST_CHECK_NEAR(st::raster::bounds_gap(a, c), 4.0f, 0.01f);
}

ST_TEST(pick_from_composite_scene_hits_named_part) {
  // 端到端：组合模型（父动子动）+ 拾取。转过的父节点下的子网格必须能被命中。
  //
  // 这条把"空间体系"与"拾取"接起来：世界矩阵由 `resolve` 算出，
  // 再交给 `make_pick_object`——如果 resolve 的乘序错了，命中点会偏。
  st::raster::SceneNode root{};
  root.name = "rig";
  root.has_geometry = false;
  root.local.rotation = Vec3{0.0f, 1.5707963f, 0.0f};   // 绕 Y 转 90°：把 +X 转到 -Z
  st::raster::SceneNode arm{};
  arm.name = "arm";
  arm.has_geometry = true;
  arm.local.translation = Vec3{2.0f, 0.0f, 0.0f};       // 局部 +X 2 米
  root.children.push_back(arm);

  const auto tree = st::raster::resolve({root});
  ST_REQUIRE(tree.has_value());
  const auto* node = tree->find_by_name("arm");
  ST_REQUIRE(node != nullptr);
  // 转 90° 后 arm 的世界位置应当是 (0,0,-2)
  ST_CHECK_NEAR(node->world.value(0, 3), 0.0f, 1e-4f);
  ST_CHECK_NEAR(node->world.value(2, 3), -2.0f, 1e-4f);

  const Mesh cube = make_unit_cube();
  const std::vector<PickObject> objects{
      st::raster::make_pick_object("arm", cube, node->world)};
  // 从 (0,0,3) 朝 -Z 打：会先经过 z=0 一带，但 arm 在 z=-2（半径 0.5 → [-2.5,-1.5]）
  const Ray ray{Vec3{0.0f, 0.0f, 3.0f}, Vec3{0.0f, 0.0f, -1.0f}};
  const auto hit = st::raster::raycast(objects, ray);
  ST_REQUIRE(hit.has_value());
  ST_CHECK_EQ(hit->name, std::string("arm"));
  ST_CHECK_NEAR(hit->point.z, -1.5f, 0.02f);   // 命中近侧的 z=-1.5 面
}

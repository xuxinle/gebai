/// 组合模型的**空间体系**回归测试：节点树、局部坐标系、枢轴、可见性继承。
///
/// ## 为什么每一条都要有量化判据
///
/// "组合模型"最容易出的错不是崩溃，而是**位置差一点点**：
/// 枢轴加错的符号、父子乘序反了、局部坐标被当成世界坐标。这些错误画出来
/// 仍然"像个模型"，只是各零件之间的相对关系不对——肉眼很难判断是哪里错了。
///
/// 所以这里的判据全部取**可复算的数**：节点的世界矩阵平移分量、把已知点
/// 过一遍矩阵后的落点、渲染后的覆盖像素数。不用"看起来对"。
#include <cmath>
#include <string>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/primitives.hpp"
#include "st/raster/scene3d_script.hpp"
#include "st/raster/scene_graph.hpp"
#include "st/test/test.hpp"

namespace {

using st::math::Mat4;
using st::math::Vec3;
using st::raster::Bounds;
using st::raster::SceneNode;
using st::raster::TransformTRS;

/// 把点过一遍矩阵（含平移）。
auto apply(const Mat4& matrix, Vec3 point) -> Vec3 {
  return Vec3{matrix.value(0, 0) * point.x + matrix.value(0, 1) * point.y +
                  matrix.value(0, 2) * point.z + matrix.value(0, 3),
              matrix.value(1, 0) * point.x + matrix.value(1, 1) * point.y +
                  matrix.value(1, 2) * point.z + matrix.value(1, 3),
              matrix.value(2, 0) * point.x + matrix.value(2, 1) * point.y +
                  matrix.value(2, 2) * point.z + matrix.value(2, 3)};
}

/// 数非背景像素。
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

auto render(const std::string& json, int width, int height) -> int {
  auto spec = st::raster::parse_scene_spec_text(json);
  if (!spec) {
    st::log::err("解析失败：{}\n", spec.error().message);
    return -1;
  }
  st::raster::Canvas canvas(width, height);
  const auto status = st::raster::render_scene(*spec, canvas, st::math::Color{14, 15, 19, 255});
  if (!status) {
    st::log::err("渲染失败：{}\n", status.error().message);
    return -1;
  }
  return inked(canvas);
}

}  // namespace

ST_TEST(scene_graph_child_inherits_parent_transform) {
  // 父沿 Y 抬 1.0；子相对父再抬 0.5 → 子在世界里应当到 1.5。
  // 判据直接取世界矩阵的平移分量：父动子动是组合模型的**定义**，
  // 它错了其它一切都不必再测。
  SceneNode parent{};
  parent.name = "parent";
  parent.has_geometry = false;
  parent.local.translation = Vec3{0.0f, 1.0f, 0.0f};

  SceneNode child{};
  child.name = "child";
  child.local.translation = Vec3{0.0f, 0.5f, 0.0f};
  parent.children.push_back(child);

  const auto tree = st::raster::resolve({parent});
  ST_REQUIRE(tree.has_value());
  ST_CHECK_EQ(tree->nodes.size(), 2U);
  const auto* found = tree->find_by_name("child");
  ST_REQUIRE(found != nullptr);
  ST_CHECK_NEAR(found->world.value(1, 3), 1.5f, 1e-5f);
  ST_CHECK_EQ(found->parent, 0);
  // 深度优先先序：父一定在子之前
  ST_CHECK_EQ(tree->nodes[0].name, std::string("parent"));
  ST_CHECK_EQ(tree->nodes[1].name, std::string("child"));
  ST_CHECK_EQ(tree->nodes[0].depth, 0U);
  ST_CHECK_EQ(tree->nodes[1].depth, 1U);
}

ST_TEST(scene_graph_parent_rotation_carries_child_around) {
  // 父绕 Y 转 90°、位于原点；子原本在 +X 方向 1.0 处。
  // 转 90° 后子应当落到 **-Z** 方向（右手系、Y 朝上：绕 Y 正转把 +X 转到 -Z）。
  //
  // 这一条钉的是"乘序"：写成 `local * parent` 会让子**不动**（=没继承父），
  // 而那种错在单层场景里完全看不出来。
  SceneNode parent{};
  parent.name = "pivot";
  parent.has_geometry = false;
  parent.local.rotation = Vec3{0.0f, 1.5707963f, 0.0f};   // π/2

  SceneNode child{};
  child.name = "tip";
  child.local.translation = Vec3{1.0f, 0.0f, 0.0f};
  parent.children.push_back(child);

  const auto tree = st::raster::resolve({parent});
  ST_REQUIRE(tree.has_value());
  const auto* tip = tree->find_by_name("tip");
  ST_REQUIRE(tip != nullptr);
  const Vec3 world_origin = apply(tip->world, Vec3{});
  ST_CHECK_NEAR(world_origin.x, 0.0f, 1e-4f);
  ST_CHECK_NEAR(world_origin.y, 0.0f, 1e-4f);
  ST_CHECK_NEAR(world_origin.z, -1.0f, 1e-4f);
}

ST_TEST(scene_graph_pivot_changes_the_rotation_center) {
  // 一个边长 1 的立方体，枢轴放在**底面中心** (0,-0.5,0)，绕 X 转 90°。
  //
  // 判据：把立方体的底面中心（局部 (0,-0.5,0)）过一遍矩阵，它必须**原地不动**
  // —— 这正是"枢轴"的定义。把它换成原点枢轴，同一个点会被转到 (0,0,0.5)。
  SceneNode node{};
  node.name = "cube";
  node.pivot = Vec3{0.0f, -0.5f, 0.0f};
  node.local.rotation = Vec3{1.5707963f, 0.0f, 0.0f};   // 绕 X 转 π/2

  const Mat4 with_pivot = st::raster::local_matrix(node);
  const Vec3 stayed = apply(with_pivot, Vec3{0.0f, -0.5f, 0.0f});
  ST_CHECK_NEAR(stayed.x, 0.0f, 1e-5f);
  ST_CHECK_NEAR(stayed.y, -0.5f, 1e-5f);
  ST_CHECK_NEAR(stayed.z, 0.0f, 1e-5f);

  node.pivot = Vec3{};
  const Mat4 without_pivot = st::raster::local_matrix(node);
  const Vec3 moved = apply(without_pivot, Vec3{0.0f, -0.5f, 0.0f});
  // 无枢轴时：底心 (0,-0.5,0) 绕原点转 +90°。
  // 右手系绕 +X 的旋转把 +Y 转到 **+Z**，于是 -Y 转到 **-Z**：
  // (0,-0.5,0) → (0,0,-0.5)。判据写死这个符号——符号写反正是枢轴类缺陷的典型形态。
  ST_CHECK_NEAR(moved.y, 0.0f, 1e-5f);
  ST_CHECK_NEAR(moved.z, -0.5f, 1e-5f);
  // 两者必须不同（否则"枢轴生效"这条判据不可分辨）
  ST_CHECK(std::abs(stayed.z - moved.z) > 0.4f);
}

ST_TEST(scene_graph_pivot_presets_follow_bounds) {
  // 预设按包围盒算：底面中心 / 顶面中心 / 中心。
  st::raster::primitives::PrimitiveParams params{};
  params.radius = 0.5f;
  params.depth = 1.0f;
  params.height = 1.0f;
  const auto cube = st::raster::primitives::make_cube(params);
  const Bounds box = st::raster::bounds(cube);
  ST_CHECK_NEAR(box.min.y, -0.5f, 1e-4f);
  ST_CHECK_NEAR(box.max.y, 0.5f, 1e-4f);

  const Vec3 bottom = st::raster::pivot_from_bounds(box, st::raster::PivotPreset::Bottom);
  ST_CHECK_NEAR(bottom.x, 0.0f, 1e-5f);
  ST_CHECK_NEAR(bottom.y, -0.5f, 1e-4f);
  ST_CHECK_NEAR(bottom.z, 0.0f, 1e-5f);

  const Vec3 top = st::raster::pivot_from_bounds(box, st::raster::PivotPreset::Top);
  ST_CHECK_NEAR(top.y, 0.5f, 1e-4f);

  const Vec3 center = st::raster::pivot_from_bounds(box, st::raster::PivotPreset::Center);
  ST_CHECK_NEAR(center.y, 0.0f, 1e-4f);

  // origin 恒为零（且空盒也不炸）
  const Vec3 origin = st::raster::pivot_from_bounds(box, st::raster::PivotPreset::Origin);
  ST_CHECK_NEAR(origin.x, 0.0f, 1e-6f);
  const Vec3 from_empty =
      st::raster::pivot_from_bounds(Bounds::empty(), st::raster::PivotPreset::Bottom);
  ST_CHECK_NEAR(from_empty.y, 0.0f, 1e-6f);
  // 预设名往返
  ST_CHECK(st::raster::parse_pivot_preset("Bottom").value() == st::raster::PivotPreset::Bottom);
  ST_CHECK(st::raster::parse_pivot_preset("feet").value() == st::raster::PivotPreset::Bottom);
  ST_CHECK(!st::raster::parse_pivot_preset("elbow").has_value());
}

ST_TEST(scene_graph_rejects_duplicate_names_and_cycles) {
  // 重名：按名字寻址会歧义 → 必须拒绝（而不是后者覆盖前者）
  SceneNode a{};
  a.name = "same";
  SceneNode b{};
  b.name = "same";
  const auto duplicated = st::raster::resolve({a, b});
  ST_REQUIRE(!duplicated.has_value());
  ST_CHECK(duplicated.error().message.find("same") != std::string::npos);
  ST_CHECK(duplicated.error().message.find("唯一") != std::string::npos);

  // 环：flat 写法里 A→B→A
  std::vector<SceneNode> nodes(2);
  nodes[0].name = "a";
  nodes[1].name = "b";
  const auto cycled = st::raster::attach_by_parent(nodes, {"b", "a"});
  ST_REQUIRE(!cycled.has_value());
  ST_CHECK(cycled.error().message.find("环") != std::string::npos);

  // 自指
  std::vector<SceneNode> self_ref(1);
  self_ref[0].name = "self";
  const auto self_cycle = st::raster::attach_by_parent(self_ref, {"self"});
  ST_REQUIRE(!self_cycle.has_value());

  // 父不存在 → NotFound（且报出是谁要挂谁）
  std::vector<SceneNode> missing(1);
  missing[0].name = "child";
  const auto not_found = st::raster::attach_by_parent(missing, {"ghost"});
  ST_REQUIRE(!not_found.has_value());
  ST_CHECK(not_found.error().code == st::ErrorCode::NotFound);
  ST_CHECK(not_found.error().message.find("ghost") != std::string::npos);
  ST_CHECK(not_found.error().message.find("child") != std::string::npos);
}

ST_TEST(scene_graph_flat_and_nested_forms_are_equivalent) {
  // 同一个模型，嵌套写法 与 扁平写法 必须得到**逐位相同**的世界矩阵。
  // 这条钉的是"两种入口不会漂移"——否则用户会发现"换个写法位置就变了"。
  const std::string nested = R"({
    "objects": [
      { "name": "arm", "group": true, "translate": [0.5, 0.3, 0.0], "rotate": [0.0, 0.6, 0.0],
        "children": [
          { "name": "upper", "shape": "capsule", "radius": 0.12, "depth": 0.8,
            "translate": [0.0, -0.4, 0.0], "rotate": [0.3, 0.0, 0.0] },
          { "name": "hand", "shape": "uv_sphere", "radius": 0.1, "translate": [0.0, -0.85, 0.0] }
        ] }
    ]
  })";
  const std::string flat = R"({
    "objects": [
      { "name": "arm", "group": true, "translate": [0.5, 0.3, 0.0], "rotate": [0.0, 0.6, 0.0] },
      { "name": "upper", "parent": "arm", "shape": "capsule", "radius": 0.12, "depth": 0.8,
        "translate": [0.0, -0.4, 0.0], "rotate": [0.3, 0.0, 0.0] },
      { "name": "hand", "parent": "arm", "shape": "uv_sphere", "radius": 0.1,
        "translate": [0.0, -0.85, 0.0] }
    ]
  })";
  auto a = st::raster::parse_scene_spec_text(nested);
  auto b = st::raster::parse_scene_spec_text(flat);
  ST_REQUIRE(a.has_value());
  ST_REQUIRE(b.has_value());
  // 嵌套写法的顶层只有 arm；扁平写法建树后也只剩 arm 一个根
  ST_CHECK_EQ(a->objects.size(), 1U);
  ST_CHECK_EQ(b->objects.size(), 1U);
  ST_CHECK_EQ(a->objects[0].children.size(), 2U);
  ST_CHECK_EQ(b->objects[0].children.size(), 2U);

  // 逐节点比世界矩阵（用 scene_graph 求解两棵树）
  const auto build = [](const st::raster::SceneObjectSpec& object, const auto& self,
                        SceneNode& out) -> void {
    out.name = object.name;
    out.has_geometry = !object.group;
    out.local.translation = object.translate;
    out.local.rotation = object.rotate;
    out.local.scale = object.scale;
    for (const auto& child : object.children) {
      SceneNode slot{};
      self(child, self, slot);
      out.children.push_back(slot);
    }
  };
  SceneNode root_a{};
  build(a->objects[0], build, root_a);
  SceneNode root_b{};
  build(b->objects[0], build, root_b);

  const auto tree_a = st::raster::resolve({root_a});
  const auto tree_b = st::raster::resolve({root_b});
  ST_REQUIRE(tree_a.has_value());
  ST_REQUIRE(tree_b.has_value());
  ST_CHECK_EQ(tree_a->nodes.size(), tree_b->nodes.size());
  ST_CHECK_EQ(tree_a->nodes.size(), 3U);
  for (std::size_t i = 0; i < tree_a->nodes.size(); ++i) {
    ST_CHECK_EQ(tree_a->nodes[i].path, tree_b->nodes[i].path);
    for (int column = 0; column < 4; ++column) {
      for (int row = 0; row < 4; ++row) {
        ST_CHECK_NEAR(tree_a->nodes[i].world.value(row, column),
                      tree_b->nodes[i].world.value(row, column), 1e-6f);
      }
    }
  }
}

ST_TEST(scene_graph_visibility_inherits) {
  // 父不可见 → 整棵子树都不可见（哪怕子是 visible=true）。
  // 这一条对应真实编辑动作："先把一台设备藏起来"，不该逐个零件去关。
  SceneNode root{};
  root.name = "device";
  root.has_geometry = false;
  root.visible = false;
  SceneNode part{};
  part.name = "part";
  part.visible = true;
  root.children.push_back(part);

  const auto tree = st::raster::resolve({root});
  ST_REQUIRE(tree.has_value());
  const auto* found = tree->find_by_name("part");
  ST_REQUIRE(found != nullptr);
  ST_CHECK(!found->visible);
}

ST_TEST(scene_script_composite_model_renders_children) {
  // 端到端：一个"支架 + 两个球"的组合，用**父节点**统一抬起，
  // 两个球必须跟着一起动。判据是渲染覆盖像素数（父不动时球在画面外/贴边）。
  const std::string json = R"({
    "camera": { "eye": [0.0, 0.8, 4.2], "target": [0.0, 0.6, 0.0], "fov": 40 },
    "lighting": { "ambient": 0.4, "diffuse": 0.8, "rim": 0.2 },
    "objects": [
      { "name": "rig", "group": true, "translate": [0.0, 1.2, 0.0],
        "children": [
          { "name": "left", "shape": "uv_sphere", "radius": 0.3, "translate": [-0.5, 0.0, 0.0],
            "color": "#88aaff" },
          { "name": "right", "shape": "uv_sphere", "radius": 0.3, "translate": [0.5, 0.0, 0.0],
            "color": "#ffaa88" }
        ] }
    ]
  })";
  const int ink = render(json, 260, 200);
  st::log::out("[graph] composite ink={}\n", ink);
  ST_CHECK(ink > 0);

  // 把父的抬起量改成 0（球会落到画面下方中央）→ 覆盖像素数应当变化，
  // 这证明"父的变换确实作用到了子身上"（而不是子被当成世界坐标画）。
  std::string lower = json;
  const std::string from = R"("translate": [0.0, 1.2, 0.0],)";
  const std::string to = R"("translate": [0.0, 0.0, 0.0],)";
  const std::size_t at = lower.find(from);
  ST_REQUIRE(at != std::string::npos);
  lower.replace(at, from.size(), to);
  const int ink_lowered = render(lower, 260, 200);
  st::log::out("[graph] lowered ink={}\n", ink_lowered);
  ST_CHECK(ink_lowered != ink);
}

ST_TEST(scene_script_pivot_at_bottom_rotates_in_place) {
  // `pivot_at: "bottom"` 的立方体绕 X 转 90°：它会"就地倾倒"而不是**插进地里**。
  // 判据：渲染后物体不越出画布下沿（越界会被裁掉，覆盖像素数明显变小）。
  const std::string with_pivot = R"({
    "camera": { "eye": [0.0, 0.5, 3.6], "target": [0.0, 0.0, 0.0], "fov": 42 },
    "objects": [
      { "name": "crate", "shape": "cube", "radius": 0.5, "depth": 1.0, "height": 1.0,
        "pivot_at": "bottom", "rotate": [1.5707963, 0.0, 0.0], "color": "#d4b07a" }
    ]
  })";
  // 同样的方块但枢轴在原点（默认）：转 90° 后一半会沉到 y<0
  const std::string without_pivot = R"({
    "camera": { "eye": [0.0, 0.5, 3.6], "target": [0.0, 0.0, 0.0], "fov": 42 },
    "objects": [
      { "name": "crate", "shape": "cube", "radius": 0.5, "depth": 1.0, "height": 1.0,
        "rotate": [1.5707963, 0.0, 0.0], "color": "#d4b07a" }
    ]
  })";
  const int ink_pivot = render(with_pivot, 240, 240);
  const int ink_origin = render(without_pivot, 240, 240);
  st::log::out("[graph] pivot={} origin={}\n", ink_pivot, ink_origin);
  ST_CHECK(ink_pivot > 0);
  ST_CHECK(ink_origin > 0);
  // 枢轴在底面时，倾倒后整块仍在视线正中（覆盖面积更大）；
  // 原点枢轴时它有一半落到 y<0（画面下方），可见面积更小。
  ST_CHECK(ink_pivot > ink_origin);
}

ST_TEST(scene_script_composite_errors_carry_paths) {
  // 组合写法的错误必须指到**具体节点**。模型拿到 "objects[0].children[1].parent"
  // 才能改；拿到"解析失败"只能重写。
  const std::string nested_with_parent = R"({
    "objects": [ { "name": "a", "children": [ { "shape": "cube", "parent": "a" } ] } ]
  })";
  auto nested = st::raster::parse_scene_spec_text(nested_with_parent);
  ST_REQUIRE(!nested.has_value());
  ST_CHECK(nested.error().message.find("children[0]") != std::string::npos);
  ST_CHECK(nested.error().message.find("parent") != std::string::npos);

  const std::string bad_pivot = R"({
    "objects": [ { "name": "a", "shape": "cube", "pivot_at": "elbow" } ]
  })";
  auto pivot = st::raster::parse_scene_spec_text(bad_pivot);
  ST_REQUIRE(!pivot.has_value());
  ST_CHECK(pivot.error().message.find("pivot_at") != std::string::npos);
  ST_CHECK(pivot.error().message.find("bottom") != std::string::npos);   // 可用清单

  const std::string missing_parent = R"({
    "objects": [ { "name": "a", "shape": "cube", "parent": "ghost" } ]
  })";
  auto missing = st::raster::parse_scene_spec_text(missing_parent);
  ST_REQUIRE(!missing.has_value());
  ST_CHECK(missing.error().message.find("ghost") != std::string::npos);

  const std::string duplicate = R"({
    "objects": [ { "name": "same", "shape": "cube" }, { "name": "same", "shape": "cube" } ]
  })";
  auto dup = st::raster::parse_scene_spec_text(duplicate);
  ST_REQUIRE(!dup.has_value());
  ST_CHECK(dup.error().message.find("same") != std::string::npos);
}

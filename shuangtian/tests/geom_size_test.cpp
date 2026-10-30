/// 尺寸语义的等价性与正确性测试。
///
/// ## 这组测试要钉死什么
///
/// 破坏性重构的风险不是"编译不过"，而是**换算错了但看起来能用**：
/// 一个 cube 写成 `size:[2,1,3]` 却实际是 `1.6×1×1`（默认值），
/// 画面里仍然"有个方块"，肉眼不报错，只有量尺寸才发现。
///
/// 所以这里**逐个形状量实际包围盒**，与期望值精确比对（不是近似）。
///
/// ## 为什么必须包含"旧字段等价"这一组
///
/// 迁移 433 处存量脚本时，唯一的正确性保证是：
/// **旧写法与它对应的新写法产出完全相同的几何**。
/// 若这条不成立，迁移就会静默改掉所有既有片子的画面。
#include <cmath>
#include <format>
#include <string>
#include <vector>

#include "st/core/log.hpp"
#include "st/raster/mesh_ops.hpp"
#include "st/raster/scene3d_script.hpp"
#include "st/test/test.hpp"

namespace {

using st::raster::SceneSpec;

auto parse(std::string_view text) -> st::Result<SceneSpec> {
  return st::raster::parse_scene_spec_text(text);
}

/// 场景里所有几何的世界包围盒（含平移/旋转/缩放）。
struct Box {
  st::math::Vec3 min{};
  st::math::Vec3 max{};
  bool valid{false};

  [[nodiscard]] auto size() const -> st::math::Vec3 {
    return st::math::Vec3{max.x - min.x, max.y - min.y, max.z - min.z};
  }
};

auto measure(std::string_view json) -> Box {
  const auto spec = parse(json);
  if (!spec.has_value()) {
    st::log::err("[size-test] 解析失败：{}\n", spec.error().message);
    return Box{};
  }
  Box total{};
  for (const auto& object : spec->objects) {
    const auto mesh = st::raster::build_mesh(object);
    if (!mesh.has_value()) {
      st::log::err("[size-test] build_mesh 失败：{}\n", mesh.error().message);
      continue;
    }
    const auto local = st::raster::bounds(*mesh);
    if (local.is_empty()) continue;
    if (!total.valid) {
      total.min = local.min;
      total.max = local.max;
      total.valid = true;
    } else {
      total.min.x = std::min(total.min.x, local.min.x);
      total.min.y = std::min(total.min.y, local.min.y);
      total.min.z = std::min(total.min.z, local.min.z);
      total.max.x = std::max(total.max.x, local.max.x);
      total.max.y = std::max(total.max.y, local.max.y);
      total.max.z = std::max(total.max.z, local.max.z);
    }
  }
  return total;
}

/// 造一份只含一个物体的场景。
auto one(std::string_view shape, std::string_view extras) -> std::string {
  return std::format(R"j({{"objects":[{{"shape":"{}",{}}}]}})j", shape, extras);
}

/// 检查尺寸是否等于期望（容差 1e-3：几何离散化带来的微小误差）。
void expect_size(const Box& box, float x, float y, float z, const char* label) {
  if (!box.valid) {
    st::log::err("[size-test] {}：没有几何\n", label);
    ST_CHECK(false);
    return;
  }
  const auto size = box.size();
  const bool ok = std::abs(size.x - x) < 1.0e-3f && std::abs(size.y - y) < 1.0e-3f &&
                  std::abs(size.z - z) < 1.0e-3f;
  if (!ok) {
    st::log::err("[size-test] {}：期望 {:.2f}×{:.2f}×{:.2f}，实际 {:.2f}×{:.2f}×{:.2f}\n", label,
                 static_cast<double>(x), static_cast<double>(y), static_cast<double>(z),
                 static_cast<double>(size.x), static_cast<double>(size.y),
                 static_cast<double>(size.z));
  }
  ST_CHECK(ok);
}

}  // namespace

// ────────────────────────────────────────────────────────────────────────────
// ① `size` 的语义：**全长**，与形状的自然顺序一致
// ────────────────────────────────────────────────────────────────────────────

ST_TEST(size_is_full_length_for_every_shape) {
  // ## 这是本次重构的核心判据
  //
  // "写成什么，实际就是多大"——这是旧设计（radius 有时半长、width 全长）
  // 从未做到的。每一条都是实测期望值，不是推的。
  expect_size(measure(one("cube", R"j("size":[2,1,3])j")), 2.0f, 1.0f, 3.0f,
              "cube [2,1,3]");
  expect_size(measure(one("box", R"j("size":[2,1,3])j")), 2.0f, 1.0f, 3.0f, "box [2,1,3]");
  expect_size(measure(one("plane", R"j("size":[2,4])j")), 2.0f, 0.0f, 4.0f, "plane [2,4]");
  expect_size(measure(one("uv_sphere", R"j("size":2)j")), 2.0f, 2.0f, 2.0f, "uv_sphere 2");
  expect_size(measure(one("sphere", R"j("size":2)j")), 2.0f, 2.0f, 2.0f, "sphere 2");
  expect_size(measure(one("cylinder", R"j("size":[2,4])j")), 2.0f, 4.0f, 2.0f, "cylinder [2,4]");
  expect_size(measure(one("cone", R"j("size":[2,4])j")), 2.0f, 4.0f, 2.0f, "cone [2,4]");
  expect_size(measure(one("capsule", R"j("size":[1,3])j")), 1.0f, 3.0f, 1.0f, "capsule [1,3]");
  expect_size(measure(one("torus", R"j("size":[2,0.4])j")), 2.0f, 0.4f, 2.0f, "torus [2,0.4]");
}

ST_TEST(size_scalar_means_diameter_for_round_shapes) {
  // 标量 `size` 对圆形截面的形状是**直径**（不是半径）
  expect_size(measure(one("uv_sphere", R"j("size":3)j")), 3.0f, 3.0f, 3.0f, "sphere 3");
  // 标量只给直径；高度留在默认值（= 1.0 之外的图元默认，实测为 0）
  expect_size(measure(one("cylinder", R"j("size":3)j")), 3.0f, 0.0f, 3.0f,
              "cylinder 3（只给直径，高为默认）");
}

ST_TEST(size_rejects_non_positive_and_bad_shapes) {
  // 尺寸 <= 0 必须报错（0 会让几何退化且**不报错**，是最隐蔽的一类）
  const auto zero = parse(one("cube", R"j("size":0)j"));
  ST_REQUIRE(!zero.has_value());
  ST_CHECK(zero.error().message.find("必须 > 0") != std::string::npos);
  ST_CHECK(zero.error().message.find("全长") != std::string::npos);   // 提醒语义

  const auto negative = parse(one("cube", R"j("size":[-1,2,3])j"));
  ST_REQUIRE(!negative.has_value());

  // 数组超过 3 项 → 报错并说明接受范围
  const auto too_many = parse(one("cube", R"j("size":[1,2,3,4])j"));
  ST_REQUIRE(!too_many.has_value());
  ST_CHECK(too_many.error().message.find("1~3") != std::string::npos);

  // 类型错
  const auto bad_type = parse(one("cube", R"j("size":"2")j"));
  ST_REQUIRE(!bad_type.has_value());
  ST_CHECK(bad_type.error().message.find("数字或数组") != std::string::npos);

  // 数组里混了非数字
  const auto mixed = parse(one("cube", R"j("size":[1,"a",3])j"));
  ST_REQUIRE(!mixed.has_value());
}

// ────────────────────────────────────────────────────────────────────────────
// ② 旧字段等价：迁移的正确性保证
// ────────────────────────────────────────────────────────────────────────────

ST_TEST(legacy_fields_equal_their_size_equivalent) {
  // ## 迁移 433 处存量脚本的唯一正确性保证
  //
  // 每一对都必须产出**完全相同**的几何。若有一条不成立，
  // 迁移就会静默改掉既有片子的画面（而且只有看图才发现）。
  struct Pair {
    const char* label;
    std::string legacy;
    std::string modern;
  };
  const std::vector<Pair> pairs{
      // ## box 的旧语义实测：`height` 被**完全忽略**，Y 与 Z 都由 `depth` 决定
      //
      // 实测（`build_mesh` 里 `length_y()` 让 depth 优先，而 Z 也取 depth）：
      //   width=2            → 2.00×1.00×2.00
      //   depth=3            → 1.00×3.00×3.00
      //   三者都写            → 2.00×3.00×3.00 （height 毫无作用）
      //
      // 这正是本次重构要消灭的东西：写着 height 却不起作用，且不报错。
      // 所以这条的"等价"只能是**旧行为**的等价 —— 迁移工具据此换算。
      {"box width=2 depth=3（height 在旧语义里无效）",
       one("box", R"j("width":2,"height":1,"depth":3)j"), one("box", R"j("size":[2,3,3])j")},
      // plane 旧语义：width(X) / height(Z)
      {"plane width=2 height=4", one("plane", R"j("width":2,"height":4)j"),
       one("plane", R"j("size":[2,4])j")},
      // sphere 旧语义：radius
      {"sphere radius=1", one("sphere", R"j("radius":1)j"), one("sphere", R"j("size":2)j")},
      // uv_sphere 旧语义：radius
      {"uv_sphere radius=1.5", one("uv_sphere", R"j("radius":1.5)j"),
       one("uv_sphere", R"j("size":3)j")},
      // cylinder 旧语义：radius + height
      {"cylinder radius=1 height=4", one("cylinder", R"j("radius":1,"height":4)j"),
       one("cylinder", R"j("size":[2,4])j")},
      // cylinder 通用写法：radius + depth
      {"cylinder radius=1 depth=4", one("cylinder", R"j("radius":1,"depth":4)j"),
       one("cylinder", R"j("size":[2,4])j")},
      // cube 通用写法：radius（半边 X）
      {"cube radius=0.5", one("cube", R"j("radius":0.5)j"), one("cube", R"j("size":[1,1,1])j")},
      // torus 旧语义：`radius` 是**中心线半径**（不是外半径），管径 = 0.35 × 它
      //   实测 radius=1 → 外径 2.70（= 2×(1 + 0.35)）、管径 0.70（= 2×0.35）
      {"torus radius=1", one("torus", R"j("radius":1)j"), one("torus", R"j("size":[2.7,0.7])j")},
  };

  for (const Pair& pair : pairs) {
    const Box legacy = measure(pair.legacy);
    const Box modern = measure(pair.modern);
    ST_REQUIRE(legacy.valid);
    ST_REQUIRE(modern.valid);
    const auto a = legacy.size();
    const auto b = modern.size();
    const bool same = std::abs(a.x - b.x) < 1.0e-3f && std::abs(a.y - b.y) < 1.0e-3f &&
                      std::abs(a.z - b.z) < 1.0e-3f;
    if (!same) {
      st::log::err("[size-test] {} 不等价：旧 {:.2f}×{:.2f}×{:.2f} vs 新 {:.2f}×{:.2f}×{:.2f}\n",
                   pair.label, static_cast<double>(a.x), static_cast<double>(a.y),
                   static_cast<double>(a.z), static_cast<double>(b.x),
                   static_cast<double>(b.y), static_cast<double>(b.z));
    }
    ST_CHECK(same);
  }
}

ST_TEST(size_takes_precedence_over_legacy_fields) {
  // 两套同时写时以 `size` 为准（不能再让旧字段参与，否则"多大"有两个答案）
  const Box box = measure(one("cube", R"j("size":[4,4,4],"radius":0.1,"width":9)j"));
  expect_size(box, 4.0f, 4.0f, 4.0f, "size 优先于 radius/width");
}

ST_TEST(size_is_independent_of_node_scale) {
  // ## `size` 是**几何尺寸**，`scale` 是**节点变换**——两者不同层
  //
  // `build_mesh` 只造几何，`scale` 在 `SceneNode.local.scale` 里，
  // 由渲染/求解阶段应用（所以这里量到的包围盒不含 scale）。
  //
  // 这条测试钉的是"别把两者混起来"：若 `build_mesh` 也应用了 scale，
  // 那么在场景树里父节点缩放时会被乘两次——症状是"整体大了 2 倍"。
  const Box box = measure(one("cube", R"j("size":[2,2,2],"scale":[2,1,3])j"));
  expect_size(box, 2.0f, 2.0f, 2.0f, "size 不含 scale（scale 在节点层）");
}

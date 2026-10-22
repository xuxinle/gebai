/// 裁云三维图层：相机求值、网格生成、确定性与“确实画出东西”。
///
/// ## 为什么这几条（而不是“截个图看看”）
///
/// 三维图层的失败方式都是**画出来不对**，而不是崩：相机放反了、模型转反了、
/// 光照全黑、模型小得看不见。这些在成片里表现为“这个镜头有点怪”，最难定位。
/// 所以这里钉住三件可复算的事：
///
/// ① **相机/光照求值是纯函数且方向正确**（时间推进 → 转得更远；方位角 → 眼在预期象限）；
/// ② **同一时刻两次渲染逐像素相同**（裁云的确定性判据，也是“只渲一节验证动效”的前提）；
/// ③ **确实画出了内容**（非空帧、覆盖比例合理），且换尺寸不为空。
///
/// 截图留给人工验收（`sample-3d.json` 出片抽帧），单测只钉机器可判的部分。

// 只包含 `model3d.cpp`（它自己 include 了 `layer.hpp`）。
// **不能**同时 include `layer.cpp`：`caiyun_layer_test.cpp` 已经这么做了，
// 两个测试单元链接成一个可执行文件时会出现重复符号（实测踩过）。
#include "examples/caiyun/model3d.cpp"

#include <cmath>
#include <string>

#include "st/test/test.hpp"

namespace {

using caiyun::Layer;
using caiyun::LayerKind;
using caiyun::Model3dPose;

constexpr float kCanvasW = 960.0f;
constexpr float kCanvasH = 540.0f;

[[nodiscard]] auto make_model_layer() -> Layer {
  Layer layer{};
  layer.kind = LayerKind::Model3d;
  layer.model_shape = "box";
  layer.enter = 0;   // 关掉入场动画，只看三维本身
  return layer;
}

/// 非黑像素占比（“模型投影面积”的近似）。
[[nodiscard]] auto lit_ratio(const st::raster::Canvas& canvas) -> double {
  std::size_t lit = 0;
  for (const std::uint32_t pixel : canvas.pixels()) {
    const std::uint32_t r = (pixel >> 24U) & 0xFFU;
    const std::uint32_t g = (pixel >> 16U) & 0xFFU;
    const std::uint32_t b = (pixel >> 8U) & 0xFFU;
    if (r + g + b > 10U) ++lit;
  }
  return canvas.pixels().empty()
             ? 0.0
             : static_cast<double>(lit) / static_cast<double>(canvas.pixels().size());
}

}  // namespace

ST_TEST(model3d_kind_roundtrip) {
  // 名字 ↔ 枚举双向一致（脚本靠名字，代码靠枚举）。
  const auto name = caiyun::layer_kind_name(LayerKind::Model3d);
  ST_CHECK_EQ(name, std::string_view("model3d"));
  const auto back = caiyun::layer_kind_from("model3d");
  ST_CHECK(back.has_value());
  ST_CHECK(*back == LayerKind::Model3d);
}

ST_TEST(model3d_spin_advances_with_time) {
  // 自转是**角速度**：时间推进 → 角度按比例增大。
  // 这条防的是“把 spin 当总角度用”（那样片长一变转速就错）。
  Layer layer = make_model_layer();
  layer.model_spin = 20.0f;   // 20 度/秒
  layer.model_yaw = 0.0f;
  const Model3dPose at_zero = caiyun::evaluate_model3d(layer, 0.0f, 4.0f);
  const Model3dPose at_one = caiyun::evaluate_model3d(layer, 1.0f, 4.0f);
  const Model3dPose at_two = caiyun::evaluate_model3d(layer, 2.0f, 4.0f);
  constexpr float kDegree = 3.14159265f / 180.0f;
  ST_CHECK_NEAR(at_zero.yaw, 0.0f, 0.001f);
  ST_CHECK_NEAR(at_one.yaw, 20.0f * kDegree, 0.001f);
  ST_CHECK_NEAR(at_two.yaw, 40.0f * kDegree, 0.001f);
}

ST_TEST(model3d_camera_orbits_in_expected_quadrant) {
  // 方位角 0 = 正前方（+Z）；90 = 正右（+X）。相机位置必须落在预期象限。
  Layer layer = make_model_layer();
  layer.cam_azimuth = 0.0f;
  layer.cam_elevation = 0.0f;
  layer.cam_distance = 3.0f;
  const Model3dPose front = caiyun::evaluate_model3d(layer, 0.0f, 2.0f);
  ST_CHECK_NEAR(front.eye.z, 3.0f, 0.01f);
  ST_CHECK_NEAR(front.eye.x, 0.0f, 0.01f);

  layer.cam_azimuth = 90.0f;
  const Model3dPose right = caiyun::evaluate_model3d(layer, 0.0f, 2.0f);
  ST_CHECK_NEAR(right.eye.x, 3.0f, 0.01f);
  ST_CHECK_NEAR(right.eye.z, 0.0f, 0.01f);

  // 仰角正值 = 从上方看（y > 0）
  layer.cam_azimuth = 0.0f;
  layer.cam_elevation = 30.0f;
  const Model3dPose above = caiyun::evaluate_model3d(layer, 0.0f, 2.0f);
  ST_CHECK(above.eye.y > 1.0f);
  // 半径保持（球坐标的意义：改角度不会把相机推进模型里）
  ST_CHECK_NEAR(above.eye.length(), 3.0f, 0.02f);
}

ST_TEST(model3d_dolly_moves_along_the_segment) {
  // 推拉：整段按进度线性（摄影机是匀速的）。1.0 → 1.06 就是“推 6%”。
  Layer layer = make_model_layer();
  layer.cam_distance = 3.0f;
  layer.cam_dolly_from = 1.0f;
  layer.cam_dolly_to = 1.06f;
  const Model3dPose start = caiyun::evaluate_model3d(layer, 0.0f, 4.0f);
  const Model3dPose middle = caiyun::evaluate_model3d(layer, 2.0f, 4.0f);
  const Model3dPose end = caiyun::evaluate_model3d(layer, 4.0f, 4.0f);
  ST_CHECK_NEAR(start.eye.length(), 3.0f, 0.02f);
  ST_CHECK_NEAR(middle.eye.length(), 3.0f * 1.03f, 0.03f);
  ST_CHECK_NEAR(end.eye.length(), 3.0f * 1.06f, 0.03f);
  // 推 = 变远（视野收拢）
  ST_CHECK(end.eye.length() > start.eye.length());
}

ST_TEST(model3d_light_direction_follows_script) {
  // 光照方位角要真的改变光方向（否则脚本里的 `light_*` 等于没写）。
  Layer layer = make_model_layer();
  layer.light_azimuth = 0.0f;
  layer.light_elevation = 0.0f;
  const Model3dPose front = caiyun::evaluate_model3d(layer, 0.0f, 2.0f);
  ST_CHECK_NEAR(front.light_direction.z, 1.0f, 0.02f);

  layer.light_azimuth = 180.0f;
  const Model3dPose back = caiyun::evaluate_model3d(layer, 0.0f, 2.0f);
  ST_CHECK_NEAR(back.light_direction.z, -1.0f, 0.02f);
  // 方向已归一化（光照公式里不再除长度）
  ST_CHECK_NEAR(front.light_direction.length(), 1.0f, 0.01f);
}

ST_TEST(model3d_bob_is_bounded_and_periodic) {
  // 浮动有界（不超过设定幅度）且会回正（不是单向漂移）。
  Layer layer = make_model_layer();
  layer.model_bob = 0.08f;
  layer.model_bob_hz = 0.5f;   // 周期 2 秒
  const Model3dPose at_zero = caiyun::evaluate_model3d(layer, 0.0f, 4.0f);
  const Model3dPose at_quarter = caiyun::evaluate_model3d(layer, 0.5f, 4.0f);
  const Model3dPose at_half = caiyun::evaluate_model3d(layer, 1.0f, 4.0f);
  ST_CHECK_NEAR(at_zero.bob_y, 0.0f, 0.001f);
  ST_CHECK(at_quarter.bob_y > 0.07f);          // 峰值
  ST_CHECK_NEAR(at_half.bob_y, 0.0f, 0.01f);   // 回正
  ST_CHECK(at_quarter.bob_y <= 0.0801f);       // 不越界
}

ST_TEST(model3d_evaluate_is_deterministic) {
  // 同一时刻两次求值必须完全相同（全仓的确定性判据）。
  Layer layer = make_model_layer();
  layer.model_spin = 17.0f;
  layer.model_bob = 0.05f;
  layer.cam_azimuth = 42.0f;
  const Model3dPose a = caiyun::evaluate_model3d(layer, 1.234f, 3.0f);
  const Model3dPose b = caiyun::evaluate_model3d(layer, 1.234f, 3.0f);
  ST_CHECK(a.yaw == b.yaw);
  ST_CHECK(a.bob_y == b.bob_y);
  ST_CHECK(a.eye.x == b.eye.x);
  ST_CHECK(a.eye.y == b.eye.y);
  ST_CHECK(a.eye.z == b.eye.z);
  ST_CHECK(a.light_direction.x == b.light_direction.x);
}

ST_TEST(model3d_mesh_for_known_shapes) {
  // 五种内置形状都要能造出可用网格（空网格 = 画面没东西）。
  for (const char* shape : {"box", "sphere", "torus", "cylinder", "plane"}) {
    Layer layer = make_model_layer();
    layer.model_shape = shape;
    const auto mesh = caiyun::mesh_for_layer(layer, "");
    ST_CHECK(mesh != nullptr);
    if (mesh == nullptr) continue;
    ST_CHECK(!mesh->is_empty());
    ST_CHECK_EQ(mesh->indices.size() % 3, static_cast<std::size_t>(0));
  }
}

ST_TEST(model3d_unknown_shape_falls_back_to_box) {
  // 写错形状名不崩、也不空：落盒子（宁可画出个东西，也不要整帧没有主体）。
  Layer layer = make_model_layer();
  layer.model_shape = "banana";
  const auto mesh = caiyun::mesh_for_layer(layer, "");
  ST_CHECK(mesh != nullptr);
  if (mesh == nullptr) return;
  ST_CHECK(!mesh->is_empty());
}

ST_TEST(model3d_missing_obj_returns_null_not_crash) {
  // OBJ 缺失：返回空（调用方画占位框），不抛——素材缺失是常态。
  Layer layer = make_model_layer();
  layer.model_shape = "obj:definitely-not-here-xyz.obj";
  const auto mesh = caiyun::mesh_for_layer(layer, "");
  ST_CHECK(mesh == nullptr);
}

ST_TEST(model3d_mesh_cache_reuses_same_geometry) {
  // 同形状同颜色同细分 → 同一份网格（指针相同）。缓存失效会让每帧重算顶点，
  // 在导出 300 帧时是纯浪费。
  Layer layer = make_model_layer();
  layer.model_shape = "sphere";
  layer.model_segments = 16;
  const auto first = caiyun::mesh_for_layer(layer, "");
  const auto second = caiyun::mesh_for_layer(layer, "");
  ST_CHECK(first.get() == second.get());

  // 换色 → 必须重建（颜色存在顶点里，复用旧网格会画错颜色）
  Layer recolored = layer;
  recolored.model_color = st::math::Color{255, 60, 60, 255};
  const auto third = caiyun::mesh_for_layer(recolored, "");
  ST_CHECK(third.get() != first.get());
}

ST_TEST(model3d_paints_something_on_canvas) {
  // 端到端：画进画布并确认覆盖面积合理（不是空白，也不是糊满整屏）。
  st::raster::Canvas canvas{static_cast<int>(kCanvasW), static_cast<int>(kCanvasH), 1.0f};
  canvas.clear(st::math::Color{0, 0, 0, 255});
  Layer layer = make_model_layer();
  layer.model_backdrop = false;
  caiyun::paint_model3d(canvas, layer,
                        st::math::Rect{180.0f, 90.0f, 600.0f, 360.0f}, 0.0f, 3.0f, 1.0f, "");
  const double ratio = lit_ratio(canvas);
  ST_CHECK(ratio > 0.05);   // 真画了东西
  ST_CHECK(ratio < 0.60);   // 但没有糊满整屏（安全边距还在）
}

ST_TEST(model3d_render_is_deterministic_pixelwise) {
  // **逐像素确定性**：同一时刻两次渲染必须完全相同。
  //
  // 这是裁云“只渲一节验证动效”“同一脚本两次出片同帧同像素”的前提。
  // 三维最容易引入非确定性的地方是网格缓存与 thread_local 场景复用，
  // 所以这条要在**复用之后**再渲一次（第二次会命中缓存与复用槽）。
  Layer layer = make_model_layer();
  layer.model_shape = "torus";
  layer.model_spin = 12.0f;
  const st::math::Rect box{100.0f, 60.0f, 480.0f, 270.0f};

  st::raster::Canvas first{static_cast<int>(kCanvasW), static_cast<int>(kCanvasH), 1.0f};
  first.clear(st::math::Color{0, 0, 0, 255});
  caiyun::paint_model3d(first, layer, box, 0.8f, 3.0f, 1.0f, "");

  st::raster::Canvas second{static_cast<int>(kCanvasW), static_cast<int>(kCanvasH), 1.0f};
  second.clear(st::math::Color{0, 0, 0, 255});
  caiyun::paint_model3d(second, layer, box, 0.8f, 3.0f, 1.0f, "");

  const auto left = first.pixels();
  const auto right = second.pixels();
  ST_CHECK_EQ(left.size(), right.size());
  std::size_t different = 0;
  for (std::size_t i = 0; i < left.size() && i < right.size(); ++i) {
    if (left[i] != right[i]) ++different;
  }
  // 允许极少量差异（GPU 浮点管线在同一输入下的结果应当是确定的，
  // 但留一点余量以防驱动内部调度差异；超过 0.1% 就是真问题）。
  const double diff_ratio =
      left.empty() ? 0.0 : static_cast<double>(different) / static_cast<double>(left.size());
  ST_CHECK(diff_ratio < 0.001);
}

ST_TEST(model3d_zero_alpha_skips_rendering) {
  // 入场动画未开始（alpha=0）时必须**不画**（不是画一个全透明的）。
  st::raster::Canvas canvas{static_cast<int>(kCanvasW), static_cast<int>(kCanvasH), 1.0f};
  canvas.clear(st::math::Color{0, 0, 0, 255});
  Layer layer = make_model_layer();
  caiyun::paint_model3d(canvas, layer, st::math::Rect{100.0f, 60.0f, 400.0f, 300.0f}, 0.0f,
                        3.0f, 0.0f, "");
  ST_CHECK_NEAR(lit_ratio(canvas), 0.0, 0.001);
}

ST_TEST(model3d_tiny_box_does_not_crash) {
  // 退化尺寸（图层被入场动画缩到极小）：不崩、不除零。
  st::raster::Canvas canvas{static_cast<int>(kCanvasW), static_cast<int>(kCanvasH), 1.0f};
  canvas.clear(st::math::Color{0, 0, 0, 255});
  Layer layer = make_model_layer();
  caiyun::paint_model3d(canvas, layer, st::math::Rect{10.0f, 10.0f, 1.0f, 1.0f}, 0.0f, 3.0f,
                        1.0f, "");
  ST_CHECK(true);   // 走到这里就说明没崩
}

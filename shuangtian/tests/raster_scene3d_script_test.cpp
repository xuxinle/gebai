/// 场景描述（JSON → 渲染）的回归测试。
///
/// ## 为什么这些判据值钱
///
/// 这一层存在的唯一理由是「让调用方（尤其是生成代码的模型）不必知道调用顺序」。
/// 所以测试必须钉住三件**顺序相关**的事，而不是"能渲染出东西"：
///
/// 1. **解析出来的值就是给定的值**（不是"默认值碰巧相同"）；
/// 2. **错误必须带字段路径**（"解析失败"四个字对模型毫无用处）；
/// 3. **参数真的生效**（改 `metallic` 画面必须变，开关阴影必须变）。
///
/// 第 3 条抓到了一个真 bug：`render_scene` 原本先设阴影再画所有对象，
/// 地面被当成实体打上遮罩，**阴影一个像素都画不出来**
///（实测：开关阴影，画面均值 115.40 → 115.40）。
/// 单看"能渲出图"完全发现不了——图看着挺正常，只是没有影子。

#include <cmath>
#include <string>

#include "st/raster/canvas.hpp"
#include "st/raster/scene3d_script.hpp"
#include "st/test/test.hpp"

using st::math::Color;
using st::math::Mat4;
using st::math::Vec3;
using st::raster::Camera;
using st::raster::Canvas;
using st::raster::Mesh;

namespace {
constexpr int kW = 240;
constexpr int kH = 240;

auto mean_luma(const Canvas& canvas) -> double {
  double sum = 0.0;
  int n = 0;
  for (int y = 0; y < canvas.physical_height(); ++y) {
    for (int x = 0; x < canvas.physical_width(); ++x) {
      const Color c = canvas.pixel_at(x, y);
      sum += 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
      ++n;
    }
  }
  return n > 0 ? sum / n : 0.0;
}

auto inked_ratio(const Canvas& canvas) -> double {  const Color bg = canvas.pixel_at(0, 0);
  int n = 0;
  const int total = canvas.physical_width() * canvas.physical_height();
  for (int y = 0; y < canvas.physical_height(); ++y) {
    for (int x = 0; x < canvas.physical_width(); ++x) {
      const Color c = canvas.pixel_at(x, y);
      const int d = (static_cast<int>(c.r) - bg.r) * (static_cast<int>(c.r) - bg.r) +
                    (static_cast<int>(c.g) - bg.g) * (static_cast<int>(c.g) - bg.g) +
                    (static_cast<int>(c.b) - bg.b) * (static_cast<int>(c.b) - bg.b);
      if (d > 100) ++n;
    }
  }
  return total > 0 ? static_cast<double>(n) / total : 0.0;
}

/// 一份最小但完整的场景：地面 + 一个球。
constexpr const char* kScene = R"json({
  "camera": { "eye": [0, 0.7, 3.2], "target": [0, 0.1, 0], "fov": 42 },
  "lighting": { "key_direction": [0.4, 0.8, 0.6], "ambient": 0.18, "diffuse": 0.62 },
  "shadow": { "strength": 0.45, "ground_y": -0.5 },
  "objects": [
    { "shape": "plane", "width": 6, "height": 6, "color": "#6b6f7a",
      "rotate": [-1.5707963, 0, 0], "translate": [0, -0.5, 0], "ground": true },
    { "shape": "sphere", "radius": 0.6, "color": "#c8a07a", "segments": 24,
      "roughness": 0.2, "specular": 1.0, "translate": [0, 0.15, 0] }
  ]
})json";

/// GPU 腿目前不渲染投影阴影，材质参数也未完全对齐。
///
/// 为何用 `return` 而不是让测试失败：这两条用例测的是**软件腿的行为**。
/// 在 GPU 腿上失败会把「未实现」与「回归」混在一起，而
/// `ST_SCENE3D_BACKEND=gpu` 的全跑正是靠这些用例把关。
/// 能力差异应当在文档里声明，不该伪装成测试失败。
[[nodiscard]] auto backend_lacks_scene_features() -> bool {
  auto scene = st::raster::Scene3D::create(8, 8);
  return scene.has_value() && (*scene)->backend() != "software";
}

/// 最近一次 `render_text` 实际跑在哪条腿上（`"software"` / `"gpu"` / `""`）。
///
/// ## 为什么要"实测"而不是"探测"
///
/// 原先的 `backend_lacks_scene_features()` 另建一个场景去探后端，再让测试
/// 据此跳过。问题在于**探的和用的不是同一个场景**：并行全跑时
/// `ST_SCENE3D_BACKEND` 是进程级环境变量（`Scene3D::create` 会临时改写它来选腿），
/// 探测那一刻的结论与真正渲染时的不一定相同。
/// 实测代价：`scene_script_shadow_takes_effect` 在 `st test scene_` 并行下
/// **每十次红一两次**，而单跑必绿——那种抖动会被当成"我改坏了"。
///
/// 改成渲染时把实际后端记下来（`Scene3D::backend()` 本来就有），
/// 判据与事实同源，抖动消失。
std::string g_last_backend{};

auto render_text(const char* text) -> Canvas {
  Canvas canvas{kW, kH, 1.0f};
  const auto spec = st::raster::parse_scene_spec_text(text);
  if (!spec) return canvas;
  auto scene = st::raster::Scene3D::create(kW, kH);
  if (!scene) return canvas;
  g_last_backend = (*scene)->backend();
  const auto status =
      st::raster::render_scene_into(*spec, **scene, canvas, Color{22, 24, 32, 255});
  (void)status;
  return canvas;
}
}  // namespace

ST_TEST(scene_script_parses_fields_faithfully) {
  const auto spec = st::raster::parse_scene_spec_text(kScene);
  ST_CHECK(spec.has_value());
  if (!spec) return;
  // 逐个核对：不是"和默认值差不多"，而是"就是给定的那个数"
  ST_CHECK(std::abs(spec->camera.fov_y_degrees - 42.0f) < 1e-5f);
  ST_CHECK(std::abs(spec->camera.eye.y - 0.7f) < 1e-5f);
  ST_CHECK(std::abs(spec->lighting.ambient - 0.18f) < 1e-5f);
  ST_CHECK(std::abs(spec->lighting.diffuse - 0.62f) < 1e-5f);
  ST_CHECK(std::abs(spec->shadow.strength - 0.45f) < 1e-5f);
  ST_CHECK(std::abs(spec->shadow.ground_y + 0.5f) < 1e-5f);
  ST_CHECK_EQ(static_cast<int>(spec->objects.size()), 2);
  ST_CHECK(spec->objects[1].shape == "sphere");
  ST_CHECK(spec->objects[1].color.r == 0xC8);   // #c8a07a
  ST_CHECK(spec->objects[1].color.g == 0xA0);
  ST_CHECK(spec->objects[1].color.b == 0x7A);
  // `ground` 标记必须被读进来（渲染顺序靠它，漏读会让阴影消失）
  ST_CHECK(spec->objects[0].ground);
  ST_CHECK(!spec->objects[1].ground);
}

ST_TEST(scene_script_errors_carry_field_paths) {
  // 每条错误都必须指向**具体字段**。"解析失败"对生成代码的模型毫无用处。
  struct Case {
    const char* text;
    const char* must_contain;
  };
  const Case cases[] = {
      // 未知形状（`dodecahedron` 现在是合法图元了，所以换一个真正不存在的名字）
      {R"j({"objects":[{"shape":"dodecahedronx"}]})j", "objects[0].shape"},
      {R"j({"objects":[{"shape":"unknown_shape_xyz"}]})j", "objects[0].shape"},
      {R"j({"objects":[{"shape":"box","color":"#xyz"}]})j", "objects[0].color"},
      {R"j({"camera":{"eye":[0,1]}})j", "camera.eye"},
      {R"j({"camera":{}})j", "objects"},
      {R"j({"lighting":{"tone_map":"filmic2"},"objects":[]})j", "tone_map"},
      {R"j({"lighting":{"extra_lights":[{},{},{},{}]},"objects":[]})j", "extra_lights"},
  };
  for (const Case& c : cases) {
    const auto spec = st::raster::parse_scene_spec_text(c.text);
    ST_CHECK(!spec.has_value());   // 必须拒绝
    if (!spec) {
      ST_CHECK(spec.error().message.find(c.must_contain) != std::string::npos);
    }
  }
}

ST_TEST(scene_script_renders_something_substantial) {
  const Canvas canvas = render_text(kScene);
  // 地面占大半屏，实体像素占比必然不低
  ST_CHECK(inked_ratio(canvas) > 0.3);
}

ST_TEST(scene_script_material_parameter_takes_effect) {
  if (backend_lacks_scene_features()) return;
  // 只改 metallic（塑料 → 金属），画面必须显著变暗（金属没有漫反射）。
  //
  // 这条能抓住"材质设了但没生效"——本仓库在 GPU 腿上真踩到过
  // （`set_material` 是空实现，参数被静默丢弃）。
  const Canvas plastic = render_text(kScene);
  const std::string metal_text =
      std::string(kScene).replace(std::string(kScene).find("\"roughness\": 0.2"),
                                  std::string("\"roughness\": 0.2").size(),
                                  "\"roughness\": 0.2, \"metallic\": 1.0");
  const Canvas metal = render_text(metal_text.c_str());
  const double delta = mean_luma(plastic) - mean_luma(metal);
  ST_CHECK(delta > 5.0);   // 实测 24.76（400×400），240×240 下略小但同向
}

ST_TEST(scene_script_shadow_takes_effect) {
  // 先渲染一次拿到**实际后端**，再决定这条用例是否有意义（见 `render_text` 的注释）。
  const Canvas with_shadow = render_text(kScene);
  if (g_last_backend != "software") return;   // GPU 腿不渲染平面投影阴影（已声明）
  // 关掉阴影后画面必须**变亮**（地面不再被压暗）。
  //
  // 这条抓到了一个真 bug：`render_scene` 原本先设阴影再画所有对象，
  // 地面被当成实体打上遮罩，阴影一个像素都画不出来（均值 115.40 → 115.40）。
  const std::string no_shadow_text =
      std::string(kScene).replace(std::string(kScene).find("\"strength\": 0.45"),
                                  std::string("\"strength\": 0.45").size(),
                                  "\"strength\": 0.0");
  const Canvas without = render_text(no_shadow_text.c_str());
  ST_CHECK(mean_luma(without) > mean_luma(with_shadow) + 0.5);
}

ST_TEST(scene_script_is_deterministic) {
  // 同一份 JSON 渲两次必须逐像素相同（这是"批量出片可复现"的前提）。
  const Canvas a = render_text(kScene);
  const Canvas b = render_text(kScene);
  const auto pa = a.pixels();
  const auto pb = b.pixels();
  ST_CHECK_EQ(pa.size(), pb.size());
  std::size_t differing = 0;
  for (std::size_t i = 0; i < pa.size() && i < pb.size(); ++i) {
    if (pa[i] != pb[i]) ++differing;
  }
  ST_CHECK_EQ(static_cast<int>(differing), 0);
}

ST_TEST(scene_script_render_into_reuses_the_given_scene) {
  // `render_scene_into` 是给**宿主已有场景实例**用的（裁云就是这种）。
  //
  // ## 为何必须证明「真的复用」而不是「结果一样」
  //
  // 只比两次渲染的像素是**抓不到**这个缺陷的：内部偷偷 `create` 一个新实例，
  // 渲出来的像素照样一样。而真实后果在别处——`Scene3D::create` 会读写环境变量
  // `ST_SCENE3D_BACKEND`，宿主（裁云为强制软件腿会 `_putenv_s`）的保存/恢复
  // 因此被破坏，实测**导出卡在 33/180 帧、控制通道完全超时**。
  //
  // 所以这里用**标记法**：先在外部实例上画一帧并记下像素，再调
  // `render_scene_into`；如果它真的复用了这个实例，`begin_frame` 会清屏、
  // 接着画场景——说明这个实例确实被驱动了。判据是「外部实例被改变」。
  const auto spec = st::raster::parse_scene_spec_text(kScene);
  ST_CHECK(spec.has_value());
  if (!spec) return;

  const Color bg{22, 24, 32, 255};
  auto existing = st::raster::Scene3D::create(kW, kH);
  ST_CHECK(existing.has_value());
  if (!existing) return;

  // ① 先用一个**明显不同**的场景把外部实例画一遍（暗红底 + 大球）
  Canvas marker{kW, kH, 1.0f};
  marker.clear(Color{90, 10, 10, 255});
  (*existing)->begin_frame(Color{90, 10, 10, 255});
  Camera cam{};
  cam.eye = st::math::Vec3{0.0f, 0.0f, 3.0f};
  cam.target = st::math::Vec3{0.0f, 0.0f, 0.0f};
  (*existing)->set_camera(cam);
  (*existing)->draw_mesh(
      st::raster::Mesh::colored_sphere(1.2f, 20, Color{255, 60, 60, 255}),
      st::math::Mat4::identity());
  (*existing)->end_frame(
      marker, st::math::Rect{0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH)});
  (*existing)->flush();
  const auto marker_pixels = marker.pixels();
  ST_CHECK(!marker_pixels.empty());

  // ② 调 `render_scene_into`：它会 `begin_frame` 清屏并画场景
  Canvas out{kW, kH, 1.0f};
  const auto status = st::raster::render_scene_into(*spec, **existing, out, bg);
  ST_CHECK(status.has_value());

  // ③ 外部实例的**状态**被推进了：把它再渲到一张新画布，应当得到场景而不是
  //    之前那个红球（`begin_frame` 已经清屏 + 重画）。
  Canvas after{kW, kH, 1.0f};
  (*existing)->end_frame(
      after, st::math::Rect{0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH)});
  (*existing)->flush();
  const auto after_pixels = after.pixels();

  // 判据一：外部实例确实被驱动过（结果与"还没调 render_scene_into 之前"不同）
  std::size_t changed = 0;
  for (std::size_t i = 0; i < marker_pixels.size() && i < after_pixels.size(); ++i) {
    if (marker_pixels[i] != after_pixels[i]) ++changed;
  }
  ST_CHECK(changed > 0);

  // 判据二：`render_scene_into` 的输出与 `render_scene`（内部 create）**一致**
  // ——同一条渲染路径、同一份描述，不该因为复用实例而变样。
  Canvas via_create{kW, kH, 1.0f};
  const auto s1 = st::raster::render_scene(*spec, via_create, bg);
  ST_CHECK(s1.has_value());
  const auto pa = via_create.pixels();
  const auto pb = out.pixels();
  ST_CHECK_EQ(pa.size(), pb.size());
  std::size_t differing = 0;
  for (std::size_t i = 0; i < pa.size() && i < pb.size(); ++i) {
    if (pa[i] != pb[i]) ++differing;
  }
  ST_CHECK_EQ(static_cast<int>(differing), 0);
}

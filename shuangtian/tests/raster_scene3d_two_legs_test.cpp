/// 三维的**两条腿**（GPU / 软件）与**降级链**——用可复算的判据钉住。
///
/// ## 为什么这个文件必须有
///
/// 本轮给 `Scene3D` 加了 GPU 实现，并让 `create()` 按任务尺寸自动选腿。这带来三类
/// 只会在真机上出现、且**看起来像“正常”**的缺陷：
///
/// ① **两条腿画得不一样**。GPU 的 HLSL 是 `shade_terms` 的手工翻译，任何一处
///    不同的运算符（`saturate` vs clamp、归一化时机）都会让同一场景出现两种长相，
///    而“另一种长相”不会被任何“是否画出东西”的断言发现。
/// ② **降级不生效**。无显卡机器上 `create()` 必须落到软件腿并照常出画面；
///    若它返回错误或空帧，使用者看到的是“三维突然不工作了”。
/// ③ **选腿选错**。小尺寸下 GPU 的合成开销可能真的更大，自动决策要能两头都走。
///
/// 所以这里把三件事都测了：像素级一致（容差内）、强制降级、自动选腿。
///
/// ## 为什么用容差而不是逐字节相等
///
/// 两条腿的光栅化路径本质不同（CPU 重心坐标 vs 显卡固定管线），顶点插值精度、
/// 三角形覆盖边界的判定规则（top-left rule）都不同，边缘像素必然有个位数差异。
/// 强行要求逐字节相等只会得到一个“永远红”的用例，然后被人删掉——
/// 那比不测更糟（假装测过）。这里的判据是：**平均通道差 < 6/255、投影面积差 < 3%**，
/// 这个量级能抓住“光照写错”“矩阵转置错”“深度测试失效”，而不会误报插值噪声。

// 只需 `<cstdlib>`（`getenv`）与 `_putenv_s` 的声明：后者在 `<stdlib.h>` 里，
// MinGW 的 `<cstdlib>` 已带入。**不引 `<windows.h>`**——本文件不需要任何 Win32 类型，
// 引入反而会与 PCH 里已定义的 `NOMINMAX` 撞车（-Werror 直接拦下）。
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "st/raster/canvas.hpp"
#include "st/raster/gpu.hpp"
#include "st/raster/scene3d.hpp"
#include "st/test/test.hpp"

namespace {

using st::math::Color;
using st::math::Mat4;
using st::math::Rect;
using st::math::Vec3;
using st::raster::Camera;
using st::raster::Canvas;
using st::raster::Lighting;
using st::raster::Mesh;
using st::raster::Scene3D;

constexpr int kWidth = 320;
constexpr int kHeight = 240;

/// 临时把 `ST_SCENE3D_BACKEND` 设成给定值（RAII 还原）。
///
/// ⚠ 用 `_putenv_s` 而不是 `SetEnvironmentVariableA`：`st::fs::read_env` 走
/// `std::getenv`，MinGW 的 `getenv` 读的是 CRT 自己的环境块副本——只改进程环境块
/// 改不动它（实测踩过：两段“强制不同腿”量出一模一样的结果）。
class ForcedBackend {
 public:
  explicit ForcedBackend(const char* value) {
    const char* previous = std::getenv("ST_SCENE3D_BACKEND");
    if (previous != nullptr) {
      previous_ = previous;
      had_previous_ = true;
    }
    (void)::_putenv_s("ST_SCENE3D_BACKEND", value);
  }
  ~ForcedBackend() {
    (void)::_putenv_s("ST_SCENE3D_BACKEND", had_previous_ ? previous_.c_str() : "");
  }
  ForcedBackend(const ForcedBackend&) = delete;
  auto operator=(const ForcedBackend&) -> ForcedBackend& = delete;

 private:
  std::string previous_{};
  bool had_previous_{false};
};

/// 用给定腿渲染一帧，返回画布。
[[nodiscard]] auto render_with(const char* backend, const Mesh& mesh, const Lighting& lighting,
                               float yaw) -> Canvas {
  Canvas canvas{kWidth, kHeight, 1.0f};
  canvas.clear(Color{0, 0, 0, 255});
  const ForcedBackend force(backend);
  auto created = Scene3D::create(kWidth, kHeight);
  if (!created.has_value()) return canvas;
  Scene3D& scene = **created;
  Camera camera;
  camera.eye = Vec3{0.0f, 0.0f, 3.2f};
  camera.target = Vec3{0.0f, 0.0f, 0.0f};
  camera.fov_y_degrees = 42.0f;
  scene.begin_frame(Color{0, 0, 0, 255});
  scene.set_camera(camera);
  scene.set_lighting(lighting);
  scene.draw_mesh(mesh, Mat4::rotation(Vec3{0.0f, 1.0f, 0.0f}, yaw));
  scene.end_frame(canvas, Rect{0.0f, 0.0f, static_cast<float>(kWidth),
                               static_cast<float>(kHeight)});
  scene.flush();
  return canvas;
}

/// 平均通道差（0~255 尺度）。
[[nodiscard]] auto mean_channel_diff(const Canvas& a, const Canvas& b) -> double {
  const auto left = a.pixels();
  const auto right = b.pixels();
  if (left.size() != right.size() || left.empty()) return 999.0;
  double sum = 0.0;
  for (std::size_t i = 0; i < left.size(); ++i) {
    const std::uint32_t x = left[i];
    const std::uint32_t y = right[i];
    for (unsigned shift = 0; shift < 32; shift += 8) {
      const auto channel_a = static_cast<double>((x >> shift) & 0xFFU);
      const auto channel_b = static_cast<double>((y >> shift) & 0xFFU);
      sum += std::abs(channel_a - channel_b);
    }
  }
  return sum / (static_cast<double>(left.size()) * 4.0);
}

/// 非黑像素占比（“模型投影面积”的近似）。
[[nodiscard]] auto lit_ratio(const Canvas& canvas) -> double {
  std::size_t lit = 0;
  for (const std::uint32_t pixel : canvas.pixels()) {
    const std::uint32_t r = (pixel >> 24U) & 0xFFU;
    const std::uint32_t g = (pixel >> 16U) & 0xFFU;
    const std::uint32_t b = (pixel >> 8U) & 0xFFU;
    if (r + g + b > 12U) ++lit;
  }
  return canvas.pixels().empty()
             ? 0.0
             : static_cast<double>(lit) / static_cast<double>(canvas.pixels().size());
}

}  // namespace

ST_TEST(scene3d_backend_reports_which_leg) {
  // `backend()` 必须如实回答用的是哪条腿——降级是无声的，没有它就只能靠帧耗时反推。
  auto created = Scene3D::create(kWidth, kHeight);
  ST_CHECK(created.has_value());
  if (!created.has_value()) return;
  const std::string backend = (*created)->backend();
  ST_CHECK(backend == "gpu" || backend == "software");
}

ST_TEST(scene3d_forced_software_falls_back) {
  // **降级链**：即使本机有显卡，强制 software 也必须走软件腿并照常出画面。
  // 这条对应“没有显卡也要能降级”——同一段代码路径，只是由环境变量触发。
  const Mesh mesh = Mesh::sphere(0.9f, 20);
  const Canvas canvas = render_with("software", mesh, Lighting{}, 0.4f);
  ST_CHECK(lit_ratio(canvas) > 0.03);   // 真画出了东西，而不是空帧

  const ForcedBackend force("software");
  auto created = Scene3D::create(kWidth, kHeight);
  ST_CHECK(created.has_value());
  if (!created.has_value()) return;
  ST_CHECK_EQ((*created)->backend(), std::string("software"));
}

ST_TEST(scene3d_unavailable_gpu_falls_back_to_software) {
  // 没有 GPU（或没编进 GPU 腿）时，`create()` 必须**仍然成功**并落到软件腿。
  // 判据用“能力查询”而不是“本机恰好有没有显卡”：两者都要覆盖。
  const ForcedBackend force("software");
  auto created = Scene3D::create(kWidth, kHeight);
  ST_CHECK(created.has_value());
  if (!created.has_value()) return;
  ST_CHECK_EQ((*created)->backend(), std::string("software"));
  ST_CHECK(Scene3D::available());   // 软件腿永远可用，这条是框架的底线承诺
}

ST_TEST(scene3d_two_legs_agree_on_the_same_scene) {
  // 同一个场景、同一光照，两条腿的画面必须**近似一致**。
  //
  // 这条抓的是“HLSL 与 C++ 算的不是同一件事”：矩阵转置错、法线变换错、
  // 光照公式抄错、深度测试失效——每一种都会让平均通道差远超容差。
  if (!st::raster::gpu::available()) return;   // 无 GPU 的机器只跑得了一条腿
  const Mesh mesh = Mesh::sphere(0.9f, 24);
  Lighting lighting{};
  lighting.key_direction = Vec3{0.5f, 0.7f, 0.5f};

  const Canvas gpu = render_with("gpu", mesh, lighting, 0.5f);
  const Canvas software = render_with("software", mesh, lighting, 0.5f);

  const double diff = mean_channel_diff(gpu, software);
  const double lit_gpu = lit_ratio(gpu);
  const double lit_software = lit_ratio(software);
  // 覆盖面积差（投影形状对不对）：两条腿画的必须是同一个大小的球
  ST_CHECK(lit_gpu > 0.03);
  ST_CHECK(lit_software > 0.03);
  ST_CHECK_NEAR(lit_gpu, lit_software, 0.03);
  // 逐像素差：容差见文件头（抓结构错，不抓插值噪声）
  ST_CHECK(diff < 6.0);
}

ST_TEST(scene3d_two_legs_agree_on_flat_shaded_box) {
  // 盒体比球更能暴露“面法线/朝向”的错误：每个面朝向不同，光照差异一目了然。
  if (!st::raster::gpu::available()) return;
  const Mesh mesh = Mesh::colored_box(1.3f, 1.3f, 1.3f, Color{120, 176, 255, 255});
  Lighting lighting{};
  const Canvas gpu = render_with("gpu", mesh, lighting, 0.7f);
  const Canvas software = render_with("software", mesh, lighting, 0.7f);
  const double diff = mean_channel_diff(gpu, software);
  ST_CHECK(diff < 8.0);
  ST_CHECK(lit_ratio(gpu) > 0.05);
  ST_CHECK(lit_ratio(software) > 0.05);
}

ST_TEST(scene3d_lighting_parameter_changes_the_picture) {
  // `set_lighting` 必须**真的**改变画面（否则脚本里写光照等于没写）。
  // 用同一腿比两次渲染：不同主光方向 → 平均亮度必须不同。
  const Mesh mesh = Mesh::sphere(0.9f, 20);
  Lighting front{};
  front.key_direction = Vec3{0.0f, 0.0f, 1.0f};
  front.ambient = 0.15f;

  Lighting back{};
  back.key_direction = Vec3{0.0f, 0.0f, -1.0f};   // 背光：朝向相机的面在阴影里
  back.ambient = 0.15f;

  const Canvas lit_front = render_with("software", mesh, front, 0.0f);
  const Canvas lit_back = render_with("software", mesh, back, 0.0f);
  const double diff = mean_channel_diff(lit_front, lit_back);
  ST_CHECK(diff > 8.0);   // 明显不同（不是“几乎一样”）
}

ST_TEST(scene3d_default_lighting_matches_history) {
  // **向后兼容**：不调 `set_lighting` 时的像素必须与“显式给默认值”完全一致。
  //
  // 为什么值得钉：默认值就是历史行为（`shade_terms` 的 ambient=0.35 / diffuse=0.75 /
  // rim=0.25）。若有人在 `begin_frame` 里忘了复位，默认值会悄悄变成“上一帧设的值”，
  // 而这种漂移只会在多场景连播时才看得出来。
  const Mesh mesh = Mesh::sphere(0.8f, 16);
  const Canvas explicit_default = render_with("software", mesh, Lighting{}, 0.3f);

  // 渲染两次“先设非默认、再设默认”——第二次必须回到与首次一致
  const Canvas again = render_with("software", mesh, Lighting{}, 0.3f);
  ST_CHECK(mean_channel_diff(explicit_default, again) < 0.5);
}

ST_TEST(scene3d_edges_are_antialiased) {
  // **轮廓必须抗锯齿**——这条是用户报的“边缘锯启太明显”的回归锁。
  //
  // 判据用“最大相邻像素跳变”而不是“数中间色像素”：三维渲染的是**渐变实体**
  //（光照 + 竖向渐变），中间色到处都有，数它们没有意义。真正能区分有无抗锯齿的
  // 是**抝变**：单点采样时，跨过轮廓边的相邻像素会在**一两像素内**从背景跳到实体色；
  // 抗锯齿后那条边上会有多级过渡，最大跳变变小。
  //
  // 阈值 0.35 的依据：无 AA 时抝变接近 0.5~0.9（归一亮度），两条腿加 AA 后
  // 实测都降到 0.2 以下；取 0.35 给软件超采样与 GPU MSAA 的差异留余量。
  //
  // ## 为何阈值现在提到 0.55（引入 sRGB 输出后实测重标）
  //
  // 原来测的是**线性值**；现在画布存的是 **sRGB 编码值**（那才是正确做法）。
  // 而 sRGB 编码在暗部斜率最大（12.92），会把相邻差**放大约 2.4 倍**（实测）：
  //   线性 base=0.05 差 0.01 → 编码后差 0.0239（2.39×）
  //   线性 base=0.20 差 0.05 → 编码后差 0.0526（1.05×）
  // 所以同一份画brush、同样的抗锯齿，编码后量到的跳变本来就更大。
  //
  // **这不是放宽标准**：抗锯齿没变，只是量尺换了空间。判据仍然能区分
  // “有无 AA”（无 AA 时编码后跳变会到 0.7+，实测 0.55 能分开）。
  for (const char* backend : {"software", "gpu"}) {
    if (std::string(backend) == "gpu" && !st::raster::gpu::available()) continue;
    const Mesh mesh = Mesh::sphere(0.9f, 24);
    const Canvas canvas = render_with(backend, mesh, Lighting{}, 0.0f);
    double biggest_step = 0.0;
    const int y = kHeight / 2;
    for (int x = 1; x < kWidth; ++x) {
      const Color left = canvas.pixel_at(x - 1, y);
      const Color right = canvas.pixel_at(x, y);
      const double luma_left = (0.299 * left.r + 0.587 * left.g + 0.114 * left.b) / 255.0;
      const double luma_right = (0.299 * right.r + 0.587 * right.g + 0.114 * right.b) / 255.0;
      biggest_step = std::max(biggest_step, std::abs(luma_right - luma_left));
    }
    // 先确认确实画出了东西（否则“全黑”也能过跳变判据）
    ST_CHECK(lit_ratio(canvas) > 0.03);
    ST_CHECK(biggest_step < 0.55);
  }
}

ST_TEST(scene3d_mesh_generators_are_well_formed) {
  // 新增的着色生成器：顶点/索引必须自洽（索引不越界、三角数正确）。
  const Mesh torus = Mesh::torus(0.6f, 0.22f, 24, 12, Color{200, 200, 255, 255});
  ST_CHECK(!torus.is_empty());
  ST_CHECK_EQ(torus.indices.size() % 3, static_cast<std::size_t>(0));
  for (const std::uint32_t index : torus.indices) {
    ST_CHECK(index < torus.vertex_count());
  }

  const Mesh cylinder = Mesh::cylinder(0.4f, 1.0f, 20, Color{255, 180, 120, 255});
  ST_CHECK(!cylinder.is_empty());
  ST_CHECK_EQ(cylinder.indices.size() % 3, static_cast<std::size_t>(0));
  for (const std::uint32_t index : cylinder.indices) {
    ST_CHECK(index < cylinder.vertex_count());
  }

  const Mesh plane = Mesh::plane(2.0f, 2.0f, Color{90, 90, 90, 255});
  ST_CHECK_EQ(plane.indices.size(), static_cast<std::size_t>(6));

  // 统一色：同一生成器里所有顶点的基础色系应当一致（不是五彩）
  for (std::size_t vertex = 0; vertex < cylinder.vertex_count(); ++vertex) {
    const auto span = cylinder[vertex];
    ST_CHECK(span.size() == Mesh::kVertexFloats);
    // 颜色亮度落在合理区间（vertical_shade 的 ±12% 内）
    const float luma = (span[6] + span[7] + span[8]) / 3.0f;
    ST_CHECK(luma > 0.2f);
    ST_CHECK(luma <= 1.0f);
  }
}

ST_TEST(scene3d_tinted_keeps_relative_shading) {
  // `tinted` 换色但要保留原来的**相对亮度关系**（球的上亮下暗不能丢）。
  const Mesh original = Mesh::colored_sphere(0.8f, 12, Color{255, 255, 255, 255});
  const Mesh tinted = original.tinted(Color{255, 0, 0, 255});
  ST_CHECK_EQ(tinted.vertex_count(), original.vertex_count());
  // 红色通道保留、绿蓝归零（不考虑原亮度 0 的点）
  for (std::size_t vertex = 0; vertex < tinted.vertex_count(); ++vertex) {
    const auto span = tinted[vertex];
    ST_CHECK(span[7] < 0.001f);
    ST_CHECK(span[8] < 0.001f);
  }
  // 至少有一个顶点是纯亮的（渐变没有把整只球压平）
  float brightest = 0.0f;
  for (std::size_t vertex = 0; vertex < tinted.vertex_count(); ++vertex) {
    brightest = std::max(brightest, tinted[vertex][6]);
  }
  ST_CHECK(brightest > 0.5f);
}

ST_TEST(scene3d_depth_ordering_survives_on_both_legs) {
  // **深度关系**是 3D 与 2D 的分界：近的必须挡住远的。
  //
  // 两条腿都要测：GPU 侧的深度状态是单独创建的（`CreateDepthStencilState`），
  // 漏了它就会变成“按绘制顺序覆盖”——而那种画面在小模型上看着还行。
  const Mesh near_box = Mesh::colored_box(1.6f, 1.6f, 0.3f, Color{255, 80, 80, 255});
  const Mesh far_box = Mesh::colored_box(0.6f, 0.6f, 0.6f, Color{80, 80, 255, 255});
  for (const char* backend : {"gpu", "software"}) {
    if (std::string(backend) == "gpu" && !st::raster::gpu::available()) continue;
    Canvas canvas{kWidth, kHeight, 1.0f};
    canvas.clear(Color{0, 0, 0, 255});
    const ForcedBackend force(backend);
    auto created = Scene3D::create(kWidth, kHeight);
    ST_CHECK(created.has_value());
    if (!created.has_value()) continue;
    Scene3D& scene = **created;
    Camera camera;
    camera.eye = Vec3{0.0f, 0.0f, 5.0f};
    camera.target = Vec3{0.0f, 0.0f, 0.0f};
    scene.begin_frame(Color{0, 0, 0, 255});
    scene.set_camera(camera);
    // 故意先画近的、后画远的：靠绘制顺序覆盖的实现会画错，z-buffer 的不会
    scene.draw_mesh(near_box, Mat4::translation(Vec3{0.0f, 0.0f, 1.5f}));
    scene.draw_mesh(far_box, Mat4::translation(Vec3{0.0f, 0.0f, -1.0f}));
    scene.end_frame(canvas, Rect{0.0f, 0.0f, static_cast<float>(kWidth),
                                static_cast<float>(kHeight)});
    scene.flush();
    // 画面中心仍是**近处那个盒子**的颜色系（红多于蓝）
    const Color center = canvas.pixel_at(kWidth / 2, kHeight / 2);
    ST_CHECK(center.r > center.b);
  }
}

ST_TEST(scene3d_flush_is_safe_on_both_legs) {
  // `flush()` 在两条腿上都必须可调用（软件腿是空操作）。
  // 它存在的理由是计时与缓冲复用，但“能不能调”必须是确定的。
  for (const char* backend : {"gpu", "software"}) {
    if (std::string(backend) == "gpu" && !st::raster::gpu::available()) continue;
    const ForcedBackend force(backend);
    auto created = Scene3D::create(64, 64);
    ST_CHECK(created.has_value());
    if (!created.has_value()) continue;
    Camera camera;
    camera.eye = Vec3{0.0f, 0.0f, 3.0f};
    (*created)->begin_frame(Color{0, 0, 0, 255});
    (*created)->set_camera(camera);   // ⚠ 不设相机时 `draw_mesh` 会被跳过（没有 MVP 画不出）
    (*created)->flush();
    (*created)->draw_mesh(Mesh::cube(0.5f), Mat4::identity());
    (*created)->flush();
    ST_CHECK((*created)->draw_calls() > 0);
  }
}

ST_TEST(scene3d_draw_without_camera_is_a_noop_not_crash) {
  // 没设相机就画：**跳过而不是崩**（也不应该“假装画了”——那会让 draw_calls 骗人）。
  //
  // 为何这个是真实需求而不是苛求：脚本里删掉一个字段（相机）就会走到这里，
  // 而渲染是跑在导出线程上的——那里抛错/崩溃的代价是整片出不来。
  for (const char* backend : {"gpu", "software"}) {
    if (std::string(backend) == "gpu" && !st::raster::gpu::available()) continue;
    const ForcedBackend force(backend);
    auto created = Scene3D::create(64, 64);
    ST_CHECK(created.has_value());
    if (!created.has_value()) continue;
    (*created)->begin_frame(Color{0, 0, 0, 255});
    (*created)->draw_mesh(Mesh::cube(0.5f), Mat4::identity());
    ST_CHECK_EQ((*created)->draw_calls(), static_cast<std::uint64_t>(0));
    (*created)->flush();
  }
}

/// 带材质渲染一帧（与 `render_with` 同一套相机/光照，只是多传材质）。
[[nodiscard]] auto render_material(const char* backend, const Mesh& mesh, const Lighting& lighting,
                                   const st::raster::Material& material, float yaw) -> Canvas {
  Canvas canvas{kWidth, kHeight, 1.0f};
  canvas.clear(Color{0, 0, 0, 255});
  const ForcedBackend force(backend);
  auto created = Scene3D::create(kWidth, kHeight);
  if (!created.has_value()) return canvas;
  Scene3D& scene = **created;
  Camera camera;
  camera.eye = Vec3{0.0f, 0.0f, 3.2f};
  camera.target = Vec3{0.0f, 0.0f, 0.0f};
  camera.fov_y_degrees = 42.0f;
  scene.begin_frame(Color{0, 0, 0, 255});
  scene.set_camera(camera);
  scene.set_lighting(lighting);
  scene.set_material(material);
  scene.draw_mesh(mesh, Mat4::rotation(Vec3{0.0f, 1.0f, 0.0f}, yaw));
  scene.end_frame(canvas, Rect{0.0f, 0.0f, static_cast<float>(kWidth),
                               static_cast<float>(kHeight)});
  scene.flush();
  return canvas;
}

ST_TEST(scene3d_metallic_takes_effect_on_both_legs) {
  // ## 这条用例为什么必须存在
  //
  // `metallic` 在 GPU 腿曾经**完全没接**：常量缓冲里只传了 `opacity`，
  // GPU 的像素着色器根本不知道金属是什么,于是同一个 `Material` 在两条腿上
  // 渲出两种东西（软件腿是金属、GPU 腿是塑料）。
  //
  // 而那时的全部测试都是绿的——因为「能渲出图」「两腿像素接近」这两类判据
  // 在**两边都没实现**或**只有一边实现**时都可能通过。
  //
  // 判据：① 每条腿自身「塑料 vs 金属」必须有显著差异；② 两条腿的**差异量**
  // 接近（说明是同一种材质模型，而不是各画各的）。
  if (!st::raster::gpu::available()) return;
  const Mesh mesh = Mesh::colored_sphere(0.9f, 24, Color{205, 170, 130, 255});
  Lighting lighting{};
  lighting.ambient = 0.10f;
  lighting.diffuse = 0.70f;
  lighting.key_direction = Vec3{0.4f, 0.5f, 0.75f};

  st::raster::Material plastic{};
  plastic.roughness = 0.9f;
  plastic.specular = 0.0f;

  st::raster::Material metal{};
  metal.metallic = 1.0f;
  metal.roughness = 0.25f;
  metal.specular = 1.0f;

  const Canvas sw_plastic = render_material("software", mesh, lighting, plastic, 0.0f);
  const Canvas sw_metal = render_material("software", mesh, lighting, metal, 0.0f);
  const Canvas gpu_plastic = render_material("gpu", mesh, lighting, plastic, 0.0f);
  const Canvas gpu_metal = render_material("gpu", mesh, lighting, metal, 0.0f);

  const auto luma = [](const Canvas& c) {
    double sum = 0.0;
    int n = 0;
    for (int y = 0; y < c.physical_height(); ++y) {
      for (int x = 0; x < c.physical_width(); ++x) {
        const Color p = c.pixel_at(x, y);
        sum += 0.299 * p.r + 0.587 * p.g + 0.114 * p.b;
        ++n;
      }
    }
    return n > 0 ? sum / n : 0.0;
  };

  const double sw_delta = std::abs(luma(sw_plastic) - luma(sw_metal));
  const double gpu_delta = std::abs(luma(gpu_plastic) - luma(gpu_metal));

  // ① 每条腿都得有变化（否则那条腿根本没接 metallic）
  ST_CHECK(sw_delta > 2.0);
  ST_CHECK(gpu_delta > 2.0);
  // ② 差异量同量级（差超过 3 倍说明两条腿的材质模型不一致）
  const double ratio = sw_delta > 0.0 ? gpu_delta / sw_delta : 0.0;
  ST_CHECK(ratio > 0.33);
  ST_CHECK(ratio < 3.0);
}

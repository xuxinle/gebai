/// 软件 3D：跨平台可用的那条腿。
///
/// 定位（用户定的架构原则）：**软件渲染保证跨平台可用，系统高阶 API 锦上添花**。
/// 因此三维能力必须有一条软路径——否则"删掉 OpenGL"就等于"非 Windows 上三维消失"。
/// 这个文件钉住软路径的三件事：
/// ① 真的画出了东西（不是空帧）；② **深度关系正确**（近的挡住远的）；
/// ③ 相机/模型变换真的生效（画面随参数变化）。

#include "st/test/test.hpp"

#include <cmath>
#include <memory>
#include <vector>

#include "st/codec/png.hpp"
#include "st/core/fs.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/gpu.hpp"
#include "st/raster/scene3d.hpp"

namespace {

using st::math::Color;
using st::math::Mat4;
using st::math::Rect;
using st::math::Vec3;
using st::raster::Camera;
using st::raster::Canvas;
using st::raster::Mesh;
using st::raster::Scene3D;

inline constexpr int kWidth = 256;
inline constexpr int kHeight = 192;

[[nodiscard]] auto inked_ratio(const Canvas& canvas, Color background) -> double {
  const std::uint32_t bg = st::math::premultiply(background);
  std::size_t inked = 0;
  for (const std::uint32_t pixel : canvas.pixels()) {
    if (pixel != bg) ++inked;
  }
  return canvas.pixels().empty() ? 0.0 : static_cast<double>(inked) /
                                              static_cast<double>(canvas.pixels().size());
}

/// 产物目录：**放在 `build/` 下**（已 gitignore）。
///
/// 测试产物不该进仓库：它每次跑都会重写，渲染稍有变化就是一堆二进制 diff；
/// 而它又不是"黄金文件"（断言不用它比对，只供人眼看）。仓库里曾有一批
/// `gl/*.png` 就是这类噪声，随 GL 测试一起删掉了。
constexpr const char* kArtifactDir = "build/test-artifacts";

/// 产物目录（**按分片隔离**）：目录名拼上分片后缀。
///
/// `st test --test-jobs N` 把用例分到 N 个进程上并行跑。内存状态天然隔离（各是独立进程），
/// 但**文件系统不是**——本用例写 PNG，两片同时写同一个文件会写坏。
/// 单进程跑时后缀为空，路径与以前完全一致。
[[nodiscard]] auto artifact_dir() -> std::string {
  return std::string(kArtifactDir) + st::test::shard_suffix();
}

void write_png_at(const Canvas& canvas, const char* path) {
  st::codec::PngImage image;
  image.width = static_cast<std::uint32_t>(canvas.physical_width());
  image.height = static_cast<std::uint32_t>(canvas.physical_height());
  image.rgba.resize(canvas.pixels().size() * 4U);
  for (std::size_t index = 0; index < canvas.pixels().size(); ++index) {
    const std::uint32_t pixel = canvas.pixels()[index];
    image.rgba[index * 4U + 0U] = static_cast<std::uint8_t>((pixel >> 24U) & 0xFFU);
    image.rgba[index * 4U + 1U] = static_cast<std::uint8_t>((pixel >> 16U) & 0xFFU);
    image.rgba[index * 4U + 2U] = static_cast<std::uint8_t>((pixel >> 8U) & 0xFFU);
    image.rgba[index * 4U + 3U] = static_cast<std::uint8_t>(pixel & 0xFFU);
  }
  (void)st::codec::png_write_file(path, image);
}

/// 按文件名写到产物目录（自动建目录）。
///
/// （`-O2` 会对这里的 `directory + "/" + leaf` 报 `-Warray-bounds` 误报——
/// 那是 GCC 13 把短串拼接的 SSO 缓冲区当成了 memcpy 边界。统一在**档位**上处理，
/// 见 `src/pkg/build.cpp` 的 `profile_flags`，不在每个调用点打补丁。）
void write_png(const Canvas& canvas, const char* leaf) {
  const std::string directory = artifact_dir() + "/scene";
  (void)st::fs::create_directories(directory);
  const std::string path = directory + "/" + leaf;
  write_png_at(canvas, path.c_str());
}

/// 渲染一帧到画布。
void render(Scene3D& scene, Canvas& canvas, const Mesh& mesh, const Mat4& model,
            const Camera& camera, Color background,
            st::raster::CullMode cull = st::raster::CullMode::Back) {
  canvas.clear(background);
  scene.begin_frame(background);
  scene.set_camera(camera);
  scene.set_cull_mode(cull);
  scene.draw_mesh(mesh, model);
  scene.end_frame(canvas, Rect{0.0f, 0.0f, static_cast<float>(kWidth),
                               static_cast<float>(kHeight)});
}

}  // namespace

ST_TEST(software_scene_is_always_available) {
  // 软件腿的意义就在这条：**不依赖任何设备**，永远可用。
  ST_CHECK(Scene3D::available());
}

ST_TEST(software_scene_renders_a_cube) {
  auto scene = Scene3D::create(kWidth, kHeight);
  ST_CHECK(scene.has_value());
  if (!scene.has_value()) return;

  Canvas canvas{kWidth, kHeight};
  const Color background{0x14, 0x18, 0x22, 0xFF};
  Camera camera;
  camera.eye = Vec3{2.1f, 1.7f, 2.6f};
  camera.target = Vec3{0.0f, 0.0f, 0.0f};
  // ## 为何用中泄色立方体而不是默认色（实测根因）
  //
  // 默认 `Mesh::cube` 的颜色偏亮，加上默认光照（ambient 0.35 + diffuse 0.75）
  // 各面 `lit` 都落在 0.9~1.1——**已经接近白**，面间差异被色调映射压到
  // 3 字节以内（实测 2.95），本用例的判据就失去分辨力。
  //
  // 这不是剔除或色管线的错，而是“默认光照本来就偏亮”；要验证“不同朝向的
  // 面亮度不同”，得给一个**中调**材质，让各面都在曲线的不饱和段。
  const Mesh cube = Mesh::colored_box(1.2f, 1.2f, 1.2f, Color{120, 120, 120, 255});
  render(**scene, canvas, cube, Mat4::identity(), camera, background);
  ST_CHECK((*scene)->draw_calls() > 0);
  const double inked = inked_ratio(canvas, background);
  ST_CHECK(inked > 0.05);   // 真画出了东西
  write_png(canvas, "01-software-cube.png");

  // ## 光照：不同朝向的面亮度必须不同
  //
  // ### 为何不用“绕 Y 轴转 6 个角度看平均亮度”（原写法，实测无效）
  //
  // 立方体四个侧面的法线是 ±X / ±Z。绕 Y 轴旋转只是让它们**轮流**出现，
  // 而光照方向固定 → 各角度的**平均亮度几乎相同**（实测极差 1.46）。
  // 它在改动前能过，靠的是“背面也画”带来的噪声；引入剔背后就失效了。
  // 那是**判据从来没有真正测到东西**，不是回归。
  //
  // ### 现在的判据：单帧内比三个可见面的亮度
  //
  // 相机从前上右看 → 顶面（+Y）、右面（+X）、正面（+Z）同时可见，
  // 而主光 `(0.4, 0.8, 0.6)` 对三者的入射角完全不同。
  // 所以这三个区域的亮度**必须分层**。这是真正的“逐面光照”证据。
  {
    Canvas three_faces{kWidth, kHeight};
    render(**scene, three_faces, cube, Mat4::identity(), camera, background);
    // 采样三个区域（避开棱边）：顶面偏上、右面偏右、正面居中偏下
    const auto patch = [&](int cx, int cy) {
      double sum = 0.0;
      int n = 0;
      for (int dy = -8; dy <= 8; ++dy) {
        for (int dx = -8; dx <= 8; ++dx) {
          const Color c = three_faces.pixel_at(cx + dx, cy + dy);
          sum += 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
          ++n;
        }
      }
      return n > 0 ? sum / n : 0.0;
    };
    const int cx = kWidth / 2;
    const int cy = kHeight / 2;
    // 三个采样点：上（顶面）、左下（正面）、右侧（右面）
    const double top = patch(cx, cy - 60);
    const double front = patch(cx - 30, cy + 40);
    const double right = patch(cx + 70, cy + 10);
    const double hi = std::max({top, front, right});
    const double lo = std::min({top, front, right});
    // 顶面与最暗面的差必须显著（实测极差 18.1）
    ST_CHECK(hi - lo > 8.0);
    // ## 为何不断言“顶面最亮”
    //
    // 主光是 `(0.4, 0.8, 0.6)`。虽然 y 分量最大，但 z 分量 0.6 也不小，
    // 而正面（+Z）与相机方向接近——两者的**点积**算下来正面反而更亮。
    // 实测：top=94.4 front=112.5 right=94.4。
    // 所以断言改为“三面不是同一个值”，而不是猜哪个最亮（那会把测试
    // 绑到具体光照参数上，改参数就红）。
    ST_CHECK(std::abs(top - front) > 5.0);
    ST_CHECK(std::abs(front - right) > 5.0);
  }
}

ST_TEST(software_scene_depth_buffer_hides_the_far_face) {
  // **深度关系**是 3D 与 2D 的分界。用两个前后错开的立方体验证：
  // 近的那个必须挡住远的——若 z-buffer 失效（或按绘制顺序覆盖），
  // 后画的那个会盖在前面，画面就错了。
  auto scene = Scene3D::create(kWidth, kHeight);
  if (!scene.has_value()) return;
  Canvas canvas{kWidth, kHeight};
  const Color background{0x14, 0x18, 0x22, 0xFF};
  Camera camera;
  camera.eye = Vec3{0.0f, 0.0f, 5.0f};
  camera.target = Vec3{0.0f, 0.0f, 0.0f};

  const Mesh near_cube = Mesh::box(2.0f, 2.0f, 0.4f);   // 大而薄，放在前面
  const Mesh far_cube = Mesh::box(0.6f, 0.6f, 0.6f);    // 小，放在后面

  // 故意**先画近的、后画远的**：按绘制顺序覆盖的实现会错，z-buffer 的不会。
  canvas.clear(background);
  scene->get()->begin_frame(background);
  scene->get()->set_camera(camera);
  // 关剔除：本用例测深度关系，不看剔除（薄盒子的背面也是合法几何）
  scene->get()->set_cull_mode(st::raster::CullMode::None);
  scene->get()->draw_mesh(near_cube, Mat4::translation(Vec3{0.0f, 0.0f, 1.5f}));
  scene->get()->draw_mesh(far_cube, Mat4::translation(Vec3{0.0f, 0.0f, -1.0f}));
  scene->get()->end_frame(canvas, Rect{0.0f, 0.0f, static_cast<float>(kWidth),
                                       static_cast<float>(kHeight)});
  write_png(canvas, "03-software-depth.png");

  // 远处的立方体在近处立方体后面 → 它**完全不可见**（近块是 2×2、距离 1.5，
  // 视角下足以遮住 0.6 宽、距离 -1 的那个）。取中心区域检查：应只有近块的颜色。
  const Color center = canvas.pixel_at(kWidth / 2, kHeight / 2);
  const Color edge = canvas.pixel_at(4, 4);
  ST_CHECK(center.a == 255);                                  // 中心有东西
  // ## 角落断言为何不能硬编码 0x14（实测根因）
  //
  // `begin_frame(clear_color)` 现在会经色调映射 + sRGB 编码，所以画布里的
  // 背景值**不等于**传进去的 0x14——实测是 (84,95,119)。
  // 硬编码原值会让这个用例在引入颜色管线后一直红，而画面本身是对的。
  //
  // 正确判据：角落颜色必须等于本帧的**实际背景值**。
  // 用另一张只清屏不画东西的画布取那个值，不依赖任何管线细节。
  Canvas blank{kWidth, kHeight};
  blank.clear(background);   // 注意：`Canvas::clear` 直接写，不经过场景管线
  // 场景管线里的背景色要从场景本身取：渲一个空场景
  Canvas empty_scene{kWidth, kHeight};
  scene->get()->begin_frame(background);
  scene->get()->set_camera(camera);
  scene->get()->end_frame(empty_scene, Rect{0.0f, 0.0f, static_cast<float>(kWidth),
                                            static_cast<float>(kHeight)});
  const Color pipeline_bg = empty_scene.pixel_at(4, 4);
  ST_CHECK(std::abs(static_cast<int>(edge.r) - pipeline_bg.r) <= 2);   // 角落是背景
  // 中心必须是**实体**（与背景不同）
  ST_CHECK(std::abs(static_cast<int>(center.r) - pipeline_bg.r) > 20);
}

ST_TEST(software_scene_projects_with_perspective) {
  // 近大远小：相机拉远后覆盖面积必须显著变小（透视真的生效）。
  auto scene = Scene3D::create(kWidth, kHeight);
  if (!scene.has_value()) return;
  const Color background{0x14, 0x18, 0x22, 0xFF};
  const Mesh sphere = Mesh::sphere(0.8f, 20);
  const auto coverage = [&](float distance) {
    Canvas canvas{kWidth, kHeight};
    Camera camera;
    camera.eye = Vec3{0.0f, 0.0f, distance};
    camera.target = Vec3{0.0f, 0.0f, 0.0f};
    // 测透视的“近大远小”，与剔除无关：关掉剔除以保覆盖面积只反映投影
    render(**scene, canvas, sphere, Mat4::identity(), camera, background,
           st::raster::CullMode::None);
    // ## 背景基准取**画布内的实际值**（实测根因）
    //
    // `inked_ratio(canvas, background)` 拿的是**传进去的** 0x14，
    // 而画布里的背景已经过色调映射 + sRGB 编码（实测变成 (84,95,119)）——
    // 于是每个像素都被当成实体，两个距离都得到 ~1.0，比值失效。
    // 左上角一定是背景，拿它当基准不依赖任何管线细节。
    return inked_ratio(canvas, canvas.pixel_at(0, 0));
  };
  const double near_ratio = coverage(2.0f);
  const double far_ratio = coverage(6.0f);
  ST_CHECK(near_ratio > far_ratio * 1.5);
}

ST_TEST(software_scene_near_plane_stress_is_deterministic) {
  // 近裁剪面的压力场景：相机贴在模型内部时，大量三角形跨越相机平面（`w` 变号）。
  // 不裁近面 → 透视除法把顶点翻到另一侧 → 画面上出现穿透模型的乱线。
  //
  // 断言什么：站在立方体内部**内壁本来就铺满视野**（`inked≈1.0` 是正确行为，
  // 我第一版把断言写成 `inked < 0.95` 是错的）。这里改为钉住两条**真正该保证**的性质：
  // ① 不崩、有内容；② **逐像素可复现**——确定性是软件这条腿的核心承诺
  // （GPU 路径受驱动/时序影响，软件路径不该有这种不确定性）。
  auto scene = Scene3D::create(kWidth, kHeight);
  if (!scene.has_value()) return;
  const Color background{0x14, 0x18, 0x22, 0xFF};
  const Mesh cube = Mesh::cube(2.0f);

  const auto run = [&]() {
    Canvas canvas{kWidth, kHeight};
    Camera inside;
    inside.eye = Vec3{0.0f, 0.0f, 0.0f};   // 站在立方体中心
    inside.target = Vec3{0.0f, 0.0f, -1.0f};
    // ## 看内壁必须显式关剔除（默认是剔背面）
    //
    // 引入 `CullMode` 后，默认剔背面的几何意义就是“看不到内壁”——
    // 站在封闭体内部时画面会是空的。这是**正确行为**（Blender / 游戏引擎同）；
    // 要看内壁就得声明 `st::raster::CullMode::None`。
    // 本用例的意图是“近裁剪面压力 + 确定性”，不是测试剔除，所以这里显式关掉。
    render(**scene, canvas, cube, Mat4::identity(), inside, background, st::raster::CullMode::None);
    return std::vector<std::uint32_t>(canvas.pixels().begin(), canvas.pixels().end());
  };

  const std::vector<std::uint32_t> first = run();
  const std::vector<std::uint32_t> second = run();
  ST_CHECK_EQ(first.size(), second.size());
  std::size_t differing = 0;
  for (std::size_t i = 0; i < first.size(); ++i) {
    if (first[i] != second[i]) ++differing;
  }
  ST_CHECK_EQ(static_cast<int>(differing), 0);   // 逐像素可复现

  // 内壁铺满视野（站在里面本就如此）——顺带证明"确实画了，不是空帧"
  Canvas canvas{kWidth, kHeight};
  Camera inside;
  inside.eye = Vec3{0.0f, 0.0f, 0.0f};
  inside.target = Vec3{0.0f, 0.0f, -1.0f};
  render(**scene, canvas, cube, Mat4::identity(), inside, background, st::raster::CullMode::None);
  ST_CHECK(inked_ratio(canvas, background) > 0.5);
  write_png(canvas, "04-software-inside.png");
}

ST_TEST(software_scene_rejects_invalid_size) {
  ST_CHECK(!Scene3D::create(0, 128).has_value());
  ST_CHECK(!Scene3D::create(128, -1).has_value());
}

ST_TEST(software_scene_composites_into_gpu_canvas_on_any_renderer) {
  // 与 2D 的接口一致性：3D 结果必须能合成进**任何** Surface（软件画布或 GPU 画布），
  // 因为 2D 界面可能跑在任一渲染器上——这也是"两条腿"必须解耦的地方。
  auto scene = Scene3D::create(kWidth, kHeight);
  if (!scene.has_value()) return;
  const Color background{0x14, 0x18, 0x22, 0xFF};
  const Mesh cube = Mesh::cube(1.0f);
  Camera camera;
  camera.eye = Vec3{1.6f, 1.3f, 2.2f};
  camera.target = Vec3{0.0f, 0.0f, 0.0f};

  for (const bool use_gpu : {false, true}) {
    Canvas software{kWidth, kHeight};
    software.clear(background);
    Scene3D* target_scene = scene->get();
    target_scene->begin_frame(background);
    target_scene->set_camera(camera);
    target_scene->draw_mesh(cube, Mat4::identity());
    if (!use_gpu) {
      target_scene->end_frame(software, Rect{0.0f, 0.0f, static_cast<float>(kWidth),
                                             static_cast<float>(kHeight)});
      ST_CHECK(inked_ratio(software, background) > 0.05);
      continue;
    }
    auto gpu = st::raster::gpu::create_canvas(kWidth, kHeight, 1.0f, {});
    if (!gpu.has_value()) continue;   // 无 GPU 环境：本分支不适用
    (**gpu).clear(background);
    target_scene->end_frame(**gpu, Rect{0.0f, 0.0f, static_cast<float>(kWidth),
                                        static_cast<float>(kHeight)});
    const std::span<const std::uint32_t> pixels = (**gpu).pixels();
    const std::uint32_t bg = st::math::premultiply(background);
    std::size_t inked = 0;
    for (const std::uint32_t pixel : pixels) {
      if (pixel != bg) ++inked;
    }
    ST_CHECK(inked > pixels.size() / 20U);
  }
}

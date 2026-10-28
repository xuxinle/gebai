// 三维能力矩阵：逐项核对「软件腿 / GPU 腿」是否都真的支持。
//
// ## 为什么需要这份探针
//
// 这条线上**反复**出现同一类缺陷：功能在软件腿上做了、GPU 腿上没做，
// 而测试跑在默认后端上——于是"全绿"但用户在另一台机器上看到的是另一幅画面。
// 实测遇到的三个：
//   1. `set_material` 在 GPU 腿是空实现 → 透明度被静默丢弃；
//   2. GPU 只有一套固定写深度的状态 → 透明物体被自己挡住；
//   3. GPU 腿不渲染投影阴影 → 同一场景一边有影子一边没有。
//
// 所以判据不能是"功能存在"（那查不出来），而是**同一份输入、两条腿各渲一遍、
// 逐项量化差异**。差异超过阈值就报出来，由人决定是"补齐"还是"声明不支持"。
//
// ## 判据怎么定（不能要求逐像素相同）
//
// 两条腿的**着色路径**本来就不同（软件腿逐像素调 `shade_terms`，GPU 腿是它的
// HLSL 翻译），浮点顺序差异会让像素有 ±1~2 的抖动。而**几何与结构**应当一致：
// 覆盖面积、遮挡关系、阴影是否存在。
//
// 所以逐项用**结构性指标**比，不比原始像素。
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "st/codec/png.hpp"
#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/mesh.hpp"
#include "st/raster/scene3d.hpp"

namespace {
using namespace st::raster;
using st::math::Color;
using st::math::Mat4;
using st::math::Vec3;

constexpr int kW = 320;
constexpr int kH = 320;

/// 实体像素数与平均亮度（结构性指标，对 ±2 的着色抖动不敏感）。
struct Measure {
  int inked{0};
  double luma{0.0};
  int changed_from_bg{0};
};

auto measure(const Canvas& canvas) -> Measure {
  const Color bg = canvas.pixel_at(0, 0);
  Measure m{};
  int n = 0;
  for (int y = 0; y < canvas.physical_height(); ++y) {
    for (int x = 0; x < canvas.physical_width(); ++x) {
      const Color c = canvas.pixel_at(x, y);
      const int d = (static_cast<int>(c.r) - bg.r) * (static_cast<int>(c.r) - bg.r) +
                    (static_cast<int>(c.g) - bg.g) * (static_cast<int>(c.g) - bg.g) +
                    (static_cast<int>(c.b) - bg.b) * (static_cast<int>(c.b) - bg.b);
      if (d > 100) ++m.inked;
      m.luma += 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
      ++n;
    }
  }
  if (n > 0) m.luma /= n;
  m.changed_from_bg = m.inked;
  return m;
}

/// 一个可测能力：给它一份"只有它才有的差异"，看两条腿是否都表现出来。
struct Capability {
  const char* name;
  char status;   ///< 'both' / 'software' / 'gpu' / 'none'
  double sw_delta;
  double gpu_delta;
};

}  // namespace

/// 用指定后端渲染一个变体。`variant` 决定用哪种能力。
///
/// 为何用"变体 + 差值"而不是"绝对值"：能力的**存在性**体现在
/// 「开启 vs 关闭」的画面差异上。两条腿的绝对值可能不同（着色顺序），
/// 但**差异有没有**应当一致。
static auto render_variant(const char* backend, int variant, Canvas& canvas) -> bool {
  // 通过环境变量强制后端（与探针其它部分同一套惯例）
  const char* prev = std::getenv("ST_SCENE3D_BACKEND");
  const std::string saved = prev != nullptr ? prev : "";
  const bool had = prev != nullptr;
#ifdef _WIN32
  (void)::_putenv_s("ST_SCENE3D_BACKEND", backend);
#else
  (void)::setenv("ST_SCENE3D_BACKEND", backend, 1);
#endif
  auto scene = Scene3D::create(kW, kH);
#ifdef _WIN32
  (void)::_putenv_s("ST_SCENE3D_BACKEND", had ? saved.c_str() : "");
#else
  if (had) {
    (void)::setenv("ST_SCENE3D_BACKEND", saved.c_str(), 1);
  } else {
    (void)::unsetenv("ST_SCENE3D_BACKEND");
  }
#endif
  if (!scene.has_value()) return false;
  // 确认真的拿到了想要的后端
  if ((*scene)->backend() != backend) return false;

  const Color bg{20, 22, 30, 255};
  canvas.clear(bg);
  Camera cam;
  cam.eye = Vec3{0.0f, 1.5f, 3.4f};
  cam.target = Vec3{0.0f, 0.1f, 0.0f};
  (*scene)->begin_frame(bg);
  (*scene)->set_camera(cam);
  (*scene)->set_cull_mode(CullMode::Back);

  // 地面（在 set_planar_shadow 之前画 = 不算实体）
  (*scene)->draw_mesh(Mesh::plane(20.0f, 20.0f, Color{105, 108, 118, 255}),
                      Mat4::rotation(Vec3{1.0f, 0.0f, 0.0f}, -1.5707963f)
                          * Mat4::translation(Vec3{0.0f, -0.5f, 0.0f}));

  // variant 0 = 全部关闭（基线）；1 = 阴影；2 = 透明；3 = 金属高光
  PlanarShadow shadow{};
  shadow.ground_y = -0.5f;
  shadow.strength = (variant == 1) ? 0.55f : 0.0f;
  (*scene)->set_planar_shadow(shadow);

  Material mat{};
  if (variant == 2) {
    mat.opacity = 0.45f;
  }
  if (variant == 3) {
    mat.metallic = 1.0f;
    mat.roughness = 0.15f;
    mat.specular = 1.0f;
  }
  (*scene)->set_material(mat);

  (*scene)->draw_mesh(Mesh::colored_sphere(0.62f, 28, Color{205, 170, 130, 255}),
                      Mat4::translation(Vec3{0.0f, 0.12f, 0.0f}));
  (*scene)->end_frame(
      canvas, st::math::Rect{0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH)});
  (*scene)->flush();
  return true;
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;

  struct Case {
    const char* name;
    int variant;
  };
  const Case cases[] = {
      {"投影阴影", 1},
      {"透明混合", 2},
      {"金属高光", 3},
  };

  st::log::out("{:<12}{:>10}{:>10}{:>10}{:>10}\n", "能力", "软件差值", "GPU差值", "软件", "GPU");
  int mismatches = 0;
  for (const Case& c : cases) {
    Canvas sw_base{kW, kH, 1.0f};
    Canvas sw_on{kW, kH, 1.0f};
    Canvas gpu_base{kW, kH, 1.0f};
    Canvas gpu_on{kW, kH, 1.0f};
    if (!render_variant("software", 0, sw_base) || !render_variant("software", c.variant, sw_on) ||
        !render_variant("gpu", 0, gpu_base) || !render_variant("gpu", c.variant, gpu_on)) {
      st::log::out("{:<12}  后端不可用，跳过\n", c.name);
      continue;
    }
    const Measure a = measure(sw_base);
    const Measure b = measure(sw_on);
    const Measure d = measure(gpu_base);
    const Measure e = measure(gpu_on);
    // 差值用「平均亮度变化」——它把"阴影变暗 / 透明露底 / 金属变暗"都包含了
    const double sw_delta = std::abs(a.luma - b.luma);
    const double gpu_delta = std::abs(d.luma - e.luma);
    const bool sw_has = sw_delta > 0.3;
    const bool gpu_has = gpu_delta > 0.3;
    st::log::out("{:<12}{:>10.2f}{:>10.2f}{:>10}{:>10}\n", c.name, sw_delta, gpu_delta,
              sw_has ? "有" : "无", gpu_has ? "有" : "无");
    if (sw_has != gpu_has) ++mismatches;
  }

  // ## GPU 阴影是**已知未实现**，不是回归
  //
  // 它需要额外一套动态顶点缓冲与一遍绘制（投影几何每帧不同，无法走
  // IMMUTABLE 缓存路径）；而裁云导出**强制走软件腿**（`kTargetAcceptsTexture`
  // 恒为 false，因为结果要回读到软件画布），所以这条路径在主用法上用不到。
  //
  // 把它单列出来，是为了让差异**显式可见**——本仓库反复出现的正是
  // 「一边有一边没有、而测试跑在默认后端所以看不出来」。
  st::log::out("\n已知未实现：GPU 腿的投影阴影（设计决策，见 BACKLOG）\n");
  st::log::out("{}\n", mismatches == 0 ? "其余能力两腿一致 ✓"
                                    : "有 " + std::to_string(mismatches) + " 项能力两腿不一致");
  return mismatches == 0 ? 0 : 1;
}

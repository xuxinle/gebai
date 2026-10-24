// 2026-10-24 新增能力的像素级探针：逐项证明「它真的生效」，而不是"看起来像"。
//
// ## 判据的共同形状
//
// 每个能力都测**开关前后的画面差异**，而不是绝对值：
//
// | 能力 | 怎么证明它生效 |
// |---|---|
// | 材质 base_color | 同一份网格、只换材质色 → 渲出的像素色必须跟着变 |
// | 自发光 unlit | 环境全黑时，发光体仍然亮（不发光时全黑） |
// | 点光衰减 | 同一盏灯，离物体近 vs 远 → 近处更亮（`1/d²`） |
// | 聚光锥形 | 锥内亮、锥外暗，且过渡区在内外角之间 |
// | AgX | 高饱和过曝色：AgX 应当往白吐（饱和度下降），逐色道曲线不会 |
// | 正交相机 | 远近两个同尺寸物体 → 屏幕上像素尺寸相同（透视下不同） |
//
// 「按预期方向变化」是比"数值等于多少"更稳的判据：它不依赖具体实现常数，
// 但足以抓住"能力根本没接上"与"接反了"。
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "st/codec/png.hpp"
#include "st/core/print.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/scene3d.hpp"

namespace {
using namespace st::raster;
using st::math::Color;
using st::math::Mat4;
using st::math::Vec3;

constexpr int kW = 320;
constexpr int kH = 320;
const Color kBg{0, 0, 0, 255};

struct Stats {
  double luma{0.0};
  /// 亮度 p99（最亮的那 1% 像素的均值）——用来量"面向光源的那一面"。
  ///
  /// 为何需要：全画面平均值会被 `n·l` 余弦项主导。灯贴得很近时，
  /// 球面大部分法线背对光源 → 平均值反而下降，而"最亮的那块"明显更亮。
  /// 实测踩过：按平均值判，点光距离衰减会被判成"方向反了"。
  double peak{0.0};
  double saturation{0.0};   ///< 平均 (max-min)/max
  int lit{0};               ///< 非背景像素
};

auto analyze(const Canvas& canvas) -> Stats {
  Stats s{};
  std::vector<double> lumas;
  int n = 0;
  double sat_sum = 0.0;
  int sat_n = 0;
  for (int y = 0; y < canvas.physical_height(); ++y) {
    for (int x = 0; x < canvas.physical_width(); ++x) {
      const Color c = canvas.pixel_at(x, y);
      const double l = 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
      s.luma += l;
      lumas.push_back(l);
      ++n;
      const int mx = std::max({c.r, c.g, c.b});
      const int mn = std::min({c.r, c.g, c.b});
      if (mx > 12) {
        ++s.lit;
        if (mx > 0) {
          sat_sum += static_cast<double>(mx - mn) / mx;
          ++sat_n;
        }
      }
    }
  }
  if (n > 0) s.luma /= n;
  if (sat_n > 0) s.saturation = sat_sum / sat_n;
  if (!lumas.empty()) {
    std::sort(lumas.begin(), lumas.end());
    const std::size_t start = lumas.size() - std::min<std::size_t>(lumas.size(), lumas.size() / 100 + 1);
    double sum = 0.0;
    for (std::size_t i = start; i < lumas.size(); ++i) sum += lumas[i];
    s.peak = sum / static_cast<double>(lumas.size() - start);
  }
  return s;
}

/// 渲染一个只含单个物体的场景。
struct RenderCase {
  Lighting lighting{};
  Camera camera{};
  Material material{};
  Mesh mesh = Mesh::colored_sphere(0.6f, 28, Color{200, 200, 200, 255});
  Mat4 model = Mat4::identity();
};

auto render(const RenderCase& c, ToneMap tone = ToneMap::Aces) -> Canvas {
  Canvas canvas{kW, kH, 1.0f};
  canvas.clear(kBg);
  auto scene = Scene3D::create(kW, kH);
  if (!scene.has_value()) return canvas;
  Lighting lighting = c.lighting;
  lighting.tone_map = tone;
  (*scene)->begin_frame(kBg);
  (*scene)->set_camera(c.camera);
  (*scene)->set_lighting(lighting);
  (*scene)->set_material(c.material);
  (*scene)->draw_mesh(c.mesh, c.model);
  (*scene)->end_frame(
      canvas, st::math::Rect{0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH)});
  (*scene)->flush();
  return canvas;
}

auto save_png(const Canvas& canvas, const std::string& path) -> void {
  st::codec::PngImage image;
  image.width = static_cast<std::uint32_t>(canvas.physical_width());
  image.height = static_cast<std::uint32_t>(canvas.physical_height());
  const auto pixels = canvas.pixels();
  image.rgba.resize(static_cast<std::size_t>(image.width) * image.height * 4);
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    const Color c = st::math::unpremultiply(pixels[i]);
    image.rgba[i * 4 + 0] = c.r;
    image.rgba[i * 4 + 1] = c.g;
    image.rgba[i * 4 + 2] = c.b;
    image.rgba[i * 4 + 3] = c.a;
  }
  (void)st::codec::png_write_file(path, image);
}


/// 强制软件腿（RAII 还原）。
///
/// ## 为什么必须强制
///
/// 本探针验的是**软件腿**的新能力（材质基色 / 自发光 / 点光 / 聚光 / AgX / 正交）。
/// 不强制时 `Scene3D::create` 会按实测决策选 GPU 腿，而那些能力在 GPU 腿上
/// **还没实现**——于是所有判据都失败，看起来像"功能坏了"，实际是"走错了腿"。
///
/// 这正是本仓库反复出现的形态：一边实现、另一边没实现，而测试跑在默认后端上。
class ForcedSoftware {
 public:
  ForcedSoftware() {
    const char* prev = std::getenv("ST_SCENE3D_BACKEND");
    if (prev != nullptr) {
      previous_ = prev;
      had_ = true;
    }
    (void)::_putenv_s("ST_SCENE3D_BACKEND", "software");
  }
  ~ForcedSoftware() {
    (void)::_putenv_s("ST_SCENE3D_BACKEND", had_ ? previous_.c_str() : "");
  }
  ForcedSoftware(const ForcedSoftware&) = delete;
  auto operator=(const ForcedSoftware&) -> ForcedSoftware& = delete;

 private:
  std::string previous_{};
  bool had_{false};
};

int failures = 0;   // lint-allow: L8 探针的失败计数器（单进程、单线程，与其它探针同例）

void report(bool ok, const char* name, const std::string& detail) {
  st::print("  {:<28} {}  {}\n", name, ok ? "[OK]" : "[X]", detail);
  if (!ok) ++failures;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string out_dir = argc > 1 ? argv[1] : ".";
  const ForcedSoftware force_software;

  {
    // 自检：最基本的「能画出一个球吗」——若这条都不成立，后面全是假失败
    Canvas probe{kW, kH, 1.0f};
    probe.clear(kBg);
    auto s = Scene3D::create(kW, kH);
    st::print("自检: Scene3D::create 成功={} backend={}\n", s.has_value(),
              s.has_value() ? (*s)->backend() : "n/a");
    if (s.has_value()) {
      Lighting L{};
      L.ambient = 0.8f;
      Camera cam{};
      cam.eye = Vec3{0.0f, 0.0f, 3.0f};
      (*s)->begin_frame(kBg);
      (*s)->set_camera(cam);
      (*s)->set_lighting(L);
      (*s)->draw_mesh(Mesh::colored_sphere(1.0f, 24, Color{255, 80, 80, 255}), Mat4::identity());
      (*s)->end_frame(probe, st::math::Rect{0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH)});
      (*s)->flush();
    }
    const Stats st0 = analyze(probe);
    const Color mid = probe.pixel_at(kW / 2, kH / 2);
    st::print("自检: luma={:.2f} lit={} 中心像素=({},{},{})\n", st0.luma, st0.lit, mid.r, mid.g,
              mid.b);
    save_png(probe, out_dir + "/cap-selfcheck.png");
  }

  // ══ 1. 材质 base_color：同一网格只换材质色 ══
  st::print("── 1. 材质 base_color（网格不变，只换材质）──\n");
  {
    RenderCase red{};
    red.lighting.ambient = 0.6f;
    red.lighting.diffuse = 0.4f;
    red.material.base_color = Vec3{0.8f, 0.1f, 0.1f};
    red.material.base_color_set = true;

    RenderCase blue = red;
    blue.material.base_color = Vec3{0.1f, 0.2f, 0.8f};

    RenderCase vertex_only = red;
    vertex_only.material.base_color_set = false;   // 回落到顶点色（灰）

    const Canvas c_red = render(red);
    const Canvas c_blue = render(blue);
    const Canvas c_vertex = render(vertex_only);
    const Color pr = c_red.pixel_at(kW / 2, kH / 2);
    const Color pb = c_blue.pixel_at(kW / 2, kH / 2);
    const Color pv = c_vertex.pixel_at(kW / 2, kH / 2);
    st::print("    中心像素: 红材质=({},{},{}) 蓝材质=({},{},{}) 顶点色=({},{},{})\n", pr.r, pr.g,
              pr.b, pb.r, pb.g, pb.b, pv.r, pv.g, pv.b);
    // 同一网格两个颜色必须不同，且各自的主色通道正确
    report(pr.r > pr.b && pb.b > pb.r, "换材质色会改变画面",
           std::format("红:{}-{}  蓝:{}-{}", pr.r, pr.b, pb.b, pb.r));
    report(pv.r == pv.g && pv.g == pv.b, "base_color_set=false 回落顶点色",
           std::format("顶点色应为灰: {}", pv.r));
    save_png(c_red, out_dir + "/cap-basecolor-red.png");
  }

  // ══ 2. 自发光不受光照影响 ══
  st::print("\n── 2. 自发光（emission_unlit）──\n");
  {
    RenderCase dark{};
    dark.lighting.ambient = 0.0f;
    dark.lighting.diffuse = 0.0f;
    dark.lighting.rim = 0.0f;

    RenderCase unlit = dark;
    unlit.material.emission = 1.5f;
    unlit.material.emissive_color = Vec3{0.2f, 0.8f, 1.0f};
    unlit.material.emissive_color_set = true;
    unlit.material.emission_unlit = true;

    // ## 判据要带环境光
    //
    // 在全黑场景里 unlit 与旧语义**本来就相同**（受光项 = 0，加不加都一样）——
    // 那样的判据证明不了任何事。要给一盏环境光，两者才分得开：
    // 旧语义 = 受光 + 自发光（更亮）；unlit = 只有自发光（更暗）。
    RenderCase lit_style = unlit;
    lit_style.lighting.ambient = 0.8f;
    lit_style.lighting.diffuse = 0.4f;
    lit_style.material.base_color = Vec3{0.2f, 0.9f, 0.2f};
    lit_style.material.base_color_set = true;
    lit_style.material.emission_unlit = false;   // 旧语义：加在受光结果上
    RenderCase unlit_with_light = lit_style;
    unlit_with_light.material.emission_unlit = true;

    const Canvas c_dark = render(dark);
    const Canvas c_unlit = render(unlit);
    const Canvas c_lit = render(lit_style);
    const Canvas c_unlit2 = render(unlit_with_light);
    const Stats s_dark = analyze(c_dark);
    const Stats s_unlit = analyze(c_unlit);
    st::print("    亮度: 全黑={:.2f} 发光={:.2f}  可见像素: {} -> {}\n", s_dark.luma, s_unlit.luma,
              s_dark.lit, s_unlit.lit);
    report(s_unlit.luma > 20.0, "纯黑环境下发光体仍亮", std::format("luma={:.2f}", s_unlit.luma));
    report(s_unlit.lit > 1000, "发光体有实体像素", std::format("lit={}", s_unlit.lit));
    // 有环境光时：unlit 丢掉受光项 → 必须明显不同于旧语义
    const Stats s_lit = analyze(c_lit);
    const Stats s_unlit2 = analyze(c_unlit2);
    st::print("    有环境光时: unlit={:.2f} vs 受光语义={:.2f}\n", s_unlit2.luma, s_lit.luma);
    report(std::abs(s_unlit2.luma - s_lit.luma) > 1.0, "两种自发光语义不同",
           std::format("{:.2f} vs {:.2f}", s_unlit2.luma, s_lit.luma));
    save_png(c_unlit, out_dir + "/cap-emission-unlit.png");
  }

  // ══ 3. 点光距离衰减 ══
  st::print("\n── 3. 点光距离衰减（1/d²）──\n");
  {
    const auto point_at = [](float z) {
      RenderCase c{};
      c.lighting.ambient = 0.02f;
      c.lighting.diffuse = 0.0f;
      c.lighting.rim = 0.0f;
      Light pl{};
      pl.kind = LightKind::Point;
      pl.position = Vec3{0.0f, 0.0f, z};
      pl.color = Vec3{1.0f, 1.0f, 1.0f};
      pl.intensity = 1.2f;
      c.lighting.extra_lights[0] = pl;
      c.camera.eye = Vec3{0.0f, 0.0f, 4.0f};
      return c;
    };
    // 灯拉得足够近/远，让 1/d² 的差异明显；太近会饱和，太远会被环境光淹没
    const Stats near_light = analyze(render(point_at(0.9f)));
    const Stats far_light = analyze(render(point_at(2.9f)));
    st::print("    灯距 0.9 → 峰值 {:.2f}（均值 {:.2f}）；灯距 2.9 → 峰值 {:.2f}（均值 {:.2f}）\n",
              near_light.peak, near_light.luma, far_light.peak, far_light.luma);
    // 判据用**峰值**：近处那面被强照，均值会被背光半球拉低（见 Stats::peak 说明）
    report(near_light.peak > far_light.peak * 1.3, "近处峰值明显更亮",
           std::format("{:.2f} vs {:.2f}", near_light.peak, far_light.peak));

    // 半径截断：超出 radius 后应当只剩环境光
    // 同一个灯位（近处），只改半径：球面离灯约 0.3~0.9 单位
    RenderCase clipped = point_at(0.9f);
    clipped.lighting.extra_lights[0].radius = 3.0f;   // 远大于距离 → 衰减很小
    RenderCase outside = point_at(0.9f);
    outside.lighting.extra_lights[0].radius = 0.25f;  // 远小球半径 → 只有一小块被照亮
    const Stats in_r = analyze(render(clipped));
    const Stats out_r = analyze(render(outside));
    st::print("    radius=2.0 → {:.2f}；radius=0.5 → {:.2f}\n", in_r.luma, out_r.luma);
    report(in_r.luma > out_r.luma * 2.0, "radius 截断生效",
           std::format("{:.2f} vs {:.2f}", in_r.luma, out_r.luma));
  }

  // ══ 4. 聚光锥形 ══
  st::print("\n── 4. 聚光锥形（内锥外锥）──\n");
  {
    // 一堵墙正对相机，聚光灯从相机侧照过去 → 中心亮、四周暗。
    //
    // ## 为什么要用盒子而不是平面
    //
    // `Mesh::plane` 的法线方向与 `CullMode::Back` 的剔除方向绑定：朝向搞反时
    // 整块面被剔除，画面上什么都没有（实测：中心=0 角落=0，看起来像"聚光没生效"）。
    // 用扁盒子（每个面法线都朝外）就不依赖这个假设。
    const auto spot_scene = [](float inner, float outer) {
      RenderCase c{};
      // 给一点环境光：否则锥外区域是纯黑，与黑背景无法区分，
      // 判据会读到"一个像素都没有"（实测踩过，看起来像"聚光完全没生效"）。
      c.lighting.ambient = 0.08f;
      c.lighting.diffuse = 0.0f;
      c.lighting.rim = 0.0f;
      Light sp{};
      sp.kind = LightKind::Spot;
      sp.position = Vec3{0.0f, 0.0f, 2.5f};
      sp.direction = Vec3{0.0f, 0.0f, -1.0f};   // 朝 -Z 照
      sp.color = Vec3{1.0f, 1.0f, 1.0f};
      sp.intensity = 2.0f;
      sp.spot_inner_degrees = inner;
      sp.spot_outer_degrees = outer;
      c.lighting.extra_lights[0] = sp;
      c.camera.eye = Vec3{0.0f, 0.0f, 3.0f};
      c.mesh = Mesh::colored_sphere(0.5f, 24, Color{220, 220, 220, 255});
      return c;
    };
    const Canvas wide = render(spot_scene(30.0f, 40.0f));
    const Canvas narrow = render(spot_scene(5.0f, 12.0f));
    // 中心与角落的亮度比
    const auto ratio = [](const Canvas& c) {
      const Color center = c.pixel_at(kW / 2, kH / 2);
      // 球的边缘（偏离中心约 55px）——锥外/锥边的落点
      const Color corner = c.pixel_at(kW / 2, kH / 2 - 55);
      const double cl = 0.2126 * center.r + 0.7152 * center.g + 0.0722 * center.b;
      const double kl = 0.2126 * corner.r + 0.7152 * corner.g + 0.0722 * corner.b;
      return kl > 0.01 ? cl / kl : 999.0;
    };
    const double wide_ratio = ratio(wide);
    const double narrow_ratio = ratio(narrow);
    st::print("    中心/角落亮度比: 宽锥(30°/40°)={:.2f}  窄锥(5°/12°)={:.2f}\n", wide_ratio,
              narrow_ratio);
    // 判据：窄锥的「中心/边缘」对比必须明显强于宽锥。
    // 阈值 1.2 是按实测定的（宽 1.25 / 窄 1.70 → 比 1.36）：球面上离轴最远的点
    // 仍在窄锥内，所以对比不会像平面那样悬殊。要的是**方向正确且显著**，
    // 不是"数值超过某个想当然的数"。
    report(narrow_ratio > wide_ratio * 1.2, "窄锥的对比更强",
           std::format("{:.2f} vs {:.2f}（比 {:.2f}）", narrow_ratio, wide_ratio,
                       wide_ratio > 0 ? narrow_ratio / wide_ratio : 0.0));
    save_png(narrow, out_dir + "/cap-spot-narrow.png");
  }

  // ══ 5. AgX 的高光去饱和 ══
  st::print("\n── 5. AgX 色调映射（高饱和过曝要往白吐）──\n");
  {
    RenderCase hot{};
    hot.lighting.ambient = 0.0f;
    hot.lighting.diffuse = 0.0f;
    hot.material.emission = 6.0f;   // 严重过曝
    hot.material.emissive_color = Vec3{1.0f, 0.0f, 0.0f};
    hot.material.emissive_color_set = true;
    hot.material.emission_unlit = true;

    const Stats aces = analyze(render(hot, ToneMap::Aces));
    const Stats agx = analyze(render(hot, ToneMap::Agx));
    const Stats filmic = analyze(render(hot, ToneMap::Filmic));
    st::print("    饱和度: ACES={:.3f}  Filmic={:.3f}  **AgX={:.3f}**\n", aces.saturation,
              filmic.saturation, agx.saturation);
    st::print("    亮度:   ACES={:.1f}  Filmic={:.1f}  AgX={:.1f}\n", aces.luma, filmic.luma,
              agx.luma);
    // AgX 的关键特征：过曝时不保持纯红，而是去饱和
    report(agx.saturation < aces.saturation * 0.95, "AgX 比 ACES 更去饱和",
           std::format("{:.3f} < {:.3f}", agx.saturation, aces.saturation));
    save_png(render(hot, ToneMap::Agx), out_dir + "/cap-tonemap-agx.png");
    save_png(render(hot, ToneMap::Aces), out_dir + "/cap-tonemap-aces.png");
  }

  // ══ 6. 正交相机（远近同尺寸）══
  st::print("\n── 6. 正交相机（远近同尺寸）──\n");
  {
    const auto two_spheres = [](ProjectionKind kind) {
      Canvas canvas{kW, kH, 1.0f};
      canvas.clear(kBg);
      auto scene = Scene3D::create(kW, kH);
      if (!scene.has_value()) return canvas;
      Lighting lighting{};
      lighting.ambient = 0.9f;
      lighting.diffuse = 0.1f;
      lighting.rim = 0.0f;
      Camera cam{};
      cam.eye = Vec3{0.0f, 0.0f, 6.0f};
      cam.projection = kind;
      cam.ortho_scale = 2.4f;
      cam.far_z = 60.0f;
      (*scene)->begin_frame(kBg);
      (*scene)->set_camera(cam);
      (*scene)->set_lighting(lighting);
      // 一个近（z=0.5）、一个远（z=-2.5），尺寸相同
      (*scene)->draw_mesh(Mesh::colored_sphere(0.5f, 24, Color{230, 230, 230, 255}),
                          Mat4::translation(Vec3{-0.9f, 0.0f, 0.5f}));
      (*scene)->draw_mesh(Mesh::colored_sphere(0.5f, 24, Color{230, 230, 230, 255}),
                          Mat4::translation(Vec3{0.9f, 0.0f, -2.5f}));
      (*scene)->end_frame(
          canvas, st::math::Rect{0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH)});
      (*scene)->flush();
      return canvas;
    };
    // 量每个球的宽度（扫描行上有亮像素的列数）
    const auto ball_width = [](const Canvas& c, int x_center) {
      int count = 0;
      for (int x = 0; x < c.physical_width(); ++x) {
        const Color p = c.pixel_at(x, kH / 2);
        if (0.2126 * p.r + 0.7152 * p.g + 0.0722 * p.b > 40.0 &&
            std::abs(x - x_center) < kW / 4) {
          ++count;
        }
      }
      return count;
    };
    const Canvas persp = two_spheres(ProjectionKind::Perspective);
    const Canvas ortho = two_spheres(ProjectionKind::Orthographic);
    // 近的那个在左侧，远的在右侧
    const int p_near = ball_width(persp, kW / 4);
    const int p_far = ball_width(persp, 3 * kW / 4);
    const int o_near = ball_width(ortho, kW / 4);
    const int o_far = ball_width(ortho, 3 * kW / 4);
    st::print("    透视: 近球宽={}px 远球宽={}px（比 {:.2f}）\n", p_near, p_far,
              p_far > 0 ? static_cast<double>(p_near) / p_far : 0.0);
    st::print("    正交: 近球宽={}px 远球宽={}px（比 {:.2f}）\n", o_near, o_far,
              o_far > 0 ? static_cast<double>(o_near) / o_far : 0.0);
    report(p_near > p_far * 1.15, "透视下近大远小",
           std::format("{} vs {}", p_near, p_far));
    report(std::abs(o_near - o_far) <= 2, "正交下同样大小",
           std::format("{} vs {}（差 {}）", o_near, o_far, std::abs(o_near - o_far)));
    save_png(ortho, out_dir + "/cap-ortho.png");
  }


  // ══ 7. 阴影贴图 ：**已知缺陷，不计入通过** ══
  //
  // 现状：贴图能建、能读出遮挡，但**过度遮蔽**——地面整片被压成 0
  //（无阴影 84.4 → 有阴影 0.0），而不是只在球下方变暗。
  //
  // 已定位的部分：投影相机正确（位于场景上方朝下）、几何收集正确
  //（282~513 个三角形）、贴图覆盖约 0.5~0.8% 像素（斜视地面被压扁，属正常）。
  // 未定位的部分：`shadow_factor_at` 的深度比较在某些采样上恒判"在阴影里"。
  //
  // 为何先不启用：它现在会让画面**变错**（整片发黑），比"没有阴影"更糟。
  // 所以 `set_shadow_map` 保留在 API 里但**默认不启用**，
  // 等定位到根因再开。这个状态记在 BACKLOG 与文档里，不藏着。
  st::print("\n── 7. 阴影贴图：已知缺陷（过度遮蔽），本版不启用 ──\n");
  st::print("    详见 BACKLOG「三维：阴影贴图过度遮蔽」\n");

  st::print("\n{}\n", failures == 0 ? "全部通过" : std::format("有 {} 项失败", failures));
  return failures == 0 ? 0 : 1;
}

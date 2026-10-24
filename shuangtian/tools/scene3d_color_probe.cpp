/// 颜色管线接入三维渲染的**视觉对照**探针：同一场景、同一光照，
/// 分别用「旧行为（线性值直存）」与「新输出链（色调映射 + sRGB）」渲染，
/// 落盘两张 PNG 供人工核对，并打印关键的统计量。
///
/// ## 为什么要这个探针（而不是只看断言）
///
/// 颜色管线的改动可以通过所有单元测试，但**画面可能仍然不对**：
/// 断言测的是"函数对不对"，这里测的是"接进去以后整幅画对不对"。
/// 实测意义：把线性值当 sRGB 存时，整幅**系统性偏暗**——
/// 这种偏差任何断言都不会报，只有看图或量平均亮度才能发现。
///
/// ## 三个统计量
///
/// - `mean_linear`：画面的平均**线性**亮度（转换回线性域后量）。
///   它回答"整体亮度对不对"。旧做法会偏低。
/// - `clipped_ratio`：线性值 ≥0.999 的像素占比（过曝面积）。
///   旧做法先 clamp 再存，高光会被压成一片纯白——这个比例会很高。
/// - `mean_srgb`：直接对存储字节求均值（**人在屏幕上看到的**）。
///
/// 用法：`scene3d_color_probe <输出目录>`
///
/// 注意：本探针**不依赖 GPU**（软件腿即可），保证在无显卡机器上也能跑。

#include <cstdio>
#include <string>
#include <vector>

#include "st/codec/png.hpp"
#include "st/core/print.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/color_pipeline.hpp"
#include "st/raster/mesh.hpp"
#include "st/raster/scene3d.hpp"

namespace {

using st::math::Color;
using st::math::Vec3;
using st::raster::Canvas;
using st::raster::Lighting;
using st::raster::Mesh;
using st::raster::Scene3D;
using st::raster::ToneMap;

constexpr int kWidth = 640;
constexpr int kHeight = 640;

/// 用指定光照渲染一个网格到画布（软件腿，保证可跑）。
///
/// ## 为何背景用深灰而不是纯黑
///
/// 纯黑背景下，sRGB 编码的暗部差异“看不见”——而暗部恰好是旧做法伤得最重的地方
/// （线性 0.05 被当成 sRGB 存后，编码回去只剩 0.005）。
/// 深灰底 + 中等亮度球，才能让“旧做法偏暗”在图上看得见。
constexpr st::math::Color kBackground{26, 28, 36, 255};

auto render(const Mesh& mesh, const Lighting& lighting) -> Canvas {
  auto scene = Scene3D::create(kWidth, kHeight);
  if (!scene.has_value()) return Canvas{kWidth, kHeight, 1.0f};
  Canvas canvas{kWidth, kHeight, 1.0f};
  canvas.clear(kBackground);
  Scene3D& s = *scene.value();
  s.begin_frame(kBackground);
  st::raster::Camera camera{};
  camera.eye = Vec3{0.0f, 0.9f, 3.4f};
  camera.target = Vec3{0.0f, 0.0f, 0.0f};
  s.set_camera(camera);
  s.set_lighting(lighting);
  s.draw_mesh(mesh, st::math::Mat4::identity());
  s.end_frame(canvas, st::math::Rect{0.0f, 0.0f, static_cast<float>(kWidth),
                                     static_cast<float>(kHeight)});
  s.flush();
  return canvas;
}

struct Stats {
  double mean_srgb{0.0};
  double mean_linear{0.0};
  double clipped_ratio{0.0};
  int lit_pixels{0};
};

/// 把“走完新链（None 档）”的图**反推**回旧行为的字节。
///
/// ## 为何要反推而不是直接渲染两次
///
/// 旧行为是 `byte = lround(lit * 255)`（线性值直接当字节，不经 sRGB 编码）。
/// 代码已改成统一走 `encode_pixel`，所以旧行为已经没有渲染路径了。
/// 但两者一一对应：
///   新(None) 字节 = srgb8(lit)  ⇒  lit = srgb_to_linear(byte/255)
///   旧字节        = lround(lit * 255)
/// 所以从 None 档的图能**精确反推**旧图（除 8bit 量化误差）。
///
/// ## 这个对照要证明什么
///
/// 旧行为把线性值当字节存 ⇒ 屏幕上显示的线性亮度 ≈ lit²·²，**系统性偏暗**。
/// 拿这两个图对比，差异就是“多年以来画面偏暗了多少”。
auto reconstruct_legacy(const Canvas& none_canvas) -> Canvas {
  Canvas out{none_canvas.physical_width(), none_canvas.physical_height(), 1.0f};
  for (int y = 0; y < out.physical_height(); ++y) {
    for (int x = 0; x < out.physical_width(); ++x) {
      const Color c = none_canvas.pixel_at(x, y);
      const auto back = [](std::uint8_t v) -> std::uint8_t {
        const float lin = st::raster::srgb_to_linear(static_cast<float>(v) / 255.0f);
        const long q = std::lround(lin * 255.0f);
        return static_cast<std::uint8_t>(q < 0 ? 0 : (q > 255 ? 255 : q));
      };
      out.set_pixel(x, y, Color{back(c.r), back(c.g), back(c.b), c.a});
    }
  }
  return out;
}

/// 量一幅画：把存储字节**解码回线性**再统计（这样"偏暗"才是可比的）。
///
/// 背景判定不能看“是否纯黑”：`begin_frame` 的清屏色经过 sRGB 编码后
/// 不一定是 0（清 0.05 线性 → 0.23 sRGB）。所以这里用**与清屏色的距离**判背景。
auto measure(const Canvas& canvas, Color background) -> Stats {
  Stats st{};
  const int w = canvas.physical_width();
  const int h = canvas.physical_height();
  double sum_srgb = 0.0;
  double sum_linear = 0.0;
  int clipped = 0;
  int lit = 0;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const Color c = canvas.pixel_at(x, y);
      // 与背景色差很小 → 背景（不是实体）
      const int dr = static_cast<int>(c.r) - static_cast<int>(background.r);
      const int dg = static_cast<int>(c.g) - static_cast<int>(background.g);
      const int db = static_cast<int>(c.b) - static_cast<int>(background.b);
      if (dr * dr + dg * dg + db * db < 100) continue;
      ++lit;
      const double srgb = (0.299 * c.r + 0.587 * c.g + 0.114 * c.b) / 255.0;
      sum_srgb += srgb;
      const double lin = static_cast<double>(st::raster::srgb_to_linear(
          static_cast<float>(srgb)));
      sum_linear += lin;
      if (srgb >= 0.999) ++clipped;
    }
  }
  if (lit > 0) {
    st.mean_srgb = sum_srgb / lit;
    st.mean_linear = sum_linear / lit;
    st.clipped_ratio = static_cast<double>(clipped) / lit;
    st.lit_pixels = lit;
  }
  return st;
}

/// 把画布写成 PNG（与其它探针同一路径：`codec::png_write_file`）。
///
/// ## 字节序坑（实测踩到）
///
/// 画布像素是 **0xAARRGGBB**（R 在高位、A 在最低位），而且通道已**预乘**。
/// 一开始按 `0xRRGGBBAA` 展开，于是“蓝色通道”取到了 alpha（恒 255）——
/// 图看上去一片蓝，而统计量却在正常变化。**一定要经 `unpremultiply`**。
auto save_png(const Canvas& canvas, const std::string& path) -> bool {
  st::codec::PngImage image;
  image.width = static_cast<std::uint32_t>(canvas.physical_width());
  image.height = static_cast<std::uint32_t>(canvas.physical_height());
  const auto pixels = canvas.pixels();
  image.rgba.resize(static_cast<std::size_t>(image.width) * image.height * 4);
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    const st::math::Color c = st::math::unpremultiply(pixels[i]);
    image.rgba[i * 4 + 0] = c.r;
    image.rgba[i * 4 + 1] = c.g;
    image.rgba[i * 4 + 2] = c.b;
    image.rgba[i * 4 + 3] = c.a;
  }
  return static_cast<bool>(st::codec::png_write_file(path, image));
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const std::string out_dir = argc > 1 ? argv[1] : ".";
  st::print("三维颜色管线对照探针（{}x{}，软件腿，保证无显卡也能跑）\n\n", kWidth, kHeight);

  // 场景：一个球，用**自定义中调灰**。
  //
  // ## 为何写 `vertices[]` 是错的（实测踩到）
  //
  // `Mesh::vertices` 存的是**线性**值（`mesh.cpp` 的 `to_unit` 已经把 sRGB 字节
  // 解码过）。直接写 `0.55` 等于当线性值用，绕过了“sRGB→线性”那一层，
  // 于是本次修改（顶点色解码）在探针里**完全看不出来**——实测数值一字未改。
  //
  // 正解：走 `Mesh::sphere("半径", "圆度", Color)` 这个公开入口，传 sRGB 字节。
  // 这才是调用方（裁云/界面）的真实用法。
  const Mesh sphere = Mesh::colored_sphere(0.85f, 24, st::math::Color{140, 128, 115, 255});
  Lighting lighting{};
  lighting.key_direction = Vec3{0.45f, 0.8f, 0.55f};

  // ① 旧行为：线性值当字节直存（已无渲染路径，用 None 档反推）
  Lighting none = lighting;
  none.tone_map = ToneMap::None;
  const Canvas none_canvas = render(sphere, none);
  const Canvas old_canvas = reconstruct_legacy(none_canvas);

  // ② 新输出链：ACES + sRGB
  Lighting modern = lighting;
  modern.tone_map = ToneMap::Aces;
  const Canvas new_canvas = render(sphere, modern);

  const Stats a = measure(old_canvas, kBackground);
  const Stats b = measure(new_canvas, kBackground);

  st::print("{:<28}{:>12}{:>12}\n", "指标", "旧(None直存)", "新(ACES+sRGB)");
  st::print("{:<28}{:>12.4f}{:>12.4f}\n", "平均 sRGB 亮度（屏幕上）", a.mean_srgb, b.mean_srgb);
  st::print("{:<28}{:>12.4f}{:>12.4f}\n", "平均线性亮度", a.mean_linear, b.mean_linear);
  st::print("{:<28}{:>12.2f}{:>12.2f}\n", "过曝像素占比 %", a.clipped_ratio * 100.0,
            b.clipped_ratio * 100.0);
  st::print("{:<28}{:>12}{:>12}\n", "实体像素数", a.lit_pixels, b.lit_pixels);

  const std::string p_old = out_dir + "/scene3d_color_legacy.png";
  const std::string p_new = out_dir + "/scene3d_color_modern.png";
  const bool ok_old = save_png(old_canvas, p_old);
  const bool ok_new = save_png(new_canvas, p_new);
  st::print("\n落盘：{}\n", ok_old ? p_old : std::string("(失败)"));
  st::print("落盘：{}\n", ok_new ? p_new : std::string("(失败)"));

  // 判读：新链的线性亮度应当**高于**旧的（旧的把线性当 sRGB，等于额外压暗一次）
  st::print("\n判读：新链平均线性亮度 {:.4f} vs 旧 {:.4f}（{}）\n", b.mean_linear, a.mean_linear,
            b.mean_linear > a.mean_linear ? "更亮，符合预期" : "未提升，需检查");
  return (ok_old && ok_new) ? 0 : 1;
}

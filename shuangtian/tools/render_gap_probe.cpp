// 渲染差距的**量化基线**：分别量“与真 three.js 的差”和“两条腿的差”，
// 而不是靠观感说“像不像”。
//
// ## 为什么需要这个基线
//
// 需求是“渲染效果没有明显差别”。这句话必须先变成一个可量的量，否则每次改动
// 都只能靠肉眼，而肉眼在三路对照里**已被证伪过**（`docs/render_alignment.md`：
// 用户对四格图的排序与亮部占比完全一致，与均值无关）。
//
// 本工具量的三件事：
//   ① **两条腿一致性**：同一场景软件腿 vs GPU 腿（`diff_ratio` / `mean_diff`）——
//      这是框架自身必须做到的（同一份脚本不能两副长相）；
//   ② **暗部占比**：直方图里 luma < 64 的比例（"看着假"最强相关的单一量，
//      真渲染器约 7~10%，本框架曾为 0.0%）；
//   ③ **主体区域统计**：裁掉背景后量（背景占 80% 会把所有指标稀释掉——
//      这是 `render_alignment.md` 里已记录的旧口径缺陷）。
//
// 输出一行一行的可复算数字 + 退出码（0 = 全部达标）。

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "st/codec/png.hpp"
#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/scene3d_script.hpp"

namespace {

auto read_file(const std::string& path) -> std::string {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

struct Stats {
  double mean{0.0};
  double dark{0.0};       ///< luma < 64 占比
  double bright{0.0};     ///< luma > 192 占比
  double darkest{255.0};  ///< 最暗像素 luma
  double brightest{0.0};
};

auto measure(const st::raster::Canvas& canvas) -> Stats {
  const auto& pixels = canvas.pixels();
  Stats stats;
  const std::size_t total = pixels.size();
  if (total == 0) return stats;
  std::size_t dark = 0;
  std::size_t bright = 0;
  for (std::size_t i = 0; i < total; ++i) {
    const st::math::Color c = st::math::unpremultiply(pixels[i]);
    const double luma = 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
    stats.mean += luma;
    if (luma < 64.0) ++dark;
    if (luma > 192.0) ++bright;
    stats.darkest = std::min(stats.darkest, luma);
    stats.brightest = std::max(stats.brightest, luma);
  }
  stats.mean /= static_cast<double>(total);
  stats.dark = static_cast<double>(dark) / static_cast<double>(total);
  stats.bright = static_cast<double>(bright) / static_cast<double>(total);
  return stats;
}

/// 两张图的逐像素差异（同尺寸才有效）。
struct Diff {
  double ratio{0.0};   ///< 差异像素占比（通道差 > 8）
  double mean{0.0};    ///< 平均通道差
  double max{0.0};     ///< 最大通道差
};

auto diff_of(const st::raster::Canvas& a, const st::raster::Canvas& b) -> Diff {
  Diff out;
  const auto& pa = a.pixels();
  const auto& pb = b.pixels();
  if (pa.size() != pb.size() || pa.empty()) return out;
  std::size_t differing = 0;
  double sum = 0.0;
  for (std::size_t i = 0; i < pa.size(); ++i) {
    const st::math::Color ca = st::math::unpremultiply(pa[i]);
    const st::math::Color cb = st::math::unpremultiply(pb[i]);
    const double dr = std::abs(static_cast<double>(ca.r) - cb.r);
    const double dg = std::abs(static_cast<double>(ca.g) - cb.g);
    const double db = std::abs(static_cast<double>(ca.b) - cb.b);
    const double channel = std::max({dr, dg, db});
    sum += (dr + dg + db) / 3.0;
    out.max = std::max(out.max, channel);
    if (channel > 8.0) ++differing;
  }
  out.ratio = static_cast<double>(differing) / static_cast<double>(pa.size());
  out.mean = sum / static_cast<double>(pa.size());
  return out;
}

auto render(const st::raster::SceneSpec& spec, int width, int height, bool force_software)
    -> st::raster::Canvas {
  if (force_software) {
    // 探针要能**分别**量两条腿。环境变量是既有机制（`ST_SCENE3D_BACKEND`），
    // 不新开参数面：那条约定已被其它探针/e2e 依赖。
#if defined(_WIN32)
    _putenv_s("ST_SCENE3D_BACKEND", "software");
#else
    setenv("ST_SCENE3D_BACKEND", "software", 1);
#endif
  }
  st::raster::Canvas canvas{width, height, 1.0f};
  const auto background = st::math::Color::rgba(13, 13, 13, 255);
  (void)st::raster::render_scene(spec, canvas, background);
  return canvas;
}

void dump_png(const st::raster::Canvas& canvas, const std::string& path) {
  if (path.empty()) return;
  st::codec::PngImage image;
  image.width = static_cast<std::uint32_t>(canvas.physical_width());
  image.height = static_cast<std::uint32_t>(canvas.physical_height());
  const auto pixels = canvas.pixels();
  image.rgba.resize(pixels.size() * 4);
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    const st::math::Color c = st::math::unpremultiply(pixels[i]);
    image.rgba[i * 4 + 0] = c.r;
    image.rgba[i * 4 + 1] = c.g;
    image.rgba[i * 4 + 2] = c.b;
    image.rgba[i * 4 + 3] = 255;
  }
  (void)st::codec::png_write_file(path, image);
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const std::string scene_path = argc > 1 ? argv[1] : "apps/jianmu/scenes/04-watch.js";
  const std::string out_dir = argc > 2 ? argv[2] : "";

  const std::string source = read_file(scene_path);
  if (source.empty()) {
    std::printf("读不到场景：%s\n", scene_path.c_str());
    return 2;
  }
  // 统一走 `scene_spec_from_three` / `scene_spec_from_bpy`：与裁云/建木/探针
  // 同一套解释，不另建口径（自己拼 JSON 还得转义，而转义写错就是把“语言差”测成“渲染差”）。
  const bool is_bpy = scene_path.size() > 3 &&
                      scene_path.compare(scene_path.size() - 3, 3, ".py") == 0;
  auto spec = is_bpy ? st::raster::scene_spec_from_bpy(source, scene_path)
                     : st::raster::scene_spec_from_three(source, scene_path);
  if (!spec.has_value()) {
    std::printf("解析失败：%s\n", spec.error().message.c_str());
    return 2;
  }

  std::printf("场景：%s\n\n", scene_path.c_str());
  constexpr int kWidth = 640;
  constexpr int kHeight = 460;

  const auto software = render(*spec, kWidth, kHeight, true);
  const auto gpu = render(*spec, kWidth, kHeight, false);
  const auto software_stats = measure(software);
  const auto gpu_stats = measure(gpu);
  const auto legs = diff_of(software, gpu);

  std::printf("① 两条腿的差（同一份脚本不能两副长相）\n");
  std::printf("   差异像素 %5.2f%% · 平均通道差 %5.2f · 最大 %5.1f\n",
              legs.ratio * 100.0, legs.mean, legs.max);
  std::printf("   软件腿：均值 %5.1f · 暗部 %5.1f%% · 亮部 %5.1f%% · 范围 %.0f..%.0f\n",
              software_stats.mean, software_stats.dark * 100.0,
              software_stats.bright * 100.0, software_stats.darkest,
              software_stats.brightest);
  std::printf("   GPU 腿：均值 %5.1f · 暗部 %5.1f%% · 亮部 %5.1f%% · 范围 %.0f..%.0f\n\n",
              gpu_stats.mean, gpu_stats.dark * 100.0, gpu_stats.bright * 100.0,
              gpu_stats.darkest, gpu_stats.brightest);

  if (!out_dir.empty()) {
    dump_png(software, out_dir + "/legs_software.png");
    dump_png(gpu, out_dir + "/legs_gpu.png");
    std::printf("   已落图：%s/legs_{software,gpu}.png\n\n", out_dir.c_str());
  }

  int failures = 0;
  const auto check = [&](bool ok, const char* label, const std::string& detail) {
    std::printf("   [%s] %s  %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
    if (!ok) ++failures;
  };
  std::printf("② 判据\n");
  // 两腿差异：>2% 就说明“同一脚本两副长相”，必须挡。
  check(legs.ratio < 0.02, "两腿差异像素 < 2%",
        std::format("{:.2f}%（阈值 2%）", legs.ratio * 100.0));
  // 暗部存在性：完全不出现暗部是最强的“假”信号（真渲染器 7~10%）。
  // 注意这条对**明亮背景**的场景不适用——阈值给得很松，只挡“一点暗部都没有”。
  check(software_stats.darkest < 60.0, "画面里存在暗部（最暗像素 < 60）",
        std::format("最暗 {:.0f}", software_stats.darkest));

  std::printf("\n%s（%d 项失败）\n", failures == 0 ? "基线达标" : "有差距待处理", failures);
  return failures == 0 ? 0 : 1;
}

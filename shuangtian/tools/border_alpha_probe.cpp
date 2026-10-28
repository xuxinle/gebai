// 圆角边框环的 **alpha 传递** 诊断：透明底上画不同 alpha 的环，量输出像素的 alpha。
//
// ## 为什么单独立一个探针
//
// `ring_raster_probe` 量的是**墨量（面积）精度**——它用不透明的黑色画在白底上，
// 只关心「覆盖对不对」。而本条要回答的是另一个问题：**颜色自带的 alpha 有没有被尊重**。
// 两者正交：一条路径可以面积完全正确、同时把 alpha 丢掉（实色底上看不出来，
// 一旦画在浅色/深色 UI 就会变成一道刺眼的实线）。
//
// ## 判据
//
// 透明底、纯色画笔、无裁剪。对 alpha ∈ {0, 8, 25, 50, 100, 255}%（字节 0/20/64/128/255）
// 各画一次 1 逻辑 px 宽的圆角环，取**四条直边中点的像素 alpha**：
//   · 应与输入 alpha 一致（±2/255 的量化零头）；
//   · 四条边应彼此一致（差 ≤2）——不一致说明环的两侧路径权重不对称；
//   · alpha=0 必须**一个像素都不写**（画布保持全透明）。
//
// 同时跑「实心圆角矩形」作对照：若实心正确而环错误，根因就在环路径；
// 若两者都错，根因在更底层的混合。
//
// 同时跑「几何贴图缓存 开/关」两遍：若开缓存错而关缓存对，根因在缓存契约。
//
// 用法： border_alpha_probe [scale]

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <string>

#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/math/path.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"

namespace {

using st::math::Path;
using st::raster::Canvas;

constexpr int kLogicalW = 200;
constexpr int kLogicalH = 160;
constexpr float kRadius = 14.0f;
constexpr float kWidth = 1.0f;
const st::math::Rect kBox{40.0f, 40.0f, 120.0f, 80.0f};

/// 四条直边中点的**物理**坐标（由逻辑框换算；scale 参与）。
struct EdgeSample {
  const char* name;
  st::math::Point logical;
};

[[nodiscard]] auto edge_samples() -> std::array<EdgeSample, 4> {
  const float cx = kBox.x + kBox.width * 0.5f;
  const float cy = kBox.y + kBox.height * 0.5f;
  return {EdgeSample{"left", st::math::Point{kBox.x, cy}},
          EdgeSample{"right", st::math::Point{kBox.right(), cy}},
          EdgeSample{"top", st::math::Point{cx, kBox.y}},
          EdgeSample{"bottom", st::math::Point{cx, kBox.bottom()}}};
}

/// 在一条边的中点取 **5×5 窗口**的 alpha 最大值。
///
/// 为什么扫二维窗口而不是沿一个方向 ±2：
/// 边框是**向内**的环形填充（外缘对齐 rect 边），所以落在 rect 上的逻辑坐标
/// 换算成物理像素后，可能正好是「边之外」的第一个像素；而哪一侧在外，取决于
/// 是左/上边还是右/下边。先前按「横边扫 x、竖边扫 y」只对了左/上两边，
/// 右/下边恒读到 0（量尺缺陷，不是被测对象）。二维窗口对四条边一视同仁。
/// 窗口 ±2 物理像素足够覆盖 1 逻辑 px 的边在任意 DPI 下的落点，
/// 又不会吃到相邻的另一条边（最短边 80 逻辑 px）。
[[nodiscard]] auto peak_alpha_on_edge(const Canvas& canvas, st::math::Point logical, float scale)
    -> int {
  const int px = static_cast<int>(std::lround(logical.x * scale));
  const int py = static_cast<int>(std::lround(logical.y * scale));
  int best = 0;
  for (int dy = -2; dy <= 2; ++dy) {
    for (int dx = -2; dx <= 2; ++dx) {
      const int x = px + dx;
      const int y = py + dy;
      if (x < 0 || y < 0 || x >= canvas.physical_width() || y >= canvas.physical_height()) continue;
      const int a = canvas.pixel_at(x, y).a;
      if (a > best) best = a;
    }
  }
  return best;
}

/// 画一次，回四条边的峰值 alpha（顺序：left/right/top/bottom）。
[[nodiscard]] auto measure_edges(const Path& path, std::uint8_t color_alpha, float scale,
                                 bool cache_enabled) -> std::array<int, 4> {
  Canvas canvas = Canvas::for_logical_size(kLogicalW, kLogicalH, scale);
  canvas.clear(st::math::Color{0, 0, 0, 0});
  canvas.set_geometry_cache_enabled(cache_enabled);
  canvas.set_shadow_run_grouping_enabled(cache_enabled);
  canvas.set_alpha_table_enabled(cache_enabled);
  canvas.fill_path(path, st::raster::Paint::solid(st::math::Color{255, 255, 255, color_alpha}));
  std::array<int, 4> out{};
  const auto samples = edge_samples();
  for (std::size_t i = 0; i < samples.size(); ++i) {
    out[i] = peak_alpha_on_edge(canvas, samples[i].logical, scale);
  }
  return out;
}

/// 全画布的 alpha 总和（用于断言 alpha=0 时「一个像素都没写」）。
///
/// 用 `std::int64_t` 而不是 `long`：`long` 在 Windows 上是 32 位（`CONVENTIONS` §10），
/// 累加整幅画布的 alpha 虽然离溢出还远，但这条禁令是**按类型**拦的，
/// 写成 `long` 会被 lint L17 拦下——宽窄不重要，一致才重要。
[[nodiscard]] auto total_alpha(const Path& path, std::uint8_t color_alpha, float scale)
    -> std::int64_t {
  Canvas canvas = Canvas::for_logical_size(kLogicalW, kLogicalH, scale);
  canvas.clear(st::math::Color{0, 0, 0, 0});
  canvas.fill_path(path, st::raster::Paint::solid(st::math::Color{255, 255, 255, color_alpha}));
  std::int64_t sum = 0;
  for (int y = 0; y < canvas.physical_height(); ++y) {
    for (int x = 0; x < canvas.physical_width(); ++x) sum += canvas.pixel_at(x, y).a;
  }
  return sum;
}

struct Row {
  const char* label;
  std::uint8_t alpha;
  std::array<int, 4> ring;
  std::array<int, 4> solid;
};

[[nodiscard]] auto max_of(const std::array<int, 4>& v) -> int {
  int m = 0;
  for (int x : v) m = x > m ? x : m;
  return m;
}
[[nodiscard]] auto min_of(const std::array<int, 4>& v) -> int {
  int m = v[0];
  for (int x : v) m = x < m ? x : m;
  return m;
}

void report_group(st::probe::result& verdict, const char* group, float scale, bool cache_enabled,
                  bool& any_bad) {
  st::log::out("\n--- {}（scale={:.1f}，几何贴图缓存={}）---\n", group, static_cast<double>(scale),
            cache_enabled ? "开" : "关");
  st::log::out("{:>10} | {:>17} | {:>17}\n", "输入 alpha", "环 left/right/top/bot", "实心 left/right/top/bot");

  for (const std::uint8_t alpha : {std::uint8_t{255}, std::uint8_t{128}, std::uint8_t{64},
                                   std::uint8_t{20}, std::uint8_t{0}}) {
    const auto ring = measure_edges(st::math::make_rounded_border_ring(kBox, kRadius, kWidth),
                                    alpha, scale, cache_enabled);
    Path solid;
    solid.add_rounded_rect(kBox, kRadius);
    const auto filled = measure_edges(solid, alpha, scale, cache_enabled);
    st::log::out("{:>10} | {:>17} | {:>17}\n", static_cast<int>(alpha),
              std::format("{}/{}/{}/{}", ring[0], ring[1], ring[2], ring[3]),
              std::format("{}/{}/{}/{}", filled[0], filled[1], filled[2], filled[3]));

    // ① 环的四边应等于输入 alpha（±3 容忍量化零头）
    const int ring_peak = max_of(ring);
    if (alpha == 0) {
      if (ring_peak != 0) {
        any_bad = true;
        verdict.add(std::format("环 alpha=0 不写像素 ({})", group), false,
                    std::format("仍写出峰值 {}", ring_peak));
      }
      continue;
    }
    if (std::abs(ring_peak - static_cast<int>(alpha)) > 3) {
      any_bad = true;
      verdict.add(std::format("环 alpha 保真 ({}, 输入 {})", group, static_cast<int>(alpha)),
                  false, std::format("实测峰值 {}", ring_peak));
    }
    // ② 四边应彼此一致（环的两侧路径权重不对称会在这里现形）
    if (max_of(ring) - min_of(ring) > 3) {
      any_bad = true;
      verdict.add(std::format("环四边一致 ({}, 输入 {})", group, static_cast<int>(alpha)), false,
                  std::format("max {} - min {} = {}", max_of(ring), min_of(ring),
                              max_of(ring) - min_of(ring)));
    }
  }
  // alpha=0 的全画布断言（比只看边更硬）
  const std::int64_t zero_total =
      total_alpha(st::math::make_rounded_border_ring(kBox, kRadius, kWidth), 0, scale);
  if (zero_total != 0) {
    any_bad = true;
    verdict.add(std::format("环 alpha=0 全画布为空 ({})", group), false,
                std::format("alpha 总和 {}", zero_total));
  }
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "圆角边框环的 alpha 传递诊断：透明底上量四条直边的峰值 alpha 是否等于输入",
      .criterion = "四条边峰值 alpha 各自与输入一致（±3），四边彼此一致（极差 ≤3），"
                   "alpha=0 时一个像素都不写",
      .usage_args = "[scale]",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("border_alpha_probe", spec, opts)) return st::probe::exit_ok;
  st::probe::result verdict{};

  const float scale =
      opts.positional(0) ? static_cast<float>(std::atof(std::string(*opts.positional(0)).c_str()))
                         : 1.0f;
  st::log::out("=== 圆角边框环 alpha 传递（逻辑框 120×80，r=14，宽=1）===\n");

  bool any_bad = false;
  report_group(verdict, "缓存开", scale, true, any_bad);
  report_group(verdict, "缓存关", scale, false, any_bad);

  if (!any_bad) verdict.add("alpha 传递", true, "全部档位与四边均一致");
  verdict.conclude();
  return st::probe::main_of(spec, opts, verdict);
}

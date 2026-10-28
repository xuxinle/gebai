// **三带拆分 vs 整块光栅化：谁更准？**（不是“谁更快”）
//
// 背景：让 `fill_rect(radius>0)` 走几何贴图缓存后，贴图内容是**整块路径光栅化**，
// 而关闭缓存时走的是**三带拆分**。两者的 A/B 探针（`sprite_equiv_probe`）报
// 「1 像素 / 通道差 1」——这说明“三带拆分 ≡ 整块”在既有测试的 8 个档位之外并不严格成立。
//
// 本探针回答该保留哪一条：用**高倍超采样**作参考真值，分别量两者与真值的偏差。
// 判据：偏差更小的那条才是“更准”，另一条是它的近似。
//
// 这不是“性能”探针，而是“正确性归因”探针——差异出现时必须能说清谁对。

#include <cmath>
#include <cstdint>
#include <format>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/math/path.hpp"

namespace {

using st::math::Color;
using st::math::Rect;
using st::raster::Canvas;
using st::raster::Paint;

/// 超高倍超采样得到“参考真值”覆盖率（只用于比像素，不作交付）。
///
/// 做法：把同一形状放大 `kSuper` 倍画到一张大画布上（1 倍下每像素对应 kSuper² 个子像素），
/// 每个子像素取 alpha 的算术平均即为该像素的真实覆盖率。
[[nodiscard]] auto supersampled_coverage(Rect rect, float radius, int width, int height,
                                         int kSuper) -> std::vector<double> {
  const int big_w = width * kSuper;
  const int big_h = height * kSuper;
  Canvas big(big_w, big_h, 1.0f);
  big.clear(Color{0, 0, 0, 0});
  st::math::Path path;
  path.add_rounded_rect(Rect{rect.x * static_cast<float>(kSuper),
                             rect.y * static_cast<float>(kSuper),
                             rect.width * static_cast<float>(kSuper),
                             rect.height * static_cast<float>(kSuper)},
                        radius * static_cast<float>(kSuper));
  big.fill_path(path, Paint::solid(Color{255, 255, 255, 255}));

  std::vector<double> out(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0.0);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      std::uint64_t sum = 0;
      for (int sy = 0; sy < kSuper; ++sy) {
        for (int sx = 0; sx < kSuper; ++sx) {
          const auto pixel = big.pixel_at(x * kSuper + sx, y * kSuper + sy);
          sum += pixel.a;
        }
      }
      const double mean = static_cast<double>(sum) / static_cast<double>(kSuper * kSuper);
      out[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
          static_cast<std::size_t>(x)] = mean / 255.0;
    }
  }
  return out;
}

/// 把一张画布的 alpha 取出成 0~1 覆盖率。
[[nodiscard]] auto canvas_coverage(const Canvas& canvas, int width, int height)
    -> std::vector<double> {
  std::vector<double> out(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0.0);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      out[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
          static_cast<std::size_t>(x)] = static_cast<double>(canvas.pixel_at(x, y).a) / 255.0;
    }
  }
  return out;
}

[[nodiscard]] auto total_abs_error(const std::vector<double>& actual,
                                   const std::vector<double>& truth) -> double {
  double sum = 0.0;
  for (std::size_t index = 0; index < actual.size() && index < truth.size(); ++index) {
    sum += std::abs(actual[index] - truth[index]);
  }
  return sum;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "三带拆分 vs 整块光栅化：与超高倍超采样真值的偏差",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("round_rect_accuracy_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  constexpr int kWidth = 200;
  constexpr int kHeight = 160;
  constexpr int kSuper = 8;
  // **背景必须透明**：`pixel_at` 返回的是非预乘色，若底不透明则 alpha 恒为 255，
  // 覆盖率的差异会被完全掩盖（实测两个变体算出**一模一样**的偏差 22455.07——
  // 那就是量法失效的信号，不是“两者真的一样准”）。
  const Color background{0, 0, 0, 0};
  const Color ink{60, 90, 160, 255};

  struct Case {
    const char* name;
    Rect rect;
    float radius;
  };
  const Case cases[] = {
      {"对齐整数坐标", Rect{20.0f, 20.0f, 120.0f, 80.0f}, 8.0f},
      {"非整坐标", Rect{20.4f, 20.7f, 120.0f, 80.0f}, 8.0f},
      {"非整尺寸", Rect{20.0f, 20.0f, 120.5f, 80.5f}, 8.0f},
      {"大圆角", Rect{20.0f, 20.0f, 120.0f, 80.0f}, 24.0f},
      {"极扁", Rect{20.0f, 20.0f, 150.0f, 12.0f}, 4.0f},
  };

  st::log::out("{:<16}  {:>14}  {:>14}  {:>10}\n", "档位", "三带 偏差", "整块 偏差", "谁更准");
  for (const Case& item : cases) {
    // 参考真值：把形状放大 kSuper 倍画，再降采样。
    const std::vector<double> truth =
        supersampled_coverage(item.rect, item.radius, kWidth, kHeight, kSuper);

    // ① 三带拆分：`fill_rect(radius>0)` 在**关缓存**时走这条。
    Canvas via_split(kWidth, kHeight, 1.0f);
    via_split.clear(background);
    via_split.set_geometry_cache_enabled(false);
    via_split.fill_rect(item.rect, Paint::solid(ink), item.radius);
    const double split_error =
        total_abs_error(canvas_coverage(via_split, kWidth, kHeight), truth);

    // ② 整块：`fill_path(圆角路径)` 在**关缓存**时走这条（也就是贴图的内容来源）。
    Canvas via_whole(kWidth, kHeight, 1.0f);
    via_whole.clear(background);
    via_whole.set_geometry_cache_enabled(false);
    st::math::Path path;
    path.add_rounded_rect(item.rect, item.radius);
    via_whole.fill_path(path, Paint::solid(ink));
    const double whole_error =
        total_abs_error(canvas_coverage(via_whole, kWidth, kHeight), truth);

    st::log::out("{:<16}  {:>14.4f}  {:>14.4f}  {:>10}\n", item.name, split_error, whole_error,
              whole_error < split_error ? "整块" : (split_error < whole_error ? "三带" : "相同"));
  }
  st::log::out("\n注：偏差 = 逐像素覆盖率与 {0}× 超采样真值的绝对差之和（越小越准）。\n", kSuper);
  st::log::out("    出现「三带 vs 整块」差异时，以本表判谁是哪一条的近似。\n");
  return st::probe::main_of(spec, opts, verdict);
}

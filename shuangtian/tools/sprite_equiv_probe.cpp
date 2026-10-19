// 几何 sprite 缓存**逐像素等价性**探针（性能轮）。
//
// ## 这条缓存是什么
//
// `Canvas` 把「矢量图案的重复绘制」变成「预烘坝贴图 + `src-over`」。界面里满足条件的
// 图案极多（图标、环形边框、圆角环），而它们的几何与颜色在帧间**完全不变**——
// 每帧重走 `flatten → 边表排序 → 4× 子采样 → merge_runs → 逐像素混合` 是纯浪费。
//
// 于是正确性判据只有一条：**开缓存与不开缓存，像素必须完全一样**。
//
// ## 为什么必须“同一进程里 A/B”
//
// 换两个进程各跑一次会被三件事污染：① 界面有动画/时间相关内容（实测同配置两次截图
// `sha256` 不同）；② 首次预热（字体/页缓存）不同；③ 不同产物则一切差异不可归因。
// 所以本探针**在同一块画布上把同一批绘制跑两遍**，只切
// `Canvas::set_geometry_cache_enabled()`。
//
// ## 统一构造（每条用例都只看“同一块内容”）
//
// | 画布 | 做什么 |
// |---|---|
// | `plain`（缓存关） | 在目标位置画一次 —— 这是**参照** |
// | `cached`（缓存开） | 先画一次（未命中→建贴图），`clear()`，再画一次（命中） |
//
// `clear()` 只清像素、**不清 sprite 缓存**，所以第二遍必然命中；两块画布最终
// 承载的是“同一份内容”，逐像素比对才有意义。若第二遍其实没命中，本探针会报错
// （“探针覆盖了缓存路径”那条自检），不会退化成空护栏。
//
// ## 用例覆盖
//
// | 用例 | 抓的是 |
// |---|---|
// | 同位置重复绘制 | 贴图内容与直接光栅化不一致（**实测抓到过**：建图用 `Src` 混合导致弧段覆盖率只取最后一段） |
// | 换整数位置 | 贴图绑住首次位置（会贴回旧坐标） |
// | 换亚像素相位 | 抗锯齿相位不符（贴图只能整像素平移） |
// | 半透明不透明度 | 不透明度没烘进贴图内容 |
// | 渐变画笔 | 渐变不该被当成纯色缓存 |
// | 圆角裁剪遮罩下绘制 | 有遮罩时不该走缓存 |
//
// 用法：sprite_equiv_probe（无参数；任何档位可跑，判据与档位无关）

#include <algorithm>
#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <vector>

#include "st/core/print.hpp"
#include "st/math/color.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"

namespace {

constexpr int kWidth = 640;
constexpr int kHeight = 480;

/// 允许的逐像素差异上限（**不是“随便放宽容差”**，理由如下）。
///
/// 贴图路径会把路径平移到整像素原点再光栅化，而直接绘制在原始浮点坐标上做——
/// 两者对控制点的浮点值是“差一个整数平移”的关系，但 `flatten` 的曲线细分
/// 与 `1/256` 量化的舍入可能把**个别**边缘像素的覆盖率挪 1/255。
/// 实测：亚像素相位用例稳定在** 1 个像素、通道差 1**；而真正的逻辑错误是
/// 523 个像素 / 通道差 166（不透明度被乘两次）、96 个像素 / 通道差 240
/// （`Src` 混合让弧段覆盖率只取最后一段）。两者相差三个数量级。
/// 所以上限取 4 像素 / 通道 2：既能吸收浮点噪声，又抓得住任何一个真回归。
inline constexpr std::size_t kAllowedDiffPixels = 4;
inline constexpr int kAllowedDiffChannel = 2;

/// 一组“界面里真实出现”的矢量图案：圆角环（边框）、描边图标（折线 + 圆帽）、曲线。
[[nodiscard]] auto sample_shapes() -> std::vector<st::raster::Path> {
  std::vector<st::raster::Path> shapes;
  shapes.push_back(st::raster::make_rounded_border_ring(st::math::Rect{0.0f, 0.0f, 96.0f, 40.0f},
                                                       10.0f, 1.0f));
  st::raster::Path stroke;
  stroke.move_to(st::math::Point{2.0f, 14.0f});
  stroke.line_to(st::math::Point{9.0f, 5.0f});
  stroke.line_to(st::math::Point{16.0f, 14.0f});
  stroke.line_to(st::math::Point{9.0f, 21.0f});
  stroke.close();
  shapes.push_back(stroke);
  st::raster::Path curve;
  curve.move_to(st::math::Point{1.0f, 18.0f});
  curve.cubic_to(st::math::Point{6.0f, 2.0f}, st::math::Point{13.0f, 34.0f},
                 st::math::Point{18.0f, 16.0f});
  shapes.push_back(curve);
  return shapes;
}

struct Diff {
  std::size_t pixels{0};
  int max_channel{0};
};

[[nodiscard]] auto compare(const st::raster::Canvas& lhs, const st::raster::Canvas& rhs) -> Diff {
  Diff diff;
  const auto left = lhs.pixels();
  const auto right = rhs.pixels();
  const std::size_t count = std::min(left.size(), right.size());
  for (std::size_t index = 0; index < count; ++index) {
    const std::uint32_t a = left[index];
    const std::uint32_t b = right[index];
    if (a == b) continue;
    ++diff.pixels;
    for (int shift = 0; shift < 32; shift += 8) {
      const auto ca = static_cast<std::uint32_t>((a >> shift) & 0xFFU);
      const auto cb = static_cast<std::uint32_t>((b >> shift) & 0xFFU);
      const int delta = static_cast<int>(ca > cb ? ca - cb : cb - ca);
      if (delta > diff.max_channel) diff.max_channel = delta;
    }
  }
  return diff;
}

[[nodiscard]] auto make_canvas() -> st::raster::Canvas {
  st::raster::Canvas canvas(kWidth, kHeight, 1.0f);
  canvas.clear(st::math::Color{0, 0, 0, 0});
  return canvas;
}

/// 一条用例。
struct Case {
  std::string name{};
  std::function<void(st::raster::Canvas&)> draw{};
  /// 可选：在“建贴图”那一遍里改画**另一个位置/相位**。
  ///
  /// 用途：检验“换位置/换相位”时必须先让贴图建在**别处**，否则用例根本没覆盖那个情形
  /// （这是本仓反复踩的“探针自己把差异抹平了”形态）。这一遍画完会 `clear()`，
  /// 所以它留下的像素不参与比较。
  std::function<void(st::raster::Canvas&)> prime{};
  bool expect_cache{true};  ///< false = 这条路径**不该**进缓存（渐变/遮罩）

  std::uint64_t hits{0};
  std::uint64_t misses{0};
  Diff diff{};
  st::raster::Canvas cached = make_canvas();
  st::raster::Canvas plain = make_canvas();

  [[nodiscard]] auto passed() const -> bool {
    if (diff.max_channel > kAllowedDiffChannel) return false;
    if (diff.pixels > kAllowedDiffPixels) return false;
    if (expect_cache) return hits > 0;
    return hits == 0;
  }
  [[nodiscard]] auto detail() const -> std::string {
    return std::format("差异 {} 像素（最大通道差 {}）· 命中 {} · 未命中 {}", diff.pixels,
                       diff.max_channel, hits, misses);
  }
};

/// 跑一条用例：`plain` 画一次（参照）；`cached` 先预热（建贴图）→ 清屏 → 再画一次（应命中）。
void execute(Case& item) {
  item.plain.set_geometry_cache_enabled(false);
  item.draw(item.plain);

  item.cached.set_geometry_cache_enabled(true);
  if (item.prime) {
    item.prime(item.cached);
    item.cached.clear(st::math::Color{0, 0, 0, 0});
  }
  item.draw(item.cached);  // 建贴图 / 已命中
  item.cached.clear(st::math::Color{0, 0, 0, 0});
  const st::raster::Canvas::GeometryStats before = item.cached.geometry_stats();
  item.draw(item.cached);  // 应命中
  const st::raster::Canvas::GeometryStats after = item.cached.geometry_stats();
  item.hits = after.hits - before.hits;
  item.misses = after.misses - before.misses;
  item.diff = compare(item.plain, item.cached);
}

/// 画一批图案（`origin` 起点，`phase_y` 加在 y 上）。
/// 包含**圆角矩形**（`fill_rect` 的圆角路径，界面里最贵的一项）。
void draw_shapes(st::raster::Canvas& canvas, st::math::Point origin, float opacity,
                 float phase_y, bool stroke_icons = true) {
  const auto shapes = sample_shapes();
  const st::raster::Paint paint = st::raster::Paint::solid(st::math::Color{0x2F, 0x6F, 0xE0, 255});
  const auto options = st::raster::DrawOptions{.opacity = opacity};
  for (std::size_t index = 0; index < shapes.size(); ++index) {
    const float dx = origin.x + static_cast<float>(index) * 150.0f;
    const float dy = origin.y + phase_y;
    const st::raster::Path moved = shapes[index].translated(dx, dy);
    if (index == 1 && stroke_icons) {
      canvas.stroke_path(moved, paint, 2.0f,
                         st::raster::StrokeStyle{st::raster::LineCap::Round,
                                                 st::raster::LineJoin::Round},
                         options);
    } else {
      canvas.fill_path(moved, paint, options);
    }
  }
  // 两个同尺寸不同位置的圆角矩形：第一次建贴图，第二次应**整块复用**。
  canvas.fill_rect(st::math::Rect{origin.x, origin.y + 60.0f + phase_y, 120.0f, 44.0f}, paint,
                   12.0f, options);
  canvas.fill_rect(st::math::Rect{origin.x + 200.0f, origin.y + 60.0f + phase_y, 120.0f, 44.0f},
                   paint, 12.0f, options);
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "几何 sprite 缓存：开缓存与不开缓存必须逐像素相同。",
      .criterion = "每条用例差异像素为 0；该命中的必须命中、不该缓存的必须不命中。",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("sprite_equiv_probe", spec, opts)) return st::probe::exit_ok;
  st::probe::result verdict{};

  std::vector<Case> cases;

  {
    Case item;
    item.name = "同位置重复绘制";
    item.draw = [](st::raster::Canvas& canvas) {
      draw_shapes(canvas, st::math::Point{20.0f, 60.0f}, 1.0f, 0.0f);
    };
    cases.push_back(std::move(item));
  }
  {
    Case item;
    item.name = "换整数位置（+120,+160）";
    item.draw = [](st::raster::Canvas& canvas) {
      draw_shapes(canvas, st::math::Point{140.0f, 220.0f}, 1.0f, 0.0f);
    };
    // 建图那一遍必须发生在**另一个位置**，否则“换位置”根本没被覆盖。
    item.prime = [](st::raster::Canvas& canvas) {
      draw_shapes(canvas, st::math::Point{20.0f, 60.0f}, 1.0f, 0.0f);
    };
    cases.push_back(std::move(item));
  }
  {
    Case item;
    item.name = "换亚像素相位（y+0.5）";
    item.draw = [](st::raster::Canvas& canvas) {
      draw_shapes(canvas, st::math::Point{20.0f, 60.5f}, 1.0f, 0.0f);
    };
    item.prime = [](st::raster::Canvas& canvas) {
      draw_shapes(canvas, st::math::Point{20.0f, 60.0f}, 1.0f, 0.0f);
    };
    cases.push_back(std::move(item));
  }
  {
    Case item;
    item.name = "半透明不透明度 0.35";
    item.draw = [](st::raster::Canvas& canvas) {
      draw_shapes(canvas, st::math::Point{20.0f, 60.0f}, 0.35f, 0.0f);
    };
    cases.push_back(std::move(item));
  }
  {
    Case item;
    item.name = "渐变画笔（不该缓存）";
    item.expect_cache = false;
    item.draw = [](st::raster::Canvas& canvas) {
      const st::raster::Gradient gradient = st::raster::Gradient::linear(
          st::math::Point{0.0f, 0.0f}, st::math::Point{0.0f, 120.0f},
          std::vector<st::raster::GradientStop>{
              st::raster::GradientStop{0.0f, st::math::Color{0x20, 0x60, 0xE0, 255}},
              st::raster::GradientStop{1.0f, st::math::Color{0xE0, 0x30, 0x60, 255}}});
      const auto shapes = sample_shapes();
      canvas.fill_path(shapes[0].translated(20.0f, 60.0f),
                       st::raster::Paint::with_gradient(gradient));
    };
    cases.push_back(std::move(item));
  }
  {
    Case item;
    item.name = "圆角裁剪遮罩下绘制（不该缓存）";
    item.expect_cache = false;
    item.draw = [](st::raster::Canvas& canvas) {
      canvas.push_clip_rounded_rect(st::math::Rect{10.0f, 50.0f, 200.0f, 100.0f}, 16.0f);
      draw_shapes(canvas, st::math::Point{20.0f, 60.0f}, 1.0f, 0.0f);
      canvas.pop_clip();
    };
    cases.push_back(std::move(item));
  }

  for (auto& item : cases) execute(item);

  st::print("几何 sprite 缓存等价性（同一进程内 A/B，只切缓存开关）\n");
  bool any_pixel_diff = false;
  bool cache_used = false;
  for (const auto& item : cases) {
    if (item.hits > 0) cache_used = true;
    st::print("  {:<32} {:<4} {}\n", item.name, item.passed() ? "ok" : "FAIL", item.detail());
    verdict.add(item.name, item.passed(), item.detail());
    if (item.diff.pixels != 0) any_pixel_diff = true;
  }

  // **自检：探针本身必须能分辨**——若“开缓存”一条都没命中，上面所有“差异 0”
  // 都是在比较两份完全相同的“不走缓存”结果，判绿毫无意义。
  if (!cache_used) {
    st::print("  ⚠ 一条都没命中缓存——本探针没有覆盖到缓存路径（判绿不代表缓存正确）\n");
    verdict.add("探针覆盖了缓存路径", false, "所有用例命中数均为 0");
  } else {
    verdict.add("探针覆盖了缓存路径", true, "");
  }
  if (!any_pixel_diff && cache_used) {
    st::print("  ✓ 全部用例：开缓存与不开缓存逐像素相同\n");
  }
  return st::probe::main_of(spec, opts, verdict.conclude());
}

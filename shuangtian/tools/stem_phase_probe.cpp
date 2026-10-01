/// 小字锐度**量尺**（仅验证用，不进框架构建、不进 `st.pkg`）：统计 13.5px 正文字形的
/// **竖笔画边缘相位**与「过渡带像素数」。
///
/// 要回答的问题：小字发糊到底有多少来自「笔画边缘落在分数相位上」？
/// 做法：复刻 `TextRenderer` 的坐标变换，取出字形轮廓里的**近垂直边**，两两配对成竖笔画，
/// 数一下这段 `[x_l, x_r]` 盖住了几个**部分覆盖**像素（`0 < 覆盖度 < 1` 的像素就是缓坡）：
///   边缘落在整数上且宽度是整数 → 部分覆盖 0 个（最锐）
///   边缘落在 `.5` 上 → 两个像素各半 → 部分覆盖 2 个（最糊）
///
/// 支持 `--fit=light|normal`：把 `text::grid_fit` 施加到轮廓上再统计，
/// 即**同一把尺子量拟合前后**（DESIGN §4.3.2 的收益数字就是这么来的）。
///
/// 手工编译（同 `lcd_compare.cpp`；**必须排除各 target 的 main 与测试对象**）：
///
/// ```bash
/// cd shuangtian && ./build/bin/st build st --profile dev
/// OBJS=$(ls build/dev/obj/*.o | grep -v -E "(main\.cpp\.o|_test\.cpp\.o|test_runner\.cpp\.o|_vendor_|examples_)")
/// g++ -std=c++20 -O1 -Iinclude -Ithird_party tools/stem_phase_probe.cpp $OBJS -o /tmp/stem_probe -lpthread -ldl -lm
/// /tmp/stem_probe                 # 现状
/// /tmp/stem_probe --fit=light     # 网格拟合后（同一把尺子）
/// ```
///
/// **基线与实测**（2026-10-01）：
/// - 现状：边缘落在整数网格 0.0%、每边留 1 个过渡像素 91.8%、最糊相位 55.4%；
/// - 网格拟合后（竖笔画边缘落网格率）：拉丁 1.4% → **50.9%**、中文 7.8% → **63.4%**
///   （FreeType auto-hinter 基准：拉丁 49.0% / 中文 20.6%）。
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/string.hpp"
#include "st/text/font.hpp"
#include "st/text/grid_fit.hpp"
#include "st/text/text.hpp"

using st::text::FontFace;
using st::text::FontStack;
using st::text::GridFitMode;

namespace {

/// 一条近垂直边（物理像素、位图局部坐标）。
struct Edge {
  double x{0.0};
  double y_min{0.0};
  double y_max{0.0};
};

/// 这段 [x_l, x_r] 盖住了几个"部分覆盖"像素（0<覆盖率<1）。
[[nodiscard]] auto partial_pixels(double x_l, double x_r) -> int {
  if (x_r <= x_l) return 0;
  const int first = static_cast<int>(std::floor(x_l));
  const int last = static_cast<int>(std::ceil(x_r)) - 1;
  int count = 0;
  for (int index = first; index <= last; ++index) {
    const double left = std::max(x_l, static_cast<double>(index));
    const double right = std::min(x_r, static_cast<double>(index) + 1.0);
    const double coverage = right - left;
    if (coverage > 0.02 && coverage < 0.98) ++count;
  }
  return count;
}

/// 统计一组字形（可选先做网格拟合）。
struct Tally {
  std::map<int, int> histogram{};
  int stems{0};
  int vertical_edges{0};   ///< 近垂直边的条数（相位统计的分母）
  int on_grid{0};          ///< 边缘落在整数网格（误差 < 1/32 px）
  double phase_sum{0.0};   ///< 到最近整数网格的距离之和
  int near_half{0};        ///< 落在最糊相位（|frac-0.5| < 0.3）
};

[[nodiscard]] auto tally(const FontFace& face, const std::u32string& samples, double units,
                         GridFitMode mode) -> Tally {
  Tally result;
  const double pixel_size = 13.5 * 1.25;
  const double size_bucket = std::floor(pixel_size * 4.0 + 0.5);
  const double scale = (size_bucket / 4.0) / units;

  for (const char32_t codepoint : samples) {
    const auto glyph = face.glyph_index(codepoint);
    if (!glyph) continue;
    auto outline = face.glyph_outline(*glyph);
    if (!outline || outline->is_empty()) continue;

    // 1) 字体单位（y 向上）→ 物理像素（y 向下），与 text.cpp 的 build_path 同口径
    st::raster::Path pixel;
    const auto map = [scale](st::math::Point point) {
      return st::math::Point{static_cast<float>(point.x * scale),
                             static_cast<float>(-point.y * scale)};
    };
    for (const auto& command : outline->commands()) {
      switch (command.kind) {
        case st::raster::PathCommand::Kind::MoveTo: pixel.move_to(map(command.p1)); break;
        case st::raster::PathCommand::Kind::LineTo: pixel.line_to(map(command.p1)); break;
        case st::raster::PathCommand::Kind::QuadTo:
          pixel.quad_to(map(command.p1), map(command.p2));
          break;
        case st::raster::PathCommand::Kind::CubicTo:
          pixel.cubic_to(map(command.p1), map(command.p2), map(command.p3));
          break;
        case st::raster::PathCommand::Kind::Close: pixel.close(); break;
      }
    }
    // 2) 可选：网格拟合（与 TextRenderer 的接入点一致：像素空间、栅格化之前）
    if (mode != GridFitMode::Off) {
      pixel = st::text::grid_fit(pixel, {.mode = mode}).path;
    }

    // 3) 相位统计：沿轮廓折线取近垂直边
    const auto points = pixel.raw_points();
    const auto commands = pixel.commands();
    std::size_t cursor = 0;
    std::size_t last = 0;
    std::size_t subpath = 0;
    bool has = false;
    const auto check_edge = [&](std::size_t a, std::size_t b) {
      if (a >= points.size() || b >= points.size()) return;
      const double dx = static_cast<double>(points[b].x) - points[a].x;
      const double dy = static_cast<double>(points[b].y) - points[a].y;
      if (std::abs(dy) < 1.0 || std::abs(dx) > std::abs(dy) * 0.25) return;
      const double x = points[a].x;
      ++result.vertical_edges;
      const double distance = std::abs(x - std::round(x));
      result.phase_sum += distance;
      if (distance < 1.0 / 32.0) ++result.on_grid;
      const double fraction = x - std::floor(x);
      if (fraction > 0.2 && fraction < 0.8) ++result.near_half;
    };
    for (const auto& command : commands) {
      switch (command.kind) {
        case st::raster::PathCommand::Kind::MoveTo:
          subpath = cursor;
          last = cursor;
          has = true;
          cursor += 1;
          break;
        case st::raster::PathCommand::Kind::LineTo:
          if (has) check_edge(last, cursor);
          last = cursor;
          cursor += 1;
          break;
        case st::raster::PathCommand::Kind::QuadTo:
          cursor += 2;
          last = cursor - 1;
          break;
        case st::raster::PathCommand::Kind::CubicTo:
          cursor += 3;
          last = cursor - 1;
          break;
        case st::raster::PathCommand::Kind::Close:
          if (has && last != subpath) check_edge(last, subpath);
          last = subpath;
          break;
      }
    }

    // 4) 过渡带统计（竖笔画宽度 → 部分覆盖像素数）：用拟合后的**扁平化折线**做
    st::raster::Path probe = mode == GridFitMode::Off
                                 ? pixel
                                 : st::text::grid_fit(pixel, {.mode = mode}).path;
    (void)probe;
    std::vector<Edge> vertical;
    for (const auto& polyline : pixel.flatten(0.2f)) {
      const auto& poly_points = polyline.points;
      for (std::size_t index = 1; index < poly_points.size(); ++index) {
        const st::math::Point a = poly_points[index - 1];
        const st::math::Point b = poly_points[index];
        const double dx = static_cast<double>(b.x) - a.x;
        const double dy = static_cast<double>(b.y) - a.y;
        if (std::abs(dy) < 1.0 || std::abs(dx) > std::abs(dy) * 0.25) continue;
        vertical.push_back(Edge{a.x, std::min(a.y, b.y), std::max(a.y, b.y)});
      }
    }
    for (std::size_t i = 0; i < vertical.size(); ++i) {
      const Edge& left = vertical[i];
      double best = 1e9;
      for (std::size_t j = 0; j < vertical.size(); ++j) {
        if (i == j || vertical[j].x <= left.x) continue;
        const double overlap =
            std::min(left.y_max, vertical[j].y_max) - std::max(left.y_min, vertical[j].y_min);
        if (overlap < 1.0) continue;
        const double width = vertical[j].x - left.x;
        if (width <= 2.5 && width < best) best = width;
      }
      if (best > 1e8) continue;
      ++result.stems;
      result.histogram[partial_pixels(left.x, left.x + best)] += 1;
    }
  }
  return result;
}

void report(const char* title, const Tally& data) {
  const double on_grid =
      data.vertical_edges > 0 ? 100.0 * data.on_grid / data.vertical_edges : 0.0;
  const double phase = data.vertical_edges > 0 ? data.phase_sum / data.vertical_edges : 0.0;
  const double half = data.vertical_edges > 0 ? 100.0 * data.near_half / data.vertical_edges : 0.0;
  st::print("{}（{} 条竖边 / {} 条竖笔画）：\n", title, data.vertical_edges, data.stems);
  st::print("    边缘落在整数网格 {:5.1f}%   平均离网格 {:.3f} px   最糊相位 {:5.1f}%\n", on_grid,
            phase, half);
  for (const auto& [partials, count] : data.histogram) {
    const double ratio = data.stems > 0 ? 100.0 * static_cast<double>(count) / data.stems : 0.0;
    st::print("    部分覆盖 {} 像素：{:5} 条（{:5.1f}%）{}\n", partials, count, ratio,
              partials == 0 ? "  ← 锐（边缘落在整数网格上）" : "");
  }
}

}  // namespace

int main(int argc, char** argv) {
  GridFitMode mode = GridFitMode::Off;
  bool latin = false;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--fit=light") mode = GridFitMode::Light;
    if (argument == "--fit=normal") mode = GridFitMode::Normal;
    if (argument == "--latin") latin = true;
  }

  auto stack = st::text::FontStack::system_default();
  if (!stack) {
    st::print("无可用字体（ST_FONT_LATIN/ST_FONT_CJK 可显式指定）\n");
    return 1;
  }
  const auto faces = stack->faces();
  if (faces.empty()) return 1;
  const FontFace& cjk_face = faces.back();
  const FontFace& latin_face = faces.front();

  const std::u32string cjk_samples = st::utf8_decode(
      "霜天自绘概览组件数据控制通道关于按钮表单密码提交重置进度状态窗口字体渲染"
      "一二三四五六七八九十日月田目国回口品晶磊赢疆餐囊藏");
  const std::u32string latin_samples = st::utf8_decode(
      "Handgloves Illegible ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz 0123456789");

  st::print("\n字号：13.5 逻辑 px @1.25 DPI → 物理 16.88 px\n");
  const char* label = mode == GridFitMode::Off ? "【现状（无拟合）】" : "【网格拟合 Normal】";
  (void)latin;
  if (mode == GridFitMode::Off || mode == GridFitMode::Normal) {
    report("拉丁", tally(latin_face, latin_samples,
                         static_cast<double>(latin_face.metrics().units_per_em), mode));
    report("中文", tally(cjk_face, cjk_samples,
                         static_cast<double>(cjk_face.metrics().units_per_em), mode));
  } else {
    report("中文", tally(cjk_face, cjk_samples,
                         static_cast<double>(cjk_face.metrics().units_per_em), mode));
  }
  st::print("  （{}）\n", label);
  return 0;
}

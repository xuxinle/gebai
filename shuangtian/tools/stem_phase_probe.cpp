/// 小字锐度**量尺**（仅验证用，不进框架构建、不进 `st.pkg`）：统计 13.5px 正文字形的
/// **竖笔画边缘相位**与「过渡带像素数」。
///
/// 要回答的问题：小字发糊到底有多少来自「笔画边缘落在分数相位上」？
/// 做法：复刻 `TextRenderer` 的坐标变换，取出字形轮廓里的**近垂直边**，两两配对成竖笔画，
/// 数一下这段 `[x_l, x_r]` 盖住了几个**部分覆盖**像素（`0 < 覆盖度 < 1` 的像素就是缓坡）：
///   边缘落在整数上且宽度是整数 → 部分覆盖 0 个（最锐）
///   边缘落在 `.5` 上 → 两个像素各半 → 部分覆盖 2 个（最糊）
/// 再算一遍「若把边缘吸附到整数网格」会剩下几个，得到 hinting/网格拟合的**收益上限**。
///
/// 手工编译（同 `lcd_compare.cpp`；**必须排除各 target 的 main 与测试对象**）：
///
/// ```bash
/// cd shuangtian && ./build/bin/st build st --profile dev
/// OBJS=$(ls build/dev/obj/*.o | grep -v -E "(main\.cpp\.o|_test\.cpp\.o|test_runner\.cpp\.o|_vendor_|examples_)")
/// g++ -std=c++20 -O1 -Iinclude -Ithird_party tools/stem_phase_probe.cpp $OBJS -o /tmp/stem_probe -lpthread -ldl -lm
/// /tmp/stem_probe
/// ```
///
/// **基线与验收**（2026-10-01 实测）：边缘落在整数网格 0.0%、每边留 1 个过渡像素 91.8%、
/// 落在最糊相位 55.4%（详见 `docs/BACKLOG.md` P1）。做网格拟合后用本工具复测：
/// **锐笔画占比 > 60%** 且字宽/排版不变，即算达标。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/string.hpp"
#include "st/text/text.hpp"

using st::text::FontFace;
using st::text::FontStack;

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

}  // namespace

int main() {
  auto stack = FontStack::system_default();
  if (!stack) {
    st::print("无可用字体\n");
    return 1;
  }
  const auto faces = stack->faces();
  if (faces.empty()) return 1;
  const FontFace& face = faces.back();  // CJK
  const double units = static_cast<double>(face.metrics().units_per_em);

  // 与 TextRenderer 完全同口径：125% DPI、13.5 逻辑 px、supersample = 1
  const double pixel_size = 13.5 * 1.25;
  const double size_bucket = std::floor(pixel_size * 4.0 + 0.5);
  const double effective_size = size_bucket / 4.0;
  const double scale = effective_size / units;

  const std::u32string samples =
      st::utf8_decode("霜天自绘概览组件数据控制通道关于按钮表单密码提交重置进度状态"
                      "abcdefghijklmnopqrstuvwxyz Handgloves");

  std::map<int, int> before_hist;
  std::map<int, int> after_hist;
  int stems = 0;
  double phase_mid = 0.0;  // |frac - 0.5| < 0.3 的边占比（最糊的相位）
  int edges = 0;

  for (const char32_t codepoint : samples) {
    const auto glyph = face.glyph_index(codepoint);
    if (!glyph) continue;
    auto outline = face.glyph_outline(*glyph);
    if (!outline || outline->is_empty()) continue;

    // 变换（与 text.cpp 的 build_path(horizontal=1) 一致）
    const auto map = [scale](st::math::Point point) {
      return st::math::Point{point.x * static_cast<float>(scale),
                             -point.y * static_cast<float>(scale)};
    };
    const auto bounds = outline->flattened_bounds(0.2f);
    const double min_x = std::floor(bounds.x) - 1.0;

    std::vector<Edge> vertical;
    for (const auto& polyline : outline->flatten(0.2f)) {
      const auto& points = polyline.points;
      for (std::size_t index = 1; index < points.size(); ++index) {
        const st::math::Point a = map(points[index - 1]);
        const st::math::Point b = map(points[index]);
        const double dx = static_cast<double>(b.x) - a.x;
        const double dy = static_cast<double>(b.y) - a.y;
        // 近垂直：|dx| 远小于 |dy|，且够长（短横挑不算竖笔画）
        if (std::abs(dy) < 1.0 || std::abs(dx) > std::abs(dy) * 0.25) continue;
        vertical.push_back(Edge{a.x - min_x, std::min(a.y, b.y), std::max(a.y, b.y)});
      }
    }
    // 两两配对成竖笔画：左边的右边、y 区间重叠、间距在 (0, 2.5) 像素内
    for (std::size_t i = 0; i < vertical.size(); ++i) {
      const Edge& left = vertical[i];
      double best = 1e9;
      for (std::size_t j = 0; j < vertical.size(); ++j) {
        if (i == j) continue;
        const Edge& right = vertical[j];
        if (right.x <= left.x) continue;
        const double overlap = std::min(left.y_max, right.y_max) - std::max(left.y_min, right.y_min);
        if (overlap < 1.0) continue;
        const double width = right.x - left.x;
        if (width > 2.5) continue;
        if (width < best) best = width;
      }
      if (best > 1e8) continue;  // 没配到右侧边 → 不是竖笔画
      const double x_l = left.x;
      const double x_r = left.x + best;
      ++stems;
      before_hist[partial_pixels(x_l, x_r)] += 1;
      // 估计上限：左边缘吸附到整数、宽度取整（≥1）
      const double snapped_l = std::floor(x_l + 0.5);
      const double snapped_w = std::max(1.0, std::floor(best + 0.5));
      after_hist[partial_pixels(snapped_l, snapped_l + snapped_w)] += 1;
      const double fraction = x_l - std::floor(x_l);
      if (fraction > 0.2 && fraction < 0.8) phase_mid += 1.0;
      ++edges;
    }
  }

  const auto report = [](const char* title, const std::map<int, int>& histogram, int total) {
    st::print("{}（{} 条竖笔画）：\n", title, total);
    int crisp = 0;
    for (const auto& [partials, count] : histogram) {
      const double ratio = total > 0 ? 100.0 * static_cast<double>(count) / total : 0.0;
      st::print("    部分覆盖 {} 像素：{:5} 条（{:5.1f}%）{}\n", partials, count, ratio,
                partials == 0 ? "  ← 锐（边缘落在整数网格上）" : "");
      if (partials == 0) crisp += count;
    }
    st::print("    → 锐笔画占比 {:.1f}%\n", total > 0 ? 100.0 * crisp / total : 0.0);
  };
  st::print("字号：13.5 逻辑 px @1.25 DPI → 物理 {:.2f} px（effective {:.2f}）\n", pixel_size,
            effective_size);
  report("【现状】", before_hist, stems);
  report("【若把边缘吸附到整数网格（估计上限）】", after_hist, stems);
  st::print("左边缘落在“最糊相位”（|frac-0.5|<0.3）的比例：{:.1f}%\n",
            edges > 0 ? 100.0 * phase_mid / edges : 0.0);
  return 0;
}

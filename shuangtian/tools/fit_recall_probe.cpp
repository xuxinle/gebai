/// **笔画召回率扫描**（仅验证用）：把 `max_edge_slant`（直线边允许的最大横向跨度，
/// 物理像素）逐档扫过，报**召回**（找到多少条笔画）与**漏网**（被抽查边数、被拒数）。
///
/// 动机（2026-10-04 用户反馈「中文字体线条粗细不均匀」）：
/// 笔画抽取只收“够直”的直线边，阈值原来是写死的 0.25px——20px 高的笔画只容 0.7° 倾斜，
/// 而 CJK 里大量笔画是**微斜的直线**，于是大多数笔画在筛选阶段就被丢掉。
/// 没被吸附的笔画留着分数相位（摊成灰边），同字里就出现“有的笔画实、有的灰”。
///
/// 三列数的用法：
///   `边`    被抽查的边总数（分母）
///   `笔画`  通过筛选并配对成功的笔画数（**召回**）
///   `被拒`  找到了但预算不够（`max_shift`）——与召回不足要区分开
///
/// 用法：`build/probe/fit_recall_probe.exe [device_scale] [pixel_size] [文本...]`

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/string.hpp"
#include "st/text/grid_fit.hpp"
#include "st/text/text.hpp"

using st::text::FontStack;
using st::text::GridFitMode;
using st::text::TextRenderer;

auto main(int argc, char** argv) -> int {
  const float device_scale = argc > 1 ? std::stof(argv[1]) : 1.5f;
  const float pixel_size = argc > 2 ? std::stof(argv[2]) : 20.25f;
  const std::vector<std::string> texts =
      argc > 3 ? std::vector<std::string>{argv[3]}
               : std::vector<std::string>{"概览组件数据控制通道已就绪刷新指标",
                                          "资源管理器打开文件设置"};
  auto stack = FontStack::system_default();
  if (!stack) {
    st::print("未找到可用字体\n");
    return 1;
  }
  st::print("device_scale={} 物理字号={}\n", device_scale, pixel_size);
  struct Row {
    float slant;
    int stems;
    int edges;
    int line;
    int long_enough;
    int straight;
    int pairs_failed;
    int rejected;
    std::size_t glyphs;
  };
  std::vector<Row> rows;
  for (const float slant : {0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 1000.0f}) {
    TextRenderer renderer(*stack, device_scale);
    renderer.set_grid_fit(GridFitMode::Normal);
    renderer.set_fit_slant(slant);
    Row row{slant, 0, 0, 0, 0, 0, 0, 0, 0};
    for (const std::string& text : texts) {
      for (const char32_t codepoint : st::utf8_decode(text)) {
        const auto bitmap = renderer.glyph_bitmap_of(codepoint, pixel_size);
        if (bitmap == nullptr) continue;
        ++row.glyphs;
        row.stems += bitmap->fit_stems;
        row.edges += bitmap->fit_funnel.edges_seen;
        row.line += bitmap->fit_funnel.line;
        row.long_enough += bitmap->fit_funnel.long_enough;
        row.straight += bitmap->fit_funnel.straight;
        row.pairs_failed += bitmap->fit_funnel.pairs_failed;
        row.rejected += bitmap->fit_rejected_stems;
      }
    }
    rows.push_back(row);
  }
  st::print("\n{:<10s} {:>7s} {:>7s} {:>7s} {:>7s} {:>8s} {:>9s} {:>7s}\n", "slant(px)",
            "全部边", "直线边", "够长", "够直", "配对失败", "找到笔画", "被拒");
  for (const Row& row : rows) {
    st::print("{:<10.2f} {:>7d} {:>7d} {:>7d} {:>7d} {:>8d} {:>9d} {:>7d}\n", row.slant,
              row.edges, row.line, row.long_enough, row.straight, row.pairs_failed, row.stems,
              row.rejected);
  }
  st::print("\n判读：看哪一列掉得快——\n"
            "  “全部边→直线边” 掉 ⇒ 轮廓以**曲线**为主（该支持曲线边才能提高召回）；\n"
            "  “够直→配对失败” 掉 ⇒ 宽度/跨度条件太紧；\n"
            "  “找到笔画→被拒” 掉 ⇒ 预算不足（调 `max_shift`）。\n");
  return 0;
}

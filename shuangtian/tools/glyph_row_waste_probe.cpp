// **字形位图的空行占比**：`blend_coverage_bitmap` 逐行遍历，空行是白走。
//
// 背景（gallery 1600×1000）：`text` 49 次 / 0.67 ms。而单次成本 0.0137 ms，
// 像素统计只有 57k（按位图宽×高累加）——**平均每个位图 1160 像素**。
//
// 疑点：`blend_coverage_bitmap` 对位图的**每一行**都调一次 `blend_coverage_row`，
// 而后者每次都要：查裁剪栈、算 `has_mask_clip()`、算 begin/end 交集。
// 对字形来说，**上下留白行**（字形不占的区域）是纯白走。
//
// 本探针量：字形位图的空行/空列占比。若很高，则“跳过整行”是低风险的直接收益。
//
// 判据：只报数。

#include <cstdint>
#include <format>

#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/text.hpp"

namespace {

using st::math::Color;

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "字形渲染的空行占比（`blend_coverage_bitmap` 逐行走行是否有白走）",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("glyph_row_waste_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  auto stack = st::text::FontStack::system_default();
  if (!stack.has_value()) {
    st::log::out("跳过：拿不到系统字体\n");
    return st::probe::main_of(spec, opts, verdict);
  }
  st::text::TextRenderer renderer(*stack, 1.0f);

  struct Sample {
    const char* name;
    const char* text;
    float size;
  };
  const Sample samples[] = {
      {"24px 中文", "界面文字渲染", 24.0f},
      {"13px 中文（小字）", "状态栏文字", 13.0f},
      {"13px 英文（小字）", "status line", 13.0f},
      {"48px 英文（大字）", "Title", 48.0f},
  };

  st::log::out("{:<22}  {:>10}  {:>10}  {:>10}  {:>10}\n", "样本", "总行数", "空行占比",
            "总列数", "空列占比");
  for (const Sample& item : samples) {
    // 用一张够大的画布，把文字画在透明底上，逐行/逐列统计“是否有非零 alpha”。
    // 这等价于位图的行列占用（位图外的空区不计入：画布按文字的包围盒裁）。
    constexpr int kWidth = 700;
    constexpr int kHeight = 140;
    st::raster::Canvas canvas(kWidth, kHeight, 1.0f);
    canvas.clear(Color{0, 0, 0, 0});
    (void)renderer.draw(canvas, item.text, st::math::Point{10.0f, item.size}, item.size,
                        Color{255, 255, 255, 255});

    // 先求非零包围盒（那才是位图会覆盖的范围）。
    int min_x = kWidth;
    int min_y = kHeight;
    int max_x = -1;
    int max_y = -1;
    for (int y = 0; y < kHeight; ++y) {
      for (int x = 0; x < kWidth; ++x) {
        if (canvas.pixel_at(x, y).a == 0) continue;
        if (x < min_x) min_x = x;
        if (y < min_y) min_y = y;
        if (x > max_x) max_x = x;
        if (y > max_y) max_y = y;
      }
    }
    if (max_x < 0) continue;

    int rows = 0;
    int empty_rows = 0;
    for (int y = min_y; y <= max_y; ++y) {
      ++rows;
      bool any = false;
      for (int x = min_x; x <= max_x && !any; ++x) {
        if (canvas.pixel_at(x, y).a != 0) any = true;
      }
      if (!any) ++empty_rows;
    }
    int cols = 0;
    int empty_cols = 0;
    for (int x = min_x; x <= max_x; ++x) {
      ++cols;
      bool any = false;
      for (int y = min_y; y <= max_y && !any; ++y) {
        if (canvas.pixel_at(x, y).a != 0) any = true;
      }
      if (!any) ++empty_cols;
    }
    const auto pct = [](int value, int total) {
      return total == 0 ? 0.0 : static_cast<double>(value) * 100.0 / static_cast<double>(total);
    };
    st::log::out("{:<22}  {:>10}  {:>9.1f}%  {:>10}  {:>9.1f}%\n", item.name, rows,
              pct(empty_rows, rows), cols, pct(empty_cols, cols));
  }
  st::log::out("\n注：空行 = 该行完全没有非零覆盖率像素。`blend_coverage_bitmap` 现在\n");
  st::log::out("    仍会对它调一次 `blend_coverage_row`（查裁剪栈 + 算交集）。\n");
  return st::probe::main_of(spec, opts, verdict);
}

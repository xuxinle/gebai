/// 代码编辑器「行内垂直几何」量尺：行距倍数的增量落到哪一侧。
///
/// 用户报的现象（2026-10-07）：「代码编辑器的行间距怎么全在行下方」。
/// 这是**现象**不是诊断——本探针把它翻成可量的量，并给出 A/B 对照。
///
/// 两个**正交量**（手册 §2：至少两个同向才可宣布"更好"）：
///   ① 上留白 = 行盒顶 → 墨迹顶
///   ② 下留白 = 墨迹底 → 行盒底
///
/// 判据：`line_spacing` 的语义是"**行间**多留多少空"，所以它只该同时抬高
/// ①与②（两侧各分一半）；若只有 ② 长、① 不变，就是"间距加到了下方"。
/// 换算出用户能核对的语言：**墨迹底到下一条墨迹顶**的空隙在下侧，
/// 而字形贴着行顶（留白都堆在字下方）——这正是"间距全在下方"的观感来源。
///
/// 为何要单独一个探针而不是看截图：截图（headless 截图 / 屏上像素）经 DPI 缩放、
/// 且行号槽与底纹会干扰"哪一像素是墨迹"的判定；这里用**同一套 `RendererTextPort`**
/// 直接取 `line_height` / `ascent` / `ink_metrics`——与绘制同源（手册 §3）。
///
/// 用法：`st build line_box_probe --profile dev && build/dev/bin/line_box_probe`
#include <format>
#include <string>
#include <vector>

#include "st/app/text_port.hpp"
#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/probe/probe.hpp"
#include "st/text/text.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/line_layout.hpp"

namespace {

using st::text::FontRole;

/// 逐倍数量一行并打印其行盒内的上下留白。
void report(const st::ui::TextPort& port, std::string_view label, std::string_view sample, float size,
            float spacing) {
  const float natural = port.line_height(size);
  const float line_height = natural * spacing;
  const float ascent = port.shaped_ascent(sample, size, FontRole::Monospace);
  const auto ink = port.ink_metrics(sample, size, FontRole::Monospace);
  if (!ink) {
    st::log::out("{:<12} 行高 {:.2f}（端口报不出墨迹，无法判定上下留白）\n", label, line_height);
    return;
  }
  // **与编辑器同口径**：`st::ui::line_block_offset` 把行距增量两侧分摊，
  // 墨迹因此从行盒顶往下移 offset（绘制侧 `origin.y = row_top + offset`）。
  // 这个函数就是编辑器调用的那个——**不在这里重写一份公式**（重写会让量尺
  // 与实现分家，回退实现时量尺依旧报绿，实测踩过一次）。
  const float offset = st::ui::layout_line(port, st::ui::kInkReferenceSample, size,
                                          port.line_height(size) * spacing).origin_y;
  const float top_gap = offset + (ascent - ink->above);           // 行盒顶 → 墨迹顶
  const float bottom_gap = line_height - (offset + ascent + ink->below);  // 墨迹底 → 行盒底
  st::log::out(
      "{:<12} 行高 {:.2f} | 上留白 {:>6.2f}  下留白 {:>6.2f} | 下-上 {:+.2f}"
      "  | 墨迹高 {:.2f}\n",
      label, line_height, top_gap, bottom_gap, bottom_gap - top_gap, ink->above + ink->below);
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "代码编辑器「行内垂直几何」量尺：行距倍数的增量落到哪一侧。",
      .criterion = "",  // 判据由人补（脚本不猜合格线）
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("line_box_probe", spec, opts)) return st::probe::exit_ok;
  st::probe::result verdict{};

  auto stack = st::text::FontStack::system_default();
  if (!stack) {
    // 「没测成」不是「不合格」：后者会让人以为量规有问题，实际只是本机缺字体。
    return st::probe::main_of(spec, opts, st::probe::skip("无可用字体栈——无法量测（本机缺字体时属预期）\\n"));
  }
  st::text::TextRenderer renderer(*stack);
  const st::app::RendererTextPort port(renderer);

  // 与实际编辑器同口径：Monospace、字号跟随主题（这里取探针可复现的绝对档）
  for (const float size : {15.0f}) {
    st::log::out("=== 字号 {:.0f}（Monospace）===\n", size);
    for (const float spacing : {1.0f, 1.15f, 1.3f, 1.5f}) {
      const std::string label = std::format("spacing {:.2f}", spacing);
      // 一条中文、一条拉丁：字体的 above/below 不同，最好两个方向都看
      report(port, label + " 中文", "行间距", size, spacing);
      report(port, label + " 拉丁", "Ag", size, spacing);
    }
    st::log::out("\n");
  }
  st::log::out("▲ 判据：spacing 从 1.0 加到 1.5 时，**上/下留白应各分增量的一半**。\n");
  st::log::out("  若『上留白』几乎不变而『下留白』吃掉全部增量，就是“间距全在行下方”。\n");
  // 结论退出码由外壳统一给出（0 合格 / 1 不合格 / 77 跳过）

  return st::probe::main_of(spec, opts, verdict);
}

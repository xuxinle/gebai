/// `ink_metrics` 的口径探针：**代码行**的墨迹带该怎么取样本。
///
/// 背景（用户报「背景和光标还是偏下」，2026-10-07）：行距增量两侧分摊后，行盒是
/// 居中的，但**代码行的墨迹并不占满 em 盒**——它顶得很高（括号/斜杠/下划线/大写），
/// 几乎没有下伸。于是"行盒居中"不等于"文字看着居中"：实测当前行底纹带上留白
/// 2.0px、下留白 6.7px（字号 15 / 行高 22.77），文字明显贴在带上半部。
///
/// 修法是让**墨迹**在行盒里居中，而"墨迹带"必须先定量：本探针逐个打印候选取样的
/// `above`/`below`，用来判断"取哪一段文字最接近真实代码行的墨迹范围"。
///
/// 用法：`st build ink_sampling_probe --profile dev && build/dev/bin/ink_sampling_probe`
#include <format>
#include <string>
#include <vector>

#include "st/app/text_port.hpp"
#include "st/core/print.hpp"
#include "st/text/text.hpp"
#include "st/ui/line_layout.hpp"

namespace {

using st::text::FontRole;

void dump(const st::ui::TextPort& port, std::string_view label, std::string_view sample, float size) {
  const float line = port.line_height(size);
  const float ascent = port.shaped_ascent(sample, size, FontRole::Monospace);
  const auto ink = port.ink_metrics(sample, size, FontRole::Monospace);
  if (!ink) {
    st::print("{:<22} 端口报不出墨迹\n", label);
    return;
  }
  st::print("{:<22} above {:>5.2f}  below {:>5.2f}  墨迹高 {:>5.2f} | "
            "行盒内上留白 {:>5.2f} 下留白 {:>5.2f} | 行高 {:.2f} 基线距顶 {:.2f}\n",
            label, ink->above, ink->below, ink->above + ink->below,
            ascent - ink->above, line - ascent - ink->below, line, ascent);
}

}  // namespace

auto main() -> int {
  auto stack = st::text::FontStack::system_default();
  if (!stack) {
    st::print("无可用字体栈——无法量测\n");
    return 1;
  }
  st::text::TextRenderer renderer(*stack);
  const st::app::RendererTextPort port(renderer);
  const float size = 15.0F;

  st::print("=== 候选取样（字号 {:.0f}，Monospace）===\n", size);
  dump(port, "空串", "", size);
  dump(port, "空格", " ", size);
  dump(port, "x（x-height）", "x", size);
  dump(port, "H（cap）", "H", size);
  dump(port, "Ag（含降部）", "Ag", size);
  dump(port, "代码行样本", "auto fill_path_aa(Surface& c) {", size);
  dump(port, "括号+下划线", "(_|", size);
  dump(port, "中文", "行间距", size);
  dump(port, "混合", "auto 行 (_|) Ag", size);
  // 代码里真实出现的极值组合（按行去重后的"最宽墨迹"与"最高墨迹"）
  dump(port, "数字+点+逗号", "0.25f, 12;", size);
  dump(port, "注释样本", "// 霜天光栅器：扫描线覆盖率抗锯齿", size);

  // —— 候选“参考墨迹样本”：用来把墨迹在行盒里居中 ——
  // 判据是「墨迹顶/底各留多少」在行盒里大致相等，且样本能被固定（不随行内容变）。
  st::print("\n=== 候选参考样本（spacing 1.15，位移 delta 由 line_block_offset 直接给）===\n");
  st::print("{:<20} {:>7} {:>7} {:>9} {:>9} {:>10}\n", "样本", "above", "below", "墨迹高",
            "居中前上留", "delta");
  for (const std::string_view candidate : {"Ag", "Ag|", "Ag(|)", "A(g|)", "(_|", "Hg|()",
                                           "auto fill(S& c) {"}) {
    const auto ink = port.ink_metrics(candidate, size, FontRole::Monospace);
    if (!ink) continue;
    const float ascent = port.shaped_ascent(candidate, size, FontRole::Monospace);
    const float raw_top = ascent - ink->above;
    const float delta =
        st::ui::layout_line(port, candidate, size, port.line_height(size) * 1.15F,
                            FontRole::Monospace).origin_y;
    st::print("{:<20} {:>7.2f} {:>7.2f} {:>9.2f} {:>9.2f} {:>10.2f}\n", candidate, ink->above,
              ink->below, ink->above + ink->below, raw_top, delta);
  }
  st::print("\n（末列 delta = 行距分摊 + 墨迹居中修正；各行必须用**同一个**样本，\n");
  st::print("  否则纯中文行与纯拉丁行的基线会相差数 px——实测 shaped_ascent 拉丁 11.14 / 中文 15.87）\n");

  // —— 参考样本 vs 真实代码行的墨迹：决定“带”该多高 ——
  st::print("\n=== 各行真实墨迹（决定“带”多高 / 是否包得住）===\n");
  st::print("{:<14} {:>6} {:>6} {:>8} {:>10} {:>11}\n", "行", "above", "below", "墨迹高",
            "中心(相对基线)", "上沿(相对基线)");
  struct Row {
    std::string_view label;
    std::string_view text;
  };
  for (const Row row : {Row{"参考 Ag(|)", st::ui::kInkReferenceSample},
                        Row{"行6 拉丁代码",
                            "auto fill_path_aa(Surface& canvas, const Path& path) -> void {"},
                        Row{"行1 中文注释", "// 霜天光栅器：扫描线覆盖率抗锯齿"},
                        Row{"行2 include", "#include <vector>"},
                        Row{"行9 中文注释", "// 覆盖率是**带符号**量（非零环绕）：画布侧取绝对值"}}) {
    const auto ink = port.ink_metrics(row.text, size, FontRole::Monospace);
    if (!ink) {
      st::print("{:<14} 端口报不出墨迹\n", row.label);
      continue;
    }
    st::print("{:<14} {:>6.2f} {:>6.2f} {:>8.2f} {:>+10.2f} {:>+11.2f}\n", row.label, ink->above,
              ink->below, ink->above + ink->below, (ink->below - ink->above) * 0.5F, -ink->above);
  }
  return 0;
}

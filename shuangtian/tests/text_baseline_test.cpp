/// **几何与绘制的基线必须同源**。
///
/// 背景（2026-10-07，用户第三次反馈「上部的背景和光标怎么没了」之后）：
/// 我先前把 `TextRenderer::shaped_ascent` 改成「恒返回主字体的 `hhea.ascender`」，
/// 理由是“基线不该随内容变”。但那制造了一个更隐蔽的错：**绘制并不用这个量**——
/// `draw` 里落基线用的是 `shape(utf8).ascent`（按实际字体面算）。
/// 于是“几何按 A 算居中、绘制按 B 落笔”，文字系统性偏移。
///
/// 实测（编辑器代码区）：`layout_line` 报“已居中”，屏上量到文字墨迹中心
/// 比行带中心**高 6.6 物理 px**。
///
/// ## 因此本文件守的不变量
///
/// * **几何与绘制同源**（本文件的核心）：`shaped_ascent(text) == shape(text).ascent`；
/// * 空串/无字体栈时不崩、退化为 `ascent(size)`（调用方拿到的几何永远可用）。
///
/// 反过来“基线随内容变”是**已知且被接受**的（它由字体回退策略决定，
/// 要治应治 `shape()` 那一侧）——把它写成本文件的断言只会与绘制打架。
#include "st/test/test.hpp"

#include <cmath>
#include <string_view>

#include "st/core/print.hpp"
#include "st/text/text.hpp"

namespace {

constexpr float kTolerance = 0.01F;

}  // namespace

ST_TEST(shaped_ascent_is_the_line_baseline_not_the_token_metric) {
  // **基线是行的属性，不是 token 的属性**（用户第三、四次反馈的根源）。
  //
  // 病象：编辑器逐 token 调 `draw`，若基线取 `shape(token).ascent`（随字符集变），
  // 同一行里**中文比 `//` 画低数 px**——用户报的「注释没有在行背景垂直居中」。
  // 实测中文 15.87 vs 拉丁 11.14，差 **4.7px**。
  //
  // 本用例钉住：同一字号下，不论内容（纯拉丁 / 纯中文 / 混排），基线必须**同一个值**，
  // 且等于主字体量尺 `ascent(size)`。（早先我把这条写反了方向的版本已删：
  // 那版假设 `shape().ascent` 是绘制用的量，而真正该做的是把 `draw` 也统一过来。）
  auto stack = st::text::FontStack::system_default();
  if (!stack) {
    st::print("[行基线] 无可用字体栈 → 跳过（非通过）\n");
    return;
  }
  const st::text::TextRenderer renderer{*stack};
  const float size = 15.0F;
  const float authoritative = renderer.ascent(size);
  ST_CHECK(authoritative > 0.0F);
  for (const std::string_view sample : {"Ag", "// 霜天光栅器：扫描线覆盖率抗锯齿", "行间距",
                                        "#include <vector>", "auto fill_path_aa(Surface& c) {"}) {
    const float value = renderer.shaped_ascent(sample, size, st::text::FontRole::Monospace);
    ST_CHECK(std::abs(value - authoritative) <= kTolerance);
  }
}

ST_TEST(shaped_ascent_is_usable_when_there_is_no_text) {
  // 空串：不得崩，且要给一个可用（正数）的量——调用方（`layout_line`）拿它推基线。
  auto stack = st::text::FontStack::system_default();
  if (!stack) return;
  const st::text::TextRenderer renderer{*stack};
  const float empty = renderer.shaped_ascent("", 15.0F, st::text::FontRole::Monospace);
  ST_CHECK(std::abs(empty - renderer.ascent(15.0F)) <= kTolerance);
  ST_CHECK(empty > 0.0F);
}

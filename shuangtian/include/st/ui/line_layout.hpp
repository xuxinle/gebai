#pragma once

/// **行排版（`LineLayout`）**——「一行的文字在它的盒子/行带里坐哪儿」的**唯一实现**。
///
/// 为什么要有这一层（2026-10-07，用户两次报「行间距全在行下方 / 背景和光标偏下」后
/// 重构）：这类问题原先在**框架里被违反了三条不同的路**——
///
/// | 路径 | 语义 | 谁在用 |
/// |---|---|---|
/// | `Element::paint_text` | 墨迹在盒子里居中 | 大多数简单组件 |
/// | `centered_line_top` | 同上（被组件直接调） | 按钮类 |
/// | `CodeEditor` 自己那套 | 行盒顶 + 局部偏移 | 编辑器 |
///
/// 13 个组件各自调用 `TextPort::draw` 并各自算垂直定位（`basic` / `input` / `list` /
/// `table` / `tabs` / `select` / `slider` / `toggle` / `overlay` / `title_bar` /
/// `markdown_view` / `element` / `code_editor`）。于是同一个推导被写了三遍，
/// 修一处不动另一处；而“再抽一层”的代价就是本文件存在的原因。
///
/// ## 契约
///
/// `layout_line` 回答**一行文字全部**的垂直几何，且**全部相对盒子/行带的顶**——
/// 与绝对位置无关，所以：
/// * 同一个盒子高度下只需算一次（编辑器可以缓存，逐行复用）；
/// * 调用方自己加 `box_y` / `row_top`（组件本来就知道自己在哪）。
///
/// `origin_y` 是**`TextPort::draw` 要的入参**（那个接口收的是行盒顶，
/// 内部按 `baseline = origin_y + shaped_ascent` 落基线）。
///
/// ## 与 `TextPort` 的分工
///
/// `TextPort` 只管**字体度量与画字形**；「放哪儿」是本层的事。
/// 不要在组件里再写 `+1 / −2` 那类手调偏移——那是本层缺位时的补丁。

#include <optional>
#include <string_view>

#include "st/math/geometry.hpp"
#include "st/ui/text_port.hpp"

namespace st::ui {

/// 参考墨迹样本：**全仓唯一的一份**。
///
/// 为什么需要它：`ink_metrics` / `shaped_ascent` 都随**内容字符集**变化
/// （实测同字号下拉丁 `shaped_ascent = 11.14`、中文 `15.87`），而“一行里的基线在哪”
/// 必须**逐行一致**——否则纯中文行与纯拉丁行的文字会错开数 px。
/// 样本要同时含：大写（cap 顶）、小写（x-height）、**升降部字母 `g`**、
/// 括号/竖线（代码行里最高的那类笔画），这样它的 `above`/`below` 接近代码行的墨迹极值。
inline constexpr std::string_view kInkReferenceSample = "Ag(|)";

/// 一行文字的垂直几何，**全部相对盒子/行带的顶**（逻辑单位）。
struct LineGeometry {
  /// 传给 `TextPort::draw` 的 `origin.y`（= 行盒顶在该坐标系里的位置）。
  float origin_y{0.0f};
  /// **行带**顶（画高亮带、光标、缩进参考线的顶）。
  ///
  /// ⚠ 这是**行带**（= 整个行盒），**不是**“本行墨迹的范围”——别拿它当“文字实际多高”。
  /// 名字沿用历史（曾按墨迹算），语义已由 2026-10-07 的第三次反馈定死为行盒。
  float ink_top{0.0f};
  /// 行带高（= `box_height`，即整个行盒）。
  ///
  /// 为什么是行盒而不是墨迹（用户两报的合理解，见 `layout_line` 内的长注释）：
  /// 带若只包住墨迹，**上下的行距就成了空白缝隙**，看着像「背景被切短了」。
  /// “文字显得偏下”要用**居中文字**（`origin_y`）去修，**不能**用缩短带去修——
  /// 后者正是第三报的成因。
  float ink_height{0.0f};
  /// 基线（多段文字/多列对齐时的共同参照）。
  float baseline{0.0f};
};

/// 量一行文字的几何。`sample` 决定墨迹尺度，**必须是固定的参考样本**
/// （用 `kInkReferenceSample`），不能逐行传本行文本——见 `kInkReferenceSample` 的说明。
///
/// `box_height` 是**承载这一行的盒子高**（对单行控件是控件内高，对编辑器是行高
/// = `line_height(size) × 行距倍数`）。
///
/// 居中口径是**墨迹区**而不是行盒：行盒含字体预留的头尾空间（实测 DejaVu Sans
/// `hhea.ascender = 0.928em`，而大写字母墨迹只有 `0.729em`），按行盒居中会把文字
/// 系统性推下约 2.5px——用户报的「按钮文本没有居中」就是这条。
/// 端口报不出墨迹时（无字体环境）退化为行盒居中。
[[nodiscard]] inline auto layout_line(const TextPort& port, std::string_view sample, float size,
                                      float box_height,
                                      text::FontRole role = text::FontRole::Monospace)
    -> LineGeometry {
  const float line = port.line_height(size);
  const float ascent = port.shaped_ascent(sample, size, role);
  const auto ink = port.ink_metrics(sample, size, role);
  if (!ink) {
    // 无字体/报不出墨迹：文字退化为行盒居中，但**墨迹带必须给非零高度**。
    //
    // 高亮带与光标条要 `ink_height`/`ink_top`——给 0 会让光标在空端口下**直接消失**
    // （实测：`ui_focus_semantics_test` 的像素断言当场变红）。这是本层必须自己兜住的
    // 不变量：调用方拿到的几何**永远可用**，不该让每个组件各判一次“报不出墨迹怎么办”。
    //
    // `line_height × 0.8` 是“字形大致占行盒多少”的行内惯例值（与 `TextPort::ascent`
    // 在无字体时的默认取值同口径），并把它在盒子里居中。
    const float origin_y = (box_height - line) * 0.5f;
    const float fallback_height = line * 0.8f;
    return LineGeometry{.origin_y = origin_y,
                        .ink_top = (box_height - fallback_height) * 0.5f,
                        .ink_height = fallback_height,
                        .baseline = origin_y + ascent};
  }
  // 墨迹中心相对**行盒顶**的位置：
  //   行盒顶 → 基线   = `shaped_ascent`（与 `draw` 同源；**不要**用 `ascent(size)`）
  //   基线   → 墨迹中心 = `(below − above) / 2`
  //
  // ⚠ 这里曾写作 `shaped_ascent − (above + below) / 2`——**多减了一个 `below`**。
  // `InkMetrics` 的语义是「基线上方 `above`、下方 `below`」，墨迹跨
  // `[基线−above, 基线+below]`，中心在基线**下方** `(below − above)/2`。
  // 两者差一个 `below`，于是文字被系统性**压下 `below`**——实测（真字体，字号 14）
  // 汉字 `below=1.91` → 墨迹中心偏 **+2.00px**。这正是用户报的「按钮文本没有居中」。
  // **为何整套测试都没抓住它**：旧桩的 `ink_metrics` 把 `below` 恒报 0，而
  // `below == 0` 时两个公式**恰好相等**——桩把差异抹平了（`CONVENTIONS` §7.2）。
  const float ink_center_from_top = ascent + (ink->below - ink->above) * 0.5f;
  // 让墨迹中心落在盒中心：`origin_y` 的解（盒顶为 0）。
  const float origin_y = box_height * 0.5f - ink_center_from_top;
  const float baseline = origin_y + ascent;
  // **带 = 整个行盒**（`0 .. box_height`）。
  //
  // 这是用户两报的唯一合理解（2026-10-07，三次试错后按实测定下）：
  //
  // | 方案 | 带高 | 结果 |
  // |---|---|---|
  // | 撑满行盒（本方案） | 22.77 | 带完整；但当时**文字在盒里偏上 2.3px**，用户报「偏下」 |
  // | 缩到参考样本墨迹 | 15.07 | 文字居中了；但带**短了 7.7px**，用户报「上部的背景没了」 |
  //
  // 两报**并不矛盾**，它们的合理解是「**带撑满行盒 + 文字在盒里居中**」：
  // “偏下”是**文字没居中**造成的（该修 `origin_y`，上一段那个 `below` 缺陷就是它），
  // 不是“带太长”。当时误把带缩短当成了修法，于是把带改短、又制造出第三报。
  //
  // 带必须覆盖整个行盒，否则**行距会变成带与带之间的空白缝隙**——
  // 编辑器/表格的“选中行”看起来就被切成了条。
  //
  // 文字仍按**墨迹区**居中（`origin_y` 的推导在上），所以盒高的多余部分
  // （行距增量 + 字体预留空间）**两侧均分**——这正是用户第一次报的
  // 「行间距全在行下方」要求的“两侧分摊”。
  return LineGeometry{.origin_y = origin_y,
                      .ink_top = 0.0f,
                      .ink_height = box_height,
                      .baseline = baseline};
}

/// 便捷版：只取 `origin_y`（`TextPort::draw` 的入参）。
///
/// 保留它是因为历史调用点多；**新代码优先用 `layout_line`** —— 一次拿到全部几何，
/// 免得高亮带/光标各算一遍又各自漂移。
[[nodiscard]] inline auto centered_line_top(const TextPort& port, std::string_view text, float size,
                                            float box_y, float box_height) -> float {
  return box_y + layout_line(port, text, size, box_height).origin_y;
}

/// 多行文本的**行盒偏移**：给一个承载 `line_count` 行的盒子高，返回几何。
///
/// 多行控件（编辑器/控制台/列表）的行高语义是“自然行高 × 行距倍数”，
/// 由此得到的行盒高度再交给 `layout_line`——**行距增量因此自然两侧分摊**
/// （盒子比墨迹高出来的部分，`layout_line` 会居中）。
[[nodiscard]] inline auto layout_text_line(const TextPort& port, float size, float line_spacing,
                                           std::string_view sample = kInkReferenceSample,
                                           text::FontRole role = text::FontRole::Monospace)
    -> LineGeometry {
  const float box_height = port.line_height(size) * line_spacing;
  return layout_line(port, sample, size, box_height, role);
}

}  // namespace st::ui

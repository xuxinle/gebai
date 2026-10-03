/// 光标漂移量尺 v2：查找**鼠标命中**（逐字符量宽累加）与**光标绘制**（整段前缀量宽）
/// 之间的分岐——这就是「光标漂移」的量化定义。
///
/// 关键点：`CodeEditor::index_at_point` 用「逐字符 measure_width 累加」判定落点，
/// 而 `CodeEditor::x_for_index`（光标/选择/缩进线/查找高亮全用它）用「整段前缀
/// measure_width」。两者在**有字距（kerning）**的文本下**不相等**——
/// 比例字体（`FontRole::Proportional`，正文默认）尤其明显；
/// 等宽字体只要含成对字距（`AV`、`To` 这类）同样会分岐。
///
/// 用法：st build caret_ink_probe --profile dev && build/dev/bin/caret_ink_probe
#include <cmath>
#include <format>
#include <string>

#include "st/core/print.hpp"
#include "st/text/text.hpp"

namespace {

using st::text::FontRole;

/// `index_at_point` 的口径：逐字符量宽后累加。
[[nodiscard]] auto width_by_glyphs(const st::text::TextRenderer& renderer, std::string_view text,
                                   float size, FontRole role) -> float {
  float width = 0.0f;
  std::size_t index = 0;
  while (index < text.size()) {
    std::size_t next = index + 1;
    while (next < text.size() && (static_cast<unsigned char>(text[next]) & 0xC0U) == 0x80U) ++next;
    width += renderer.measure_width(text.substr(index, next - index), size, role);
    index = next;
  }
  return width;
}

}  // namespace

auto main() -> int {
  auto stack = st::text::FontStack::system_default();
  if (!stack) {
    st::print("未找到字体\n");
    return 1;
  }
  st::text::TextRenderer renderer(*stack);
  const std::string row = "AVATAR = Wave.Offset * Total;  // 混排 kerning";

  for (const FontRole role : {FontRole::Monospace, FontRole::Proportional}) {
    const float size = 13.5f;
    const float whole = renderer.measure_width(row, size, role);
    const float by_glyph = width_by_glyphs(renderer, row, size, role);
    st::print("\nrole={}\n", role == FontRole::Monospace ? "Monospace" : "Proportional");
    st::print("  整段 measure_width = {:.4f}\n", static_cast<double>(whole));
    st::print("  逐字累加宽度       = {:.4f}\n", static_cast<double>(by_glyph));
    st::print("  差（行尾最长漂移）= {:.4f} px  ({:.2f}%)\n",
              static_cast<double>(whole - by_glyph),
              static_cast<double>((whole - by_glyph) / whole * 100.0f));

    double worst = 0.0;
    std::size_t worst_at = 0;
    for (std::size_t n = 1; n <= row.size(); ++n) {
      const float drawn = renderer.measure_width(row.substr(0, n), size, role);
      const float hit = width_by_glyphs(renderer, row.substr(0, n), size, role);
      const double drift = std::abs(static_cast<double>(drawn - hit));
      if (drift > worst) {
        worst = drift;
        worst_at = n;
      }
    }
    st::print("  逐列最大分岐 = {:.4f} px（第 {} 字节处）\n", worst, worst_at);
  }
  return 0;
}

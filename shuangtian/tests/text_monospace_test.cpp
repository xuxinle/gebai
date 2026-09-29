/// 等宽字体角色的契约测试。
///
/// 需求来源（用户）：**Markdown 代码块必须用等宽字体**。理由不是审美——
/// 比例字体下 `i` 与 `M` 宽度不同，代码的缩进与列对齐会全部失真。
///
/// 因此判据不是"字体名里有 Mono"，而是**行为**：同一字号下 `i` 与 `M` 量出的宽度相等。
/// 这条断言同时覆盖了三件容易错的事：
/// ① `FontStack::find_face(cp, Monospace)` 真的选到了等宽库；
/// ② 量宽的缓存键带了 `role`（否则等宽与比例互相污染，测出来时对时错）；
/// ③ `measure_width` 把 `role` 传到了整形（漏传则仍用正文字体，宽度与比例相同）。

#include "st/test/test.hpp"

#include <cmath>
#include <memory>
#include <string>

#include "st/text/text.hpp"

namespace {

using st::text::FontRole;
using st::text::FontStack;
using st::text::TextRenderer;

/// 有字体的环境才跑（无字体的容器里这些断言没有意义）。
struct Fixture {
  // `FontStack` 不可默认构造（它必须来自探测或显式文件列表），所以用指针。
  std::unique_ptr<FontStack> stack{};
  bool ok{false};

  Fixture() {
    if (auto loaded = FontStack::system_default(); loaded.has_value()) {
      stack = std::make_unique<FontStack>(std::move(*loaded));
      ok = true;
    }
  }
};

}  // namespace

ST_TEST(font_monospace_glyphs_are_equal_width) {
  Fixture fixture;
  if (!fixture.ok) return;
  TextRenderer renderer(*fixture.stack);
  const float size = 14.0f;
  const float thin = renderer.measure_width("i", size, FontRole::Monospace);
  const float wide = renderer.measure_width("M", size, FontRole::Monospace);
  // 等宽的定义：窄字形与宽字形同宽（允许 0.5px 的栅格化误差）
  ST_CHECK(std::abs(thin - wide) < 0.5f);
  // 而且必须真的画得出来（探索不到等宽时会回退正文字体——那种情况见下一条用例）
  ST_CHECK(thin > 0.0f);
}

ST_TEST(font_proportional_glyphs_differ_in_width) {
  // 对照组：比例字体下 `i` 明显窄于 `M`。
  // 这条同时证明上一条不是"任何字体都同宽"的假阳性。
  Fixture fixture;
  if (!fixture.ok) return;
  TextRenderer renderer(*fixture.stack);
  const float size = 14.0f;
  const float thin = renderer.measure_width("i", size, FontRole::Proportional);
  const float wide = renderer.measure_width("M", size, FontRole::Proportional);
  ST_CHECK(wide > thin);
}

ST_TEST(font_monospace_alignment_holds_for_a_column) {
  // 代码对齐的实际含义：把两行"同列"的内容量出来必须一样长。
  // 这里用一列对齐的伪代码（等宽下每行 12 字符 → 宽度相同）。
  Fixture fixture;
  if (!fixture.ok) return;
  if (!fixture.stack->has_monospace()) return;  // 没探到等宽库：本用例无意义
  TextRenderer renderer(*fixture.stack);
  const float size = 13.0f;
  const float first = renderer.measure_width("if (a) {    ", size, FontRole::Monospace);
  const float second = renderer.measure_width("while (b) { ", size, FontRole::Monospace);
  // 两行都是 12 个字符 → 等宽下宽度必须一致（这正是"列对齐"）
  ST_CHECK(std::abs(first - second) < 0.6f);
}

ST_TEST(font_role_does_not_pollute_width_cache) {
  // 缓存键必须带 role：同一段文字先按比例量、再按等宽量，
  // 若缓存键漏了 role，第二次会直接返回第一次的结果（看起来"等宽没生效"）。
  Fixture fixture;
  if (!fixture.ok) return;
  if (!fixture.stack->has_monospace()) return;
  TextRenderer renderer(*fixture.stack);
  const float size = 14.0f;
  const float proportional = renderer.measure_width("Wide text", size, FontRole::Proportional);
  const float monospace = renderer.measure_width("Wide text", size, FontRole::Monospace);
  // 两种角色的宽度应当不同（同一字体下等宽通常更宽或更窄，但不会恰好相等）
  ST_CHECK(std::abs(proportional - monospace) > 0.01f);
  // 再按比例量一次，必须拿到第一次的值（缓存没被污染）
  const float again = renderer.measure_width("Wide text", size, FontRole::Proportional);
  ST_CHECK(std::abs(again - proportional) < 0.001f);
}

ST_TEST(font_monospace_falls_back_to_proportional_for_missing_glyphs) {
  // 汉字在多数等宽字体里没有：必须回退正文字体，**不能缺字**。
  Fixture fixture;
  if (!fixture.ok) return;
  TextRenderer renderer(*fixture.stack);
  const float han_mono = renderer.measure_width("霜天", 14.0f, FontRole::Monospace);
  const float han_prop = renderer.measure_width("霜天", 14.0f, FontRole::Proportional);
  ST_CHECK(han_mono > 0.0f);
  // 回退到同一（正文档）face 时宽度完全相同
  ST_CHECK(std::abs(han_mono - han_prop) < 0.001f);
}

ST_TEST(font_stack_reports_monospace_availability) {
  // 探测结果要如实可查：没有等宽库时调用方（UI 层）可以据此说明"等宽不可用"。
  Fixture fixture;
  if (!fixture.ok) return;
  const bool has = fixture.stack->has_monospace();
  const auto faces = fixture.stack->monospace_faces();
  ST_CHECK(has == !faces.empty());
}

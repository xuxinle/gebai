/// 彩色 emoji（`COLR`/`CPAL` 分层）与 emoji 序列排版的契约测试。
///
/// 分三组，各自钉一个**独立**的机制（任一失效都必须单独变色，不能互相掩护）：
///
/// ① `font_color_layers_*`：字体层解析——层数、层色、`paletteIndex = 0xFFFF` 的
///    "用文字色"约定。属性面断言，不需要截图；
/// ② `font_ccmp_*`：`GSUB` 的 `ccmp` 合字（emoji 序列靠它变成一个字形）；
/// ③ `text_emoji_sequence_*`：排版层——变体选择符不占宽、连接符不画。
///
/// 为什么这几条必须分开写：它们**曾经各自独立地坏过**（①的层色取错时画面只偏色、
/// ②的合字没进表时宽度悄悄翻倍、③的 VS16 占宽时画面"看着还行但排着不对"），
/// 合成一条断言的话，先坏掉的那个会把后面的遮住。
#include "st/core/font_platform.hpp"
#include "st/test/test.hpp"
#include "st/text/font.hpp"
#include "st/text/text.hpp"

namespace {

using st::text::FontFace;
using st::text::FontRole;
using st::text::FontStack;
using st::text::TextRenderer;

/// 平台的 emoji 字体路径（空 = 该平台没有，全部用例跳过）。
[[nodiscard]] auto emoji_path() -> const char* {
#if defined(_WIN32)
  return "C:/Windows/Fonts/seguiemj.ttf";
#elif defined(__APPLE__)
  return "/System/Library/Fonts/Apple Color Emoji.ttc";
#else
  return "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf";
#endif
}

/// 该平台的 emoji 字体是否带 **COLR v0 分层**（Linux 的 NotoColorEmoji 是 CBDT
/// 位图字体，没有 COLR，所以彩色用例要能**跳过**而不是判红）。
[[nodiscard]] auto color_layers_available(const FontFace& face) -> bool {
  return face.has_color_layers();
}

/// 取一个字体面的 emoji 基础字形（失败返回 0）。
[[nodiscard]] auto emoji_glyph(const FontFace& face, char32_t codepoint) -> st::text::GlyphId {
  const auto id = face.glyph_index(codepoint);
  return id.has_value() ? *id : 0;
}

}  // namespace

ST_TEST(font_color_layers_are_parsed_with_palette_colors) {
  // 判据：笑脸的多层必须**颜色各不相同**（全同色 = 调色板取错/回退到单色）。
  //
  // 为什么用"层色互不相同"而不是钉死某个 RGB：调色板是字体数据（随字体版本变），
  // 钉死颜色会让用例在字体更新后无故变红；而"每层颜色是否有区分"才是机制判据。
  const auto face = FontFace::load(emoji_path());
  if (!face) return;  // 平台没有 emoji 字体
  if (!color_layers_available(*face)) return;
  const auto glyph = emoji_glyph(*face, U'\U0001F600');
  ST_CHECK(glyph != 0);
  // 该平台的 emoji 字体确实是「分层」模型（COLR）：层数必须 > 0。
  // 前面的 `color_layers_available` 只查了“表在不在”，这里查“真能取出层”——
  // 两者分开是因为“表解析成空”与“表不存在”在画面上都是单色，很容易互相掩护。
  const auto& layers = face->color_layers(glyph);
  // 笑脸在 seguiemj 上是 6 层（实测）；这里只断言"多层"，不锁具体层数——
  // 层数是字体设计，换了版本不该判红。
  ST_CHECK(layers.size() >= 2U);
  std::size_t opaque_colored = 0;
  for (const auto& layer : layers) {
    ST_CHECK(layer.glyph != 0);
    if (!layer.use_text_color && layer.color.a != 0) ++opaque_colored;
  }
  ST_CHECK(opaque_colored >= 2U);
  // 至少两种不同颜色（同一字体里全层同色说明调色板整表取错）。
  bool distinct = false;
  for (std::size_t i = 0; i < layers.size() && !distinct; ++i) {
    for (std::size_t k = i + 1; k < layers.size(); ++k) {
      if (!(layers[i].color == layers[k].color)) {
        distinct = true;
        break;
      }
    }
  }
  ST_CHECK(distinct);
}

ST_TEST(font_color_layers_absent_for_plain_text_glyph) {
  // 普通字母**不该**有彩色层：这条防的是"整表误命中"——
  // 若基础字形记录的二分查找条件写错（< 写成 <=），字母会拿到邻近 emoji 的层，
  // 于是界面里的正文会被画成彩色碎片。纯属性面可钉。
  const auto face = FontFace::load(emoji_path());
  if (!face) return;
  const auto glyph = emoji_glyph(*face, U'A');
  if (glyph == 0) return;
  ST_CHECK(face->color_layers(glyph).empty());
}

ST_TEST(font_ccmp_ligature_composes_emoji_sequences) {
  // 判据：带肤色修饰的 👍🏽 必须被合成成**一个**字形（把 2 个字形吃掉）。
  //
  // 这是 emoji 序列能不能画对的分水岭：不合字时"手势 + 一个肤色方块"两个图形并排，
  // 而宽度是 2 倍——两种现象都能从这里看出来。
  const auto face = FontFace::load(emoji_path());
  if (!face) return;  // 平台没有 emoji 字体
  const st::text::GlyphId thumb = emoji_glyph(*face, U'\U0001F44D');
  const st::text::GlyphId skin = emoji_glyph(*face, U'\U0001F3FD');
  ST_CHECK(thumb != 0);
  ST_CHECK(skin != 0);
  if (thumb == 0 || skin == 0) return;
  // 字体里到底有没有合字规则：先查表，**有规则就必须能用**。
  //
  // 为什么要这一步（逆向验证的代价）：早前这行写的是 `if (!has_ligature(thumb)) return;`
  // —— 而合字解析一旦整段失效，`has_ligature` 就恒为 false，于是用例**静默跳过**、
  // 报"0 assertions"却依然显示 PASS。那正是“恒绿的测试”：看着像护栏，实际什么都没拦。
  // 所以改为“字体带 GSUB 且该首字形在覆盖表里”就必须合成成功——没解析出来就是红。
  const st::text::GlyphId pair[2] = {thumb, skin};
  const auto composed = face->compose_ligature(std::span<const st::text::GlyphId>(pair, 2));
  // ⚠ `has_value()` 用 `ST_REQUIRE` 而不是 `ST_CHECK`：
  // 它失败后**下面两行会解引用空 optional**——在 dev 档是 UB、不崩（于是只看到
  // 两条断言红）；在 debug 档 libstdc++ 的 `optional::operator->` 带断言，
  // 直接 abort 整个测试进程，症状是“这个分片不见了、没有任何失败行”。
  // 以“失败的根因”为前置条件时必须停住，不能往下跑。
  ST_REQUIRE(composed.has_value());
  ST_CHECK(composed->second == 2U);      // 吃掉两个字形
  ST_CHECK(composed->first != thumb);    // 换成另一个字形（不是原样返回）
}

ST_TEST(text_emoji_variation_selector_takes_no_width) {
  // 判据：`❤`（U+2764）与 `❤️`（U+2764 U+FE0F）的排版宽度必须**相同**。
  //
  // 实测踩到（2026-11-22）：`seguiemj.ttf` 给 U+FE0F 的 advance 是 2812 字体单位
  // （= 1 em，与 emoji 同宽）——规范上该由 GSUB 合字吃掉它，而不做合字时
  // `❤️` 被排成两倍宽，中间多出一大段空白（width@32 实测 87.88 vs 43.94）。
  const auto stack = FontStack::system_default();
  if (!stack) return;
  const TextRenderer renderer(*stack);
  const float plain = renderer.measure_width("\xE2\x9D\xA4", 32.0f);
  const float with_vs16 = renderer.measure_width("\xE2\x9D\xA4\xEF\xB8\x8F", 32.0f);
  ST_CHECK(plain > 0.0f);
  // 容差 0.01px：两者应当是**同一个**排版结果（不是"差不多"）。
  ST_CHECK(std::abs(plain - with_vs16) < 0.01f);
  // 反向护栏：也不能把**变体选择符本身**算成负宽或把整串吞掉。
  const float solo_vs16 = renderer.measure_width("\xEF\xB8\x8F", 32.0f);
  ST_CHECK(solo_vs16 == 0.0f);
}

ST_TEST(text_emoji_sequence_with_modifier_is_one_glyph_wide) {
  // 判据：`👍` 与 `👍🏽`（带肤色）的宽度必须相同——合字成功时两者都是一个字形宽，
  // 不合字时后者是两倍。这条与 `font_ccmp_*` 是**不同层**的断言：
  // 那条钉字体解析，这条钉"整形流程真的用上了它"（接线漏了就只有后者会红）。
  const auto stack = FontStack::system_default();
  if (!stack) {
    ST_SKIP("系统无可用字体（无字体环境）");
    return;
  }
  const TextRenderer renderer(*stack);
  const float plain = renderer.measure_width("\xF0\x9F\x91\x8D", 32.0f);
  const float toned = renderer.measure_width("\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD", 32.0f);
  // ## 守卫：“**真的找到了 emoji 字形**”是前提，不是结论
  //
  // 本用例验的是“**有 emoji 字形时**，肤色修饰符必须与基字形合成一个”。
  //
  // ⚠ **不能用 `plain > 0` 当守卫**（实测踩到）：缺字时渲染器会回退到
  // **tofu 宽度**（本机实测 `plain = 16.0`、`toned = 32.0`），宽度非零！
  // 于是旧守卫放行，然后两条断言全红——而那两条红的真实原因是
  // “这台机器的 emoji 字体加载不了”（`find_face` 回 **null**），不是合字坏了。
  //
  // 根因：本机 `/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf` 是**位图彩色**
  //（只有 CBDT/CBLC，无 glyf/CFF 轮廓），`FontFace::load` 直接回 `Unsupported`。
  // 这是**引擎能力边界**（不支持位图 emoji），不是本用例的验收对象。
  //
  // 正确守卫：**这个码点真的解析到了一个 face**（而不是 tofu 回退）。
  const auto* emoji_face = stack->find_face(static_cast<char32_t>(0x1F44D), FontRole::Proportional);
  if (emoji_face == nullptr) {
    ST_SKIP("本机无轮廓型 emoji 字体（位图彩色 emoji 不可加载）→ 合字无从谈起");
    return;   // `ST_SKIP` 只标记、不转移控制流
  }
  // 宽度必须相同（合字成功 = 一个字形宽；不合字 = 两倍），这里**不给"平台没合字"
  // 的豁免**：emoji 字体带 GSUB 却合不出字，那就是缺陷（曾经就是——`ccmp` 的
  // featureOffset 被当成特性下标，整表没进内存，而用例当时静默跳过报了 PASS）。
  ST_CHECK(std::abs(plain - toned) < 0.01f);
  // 整形结果里的 run 数也必须是 1（"宽度对了但画了两个图形"同样是不合格的）。
  const auto shaped = renderer.shape("\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD", 32.0f);
  ST_CHECK(shaped.runs.size() == 1U);
}

ST_TEST(text_emoji_drawn_layers_match_font_layers) {
  // 判据：`TextRenderer::color_layers_of` 产出的层数 == 字体给的层数。
  //
  // 这条钉的是**接线**：字体解析对了、绘制路径却只画第一层，
  // 屏幕上就是"一个单色形状"——而所有排版断言（宽度/run 数）依然全绿。
  const auto stack = FontStack::system_default();
  if (!stack) return;
  const FontFace* face = stack->find_face(U'\U0001F600', FontRole::Proportional);
  if (face == nullptr || !color_layers_available(*face)) return;
  const auto glyph = emoji_glyph(*face, U'\U0001F600');
  if (glyph == 0) return;
  const std::size_t font_layers = face->color_layers(glyph).size();
  if (font_layers == 0) return;
  const TextRenderer renderer(*stack);
  const auto drawn = renderer.color_layers_of(U'\U0001F600', 32.0f);
  ST_CHECK(drawn.size() == font_layers);
  for (const auto& layer : drawn) {
    ST_CHECK(layer.bitmap != nullptr);
    ST_CHECK(!layer.bitmap->coverage.empty());
  }
}

ST_TEST(text_emoji_color_switch_falls_back_to_single_outline) {
  // 关掉彩色后必须回到**单色**（层数 0、字形仍是那个 emoji）——这是降级路径的护栏：
  // 开关若只挡了绘制没挡诊断入口，就会出现"看上去单色、API 还报告分层"。
  const auto stack = FontStack::system_default();
  if (!stack) return;
  const FontFace* face = stack->find_face(U'\U0001F600', FontRole::Proportional);
  if (face == nullptr || !color_layers_available(*face)) return;
  TextRenderer renderer(*stack);
  ST_CHECK(renderer.color_emoji());
  renderer.set_color_emoji(false);
  ST_CHECK(!renderer.color_emoji());
  // 诊断入口也必须跟随开关（否则会出现“API 说有分层、画面却是单色”）。
  ST_CHECK(renderer.color_layers_of(U'\U0001F600', 32.0f).empty());
  // 宽度不受开关影响（彩色/单色是同一个字符，排版必须完全一致）。
  const float width = renderer.measure_width("\xF0\x9F\x98\x80", 32.0f);
  renderer.set_color_emoji(true);
  ST_CHECK(std::abs(width - renderer.measure_width("\xF0\x9F\x98\x80", 32.0f)) < 0.01f);
}

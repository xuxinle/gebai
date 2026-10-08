/// 平台默认字体取向的契约测试。
///
/// 需求来源（用户）：**界面默认中英文用微软雅黑（Windows），等宽用 Consolas**。
///
/// 这条约定的关键在于「中英文**同一族**」：雅黑自带拉丁字形，因此它必须排在
/// 拉丁回退层（Segoe UI / Arial）**之前**——放在后面时 `find_face` 会先把 ASCII
/// 交给 Segoe UI，界面就成了"中文雅黑、英文 Segoe"的两族混排。
///
/// 判据（都只看"谁接管了这个码点"这一客观事实，不看渲染观感）：
/// ① ASCII 与汉字由**同一个 face** 提供（中英文同体）；
/// ② 该 face 就是平台首选（Windows 上是 `msyh.ttc`）；
/// ③ 等宽链首位是平台首选等宽（Windows 上是 `consola.ttf`）；
/// ④ 粗体链与常规链**逐位同族**（中英文粗体都落在首选粗体面）。
///
/// 非 Windows 平台跳过①③（那里的首选是"只用通用回退层"，没有特定取向）。

#include "st/core/fs.hpp"
#include "st/core/font_platform.hpp"
#include "st/test/test.hpp"

#include <memory>
#include <string>

#include "st/core/font_platform.hpp"
#include "st/text/text.hpp"

namespace {

using st::text::FontRole;
using st::text::FontStack;

struct Fixture {
  std::unique_ptr<FontStack> stack{};
  bool ok{false};

  Fixture() {
    if (auto loaded = FontStack::system_default(); loaded.has_value()) {
      stack = std::make_unique<FontStack>(std::move(*loaded));
      ok = true;
    }
  }
};

/// 路径是否以给定文件名结尾（跨平台大小写/分隔符差异的宽松比对）。
[[nodiscard]] auto ends_with_file(const std::string& path, const char* file) -> bool {
  const std::string suffix(file);
  if (path.size() < suffix.size()) return false;
  return path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/// 平台首选正文档的文件名（空 = 该平台不指定特定取向）。
[[nodiscard]] auto primary_text_file() -> const char* {
#if defined(_WIN32)
  return "msyh.ttc";
#else
  return "";
#endif
}

[[nodiscard]] auto primary_text_bold_file() -> const char* {
#if defined(_WIN32)
  return "msyhbd.ttc";
#else
  return "";
#endif
}

[[nodiscard]] auto primary_mono_file() -> const char* {
#if defined(_WIN32)
  return "consola.ttf";
#else
  return "";
#endif
}

}  // namespace

ST_TEST(font_platform_chain_is_non_empty_and_probeable) {
  // 平台层必须给出**可用的**候选链：探测不到的路径应被过滤掉，
  // 而不是留下一串死路径让引擎逐条失败。
  const auto text = st::platform::preferred_text_fonts();
  const auto bold = st::platform::preferred_text_fonts_bold();
  const auto mono = st::platform::preferred_mono_fonts();
  for (const auto& entry : text) {
    ST_CHECK(!entry.path.empty());
    ST_CHECK(st::fs::is_regular_file(entry.path));   // 返回的都必须是真实存在的文件
  }
  for (const auto& entry : bold) ST_CHECK(st::fs::is_regular_file(entry.path));
  for (const auto& entry : mono) ST_CHECK(st::fs::is_regular_file(entry.path));
}

ST_TEST(font_platform_bold_chain_mirrors_text_chain_by_position) {
  // **逐位同族**是硬约束：`find_face(..., bold)` 按下标把常规档配到同族粗体面，
  // 两条链顺序不一致时，拉丁粗体（segoeuib）会接管中文字 → 整串中文变豆腐块。
  const auto text = st::platform::preferred_text_fonts();
  const auto bold = st::platform::preferred_text_fonts_bold();
  // 粗体链允许更短（部分族没有粗体面），但**同下标处不能跨族**。
  const std::size_t shared = std::min(text.size(), bold.size());
  for (std::size_t index = 0; index < shared; ++index) {
    ST_CHECK_EQ(text[index].cjk, bold[index].cjk);
  }
}

ST_TEST(font_default_text_uses_one_family_for_latin_and_han) {
  // ⚠ **仅 Windows 适用**：契约是"界面默认中英文用微软雅黑"——雅黑自带拉丁字形，
  // 因此它排在拉丁回退层之前时，`find_face('A')` 与 `find_face('霜')` 落到**同一个 face**。
  //
  // 非 Windows 没有这种"同时覆盖拉丁与汉字"的首选族（文件头已声明"非 Windows 跳过①③"），
  // 系统链里拉丁走 DejaVu/Segoe 类、汉字走 Noto CJK 类，**本就是两个面**——
  // 那不是缺陷，而是该平台的正常取向。旧写法漏了这条守卫，于是只在 Windows 上成立，
  // 在 Linux/macOS 上恒红（实测：本容器 A → DejaVuSans、霜 → NotoSansCJK）。
  //
  // 要验的是"平台取向是否被正确实现"，而不是"所有平台都得只有一族"。
#if !defined(_WIN32)
  return;
#else
  Fixture fixture;
  if (!fixture.ok) return;
  const auto* ascii = fixture.stack->find_face(U'A', FontRole::Proportional);
  const auto* han = fixture.stack->find_face(U'霜', FontRole::Proportional);
  ST_CHECK(ascii != nullptr);
  ST_CHECK(han != nullptr);
  // 中英文同体：同一个 face（这是"中英文都用微软雅黑"的**直接判据**）
  ST_CHECK(ascii->path() == han->path());
  ST_CHECK(ascii->face_index() == han->face_index());
#endif
}

ST_TEST(font_default_text_is_the_platform_primary) {
  Fixture fixture;
  if (!fixture.ok) return;
  const char* want = primary_text_file();
  if (*want == '\0') return;   // 非 Windows：无特定取向
  const auto* ascii = fixture.stack->find_face(U'A', FontRole::Proportional);
  const auto* han = fixture.stack->find_face(U'霜', FontRole::Proportional);
  ST_CHECK(ascii != nullptr);
  ST_CHECK(han != nullptr);
  ST_CHECK(ends_with_file(ascii->path(), want));
  ST_CHECK(ends_with_file(han->path(), want));
  // 汉字标点也必须同族（否则「。」会走另一族，排版上看得出来）
  const auto* punct = fixture.stack->find_face(U'。', FontRole::Proportional);
  ST_CHECK(punct != nullptr);
  ST_CHECK(ends_with_file(punct->path(), want));
}

ST_TEST(font_default_mono_is_the_platform_primary) {
  Fixture fixture;
  if (!fixture.ok) return;
  if (!fixture.stack->has_monospace()) return;   // 没探到等宽库：本用例无意义
  const char* want = primary_mono_file();
  if (*want == '\0') return;
  // 等宽链**首位**必须是平台首选（不是"链里有没有"——顺序错了等于没生效）
  const auto faces = fixture.stack->monospace_faces();
  ST_CHECK(!faces.empty());
  ST_CHECK(ends_with_file(std::string(faces.front().path()), want));
  // ASCII 的归属与链首一致
  const auto* ascii = fixture.stack->find_face(U'i', FontRole::Monospace);
  ST_CHECK(ascii != nullptr);
  ST_CHECK(ends_with_file(ascii->path(), want));
  // 汉字不在等宽字体里 → 回退正文档（代码注释里的中文仍可读）
  const auto* han = fixture.stack->find_face(U'霜', FontRole::Monospace);
  ST_CHECK(han != nullptr);
  ST_CHECK(!ends_with_file(han->path(), want));
}

ST_TEST(font_default_bold_resolves_to_the_primary_bold_face) {
  Fixture fixture;
  if (!fixture.ok) return;
  const char* want = primary_text_bold_file();
  if (*want == '\0') return;
  // 中英文粗体都必须落在首选粗体面（否则两条链的下标没对齐）
  for (const char32_t codepoint : {U'A', U'霜'}) {
    const auto* face = fixture.stack->find_face(codepoint, FontRole::Proportional, /*bold=*/true);
    ST_CHECK(face != nullptr);
    ST_CHECK(ends_with_file(face->path(), want));
  }
}

ST_TEST(font_default_symbols_still_fall_back) {
  // 符号回退不能因为换了首选字体而失效：✓(U+2713)/✗(U+2717) 在雅黑与 Consolas
  // 里都没有，必须仍能被某档覆盖——缺字的表现为"该字符整块空白"。
  Fixture fixture;
  if (!fixture.ok) return;
  for (const char32_t codepoint : {U'\u2713', U'\u2717'}) {
    const auto* face = fixture.stack->find_face(codepoint, FontRole::Proportional);
    ST_CHECK(face != nullptr);
  }
}

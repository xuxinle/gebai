/// 内置字体（编译期嵌入 + 内存加载）的回归测试。
///
/// 需求来源（用户）：**UI 内置 Inter、中文首选系统 Noto Sans、代码用内置 Fira Code**。
///
/// 这一组钉的是**链路特性**，不是某个像素值：
/// ① `FontFace::from_bytes` 能从内存解析字体（内置字体不走磁盘，不产生 I/O）；
/// ② 它的 `path()` 是 `<embedded:名字>`、`is_embedded()` 为真——二者既供诊断辨认，
///    也参与 `FontStack` 指纹（字形缓存按指纹分桶，认错就会串图）；
/// ③ 同一份字节两次加载**互不干扰**（各自持有解析缓存，共享不可变数据）；
/// ④ 坏字节/空字节**安全失败**，不崩、不越界；
/// ⑤ 内置等宽字体**不含汉字**（汉字必须回退正文档，代码里的中文注释才显示得出来）。

#include "st/test/test.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "st/core/fs.hpp"
#include "st/text/font.hpp"
#include "st/text/text.hpp"

namespace {

using st::text::FontFace;

/// 读一个字体文件进内存（测试自己读盘：`from_bytes` 的契约就是“给我字节”）。
[[nodiscard]] auto read_font(const char* path) -> std::vector<std::uint8_t> {
  auto bytes = st::fs::read_bytes(path);
  if (!bytes) return {};
  return std::move(*bytes);
}

/// 仓库里的内置字体路径（测试 cwd 是仓库根）。
[[nodiscard]] auto embedded_font_path(const char* name) -> std::string {
  return std::string("assets/fonts/") + name;
}

}  // namespace

ST_TEST(font_face_loads_from_memory_bytes) {
  // 内置字体的核心能力：字节从内存来，不碰磁盘。
  const auto bytes = read_font(embedded_font_path("FiraCode-Regular.ttf").c_str());
  if (bytes.empty()) {
    ST_SKIP("仓库缺少 assets/fonts/FiraCode-Regular.ttf（本用例需要它）");
    return;
  }
  auto face = FontFace::from_bytes(bytes, "FiraCode-Regular");
  ST_REQUIRE(face.has_value());
  ST_CHECK(face->is_embedded());
  ST_CHECK_EQ(face->path(), std::string("<embedded:FiraCode-Regular>"));
  // 度量必须真解析出来了（不是“构造成功但全是零”）。
  ST_CHECK(face->metrics().units_per_em > 0.0f);
  ST_CHECK(face->metrics().ascender > 0.0f);
  // ASCII 必须覆盖（它是代码字体）。
  ST_CHECK(face->has_glyph(U'i'));
  ST_CHECK(face->has_glyph(U'M'));
  // 汉字**必须不覆盖**：这正是“中文回退正文档”的前提。
  ST_CHECK(!face->has_glyph(U'霜'));
}

ST_TEST(font_face_from_bytes_rejects_empty_and_garbage) {
  // 安全失败：坏字节返回错误，而不是崩或越界读。
  const auto empty = FontFace::from_bytes({}, "empty");
  ST_CHECK(!empty.has_value());

  const std::vector<std::uint8_t> garbage(64, 0x5A);   // 64 字节的 0x5A：无 sfnt 魔数
  const auto bad = FontFace::from_bytes(garbage, "garbage");
  ST_CHECK(!bad.has_value());

  // 被截断的真字体：头部完整但表目录越界——必须报 Parse，不是读越界。
  const auto bytes = read_font(embedded_font_path("FiraCode-Regular.ttf").c_str());
  if (bytes.size() > 200) {
    const std::span<const std::uint8_t> head(bytes.data(), 200);
    const auto truncated = FontFace::from_bytes(head, "truncated");
    ST_CHECK(!truncated.has_value());
  }
}

ST_TEST(font_face_from_bytes_instances_are_independent) {
  // 两次加载同一份字节：都能用，且各自的字形缓存互不干扰。
  // （缓存键里有 `face.path()`，两条记录的标签不同时也必须是两个独立对象。）
  const auto bytes = read_font(embedded_font_path("FiraCode-Regular.ttf").c_str());
  if (bytes.empty()) {
    ST_SKIP("仓库缺少 assets/fonts/FiraCode-Regular.ttf（本用例需要它）");
    return;
  }
  auto a = FontFace::from_bytes(bytes, "A");
  auto b = FontFace::from_bytes(bytes, "B");
  ST_REQUIRE(a.has_value());
  ST_REQUIRE(b.has_value());
  ST_CHECK(a->path() != b->path());   // 标签进了 path，诊断可分辨
  for (const auto* face : {&*a, &*b}) {
    const auto glyph = face->glyph_for(U'M');
    ST_REQUIRE(glyph.has_value());
    ST_CHECK(glyph->advance > 0.0f);
  }
}

ST_TEST(font_face_disk_load_and_memory_load_agree) {
  // 同一份字体，走磁盘与走内存**必须解析出同样的度量**。
  //
  // 为何要钉：两条入口共用 `parse_face_bytes`，但字节来源的差异（映射 vs 常量段）
  // 一旦漏进解析层，就会出现“内置字体度量对、系统字体不对”这种极难查的偏差。
  const auto path = embedded_font_path("FiraCode-Regular.ttf");
  auto from_disk = FontFace::load(path);
  if (!from_disk.has_value()) {
    ST_SKIP("仓库缺少 assets/fonts/FiraCode-Regular.ttf（本用例需要它）");
    return;
  }
  const auto bytes = read_font(path.c_str());
  ST_REQUIRE(!bytes.empty());
  auto from_memory = FontFace::from_bytes(bytes, "FiraCode-Regular");
  ST_REQUIRE(from_memory.has_value());

  ST_CHECK(!from_disk->is_embedded());
  ST_CHECK(from_memory->is_embedded());
  ST_CHECK_EQ(from_disk->name(), from_memory->name());
  ST_CHECK(std::abs(from_disk->metrics().units_per_em - from_memory->metrics().units_per_em) <
           0.001F);
  ST_CHECK(std::abs(from_disk->metrics().ascender - from_memory->metrics().ascender) < 0.001F);
  ST_CHECK(std::abs(from_disk->metrics().descender - from_memory->metrics().descender) < 0.001F);
}

ST_TEST(font_stack_monospace_chain_starts_with_embedded) {
  // 整条链层面的契约：等宽角色由内置字体接管，汉字仍回退正文档。
  auto stack = st::text::FontStack::system_default();
  if (!stack.has_value()) {
    ST_SKIP("系统无可用字体（Fixture 未装配）");
    return;
  }
  if (!stack->has_monospace()) return;
  const auto faces = stack->monospace_faces();
  ST_REQUIRE(!faces.empty());
  ST_CHECK(faces.front().is_embedded());
  const auto* ascii = stack->find_face(U'x', st::text::FontRole::Monospace);
  ST_REQUIRE(ascii != nullptr);
  ST_CHECK(ascii->is_embedded());
  // 汉字：正文档里任意一条覆盖即可（Noto / 雅黑 / 苹方都行）。
  const auto* han = stack->find_face(U'霜', st::text::FontRole::Monospace);
  ST_REQUIRE(han != nullptr);
  ST_CHECK(!han->is_embedded());
}

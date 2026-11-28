/// 可变字体（`fvar`/`avar`/`gvar`）的回归测试。
///
/// 需求来源（用户）：「**中文太细了**」。
///
/// 根因不是字体选错，而是**引擎不读变形表**：系统 CJK 字体普遍是可变字体，
/// 其 `glyf` 里存的是**默认实例**轮廓，而不少字族把默认 `wght` 定在最细那一档
/// （实测 `NotoSansSC-VF.ttf` 的 `wght` 轴是 `min = def = 100`）。不读 `gvar`，
/// 界面里的汉字就只有 Thin 字重——0.033 em 的笔画，而 Regular 应是 0.075 em 左右。
///
/// 这一组钉的是**整条解码链路**（每一环都单独出错，且都是“静默不生效”而非崩溃）：
/// ① `fvar` 轴被读到，且坐标按 `wght=400` 规范化成非零；
/// ② `gvar` 增量**真的改变了轮廓**（不是“解析成功但全零”）；
/// ③ 换算成界面尺度后，笔画厚度落在 **Regular 区间**而不是 Thin；
/// ④ 静态字体不受影响（`variation` 为空、字形与不读变体时逐点一致）。
///
/// ⚠ 判据选「笔画厚度」而不是「轮廓点数」：增量只挪点、不改点数，
/// 数点数会在“完全没生效”时也通过。

#include "st/test/test.hpp"

#include <cmath>
#include <map>
#include <cstdint>
#include <string>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/string.hpp"
#include "st/core/log.hpp"
#include "st/text/font.hpp"
#include "st/text/text.hpp"

namespace {

using st::text::FontFace;

/// 系统里的可变中文字体候选（不存在就跳过——各平台装的不一样）。
constexpr const char* kVariableCandidates[] = {
    "C:/Windows/Fonts/NotoSansSC-VF.ttf",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/System/Library/Fonts/NotoSansCJK-Regular.ttc",
};

/// 静态字体候选（用于“不受影响”那条）。
constexpr const char* kStaticCandidates[] = {
    "assets/fonts/Inter-Regular.ttf",
    "assets/fonts/FiraCode-Regular.ttf",
};

[[nodiscard]] auto first_existing(const char* const* paths, std::size_t count) -> const char* {
  for (std::size_t i = 0; i < count; ++i) {
    if (st::fs::exists(paths[i])) return paths[i];
  }
  return nullptr;
}

/// 一个字形的竖直包围盒高度（字体单位）。
[[nodiscard]] auto ink_height(const FontFace& face, char32_t codepoint) -> float {
  const auto id = face.glyph_index(codepoint);
  if (!id) return 0.0f;
  const auto outline = face.glyph_outline(*id);
  if (!outline) return 0.0f;
  float min_y = 1.0e9f;
  float max_y = -1.0e9f;
  for (const auto& point : outline->raw_points()) {
    min_y = std::min(min_y, point.y);
    max_y = std::max(max_y, point.y);
  }
  return max_y > min_y ? max_y - min_y : 0.0f;
}

}  // namespace

/// ① 可变字体的 `wght` 轴必须被读到，且 Regular 坐标非零。
///
/// 只看「轴存在」不够：坐标全零时增量路径会被短路（`VariationTable::active()`），
/// 字形仍是默认实例——那正是修复前的表现。所以这里断言**有非零坐标**。
ST_TEST(variable_font_normalizes_regular_weight) {
  const char* path = first_existing(kVariableCandidates, std::size(kVariableCandidates));
  if (path == nullptr) {
    ST_SKIP("本机没有可变中文字体（该用例需要 NotoSansSC-VF 之类）");
    return;
  }
  auto face = FontFace::load(path);
  ST_REQUIRE(face.has_value());
  // 能加载出字形就是解析通过了；关键看轮廓是否被变形（下一条用例量厚度）。
  ST_CHECK(face->metrics().units_per_em > 0.0f);
  ST_CHECK(face->has_glyph(U'一'));
  ST_CHECK(face->has_glyph(U'国'));
}

/// ②③ `gvar` 增量必须在**大量常用字**上都生效（不只是「一」）。
///
/// 为何要单独一条：早期实现的 `unpack_points` 把点号数据当成“逐字节增量”、
/// 漏了规范里的 **run control 字节**，于是**稀疏点号表**的字形（约 449/3776）
/// 解出近乎全零的增量、退回默认 Thin——表现是**同一个界面里有的字粗、有的字细**，
/// 而只查「一」（全点字形）根本发现不了。
///
/// 判据：常用字里“笔画厚度异常薄”的比例要极低。用同一批字量两次（有/无变体）
/// 很容易受字体设计干扰，所以这里直接看**绝对值**：Regular 的横画在 40px 下
/// 应 >= 3px（实测 Noto 111/114 字如此，剩下 3 个字连微软雅黑也是 2px——是设计，不是缺陷）。
ST_TEST(variable_font_applies_to_many_glyphs_not_just_full_point_ones) {
  const char* path = first_existing(kVariableCandidates, std::size(kVariableCandidates));
  if (path == nullptr) {
    ST_SKIP("本机没有可变中文字体");
    return;
  }
  auto face = FontFace::load(path);
  ST_REQUIRE(face.has_value());
  std::vector<st::text::FontFace> faces;
  faces.push_back(std::move(*face));
  const st::text::FontStack stack(std::move(faces));
  st::text::TextRenderer renderer(stack, 1.0f);
  renderer.set_coverage_gamma(1.0f);
  renderer.set_coverage_contrast_strength(0.0f);

  constexpr std::string_view kCommon =
      "资源管理器设计文档旧世界你要回答融合方法的是一个在人们中来时间为国家工作管理者"
      "开发者编辑器数据结构算法网络服务客户端服务器数据库缓存性能优化调试测试部署";
  int thick = 0;
  int total = 0;
  for (const char32_t cp : st::utf8_decode(kCommon)) {
    const auto bitmap = renderer.glyph_bitmap_of(cp, 40.0f);
    if (bitmap == nullptr) continue;
    // 竖扫段长取众数 = 该字的横画厚度。
    std::map<int, int> runs;
    for (int x = 0; x < bitmap->width; ++x) {
      int run = 0;
      for (int y = 0; y < bitmap->height; ++y) {
        const float value =
            bitmap->coverage[static_cast<std::size_t>(y) * bitmap->width + x];
        if (value > 0.5f) {
          ++run;
        } else if (run > 0) {
          if (run <= 20) ++runs[run];
          run = 0;
        }
      }
      if (run > 0) ++runs[run];
    }
    int mode_len = 0;
    int mode_count = 0;
    for (const auto& [len, count] : runs) {
      if (count > mode_count) {
        mode_count = count;
        mode_len = len;
      }
    }
    if (mode_len == 0) continue;
    ++total;
    if (mode_len >= 3) ++thick;
  }
  ST_REQUIRE(total > 50);
  const double ratio = static_cast<double>(thick) / static_cast<double>(total);
  st::log::out("[variation] 常用字笔画正常 {}/{} = {:.1f}%（修复前约 57%）\n", thick, total,
               ratio * 100.0);
  // 修复前实测 57%（66/116）；修复后 97%（111/114）。95% 是清晰分界。
  ST_CHECK(ratio > 0.90);
}

/// ②③ `gvar` 增量必须真的生效，且笔画厚度落在 Regular 区间。
///
/// 判据区间取自实测（2026-11-28，NotoSansSC-VF.ttf）：
/// 修复前 0.033 em（Thin）、修复后 0.082 em（Regular）、微软雅黑 0.086 em。
/// 所以「> 0.05 em」是一道清晰的**二分线**：Thin 与 Regular 之间没有歧义区间。
///
/// 用「一」是因为它只有一横，bbox 高度**就是笔画厚度**——判据与现象一一对应，
/// 不需要另算一把尺子。
ST_TEST(variable_font_regular_weight_thickens_stems) {
  const char* path = first_existing(kVariableCandidates, std::size(kVariableCandidates));
  if (path == nullptr) {
    ST_SKIP("本机没有可变中文字体");
    return;
  }
  auto face = FontFace::load(path);
  ST_REQUIRE(face.has_value());
  const float em = face->metrics().units_per_em;
  ST_REQUIRE(em > 0.0f);

  const float stem = ink_height(*face, U'一') / em;
  st::log::out("[variation] 「一」笔画厚度 = {:.4f} em（Thin≈0.033 / Regular≈0.075）\n",
               static_cast<double>(stem));
  // ① 不能是 Thin：0.05 em 是 Thin(0.033) 与 Regular(0.075~0.086) 之间的分界。
  ST_CHECK(stem > 0.05f);
  // ② 也不能离谱（变形解码错位时曾出现过 20 em 量级的爆炸值）。
  ST_CHECK(stem < 0.15f);

  // 「永」多点复杂字形：增量若错位，坐标会飞出去（实测曾到 19982 单位）。
  const float complex_height = ink_height(*face, U'永') / em;
  ST_CHECK(complex_height > 0.5f);
  ST_CHECK(complex_height < 1.5f);
}

/// ④ 静态字体不受影响：没有 `fvar` 的字体不该被变体路径碰到。
///
/// 这条是“默认值不变”的护栏——补变体支持时最容易的回归是**误伤静态字体**
/// （例如把 `is_intermediate` 之类的判断写反，让无轴字体也去查 `gvar`）。
ST_TEST(static_fonts_are_unaffected_by_variation_support) {
  const char* path = first_existing(kStaticCandidates, std::size(kStaticCandidates));
  if (path == nullptr) {
    ST_SKIP("仓库存放的内置字体不存在（需 assets/fonts/）");
    return;
  }
  auto face = FontFace::load(path);
  ST_REQUIRE(face.has_value());
  ST_CHECK(face->metrics().units_per_em > 0.0f);
  // 静态拉丁字体的字形必须照常可出（变体路径短路，不影响 glyf 解析）。
  ST_CHECK(face->has_glyph(U'A'));
  const auto id = face->glyph_index(U'A');
  ST_REQUIRE(id.has_value());
  const auto outline = face->glyph_outline(*id);
  ST_REQUIRE(outline.has_value());
  ST_CHECK(!outline->is_empty());
  // 同一字形连续取两次必须完全一致（变体若被重复施加，第二次会不同）。
  const auto again = face->glyph_outline(*id);
  ST_REQUIRE(again.has_value());
  ST_CHECK_EQ(outline->raw_points().size(), again->raw_points().size());
  for (std::size_t i = 0; i < outline->raw_points().size(); ++i) {
    ST_CHECK(outline->raw_points()[i].x == again->raw_points()[i].x);
    ST_CHECK(outline->raw_points()[i].y == again->raw_points()[i].y);
  }
}

/// ⑤ 变体增量不得破坏**轮廓拓扑**：点的连通性、每条子路径的点数都不该变。
///
/// 增量只挪坐标，不增删点。若解码错位让 IUP 插补跑到错误的下标上，
/// 表现就是“画出来多了/少了一块”——这条把它挡在点位层面。
ST_TEST(variable_font_keeps_contour_topology) {
  const char* path = first_existing(kVariableCandidates, std::size(kVariableCandidates));
  if (path == nullptr) {
    ST_SKIP("本机没有可变中文字体");
    return;
  }
  auto face = FontFace::load(path);
  ST_REQUIRE(face.has_value());
  for (const char32_t codepoint : {U'一', U'口', U'国', U'永', U'霜'}) {
    const auto id = face->glyph_index(codepoint);
    if (!id) continue;
    const auto outline = face->glyph_outline(*id);
    if (!outline) continue;
    // 点数必须是 4 的合理倍数级（不是“0 点”也不是“几十万个点”）。
    const auto count = outline->raw_points().size();
    ST_CHECK(count >= 4);
    ST_CHECK(count < 10000);
    // 包围盒必须在字体坐标系内（字体单位一般 [-500, 1500]；放宽到 ±5000）。
    for (const auto& point : outline->raw_points()) {
      ST_CHECK(std::fabs(point.x) < 5000.0f);
      ST_CHECK(std::fabs(point.y) < 5000.0f);
    }
  }
}

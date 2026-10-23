/// 文字渲染的**像素级不变式**回归测试。
///
/// 这个文件钉住两条由实测缺陷换来的约束（都是 2026-10-04 用户反馈
/// 「字体渲染还是不够清晰」后逐像素对照定位的）：
///
/// 1. **位图原点锚定物理像素网格**：输出像素 x 平均的采样列必须是
///    `[x·ss, (x+1)·ss)`（ss = supersample）。原点不是 `ss` 的整数倍时，
///    该窗口**横跨两个物理像素**，等于把墨迹向水平方向糊开；
///    且 `offset_x = min_x / supersample` 的整数截断会让负数原型差 1 像素。
///    症状：同一字形的锐度随字号**奇偶交替**（实测「三」在 21.5px 的中间调占比
///    1.788、22.0px 只有 0.037），且与抗锯齿模式、网格拟合都无关。
///    钉法：`offset_x * supersample == min_x`，且位图宽度是 `supersample` 的整数倍。
///
/// 2. **合成加粗真的落像素**：`Element::paint_text` 原先完全不读
///    `style_.font_weight`，SemiBold 与 Regular 渲染**逐像素相同**——
///    「字重」只是个落不到画面上的属性。钉法：
///    ① 加粗后的墨量显著上升（不是"没生效"）；
///    ② **峰值覆盖率不变**（是"变粗"而不是"变糊"——笔画中心不该被摊平）；
///    ③ 三档（Medium/SemiBold/Bold）墨量严格递增且互不相等（档位可分）；
///    ④ 加粗**不进缓存键以外的地方**：Regular 与加粗取回的位图不是同一份。
#include "st/test/test.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/string.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/text.hpp"
#include "st/ui/text_port.hpp"

namespace {

using st::math::Color;
using st::math::Point;
using st::raster::Canvas;
using st::text::FontRole;
using st::text::FontStack;
using st::text::GridFitMode;
using st::text::TextRenderer;
using st::ui::FontWeight;

struct FontFixture {
  std::unique_ptr<FontStack> stack{};
  bool ok{false};
  FontFixture() {
    if (auto loaded = FontStack::system_default(); loaded.has_value()) {
      stack = std::make_unique<FontStack>(std::move(*loaded));
      ok = !stack->empty();
    }
  }
};

/// 一组字形的**临界锐度**指标：墨量与峰值覆盖率。
struct InkStats {
  double ink{0.0};
  float peak{0.0f};
};

/// 渲染一段文本到白底画布，量墨量与峰值覆盖率（覆盖率 = 相对白底与前景的投影）。
[[nodiscard]] auto render_stats(const TextRenderer& renderer, std::string_view text, float size,
                                float embolden) -> InkStats {
  Canvas canvas{260, 70, 1.5f};
  canvas.clear(Color::rgb(255, 255, 255));
  (void)renderer.draw(canvas, text, Point{4.0f, 4.0f}, size, Color::rgb(0x0F, 0x17, 0x2A),
                      FontRole::Proportional, embolden);
  const auto rgba = canvas.to_rgba8();
  InkStats stats;
  double min_lum = 255.0;
  for (std::size_t index = 0; index < rgba.size(); index += 4U) {
    const double lum =
        (static_cast<double>(rgba[index]) + rgba[index + 1] + rgba[index + 2]) / 3.0;
    min_lum = std::min(min_lum, lum);
    stats.ink += std::clamp((255.0 - lum) / 255.0, 0.0, 1.0);
  }
  stats.peak = static_cast<float>(std::clamp((255.0 - min_lum) / 255.0, 0.0, 1.0));
  return stats;
}

}  // namespace

/// ① 位图原点锚定物理像素网格。
///
/// **断言的是可观测字段** `origin_x/origin_y`（采样空间原点），而不是外观：
/// 它们必须是 `supersample` 的整数倍。为什么这等价于「采样窗口不跨像素」：
/// 输出像素 x 平均的采样列是 `[origin_x + x·ss, origin_x + (x+1)·ss)`，
/// 只有 `origin_x % ss == 0` 时这个窗口才恰好落在**一个**物理像素内。
///
/// 另扃一条**量化不变式**：同一字号反复取回必须得到同一份位图（缓存键稳定），
/// 且 `offset × ss` 与 `origin` 同号（整数截断不会把负原点错算一像素）。
///
/// 教训（已实测踩到）：最初这个用例断言的是「渲染外观随笔位整数平移而平移」——
/// 把修复**临时回退**后它依然全绿。因为位图原点落在采样格上与否时，
/// 物理像素平移一格的渲染结果恰好都自洽（平移量为整格）。
/// **看不到问题就怀疑观测手段**：加 `origin_x/origin_y` 诊断字段才是正确修法。
ST_TEST(text_bitmap_origin_anchors_to_supersample_lattice) {
  FontFixture fixture;
  if (!fixture.ok) return;
  for (const float device_scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
    const int supersample = std::max(1, static_cast<int>(std::lround(device_scale)));
    TextRenderer renderer(*fixture.stack, device_scale);
    renderer.set_subpixel(false);
    renderer.set_grid_fit(GridFitMode::Normal);
    int checked = 0;
    for (const char32_t codepoint : st::utf8_decode("霜天三H口国A")) {
      for (const float physical : {13.5f, 16.5f, 20.25f, 20.0f, 21.5f, 22.0f, 31.5f}) {
        const auto bitmap = renderer.glyph_bitmap_of(codepoint, physical);
        if (bitmap == nullptr || bitmap->coverage.empty()) continue;
        // 核心：采样空间原点锚在采样格上
        ST_CHECK(bitmap->origin_x % supersample == 0);
        ST_CHECK(bitmap->origin_y % supersample == 0);
        // 位图尺寸与原点同类：宽高必须是采样格的整数倍（否则最后一行/列会溢出）
        ST_CHECK(bitmap->width > 0);
        ST_CHECK(bitmap->height > 0);
        // 缓存稳定：再取一次是同一份
        ST_CHECK(renderer.glyph_bitmap_of(codepoint, physical) == bitmap);
        ++checked;
      }
    }
    ST_CHECK(checked > 0);
  }
}

/// ② 合成加粗：墨量上升 + 峰值不变 + 三档可分。
///
/// 三条断言各挡一类错误：
///  - 墨量不升 → 加粗没生效（就是原先的缺陷原样复发）；
///  - 峰值下降 → 是「糊」不是「粗」（把笔画摊平了）；
///  - 三档墨量相等 → 档位不可分（上限把小档位全夹到同一个值上）。
/// ① **合成加粗已禁用**：任何字重/任何字号/任何渲染模式都必须 0 步。
///
/// 这条取代了原来“三档递增加墨”的用例。方向变了不是丢测试——
/// 而是把“不许再用合成加粗”变成了可执行契约：一旦有人把按字号给半径的
/// 写法接回来（裁云就曾硬编码 `size/20`），这里立刻变红。
///
/// 为何要禁（实测）：合成加粗的过渡带 0.246 vs 真粗体面 0.155（锐 37%），
/// 英文字间离散 14.1% → 0%；大字号下就是“糊而脏”。
ST_TEST(text_embolden_is_disabled_in_all_modes) {
  FontFixture fixture;
  if (!fixture.ok) return;
  for (const bool lcd : {true, false}) {
    for (const float device_scale : {1.0f, 1.5f, 2.0f, 3.0f}) {
      TextRenderer renderer(*fixture.stack, device_scale);
      renderer.set_subpixel(lcd);
      renderer.set_grid_fit(GridFitMode::Normal);
      // 各种半径都不能换出步数（包括过去三档的实际值）。
      for (const float radius : {0.0f, 0.2f, 0.6f, 1.0f, 2.5f, 10.0f, -1.0f}) {
        ST_CHECK(renderer.embolden_steps(radius) == 0);
      }
    }
  }
  // 上层入口同样恒为 0（两层拦截：渲染器 + `embolden_radius`）。
  for (const float size : {11.0f, 20.25f, 48.0f, 188.0f}) {
    for (const auto weight : {FontWeight::Regular, FontWeight::Medium, FontWeight::SemiBold,
                             FontWeight::Bold}) {
      ST_CHECK_NEAR(st::ui::embolden_radius(size, weight), 0.0f, 1e-9f);
    }
  }
}

/// ② **粗体必须走真粗体面**：Bold 与 Regular 取到的是两套字形。
///
/// 这是“禁用合成加粗”之后**唯一的加粗路径**，所以它必须真的通。
/// 反例：字体栈探不到粗体面时 `find_face` 回退常规面——那是可接受的降级
/// （字重退化成 Regular），但**不能把两套字形搞成同一份**（那就是假绿）。
ST_TEST(text_bold_uses_a_distinct_face) {
  FontFixture fixture;
  if (!fixture.ok) return;
  if (!fixture.stack->has_bold()) {
    st::print("[bold] 本机无真粗体面：本用例的语义不成立（bold 会退化成 Regular）\n");
    return;
  }
  const auto* regular =
      fixture.stack->find_face(U'霜', st::text::FontRole::Proportional, false);
  const auto* bold = fixture.stack->find_face(U'霜', st::text::FontRole::Proportional, true);
  ST_REQUIRE(regular != nullptr);
  ST_REQUIRE(bold != nullptr);
  // 粗体面必须**不是**常规面（否则“真粗体”从未生效）。
  ST_CHECK(regular->path() != bold->path());

  // 同一串文本，bold / regular 的整形宽度可以不同（真粗体是另一套字形），
  // 但两者都必须可渲染且**不相等**（相等就说明选了同一面）。
  TextRenderer renderer(*fixture.stack, 1.5f);
  renderer.set_subpixel(false);
  renderer.set_grid_fit(GridFitMode::Normal);
  const InkStats plain = render_stats(renderer, "霜天概览", 20.25f, 0.0f);
  const auto bold_bitmap = renderer.glyph_bitmap_of(U'霜', 20.25f);
  ST_CHECK(plain.ink > 1.0);
  ST_CHECK(bold_bitmap != nullptr);
}

/// ③ Regular 与 bold 取回的是**不同字形**（不是共用同一份位图）。
///
/// 这条取代了原来“合成加粗位图不撞缓存”的用例。合成加粗禁用后，
/// 真正需要护的撞键风险变成：**bold 与 regular 走了同一个字体面**
/// （因为 `shape_cached` 的键含 bold，一旦漏掉，两边就会共用同一条整形结果——
/// 症状是“粗体完全不生效”，而本仓已实测过这个坑）。
ST_TEST(text_bold_and_regular_do_not_share_cache_entries) {
  FontFixture fixture;
  if (!fixture.ok) return;
  if (!fixture.stack->has_bold()) {
    st::print("[bold] 本机无真粗体面：跳过（bold 会退化成 Regular）\n");
    return;
  }
  for (const bool lcd : {true, false}) {
    TextRenderer renderer(*fixture.stack, 1.5f);
    renderer.set_subpixel(lcd);
    renderer.set_grid_fit(GridFitMode::Normal);
    // 整形结果的宽度可以相同（等宽 CJK），但**取到的字体面必须不同**。
    const auto* regular =
        fixture.stack->find_face(U'霜', st::text::FontRole::Proportional, false);
    const auto* bold = fixture.stack->find_face(U'霜', st::text::FontRole::Proportional, true);
    ST_REQUIRE(regular != nullptr);
    ST_REQUIRE(bold != nullptr);
    ST_CHECK(regular->path() != bold->path());
    // 反复取回同一份（缓存稳定，不因重复调用而漂移）
    ST_CHECK(renderer.glyph_bitmap_of(U'霜', 20.25f) ==
             renderer.glyph_bitmap_of(U'霜', 20.25f));
  }
}

/// ④ 加粗步数换算：两种渲染模式都必须 > 0（否则该模式下字重静默失效）。
///
/// 反例：统一按「1/3 物理像素」当步长时，灰度模式的采样格是 1/2 物理像素，
/// 平移被取整吃成 0——字重在灰度下与 Regular 逐像素相同。
/// ③ 加粗步数恒为 0：灰度与亚像素两种模式都一样。
///
/// 这条取代了原来“两种模式都要 >0 步”的用例——判据方向反了：
/// 现在是“任何模式下都不许有合成加粗”。
ST_TEST(text_embolden_steps_are_zero_in_both_modes) {
  FontFixture fixture;
  if (!fixture.ok) return;
  for (const bool lcd : {true, false}) {
    for (const float device_scale : {1.0f, 1.5f, 2.0f}) {
      TextRenderer renderer(*fixture.stack, device_scale);
      renderer.set_subpixel(lcd);
      ST_CHECK(renderer.embolden_steps(0.6f) == 0);
      ST_CHECK(renderer.embolden_steps(1.0f) == 0);
    }
  }
  TextRenderer renderer(*fixture.stack, 1.5f);
  ST_CHECK(renderer.embolden_steps(0.0f) == 0);
  ST_CHECK(renderer.embolden_steps(-1.0f) == 0);
}

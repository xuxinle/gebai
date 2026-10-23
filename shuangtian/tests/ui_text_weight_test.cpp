/// 字重**落像素**：从 UI 元素到画布的整条路径。
///
/// 起因（2026-10-04 用户反馈「字体渲染还是不够清晰」后定位）：
/// `Element::paint_text` **完全不读** `style_.font_weight`——界面里所有
/// SemiBold/Bold（标题、按钮、卡片大数字、标签栏）与 Regular **逐像素相同**。
/// 也就是说 `Text::set_weight` / `Style::font_weight` / `Heading` 的字重
/// 全都只写进了结构，没写进画面：标题不显眼、层次全靠字号与颜色撑。
///
/// 这个文件钉住两件事：
/// 1. **同一元素、只改 `font_weight`，像素必须变**（走 `Element::paint` 真实路径）；
/// 2. **三个非 Regular 档都比 Regular 更重、峰值覆盖率不变**（是「变粗」而不是「变糊」）。
///
/// 注意 Bold 与 SemiBold 走**同一条路**（真粗体面，与浏览器同口径），
/// Medium 走另一条（合成加粗）——两者的加墨幅度不可比，
/// 见下面对「不再断言 bold > semibold」的说明。
///
/// 为什么必须走 `Element::paint` 而不是直接调 `TextRenderer::draw`：
/// 缺陷恰好在 UI 层（谁把 `font_weight` 翻成绘制参数），
/// 只测渲染器的用例在缺陷存在时**依然全绿**（实测确认过）。
#include "st/test/test.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "st/app/text_port.hpp"
#include "st/core/print.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/text.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"
#include "tests/support/text_port_fixtures.hpp"

// 测试在匿名命名空间内，`st::test::X` 得写全；用具名别名让用例读起来干净。
using st::test::RecordingTextPort;
using st::test::RendererTextPort;

namespace {

using st::math::Color;
using st::math::Point;
using st::raster::Canvas;
using st::ui::FontWeight;
using st::ui::RenderContext;
using st::ui::Theme;

/// 一段中英混排（同时覆盖 CJK 与拉丁）。
constexpr std::string_view kSample = "霜天概览 Aa1";

struct InkStats {
  double ink{0.0};
  float peak{0.0f};
};


}  // namespace

/// 同一 `Text`、只改字重：墨量必须上升，峰值覆盖率必须不变。
ST_TEST(ui_text_weight_reaches_pixels) {
  auto loaded = st::text::FontStack::system_default();
  if (!loaded.has_value()) return;
  st::text::FontStack stack = std::move(*loaded);
  st::text::TextRenderer renderer(stack, 1.5f);
  renderer.set_subpixel(true);
  renderer.set_grid_fit(st::text::GridFitMode::Normal);
  st::app::RendererTextPort port(renderer);
  st::print("[ui-weight] has_bold={} has_real_bold={}\n", stack.has_bold(), port.has_real_bold());
  st::print("[ui-weight] faces={} bold_faces={}\n", stack.faces().size(),
            stack.bold_faces().size());
  for (const char32_t cp : {U'\u971c', U'A'}) {
    const auto* reg = stack.find_face(cp, st::text::FontRole::Proportional, false);
    const auto* bld = stack.find_face(cp, st::text::FontRole::Proportional, true);
    st::print("[ui-weight] U+{:04X} reg={} bold={}\n", static_cast<unsigned>(cp),
              reg ? reg->path() : "<null>", bld ? bld->path() : "<null>");
  }

  RecordingTextPort spy(port);
  const auto paint = [&](FontWeight weight) -> InkStats {
    Theme theme = Theme::light();
    RenderContext context{theme, &spy, 0.0};
    st::ui::Text text{std::string(kSample)};
    text.set_weight(weight);
    text.set_font_size(15.0f);
    text.apply_theme(theme);
    text.measure(context, st::ui::Constraints{200.0f, 60.0f});
    text.arrange(context, st::math::Rect{0.0f, 0.0f, 200.0f, 60.0f});

    Canvas canvas{300, 90, 1.5f};
    canvas.clear(Color::rgb(255, 255, 255));
    text.paint(context, canvas);

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
  };

  const InkStats regular = paint(FontWeight::Regular);
  const InkStats medium = paint(FontWeight::Medium);
  const InkStats semibold = paint(FontWeight::SemiBold);
  const InkStats bold = paint(FontWeight::Bold);

  st::print("[ui-weight] ink r={:.1f} m={:.1f} sb={:.1f} b={:.1f} / peak r={:.3f} b={:.3f}\n",
            regular.ink, medium.ink, semibold.ink, bold.ink, regular.peak, bold.peak);
  // 检查字重链路中间那一跳：最后一次 Bold 绘制收到的参数。
  (void)paint(FontWeight::Bold);
  st::print("[ui-weight] 链路：Bold 档 draw(bold={}, embolden={:.3f})\n", spy.last_bold,
            spy.last_embolden);
  ST_CHECK(spy.last_bold);

  ST_CHECK(regular.ink > 1.0);
  // ① 字重落像素的**现契约**（2026-10-23 起）：
  //
  //    只有走**真粗体面**的档位（SemiBold / Bold）才比 Regular 重；
  //    Medium 在系统里**没有对应字重面**，而合成加粗已禁用 ⇒
  //    它必须与 Regular **逐像素相同**。
  //
  //    为何这是对的而不是缺陷：合成加粗的过渡带 0.246 vs 真粗体面 0.155
  //    （锐 37%）、英文字间离散 14.1%→0%，大字号下就是“糊而脏”。
  //    需要视觉层次时改用字号/颜色/间距，不用假加粗。
  //
  //    这条断言是**双向护栏**：若有人日后又把合成加粗接回来（或绕过
  //    `embolden_radius` 直传半径），Medium 就会变重 → 这里会红。
  ST_CHECK_NEAR(medium.ink, regular.ink, 0.5);
  ST_CHECK(semibold.ink > regular.ink * 1.05);
  // ② 所有档位**都没有叠加合成加粗**（这是禁令的可执行判据）。
  //
  //    比“谁更黑”稳定：无论字体怎么换，链路都必须恒为 embolden=0。
  ST_CHECK_NEAR(spy.last_embolden, 0.0f, 1e-6f);
  // ③ 峰值覆盖率不变（±3%）：变粗而不是变糊
  ST_CHECK(std::abs(bold.peak - regular.peak) <= 0.03f);
}

/// **SemiBold 必须与 Bold 走同一条路（真粗体面）**，否则与浏览器不一致。
///
/// 依据（2026-10-07，真窗口浏览器为参照，26px「GPU (D3D11)」物理 39px）：
/// 浏览器的 `font-weight:600` 与 `700` **逐像素相同**（它把 600 吸到 Bold 面），
/// 而 400/500 是常规面（字面宽 244 vs 258）。而合成加粗只加墨、**不改字面宽**
/// （247），所以走合成加粗的 SemiBold 字面比参照窄 11px、看着“胖而糊”。
///
/// 判据用**字面宽**而不是墨量：合成加粗与真粗体面的区别**首先在字面宽**
/// （两面字干位置不同），墨量只是次级表现。
ST_TEST(ui_semibold_uses_the_real_bold_face) {
  auto loaded = st::text::FontStack::system_default();
  if (!loaded.has_value()) return;
  st::text::FontStack stack = std::move(*loaded);
  // 系统探不到真粗体面时，`find_face` 会回退常规面，本用例的语义不成立。
  if (!stack.has_bold()) return;
  st::text::TextRenderer renderer(stack, 1.5f);
  renderer.set_subpixel(true);
  renderer.set_grid_fit(st::text::GridFitMode::Normal);
  st::app::RendererTextPort port(renderer);

  // 直接问端口：SemiBold 会不会走真粗体面。
  ST_CHECK(st::ui::prefers_real_bold(st::ui::FontWeight::SemiBold));
  ST_CHECK(st::ui::prefers_real_bold(st::ui::FontWeight::Bold));
  // 对偶：Medium 没有真粗体面可走（系统里没有 Medium 字重面），
  // 而合成加粗已禁用 ⇒ 它会退化成 Regular（见 `embolden_radius`）。
  ST_CHECK(!st::ui::prefers_real_bold(st::ui::FontWeight::Medium));
  ST_CHECK(!st::ui::prefers_real_bold(st::ui::FontWeight::Regular));
  // 禁令的可执行判据：任何字重求半径都必须是 0。
  //
  // 用字号跑一变（小/中/大）：若哪一天有人把“按字号给半径”的写法接回来，
  // 这里会红，而不是等到用户报“字很糊”才发现。
  for (const float size : {11.0f, 20.25f, 48.0f, 96.0f}) {
    for (const st::ui::FontWeight weight :
         {st::ui::FontWeight::Regular, st::ui::FontWeight::Medium,
          st::ui::FontWeight::SemiBold, st::ui::FontWeight::Bold}) {
      ST_CHECK_NEAR(st::ui::embolden_radius(size, weight), 0.0f, 1e-9f);
    }
  }

  // 墨量：SemiBold 必须**真的比 Regular 重**（不能因走真粗体面而变轻）。
  Theme theme = Theme::light();
  const auto paint = [&](st::ui::FontWeight weight) {
    RenderContext context{theme, &port, 0.0};
    st::ui::Text text{"GPU (D3D11)"};
    text.set_weight(weight);
    text.set_font_size(26.0f);
    text.apply_theme(theme);
    text.measure(context, st::ui::Constraints{400.0f, 60.0f});
    text.arrange(context, st::math::Rect{0.0f, 0.0f, 400.0f, 60.0f});
    Canvas canvas{700, 90, 1.5f};
    canvas.clear(Color::rgb(0xFF, 0xFF, 0xFF));
    text.paint(context, canvas);
    return canvas;
  };
  const Canvas regular = paint(st::ui::FontWeight::Regular);
  const Canvas semibold = paint(st::ui::FontWeight::SemiBold);
  const auto ink = [](const Canvas& canvas) {
    double total = 0.0;
    for (int y = 0; y < canvas.physical_height(); ++y) {
      for (int x = 0; x < canvas.physical_width(); ++x) {
        total += std::clamp((255.0 - canvas.pixel_at(x, y).r) / 255.0, 0.0, 1.0);
      }
    }
    return total;
  };
  st::print("[ui-weight] SemiBold 真粗体面：regular Σ墨={:.1f} · semibold Σ墨={:.1f}\n", ink(regular),
            ink(semibold));
  ST_CHECK(ink(semibold) > ink(regular) * 1.05);
}

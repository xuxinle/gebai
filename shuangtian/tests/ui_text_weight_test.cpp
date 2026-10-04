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
/// 2. **越高字重墨量越大、峰值覆盖率不变**（是「变粗」而不是「变糊」）。
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

  const auto paint = [&](FontWeight weight) -> InkStats {
    Theme theme = Theme::light();
    RenderContext context{theme, &port, 0.0};
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

  ST_CHECK(regular.ink > 1.0);
  // ① 字重真的落像素（每一档都加墨）
  ST_CHECK(medium.ink > regular.ink * 1.05);
  ST_CHECK(semibold.ink > medium.ink);
  ST_CHECK(bold.ink > semibold.ink);
  // ② 峰值覆盖率不变（±3%）：变粗而不是变糊
  ST_CHECK(std::abs(bold.peak - regular.peak) <= 0.03f);
}

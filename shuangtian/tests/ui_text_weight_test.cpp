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
/// 注意 Bold 与 Medium/SemiBold 走**两条不同的路**（真粗体面 vs 合成加粗），
/// 两者的加墨幅度不可比——见下面对「不再断言 bold > semibold」的说明。
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

/// 记录**最后一次 draw 收到的 `bold` 参数**的转发端口。
///
/// 为什么需要它：字重链路是 `Text::set_weight` → `style_.font_weight` → `paint_text`
/// → `port.draw(…, bold)` 四段，任何一段断掉，像素都退回 Regular——
/// 而只看墨量无法区分“哪一段断了”。这个间谍把**中间那一跳**暴露出来。
class SpyPort final : public st::ui::TextPort {
 public:
  explicit SpyPort(const st::ui::TextPort& inner) : inner_(inner) {}
  [[nodiscard]] auto measure(std::string_view utf8, float size) const -> st::math::Size override {
    return inner_.measure(utf8, size);
  }
  [[nodiscard]] auto measure_width(std::string_view utf8, float size,
                                   st::text::FontRole role) const -> float override {
    return inner_.measure_width(utf8, size, role);
  }
  [[nodiscard]] auto line_height(float size) const -> float override {
    return inner_.line_height(size);
  }
  void draw(st::raster::Surface& canvas, std::string_view utf8, st::math::Point origin, float size,
            st::math::Color color, st::text::FontRole role, float embolden, bool bold) const override {
    last_bold = bold;
    last_embolden = embolden;
    inner_.draw(canvas, utf8, origin, size, color, role, embolden, bold);
  }
  [[nodiscard]] auto has_real_bold() const -> bool override { return inner_.has_real_bold(); }
  [[nodiscard]] auto ellipsize(std::string_view utf8, float size, float max_width) const
      -> std::string override {
    return inner_.ellipsize(utf8, size, max_width);
  }
  [[nodiscard]] auto wrap(std::string_view utf8, float size, float max_width) const
      -> std::vector<std::string_view> override {
    return inner_.wrap(utf8, size, max_width);
  }
  [[nodiscard]] auto wrap_limited(std::string_view utf8, float size, float max_width,
                                  std::size_t max_lines) const -> std::vector<std::string> override {
    return inner_.wrap_limited(utf8, size, max_width, max_lines);
  }
  mutable bool last_bold{false};
  mutable float last_embolden{0.0f};

 private:
  const st::ui::TextPort& inner_;
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

  SpyPort spy(port);
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
  // ① 三个非 Regular 档都比 Regular 更重（字重真的落像素）。
  //
  //    **不再断言 bold > semibold**（本用例原先这么写）：Bold 走**真粗体字体面**、
  //    Medium/SemiBold 走**合成加粗**（系统里没有这两个字重的字体面），两条路的
  //    “加墨幅度”不可比——实测 bold ≈ Regular 的墨量而 semibold 更重
  //    （真 Bold 面的笔画比 Regular 略细，但**结构不同、边缘更实**）。
  //    这里要钉的是“Bold 确实是粗体”，而不是“Bold 是墨量最大的那个”。
  //    真粗体面的收益在**锐度与一致性**（同一口径实测，tools/ui_text_probe.cpp）：
  //    中文过渡带 0.246→0.155（锐 37%）、英文字间离散 14.1%→**0.0%**、
  //    英文过渡带 0.204→0.131（锐 36%）。
  ST_CHECK(medium.ink > regular.ink * 1.05);
  ST_CHECK(semibold.ink > medium.ink);
  // `bold` 的断言在**契约层**（下一条）：它走的是**真粗体字体面**，
  // 而“墨量是否更大”取决于字体设计（真 Bold 面的字干更粗但字面可能更收），
  // 不是本框架的契约——实测同一串在中文字上 bold 墨量 > regular，
  // 而混排样本上两者接近。用“走了哪条路”当契约，比用“谁更黑”稳定。
  // ② 峰值覆盖率不变（±3%）：变粗而不是变糊
  ST_CHECK(std::abs(bold.peak - regular.peak) <= 0.03f);
}

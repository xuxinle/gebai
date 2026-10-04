#pragma once
#include "st/text/text.hpp"  // text::FontRole（等宽/正文角色）

/// 文本绘制端口：UI 层对"字体引擎"的唯一依赖面（依赖倒置）。
/// 目的：`ui` 层不依赖 `text` 层实现（编译解耦、可注入假实现做布局单测）；
/// 由 `app` 层用 `text::TextRenderer` 适配（见 `include/st/app/text_port.hpp`）。

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/style.hpp"  // FontWeight（合成加粗档位）

namespace st::ui {

class TextPort {
 public:
  virtual ~TextPort() = default;

  /// 度量文本尺寸（宽 × 行高）。
  [[nodiscard]] virtual auto measure(std::string_view utf8, float size) const -> math::Size = 0;
  [[nodiscard]] virtual auto measure_width(
      std::string_view utf8, float size,
      text::FontRole role = text::FontRole::Proportional) const -> float = 0;
  [[nodiscard]] virtual auto line_height(float size) const -> float = 0;
  /// `origin` 为行左上角。
  ///
  /// `embolden` 为**合成加粗的笔画外扩半径（物理像素）**，0 = 不加粗。
  /// 存在的理由：框架没有独立字重的字体面（字体栈是单面的），而界面里的标题/按钮/
  /// 大数字都标了非 Regular 字重——不提供这条路，「字重」就只是一个落不到像素上的属性
  /// （实测：`Element::paint_text` 原先完全不读 `style_.font_weight`，
  /// SemiBold 与 Regular 渲染逐像素相同）。
  /// 做法与 Skia `SkFont::setEmbolden` / FreeType `FT_GlyphSlot_Embolden` 同一取向：
  /// 把同一份字形沿**水平方向**外扩。
  ///
  /// 为什么传半径而不是采样格步数：步数取决于**渲染模式**（亚像素/灰度）与超采样倍率，
  /// 那是渲染器自己的状态；端口只转递物理口径的语义量（半径由 `embolden_radius` 给出）。
  virtual void draw(raster::Surface& canvas, std::string_view utf8, math::Point origin, float size,
                    math::Color color, text::FontRole role = text::FontRole::Proportional,
                    float embolden = 0.0f, bool bold = false) const = 0;
  /// 字体栈是否提供**真粗体面**（没有时调用方仍可用合成加粗补足；默认无）。
  [[nodiscard]] virtual auto has_real_bold() const -> bool { return false; }
  [[nodiscard]] virtual auto ellipsize(std::string_view utf8, float size, float max_width) const
      -> std::string = 0;
  [[nodiscard]] virtual auto wrap(std::string_view utf8, float size, float max_width) const
      -> std::vector<std::string_view> = 0;
  /// 按最大行数折行（超出末行以省略号收尾）。
  [[nodiscard]] virtual auto wrap_limited(std::string_view utf8, float size, float max_width,
                                          std::size_t max_lines) const -> std::vector<std::string> = 0;
};

/// 空实现：无字体环境下布局仍可运行（文本宽度按 0 计，绘制为 no-op）。
class NullTextPort final : public TextPort {
 public:
  [[nodiscard]] auto measure(std::string_view utf8, float size) const -> math::Size override;
  [[nodiscard]] auto measure_width(
      std::string_view utf8, float size,
      text::FontRole role = text::FontRole::Proportional) const -> float override;
  [[nodiscard]] auto line_height(float size) const -> float override;
  /// `bold=true` = 该字重**已由真粗体字体面承担**（见 `prefers_real_bold`）：
  /// 此时合成加粗不要叠加（叠了会把已加粗的字再撑开，字腔粘住）。
  void draw(raster::Surface& canvas, std::string_view utf8, math::Point origin, float size,
            math::Color color, text::FontRole role = text::FontRole::Proportional,
            float embolden = 0.0f, bool bold = false) const override;
  [[nodiscard]] auto ellipsize(std::string_view utf8, float size, float max_width) const
      -> std::string override;
  [[nodiscard]] auto wrap(std::string_view utf8, float size, float max_width) const
      -> std::vector<std::string_view> override;
  [[nodiscard]] auto wrap_limited(std::string_view utf8, float size, float max_width,
                                  std::size_t max_lines) const -> std::vector<std::string> override;

  [[nodiscard]] static auto instance() -> const NullTextPort&;
};

/// 合成加粗（fake bold）在**物理像素**下的笔画外扩半径。
///
/// 与 Skia `SkFont::setEmbolden` / FreeType `FT_GlyphSlot_Embolden` 同一取向：
/// 按**字号**给比例值，而不是固定像素——固定像素在 11px 上会把字糊死、
/// 在 48px 上又完全看不出来。
///
/// 三档比例经实测校准（`tools/text_weight_probe.cpp`）：以 20.25px 中英混排为例，
/// 墨量增幅 Medium ≈ +17%、SemiBold ≈ +30%、Bold ≈ +42%（**峰值覆盖率不变**，
/// 即“变粗”而不是“变糊”）；作为对照，Chrome 在同一字体上切**真 Bold 字体面**
/// 的墨量增幅是 +55%~+79%——那背后是另一套字形轮廓，合成加粗不该去追那个数，
/// 否则 CJK 小字的笔画会粘在一起。
[[nodiscard]] inline auto embolden_radius(float pixel_size, FontWeight weight) noexcept -> float {
  switch (weight) {
    case FontWeight::Medium: return pixel_size / 48.0f;
    case FontWeight::SemiBold: return pixel_size / 28.0f;
    case FontWeight::Bold: return pixel_size / 18.0f;
    case FontWeight::Regular: break;
  }
  return 0.0f;
}

/// 该字重是否应当改用**真粗体字体面**（`FontStack::find_face(…, bold=true)`）。
///
/// 依据（2026-10-04 实测）：用户报「中文粗体有点糊、英文不够均匀锐利」，根因是
/// 粗体靠**合成加粗**（同轮廓水平平移重复填充），而系统里**本来就有真粗体面**。
/// 同一口径实测（逻辑字号 20.25 / 缩放 1.5）：
///
/// | 指标 | 合成加粗 | 真粗体面 |
/// |---|---|---|
/// | 中文过渡带占比 | 0.246 | **0.155**（锐 37%） |
/// | 中文过渡/墨像素 | 0.433 | **0.369** |
/// | 英文字间离散 | 14.1% | **0.0%** |
/// | 英文过渡带占比 | 0.204 | **0.131**（锐 36%） |
///
/// 因此 **Bold 档优先用真粗体面**（探不到时 `find_face` 自动回退常规面，此时仍可
/// 叠加合成加粗补足）；`Medium`/`SemiBold` 在系统里没有对应字体面，仍走合成加粗。
[[nodiscard]] inline auto prefers_real_bold(FontWeight weight) noexcept -> bool {
  return weight == FontWeight::Bold;
}

}  // namespace st::ui

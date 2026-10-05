#pragma once

/// 设计系统（`DESIGN.md` §5）：token 表 + 亮/暗主题。
/// 霜天意象——冷冽清晨：中性色偏冷，品牌色冰蓝，辅以青色点缀。

#include <cstdint>
#include <string>
#include <string_view>

#include "st/math/color.hpp"
#include "st/ui/style.hpp"

namespace st::ui {

/// 颜色 token（亮/暗各一套）。
struct Palette {
  math::Color bg{};
  math::Color surface{};
  math::Color surface_alt{};
  math::Color surface_sunken{};
  math::Color border{};
  math::Color border_strong{};
  math::Color text{};
  math::Color text_muted{};
  math::Color text_faint{};
  math::Color primary{};
  math::Color primary_hover{};
  math::Color primary_active{};
  /// 通用交互态（不依赖主色）：任意可交互表面悬浮/按下时的背景与描边。
  /// 与 `primary_*` 分开：主色是"品牌色按钮"的语气，这两个是"中性表面"的语气。
  math::Color surface_hover{};
  math::Color surface_pressed{};
  math::Color border_hover{};
  /// 悬浮外发光（"特效"）：一圈低不透明度的强调色，用于卡片/列表项的聚焦提示。
  math::Color glow{};
  math::Color primary_soft{};
  math::Color on_primary{};
  math::Color accent{};
  math::Color accent_soft{};
  math::Color success{};
  math::Color warning{};
  math::Color danger{};
  math::Color focus_ring{};
  math::Color overlay{};
  math::Color shadow{};
  math::Color selection{};
  math::Color code_bg{};
  math::Color code_border{};
};

/// 语法高亮色板（代码编辑器与代码块共用；token 类别 → 颜色）。
///
/// 独立于通用 `Palette`：编辑器配色是**高辨识度优先**（关键字/字符串/注释/数字彼此可区分），
/// 与界面语义色（primary/danger）诉求不同——分开后换主题只需替换这一组。
struct SyntaxPalette {
  math::Color plain{};
  math::Color keyword{};
  math::Color type{};
  math::Color string{};
  math::Color number{};
  math::Color comment{};
  math::Color function{};
  math::Color operator_{};
  math::Color punctuation{};
  math::Color preprocessor{};
  math::Color builtin{};
  math::Color attribute{};
  math::Color key{};
  math::Color tag{};
  math::Color inserted{};   ///< Diff 新增行
  math::Color deleted{};    ///< Diff 删除行
  math::Color line_number{};
  math::Color current_line{};      ///< 当前行底色（半透明）
  math::Color selection{};
  math::Color cursor{};
  math::Color matching_bracket{};  ///< 括号配对高亮
  math::Color find_highlight{};    ///< 查找命中底色（全部匹配）
  math::Color find_active{};       ///< 查找当前命中（更强）
};

/// 尺度 token（间距/圆角/字号/动效）。
struct Metrics {
  float space_xs{4.0f};
  float space_sm{8.0f};
  float space_md{12.0f};
  float space_lg{16.0f};
  float space_xl{24.0f};
  float space_2xl{32.0f};

  /// 悬浮特效参数（单位：逻辑像素 / 秒）。
  float hover_lift{1.5f};         ///< 上浮距离（卡片/按钮的"抬起"感）
  float hover_glow_width{3.0f};   ///< 外发光宽度
  double hover_duration{0.12};    ///< 悬浮过渡时长（0 = 立即，不做动画）
  float press_sink{1.0f};         ///< 按下下沉距离（与 lift 形成"按下"的手感）

  float radius_sm{6.0f};
  float radius_md{10.0f};
  float radius_lg{14.0f};
  float radius_xl{20.0f};
  float radius_pill{999.0f};

  float font_xs{13.0f};
  float font_sm{14.0f};
  float font_base{15.0f};
  float font_lg{17.0f};
  float font_xl{20.0f};
  float font_2xl{26.0f};
  float font_3xl{34.0f};

  float line_height_body{1.45f};
  float line_height_heading{1.25f};

  float control_height{34.0f};
  float control_height_sm{28.0f};
  float control_height_lg{40.0f};
  float border_width{1.0f};
  float focus_width{2.0f};

  float motion_fast{120.0f};
  float motion_normal{180.0f};
  float motion_slow{260.0f};

  /// **整条字号阶梯统一放大/缩小**（默认 1.0 = 不动）。
  ///
  /// 为什么是"整条阶梯"而不是单档：字号之间是**相对关系**（正文 vs 次要 vs 标题），
  /// 只动一档会把层次拉平（例如 sm 与 base 都是 14 就分不出主次）。
  /// 缩放作用在 `font_*` 上，**间距/控件高度不动**——它们有自己的视觉基准，
  /// 跟着字号一起放大只会让界面变松散。
  ///
  /// 调用时机：构造主题之后、组件 `apply_theme` 之前（见 `scale_fonts`）。
  float font_scale{1.0f};

  /// 按 `factor` 缩放全部 `font_*` 档位（结果写回各字段，并记录 `font_scale`）。
  /// 只接受正数；非正值忽略（不要静默把字号变成 0——那会让整屏文字消失）。
  void scale_fonts(float factor) noexcept {
    if (!(factor > 0.0f)) return;
    const float ratio = factor / font_scale;
    font_xs *= ratio;
    font_sm *= ratio;
    font_base *= ratio;
    font_lg *= ratio;
    font_xl *= ratio;
    font_2xl *= ratio;
    font_3xl *= ratio;
    font_scale = factor;
  }
};

enum class ThemeMode : std::uint8_t { Light, Dark };

/// 语义色调（组件用它表达意图，实际颜色由主题解析）。
enum class Tone : std::uint8_t {
  Default,
  Muted,
  Faint,
  Primary,
  Accent,
  Success,
  Warning,
  Danger,
  OnPrimary,
};

class Theme {
 public:
  [[nodiscard]] static auto light() -> Theme;
  [[nodiscard]] static auto dark() -> Theme;
  [[nodiscard]] static auto by_mode(ThemeMode mode) -> Theme;

  [[nodiscard]] auto mode() const noexcept -> ThemeMode { return mode_; }
  [[nodiscard]] auto colors() const noexcept -> const Palette& { return colors_; }
  [[nodiscard]] auto colors() noexcept -> Palette& { return colors_; }
  [[nodiscard]] auto metrics() const noexcept -> const Metrics& { return metrics_; }
  [[nodiscard]] auto metrics() noexcept -> Metrics& { return metrics_; }
  [[nodiscard]] auto syntax() const noexcept -> const SyntaxPalette& { return syntax_; }
  [[nodiscard]] auto syntax() noexcept -> SyntaxPalette& { return syntax_; }

  [[nodiscard]] auto font_family() const -> const std::string& { return font_family_; }
  void set_font_family(std::string family) { font_family_ = std::move(family); }

  /// 基础间距的倍数（4px 栅格）。
  [[nodiscard]] auto space(float steps) const noexcept -> float { return 4.0f * steps; }

 private:
  ThemeMode mode_{ThemeMode::Light};
  Palette colors_{};
  SyntaxPalette syntax_{};
  Metrics metrics_{};
  std::string font_family_{"Noto Sans CJK SC"};
};

/// 色调 → 实际颜色（亮/暗主题通用）。
[[nodiscard]] auto tone_color(const Theme& theme, Tone tone) -> math::Color;
/// 色调的稳定短名（控制通道/视觉树上报复用；不要拿枚举序号——那会随枚举顺序变）。
[[nodiscard]] constexpr auto tone_name(Tone tone) noexcept -> std::string_view {
  switch (tone) {
    case Tone::Default: return "default";
    case Tone::Muted: return "muted";
    case Tone::Faint: return "faint";
    case Tone::Primary: return "primary";
    case Tone::Accent: return "accent";
    case Tone::Success: return "success";
    case Tone::Warning: return "warning";
    case Tone::Danger: return "danger";
    case Tone::OnPrimary: return "on_primary";
  }
  return "default";
}
/// 色调的浅底（徽标/Chip 背景）。
[[nodiscard]] auto tone_soft_color(const Theme& theme, Tone tone) -> math::Color;

/// 阴影 token（随主题取色）。
[[nodiscard]] auto shadow_sm(const Theme& theme) -> Shadow;
[[nodiscard]] auto shadow_md(const Theme& theme) -> Shadow;
[[nodiscard]] auto shadow_lg(const Theme& theme) -> Shadow;

}  // namespace st::ui

#pragma once

/// 设计系统（`DESIGN.md` §5）：token 表 + 亮/暗主题。
/// 霜天意象——冷冽清晨：中性色偏冷，品牌色冰蓝，辅以青色点缀。

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/color.hpp"
#include "st/ui/style.hpp"

namespace st::ui {

/// 颜色 token（亮/暗各一套）。
///
/// ## 亚克力口径（默认主题）
///
/// 默认调色板是**黑色亚克力 / 白色亚克力**（与歌白同名主题同源）：近中性而略偏冷的
/// 半透明层叠，层次靠**表面阶梯 + 描边 + 内高光**表达，而不是靠高饱和品牌色。
/// 三条因此而来的约定，改 token 时不要破坏：
///
/// 1. **描边与交互态是半透明的**（`border*`/`surface_hover`/`surface_pressed`/`highlight`）。
///    亚克力的描边是「叠在任意底色上的一道白光/黑光」，而不是一个固定灰值——
///    固定灰值在 `bg` 与 `surface` 上会显出两种强度，这也是浅色主题里
///    「同一个 `border` 在卡片上清楚、在页底上糊掉」的根因。
///    测量这类 token 时必须先**合成到底色**（`ui_theme_test.cpp` 的 `composite`）。
/// 2. **表面阶梯必须逐级可辨**：`bg` → `surface` → `surface_alt`/`surface_raised` 相邻
///    对比度差要够（否则「同色系」退化成「一片同色」）。
/// 3. **`surface_sunken` 在两个模式下都更暗**（凹槽/输入底/代码底）：
///    语义是「凹进去」，不随亮暗反转。
struct Palette {
  math::Color bg{};
  math::Color surface{};
  math::Color surface_alt{};
  math::Color surface_sunken{};
  /// 抬升面：弹出层 / 菜单 / 对话框 / Toast 的底色，比 `surface` 再亮一档。
  /// 浅色下与 `surface` 同为白系（抬升靠阴影表达），深色下必须**比 `surface` 亮**。
  math::Color surface_raised{};
  math::Color border{};
  math::Color border_strong{};
  /// 更弱的分隔线（列表行 / 区段之间）：比 `border` 再淡一档，用于「同组内」的细分。
  math::Color border_subtle{};
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
  /// **顶部内高光**（亚克力的"玻璃边缘"）：抬升面顶端一道极弱亮线。
  /// 由 `Style::top_highlight` 消费（`Element::paint_box` 统一绘制），
  /// 在圆角内裁剪后只画顶边那一条——这才有"一块玻璃"而不是"一个色块"的观感。
  math::Color highlight{};
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

  /// **阴影强度档位**（1.0 = 默认；`shadow_sm/md/lg` 依次乘上）。
  ///
  /// 为什么是旋钮而不是写死的数：亚克力的"浮起"全靠阴影，而**浅底与深底对同一道
  /// 阴影的感知差好几倍**（深底几乎看不见、浅底一浓就显脏）。把强度与扩散拆成两个
  /// 可调量，自定义主题才不必改代码就能把投影调到合适——见 `shadow_*()`。
  float shadow_strength{1.0f};
  /// **阴影扩散档位**（1.0 = 默认；乘在两层模糊半径上）。调大可让投影更"散"而不更"黑"。
  float shadow_spread{1.0f};

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

/// 终端色板（`st::ui::Terminal` 专用）。
///
/// 为什么独立于 `Palette`：终端要的是「一块屏幕」的观感——底色 / 前景 / 光标
/// 自成一套，且 **16 个 ANSI 标准色必须成组上下标**。后者是硬需求：
/// ANSI 色是**协议固定语义**（`31` 永远是红、`93` 永远是亮黄），
/// 程序按它们选色，主题不能只换个底色就了事。
///
/// 为何不能一套色用到底：前 16 色在不同底色上**对比度方向相反**——
/// 为深底调的亮黄（`#E5E510`）放到白底上对比度只有 **1.00**（字面看不见）、
/// 亮白 `#E5E5E5` 是 **1.07**、亮绿 `#23D18B` 是 **1.47**。
/// 所以亮暗两套各自成表，且**每色对底色 ≥4.5:1**（WCAG AA 正文口径，见回归断言）。
struct TerminalPalette {
  math::Color bg{};        ///< 屏幕底色（不借 `surface_sunken`：那是通用凹槽语义）
  math::Color fg{};        ///< 默认前景（`SGR 39` 回到它）
  math::Color cursor{};
  math::Color selection{};
  /// xterm 前 16 色：0-7 标准、8-15 亮色。加粗（`SGR 1`）会把 0-7 提升到 8-15。
  std::array<math::Color, 16> ansi{};
};

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
  [[nodiscard]] auto terminal() const noexcept -> const TerminalPalette& { return terminal_; }
  [[nodiscard]] auto terminal() noexcept -> TerminalPalette& { return terminal_; }

  [[nodiscard]] auto font_family() const -> const std::string& { return font_family_; }
  void set_font_family(std::string family) { font_family_ = std::move(family); }

  /// 主题名（`light`/`dark` 等内置名，或自定义主题自报的名字）。
  /// 只作标识与上报用：不改色、不影响渲染。
  [[nodiscard]] auto name() const -> const std::string& { return name_; }
  void set_name(std::string name) { name_ = std::move(name); }

  /// 基础间距的倍数（4px 栅格）。
  [[nodiscard]] auto space(float steps) const noexcept -> float { return 4.0f * steps; }

 private:
  ThemeMode mode_{ThemeMode::Light};
  Palette colors_{};
  SyntaxPalette syntax_{};
  TerminalPalette terminal_{};
  Metrics metrics_{};
  std::string font_family_{"Noto Sans CJK SC"};
  std::string name_{"light"};
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

/// 阴影 token（随主题取色，并受 `Metrics::shadow_strength/shadow_spread` 调节）。
[[nodiscard]] auto shadow_sm(const Theme& theme) -> Shadow;
[[nodiscard]] auto shadow_md(const Theme& theme) -> Shadow;
[[nodiscard]] auto shadow_lg(const Theme& theme) -> Shadow;

/// **令牌名表**（`"bg"` `"surface"` `"text_muted"` …）——自定义主题与控制通道共用同一份。
///
/// 为什么把名字集中在这里而不是各写各的字符串：
/// 主题文件的键、控制通道 `theme` 方法的 `set` 键、以及错误消息里的提示
/// 必须是**同一套拼写**。分散写时改一处漏一处的结果是「文件里写对了、通道不认」，
/// 而两边都只报「未知 token」。
///
/// 返回的向量顺序即设计文档里的叙述顺序；每项是 `std::string_view`（指向静态存储）。
[[nodiscard]] auto palette_token_names() -> const std::vector<std::string_view>&;
/// 按名取色；未知名返回 `nullopt`。
[[nodiscard]] auto palette_token(const Palette& palette, std::string_view name)
    -> std::optional<math::Color>;
/// 按名写色；未知名返回 `false`（且不碰 `palette`）。
[[nodiscard]] auto set_palette_token(Palette& palette, std::string_view name, math::Color color)
    -> bool;

/// 尺度令牌名（`"radius_md"` `"font_base"` `"shadow_strength"` …），同上。
[[nodiscard]] auto metric_token_names() -> const std::vector<std::string_view>&;
/// 按名取尺度值；未知名返回 `nullopt`。
[[nodiscard]] auto metric_token(const Metrics& metrics, std::string_view name)
    -> std::optional<float>;
/// 按名写尺度值；未知名返回 `false`。`font_scale` 是**派生量**（记录缩放倍数），
/// 直接写它不会重算字号阶梯——要整体缩放字号用 `Metrics::scale_fonts`。
[[nodiscard]] auto set_metric_token(Metrics& metrics, std::string_view name, float value) -> bool;

}  // namespace st::ui

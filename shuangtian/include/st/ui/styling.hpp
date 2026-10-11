#pragma once

/// 样式系统（CSS 级主题化，`DESIGN.md` §5.x）：**组件一行声明外观，主题切换自动跟随**。
///
/// ## 分层（类比 CSS）
///
/// | 层 | 对应 CSS 概念 | 本文件 |
/// |---|---|---|
/// | `Theme`（Palette/Metrics） | `:root` 变量表 | theme.hpp（既有） |
/// | `Appearance` | 一条规则（`.btn.primary { … }`） | 本文件 |
/// | `Element::apply_appearance()` | 规则落到元素 | element.hpp（接线） |
/// | `BoxProps::variant/elevation/…` | `class="btn primary"` | dsl.hpp（直通） |
///
/// ## 设计动机（为什么在 Palette 之上再有一层）
///
/// 旧路径：每个组件在自己的 `apply_theme` 里读 Palette 拼「背景 + 描边 + 圆角 +
/// 阴影 + 字号 + 高度」——40+ 处各自为政，同一个「主按钮」在不同组件里长得不一样，
/// 改主题要逐个找。新路径：组件声明**语义**（variant/elevation/size/radius/density），
/// `Tokens` 统一解析成具体值——一致性与换肤都只在一处发生。
///
/// ## 联动规则（都是跨 token 的组合，散在组件里就是不一致的来源）
///
/// - `elevation`（0-3）：表面阶梯（Base→Raised）+ 阴影档（none→sm→md→lg）+ 顶部内高光（≥1 才有）
/// - `size`（sm/md/lg）：控件高度 + 字号档 + 图标盒径三联动
/// - `density`：间距倍率（紧凑 0.85 / 常规 1.0 / 舒适 1.15），作用于 padding/gap
///
/// 用法（命令式）：
/// ```cpp
/// auto& btn = custom<Button>(c, [](Button& b) {
///   b.apply_appearance({.variant = Variant::Primary, .size = Size::Large});
/// });
/// ```
/// 用法（声明式）：`button(c, "保存", on_click, {.variant = Variant::Primary})`。

#include <cstdint>
#include <optional>

#include "st/ui/theme.hpp"

namespace st::ui {

class Element;   // 前向声明：Appearance 不需要完整类型（apply_appearance 的实现才要）

/// 组件变体（视觉语气；≈ CSS 的组件级 class）。
enum class Variant : std::uint8_t {
  Plain,      ///< 无底无框（纯内容；列表行/标签文字）
  Neutral,    ///< 中性表面（默认按钮/卡片内容区）
  Primary,    ///< 品牌主色实底 + on_primary 前景
  Accent,     ///< 强调色描边（次级强调；实底太吵时用）
  Success,    ///< 成功语义（提交完成态/健康指示）
  Warning,    ///< 警告语义（谨慎操作提示）
  Danger,     ///< 危险语义（删除/不可逆操作的确认按钮）
  Ghost,      ///< 幽灵（透明底 + hover 才显色；工具栏/行内动作）
};

/// 抬升档位（阴影 + 表面 + 高光联动）。
enum class Elevation : std::uint8_t {
  Flat = 0,    ///< 贴底（与父同面；列表/表单行）
  Raised = 1,  ///< 卡片（sm 阴影；可点内容块）
  Overlay = 2, ///< 弹层（md 阴影；菜单/下拉）
  Modal = 3,   ///< 模态（lg 阴影 + 抬升面；对话框）
};

/// 圆角档（跟随主题 token，不写死数值）。
enum class RadiusMode : std::uint8_t { None, SM, MD, LG, XL, Pill };

/// 控件尺寸档（高度 + 字号 + 图标三联动）。
enum class ControlSize : std::uint8_t { Small, Medium, Large };

/// 密度（间距缩放；只作用 padding/gap，不动字号与控件高度）。
enum class Density : std::uint8_t { Compact, Regular, Comfortable };

/// 一条外观规则（全部字段可选——缺省 =「不动」，显式设了才覆盖）。
///
/// 与 CSS 的差别：没有级联与选择器——规则只作用于**本元素**（组合由容器显式做）。
/// 这是刻意的：级联的覆盖顺序是 GUI 样式混乱的头号来源，语义显式化更可查。
struct Appearance {
  std::optional<Variant> variant{};
  std::optional<Elevation> elevation{};
  std::optional<RadiusMode> radius{};
  std::optional<ControlSize> size{};
  std::optional<Density> density{};
  /// 文本色调（设了则覆盖 tone 解析）。
  std::optional<Tone> text_tone{};
};

/// 主题解析快照：把 `Theme` 的 token 组装成 Appearance 需要的**具体值**。
///
/// 为什么是个类而不是散函数：解析结果在一次布局里被几十个组件共享——
/// 快照一次、处处引用（换主题时由 UiRoot 重解析一次）。组件**只读**它，
/// 不再自己读 Palette 拼参数。
class Tokens {
 public:
  explicit Tokens(const Theme& theme);

  // —— 变体 → 颜色组 ——
  struct VariantColors {
    math::Color background{};
    math::Color border{};
    math::Color text{};
    /// hover/pressed 态的背景（无态交互的组件忽略）。
    math::Color background_hover{};
    math::Color background_pressed{};
  };
  [[nodiscard]] auto variant_colors(Variant variant) const -> VariantColors;

  // —— 抬升 → 表面/阴影 ——
  [[nodiscard]] auto elevation_surface(Elevation level) const -> math::Color;
  [[nodiscard]] auto elevation_shadow(Elevation level) const -> Shadow;
  /// 抬升 ≥1 时才有顶部内高光（「玻璃上缘」）；返回透明色表示无。
  [[nodiscard]] auto elevation_highlight(Elevation level) const -> math::Color;

  // —— 档位 → 数值 ——
  [[nodiscard]] auto radius_value(RadiusMode mode) const -> float;
  /// 控件高度 / 字号 / 图标盒径（三联动，单一来源）。
  [[nodiscard]] auto control_height(ControlSize size) const -> float;
  [[nodiscard]] auto font_size(ControlSize size) const -> float;
  [[nodiscard]] auto icon_size(ControlSize size) const -> float;
  /// 密度倍率（作用在 padding/gap 上）。
  [[nodiscard]] auto density_factor(Density density) const -> float;

  [[nodiscard]] auto palette() const -> const Palette& { return palette_; }
  [[nodiscard]] auto metrics() const -> const Metrics& { return metrics_; }

 private:
  /// 快照本体：构造时**拷贝** Palette/Metrics（不存 Theme 指针——
  /// Tokens 常比构造它的 Theme 活得久（作为返回值/成员），存指针必悬垂；
  /// 快照的意义就是「持有解析结果」而非「引用来源」）。拷贝代价 ~400B，可忽略。
  Palette palette_{};
  Metrics metrics_{};
  Shadow shadow_sm_{};
  Shadow shadow_md_{};
  Shadow shadow_lg_{};
};

/// 把一条 `Appearance` 规则落到元素的 `style_`（不覆写的字段保持元素现状）。
///
/// 这是**唯一**的落点函数：命令式（`element.apply_appearance`）与声明式
/// （`BoxProps` 直通）最终都到这——两套入口不可能长歪成两种效果。
/// 返回是否改变了任何样式值（调用方据此决定 `mark_dirty`）。
[[nodiscard]] auto apply_appearance(Element& element, const Tokens& tokens,
                                    const Appearance& appearance) -> bool;

/// 变体的稳定短名（控制通道/视觉树上报；与 `tone_name` 同一约定）。
[[nodiscard]] constexpr auto variant_name(Variant variant) noexcept -> std::string_view {
  switch (variant) {
    case Variant::Plain: return "plain";
    case Variant::Neutral: return "neutral";
    case Variant::Primary: return "primary";
    case Variant::Accent: return "accent";
    case Variant::Success: return "success";
    case Variant::Warning: return "warning";
    case Variant::Danger: return "danger";
    case Variant::Ghost: return "ghost";
  }
  return "neutral";
}

}  // namespace st::ui

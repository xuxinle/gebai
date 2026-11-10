#pragma once

/// @file button.hpp
/// @brief `Button`：按钮（含变体与尺寸；支持前导图标与点击回调）。。
///
/// 从 `basic.hpp` 拆出（**一组件一头**）。拆分规则与理由见 `st/ui/components/README.md`：
/// 一个头一个组件 → 依赖关系写在组件自己身上 → include 谁就只有谁进产物。
/// `basic.hpp` 仍保留**聚合**（只 include 各子头），既有引用不必一次改完。

#include <functional>
#include <string>
#include <string_view>
#include "st/ui/component_registry.hpp"
#include "st/ui/components/icon_view.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 按钮（含变体与尺寸；支持前导图标与点击回调）。
///
/// ## 为何用 `ui::Variant` 而不是自己一套（2026-11-10，词表统一）
///
/// 曾经这里有一个 `Button::Variant`（Primary/Secondary/Ghost/Soft/Danger），
/// 与 `st::ui::Variant`（Plain/Neutral/Primary/Accent/Success/Warning/Danger/Ghost）
/// **同名不同表**。两份词表并存的直接后果（实测相期「全部忽略」蓝底）：
///
/// XML 写 `variant="ghost"` → 走样式系统落进 `style.background`（透明，对）
/// 同时 `variant_` 成员**仍是构造默认的 Primary** → `apply_theme` 把主色渐变
/// 又铺了一遍 → `paint_box`「渐变优先于纯色」→ 屏幕上是一块主色底，
/// 而 `get_property("variant")` 报 `primary`（它读的是成员，不是声明）。
///
/// 两份词表还各自残缺：`Soft` 只有 Button 有、`Plain/Neutral/Accent/Success/Warning`
/// 只有样式系统有。**任一侧都不可能完整**——这就是缺陷的宿主。
/// 词表只能有一份。统一到 `ui::Variant` 后，`Soft` 补进词表、
/// `Secondary` 合并为 `Neutral`（两者语义等价：surface 底 + border 描边）。
class Button : public Element {
 public:
  /// 视觉语气（**就是 `st::ui::Variant`**，不是按钮私有的一套）。
  using Variant = st::ui::Variant;
  /// 内容横向对齐（与布局侧的 `st::ui::Align` 同名同义，但作为按钮自己的接口
  /// 暴露：调用方写 `Button::Align::Start` 才读得出“这是按钮的内容对齐”）。
  ///
  /// 为何需要它（2026-10-11，实测）：默认**居中**对单个按钮是对的，但
  /// **列表行**用的也是 Button（`Ghost` + `grow` 铺满一行）。居中的行文字在
  /// 窄栏里会被推到中间，几十行叠在一起左缘参差不齐——列表是拿来“扫”的，
  /// 扫的是左缘。实测截图：Git 日志三行各自居中，看着像三个独立按钮
  /// 而不是一个列表。
  ///
  /// 默认 `Center` 保证既有界面零变化；列表行显式写 `Start`。
  using Align = st::ui::Align;
  enum class Size : std::uint8_t { Small, Medium, Large };

  explicit Button(std::string label, Variant variant = Variant::Primary,
                  Size size = Size::Medium);
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Button"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Button; }

  void set_label(std::string label);
  [[nodiscard]] auto label() const -> const std::string& { return label_; }
  void set_icon(std::string icon);
  void set_variant(Variant variant);
  void set_size(Size size);
  void set_tone(Tone tone);
  /// 内容横向对齐（默认居中；列表行传 `Start`）。
  void set_align(Align align) noexcept { content_align_ = align; }
  [[nodiscard]] auto align() const noexcept -> Align { return content_align_; }
  /// 超长时用**中段省略**（保留头尾）——路径/长标识专用。
  ///
  /// 尾部省略用在路径上会把文件名砍掉：目录 `packages/agents/src/...` 先占满宽度，
  /// 而用户看的恰恰是文件名（实测：“目录还在、文件名没了”）。
  void set_ellipsize_middle(bool value) noexcept {
    if (ellipsize_middle_ == value) return;
    ellipsize_middle_ = value;
    mark_dirty();
  }
  [[nodiscard]] auto ellipsize_middle() const noexcept -> bool { return ellipsize_middle_; }
  [[nodiscard]] auto variant() const noexcept -> Variant { return variant_; }

  /// **图标盒径**（逻辑 px）——与 `apply_theme` 同一入口写入，供测试/宿主读取。
  ///
  /// 为何是一等读取面：图标尺寸是观感量，需要一个**被测对象自报的值**
  /// （`perceptual_changes.md` §14：不要信旁路探针）。测试直接读它，
  /// 就不必在测试里复刻一遍公式（复刻的公式一旦与实现脱钩就成假护栏）。
  [[nodiscard]] auto icon_box() const noexcept -> float { return icon_size_; }

  /// 当前图标在 `icon_box()` 盒里的**实际墨迹尺寸**（逻辑 px）。
  ///
  /// 排版（`measure`）与绘制（`paint_content`）都必须用它而不是盒宽：盒里
  /// 有透明留白，按盒宽排版会让内容重心偏向文字一侧（见 `measure` 里的推导）。
  [[nodiscard]] auto icon_ink_size() const -> math::Size {
    return Icon::ink_size(icon_, math::Size{icon_size_, icon_size_});
  }

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  void activate() override;
  [[nodiscard]] auto semantics_text() const -> std::string override { return label_; }
  [[nodiscard]] auto semantics_flags() const -> SemanticsFlags override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

  std::function<void()> on_click{};
  /// 图标在左（默认）或右。
  bool icon_leading{true};

 private:
  std::string label_{};
  std::string icon_{};
  Variant variant_{Variant::Primary};
  Size size_{Size::Medium};
  Tone tone_{Tone::Default};
  /// 图标盒径（由 `apply_theme` 按 Size 档从 `Metrics::icon_size*` 取）。
  float icon_size_{22.0f};
  /// 内容横向对齐（**与 `style_.align_items` 区分**：那个管的是“图标与文字在
  /// 交叉轴上如何对齐”，本字段管的是“整块内容在按钮里靠哪边”）。
  Align content_align_{Align::Center};
  /// 超长时中段省略（见 `set_ellipsize_middle`；默认尾部省略）。
  bool ellipsize_middle_{false};
};

}  // namespace st::ui

ST_UI_COMPONENT_WITH(::st::ui::Button, "Button", std::make_unique<::st::ui::Button>(""))

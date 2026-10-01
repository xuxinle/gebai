#pragma once

/// 组件库 A · 标签页（`DESIGN.md` §4.5）：`Tabs`（横向标签条）。
///
/// 依赖纪律：文本经 `context.text`（空则退化 `NullTextPort`）；颜色/间距/字号一律取
/// `context.theme` 的 token（`apply_theme` 时落进 `style_`），组件内不硬编码颜色。
/// 标签项由本组件自绘（不产生子 Element），命中区与指示条锚点经 `tab_rect` 对外暴露。
///
/// 视觉：选中项 `text` 色 + 下方 2px `primary` 指示条（按 `metrics.motion_normal` 滑动），
/// 未选中 `text_muted`，hover 背景 `surface_alt`，disabled 统一降到 `text_faint`。
/// 可关闭项 hover 时在右侧绘制 ×（canvas 线条自绘），修改项标签旁绘制小圆点（`text_muted`）。
///
/// 交互：点击标签切换；点击 × 触发 `on_close(index)`（不直接删除——由调用方决定）；
/// `ArrowLeft`/`ArrowRight` 循环切换、`Home`/`End` 到首/末；`Enter`/`Space` 重新确认当前项；
/// 切换经 `on_change(active_index)` 通知。标签总宽超出组件宽度时显示左右箭头（点击滚动）
/// 并响应滚轮横向滚动；`active` 项自动滚动到可见。
///
/// 数据刷新用 `sync_tabs`（按 `key` 对齐复用，同 key 保持活动态；语义同 `List::sync_items`）；
/// 只改展示元数据时用 `set_tab_meta`。`set_tabs`/`add_tab` 是纯标签 API（无 key，
/// key 自动取 label 值），行为与历史版本完全一致。
///
/// 控制通道属性面：`active`（序号或标签名，可写）、`options`（`|` 分隔的标签清单，可写）、
/// `label`（当前标签名，可写=按名选中）、`scroll`（横向滚动偏移，可读写）。

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

class Tabs : public Element {
 public:
  Tabs();

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Tabs"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Tab; }

  /// 结构化标签项（`sync_tabs` 用）：`key` 是业务身份（同 key 视为同一标签）。
  struct Tab {
    std::string key{};
    std::string label{};
    bool modified{false};
    bool closable{false};
  };

  /// 整表替换（活动项夹取到合法范围，越界时回到首项）。
  void set_tabs(std::vector<std::string> labels);
  void add_tab(std::string label);
  void clear_tabs();

  /// 按 key 同步标签数据：**保留同 key 项的次序、元数据与活动态**。
  ///
  /// - 新增的 key → 按数据顺序插入（追加在末尾）；
  /// - 保留的 key → 更新 label/modified/closable（沿用本组件内的位置）；
  /// - 消失的 key → 移除该标签；
  /// - 活动态跟随**活动项的 key** 走：活动 key 保留则索引随位置更新，
  ///   活动 key 消失则落到离它最近的位置（不派发 `on_change`，数据刷新不是用户选择）。
  /// 空表合法（清空全部标签）。未显式给 key 的项按 label 值对齐。
  void sync_tabs(const std::vector<Tab>& tabs);
  /// 更新第 `index` 项的展示元数据（越界忽略）。
  void set_tab_meta(std::size_t index, bool modified, bool closable);
  /// 第 `index` 项是否带修改点/可关闭（越界为 false）。
  [[nodiscard]] auto tab_modified(std::size_t index) const -> bool;
  [[nodiscard]] auto tab_closable(std::size_t index) const -> bool;
  /// 活动项的业务 key（无标签为空串）。
  [[nodiscard]] auto active_key() const -> std::string_view;
  /// 按业务 key 定位（不存在返回 `std::nullopt`）。
  [[nodiscard]] auto index_of_key(std::string_view key) const -> std::optional<std::size_t>;

  [[nodiscard]] auto tab_count() const noexcept -> std::size_t { return labels_.size(); }
  /// 越界返回空视图。
  [[nodiscard]] auto tab_label(std::size_t index) const -> std::string_view;

  /// 设置活动标签（越界自动夹取；`notify` 为 true 时触发 `on_change`）。
  void set_active(std::size_t index, bool notify = false);
  [[nodiscard]] auto active_index() const noexcept -> std::size_t { return active_; }
  [[nodiscard]] auto active_label() const -> std::string_view;

  /// 标签项矩形（命中/指示条锚定/测试用；未布局或越界时为空矩形）。
  [[nodiscard]] auto tab_rect(std::size_t index) const -> math::Rect;
  /// 全部标签所需宽度（布局期缓存；未布局时为 0）。
  [[nodiscard]] auto tabs_width() const noexcept -> float { return total_width_; }

  // —— 横向溢出滚动 ——
  /// 当前滚动偏移（px，非负；未溢出时恒 0）。
  [[nodiscard]] auto scroll_offset() const noexcept -> float { return scroll_offset_; }
  /// 设置滚动偏移（自动夹取到 `[0, max_scroll]`；重排后超出会再夹取）。
  void set_scroll_offset(float offset);
  /// 最大可滚动距离（`tabs_width - bounds.width`，不足为 0）。
  [[nodiscard]] auto max_scroll() const noexcept -> float;
  /// × 关闭区矩形（命中优先级高于标签本体；非 closable 或不可见项为空矩形）。
  [[nodiscard]] auto close_rect(std::size_t index) const -> math::Rect;
  /// 溢出箭头命中区（不溢出为空矩形；测试与自动化定位用）。
  [[nodiscard]] auto overflow_arrow_rect(bool right) const -> math::Rect;

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  /// 排布后夹取滚动偏移（容器变窄时旧偏移可能超出新的 max_scroll）。
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  void activate() override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto semantics_flags() const -> SemanticsFlags override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

  /// 活动标签变化回调。
  std::function<void(std::size_t)> on_change{};
  /// 关闭请求回调（点击 ×；组件不删除标签，由调用方决定）。
  std::function<void(std::size_t)> on_close{};

 private:
  /// 重算标签项宽度缓存（布局期调用；文本度量依赖 `context.text`）。
  void rebuild_widths(const RenderContext& context) const;
  [[nodiscard]] auto tab_index_at(math::Point point) const -> std::optional<std::size_t>;
  /// 指示条当前位置（180ms 滑动；静态帧直接到位）。
  [[nodiscard]] auto resolve_indicator(const RenderContext& context, math::Rect target) const
      -> math::Rect;
  /// 按标签名定位（不存在返回 nullopt）。
  [[nodiscard]] auto index_of_label(std::string_view label) const -> std::optional<std::size_t>;
  /// 滚动到让第 `index` 个标签尽量可见（越界忽略；布局后有效）。
  void scroll_to_visible(std::size_t index);
  /// 溢出时两侧箭头区（不溢出为空矩形）。
  [[nodiscard]] auto arrow_left_rect() const -> math::Rect;
  [[nodiscard]] auto arrow_right_rect() const -> math::Rect;

  std::vector<std::string> labels_{};
  std::vector<std::string> keys_{};      ///< 与 labels_ 同序的业务 key（空 = 用 label 值）
  std::vector<char> modified_{};         ///< 修改点标记（与 labels_ 同序）
  std::vector<char> closable_{};         ///< 可关闭标记（与 labels_ 同序）
  mutable std::vector<float> widths_{};  ///< 每个标签项宽（含内边距），布局期缓存
  mutable float total_width_{0.0f};      ///< 标签条总宽（含项间距）
  std::size_t active_{0};
  float gap_{0.0f};         ///< 标签项间距（`metrics.space_xs`）
  int hover_index_{-1};     ///< 指针悬停项（-1 无）
  float scroll_offset_{0.0f};  ///< 横向滚动偏移（溢出时 > 0）

  // 指示条动画状态（绘制期推进）
  mutable float indicator_x_{0.0f};
  mutable float indicator_width_{0.0f};
  mutable float indicator_from_x_{0.0f};
  mutable float indicator_from_width_{0.0f};
  mutable double indicator_start_{-1.0};
  mutable double last_time_{0.0};
};

}  // namespace st::ui

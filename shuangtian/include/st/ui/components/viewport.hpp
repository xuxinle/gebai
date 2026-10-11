#pragma once

/// **二维缩放/平移视口**（`Viewport`）：把一块内容按缩放与平移映射到视口里，
/// 超出部分裁剪，可选棋盘格透明底。
///
/// ## 与 `ScrollView` 的分工（两者不是一回事）
///
/// `ScrollView` 是**一维滚动**：内容尺寸不变，只平移纵轴，语义是「内容比视口长」。
/// 本组件是**二维缩放平移**：内容被 `zoom` 缩放、被 `pan` 平移，语义是「看大图」——
/// 缩放以光标为锚点、平移跟手、空白处显棋盘格。把两者合并会让每个用例都带上一堆
/// 用不上的参数（而这正是「一个通用件谁都嫌重」的来源）。
///
/// ## 坐标系（本组件唯一的对外契约）
///
/// - **视口坐标**：相对本组件 `bounds_` 左上角（逻辑像素）。
/// - **内容坐标**：内容自己的坐标（不受 zoom/pan 影响）。
/// - 映射：`view = (content - pan) * zoom`，因此 `content = view / zoom + pan`。
///
/// 宿主做「光标处像素坐标回显」时直接用 `to_content()`——**不要自己拼这个式子**：
/// 拼错的表现是「回显的数与画面差半个屏幕」，而它看起来只是「数字不对」。
///
/// ## 为何缩放要以锚点为心（`zoom_at`）
///
/// 滚轮缩放的期望是「光标下的那块内容不动」。若只改 `zoom` 不调 `pan`，
/// 整幅图会以左上角为心缩放——用户得一边滚一边拖才能看住目标，体验上就是「缩放不好用」。
/// 这不是润色项，而是缩放功能的正确性判据（单测的核心断言就是它）。

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

class Viewport : public Element {
 public:
  /// 缩放范围。下限取 5%（看整张大图），上限 3200%（逐个像素看细节）——
  /// 与常见图像编辑器同量级，且避免极端值下的浮点精度问题（`zoom` 参与坐标换算）。
  static constexpr float kMinZoom{0.05f};
  static constexpr float kMaxZoom{32.0f};
  /// 滚轮每一格的缩放倍率（乘性：`zoom *= kZoomStep` 或 `/ kZoomStep`）。
  /// 用**乘性**而不是加性：加性步长在 0.1 倍时一步翻倍、在 16 倍时几乎不动。
  static constexpr float kZoomStep{1.25f};

  Viewport();

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Viewport"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Panel; }

  /// 内容槽（**一个**子元素，与 `ScrollView` 同约定：位置对齐时计入内容子元素）。
  ///
  /// 为何不支持多子元素：二维缩放平移下「多个子元素各自多大、锚点在哪」没有自然答案
  /// （`ScrollView` 能按列堆叠是因为一维）。要放多个就在内容里自己放一个容器。
  void set_content(std::unique_ptr<Element> content);
  [[nodiscard]] auto content() noexcept -> Element* {
    return children_.empty() ? nullptr : children_.front().get();
  }
  [[nodiscard]] auto content() const noexcept -> const Element* {
    return children_.empty() ? nullptr : children_.front().get();
  }
  [[nodiscard]] auto content_child_count() const noexcept -> std::size_t override {
    return children_.size();
  }

  // —— 缩放 / 平移 ——
  void set_zoom(float zoom);
  [[nodiscard]] auto zoom() const noexcept -> float { return zoom_; }
  /// **以视口内某点为锚**缩放：该点的内容坐标在缩放前后不变。
  ///
  /// ⚠ 到边界（`kMinZoom`/`kMaxZoom`）后**不得再推 pan**：夹取之后仍按未夹取的
  /// 倍率推 pan，会让 pan 在到达极限后继续漂移——表现是「缩放到头了，图还在慢慢跑」。
  void zoom_at(math::Point view_point, float factor);
  /// 便捷：按 `kZoomStep` 滚一格（`in = true` 放大）；锚点通常给鼠标位置。
  void zoom_step_at(math::Point view_point, bool in);

  void set_pan(math::Point pan) { pan_ = pan; mark_layout_dirty(); }
  [[nodiscard]] auto pan() const noexcept -> math::Point { return pan_; }

  /// 把内容**居中并适配**到视口（`fit`）：`zoom` 取「内容刚好铺满视口」的值（不放大超过 1）。
  /// 需要知道内容尺寸，所以只在 `arrange` 之后调用才有意义。
  void fit_to_view();

  // —— 坐标换算（宿主可用的唯一契约，见文件头）——
  [[nodiscard]] auto to_content(math::Point view_point) const -> math::Point;
  [[nodiscard]] auto to_view(math::Point content_point) const -> math::Point;

  // —— 棋盘格透明底 ——
  /// 开关（默认**开**：看大图时「这里没有像素」与「这里是白色像素」必须能分辨）。
  void set_checkerboard(bool enabled) { checkerboard_ = enabled; mark_dirty(); }
  [[nodiscard]] auto checkerboard() const noexcept -> bool { return checkerboard_; }
  /// 格子边长（逻辑像素）。默认取主题的 `metrics().space_sm`。
  void set_checker_size(float size) { checker_size_ = size; mark_dirty(); }
  [[nodiscard]] auto checker_size() const noexcept -> float { return checker_size_; }

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  /// 棋盘格画在**内容之下**（见实现里的说明）；不覆写 `paint` 是为了不绕开
  /// `Element::paint` 的裁剪与子元素绘制。
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;

  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action,
                                   std::string_view argument) -> bool override;

  /// 缩放或平移变化时回调（宿主用它做「坐标回显」与「保存视图状态」）。
  std::function<void()> on_view_changed{};

 private:
  /// 内容在内容坐标系里的尺寸（来自 `content()->measured_size()`）。
  [[nodiscard]] auto content_size() const -> math::Size;
  /// 把 `zoom_` 夹到 `[kMinZoom, kMaxZoom]`；返回夹取后的值。
  [[nodiscard]] static auto clamp_zoom(float zoom) -> float;

  float zoom_{1.0f};
  math::Point pan_{};          ///< 内容坐标里「视口左上角」对应的点
  bool checkerboard_{true};
  float checker_size_{0.0f};   ///< 0 = 用主题令牌（`apply_theme` 时填）
  math::Color checker_light_{};
  math::Color checker_dark_{};
  math::Color border_{};
  /// 中键/空格拖拽平移的进行态（拖动归属由 `UiRoot` 按 `pressed_` 保证）。
  bool dragging_{false};
  math::Point drag_origin_view_{};
  math::Point drag_origin_pan_{};
  /// 滚轮缩放锚点：最后一次鼠标位置（`on_event` 里记，滚轮事件本身也带位置）。
  math::Point last_pointer_{};
};

}  // namespace st::ui

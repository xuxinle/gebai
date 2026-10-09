#pragma once

/// 组件库 · 布局（`DESIGN.md` §4.5.1）：`SplitView`（可拖拽分栏）。
///
/// 由来（DESIGN §8.1.1 反推的框架缺口）：「SplitHandle 曾是示例级的自绘组件，
/// 重写后编辑器侧栏/主区/面板仍无拖拽分栏——它该从示例级自绘升为框架组件」。
/// 因此本组件把「两个面板 + 中间可拖手柄」的共同语义收进框架：
/// 视觉（发丝分隔线 + 悬停主色 + 拖拽加粗）与交互（悬停反馈、拖拽改比例、
/// 键盘步进、双击归位）全部走主题令牌，应用不再各写一套。
///
/// 语义模型：**两子 + 一个比例**。`set_first/set_second` 提供两个面板（各自容器）；
/// `ratio` 是第一个面板占主轴的比例（夹取到 `[min_ratio, 1-min_ratio]`）。
/// 布局方向由 `orientation` 决定（`Horizontal` = 左右分栏、`Vertical` = 上下分栏）。
///
/// 与 Flex 的关系：SplitView **不是** Flex 容器——它按比例分配，不做 shrink-to-fit；
/// 子面板宽度由比例与总宽决定（应用侧不必也不该给子面板写死宽度）。
///
/// 控制通道属性面：`ratio`（浮点字符串，写回即夹取并重排）、`min_ratio`、
/// `orientation`（`horizontal`/`vertical`）；动作面：`step_forward`/`step_backward`
/// （按键盘步长挪）、`reset`（回默认比例）。拖拽走 `MouseDown`→`MouseMove`×n→`MouseUp`，
/// 与 ScrollBar 拖滑块同一套事件契约（`UiRoot` 的拖拽归属保证拖出手柄也不断）。

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 可拖拽分栏：两个面板 + 中间手柄，拖拽改变两者比例。
///
/// 手柄视觉：常态 1px 发丝线（`border`）、悬停 2px（`border_strong`）、
/// 拖拽 2px `primary`（与滚动条拇指同一套色阶）；命中区比视觉线宽（见 `kGrabWidth`），
/// 保证「看起来一条线、拖起来不费劲」。
class SplitView : public Element {
 public:
  /// 分栏方向：左右（默认）或上下。
  enum class Orientation : std::uint8_t { Horizontal, Vertical };

  SplitView();
  explicit SplitView(Orientation orientation);

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "SplitView"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Panel; }

  /// 设置两个面板（接管所有权；已有面板被替换）。
  ///
  /// **面板就是子元素**（`children_[0]` / `children_[1]`）——这是刻意的：
  /// 框架的 id 分配、绘制、命中测试、语义/视觉树全部按 `children_` 遍历；
  /// 自持 `unique_ptr` 成员会让面板"在树上消失"（实测：迁入后 `find #sidebar` 报 not_found）。
  /// 两侧都允许为空——只有一侧时布局优雅退化（单面板占满）。
  void set_first(std::unique_ptr<Element> panel);
  void set_second(std::unique_ptr<Element> panel);
  [[nodiscard]] auto first() noexcept -> Element* { return child_count() > 0 ? child_at(0) : nullptr; }
  [[nodiscard]] auto second() noexcept -> Element* { return child_count() > 1 ? child_at(1) : nullptr; }

  /// 分栏方向（`Horizontal` = 左右、`Vertical` = 上下）。
  void set_orientation(Orientation orientation);
  [[nodiscard]] auto orientation() const noexcept -> Orientation { return orientation_; }

  /// 第一面板占比（夹取到 `[min_ratio, 1 - min_ratio]`）。
  void set_ratio(float ratio, bool notify = false);
  [[nodiscard]] auto ratio() const noexcept -> float { return ratio_; }

  /// 最小占比（两侧对称；夹取到 `(0, 0.5)`）。
  void set_min_ratio(float value);
  [[nodiscard]] auto min_ratio() const noexcept -> float { return min_ratio_; }

  /// 键盘步长（夹取到 `(0, 0.5)`）。
  void set_step(float step);
  [[nodiscard]] auto step() const noexcept -> float { return step_; }

  /// 手柄宽/高（命中区尺寸，默认 8px；视觉线宽见 paint）。
  void set_handle_size(float size);
  [[nodiscard]] auto handle_size() const noexcept -> float { return handle_size_; }

  // —— 面板显隐（保留子元素，只退出布局与交互）——
  //
  // 与"从条件分支里抽掉一侧"的区别：抽掉会**销毁**子元素（声明式末尾裁剪），
  // 而这里只是**藏**——子元素带着全部状态留在树上，重新显示时原样回来。
  // 典型场景：底部终端面板收起（会话/回看保留）、侧栏折叠。
  // 隐藏侧退出布局：另一侧单面板退化占满（同"只有一侧"的几何），手柄随之消失。
  void set_second_hidden(bool hidden);
  [[nodiscard]] auto second_hidden() const noexcept -> bool { return second_hidden_; }
  void set_first_hidden(bool hidden);
  [[nodiscard]] auto first_hidden() const noexcept -> bool { return first_hidden_; }

  /// 比例变化回调（拖拽过程中逐次触发，与 Slider 同口径）。
  std::function<void(float)> on_change{};

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  void activate() override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto semantics_flags() const -> SemanticsFlags override;
  [[nodiscard]] auto get_property(std::string_view name) const
      -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

  /// 手柄矩形（**命中区**，逻辑像素）——主轴按 `ratio` 定位、副轴贴满。
  /// 公开给测试与自动化（断言的手柄位置/发器比例验证需要它；与绘制共用同一几何）。
  [[nodiscard]] auto handle_rect() const -> math::Rect;

 private:
  /// 主轴总长（方向相关）。
  [[nodiscard]] auto main_extent() const noexcept -> float;
  /// 把指针坐标（主轴方向）映射为比例。
  [[nodiscard]] auto ratio_at(float pointer_main) const noexcept -> float;
  /// 按比例给两侧算出矩形（同一份几何，arrange 与拖拽共用）。
  void layout_children(const RenderContext& context);
  /// 夹取比例到合法区间。
  [[nodiscard]] auto clamp_ratio(float value) const noexcept -> float;

  Orientation orientation_{Orientation::Horizontal};
  float ratio_{0.5f};
  float min_ratio_{0.15f};
  float step_{0.05f};
  float handle_size_{8.0f};
  bool dragging_{false};
  bool first_hidden_{false};
  bool second_hidden_{false};
  /// 拖拽起点的「指针主轴坐标 - 手柄中心」：拖拽时保持抓取点相对位置（不跳变）。
  float grab_offset_{0.0f};
};

}  // namespace st::ui

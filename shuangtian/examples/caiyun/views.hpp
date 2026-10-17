#pragma once

/// 裁云（Nimbus）的**时间轴适配器**与**预览监视器**。
///
/// ## 时间轴已换内置件（2026-10-16，caiyun 拆解步骤 6）
///
/// 原先这里是 450 行手绘时间轴（标尺/轨道/片段/命中/拖拽/吸附全在类里，只靠
/// e2e 截图断言）。55 轮曾定案「整件换内置不做」——前提是「需造 Model→Project
/// 反向同步桥，复杂度不低于手写面」。**该前提已被拆解计划推翻**：S1 把桥做进了
/// 组件接口（`st::ui::TimelineView::EditDelegate`，21 例 view 测试钉住），编辑
/// 语义经 delegate 走 Project 同源，桥的复杂度从应用层移到框架层一次实现
///（docs/caiyun_timelineview_refactor_plan.md §③「判据反转」）。于是本类瘦身为
/// **薄适配器**：
/// - 内部持 `st::ui::TimelineView`（组合转发五大生命周期）；
/// - **绘制/命中/几何**全部来自内置件（投影：`sync_from_project` 每次交互前
///   从 Project 重建 Model）；
/// - **编辑全部走 Project**：`EditDelegate` 的 move/trim 转
///   `Project::move_clip/trim`（波纹/重叠/时长钳制等领域规则在 Project），
///   snap 候选也按 Project 真值平移；播放头拖动/seek 转 `Project::playhead`
///   写 + 双通知（on_seek/on_project_changed）；
/// - 控制协议薄壳（九动作四属性）原样平移——**e2e 契约字节不变**；
/// - 选中态真值留 Project（本类不再自存 selected——C1-C3 双源同步缺陷面就此
///   消灭，读取恒经 `project_->selected`）。
///
/// 注册名 `CaiyunTimeline`（main.cpp:540，e2e 选择器依赖）；C++ 类名保留
/// `TimelineView`（示例内既有引用面不动）。
///
/// 监视器（`MonitorView`）与时间轴分离的理由不变：监视器的重绘频率（播放中
/// 每帧）远高于时间轴（只有播放头动），合成到一起会让整条时间轴跟着每帧重画。

#include <functional>
#include <string>

#include "project.hpp"
#include "st/ui/element.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/components/timeline_view.hpp"  // 内置时间轴（本适配器的全部几何/绘制/命中）
#include "st/ui/timeline.hpp"                  // 时间单位与换算（秒 ↔ 毫秒集中一处）

namespace caiyun {

/// 时间轴薄适配器：内置 `st::ui::TimelineView` + Project 投影 + EditDelegate 编辑通道。
class TimelineView final : public st::ui::Element {
 public:
  TimelineView();
  ~TimelineView() override;   // 定义在 views.cpp（delegate_ 是 unique_ptr<不完整类>，
                              // 析构必须留在 Delegate 完整的翻译单元）

  /// 注册名必须与 `register_element_type<T>` 的名字**逐字一致**（否则声明式每次
  /// 重组都会把元素当"类型不符"销毁重建——实测现象是每帧一条
  /// "同位类型切换"日志、且元素级状态（拖拽/滚动）永远归零）。
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "CaiyunTimeline"; }
  [[nodiscard]] auto role() const noexcept -> st::ui::Role override { return st::ui::Role::Panel; }

  void set_project(Project* project);
  /// 工程内容变化后的回调（宿主用来刷新检查器/时间码）。
  std::function<void()> on_project_changed{};
  /// 播放头变化（拖动中）的回调——监视器据此重画。
  std::function<void()> on_seek{};

  /// 选中片段 id（真值在 Project；`-1` = 无）。
  [[nodiscard]] auto selected_clip() const noexcept -> int {
    return project_ != nullptr ? project_->selected : -1;
  }

  void apply_theme(const st::ui::Theme& theme) override;
  void measure(const st::ui::RenderContext& context, const st::ui::Constraints& constraints) override;
  void arrange(const st::ui::RenderContext& context, st::math::Rect rect) override;
  void paint_content(const st::ui::RenderContext& context, st::raster::Surface& canvas) const override;
  auto on_event(const st::ui::RenderContext& context, st::ui::Event& event) -> bool override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

 private:
  /// 把 Project 重建进内置件的 Model（**投影**：轨道按视觉序 V2 上 = 下标 0；
  /// tint 映射 generator_color；选中态从 Project.selected 反推）。set_project /
  /// paint / on_event 入口调用——绘制与命中始终读到最新几何。
  void sync_from_project() const;

  /// 交互后统一走这条（监视器 refresh + 检查器 bump 都挂在宿主回调上）。
  void notify_changed();
  /// 播放头变了：双通知（on_seek 每帧跟手 + on_project_changed 状态刷新）。
  void notify_seek();

  /// EditDelegate 的实现（内部类，定义在 views.cpp）：move/trim 转 Project，
  /// snap 按 Project 真值平移。`mutable`：投影同步发生在 const 路径
  ///（`paint_content`），delegate 内部触发 `sync_from_project` 与之同因。
  struct Delegate;

  Project* project_{nullptr};
  /// 内置时间轴（拥有；组合而非继承——Element 契约面由本类转发，避免双亲
  /// 双 bounds 的二义性）。
  std::unique_ptr<st::ui::TimelineView> view_{std::make_unique<st::ui::TimelineView>()};
  /// delegate 实例（拥有；`view_->set_edit_delegate` 收它的裸指针，生命周期
  /// 由本类的 unique_ptr 保证——两个成员同生共死，无悬垂窗口）。
  std::unique_ptr<Delegate> delegate_{};
};

/// 自绘预览监视器：显示播放头处当前帧的合成结果。
///
/// 与时间轴分离的理由：监视器的重绘频率（播放中每帧）远高于时间轴
///（只有播放头动），合成到一起会让整条时间轴跟着每帧重画。
class MonitorView final : public st::ui::Element {
 public:
  MonitorView();

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "MonitorView"; }
  [[nodiscard]] auto role() const noexcept -> st::ui::Role override { return st::ui::Role::Panel; }

  void set_project(Project* project);
  /// 强制按当前播放头重合成（seek/播放推进后调用）。
  void refresh();
  /// 取最近一帧的 **FNV-1a 64 哈希**（e2e 用它断言"同帧两次截图逐像素相同"）。
  ///
  /// 为什么在组件里算而不是让 e2e 截屏后比：截屏含**整个窗口**（含状态栏的
  /// 时间码文字、播放头位置等），那些量本来就该随播放头变——拿它们当判据会把
  /// "预览真的跟着走"与"UI 文字变了"混在一起。这里哈希的是**监视器画布本身**。
  [[nodiscard]] auto frame_hash() const noexcept -> std::uint64_t { return frame_hash_; }
  [[nodiscard]] auto frame_time() const noexcept -> double { return frame_time_; }

  void apply_theme(const st::ui::Theme& theme) override;
  void measure(const st::ui::RenderContext& context, const st::ui::Constraints& constraints) override;
  void paint_content(const st::ui::RenderContext& context, st::raster::Surface& canvas) const override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

 private:
  Project* project_{nullptr};
  /// 最近一次合成的帧（离屏）；`paint_content` 直接贴它。
  mutable st::raster::Canvas frame_;
  mutable double frame_time_{-1.0e9};
  mutable std::uint64_t frame_hash_{0};
  mutable bool frame_valid_{false};
  st::math::Color background_{};
  st::math::Color border_{};
};

}  // namespace caiyun

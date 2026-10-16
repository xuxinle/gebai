#pragma once

/// **时间轴视图**（`TimelineView`）：把 `st::ui::timeline::Model` 画出来并允许编辑。
///
/// ## 为什么数据与绘制分开（这个组件存在的意义）
///
/// caiyun 原先把「轨道/片段数据」与「命中判定/拖拽/修剪/吸附/标尺刻度」全写在一个
/// 400 行的自绘件里，那些逻辑只能靠截图验证。数据层（`timeline.hpp`）抽走之后
/// 有 7 例 79 断言的纯逻辑测试；本组件只做三件事：
/// **调换算 → 画 → 把鼠标事件翻译成模型操作**。
///
/// ## 三条交互约定（都是"错了会让人以为功能坏了"的地方）
///
/// 1. **拖拽期间不吸附到自己**：`Model::snap` 的 `exclude_*` 参数必须传**被拖的那个
///    片段**——不传的话它会永远吸在自己原来的位置，表现是"拖不动"。
/// 2. **修剪要动 `source_in`**：左边缘往右拖时，源入点要同步 +Δ（否则"修剪"变成了
///    "把内容往后挪"，而画面看起来只是"片段变短了"——素材内容错位在那个尺度上看不出来）。
/// 3. **锁定轨道不接受编辑**（但仍可选中查看）：锁定的语义是"别动它"，
///    不是"看不见它"。
///
/// ## 空白处拖动 = 平移（而不是框选）
///
/// 两个都合理，选平移的理由是**它不需要额外状态**：框选要维护"框"、要定义
/// "被框中的片段"、还要与单击（取消选择）区分开——而那三件事都要有可见反馈才不困惑。
/// 时间轴的高频动作是"看别处"，平移正好；框选留给将来的多选需求。

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/ui/element.hpp"
#include "st/ui/timeline.hpp"

namespace st::ui {

class TimelineView : public Element {
 public:
  /// 标尺带高度（逻辑像素）。
  static constexpr float kRulerHeight{24.0f};
  /// 轨道名前缀栏宽度（`名称` 显示在轨道左端）。
  static constexpr float kTrackHeaderWidth{88.0f};
  /// 修剪边缘的命中带宽度（与 `Model::hit_test` 的默认一致）。
  static constexpr float kEdgePx{6.0f};

  TimelineView();

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "TimelineView"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Panel; }

  /// 数据模型（宿主填充轨道与片段；组件只读它、改它）。
  [[nodiscard]] auto model() noexcept -> timeline::Model& { return model_; }
  [[nodiscard]] auto model() const noexcept -> const timeline::Model& { return model_; }

  /// **投影刷新防线**：宿主整体重建数据（`set_tracks` / 改 `tracks()` 后）必须调它。
  ///
  /// 拖拽中宿主换数据时，悬置的 `drag_` 快照指向旧投影（旧轨道/旧下标）——若不
  /// 复位，后续 MouseMove 会拿旧快照去改**新下标**对应的片段（陈旧下标改错对象）。
  /// 本入口复位一切悬置拖拽态（拖拽将「松手」而不是「粘住指针」）并清悬停标记
  ///（悬停中的片段可能已不存在；不清则残留高亮永远画不到正确对象上）。
  ///
  /// MouseMove 里另有快照自校验兜底（见 on_event）：宿主忘调本入口也不会改错
  /// 对象——两道防线是给不同读者的：这一个是契约，那一个是保险。
  void notify_model_replaced();

  /// 显示选项。
  void set_show_ruler(bool value) { show_ruler_ = value; mark_layout_dirty(); }
  [[nodiscard]] auto show_ruler() const noexcept -> bool { return show_ruler_; }
  /// 吸附开关（关掉时拖拽完全跟手，不再被候选拉走）。
  void set_snap_enabled(bool value) noexcept { snap_enabled_ = value; }
  [[nodiscard]] auto snap_enabled() const noexcept -> bool { return snap_enabled_; }
  /// 吸附候选来源（透传给 `Model::snap`）。
  void set_snap_options(timeline::SnapOptions options) { snap_options_ = std::move(options); }
  [[nodiscard]] auto snap_options() const noexcept -> const timeline::SnapOptions& {
    return snap_options_;
  }
  /// 选中片段的修剪手柄（左右缘竖条）显隐。默认 false：手柄只在**选中**且开关
  /// 打开时画——与全时段可见的「命中带」（`Model::hit_test` 的 kEdgePx，恒在）
  /// 是两回事：命中带管“能不能抓”，手柄管“看得见抓哪”。
  void set_show_trim_handles(bool value) { show_trim_handles_ = value; mark_dirty(); }
  [[nodiscard]] auto show_trim_handles() const noexcept -> bool { return show_trim_handles_; }
  /// 轨道名前缀栏（`kTrackHeaderWidth` 宽）显隐。默认 true：多轨时间轴靠它
  /// 认轨道；紧凑形态（单轨/双轨、标签画在轨道里）关掉它换回整幅内容宽度。
  /// 几何面（lane_rect/ruler_rect/track_band）、命中与像素采样随开关整体平移——
  /// 「点同一个 x 命中同一个时刻」在开关前后保持（时间↔像素换算只看 lane 左缘）。
  /// 走 `mark_layout_dirty`：lane 宽度变了，`Model` 的 `viewport_width` 要重新同步
  ///（arrange 里做）。
  void set_show_track_headers(bool value) { show_track_headers_ = value; mark_layout_dirty(); }
  [[nodiscard]] auto show_track_headers() const noexcept -> bool { return show_track_headers_; }

  /// 播放头**静默**写入口（外部驱动）：写 Model + `mark_dirty()`，**不触发**
  /// `on_playhead_changed`。
  ///
  /// 谁该用哪个：
  /// - **用户交互**（拖播放头/点标尺）→ 组件自己发回调，宿主不用调任何写口。
  /// - **控制协议/脚本**写 `set_property("playhead", …)` → 会发回调（那是「外部命令
  ///   改变了当前时间」的对外口径，宿主该知道）。
  /// - **宿主主循环每帧推进**（播放、步进、检查器滑杆这类「时间真值源在宿主手里」
  ///   的写）→ **必须走本入口**：若走 set_property，会形成「写→回调→宿主 refresh/
  ///   再写」的推拉回环（caiyun 换件方案 S2，docs/caiyun_timelineview_refactor_plan.md
  ///   §③/G21）。写完记得 mark_dirty 已在内部做好，宿主不必再标。
  void set_playhead_external(timeline::Millis time) {
    model_.set_playhead(time);
    mark_dirty();
  }

  // —— 编辑委托通道（caiyun 换件方案 S1，docs/caiyun_timelineview_refactor_plan.md
  // §③/§④ 步骤 4）——「数据在别处」的集成形态：真值源在宿主（Project/工程文件）
  // 手里，组件自持 Model 只是**投影**。挂上 delegate 后，拖拽编辑不再是「直接写
  // 自持 Model」，改走「组件算出拖拽目标 → 交宿主裁决 → 宿主说了算」：接受则
  // 组件按目标写投影，拒绝则把投影恢复到裁决前的值（净效果 = 自持 Model 不动）。
  // 不挂 delegate 时行为与旧版字节一致（gallery 卡与全部存量用例依赖现状语义）。
  struct EditDelegate {
    /// 修剪边缘（**编辑面**口径）。与 `timeline::ClipPart`（**命中面**，含 Body/
    /// None——命中到片体不是修剪）是两个枚举：分开而非复用，是为了让「trim 收到
    /// Body」在类型上就不可能——那类「无害但语义含混」的传参靠窄类型挡掉。
    enum class Edge : std::uint8_t {
      Left,    ///< 入点（宿主侧应同步源入点语义，参考内置直写分支的联动写法）
      Right,   ///< 出点（只改时长）
    };

    virtual ~EditDelegate() = default;

    /// 移动片段到新起点（`duration`/`source_in` 不变——移动不是缩放）。
    /// 入参是拖拽想达成的**目标**（不是增量）——增量口径会把快照知识泄漏给宿主。
    /// 返回 false = 宿主拒绝该编辑。
    [[nodiscard]] virtual auto move(std::size_t track, std::size_t clip,
                                    timeline::Millis new_start) -> bool = 0;
    /// 修剪边缘到新时间：`edge == Left` 时 `time` 是片段的新**起点**，
    /// `edge == Right` 时是新**结束时刻**（start + duration）。
    /// 返回 false = 宿主拒绝该编辑。
    [[nodiscard]] virtual auto trim(std::size_t track, std::size_t clip, Edge edge,
                                    timeline::Millis time) -> bool = 0;
    /// 吸附判定：把候选时间吸到宿主真值源的吸附点（其他片段边缘/播放头/标记…）。
    /// 与内置 `Model::snap` 同语义——不吸附就原样返回 candidate；返回什么组件就
    /// 用什么（吸附的最终解释权在宿主，无法拒绝）。`exclude` 是被拖片段拖拽前的
    /// 起点（同内置排除自片段的语义，换成时间口径：宿主据此在真值源里定位并排除
    /// 它，也可无视而用自己的拖拽状态）。
    [[nodiscard]] virtual auto snap(timeline::Millis candidate,
                                    timeline::Millis exclude) -> timeline::Millis = 0;
  };

  /// 挂编辑委托（`nullptr` = 卸下，回到内置直写编辑）。不持有所有权：宿主须
  /// 保证 delegate 活得比组件对它的使用史更久（组件只在拖拽事件里同步调用它）。
  /// **重入说明**：宿主在 move/trim 回调内改写真值源后重新 `set_tracks` 重建投影
  /// 是预期用法（投影语义——宿主的值永远最后落笔）；组件不因此复位拖拽状态，
  /// 拖拽快照取自 MouseDown 时刻的投影，重建后继续有效（同一片段下标）。这也是
  /// 给 S3（投影刷新防线）的契约：挂 delegate 时宿主每帧重建投影是常态，S3 的
  /// 「陈旧快照即复位」判据在本形态下不适用（合并时须豁免）。
  void set_edit_delegate(EditDelegate* delegate) noexcept { edit_delegate_ = delegate; }
  [[nodiscard]] auto edit_delegate() const noexcept -> EditDelegate* { return edit_delegate_; }

  // —— 回调（宿主据此更新"当前时间"显示、落盘撤销栈等）——
  std::function<void(timeline::Millis)> on_playhead_changed{};
  std::function<void(std::size_t track, std::size_t clip)> on_clip_moved{};
  std::function<void(std::size_t track, std::size_t clip)> on_clip_trimmed{};

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;

  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action,
                                   std::string_view argument) -> bool override;

 private:
  /// “无悬停/无命中”哨兵（不同子组件间通用，`std::size_t` 的 kNone 同族）。
  static constexpr std::size_t kNoIndex{static_cast<std::size_t>(-1)};

  /// 拖拽目标（一次只可能拖一样）。
  struct Drag {
    enum class Kind : std::uint8_t { None, Playhead, ClipBody, ClipLeft, ClipRight, Pan };
    Kind kind{Kind::None};
    std::size_t track{0};
    std::size_t clip{0};
    /// 开始拖动时的指针位置（屏幕）——用来算增量，而不是"跟着指针跳变"。
    ///
    /// 用增量而非绝对位置的理由：修剪的"最小长度"与"不能越过相邻片段"这类约束
    /// 一旦生效，绝对定位会让片段在约束边界上"粘住后跳走"；增量式则自然停住。
    math::Point origin{};
    timeline::Millis clip_start{0};
    timeline::Millis clip_duration{0};
    timeline::Millis clip_source_in{0};
    timeline::Millis scroll{0};
  };

  /// 时间轴的绘图区（排除轨道名前缀栏）。
  [[nodiscard]] auto lane_rect() const -> math::Rect;
  /// 标尺带矩形（`show_ruler_` 为假时为空）。
  [[nodiscard]] auto ruler_rect() const -> math::Rect;
  /// 轨道带矩形（内容区坐标，含横向滚动）。
  [[nodiscard]] auto track_band(std::size_t index) const -> math::Rect;
  /// 片段矩形（为空 = 该片段不可见/不存在）。
  [[nodiscard]] auto clip_rect(std::size_t track, std::size_t clip) const -> math::Rect;
  /// 屏幕 x → 时间轴时间（把 lane 内的相对 x 转成模型坐标）。
  [[nodiscard]] auto x_to_time(float screen_x) const -> timeline::Millis;
  [[nodiscard]] auto time_to_x(timeline::Millis time) const -> float;

  timeline::Model model_{};
  bool show_ruler_{true};
  bool snap_enabled_{true};
  bool show_trim_handles_{false};
  bool show_track_headers_{true};
  timeline::SnapOptions snap_options_{};
  Drag drag_{};
  EditDelegate* edit_delegate_{nullptr};
  /// 播放头是否正在被拖（绘制时把柄画实心，提示"你抓着它"）。
  bool dragging_playhead_{false};
  /// 悬停中的片段（lane 内坐标；无悬停 = kNone）。自绘内容不受 Element 框体层
  /// hover_effect 管（那只作用于整块框体），片段级悬停须组件自跟踪：MouseMove
  /// 查询命中更新，HoverOut 清除（与 caiyun 手写版同一套最小实现）。
  std::size_t hover_track_{kNoIndex};
  std::size_t hover_clip_{kNoIndex};

  math::Color surface_{};
  math::Color surface_alt_{};
  math::Color surface_sunken_{};
  math::Color border_{};
  math::Color text_color_{};
  math::Color text_muted_{};
  math::Color accent_{};
  math::Color clip_fill_{};
  math::Color clip_selected_{};
  math::Color playhead_color_{};
  math::Color muted_track_{};
  float font_size_{12.0f};
};

}  // namespace st::ui

#pragma once

/// @file native_host.hpp
/// @brief `NativeHost`：把**外部程序的窗口**嵌进界面里的一块矩形（典型：内嵌浏览器）。
///
/// ## 它解决什么问题
///
/// 有些东西本框架**不打算自己实现**——最典型的是"渲染任意真网页"：那需要一个完整
/// 浏览器引擎，而框架的取向是"不做浏览器引擎"（`DESIGN.md` §1 非目标）。但现实需求
/// （内网系统、图表看板、网页编辑器）确实存在。
///
/// `NativeHost` 是这类需求的**逃生门**：框架不渲染网页，只负责
///
/// ```
///   给外部窗口一块正确的屏幕矩形  +  把它裁在自己的范围内  +  销毁时把它还原
/// ```
///
/// 界面其余部分（布局、主题、事件、其余组件）完全不受影响——外部窗口是**平台合成器**
/// 叠上去的一层，不是本框架画的。
///
/// ## 不可用时的行为（**关键契约**）
///
/// 这个组件在**任何**环境下都不会让界面崩坏或报错：
///
/// | 环境 | 行为 |
/// |---|---|
/// | 后端不支持（无头 / 未实现平台） | 退化为**占位块**：按布局占位、画一个提示底色；`attached` 恒 false |
/// | 传入的句柄无效 | `attach` 返回 false，组件保持未嵌入状态（不抛异常、不留半嵌入） |
/// | 嵌入后外部程序退出 | 自检读数显示 `attached = false`（下次 `sync` 时检测到），界面照常 |
/// | 嵌入后切到其它页 / 被祖先隐藏 | **自动藏起**（可见区为空 ⇒ 后端隐藏宿主），切回来自动恢复 |
/// | 滚出滚动容器的可见区 | 只露出交集那块（与绘制链的裁剪口径一致） |
///
/// 这条契约是**结构保证**而不是靠调用方自觉：`HostFocus` 的五个能力方法**默认全部
/// 降级**（`false` / 空操作），所以没接 provider 的宿主（离屏渲染、测试桩、旧后端）
/// 天然走到占位路径。
///
/// ## `capture` 看不到它（能力表必须写清）
///
/// 嵌入内容的像素**不进 `framebuffer()`**：它由平台合成器直接上屏，而 `capture` 只读
/// 软件光栅器。所以 `capture` 在那块矩形上拍到的是**本组件自己画的占位底色**，与真实
/// 嵌入内容无关。
///
/// 推论（很重要）：**任何以 `capture` / `visual_diff` 为判据的回归都看不见嵌入内容**。
/// 它冻了、白了、外部进程崩了，像素回归仍然全绿。要确认"嵌入物真的在跑/对位"，只能读
/// 本组件暴露的属性面（`attached` / `content_x` / `content_y` / `content_width` / `host_width`…）。
///
/// ## 用法
///
/// ```cpp
/// auto host = std::make_unique<NativeHost>();
/// host->set_preferred_size({640.0f, 480.0f});
/// // 拿到外部窗口句柄后（比如由应用自己启的浏览器进程）：
/// const bool ok = host->attach(browser_window_handle);
/// if (!ok) { /* 后端不支持或句柄无效：组件已退化为占位块，界面正常 */ }
/// ```
///
/// ## 生命周期责任（调用方必须知道）
///
/// - **本组件不拥有外部窗口**：`attach` 不创建它，析构也不销毁它。析构/`detach` 只做
///   "把它还原成独立顶层窗口"（reparent 回桌面），外部程序与窗口仍然存在，由**调用方**
///   决定什么时候关掉。
/// - 反过来说：**外部窗口必须在 `NativeHost` 之后销毁**。若先杀了外部进程，句柄会失效，
///   此时本组件只会安全地报 `attached = false`（不会崩溃），但那一层窗口已经没了。

#include <string>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/ui/component_registry.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 把外部程序的窗口嵌进界面的一个矩形里。
///
/// 见文件头注释：这是"不做浏览器引擎"的逃生门，不可用时**静默退化为占位块**。
class NativeHost : public Element {
 public:
  NativeHost();
  ~NativeHost() override;
  NativeHost(const NativeHost&) = delete;
  auto operator=(const NativeHost&) -> NativeHost& = delete;

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "NativeHost"; }

  /// 属性面：自检读数的**只读**投影。
  ///
  /// 为何必须暴露：嵌入内容的像素不进 `framebuffer()`，"它还活着吗/对位了吗"
  /// 只能从这组数字里读。上也界面、下到控制通道（`get id` 直接看）。
  [[nodiscard]] auto readonly_property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto get_property(std::string_view name) const
      -> std::optional<std::string> override;

  /// 期望尺寸（逻辑像素）。宽度不指定时撑满可用宽度；高度不指定时用 `kDefaultHeight`。
  void set_preferred_size(math::Size size);
  [[nodiscard]] auto preferred_size() const noexcept -> math::Size { return preferred_; }

  /// 把外部窗口 `child`（**原生句柄**）嵌进来。
  ///
  /// 返回值语义（**不假装成功**）：
  /// - `true`：已嵌入（或本来就是这个句柄，重复调用是幂等的）；
  /// - `false`：无宿主 / 后端不支持 / 句柄无效 / 嵌入失败。此时组件处于**占位块**状态，
  ///   界面与布局不受任何影响。
  auto attach(void* child) -> bool;

  /// 解除嵌入：把外部窗口还原成**独立的顶层窗口**（不销毁它），组件回到占位块状态。
  void detach();

  /// 当前是否已嵌入。注意它读的是**宿主侧的真实状态**（后端读数），不是本地标志位：
  /// 外部窗口被关掉后，下一次布局同步时它会变成 `false`。
  [[nodiscard]] auto attached() const -> bool;

  /// 占位块是否显示（未嵌入时画一块提示底色）。默认 `true`。
  ///
  /// 为什么默认显示而不是留空：留空看起来像"组件没渲染"（用户/智能体都会误判为缺陷）；
  /// 显示一块明确的占位块才是"这里本该有东西，但当前环境给不了"的诚实表达。
  void set_placeholder_visible(bool visible);
  [[nodiscard]] auto placeholder_visible() const noexcept -> bool { return placeholder_; }

  /// 占位块上显示的文字（未嵌入时）。空串 = 只画底色不写字。
  void set_placeholder_text(std::string text);
  [[nodiscard]] auto placeholder_text() const -> const std::string& { return placeholder_text_; }

  /// 当前宿主侧的自检读数（未嵌入时 `attached = false`）。
  ///
  /// 为什么要公开它：嵌入内容的像素不进 `framebuffer()`，这组读数是**唯一**能
  /// 判断“内容是否真的在跑、有没有落在 (0,0) 并填满宿主”的信息源。
  /// 应用层把它贴到界面上/日志里，才能让人（与自动化）看见嵌入物的真实状态。
  [[nodiscard]] auto child_view_probe() const -> Element::ChildViewProbe;

  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  /// 可见性变了（含被祖先隐藏）：外部窗口必须跟着藏/显——它不经过 `paint`
  /// 的 clip 栈，也不会在隐藏子树里被 `arrange` 到。
  void on_visibility_changed() override;
  void apply_theme(const Theme& theme) override;

 private:
  /// 把当前 `bounds_` 同步给宿主（布局变了、或嵌入后第一次摆放）。
  /// 无宿主/未嵌入时为空操作。
  void sync_bounds();
  /// 重新量一次并落位（用于“外部窗口还在重排”的收敛阶段）。
  void refine_bounds();
  /// 内容是否已经落在 (0,0) 且尺寸等于宿主（“真的就位了”的唯一判据）。
  [[nodiscard]] auto content_fits() const -> bool;
  /// 读宿主侧的嵌入状态（每次都要重新读——外部窗口可能已经没了）。
  [[nodiscard]] auto probe() const -> Element::ChildViewProbe;

  static constexpr float kDefaultHeight{360.0f};
  /// 嵌入/改尺寸后最多再重测多少帧（≈ 带上限的短收敛，不是常驻轮询）。
  static constexpr int kMaxRefineFrames{20};

  /// 外部窗口句柄（`nullptr` = 未嵌入）。
  void* child_{nullptr};
  math::Size preferred_{0.0f, kDefaultHeight};
  bool placeholder_{true};
  std::string placeholder_text_{"原生窗口不可用"};
  /// 上一次同步给宿主的矩形（避免每帧都 `SetWindowPos` 造成无谓的重绘闪烁）。
  math::Rect last_synced_{};
  /// 上一次下发的**可见矩形**（`visible_rect()`）。单独记它：只比 `bounds_`
  /// 不够——切页/滚动时 `bounds_` 可能一点没变，而可见区已经没它了。
  math::Rect last_clip_{};
  bool has_synced_{false};
  /// 收敛剩余帧数。
  ///
  /// 为何需要它：外部窗口 reparent/resize 后的**内部重排是异步的**（浏览器在
  /// 自己的消息循环里才把标题栏/渲染层按新尺寸摆好）。刚 `SetWindowPos` 完就去
  /// 量“内容子窗口偏移”，量到的是**旧布局**的值；拿着它去顶偏移，内容就会被
  /// 错位（实测：量到 107，而重排完的真实偏移是 0——内容被顶出去 107px、
  /// 底部溢出近百像素，而所有属性面读数看起来都“正常”）。
  ///
  /// 所以嵌入/改尺寸后的若乾帧里重新量几遍，直到读数稳定。上限很小
  ///（见 `kMaxRefineFrames`）：这是**视觉层**的修正，不该变成常驻轮询。
  int refine_frames_{0};
  /// 占位底色（未嵌入时画）。
  math::Color placeholder_fill_{0, 0, 0, 0};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::NativeHost, "NativeHost")

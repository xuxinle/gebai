#pragma once

/// @file input_box.hpp
/// @brief `Input`：单行文本输入框。。
///
/// 从 `input.hpp` 拆出（**一组件一头**）。拆分规则与理由见 `st/ui/components/README.md`：
/// 一个头一个组件 → 依赖关系写在组件自己身上 → include 谁就只有谁进产物。
/// `input.hpp` 仍保留**聚合**（只 include 各子头），既有引用不必一次改完。

#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include "st/math/geometry.hpp"
#include "st/ui/component_registry.hpp"
#include "st/ui/components/select_panel.hpp"
#include "st/ui/element.hpp"
#include "st/ui/blink.hpp"

namespace st::ui {

/// 单行文本输入框。
///
/// 交互：点击定位光标、`Home`/`End`/方向键移动、`TextInput` 插入、`Backspace`/`Delete` 删除、
/// `Enter` 触发 `on_submit`；内容变化触发 `on_change`。光标闪烁由 `context.time_seconds` 驱动（1s 周期）。
/// 密码态与普通文本**只差字符形状**：掩码是纯显示层，选中/复制/双击选词/输入法
/// 全部照常（右侧另有“显示/隐藏”切换钮，见 `set_password_reveal`）。
/// 语义：`text` 暴露占位符（无障碍名称），`value` 暴露当前文本（密码态且掩码时为掩码 `•`）。
class Input : public Element {
 public:
  Input();

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Input"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::TextInput; }

  /// 换一份文档（**光标移到末尾**）。
  ///
  /// 语义约定（全仓统一）：
  /// - `set_text` = **换了一份文档**：光标/滚动重置；适合“打开文件、程序化载入”。
  /// - `update_text` = **内容变了但位置要保住**：光标按**字符偏移**保持；
  ///   适合“重新计算后回写同一份内容”。
  ///
  /// 混用代价实测过（`CodeEditor` 曾用 `set_text` 做重排回写）：每敲一个字光标就
  /// 跳回行首、滚动回顶、撤销栈被清空。
  void set_text(std::string text);
  /// 更新文本但**保住光标位置**（见 `set_text` 的语义约定）。
  ///
  /// 偏移超出新文本长度时夹到末尾；文本未变时什么都不做（不标脏）。
  void update_text(std::string text);
  /// 占位文本（内容为空时以 `text_faint` 绘制）。
  void set_placeholder(std::string text);
  /// 密码模式：显示掩码 `•`（`value()` 仍返回明文）。
  void set_password(bool value) noexcept;

  // —— 密码态的「显示/隐藏」切换 ——
  //
  // 掩码是**纯显示层**：选中、拖选、双击选词、`Ctrl+A/C/X`、剪贴板、输入法
  // 与普通文本框完全一致（除“字符画成 `•`”之外没有第二处差别）。
  // 切换按钮长在输入框**内部右端**——与内嵌下拉同一套排布规则（右侧预留槽位、
  // 命中区在框内），否则会得到“输入框 + 旁边一个按钮”两个视觉体。

  /// 密码态下是否提供切换按钮（**默认开启**：密码框本该能看一眼再决定）。
  /// 关闭时右侧槽位被收回（文字可用宽度变宽）。
  void set_password_reveal(bool enabled);
  [[nodiscard]] auto password_reveal() const noexcept -> bool { return password_reveal_; }
  /// 当前是否以**明文**显示。**只影响显示**：`value()` 恒为明文。
  /// 进入密码态时复位为 `false`（切换模式不该顺带把秘密摊开）。
  void set_password_visible(bool visible);
  [[nodiscard]] auto password_visible() const noexcept -> bool { return password_visible_; }
  /// 切换按钮的命中矩形（测试/自动化定位用；非密码态或未开启时为空矩形）。
  [[nodiscard]] auto password_reveal_rect() const noexcept -> math::Rect { return reveal_rect_; }
  /// 前缀图标名（图标集名，如 `"search"`；为空则不占格子也不画）。
  void set_icon_prefix(std::string_view icon_name);

  // —— 内嵌下拉（输入框右侧的下拉按钮 + 选项面板）——
  //
  // 适用场景：值既**可手填**又可**从列表选**（如模型名、分支名、环境名）。
  // 两个需求共存，所以不能做成“纯下拉”，也不能把两个控件并排拼——
  // 那会得到“输入框 + 旁边的按钮”两个视觉体，而用户心里它是一个字段。
  // 本组接口让下拉按钮**长在输入框里**（右侧预留槽位，命中区在框内）。
  //
  // 生命周期：**选项由调用方填**（`set_dropdown_options`）。面板的挂载/摘除
  // 走 `overlay_host`/`overlay_remove`（与 `Select` 同一约定，宿主通常是 UiRoot）。

  /// 启用/关闭内嵌下拉按钮。关闭时右侧槽位被收回（文字可用宽度变宽）。
  void set_dropdown(bool enabled);
  [[nodiscard]] auto has_dropdown() const noexcept -> bool { return dropdown_enabled_; }
  /// 选项表（`label` 为空时显示 `value`）。空表时按钮不展开，
  /// 而是先调 `on_dropdown_need_options`（见下）。
  void set_dropdown_options(std::vector<SelectOption> options);
  [[nodiscard]] auto dropdown_options() const noexcept -> const std::vector<SelectOption>& {
    return dropdown_options_;
  }
  /// **选项尚未就绪时用户点了箭头**：回调里启动拉取即可。
  ///
  /// 选项到位后若用户当时确实点过箭头，会自动展开（无需再点一次）。
  /// 设了这个回调时，“空表”不再意味着“点了没反应”。
  std::function<void()> on_dropdown_need_options{};
  /// 展开/收起选项面板（未启用/无选项/未设 `overlay_host` 时无效）。
  void set_dropdown_open(bool value);
  [[nodiscard]] auto dropdown_open() const noexcept -> bool { return dropdown_open_; }
  /// 右侧按钮命中矩形（测试/自动化定位用；未启用时为空矩形）。
  [[nodiscard]] auto dropdown_rect() const noexcept -> math::Rect { return dropdown_rect_; }

  /// 列表项被选定：实参是选项的 `value`。**选中后已自动回填 `set_text`**，
  /// 本回调只用于“选择之后还想做点什么”（发请求、刷新依赖等）。
  std::function<void(std::string_view)> on_dropdown_pick{};
  /// 面板挂载回调（**必需**，否则无法展开）。
  std::function<void(std::unique_ptr<Element>)> overlay_host{};
  /// 面板摘除回调（可选）。
  std::function<void(Element*)> overlay_remove{};
  /// **背景与父级一致**：不铺凹陷底色（**描边照画**）。
  ///
  /// 用途：把输入框放进一行密集的控件里（如日志工具栏）时，凹陷底色会把它
  /// 从那一排里“扒”出来，看着不像同一个工具条。
  ///
  /// 为何**保留描边**：底色抹平后，1px 描边是这个控件“可以输入”的唯一视觉线索；
  /// 两者都不画的话，它在工具栏里与一片空白无异。
  ///
  /// 为何不是“自己改一个颜色”：颜色得随主题变，而这里要的是**不画底色**——
  /// 透明底让父级背景自然透出，主题切换无需额外同步。
  void set_seamless_background(bool seamless);
  [[nodiscard]] auto seamless_background() const noexcept -> bool { return seamless_bg_; }
  /// 设置光标字节位置（自动夹取到合法 UTF-8 码点边界）。
  void set_cursor_index(std::size_t index);
  /// 选区（字节偏移，**已归一化**且已夹到码点边界）；无选择时 `begin == end`。
  ///
  /// 单行框的选区不需要“行号”——用**字节区间**表达就够。
  /// 与 `CodeEditor` / `MarkdownView` / `Terminal` 的 `selection` 属性同一语义。
  [[nodiscard]] auto selection() const -> std::pair<std::size_t, std::size_t>;
  /// 设置选区（反向传参会自动归一；两端都夹到码点边界与文本长度内）。
  void set_selection(std::size_t anchor, std::size_t focus);
  /// 清空选区（不动光标）。
  void clear_selection() noexcept { select_anchor_ = kNoSelection; }
  /// 是否有非空选区。
  [[nodiscard]] auto has_selection() const -> bool;
  /// 选中文本（与普通文本框同口径：选中什么就返回什么；掩码只影响绘制）。
  [[nodiscard]] auto selected_text() const -> std::string;

  [[nodiscard]] auto value() const noexcept -> const std::string& { return text_; }
  [[nodiscard]] auto placeholder() const noexcept -> const std::string& { return placeholder_; }
  [[nodiscard]] auto password() const noexcept -> bool { return password_; }
  [[nodiscard]] auto icon_prefix() const noexcept -> const std::string& { return icon_prefix_; }
  [[nodiscard]] auto cursor_index() const noexcept -> std::size_t { return cursor_; }

  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  /// 按**光标位置**决定指针：内嵌下拉箭头处是可点击区（`Hand`），
  /// 其余是文本编辑区（`Text`）。
  ///
  /// 为何不能只在构造里 `set_cursor(MouseCursor::Text)` 就完事：那是**元素级**
  /// 的静态值，于是整个输入框（包括那个按钮）都是 I-beam，
  /// 鼠标移到箭头上看着像可以选中文字、实际点下去却是展开下拉——两者不一致。
  [[nodiscard]] auto cursor_at(math::Point point) const noexcept -> MouseCursor override;
  void activate() override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto semantics_flags() const -> SemanticsFlags override;

  // 属性面：协议 `set` / 脚本 `$('#x').set()` 的**唯一入口**。
// 不实现它，`{"value": "x"}` 会被静默忽略——实测踩过：对输入框 set value 一直无效且不报错。
[[nodiscard]] auto get_property(std::string_view name) const
-> std::optional<std::string> override;
auto set_property(std::string_view name, std::string_view value) -> bool override;
[[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  /// 只读属性（`selection`）：`get` 能读、`set` 不生效。
  [[nodiscard]] auto readonly_property_names() const -> std::vector<std::string_view> override;
/// 动作面（与 `TextArea` 对齐；此前单行 Input 缺这一面——DESIGN §8.1.1 反推的
/// 「API 存在但动作面未实现」同族缺口）：`submit`/`activate`（触发 `on_submit`）、
/// `clear`（清空）。
[[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
-> bool override;

  /// 主题变化 → 重落底色/描边（见 `.cpp` 处的注释：它们原本只在 measure 里
  /// 做一次性“未设则填”，切主题时 style 会永久停在旧值）。
  void apply_theme(const Theme& theme) override;

  /// 内容变化回调（实参为最新文本，指向组件内部存储）。
  std::function<void(std::string_view)> on_change{};
  /// `Enter` 提交回调。
  std::function<void(std::string_view)> on_submit{};

  private:
 /// 我方上一次写进 `style_.background` / `border_color` 的值（全透明 = 从未写过）。
 /// 用于区分“这个色是主题落的”与“这个色是调用方显式设的”——前者该随主题更新，
 /// 后者不能动。仅看 `a == 0` 不够：首轮填充后它就再也分不清两者了。
 math::Color applied_theme_bg_{};
 math::Color applied_theme_border_{};
   /// 底色/描边是否已由主题接管（首轮无条件接管一次，之后才看“有没有被调用方改过”）。
  bool theme_style_owned_{false};
  /// 无缝背景（不画底/不画边，透出父级）。
  bool seamless_bg_{false};
   [[nodiscard]] auto inner_box(const RenderContext& context) const -> math::Rect;
  [[nodiscard]] auto display_text() const -> std::string;
  /// 切换按钮是否在当前形态下存在（密码态 + 未被关掉）。
  [[nodiscard]] auto reveal_active() const noexcept -> bool {
    return password_ && password_reveal_;
  }
  /// 切换按钮的槽宽（逻辑 px）；不可用时为 0（`inner_box` 不扣它）。
  [[nodiscard]] auto reveal_slot_width(const RenderContext& context) const -> float;
  [[nodiscard]] auto cursor_x(const RenderContext& context) const -> float;
  /// 给定**字节索引** → 屏幕 x（与 `cursor_x` 同算法；选区高亮两端用它）。
  [[nodiscard]] auto index_to_x(const RenderContext& context, std::size_t text_index) const -> float;
  [[nodiscard]] auto index_at_x(const RenderContext& context, float x) const -> std::size_t;
  auto handle_key(const Event& event) -> bool;
  void insert_text(std::string_view inserted);
  void notify_change();

  std::string text_{};
  std::string placeholder_{};
  std::string icon_prefix_{};

  // —— 内嵌下拉（右侧按钮 + 选项面板）——
  bool dropdown_enabled_{false};
  bool dropdown_open_{false};
  /// 用户点过箭头、但当时选项未就绪：选项到位后自动展开。
  bool dropdown_pending_open_{false};
  std::vector<SelectOption> dropdown_options_{};
  /// 右侧按钮命中区（`measure` 时算定；绘制与命中都用它，单一真源）。
  math::Rect dropdown_rect_{};
  /// 已挂上的面板（非拥有；`overlay_remove` 后置空）。
  ///
  /// 存 `SelectPanel*` 而不是 `Element*`：`arrange` 里要调它的
  /// `set_anchor`（重排后面板要跟着控件走）。
  SelectPanel* dropdown_panel_{nullptr};
  /// 面板延迟摘除（见 `flush_dropdown_dismiss`）。
  mutable bool dropdown_dismiss_pending_{false};
  /// 延迟摘除期间待摘的面板（`flush_dropdown_dismiss` 是 const，
  /// 而它要改 `dropdown_panel_`；用一个 mutable 中转）。
  mutable SelectPanel* dropdown_pending_remove_{nullptr};

  void open_dropdown_panel();
  void close_dropdown_panel();
  void flush_dropdown_dismiss() const;
  void pick_dropdown_item(std::size_t index);
  [[nodiscard]] auto dropdown_slot_width(const RenderContext& context) const -> float;
  /// 按当前排版状态算插入点矩形（客户区逻辑坐标）。
  ///
  /// 抽成函数是因为**两个时机都要用**：`arrange`（几何刚定，缓存它供输入法用）
  /// 与 `paint_content`（绘制时光标就在这里）。两处各写一遍算式必然漂移。
  [[nodiscard]] auto caret_rect_now(const RenderContext& context) const -> math::Rect;
  std::size_t cursor_{0};
  /// 输入法预编辑串（显示态；**不进 `text_`**）。
  std::string preedit_{};
  /// 上一帧绘制的插入点矩形（`caret_rect()` 用；它拿不到 `RenderContext`）。
  mutable math::Rect last_caret_rect_{};
 public:
  /// **输入法候选窗的锚点**（客户区逻辑坐标）。
  ///
  /// 为何不能只返回 `last_caret_rect_`：那个缓存只在 `paint_content` 里写，
  /// 而 `publish_ime_caret()` 在**聚焦时**与**事件派发后**就会被调——
  /// 首次聚焦/刚切开面板那一帧，本元素还没画过，缓存是空矩形，
  /// 上报出去就是 `(0,0,0,0)` ⇒ 系统把候选窗放屏幕上角（这就是
  /// “候选窗没对到光标”的直接原因）。
  ///
  /// 兜底：缓存无效时按当前 `bounds_` 推出“首行行首”的位置。
  /// 它不依赖文字排版（拿不到 `RenderContext`），所以横向只能给行首——
  /// 但**纵向一定对**，而候选窗贴不贴光标行主要由纵坐标决定；
  /// 下一帧画过之后缓存就接管，横向也跟着准。
  [[nodiscard]] auto caret_rect() const -> math::Rect override {
    if (!last_caret_rect_.is_empty()) return last_caret_rect_;
    if (bounds_.is_empty()) return math::Rect{};
    // 行盒估算：与 `paint_content` 同一套内边距，但不知道字号——
    // 用可用高度的最小行（`style_.font_size` 是最接近的可信值）。
    const float line = style_.font_size > 0.0f ? style_.font_size * 1.45f : 18.0f;
    const float x = bounds_.x + style_.padding.left + 4.0f;
    const float y = bounds_.y + (bounds_.height - line) * 0.5f;
    return math::Rect{x, y, 2.0f, line};
  }

 private:
  bool password_{false};
  /// 密码态是否提供切换按钮（默认开）。
  bool password_reveal_{true};
  /// 当前是否明文显示（只影响显示层）。
  bool password_visible_{false};
  /// 切换按钮命中区（`arrange` 时算定；绘制与命中都用它，单一真源）。
  math::Rect reveal_rect_{};
  /// 光标闪烁状态机（协议见 `st/ui/blink.hpp`）。`mutable`：`paint_content` 是 const。
  mutable BlinkCursor caret_blink_{};

  // —— 选区（2026-10-14）——
  //
  // 单行框的选区只需**一个字节偏移**：锚点与光标（`cursor_`）就是两端。
  // （不用 `Caret{line, offset}`：这里根本没有“行”这个维度，多一层反而易错。）
  // `kNoSelection` = 无选区（与“锚点 == 光标”区分开：单击不应当成“有选择”）。
  static constexpr std::size_t kNoSelection = std::numeric_limits<std::size_t>::max();
  std::size_t select_anchor_{kNoSelection};   ///< 拖选起点（按下那一端）
  bool selecting_{false};                     ///< 鼠标拖选中
  /// 双击/三击判定：上次按下的时间与位置（时间取自 `RenderContext`）。
  std::int64_t last_click_ms_{-1};
  float last_click_x_{0.0f};
  int click_streak_{0};   ///< 连击次数（1=单击、2=双击选词、3=三击整行）

  /// 选区两端（已归一 + 夹取）；无选区时 `{cursor_, cursor_}`。
  [[nodiscard]] auto ordered_selection() const -> std::pair<std::size_t, std::size_t>;
  /// 删除当前选区（用于替换输入）；返回是否有东西被删。
  auto erase_selection() -> bool;
  /// 在光标处插入并清空选区。**这是所有文本插入的唯一入口**——
  /// 否则“有选区时插入”的行为会在多处各写一遍（必然漏一处）。
  void replace_selection_with(std::string_view inserted);
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::Input, "Input")

#pragma once

/// @file mind_map_view.hpp
/// @brief `MindMapView`：思维导图的**查看 / 编辑 / 保存**（自绘，零外部依赖）。
///
/// ## 它由什么组成（与 `MermaidView` / `GraphView` 同一二分工）
///
/// | 层 | 职责 | 能否单测 |
/// |---|---|---|
/// | `st::md::mindmap`（`st/md/mindmap.hpp`） | 树模型、编辑操作、FreeMind `.mm` 往返、tidy-tree 布局——纯计算 | ✅ 纯函数，可穷举 |
/// | **本组件** | 把几何画成像素、命中/拖拽/键盘、内嵌改名、属性面/动作面、文件读写 | 截图 + 属性断言 |
///
/// ## 为什么内嵌改名用真的 `Input`，而不是自己写光标
///
/// 单行内嵌编辑要覆盖的东西比看上去多得多：光标闪烁（`BlinkCursor`）、选区与拖选、
/// 双击选词/三击选行、`Home`/`End`/方向键、`Ctrl+A/C/X/V`、输入法预编辑
/// （`ImeComposition` 与候选窗定位，经 `caret_rect()` 上报）、退格连删……
/// 这些在 `Input` 里已经各自有结论（每一条都对应一次实测修正）。自建一套"迷你编辑器"
/// 的代价是把那些结论**再踩一遍**，且只有一半功能。
///
/// 所以改名是：双击节点 → 在该节点位置**挂一个 `Input` 子元素**（绝对定位、铺在节点框上）
/// → 它拿到焦点 → `Enter`/失焦提交，`Escape` 取消。组件自己只做"生命周期"与"提交语义"。
///
/// ## 为什么保存用 FreeMind `.mm`（而不是自定义 JSON）
///
/// 思维导图的价值有一半在**交换**：用户写完要给同事看、要导入 XMind、要进版本库。
/// `.mm` 是被多个工具读的开源原生格式；自定义 JSON 只有霜天能读。
/// 实测（209 节点）：`.mm` 15.7KB / gzip 1.6KB，自定义 JSON 10.8KB / gzip 1.5KB——
/// **体积差距可忽略**，而互操作差距是"别人能不能打开"。
/// 详见 `st/md/mindmap.hpp` 的序列化小节。
///
/// ## 交互（这是用户唯一直接感知的部分）
///
/// | 手势 | 行为 |
/// |---|---|
/// | 左键点节点 | 选中（**不展开**——点头展开会让"只想选一下"的人误操作） |
/// | 双击节点 | **进入改名**（内嵌 `Input`，`Enter` 提交 / `Esc` 取消） |
/// | 点折叠徽标 | 展开/折叠该节点的子树 |
/// | **左键拖节点** | 拖到另一节点上 = 改父级；落到空白 = 取消（拖到自己后代上直接拒绝） |
/// | 左键拖空白 | 平移视图 |
/// | 滚轮 | **以指针为锚缩放**（与 `MermaidView`/`ThreeView` 同约定，不需要 Ctrl） |
/// | 双击空白 | 回初始视角 |
/// | `Tab` / `Shift+Tab` | 选中下一/上一个**可见**节点 |
/// | `↑↓←→` | 按几何方向选相邻节点（不是按数组顺序——那样在思维导图里完全是错的） |
/// | `Enter` | 改名 |
/// | `Space` | 折叠/展开 |
/// | `Insert`（或 `Ctrl+Enter`） | 加子节点 |
/// | `Shift+Insert`（或 `Enter` 在选中时的同级语义见 `add_sibling`） | 加同级 |
/// | `Delete` | 删子树（**根不可删**） |
/// | `F2` | 改名 |
/// | `Ctrl+Z` / `Ctrl+Shift+Z`（或 `Ctrl+Y`） | 撤销 / 重做 |
/// | `Ctrl+S` | 存到 `file_path()`（没设路径则触发 `on_save_requested`） |
///
/// 键盘与鼠标**走同一条编辑入口**（`apply_edit`），所以撤销栈不会出现"鼠标操作能撤、
/// 键盘操作撤不了"这类不对称。
///
/// ## 属性面（控制通道 `get` / `set`）
///
/// 可写：`file`（路径；`set` 只记录不读写，真要读用动作 `load`）、`direction`
/// （`right`/`both`）、`font_size`、`zoom`、`fit`、`pan_x`/`pan_y`、`selected`（节点下标）、
/// `editable`、`pan_enabled`、`wheel_zoom`、`show_collapse_badges`。
/// 只读：`node_count`、`visible_count`、`depth`、`selected_text`、`path`（选中节点的
/// 祖先链文字，`/` 分隔）、`error`（最近一次读写失败原因）、`content_width`/`content_height`、
/// `scale`、`dragging`、`editing`、`can_undo`/`can_redo`、`dirty`。
///
/// 动作：`load`/`save`（用 `file` 或参数里的路径；参数省略时用已设路径）、
/// `reset_view`、`fit`、`zoom_in`/`zoom_out`、`expand_all`/`collapse_all`、
/// `select_root`、`add_child`/`add_sibling`/`remove`/`rename`（参数 = 新文字）/
/// `toggle_collapse`、`undo`/`redo`、`new`（重置成单根文档）。
///
/// ## 保存语义：`dirty` 与 `on_save_requested`
///
/// 组件**不自己决定何时写盘**（那是应用的事：可能要先弹"是否保存"、可能写只读文件系统、
/// 可能要把内容另存进沙箱）。所以：
/// - 编辑后 `dirty()` 为真，`Ctrl+S` 与动作 `save` 会**先试** `file_path()`；
/// - 没有路径、或写失败时，调 `on_save_requested`（应用自己弹保存框），
///   **并把失败原因留在 `last_error()` 里**——不静默吞掉。

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"
#include "st/edit/undo.hpp"
#include "st/math/geometry.hpp"
#include "st/md/mindmap.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/component_registry.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"

namespace st::ui {

class Input;

/// 思维导图视图（查看 + 编辑 + 保存）。
class MindMapView : public Element, public edit::CommandContext {
 public:
  static constexpr float kMinZoom{0.15f};
  static constexpr float kMaxZoom{8.0f};
  /// 滚轮缩放每格倍率（与 `MermaidView`/`ThreeView` 同一手感）。
  static constexpr float kWheelZoomStep{1.12f};

  MindMapView();
  ~MindMapView() override;
  MindMapView(const MindMapView&) = delete;
  auto operator=(const MindMapView&) -> MindMapView& = delete;

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "MindMapView"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Panel; }

  // —— 内容 ——

  /// 换整份文档（**会清空撤销历史**：这是"打开了另一份文件"，不是一次编辑）。
  void set_document(md::mindmap::Document document);
  [[nodiscard]] auto document() const noexcept -> const md::mindmap::Document& { return document_; }

  /// 用纯文本建一棵"根 + 若干一级分支"的导图（`text` 的每行一个分支）。
  ///
  /// 存在的理由：XML / JS / 控制通道都只能传字符串。没有这个入口，声明式侧
  /// 就只能"建一个空导图"，而空导图看起来像组件没工作（实测这类误判极常见）。
  void set_source(std::string_view text);
  /// 当前导图的**文字投影**（每行一个节点，按缩进表示层级）。与 `set_source` 配对。
  [[nodiscard]] auto source() const -> std::string;

  // —— 选中与编辑 ——

  /// 选中节点（越界或 `kNoNode` = 清除选中）。
  void set_selected(std::size_t index);
  [[nodiscard]] auto selected() const noexcept -> std::size_t { return selected_; }
  /// 选中的节点（无选中给 `nullptr`）。
  [[nodiscard]] auto selected_node() const noexcept -> const md::mindmap::Node*;

  /// 允许编辑（默认**开**）。关掉后所有编辑动作与键盘编辑键如实拒绝（`invoke` 返回 false）。
  void set_editable(bool editable) noexcept { editable_ = editable; }
  [[nodiscard]] auto editable() const noexcept -> bool { return editable_; }

  /// 进入改名（挂内嵌 `Input`）。返回 false = 节点不存在 / 只读 / 已在下标越界。
  auto begin_rename(std::size_t index) -> bool;
  /// 提交改名（`text` 为空时**改成空串**——用户可能就是要把文字清掉；取消走 `cancel_rename`）。
  auto commit_rename(std::string text) -> bool;
  /// 取消改名（不发变更通知）。
  void cancel_rename();
  [[nodiscard]] auto is_renaming() const noexcept -> bool { return renaming_ != md::mindmap::Node::kNoNode; }
  [[nodiscard]] auto renaming_index() const noexcept -> std::size_t { return renaming_; }

  // —— 文件读写（FreeMind `.mm`）——

  /// 设定当前文件路径（**不读也不写**，只记录；`load`/`save` 动作与 `Ctrl+S` 用它）。
  void set_file_path(std::string path);
  [[nodiscard]] auto file_path() const noexcept -> const std::string& { return file_path_; }

  /// 从 `.mm` 文件读入。失败经 `Status` 如实上报，并在 `last_error()` 留一句人话。
  auto load_file(std::string_view path) -> Status;
  /// 写到 `.mm` 文件。失败同上。
  auto save_file(std::string_view path) -> Status;

  /// 当前文件路径下的读写（路径为空时如实失败 `Invalid`）。
  auto load() -> Status;
  auto save() -> Status;

  /// 换文档/编辑后为真；成功存取后归假。应用据此显示"未保存"标记。
  [[nodiscard]] auto dirty() const noexcept -> bool { return dirty_; }
  void clear_dirty() noexcept { dirty_ = false; }
  /// 最近一次读写失败的原因（空 = 没有失败）。
  [[nodiscard]] auto last_error() const noexcept -> const std::string& { return last_error_; }

  /// 需要应用自己保存时回调（无路径 / 写失败 / 用户按了 `Ctrl+S` 而本组件不该写盘）。
  ///
  /// 参数是**组件自己请求的保存内容**（已序列化好的 `.mm` 文本）：应用可以直接落盘，
  /// 也可以弹一个保存框。为什么不传路径：路径归属是应用的（可能还没选）。
  std::function<void(const std::string& suggested_name, const std::string& content)>
      on_save_requested{};

  /// 文档变更通知（编辑落定后调；`外部`回写 `set_document` **不**触发它）。
  std::function<void()> on_change{};

  // —— 布局与外观 ——

  /// 布局方向（`Right` 逻辑图 / `Both` 经典双向）。
  void set_direction(md::mindmap::LayoutDirection direction);
  [[nodiscard]] auto direction() const noexcept -> md::mindmap::LayoutDirection {
    return options_.direction;
  }
  void set_layout_font_size(float size);
  [[nodiscard]] auto layout_font_size() const noexcept -> float { return options_.font_size; }
  /// 布局选项（改完调 `relayout()` 生效）。
  [[nodiscard]] auto layout_options() -> md::mindmap::LayoutOptions& { return options_; }
  /// 内容尺寸（逻辑像素，已含留白；属性面 `content_width`/`content_height`）。
  [[nodiscard]] auto content_size() const noexcept -> math::Size { return content_size_; }
  /// 当前生效倍率（`fit × zoom`）。
  [[nodiscard]] auto effective_scale() const noexcept -> float { return scale_; }

  // —— 视图变换 ——

  void set_zoom(float zoom);
  [[nodiscard]] auto zoom() const noexcept -> float { return zoom_; }
  /// 是否"缩到装得下"（默认**开**：换文档/新建后立刻能看到全貌）。
  void set_fit(bool fit);
  [[nodiscard]] auto fit() const noexcept -> bool { return fit_; }
  void pan_by(float dx, float dy);
  void set_pan(float x, float y);
  [[nodiscard]] auto pan_x() const noexcept -> float { return pan_x_; }
  [[nodiscard]] auto pan_y() const noexcept -> float { return pan_y_; }
  /// 回到居中、`zoom = 1`（`fit` 仍生效）。
  void reset_view();
  /// 把选中的节点移到可见区域内（键盘导航后必须调，否则选到视口外等于没选）。
  void ensure_selected_visible();
  /// 以某个屏幕点为锚缩放（缩放前后该点下的内容不动）。
  void zoom_at(math::Point anchor, float factor);

  /// 拖拽平移开关（默认开）。
  void set_pan_enabled(bool enabled) noexcept { pan_enabled_ = enabled; }
  [[nodiscard]] auto pan_enabled() const noexcept -> bool { return pan_enabled_; }
  /// 滚轮直接缩放（默认开）。
  void set_wheel_zoom(bool enabled) noexcept { wheel_zoom_ = enabled; }
  [[nodiscard]] auto wheel_zoom() const noexcept -> bool { return wheel_zoom_; }
  /// 拖拽节点改父级（默认开）。
  void set_drag_reparent(bool enabled) noexcept { drag_reparent_ = enabled; }
  [[nodiscard]] auto drag_reparent() const noexcept -> bool { return drag_reparent_; }
  /// 画折叠徽标（默认开）。
  void set_show_collapse_badges(bool enabled) noexcept { show_collapse_badges_ = enabled; }
  [[nodiscard]] auto show_collapse_badges() const noexcept -> bool {
    return show_collapse_badges_;
  }
  /// 是否正在拖（属性面 `dragging`）。
  [[nodiscard]] auto dragging() const noexcept -> bool { return drag_mode_ != DragMode::None; }

  // —— 撤销 / 重做 ——

  [[nodiscard]] auto can_undo() const noexcept -> bool { return undo_.can_undo(); }
  [[nodiscard]] auto can_redo() const noexcept -> bool { return undo_.can_redo(); }
  auto undo() -> bool;
  auto redo() -> bool;
  /// 撤销栈里的条目名（历史面板用）。
  [[nodiscard]] auto undo_entries() const -> std::vector<edit::HistoryEntry> {
    return undo_.entries();
  }

  // —— 批量展开/折叠 ——

  void expand_all();
  void collapse_all();

  // —— Element 接口 ——

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  [[nodiscard]] auto cursor_at(math::Point point) const noexcept -> MouseCursor override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto readonly_property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

  // —— 诊断（自动化/测试）——

  /// 重跑一次布局（改完 `layout_options()` 后调）。
  void relayout();
  /// 最近一次布局结果（内容坐标）。
  [[nodiscard]] auto layout_result() const noexcept -> const md::mindmap::Layout& {
    return layout_;
  }
  /// 内容坐标 → 屏幕坐标（`paint` 与交互反算共用同一份公式）。
  [[nodiscard]] auto content_origin() const -> math::Point;
  /// 屏幕坐标 → 内容坐标。
  [[nodiscard]] auto to_content(math::Point screen) const -> math::Point;
  /// 屏幕点命中的节点（未命中给 `Node::kNoNode`）。
  [[nodiscard]] auto node_at(math::Point screen) const -> std::size_t;
  /// 节点的屏幕矩形（未命中/被折叠隐藏给空矩形）。自动化点节点用它，不靠猜坐标。
  [[nodiscard]] auto node_rect(std::size_t index) const -> math::Rect;
  /// 折叠徽标相对节点框的矩形（内容坐标；`show_collapse_badges` 关掉时为空）。
  [[nodiscard]] auto badge_rect(std::size_t index) const -> math::Rect;

  /// 押着的（已摘下、未释放）改名框数：**0 或 1**。
  ///
  /// 为何要暴露它："提交时不能当场销毁自己"这条不变式，靠指针对不对是测不出来的
  /// （debug 档下读悬垂指针不一定崩——实测回退成“立即 reset”后用例仍绿）。
  /// 可测的是**时序**：提交后那一瞬它必须是**押着**的，而下一次事件之后必须已释放。
  [[nodiscard]] auto retired_input_count() const noexcept -> std::size_t {
    return retiring_input_ == nullptr ? 0U : 1U;
  }

 private:
  /// 拖拽的三种形态（**必须先分清**：混在一起就会"拖节点变成平移视图"）。
  enum class DragMode : std::uint8_t {
    None,
    /// 按在空白处 → 平移视图。
    Pan,
    /// 按在节点上 → 可能的改父级（超过阈值才真进入）。
    Reparent,
  };

  /// 一次可撤销的编辑：**整树快照**。
  ///
  /// 为什么是整树而不是"操作补丁"：树很小（百节点量级），快照成本可忽略；
  /// 而补丁式要为每种操作（加/删/移/改名/折叠）各写一对正反操作，
  /// 任何一对写错都是"撤销后树坏了"——这类缺陷只能在很后面才被发现。
  class SnapshotCommand : public edit::Command {
   public:
    SnapshotCommand(md::mindmap::Document before, md::mindmap::Document after, std::string name);
    void undo(edit::CommandContext& context) override;
    void redo(edit::CommandContext& context) override;
    [[nodiscard]] auto name() const -> std::string override;
    [[nodiscard]] auto is_empty() const -> bool override;

   private:
    md::mindmap::Document before_{};
    md::mindmap::Document after_{};
    std::string name_{};
  };

  /// 统一的编辑入口：改前拍快照 → 跑 `mutate` → 改后拍快照 → 入撤销栈。
  ///
  /// 所有编辑（鼠标/键盘/动作面）都走它，因此撤销栈不会漏掉任何一条路径。
  /// `mutate` 返回 false 表示"没改动"（不产生历史条目）。
  auto apply_edit(std::string_view name, const std::function<bool(md::mindmap::Document&)>& mutate)
      -> bool;

  /// 按当前文档重建布局（`measure` 与 `relayout` 都经它，保证只有一套口径）。
  void rebuild_layout(const md::mindmap::MeasureText& measure);
  /// 视口倍率与居中偏移。
  void update_view();
  /// 布局之后：把内嵌改名框摆回它节点的位置。
  void place_rename_input();
  /// 结束改名（`commit=false` 时丢弃）。**幂等**。
  void finish_rename(bool commit);
  /// 内嵌改名框的父子生命周期（挂/摘）。
  void attach_rename_input();
  /// 摘除内嵌改名框——**只摘不销毁**（见 `.cpp` 里“押到退役区”的说明）。
  void detach_rename_input();
  /// 释放上一轮押下的改名框（下一帧再放，那时已不在它的派发栈里）。
  void flush_retired_input();

  /// 键盘编辑（返回 true = 已消费）。
  auto handle_key(const RenderContext& context, Event& event) -> bool;
  /// 按几何方向找相邻节点（`↑↓←→`）。
  [[nodiscard]] auto neighbor_in_direction(std::size_t from, int dx, int dy) const -> std::size_t;
  /// 全部可见节点的下标（`Tab` 序：先序 = 与绘制顺序一致）。
  [[nodiscard]] auto visible_order() const -> std::vector<std::size_t>;

  // —— 数据与视图状态 ——
  md::mindmap::Document document_{};
  md::mindmap::LayoutOptions options_{};
  md::mindmap::Layout layout_{};
  math::Size content_size_{};
  std::size_t selected_{md::mindmap::Node::kNoNode};

  float zoom_{1.0f};
  bool fit_{true};
  float pan_x_{0.0f};
  float pan_y_{0.0f};
  float scale_{1.0f};

  bool editable_{true};
  bool pan_enabled_{true};
  bool wheel_zoom_{true};
  bool drag_reparent_{true};
  bool show_collapse_badges_{true};

  std::string file_path_{};
  std::string last_error_{};
  bool dirty_{false};

  edit::UndoStack undo_;

  // —— 内嵌改名 ——
  std::size_t renaming_{md::mindmap::Node::kNoNode};
  /// 内嵌输入框（**子元素**，所有权在本元素的 `children_` 里；这里只是裸引用）。
  /// 摘除后必须立即置空——它已被 `remove_child` 交回并押到退役区。
  Input* rename_input_{nullptr};
  /// 已从树上摘下、**还没释放**的改名框（下一帧释放）。
  ///
  /// 为何要它而不是当场 `reset()`：提交是经 `Input::on_submit` 进来的，
  /// 那一刻正在执行的就是它的成员函数——当场销毁会让它在返回途中用已释放的 `this`
  /// （实测：`invoke submit` 直接让整个应用退出，而单测直接调 `commit_rename` 是绿的）。
  std::unique_ptr<Element> retiring_input_{};
  /// 改名框的节点位置（`place_rename_input` 用；存内容坐标，避免每帧查布局）。
  math::Rect rename_anchor_{};

  // —— 拖拽 ——
  ///
  /// 三种形态用一个状态机：按在节点上 = `Reparent`（超过 `kDragThreshold` 才真算拖），
  /// 按在空白 = `Pan`。分不清的后果是"想拖节点改父级，结果整个视图平移了"。
  static constexpr float kDragThreshold{4.0f};
  DragMode drag_mode_{DragMode::None};
  math::Point drag_origin_{};
  math::Point drag_current_{};
  float drag_pan_x_{0.0f};
  float drag_pan_y_{0.0f};
  std::size_t drag_node_{md::mindmap::Node::kNoNode};
  /// 拖拽中当前悬停的目标（画高亮用；`kNoNode` = 无处可落）。
  std::size_t drag_target_{md::mindmap::Node::kNoNode};
  /// 已经越过阈值（区分"点一下"与"拖一下"）。
  bool drag_active_{false};

  // —— 主题色（`apply_theme` 填，绘制只读）——
  math::Color surface_{};
  math::Color border_{};
  math::Color text_color_{};
  math::Color text_muted_{};
  math::Color link_color_{};
  math::Color accent_{};
  math::Color selection_fill_{};
  math::Color root_fill_{};
  math::Color depth_fill_{};
  std::vector<math::Color> branch_colors_{};

  /// 间距与尺寸（从 `options_.font_size` 派生，改字号时自动跟随）。
  float line_height_{0.0f};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::MindMapView, "MindMapView")

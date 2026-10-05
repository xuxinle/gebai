#pragma once

/// 组件树核心：Element 基类（布局/绘制/事件/语义）+ 语义树与视觉树快照（控制通道 tree/visual 用）。
/// 设计原则：组件是**自绘**的——没有系统控件，全部像素由 renderer 产生，外观跨平台一致。

#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/style.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"

namespace st::ui {

using ElementId = std::string;

/// 语义角色（无障碍与自动化对齐；控制通道 `tree`/`find` 的选择维度）。
enum class Role : std::uint8_t {
  None,
  Panel,
  Text,
  Heading,
  Button,
  Link,
  Icon,
  TextInput,
  TextArea,
  Checkbox,
  Radio,
  Switch,
  Slider,
  Select,
  List,
  ListItem,
  Table,
  Cell,
  Tab,
  ProgressBar,
  Dialog,
  Tooltip,
  ScrollBar,
  Separator,
  Image,
  Markdown,
  Code,
  Tree,
  TreeItem,
  Menu,
  MenuItem,
};

[[nodiscard]] constexpr auto to_string(Role role) noexcept -> std::string_view {
  switch (role) {
    case Role::None: return "none";
    case Role::Panel: return "panel";
    case Role::Text: return "text";
    case Role::Heading: return "heading";
    case Role::Button: return "button";
    case Role::Link: return "link";
    case Role::Icon: return "icon";
    case Role::TextInput: return "text_input";
    case Role::TextArea: return "text_area";
    case Role::Checkbox: return "checkbox";
    case Role::Radio: return "radio";
    case Role::Switch: return "switch";
    case Role::Slider: return "slider";
    case Role::Select: return "select";
    case Role::List: return "list";
    case Role::ListItem: return "list_item";
    case Role::Table: return "table";
    case Role::Cell: return "cell";
    case Role::Tab: return "tab";
    case Role::ProgressBar: return "progress_bar";
    case Role::Dialog: return "dialog";
    case Role::Tooltip: return "tooltip";
    case Role::ScrollBar: return "scroll_bar";
    case Role::Separator: return "separator";
    case Role::Image: return "image";
    case Role::Markdown: return "markdown";
    case Role::Code: return "code";
    case Role::Tree: return "tree";
    case Role::TreeItem: return "tree_item";
    case Role::Menu: return "menu";
    case Role::MenuItem: return "menu_item";
  }
  return "none";
}

inline constexpr std::string_view kFlagTrue = "true";

struct SemanticsFlags {
  bool visible{true};
  bool enabled{true};
  bool focused{false};
  bool hovered{false};
  bool pressed{false};
  bool selected{false};
  bool checked{false};
  bool scrollable{false};
  bool editable{false};
};

/// 语义树节点（`tree` 协议输出；不含绘制信息）。
struct SemanticsNode {
  ElementId id{};
  std::string type{};
  Role role{Role::None};
  math::Rect bounds{};
  std::string text{};
  std::string value{};
  SemanticsFlags flags{};
  std::vector<SemanticsNode> children{};
};

/// 视觉树节点（`visual` 协议输出：实际画了什么、命中区在哪）。
struct VisualNode {
  ElementId id{};
  std::string type{};
  math::Rect bounds{};
  bool visible{true};
  float opacity{1.0f};
  std::string fill{};      ///< 主填充（css 字符串，如 `#2563eb`）
  float radius{0.0f};
  std::string text{};      ///< 该层绘制的文本（若有）
  /// 该层**文本实际用了什么颜色/字号/字重**（`style_.color` 的 css 串）。
  ///
  /// 为何必须上报：文本颜色是**像素里才看得见、语义树里根本没有**的属性——
  /// 不给它一个读数口，“标红生效了吗”就只能靠截图逐像素猜（脆且慢）。
  /// 白底黑字时还要区分“未设”与“黑白”——所以用**空串**表示“本层不画文本”。
  /// （2026-10-05 补：DSL `BoxProps::color/size/weight` 就是靠它做回归断言的。）
  std::string text_color{};
  float font_size{0.0f};
  std::string font_weight{};   ///< `regular`/`medium`/`semi_bold`/`bold`
  bool hit_target{false};  ///< 是否参与命中测试
  std::uint32_t depth{0};
  std::vector<VisualNode> children{};
};

/// 字重的稳定短名（视觉树上报与控制通道用；不要拿枚举序号，那会随枚举顺序变）。
[[nodiscard]] constexpr auto to_string(FontWeight weight) noexcept -> std::string_view {
  switch (weight) {
    case FontWeight::Regular: return "regular";
    case FontWeight::Medium: return "medium";
    case FontWeight::SemiBold: return "semi_bold";
    case FontWeight::Bold: return "bold";
  }
  return "regular";
}

/// 布局约束。
struct Constraints {
  float max_width{kUnbounded};
  float max_height{kUnbounded};
  float available_width{kUnbounded};
  float available_height{kUnbounded};
};

/// 「填满父级」的组件在测量时该取什么宽/高。
///
/// ⚠ 为什么必须有这个函数：`max_width` 是**上界**，而行布局给子节点的上界是
/// `kUnbounded`（1e9）——标题栏/窗框这类"铺满父级"的组件直接拿它当尺寸，
/// 就会量出天文数字（父容器再把整行撞爆，表现为布局整个错位）。
/// `available_*` 才是父级**实际可用**的尺寸，这正是该取的值。
/// 两个字段的语义差异很容易被忽略（它们在列布局里恰好相等），故收成一个函数
/// 并在注释里写清楚。
[[nodiscard]] constexpr auto fill_width(const Constraints& constraints, float explicit_width,
                                       float fallback = 0.0f) noexcept -> float {
  if (explicit_width >= 0.0f && explicit_width != kAuto) return explicit_width;
  if (constraints.max_width < kUnbounded) return constraints.max_width;
  if (constraints.available_width < kUnbounded) return constraints.available_width;
  return fallback;
}

[[nodiscard]] constexpr auto fill_height(const Constraints& constraints, float explicit_height,
                                        float fallback = 0.0f) noexcept -> float {
  if (explicit_height >= 0.0f && explicit_height != kAuto) return explicit_height;
  if (constraints.max_height < kUnbounded) return constraints.max_height;
  if (constraints.available_height < kUnbounded) return constraints.available_height;
  return fallback;
}

enum class EventKind : std::uint8_t {
  MouseMove,
  MouseDown,
  MouseUp,
  Click,
  DoubleClick,
  TripleClick,
  Wheel,
  KeyDown,
  KeyUp,
  TextInput,
  FocusIn,
  FocusOut,
  HoverIn,
  HoverOut,
};

struct Event {
  EventKind kind{EventKind::MouseMove};
  math::Point position{};
  int button{0};
  int click_count{1};
  float wheel_delta{0.0f};
  std::string key{};
  std::string code{};
  std::string text{};
  bool ctrl{false};
  bool shift{false};
  bool alt{false};
  bool meta{false};
  bool handled{false};
};

/// 命中测试与绘制所需的渲染环境。
struct RenderContext {
  const Theme& theme;
  const TextPort* text{nullptr};  ///< 空时退化为 `NullTextPort`
  double time_seconds{0.0};

  /// 本帧**实际执行了绘制**的元素数（视口剔除会缩小它）。
  ///
  /// 为什么要暴露它：视口剔除这类优化的正确性没法靠"看截图"证明——
  /// 而本应用跨运行的像素**不是确定的**（悬浮过渡的冻结值依赖帧时序，实测同一实例
  /// 连拍两次仍有上千像素差异），所以"A/B 截图逐像素比对"这条路本身不可用。
  /// 计数是可复现的结构性证据：既能证明"屏幕外确实没画"，
  /// 也能配合"每个元素自己声明是否在视口内"来证明"该画的都画了"。
  std::uint64_t* painted_elements{nullptr};
};

class Element {
 public:
  Element();
  virtual ~Element();
  Element(const Element&) = delete;
  auto operator=(const Element&) -> Element& = delete;

  // —— 标识与类型 ——
  [[nodiscard]] auto id() const noexcept -> const ElementId& { return id_; }
  void set_id(ElementId id) { id_ = std::move(id); }

  /// 稳定逻辑身份（可选）：同一逻辑元素在**重建/重排后仍拿到同一个 id**。
  ///
  /// 自动 id 是路径式的（`Type[index]`），索引会随插入/删除/筛选整体位移，于是
  /// 「列表刷新后原来看中的那一项变成别的东西」。显式 `set_id` 能解决，但要求调用方
  /// 同时维护「元素到 id」的映射、重建时把 id 重新贴回去，容易漏。
  ///
  /// `key` 与之互补：只描述「这个元素在兄弟之间是谁」（业务身份，如任务 id），
  /// 路径拼装、去重、选择器安全性交给框架。带 key 的自动 id 形如 `tasks/ListItem@task-42`。
  [[nodiscard]] auto key() const noexcept -> const std::string& { return key_; }
  void set_key(std::string key) { key_ = std::move(key); }
  [[nodiscard]] virtual auto type() const noexcept -> std::string_view { return "Element"; }
  [[nodiscard]] virtual auto role() const noexcept -> Role { return Role::None; }

  // —— 树结构 ——
  [[nodiscard]] auto parent() const noexcept -> Element* { return parent_; }
  /// 路径式自动 id（未显式设置时由 UiRoot 在挂载时生成）。
  [[nodiscard]] auto derived_id() const -> ElementId;
  [[nodiscard]] auto children() const noexcept -> std::span<const std::unique_ptr<Element>> {
    return children_;
  }
  [[nodiscard]] auto children() noexcept -> std::span<std::unique_ptr<Element>> { return children_; }
  /// 追加子元素（**载体内部件在后**：如 `ScrollView` 重写它把内容插到滚动条之前）。
  virtual auto add_child(std::unique_ptr<Element> child) -> Element*;
  /// 在指定位置插入子元素（同上：少数组件会重写以保持内部件位置）。
  virtual auto insert_child(std::size_t index, std::unique_ptr<Element> child) -> Element*;
  auto remove_child(Element* child) -> std::unique_ptr<Element>;
  void clear_children();
  [[nodiscard]] auto child_count() const noexcept -> std::size_t { return children_.size(); }
  [[nodiscard]] auto child_at(std::size_t index) const noexcept -> Element*;

  /// **载体子元素数**（默认 1——每个子元素都是调用方的）。
  ///
  /// 少数组件会把自己的内部件放进 `children_`（如 `ScrollView` 的滚动条），
  /// 它们**不归调用方管**：声明式的「位置对齐 + 末尾裁剪」必须跳过它们，
  /// 否则会把自己的滚动条当成「上一帧多声明的残留」移除掉——
  /// 而组件持有的 `bar_` 裸指针随即悬垂（实测：声明式里 ScrollView 与 List
  /// 分支互切 → `ScrollView::arrange` 在 `bar_->arrange` 上段错误）。
  ///
  /// 子元素布局约定：**调用方的子元素在前，载体内部件在后**（本仓库统一遵守）。
  [[nodiscard]] virtual auto content_child_count() const noexcept -> std::size_t {
    return children_.size();
  }

  /// 宿主（`UiRoot`）的非拥有指针，未上树时为 `nullptr`。
  ///
  /// 用途：子树里的组件需要主动**改变焦点**时（例如命令面板要求“打开后键盘直达
  /// 过滤框”），而焦点簿记归根所有——`UiRoot::set_focus` 是唯一入口（它要发
  /// FocusOut/FocusIn 并维护 Tab 环）。直接把 `UiRoot` 写进头文件会造成
  /// `element.hpp ↔ ui_root.hpp` 循环依赖，因此这里存**类型擦除的 `void*`**，
  /// 由 `UiRoot` 在挂载/摘除时维护；需要根的子组件在自己的 .cpp 里包含
  /// `ui_root.hpp` 并强转（依赖方向仍然单向）。`owner_as<T>()` 把这步收成一行。
  [[nodiscard]] auto owner() const noexcept -> void* { return owner_; }
  void set_owner(void* owner) noexcept { owner_ = owner; }
  template <typename T>
  [[nodiscard]] auto owner_as() const noexcept -> T* {
    return static_cast<T*>(owner_);
  }

  // —— 样式 ——
  [[nodiscard]] auto style() noexcept -> Style& { return style_; }
  [[nodiscard]] auto style() const noexcept -> const Style& { return style_; }
  void set_style(Style style) { style_ = std::move(style); }
  [[nodiscard]] auto visible() const noexcept -> bool { return visible_; }
  void set_visible(bool visible) noexcept { visible_ = visible; }
  [[nodiscard]] auto enabled() const noexcept -> bool { return enabled_; }
  void set_enabled(bool enabled) noexcept { enabled_ = enabled; }
  [[nodiscard]] auto focusable() const noexcept -> bool { return focusable_; }
  void set_focusable(bool value) noexcept { focusable_ = value; }
  [[nodiscard]] virtual auto hit_test(math::Point) const noexcept -> bool;

  // —— 交互状态 ——
  /// 悬浮"特效"的声明式开关（组件自己决定怎么用）。
  ///
  /// 为什么做成声明而不是让组件各自硬编码：悬浮反馈是**跨组件的一致性**问题
  /// （按钮、列表项、表格行、标签页若各写一套，观感一定会散）。
  /// 这里定义"可以有哪些效果"，具体参数由主题令牌给，组件只声明要哪几项。
  struct HoverEffect {
    bool enabled{false};       ///< 总开关
    bool background{true};     ///< 背景提亮/变色
    bool border{false};        ///< 描边变色
    bool lift{true};           ///< 上浮（配合阴影，产生"抬起"感）
    bool glow{false};          ///< 外发光（强调悬浮焦点）
    bool cursor{true};         ///< 手型光标（窗口后端可据此设置）
  };

  [[nodiscard]] auto hovered() const noexcept -> bool { return hovered_; }
  [[nodiscard]] auto hover_effect() const noexcept -> const HoverEffect& { return hover_effect_; }
  void set_hover_effect(HoverEffect effect) noexcept { hover_effect_ = effect; }
  /// 便捷设置：开启悬浮特效（默认子项）。
  void set_hover_enabled(bool enabled = true) noexcept { hover_effect_.enabled = enabled; }

  /// 悬浮回调（进入/离开各触发一次）。
  /// 与 `EventKind::HoverIn/HoverOut` 事件并行提供：事件适合统一处理（如状态栏），
  /// 回调适合"这个按钮悬浮时要做什么"这类局部逻辑，两者用途不同。
  void set_on_hover(std::function<void(bool)> callback) { on_hover_ = std::move(callback); }
  [[nodiscard]] auto has_hover_callback() const noexcept -> bool {
    return static_cast<bool>(on_hover_);
  }
  /// 触发悬浮回调（由 UiRoot 在派发 HoverIn/HoverOut 时调用）。
  void notify_hover(bool hovered) {
    if (on_hover_) on_hover_(hovered);
  }

  /// 推进悬浮动画并返回进度（0 = 完全未悬浮，1 = 完全悬浮）。
  ///
  /// 与 `Switch` 的开关动画同一套做法：没用独立的 tick 钩子，而是在 `paint()` 里
  /// 用 `context.time_seconds` 推进——少一条需要应用驱动的生命周期，也就不存在
  /// "忘了 tick 所以动画不动"这类问题。动画期间会 `mark_dirty` 让下一帧继续。
  auto advance_hover(const RenderContext& context) const -> float {
    const float target = hovered_ ? 1.0f : 0.0f;
    const double duration = context.theme.metrics().hover_duration;
    const double now = context.time_seconds;
    // `advancing`：时间在走吗？静态帧（首帧/离屏单帧/测试）直接到位，
    // 免得画面停在半程。这条与 `Switch::knob_progress` 同一口径。
    const bool advancing = now > last_hover_time_;
    last_hover_time_ = now;

    // ⚠ 这里必须严格区分"正在过渡"与"已经静止"。
    // 早先的写法是"只要 elapsed < 1 就声明 animating"，而静止元素每帧都会把
    // `hover_start_` 重置为当前时间 → elapsed 恒为 0 → **永久声明 animating**
    // → 根节点每帧都脏 → 应用 100% 占一个核（实测 6 秒耗 6.12 秒 CPU）。
    // 忙循环对"帧耗时基准"是不可见的，所以当时没被测出来。
    if (hover_start_ >= 0.0) {   // —— 过渡中 ——
      if (duration <= 0.0 || !advancing) {
        hover_t_ = target;
        hover_start_ = -1.0;
        hover_animating_ = false;
        return hover_t_;
      }
      const double progress = (now - hover_start_) / duration;
      if (progress >= 1.0) {
        hover_t_ = target;
        hover_start_ = -1.0;
        hover_animating_ = false;
      } else {
        hover_t_ = hover_from_ + (target - hover_from_) * static_cast<float>(progress);
        hover_animating_ = true;   // 只在这一支声明"还要下一帧"
        // 同时上报损坏区：过渡帧只需重画该元素那块（增量重绘；否则下一帧
        // 损坏区为空 → 回落整帧，实测悬停过渡仍然每帧全屏）。
        record_damage();
      }
      return hover_t_;
    }

    // —— 静止：只有目标与当前值不同才启动过渡 ——
    if (hover_t_ != target) {
      if (duration <= 0.0 || !advancing) {
        hover_t_ = target;   // 静态帧直接落位（不卡半程）
        hover_animating_ = false;
        return hover_t_;
      }
      hover_from_ = hover_t_;   // 从**当前视觉进度**接着动（不跳变）
      hover_start_ = now;
      hover_animating_ = true;
      record_damage();
      return hover_t_;
    }
    hover_animating_ = false;   // 真正静止：不请求下一帧
    return hover_t_;
  }  /// 当前悬浮进度（不推进，只读；用于布局等非绘制阶段）。
  [[nodiscard]] auto hover_progress() const noexcept -> float { return hover_t_; }
  [[nodiscard]] auto pressed() const noexcept -> bool { return pressed_; }
  [[nodiscard]] auto focused() const noexcept -> bool { return focused_; }
  void set_hovered(bool value) noexcept { hovered_ = value; }
  void set_pressed(bool value) noexcept { pressed_ = value; }
  virtual void set_focused(bool value) noexcept { focused_ = value; }

  // —— 布局与绘制 ——
  [[nodiscard]] auto bounds() const noexcept -> math::Rect { return bounds_; }
  [[nodiscard]] auto measured_size() const noexcept -> math::Size { return measured_; }
  /// 从主题刷新 `style_`（UiRoot 在布局前按脏标记调用）：组件据此把 token 落到具体样式。
  ///
  /// ⚠ 覆写时**必须把显式覆盖放回去**（见 `apply_text_overrides`）：
  /// `apply_theme` 是每帧从主题重算颜色/字号/字重的地方，而 DSL 的
  /// `BoxProps::color/hex_color/size/weight` 是“盖过主题”的显式意图——
  /// 不调本函数就是一个“设了颜色，首帧对、下一帧被主题盖回”的隐形 bug。
  virtual void apply_theme(const Theme& theme) { (void)theme; apply_text_overrides(); }

  // —— 排版覆盖（DSL `BoxProps` 的落点）——
  //
  // 为什么放在基类而不是每个组件各写一份：要盖过主题的是**任何会画文字的元素**
  // （Text/Heading/Button/Badge/List 项…），而在每个 `apply_theme` 里重抄一遍
  // “有没有显式覆盖”的判定，漏一个就是那个组件静默不生效。
  // 这里把「意图」与「落位」分开：调用方设一次意图（set_text_*），
  // `apply_theme` 末尾统一回放（`apply_text_overrides`）。

  /// 设文本色（显式值，盖过主题的 tone 映射）。已存的**色调覆盖被清除**（二者互斥）。
  void set_text_color(math::Color color) {
    text_color_override_ = color;
    text_tone_override_.reset();
    mark_dirty();
  }
  /// 设文本色为**主题语义色调**（跟着主题走；首选方式）。已存的字面色覆盖被清除。
  void set_text_tone(Tone tone) {
    text_tone_override_ = tone;
    text_color_override_.reset();
    mark_dirty();
  }
  /// 设字号（显式值，盖过主题的 `font_base`）。
  void set_text_size(float size) {
    text_size_override_ = size;
    mark_layout_dirty();   // 字号变 → 度量变（不只是重绘）
  }
  /// 设字重（显式值）。
  void set_text_weight(FontWeight weight) {
    text_weight_override_ = weight;
    mark_dirty();
  }
  [[nodiscard]] auto text_color_override() const noexcept -> const std::optional<math::Color>& {
    return text_color_override_;
  }
  [[nodiscard]] auto text_tone_override() const noexcept -> const std::optional<Tone>& {
    return text_tone_override_;
  }
  [[nodiscard]] auto text_size_override() const noexcept -> float { return text_size_override_; }
  [[nodiscard]] auto text_weight_override() const noexcept -> const std::optional<FontWeight>& {
    return text_weight_override_;
  }
  /// 把显式排版覆盖回放进 `style_`（`apply_theme` 末尾调；没有覆盖就不动）。
  void apply_text_overrides() {
    if (text_color_override_.has_value()) style_.color = *text_color_override_;
    if (text_size_override_ >= 0.0f) style_.font_size = text_size_override_;
    if (text_weight_override_.has_value()) style_.font_weight = *text_weight_override_;
  }
  /// 清掉全部显式排版覆盖（回到纯主题；测试与“恢复默认”用）。
  void clear_text_overrides() {
    text_color_override_.reset();
    text_tone_override_.reset();
    text_size_override_ = -1.0f;
    text_weight_override_.reset();
    mark_layout_dirty();
  }
  /// 计算自身尺寸（写入 `measured_`）；容器组件需递归测量子节点。
  virtual void measure(const RenderContext& context, const Constraints& constraints);
  /// 应用最终矩形并布局子节点。
  virtual void arrange(const RenderContext& context, math::Rect rect);
  /// 绘制自身与子节点（坐标已由 arrange 定好，直接画到画布绝对坐标）。
  virtual void paint(const RenderContext& context, raster::Surface& canvas) const;
  /// 子类绘制自身内容（在 paint_box 之后、子节点之前）。
  ///
  /// **坐标系**：画布是**视口绝对坐标**——自绘几何必须从 `bounds_.x/y` 起算
  /// （`canvas.fill_rect({bounds_.x + …, bounds_.y + …})`），不得按局部坐标画：
  /// 按局部坐标会整块位移（实测：自绘树按局部坐标画 → 上移 70px 压住标题）。
  /// 基线接口（`paint_box`/`paint_text`/子节点）已按 `bounds_` 落位，只有自己的几何需手动偏移。
  virtual void paint_content(const RenderContext& context, raster::Surface& canvas) const {
    (void)context;
    (void)canvas;
  }

  // —— 事件 ——
  /// 事件处理：返回 true = **已消费**（冒泡停止，UiRoot 不再派给祖先/焦点链/快捷键），
  /// false = 未处理（继续冒泡到父级；到达根后进入 Tab 焦点环等全局语义）。
  /// 组件层契约：只对**自己认识的键/事件**返回 true——未识别的组合键必须放行冒泡，
  /// 否则全局快捷键（如 Ctrl+S 保存）没有落点。
  virtual auto on_event(const RenderContext& context, Event& event) -> bool;
  /// 键盘激活（Enter/Space）。
  virtual void activate() {}
  /// 本元素（含子树）是否参与命中拦截：`hit_test` 与浮层键盘派发都先问它。
  /// 默认 `true`；「逻辑上在场但不应拦截输入」的形态（隐藏浮层、透明遮罩）覆写为
  /// `false` 或返回 `visible()`——不可见的浮层不再截住下层内容。
  [[nodiscard]] virtual auto intercepts_input() const noexcept -> bool { return true; }
  /// 行为注入：在组件自身实现**之后**、冒泡**之前**追加一次回调（免子类化的小交互，
  /// 如「拖拽把手改宽度」）。返回 true = 已消费（冒泡停止）。仅对直接派发到本元素的
  /// 事件调用（祖先/后代的不经过本 handler）。
  void set_event_handler(std::function<bool(Event&)> handler) { event_handler_ = std::move(handler); }
  [[nodiscard]] auto has_event_handler() const noexcept -> bool {
    return static_cast<bool>(event_handler_);
  }

  // —— 语义与视觉快照 ——
  [[nodiscard]] virtual auto semantics_text() const -> std::string { return {}; }
  [[nodiscard]] virtual auto semantics_value() const -> std::string { return {}; }
  [[nodiscard]] virtual auto semantics_flags() const -> SemanticsFlags;
  void collect_semantics(SemanticsNode& node) const;
  void collect_visual(VisualNode& node) const;

  // —— 通用属性面（控制通道 get/set 用；组件按需覆写）——
  /// 读取自定义属性（返回空表示不支持）。
  [[nodiscard]] virtual auto get_property(std::string_view name) const
      -> std::optional<std::string>;
  /// 写入自定义属性；返回是否生效。
  virtual auto set_property(std::string_view name, std::string_view value) -> bool;
  /// 组件暴露的可读写属性名。
  [[nodiscard]] virtual auto property_names() const -> std::vector<std::string_view>;
  /// 动作触发（`invoke`；返回是否已处理）。
  [[nodiscard]] virtual auto invoke_action(std::string_view action, std::string_view argument)
      -> bool;

  /// 标记需要重新布局/重绘（由 UiRoot 消费）。
  void mark_dirty();
  /// 本元素是否还有未跑完的悬浮过渡（每帧绘制时续期，由 `UiRoot` 汇总消费）。
  ///
  /// 存在的理由：`UiRoot::clear_dirty()` 在每帧绘制后清脏标记，而过渡要靠
  /// "还有人在动"才能拿到下一帧——否则**动画会停在第一帧**（淡入只走一格，
  /// 看起来像"卡住不动"）。这里的标记是 mutable：绘制是 const 方法（与
  /// `Switch::toggle_time_` 同一套做法）。
  /// 元素是否可以主动请求"再来一帧"（动画）。
  ///
  /// 为什么做成**协议**而不是各组件各写一份：帧预算只有一份，
  /// 谁在动就得由同一个机制汇总——否则会出现"某组件自己在 paint 里标脏，
  /// 绕过了汇总"这类看不见的循环（实测踩过：逐像素写 GPU 画布导致主线程打满）。
  /// 规则很硬：**只有真正还在动的元素才请求**，静态时必须停下。
  ///
  /// 同时上报损坏区：动画元素下一帧只需重画它自己那块（增量重绘）。
  void request_animation() const noexcept {
    animation_requested_ = true;
    record_damage();
  }
  [[nodiscard]] auto animation_requested() const noexcept -> bool {
    return animation_requested_ || hover_animating_;
  }
  void clear_animation_request() const noexcept {
    animation_requested_ = false;
    hover_animating_ = false;
  }
  /// 标记“本元素（及其祖先）需要重新 measure/arrange”。
  ///
  /// **虚函数**，子组件可收紧冒泡范围：默认实现沿 `parent_` 链把标记写到所在树的根
  /// （根上的标记是 `UiRoot::tree_layout_dirty()` 的 O(1) 汇总），而 `UiRoot::layout()`
  /// 一旦真的跑起来就会 `pending_full_ = true`——**整帧重绘**。
  ///
  /// 因此“几何只取决于自身”的组件（如 `CodeEditor`：行高/行宽都是惰性重算的）应当
  /// 覆写成**不冒泡**——否则每一次编辑都会拖出一次整帧重绘（实测 1280×800 下 11.5 ms，
  /// 增量重绘形同虚设；控制通道表现为写操作 15.9 ms vs 读操作 4.1 ms）。
  virtual void mark_layout_dirty();
  [[nodiscard]] auto dirty() const noexcept -> bool { return dirty_; }
  [[nodiscard]] auto layout_dirty() const noexcept -> bool { return layout_dirty_; }
  void clear_dirty() noexcept;

  // —— 增量重绘：元素级损坏区上报 ——
  //
  // 元素没有 UiRoot 反指，只能把"我变了"沿 parent 链**累积到所在树的根元素**上
  // （content 根或 overlay 根），由 UiRoot 在每帧开始时收集消费（见 `paint_frame`）。
  // 上报的矩形 = `bounds + 上次绘制时的安全外扩`（阴影/发光会画到盒子外，
  // 外扩量就是 `paint_margin`；从未绘制过的元素外扩未知——保守要求整帧）。

  /// 损坏上报结果（UiRoot 收集用）。
  struct DamageReport {
    math::Rect rect{};         ///< 需要重绘的逻辑区域（已并入绘制外扩）
    bool valid{false};         ///< 有无累积
    bool needs_full{false};    ///< 外扩未知（从未绘制过）→ 整帧重绘兜底
  };
  /// 消费累积的损坏区（调用后清零；mutable：由 UiRoot 在帧首收集，元素只负责记录）。
  [[nodiscard]] auto take_damage() const noexcept -> DamageReport;
  /// 把自己的损坏区记到所在树的根元素（内部用；`mark_dirty`/`request_animation` 调用）。
  void record_damage() const noexcept;

 protected:
  /// 子类绘制自身的"盒子"（背景/边框/圆角/阴影）。
  void paint_box(const RenderContext& context, raster::Surface& canvas) const;
  /// 子类绘制文本（自动按 `text_align` 定位）。
  auto paint_text(const RenderContext& context, raster::Surface& canvas, std::string_view text,
                  math::Rect box) const -> void;
  /// 布局子节点（Flex 子集：方向/间距/增长/对齐）。
  void layout_children(const RenderContext& context, math::Rect content);
  [[nodiscard]] auto content_box() const noexcept -> math::Rect;

  Style style_{};
  math::Rect bounds_{};
  math::Size measured_{};
  ElementId id_{};
  std::string key_{};
  Element* parent_{nullptr};
  std::vector<std::unique_ptr<Element>> children_{};
  bool visible_{true};
  bool enabled_{true};
  bool focusable_{false};
  bool hovered_{false};
  HoverEffect hover_effect_{};
  std::function<void(bool)> on_hover_{};
  /// 行为注入回调（`set_event_handler`；组件实现之后、冒泡之前调用）。
  std::function<bool(Event&)> event_handler_{};
  /// 悬浮过渡状态（mutable：绘制是 const 方法，与 `Switch::toggle_time_` 同一套做法）。
  mutable float hover_t_{0.0f};
  mutable float hover_from_{0.0f};
  mutable double hover_start_{-1.0};    ///< 过渡起点时间；`-1` = 未在过渡中
  /// 上一帧的时间戳。初值 0 与 `Switch::last_time_` 同口径：
  /// 静态首帧（time=0）要判成"时间没在走"→ 直接落位，否则画面会停在过渡起点。
  mutable double last_hover_time_{0.0};
  mutable bool hover_animating_{false};
  /// 组件主动请求的续帧（与悬浮过渡共用汇总链路）。
  mutable bool animation_requested_{false};
  bool pressed_{false};
  bool focused_{false};
  bool dirty_{true};
  bool layout_dirty_{true};
  /// 上次绘制时的安全外扩（`paint_margin`；-1 = 尚未绘制过 → 损坏区按整帧兜底）。
  mutable float paint_margin_hint_{-1.0f};
  /// 累积的损坏区（记录在**所在树的根元素**上；UiRoot 帧首取走）。
  mutable math::Rect damage_{};
  mutable bool damage_valid_{false};
  mutable bool damage_needs_full_{false};
  /// 宿主（`UiRoot`），由 UiRoot 在挂载/摘除时维护；未上树为 nullptr。
  void* owner_{nullptr};

  /// 排版显式覆盖（DSL `BoxProps` 的落点）——缺省表示“不干预主题”。
  /// 色值用两个 `optional`（互斥：设一个清另一个），于是“最后设的那个生效”不需要额外排序逻辑。
  std::optional<math::Color> text_color_override_{};
  std::optional<Tone> text_tone_override_{};
  float text_size_override_{-1.0f};
  std::optional<FontWeight> text_weight_override_{};
};

/// 便捷容器：行/列布局面板。
class Panel : public Element {
 public:
  explicit Panel(FlexDirection direction = FlexDirection::Column);
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Panel"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Panel; }
};

/// 空占位（用于弹性间距）。
class Spacer : public Element {
 public:
  explicit Spacer(float size = 0.0f);
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Spacer"; }
};

}  // namespace st::ui

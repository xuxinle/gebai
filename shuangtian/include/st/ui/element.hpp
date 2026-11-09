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
#include "st/ui/animation.hpp"
#include "st/ui/property_schema.hpp"
#include "st/ui/style.hpp"
#include "st/ui/styling.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"

namespace st::ui {

/// 前向声明：`HostFocus` 的浮层接口要用到 `OverlayLayout`（原为 `UiRoot` 的嵌套枚举，
/// 嵌套枚举在仅前向声明 `UiRoot` 时不可用——提为顶层，`UiRoot` 复用）。
class UiRoot;

/// 叠加层排布形态（`HostFocus::host_add_overlay` 与 `UiRoot::add_overlay` 共用）：
enum class OverlayLayout { Stack, FillViewport };

/// 元素基类……
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
  /// **窗口级**指针离开（指针滑出整个客户区，不是离开某个元素）。
  ///
  /// 与 `HoverOut` 的分工（两者语义不同层，混用会互相干扰）：
  /// - `HoverIn/HoverOut` 是**元素级**事件：UiRoot 在命中目标切换时派发给元素，
  ///   元素自己决定悬浮视觉；
  /// - `WindowLeave` 是**窗口级**事实：由后端产生（Win32 `WM_MOUSELEAVE`、
  ///   X11 `LeaveNotify`、Wayland `pointer_leave`），UiRoot 据此**一次性清理**
  ///   全部指针态（hover、光标形态、合并中的移动）。
  ///   它**不会**派发给元素——元素不需要知道“指针出窗”与“指针移到我之外”的区别，
  ///   两者都表现为自己的 HoverOut。
  ///
  /// 为何必须存在：指针出窗后不再有 MouseMove，`hovered_` 与光标会卡在最后命中的
  /// 元素上（高亮不灭、手型光标被带出窗口）。后端检测到离开后发本事件，
  /// 由 UiRoot 统一收尾。
  WindowLeave,
  /// **输入法组合中**（IME preedit）。`text` = 预编辑串，`preedit_cursor` = 串内光标（码点偏移）。
  ///
  /// 为何不把预编辑当成 `TextInput` 直接插入文本：预编辑是**尚未确定**的内容——
  /// 用户按 Esc 会丢弃、按空格选字会整体替换成别的串。若每次组合都 `insert_text`，
  /// 一次输入就会进撤销栈几十次，而且“取消”无法回退（因为已经落盘）。
  /// 所以它是一条**独立的、非持久的显示态**：组件只把它画在光标处，
  /// 真正的提交仍走 `TextInput`（后端在 `GCS_RESULTSTR` 时发一条）。
  ///
  /// 空串 = 组合结束（无东西可画）。
  ImeComposition,
};

/// 点击族：一次按放收尾时那**一条**事件（按连击数升到 Click/DoubleClick/TripleClick）。
///
/// ## 为何需要这个谓词（实测代价）
///
/// 窗口后端保证：**每一次物理按放恰好产生一条**点击族事件
/// （唯一真源是 `src/shell/click_gesture.hpp`）。因此“按一下就激活一次”的控件
/// 必须对三种**同等**处理——只认 `Click` + `DoubleClick` 的控件会让“连点三下”的
/// 第三下**静默丢失**：实测 `Button` 三连点只触发 2 次，用户看到的是“第三下没反应”。
///
/// 反过来，需要区分击数的控件（代码区双击选词/三击选行、标题栏双击最大化、
/// 文件列表双击进入）仍按 `EventKind` 各自分支——它们要的不是“激活一次”。
[[nodiscard]] constexpr auto is_click_family(EventKind kind) noexcept -> bool {
  return kind == EventKind::Click || kind == EventKind::DoubleClick ||
         kind == EventKind::TripleClick;
}

/// 鼠标指针形态（**语义名**，不是平台常量）。
///
/// 为何用语义而不是直接存 Win32 的 `IDC_*` / XCursor 名：同一个意图在三个平台上
/// 是三个不同的 API（`IDC_SIZEWE` / `XC_sb_h_double_arrow` / `NSCursor.resizeLeftRight`），
/// 元素层不该知道这些。语义名让“按钮该用手形指针”这件事只写一遍，
/// 各后端自己映射。
///
/// 归类依据是**用户在那一块能做什么**，不是组件叫什么：
/// - `Hand`：可点开/可激活（按钮、菜单项、树行、链接）
/// - `Text`：可编辑/可选择文字（输入框、代码区、终端）
/// - `ResizeH/V`：可拖动改变尺寸（分栏分隔线、可拖边框）
/// - `Move`：可拖动移动（将来用于可拖浮层标题）
/// - `Crosshair`：精确取点（取色器、画布绘制）
/// - `NotAllowed`：当前禁止操作（只读区拖放、禁用控件）
/// - `Arrow`：默认。**一切没声明的都落回它**。
enum class MouseCursor : std::uint8_t {
  Arrow,
  Hand,
  Text,
  ResizeH,
  ResizeV,
  /// 对角缩放（窗口四角用）。单轴缩放用 `ResizeH`/`ResizeV`。
  ResizeNWSE,
  ResizeNESW,
  Move,
  Crosshair,
  NotAllowed,
  /// 可拖拽滚动（垂直/水平滚动条的轨道与滑块）。
  ///
  /// 为何不只复用 `Arrow`：`Arrow` 在 `Element::cursor_at` 里还兼着**“我没意见，交给上层”**
  /// 的含义，必须留空；给了 `Scroll` 才能明确宣告“我这里就是滚动条”，
  /// 否则光标会沿父链回退到编辑器的 I-beam（实测：代码编辑器自绘的滚动条一直显 I 形光标）。
  /// 各后端映射：Win32/`X11`/Wayland 都用各自的**默认箭头**（与 VSCode 一致）。
  Scroll,
};

/// 语义名的稳定短串（控制通道属性面 / 日志 / 测试断言用）。
///
/// 为什么需要：光标是**系统级效果**，截图里看不见——没有这个名字，
/// “hover 分隔线时光标变成了双向箭头”这件事就只能靠人眼看。
[[nodiscard]] constexpr auto mouse_cursor_name(MouseCursor cursor) noexcept -> std::string_view {
  switch (cursor) {
    case MouseCursor::Arrow: return "arrow";
    case MouseCursor::Hand: return "hand";
    case MouseCursor::Text: return "text";
    case MouseCursor::ResizeH: return "resize-h";
    case MouseCursor::ResizeV: return "resize-v";
    case MouseCursor::ResizeNWSE: return "resize-nwse";
    case MouseCursor::ResizeNESW: return "resize-nesw";
    case MouseCursor::Move: return "move";
    case MouseCursor::Crosshair: return "crosshair";
    case MouseCursor::NotAllowed: return "not-allowed";
    case MouseCursor::Scroll: return "scroll";
  }
  return "arrow";
}

struct Event {
  EventKind kind{EventKind::MouseMove};
  math::Point position{};
  int button{0};
  int click_count{1};
  float wheel_delta{0.0f};
  std::string key{};
  std::string code{};
  std::string text{};
  /// 预编辑串内的光标位置（**码点**偏移；仅 `EventKind::ImeComposition` 有意义）。
  ///
  /// 为何不用字节偏移：预编辑串来自 IME，里面可能是任意 Unicode；
  /// 而码点偏移是 IME 原生口径（`GCS_CURSORPOS` 给的也是 UTF-16 码元数，
  /// 转成码点数才是“用户看到光标在第几个字后面”）。
  int preedit_cursor{0};
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

  /// **减少动效**（无障碍：`prefers-reduced-motion` 同语义）。
  ///
  /// 为真时所有动画/过渡**直接落终态**（不播中间帧），且 `paint` 不会为动画续帧。
  /// 为何要把它放进 `RenderContext` 而不是一个全局开关：全局状态会跨线程/
  /// 跨实例泄漏（同时跑两个窗口时一个开了就全开了），而它天然是“这一次渲染的
  /// 环境”。宿主（多窗口/移动端/系统设置）在构造 context 时决定它。
  ///
  /// 必须放在**字段末尾**：本结构是聚合体（大量调用点写
  /// `RenderContext{theme, port, time, &count}`），插在中间会静默改变既有
  /// 初始化的字段对应关系（实际踩到：`nullptr` 被解作 `bool`，报
  /// “converting to bool from nullptr_t”）。新增字段一律追加。
  bool reduced_motion{false};
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
  /// 上一帧本元素**子树**的绘制耗时（纳秒；仅剖析时非零）。
  /// 父元素算“自身耗时”时减它，逐层得到真实的自持成本。
  ///
  /// 与 `paint_cost_serial()` 一起用：只减**同帧**的值（子元素这一帧被视口剔除时，
  /// 它手里的是旧帧的读数）。
  [[nodiscard]] auto paint_cost_ns() const noexcept -> std::int64_t { return paint_cost_ns_; }
  [[nodiscard]] auto paint_cost_serial() const noexcept -> std::uint64_t {
    return paint_cost_serial_;
  }

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
  /// 用途：子树里的组件需要主动改变**焦点**（例如命令面板要求“打开后键盘直达
  /// 过滤框”——不调这句则敲的字会跑进底层编辑器，面板看着开着却打不了字），
  /// 而焦点簿记归根所有（`UiRoot::set_focus` 是唯一入口：它要发 FocusOut/FocusIn
  /// 并维护 Tab 环）。
  ///
  /// ## 为什么不是 `void*`（结构评审 A3）
  ///
  /// 原先这里存的是**类型擦除的 `void*`**，靠 `owner_as<T>()`（内部 `static_cast`）
  /// 强转回来。它确实躲开了 `element.hpp ↔ ui_root.hpp` 的循环依赖，**但代价是
  /// 类型系统完全失效**：转到错误类型是静默 UB，编译器（-Wall -Wextra -Werror 全开）
  /// 一句话都不会说。
  ///
  /// 现在换成**小契约 `HostFocus`**（与 `shell` 的 `WindowControl` 同一手法：
  /// 上层/平台中立的窄接口下沉，实现方去实现它）：
  ///   · 依赖方向仍然单向（`ui_root` 包含 `element`，反向只认这个界面）；
  ///   · `static_cast` 到错类型这类错误**变成编译错误**；
  ///   · 没宿主时**如实返回 `false`**（“没宿主”与“要了但没成”两件事可分）。
  ///
  /// 为什么只放“焦点”这一个能力、不把 `UiRoot` 整个暴露成接口：界面越窄越好
  /// （消费者只有一个：`CommandPalette::grab_focus`）。将来真需要更多宿主能力时，
  /// 应**继续往这个界面加纯虚函数**，而不要退回 `void*`。
  class HostFocus {
   public:
    virtual ~HostFocus() = default;
    /// 把键盘焦点转到 `element`（等价 `UiRoot::set_focus`）；不可聚焦/不在树上时为 `false`。
    virtual auto set_keyboard_focus(Element* element) -> bool = 0;
    /// 读宿主剪贴板（无剪贴板能力/读失败时为空串，不抢错）。
    /// 终端粘贴（`Ctrl+Shift+V`）等组件能力从这取——剪贴板是**平台**能力，
    /// 组件不该也不必知道后端长什么样（实现在 `UiRoot`→`Application`→backend）。
    [[nodiscard]] virtual auto host_clipboard_text() -> std::string { return {}; }
    /// **写**宿主剪贴板（2026-10-12 补齐写侧——此前只有读，组件的 copy 动作面
    /// 接了也没路可走：终端的 `invoke copy` 一直空转返回 false）。
    /// 返回 false = 无宿主/写入失败（调用方如实上报，不假装成功）。
    /// 与读侧对称地走窄接口：实现在 `UiRoot`→`Application`→backend
    ///（win32 真剪贴板 / headless 进程内模拟），headless 与有窗口行为一致。
    virtual auto host_set_clipboard_text([[maybe_unused]] std::string_view text) -> bool {
      return false;
    }
    /// **焦点环是否该画**（只在键盘导航时为真）。
    ///
    /// 为何放在宿主契约里而不是各组件自己推断：鼠标按下可聚焦元素**也会给焦点**
    /// （`UiRoot` 的 `MouseDown` 分支），而点一下就在按钮上留一圈边框不对
    ///——Windows / VSCode 的惯例都是「焦点环只在键盘导航时出现」。
    /// 而“上一次输入是键盘还是鼠标”是**全局**状态，组件无从知道；
    /// 分散实现必然漂移（看上去就是“有的按钮带框有的不带”）。
    /// 默认真：**没有宿主**时按“该画”处理（离屏/测试渲染保持旧观感）。
    [[nodiscard]] virtual auto focus_ring_visible() const -> bool { return true; }
    /// 焦点环可见性变了——组件据此重绘（`UiRoot` 在切换时调）。
    virtual void on_focus_ring_changed() {}
    /// 宿主的当前主题（样式系统 `apply_appearance` 立即落位用）。
    ///
    /// 为什么进契约：外观规则要解析成具体值（颜色/圆角/阴影），而真值源在
    /// UiRoot——元素自己不存主题（存了就是影子状态，换肤会分岔）。
    /// 无宿主/未实现时返回空指针（调用方退回内置亮色主题）。
    [[nodiscard]] virtual auto host_theme() const -> const Theme* { return nullptr; }

    /// 换主题（按内置主题库名字）。**唯一真切换点**：组件不自己实现换肤，
    /// 而是问宿主要——宿主的实现（Application）负责合成/持久化/通知。
    /// 名字无效/宿主不支持 → 返回 false（调用方如实回落，不假装成功）。
    virtual auto host_apply_theme(std::string_view /*name*/) -> bool { return false; }

    /// 挂一个浮层（模态对话框/菜单/主题选择器用）。返回 false = 无宿主/不支持。
    virtual auto host_add_overlay(std::unique_ptr<Element> /*overlay*/,
                                  OverlayLayout /*layout*/) -> bool {
      return false;
    }
    /// 摘一个浮层（对应用户点击遮罩/Esc 关闭）。返回 false = 无宿主/不在场。
    virtual auto host_remove_overlay(Element* /*overlay*/) -> bool { return false; }
  };

  [[nodiscard]] auto host() const noexcept -> HostFocus* { return host_; }
  void set_host(HostFocus* host) noexcept { host_ = host; }

  /// 把宿主契约**转发到整棵子树**（新挂入的子元素也能拿到宿主）。
  ///
  /// 为什么不能只在 `UiRoot::set_content/add_overlay` 时递归写一次：声明式每次重组
  /// 都会新建元素（`create_element` 直接 `add_child`，不经过 UiRoot），于是**新元素的
  /// `host_` 永远是空**——`CommandPalette::grab_focus()` 这类「要宿主给我焦点」的组件
  /// 只能走兜底路径，表现为「面板打开了，键盘焦点却留在底层编辑器」。
  ///
  /// 挂载点收敛到 `add_child`/`insert_child`，新元素随挂入自动继承父元素的宿主。
  void adopt_host();

  // —— 样式 ——
  [[nodiscard]] auto style() noexcept -> Style& { return style_; }
  [[nodiscard]] auto style() const noexcept -> const Style& { return style_; }
  void set_style(Style style) { style_ = std::move(style); }
  [[nodiscard]] auto visible() const noexcept -> bool { return visible_; }
  void set_visible(bool visible) noexcept {
    if (visible_ == visible) return;
    visible_ = visible;
    // **必须标脏布局**：可见性是布局输入（`measure`/`layout_children` 都跳过
    // 不可见子元素），不标脏的话“运行时隐藏一个兄弟”不会让剩下的重新分空间——
    // 现象是隐藏了但位置没变、剩下那个还在原处占一半（相期实测：
    // 空态隐了卡片区，空态却仍在半屏内居中，看起来“没居中”）。
    // 这里用 `mark_layout_dirty()` 而不是仅 `mark_dirty()`：前者会连带重测父级。
    mark_layout_dirty();
  }

  /// **悬停提示**（提示气泡文本；空串 = 不提示）。
  ///
  /// 为何做成一等属性而不是“自己 new 个 Tooltip”：提示是**交互平台能力**
  ///——什么时候弹（悬停多久）、弹在哪（上方/下方翻转）、什么时候收（移开/按下）
  /// 全是共性，与具体控件无关。做成一等属性后调用方只写一句 `set_hint("刷新")`。
  ///
  /// 实现上**不是**每个元素一个气泡：`UiRoot` 只有**一个共享气泡**，悬停时把它
  /// 按目标矩形摆好、逐帧推进延时。若每个元素自建气泡，几百个图标按钮就是几百个
  /// 隐藏子元素（内存/遍历/对账都会疼）。
  void set_hint(std::string hint) { hint_ = std::move(hint); }
  [[nodiscard]] auto hint() const noexcept -> const std::string& { return hint_; }
  [[nodiscard]] auto enabled() const noexcept -> bool { return enabled_; }
  void set_enabled(bool enabled) noexcept { enabled_ = enabled; }
  [[nodiscard]] auto focusable() const noexcept -> bool { return focusable_; }
  void set_focusable(bool value) noexcept { focusable_ = value; }
  /// 这个点是否命中本元素（用于事件派发；覆写即改变“能不能被点到”的语义）。
  ///
  /// ⚠ **自绘子元素做“命中到了什么”的判定时，请另起名字**。
  ///
  /// 这是本仓一个反复被踩的命名坑（caiyun 的时间轴只好叫 `grab_at`）：
  /// - 同名不同返回类型 → 直接编译报 conflicting return type；
  /// - 即便返回类型相同（都想返“命中的子件”），覆写的也是**基类的命中语义**——
  ///   于是 `UiRoot` 的点派发会跟着变，表现为“某个自绘组件一出现，整块区域就点不动了”。
  ///
  /// 建议命名：`grab_at` / `part_at` / `handle_at`（语义是“本控件内部的细分命中”，
  /// 而不是“这个点属不属于我”）。
  [[nodiscard]] virtual auto hit_test(math::Point) const noexcept -> bool;

  /// 仅命中**子树内可交互的孙元素**（不含自身矩形）：默认实现为「任一子元素 `hit_test`」。
  ///
  /// 用途：窗口级容器（声明式 overlay 宿主 Panel、模态遮罩等）自身铺满视口、
  /// 但**不应按整块矩形吃掉下层输入**——否则面板外的点击（想点编辑器、想关面板）
  /// 全部被容器吞掉，表现为「界面卡死」。这类容器覆写本函数，把命中收窄到
  /// 真正在自己的子元素上。
  [[nodiscard]] virtual auto hit_test_children(math::Point point) const noexcept -> bool;

  /// 本元素是否要**在子元素之前**优先拿命中（默认 `false` = 子元素优先）。
  ///
  /// 存在理由（2026-10-18，实测踩到）：派发默认是“子元素优先”——递归整棵子树，
  /// 最后才问自己。那对绝大多数容器是对的。但一类控件自己拥有一个
  /// **与子元素几何重叠**的交互区：
  ///
  /// `SplitView` 的分隔手柄就是。让它占独立布局空间（旧实现 8px）会在两侧面板
  /// 之间留下一条露出页面底色的缝（观感是“分割线有填充宽度 + 边框”）；
  /// 改成贴边重叠后，手柄与两个子面板在几何上叠在一起——
  /// 若仍按“子元素优先”，手柄永远收不到 `MouseDown`，拖拽直接失效。
  ///
  /// 语义：返回 `true` 且 `hit_test(point)` 为真时，本元素先拿命中。
  /// 覆写者必须让 `hit_test` 只在自己的“独占区”上返回真（如手柄矩形），
  /// 否则会把子元素的点击全吃掉。
  [[nodiscard]] virtual auto claims_hit_before_children(math::Point) const noexcept -> bool {
    return false;
  }

  // —— 鼠标指针 ——
  /// 本元素在**默认情形**下要的指针形态（组件构造时声明一次）。
  ///
  /// 与 `cursor_at` 的分工：这里表达“这类控件整体上是什么交互意图”
  ///（按钮 → Hand、编辑器 → Text），而 `cursor_at` 允许**按位置细分**——
  /// 那才是真实的复杂控件的形状（编辑器正文是 Text，但行号栏/折叠箭头不是）。
  void set_cursor(MouseCursor cursor) noexcept {
    if (extras_ == nullptr) extras_ = std::make_unique<RenderExtras>();
    extras_->cursor = cursor;
  }
  [[nodiscard]] auto cursor() const noexcept -> MouseCursor {
    return extras_ != nullptr ? extras_->cursor : MouseCursor::Arrow;
  }
  /// 指针在 `point`（**本元素坐标系**里的绝对点）处该显示什么形态。
  /// 默认返回 `set_cursor` 声明的那个。**按位置细分**是留给需要分区的控件覆写的：
  /// `CodeEditor` 正文给 Text、行号栏给 Arrow；`SplitView` 在分隔线上给 Resize。
  ///
  /// ⚠ 返回 `Arrow` 有两种含义：“我这里就要箭头”与“我没意见，交给上层”。
  /// 区分它们靠调用方**逐级向上回退**：`UiRoot` 拿到 `Arrow` 时会去问父元素的
  /// `cursor()`（而不是 `cursor_at`，因为父元素只知道自己整体是什么）。
  [[nodiscard]] virtual auto cursor_at(math::Point point) const noexcept -> MouseCursor {
    (void)point;
    return cursor();
  }

  // —— 输入法插入点 ——
  /// 本元素**文本插入点**在屏幕上的矩形（逻辑像素）。
  ///
  /// 用途：输入法的候选窗/组合窗要贴着当前打字位置弹出——那是“用户在看哪里”的
  /// 唯一可信坐标。由组件自己回答，因为只有它知道行高、滚动偏移、字宽口径
  ///（宿主自己拼 `bounds_.x + gutter + …` 必然与绘制分叉）。
  ///
  /// 默认返回空矩形 = “我这里没有文本插入点”（按钮、面板…）；
  /// `UiRoot` 据它决定要不要向宿主后端上报。
  ///
  /// 为何用**缓存几何**而不是收 `RenderContext`：它会在事件派发里被调
  ///（那时拿不到绘制上下文，与 `CodeEditor::caret_screen_rect` 同一条理由）。
  [[nodiscard]] virtual auto caret_rect() const -> math::Rect { return math::Rect{}; }

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
    RenderExtras& state = extras();   // 惰性创建：第一次推进悬浮过渡时才分配
    const float target = hovered_ ? 1.0f : 0.0f;
    const double duration = context.theme.metrics().hover_duration;
    const double now = context.time_seconds;
    // `advancing`：时间在走吗？静态帧（首帧/离屏单帧/测试）直接到位，
    // 免得画面停在半程。这条与 `Switch::knob_progress` 同一口径。
    const bool advancing = now > state.last_hover_time;
    state.last_hover_time = now;

    // ⚠ 这里必须严格区分"正在过渡"与"已经静止"。
    // 早先的写法是"只要 elapsed < 1 就声明 animating"，而静止元素每帧都会把
    // `hover_start_` 重置为当前时间 → elapsed 恒为 0 → **永久声明 animating**
    // → 根节点每帧都脏 → 应用 100% 占一个核（实测 6 秒耗 6.12 秒 CPU）。
    // 忙循环对"帧耗时基准"是不可见的，所以当时没被测出来。
    if (state.hover_start >= 0.0) {   // —— 过渡中 ——
      if (duration <= 0.0 || !advancing) {
        state.hover_t = target;
        state.hover_start = -1.0;
        state.hover_animating = false;
        return state.hover_t;
      }
      const double progress = (now - state.hover_start) / duration;
      if (progress >= 1.0) {
        state.hover_t = target;
        state.hover_start = -1.0;
        state.hover_animating = false;
      } else {
        state.hover_t = state.hover_from + (target - state.hover_from) * static_cast<float>(progress);
        state.hover_animating = true;   // 只在这一支声明"还要下一帧"
        // 同时上报损坏区：过渡帧只需重画该元素那块（增量重绘；否则下一帧
        // 损坏区为空 → 回落整帧，实测悬停过渡仍然每帧全屏）。
        record_damage();
      }
      return state.hover_t;
    }

    // —— 静止：只有目标与当前值不同才启动过渡 ——
    if (state.hover_t != target) {
      if (duration <= 0.0 || !advancing) {
        state.hover_t = target;   // 静态帧直接落位（不卡半程）
        state.hover_animating = false;
        return state.hover_t;
      }
      state.hover_from = state.hover_t;   // 从**当前视觉进度**接着动（不跳变）
      state.hover_start = now;
      state.hover_animating = true;
      record_damage();
      return state.hover_t;
    }
    state.hover_animating = false;   // 真正静止：不请求下一帧
    return state.hover_t;
  }  /// 当前悬浮进度（不推进，只读；用于布局等非绘制阶段）。
  [[nodiscard]] auto hover_progress() const noexcept -> float {
    const RenderExtras* state = extras_if_any();
    return state != nullptr ? state->hover_t : 0.0f;
  }

  // —— 动画（CSS 式；引擎见 `st/ui/animation.hpp`）——
  //
  // ## 为什么运行态长在 `Element` 而引擎是纯函数
  //
  // 引擎只会算“某一刻该是什么值”，而“从哪一刻开始算”“要不要续帧”“什么时候
  // 算播完”是元素自己的生命周期——它已经在 `paint()` 里拿到 `RenderContext`
  // （`time_seconds` 在那里）。把这三件事放到元素上，与 `advance_hover` 完全同构，
  // 于是：
  //
  // - **不需要新的 tick 钩子**（应用不必驱动任何东西，也就没有“忘了 tick
  //   所以动画不动”这类缺陷）；
  // - **续帧走既有的 `request_animation()` 汇总链**（`UiRoot::collect_animation_requests`），
  //   播完就不再请求——不会重踩 2026-10-29 那个“静止画面每帧空转”的坑。
  //
  // 与 CSS 的差别：**不支持逐关键帧的 timing-function**（段间统一，见头文件的能力表）。

  /// 设一个关键帧动画（从**下一次绘制**起播）。
  ///
  /// 语义对齐 CSS：`delay` 负值为“从动画中途开始”；`iterations <= 0` 为无限循环；
  /// `fill: forwards/both` 才会在结束后保留终态（否则回落到元素自身样式）。
  /// `frames` 传 `anim::preset_keyframes("fadeInUp")` 或自己构造的关键帧。
  void set_animation(anim::Animation animation, anim::Keyframes frames);
  /// 用内置预设设一个动画（包含了时长/循环/缓动；名字见 `anim::preset_names()`）。
  /// 名字不存在时返回 `false`（**不静默装个空的**：那会让“写错了动画名”看起来像
  /// “动画生效了但看不出效果”）。
  [[nodiscard]] auto set_preset_animation(std::string_view preset) -> bool;
  void clear_animation();
  /// 改动画的**延迟**（秒）。只能在 `set_animation` 之后调（动画未设时无操作）。
  ///
  /// 为何要单独的入口：声明式里“预设名 + 错开延迟”是最常见的入场写法
  ///（一屏卡片依次出现），而把延迟塞进预设名会让名字组合爆炸
  ///（`fadeInUp-delay-2` 之类）。
  void set_animation_delay(double seconds);
  /// 是否设过过渡（声明式用来避开“每帧重设 → 每帧重置过渡状态”）。
  [[nodiscard]] auto transition_configured() const noexcept -> bool;
  /// 是否还在播（结束后为 `false`；诊断/测试用）。
  [[nodiscard]] auto animation_running() const noexcept -> bool;
  /// 本帧生效的动画属性（未设动画时全为空）。
  [[nodiscard]] auto animation_props() const noexcept -> const anim::Props&;
  /// 动画正在作用在几何上吗（位移/缩放非恒等）——绘制与命中需要特殊处理。
  [[nodiscard]] auto animation_transforms() const noexcept -> bool;

  /// 设一条属性过渡（`transition`）：此后**样式值变化时**补间过去而不是瞬变。
  /// 与动画的分工：过渡由“值变了”触发（交互态、主题切换、数据刷新），
  /// 动画由“想播”触发——与 CSS 里 `transition` / `animation` 的分工一致。
  void set_transition(anim::Transition transition);
  void clear_transition();
  /// 当前过渡是否在进行。
  [[nodiscard]] auto transition_active() const noexcept -> bool;

  [[nodiscard]] auto pressed() const noexcept -> bool { return pressed_; }
  [[nodiscard]] auto focused() const noexcept -> bool { return focused_; }
  /// 焦点环策略（组件级声明，不是运行时状态）。
  ///
  /// ## 为何需要这个开关
  ///
  /// 「焦点环只在键盘导航时画」默认是对的（鼠标点出来的焦点不该留边框），
  /// 但它有两个前提不总成立：
  ///
  /// 1. **窗口激活时焦点默认落在第一个可聚焦元素上**，而那一下既不是键盘
  ///    也不是鼠标——`UiRoot::focus_ring_visible` 默认 true，于是绕过启发式
  ///    直接画出环。相期实测：切回前台，整个卡片区一圈灰框；鼠标一动就消失。
  /// 2. **有些控件要焦点但不该有环**：滚动容器（要接 ↑↓/PgUp）、
  ///    面板/列表容器（要接快捷键）——它们没有「可操作目标」的语义，
  ///    VS Code / 浏览器也都不给它们画环。
  ///
  /// 所以把「要不要环」从**全局启发式**降维成**组件声明**：
  /// 容器类声明 `Never`，交互控件保持默认 `Auto`。
  enum class FocusRingPolicy : std::uint8_t {
    Auto,   ///< 默认：宿主说该画就画（键盘导航时）
    Never,  ///< 永不画（容器类：要焦点接键，但不是操作目标）
  };
  [[nodiscard]] auto focus_ring_policy() const noexcept -> FocusRingPolicy { return focus_ring_policy_; }
  void set_focus_ring_policy(FocusRingPolicy policy) noexcept {
    if (focus_ring_policy_ == policy) return;
    focus_ring_policy_ = policy;
    mark_dirty();
  }
  /// 此刻是否该画焦点环（**唯一判定处**，组件不得各自推导）。
  ///
  /// 三个条件：
  /// 1. 自己确实有焦点；
  /// 2. 组件没有否决（`FocusRingPolicy::Never`）；
  /// 3. 宿主认为该画（上一次输入是键盘）——没宿主（离屏/单测）时视为该画。
  [[nodiscard]] auto wants_focus_ring() const noexcept -> bool {
    if (!focused_) return false;
    if (focus_ring_policy_ == FocusRingPolicy::Never) return false;
    const auto* h = host();
    return h == nullptr || h->focus_ring_visible();
  }
  void set_hovered(bool value) noexcept { hovered_ = value; }
  void set_pressed(bool value) noexcept { pressed_ = value; }
  /// 焦点是视觉状态（焦点环/终端光标闪烁都随它变化）——变化必须标脏。
  ///
  /// 旧实现只改标志不标脏：靠终端的常驻 `request_animation`（帧循环永不收敛）
  /// 把所有状态变化“顺带”画出来。2026-10-29 修 CPU 空转（闪烁改定时唤醒）后，
  /// 帧循环真正安静下来，这个隐藏缺陷才暴露：`invoke focus` 后界面不再重绘，
  /// 光标闪烁也因此拿不到第一帧去登记唤醒点（headless 实测光标冻结）。
  virtual void set_focused(bool value) noexcept {
    if (focused_ == value) return;
    focused_ = value;
    mark_dirty();
  }

  // —— 布局与绘制 ——
  [[nodiscard]] auto bounds() const noexcept -> math::Rect { return bounds_; }
  [[nodiscard]] auto measured_size() const noexcept -> math::Size { return measured_; }
  /// 从主题刷新 `style_`（UiRoot 在布局前按脏标记调用）：组件据此把 token 落到具体样式。
  ///
  /// ⚠ 覆写时**必须把显式覆盖放回去**（见 `apply_text_overrides`）：
  /// `apply_theme` 是每帧从主题重算颜色/字号/字重的地方，而 DSL 的
  /// `BoxProps::color/hex_color/size/weight` 是“盖过主题”的显式意图——
  /// 不调本函数就是一个“设了颜色，首帧对、下一帧被主题盖回”的隐形 bug。
  ///
  /// 基类实现统一处理**语义表面档位**（`set_surface`）：容器（`Panel`）不覆写
  /// `apply_theme`，所以“背景色跟着主题走”这件事只能在这里做——否则应用侧
  /// 要么硬编码颜色（主题一切换就错）、要么自己读色板（依赖方向反了）。
  virtual void apply_theme(const Theme& theme) {
    apply_surface(theme);
    apply_text_overrides();
    // 外观规则回放（声明过 `apply_appearance` 的元素在此跟随主题：
    // Tokens 从新主题现解析——换肤一致性只在 `apply_appearance` 一处发生）。
    if (extras_ != nullptr && extras_->appearance.has_value()) {
      const Tokens tokens(theme);
      (void)ui::apply_appearance(*this, tokens, *extras_->appearance);
      apply_text_overrides();   // 文本显式覆盖最后回放（盖过 variant 的 text）
    }
  }

  /// 声明外观规则（CSS 级主题化的入口）：规则存档 + **立即**落位一次
  ///（后续每次 `apply_theme` 自动回放——主题切换无需组件配合）。
  ///
  /// 语义字段见 `Appearance`；未设的字段不干预（与现有 style 共存）。
  /// 返回是否改变了样式（调用方可据此决定是否请求重排）。
  [[nodiscard]] auto apply_appearance(const Appearance& appearance) -> bool {
    extras().appearance = appearance;
    const Theme* theme = host() != nullptr ? host()->host_theme() : nullptr;
    const Tokens tokens(theme != nullptr ? *theme : Theme::light());
    return ui::apply_appearance(*this, tokens, appearance);
  }

  /// 容器的**语义表面档位**（背景色跟着主题走，而不是硬编码）。
  ///
  /// 存在理由：工作台的横向条带（标题栏 / 活动栏 / 状态栏）必须同色——它们
  /// 合起来是一圈“外壳”，色不一致会在拐角处露馅。标题栏的颜色由 `TitleBar`
  /// 自己取 `surface_alt`；而活动栏/状态栏是通用 `Panel`，没有地方表达
  /// “我要跟标题栏同色”。加了这个档位，应用侧只需声明语义，主题切换自动跟随。
  enum class Surface : std::uint8_t {
    None,    ///< 不画底色（构造默认；`Panel` 的透明态）
    Base,    ///< `colors.surface`（普通面板底）
    Alt,     ///< `colors.surface_alt`（外壳条带：标题栏/活动栏/状态栏）
    Sunken,  ///< `colors.surface_sunken`（凹槽、输入底、代码底）
    /// `colors.surface_content`（**编辑内容区**：编辑器/终端/预览这类“我在这里读东西”的面）。
    ///
    /// 为何必须是独立一档而不是复用 `Sunken`（2026-10-18 配色重整，用户报
    /// “编辑器背景不够下沉与 UI 没区分度”）：两个模式的“围白”方向相反——
    ///   暗色：外壳亮 → 内容**更暗**（往里沉），`Sunken` 正好；
    ///   亮色：白到顶了 → 内容**归白**、外壳压灰（VSCode Light / IDEA Light 都这样），
    ///        此时“更暗”反而是错的（编辑器会显得脏、发灰）。
    /// 一档值同时服务两个模式，但它**各自取不同层的 token**，由主题决定。
    Content,
  };

  /// 设语义表面档位（`Surface::None` = 不画底色）。
  ///
  /// 需在下一次 `apply_theme` 前调用（UiRoot 每帧按需调）——若在首帧之后改变，
  /// 本次改变在下一帧的主题刷新时生效（`Panel` 的透明默认值与 `None` 一致，
  /// 所以“先构造后设置”的正常用法没有可见延迟）。
  void set_surface(Surface surface) noexcept { extras().surface = surface; }
  [[nodiscard]] auto surface() const noexcept -> Surface {
    return extras_ != nullptr ? extras_->surface : Surface::None;
  }

 protected:
  /// 把表面档位落到 `style_.background`（基类 `apply_theme` 调；组件覆写时请转调）。
  void apply_surface(const Theme& theme) {
    if (extras_ == nullptr || extras_->surface == Surface::None) return;
    const Palette& colors = theme.colors();
    switch (extras_->surface) {
      case Surface::None: return;
      case Surface::Base: style_.background = colors.surface; return;
      case Surface::Alt: style_.background = colors.surface_alt; return;
      case Surface::Sunken: style_.background = colors.surface_sunken; return;
      // 内容区：两个模式取**不同的层**（理由见枚举处注释）——
      // 这正是“只用一个 `Sunken` 做不到”的那件事。
      case Surface::Content:
        style_.background = theme.mode() == ThemeMode::Dark ? colors.surface_sunken
                                                            : colors.surface_raised;
        return;
    }
  }

 public:

  // —— 排版覆盖（DSL `BoxProps` 的落点）——
  //
  // 为什么放在基类而不是每个组件各写一份：要盖过主题的是**任何会画文字的元素**
  // （Text/Heading/Button/Badge/List 项…），而在每个 `apply_theme` 里重抄一遍
  // “有没有显式覆盖”的判定，漏一个就是那个组件静默不生效。
  // 这里把「意图」与「落位」分开：调用方设一次意图（set_text_*），
  // `apply_theme` 末尾统一回放（`apply_text_overrides`）。

  /// 设文本色（显式值，盖过主题的 tone 映射）。已存的**色调覆盖被清除**（二者互斥）。
  void set_text_color(math::Color color) {
    extras().text_color_override = color;
    extras().text_tone_override.reset();
    mark_dirty();
  }
  /// 设文本色为**主题语义色调**（跟着主题走；首选方式）。已存的字面色覆盖被清除。
  void set_text_tone(Tone tone) {
    extras().text_tone_override = tone;
    extras().text_color_override.reset();
    mark_dirty();
  }
  /// 设字号（显式值，盖过主题的 `font_base`）。
  void set_text_size(float size) {
    extras().text_size_override = size;
    mark_layout_dirty();   // 字号变 → 度量变（不只是重绘）
  }
  /// 设字重（显式值）。
  void set_text_weight(FontWeight weight) {
    extras().text_weight_override = weight;
    mark_dirty();
  }
  // 读取侧不分配：无附属状态时返回静态空值（`extras_if_any()` 为 nullptr 即"未设过"）。
  static const std::optional<math::Color> kNoColor;
  static const std::optional<Tone> kNoTone;
  static const std::optional<FontWeight> kNoWeight;
  [[nodiscard]] auto text_color_override() const noexcept -> const std::optional<math::Color>& {
    const RenderExtras* state = extras_if_any();
    return state != nullptr ? state->text_color_override : kNoColor;
  }
  [[nodiscard]] auto text_tone_override() const noexcept -> const std::optional<Tone>& {
    const RenderExtras* state = extras_if_any();
    return state != nullptr ? state->text_tone_override : kNoTone;
  }
  [[nodiscard]] auto text_size_override() const noexcept -> float {
    const RenderExtras* state = extras_if_any();
    return state != nullptr ? state->text_size_override : -1.0f;
  }
  [[nodiscard]] auto text_weight_override() const noexcept -> const std::optional<FontWeight>& {
    const RenderExtras* state = extras_if_any();
    return state != nullptr ? state->text_weight_override : kNoWeight;
  }
  /// 把显式排版覆盖回放进 `style_`（`apply_theme` 末尾调；没有覆盖就不动）。
  void apply_text_overrides() {
    const RenderExtras* state = extras_if_any();
    if (state == nullptr) return;   // 从未设过覆盖：不动（连分配都不做）
    if (state->text_color_override.has_value()) style_.color = *state->text_color_override;
    if (state->text_size_override >= 0.0f) style_.font_size = state->text_size_override;
    if (state->text_weight_override.has_value()) style_.font_weight = *state->text_weight_override;
  }
  /// 清掉全部显式排版覆盖（回到纯主题；测试与“恢复默认”用）。
  void clear_text_overrides() {
    RenderExtras* state = extras_if_any() != nullptr ? extras_.get() : nullptr;
    if (state != nullptr) {
      state->text_color_override.reset();
      state->text_tone_override.reset();
      state->text_size_override = -1.0f;
      state->text_weight_override.reset();
    }
    mark_layout_dirty();
  }
  /// 计算自身尺寸（写入 `measured_`）；容器组件需递归测量子节点。
  virtual void measure(const RenderContext& context, const Constraints& constraints);
  /// 应用最终矩形并布局子节点。
  virtual void arrange(const RenderContext& context, math::Rect rect);
  /// 绘制自身与子节点（坐标已由 arrange 定好，直接画到画布绝对坐标）。
  ///
  /// **不要直接重写它来“换一种画法”**——子类改 `paint_content` / `paint_box`；
  /// 确实需要接管整个绘制流程的（`ScrollView`/`Toast` 这类），请重写 `paint_impl`。
  ///
  /// 为何这样分：`paint` 是**唯一统一的入口**（性能剖析的计时就在它身上）。
  /// 统计若挂在它里面，重写了 `paint` 的组件就会“免费逃过”统计——实测踩过：
  /// 报告里 `body`（ScrollView）自持 45.8ms，而实际是它下面的整棵内容树。
  virtual void paint(const RenderContext& context, raster::Surface& canvas) const;
  /// 真正的绘制体（`paint` 做可见性/剔除/计时，然后调它）。
  ///
  /// 默认实现就是原来的 `paint` 主体（画自身 + 递归子节点）；需要换整个流程的
  /// 组件重写本函数——于是计时与热点的父子关系自动正确。
  virtual void paint_impl(const RenderContext& context, raster::Surface& canvas) const;
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
  ///
  /// 也可用 `set_intercepts_input(false)` 在实例上关掉（见那里）。
  [[nodiscard]] virtual auto intercepts_input() const noexcept -> bool {
    return intercepts_input_override_.value_or(true);
  }
  /// **实例级**关掉输入拦截（不用为“透明命中层”专门写一个子类）。
  ///
  /// 为何需要它（2026-11-05，实测绕路的代价）：
  /// 想做一个“视觉在上、点击穿透到下层按钮”的叠加层（典型：两行列表行——
  /// 文字盖在可点区域上）。以前只能子类化覆写 `intercepts_input()`，
  /// 而 DSL 里没有子类可写，于是被迫把两行内容塞进 `Button` 的**单行** label，
  /// 排版直接崩（状态字母跑到文件名左侧）。
  void set_intercepts_input(bool value) noexcept {
    intercepts_input_override_ = value;
    mark_dirty();
  }
  /// 恢复默认（跟随虚函数实现）。
  void clear_intercepts_input() noexcept { intercepts_input_override_.reset(); }
  /// **指针移动**时是否参与命中：默认与 `hit_test` 同义。
  ///
  /// 存在的理由：有些元素按整块矩形吃掉**点击**是**对的**（模态屏障——面板外点击
  /// 不该落到下层），但同一块矩形不该连**移动**也吃掉：那会让宿主收不到 hover。
  ///
  /// 实测（2026-10-07，菜单栏下拉）：屏障把 `MouseMove` 一并吞了，于是
  /// 「面板开着时移到另一个标题」不再切换菜单——用户报的「点击其他按钮直接切换列表」
  /// 与「面板不跟随按钮的位置」两条都由此而来（面板开着时 `menubar.hovered` 恒 false）。
  ///
  /// 需要“只拦点击、放行移动”的元素覆写本函数（屏障形态返回 `hit_test_children`）。
  [[nodiscard]] virtual auto hit_test_pointer_move(math::Point point) const noexcept -> bool {
    return hit_test(point);
  }
  /// 行为注入：在组件自身实现**之后**、冒泡**之前**追加一次回调（免子类化的小交互，
  /// 如「拖拽把手改宽度」）。返回 true = 已消费（冒泡停止）。仅对直接派发到本元素的
  /// 事件调用（祖先/后代的不经过本 handler）。
  ///
  /// 分发由 `UiRoot::dispatch_to` 统一做：先 `on_event`（组件覆写），未消费再调它。
  /// **不要在组件覆写里自己调它**——那样每个覆写点都得记得，漏一个就静默失效
  /// （实测：`Input` 上装的 ↑↓ 历史处理器从未被调用，因为 `Input::on_event` 覆写
  /// 直接 `return handle_key(...)`，根本没给 handler 机会；而调用方看到的是
  /// “设了处理器、没报错、也没效果”）。
  void set_event_handler(std::function<bool(Event&)> handler) { event_handler_ = std::move(handler); }
  /// 行为注入回调的调用口（**仅供 `UiRoot::dispatch_to`**）。
  [[nodiscard]] auto invoke_event_handler(Event& event) -> bool {
    return event_handler_ ? event_handler_(event) : false;
  }
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
  /// 组件暴露的**可读写**属性名（`set` 只认这一份；`get` 也遍历它）。
  [[nodiscard]] virtual auto property_names() const -> std::vector<std::string_view>;
  /// 组件暴露的**只读**属性名（`get` 能读、`set` 不应生效）。
  ///
  /// 为何要单开一份（2026-10-14）：契约上 `property_names()` 是「可读写」，但
  /// 有些属性天然只读（`selection`、`line_height`、诊断信息…）。旧实现把它们
  /// 混在 `property_names()` 里，“声明的名字必须可写”这条通用护欄就必然失败
  /// （实测：给 Input 加只读 `selection` 后 `ui_actions_set_changes_all_declared_
  /// string_properties` 变红）；而不入表又会让 `get` 读不到（快照只遍历
  /// `property_names()`）。两份表把这两个需求拆开。
  [[nodiscard]] virtual auto readonly_property_names() const -> std::vector<std::string_view>;
  /// 属性**完整描述**（可选覆写；默认空表）。
  ///
  /// ## 它与上面三个接口的关系
  ///
  /// `property_names()` / `readonly_property_names()` 只回答**名字**，
  /// 足以支撑控制通道的 `get`/`set`，但回答不了生成面板需要的类型/范围/步进。
  /// 本接口补上那一层，**不取代**它们：两者同时存在时，
  /// `validate_property_name_consistency()` 可核对两份描述不矛盾（避免
  /// “名字表说可写、描述说只读”这类静默不一致）。
  ///
  /// **默认空表 = 零回落成本**：存量组件一行不改就继续工作，
  /// 面板只对声明了描述的元素生成控件（渐进采用，而不是一次性改造全部控件）。
  [[nodiscard]] virtual auto property_schema() const
      -> std::span<const PropertyDesc>;
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
    const RenderExtras* state = extras_if_any();
    return animation_requested_ || (state != nullptr && state->hover_animating);
  }
  void clear_animation_request() const noexcept {
    animation_requested_ = false;
    if (RenderExtras* state = extras_.get(); state != nullptr) state->hover_animating = false;
  }
  /// 请求在指定单调时刻得到下一帧（低频周期事件专用）。
  ///
  /// 与 `request_animation` 的分工——两者的滥用史都实测烧过 CPU：
  /// · `request_animation` = **连续动画**（悬浮过渡/骨架屏）：每帧都在动，
  ///   每帧请求下一帧，动画结束自然停。
  /// · `request_wake_at` = **定时点**（光标闪烁/Toast 倒计时）：一秒只变两次的
  ///   东西若走连续续帧，主循环就会以帧预算上限（60fps）空转重绘同一张静止画面
  ///   ——纯 CPU 软件光栅下实测吃满 5 核（2026-10-29 用户报"gbcode 占用 70%"）。
  ///   语义是"到 t 这个时刻我需要一帧"：帧循环会把空闲等待精确夹到最近的唤醒点，
  ///   期间真正睡眠。
  ///
  /// 时间口径：与应用帧时间轴同源（`UiRoot::set_time` 推进的 `time_seconds`，
  /// 单调秒）。重复调用取更早者（多组件各有唤醒点时，最先到的先服务）。
  /// 与 `request_animation` 互不排斥——一个元素可以既在连续动又有定时点。
  void request_wake_at(double monotonic_seconds) const noexcept {
    if (wake_at_ < 0.0 || monotonic_seconds < wake_at_) wake_at_ = monotonic_seconds;
  }
  /// 本元素登记的唤醒时刻（< 0 = 无）；由 `collect_animation_requests` 消费。
  [[nodiscard]] auto wake_at() const noexcept -> double { return wake_at_; }
  void clear_wake_request() const noexcept { wake_at_ = -1.0; }
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
  /// 是否有未消费的损坏上报（只读不清零）。
  ///
  /// 存在的理由（2026-10-29）：`UiRoot::needs_frame()` 要能看到**帧间**登记的
  /// 损坏区——搬运（`collect_tree_damage`）发生在帧首，若只看 UiRoot 侧的
  /// `pending_damage_`，两帧之间的变更就永远等不来帧（无事件伴随的状态变化
  /// 正中盲区：`invoke focus` 后界面不重绘、终端光标闪烁拿不到首帧去登记唤醒点）。
  [[nodiscard]] auto has_pending_damage() const noexcept -> bool {
    const RenderExtras* state = extras_if_any();
    return state != nullptr && state->damage_valid;
  }
  /// 把自己的损坏区记到所在树的根元素（内部用；`mark_dirty`/`request_animation` 调用）。
  void record_damage() const noexcept;
  /// **报一块自定义矩形**的损坏（比 `mark_dirty` 更细的粒度）。
  ///
  /// 为什么需要：`mark_dirty` 报的是整个元素 bounds——对 CodeEditor 这类
  /// 大面积组件，一次「悬停行底纹变化」实际只改了两条 20px 的细条，却要求
  /// 重绘整个编辑器（占视口 80%+ → 超 55% 阈值退化成整帧 + 整窗上屏）。
  /// WARP（纯 CPU 模拟 GPU）下那是每帧 ~7ms 的整窗拷贝，鼠标扫过文本区
  /// 时 CPU 直接拉满（2026-10-29 实测 382% 单核，本接口就是为修它而生）。
  ///
  /// 语义：`rect` 是**窗口坐标**（与 `bounds_` 同坐标系，含滚动偏移后的
  /// 可见位置）；只登记损坏不置 dirty 标志——组件需要重绘自己的其它部分时
  /// 仍应调 `mark_dirty()`。
  void record_damage_rect(math::Rect rect) const noexcept;

 protected:
  /// 子类绘制自身的"盒子"（背景/边框/圆角/阴影）。
  void paint_box(const RenderContext& context, raster::Surface& canvas) const;
  /// 子类绘制文本（自动按 `text_align` 定位）。
  auto paint_text(const RenderContext& context, raster::Surface& canvas, std::string_view text,
                  math::Rect box) const -> void;
  /// **本帧真正用于绘制的样式**：无动画时就是 `style_`（零开销、同一个对象），
  /// 有动画时是「`style_` + 动画覆盖值」的快照。
  ///
  /// 为何不把动画值直接写回 `style_`：那是**真实样式**——主题切换、
  /// 控制通道读回、`apply_appearance` 都会看到被动画污染的假值，
  /// 而且“动画结束复位”一旦有一处漏了，界面就永久错位。
  [[nodiscard]] auto paint_style() const noexcept -> const Style&;
  /// 推进动画/过渡状态并算好本帧的 `paint_style()`。必须在**绘制与读取几何之前**调。
  /// 返回是否仍在动（调用方据此续帧）。
  [[nodiscard]] auto advance_animation(const RenderContext& context) const -> bool;
  /// 布局子节点（Flex 子集：方向/间距/增长/对齐）。
  void layout_children(const RenderContext& context, math::Rect content);
  /// 摆放一个**绝对定位**的子元素（不参与 flex 流；见 `Style::absolute`）。
  void place_absolute(const RenderContext& context, Element& child, math::Rect content);
  [[nodiscard]] auto content_box() const noexcept -> math::Rect;

  Style style_{};
  math::Rect bounds_{};
  math::Size measured_{};
  ElementId id_{};
  std::string key_{};
  /// 悬停提示文本（空 = 不提示；见 `set_hint`）。
  std::string hint_{};
  Element* parent_{nullptr};
  std::vector<std::unique_ptr<Element>> children_{};
  bool visible_{true};
  bool enabled_{true};
  /// 实例级的“不拦截输入”覆盖（见 `set_intercepts_input`）；空 = 跟随虚函数。
  std::optional<bool> intercepts_input_override_{};
  bool focusable_{false};
  bool hovered_{false};
  /// 上一帧本元素**子树**的绘制耗时（纳秒；仅剖析时非零）。
  ///
  /// 存在的唯一理由：父元素算"自身耗时"时要减掉子元素——只报含子孙的耗时，
  /// 排序永远落在根容器上，而根容器是不可改的（那正是"看不出该改哪里"的成因）。
  /// 非剖析时恒为 0（不读时钟）。`mutable`：`paint` 是 const 方法。
  mutable std::int64_t paint_cost_ns_{0};
  /// 上一条耗时属于哪一帧（帧序号）。父元素只减**同帧**的子树耗时：
  /// 子元素若这一帧被视口剔除（`paint` 没跑），残留的是旧值，减它会得出
  /// 负的自持成本（被夹到 0）或把别人的账算到子元素头上。
  mutable std::uint64_t paint_cost_serial_{0};
  HoverEffect hover_effect_{};
  std::function<void(bool)> on_hover_{};
  /// 行为注入回调（`set_event_handler`；组件实现之后、冒泡之前调用）。
  std::function<bool(Event&)> event_handler_{};
  /// 悬浮过渡状态（mutable：绘制是 const 方法，与 `Switch::toggle_time_` 同一套做法）。
  ///
  /// **惰性分配**（结构评审 A2）：这组字段是“悬浮过渡在飞”时的瞬时状态，
  /// 而绝大多数元素从未启动过过渡（子项无 hover 效果、静态界面、离屏渲染）。
  /// 但它们与 `damage_ * / paint_margin_hint_ / text_*_override_` 一共占了
  /// `sizeof(Element)` 中约 **80 字节**（实测 432 → 352），而后几项的使用率甚至更低：
  /// `damage_*` 只写在**根元素**上（`record_damage` 把损坏区记到所在树的根），
  /// `text_*_override_` 只有接了 DSL 排版覆盖的元素才有。
  /// 存成指针后，"未用到"只付 8 字节，"用到"才付一份堆分配。
  struct RenderExtras {
    float hover_t{0.0f};
    float hover_from{0.0f};
    double hover_start{-1.0};            ///< 过渡起点时间；`-1` = 未在过渡中
    /// 上一帧时间戳。初值 0 与 `Switch::last_time_` 同口径：
    /// 静态首帧（time=0）要判成"时间没在走"→ 直接落位，否则画面会停在过渡起点。
    double last_hover_time{0.0};
    bool hover_animating{false};
    /// 上次绘制时的安全外扩（`paint_margin`；-1 = 尚未绘制过 → 损坏区按整帧兜底）。
    float paint_margin_hint{-1.0f};
    /// 累积的损坏区（记录在**所在树的根元素**上；UiRoot 帧首取走）。
    math::Rect damage{};
    bool damage_valid{false};
    bool damage_needs_full{false};
    /// 排版显式覆盖（DSL `BoxProps` 的落点）——缺省表示"不干预主题"。
    /// 色值用两个 `optional`（互斥：设一个清另一个），于是"最后设的那个生效"不需要额外排序逻辑。
    std::optional<math::Color> text_color_override{};
    std::optional<Tone> text_tone_override{};
    float text_size_override{-1.0f};
    std::optional<FontWeight> text_weight_override{};
    /// 语义表面档位（`Element::Surface`；`None` = 不画底色，与构造默认一致）。
    Surface surface{Surface::None};
    /// 鼠标指针形态（`set_cursor` 声明的“这类控件整体用哪种”）。
    MouseCursor cursor{MouseCursor::Arrow};
    /// 外观规则（`apply_appearance` 的意图存档；`apply_theme` 每帧回放——
    /// 主题切换后所有联动自动跟随，这是「CSS 控制元素样式」的落点）。
    /// `has_value()` 为假 = 未声明外观（组件自己的 apply_theme 说了算）。
    std::optional<Appearance> appearance{};

    // —— 动画（CSS 式；引擎见 `st/ui/animation.hpp`）——
    //
    // 惰性：没设过动画/过渡的元素不付任何代价（本结构本身也是惰性分配的，
    // 见上方 `extras()` 的说明）。
    anim::Animation animation{};
    /// 已归一化的关键帧（构造时排序并补齐端点；每帧不再重算）。
    anim::Keyframes keyframes{};
    /// 动画起点（单调秒；`-1` = 尚未起播——首帧绘制时才定，这样“设了但还没上屏”
    /// 的元素不会因为构造与首帧之间的时间差而生从半程开始播）。
    double anim_start{-1.0};
    /// 是否设过动画（与 `anim_running` 不同：后者是“本帧还在播”）。
    bool anim_active{false};
    bool anim_running{false};
    /// 本帧生效的动画属性（已合成到 `paint_style()` 里）。
    anim::Props anim_props{};
    /// 本帧的动画样式的“快照”（`paint_style()` 返回它；只在 `anim_props`
    /// 非空时使用）——存成字段而不是每次现造，避免每帧复制一份 `Style`。
    Style animated_style{};

    anim::Transition transition{};
    double transition_start{-1.0};
    bool transition_running{false};
    /// 过渡的起点/终点属性快照（`from` 在值变化那一帧捕获，`to` 每帧跟随当前样式）。
    anim::Props transition_from{};
    anim::Props transition_to{};
    /// 捕获“当前样式对应的属性快照”（判断值变没变、做起止点）。
    anim::Props style_snapshot{};
    /// 是否已记下初始快照。**没有它就会“启动即过渡”**：首帧 `style_snapshot` 为空，
    /// 与真实样式逐项不等，会被当成“值变了”而开一段无意义的过渡（多拉十几帧）。
    bool style_snapshot_valid{false};
  };

  /// 取（必要时创建）渲染附属状态。**非 const** 的入口（`advance_hover` 用 `mutable` 成员调）。
  [[nodiscard]] auto extras() const noexcept -> RenderExtras& {
    if (extras_ == nullptr) extras_ = std::make_unique<RenderExtras>();
    return *extras_;
  }  /// 只读访问（**不存在则为 `nullptr`**）：查询路径用，不分配。
  [[nodiscard]] auto extras_if_any() const noexcept -> const RenderExtras* {
    return extras_.get();
  }

  /// 组件主动请求的续帧（与悬浮过渡共用汇总链路）。
  mutable bool animation_requested_{false};
  /// 本元素这一帧的**动画平移量**（逻辑像素）与缩放。
  /// `mutable`：在 `paint()`（const）里推进；命中测试与子元素坐标都读它。
  mutable math::Point animation_translate_{};
  mutable float animation_scale_{1.0f};

 public:
  /// 本帧的动画平移量（命中测试反算坐标用；未设动画时为原点）。
  [[nodiscard]] auto animation_translate() const noexcept -> math::Point {
    return animation_translate_;
  }
  /// 本帧的动画缩放（诊断用；命中暂不按缩放反算，见 `animation_scale` 的说明）。
  [[nodiscard]] auto animation_scale() const noexcept -> float { return animation_scale_; }

 protected:
  /// 离屏合成期间临时改走“去掉了动画透明度”的样式（防止乘两次）。
  mutable const Style* style_override_{nullptr};

  /// 带几何变换的绘制入口（`paint()` 在剥除、计时后调它）。
  virtual void paint_animated(const RenderContext& context, raster::Surface& canvas) const;
  /// 登记的唤醒时刻（单调秒；< 0 = 无）。mutable：paint 是 const 路径。
  mutable double wake_at_{-1.0};
  bool pressed_{false};
  bool focused_{false};
  FocusRingPolicy focus_ring_policy_{FocusRingPolicy::Auto};
  bool dirty_{true};
  bool layout_dirty_{true};
  /// 宿主（`UiRoot`），由 UiRoot 在挂载/摘除时维护；未上树为 nullptr。
  /// 宿主焦点契约（未上树时为 `nullptr`）。**类型化**——见上方 `HostFocus` 的说明。
  HostFocus* host_{nullptr};
  /// 渲染附属状态（悬浮过渡 / 损坏区 / 排版覆盖）——惰性：未用到则为 `nullptr`。
  /// `mutable`：这些状态在 `paint()`（const）里推进（与原先各字段上的 `mutable` 同义）。
  mutable std::unique_ptr<RenderExtras> extras_{};
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
  /// 副轴尺寸夹到非负（见实现处说明：容器比占位块矮时会得到负高度矩形）。
  void arrange(const RenderContext& context, math::Rect rect) override;
};

}  // namespace st::ui

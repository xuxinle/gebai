#pragma once

/// @file mindmap.hpp
/// @brief 思维导图的**数据模型、编辑操作、序列化与布局**（纯计算层，零 `st::ui` 依赖）。
///
/// ## 为什么单独一层（与 `mermaid` / `graph_layout` / `timeline` 同一分工）
///
/// | 层 | 工作 | 能否单测 |
/// |---|---|---|
/// | 本文件 | 树模型、编辑（增删改移）、FreeMind `.mm` 往返、tidy-tree 布局 | ✅ 纯函数，可穷举 |
/// | `components/mind_map_view.hpp` | 把几何画成像素、接属性面/动作面与交互 | 截图 + 属性断言 |
///
/// 布局与编辑出错的表现是「图看着别扭」（兄弟节点压在一起、改了父级却还在原处、
/// 存盘再读回来少了一个分支），靠截图一条条看是查不动的；在这一层它们都能被断言钉死。
///
/// ## 为什么树用**扁平数组 + 下标**，不用 `unique_ptr` 链
///
/// 1. **编辑操作要能"整树快照"进撤销栈**：`std::vector<Node>` 一份拷贝就是一份快照，
///    而指针树要么写深拷贝、要么共享所有权——后者会让"撤销后又被别处改掉"成为可能。
/// 2. **下标是稳定句柄之外的第二把尺子**：跨 HTML/JS/协议传树时字符串 id 才是稳定的，
///    但**同一进程内**按下标随机访问便宜且不会被 id 重名坑到。
/// 3. **布局要在遍历里按下标回填几何**（`layout_` 与 `nodes_` 同序），指针树要额外维护
///    一张"指针 → 下标"表，而那张表与树结构一旦不同步就是静默错位。
///
/// 代价是**删除要自己搬移下标**（见 `remove_subtree`）：它把被删子树之后的节点整体前移，
/// 并修正所有 `parent`/`children` 引用。这件事只有一处实现，因此可测。
///
/// ## 确定性是硬要求
///
/// 同一棵树必得同一份几何：布局不含随机、不依赖容器迭代外的任何状态，
/// 兄弟顺序即声明顺序。否则截图回归会飘，缺陷也无法复现。
///
/// ## 度量为什么由调用方注入
///
/// 本层不认识字体（`ui` 层对 `text` 层的依赖倒置同理）。节点多宽取决于标签排出来多宽，
/// 所以布局收一个 `MeasureText`；测试注入定宽桩，组件注入真实 `TextPort`。

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "st/core/error.hpp"
#include "st/math/geometry.hpp"

namespace st::md::mindmap {

/// 节点（扁平数组里的一项）。
///
/// `parent` / `children` 都是**下标**（`kNoNode` = 无）。为什么两边都存：
/// 上查找（祖先链、层级、拖拽合法性）与下遍历（布局、绘制顺序）都是热路径，
/// 每次现场扫 `children` 反查父级是 O(n²)——而一致性由**所有写操作都经本类的
/// 成员函数**保证（`nodes()` 只读返回，没有外部可改的入口）。
struct Node {
  /// 稳定业务 id（`FreeMind` 的 `ID` 属性；进程内不要求唯一，重名时按下标区分）。
  ///
  /// 为何要往返它：它是**外部世界的引用锚**（FreeMind 的笔记链接、超链接、
  /// 其它导图里指向本节点的话都会用它）。丢了它，一次存取就会把外部指涉全部打断。
  std::string id{};
  /// 显示文字（`FreeMind` 的 `TEXT`）。
  std::string text{};
  /// 备注（`FreeMind` 的 `NOTE`，**多行**；不画在节点里，详情面板/提示用）。
  std::string note{};
  /// 超链接（`FreeMind` 的 `LINK`）。
  std::string link{};
  std::size_t parent{kNoNode};
  /// 子节点下标（**顺序即显示顺序**）。
  std::vector<std::size_t> children{};
  /// 折叠：为真时它的子树不参与布局与绘制（节点自己仍显示，带一个徽标表示还有内容）。
  bool collapsed{false};
  /// 强调色（ARGB；0 = 用层级默认色）。让用户能标出"这条分支是重点"。
  ///
  /// 与 `FreeMind` 的 `COLOR`（`#rrggbb`）互相换算——那边没有 alpha，
  /// 往返时按不透明处理（本层不会把 alpha 写出去，见 `to_freemind`）。
  std::uint32_t argb{0U};

  /// **本层不解释但必须往返**的属性（字体 `STYLE`、图标 `ICON`、时间戳 `CREATED`…）。
  ///
  /// 为何单列一个表而不是给 `Node` 加十几个字段：`.mm` 的属性集是**外部格式**的，
  /// 它会随 FreeMind 版本增加。把它们逐个提成结构体字段，就是让框架的节点模型
  /// 跟着一个外部格式的版本跑；而原样透传的代价只是序列化层多一个循环，
  /// 收益是"别人的文件读进来再存出去不会掉东西"。
  std::vector<std::pair<std::string, std::string>> extra_attributes{};

  static constexpr std::size_t kNoNode = static_cast<std::size_t>(-1);
};

/// 导图元信息（标题、版本）。序列化时写在最外层。
struct Meta {
  std::string title{};
  /// 写盘时的 `<map version="…">` 主版本号（`0` = 不写版本头，也是合法的老文件形态）。
  std::int32_t version{0};
};

/// 布局方向。
enum class LayoutDirection : std::uint8_t {
  /// 逻辑图：根在左，整棵树向右展开（层级最清晰，适合长文档）。
  Right,
  /// 经典思维导图：根居中，一级子节点**左右交替**，各自继续向外展开。
  Both,
};

[[nodiscard]] auto to_string(LayoutDirection direction) noexcept -> std::string_view;
[[nodiscard]] auto parse_direction(std::string_view name) noexcept
    -> std::optional<LayoutDirection>;

// ── 树模型 ────────────────────────────────────────────────────────────────────

/// 思维导图文档：一棵树 + 元信息 + 撤销无关的纯数据。
///
/// 所有编辑都走成员函数（没有暴露可写的 `nodes()`），因此三条不变量
/// （父子双向一致、无环、根唯一）在任何时刻都成立——这是 `layout` 与 `to_json`
/// 敢于假定结构合法的前提。
class Document {
 public:
  /// 建一棵只有根的文档。
  explicit Document(std::string root_text);

  /// 建一份空文档（**无根**，`valid()` 为假）。反序列化失败时的初值。
  Document() = default;

  [[nodiscard]] static auto empty() -> Document;

  // —— 读 ——

  [[nodiscard]] auto valid() const noexcept -> bool { return !nodes_.empty() && root_ != Node::kNoNode; }
  [[nodiscard]] auto empty_document() const noexcept -> bool { return nodes_.empty(); }
  [[nodiscard]] auto node_count() const noexcept -> std::size_t { return nodes_.size(); }
  [[nodiscard]] auto root() const noexcept -> std::size_t { return root_; }
  [[nodiscard]] auto nodes() const noexcept -> const std::vector<Node>& { return nodes_; }
  [[nodiscard]] auto meta() noexcept -> Meta& { return meta_; }
  [[nodiscard]] auto meta() const noexcept -> const Meta& { return meta_; }

  /// 取节点（越界给 `nullptr`，不抛——与框架错误契约同一口径）。
  [[nodiscard]] auto node(std::size_t index) const noexcept -> const Node*;
  /// 节点文字（越界给空串）。
  [[nodiscard]] auto text_of(std::size_t index) const -> std::string;
  /// 深度（根 = 0；越界给 0）。
  [[nodiscard]] auto depth_of(std::size_t index) const noexcept -> std::size_t;
  /// 祖先链（从根到它，含自身；越界给空）。
  [[nodiscard]] auto path_of(std::size_t index) const -> std::vector<std::size_t>;
  /// 它是否是 `ancestor` 的后代（含自身）。拖拽成环检查用。
  [[nodiscard]] auto is_descendant_of(std::size_t index, std::size_t ancestor) const noexcept
      -> bool;
  /// 子树节点数（含自身；`collapsed` 不截断——折叠只影响显示）。
  [[nodiscard]] auto subtree_size(std::size_t index) const noexcept -> std::size_t;
  /// 全部节点的最大深度（空文档 = 0）。
  [[nodiscard]] auto max_depth() const noexcept -> std::size_t;
  /// 折叠状态下的**可见**节点数（布局与绘制的规模）。
  [[nodiscard]] auto visible_count() const noexcept -> std::size_t;

  // —— 编辑（每个都返回受影响节点的下标；失败给 `kNoNode`）——

  /// 换根文字。空文档时无副作用。
  void set_root_text(std::string text);
  /// 换节点文字。
  auto set_text(std::size_t index, std::string text) -> bool;
  /// 换节点备注。
  auto set_note(std::size_t index, std::string note) -> bool;
  /// 换节点超链接。
  auto set_link(std::size_t index, std::string link) -> bool;
  /// 换节点 id（**反序列化保真用**：让存取再读回来的 id 与原来一致，
  /// 否则外部的引用（笔记链接、其它导图的指涉）会在一次往返后全部失效）。
  auto restore_id(std::size_t index, std::string id) -> bool;
  /// 换节点强调色（0 = 用默认）。
  auto set_color(std::size_t index, std::uint32_t argb) -> bool;
  /// 折叠/展开。
  auto set_collapsed(std::size_t index, bool collapsed) -> bool;
  /// 反转折叠状态。
  auto toggle_collapsed(std::size_t index) -> bool;

  /// 加子节点（追加到末尾），返回新节点下标。
  auto add_child(std::size_t parent, std::string text = "新主题") -> std::size_t;
  /// 加同级（插到自己后面），返回新节点下标；对根操作等于 `add_child`。
  auto add_sibling(std::size_t index, std::string text = "新主题") -> std::size_t;
  /// 删子树（根不可删），返回被删的根的下标位置之后的"下一个可用下标"（便于选中）。
  ///
  /// 返回的是**删除后**应当选中的节点：优先下一个同级，否则父级。无可用时给 `kNoNode`。
  auto remove_subtree(std::size_t index) -> std::size_t;
  /// 改父级（`index` 挂到 `new_parent` 末尾）。
  ///
  /// 拒绝三种：`new_parent` 是自己或自己的后代（成环）、`index` 是根、下标越界。
  auto reparent(std::size_t index, std::size_t new_parent) -> bool;
  /// 在同级里前移/后移一位。
  auto move_up(std::size_t index) -> bool;
  auto move_down(std::size_t index) -> bool;

  // —— 整体替换（载入 / 撤销恢复）——

  /// 整份替换（`valid()` 由传入内容决定）。
  void reset(Document document);
  /// 清空成空文档。
  void clear();

  // —— 序列化：FreeMind `.mm` ——
  //
  // ## 为什么用 FreeMind 的 `.mm` 而不是自定义 JSON
  //
  // 思维导图的**价值有一半在交换**：用户写完要拿去 FreeMind / XMind / 思维导图工具里看、
  // 要发给别人、要进版本库。自定义格式每一条都要写导入导出适配，而 `.mm` 是
  // 一个**被多个工具读**的开源原生格式（`<map><node TEXT="…">`）。
  //
  // 代价与边界（写在这里免得下次又"优化"掉）：
  // ① `.mm` 的表达力弱于自定义格式（无 polyline 之外的样式、无任意 `meta`）——
  //    所以本模型**只保留 `.mm` 能表达且导图真正需要的字段**（id/text/note/link/
  //    collapsed/color/children），没有"只有我们能读懂"的私有字段；
  // ② 读入时**不认识的属性一律保留到 `Meta::extra_attributes` 与 `Node::extra`**（见下），
  //    否则 FreeMind 用户设的字体/图标/时间戳会在一次存取后无声消失
  //    （那是数据丢失，不是"清洁化"）。

  /// 写 FreeMind `.mm` 文本。
  ///
  /// `pretty` 为真时按层级缩进（人读、git diff 友好）；否则紧凑。
  [[nodiscard]] auto to_freemind(bool pretty = true) const -> std::string;
  /// 读 FreeMind `.mm` 文本。失败经 `Result` 如实上报（错误行号来自 `st::xml`）。
  [[nodiscard]] static auto from_freemind(std::string_view text) -> Result<Document>;

 private:
  /// 内部：追加一个节点并返回下标（不校父级。
  auto append_node(std::size_t parent, std::string text) -> std::size_t;
  /// 内部：从 `index` 的 `children` 里摘掉 `child`。
  void detach_child(std::size_t index, std::size_t child);

  std::vector<Node> nodes_{};
  std::size_t root_{Node::kNoNode};
  Meta meta_{};
};

// ── 布局 ──────────────────────────────────────────────────────────────────────

/// 文本宽度度量（纯计算层不认识字体，由调用方注入）。
using MeasureText = std::function<float(std::string_view text, float font_size)>;

/// 布局参数。**尺寸都从 `font_size` 派生或按倍数放**：写死像素常量会在放大查看时
/// 比例失衡（字大了、内边距没变）。
struct LayoutOptions {
  float font_size{14.0f};
  /// 行高倍数（多行折行的标签）。
  float line_gap{1.4f};
  /// 节点内边距。
  float pad_x{14.0f};
  float pad_y{9.0f};
  /// 节点最小/最大宽（窄标签不该缩成一小块，长标签不该横向撑爆）。
  float min_node_width{84.0f};
  float max_node_width{240.0f};
  /// 层与层之间的水平间距。
  float level_gap{56.0f};
  /// 同级（兄弟子树）之间的垂直间距。
  float sibling_gap{14.0f};
  /// 整图留白。
  float margin{28.0f};
  /// 方向。
  LayoutDirection direction{LayoutDirection::Right};
};

/// 一个节点的最终框（**内容坐标**：原点在图左上角，不含视图的缩放与平移）。
struct NodeBox {
  math::Rect rect{};
  /// 文字可排区域（已按内边距缩好；绘制层按它居左画）。
  math::Rect text_rect{};
  /// 折行后的标签行。
  std::vector<std::string> lines{};
  /// 层级（根的展开子节点 = 1）。
  std::size_t depth{0};
  /// 该节点在**画布上的水平侧**（-1 左、+1 右、0 居中）。`Both` 布局用它决定连线走向。
  int side{0};
  /// 是否有被折叠隐藏的子节点（画折叠徽标）。
  bool has_hidden_children{false};
  /// 折叠的子节点数（徽标上的数字；0 = 无徽标）。
  std::size_t hidden_children{0};
};

/// 一条连线（父 → 子的三次贝塞尔控制点）。
struct LinkGeometry {
  /// 两端节点下标（`Document::nodes()` 的下标）。
  std::size_t parent{0};
  std::size_t child{0};
  /// 起点（父框在该侧的边中点）。
  math::Point from{};
  /// 终点（子框对侧的边中点）。
  math::Point to{};
  /// 两个控制点（三次贝塞尔；水平方向拉出，形成思维导图的"藤蔓"）。
  math::Point control1{};
  math::Point control2{};
};

struct Layout {
  /// 与 `Document::nodes()` **同序**（被折叠隐藏的节点框为空矩形，`lines` 为空）。
  std::vector<NodeBox> boxes{};
  std::vector<LinkGeometry> links{};
  /// 整张图的包围尺寸（已含 `margin`）。
  math::Size size{};
};

/// "装得下"时用的初始视口提示（组件用它算 fit 倍率）。
[[nodiscard]] auto layout(const Document& document, const LayoutOptions& options,
                          const MeasureText& measure) -> Layout;

/// 按宽度折行（按**码点**切，不会切出半个汉字）。最多 `max_lines` 行，超出末行加省略号。
///
/// 为什么放在本层：布局与绘制必须用**同一份**行文本——两处各折一次，行数就会不同
/// （实测症状：节点框算的是两行、画的时候三行，末行溢出框外）。
[[nodiscard]] auto wrap_text(std::string_view text, float font_size, float max_width,
                            std::size_t max_lines, const MeasureText& measure)
    -> std::vector<std::string>;

/// 距离某点最近的节点（内容坐标；未命中给 `kNoNode` 语义的 `Node::kNoNode`）。
///
/// 只按**框**判定，不做 z 序——思维导图的框不重叠（tidy-tree 保证），
/// 因此"包含该点的那个框"就是唯一答案。
[[nodiscard]] auto hit_test(const Layout& layout, math::Point point) -> std::size_t;

}  // namespace st::md::mindmap

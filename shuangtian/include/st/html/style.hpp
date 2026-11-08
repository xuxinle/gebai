#pragma once

/// @file style.hpp
/// @brief HTML/CSS 子集样式层：**选择器匹配 + 层叠 + 具体属性解析**。
///
/// ## 支持到什么程度（如实列全，不写“基础”这种含糊词）
///
/// ### 选择器
///
/// | 写法 | 支持 | 备注 |
/// |---|---|---|
/// | `tag` | ✅ | 标签名（大小写不敏感） |
/// | `.cls` | ✅ | 类（按空白切分后整体比较） |
/// | `#id` | ✅ | id |
/// | `*` | ✅ | 通配 |
/// | `a b` | ✅ | 后代（任一祖先匹配即可） |
/// | `a > b` | ✅ | 直接子代 |
/// | `a, b` | ✅ | 选择器列表（逐个匹配，命中即算） |
/// | `:hover` / `:first-child` / `:last-child` 等伪类 | ❌ | 静态渲染没有交互态；写了就**丢掉该条规则**（不静默当通配） |
/// | 属性选择器 `[x=y]` | ❌ | 同上（明确不支持，不猜） |
///
/// ### 属性
///
/// 颜色 / 字号 / 字重 / 斜体 / 行高 / 字间距 / 文本对齐 / 文本装饰 /
/// 宽度 / 高度 / 最大宽度 / 外边距 / 内边距 / 边框（宽度·颜色·圆角）/
/// 背景色 / 显示模式 / 列表样式类型 / 空白保留模式。
///
/// **不支持**（出现即忽略该声明）：`float` / `position` / `flex` / `grid` /
/// `transform` / `animation` / `transition` / 百分比高度 / `calc()` / 变量 `var(--x)`。
/// 忽略而不是报错：网页里这些属性满天飞，一份样式表里有一条不认识的不该让整页不可读。
///
/// ### 层叠（简化但**顺序正确**）
///
/// 优先级按 CSS 的常用近似：`!important` > 内联 `style` > 选择器特异性
/// （id×100 + class×10 + tag×1，后代组合取各项之和） > 同特异性看**出现顺序**
/// （后出现的赢）。`!important` 支持，因为它承载作者的真实意图。
///
/// **继承**只做「可继承属性」那一批（颜色 / 字号 / 字重 / 斜体 / 行高 /
/// 字间距 / 文本对齐 / 空白模式）——与浏览器行为一致。其余属性一律不继承
/// （`margin` 之类继承会立刻把页面撑歪，这是 CSS 里最经典的误继承）。

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "st/html/dom.hpp"
#include "st/math/color.hpp"

namespace st::html {

/// 文本对齐（沿 CSS 的取值）。
enum class TextAlign : std::uint8_t { Start, Center, End, Justify };

/// `display`（只区分排版真正需要的几类）。
enum class Display : std::uint8_t {
  Block,   ///< 块级（独占一行）
  Inline,  ///< 行内
  InlineBlock,  ///< 行内但可设宽高
  None,    ///< 不渲染（`display:none` / `<script>` / `<style>` / `<head>`）
  ListItem,  ///< 列表项（块级 + 项目符号）
  Table,     ///< 表格（简化为块级，逐行排版）
  TableRow,
  TableCell,
};

/// 空白处理（沿 CSS `white-space`）。
enum class WhiteSpace : std::uint8_t { Normal, Pre, PreWrap, NoWrap };

/// 列表标记类型。
enum class ListStyle : std::uint8_t { Disc, Circle, Square, Decimal, None };

/// 一条**具体**样式（全部字段都有确定值；继承与层叠在 `resolve` 里完成）。
struct Style {
  math::Color color{0, 0, 0, 255};
  math::Color background{0, 0, 0, 0};
  bool has_background{false};

  float font_size{16.0f};
  std::uint16_t font_weight{400};   ///< 100..900（700 以上按粗体画）
  bool italic{false};
  bool monospace{false};
  bool underline{false};
  bool strikethrough{false};

  /// 行高倍数（**相对字号**：`1.5` = 1.5 倍）。CSS 里 `line-height: 24px` 会换算成倍数。
  float line_height{1.45f};
  /// 字间距（px）。
  float letter_spacing{0.0f};
  TextAlign text_align{TextAlign::Start};

  /// 盒子尺寸（`kAuto` = 不指定）。
  float width{-1.0f};
  float height{-1.0f};
  float max_width{-1.0f};

  /// 内边距 / 外边距（px；简化为单值——`padding: 4px 8px` 取第一段作纵向、第二段作横向）。
  float padding_top{0.0f};
  float padding_right{0.0f};
  float padding_bottom{0.0f};
  float padding_left{0.0f};
  float margin_top{0.0f};
  float margin_right{0.0f};
  float margin_bottom{0.0f};
  float margin_left{0.0f};

  float border_width{0.0f};
  math::Color border_color{0, 0, 0, 0};
  /// 只画左边那道线（`border-left` / 引用块）；矩形描边会把四周都画上一圈，
  /// 而引用式提示块只需要一道竖线——四条框看上去就是“套了个框”。
  bool border_left_only{false};
  float radius{0.0f};

  Display display{Display::Inline};
  WhiteSpace white_space{WhiteSpace::Normal};
  ListStyle list_style{ListStyle::Disc};

  /// 标题级别（1..6；非标题为 0）——排版层据此给默认字号/间距/字重。
  std::uint8_t heading_level{0};

  [[nodiscard]] auto bold() const noexcept -> bool { return font_weight >= 600U; }
};

/// 一条选择器里的单个复合选择器（如 `div.card#main`）。
struct CompoundSelector {
  std::string tag{};               ///< 空 = 不限
  std::string id{};                ///< 空 = 不限
  std::vector<std::string> classes{};   ///< 全部都要匹配
};

/// 一个选择器（复合选择器序列 + 组合符；从左到右是**祖先 → 后代**）。
struct Selector {
  std::vector<CompoundSelector> parts{};
  /// `combinators[i]` 表示 `parts[i]` 与 `parts[i+1]` 之间的关系：`true` = 直接子代。
  std::vector<bool> child_combinator{};
  /// 特异性分量（`!important` 另算）。
  int specificity{0};
};

/// 一条声明（属性名 + 值 + 是否 important）。
struct Declaration {
  std::string property{};
  std::string value{};
  bool important{false};
};

/// 一条规则。
struct Rule {
  std::vector<Selector> selectors{};
  std::vector<Declaration> declarations{};
  /// 规则在样式表里的出现顺序（同特异性时后者赢）。
  std::size_t order{0};
};

/// 解析后的样式表。
struct StyleSheet {
  std::vector<Rule> rules{};

  /// 解析 CSS 文本（容错：认不出选择器/声明的部分被跳过）。
  static auto parse(std::string_view css) -> StyleSheet;
};

/// 解析一份内联 `style="…"`（返回声明表）。
[[nodiscard]] auto parse_inline_style(std::string_view text) -> std::vector<Declaration>;

/// 文档树 → 已解析样式的映射。
///
/// ## 为什么是「一次性预计算」而不是「排版时现查」
///
/// 排版需要**祖先的样式**（继承）与**兄弟位置**（`:first-child` 类）。边排边查会让
/// 每个节点都重走一遍祖先链，且文本节点还要再查父节点——实测在 1000 节点页面上
/// 是 O(n·depth·rules) 的重复工作。这里一次性算出「节点指针 → Style」，
/// 排版层只做查表。
class StyleResolver {
 public:
  StyleResolver(const StyleSheet& sheet, const Style& initial);

  /// 解析整棵树（返回 `*this` 便于链式）。
  auto resolve_tree(const Node& root) -> StyleResolver&;

  /// 取某节点的样式（未解析返回初始样式）。
  [[nodiscard]] auto style_of(const Node* node) const -> const Style&;

  /// 用户代理默认值（标签 → 默认样式）；`initial` 是 `html` 的起点。
  [[nodiscard]] static auto user_agent_style(std::string_view tag) -> Style;
  /// 上述默认值的**初始样式**（浏览器 `html` 的 16px 黑字白底）。
  [[nodiscard]] static auto initial_style() -> Style;

 private:
  /// 一条候选声明（带优先级，用于层叠排序）。
  struct Candidate {
    std::string property{};
    std::string value{};
    int precedence{0};   ///< 越大越优先
  };

  void resolve_node(const Node& node, const Style& parent_style, int depth);
  /// 应用一条声明（属性名 + 值 → 写进 `style`）。
  ///
  /// 不认识的属性/值——**静默忽略那一条**（不是整个样式块）：网页里
  /// `box-shadow`/`transition` 这类声明满天飞，因为一条不认识就让整页不可读是不可接受的。
  static void apply_declaration(Style& style, std::string_view property,
                                std::string_view value, float em_base);
  /// 选择器「最右一段」是否匹配该节点（**不**检查祖先）。
  [[nodiscard]] static auto matches_compound(const Node& node, const CompoundSelector& part)
      -> bool;
  /// 整条选择器是否匹配（沿祖先链向上）。
  [[nodiscard]] static auto matches_selector(const Node& node, const std::vector<const Node*>& ancestors,
                                             const Selector& selector) -> bool;

  const StyleSheet& sheet_;
  Style initial_{};
  std::map<const Node*, Style> styles_{};
  /// 祖先链（解析期维护；`resolve_node` 递归时的栈）。
  std::vector<const Node*> ancestors_{};
  /// 节点在自己父节点里的元素序号与同标签序号（`:first-child` 之类用；预留）。
  int depth_limit_{512};
};

}  // namespace st::html

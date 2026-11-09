#pragma once

/// @file html_view.hpp
/// @brief `HtmlView`：HTML 预览组件（自研排版；支持基础 CSS 子集，零依赖、无 JS、不联网）。
///
/// ## 能力边界（写全，不含糊）
///
/// | 能力 | 状态 |
/// |---|---|
/// | HTML 标签树（含隐式闭合、空元素、注释、CDATA、实体） | ✅ |
/// | `<style>` 内联样式表 + `style="…"` 内联声明 | ✅ |
/// | 标签 / `.class` / `#id` / `*` / 后代 / 子代 / 选择器列表 | ✅ |
/// | 层叠（特异性 + 顺序 + `!important`）与可继承属性 | ✅ |
/// | 块级流（段落/标题/列表/引用/代码块/分隔线）+ 行内混排（含折行） | ✅ |
/// | 表格（按行/单元格简化排版） | ✅ |
/// | `<pre>` 保留空白、`white-space` 四种取值 | ✅ |
/// | 滚动（内容高超过视口时可滚） | ✅ |
/// | 链接点击（上报 URL，打开由宿主做） | ✅ |
/// | `<script>` 执行 / JS | ❌ 明确不执行 |
/// | 外部 CSS（`<link>`）/ 外部图片（`<img src="http…">`） | ❌ 不联网，不取资源 |
/// | `float` / `position` / `flex` / `grid` / `transform` / 动画 | ❌ 忽略这些声明 |
///
/// 「忽略声明而不是报错」是刻意取向：真实网页里有一堆不认识的属性，
/// 一条不认识就让整页不可读是不可接受的。
///
/// ## 为什么自研而不内嵌 WebView
///
/// 宿主系统 WebView 能渲染真网页，但它带来三件与框架取向冲突的事：
/// ① 依赖系统运行时（无头 CI / 跨平台打包都会缺）；② 无法被像素回归驱动
/// （霜天的验证闭环长在 `capture` + `visual_diff` 上）；③ 引入一条不受控的
/// 外部渲染路径（同一份输入在不同机器上结果不同）。本组件的目标读者是
/// **智能体产出的 HTML 产物预览**（文档/报告/卡片），它需要的是「结果确定、可截图断言」。
///
/// ## 两趟渲染（与 `MarkdownView` 同构）
///
/// `measure` 把文档树排版成扁平行模型（内容坐标），`paint_content` 只按滚动偏移
/// 平移绘制——画不改布局、滚不重排版。

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/html/style.hpp"
#include "st/math/geometry.hpp"
#include "st/ui/animation.hpp"
#include "st/ui/component_registry.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// HTML 预览组件。
class HtmlView : public Element {
 public:
  HtmlView();
  explicit HtmlView(std::string html);

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "HtmlView"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Markdown; }

  // —— 内容 ——
  /// 全量替换 HTML 内容（重新解析 + 重新排版）。
  void set_html(std::string html);
  [[nodiscard]] auto html() const noexcept -> const std::string& { return source_; }
  /// 内容 `title`（`<title>` 文本；没有为空）。
  [[nodiscard]] auto title() const -> const std::string& { return document_.title; }
  /// 纯文本（去标签、按文档顺序）：便于控制通道 `find` / `wait` 定位内容。
  [[nodiscard]] auto plain_text() const -> const std::string& { return plain_; }
  /// 解析出的样式规则条数（诊断「这份 CSS 认了几条」）。
  [[nodiscard]] auto rule_count() const noexcept -> std::size_t { return style_sheet_.rules.size(); }
  /// 排版出的行数（诊断）。
  [[nodiscard]] auto line_count() const noexcept -> std::size_t { return rows_.size(); }

  // —— 外观 ——
  /// 正文基准字号（`<= 0` = 用主题 `font_base`）。
  void set_base_font_size(float size);
  [[nodiscard]] auto base_font_size() const noexcept -> float { return base_font_size_; }
  /// 内边距（px）。
  void set_padding(float padding);
  /// 是否用主题色覆盖文档自带的文字/背景色（默认 **false**：预览网页就该像网页）。
  void set_follow_theme(bool value);
  /// 内容最大宽度（`<= 0` = 撑满；阅读类页面限宽更接近浏览器观感）。
  void set_max_content_width(float width);
  /// 滚动偏移（px，从内容顶算起）。
  void set_scroll(float offset);
  [[nodiscard]] auto scroll() const noexcept -> float { return scroll_; }

  /// 链接点击回调（参数为 `href`）。打开浏览器是宿主的事。
  std::function<void(std::string_view url)> on_link_click{};

  /// 文档底色（`<body>`/`<html>` 的 `background`；没有则白）。
  [[nodiscard]] auto document_background() const noexcept -> math::Color {
    return document_background_;
  }

  /// 排版结果的自省快照（**只读诊断面**）：每行的内容坐标矩形 + 文本。
  ///
  /// 为何要暴露它：行内混排的缺陷（片段重叠）在属性面上完全看不出来
  /// （段落数、文本内容全对），而像素断言又拿不到“谁压在谁上面”。
  /// 有了几何快照，测试可以直接断言“同一行的片段 x 递增且首尾相接”。
  struct RowSnapshot {
    math::Rect box{};
    math::Rect ink{};   ///< 该片段的**自身**矩形（背景/边框画它，不被块装饰拉伸）
    std::string text{};
  };
  [[nodiscard]] auto row_geometry() const -> std::vector<RowSnapshot>;

  // —— Element 协议 ——
  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  [[nodiscard]] auto hit_test(math::Point point) const noexcept -> bool override;
  [[nodiscard]] auto semantics_text() const -> std::string override { return plain_; }
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto semantics_flags() const -> SemanticsFlags override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto readonly_property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

 private:
  /// 排版出的一行（内容坐标：相对内容盒左上角）。
  struct Row {
    math::Rect box{};          ///< 行盒（含 padding 的行内容区）
    std::string text{};        ///< 该行的文本（空 = 只有盒子，如分隔线/图片占位）
    float font_size{16.0f};
    math::Color color{};
    math::Color background{0, 0, 0, 0};
    bool has_background{false};
    std::uint16_t font_weight{400};
    bool italic{false};
    bool monospace{false};
    bool underline{false};
    bool strikethrough{false};
    float line_height{1.45f};
    float letter_spacing{0.0f};
    /// 文本相对行盒的水平偏移（对齐 / 列表缩进）。
    float text_x{0.0f};
    /// 该片段**自身**的矩形（行内背景色/边框/下划线画它）。
    ///
    /// 为何要与 `box` 分开：块级装饰（`<div class="box">` 的背景/边框）会把
    /// `box` 横向拉伸到整块宽——那是**块的**矩形。行内元素（`<code>` 的灰底）
    /// 需要的是**自己的**矩形；两者共用一个字段就会把行内底色拉满整行。
    math::Rect ink{};
    /// 该行水平对齐（行内流分段后，对齐只能在「整行」上生效）。
    html::TextAlign text_align{html::TextAlign::Start};
    /// 列表项目符号（非空则画）。
    std::string bullet{};
    /// 边框（`border_width > 0` 时画；只画整块，不做逐边）。
    float border_width{0.0f};
    math::Color border_color{0, 0, 0, 0};
    /// 只画左边那道线（`border-left` / 引用块）。
    bool border_left_only{false};
    float radius{0.0f};
    /// 链接 URL（非空 = 该行可点）。
    std::string link{};
    /// 该行所属块元素声明的 CSS `animation`（`nullptr` = 无）。
    ///
    /// 为何挂在**行**上：本组件是扁平行列表（`rows_`）而不是元素树，
    /// 而动画的生效范围是“那个块元素”（可能占多行）。只持指针，不持数据——
    /// `AnimationSpec` 的生命周期随 `style_sheet_`（与 `resolver_` 同生共死）。
    const html::AnimationSpec* animation{nullptr};
    /// 该行所属块元素的 `transition-duration`（秒；`0` = 无）。
    double transition_duration{0.0};
  };

  /// 排版中间状态：当前块（一个块可以占多行）。
  struct BlockLayout {
    float x{0.0f};
    float y{0.0f};
    float width{0.0f};
    float padding_left{0.0f};
    float padding_right{0.0f};
    float padding_top{0.0f};
    float padding_bottom{0.0f};
    math::Color background{0, 0, 0, 0};
    bool has_background{false};
    float border_width{0.0f};
    math::Color border_color{0, 0, 0, 0};
    bool border_left_only{false};
    float radius{0.0f};
    /// 块首行的行索引（画背景用）。
    std::size_t first_row{0};
    /// 块声明的动画/过渡（排版时从 `html::Style` 抄下来，落到它产生的每一行上）。
    const html::AnimationSpec* animation{nullptr};
    double transition_duration{0.0};
    bool has_rows{false};
  };

  /// 局部样式环境（行内嵌套：`<b><i>x</i></b>` 要能叠加）。
  struct InlineStyle {
    float font_size{16.0f};
    math::Color color{};
    std::uint16_t font_weight{400};
    bool italic{false};
    bool monospace{false};
    bool underline{false};
    bool strikethrough{false};
    float letter_spacing{0.0f};
    float line_height{1.45f};
    std::string link{};
    /// 行内元素自己的背景（`<code>` 的灰底）。
    math::Color inline_background{0, 0, 0, 0};
    bool has_inline_background{false};
    /// 行内元素自带的内边距（`<code>` 的 padding）——同时用于**占位**与**底色扩边**。
    ///
    /// 为何必须从行内流一路带下来：`<code>a</code><code>b</code>` 两个相邻片段若
    /// 不各自外扩，两块的底色会连成一整条（看起来像一个大号代码块而不是两个）。
    /// 只取 `padding_left`（左右对称的写法常见），两侧都按它外扩。
    float inline_padding{0.0f};
  };

  /// 重新排版（`measure` 调用；结果落进 `rows_` 与 `content_height_`）。
  void rebuild(const RenderContext& context, float available_width);
  /// 一个行内文本片段（**不定长**：真正的段落分行在排版结束后统一做）。
  ///
  /// 为何不只是字符串数组（首版的错误）：行内混排 `<p>a <b>b</b> c</p>` 要求三段
  /// 落在同一行——而代码里若用 `lines[] + styles[]` 一一对应，每个片段就各自成了一行，
  /// 于是截图上是“粗体占一行、斜体占一行”。现在用行内流（各片段拼成一串、
  /// 携各自样式与起始字符偏移），折行在 `finish_inline_flow()` 里统一算。
  struct Run {
    std::string text{};
    InlineStyle style{};
  };
  struct InlineFlow {
    std::vector<Run> runs{};
    /// 行内流的总字符数（用于把字符偏移映射到行）。
    std::size_t length{0};
  };

  /// 递归排版一个节点（行内内容推进 `flow`，块级内容产生 `rows_`）。
  void layout_node(const RenderContext& context, const html::Node& node, float left, float top,
                   float width, InlineStyle inline_style, float& cursor_y, InlineFlow& flow,
                   BlockLayout* block);
  /// 把行内流折成 `rows_`（**行内混排的唯一出口**）。
  void finish_inline_flow(const RenderContext& context, InlineFlow& flow, float left,
                          float& cursor_y, float width, BlockLayout& block);
  /// 把一个块的行缓冲冲刷成 `rows_`。
  void flush_lines(const RenderContext& context, std::vector<std::string>& lines,
                   std::vector<InlineStyle>& styles, float left, float& cursor_y, float width,
                   BlockLayout& block);
  /// 收集纯文本。
  static void collect_text(const html::Node& node, std::string& out);
  /// 计算可点行的 URL（命中用）。
  [[nodiscard]] auto link_at(math::Point point) const -> std::string;
  /// `html::KeyframesSpec` → 引擎关键帧（属性名不同、值已解析）。
  ///
  /// 为何要做这一层转换而不是让 `html` 层直接产 `anim::Keyframes`：
  /// 两个模块是**独立层**（`html` 是解析，`ui` 是绘制）。让解析器 include
  /// UI 引擎的头，会让“只想预览一段静态 HTML”的宿主也拖进整套动画。
  [[nodiscard]] static auto to_engine_keyframes(const html::KeyframesSpec& spec)
      -> anim::Keyframes;
  /// `html::AnimationSpec` → 引擎播放参数（含 `timing` 字符串解析）。
  [[nodiscard]] static auto to_engine_animation(const html::AnimationSpec& spec)
      -> anim::Animation;
  /// 本份文档的动画起点（单调秒；首次绘制时定）。
  ///
  /// 为何不是“每个块各自一个起点”：CSS 里同一份文档的多个 `animation`
  /// 都在**页面就绪那一刻**同时起算（错开靠 `delay`，不靠错开起点）。
  /// 各自计时会让“重新排版一次”就重置一部分动画（重排会重建 `rows_`）。
  mutable double html_animation_start_{-1.0};

  /// 文档底色（从 `<body>`/`<html>` 的 `background` 取；没有则白）。
  ///
  /// ## 为何必须自己画
  ///
  /// 网页的文字色是**按白底设计的**（如 `#1b1f24`）。若把这样的文字直接画在
  /// 暗色主题的 `surface` 上，结果是一片“几乎看不见的深灰”——预览就不叫预览了。
  /// 浏览器不会把网页文字“适配”到宿主主题，这里也不该（`follow_theme` 是显式开关）。
  /// 所以组件自己铺一层文档底色，文字色才回到作者的原意。
  math::Color document_background_{255, 255, 255, 255};
  std::string source_{};
  html::Document document_{};
  html::StyleSheet style_sheet_{};
  /// 样式解析器（**持有引用成员，不可赋值**——所以用指针持有并重建）。
  std::unique_ptr<html::StyleResolver> resolver_{};
  std::string plain_{};
  mutable std::vector<Row> rows_{};
  float content_height_{0.0f};
  float content_width_{0.0f};
  float scroll_{0.0f};
  float padding_{14.0f};
  float base_font_size_{0.0f};
  float max_content_width_{0.0f};
  bool follow_theme_{false};
  bool layout_valid_{false};
  float laid_out_width_{-1.0f};
  mutable int hover_row_{-1};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::HtmlView, "HtmlView")

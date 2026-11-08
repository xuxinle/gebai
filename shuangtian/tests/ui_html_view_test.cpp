/// `HtmlView` 回归护栏：HTML 解析 → CSS 子集 → 排版 → 绘制。
///
/// ## 分层守（与实现同构，失败时能直接指到层）
///
/// | 层 | 守什么 | 为什么 |
/// |---|---|---|
/// | 解析 | 隐式闭合、空元素、实体、`<pre>` 空白、`<style>` 提取 | 真实网页大量不写闭合标签；这几条错了树就整棵歪 |
/// | 样式 | 选择器匹配、特异性、`!important`、继承 | 层叠错了表现为“某个样式不生效”，肉眼极难定位 |
/// | 排版 | 块级纵向推进、行内折行、列表符号、表格分栏 | 属性全对而画面乱，是这类组件最典型的失败形态 |
/// | 绘制 | 像素级：文字/背景/边框真的画出来了 | 防止“外壳画了、内容空”（包一在 `SegmentedControl` 上真踩过） |

#include "st/test/test.hpp"

#include <cmath>
#include <string>
#include <vector>

#include "st/html/dom.hpp"
#include "st/html/style.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/components/html_view.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"

#include "tests/support/text_port_fixtures.hpp"

using st::test::FixedAdvanceTextPort;

namespace {

using st::math::Point;
using st::math::Rect;

[[nodiscard]] auto find_child(const st::html::Node& node, std::string_view tag)
    -> const st::html::Node* {
  return node.child(tag);
}

}  // namespace

// ————————————————————————————————————————————————————————————————————————————
// 解析层
// ————————————————————————————————————————————————————————————————————————————

/// 隐式闭合：`<li>a<li>b` 必须是**两个兄弟**，不是层层嵌套。
///
/// 这条错了的表现是「每段越缩越深」——数据层看起来完全合法（树就是深的）。
ST_TEST(html_parse_implicit_close_makes_siblings) {
  const auto doc = st::html::parse("<ul><li>甲<li>乙<li>丙</ul>");
  const st::html::Node* ul = find_child(doc.root, "ul");
  ST_REQUIRE(ul != nullptr);
  int items = 0;
  for (const st::html::Node& child : ul->children) {
    if (child.kind == st::html::NodeKind::Element && child.tag == "li") ++items;
  }
  ST_CHECK_EQ(items, 3);
  // 第一个 li 里不能再有 li（那才是层层嵌套的形态）。
  int nested = 0;
  for (const st::html::Node& child : ul->children) {
    if (child.tag != "li") continue;
    for (const st::html::Node& inner : child.children) {
      if (inner.kind == st::html::NodeKind::Element && inner.tag == "li") ++nested;
    }
  }
  ST_CHECK_EQ(nested, 0);
}

/// 空元素：`<br>` / `<img>` / `<hr>` 不需要闭合，也不会吞掉后面的内容。
ST_TEST(html_parse_void_elements_do_not_eat_content) {
  ST_CHECK(st::html::is_void_element("br"));
  ST_CHECK(st::html::is_void_element("img"));
  ST_CHECK(st::html::is_void_element("hr"));
  ST_CHECK(!st::html::is_void_element("div"));

  const auto doc = st::html::parse("<div>前<br>后</div>");
  const st::html::Node* div = find_child(doc.root, "div");
  ST_REQUIRE(div != nullptr);
  // br 是 div 的孩子，而「后」仍是 div 的文本孩子（没被吞到 br 里）。
  int texts = 0;
  int brs = 0;
  for (const st::html::Node& child : div->children) {
    if (child.kind == st::html::NodeKind::Text) ++texts;
    if (child.kind == st::html::NodeKind::Element && child.tag == "br") ++brs;
  }
  ST_CHECK_EQ(brs, 1);
  ST_CHECK(texts >= 2);   // "前" + "后"
}

/// 实体解码与 `<pre>` 空白保留。
ST_TEST(html_parse_entities_and_pre_whitespace) {
  const auto doc = st::html::parse("<p>a &lt; b &amp;&amp; &#65;&#x42;</p>");
  const st::html::Node* p = find_child(doc.root, "p");
  ST_REQUIRE(p != nullptr);
  std::string text;
  for (const st::html::Node& child : p->children) {
    if (child.kind == st::html::NodeKind::Text) text += child.text;
  }
  ST_CHECK_EQ(text, std::string{"a < b && AB"});

  // pre：换行与缩进保留（普通元素里它们会被折叠成单个空格）。
  const auto pre_doc = st::html::parse("<pre>第一行\n  缩进</pre>");
  const st::html::Node* pre = find_child(pre_doc.root, "pre");
  ST_REQUIRE(pre != nullptr);
  std::string pre_text;
  for (const st::html::Node& child : pre->children) {
    if (child.kind == st::html::NodeKind::Text) pre_text += child.text;
  }
  ST_CHECK(pre_text.find('\n') != std::string::npos);
  ST_CHECK(pre_text.find("  ") != std::string::npos);
}

/// `<style>` 内容被抽到 `style_text`，且**不进**渲染树与语义文本。
///
/// 这条错了会很隐蔽：`<style>` 的 CSS 文本会被当正文画出来。
ST_TEST(html_parse_style_extracted_not_rendered) {
  const auto doc =
      st::html::parse("<style>p { color: red; }</style><p>正文</p>");
  ST_CHECK(doc.style_text.find("color") != std::string::npos);
  // `p` 应该是根的孩子，且**没有**把 CSS 文本当正文。
  const st::html::Node* p = find_child(doc.root, "p");
  ST_REQUIRE(p != nullptr);
  const st::html::Node* style = find_child(doc.root, "style");
  ST_REQUIRE(style != nullptr);
  // style 元素自身没有文本孩子（内容在 style_text 里，不在树里）。
  int style_texts = 0;
  for (const st::html::Node& child : style->children) {
    if (child.kind == st::html::NodeKind::Text) ++style_texts;
  }
  ST_CHECK_EQ(style_texts, 0);
}

// ————————————————————————————————————————————————————————————————————————————
// 样式层
// ————————————————————————————————————————————————————————————————————————————

/// 选择器与特异性：`#id` > `.cls` > `tag`，同特异性看顺序，`!important` 最高。
ST_TEST(html_css_specificity_and_important) {
  const auto sheet = st::html::StyleSheet::parse(R"(
    p { color: #000000; }
    .warn { color: #ff0000; }
    #special { color: #00ff00; }
    .warn { color: #0000ff; }
    #special { color: #123456 !important; }
  )");
  ST_CHECK(sheet.rules.size() >= 4U);

  const auto doc = st::html::parse(R"(<p id="special" class="warn">x</p>)");
  st::html::StyleResolver resolver(sheet, st::html::StyleResolver::initial_style());
  resolver.resolve_tree(doc.root);
  const st::html::Node* p = find_child(doc.root, "p");
  ST_REQUIRE(p != nullptr);
  const st::html::Style& style = resolver.style_of(p);
  // `!important` 的 #123456 胜出。
  ST_CHECK_EQ(static_cast<int>(style.color.r), 0x12);
  ST_CHECK_EQ(static_cast<int>(style.color.g), 0x34);
  ST_CHECK_EQ(static_cast<int>(style.color.b), 0x56);
}

/// 继承：可继承属性（颜色/字号）下沉；**不可继承属性（边距）不下沉**。
///
/// 后者是 CSS 里最经典的误继承——`margin` 一旦继承，页面会整块撑歪。
ST_TEST(html_css_inheritance_is_selective) {
  const auto sheet = st::html::StyleSheet::parse("div { color: #ff0000; font-size: 20px; margin-top: 40px; }");
  const auto doc = st::html::parse("<div><span>x</span></div>");
  st::html::StyleResolver resolver(sheet, st::html::StyleResolver::initial_style());
  resolver.resolve_tree(doc.root);
  const st::html::Node* div = find_child(doc.root, "div");
  ST_REQUIRE(div != nullptr);
  const st::html::Node* span = find_child(*div, "span");
  ST_REQUIRE(span != nullptr);

  const st::html::Style& div_style = resolver.style_of(div);
  const st::html::Style& span_style = resolver.style_of(span);
  ST_CHECK_NEAR(div_style.font_size, 20.0f, 0.01f);
  // 继承：字号与颜色跟着下去。
  ST_CHECK_NEAR(span_style.font_size, 20.0f, 0.01f);
  ST_CHECK_EQ(static_cast<int>(span_style.color.r), 0xff);
  // 不继承：边距。
  ST_CHECK_NEAR(span_style.margin_top, 0.0f, 0.01f);
}

/// 不认识的属性被忽略，但**不影响**同一条规则里的其它声明。
///
/// 这条是「预览真实网页」的可用性底线：一份样式表里有 `box-shadow`/`transition`
/// 不该让整条规则失效。
ST_TEST(html_css_unknown_properties_do_not_kill_the_rule) {
  const auto sheet = st::html::StyleSheet::parse(
      "div { box-shadow: 0 1px 2px red; transition: all .2s; color: #0000ff; float: left; }");
  const auto doc = st::html::parse("<div>x</div>");
  st::html::StyleResolver resolver(sheet, st::html::StyleResolver::initial_style());
  resolver.resolve_tree(doc.root);
  const st::html::Node* div = find_child(doc.root, "div");
  ST_REQUIRE(div != nullptr);
  // `color` 生效（尽管它两侧都是不认识的属性）。
  ST_CHECK_EQ(static_cast<int>(resolver.style_of(div).color.b), 0xff);
}

/// 伪类/属性选择器**明确不支持**：写了就丢掉那条选择器（不静默当通配）。
ST_TEST(html_css_pseudo_class_is_dropped_not_wildcarded) {
  const auto sheet = st::html::StyleSheet::parse("a:hover { color: #ff0000; } p { color: #00ff00; }");
  const auto doc = st::html::parse("<a href='#'>x</a>");
  st::html::StyleResolver resolver(sheet, st::html::StyleResolver::initial_style());
  resolver.resolve_tree(doc.root);
  const st::html::Node* a = find_child(doc.root, "a");
  ST_REQUIRE(a != nullptr);
  // 若被当通配，`<a>` 会拿到红色；正确行为是保留 UA 的链接蓝。
  ST_CHECK(resolver.style_of(a).color.r != 0xff || resolver.style_of(a).color.g != 0x00);
}

// ————————————————————————————————————————————————————————————————————————————
// 组件层：排版与绘制
// ————————————————————————————————————————————————————————————————————————————

/// 排版真的产出了行，且块级结构让它**纵向递增**（不是全叠在 y=0）。
ST_TEST(html_view_layouts_blocks_downward) {
  st::ui::Theme theme = st::ui::Theme::light();
  FixedAdvanceTextPort port;
  st::ui::RenderContext context{theme, &port, 0.0};

  st::ui::HtmlView view(R"(<h1>标题</h1><p>第一段</p><p>第二段</p>)");
  view.apply_theme(theme);
  const st::ui::Constraints constraints;
  view.measure(context, constraints);
  view.arrange(context, Rect{0.0f, 0.0f, 480.0f, view.measured_size().height});

  ST_CHECK(view.line_count() >= 3U);
  // 语义文本涵盖三段内容。
  const std::string plain = view.plain_text();
  ST_CHECK(plain.find("标题") != std::string::npos);
  ST_CHECK(plain.find("第一段") != std::string::npos);
  ST_CHECK(plain.find("第二段") != std::string::npos);
  // 内容高必须大于单行高（说明纵向真的推进了）。
  ST_CHECK(view.measured_size().height > 40.0f);
}

/// 绘制真的画出了文字（像素级）：不是只有背景。
///
/// 这条对应的缺陷形态：`SegmentedControl` 首版只画了外壳、文字全空，
/// 而属性面完全正常——只有看像素/截图能发现。
ST_TEST(html_view_paints_text_pixels) {
  st::ui::Theme theme = st::ui::Theme::light();
  FixedAdvanceTextPort port;
  st::ui::RenderContext context{theme, &port, 0.0};

  st::ui::HtmlView view("<p>abc</p>");
  view.apply_theme(theme);
  const st::ui::Constraints constraints;
  view.measure(context, constraints);
  const float height = std::min(view.measured_size().height, 200.0f);
  view.arrange(context, Rect{0.0f, 0.0f, 320.0f, height});

  // 桩端口不落像素，所以这里断言的是「文本被送进端口绘制」这条链路。
  st::raster::Canvas canvas(320, static_cast<int>(height));
  canvas.clear(theme.colors().surface);
  view.paint(context, canvas);
  bool painted_text = false;
  for (const std::string& drawn : port.drawn()) {
    if (drawn.find("abc") != std::string::npos) {
      painted_text = true;
      break;
    }
  }
  ST_CHECK(painted_text);
}

/// 折行：长文本在给定宽度下被折成多行（不是一行溢出）。
ST_TEST(html_view_wraps_long_text) {
  st::ui::Theme theme = st::ui::Theme::light();
  FixedAdvanceTextPort port;   // 每字符 8px
  st::ui::RenderContext context{theme, &port, 0.0};

  st::ui::HtmlView narrow(std::string("<p>") + std::string(60, 'x') + "</p>");
  narrow.set_padding(0.0f);
  narrow.apply_theme(theme);
  st::ui::Constraints narrow_constraints{};
  narrow_constraints.max_width = 160.0f;
  narrow.measure(context, narrow_constraints);
  // 160px / 8px = 20 字符/行 → 60 字符至少要 3 行。
  ST_CHECK(narrow.line_count() >= 3U);

  st::ui::HtmlView wide(std::string("<p>") + std::string(60, 'x') + "</p>");
  wide.set_padding(0.0f);
  wide.apply_theme(theme);
  st::ui::Constraints wide_constraints{};
  wide_constraints.max_width = 900.0f;
  wide.measure(context, wide_constraints);
  // 900px / 8px = 112 字符/行 → 60 字符一行放得下。
  ST_CHECK(wide.line_count() == 1U);
  ST_CHECK(narrow.line_count() > wide.line_count());
}

/// 列表：每个 `<li>` 产出一行，且带项目符号；有序列表带序号。
ST_TEST(html_view_lists_produce_items) {
  st::ui::Theme theme = st::ui::Theme::light();
  FixedAdvanceTextPort port;
  st::ui::RenderContext context{theme, &port, 0.0};
  const st::ui::Constraints constraints;

  st::ui::HtmlView view("<ul><li>甲</li><li>乙</li></ul>");
  view.apply_theme(theme);
  view.measure(context, constraints);
  view.arrange(context, Rect{0.0f, 0.0f, 400.0f, view.measured_size().height});
  ST_CHECK(view.line_count() >= 2U);
  ST_CHECK(view.plain_text().find("甲") != std::string::npos);
  ST_CHECK(view.plain_text().find("乙") != std::string::npos);
}

/// `border-left` 只画左缘一道（不是四条框）。
///
/// ## 为何单独一条
///
/// 引用式提示块/引用块在 CSS 里是 `border-left: 4px solid #x`。若把它当成矩形描边，
/// 画出来是一个**四边都有框**的盒子——读起来像“套了个框”而不是“引用”。
/// 这条同时钉住两件事：解析结果带 `border_left_only`，且**渲染时只在左缘**出墨。
ST_TEST(html_css_border_left_draws_only_left_edge) {
  // ① 解析层
  const auto sheet = st::html::StyleSheet::parse(".box { border-left: 4px solid #2563eb; }");
  const auto doc = st::html::parse(R"(<div class="box">x</div>)");
  st::html::StyleResolver resolver(sheet, st::html::StyleResolver::initial_style());
  resolver.resolve_tree(doc.root);
  const st::html::Node* div = find_child(doc.root, "div");
  ST_REQUIRE(div != nullptr);
  const st::html::Style& style = resolver.style_of(div);
  ST_CHECK_NEAR(style.border_width, 4.0f, 0.01f);
  ST_CHECK(style.border_left_only);

  // ② 绘制层：左缘有色、右缘无色
  st::ui::Theme theme = st::ui::Theme::light();
  FixedAdvanceTextPort port;
  st::ui::RenderContext context{theme, &port, 0.0};
  st::ui::HtmlView view(
      R"(<style>.box { border-left: 6px solid #ff0000; padding: 4px; }</style>"
      R"(<div class="box">内容够长到占一行</div>)");
  view.apply_theme(theme);
  view.measure(context, st::ui::Constraints{});
  view.arrange(context, st::math::Rect{0.0f, 0.0f, 400.0f, view.measured_size().height});

  st::raster::Canvas canvas(400, static_cast<int>(std::max(view.measured_size().height, 30.0f)));
  canvas.clear(theme.colors().surface);
  view.paint(context, canvas);

  // 在盒子中部的高度上横扫一行，统计“红色墨迹”出现在哪些 x 区间。
  const int mid_y = canvas.physical_height() / 2;
  int red_min_x = 1'000'000;
  int red_max_x = -1;
  for (int x = 0; x < canvas.physical_width(); ++x) {
    const auto px = canvas.pixel_at(x, mid_y);
    // 红色墨迹：R 明显大于 G/B
    if (static_cast<int>(px.r) > static_cast<int>(px.g) + 40 &&
        static_cast<int>(px.r) > static_cast<int>(px.b) + 40) {
      red_min_x = std::min(red_min_x, x);
      red_max_x = std::max(red_max_x, x);
    }
  }
  ST_CHECK(red_max_x >= 0);                       // 左缘真的有线
  // 红线只能贴着左缘（宽度不超过边框 + 一点抗锯齿）——若画成四边框，
  // 右缘也会出现红像素，`red_max_x` 会跑到画面右侧。
  ST_CHECK(red_max_x < 40);
}

/// **行内片段的水平位置必须首尾相接**（不重叠、不重叠留白）。
///
/// ## 为何必须有这条（它抓的是一个真实缺陷）
///
/// 首版 `layout_node` 在递归行内元素时把 `cursor_y` 作为游标传进去，而 `<code>`
/// 的 UA 样式带 `padding`/`margin`，于是游标被推进——下一段文字就**盖在上一个字上**。
/// 表现是“`.class#id` 糊成一团”，而且每个行内元素的缺陷就在它自己身上，
/// 带 `padding` 的块里，**首个 run 与后续行内元素不能重叠**。
///
/// ⚠ 为何与下一条分开：`html_view_inline_runs_do_not_overlap` 用的是**无 padding**
/// 的 `<p>`，而缺陷只出现在「块有 padding 且内部有行内元素」时——`padding` 被算了
/// 两次，于是首个 run 不动、后续被踢 10px，两段文字叠在一起（截图实测：
/// 引用块里「引用式提示块：」与「背景色 + 左边框」重叠）。
/// 无 padding 的块上两者位置相同，那条用例自然看不出问题。
ST_TEST(html_padded_block_inline_runs_do_not_overlap) {
  st::ui::Theme theme = st::ui::Theme::light();
  FixedAdvanceTextPort port;
  st::ui::RenderContext context{theme, &port, 0.0};

  st::ui::HtmlView view(
      R"(<style>.box { background: #eef2ff; padding: 10px; }</style>`
         `<div class="box">前缀 <strong>加粗</strong> 结尾</div>)");
  view.apply_theme(theme);
  view.measure(context, st::ui::Constraints{});
  view.arrange(context, st::math::Rect{0.0f, 0.0f, 400.0f, view.measured_size().height});

  // ⚠ 断言 **`ink`**（片段自身矩形）而不是 `box`：
  // 有背景/边框的块会把同一行所有片段的 `box` 拉成整块宽（铺底色用），
  // 于是每个片段的 `box.x` 都是块左缘——拿它判重叠永远“不重叠”（假绿）。
  // 文字真正的横向位置在 `ink.x`。
  const auto geometry = view.row_geometry();
  float previous_right = -1.0f;
  bool saw_first = false;
  for (const auto& row : geometry) {
    if (row.text.find("前缀") != std::string::npos) saw_first = true;
    if (!saw_first) continue;
    ST_CHECK(row.ink.x >= previous_right - 0.5f);
    previous_right = row.ink.right();
  }
  ST_CHECK(saw_first);
}

/// 属性面完全正常（段落数、文本内容都对）。
ST_TEST(html_view_inline_runs_do_not_overlap) {
  st::ui::Theme theme = st::ui::Theme::light();
  FixedAdvanceTextPort port;
  st::ui::RenderContext context{theme, &port, 0.0};

  st::ui::HtmlView view(
      R"(<p>前缀 <code>CODE</code> 中间 <b>BOLD</b> 结尾</p>)");
  view.set_padding(0.0f);
  view.apply_theme(theme);
  view.measure(context, st::ui::Constraints{});
  view.arrange(context, st::math::Rect{0.0f, 0.0f, 600.0f, view.measured_size().height});

  // 收集该段的行内片段：取每一行内的所有非空文本盒，断言它们首尾相接（不重叠）。
  // 桩端口每字符 8px，所以“接不接得上”是很硬的判据。
  st::raster::Canvas canvas(600, static_cast<int>(std::max(view.measured_size().height, 30.0f)));
  canvas.clear(theme.colors().surface);
  view.paint(context, canvas);

  // 逐行检查：**同一行的片段必须首尾相接**（不重叠）。
  // 桩端口每字符 8px，所以一重叠就是两个文字压在一起。
  const auto geometry = view.row_geometry();
  ST_CHECK(geometry.size() >= 3U);
  // 该段在 600px 宽、每字符 8px 下应只占**一个视觉行**（约 25 字符）。
  // 注：`line_count()` 是**行内片段数**（一个片段一个 Row），不是视觉行数；
  // 视觉行数按 y 去重数。
  const auto distinct_lines = [&geometry]() {
    std::vector<float> ys;
    for (const auto& row : geometry) {
      bool seen = false;
      for (const float y : ys) {
        if (std::abs(y - row.box.y) < 0.5f) seen = true;
      }
      if (!seen) ys.push_back(row.box.y);
    }
    return ys.size();
  }();
  ST_CHECK_EQ(distinct_lines, 1U);

  // 同一行的片段必须首尾相接（不重叠）。
  for (std::size_t left = 0; left < geometry.size(); ++left) {
    for (std::size_t right = left + 1U; right < geometry.size(); ++right) {
      if (std::abs(geometry[left].box.y - geometry[right].box.y) >= 0.5f) continue;
      const Rect a = geometry[left].box;
      const Rect b = geometry[right].box;
      const bool disjoint = a.right() <= b.x + 0.5f || b.right() <= a.x + 0.5f;
      ST_CHECK(disjoint);
    }
  }
}

/// **行内片段不得被裁掉尾部**（`<code>` 末尾不下于整串的宽）。
///
/// 为何单独一条：排版期按“逐字符量宽”折行，而绘制期若再按某个盒子宽截一次
/// （或盒子宽没把行内元素自己的 padding 算进去），末尾字就会静默消失——
/// 实测表现为“`.class` 显示成 `.clas`”。这类缺陷属性面完全看不出来。
ST_TEST(html_view_inline_piece_is_not_truncated) {
  st::ui::Theme theme = st::ui::Theme::light();
  FixedAdvanceTextPort port;   // 每字符 8px
  st::ui::RenderContext context{theme, &port, 0.0};

  st::ui::HtmlView view(R"(<p><code>.class</code> 尾部文字</p>)");
  view.set_padding(0.0f);
  view.apply_theme(theme);
  view.measure(context, st::ui::Constraints{});
  view.arrange(context, st::math::Rect{0.0f, 0.0f, 600.0f, view.measured_size().height});

  // 片段盒宽必须 ≥ 文字本身的宽（桩端口每字符 8px）。
  const auto geometry = view.row_geometry();
  bool found = false;
  for (const auto& row : geometry) {
    if (row.text.find(".class") == std::string::npos) continue;
    found = true;
    const float expected = FixedAdvanceTextPort::kAdvance * 6.0f;   // ".class" 六个字符
    ST_CHECK(row.ink.width >= expected - 0.5f);
  }
  ST_CHECK(found);

  // 真的被画出来了（且原文完整）。
  st::raster::Canvas canvas(600, static_cast<int>(std::max(view.measured_size().height, 30.0f)));
  canvas.clear(theme.colors().surface);
  view.paint(context, canvas);
  bool painted = false;
  for (const std::string& text : port.drawn()) {
    if (text == ".class") painted = true;
  }
  ST_CHECK(painted);
}

/// 属性面：`html` 往返、`rules`/`lines` 可读、未知属性拒绝。
ST_TEST(html_view_property_surface) {
  st::ui::HtmlView view;
  ST_CHECK(view.set_property("html", "<p>hi</p>"));
  ST_CHECK_EQ(view.get_property("html").value_or(""), std::string{"<p>hi</p>"});
  ST_CHECK(view.get_property("lines").has_value());
  ST_CHECK(view.get_property("rules").has_value());

  ST_CHECK(view.set_property("scroll", "12"));
  ST_CHECK(!view.get_property("__nope__").has_value());
  ST_CHECK(!view.set_property("__nope__", "x"));
  ST_CHECK(!view.invoke_action("__nope__", ""));

  // `reload`：同一份源重新解析（幂等）。
  const std::string before = view.get_property("html").value_or("");
  ST_CHECK(view.invoke_action("reload", ""));
  ST_CHECK_EQ(view.get_property("html").value_or(""), before);
}

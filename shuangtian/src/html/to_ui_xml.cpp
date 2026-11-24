#include "st/html/to_ui_xml.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "st/html/style.hpp"

namespace st::html {
namespace {

// ————————————————————————————————————————————————————————————————————————————
// 取向与约束（写在这里，改代码前先读）
// ————————————————————————————————————————————————————————————————————————————
//
// 本文件的输入是 **已解析的 HTML 树 + 已解析的样式**（`st::html::parse` 与
// `st::html::StyleResolver`），输出是**霜天 XML 文本**。中间没有任何渲染：
// 它不做像素排版，只做"语义 + 样式声明"的搬运。
//
// 三条贯穿全文的判据：
//
// 1. **结构优先于视觉效果**。块级 → `<Column>`，行内内容合并成一个 `<Text>`。
//    不为了还原浏览器盒子而切碎行内片段（会破坏文本顺序、且霜天 Text 是单样式单元）。
// 2. **生成的东西必须能被 `build_from_xml` 严格模式吃下**。属性名一律取
//    `xml_ui.cpp` 白名单里的名字，样式串一律用 `sheet_key_names()` 的键。
//    拼错不会被静默吞掉——建树报告会报，但那是下游的事；生成器不该制造它。
// 3. **丢掉了什么必须报**。unsupported 的 CSS 声明、未映射的标签、图片、脚本
//    全部进报告。静默丢弃会让"翻了但样式不对"变成谜。

/// 转义成 XML 文本（`< > & " '`）。
[[nodiscard]] auto escape_xml(std::string_view text) -> std::string {
  std::string out;
  out.reserve(text.size() + text.size() / 8);
  for (const char ch : text) {
    switch (ch) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default: out.push_back(ch); break;
    }
  }
  return out;
}

/// 去掉首尾空白（不折内部）。
[[nodiscard]] auto trim_view(std::string_view text) -> std::string_view {
  const auto is_space = [](char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
  };
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && is_space(text[begin])) ++begin;
  while (end > begin && is_space(text[end - 1])) --end;
  return text.substr(begin, end - begin);
}

/// 小数点后最多两位、去掉无意义的尾零。
///
/// 为何不直接 `{:.2f}`：`gap="8.00"` 读起来像机器产物，而生成的 XML 是给人改的。
/// `gap="8"` 才是可读的；但 `0.35` 这类值必须保住精度（所以不是"取整"）。
[[nodiscard]] auto num(float value) -> std::string {
  if (std::abs(value - std::round(value)) < 0.005f) {
    return std::format("{}", static_cast<std::int64_t>(std::llround(value)));
  }
  std::string text = std::format("{:.2f}", static_cast<double>(value));
  while (!text.empty() && text.back() == '0') text.pop_back();
  if (!text.empty() && text.back() == '.') text.pop_back();
  return text;
}

// ————————————————————————————————————————————————————————————————————————————
// 标签映射
// ————————————————————————————————————————————————————————————————————————————

/// 块级容器标签：一律降级成 `<Column>`（行内内容先合并成 `<Text>` 子元素）。
bool is_block_container(std::string_view tag) {
  static const std::string_view kTags[] = {
      "div",     "section", "article", "aside",  "header",   "footer", "main",
      "nav",     "form",    "fieldset", "figure", "address", "center", "details",
      "summary", "body",    "html",    "dl",     "dt",      "dd",     "figcaption",
      "hgroup",  "dialog",  "span-block", // 占位：不参与匹配，只为对齐可读性
      "label",   "tbody",   "thead",   "tfoot", "caption",  "colgroup",
  };
  return std::find(std::begin(kTags), std::end(kTags), tag) != std::end(kTags);
}

/// 行内标签：**不单独建元素**，文本并入父块的文本流。
bool is_inline(std::string_view tag) {
  static const std::string_view kTags[] = {
      "span",   "b",      "strong", "em",     "i",      "u",       "s",     "strike",
      "small",  "big",    "sub",    "sup",    "mark",   "del",     "ins",   "abbr",
      "cite",   "q",      "dfn",    "time",   "var",    "samp",    "tt",    "font",
      "br",     "wbr",    "nobr",   "bdi",    "bdo",    "ruby",    "rt",    "rp",
  };
  return std::find(std::begin(kTags), std::end(kTags), tag) != std::end(kTags);
}

/// `<pre>` 语义（保留原样空白）。
bool keeps_whitespace(std::string_view tag) {
  return tag == "pre" || tag == "textarea" || tag == "listing" || tag == "xmp";
}

/// 标题级别（`h1`..`h6`；非标题返回 0）。
auto heading_level(std::string_view tag) -> std::uint32_t {
  if (tag.size() != 2 || tag[0] != 'h' || tag[1] < '1' || tag[1] > '6') return 0;
  return static_cast<std::uint32_t>(tag[1] - '0');
}

// ————————————————————————————————————————————————————————————————————————————
// 文本收集：把一个块里的行内内容合并成一段文本
// ————————————————————————————————————————————————————————————————————————————

/// 收集一个子树里的**可读文本**（按文档顺序，行内标签不切分）。
///
/// `keep_space` 为真时保留原有换行与连续空格（`<pre>`）；否则按 HTML 规则折叠。
/// 行内 `<br>` 变成换行（多行 `Text` 用它表达分行的位置）。
void collect_text(const Node& node, bool keep_space, std::string& out,
                  bool skip_links = false) {
  if (node.kind == NodeKind::Text) {
    if (keep_space) {
      out += node.text;
      return;
    }
    // 折叠：连续空白 → 单个空格；但**不 trim**（首尾由调用方在整段上处理，
    // 逐节点 trim 会把 "a <b>b</b> c" 粘成 "ab c"）。
    bool pending = false;
    for (const char ch : node.text) {
      const bool space = ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f';
      if (space) {
        pending = true;
        continue;
      }
      if (pending && !out.empty()) out.push_back(' ');
      pending = false;
      out.push_back(ch);
    }
    // 尾部空白留给下一个节点判断（不在这里落），但要记住"有过空白"——
    // 用一个空格表达：下一个节点若不是空白开头，它就会粘上去（正确）。
    if (pending && !out.empty()) out.push_back(' ');
    return;
  }
  if (node.tag == "script" || node.tag == "style" || node.tag == "template") return;
  // `<a>` 在块级上下文里会落成独立的 `Link` 元素（见 `emit_into`），
  // 文本流里就不能再收它一遍——否则同一段文字输出两遍。
  if (skip_links && node.tag == "a") return;
  if (node.tag == "br") {
    out.push_back('\n');
    return;
  }
  for (const Node& child : node.children) collect_text(child, keep_space, out, skip_links);
}

/// 收集后做整体规整（折叠模式：段落内连续空白折成单空格并 trim；
/// 保留模式：只去掉行尾空白与首尾空行）。
[[nodiscard]] auto tidy_text(std::string text, bool keep_space) -> std::string {
  if (keep_space) {
    // `<pre>` 的**行首缩进是内容的一部分**，不能动；只去尾随空白与首尾空行，
    // 否则 `multiline` 文本会在末尾多出一大块空白。
    std::string out;
    out.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
      const char ch = text[index];
      if (ch == '\r') continue;
      out.push_back(ch);
    }
    // 去掉每行行尾空白。
    std::string lined;
    lined.reserve(out.size());
    std::size_t begin = 0;
    while (begin <= out.size()) {
      const std::size_t end = out.find('\n', begin);
      const std::string_view line =
          end == std::string::npos ? std::string_view(out).substr(begin)
                                   : std::string_view(out).substr(begin, end - begin);
      std::size_t last = line.size();
      while (last > 0 && (line[last - 1] == ' ' || line[last - 1] == '\t')) --last;
      lined.append(line.substr(0, last));
      if (end == std::string::npos) break;
      lined.push_back('\n');
      begin = end + 1;
    }
    // 去首尾空行。
    std::size_t from = 0;
    while (from < lined.size() && lined[from] == '\n') ++from;
    std::size_t to = lined.size();
    while (to > from && lined[to - 1] == '\n') --to;
    return lined.substr(from, to - from);
  }
  // 折叠模式：把内部连续空白（含换行）折成单个空格，再去首尾。
  std::string collapsed;
  collapsed.reserve(text.size());
  bool pending = false;
  for (const char ch : text) {
    const bool space = ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f';
    if (space) {
      pending = true;
      continue;
    }
    if (pending && !collapsed.empty()) collapsed.push_back(' ');
    pending = false;
    collapsed.push_back(ch);
  }
  return std::string(trim_view(collapsed));
}

// ————————————————————————————————————————————————————————————————————————————
// 样式 → XML 属性 / `style` 串
// ————————————————————————————————————————————————————————————————————————————

/// 一条 `style="…"` 声明（键已归一，值已转成霜天认识的写法）。
struct SheetEntry {
  std::string key{};
  std::string value{};
};

/// CSS 颜色 → 霜天色串（`#rrggbb` / `#rrggbbaa`）。
[[nodiscard]] auto color_text(const math::Color& color) -> std::string {
  if (color.a == 255U) return std::format("#{:02x}{:02x}{:02x}", color.r, color.g, color.b);
  return std::format("#{:02x}{:02x}{:02x}{:02x}", color.r, color.g, color.b, color.a);
}

/// 字重数值 → 霜天的四档枚举名（`style` 串的 `weight` 只认这四个）。
[[nodiscard]] auto weight_name(std::uint16_t weight) -> std::string_view {
  if (weight >= 700U) return "bold";
  if (weight >= 600U) return "semibold";
  if (weight >= 500U) return "medium";
  return "regular";
}

/// 把 `st::html::Style` 翻成"XML 属性 + style 串键值"。
///
/// 为什么两路而不是全塞进 `style`：`style` 串支持的是霜天 `Style` 字段（布局/绘制/
/// 玻璃/文字），而**组件自己的属性**（`label`/`level`/`multiline`…）不在其中。
/// 于是"能落到属性面"的走属性，"其余外观"走 `style` 串——两者写的是同一份
/// 最终样式，只是入口不同（框架里那四条入口共用一份键表）。
struct StyleOut {
  /// 直接写成 XML 属性的（`padding`/`gap`/`width`…）。
  std::vector<std::pair<std::string, std::string>> attributes{};
  /// 汇成 `style="…"` 的键值。
  std::vector<SheetEntry> sheet{};
  /// 没落进 XML 的 CSS 声明（进报告）。
  std::vector<std::string> dropped{};
};

/// 一个块在 HTML 里的"盒模型是否值得表达"：`<span>` 这类行内元素不该带 margin。
///
/// `display` 由样式层算好（含 UA 默认值）：`div`/`p`/`h*` 是 Block，`span` 是 Inline。
/// 判据用它而不是看标签名，是因为 `display:block` 写在 `<span>` 上时它**真的**是块级。
[[nodiscard]] auto is_boxed(const Style& style) -> bool {
  return style.display == Display::Block || style.display == Display::InlineBlock ||
         style.display == Display::ListItem || style.display == Display::Table ||
         style.display == Display::TableRow || style.display == Display::TableCell;
}

/// 一个块在 CSS 里"自带多少上下留白"（块间空隙的来源）。
///
/// ## 为什么是 `gap` 而不是 `margin`
///
/// 霜天的 `margin` 是**把子元素盒四边向内缩**（`arrange` 里 `target.inset(margin)`），
/// 不是 CSS 的外侧留白；写上去会挤窄并裁掉内容（见 `docs/BACKLOG.md` P1）。
/// 而块与块之间的空隙在霜天里的正确表达是**父容器的 `gap`**。
///
/// 于是把 CSS 里"相邻两块的 margin 合并"折算成一个 gap：
/// `gap ≈ (上块 margin-bottom + 下块 margin-top) / 2` —— 取半而不是相加，
/// 因为 CSS 的相邻 margin 会**折叠**（取较大者），而两个块各自 `margin: 8`
/// 在浏览器里就是 8px 间隔，不是 16px。用最大值更贴近，但会让
/// "有标题的大间距"与"普通段落"拉不开差距，所以取两者较大值的平均值作为近似。
[[nodiscard]] auto block_spacing(const Style& style) -> float {
  return std::max(style.margin_top, style.margin_bottom);
}

/// 汇总一组子节点的期望块间距（0 = 不加 gap）。
[[nodiscard]] auto children_spacing(const Node& parent, const StyleResolver& resolver) -> float {
  float total = 0.0f;
  std::size_t count = 0;
  for (const Node& child : parent.children) {
    if (child.kind != NodeKind::Element) continue;
    if (is_inline(child.tag) || child.tag == "script" || child.tag == "style") continue;
    total += block_spacing(resolver.style_of(&child));
    ++count;
  }
  if (count <= 1) return 0.0f;
  // 相邻留白折叠：取平均而不是累加。
  return std::round(total / static_cast<float>(count));
}

/// 把一份 `Style` 翻成输出（`tag` 用于判断哪些声明与语义组件重复）。
///
/// `allow_box`：调用方是否希望表达盒模型。行内元素（`<span>`）不表达——它没有
/// 自己的盒子位置，padding/margin 写上去也不会生效（徒增噪音）。
[[nodiscard]] auto translate_style(const Style& style, std::string_view tag,
                                   bool allow_box) -> StyleOut {
  StyleOut out;
  const bool boxed_element = allow_box && is_boxed(style);

  // —— 盒模型：只有块级元素才表达（行内元素的 padding/margin 在合并文本里没有位置）
  if (boxed_element) {
    // padding：四边相同时写 `padding`，否则拆 `padding_x`/`padding_y`。
    //
    // 为何不做"四个方向各一个属性"：XML 属性面只有 `padding`/`padding_x`/`padding_y`
    // （没有 `padding_top`），这是**下游的能力边界**，不是这里偷懒。四边各不相同时
    // 取上下、左右两组近似，并把差异报进 dropped（诚实告知不是逐边精确）。
    const bool uniform_all = std::abs(style.padding_top - style.padding_right) < 0.01f &&
                             std::abs(style.padding_top - style.padding_bottom) < 0.01f &&
                             std::abs(style.padding_top - style.padding_left) < 0.01f;
    if (uniform_all) {
      if (style.padding_top > 0.0f) {
        out.attributes.emplace_back("padding", num(style.padding_top));
      }
    } else {
      const float vertical = 0.5f * (style.padding_top + style.padding_bottom);
      const float horizontal = 0.5f * (style.padding_left + style.padding_right);
      if (vertical > 0.0f) out.attributes.emplace_back("padding_y", num(vertical));
      if (horizontal > 0.0f) out.attributes.emplace_back("padding_x", num(horizontal));
      if (std::abs(style.padding_top - style.padding_bottom) > 0.5f ||
          std::abs(style.padding_left - style.padding_right) > 0.5f) {
        out.dropped.push_back(std::format("padding（上下/左右不等，已取均值 {} / {}）",
                                         num(vertical), num(horizontal)));
      }
    }
    // margin：**不映射**。
    //
    // 为什么（实测）：霜天的 `Element::arrange` 对子元素落位用的是
    // `target.inset(child_style.margin)`——把子元素自己的盒**四边向内缩**，
    // 而不是 CSS 的“外侧留白”。写上去的后果是内容被挤窄、纵向上被裁掉半截
    // （截图里“标题被切掉一半”就是这么来的），而期望的块间空隙根本没有。
    // 详见 `docs/BACKLOG.md` 的 P1「margin 的语义是向内缩」。
    //
    // 替代：块间空隙由**父容器的 `gap`** 表达（见 `emit_into` 的容器生成）；
    // 左右缩进由 `padding_x` 表达（列表项目符号的缩进就是这么来的）。
    if (style.margin_top > 0.0f || style.margin_bottom > 0.0f || style.margin_left > 0.0f ||
        style.margin_right > 0.0f) {
      out.dropped.push_back(std::format(
          "margin（{} / {} / {} / {}）—— 霜天 margin 是内缩而非外距，已改由父级 gap 表达",
          num(style.margin_top), num(style.margin_right), num(style.margin_bottom),
          num(style.margin_left)));
    }
    // 宽度/高度：只在**有确定像素值**时写（CSS `auto`/`100%` 在样式层已解析成
    // 基准宽，那一类会写死尺寸、让窗口缩放失效——所以只认显式像素）。
    if (style.width > 0.0f) out.attributes.emplace_back("width", num(style.width));
    if (style.height > 0.0f) out.attributes.emplace_back("height", num(style.height));
    if (style.max_width > 0.0f) out.attributes.emplace_back("max_width", num(style.max_width));
    if (style.radius > 0.0f) out.attributes.emplace_back("radius", num(style.radius));
  }

  // —— 背景 ——
  if (style.has_background && style.background.a > 0U) {
    out.attributes.emplace_back("background", color_text(style.background));
  }
  // —— 描边 ——
  if (style.border_width > 0.0f) {
    if (style.border_left_only) {
      // 引用块形态：霜天的 `border` 键不支持"只左边"，用 `style` 串也表达不了
      // 单边描边——退化成整个描边会让引用块看成"套了个框"（实测过的观感问题）。
      // 所以只给颜色与宽度，并如实报告"单边退化"。
      out.sheet.push_back({"border_width", num(style.border_width)});
      out.sheet.push_back({"border_color", color_text(style.border_color)});
      out.dropped.push_back("border-left（霜天无明显单边描边；已退化为整圈描边）");
    } else {
      out.sheet.push_back({"border_width", num(style.border_width)});
      out.sheet.push_back({"border_color", color_text(style.border_color)});
    }
  }

  // —— 文字 ——
  //
  // **`color` 走属性面**（`text_color`），**不走 `style` 串**：两者都能落，
  // 但属性面这条会经过 `Element::set_text_color` 记成显式覆盖，
  // 换主题后仍生效；只写 `style.color` 会被组件 `apply_theme` 盖回。
  if (tag != "hr" && style.color.a > 0U) {
    out.attributes.emplace_back("text_color", color_text(style.color));
  }
  // 字号：**必须用 `text_size`**（记成显式覆盖、主题回放末尾重新落位）。
  // `font_size` 名字也认，但它直接写 `Style::font_size`，会被 `Text::apply_theme`
  // 用主题 `font_base` 覆盖——两条通道的差异在 `tools/xml_font_size_probe.cpp` 里量过。
  if (style.font_size > 0.0f && !heading_level(tag)) {
    out.attributes.emplace_back("text_size", num(style.font_size));
  }
  if (style.monospace) out.attributes.emplace_back("monospace", "true");
  if (style.font_weight != 400U) {
    out.attributes.emplace_back("font_weight", std::string(weight_name(style.font_weight)));
  }
  if (style.text_align != TextAlign::Start) {
    // 属性面 `text_align` 收的是**数字**（0 start / 1 center / 2 end）；名字写法
    // 只在 `style` 串里成立。这里写 `style` 串（可读性更好）。
    switch (style.text_align) {
      case TextAlign::Center: out.sheet.push_back({"align", "center"}); break;
      case TextAlign::End: out.sheet.push_back({"align", "end"}); break;
      default: break;
    }
  }
  if (style.italic) {
    // 霜天没有斜体字重通道（`Style` 无 italic 字段）——如实报告，不伪造。
    out.dropped.push_back("font-style: italic（霜天无斜体通道）");
  }
  if (style.underline) out.dropped.push_back("text-decoration: underline（用 Link 表达更准）");
  if (style.strikethrough) out.dropped.push_back("text-decoration: line-through（无对应通道）");
  if (style.letter_spacing != 0.0f) {
    out.dropped.push_back(std::format("letter-spacing: {}（无对应通道）", num(style.letter_spacing)));
  }
  return out;
}

/// 把 `style` 键值拼成 `style="k v, k v"`（空则不写）。
[[nodiscard]] auto sheet_text(const std::vector<SheetEntry>& entries) -> std::string {
  std::string out;
  for (const SheetEntry& entry : entries) {
    if (entry.key.empty() || entry.value.empty()) continue;
    if (!out.empty()) out += ", ";
    out += entry.key;
    out.push_back(' ');
    out += entry.value;
  }
  return out;
}

// ————————————————————————————————————————————————————————————————————————————
// 生成器
// ————————————————————————————————————————————————————————————————————————————

/// 输出用的元素（先建中间树，再统一序列化）。
///
/// 为什么不直接拼字符串：很多决策需要"回头看父/子"（如"块里只有文本 → 不套 Column"、
/// "表格有表头 → 用 Table 而不是 Row 网格"），拼串时改不动已经写出去的东西。
struct OutElement {
  std::string tag{};
  std::vector<std::pair<std::string, std::string>> attributes{};
  std::string text{};
  /// 文本是否用 CDATA 包裹（`<pre>` 专用）。
  ///
  /// 为何需要它：`st::xml` 解析器对**普通文本**做空白折叠（`append_collapsed`）——
  /// 每行缩进与连续空格会被折成单空格，于是 `<pre>` 的排版全丢（实测：`<Text>`
  /// 里读不到 `\n` 与行首缩进）。CDATA 是它唯一保留字面的通道（那一支不折、不解码）。
  bool raw_text{false};
  std::vector<OutElement> children{};
};

/// 页面留白（根容器的 `padding`）。
///
/// 为何需要：HTML 里 `<body>` 靠 `margin: 8px` 拿页边距，而霜天没有"默认页边距"——
/// 不显式给就是 0（内容紧贴窗口边缘，标题左上角顶到 0,0）。
/// 桌面面板比网页需要更大的留白，所以固定 18px（而不是按 `<body>` 的 8px）。
inline constexpr float kRootPagePadding = 18.0f;

/// 数一棵输出树的元素数（含根）。
///
/// 为何用“数树”而不是自增计数器：各 `emit_*` 里已经按根/子/文本分别自增过，
/// 再在 `count_tag` 里自增就是重复计数——实测 45 个元素的样例被报成 1。
/// 两个计数器必然漂移，只留一个真的（输出树），它不可能“忘加”。
[[nodiscard]] auto count_elements(const OutElement& element) -> std::size_t {
  std::size_t total = 1;
  for (const OutElement& child : element.children) total += count_elements(child);
  return total;
}

class Generator {
 public:
  Generator(const StyleSheet& sheet, const ToUiXmlOptions& options)
      : resolver_(sheet, StyleResolver::initial_style()), options_(options) {}

  auto run(const Document& document) -> ToUiXmlResult {
    resolver_.resolve_tree(document.root);
    ToUiXmlResult result;
    result.title = document.title;

    // 根：`<Column grow="true">`——XML 树挂在 WindowFrame 的 content 下，
    // 根不带 grow 会让内容贴顶（docs/xml-ui.md「常见坑」第一条）。
    OutElement root;
    root.tag = "Column";
    if (options_.root_grow) root.attributes.emplace_back("grow", "true");
    if (options_.assign_ids) root.attributes.emplace_back("id", "root");
    // 页面留白：HTML 的 `<body>` 在浏览器里默认靠 `body { margin: 8px }` 拿边距，
    // 而霜天没有"默认页边距"——不显式给，内容会紧贴窗口边缘（实测：标题左上角
    // 顶到 0,0，读起来像半成品）。取 `<body>` 的 margin 与 16px 中较大的那个。
    root.attributes.emplace_back("padding", num(kRootPagePadding));

    for (const Node& child : document.root.children) {
      emit_into(root, child);
    }
    result.report.elements = count_elements(root);

    result.xml = serialize(root, 0);
    result.report.tags = std::move(tag_counts_);
    result.report.images = images_;
    result.report.scripts = scripts_;
    result.report.dropped_declarations = std::move(dropped_);
    result.report.unmapped_tags = std::move(unmapped_);
    result.report.notes = std::move(notes_);
    std::sort(result.report.dropped_declarations.begin(), result.report.dropped_declarations.end());
    result.report.dropped_declarations.erase(
        std::unique(result.report.dropped_declarations.begin(),
                    result.report.dropped_declarations.end()),
        result.report.dropped_declarations.end());
    std::sort(result.report.unmapped_tags.begin(), result.report.unmapped_tags.end());
    result.report.unmapped_tags.erase(
        std::unique(result.report.unmapped_tags.begin(), result.report.unmapped_tags.end()),
        result.report.unmapped_tags.end());
    return result;
  }

 private:
  // —— 主分派 ——
  void emit_into(OutElement& parent, const Node& node) {
    if (node.kind == NodeKind::Text) {
      // 裸文本：由父块的文本收集统一处理（这里不单独建元素）。
      return;
    }
    if (node.tag == "script") {
      ++scripts_;
      return;
    }
    if (node.tag == "style" || node.tag == "head" || node.tag == "meta" || node.tag == "link" ||
        node.tag == "title" || node.tag == "base") {
      return;   // 文档元信息，不进视图
    }
    if (node.tag == "img") {
      emit_image(parent, node);
      return;
    }
    if (is_inline(node.tag)) {
      // 行内标签在**父块的文本收集**里已被消费；单独出现在这里只可能是
      // 父级不是块（如根下散落 `<span>`），此时按文本处理。
      return;
    }
    if (const auto level = heading_level(node.tag); level > 0) {
      emit_heading(parent, node, level);
      return;
    }
    if (node.tag == "p") {
      emit_text_block(parent, node, "p");
      return;
    }
    if (node.tag == "pre") {
      emit_pre(parent, node);
      return;
    }
    if (node.tag == "blockquote") {
      emit_blockquote(parent, node);
      return;
    }
    if (node.tag == "hr") {
      count_tag("hr");
      OutElement divider;
      divider.tag = "Divider";
      divider.attributes.emplace_back("id", next_id());
      // 分隔线的上下留白由父级 `gap` 表达（不能写 `margin`——那是内缩）。
      parent.children.push_back(std::move(divider));
      ++elements_;
      return;
    }
    if (node.tag == "ul" || node.tag == "ol") {
      emit_list(parent, node);
      return;
    }
    if (node.tag == "table") {
      emit_table(parent, node);
      return;
    }
    if (node.tag == "button") {
      emit_button(parent, node);
      return;
    }
    if (node.tag == "input") {
      emit_input(parent, node);
      return;
    }
    if (node.tag == "textarea") {
      emit_textarea(parent, node);
      return;
    }
    if (node.tag == "select") {
      emit_select(parent, node);
      return;
    }
    if (node.tag == "a") {
      emit_link(parent, node);
      return;
    }
    if (is_block_container(node.tag)) {
      emit_container(parent, node);
      return;
    }
    // 认不出的标签：**内容不丢**，标签本身降级成 `<Column>`（与 `st::html`
    // 「认不出的标签照建节点」同取向）。
    unmapped_.push_back(node.tag);
    emit_container(parent, node);
  }

  // —— 各类元素 ——

  /// 通用块容器：`<Column>`，子节点递归。
  void emit_container(OutElement& parent, const Node& node) {
    const Style& style = resolver_.style_of(&node);
    OutElement element;
    element.tag = "Column";
    const std::string id = element_id(node);
    if (!id.empty()) element.attributes.emplace_back("id", id);
    apply_style(element, style, node.tag, /*allow_box=*/true);
    // 块间距：HTML 的 margin 在霜天里由**父容器的 gap** 表达（见 `block_spacing`）。
    if (const float spacing = children_spacing(node, resolver_); spacing > 0.0f) {
      element.attributes.emplace_back("gap", num(spacing));
    }

    // 有子元素 → 递归；没有子元素但有文本 → 一个 `Text` 子元素。
    bool has_element_child = false;
    for (const Node& child : node.children) {
      if (child.kind == NodeKind::Element && !is_inline(child.tag) && child.tag != "br") {
        has_element_child = true;
        break;
      }
    }
    if (has_element_child) {
      // 块里既有元素又有**裸文本**时，裸文本按顺序变成一个 `Text`
      // （否则 `<div>前言<p>x</p>后记</div>` 会丢掉前后两段）。
      std::string run;
      const bool keep = keeps_whitespace(node.tag);
      for (const Node& child : node.children) {
        const bool is_textish = child.kind == NodeKind::Text ||
                                (child.kind == NodeKind::Element && is_inline(child.tag));
        if (is_textish) {
          if (child.kind == NodeKind::Element && child.tag == "script") continue;
          collect_text(child, keep, run);
          continue;
        }
        flush_text(element, run, keep, style);
        emit_into(element, child);
      }
      flush_text(element, run, keep, style);
    } else {
      std::string text;
      collect_text(node, keeps_whitespace(node.tag), text);
      const std::string tidied = tidy_text(std::move(text), keeps_whitespace(node.tag));
      if (!tidied.empty()) {
        element.children.push_back(make_text_element(tidied, style, node.tag));
        ++elements_;
      }
    }
    count_tag(node.tag);
    parent.children.push_back(std::move(element));
    ++elements_;
  }

  void emit_text_block(OutElement& parent, const Node& node, std::string_view html_tag) {
    const Style& style = resolver_.style_of(&node);
    OutElement element;
    element.tag = "Column";
    const std::string id = element_id(node);
    if (!id.empty()) element.attributes.emplace_back("id", id);
    apply_style(element, style, html_tag, /*allow_box=*/true);

    // 文本收集**跳过 `<a>`**：它要落成独立的 `Link` 元素（下面递归时生成），
    // 若在这里一并收进 `<Text>`，同一段文字就会输出两遍（实测：`<p><a>报告</a></p>`
    // 出来是"报告"加一个"报告"链接）。
    std::string text;
    collect_text(node, false, text, /*skip_links=*/true);
    const std::string tidied = tidy_text(std::move(text), false);
    if (!tidied.empty()) {
      element.children.push_back(make_text_element(tidied, style, html_tag));
      ++elements_;
    }
    // `<p>` 里若有 `<a>` 或块级子元素（HTML 少见但合法），递归进去别丢内容。
    // 判据里**必须包含 `a`**：它被 `skip_links` 从文本流里排除了，若这里也不管它，
    // 整条链接就彻底消失（内容丢失比重复更糟）。
    for (const Node& child : node.children) {
      if (child.kind == NodeKind::Element && (!is_inline(child.tag) || child.tag == "a") &&
          child.tag != "br" &&
          child.tag != "script" && child.tag != "style") {
        emit_into(element, child);
      }
    }
    count_tag(html_tag);
    parent.children.push_back(std::move(element));
    ++elements_;
  }

  void emit_heading(OutElement& parent, const Node& node, std::uint32_t level) {
    const Style& style = resolver_.style_of(&node);
    OutElement element;
    element.tag = "Heading";
    const std::string id = element_id(node);
    if (!id.empty()) element.attributes.emplace_back("id", id);
    element.attributes.emplace_back("level", std::format("{}", level));
    // 标题的**字号由 `level` 决定**（组件的 `apply_theme` 按 level 查 token），
    // 所以这里不写 `text_size`——写了会被 `Heading::apply_theme` 的档位值盖回，
    // 制造"生成了却没生效"的假象。CSS 若显式改了字号，进报告。
    std::string text;
    collect_text(node, false, text);
    const std::string tidied = tidy_text(std::move(text), false);
    element.text = tidied;
    if (style.text_align == TextAlign::Center) element.attributes.emplace_back("text_align", "1");
    if (style.text_align == TextAlign::End) element.attributes.emplace_back("text_align", "2");
    if (style.color.a > 0U) element.attributes.emplace_back("text_color", color_text(style.color));
    const StyleResolvedHeading expected = heading_default_size(level);
    if (style.font_size > 0.0f && std::abs(style.font_size - expected.size) > 0.6f) {
      dropped_.push_back(std::format("h{} 的 font-size: {}（组件按 level 取 {}，字号以 level 为准）",
                                     level, num(style.font_size), num(expected.size)));
    }
    count_tag(node.tag);
    parent.children.push_back(std::move(element));
    ++elements_;
  }

  void emit_pre(OutElement& parent, const Node& node) {
    const Style& style = resolver_.style_of(&node);
    OutElement element;
    element.tag = "Column";
    const std::string id = element_id(node);
    if (!id.empty()) element.attributes.emplace_back("id", id);
    apply_style(element, style, "pre", /*allow_box=*/true);

    std::string text;
    collect_text(node, true, text);
    const std::string tidied = tidy_text(std::move(text), true);
    if (!tidied.empty()) {
      OutElement text_element = make_text_element(tidied, style, "pre");
      text_element.raw_text = true;   // CDATA：XML 解析器对普通文本折叠空白
      element.children.push_back(std::move(text_element));
      ++elements_;
    }
    count_tag("pre");
    parent.children.push_back(std::move(element));
    ++elements_;
  }

  void emit_blockquote(OutElement& parent, const Node& node) {
    const Style& style = resolver_.style_of(&node);
    OutElement element;
    element.tag = "Column";
    const std::string id = element_id(node);
    if (!id.empty()) element.attributes.emplace_back("id", id);
    apply_style(element, style, "blockquote", /*allow_box=*/true);

    bool has_block_child = false;
    for (const Node& child : node.children) {
      if (child.kind == NodeKind::Element && !is_inline(child.tag) && child.tag != "br") {
        has_block_child = true;
        break;
      }
    }
    if (has_block_child) {
      for (const Node& child : node.children) emit_into(element, child);
    } else {
      std::string text;
      collect_text(node, false, text);
      const std::string tidied = tidy_text(std::move(text), false);
      if (!tidied.empty()) {
        element.children.push_back(make_text_element(tidied, style, "blockquote"));
        ++elements_;
      }
    }
    count_tag("blockquote");
    parent.children.push_back(std::move(element));
    ++elements_;
  }

  void emit_list(OutElement& parent, const Node& node) {
    const Style& style = resolver_.style_of(&node);
    OutElement element;
    element.tag = "Column";
    const std::string id = element_id(node);
    if (!id.empty()) element.attributes.emplace_back("id", id);
    apply_style(element, style, node.tag, /*allow_box=*/true);

    const bool ordered = node.tag == "ol";
    std::size_t index = 0;
    for (const Node& child : node.children) {
      if (child.kind != NodeKind::Element || child.tag != "li") {
        // `<ul>` 里理论只有 `<li>`；其它内容别丢。
        if (child.kind == NodeKind::Element) emit_into(element, child);
        continue;
      }
      ++index;
      const Style& item_style = resolver_.style_of(&child);
      OutElement row;
      row.tag = "Row";
      row.attributes.emplace_back("id", next_id());
      row.attributes.emplace_back("gap", "6");
      // 项目符号：文本前缀。为什么不找组件自带的列表：霜天没有 `Ul`/`Li`
      // 组件（组件表里只有 `List`，它是"可选中行列表"语义，形状不同）。
      OutElement bullet;
      bullet.tag = "Text";
      bullet.attributes.emplace_back("id", next_id());
      bullet.text = ordered ? std::format("{}.", index) : "•";
      if (item_style.color.a > 0U) {
        bullet.attributes.emplace_back("text_color", color_text(item_style.color));
      }
      row.children.push_back(std::move(bullet));
      ++elements_;

      // 条目内容：文本合并；含块级子元素时递归。
      bool has_block_child = false;
      for (const Node& item_child : child.children) {
        if (item_child.kind == NodeKind::Element && !is_inline(item_child.tag) &&
            item_child.tag != "br") {
          has_block_child = true;
          break;
        }
      }
      OutElement content;
      content.tag = "Column";
      content.attributes.emplace_back("id", next_id());
      content.attributes.emplace_back("grow", "true");
      if (has_block_child) {
        for (const Node& item_child : child.children) emit_into(content, item_child);
      } else {
        std::string text;
        collect_text(child, false, text);
        const std::string tidied = tidy_text(std::move(text), false);
        if (!tidied.empty()) {
          content.children.push_back(make_text_element(tidied, item_style, "li"));
          ++elements_;
        }
      }
      row.children.push_back(std::move(content));
      ++elements_;
      element.children.push_back(std::move(row));
      ++elements_;
    }
    count_tag(node.tag);
    parent.children.push_back(std::move(element));
    ++elements_;
  }

  void emit_table(OutElement& parent, const Node& node) {
    // `<table>`：有 `<thead>`/首行 `<th>` → 真 `<Table>`（带表头与列宽）；
    // 否则 → 每行一个 `<Row>` 的网格（`Table` 需要列定义，无表头时反而更差）。
    const Node* header_row = nullptr;
    for (const Node& child : node.children) {
      if (child.kind != NodeKind::Element) continue;
      if (child.tag == "thead") {
        for (const Node& row : child.children) {
          if (row.kind == NodeKind::Element && row.tag == "tr") {
            header_row = &row;
            break;
          }
        }
        break;
      }
      if (child.tag == "tr") {
        for (const Node& cell : child.children) {
          if (cell.kind == NodeKind::Element && cell.tag == "th") {
            header_row = &child;
            break;
          }
        }
        break;
      }
    }
    const Style& style = resolver_.style_of(&node);
    OutElement element;
    element.tag = "Column";
    const std::string id = element_id(node);
    if (!id.empty()) element.attributes.emplace_back("id", id);
    apply_style(element, style, "table", /*allow_box=*/true);

    if (header_row != nullptr) {
      OutElement table;
      table.tag = "Table";
      table.attributes.emplace_back("id", next_id());
      std::string columns;
      for (const Node& cell : header_row->children) {
        if (cell.kind != NodeKind::Element || (cell.tag != "th" && cell.tag != "td")) continue;
        std::string text;
        collect_text(cell, false, text);
        const std::string name = tidy_text(std::move(text), false);
        if (name.empty()) continue;
        if (!columns.empty()) columns.push_back(',');
        columns += name;
      }
      if (!columns.empty()) table.attributes.emplace_back("columns", columns);
      elements_ += 2;   // Column + Table
      dropped_.push_back("table 的单元格内容（Table 组件的行数据需由逻辑层用 `rows` 灌入）");
      element.children.push_back(std::move(table));
    } else {
      // 网格形态：每行一个 `<Row>`，每格一个 `<Text>`。
      for (const Node& row : node.children) {
        if (row.kind != NodeKind::Element || row.tag != "tr") continue;
        OutElement out_row;
        out_row.tag = "Row";
        out_row.attributes.emplace_back("id", next_id());
        out_row.attributes.emplace_back("gap", "12");
        for (const Node& cell : row.children) {
          if (cell.kind != NodeKind::Element || (cell.tag != "td" && cell.tag != "th")) continue;
          OutElement out_cell;
          out_cell.tag = "Column";
          out_cell.attributes.emplace_back("id", next_id());
          out_cell.attributes.emplace_back("grow", "true");
          std::string text;
          collect_text(cell, false, text);
          const std::string tidied = tidy_text(std::move(text), false);
          if (!tidied.empty()) {
            out_cell.children.push_back(make_text_element(tidied, resolver_.style_of(&cell), "td"));
            ++elements_;
          }
          out_row.children.push_back(std::move(out_cell));
          ++elements_;
        }
        element.children.push_back(std::move(out_row));
        ++elements_;
      }
    }
    count_tag("table");
    parent.children.push_back(std::move(element));
    ++elements_;
  }

  void emit_button(OutElement& parent, const Node& node) {
    std::string text;
    collect_text(node, false, text);
    const std::string label = tidy_text(std::move(text), false);
    OutElement button;
    button.tag = "Button";
    const std::string id = element_id(node);
    if (!id.empty()) button.attributes.emplace_back("id", id);
    if (!label.empty()) button.attributes.emplace_back("label", label);
    // 按钮语义：主按钮用 primary 变体（HTML 里 `<button class="primary">` 这类
    // 意图太依赖命名习惯，只认显式的 `type="submit"`——它确实表示"主要动作"）。
    if (const auto type = node.attr("type"); type.has_value() && *type == "submit") {
      button.attributes.emplace_back("variant", "primary");
    }
    count_tag("button");
    parent.children.push_back(std::move(button));
    ++elements_;
  }

  void emit_input(OutElement& parent, const Node& node) {
    const auto type = node.attr("type");
    const std::string type_text = type.has_value() ? std::string(*type) : std::string("text");
    if (type_text == "checkbox" || type_text == "radio") {
      OutElement toggle;
      toggle.tag = type_text == "checkbox" ? "Checkbox" : "Radio";
      const std::string id = element_id(node);
      if (!id.empty()) toggle.attributes.emplace_back("id", id);
      if (const auto label = node.attr("value"); label.has_value()) {
        toggle.attributes.emplace_back("label", std::string(*label));
      }
      if (node.attr("checked").has_value()) toggle.attributes.emplace_back("checked", "true");
      count_tag("input");
      parent.children.push_back(std::move(toggle));
      ++elements_;
      return;
    }
    if (type_text == "range") {
      OutElement slider;
      slider.tag = "Slider";
      const std::string id = element_id(node);
      if (!id.empty()) slider.attributes.emplace_back("id", id);
      count_tag("input");
      parent.children.push_back(std::move(slider));
      ++elements_;
      return;
    }
    if (type_text == "submit" || type_text == "button" || type_text == "reset") {
      OutElement button;
      button.tag = "Button";
      const std::string id = element_id(node);
      if (!id.empty()) button.attributes.emplace_back("id", id);
      if (const auto value = node.attr("value"); value.has_value()) {
        button.attributes.emplace_back("label", std::string(*value));
      }
      if (type_text == "submit") button.attributes.emplace_back("variant", "primary");
      count_tag("input");
      parent.children.push_back(std::move(button));
      ++elements_;
      return;
    }
    OutElement input;
    input.tag = "Input";
    const std::string id = element_id(node);
    if (!id.empty()) input.attributes.emplace_back("id", id);
    if (const auto placeholder = node.attr("placeholder"); placeholder.has_value()) {
      input.attributes.emplace_back("placeholder", std::string(*placeholder));
    }
    if (const auto value = node.attr("value"); value.has_value()) {
      input.attributes.emplace_back("value", std::string(*value));
    }
    if (type_text == "password") input.attributes.emplace_back("password", "true");
    if (type_text != "text" && type_text != "password" && type_text != "email" &&
        type_text != "search" && type_text != "url" && type_text != "tel" &&
        type_text != "number" && type_text != "date" && type_text != "hidden") {
      dropped_.push_back(std::format("input[type={}]（已退化为普通输入框）", type_text));
    }
    // 宽度：输入框按列宽自适应（`grow`）比写死宽度更合理，且有 `size` 时按字符宽估。
    count_tag("input");
    parent.children.push_back(std::move(input));
    ++elements_;
  }

  void emit_textarea(OutElement& parent, const Node& node) {
    OutElement area;
    area.tag = "TextArea";
    const std::string id = element_id(node);
    if (!id.empty()) area.attributes.emplace_back("id", id);
    if (const auto placeholder = node.attr("placeholder"); placeholder.has_value()) {
      area.attributes.emplace_back("placeholder", std::string(*placeholder));
    }
    std::string text;
    collect_text(node, true, text);
    const std::string tidied = tidy_text(std::move(text), true);
    if (!tidied.empty()) area.attributes.emplace_back("value", tidied);
    if (const auto rows = node.attr("rows"); rows.has_value()) {
      area.attributes.emplace_back("height", num(std::strtof(std::string(*rows).c_str(), nullptr) * 20.0f));
    }
    count_tag("textarea");
    parent.children.push_back(std::move(area));
    ++elements_;
  }

  void emit_select(OutElement& parent, const Node& node) {
    OutElement select;
    select.tag = "Select";
    const std::string id = element_id(node);
    if (!id.empty()) select.attributes.emplace_back("id", id);
    std::string options;
    std::string selected;
    for (const Node& child : node.children) {
      if (child.kind != NodeKind::Element || child.tag != "option") continue;
      std::string text;
      collect_text(child, false, text);
      const std::string label = tidy_text(std::move(text), false);
      if (label.empty()) continue;
      if (!options.empty()) options.push_back('|');
      options += label;
      if (child.attr("selected").has_value()) selected = label;
    }
    if (!options.empty()) select.attributes.emplace_back("options", options);
    if (!selected.empty()) select.attributes.emplace_back("value", selected);
    count_tag("select");
    parent.children.push_back(std::move(select));
    ++elements_;
  }

  void emit_link(OutElement& parent, const Node& node) {
    std::string text;
    collect_text(node, false, text);
    const std::string label = tidy_text(std::move(text), false);
    OutElement link;
    link.tag = "Link";
    const std::string id = element_id(node);
    if (!id.empty()) link.attributes.emplace_back("id", id);
    if (!label.empty()) link.attributes.emplace_back("text", label);
    if (const auto href = node.attr("href"); href.has_value()) {
      link.attributes.emplace_back("url", std::string(*href));
      if (href->rfind("http://", 0) == 0 || href->rfind("https://", 0) == 0) {
        link.attributes.emplace_back("external", "true");
      }
    }
    count_tag("a");
    parent.children.push_back(std::move(link));
    ++elements_;
  }

  void emit_image(OutElement& parent, const Node& node) {
    ++images_;
    // 霜天没有图片组件（`st::ui` 无 Image 类型）——落成 `Avatar` 占位
    // （首字/名称给的视觉位）。像素内容不在 XML 里，如实进报告。
    OutElement avatar;
    avatar.tag = "Avatar";
    const std::string id = element_id(node);
    if (!id.empty()) avatar.attributes.emplace_back("id", id);
    std::string name;
    if (const auto alt = node.attr("alt"); alt.has_value()) name = std::string(*alt);
    if (name.empty()) {
      if (const auto src = node.attr("src"); src.has_value()) name = std::string(*src);
    }
    if (!name.empty()) avatar.attributes.emplace_back("name", name);
    const Style& style = resolver_.style_of(&node);
    if (style.width > 0.0f) avatar.attributes.emplace_back("size", num(style.width));
    count_tag("img");
    dropped_.push_back("img 的像素内容（霜天无图片组件，已落成 Avatar 占位）");
    parent.children.push_back(std::move(avatar));
    ++elements_;
  }

  // —— 辅助 ——

  /// 把一个块里的裸文本 run 落成一个 `<Text>`（空则不落）。
  void flush_text(OutElement& parent, std::string& run, bool keep_space, const Style& style) {
    const std::string tidied = tidy_text(run, keep_space);
    run.clear();
    if (tidied.empty()) return;
    parent.children.push_back(make_text_element(tidied, style, ""));
    ++elements_;
  }

  /// 造一个多行 `Text`。
  ///
  /// `grow="true"` 必须给：多行 `Text` 的测量宽是**最长那一行**的宽度，
  /// 放进列容器里就只占那么宽——于是"段落"看起来像缩在左边的一小截，
  /// 而它的折行宽度也被限制在自身最长行上（越窄越窄）。给 `grow` 后它吃掉
  /// 父容器的可用宽，才是网页里段落的样子（实测：不给时整页内容只占 540/900）。
  [[nodiscard]] auto make_text_element(const std::string& content, const Style& style,
                                       std::string_view html_tag) -> OutElement {
    OutElement text_element;
    text_element.tag = "Text";
    text_element.attributes.emplace_back("id", next_id());
    text_element.attributes.emplace_back("multiline", "true");
    text_element.attributes.emplace_back("grow", "true");
    text_element.text = content;
    copy_text_attrs(text_element, style, html_tag);
    return text_element;
  }

  /// 把文字相关属性（颜色/字号/字重/等宽）复制到 `Text` 元素上。
  ///
  /// 为什么要复制而不是让 `Text` 继承父容器：霜天**没有 CSS 那样的样式继承**——
  /// 父容器的 `text_color` 不会下沉到子 `Text`（组件的 `apply_theme` 各自从主题取值）。
  /// 所以每个 `Text` 必须自带样式，否则生出来的界面会全用主题默认色/字号。
  void copy_text_attrs(OutElement& element, const Style& style, std::string_view html_tag) {
    if (style.color.a > 0U) element.attributes.emplace_back("text_color", color_text(style.color));
    if (style.font_size > 0.0f && heading_level(html_tag) == 0) {
      element.attributes.emplace_back("text_size", num(style.font_size));
    }
    if (style.font_weight != 400U) {
      element.attributes.emplace_back("font_weight", std::string(weight_name(style.font_weight)));
    }
    if (style.monospace) element.attributes.emplace_back("monospace", "true");
    if (style.text_align == TextAlign::Center) element.attributes.emplace_back("text_align", "1");
    if (style.text_align == TextAlign::End) element.attributes.emplace_back("text_align", "2");
  }

  /// 把 `Style` 的翻译结果落进元素（属性 + `style` 串）。
  void apply_style(OutElement& element, const Style& style, std::string_view tag, bool allow_box) {
    StyleOut out = translate_style(style, tag, allow_box);
    for (auto& attribute : out.attributes) {
      element.attributes.push_back(std::move(attribute));
    }
    if (!out.sheet.empty()) {
      const std::string text = sheet_text(out.sheet);
      if (!text.empty()) element.attributes.emplace_back("style", text);
    }
    for (auto& dropped : out.dropped) dropped_.push_back(std::move(dropped));
  }

  [[nodiscard]] auto element_id(const Node& node) -> std::string {
    if (const auto id = node.attr("id"); id.has_value() && !id->empty()) {
      return std::string(*id);
    }
    return options_.assign_ids ? next_id() : std::string{};
  }

  [[nodiscard]] auto next_id() -> std::string { return std::format("n{}", ++id_counter_); }

  /// 记一次标签命中（**只统计标签表**，不碰元素总数）。
  ///
  /// 元素总数在序列化前**直接数输出树**（`count_elements`）——为什么不用自增：
  /// 各 `emit_*` 里已经按“根/子/文本”分别自增过，再加 `count_tag` 的自增就是
  /// 重复计数（实测：45 个元素的样例报成 1）。两个计数器必然漂移，只留一个真的。
  void count_tag(std::string_view tag) {
    for (auto& entry : tag_counts_) {
      if (entry.first == tag) {
        ++entry.second;
        return;
      }
    }
    tag_counts_.emplace_back(std::string(tag), 1);
  }

  /// `h1`..`h6` 按主题 token 的字号（用于判断"CSS 是否显式改过字号"）。
  ///
  /// 数值取**亮色主题的缺省令牌**（`Metrics::font_3xl` 等）：这里只用来判断
  /// "差得远不远"，精度要求低；真值在主题里，组件 `apply_theme` 会取自己的。
  struct StyleResolvedHeading {
    float size{14.0f};
  };
  [[nodiscard]] static auto heading_default_size(std::uint32_t level) -> StyleResolvedHeading {
    switch (level) {
      case 1: return {30.0f};
      case 2: return {24.0f};
      case 3: return {20.0f};
      case 4: return {17.0f};
      case 5: return {15.0f};
      default: return {14.0f};
    }
  }

  /// 序列化成 XML 文本。
  [[nodiscard]] auto serialize(const OutElement& element, int depth) const -> std::string {
    std::string pad;
    if (!options_.indent.empty()) {
      for (int index = 0; index < depth; ++index) pad += options_.indent;
    }
    std::string out = pad;
    out += "<";
    out += element.tag;
    for (const auto& [name, value] : element.attributes) {
      out += " ";
      out += name;
      out += "=\"";
      out += escape_xml(value);
      out += "\"";
    }
    const bool has_text = !element.text.empty();
    if (!has_text && element.children.empty()) {
      out += "/>";
      return out;
    }
    out += ">";
    if (has_text) {
      if (element.raw_text) {
        // CDATA：内部**不转义、不折叠**。
        //
        // 内容里若出现 `]]>` 会提前闭合，所以按 XML 的惯例拆成 `]]]]><![CDATA[>`
        // （源里一个 `]]>` 变成两个相邻 CDATA 段，拼接后内容不变）。
        out += "<![CDATA[";
        std::string_view body = element.text;
        for (;;) {
          const std::size_t hit = body.find("]]>");
          if (hit == std::string_view::npos) {
            out += body;
            break;
          }
          out += body.substr(0, hit + 2);   // 含 `]]`
          out += "]]><![CDATA[";
          body = body.substr(hit + 2);
        }
        out += "]]>";
      } else {
        out += escape_xml(element.text);
      }
      if (element.children.empty()) {
        out += "</";
        out += element.tag;
        out += ">";
        return out;
      }
    }
    out += "\n";
    for (const OutElement& child : element.children) {
      out += serialize(child, depth + 1);
      out += "\n";
    }
    out += pad;
    out += "</";
    out += element.tag;
    out += ">";
    return out;
  }

  StyleResolver resolver_;
  ToUiXmlOptions options_{};
  std::vector<std::pair<std::string, std::size_t>> tag_counts_{};
  std::vector<std::string> dropped_{};
  std::vector<std::string> unmapped_{};
  std::vector<std::string> notes_{};
  std::size_t elements_{0};
  std::size_t images_{0};
  std::size_t scripts_{0};
  std::size_t id_counter_{0};
};

}  // namespace

auto to_ui_xml(const Document& document, const ToUiXmlOptions& options) -> ToUiXmlResult {
  const StyleSheet sheet = StyleSheet::parse(document.style_text);
  Generator generator(sheet, options);
  ToUiXmlResult result = generator.run(document);
  return result;
}

auto to_ui_xml(std::string_view html, const ToUiXmlOptions& options) -> ToUiXmlResult {
  const Document document = parse(html);
  return to_ui_xml(document, options);
}

}  // namespace st::html

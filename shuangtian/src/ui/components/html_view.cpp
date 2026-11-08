#include "st/ui/components/html_view.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "st/core/string.hpp"
#include "st/html/dom.hpp"
#include "st/math/path.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/ui/line_layout.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"

#include "components_internal.hpp"

namespace st::ui {

using components_internal::fill_round_rect;
using components_internal::paint_outline;
using components_internal::text_port_of;

namespace {

/// 粗体阈值（端口没有字重通道，`>600` 走真粗体面）。
constexpr float kBoldThreshold = 600.0f;

/// 一段文字的测量宽（含字间距）。
[[nodiscard]] auto text_width(const TextPort& port, std::string_view text, float size,
                              float letter_spacing) -> float {
  if (text.empty()) return 0.0f;
  const float base = port.measure_width(text, size);
  return base + letter_spacing * static_cast<float>(text.size() - 1U);
}

/// 断行位置候选（一个字符的字节长度）。
[[nodiscard]] auto utf8_step(std::string_view text, std::size_t index) noexcept -> std::size_t {
  if (index >= text.size()) return 0;
  const unsigned char lead = static_cast<unsigned char>(text[index]);
  std::size_t step = 1;
  if ((lead & 0xE0U) == 0xC0U) step = 2;
  else if ((lead & 0xF0U) == 0xE0U) step = 3;
  else if ((lead & 0xF8U) == 0xF0U) step = 4;
  return std::min(step, text.size() - index);
}

}  // namespace

HtmlView::HtmlView() = default;

HtmlView::HtmlView(std::string html) { set_html(std::move(html)); }

// ————————————————————————————————————————————————————————————————————————————
// 内容
// ————————————————————————————————————————————————————————————————————————————

void HtmlView::set_html(std::string html) {
  source_ = std::move(html);
  document_ = html::parse(source_);
  style_sheet_ = html::StyleSheet::parse(document_.style_text);
  resolver_ =
      std::make_unique<html::StyleResolver>(style_sheet_, html::StyleResolver::initial_style());
  resolver_->resolve_tree(document_.root);
  // 文档底色：取 `<body>`（没有就 `<html>`）的背景色；都没有则白。
  // 理由见 `document_background_` 的说明：网页文字按白底设计。
  document_background_ = math::Color{255, 255, 255, 255};
  for (const html::Node& child : document_.root.children) {
    if (child.kind != html::NodeKind::Element) continue;
    if (child.tag != "body" && child.tag != "html") continue;
    const html::Style& body_style = resolver_->style_of(&child);
    if (body_style.has_background && body_style.background.a != 0U) {
      document_background_ = body_style.background;
      break;
    }
  }
  plain_.clear();
  for (const html::Node& child : document_.root.children) collect_text(child, plain_);
  plain_ = std::string(st::trim(plain_));
  scroll_ = 0.0f;
  hover_row_ = -1;
  layout_valid_ = false;
  rows_.clear();
  mark_layout_dirty();
}

void HtmlView::collect_text(const html::Node& node, std::string& out) {
  if (node.kind == html::NodeKind::Text) {
    if (!out.empty() && out.back() != ' ' && out.back() != '\n') out.push_back(' ');
    out += node.text;
    return;
  }
  // 不收集不可见元素的内容（脚本/样式不进语义文本）。
  if (node.tag == "script" || node.tag == "style" || node.tag == "head") return;
  for (const html::Node& child : node.children) collect_text(child, out);
  if (!out.empty() && out.back() != '\n') out.push_back('\n');
}

void HtmlView::set_base_font_size(float size) {
  if (base_font_size_ == size) return;
  base_font_size_ = size;
  layout_valid_ = false;
  mark_layout_dirty();
}

void HtmlView::set_padding(float padding) {
  if (padding_ == padding) return;
  padding_ = std::max(padding, 0.0f);
  layout_valid_ = false;
  mark_layout_dirty();
}

void HtmlView::set_follow_theme(bool value) {
  if (follow_theme_ == value) return;
  follow_theme_ = value;
  layout_valid_ = false;
  mark_dirty();
}

void HtmlView::set_max_content_width(float width) {
  if (max_content_width_ == width) return;
  max_content_width_ = width;
  layout_valid_ = false;
  mark_layout_dirty();
}

void HtmlView::set_scroll(float offset) {
  const float limit = std::max(content_height_ + padding_ * 2.0f - measured_.height, 0.0f);
  const float clamped = std::clamp(offset, 0.0f, limit);
  if (clamped == scroll_) return;
  scroll_ = clamped;
  mark_dirty();
}

// ————————————————————————————————————————————————————————————————————————————
// 主题与度量
// ————————————————————————————————————————————————————————————————————————————

void HtmlView::apply_theme(const Theme& theme) {
  style_.background = theme.colors().surface;
  style_.border_color = theme.colors().border;
  style_.border_width = 0.0f;
  style_.radius = theme.metrics().radius_md;
  style_.color = theme.colors().text;
  style_.font_size = theme.metrics().font_base;
  style_.clip_children = true;
  if (base_font_size_ <= 0.0f) base_font_size_ = theme.metrics().font_base;
}

void HtmlView::measure(const RenderContext& context, const Constraints& constraints) {
  float width = style_.has_explicit_width()
                    ? style_.width
                    : (constraints.max_width < kUnbounded ? constraints.max_width : 640.0f);
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
  width = std::clamp(width, style_.min_width, style_.max_width);

  const float usable = std::max(width - padding_ * 2.0f, 40.0f);
  if (!layout_valid_ || laid_out_width_ != usable) {
    rebuild(context, usable);
    laid_out_width_ = usable;
    layout_valid_ = true;
  }

  const float natural_height = content_height_ + padding_ * 2.0f;
  float height = style_.has_explicit_height() ? style_.height : natural_height;
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  height = std::clamp(height, style_.min_height, style_.max_height);
  measured_ = math::Size{width, height};
}

// ————————————————————————————————————————————————————————————————————————————
// 排版：行内流
// ————————————————————————————————————————————————————————————————————————————

/// ## 行内流为什么用“片段序列 + 硬换行标记”
///
/// 行内混排 `<p>a <b>b</b> c</p>` 要求三段落在**同一行**。首版用 `lines[] + styles[]`
/// 一一对应，结果每个片段各自成了一行——属性面全对、只有截图能看出排版崩了。
/// 现在用行内流：各片段拼成一串（携各自样式与起始字节偏移），折行与
/// `<br>` / `<pre>` 的硬换行均在 `finish_inline_flow()` 里统一处理。
void HtmlView::finish_inline_flow(const RenderContext& context, InlineFlow& flow, float left,
                                 float& cursor_y, float width, BlockLayout& block) {
  const TextPort& port = text_port_of(context);
  if (flow.runs.empty()) return;

  // ① 拼接全文 + 记录每个 run 的起始偏移；同时标出硬换行位置（`"\n"` 专用 run）。
  std::string whole;
  std::vector<std::size_t> run_start;
  run_start.reserve(flow.runs.size());
  std::vector<bool> run_is_break(flow.runs.size(), false);
  for (std::size_t index = 0; index < flow.runs.size(); ++index) {
    run_start.push_back(whole.size());
    const Run& run = flow.runs[index];
    const bool is_break = run.text == "\n";
    run_is_break[index] = is_break;
    whole += run.text;
  }

  // 该字节位置所属的 run 下标（用于按 run 的字号量宽）。
  //
  // ⚠ 用 `upper_bound` 而不是逐项扫描：run 数量多时（一页几十个行内元素）
  // 逐项扫描是 O(n²)；而 run 起始偏移本身有序，二分既准又快。
  const auto run_at = [&run_start](std::size_t offset) -> std::size_t {
    const auto found = std::upper_bound(run_start.begin(), run_start.end(), offset);
    if (found == run_start.begin()) return 0;
    return static_cast<std::size_t>(found - run_start.begin()) - 1U;
  };

  // ② 逐硬换行分段，段内贪心折行。
  std::vector<std::pair<std::size_t, std::size_t>> lines;   // [begin, end)，末尾的 `\n` 不入行
  std::size_t segment_begin = 0;
  std::size_t scan = 0;
  const auto flush_segment = [&](std::size_t segment_end) {
    // 硬换行之前的内容按宽折行。
    if (segment_begin >= segment_end) {
      lines.emplace_back(segment_begin, segment_end);
      return;
    }
    std::size_t begin = segment_begin;
    while (begin < segment_end) {
      std::size_t index = begin;
      std::size_t last_space = std::string_view::npos;
      float used = 0.0f;
      while (index < segment_end) {
        const std::size_t step = utf8_step(whole, index);
        const std::size_t owner = run_at(index);
        const InlineStyle& run_style = flow.runs[owner].style;
        // 宽度按**本 run 内从行首到本字**的实测量算，而不是逐字累加：
        // 逐字累加在真实字体下有亚像素舍入误差（每个字形宽各自舍入），
        // 累计后会把最后 1~2px 误判成“超宽”——实测表现为“`.class` 最后的
        // 字母被邻段盖住/挤到下一行”。这里按「行首到 index 的实测宽」与
        // 「行首到 index+step 的实测宽」相减，与绘制时同口径（同一个串、同一个字号）。
        const float width_here =
            port.measure_width(std::string_view(whole).substr(begin, index - begin),
                               run_style.font_size) +
            run_style.letter_spacing * static_cast<float>(index - begin);
        (void)width_here;
        const float width_next =
            port.measure_width(std::string_view(whole).substr(begin, index + step - begin),
                               run_style.font_size) +
            run_style.letter_spacing * static_cast<float>(index + step - begin);
        if (width_next > width && index > begin) break;
        used = width_next;
        if (whole[index] == ' ') last_space = index + 1U;
        index += std::min(step, segment_end - index);
      }
      (void)used;
      if (index >= segment_end) {
        lines.emplace_back(begin, segment_end);
        break;
      }
      std::size_t cut = index;
      if (last_space != std::string_view::npos && last_space > begin) cut = last_space;
      lines.emplace_back(begin, cut);
      begin = cut;
      while (begin < segment_end && whole[begin] == ' ') ++begin;
    }
  };
  while (scan < whole.size()) {
    if (whole[scan] == '\n' && run_is_break[run_at(scan)]) {
      flush_segment(scan);
      segment_begin = scan + 1U;
    }
    ++scan;
  }
  flush_segment(whole.size());
  if (lines.empty()) lines.emplace_back(0U, 0U);

  // ③ 每行产出若干个 `Row`（一个 run 一个行段），行高取该行最大行盒。
  for (const auto& [line_begin, line_end] : lines) {
    float line_height = 0.0f;
    for (std::size_t r = 0; r < flow.runs.size(); ++r) {
      if (run_is_break[r]) continue;
      const std::size_t start = run_start[r];
      const std::size_t end = r + 1U < run_start.size() ? run_start[r + 1U] : whole.size();
      if (end <= line_begin || start >= line_end) continue;
      line_height = std::max(line_height,
                             flow.runs[r].style.font_size * flow.runs[r].style.line_height);
    }
    if (line_height <= 0.0f) line_height = style_.font_size * 1.45f;

    if (line_begin >= line_end) {
      // 空行（硬换行或空段落）：仍占一个行盒高度。
      Row row{};
      row.box = math::Rect{left, cursor_y, width, line_height};
      rows_.push_back(std::move(row));
      if (!block.has_rows) {
        block.first_row = rows_.size() - 1U;
        block.has_rows = true;
      }
      cursor_y += line_height;
      continue;
    }

    float x = left;
    for (std::size_t r = 0; r < flow.runs.size(); ++r) {
      if (run_is_break[r]) continue;
      const std::size_t start = run_start[r];
      const std::size_t end = r + 1U < run_start.size() ? run_start[r + 1U] : whole.size();
      const std::size_t segment_from = std::max(line_begin, start);
      const std::size_t segment_to = std::min(line_end, end);
      if (segment_to <= segment_from) continue;
      std::string piece = whole.substr(segment_from, segment_to - segment_from);
      // 行首空格不占位（对齐到左缘），但**保留**行内空格。
      if (x <= left + 0.01f) {
        while (!piece.empty() && piece.front() == ' ') piece.erase(0, 1);
      }
      if (piece.empty()) continue;
      const InlineStyle& run_style = flow.runs[r].style;
      Row row{};
      const float piece_width =
          text_width(port, piece, run_style.font_size, run_style.letter_spacing);
      row.box = math::Rect{x, cursor_y, piece_width, line_height};
      row.ink = row.box;
      // 行内元素自带 padding：文字向右踢、底色向外扩（两边都要）。
      // 不扩的话 `<code>a</code><code>b</code>` 两块的底色会连成一整条
      // （看起来像一个代码块而不是两个）。
      if (run_style.inline_padding > 0.0f) {
        row.ink.x -= run_style.inline_padding;
        row.ink.width += run_style.inline_padding * 2.0f;
        row.text_x += run_style.inline_padding;
      }
      row.text = std::move(piece);
      // 行内元素自己的背景/边框（`<code>` 的灰底）：用 UA 样式里那一份。
      if (run_style.has_inline_background) {
        row.has_background = true;
        row.background = run_style.inline_background;
      }
      row.font_size = run_style.font_size;
      row.color = follow_theme_ ? style_.color : run_style.color;
      row.font_weight = run_style.font_weight;
      row.italic = run_style.italic;
      row.monospace = run_style.monospace;
      row.underline = run_style.underline;
      row.strikethrough = run_style.strikethrough;
      row.line_height = run_style.line_height;
      row.letter_spacing = run_style.letter_spacing;
      row.link = run_style.link;
      row.text_align = html::TextAlign::Start;
      rows_.push_back(std::move(row));
      if (!block.has_rows) {
        block.first_row = rows_.size() - 1U;
        block.has_rows = true;
      }
      x += piece_width + run_style.inline_padding * 2.0f;
    }
    cursor_y += line_height;
  }
  flow.runs.clear();
  flow.length = 0;
}

void HtmlView::flush_lines(const RenderContext& context, std::vector<std::string>& lines,
                           std::vector<InlineStyle>& styles, float left, float& cursor_y,
                           float width, BlockLayout& block) {
  // 显式行列表（`<pre>` 用）：每行一个硬换行段，交给行内流统一处理。
  if (lines.empty() || styles.size() < lines.size()) return;
  InlineFlow flow{};
  for (std::size_t index = 0; index < lines.size(); ++index) {
    Run run{};
    run.text = std::move(lines[index]);
    run.style = styles[index];
    flow.length += run.text.size();
    flow.runs.push_back(std::move(run));
    if (index + 1U < lines.size()) {
      Run newline{};
      newline.style = styles[index];
      newline.text = "\n";
      flow.runs.push_back(std::move(newline));
    }
  }
  lines.clear();
  styles.clear();
  finish_inline_flow(context, flow, left, cursor_y, width, block);
}

// ————————————————————————————————————————————————————————————————————————————
// 排版：节点
// ————————————————————————————————————————————————————————————————————————————

void HtmlView::layout_node(const RenderContext& context, const html::Node& node, float left,
                           float top, float width, InlineStyle inline_style, float& cursor_y,
                           InlineFlow& flow, BlockLayout* block) {
  (void)top;
  const html::Style& style = resolver_->style_of(&node);

  if (node.kind == html::NodeKind::Text) {
    if (style.display == html::Display::None) return;
    if (node.text.empty()) return;
    Run run{};
    run.text = node.text;
    run.style = inline_style;
    flow.length += run.text.size();
    flow.runs.push_back(std::move(run));
    return;
  }

  if (style.display == html::Display::None) return;

  // —— 行内元素：并入当前行内流 ——
  const bool is_inline = style.display == html::Display::Inline ||
                         style.display == html::Display::InlineBlock;
  if (is_inline) {
    InlineStyle next = inline_style;
    next.font_size = style.font_size > 0.0f ? style.font_size : inline_style.font_size;
    next.color = style.color;
    next.font_weight = style.font_weight;
    next.italic = style.italic;
    next.monospace = style.monospace;
    next.underline = style.underline || inline_style.underline;
    next.strikethrough = style.strikethrough || inline_style.strikethrough;
    next.letter_spacing = style.letter_spacing;
    next.line_height = style.line_height;
    // 行内元素自己的背景（`<code>` 灰底）：**不继承**——只在元素自己声明了背景时
    // 生效，否则清掉（否则外层块的浅底会漏到内层每一个行内片段上）。
    next.has_inline_background = style.has_background && style.background.a != 0U;
    next.inline_background = style.background;
    next.inline_padding = style.padding_left;
    if (const auto href = node.attr("href"); href.has_value() && !href->empty()) {
      next.link = std::string(*href);
    }
    if (node.tag == "br") {
      // 硬换行：插入一个换行标记 run（`finish_inline_flow` 会按 `\n` 切段）。
      Run newline{};
      newline.style = next;
      newline.text = "\n";
      flow.runs.push_back(std::move(newline));
      flow.length += 1U;
      return;
    }
    for (const html::Node& child : node.children) {
      layout_node(context, child, left, top, width, next, cursor_y, flow, block);
    }
    return;
  }

  // —— 块级：先把行内流冲刷掉 ——
  if (block != nullptr) finish_inline_flow(context, flow, left, cursor_y, width, *block);

  cursor_y += style.margin_top;
  const float outer_left = left + style.margin_left;
  const float content_width =
      std::max(width - style.margin_left - style.margin_right - style.padding_left -
                   style.padding_right,
               20.0f);
  const float content_left = outer_left + style.padding_left;
  cursor_y += style.padding_top;

  BlockLayout inner{};
  inner.width = width - style.margin_left - style.margin_right;
  inner.background = style.background;
  inner.has_background = style.has_background;
  inner.border_width = style.border_width;
  inner.border_color = style.border_color;
  inner.border_left_only = style.border_left_only;
  inner.radius = style.radius;
  inner.first_row = rows_.size();
  inner.has_rows = false;

  InlineStyle child_inline = inline_style;
  child_inline.font_size = style.font_size > 0.0f ? style.font_size : inline_style.font_size;
  child_inline.color = style.color;
  child_inline.font_weight = style.font_weight;
  child_inline.italic = style.italic;
  child_inline.monospace = style.monospace;
  child_inline.underline = style.underline;
  child_inline.strikethrough = style.strikethrough;
  child_inline.letter_spacing = style.letter_spacing;
  child_inline.line_height = style.line_height;
  child_inline.link = inline_style.link;

  // 块级元素自己的行内内容先攒进它自己的流。
  InlineFlow block_flow{};

  // `<hr>`：一条分隔线
  if (node.tag == "hr") {
    Row row{};
    row.box = math::Rect{content_left, cursor_y + 6.0f, content_width, std::max(style.height, 1.0f)};
    row.has_background = true;
    row.background = style.has_background ? style.background : math::Color{221, 221, 221, 255};
    rows_.push_back(std::move(row));
    cursor_y += std::max(style.height, 1.0f) + 6.0f;
  } else if (node.tag == "table") {
    // 表格：逐行排版，单元格横向分栏（等分列宽）。
    for (const html::Node& section : node.children) {
      if (section.kind != html::NodeKind::Element) continue;
      const auto layout_row = [&](const html::Node& tr) {
        std::vector<const html::Node*> cells;
        for (const html::Node& cell : tr.children) {
          if (cell.kind == html::NodeKind::Element && (cell.tag == "td" || cell.tag == "th")) {
            cells.push_back(&cell);
          }
        }
        if (cells.empty()) return;
        const float cell_width = content_width / static_cast<float>(cells.size());
        float row_top = cursor_y;
        float row_bottom = cursor_y + 4.0f;
        for (std::size_t index = 0; index < cells.size(); ++index) {
          const html::Node& cell = *cells[index];
          const html::Style& cell_style = resolver_->style_of(&cell);
          InlineStyle cell_inline = inline_style;
          cell_inline.font_size = cell_style.font_size;
          cell_inline.color = cell_style.color;
          cell_inline.font_weight = cell_style.font_weight;
          cell_inline.italic = cell_style.italic;
          cell_inline.monospace = cell_style.monospace;
          cell_inline.line_height = cell_style.line_height;
          BlockLayout cell_block{};
          cell_block.first_row = rows_.size();
          cell_block.background = cell_style.background;
          cell_block.has_background = cell_style.has_background;
          cell_block.border_width = cell_style.border_width;
          cell_block.border_color = cell_style.border_color;
          cell_block.border_left_only = cell_style.border_left_only;
          const float cell_x = content_left + cell_width * static_cast<float>(index);
          float cell_y = row_top + cell_style.padding_top;
          InlineFlow cell_flow{};
          for (const html::Node& content : cell.children) {
            layout_node(context, content, cell_x, cell_y, cell_width, cell_inline, cell_y,
                        cell_flow, &cell_block);
          }
          finish_inline_flow(context, cell_flow, cell_x, cell_y, cell_width, cell_block);
          row_bottom = std::max(row_bottom, cell_y + cell_style.padding_bottom);
          // 单元格：行盒铺满单元格宽（不是文字宽），背景/边框才能拼成一整块。
          for (std::size_t row = cell_block.first_row; row < rows_.size(); ++row) {
            Row& cell_row = rows_[row];
            cell_row.background = cell_block.background;
            cell_row.has_background = cell_block.has_background;
            cell_row.border_width = std::min(cell_block.border_width,
                                             std::max(cell_row.box.height * 0.5f, 0.5f));
            cell_row.border_color = cell_block.border_color;
            cell_row.border_left_only = cell_block.border_left_only;
            cell_row.text_x += 0.0f;
            // 先记住文字相对偏移，再把盒子扩到整格。
            const float text_offset = cell_row.box.x - cell_x;
            cell_row.box.x = cell_x;
            cell_row.box.width = cell_width;
            cell_row.text_x = text_offset;
          }
        }
        cursor_y = std::max(row_bottom, row_top + 18.0f);
      };
      if (section.tag == "tr") {
        layout_row(section);
      } else if (section.tag == "thead" || section.tag == "tbody") {
        for (const html::Node& tr : section.children) {
          if (tr.kind == html::NodeKind::Element && tr.tag == "tr") layout_row(tr);
        }
      }
    }
  } else if (node.tag == "ol" || node.tag == "ul") {
    // 列表：每个 `<li>` 一块；有序列表前置序号。
    int item_index = 0;
    const bool ordered = style.list_style == html::ListStyle::Decimal;
    for (const html::Node& child : node.children) {
      if (child.kind != html::NodeKind::Element) continue;
      if (child.tag != "li") {
        layout_node(context, child, content_left, cursor_y, content_width, child_inline, cursor_y,
                    block_flow, &inner);
        continue;
      }
      if (ordered) ++item_index;
      const html::Style& item_style = resolver_->style_of(&child);
      const float item_left = content_left;
      const float item_width =
          std::max(content_width - item_style.margin_left - item_style.margin_right, 20.0f);
      cursor_y += item_style.margin_top;
      InlineFlow item_flow{};
      BlockLayout item_block{};
      item_block.first_row = rows_.size();
      const float item_top = cursor_y;
      InlineStyle item_inline = child_inline;
      item_inline.font_size = item_style.font_size;
      item_inline.color = item_style.color;
      item_inline.font_weight = item_style.font_weight;
      item_inline.line_height = item_style.line_height;
      for (const html::Node& content : child.children) {
        // ⚠ **不能传 `cursor_y`**：行内内容不推游标，只往 `item_flow` 里追加。
        // 首版把 `cursor_y` 传进去，而 `<code>` 的 UA 样式带了 padding/margin，
        // 于是每遇到一个行内元素游标就跳一次——表现为“下一段文字盖在上一个字上”。
        // 用一个临时变量接：行内流不看它（真块级会把值写进去，那种情况由外层重算）。
        float inline_cursor = item_top;
        layout_node(context, content, item_left + item_style.margin_left, item_top, item_width,
                    item_inline, inline_cursor, item_flow, &item_block);
      }
      finish_inline_flow(context, item_flow, item_left + item_style.margin_left, cursor_y,
                         item_width, item_block);
      // 项目符号 / 序号：贴在该项首行左侧。
      if (item_block.has_rows && item_block.first_row < rows_.size()) {
        Row& first = rows_[item_block.first_row];
        std::string bullet;
        switch (style.list_style) {
          case html::ListStyle::Disc: bullet = "•"; break;
          case html::ListStyle::Circle: bullet = "◦"; break;
          case html::ListStyle::Square: bullet = "▪"; break;
          case html::ListStyle::Decimal: bullet = std::format("{}.", item_index); break;
          case html::ListStyle::None: break;
        }
        if (!bullet.empty()) {
          first.bullet = bullet;
          // 给符号腾出位置（行盒左移一点，文字右移）。
          first.text_x += 18.0f;
          first.box.x = std::max(first.box.x - 18.0f, 0.0f);
          first.box.width += 18.0f;
        }
      }
      cursor_y += item_style.margin_bottom;
    }
  } else {
    // 普通块：递归子节点。
    for (const html::Node& child : node.children) {
      // 同上：行内内容只追加进 `block_flow`，不推游标。
      float inline_cursor = cursor_y;
      layout_node(context, child, content_left, cursor_y, content_width, child_inline,
                  inline_cursor, block_flow, &inner);
    }
    finish_inline_flow(context, block_flow, content_left, cursor_y, content_width, inner);
  }

  // 块装饰：把该块的行合并成矩形来画背景/边框（横向铺满块的可用宽）。
  if (inner.has_rows && inner.first_row < rows_.size()) {
    const float block_left = outer_left;
    const float block_width = inner.width;
    for (std::size_t row = inner.first_row; row < rows_.size(); ++row) {
      if (inner.has_background) {
        rows_[row].background = inner.background;
        rows_[row].has_background = true;
      }
      if (inner.border_width > 0.0f) {
        rows_[row].border_width = inner.border_width;
        rows_[row].border_color = inner.border_color;
        rows_[row].border_left_only = inner.border_left_only;
        rows_[row].radius = inner.radius;
      }
      if (inner.has_background || inner.border_width > 0.0f) {
        // 行盒横向铺满块宽，文字按内边距内缩（**先记下文字相对偏移再扩盒子**：
        // 否则文字会左移一个 padding 位置——实测踩到）。
        const float text_offset = rows_[row].box.x - block_left;
        rows_[row].box.x = block_left;
        rows_[row].box.width = block_width;
        rows_[row].text_x = text_offset + style.padding_left;
      }
    }
  }

  cursor_y += style.padding_bottom;
  cursor_y += style.margin_bottom;
  (void)block;
}

void HtmlView::rebuild(const RenderContext& context, float available_width) {
  rows_.clear();
  content_height_ = 0.0f;
  content_width_ = available_width;

  float width = available_width;
  if (max_content_width_ > 0.0f) width = std::min(width, max_content_width_);

  html::Style root_style = html::StyleResolver::initial_style();
  if (base_font_size_ > 0.0f) root_style.font_size = base_font_size_;

  InlineStyle inline_style{};
  inline_style.font_size = root_style.font_size;
  inline_style.color = follow_theme_ ? style_.color : root_style.color;
  inline_style.font_weight = root_style.font_weight;
  inline_style.line_height = root_style.line_height;

  float cursor_y = 0.0f;
  InlineFlow flow{};
  BlockLayout block{};
  for (const html::Node& child : document_.root.children) {
    layout_node(context, child, 0.0f, 0.0f, width, inline_style, cursor_y, flow, &block);
  }
  finish_inline_flow(context, flow, 0.0f, cursor_y, width, block);

  content_height_ = cursor_y;
}

// ————————————————————————————————————————————————————————————————————————————
// 绘制
// ————————————————————————————————————————————————————————————————————————————

void HtmlView::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  const TextPort& port = text_port_of(context);
  const Palette& theme_colors = context.theme.colors();

  // ⚠ **必须自己裁剪**：`style_.clip_children` 只包裹**子元素**的绘制
  //（`Element::paint_impl` 里 `paint_content` 在裁剪之前），而滚动内容会画到
  // 组件之外——实测：往上滚时正文直接压在卡片标题上。组件自绘滚动内容时必须
  // 自己 push/pop 裁剪（`ScrollView`/`Terminal` 同样如此）。
  canvas.push_clip_rounded_rect(bounds_, style_.radius);

  // 文档底色：**先铺一层**（网页文字是按白底设计的；直接画在暗色 `surface` 上
  // 会变成一片几乎看不见的深灰）。`follow_theme` 为真时不铺（改用主题色）。
  if (!follow_theme_) {
    canvas.fill_rect(bounds_, raster::Paint::solid(document_background_), style_.radius);
  }

  // 内容横向居中（限宽阅读时）。
  const float offset_x =
      bounds_.x + padding_ +
      std::max((bounds_.width - padding_ * 2.0f - content_width_) * 0.5f, 0.0f);
  const float offset_y = bounds_.y + padding_ - scroll_;

  // —— 第 1 趟：铺底色与边框（含项目符号）、装饰线 ——
  for (std::size_t index = 0; index < rows_.size(); ++index) {
    const Row& row = rows_[index];
    const math::Rect box{offset_x + row.box.x, offset_y + row.box.y, row.box.width, row.box.height};
    if (box.bottom() < bounds_.y - 4.0f || box.y > bounds_.bottom() + 4.0f) continue;
    const math::Rect ink{offset_x + row.ink.x, offset_y + row.ink.y, row.ink.width,
                         row.ink.height};
    if (row.has_background && row.background.a != 0U) {
      fill_round_rect(canvas, row.ink.width > 0.0f ? ink : box, row.radius, row.background);
    }
    if (row.border_width > 0.0f && row.border_color.a != 0U) {
      const math::Rect surface_rect = row.ink.width > 0.0f ? ink : box;
      if (row.border_left_only) {
        // 只画左缘一道竖线（引用块/提示条）。
        canvas.fill_rect(math::Rect{surface_rect.x, surface_rect.y, row.border_width,
                                    surface_rect.height},
                         raster::Paint::solid(row.border_color));
      } else {
        paint_outline(canvas, surface_rect, row.radius, row.border_color, row.border_width);
      }
    }
    if (!row.bullet.empty()) {
      const float bullet_x = box.x + row.text_x - 16.0f;
      port.draw(canvas, row.bullet, math::Point{bullet_x, box.y}, row.font_size, row.color);
    }
    // 装饰线（下划线/删除线）也属于“非文字层”，放第 1 趟：同行的背景不会盖它们。
    if ((row.underline || row.strikethrough) && !row.text.empty()) {
      const float text_x = box.x + row.text_x;
      const auto geometry = layout_line(port, kInkReferenceSample, row.font_size, box.height);
      const float line_width = port.measure_width(row.text, row.font_size);
      if (row.underline) {
        canvas.fill_rect(math::Rect{text_x,
                                    box.y + geometry.glyph_top + geometry.glyph_height,
                                    line_width, 1.0f},
                         raster::Paint::solid(row.color));
      }
      if (row.strikethrough) {
        canvas.fill_rect(math::Rect{text_x,
                                    box.y + geometry.glyph_top + geometry.glyph_height * 0.58f,
                                    line_width, 1.0f},
                         raster::Paint::solid(row.color));
      }
    }
    // 链接悬停：底部一道主色提示线。
    if (!row.link.empty() && static_cast<int>(index) == hover_row_ && !row.text.empty()) {
      const float text_x = box.x + row.text_x;
      const float line_width = port.measure_width(row.text, row.font_size);
      canvas.fill_rect(math::Rect{text_x, box.y + box.height - 1.0f, line_width, 1.0f},
                       raster::Paint::solid(theme_colors.primary));
    }
  }

  // —— 第 2 趟：统一画文字 ——
  //
  // 为何要分两趟：同一行上相邻行内元素的**底色**会盖住前一个片段的末尾字——
  // 实测：`<code>.class</code><code>#id</code>` 中，`#id` 的灰底把 `.class` 的
  // `e` 盖掉一半（看起来像“被裁了”）。浏览器不会这样，因为行内背景先铺完整行、
  // 再统一画文字。这是一条**绘制顺序**约束，不能靠调几何解决。
  for (std::size_t index = 0; index < rows_.size(); ++index) {
    const Row& row = rows_[index];
    if (row.text.empty()) continue;
    const math::Rect box{offset_x + row.box.x, offset_y + row.box.y, row.box.width, row.box.height};
    if (box.bottom() < bounds_.y - 4.0f || box.y > bounds_.bottom() + 4.0f) continue;
    const float draw_x = box.x + row.text_x;
    const float line_top = centered_line_top(port, row.text, row.font_size, box.y, box.height);
    const bool bold = static_cast<float>(row.font_weight) >= kBoldThreshold;
    port.draw(canvas, row.text, math::Point{draw_x, line_top}, row.font_size, row.color,
              row.monospace ? text::FontRole::Monospace : text::FontRole::Proportional, 0.0f,
              bold);
  }
  canvas.pop_clip();
}

// ————————————————————————————————————————————————————————————————————————————
// 交互
// ————————————————————————————————————————————————————————————————————————————

auto HtmlView::link_at(math::Point point) const -> std::string {
  const float offset_x =
      bounds_.x + padding_ +
      std::max((bounds_.width - padding_ * 2.0f - content_width_) * 0.5f, 0.0f);
  const float offset_y = bounds_.y + padding_ - scroll_;
  for (const Row& row : rows_) {
    if (row.link.empty()) continue;
    const math::Rect box{offset_x + row.box.x, offset_y + row.box.y, row.box.width, row.box.height};
    if (box.contains(point)) return row.link;
  }
  return {};
}

auto HtmlView::on_event(const RenderContext& context, Event& event) -> bool {
  switch (event.kind) {
    case EventKind::HoverIn:
    case EventKind::HoverOut:
      hover_row_ = -1;
      mark_dirty();
      return false;
    case EventKind::MouseMove: {
      const float offset_x =
          bounds_.x + padding_ +
          std::max((bounds_.width - padding_ * 2.0f - content_width_) * 0.5f, 0.0f);
      const float offset_y = bounds_.y + padding_ - scroll_;
      int found = -1;
      for (std::size_t index = 0; index < rows_.size(); ++index) {
        if (rows_[index].link.empty()) continue;
        const Row& row = rows_[index];
        const math::Rect box{offset_x + row.box.x, offset_y + row.box.y, row.box.width,
                             row.box.height};
        if (box.contains(event.position)) {
          found = static_cast<int>(index);
          break;
        }
      }
      if (found != hover_row_) {
        hover_row_ = found;
        mark_dirty();
      }
      return false;
    }
    case EventKind::Click:
    case EventKind::DoubleClick:
    case EventKind::TripleClick: {
      const std::string link = link_at(event.position);
      if (link.empty()) return false;
      if (on_link_click) on_link_click(link);
      event.handled = true;
      return true;
    }
    case EventKind::Wheel: {
      const float step = context.theme.metrics().wheel_step;
      const float limit = std::max(content_height_ + padding_ * 2.0f - bounds_.height, 0.0f);
      if (limit <= 0.0f) return false;
      const float delta = event.wheel_delta < 0.0f ? step : -step;
      set_scroll(scroll_ + delta);
      return true;
    }
    case EventKind::KeyDown: {
      const float limit = std::max(content_height_ + padding_ * 2.0f - bounds_.height, 0.0f);
      if (limit <= 0.0f) return false;
      const std::string& key = event.key;
      if (key == "ArrowDown") {
        set_scroll(scroll_ + 24.0f);
        return true;
      }
      if (key == "ArrowUp") {
        set_scroll(scroll_ - 24.0f);
        return true;
      }
      if (key == "PageDown") {
        set_scroll(scroll_ + bounds_.height * 0.9f);
        return true;
      }
      if (key == "PageUp") {
        set_scroll(scroll_ - bounds_.height * 0.9f);
        return true;
      }
      if (key == "Home") {
        set_scroll(0.0f);
        return true;
      }
      if (key == "End") {
        set_scroll(limit);
        return true;
      }
      return false;
    }
    default: return false;
  }
}

auto HtmlView::hit_test(math::Point point) const noexcept -> bool { return bounds_.contains(point); }

auto HtmlView::semantics_value() const -> std::string {
  return std::format("html {} rows", rows_.size());
}

auto HtmlView::row_geometry() const -> std::vector<RowSnapshot> {
  std::vector<RowSnapshot> out;
  out.reserve(rows_.size());
  for (const Row& row : rows_) {
    out.push_back(RowSnapshot{.box = row.box, .ink = row.ink, .text = row.text});
  }
  return out;
}

auto HtmlView::semantics_flags() const -> SemanticsFlags {
  SemanticsFlags flags = Element::semantics_flags();
  flags.scrollable = content_height_ + padding_ * 2.0f > bounds_.height;
  flags.focused = focused_;
  return flags;
}

// ————————————————————————————————————————————————————————————————————————————
// 属性面
// ————————————————————————————————————————————————————————————————————————————

auto HtmlView::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "html" || name == "text" || name == "source") return source_;
  if (name == "title") return document_.title;
  if (name == "plain" || name == "content") return plain_;
  if (name == "scroll") return std::format("{}", scroll_);
  if (name == "padding") return std::format("{}", padding_);
  if (name == "font_size" || name == "base_font_size") return std::format("{}", base_font_size_);
  if (name == "max_width" || name == "max_content_width") {
    return std::format("{}", max_content_width_);
  }
  if (name == "follow_theme") return std::string(follow_theme_ ? kFlagTrue : "false");
  if (name == "rules") return std::to_string(style_sheet_.rules.size());
  if (name == "lines" || name == "rows") return std::to_string(rows_.size());
  if (name == "height") return std::format("{}", content_height_ + padding_ * 2.0f);
  return std::nullopt;
}

auto HtmlView::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "html" || name == "text" || name == "source") {
    set_html(std::string(value));
    return true;
  }
  if (name == "scroll") {
    const auto parsed = st::parse_f64(st::trim(value));
    if (!parsed.has_value()) return false;
    set_scroll(static_cast<float>(*parsed));
    return true;
  }
  if (name == "padding") {
    const auto parsed = st::parse_f64(st::trim(value));
    if (!parsed.has_value()) return false;
    set_padding(static_cast<float>(*parsed));
    return true;
  }
  if (name == "font_size" || name == "base_font_size") {
    const auto parsed = st::parse_f64(st::trim(value));
    if (!parsed.has_value()) return false;
    set_base_font_size(static_cast<float>(*parsed));
    return true;
  }
  if (name == "max_width" || name == "max_content_width") {
    const auto parsed = st::parse_f64(st::trim(value));
    if (!parsed.has_value()) return false;
    set_max_content_width(static_cast<float>(*parsed));
    return true;
  }
  if (name == "follow_theme") {
    const auto parsed = st::parse_bool(value);
    if (!parsed.has_value()) return false;
    set_follow_theme(*parsed);
    return true;
  }
  return false;
}

auto HtmlView::property_names() const -> std::vector<std::string_view> {
  return {"html", "padding", "font_size", "max_content_width", "follow_theme", "scroll"};
}

auto HtmlView::readonly_property_names() const -> std::vector<std::string_view> {
  return {"title", "plain", "rules", "lines", "height"};
}

auto HtmlView::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "scroll_to" || action == "set_scroll") {
    const auto parsed = st::parse_f64(st::trim(argument));
    if (!parsed.has_value()) return false;
    set_scroll(static_cast<float>(*parsed));
    return true;
  }
  if (action == "scroll_top") {
    set_scroll(0.0f);
    return true;
  }
  if (action == "scroll_bottom") {
    set_scroll(std::max(content_height_ + padding_ * 2.0f, 0.0f));
    return true;
  }
  if (action == "reload" || action == "refresh") {
    set_html(source_);
    return true;
  }
  return Element::invoke_action(action, argument);
}

}  // namespace st::ui

// ── 组件登记（`st/ui/component_registry.hpp`）────────────────────────
ST_UI_COMPONENT_IMPL(::st::ui::HtmlView);

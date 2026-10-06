#include "st/ui/components/markdown_view.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <utility>

#include "st/core/string.hpp"
#include "st/md/highlight.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"
#include "st/ui/text_port.hpp"

#include "components_internal.hpp"

namespace st::ui {

using components_internal::text_port_of;

namespace {

// —— 版式常量（尺寸/间距；颜色一律来自主题 token，不在此出现）——

constexpr float kBodyLineHeight{1.6f};        ///< 正文学行高倍数（DESIGN §5：Markdown 段落 1.6）
constexpr float kHeadingLineHeight{1.3f};
constexpr float kCodeLineHeight{1.55f};
constexpr float kListIndentStep{20.0f};       ///< 列表每级缩进（px）
constexpr float kListMarkerColumn{18.0f};     ///< 无序列表符号列宽
constexpr float kListItemGap{4.0f};           ///< 列表项之间的间距
constexpr float kBulletSize{4.0f};            ///< 无序项圆点直径
constexpr float kTaskBoxSize{14.0f};          ///< 任务项方框边长
constexpr float kInlineCodePad{4.0f};         ///< 行内码内边距
constexpr float kInlineCodeRadius{4.0f};      ///< 行内码圆角
constexpr float kCodePad{12.0f};              ///< 代码块内边距
constexpr float kLanguageLabelSize{11.0f};
constexpr float kLanguageLabelHeight{16.0f};
constexpr float kQuoteBarWidth{3.0f};         ///< 引用左侧竖条宽
constexpr float kQuotePadX{12.0f};
constexpr float kQuotePadY{10.0f};
constexpr float kTableRowHeight{28.0f};
constexpr float kTableCellPad{8.0f};
constexpr float kTableMinColumn{48.0f};
constexpr float kDividerThickness{1.0f};
constexpr float kHeadingBottomGap{8.0f};
constexpr float kHeadingRuleGap{3.0f};
constexpr float kBlockGap{12.0f};
constexpr float kNestedGap{6.0f};
constexpr float kDefaultViewWidth{640.0f};
constexpr float kMinViewWidth{80.0f};
constexpr float kMinTextWidth{48.0f};
constexpr float kWheelStep{48.0f};
constexpr float kPageRatio{0.9f};
constexpr float kBoldOffset{0.7f};            ///< 加粗近似：二次绘制的亚像素偏移
constexpr float kSemiBoldOffset{0.4f};
constexpr std::uint32_t kMaxNestingDepth{32U};

/// 标题上间距（H1..H6；`DESIGN.md` §5 的 4px 栅格）。
constexpr std::array<float, 6> kHeadingMargins{24.0f, 20.0f, 16.0f, 14.0f, 12.0f, 10.0f};


// —— 形状填充辅助 ——
// 背景/圆点在光栅器里必须走**它接受的绕向**：`Path` 的 `add_rect` / `add_rounded_rect` /
// `add_circle` 工厂与 `Canvas::fill_rect` 的非对齐回退路径生成的绕向相反，填充会被静默丢弃；
// 因此本组件自建正向（屏幕逆时针）路径，不依赖工厂绕向（对齐的矩形仍走 `fill_rect` 快速路径）。
// 该行为已回报光栅层；即使日后光栅器改为双绕向填充，自建正向路径也照常有效。

constexpr double kPi{3.14159265358979323846};
constexpr double kTwoPi{2.0 * kPi};

/// 矩形是否已落在整像素边界上（对齐时可走 `Canvas::fill_rect` 快速路径）。
[[nodiscard]] auto is_pixel_aligned(math::Rect rect) -> bool {
  return rect.x == std::floor(rect.x) && rect.y == std::floor(rect.y) &&
         rect.right() == std::floor(rect.right()) && rect.bottom() == std::floor(rect.bottom());
}

/// 正向绕向的圆角矩形路径（`radius <= 0` 退化为直角矩形）。
[[nodiscard]] auto rounded_rect_path(math::Rect rect, float radius) -> raster::Path {
  const float limit = std::min(rect.width, rect.height) * 0.5f;
  const float r = radius > limit ? limit : (radius > 0.0f ? radius : 0.0f);
  raster::Path path;
  if (r <= 0.0f) {
    path.move_to(math::Point{rect.x, rect.y});
    path.line_to(math::Point{rect.x, rect.bottom()});
    path.line_to(math::Point{rect.right(), rect.bottom()});
    path.line_to(math::Point{rect.right(), rect.y});
    path.close();
    return path;
  }
  path.move_to(math::Point{rect.x, rect.y + r});
  path.line_to(math::Point{rect.x, rect.bottom() - r});
  path.quad_to(math::Point{rect.x, rect.bottom()}, math::Point{rect.x + r, rect.bottom()});
  path.line_to(math::Point{rect.right() - r, rect.bottom()});
  path.quad_to(math::Point{rect.right(), rect.bottom()},
               math::Point{rect.right(), rect.bottom() - r});
  path.line_to(math::Point{rect.right(), rect.y + r});
  path.quad_to(math::Point{rect.right(), rect.y}, math::Point{rect.right() - r, rect.y});
  path.line_to(math::Point{rect.x + r, rect.y});
  path.quad_to(math::Point{rect.x, rect.y}, math::Point{rect.x, rect.y + r});
  path.close();
  return path;
}

/// 填充矩形（可圆角）：像素对齐且无圆角时走快速路径，否则走自建正向路径。
void fill_rect_shape(raster::Surface& canvas, math::Rect rect, math::Color color,
                     float radius = 0.0f) {
  if (rect.is_empty() || color.a == 0U) return;
  if (radius <= 0.0f && is_pixel_aligned(rect)) {
    canvas.fill_rect(rect, raster::Paint::solid(color));
    return;
  }
  canvas.fill_path(rounded_rect_path(rect, radius), raster::Paint::solid(color));
}

/// 填充发丝线（1px 分隔线/表格线）：对齐到整像素，保证任何缩放下都锐利。
void fill_line_shape(raster::Surface& canvas, math::Rect rect, math::Color color) {
  if (rect.is_empty() || color.a == 0U) return;
  const math::IntRect aligned = rect.round_out();
  canvas.fill_rect(math::Rect{static_cast<float>(aligned.x), static_cast<float>(aligned.y),
                              static_cast<float>(aligned.width),
                              static_cast<float>(aligned.height)},
                   raster::Paint::solid(color));
}

/// 填充圆点（自建正向多边形，理由同 `rounded_rect_path`）。
void fill_circle_shape(raster::Surface& canvas, math::Point center, float radius,
                       math::Color color) {
  if (radius <= 0.0f || color.a == 0U) return;
  constexpr int segments = 24;
  raster::Path path;
  for (int index = 0; index <= segments; ++index) {
    const double angle =
        kPi * 1.25 - kTwoPi * static_cast<double>(index) / static_cast<double>(segments);
    const auto x = static_cast<float>(static_cast<double>(center.x) +
                                      static_cast<double>(radius) * std::cos(angle));
    const auto y = static_cast<float>(static_cast<double>(center.y) +
                                      static_cast<double>(radius) * std::sin(angle));
    if (index == 0) {
      path.move_to(math::Point{x, y});
    } else {
      path.line_to(math::Point{x, y});
    }
  }
  path.close();
  canvas.fill_path(path, raster::Paint::solid(color));
}

/// 折行用的词元：ASCII 连续非空白为一个词、非 ASCII 逐码点、空白为分隔符、换行为硬断行。
struct WordToken {
  std::string text{};
  bool space{false};
  bool hard_break{false};
};

[[nodiscard]] auto tokenize(std::string_view text) -> std::vector<WordToken> {
  std::vector<WordToken> tokens;
  std::string word;
  const auto flush_word = [&tokens, &word]() {
    if (word.empty()) return;
    tokens.push_back(WordToken{word, false, false});
    word.clear();
  };
  std::size_t index = 0;
  while (index < text.size()) {
    const std::size_t start = index;
    const st::Codepoint codepoint = st::decode_utf8(text, index);
    const std::string_view raw = text.substr(start, index - start);
    if (codepoint.value == U'\n' || codepoint.value == U'\r') {
      flush_word();
      tokens.push_back(WordToken{std::string{}, false, true});
      continue;
    }
    if (st::is_space_codepoint(codepoint.value)) {
      flush_word();
      tokens.push_back(WordToken{" ", true, false});
      continue;
    }
    if (codepoint.value < 0x80U) {
      word.append(raw);
      continue;
    }
    // 非 ASCII（CJK/emoji/符号）：逐码点成词 ⇒ 任何位置可断行
    flush_word();
    tokens.push_back(WordToken{std::string(raw), false, false});
  }
  flush_word();
  return tokens;
}

/// 行内节点的纯文本（语义通道用：去标记）。
void append_inline_text(const std::vector<st::md::Inline>& inlines, std::string& out) {
  for (const st::md::Inline& node : inlines) {
    switch (node.kind) {
      case st::md::InlineKind::Text:
      case st::md::InlineKind::Code:
      case st::md::InlineKind::Emphasis:
      case st::md::InlineKind::Strong:
      case st::md::InlineKind::Strikethrough:
        out.append(node.text);
        break;
      case st::md::InlineKind::Link:
        out.append(node.text.empty() ? node.url : node.text);
        break;
      case st::md::InlineKind::Image:
        out.append(node.text.empty() ? node.url : node.text);
        break;
      case st::md::InlineKind::SoftBreak:
        out.push_back(' ');
        break;
      case st::md::InlineKind::HardBreak:
        out.push_back('\n');
        break;
    }
  }
}

}  // namespace

// —— 构造与内容 ——

MarkdownView::MarkdownView(std::string markdown) {
  metrics_.heading_sizes = {34.0f, 26.0f, 20.0f, 16.0f, 14.0f, 14.0f};  // = 主题 font_3xl..font_base
  metrics_.heading_margins = kHeadingMargins;
  metrics_.base = 14.0f;
  metrics_.code = 13.0f;
  metrics_.table = 12.0f;
  style_.background = math::Color{0, 0, 0, 0};
  style_.padding = math::Insets::symmetric(16.0f, 12.0f);
  style_.font_size = metrics_.base;
  set_focusable(true);
  if (!markdown.empty()) set_markdown(std::move(markdown));
}

void MarkdownView::set_markdown(std::string markdown) {
  stream_.reset();
  blocks_.clear();
  if (!markdown.empty()) blocks_ = stream_.feed(markdown);
  rebuild_plain_text();
  scroll_offset_ = 0.0f;
  want_bottom_ = false;
  needs_layout_ = true;
  mark_layout_dirty();
}

void MarkdownView::append_chunk(std::string_view chunk) {
  if (chunk.empty()) return;
  const bool stick = stick_to_bottom_ && at_bottom();
  blocks_ = stream_.feed(chunk);
  rebuild_plain_text();
  needs_layout_ = true;
  mark_layout_dirty();
  if (stick) want_bottom_ = true;
}

auto MarkdownView::markdown() const -> std::string { return stream_.source(); }

auto MarkdownView::block_count() const noexcept -> std::size_t { return blocks_.size(); }

void MarkdownView::set_selectable(bool value) {
  if (selectable_ == value) return;
  selectable_ = value;
  mark_dirty();
}

void MarkdownView::set_max_width(float width) {
  const float next = width > 0.0f ? width : 0.0f;
  if (next == max_width_) return;
  max_width_ = next;
  needs_layout_ = true;
  mark_layout_dirty();
}

void MarkdownView::set_base_font_size(float size) {
  const float next = size > 0.0f ? size : 0.0f;
  if (next == base_font_size_) return;
  base_font_size_ = next;
  needs_layout_ = true;
  mark_layout_dirty();
}

void MarkdownView::set_code_font_size(float size) {
  const float next = size > 0.0f ? size : 0.0f;
  if (next == code_font_size_) return;
  code_font_size_ = next;
  needs_layout_ = true;
  mark_layout_dirty();
}

// —— 主题 ——

void MarkdownView::apply_theme(const Theme& theme) {
  const Metrics& metrics = theme.metrics();
  TextMetrics next;
  next.heading_sizes = {metrics.font_3xl, metrics.font_2xl, metrics.font_xl,
                        metrics.font_lg,  metrics.font_base, metrics.font_base};
  next.heading_margins = kHeadingMargins;
  next.base = base_font_size_ > 0.0f ? base_font_size_ : metrics.font_base;
  next.code = code_font_size_ > 0.0f ? code_font_size_ : metrics.font_sm;
  next.table = metrics.font_xs;
  if (!(next == metrics_)) {
    metrics_ = next;
    needs_layout_ = true;  // 字号变了才需要重排版；换配色不需要（颜色在绘制时解析）
  }
  style_.background = math::Color{0, 0, 0, 0};
  style_.color = theme.colors().text;
  style_.font_size = metrics_.base;
  style_.padding = math::Insets::symmetric(16.0f, 12.0f);
}

// —— 语义文本 ——

void MarkdownView::rebuild_plain_text() {
  plain_.clear();
  append_plain(blocks_, 0);
}

void MarkdownView::append_plain(const std::vector<st::md::Block>& blocks, std::uint32_t depth) {
  if (depth > kMaxNestingDepth) return;
  for (const st::md::Block& block : blocks) {
    switch (block.kind) {
      case st::md::BlockKind::Paragraph:
      case st::md::BlockKind::Heading:
        append_inline_text(block.inlines, plain_);
        plain_.push_back('\n');
        break;
      case st::md::BlockKind::CodeBlock:
        plain_.append(block.code);
        if (!block.code.empty() && block.code.back() != '\n') plain_.push_back('\n');
        break;
      case st::md::BlockKind::List:
        for (const st::md::Block& item : block.children) {
          append_inline_text(item.inlines, plain_);
          plain_.push_back('\n');
          append_plain(item.children, depth + 1);
        }
        break;
      case st::md::BlockKind::Quote:
        append_plain(block.children, depth + 1);
        break;
      case st::md::BlockKind::Table: {
        const auto append_row = [this](const std::vector<std::string>& cells) {
          for (std::size_t index = 0; index < cells.size(); ++index) {
            if (index > 0) plain_.append(" | ");
            plain_.append(cells[index]);
          }
          plain_.push_back('\n');
        };
        append_row(block.header);
        for (const std::vector<std::string>& row : block.rows) append_row(row);
        break;
      }
      case st::md::BlockKind::Divider:
      case st::md::BlockKind::HtmlBlock:
        break;  // 分隔线无文本；HTML 块按 md 层约定不渲染、不进入语义
    }
  }
}

// —— 语义 / 控制通道 ——

auto MarkdownView::semantics_value() const -> std::string {
  return std::format("blocks={} scroll={:.1f}/{:.1f}", blocks_.size(), scroll_offset_, max_scroll());
}

auto MarkdownView::semantics_flags() const -> SemanticsFlags {
  SemanticsFlags flags = Element::semantics_flags();
  flags.scrollable = true;
  flags.selected = selectable_;
  return flags;
}

auto MarkdownView::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "markdown") return markdown();
  if (name == "text") return plain_;
  if (name == "block_count") return std::format("{}", blocks_.size());
  if (name == "content_height") return std::format("{:.2f}", content_height_);
  if (name == "scroll_offset") return std::format("{:.2f}", scroll_offset_);
  if (name == "max_scroll") return std::format("{:.2f}", max_scroll());
  if (name == "viewport_height") return std::format("{:.2f}", view_height_);
  if (name == "base_font_size") return std::format("{:.2f}", metrics_.base);
  if (name == "code_font_size") return std::format("{:.2f}", metrics_.code);
  if (name == "max_width") return std::format("{:.2f}", max_width_);
  if (name == "selectable") return selectable_ ? "true" : "false";
  if (name == "stick_to_bottom") return stick_to_bottom_ ? "true" : "false";
  if (name == "pending") return stream_.pending() ? "true" : "false";
  return std::nullopt;
}

auto MarkdownView::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "markdown") {
    set_markdown(std::string(value));
    return true;
  }
  if (name == "append") {
    append_chunk(value);
    return true;
  }
  if (name == "selectable") {
    const auto flag = st::parse_bool(value);
    if (!flag.has_value()) return false;
    set_selectable(*flag);
    return true;
  }
  if (name == "stick_to_bottom") {
    const auto flag = st::parse_bool(value);
    if (!flag.has_value()) return false;
    stick_to_bottom_ = *flag;
    return true;
  }
  if (name == "scroll_offset") {
    const auto number = st::parse_f64(value);
    if (!number.has_value()) return false;
    scroll_to(static_cast<float>(*number));
    return true;
  }
  if (name == "scroll_to_bottom") {
    scroll_to(max_scroll());
    return true;
  }
  if (name == "scroll_to_top") {
    scroll_to(0.0f);
    return true;
  }
  if (name == "max_width" || name == "base_font_size" || name == "code_font_size") {
    const auto number = st::parse_f64(value);
    if (!number.has_value()) return false;
    const auto size = static_cast<float>(*number);
    if (name == "max_width") set_max_width(size);
    else if (name == "base_font_size") set_base_font_size(size);
    else set_code_font_size(size);
    return true;
  }
  return false;
}

auto MarkdownView::property_names() const -> std::vector<std::string_view> {
  return {"markdown",       "text",          "block_count",    "content_height",
          "scroll_offset",  "max_scroll",    "viewport_height", "base_font_size",
          "code_font_size", "max_width",     "selectable",     "stick_to_bottom",
          "pending"};
}

auto MarkdownView::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "append") {
    append_chunk(argument);
    return true;
  }
  if (action == "clear") {
    set_markdown(std::string{});
    return true;
  }
  if (action == "set_markdown") {
    set_markdown(std::string(argument));
    return true;
  }
  if (action == "scroll_to_bottom") {
    scroll_to(max_scroll());
    return true;
  }
  if (action == "scroll_to_top") {
    scroll_to(0.0f);
    return true;
  }
  return Element::invoke_action(action, argument);
}

// —— 排版（`measure` 期构建行模型）——

void MarkdownView::rebuild_rows(const RenderContext& context, float width) {
  rows_.clear();
  const float avail = std::max(width, kMinTextWidth);
  if (blocks_.empty()) {
    // 空内容：占位一行（faint 的「（空）」），保证元素仍有可命中的高度
    Row row;
    const float line_height = metrics_.base * kBodyLineHeight;
    Span placeholder;
    placeholder.text = "（空）";
    placeholder.color = ColorRole::Faint;
    placeholder.size = metrics_.base;
    row.kind = RowKind::Text;
    row.box = math::Rect{0.0f, 0.0f, avail, line_height};
    row.line_height = line_height;
    row.spans.push_back(std::move(placeholder));
    rows_.push_back(std::move(row));
    content_height_ = line_height + style_.padding.vertical();
    return;
  }
  Frame frame;
  frame.avail = avail;
  float y = 0.0f;
  append_blocks(blocks_, context, frame, y);
  content_height_ = y + style_.padding.vertical();
}

void MarkdownView::append_blocks(const std::vector<st::md::Block>& blocks,
                                 const RenderContext& context, const Frame& frame, float& y) {
  const auto leading_gap = [&frame](const st::md::Block& node) -> float {
    const float body_gap = frame.nested ? kNestedGap : kBlockGap;
    switch (node.kind) {
      case st::md::BlockKind::Heading: {
        const std::uint32_t level = std::clamp(node.level, 1U, 6U);
        const float margin = kHeadingMargins[static_cast<std::size_t>(level - 1U)];
        return frame.nested ? margin * 0.5f : margin;
      }
      case st::md::BlockKind::Paragraph:
      case st::md::BlockKind::List:
      case st::md::BlockKind::Quote:
      case st::md::BlockKind::CodeBlock:
      case st::md::BlockKind::Table:
      case st::md::BlockKind::Divider:
        return body_gap;
      case st::md::BlockKind::HtmlBlock:
        return 0.0f;
    }
    return body_gap;
  };

  float pending_gap = 0.0f;
  for (const st::md::Block& block : blocks) {
    const float gap = std::max(pending_gap, leading_gap(block));
    if (y > 0.0f && gap > 0.0f) y += gap;
    pending_gap = 0.0f;
    switch (block.kind) {
      case st::md::BlockKind::Heading:
        append_heading(block, context, frame, y);
        break;
      case st::md::BlockKind::Paragraph:
        append_paragraph(block.inlines, context, frame, y, metrics_.base,
                         metrics_.base * kBodyLineHeight, FontWeight::Regular, RowKind::Text);
        break;
      case st::md::BlockKind::CodeBlock:
        append_code_block(block, context, frame, y);
        break;
      case st::md::BlockKind::List:
        append_list(block, context, frame, y);
        break;
      case st::md::BlockKind::Quote:
        append_quote(block, context, frame, y);
        break;
      case st::md::BlockKind::Table:
        append_table(block, context, frame, y);
        break;
      case st::md::BlockKind::Divider:
        append_divider(frame, y);
        pending_gap = frame.nested ? kNestedGap : kBlockGap;
        break;
      case st::md::BlockKind::HtmlBlock:
        break;  // 按 md 层约定：HTML 块原样保留、不渲染
    }
  }
}

void MarkdownView::append_heading(const st::md::Block& block, const RenderContext& context,
                                  const Frame& frame, float& y) {
  const std::uint32_t level = std::clamp(block.level, 1U, 6U);
  const float size = metrics_.heading_sizes[static_cast<std::size_t>(level - 1U)];
  const FontWeight weight = level <= 2U ? FontWeight::Bold : FontWeight::SemiBold;
  append_paragraph(block.inlines, context, frame, y, size, size * kHeadingLineHeight, weight,
                   RowKind::Heading);
  if (level <= 2U) {
    y += kHeadingRuleGap;
    append_divider(frame, y);  // 标题下方细分隔线（仅 H1/H2；颜色 = colors.border）
    y += kHeadingRuleGap;
  }
  y += kHeadingBottomGap;
}

void MarkdownView::append_paragraph(const std::vector<st::md::Inline>& inlines,
                                    const RenderContext& context, const Frame& frame, float& y,
                                    float size, float line_height, FontWeight weight,
                                    RowKind kind) {
  std::vector<Span> spans = inline_spans(inlines, frame.role, size, weight);
  if (spans.empty()) return;
  push_spans(std::move(spans), context, frame.indent, frame.avail, line_height, kind, y);
}

void MarkdownView::append_divider(const Frame& frame, float& y) {
  Row row;
  row.kind = RowKind::Divider;
  row.box = math::Rect{frame.indent, y, std::max(frame.avail, kMinTextWidth), kDividerThickness};
  row.line_height = kDividerThickness;
  row.text_x = frame.indent;
  rows_.push_back(std::move(row));
  y += kDividerThickness;
}

void MarkdownView::push_spans(std::vector<Span> spans, const RenderContext& context, float x,
                              float avail, float line_height, RowKind kind, float& y) {
  if (spans.empty()) return;
  const TextPort& port = text_port_of(context);
  const float width = std::max(avail, kMinTextWidth);
  std::vector<Span> line{};
  float line_width = 0.0f;

  const auto flush = [&rows = rows_, &line, &line_width, &y, x, width, line_height, kind]() {
    Row row;
    row.kind = kind;
    row.box = math::Rect{x, y, width, line_height};
    row.line_height = line_height;
    row.text_x = x;
    row.spans = line;
    rows.push_back(std::move(row));
    line.clear();
    line_width = 0.0f;
    y += line_height;
  };

  const auto same_style = [](const Span& left, const Span& right) {
    return left.color == right.color && left.size == right.size && left.weight == right.weight &&
           left.underline == right.underline && left.strike == right.strike &&
           left.inline_code == right.inline_code;
  };

  const auto append_piece = [&port, &line, &line_width, &same_style](const Span& style,
                                                                    std::string text) {
    if (text.empty()) return;
    const float text_width = port.measure_width(text, style.size);
    if (!line.empty() && same_style(line.back(), style)) {
      line.back().text += text;
    } else {
      Span next = style;
      next.text = std::move(text);
      line.push_back(std::move(next));
    }
    line_width += text_width;
  };

  for (const Span& span : spans) {
    if (span.text.empty()) continue;
    for (const WordToken& word : tokenize(span.text)) {
      if (word.hard_break) {
        flush();  // 硬换行：显式另起一行（空行照留）
        continue;
      }
      if (word.space) {
        if (line.empty()) continue;  // 行首不留空格
        append_piece(span, std::string{word.text});
        continue;
      }
      const float word_width = port.measure_width(word.text, span.size);
      if (!line.empty() && line_width + word_width > width) flush();
      if (word_width > width) {
        // 单词/URL 超宽：按码点硬拆，保证不越界也不丢内容
        std::string piece;
        const std::size_t codepoints = st::utf8_length(word.text);
        for (std::size_t index = 0; index < codepoints; ++index) {
          const std::string_view one = st::utf8_slice(word.text, index, 1);
          const float one_width = port.measure_width(one, span.size);
          if (!piece.empty() && port.measure_width(piece, span.size) + one_width > width) {
            append_piece(span, piece);
            flush();
            piece.clear();
          }
          piece.append(one);
        }
        if (!piece.empty()) append_piece(span, piece);
        continue;
      }
      append_piece(span, std::string{word.text});
    }
  }
  if (!line.empty()) flush();
}

auto MarkdownView::inline_spans(const std::vector<st::md::Inline>& inlines, ColorRole role,
                                float size, FontWeight weight) const -> std::vector<Span> {
  std::vector<Span> spans;
  spans.reserve(inlines.size());
  for (const st::md::Inline& node : inlines) {
    Span span;
    span.size = size;
    span.weight = weight;
    span.color = role;
    switch (node.kind) {
      case st::md::InlineKind::Text:
        span.text = node.text;
        break;
      case st::md::InlineKind::Code:
        span.text = node.text;
        span.color = ColorRole::Text;
        span.size = metrics_.code;
        span.weight = FontWeight::Regular;
        span.inline_code = true;
        span.code = true;  // 行内码也等宽
        break;
      case st::md::InlineKind::Emphasis:
        span.text = node.text;
        span.weight = FontWeight::Medium;
        break;
      case st::md::InlineKind::Strong:
        span.text = node.text;
        span.weight = FontWeight::SemiBold;
        break;
      case st::md::InlineKind::Strikethrough:
        span.text = node.text;
        span.strike = true;
        break;
      case st::md::InlineKind::Link:
        span.text = node.text.empty() ? node.url : node.text;
        span.color = ColorRole::Primary;
        span.underline = true;
        break;
      case st::md::InlineKind::Image:
        span.text = std::format("[图] {}", node.text.empty() ? node.url : node.text);
        span.color = ColorRole::Muted;
        break;
      case st::md::InlineKind::SoftBreak:
        span.text = " ";
        break;
      case st::md::InlineKind::HardBreak:
        span.text = "\n";
        break;
    }
    if (span.text.empty()) continue;
    spans.push_back(std::move(span));
  }
  return spans;
}

void MarkdownView::append_code_block(const st::md::Block& block, const RenderContext& context,
                                     const Frame& frame, float& y) {
  (void)context;
  const float size = metrics_.code;
  const float line_height = size * kCodeLineHeight;
  const float box_width = std::max(frame.avail, kMinTextWidth);
  const float text_x = frame.indent + kCodePad;
  const float text_width = std::max(box_width - kCodePad * 2.0f, kMinTextWidth);
  const bool labelled = !block.language.empty();
  const float top = y;
  float inner_y = y + kCodePad + (labelled ? kLanguageLabelHeight + 4.0f : 0.0f);
  const std::size_t first_row = rows_.size();

  const auto push_line = [this, &block, &text_x, &text_width, &frame, &box_width, &line_height,
                          &size, &top](float line_y, std::size_t begin, std::size_t end,
                                       const std::vector<st::md::Token>& tokens,
                                       std::size_t& token_index) {
    Row row;
    row.kind = RowKind::CodeLine;
    row.box = math::Rect{frame.indent, line_y, box_width, line_height};
    row.line_height = line_height;
    row.text_x = text_x;
    row.clip = math::Rect{text_x, line_y, text_width, line_height};
    row.group = GroupKind::Code;
    row.group_rect = math::Rect{frame.indent, top, box_width, 0.0f};  // 高度稍后回填
    if (end > begin) {
      row.spans = code_line_spans(block.code, begin, end, tokens, token_index, size);
      // 代码块行全部走等宽（语法高亮的每个 token 片段都是代码）
      for (Span& span : row.spans) span.code = true;
    }
    rows_.push_back(std::move(row));
  };

  const std::vector<st::md::Token> tokens = st::md::highlight(block.code, block.language);
  std::size_t token_index = 0;
  std::size_t line_begin = 0;
  while (line_begin < block.code.size()) {
    std::size_t line_end = block.code.find('\n', line_begin);
    if (line_end == std::string::npos) line_end = block.code.size();
    push_line(inner_y, line_begin, line_end, tokens, token_index);
    inner_y += line_height;
    if (line_end >= block.code.size()) break;
    line_begin = line_end + 1;
  }
  if (rows_.size() == first_row) {
    push_line(inner_y, 0, 0, tokens, token_index);  // 空代码块：仍占一行高度
    inner_y += line_height;
  }

  const float bottom = inner_y + kCodePad;
  const math::Rect group{frame.indent, top, box_width, bottom - top};
  for (std::size_t index = first_row; index < rows_.size(); ++index) {
    rows_[index].group_rect = group;
  }
  Row& head = rows_[first_row];
  head.group_first = true;
  if (labelled) head.language = block.language;
  y = bottom;
}

auto MarkdownView::code_line_spans(std::string_view code, std::size_t begin, std::size_t end,
                                   const std::vector<st::md::Token>& tokens,
                                   std::size_t& token_index, float size) const -> std::vector<Span> {
  std::vector<Span> spans;
  std::size_t position = begin;
  while (position < end) {
    while (token_index < tokens.size() && tokens[token_index].end <= position) ++token_index;
    std::size_t next = end;
    st::md::TokenKind kind = st::md::TokenKind::Plain;
    if (token_index < tokens.size()) {
      const st::md::Token& token = tokens[token_index];
      if (token.begin <= position) {
        kind = token.kind;
        next = std::min(token.end, end);
        if (next <= position) next = position + 1;  // 防御：零长度区间
      } else {
        next = std::min(token.begin, end);  // 未着色前缀
      }
    }
    if (next <= position) break;
    Span span;
    span.text = std::string(code.substr(position, next - position));
    span.size = size;
    span.color = role_of_token(kind);
    spans.push_back(std::move(span));
    position = next;
  }
  return spans;
}

void MarkdownView::append_list(const st::md::Block& block, const RenderContext& context,
                               const Frame& frame, float& y) {
  const TextPort& port = text_port_of(context);
  const float level_indent = frame.indent + static_cast<float>(block.level) * kListIndentStep;
  const float level_width =
      std::max(frame.avail - static_cast<float>(block.level) * kListIndentStep, kMinTextWidth);
  const float line_height = metrics_.base * kBodyLineHeight;

  bool has_task = false;
  for (const st::md::Block& item : block.children) has_task = has_task || item.task_item;
  float marker_column = kListMarkerColumn;
  if (has_task) {
    marker_column = kTaskBoxSize + 8.0f;
  } else if (block.ordered) {
    float widest = 0.0f;
    for (std::size_t index = 0; index < block.children.size(); ++index) {
      widest = std::max(widest, port.measure_width(std::format("{}.", index + 1U), metrics_.base));
    }
    marker_column = std::max(widest + 8.0f, kListMarkerColumn);
  }
  const float text_x = level_indent + marker_column;
  const float text_width = std::max(level_width - marker_column, kMinTextWidth);

  for (std::size_t index = 0; index < block.children.size(); ++index) {
    const st::md::Block& item = block.children[index];
    if (index > 0) y += kListItemGap;
    const std::size_t first_row = rows_.size();
    std::vector<Span> spans =
        inline_spans(item.inlines, frame.role, metrics_.base, FontWeight::Regular);
    if (!spans.empty()) {
      push_spans(std::move(spans), context, text_x, text_width, line_height, RowKind::ListRow, y);
    }
    Row* head = rows_.size() > first_row ? &rows_[first_row] : nullptr;
    if (head == nullptr) {
      // 仅有嵌套内容的列表项：仍补一行承载项目符号
      Row row;
      row.kind = RowKind::ListRow;
      row.box = math::Rect{level_indent, y, marker_column + text_width, line_height};
      row.line_height = line_height;
      row.text_x = text_x;
      rows_.push_back(std::move(row));
      head = &rows_.back();
      y += line_height;
    }
    if (item.task_item) {
      head->task = true;
      head->checked = item.checked;
      head->marker = math::Rect{level_indent,
                                head->box.y + (line_height - kTaskBoxSize) * 0.5f,
                                kTaskBoxSize, kTaskBoxSize};
    } else if (block.ordered) {
      head->marker_text = std::format("{}.", index + 1U);
      head->marker = math::Rect{level_indent, head->box.y, marker_column - 8.0f, line_height};
    } else {
      head->marker_dot = true;
      head->marker = math::Rect{level_indent, head->box.y, kBulletSize, line_height};
    }
    if (!item.children.empty()) {
      Frame child{text_x, text_width, frame.role, true};
      append_blocks(item.children, context, child, y);
    }
  }
}

void MarkdownView::append_quote(const st::md::Block& block, const RenderContext& context,
                                const Frame& frame, float& y) {
  const float top = y;
  const float inner_x = frame.indent + kQuoteBarWidth + kQuotePadX;
  const float inner_width =
      std::max(frame.avail - kQuoteBarWidth - kQuotePadX * 2.0f, kMinTextWidth);
  y += kQuotePadY;
  const std::size_t first_row = rows_.size();
  Frame inner{inner_x, inner_width, ColorRole::Muted, true};
  append_blocks(block.children, context, inner, y);
  y += kQuotePadY;
  if (rows_.size() > first_row) {
    Row& head = rows_[first_row];
    head.group = GroupKind::Quote;
    head.group_rect = math::Rect{frame.indent, top, std::max(frame.avail, kMinTextWidth), y - top};
    head.group_first = true;
  } else {
    y = top;  // 空引用不占位
  }
}

void MarkdownView::append_table(const st::md::Block& block, const RenderContext& context,
                                const Frame& frame, float& y) {
  const TextPort& port = text_port_of(context);
  const std::size_t columns = block.header.size();
  if (columns == 0) return;
  const float size = metrics_.table;
  const float padding = kTableCellPad;
  std::vector<float> widths(columns, 0.0f);
  for (std::size_t column = 0; column < columns; ++column) {
    widths[column] = port.measure_width(block.header[column], size) + padding * 2.0f + 1.0f;
  }
  for (const std::vector<std::string>& row : block.rows) {
    for (std::size_t column = 0; column < columns && column < row.size(); ++column) {
      widths[column] =
          std::max(widths[column], port.measure_width(row[column], size) + padding * 2.0f + 1.0f);
    }
  }
  float total = 0.0f;
  for (const float column_width : widths) total += column_width;
  if (total > frame.avail && total > 0.0f) {
    const float scale = frame.avail / total;
    total = 0.0f;
    for (float& column_width : widths) {
      column_width = std::max(column_width * scale, kTableMinColumn);
      total += column_width;
    }
  }

  const auto push_row = [this, &columns, &widths, &frame, &port, &padding, &total, &y, size](
                            const std::vector<std::string>& cells, bool header, bool top,
                            bool bottom) {
    Row row;
    row.kind = RowKind::TableRow;
    row.box = math::Rect{frame.indent, y, total, kTableRowHeight};
    row.line_height = kTableRowHeight;
    row.text_x = frame.indent;
    row.clip = math::Rect{frame.indent, y, std::min(total, frame.avail), kTableRowHeight};
    row.table_header = header;
    row.table_top = top;
    row.table_bottom = bottom;
    float x = frame.indent;
    for (std::size_t column = 0; column < columns; ++column) {
      Cell cell;
      cell.box = math::Rect{x, y, widths[column], kTableRowHeight};
      const std::string_view raw =
          column < cells.size() ? std::string_view(cells[column]) : std::string_view{};
      cell.text = port.ellipsize(raw, size, std::max(widths[column] - padding * 2.0f, 8.0f));
      row.cells.push_back(std::move(cell));
      x += widths[column];
    }
    rows_.push_back(std::move(row));
    y += kTableRowHeight;
  };

  push_row(block.header, true, true, block.rows.empty());
  for (std::size_t index = 0; index < block.rows.size(); ++index) {
    push_row(block.rows[index], false, false, index + 1U == block.rows.size());
  }
}

// —— 滚动 ——

auto MarkdownView::max_scroll() const noexcept -> float {
  return std::max(0.0f, content_height_ - view_height_);
}

auto MarkdownView::clamp_scroll(float offset) const noexcept -> float {
  return std::clamp(offset, 0.0f, max_scroll());
}

auto MarkdownView::at_bottom() const noexcept -> bool {
  return scroll_offset_ >= max_scroll() - 0.5f;
}

auto MarkdownView::scroll_offset() const noexcept -> float { return scroll_offset_; }

auto MarkdownView::content_height() const noexcept -> float { return content_height_; }

void MarkdownView::scroll_to(float offset) {
  const float next = clamp_scroll(offset);
  if (next == scroll_offset_) return;
  scroll_offset_ = next;
  mark_dirty();
}

void MarkdownView::settle_scroll() {
  if (want_bottom_) {
    scroll_offset_ = max_scroll();  // 追加前贴底 ⇒ 追加后继续贴底
    want_bottom_ = false;
  }
  scroll_offset_ = clamp_scroll(scroll_offset_);
}

// —— 布局与绘制 ——

void MarkdownView::measure(const RenderContext& context, const Constraints& constraints) {
  float width = constraints.max_width < kUnbounded ? constraints.max_width : kDefaultViewWidth;
  if (max_width_ > 0.0f) width = std::min(width, max_width_);
  width = std::max(width, kMinViewWidth);
  const float content_width = std::max(width - style_.padding.horizontal(), kMinTextWidth);
  if (needs_layout_ || content_width != layout_width_ || context.text != layout_port_) {
    rebuild_rows(context, content_width);
    layout_width_ = content_width;
    layout_port_ = context.text;
    needs_layout_ = false;
  }
  float height = content_height_;  // 全量内容高度（与视口无关）
  if (style_.has_explicit_height()) height = style_.height;
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  view_height_ = std::max(height, 0.0f);
  measured_ = math::Size{width, view_height_};
  settle_scroll();
}

void MarkdownView::arrange(const RenderContext& context, math::Rect rect) {
  Element::arrange(context, rect);
  view_height_ = std::max(bounds_.height, 0.0f);
  settle_scroll();
}

auto MarkdownView::role_of_token(st::md::TokenKind kind) -> ColorRole {
  switch (kind) {
    case st::md::TokenKind::Plain: return ColorRole::CodePlain;
    case st::md::TokenKind::Keyword: return ColorRole::CodeKeyword;
    case st::md::TokenKind::Type: return ColorRole::CodeType;
    case st::md::TokenKind::String: return ColorRole::CodeString;
    case st::md::TokenKind::Number: return ColorRole::CodeNumber;
    case st::md::TokenKind::Comment: return ColorRole::CodeComment;
    case st::md::TokenKind::Function: return ColorRole::CodeFunction;
    case st::md::TokenKind::Operator: return ColorRole::CodeOperator;
    case st::md::TokenKind::Punctuation: return ColorRole::CodeOperator;
    case st::md::TokenKind::Preprocessor: return ColorRole::CodePreprocessor;
    case st::md::TokenKind::Builtin: return ColorRole::CodeBuiltin;
    // 通用引擎新增的类别：Markdown 代码块里按"语义最接近的既有角色"归并
    // （编辑器组件有独立的语法色板，见 `CodeEditor` 的 SyntaxPalette）
    case st::md::TokenKind::Attribute: return ColorRole::CodePreprocessor;
    case st::md::TokenKind::Key: return ColorRole::CodeType;
    case st::md::TokenKind::Tag: return ColorRole::CodeKeyword;
    case st::md::TokenKind::Inserted: return ColorRole::CodeString;
    case st::md::TokenKind::Deleted: return ColorRole::CodeComment;
  }
  return ColorRole::CodePlain;
}

auto MarkdownView::color_of(const Theme& theme, ColorRole role) -> math::Color {
  const Palette& colors = theme.colors();
  switch (role) {
    case ColorRole::Text: return colors.text;
    case ColorRole::Muted: return colors.text_muted;
    case ColorRole::Faint: return colors.text_faint;
    case ColorRole::Primary: return colors.primary;
    case ColorRole::Success: return colors.success;
    case ColorRole::Accent: return colors.accent;
    case ColorRole::Warning: return colors.warning;
    case ColorRole::CodePlain: return colors.text;
    case ColorRole::CodeKeyword: return colors.primary;
    case ColorRole::CodeString: return colors.success;
    case ColorRole::CodeNumber: return colors.accent;
    case ColorRole::CodeComment: return colors.text_faint;
    case ColorRole::CodeType: return colors.accent.mix(colors.primary, 0.45f);
    case ColorRole::CodeFunction: return colors.text.mix(colors.primary, 0.75f);
    case ColorRole::CodeBuiltin: return colors.accent.mix(colors.text, 0.25f);
    case ColorRole::CodeOperator: return colors.text_muted;
    case ColorRole::CodePreprocessor: return colors.warning;
  }
  return colors.text;
}

void MarkdownView::draw_text_weighted(const TextPort& port, raster::Surface& canvas,
                                      std::string_view text, math::Point origin, float size,
                                      math::Color color, FontWeight weight,
                                      text::FontRole role) {
  // 端口无字重通道：SemiBold/Bold 以亚像素偏移二次绘制近似（如实说明见头文件）
  port.draw(canvas, text, origin, size, color, role);
  if (weight == FontWeight::Bold) {
    port.draw(canvas, text, math::Point{origin.x + kBoldOffset, origin.y}, size, color, role);
  } else if (weight == FontWeight::SemiBold) {
    port.draw(canvas, text, math::Point{origin.x + kSemiBoldOffset, origin.y}, size, color, role);
  }
}

void MarkdownView::draw_spans(const RenderContext& context, raster::Surface& canvas,
                              const std::vector<Span>& spans, math::Point start, float max_right,
                              float line_height) const {
  const TextPort& port = text_port_of(context);
  const Palette& colors = context.theme.colors();
  float x = start.x;
  for (const Span& span : spans) {
    if (span.text.empty()) continue;
    const float remaining = max_right - x;
    if (remaining < 2.0f) break;
    // 量宽与绘制**必须同一角色**：一个用等宽、一个用比例，代码块就会算错宽度。
    const text::FontRole role = span.font_role();
    const float natural_width = port.measure_width(span.text, span.size, role);
    std::string text = span.text;
    float width = natural_width;
    const bool truncated = natural_width > remaining;
    if (truncated) {
      text = port.ellipsize(span.text, span.size, remaining);
      if (text.empty()) break;
      width = port.measure_width(text, span.size, role);
    }
    const math::Color color = color_of(context.theme, span.color);
    if (span.inline_code) {
      const float chip_height = span.size * 1.35f;
      const float chip_y = start.y + std::max(0.0f, (line_height - chip_height) * 0.5f);
      const math::Rect chip{x - kInlineCodePad, chip_y, width + kInlineCodePad * 2.0f, chip_height};
      fill_rect_shape(canvas, chip, colors.code_bg, kInlineCodeRadius);
      raster::Path outline;
      outline.add_rounded_rect(chip.inset(math::Insets::all(0.5f)), kInlineCodeRadius - 0.5f);
      canvas.stroke_path(outline, raster::Paint::solid(colors.code_border), 1.0f);
    }
    draw_text_weighted(port, canvas, text, math::Point{x, start.y}, span.size, color, span.weight,
                     span.font_role());
    if (span.underline) {
      fill_line_shape(canvas, math::Rect{x, start.y + span.size * 1.3f, width, 1.0f}, color);
    }
    if (span.strike) {
      fill_line_shape(canvas, math::Rect{x, start.y + span.size * 0.62f, width, 1.0f}, color);
    }
    x += width + (span.inline_code ? kInlineCodePad * 2.0f : 0.0f);
    if (truncated) break;
  }
}

void MarkdownView::paint_group(const RenderContext& context, raster::Surface& canvas, const Row& row,
                               math::Point origin) const {
  const Palette& colors = context.theme.colors();
  const Metrics& metrics = context.theme.metrics();
  const math::Rect rect = row.group_rect.offset(origin.x, origin.y);
  if (rect.is_empty()) return;
  if (row.group == GroupKind::Code) {
    fill_rect_shape(canvas, rect, colors.code_bg, metrics.radius_md);
    raster::Path outline;
    outline.add_rounded_rect(rect.inset(math::Insets::all(0.5f)),
                             std::max(metrics.radius_md - 0.5f, 0.0f));
    canvas.stroke_path(outline, raster::Paint::solid(colors.code_border), metrics.border_width);
    if (!row.language.empty()) {
      const TextPort& port = text_port_of(context);
      port.draw(canvas, row.language, math::Point{rect.x + kCodePad, rect.y + kCodePad},
                kLanguageLabelSize, colors.text_faint, text::FontRole::Monospace);
    }
    return;
  }
  if (row.group == GroupKind::Quote) {
    fill_rect_shape(canvas, rect, colors.surface_alt, metrics.radius_sm);
    fill_rect_shape(canvas, math::Rect{rect.x, rect.y, kQuoteBarWidth, rect.height}, colors.primary,
                    kQuoteBarWidth * 0.5f);
  }
}

void MarkdownView::paint_marker(const RenderContext& context, raster::Surface& canvas,
                                const Row& row, math::Point origin) const {
  if (row.marker.width <= 0.0f) return;
  const Palette& colors = context.theme.colors();
  const math::Rect marker = row.marker.offset(origin.x, origin.y);
  if (row.task) {
    const math::Rect box{marker.x, marker.y, kTaskBoxSize, kTaskBoxSize};
    if (row.checked) {
      fill_rect_shape(canvas, box, colors.primary, 4.0f);
      raster::Path check;
      check.move_to(math::Point{box.x + 3.6f, box.y + 7.4f});
      check.line_to(math::Point{box.x + 6.0f, box.y + 9.8f});
      check.line_to(math::Point{box.x + 10.6f, box.y + 4.6f});
      canvas.stroke_path(check, raster::Paint::solid(colors.on_primary), 1.8f);
    } else {
      raster::Path outline;
      outline.add_rounded_rect(box.inset(math::Insets::all(0.5f)), 3.5f);
      canvas.stroke_path(outline, raster::Paint::solid(colors.border_strong), 1.4f);
    }
    return;
  }
  if (row.marker_dot) {
    const float radius = kBulletSize * 0.5f;
    fill_circle_shape(canvas, math::Point{marker.x + radius + 1.0f, marker.y + row.line_height * 0.5f},
                      radius, colors.text_muted);
    return;
  }
  if (!row.marker_text.empty()) {
    const TextPort& port = text_port_of(context);
    const float line_height = port.line_height(metrics_.base);
    const float width = port.measure_width(row.marker_text, metrics_.base);
    port.draw(canvas, row.marker_text,
              math::Point{marker.right() - width, marker.y + (row.line_height - line_height) * 0.5f},
              metrics_.base, colors.text_muted);
  }
}

void MarkdownView::paint_table_row(const RenderContext& context, raster::Surface& canvas,
                                   const Row& row, math::Point origin) const {
  const Palette& colors = context.theme.colors();
  const TextPort& port = text_port_of(context);
  const float size = metrics_.table;
  const math::Rect box = row.box.offset(origin.x, origin.y);
  if (row.table_header) fill_rect_shape(canvas, box, colors.surface_alt);
  if (row.table_top) {
    fill_line_shape(canvas, math::Rect{box.x, box.y, box.width, 1.0f}, colors.border);
  }
  if (row.table_bottom) {
    fill_line_shape(canvas, math::Rect{box.x, box.bottom() - 1.0f, box.width, 1.0f}, colors.border);
  }
  for (std::size_t index = 0; index < row.cells.size(); ++index) {
    const Cell& cell = row.cells[index];
    const math::Rect cell_box = cell.box.offset(origin.x, origin.y);
    fill_line_shape(canvas, math::Rect{cell_box.x, cell_box.y, 1.0f, cell_box.height}, colors.border);
    if (index + 1U == row.cells.size()) {
      fill_line_shape(canvas, math::Rect{cell_box.right() - 1.0f, cell_box.y, 1.0f, cell_box.height},
                      colors.border);
    }
    if (cell.text.empty()) continue;
    // 与全仓同一口径（按墨迹区居中）。
    const float text_y = centered_line_top(port, cell.text, size, cell_box.y, cell_box.height);
    port.draw(canvas, cell.text, math::Point{cell_box.x + kTableCellPad, text_y}, size,
              color_of(context.theme, row.table_header ? ColorRole::Muted : ColorRole::Text));
  }
}

void MarkdownView::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  const math::Rect content = content_box();
  const math::Point origin{content.x, content.y - scroll_offset_};
  canvas.push_clip_rect(bounds_);
  for (const Row& row : rows_) {
    const math::Rect box =
        math::Rect{row.box.x + origin.x, row.box.y + origin.y, row.box.width, row.box.height};
    const bool outside = box.bottom() < bounds_.y || box.y > bounds_.bottom();
    if (outside && !row.group_first) continue;  // 视口外剔除（组首行必须绘制背景）
    if (row.group_first) paint_group(context, canvas, row, origin);
    const bool row_clip = !row.clip.is_empty();
    const math::Rect clip =
        row_clip ? math::Rect{row.clip.x + origin.x, row.clip.y + origin.y, row.clip.width,
                              row.clip.height}
                 : content;
    if (row_clip) canvas.push_clip_rect(clip);
    switch (row.kind) {
      case RowKind::Divider:
        fill_line_shape(canvas, box, context.theme.colors().border);
        break;
      case RowKind::TableRow:
        paint_table_row(context, canvas, row, origin);
        break;
      case RowKind::Text:
      case RowKind::Heading:
      case RowKind::ListRow:
      case RowKind::CodeLine: {
        paint_marker(context, canvas, row, origin);
        const float text_y =
            box.y + std::max(0.0f, (row.box.height - row.line_height) * 0.5f);
        draw_spans(context, canvas, row.spans, math::Point{row.text_x + origin.x, text_y},
                   std::min(clip.right(), box.right()), row.line_height);
        break;
      }
    }
    if (row_clip) canvas.pop_clip();
  }
  canvas.pop_clip();
}

auto MarkdownView::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  if (!visible_ || !enabled_) return false;
  switch (event.kind) {
    case EventKind::Wheel: {
      if (event.wheel_delta == 0.0f) return false;
      scroll_to(scroll_offset_ - event.wheel_delta);
      event.handled = true;
      return true;
    }
    case EventKind::KeyDown: {
      const float page = std::max(view_height_ * kPageRatio, kWheelStep);
      if (event.key == "ArrowDown") scroll_to(scroll_offset_ + kWheelStep);
      else if (event.key == "ArrowUp") scroll_to(scroll_offset_ - kWheelStep);
      else if (event.key == "PageDown" || event.key == " ") scroll_to(scroll_offset_ + page);
      else if (event.key == "PageUp") scroll_to(scroll_offset_ - page);
      else if (event.key == "Home") scroll_to(0.0f);
      else if (event.key == "End") scroll_to(max_scroll());
      else return false;
      event.handled = true;
      return true;
    }
    default:
      return false;
  }
}

}  // namespace st::ui

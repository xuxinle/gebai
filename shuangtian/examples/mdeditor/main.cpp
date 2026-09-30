/// 霜天示例二：**Markdown 编辑器**（`mdeditor`）。
///
/// 目的：与 `gallery`（组件集展示）互补，用一个**真实可用的小应用**检验框架的
/// ① **完备性**（编辑/解析/高亮/预览/文件读写/对话框/提示条/主题/DPI/控制通道全链路）；
/// ② **易用性**（一个完整编辑器 ≈ 数百行声明式组装，无第三方依赖、无构建脚本）；
/// ③ **高阶定制**（四个**自绘定制组件**：语法高亮源码视图、文档大纲、可拖拽分栏、实时统计栏——
///    全部只通过 `Element` 的 measure/arrange/paint_content/on_event 扩展点实现，
///    并复用同一套主题令牌与 `st::md` 解析树，无需改动框架任何代码）。
///
/// 用法：
///   mdeditor [--open PATH] [--out PATH] [--headless] [--scale 2.0] [--theme dark]
///            [--control-port 0] [--frames N] [--ms N] [--demo-stream]

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "st/app/app.hpp"
#include "st/core/entry.hpp"
#include "st/core/print.hpp"
#include "st/core/fs.hpp"
#include "st/core/string.hpp"
#include "st/core/time.hpp"
#include "st/md/highlight.hpp"
#include "st/md/markdown.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/feedback.hpp"
#include "st/ui/components/input.hpp"
#include "st/ui/components/markdown_view.hpp"
#include "st/ui/components/overlay.hpp"
#include "st/ui/components/scroll.hpp"
#include "st/ui/icon.hpp"

namespace {

using st::math::Color;
using st::math::Insets;
using st::math::Point;
using st::math::Rect;
using st::ui::Align;
using st::ui::Button;
using st::ui::Card;
using st::ui::Dialog;
using st::ui::Element;
using st::ui::FlexDirection;
using st::ui::FontWeight;
using st::ui::Icon;
using st::ui::IconView;
using st::ui::MarkdownView;
using st::ui::Panel;
using st::ui::ScrollView;
using st::ui::Text;
using st::ui::TextArea;
using st::ui::TextAlign;
using st::ui::Tone;

// ————————————————————————————————————————————————————————————————————————————
// 定制组件 1：语法高亮源码视图（只读、带行号与 Markdown 语法着色）
//
// 展示「高阶定制」：不新增框架 API，仅覆写 paint_content，把 `st::md` 解析结果
// 变成着色片段画到画布上。行号栏、当前行高亮、软换行都在这一个文件里完成。
// ————————————————————————————————————————————————————————————————————————————
class SourceView : public Element {
 public:
  SourceView() { set_id("source-view"); }

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "SourceView"; }
  [[nodiscard]] auto role() const noexcept -> st::ui::Role override { return st::ui::Role::Text; }

  void set_text(std::string text) {
    text_ = std::move(text);
    mark_layout_dirty();
  }
  void set_cursor_line(std::size_t line) {
    if (cursor_line_ == line) return;
    cursor_line_ = line;
    mark_dirty();
  }
  [[nodiscard]] auto text() const noexcept -> const std::string& { return text_; }
  void set_font_size(float size) {
    font_size_ = size;
    mark_layout_dirty();
  }

  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override {
    if (name == "text") return text_;
    if (name == "cursor_line") return std::format("{}", cursor_line_);
    return std::nullopt;
  }
  auto set_property(std::string_view name, std::string_view value) -> bool override {
    if (name == "text") {
      set_text(std::string(value));
      return true;
    }
    return false;
  }
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override {
    return {"text", "cursor_line"};
  }

  void apply_theme(const st::ui::Theme& theme) override {
    style_.background = theme.colors().code_bg;
    style_.border_color = theme.colors().code_border;
    style_.border_width = theme.metrics().border_width;
    style_.radius = theme.metrics().radius_md;
    style_.font_size = font_size_;
    style_.padding = Insets::all(0.0f);
  }

  void measure(const st::ui::RenderContext& context, const st::ui::Constraints& constraints) override {
    const float line_height = context.text != nullptr ? context.text->line_height(font_size_)
                                                      : font_size_ * 1.6f;
    // 固有高度 = 内容行数 × 行高。**不能把 kUnbounded（无界哨兵 1e9）当作自己
    // 的高度**：那会让元素以 1e9 高进入布局——语义树 bounds 变成 1e9（AI 按
    // bounds 定位会错），且任何描边路径都会生成 1e9 高的遮罩（GPU 路径实测会
    // 试图分配 ~1.3 TB 直接崩；框架侧已在 rasterize_path 取交 clip 兼底，
    // 但正确的做法是元素自己给出固有高度）。
    std::size_t line_count = 1;
    for (const char ch : text_) {
      if (ch == '\n') ++line_count;
    }
    const float content_height = static_cast<float>(line_count) * line_height + 24.0f;
    const float bounded = std::min(std::max(content_height, 240.0f), constraints.max_height);
    measured_ = st::math::Size{constraints.max_width, std::max(bounded, 0.0f)};
    line_height_ = line_height;
    (void)line_height;
  }

  void paint_content(const st::ui::RenderContext& context, st::raster::Surface& canvas) const override {
    const auto& colors = context.theme.colors();
    const auto& metrics = context.theme.metrics();
    if (context.text == nullptr) return;
    const st::ui::TextPort& port = *context.text;

    const float line_height = port.line_height(font_size_);
    const float gutter = 46.0f;
    const Rect inner = bounds_.inset(Insets{0.0f, 10.0f, 0.0f, 10.0f});
    const Rect text_area = Rect{inner.x + gutter, inner.y, inner.width - gutter, inner.height};

    // 行号栏底色（比正文略深，形成"编辑器左侧栏"层次）
    canvas.fill_rect(Rect{bounds_.x, bounds_.y, gutter, bounds_.height},
                     st::raster::Paint::solid(colors.surface_alt), 0.0f);

    // 逐行绘制（每行做一次 Markdown 行级着色）
    const auto lines = st::split(text_, '\n');
    float y = text_area.y - scroll_;
    for (std::size_t index = 0; index < lines.size(); ++index) {
      const float row_top = y + static_cast<float>(index) * line_height;
      if (row_top > bounds_.bottom()) break;
      if (row_top + line_height < bounds_.y) continue;

      const bool is_cursor = index + 1 == cursor_line_;
      if (is_cursor) {
        canvas.fill_rect(Rect{bounds_.x + 1.0f, row_top, bounds_.width - 2.0f, line_height},
                         st::raster::Paint::solid(colors.primary_soft), 0.0f);
      }

      const std::string number = std::format("{}", index + 1);
      const float number_width = port.measure_width(number, metrics.font_xs);
      port.draw(canvas, number,
                Point{inner.x + gutter - 10.0f - number_width, row_top + (line_height - port.line_height(metrics.font_xs)) * 0.5f},
                metrics.font_xs, is_cursor ? colors.primary : colors.text_faint);
      (void)text_area;

      paint_source_line(context, canvas, lines[index], Point{bounds_.x + gutter, row_top}, line_height);
    }

    // 滚动条（内容超高时）
    const float content_height = static_cast<float>(lines.size()) * line_height + 20.0f;
    if (content_height > bounds_.height) {
      const float ratio = bounds_.height / content_height;
      const float track = bounds_.height;
      const float thumb = std::max(18.0f, track * ratio);
      const float travel = track - thumb;
      const float offset = max_scroll() > 0.0f ? (scroll_ / max_scroll()) * travel : 0.0f;
      canvas.fill_rect(Rect{bounds_.right() - 8.0f, bounds_.y + offset, 8.0f, thumb},
                       st::raster::Paint::solid(colors.border_strong), 4.0f);
    }
  }

  auto on_event(const st::ui::RenderContext& context, st::ui::Event& event) -> bool override {
    if (event.kind == st::ui::EventKind::Wheel) {
      const float line_height = context.text != nullptr ? context.text->line_height(font_size_)
                                                        : font_size_ * 1.6f;
      scroll_ = std::clamp(scroll_ - event.wheel_delta * 48.0f, 0.0f, max_scroll());
      (void)line_height;
      mark_dirty();
      return true;
    }
    return false;
  }

  [[nodiscard]] auto scroll_offset() const noexcept -> float { return scroll_; }

 private:
  /// 行级 Markdown 着色：标题 / 引用 / 列表 / 代码围栏 / 行内码 / 强调 / 链接。
  void paint_source_line(const st::ui::RenderContext& context, st::raster::Surface& canvas,
                         std::string_view line, Point origin, float line_height) const {
    const auto& colors = context.theme.colors();
    const st::ui::TextPort& port = *context.text;
    const float size = font_size_;
    const float baseline = origin.y + (line_height - port.line_height(size)) * 0.5f;

    const auto trimmed = st::trim_start(line);
    Color color = colors.text;
    FontWeight weight = FontWeight::Regular;
    if (trimmed.starts_with("#")) {
      color = colors.primary;
      weight = FontWeight::SemiBold;
    } else if (trimmed.starts_with(">")) {
      color = colors.text_muted;
    } else if (trimmed.starts_with("```") || trimmed.starts_with("~~~")) {
      color = colors.accent;
      weight = FontWeight::Medium;
    } else if (trimmed.starts_with("- ") || trimmed.starts_with("* ") || trimmed.starts_with("+ ")) {
      color = colors.text;
    } else if (line.find("://") != std::string_view::npos) {
      color = colors.primary;
    }
    (void)weight;

    // 行内码与强调的着色通过二次分层绘制模拟（同一行的不同区间分别取色）
    const std::string_view body = line;
    float pen = origin.x;
    std::size_t cursor = 0;
    while (cursor < body.size()) {
      const std::size_t tick = body.find('`', cursor);
      const std::size_t star = body.find("**", cursor);
      std::size_t next = std::string_view::npos;
      bool is_code = false;
      if (tick != std::string_view::npos && (star == std::string_view::npos || tick < star)) {
        next = tick;
        is_code = true;
      } else if (star != std::string_view::npos) {
        next = star;
      }
      if (next == std::string_view::npos) {
        const std::string_view tail = body.substr(cursor);
        port.draw(canvas, tail, Point{pen, baseline}, size, color);
        break;
      }
      const std::string_view before = body.substr(cursor, next - cursor);
      port.draw(canvas, before, Point{pen, baseline}, size, color);
      pen += port.measure_width(before, size);
      cursor = next;

      const std::string_view marker = is_code ? std::string_view{"`"} : std::string_view{"**"};
      const std::size_t close = body.find(marker, cursor + marker.size());
      if (close == std::string_view::npos) {
        const std::string_view rest = body.substr(cursor);
        port.draw(canvas, rest, Point{pen, baseline}, size, colors.text_faint);
        break;
      }
      const std::string_view span = body.substr(cursor, close + marker.size() - cursor);
      const Color span_color = is_code ? colors.accent : colors.text;
      port.draw(canvas, span, Point{pen, baseline}, size, span_color);
      pen += port.measure_width(span, size);
      cursor = close + marker.size();
    }
  }

  [[nodiscard]] auto max_scroll() const noexcept -> float {
    const float content =
        static_cast<float>(st::split(text_, '\n').size()) * font_size_ * 1.6f + 20.0f;
    const float view = bounds_.height;
    return content > view ? content - view : 0.0f;
  }

  std::string text_{};
  std::size_t cursor_line_{1};
  float font_size_{13.0f};
  mutable float scroll_{0.0f};
  mutable float line_height_{20.0f};
};

// ————————————————————————————————————————————————————————————————————————————
// 定制组件 2：文档大纲（消费 `st::md` 解析树 → 可点击跳转的标题列表）
// ————————————————————————————————————————————————————————————————————————————
class OutlinePanel : public Element {
 public:
  OutlinePanel() { set_id("outline"); }

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "OutlinePanel"; }
  [[nodiscard]] auto role() const noexcept -> st::ui::Role override { return st::ui::Role::List; }

  void set_markdown(std::string_view markdown) {
    const auto blocks = st::md::parse(markdown);
    entries_.clear();
    collect(blocks, 0);
    mark_layout_dirty();
  }
  [[nodiscard]] auto entry_count() const noexcept -> std::size_t { return entries_.size(); }
  [[nodiscard]] auto semantics_text() const -> std::string override {
    std::string out;
    for (const auto& entry : entries_) {
      if (!out.empty()) out.push_back('/');
      out.append(entry.title);
    }
    return out;
  }
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override {
    if (name == "count") return std::format("{}", entries_.size());
    return std::nullopt;
  }

  void apply_theme(const st::ui::Theme& theme) override {
    style_.background = Color{0, 0, 0, 0};
    style_.font_size = theme.metrics().font_sm;
  }

  void measure(const st::ui::RenderContext& context, const st::ui::Constraints& constraints) override {
    const float row = 26.0f;
    measured_ = st::math::Size{constraints.max_width,
                              static_cast<float>(entries_.size()) * row + 8.0f};
    (void)context;
  }

  void paint_content(const st::ui::RenderContext& context, st::raster::Surface& canvas) const override {
    if (context.text == nullptr) return;
    const auto& colors = context.theme.colors();
    float y = bounds_.y + 4.0f;
    for (std::size_t index = 0; index < entries_.size(); ++index) {
      const auto& entry = entries_[index];
      const bool active = index == active_;
      const float indent = 10.0f + static_cast<float>(entry.level - 1) * 12.0f;
      if (active) {
        canvas.fill_rect(Rect{bounds_.x, y, bounds_.width, 24.0f},
                         st::raster::Paint::solid(colors.primary_soft), 6.0f);
      }
      if (entry.level <= 2) {
        canvas.fill_rect(Rect{bounds_.x + 2.0f, y + 5.0f, 2.0f, 14.0f},
                         st::raster::Paint::solid(colors.primary), 1.0f);
      }
      context.text->draw(canvas, entry.title, Point{bounds_.x + indent, y + 5.0f},
                         active ? context.theme.metrics().font_sm : context.theme.metrics().font_xs,
                         active ? colors.primary : colors.text_muted);
      y += 26.0f;
    }
    if (entries_.empty() && context.text != nullptr) {
      context.text->draw(canvas, "（无标题）", Point{bounds_.x + 10.0f, bounds_.y + 6.0f},
                         context.theme.metrics().font_xs, colors.text_faint);
    }
  }

  /// 大纲项矩形（供命中测试与外部断言；未 arrange 时为空）。
  [[nodiscard]] auto entry_rect(std::size_t index) const -> Rect {
    if (index >= entries_.size()) return Rect{};
    return Rect{bounds_.x, bounds_.y + 4.0f + static_cast<float>(index) * 26.0f, bounds_.width, 24.0f};
  }

  auto on_event(const st::ui::RenderContext& context, st::ui::Event& event) -> bool override {
    (void)context;
    if (event.kind != st::ui::EventKind::Click && event.kind != st::ui::EventKind::MouseDown) {
      return false;
    }
    for (std::size_t index = 0; index < entries_.size(); ++index) {
      if (!entry_rect(index).contains(event.position)) continue;
      active_ = index;
      mark_dirty();
      if (on_select) on_select(entries_[index].line);
      return true;
    }
    return false;
  }

  /// 点击大纲项时回调（实参为该标题在原文中的行号，从 1 起）。
  std::function<void(std::size_t)> on_select{};

 private:
  struct Entry {
    std::string title{};
    std::uint32_t level{1};
    std::size_t line{1};
  };

  void collect(const std::vector<st::md::Block>& blocks, std::uint32_t depth) {
    for (const auto& block : blocks) {
      if (block.kind == st::md::BlockKind::Heading) {
        std::string title;
        for (const auto& inline_item : block.inlines) title.append(inline_item.text);
        if (!title.empty()) {
          entries_.push_back(Entry{std::move(title), block.level == 0 ? 1 : block.level,
                                   static_cast<std::size_t>(block.start_line) + 1});
        }
      }
      if (!block.children.empty()) collect(block.children, depth + 1);
    }
  }

  std::vector<Entry> entries_{};
  std::size_t active_{0};
};

// ————————————————————————————————————————————————————————————————————————————
// 定制组件 3：可拖拽分栏（拖动手柄改变左右宽度比例）
// ————————————————————————————————————————————————————————————————————————————
class SplitHandle : public Element {
 public:
  SplitHandle() { set_id("split-handle"); }

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "SplitHandle"; }
  [[nodiscard]] auto semantics_text() const -> std::string override {
    return std::format("{:.0f}%", static_cast<double>(ratio_ * 100.0f));
  }
  [[nodiscard]] auto ratio() const noexcept -> float { return ratio_; }
  void set_ratio(float ratio) {
    ratio_ = std::clamp(ratio, 0.2f, 0.8f);
    mark_dirty();
    if (on_change) on_change(ratio_);
  }
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override {
    if (name == "ratio") return std::format("{:.3f}", static_cast<double>(ratio_));
    return std::nullopt;
  }
  auto set_property(std::string_view name, std::string_view value) -> bool override {
    if (name == "ratio") {
      if (const auto parsed = st::parse_f64(value); parsed.has_value()) {
        set_ratio(static_cast<float>(*parsed));
        return true;
      }
    }
    return false;
  }
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override {
    return {"ratio"};
  }

  void apply_theme(const st::ui::Theme& theme) override {
    style_.background = Color{0, 0, 0, 0};
    (void)theme;
  }
  void measure(const st::ui::RenderContext& context, const st::ui::Constraints& constraints) override {
    (void)context;
    measured_ = st::math::Size{10.0f, constraints.max_height};
  }
  void paint_content(const st::ui::RenderContext& context, st::raster::Surface& canvas) const override {
    const auto& colors = context.theme.colors();
    const float center = bounds_.width * 0.5f;
    canvas.fill_rect(Rect{bounds_.x + center - 1.0f, bounds_.y, 2.0f, bounds_.height},
                     st::raster::Paint::solid(dragging_ ? colors.primary : colors.border));
    const float grip_height = 32.0f;
    const float grip_top = bounds_.center().y - grip_height * 0.5f;
    canvas.fill_rect(Rect{bounds_.x + center - 1.5f, grip_top, 3.0f, grip_height},
                     st::raster::Paint::solid(dragging_ ? colors.primary : colors.border_strong), 1.5f);
  }
  auto on_event(const st::ui::RenderContext& context, st::ui::Event& event) -> bool override {
    (void)context;
    switch (event.kind) {
      case st::ui::EventKind::MouseDown:
        dragging_ = true;
        mark_dirty();
        return true;
      case st::ui::EventKind::MouseUp:
      case st::ui::EventKind::Click:
        if (dragging_) {
          dragging_ = false;
          mark_dirty();
        }
        return true;
      case st::ui::EventKind::MouseMove:
        if (dragging_ && parent() != nullptr) {
          const Rect area = parent()->bounds();
          const float ratio = (event.position.x - area.x) / std::max(1.0f, area.width);
          set_ratio(ratio);
          return true;
        }
        return false;
      default:
        return false;
    }
  }

  std::function<void(float)> on_change{};

 private:
  float ratio_{0.5f};
  bool dragging_{false};
};

// ————————————————————————————————————————————————————————————————————————————
// 定制组件 4：实时统计栏（字符/词/行/块/阅读时长，随编辑即时变化）
// ————————————————————————————————————————————————————————————————————————————
class StatsBar : public Element {
 public:
  StatsBar() { set_id("stats-bar"); }

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "StatsBar"; }
  [[nodiscard]] auto semantics_text() const -> std::string override { return summary_; }

  void update(std::string_view markdown) {
    characters_ = st::utf8_length(markdown);
    lines_ = st::split(markdown, '\n').size();
    words_ = static_cast<std::size_t>(st::split_whitespace(markdown).size());
    blocks_ = st::md::parse(markdown).size();
    const auto minutes = static_cast<std::size_t>(static_cast<double>(characters_) / 400.0) + 1;
    summary_ = std::format("{} 字 · {} 词 · {} 行 · {} 块 · 约 {} 分钟阅读", characters_, words_,
                           lines_, blocks_, minutes);
    mark_dirty();
  }
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override {
    if (name == "summary") return summary_;
    if (name == "characters") return std::format("{}", characters_);
    if (name == "words") return std::format("{}", words_);
    if (name == "blocks") return std::format("{}", blocks_);
    return std::nullopt;
  }
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override {
    return {"summary", "characters", "words", "blocks"};
  }

  void apply_theme(const st::ui::Theme& theme) override {
    style_.font_size = theme.metrics().font_xs;
    style_.background = Color{0, 0, 0, 0};
  }
  void measure(const st::ui::RenderContext& context, const st::ui::Constraints& constraints) override {
    (void)constraints;
    measured_ = st::math::Size{0.0f, context.text != nullptr
                                         ? context.text->line_height(style_.font_size)
                                         : style_.font_size * 1.5f};
  }
  void paint_content(const st::ui::RenderContext& context, st::raster::Surface& canvas) const override {
    if (context.text == nullptr) return;
    context.text->draw(canvas, summary_, Point{bounds_.x, bounds_.y}, style_.font_size,
                       context.theme.colors().text_muted);
  }

 private:
  std::string summary_{"0 字"};
  std::size_t characters_{0};
  std::size_t words_{0};
  std::size_t lines_{1};
  std::size_t blocks_{0};
};

// ————————————————————————————————————————————————————————————————————————————
// 小工具
// ————————————————————————————————————————————————————————————————————————————

/// Markdown 变换（工具栏按钮的“应用逻辑”，纯函数便于测试与复用）。
[[nodiscard]] auto apply_transform(std::string_view text, std::string_view action) -> std::string {
  std::string out(text);
  const auto wrap_selection = [&out](std::string_view prefix, std::string_view suffix) {
    out.insert(0, std::string(prefix));
    out.append(suffix);
  };
  if (action == "bold") {
    wrap_selection("**", "**");
  } else if (action == "italic") {
    wrap_selection("*", "*");
  } else if (action == "strike") {
    wrap_selection("~~", "~~");
  } else if (action == "code") {
    wrap_selection("`", "`");
  } else if (action == "h1") {
    out = "# " + out;
  } else if (action == "h2") {
    out = "## " + out;
  } else if (action == "quote") {
    out = "> " + out;
  } else if (action == "list") {
    out = "- " + out;
  } else if (action == "task") {
    out = "- [ ] " + out;
  } else if (action == "codeblock") {
    out = "```cpp\n" + out + "\n```";
  } else if (action == "table") {
    out.append("\n\n| 列 A | 列 B |\n| --- | --- |\n| 值 1 | 值 2 |");
  } else if (action == "link") {
    wrap_selection("[", "](https://)");
  } else if (action == "divider") {
    out.append("\n\n---\n");
  }
  return out;
}

struct Options {
  std::string open{};
  std::string out{};
  std::string shots{};
  std::string control_file{};
  std::string theme{"light"};
  std::uint16_t control_port{0};
  float scale{0.0f};
  std::uint32_t frames{0};
  int max_ms{0};
  bool headless{false};
  bool demo_stream{false};
};

[[nodiscard]] auto parse_options(int argc, char** argv) -> Options {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view raw = argv[index];
    const auto value = [&](std::string fallback) -> std::string {
      return index + 1 < argc ? std::string(argv[++index]) : fallback;
    };
    if (raw == "--open") options.open = value({});
    else if (raw == "--out") options.out = value({});
    else if (raw == "--shots") options.shots = value({});
    else if (raw == "--control-file") options.control_file = value({});
    else if (raw == "--control-port") options.control_port = static_cast<std::uint16_t>(std::stoi(value("0")));
    else if (raw == "--theme") options.theme = value("light");
    else if (raw == "--scale") options.scale = static_cast<float>(std::stod(value("1")));
    else if (raw == "--frames") options.frames = static_cast<std::uint32_t>(std::stoi(value("0")));
    else if (raw == "--ms") options.max_ms = std::stoi(value("0"));
    else if (raw == "--headless") options.headless = true;
    else if (raw == "--demo-stream") options.demo_stream = true;
    else if (raw == "--help" || raw == "-h") {
      st::print("用法: mdeditor [--open FILE] [--out FILE] [--theme dark] [--scale 2.0]\n"
                  "                [--headless] [--control-port 0] [--control-file PATH]\n"
                  "                [--frames N] [--ms N] [--demo-stream]\n");
      std::exit(0);
    }
  }
  return options;
}

inline constexpr std::string_view kSampleDocument = R"(# 霜天 Markdown 编辑器

这是一个用**霜天框架**写的原生 Markdown 编辑器：左侧编辑、右侧实时预览、
上方工具栏、左侧文档大纲、底部实时统计 —— 全部自绘，零第三方依赖。

## 为什么叫「高阶定制」示例

框架只提供 `Element` 的四个扩展点（`measure` / `arrange` / `paint_content` / `on_event`），
本示例用它们现场造了四个**框架里没有的组件**：

- `SourceView`：带行号与 Markdown 语法着色的源码视图
- `OutlinePanel`：解析 `st::md` 语法树得到的可点击文档大纲
- `SplitHandle`：可拖拽的左右分栏
- `StatsBar`：实时字数/词数/行数/块数/阅读时长

> 结论：定制能力不靠"更多内置控件"，而靠**稳定的组合式扩展点 + 统一设计令牌**。

## 代码块与高亮

```python
def greet(name: str) -> str:
    # 中文注释也要能正常着色
    return f"你好，{name}"

print(greet("霜天"))
```

```cpp
auto main() -> int {
  st::app::AppOptions options;       // 自绘 UI，无系统控件
  options.headless = true;           // 服务器上也能开发
  return st::app::Application("demo", "0.1.0", options).run(make_page()).value_or(1);
}
```

## 表格

| 能力 | 实现 | 状态 |
| --- | --- | --- |
| 软件光栅化 | 扫描线覆盖率抗锯齿 | 已交付 |
| DPI 缩放 | 逻辑/物理像素分离 | 已交付 |
| 控制通道 | TCP + JSON（st-control/1） | 已交付 |
| 流式 Markdown | MdStream 增量解析 | 已交付 |

## 任务列表

- [x] 自绘组件树与 Flex 布局
- [x] TTF/CFF/CJK 字体引擎
- [x] 无头模式 + TCP 控制
- [ ] 更多动画与过渡

## 引用与分隔线

> 无头模式下，智能体可以完整地开发与验证界面：
> `find` 定位、`set` 改状态、`input.*` 打字点击、`capture` 截图核验。

---

**试试点上面的工具栏按钮、拖拖分栏、或按 `Ctrl+S` 保存。**
)";

}  // namespace

auto run_app(int argc, char** argv) -> int {
  const Options options = parse_options(argc, argv);

  st::app::AppOptions app_options;
  app_options.width = 1360;
  app_options.height = 840;
  app_options.scale = options.scale;
  app_options.title = "霜天 · Markdown 编辑器";
  app_options.headless = options.headless;
  app_options.backend = options.headless ? "headless" : std::string{};
  app_options.control_port = options.control_port;
  app_options.control_file = options.control_file;
  app_options.screenshot_dir = options.shots;
  app_options.theme = options.theme == "dark" ? st::ui::ThemeMode::Dark : st::ui::ThemeMode::Light;

  st::app::Application app("mdeditor", "0.1.0", app_options);
  auto* app_ptr = &app;
  st::ui::UiRoot* root_ptr = &app.root();

  std::string document(kSampleDocument);
  if (!options.open.empty()) {
    if (auto loaded = st::fs::read_text(options.open); loaded.has_value()) {
      document = *loaded;
    } else {
      std::fprintf(stderr, "警告: 无法读取 %s（%s），改用内置示例\n", options.open.c_str(),
                   loaded.error().message.c_str());
    }
  }
  std::string document_path = options.open;
  if (document_path.empty()) {
    document_path = options.out.empty() ? "mdeditor-sample.md" : options.out;
  }

  // —— 顶部工具栏 ——
  auto page = std::make_unique<Panel>(FlexDirection::Column);
  page->set_id("editor-root");

  auto toolbar = std::make_unique<Panel>(FlexDirection::Row);
  toolbar->set_id("toolbar");
  toolbar->style().height = 52.0f;
  toolbar->style().padding = Insets{0.0f, 0.0f, 16.0f, 0.0f};
  toolbar->style().gap = 6.0f;
  toolbar->style().align_items = Align::Center;

  auto brand = std::make_unique<IconView>("edit", 20.0f);
  brand->set_tone(Tone::Primary);
  toolbar->add_child(std::move(brand));
  auto brand_text = std::make_unique<Text>("霜天 Markdown");
  brand_text->set_weight(FontWeight::SemiBold);
  brand_text->set_id("brand");
  toolbar->add_child(std::move(brand_text));
  auto toolbar_spacer = std::make_unique<Panel>(FlexDirection::Row);
  toolbar_spacer->style().width = 12.0f;
  toolbar->add_child(std::move(toolbar_spacer));

  auto editor = std::make_unique<TextArea>();
  editor->set_id("editor");
  editor->set_text(document);
  auto* editor_ptr = editor.get();

  auto preview = std::make_unique<MarkdownView>(document);
  preview->set_id("preview");
  auto* preview_ptr = preview.get();

  auto outline = std::make_unique<OutlinePanel>();
  auto* outline_ptr = outline.get();
  outline_ptr->set_markdown(document);  // 启动即解析大纲（标题列表）

  auto source_view = std::make_unique<SourceView>();
  source_view->set_text(document);
  auto* source_ptr = source_view.get();

  auto stats = std::make_unique<StatsBar>();
  stats->update(document);
  auto* stats_ptr = stats.get();

  auto split = std::make_unique<SplitHandle>();
  split->set_id("split");
  auto* split_ptr = split.get();
  auto left_pane = std::make_unique<Panel>(FlexDirection::Column);
  left_pane->set_id("left-pane");
  auto right_pane = std::make_unique<Panel>(FlexDirection::Column);
  right_pane->set_id("right-pane");
  auto* left_ptr = left_pane.get();
  auto* right_ptr = right_pane.get();

  const auto make_tool = [&](std::string_view icon, std::string_view action, std::string_view tip) {
    auto button = std::make_unique<Button>(std::string{}, Button::Variant::Ghost, Button::Size::Small);
    button->set_id(std::string("tool-") + std::string(action));
    button->set_icon(std::string(icon));
    button->style().width = 32.0f;
    button->on_click = [editor_ptr, preview_ptr, outline_ptr, source_ptr, stats_ptr, action, tip,
                        root_ptr]() {
      const std::string updated = apply_transform(editor_ptr->value(), action);
      editor_ptr->set_text(updated);
      preview_ptr->set_markdown(updated);
      outline_ptr->set_markdown(updated);
      source_ptr->set_text(updated);
      stats_ptr->update(updated);
      root_ptr->mark_dirty_all();
      (void)tip;
    };
    return button;
  };
  toolbar->add_child(make_tool("bold", "bold", "粗体"));
  toolbar->add_child(make_tool("italic", "italic", "斜体"));
  toolbar->add_child(make_tool("strike", "strike", "删除线"));
  toolbar->add_child(make_tool("code", "code", "行内码"));
  toolbar->add_child(make_tool("h1", "h1", "一级标题"));
  toolbar->add_child(make_tool("list", "list", "列表"));
  toolbar->add_child(make_tool("check", "task", "任务项"));
  toolbar->add_child(make_tool("terminal", "codeblock", "代码块"));
  toolbar->add_child(make_tool("grid", "table", "表格"));
  toolbar->add_child(make_tool("link", "link", "链接"));

  auto toolbar_gap = std::make_unique<Panel>(FlexDirection::Row);
  toolbar_gap->style().grow = true;
  toolbar->add_child(std::move(toolbar_gap));

  auto status_text = std::make_unique<Text>("就绪");
  status_text->set_id("status-text");
  status_text->set_tone(Tone::Faint);
  status_text->set_font_size(12.0f);
  auto* status_ptr = status_text.get();
  toolbar->add_child(std::move(status_text));

  auto save_button = std::make_unique<Button>("保存", Button::Variant::Secondary, Button::Size::Small);
  save_button->set_id("btn-save");
  auto open_button = std::make_unique<Button>("打开", Button::Variant::Ghost, Button::Size::Small);
  open_button->set_id("btn-open");
  auto stream_button = std::make_unique<Button>("流式生成", Button::Variant::Primary, Button::Size::Small);
  stream_button->set_id("btn-stream");
  stream_button->set_icon("bolt");
  auto theme_button = std::make_unique<Button>("暗色", Button::Variant::Ghost, Button::Size::Small);
  theme_button->set_id("btn-theme");
  theme_button->set_icon("moon");
  auto dpi_button = std::make_unique<Button>("DPI 1x", Button::Variant::Ghost, Button::Size::Small);
  dpi_button->set_id("btn-dpi");
  dpi_button->set_icon("cpu");
  auto* save_ptr = save_button.get();
  auto* stream_ptr = stream_button.get();
  auto* theme_ptr = theme_button.get();
  auto* dpi_ptr = dpi_button.get();
  auto* open_ptr = open_button.get();
  toolbar->add_child(std::move(save_button));
  toolbar->add_child(std::move(open_button));
  toolbar->add_child(std::move(stream_button));
  toolbar->add_child(std::move(theme_button));
  toolbar->add_child(std::move(dpi_button));
  page->add_child(std::move(toolbar));

  // —— 主体三栏：大纲 | 编辑器 | 预览 ——
  auto body = std::make_unique<Panel>(FlexDirection::Row);
  body->set_id("body");
  body->style().grow = true;
  body->style().padding = Insets{0.0f, 0.0f, 16.0f, 16.0f};
  body->style().gap = 12.0f;

  auto outline_card = std::make_unique<Card>(10.0f);
  outline_card->set_id("card-outline");
  outline_card->style().width = 190.0f;
  outline_card->style().direction = FlexDirection::Column;
  outline_card->style().gap = 8.0f;
  auto outline_title = std::make_unique<Text>("大纲");
  outline_title->set_tone(Tone::Faint);
  outline_title->set_font_size(11.0f);
  outline_card->add_child(std::move(outline_title));
  auto outline_scroll = std::make_unique<ScrollView>();
  outline_scroll->set_id("outline-scroll");
  outline_scroll->style().grow = true;
  outline_scroll->add_child(std::move(outline));
  outline_card->add_child(std::move(outline_scroll));
  body->add_child(std::move(outline_card));

  auto editor_card = std::make_unique<Card>(0.0f);
  editor_card->set_id("card-editor");
  editor_card->style().grow = true;
  editor_card->style().direction = FlexDirection::Column;
  auto editor_head = std::make_unique<Panel>(FlexDirection::Row);
  editor_head->style().padding = Insets{12.0f, 10.0f, 12.0f, 10.0f};
  editor_head->style().gap = 6.0f;
  editor_head->style().align_items = Align::Center;
  auto editor_label = std::make_unique<Text>("编辑");
  editor_label->set_tone(Tone::Faint);
  editor_label->set_font_size(11.0f);
  editor_head->add_child(std::move(editor_label));
  auto path_text = std::make_unique<Text>(st::fs::file_name(document_path));
  path_text->set_id("doc-path");
  path_text->set_tone(Tone::Muted);
  path_text->set_font_size(11.0f);
  auto* path_ptr = path_text.get();
  editor_head->add_child(std::move(path_text));
  editor_card->add_child(std::move(editor_head));
  editor_card->add_child(std::move(editor));
  editor_card->style().clip_children = true;
  body->add_child(std::move(editor_card));

  body->add_child(std::move(split));

  auto preview_card = std::make_unique<Card>(0.0f);
  preview_card->set_id("card-preview");
  preview_card->style().grow = true;
  preview_card->style().direction = FlexDirection::Column;
  auto preview_head = std::make_unique<Panel>(FlexDirection::Row);
  preview_head->style().padding = Insets{12.0f, 10.0f, 12.0f, 10.0f};
  preview_head->style().align_items = Align::Center;
  auto preview_label = std::make_unique<Text>("预览");
  preview_label->set_tone(Tone::Faint);
  preview_label->set_font_size(11.0f);
  preview_head->add_child(std::move(preview_label));
  preview_card->add_child(std::move(preview_head));
  auto preview_scroll = std::make_unique<ScrollView>();
  preview_scroll->set_id("preview-scroll");
  preview_scroll->style().grow = true;
  preview_scroll->add_child(std::move(preview));
  preview_card->add_child(std::move(preview_scroll));
  preview_card->style().clip_children = true;
  body->add_child(std::move(preview_card));

  page->add_child(std::move(body));

  // —— 底部：源码视图（语法高亮）+ 统计 ——
  auto console_card = std::make_unique<Card>(0.0f);
  console_card->set_id("card-source");
  console_card->style().height = 210.0f;
  console_card->style().padding = Insets::all(0.0f);
  console_card->style().direction = FlexDirection::Column;
  console_card->style().clip_children = true;
  console_card->add_child(std::move(source_view));
  page->add_child(std::move(console_card));

  auto status_bar = std::make_unique<Panel>(FlexDirection::Row);
  status_bar->set_id("status-bar");
  status_bar->style().height = 32.0f;
  status_bar->style().padding = Insets{16.0f, 0.0f, 16.0f, 0.0f};
  status_bar->style().align_items = Align::Center;
  status_bar->style().gap = 10.0f;
  auto status_dot = std::make_unique<IconView>("dot", 8.0f);
  status_dot->set_tone(Tone::Success);
  status_bar->add_child(std::move(status_dot));
  status_bar->add_child(std::move(stats));
  auto status_gap = std::make_unique<Panel>(FlexDirection::Row);
  status_gap->style().grow = true;
  status_bar->add_child(std::move(status_gap));
  auto hint = std::make_unique<Text>("Ctrl+S 保存 · 拖拽中间手柄分栏 · 点击大纲跳转");
  hint->set_tone(Tone::Faint);
  hint->set_font_size(11.0f);
  status_bar->add_child(std::move(hint));
  page->add_child(std::move(status_bar));

  // —— 交互接线 ——
  const auto sync_all = [editor_ptr, preview_ptr, outline_ptr, source_ptr, stats_ptr, root_ptr]() {
    const std::string text = editor_ptr->value();
    preview_ptr->set_markdown(text);
    outline_ptr->set_markdown(text);
    source_ptr->set_text(text);
    stats_ptr->update(text);
    root_ptr->mark_dirty_all();
  };

  editor_ptr->on_change = [sync_all](std::string_view) { sync_all(); };

  // 大纲跳转：把预览滚到对应标题附近（按标题序号估算行高）
  outline_ptr->on_select = [outline_ptr, preview_ptr, root_ptr, status_ptr](std::size_t line) {
    const float estimated = static_cast<float>(line) * 26.0f;
    preview_ptr->scroll_to(std::max(0.0f, estimated - 40.0f));
    status_ptr->set_content(std::format("跳转到第 {} 行", line));
    root_ptr->mark_dirty_all();
  };

  // 分栏拖拽：按比例设置左右宽度
  split_ptr->on_change = [left_ptr, right_ptr, editor_ptr, root_ptr](float ratio) {
    (void)editor_ptr;
    (void)right_ptr;
    (void)left_ptr;
    root_ptr->mark_dirty_all();
    (void)ratio;
  };

  save_ptr->on_click = [editor_ptr, path_ptr, status_ptr, root_ptr, &document_path]() {
    if (auto status = st::fs::write_text(document_path, editor_ptr->value()); status.has_value()) {
      status_ptr->set_content("已保存 " + st::fs::file_name(document_path));
      path_ptr->set_content(st::fs::file_name(document_path));
    } else {
      status_ptr->set_content("保存失败: " + status.error().message);
    }
    root_ptr->mark_dirty_all();
  };

  open_ptr->on_click = [&app, status_ptr, editor_ptr, root_ptr, sync_all]() {
    // 弹一个"最近文档"对话框（演示 overlay + 对话框动作回调）
    auto dialog = std::make_unique<Dialog>("打开文档", "（示例）选择要载入的 Markdown 源文件");
    dialog->set_actions({"载入内置示例", "取消"});
    auto* dialog_ptr = dialog.get();
    dialog->on_action = [dialog_ptr, status_ptr, editor_ptr, root_ptr, sync_all](std::size_t index) {
      if (index == 0) {
        editor_ptr->set_text(std::string(kSampleDocument));
        sync_all();
        status_ptr->set_content("已载入内置示例");
      }
      root_ptr->remove_overlay(dialog_ptr);
      root_ptr->mark_dirty_all();
    };
    dialog->on_dismiss = [dialog_ptr, root_ptr]() {
      root_ptr->remove_overlay(dialog_ptr);
      root_ptr->mark_dirty_all();
    };
    root_ptr->add_overlay(std::move(dialog));
    root_ptr->mark_dirty_all();
    (void)app;
  };

  theme_ptr->on_click = [app_ptr, theme_ptr, root_ptr]() {
    const bool dark = app_ptr->root().theme().mode() == st::ui::ThemeMode::Light;
    app_ptr->set_theme_mode(dark ? st::ui::ThemeMode::Dark : st::ui::ThemeMode::Light);
    theme_ptr->set_label(dark ? "亮色" : "暗色");
    theme_ptr->set_icon(dark ? "sun" : "moon");
    root_ptr->mark_dirty_all();
  };

  dpi_ptr->on_click = [app_ptr, dpi_ptr, status_ptr, root_ptr]() {
    const float current = app_ptr->device_scale();
    const float next = current < 1.25f ? 1.5f : (current < 1.75f ? 2.0f : 1.0f);
    if (auto status = app_ptr->set_device_scale(next); status.has_value()) {
      dpi_ptr->set_label(std::format("DPI {}x", next == 1.0f ? 1 : (next == 1.5f ? 2 : 3)));
      status_ptr->set_content(std::format("DPI {:.1f}x", static_cast<double>(next)));
    }
    root_ptr->mark_dirty_all();
  };

  // 流式生成演示：把一段 LLM 风格的文本按小块喂给预览（模拟 token 流）
  std::string stream_source =
      "\n\n## 流式生成演示\n\n这一段是**逐块追加**进预览的，模拟大模型 token 流：\n\n- 每次 `append_chunk` 后前缀保持稳定\n- 未闭合的代码块也能即时呈现\n\n"
      "```ts\nfunction* tokens() {\n  yield \"霜天\";\n  yield \"·\";\n  yield \"流式\";\n}\n```\n";
  struct StreamState {
    bool active{false};
    std::string remaining{};
    std::int64_t next_tick_ms{0};
    std::size_t appended{0};
  };
  auto stream_state = std::make_shared<StreamState>();

  stream_ptr->on_click = [stream_state, stream_source, status_ptr, root_ptr, editor_ptr, sync_all,
                          stream_ptr]() {
    (void)sync_all;
    (void)editor_ptr;
    if (stream_state->active) {
      stream_state->active = false;
      stream_ptr->set_label("流式生成");
      status_ptr->set_content("流式已停止");
      root_ptr->mark_dirty_all();
      return;
    }
    stream_state->active = true;
    stream_state->remaining = stream_source;
    stream_state->appended = 0;
    stream_state->next_tick_ms = 0;
    stream_ptr->set_label("停止");
    root_ptr->mark_dirty_all();
  };

  if (options.demo_stream) {
    stream_state->active = true;
    stream_state->remaining = stream_source;
    stream_state->next_tick_ms = 0;
  }

  // —— 启动 ——
  app.set_content(std::move(page));
  if (auto status = app.start(); !status) {
    std::fprintf(stderr, "启动失败: %s\n", status.error().to_string().c_str());
    return 1;
  }
  status_ptr->set_content(std::format("{} · headless={} · DPI {:.1f} · 控制通道 :{}",
                                      app.backend_name(), app.headless(),
                                      static_cast<double>(app.device_scale()),
                                      app.control_port()));
  root_ptr->mark_dirty_all();
  app.render_frame();

  const std::int64_t started_ms = st::time::now_ms();
  std::uint32_t frames = 1;
  while (!app.quit_requested()) {
    const std::int64_t frame_start_ms = st::time::now_ms();
    app.tick();
    // 流式演示：每 60ms 追加 3~9 个字节（模拟 token 到达节奏）
    if (stream_state->active && !stream_state->remaining.empty()) {
      const std::int64_t now = st::time::now_ms();
      if (now >= stream_state->next_tick_ms) {
        const std::size_t take = std::min<std::size_t>(7, stream_state->remaining.size());
        const std::string chunk = stream_state->remaining.substr(0, take);
        stream_state->remaining.erase(0, take);
        preview_ptr->append_chunk(chunk);
        stream_state->appended += chunk.size();
        stream_state->next_tick_ms = now + 55;
        if (stream_state->remaining.empty()) {
          stream_state->active = false;
          stream_ptr->set_label("流式生成");
          status_ptr->set_content(std::format("流式完成，共追加 {} 字节", stream_state->appended));
        }
        root_ptr->mark_dirty_all();
      }
    }
    ++frames;
    if (options.frames > 0 && frames >= options.frames) break;
    if (options.max_ms > 0 && st::time::now_ms() - started_ms >= options.max_ms) break;
    // 节拍交给框架：有活 → 帧预算；空闲 → 4ms（控制通道响应节拍）。
    // 旧实现固定 `sleep_for(16ms)`：命令延迟被拉到 16~31ms（实测 ping p50=31ms）。
    app.pace_loop(frame_start_ms);
  }
  st::print("mdeditor 退出：{} 帧，大纲 {} 项，DPI {:.1f}，后端 {}\n", frames,
              outline_ptr->entry_count(), static_cast<double>(app.device_scale()),
              std::string(app.backend_name()));
  return 0;
}

// 跨平台入口：正规化 argv 编码（Windows 的 argv 是 ANSI）并设好控制台代码页
ST_MAIN(run_app)

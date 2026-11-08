#include "st/ui/components/tag_input.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "st/core/string.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/ui/icon.hpp"
#include "st/ui/line_layout.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"

#include "components_internal.hpp"

namespace st::ui {

using components_internal::fill_round_rect;
using components_internal::paint_outline;
using components_internal::text_port_of;

namespace {

constexpr float kTagRadius = 4.0f;
constexpr float kCloseGlyph = 9.0f;

}  // namespace

TagInput::TagInput() = default;

// ————————————————————————————————————————————————————————————————————————————
// 标签
// ————————————————————————————————————————————————————————————————————————————

auto TagInput::add_tag(std::string tag) -> bool {
  std::string trimmed(st::trim(tag));
  if (trimmed.empty()) return false;
  // 去重：同值不重复加（标签集合的语义就是集合）。回调告知调用方，便于提示。
  if (std::find(tags_.begin(), tags_.end(), trimmed) != tags_.end()) {
    if (on_duplicate) on_duplicate(trimmed);
    return false;
  }
  if (max_tags_ > 0U && tags_.size() >= max_tags_) return false;
  tags_.push_back(std::move(trimmed));
  mark_layout_dirty();
  notify_change();
  return true;
}

auto TagInput::remove_tag(std::size_t index) -> bool {
  if (index >= tags_.size()) return false;
  tags_.erase(tags_.begin() + static_cast<std::ptrdiff_t>(index));
  mark_layout_dirty();
  notify_change();
  return true;
}

auto TagInput::remove_tag_value(std::string_view tag) -> bool {
  const auto before = tags_.size();
  std::erase_if(tags_, [tag](const std::string& item) { return item == tag; });
  if (tags_.size() == before) return false;
  mark_layout_dirty();
  notify_change();
  return true;
}

void TagInput::clear_tags() {
  if (tags_.empty()) return;
  tags_.clear();
  mark_layout_dirty();
  notify_change();
}

void TagInput::set_tags(std::vector<std::string> tags) {
  tags_.clear();
  for (auto& tag : tags) {
    const std::string trimmed(st::trim(tag));
    if (trimmed.empty()) continue;
    if (std::find(tags_.begin(), tags_.end(), trimmed) != tags_.end()) continue;
    tags_.push_back(trimmed);
  }
  mark_layout_dirty();
  notify_change();
}

void TagInput::set_max_tags(std::size_t max_tags) {
  if (max_tags_ == max_tags) return;
  max_tags_ = max_tags;
  mark_layout_dirty();
}

void TagInput::set_addable(bool addable) {
  if (addable_ == addable) return;
  addable_ = addable;
  mark_layout_dirty();
}

void TagInput::set_placeholder(std::string placeholder) {
  if (placeholder_ == placeholder) return;
  placeholder_ = std::move(placeholder);
  mark_layout_dirty();
}

void TagInput::set_draft(std::string draft) {
  if (draft_ == draft) return;
  draft_ = std::move(draft);
  mark_layout_dirty();
}

auto TagInput::commit_draft() -> bool {
  if (draft_.empty()) return false;
  const std::string pending = draft_;
  if (!add_tag(pending)) return false;
  draft_.clear();
  mark_layout_dirty();
  return true;
}

void TagInput::notify_change() {
  if (on_change) on_change(tags_);
}

// ————————————————————————————————————————————————————————————————————————————
// 主题、度量与落位
// ————————————————————————————————————————————————————————————————————————————

void TagInput::apply_theme(const Theme& theme) {
  const Palette& colors = theme.colors();
  const Metrics& metrics = theme.metrics();
  style_.background = colors.surface;
  style_.border_color = colors.border;
  style_.border_width = 1.0f;
  style_.radius = kRadius;
  style_.font_size = metrics.font_xs;
  style_.color = colors.text;
  font_size_ = style_.font_size;
  apply_text_overrides();
  font_size_ = style_.font_size;
}

auto TagInput::tag_width(const RenderContext& context, std::string_view tag) const -> float {
  const TextPort& port = text_port_of(context);
  return port.measure_width(tag, font_size_) + kTagPaddingX * 2.0f + kCloseWidth;
}

void TagInput::measure(const RenderContext& context, const Constraints& constraints) {
  const TextPort& port = text_port_of(context);
  float width = style_.has_explicit_width() ? style_.width
                                            : (constraints.max_width < kUnbounded
                                                   ? constraints.max_width
                                                   : 320.0f);
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
  width = std::clamp(width, style_.min_width, style_.max_width);

  // 按宽度试排一遍算行数（与 `relayout` 同一套折行规则）。
  const float usable = std::max(width - kPadding * 2.0f, 1.0f);
  float x = 0.0f;
  int rows = 1;
  for (const auto& tag : tags_) {
    const float item = tag_width(context, tag);
    if (x > 0.0f && x + kTagGap + item > usable) {
      x = item;
      ++rows;
    } else {
      x = x == 0.0f ? item : x + kTagGap + item;
    }
  }
  // 输入位（有 draft 时按文本宽，否则最小宽）——它也要能折行。
  const float draft_width =
      std::max(port.measure_width(draft_, font_size_), kMinDraftWidth);
  if (x > 0.0f && x + kTagGap + draft_width > usable) ++rows;

  float height = style_.has_explicit_height()
                     ? style_.height
                     : static_cast<float>(rows) * kTagHeight +
                           static_cast<float>(rows - 1) * kRowGap + kPadding * 2.0f;
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  height = std::clamp(height, style_.min_height, style_.max_height);
  measured_ = math::Size{width, height};
}

void TagInput::relayout(const RenderContext& context, math::Rect rect) const {
  tag_boxes_.assign(tags_.size(), math::Rect{});
  close_boxes_.assign(tags_.size(), math::Rect{});
  draft_box_ = math::Rect{};
  if (rect.is_empty()) return;

  const float left = rect.x + kPadding;
  const float usable = std::max(rect.width - kPadding * 2.0f, 1.0f);
  float x = 0.0f;
  float y = rect.y + kPadding;

  for (std::size_t i = 0; i < tags_.size(); ++i) {
    const float item = tag_width(context, tags_[i]);
    if (x > 0.0f && x + kTagGap + item > usable) {
      x = 0.0f;
      y += kTagHeight + kRowGap;
    }
    const float origin_x = left + (x > 0.0f ? x + kTagGap : 0.0f);
    tag_boxes_[i] = math::Rect{origin_x, y, item, kTagHeight};
    close_boxes_[i] = math::Rect{origin_x + item - kCloseWidth - kTagPaddingX,
                                 y + (kTagHeight - kCloseWidth) * 0.5f, kCloseWidth, kCloseWidth};
    x = (x > 0.0f ? x + kTagGap : 0.0f) + item;
  }

  // 输入位：跟着最后一个标签在同一行末尾；放不下就折行。
  const TextPort& port = text_port_of(context);
  const float draft_width =
      std::min(std::max(port.measure_width(draft_, font_size_), kMinDraftWidth), usable);
  float draft_x = left + (x > 0.0f ? x + kTagGap : 0.0f);
  if (x > 0.0f && x + kTagGap + draft_width > usable) {
    draft_x = left;
    y += kTagHeight + kRowGap;
  }
  draft_box_ = math::Rect{draft_x, y, draft_width, kTagHeight};
}

void TagInput::arrange(const RenderContext& context, math::Rect rect) {
  Element::arrange(context, rect);
  relayout(context, rect);
}

auto TagInput::tag_rect(std::size_t index) const -> math::Rect {
  return index < tag_boxes_.size() ? tag_boxes_[index] : math::Rect{};
}

auto TagInput::close_rect(std::size_t index) const -> math::Rect {
  return index < close_boxes_.size() ? close_boxes_[index] : math::Rect{};
}

// ————————————————————————————————————————————————————————————————————————————
// 绘制
// ————————————————————————————————————————————————————————————————————————————

void TagInput::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  relayout(context, bounds_);
  const Palette& colors = context.theme.colors();
  const TextPort& port = text_port_of(context);

  for (std::size_t i = 0; i < tags_.size() && i < tag_boxes_.size(); ++i) {
    const math::Rect tag = tag_boxes_[i];
    if (tag.is_empty()) continue;
    if (tag.bottom() < bounds_.y || tag.y > bounds_.bottom()) continue;
    const bool hover = static_cast<int>(i) == hover_tag_;
    fill_round_rect(canvas, tag, kTagRadius,
                    hover ? colors.primary_soft : colors.surface_alt);
    const float text_width = tag.width - kTagPaddingX * 2.0f - kCloseWidth;
    const std::string shown =
        port.ellipsize(tags_[i], font_size_, std::max(text_width, 0.0f));
    const float top = centered_line_top(port, shown, font_size_, tag.y, tag.height);
    port.draw(canvas, shown, math::Point{tag.x + kTagPaddingX, top}, font_size_, colors.text);
    // 关闭 ×（只有可编辑时才有意义）
    if (addable_ && i < close_boxes_.size() && !close_boxes_[i].is_empty()) {
      const auto& close = close_boxes_[i];
      const bool close_hover = static_cast<int>(i) == hover_close_;
      const math::Rect glyph{close.center().x - kCloseGlyph * 0.5f,
                             close.center().y - kCloseGlyph * 0.5f, kCloseGlyph, kCloseGlyph};
      Icon::draw(canvas, "close", glyph, close_hover ? colors.danger : colors.text_faint, 1.4f);
    }
  }

  if (!draft_box_.is_empty() && addable_) {
    const bool show_placeholder = draft_.empty();
    const std::string shown = show_placeholder ? placeholder_ : draft_;
    const float text_width = port.measure_width(shown, font_size_);
    const float top =
        centered_line_top(port, shown, font_size_, draft_box_.y, draft_box_.height);
    // 占位符宽度不受 draft_box 限制（它只是提示，超出时按可用宽裁）。
    const std::string visible =
        port.ellipsize(shown, font_size_, std::max(bounds_.right() - draft_box_.x - kPadding, 10.0f));
    port.draw(canvas, visible, math::Point{draft_box_.x, top}, font_size_,
              show_placeholder ? colors.text_faint : colors.text);
    (void)text_width;

    if (focused_ && enabled_) {
      if (std::fmod(context.time_seconds, 1.0) < 0.5) {
        const float caret = draft_box_.x + port.measure_width(draft_, font_size_);
        canvas.fill_rect(math::Rect{caret, draft_box_.y + 4.0f, 1.0f, draft_box_.height - 8.0f},
                         raster::Paint::solid(colors.text));
      }
      request_animation();
    }
  }
}

// ————————————————————————————————————————————————————————————————————————————
// 交互
// ————————————————————————————————————————————————————————————————————————————

void TagInput::activate() {
  if (!enabled_) return;
  commit_draft();
}

auto TagInput::handle_key(const Event& event) -> bool {
  // 回车/逗号提交（逗号是标签输入的通行快捷键）。
  if (event.key == "Enter" || event.key == ",") {
    if (!addable_) return false;
    commit_draft();
    return true;
  }
  if (event.key == "Backspace") {
    if (!draft_.empty()) {
      // 有草稿：删草稿的最后一个码点。
      const std::size_t begin = st::utf8_prev(draft_, draft_.size());
      draft_.erase(begin);
      mark_layout_dirty();
      return true;
    }
    // 空草稿：删最后一个标签（标签输入的肌肉记忆）。
    if (!tags_.empty() && addable_) {
      remove_tag(tags_.size() - 1U);
      return true;
    }
    return false;
  }
  return false;
}

void TagInput::insert_text(std::string_view inserted) {
  if (!addable_ || inserted.empty()) return;
  // 输入里带逗号 → 顺带提交（粘贴 "a,b,c" 时能一次加多个）。
  if (inserted.find(',') != std::string_view::npos) {
    for (const auto& part : st::split(inserted, ',')) {
      const std::string trimmed(st::trim(part));
      if (trimmed.empty()) continue;
      draft_ = trimmed;
      commit_draft();
    }
    return;
  }
  draft_.append(inserted);
  mark_layout_dirty();
}

auto TagInput::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  if (!enabled_) return false;
  switch (event.kind) {
    case EventKind::HoverIn:
    case EventKind::HoverOut:
      hover_tag_ = -1;
      hover_close_ = -1;
      mark_dirty();
      return false;
    case EventKind::MouseMove: {
      int tag_found = -1;
      int close_found = -1;
      for (std::size_t i = 0; i < tag_boxes_.size(); ++i) {
        if (tag_boxes_[i].contains(event.position)) {
          tag_found = static_cast<int>(i);
          if (i < close_boxes_.size() && close_boxes_[i].contains(event.position)) {
            close_found = static_cast<int>(i);
          }
          break;
        }
      }
      if (tag_found != hover_tag_ || close_found != hover_close_) {
        hover_tag_ = tag_found;
        hover_close_ = close_found;
        mark_dirty();
      }
      return false;
    }
    case EventKind::Click:
    case EventKind::DoubleClick:
    case EventKind::TripleClick: {
      for (std::size_t i = 0; i < close_boxes_.size(); ++i) {
        if (close_boxes_[i].is_empty() || !close_boxes_[i].contains(event.position)) continue;
        remove_tag(i);
        event.handled = true;
        return true;
      }
      return false;
    }
    case EventKind::TextInput: {
      insert_text(event.text);
      event.handled = true;
      return true;
    }
    case EventKind::KeyDown: return handle_key(event);
    default: return false;
  }
}

auto TagInput::hit_test(math::Point point) const noexcept -> bool {
  return bounds_.contains(point);
}

auto TagInput::cursor_at(math::Point point) const noexcept -> MouseCursor {
  for (const auto& close : close_boxes_) {
    if (!close.is_empty() && close.contains(point)) return MouseCursor::Hand;
  }
  return MouseCursor::Text;
}

auto TagInput::semantics_text() const -> std::string {
  std::string out;
  for (std::size_t i = 0; i < tags_.size(); ++i) {
    if (i > 0) out += ", ";
    out += tags_[i];
  }
  return out;
}

auto TagInput::semantics_value() const -> std::string { return std::to_string(tags_.size()); }

auto TagInput::semantics_flags() const -> SemanticsFlags {
  SemanticsFlags flags = Element::semantics_flags();
  flags.editable = true;
  return flags;
}

// ————————————————————————————————————————————————————————————————————————————
// 属性面
// ————————————————————————————————————————————————————————————————————————————

auto TagInput::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "tags" || name == "items") {
    std::string out;
    for (std::size_t i = 0; i < tags_.size(); ++i) {
      if (i > 0) out += '|';
      out += tags_[i];
    }
    return out;
  }
  if (name == "draft" || name == "value") return draft_;
  if (name == "placeholder") return placeholder_;
  if (name == "max" || name == "max_tags") return std::to_string(max_tags_);
  if (name == "addable") return std::string(addable_ ? kFlagTrue : "false");
  if (name == "count") return std::to_string(tags_.size());
  return std::nullopt;
}

auto TagInput::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "tags" || name == "items") {
    std::vector<std::string> next;
    for (const auto& part : st::split(value, '|')) next.emplace_back(part);
    set_tags(std::move(next));
    return true;
  }
  if (name == "draft" || name == "value") {
    set_draft(std::string(value));
    return true;
  }
  if (name == "placeholder") {
    set_placeholder(std::string(value));
    return true;
  }
  if (name == "max" || name == "max_tags") {
    const auto parsed = st::parse_u64(st::trim(value));
    if (!parsed.has_value()) return false;
    set_max_tags(static_cast<std::size_t>(*parsed));
    return true;
  }
  if (name == "addable") {
    const auto parsed = st::parse_bool(value);
    if (!parsed.has_value()) return false;
    set_addable(*parsed);
    return true;
  }
  return false;
}

auto TagInput::property_names() const -> std::vector<std::string_view> {
  return {"tags", "draft", "placeholder", "max", "addable"};
}

auto TagInput::readonly_property_names() const -> std::vector<std::string_view> {
  return {"count"};
}

auto TagInput::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "add" || action == "append") {
    return add_tag(std::string(argument));
  }
  if (action == "remove" || action == "delete") {
    const std::string target(st::trim(argument));
    if (target.empty()) return false;
    if (remove_tag_value(target)) return true;
    const auto parsed = st::parse_u64(target);
    if (parsed.has_value()) return remove_tag(static_cast<std::size_t>(*parsed));
    return false;
  }
  if (action == "clear") {
    clear_tags();
    return true;
  }
  if (action == "commit" || action == "submit") {
    return commit_draft();
  }
  return Element::invoke_action(action, argument);
}

}  // namespace st::ui

// ── 组件登记（`st/ui/component_registry.hpp`）────────────────────────
ST_UI_COMPONENT_IMPL(::st::ui::TagInput);

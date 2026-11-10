#include "st/ui/components/avatar_group.hpp"

#include <algorithm>
#include <format>
#include <utility>

#include "st/core/string.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/ui/line_layout.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"

#include "components_internal.hpp"

namespace st::ui {

using components_internal::fill_round_rect;
using components_internal::text_port_of;

AvatarGroup::AvatarGroup() = default;

// ————————————————————————————————————————————————————————————————————————————
// 数据
// ————————————————————————————————————————————————————————————————————————————

void AvatarGroup::set_members(std::vector<std::string> names) {
  names_ = std::move(names);
  mark_layout_dirty();
}

void AvatarGroup::add_member(std::string name) {
  names_.push_back(std::move(name));
  mark_layout_dirty();
}

void AvatarGroup::clear_members() {
  if (names_.empty()) return;
  names_.clear();
  mark_layout_dirty();
}

void AvatarGroup::set_max_visible(std::size_t max_visible) {
  if (max_visible_ == max_visible) return;
  max_visible_ = max_visible;
  mark_layout_dirty();
}

void AvatarGroup::set_avatar_size(float size) {
  const float clamped = std::clamp(size, 12.0f, 96.0f);
  if (avatar_size_ == clamped) return;
  avatar_size_ = clamped;
  mark_layout_dirty();
}

auto AvatarGroup::initials_of(std::string_view name) -> std::string {
  // 取**第一个非空白码点**：Latin 转大写（`ada → A`），CJK 原样（`张三 → 张`）。
  //
  // CJK 取首字就是**姓氏**，这是中文头像的通行做法（微信/支付宝同款）；
  // 写全名在 28px 的圆里必然缩小到看不清，而缩写又不存在（不像 `Ada Lovelace → AL`）。
  std::size_t index = 0;
  while (index < name.size()) {
    const Codepoint codepoint = st::decode_utf8(name, index);
    if (codepoint.bytes == 0U) break;
    if (!st::is_space_codepoint(codepoint.value)) {
      char32_t value = codepoint.value;
      if (value >= U'a' && value <= U'z') value = value - U'a' + U'A';
      std::string out;
      st::encode_utf8(value, out);
      return out;
    }
    index += codepoint.bytes;
  }
  return {};
}

// ————————————————————————————————————————————————————————————————————————————
// 主题与度量
// ————————————————————————————————————————————————————————————————————————————

void AvatarGroup::apply_theme(const Theme& theme) {
  const Metrics& metrics = theme.metrics();
  ring_color_ = theme.colors().surface;   // 头像之间用底色描一圈分界（重叠才看得出来）
  fallback_fill_ = theme.colors().primary_soft;
  style_.background = math::Color{0, 0, 0, 0};
  style_.border_width = 0.0f;
  style_.border_color = math::Color{0, 0, 0, 0};
  style_.font_size = std::max(avatar_size_ * 0.42f, 9.0f);
  style_.color = theme.colors().on_primary;
  (void)metrics;
  apply_text_overrides();
}

void AvatarGroup::resolve_visible_count(float available_width) const {
  const std::size_t total = names_.size();
  if (total == 0U) {
    shown_count_ = 0;
    overflow_ = 0;
    return;
  }
  // 显式 max：直接照做。
  if (max_visible_ > 0U && total > max_visible_) {
    shown_count_ = max_visible_;
    overflow_ = total - max_visible_;
    return;
  }
  // 自动：先看全放得下吗（n 个头像的重叠总宽）。
  const auto total_width = [this](std::size_t count, bool with_badge) {
    if (count == 0U) return 0.0f;
    const std::size_t slots = count + (with_badge ? 1U : 0U);
    return avatar_size_ * (1.0f + (static_cast<float>(slots) - 1.0f) *
                                     (1.0f - kOverlapRatio));
  };
  if (max_visible_ == 0U && total_width(total, /*with_badge=*/false) <= available_width) {
    shown_count_ = total;
    overflow_ = 0;
    return;
  }
  // 逐个减到放得下（含 1 个徽标位）为止。
  std::size_t count = total;
  while (count > 1U && total_width(count, /*with_badge=*/true) > available_width) --count;
  shown_count_ = count;
  overflow_ = total - count;
}

void AvatarGroup::measure(const RenderContext& context, const Constraints& constraints) {
  (void)context;
  const float available =
      constraints.max_width < kUnbounded ? constraints.max_width : 1.0e9f;
  resolve_visible_count(available);
  // 自然宽：全部显示时的宽（交给父级决定要不要压缩）。
  const std::size_t natural_count = max_visible_ > 0U ? std::min(max_visible_, names_.size())
                                                      : names_.size();
  float width = 0.0f;
  if (natural_count > 0U) {
    width = avatar_size_ * (1.0f + (static_cast<float>(natural_count) - 1.0f) *
                                       (1.0f - kOverlapRatio));
    if (names_.size() > natural_count) width += avatar_size_ * (1.0f - kOverlapRatio);
  } else {
    width = avatar_size_;
  }
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
  width = std::clamp(width, style_.min_width, style_.max_width);

  float height = style_.has_explicit_height() ? style_.height : avatar_size_;
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  height = std::clamp(height, style_.min_height, style_.max_height);
  measured_ = math::Size{width, height};
}

// ————————————————————————————————————————————————————————————————————————————
// 落位
// ————————————————————————————————————————————————————————————————————————————

void AvatarGroup::relayout(float available_width) const {
  rects_.clear();
  overflow_box_ = math::Rect{};
  if (names_.empty() || bounds_.is_empty()) return;
  resolve_visible_count(std::min(available_width, bounds_.width));

  const float step = avatar_size_ * (1.0f - kOverlapRatio);
  const float top = bounds_.y + (bounds_.height - avatar_size_) * 0.5f;
  float x = bounds_.x;
  for (std::size_t index = 0; index < shown_count_; ++index) {
    rects_.emplace_back(x, top, avatar_size_, avatar_size_);
    x += step;
  }
  if (overflow_ > 0U) {
    overflow_box_ = math::Rect{x, top, avatar_size_, avatar_size_};
  }
}

void AvatarGroup::arrange(const RenderContext& context, math::Rect rect) {
  Element::arrange(context, rect);
  relayout(rect.width);
}

auto AvatarGroup::avatar_rect(std::size_t index) const -> math::Rect {
  return index < rects_.size() ? rects_[index] : math::Rect{};
}

// ————————————————————————————————————————————————————————————————————————————
// 绘制
// ————————————————————————————————————————————————————————————————————————————

void AvatarGroup::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty() || names_.empty()) return;
  relayout(bounds_.width);
  const TextPort& port = text_port_of(context);
  const Palette& colors = context.theme.colors();
  const float radius = avatar_size_ * 0.5f;

  for (std::size_t index = 0; index < rects_.size() && index < shown_count_; ++index) {
    const math::Rect rect = rects_[index];
    // 描一圈底色：重叠时相邻头像之间需要一道分界，否则连成一团。
    canvas.fill_circle(rect.center(), radius + 1.5f, raster::Paint::solid(ring_color_));
    canvas.fill_circle(rect.center(), radius, raster::Paint::solid(colors.primary_soft));
    const std::string initials = initials_of(names_[index]);
    if (!initials.empty()) {
      // 单字符按圆内可用宽自适应：窄头像（12~16px）下默认字号会溢出。
      const float available = avatar_size_ * 0.62f;
      float font = style_.font_size;
      const float natural = port.measure_width(initials, font);
      if (natural > available && natural > 0.0f) {
        font = std::max(font * (available / natural), 7.0f);
      }
      const float width = port.measure_width(initials, font);
      const float top = centered_line_top(port, initials, font, rect.y, rect.height);
      port.draw(canvas, initials, math::Point{rect.center().x - width * 0.5f, top}, font,
                colors.primary);
    }
  }

  if (!overflow_box_.is_empty() && overflow_ > 0U) {
    canvas.fill_circle(overflow_box_.center(), radius + 1.5f, raster::Paint::solid(ring_color_));
    canvas.fill_circle(overflow_box_.center(), radius, raster::Paint::solid(colors.surface_alt));
    const std::string label = std::format("+{}", overflow_);
    const float width = port.measure_width(label, style_.font_size);
    const float top = centered_line_top(port, label, style_.font_size, overflow_box_.y,
                                        overflow_box_.height);
    port.draw(canvas, label, math::Point{overflow_box_.center().x - width * 0.5f, top},
              style_.font_size, colors.text_muted);
  }
}

// ————————————————————————————————————————————————————————————————————————————
// 语义与属性面
// ————————————————————————————————————————————————————————————————————————————

auto AvatarGroup::semantics_text() const -> std::string {
  std::string out;
  for (std::size_t index = 0; index < names_.size(); ++index) {
    if (index > 0) out += ", ";
    out += names_[index];
  }
  return out;
}

auto AvatarGroup::semantics_value() const -> std::string { return std::to_string(names_.size()); }

auto AvatarGroup::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "members" || name == "text" || name == "items") {
    std::string out;
    for (std::size_t index = 0; index < names_.size(); ++index) {
      if (index > 0) out += '|';
      out += names_[index];
    }
    return out;
  }
  if (name == "max" || name == "max_visible") return std::to_string(max_visible_);
  if (name == "size" || name == "avatar_size") return std::format("{}", avatar_size_);
  if (name == "count") return std::to_string(names_.size());
  return Element::get_property(name);
}

auto AvatarGroup::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "members" || name == "text" || name == "items") {
    std::vector<std::string> next;
    for (const std::string_view part : st::split(value, '|')) {
      next.emplace_back(st::trim(part));
    }
    set_members(std::move(next));
    return true;
  }
  if (name == "max" || name == "max_visible") {
    const auto parsed = st::parse_u64(st::trim(value));
    if (!parsed.has_value()) return false;
    set_max_visible(static_cast<std::size_t>(*parsed));
    return true;
  }
  if (name == "size" || name == "avatar_size") {
    const auto parsed = st::parse_f64(st::trim(value));
    if (!parsed.has_value()) return false;
    set_avatar_size(static_cast<float>(*parsed));
    return true;
  }
  return Element::set_property(name, value);
}

auto AvatarGroup::property_names() const -> std::vector<std::string_view> {
  return Element::with_universal({"members", "max", "size"});
}

auto AvatarGroup::readonly_property_names() const -> std::vector<std::string_view> {
  return Element::with_universal_readonly({"count"});
}

}  // namespace st::ui

// ── 组件登记（`st/ui/component_registry.hpp`）────────────────────────
ST_UI_COMPONENT_IMPL(::st::ui::AvatarGroup);

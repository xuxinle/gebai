#include "st/ui/components/merge_view.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "st/raster/paint.hpp"
#include "st/ui/theme.hpp"

#include "components_internal.hpp"

namespace st::ui {

namespace {

/// 栏间距与段间距（逻辑像素）。
constexpr float kColumnGap{1.0f};
constexpr float kSegmentGap{2.0f};
/// 段内上下留白（让相邻段读起来是"块"而不是"一整片文本"）。
constexpr float kSegmentPadding{4.0f};
/// 按钮尺寸（取我方/取他方）。
constexpr float kButtonHeight{20.0f};
constexpr float kButtonMinWidth{52.0f};
/// 文本左内边距（行号留位）。
constexpr float kTextPadding{6.0f};

}  // namespace

MergeView::MergeView() {
  // 内容会超出自身矩形（三栏文本可能很长），必须裁剪——`clip_children` 同时
  // 决定绘制与命中（不设会出现"看不见却抢点击"）。
  style_.clip_children = true;
  set_focusable(true);   // 键盘导航（跳到下一个冲突）要能聚焦
}

void MergeView::clear() {
  plan_ = text::MergePlan{};
  base_text_.clear();
  ours_text_.clear();
  theirs_text_.clear();
  take_theirs_.clear();
  decided_.clear();
  current_segment_ = 0;
  scroll_ = 0.0f;
  mark_layout_dirty();
}

void MergeView::set_sides(std::string_view base_text, std::string_view ours_text,
                          std::string_view theirs_text) {
  base_text_ = base_text;
  ours_text_ = ours_text;
  theirs_text_ = theirs_text;
  plan_ = text::merge_three_way(base_text_, ours_text_, theirs_text);
  take_theirs_.assign(plan_.segments.size(), false);
  decided_.assign(plan_.segments.size(), false);
  // 一装载就把游标停在**第一个待决定的冲突**上（没有冲突时停在 0）——
  // 打开合并视图的人下一步就是要处理冲突，让他先自己找一遍是无谓的往返。
  current_segment_ = 0;
  for (std::size_t index = 0; index < plan_.segments.size(); ++index) {
    if (plan_.segments[index].kind == text::MergeKind::Conflict) {
      current_segment_ = index;
      break;
    }
  }
  scroll_ = 0.0f;
  mark_layout_dirty();
}

void MergeView::set_texts(std::string_view ours_text, std::string_view theirs_text) {
  // 无共同祖先：`base` 空。这是"两侧都在同一位置新增"的形态——
  // 相同内容会归 `BothSame`（不报冲突），不同则冲突。
  set_sides(std::string_view{}, ours_text, theirs_text);
}

auto MergeView::set_take_theirs(std::size_t index, bool take_theirs) -> bool {
  if (index >= plan_.segments.size()) return false;
  // 非冲突段**如实拒绝**（见头文件）：它们只有一种正确结果，
  // 允许改会让界面出现一个"按了不生效"的按钮——比禁用更糟。
  if (plan_.segments[index].kind != text::MergeKind::Conflict) return false;
  take_theirs_[index] = take_theirs;
  decided_[index] = true;
  mark_dirty();
  if (on_choice_changed) on_choice_changed();
  return true;
}

auto MergeView::takes_theirs(std::size_t index) const -> bool {
  if (index >= plan_.segments.size()) return false;
  // 非冲突段返回它**实际**会取的一侧（这样调用方不必自己判类型）。
  switch (plan_.segments[index].kind) {
    case text::MergeKind::TheirsOnly:
      return true;
    case text::MergeKind::Conflict:
      return take_theirs_[index];
    default:
      return false;
  }
}

void MergeView::set_all_conflicts(bool take_theirs) {
  for (std::size_t index = 0; index < plan_.segments.size(); ++index) {
    if (plan_.segments[index].kind == text::MergeKind::Conflict) {
      take_theirs_[index] = take_theirs;
      decided_[index] = true;
    }
  }
  mark_dirty();
  if (on_choice_changed) on_choice_changed();
}

auto MergeView::choices() const -> std::vector<int> {
  std::vector<int> out;
  out.reserve(plan_.segments.size());
  for (std::size_t index = 0; index < plan_.segments.size(); ++index) {
    out.push_back(takes_theirs(index) ? 1 : 0);
  }
  return out;
}

auto MergeView::result() const -> std::string {
  return text::merge_apply(plan_, choices());
}

auto MergeView::next_pending_conflict() -> std::optional<std::size_t> {
  // ⚠ 与 `previous_*` 同口径：**从当前段本身开始找**（不只是它后面）。
  //
  // 两个方向必须对称，否则会出现"F7 跳过眼前那一处、Shift+F7 又回到它"的循环
  //（用户按 F7 想看下一处，结果跳过了当前正在看的这一处；往回按又回到它）。
  // 只有一处未决定冲突时，两个方向都应返回它自己——这是"跳转"最不容易误解的语义。
  for (std::size_t index = current_segment_; index < plan_.segments.size(); ++index) {
    if (plan_.segments[index].kind == text::MergeKind::Conflict && !decided_[index]) {
      current_segment_ = index;
      mark_dirty();
      return index;
    }
  }
  return std::nullopt;
}

auto MergeView::previous_pending_conflict() -> std::optional<std::size_t> {
  // ⚠ 从 `current_segment_` **本身**开始往回找（而不是它前面一个）：
  // 只有一处未决定冲突时，那一处**就是**当前段——从它前面找会什么都找不到，
  // 于是"Shift+F7 在一处冲突上没反应"，而屏幕上明明还挂着"还有 1 处未决定"。
  // 实测踩到（单测第一版就是这么写的）。
  const std::size_t begin =
      plan_.segments.empty() ? 0
                             : std::min(current_segment_, plan_.segments.size() - 1);
  for (std::size_t index = begin + 1; index > 0; --index) {
    const std::size_t candidate = index - 1;
    if (plan_.segments[candidate].kind == text::MergeKind::Conflict && !decided_[candidate]) {
      current_segment_ = candidate;
      mark_dirty();
      return candidate;
    }
  }
  return std::nullopt;
}

void MergeView::set_current_segment(std::size_t index) {
  if (index >= plan_.segments.size()) return;
  current_segment_ = index;
  mark_dirty();
}

void MergeView::set_show_base(bool value) {
  if (show_base_ == value) return;
  show_base_ = value;
  mark_layout_dirty();
}

void MergeView::set_font_size(float size) {
  // `0` = 用主题默认（与 CodeEditor 同约定）。
  if (size <= 0.0f) return;
  if (font_size_ == size) return;
  font_size_ = size;
  mark_layout_dirty();
}

void MergeView::apply_theme(const Theme& theme) {
  const Palette& colors = theme.colors();
  surface_ = colors.surface;
  surface_alt_ = colors.surface_alt;
  border_ = colors.border;
  text_color_ = colors.text;
  text_muted_ = colors.text_muted;
  focus_ring_ = colors.primary;
  // 三栏的语气色：我方偏主色、他方偏中性，冲突段用警示色。
  // 用主题令牌而不是硬编码——硬编码在另一个主题上会变成刺眼的块。
  ours_tint_ = colors.primary_soft;
  theirs_tint_ = colors.surface_alt;
  conflict_tint_ = colors.warning;
  if (font_size_ <= 0.0f) font_size_ = theme.metrics().font_base;
}

auto MergeView::line_height() const -> float {
  // 用固定行高（不依赖量尺）：合并视图是"对照阅读"，行高一致比"按字体精确算"更重要，
  // 而且无头/文本端口缺失时量尺会返回 0（那会让整个视图塌成一条）。
  return line_height_ > 0.0f ? line_height_ : std::max(14.0f, font_size_ * 1.35f);
}

auto MergeView::segment_height(std::size_t index) const -> float {
  if (index >= plan_.segments.size()) return 0.0f;
  const auto& segment = plan_.segments[index];
  // 三栏同高：取三者行数的**最大值**——这样三栏逐段对齐（低于最大值的栏留空）。
  const std::size_t rows = std::max({segment.base.size(), segment.ours.size(), segment.theirs.size()});
  return static_cast<float>(rows) * line_height() + kSegmentPadding * 2.0f;
}

auto MergeView::content_height() const -> float {
  float total = 0.0f;
  for (std::size_t index = 0; index < plan_.segments.size(); ++index) {
    total += segment_height(index) + kSegmentGap;
  }
  return total;
}

auto MergeView::segment_top(std::size_t index) const -> float {
  float top = 0.0f;
  for (std::size_t cursor = 0; cursor < index && cursor < plan_.segments.size(); ++cursor) {
    top += segment_height(cursor) + kSegmentGap;
  }
  return bounds_.y + top - scroll_;
}

auto MergeView::column_rect(int column) const -> math::Rect {
  const int count = column_count();
  if (count == 0) return {};
  // `base` 栏隐藏时，column 0 是 ours、1 是 theirs（**不保留空位**——
  // 留一条空栏比"两栏各宽一点"更让人困惑）。
  const float usable = std::max(0.0f, bounds_.width - kColumnGap * static_cast<float>(count - 1));
  const float width = usable / static_cast<float>(count);
  const float x = bounds_.x + static_cast<float>(column) * (width + kColumnGap);
  return math::Rect{x, bounds_.y, width, bounds_.height};
}

auto MergeView::segment_rect(std::size_t index) const -> math::Rect {
  return math::Rect{bounds_.x, segment_top(index), bounds_.width, segment_height(index)};
}

auto MergeView::take_ours_rect(std::size_t index) const -> math::Rect {
  if (index >= plan_.segments.size()) return {};
  if (plan_.segments[index].kind != text::MergeKind::Conflict) return {};
  const math::Rect band = segment_rect(index);
  // 按钮压在**我方栏**底部右侧（"取我方"紧挨着要取的内容，指哪打哪）。
  const math::Rect column = column_rect(0);
  const float width = std::min(kButtonMinWidth, std::max(24.0f, column.width * 0.4f));
  return math::Rect{column.x + column.width - width - 6.0f,
                    band.y + band.height - kButtonHeight - 4.0f, width, kButtonHeight};
}

auto MergeView::take_theirs_rect(std::size_t index) const -> math::Rect {
  if (index >= plan_.segments.size()) return {};
  if (plan_.segments[index].kind != text::MergeKind::Conflict) return {};
  const math::Rect band = segment_rect(index);
  // "取他方"压在**最后一栏**（他方栏；`base` 隐藏时它是 1，显示时是 2）。
  const math::Rect column = column_rect(column_count() - 1);
  const float width = std::min(kButtonMinWidth, std::max(24.0f, column.width * 0.4f));
  return math::Rect{column.x + column.width - width - 6.0f,
                    band.y + band.height - kButtonHeight - 4.0f, width, kButtonHeight};
}

void MergeView::measure(const RenderContext& context, const Constraints& constraints) {
  (void)context;
  const float width = style_.has_explicit_width() ? style_.width
                     : bounds_.width > 0.0f        ? bounds_.width
                                                   : constraints.max_width;
  const float height = style_.has_explicit_height() ? style_.height
                      : bounds_.height > 0.0f        ? bounds_.height
                                                     : constraints.max_height;
  measured_ = math::Size{std::max(0.0f, width), std::max(0.0f, height)};
}

void MergeView::arrange(const RenderContext& context, math::Rect rect) {
  Element::arrange(context, rect);
}

void MergeView::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  const TextPort& port = components_internal::text_port_of(context);
  canvas.fill_rect(bounds_, raster::Paint::solid(surface_), 0.0f);

  if (plan_.segments.empty()) {
    port.draw(canvas, "无冲突内容", math::Point{bounds_.x + 12.0f, bounds_.y + 12.0f}, font_size_,
              text_muted_);
    return;
  }

  // 三栏底色（先铺满整段高度，让"这一栏到哪里"一眼可辨）。
  for (int column = 0; column < column_count(); ++column) {
    const math::Rect rect = column_rect(column);
    canvas.fill_rect(rect, raster::Paint::solid(column == 0 ? surface_ : surface_alt_), 0.0f);
  }

  const float line_h = line_height();
  for (std::size_t index = 0; index < plan_.segments.size(); ++index) {
    const auto& segment = plan_.segments[index];
    const math::Rect band = segment_rect(index);
    // 视口剔除：滚动到很远时不必画（段数可能上千）。
    if (band.y + band.height < bounds_.y || band.y > bounds_.bottom()) continue;

    // —— 段带：冲突段用警示色，其余用中性 ——
    const bool conflicted = segment.kind == text::MergeKind::Conflict;
    if (conflicted) {
      canvas.fill_rect(band, raster::Paint::solid(conflict_tint_.with_alpha_f(0.10f)), 0.0f);
    }
    if (index == current_segment_) {
      // 当前段的左缘指示条（1~2px 的细条比整块高亮更不抢眼，也更像 IDE）。
      canvas.fill_rect(math::Rect{band.x, band.y, 2.0f, band.height},
                       raster::Paint::solid(focus_ring_), 0.0f);
    }

    // —— 三栏文本 ——
    for (int column = 0; column < column_count(); ++column) {
      const math::Rect rect = column_rect(column);
      const std::vector<std::string>* lines = nullptr;
      if (show_base_) {
        switch (column) {
          case 0: lines = &segment.base; break;
          case 1: lines = &segment.ours; break;
          default: lines = &segment.theirs; break;
        }
      } else {
        lines = column == 0 ? &segment.ours : &segment.theirs;
      }
      if (lines == nullptr) continue;
      float y = band.y + kSegmentPadding;
      for (const auto& line : *lines) {
        if (y + line_h < bounds_.y) {   // 上方滚出
          y += line_h;
          continue;
        }
        if (y > bounds_.bottom()) break;
        // 栏内文本颜色：我方/他方各自可辨（不然三栏看起来是同一份内容）。
        const math::Color color = (show_base_ && column == 0) ? text_muted_ : text_color_;
        port.draw(canvas, line, math::Point{rect.x + kTextPadding, y}, font_size_, color);
        y += line_h;
      }
    }

    // —— 冲突段的两个按钮 ——
    if (conflicted) {
      const bool take_theirs = take_theirs_[index];
      const bool decided = decided_[index];
      const auto draw_button = [&](const math::Rect& rect, bool active, std::string_view label,
                                   math::Color tint) {
        canvas.fill_rect(rect, raster::Paint::solid(active ? tint : surface_),
                         rect.height * 0.25f);
        components_internal::paint_outline(canvas, rect, rect.height * 0.25f,
                                           active ? tint : border_, 1.0f);
        const float text_y = rect.y + (rect.height - line_h) * 0.5f;
        port.draw(canvas, label, math::Point{rect.x + 6.0f, text_y}, font_size_,
                  active ? text_color_ : text_muted_);
      };
      draw_button(take_ours_rect(index), decided && !take_theirs, "取我方", ours_tint_);
      draw_button(take_theirs_rect(index), decided && take_theirs, "取他方", theirs_tint_);
    }
  }

  // 顶部/底部的"还有 N 处未决定"提示条：合并时最需要知道的就是这个数字。
  if (const std::size_t pending = [this] {
        std::size_t count = 0;
        for (std::size_t index = 0; index < plan_.segments.size(); ++index) {
          if (plan_.segments[index].kind == text::MergeKind::Conflict && !decided_[index]) {
            ++count;
          }
        }
        return count;
      }();
      pending > 0) {
    const std::string hint = std::format("还有 {} 处未决定（F7 下一处）", pending);
    const float width = port.measure_width(hint, font_size_) + 16.0f;
    const math::Rect badge{bounds_.right() - width - 8.0f, bounds_.y + 6.0f, width, 20.0f};
    canvas.fill_rect(badge, raster::Paint::solid(conflict_tint_.with_alpha_f(0.9f)), 4.0f);
    port.draw(canvas, hint, math::Point{badge.x + 8.0f, badge.y + 3.0f}, font_size_,
              text_color_);
  }
}

auto MergeView::on_event(const RenderContext& /*context*/, Event& event) -> bool {
  switch (event.kind) {
    case EventKind::MouseDown: {
      if (event.button != 1) return false;
      // 按钮优先（它在段带之上）。
      for (std::size_t index = 0; index < plan_.segments.size(); ++index) {
        if (plan_.segments[index].kind != text::MergeKind::Conflict) continue;
        if (take_ours_rect(index).contains(event.position)) {
          (void)set_take_theirs(index, false);
          current_segment_ = index;
          event.handled = true;
          return true;
        }
        if (take_theirs_rect(index).contains(event.position)) {
          (void)set_take_theirs(index, true);
          current_segment_ = index;
          event.handled = true;
          return true;
        }
      }
      // 点段带 = 选中它（键盘导航的起点）。
      for (std::size_t index = 0; index < plan_.segments.size(); ++index) {
        if (segment_rect(index).contains(event.position)) {
          current_segment_ = index;
          mark_dirty();
          event.handled = true;
          return true;
        }
      }
      // 点空白：**不吞**（让上层处理，例如聚焦编辑器）。
      return false;
    }
    case EventKind::Wheel: {
      if (event.wheel_delta == 0.0f) return false;
      // 向上滚（正 delta）= 看上面的内容 ⇒ 减 scroll。
      scroll_ = std::max(0.0f, scroll_ - event.wheel_delta * line_height() * 2.0f);
      const float max_scroll = std::max(0.0f, content_height() - bounds_.height);
      scroll_ = std::min(scroll_, max_scroll);
      mark_dirty();
      event.handled = true;
      return true;
    }
    case EventKind::KeyDown: {
      if (!focused()) return false;
      if (event.key == "F7" || (event.key == "n" && event.ctrl)) {
        // 键名按平台：Windows 下功能键是 `F7`（与 `DiffView` 同约定）。
        const auto next = event.shift ? previous_pending_conflict() : next_pending_conflict();
        event.handled = true;
        return next.has_value();
      }
      return false;
    }
    default:
      return false;
  }
}

auto MergeView::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "conflicts") return std::to_string(conflict_count());
  if (name == "resolved") return std::to_string(resolved_count());
  if (name == "pending") {
    std::size_t count = 0;
    for (std::size_t index = 0; index < plan_.segments.size(); ++index) {
      if (plan_.segments[index].kind == text::MergeKind::Conflict && !decided_[index]) ++count;
    }
    return std::to_string(count);
  }
  if (name == "segments") return std::to_string(plan_.segments.size());
  if (name == "current") return std::to_string(current_segment_);
  if (name == "show_base") return show_base_ ? "true" : "false";
  return Element::get_property(name);
}

auto MergeView::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "show_base") {
    // 只认明确的两个词：把乱值当 false 会让"设错了"看起来像"功能坏了"。
    if (value == "true") {
      set_show_base(true);
      return true;
    }
    if (value == "false") {
      set_show_base(false);
      return true;
    }
    return false;
  }
  if (name == "current") {
    try {
      std::size_t used = 0;
      const auto parsed = static_cast<std::size_t>(std::stoul(std::string(value), &used));
      if (used != value.size()) return false;
      set_current_segment(parsed);
      return true;
    } catch (...) {
      return false;
    }
  }
  return Element::set_property(name, value);
}

auto MergeView::property_names() const -> std::vector<std::string_view> {
  auto names = Element::property_names();
  names.insert(names.end(), {"conflicts", "resolved", "pending", "segments", "current",
                             "show_base"});
  return names;
}

auto MergeView::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "take_ours" || action == "take_theirs") {
    // 无参数：作用于**当前段**（配合 F7 键盘流：跳过去、按键决定、再跳）。
    std::size_t index = current_segment_;
    if (!argument.empty()) {
      try {
        std::size_t used = 0;
        const auto parsed = static_cast<std::size_t>(std::stoul(std::string(argument), &used));
        if (used != argument.size()) return false;
        index = parsed;
      } catch (...) {
        return false;
      }
    }
    const bool applied = set_take_theirs(index, action == "take_theirs");
    if (applied) current_segment_ = index;
    return applied;
  }
  if (action == "take_ours_all") {
    set_all_conflicts(false);
    return true;
  }
  if (action == "take_theirs_all") {
    set_all_conflicts(true);
    return true;
  }
  if (action == "next_conflict") {
    return next_pending_conflict().has_value();
  }
  if (action == "previous_conflict") {
    return previous_pending_conflict().has_value();
  }
  return Element::invoke_action(action, argument);
}

auto MergeView::semantics_text() const -> std::string {
  return std::format("合并 {} 段，{} 处冲突，{} 处待决定", plan_.segments.size(),
                     conflict_count(),
                     [this] {
                       std::size_t count = 0;
                       for (std::size_t index = 0; index < plan_.segments.size(); ++index) {
                         if (plan_.segments[index].kind == text::MergeKind::Conflict &&
                             !decided_[index]) {
                           ++count;
                         }
                       }
                       return count;
                     }());
}

}  // namespace st::ui

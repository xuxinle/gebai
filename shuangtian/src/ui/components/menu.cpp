#include "st/ui/components/menu.hpp"

#include <algorithm>
#include <format>
#include <utility>

#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"
#include "st/ui/text_port.hpp"

#include "components_internal.hpp"

namespace st::ui {

using components_internal::draw_line;
using components_internal::fill_round_rect;
using components_internal::text_port_of;

namespace {

/// 勾选标记（✓：两段折线，笔画宽 1.6；颜色由调用方给）。
void draw_check(raster::Surface& canvas, math::Rect box, float size, math::Color color) {
  const float arm = size * 0.28f;
  const math::Point center{box.x + box.width * 0.5f - arm * 0.2f,
                           box.y + box.height * 0.5f + arm * 0.1f};
  raster::Path check;
  check.move_to(math::Point{center.x - arm, center.y - arm * 0.1f});
  check.line_to(math::Point{center.x - arm * 0.25f, center.y + arm * 0.7f});
  check.line_to(math::Point{center.x + arm, center.y - arm * 0.8f});
  canvas.stroke_path(check, raster::Paint::solid(color), 1.6f);
}

/// 子菜单指示箭头（▸：小三角填充）。
void draw_submenu_arrow(raster::Surface& canvas, math::Rect box, math::Color color) {
  const float size = 8.0f;
  const math::Point center{box.right() - size, box.y + box.height * 0.5f};
  raster::Path arrow;
  arrow.move_to(math::Point{center.x - size * 0.5f, center.y - size * 0.5f});
  arrow.line_to(math::Point{center.x + size * 0.5f, center.y});
  arrow.line_to(math::Point{center.x - size * 0.5f, center.y + size * 0.5f});
  arrow.close();
  canvas.fill_path(arrow, raster::Paint::solid(color));
}

/// 面板条目是否可激活（分隔线不可；`id`/`label` 全空的项视为分隔线兑底）。
[[nodiscard]] auto is_activatable(const MenuItem& item) noexcept -> bool {
  return !item.separator && !(item.id.empty() && item.label.empty());
}

}  // namespace

// —— MenuPanel ——

MenuPanel::MenuPanel(std::vector<MenuItem> items) : items_(std::move(items)) {
  style_.background = math::Color{0, 0, 0, 0};  // 面板底自绘（含阴影）
  set_focusable(true);
}

auto MenuPanel::item_count() const noexcept -> std::size_t { return items_.size(); }

auto MenuPanel::item_id(std::size_t index) const -> std::string_view {
  if (index >= items_.size()) return {};
  return items_[index].id;
}

void MenuPanel::set_anchor(math::Rect anchor_rect) {
  anchor_ = anchor_rect;
  mark_layout_dirty();
}

void MenuPanel::set_highlighted(std::size_t index) {
  if (index >= items_.size()) index = kNoIndex;
  if (index != kNoIndex && items_[index].separator) index = kNoIndex;
  if (highlighted_ == index) return;
  highlighted_ = index;
  mark_dirty();
}

auto MenuPanel::item_rect(std::size_t index) const -> math::Rect {
  if (index >= items_.size()) return math::Rect{};
  const float y = bounds_.y + static_cast<float>(index) * item_height_;
  return math::Rect{bounds_.x, y, bounds_.width, item_height_};
}

auto MenuPanel::item_at_point(math::Point point) const -> std::size_t {
  if (!bounds_.contains(point)) return kNoIndex;
  const auto index = static_cast<std::size_t>((point.y - bounds_.y) / item_height_);
  if (index >= items_.size()) return kNoIndex;
  return index;
}

void MenuPanel::apply_theme(const Theme& theme) {
  const Metrics& metrics = theme.metrics();
  item_height_ = kItemHeight;
  style_.background = math::Color{0, 0, 0, 0};
  style_.radius = metrics.radius_md;
  style_.color = theme.colors().text;
  style_.font_size = metrics.font_base;
}

void MenuPanel::measure(const RenderContext& context, const Constraints& constraints) {
  const TextPort& port = text_port_of(context);
  const Metrics& metrics = context.theme.metrics();
  float text_width = 0.0f;
  for (const auto& item : items_) {
    if (item.separator) continue;
    text_width = std::max(text_width, port.measure_width(item.label, metrics.font_base));
    if (!item.children.empty()) text_width += 16.0f;  // 子菜单箭头位
  }
  float width = text_width + kItemPaddingX * 2.0f + 20.0f;  // 勾选位留白
  width = std::max(width, kMinPanelWidth);
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
  width = std::clamp(width, kMinPanelWidth, kMaxPanelWidth);

  const float height = static_cast<float>(items_.size()) * item_height_;
  measured_ = math::Size{width, height};
}

void MenuPanel::arrange(const RenderContext& context, math::Rect rect) {
  (void)context;
  // 锚定优先：挂 Stack overlay 时 UiRoot 分到的是「顶部左对齐 + 自身尺寸」矩形，
  // 不是标题下方——面板必须自己落到 anchor_（标题正下方、左对齐；锚为空时用分到的矩形）。
  if (anchor_.width > 0.0f || anchor_.height > 0.0f) {
    bounds_ = math::Rect{anchor_.x, anchor_.bottom(), measured_.width, measured_.height};
  } else {
    bounds_ = rect;
  }
  layout_dirty_ = false;
}

void MenuPanel::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  const auto& colors = context.theme.colors();
  const auto& metrics = context.theme.metrics();

  // 面板：阴影 + surface 底 + radius_md（自绘，见头文件纪律）。
  const Shadow shadow = shadow_md(context.theme);
  canvas.draw_shadow(bounds_, metrics.radius_md, shadow.blur, shadow.color,
                     math::Point{shadow.offset_x, shadow.offset_y});
  fill_round_rect(canvas, bounds_, metrics.radius_md,
                  raster::Paint::solid(colors.surface));

  for (std::size_t index = 0; index < items_.size(); ++index) {
    const MenuItem& item = items_[index];
    const math::Rect row = item_rect(index);
    if (item.separator) {
      const math::Rect line{row.x + kItemPaddingX,
                            row.y + row.height * 0.5f - 0.5f,
                            std::max(0.0f, row.width - kItemPaddingX * 2.0f), 1.0f};
      canvas.fill_rect(line, raster::Paint::solid(colors.border));
      continue;
    }
    const bool highlighted = index == highlighted_;
    if (highlighted) {
      fill_round_rect(canvas,
                      math::Rect{row.x + 2.0f, row.y + 1.0f,
                                 std::max(0.0f, row.width - 4.0f), row.height - 2.0f},
                      metrics.radius_sm, raster::Paint::solid(colors.primary_soft));
    }
    // 勾选位（固定左列；无勾也占位，文本对齐一致）。
    const math::Rect check_box{row.x + kItemPaddingX, row.y, 16.0f, row.height};
    if (item.checked) draw_check(canvas, check_box, metrics.font_base, colors.primary);
    const math::Rect text_box{check_box.right(), row.y,
                              std::max(0.0f, row.width - kItemPaddingX - 20.0f -
                                                 (check_box.right() - row.x)),
                              row.height};
    draw_line(context, canvas, item.label, text_box, metrics.font_base,
              highlighted ? colors.primary : colors.text);
    if (!item.children.empty()) {
      draw_submenu_arrow(canvas, row, colors.text_muted);
    }
  }
}

auto MenuPanel::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  switch (event.kind) {
    case EventKind::MouseMove: {
      const std::size_t index = item_at_point(event.position);
      if (index != kNoIndex && !items_[index].separator) set_highlighted(index);
      return true;  // 面板内悬停跟踪
    }
    case EventKind::Click:
    case EventKind::MouseDown: {
      const std::size_t index = item_at_point(event.position);
      if (index == kNoIndex) return true;  // 面板空白：吞掉（模态）
      if (items_[index].separator) return true;
      set_highlighted(index);
      event.handled = true;
      if (on_activate) on_activate(index);
      if (on_close) on_close();
      return true;
    }
    case EventKind::KeyDown: {
      if (event.key == "Escape" || event.key == "Esc") {
        event.handled = true;
        if (on_close) on_close();
        return true;
      }
      if (items_.empty()) return false;
      // ↑↓ 键盘导航（跳过分隔线）。
      if (event.key == "ArrowDown" || event.key == "ArrowUp") {
        const std::ptrdiff_t step = event.key == "ArrowDown" ? 1 : -1;
        std::ptrdiff_t next = highlighted_ == kNoIndex
                                  ? (step > 0 ? 0 : static_cast<std::ptrdiff_t>(items_.size()) - 1)
                                  : static_cast<std::ptrdiff_t>(highlighted_) + step;
        while (next >= 0 && next < static_cast<std::ptrdiff_t>(items_.size()) &&
               items_[static_cast<std::size_t>(next)].separator) {
          next += step;
        }
        if (next >= 0 && next < static_cast<std::ptrdiff_t>(items_.size())) {
          set_highlighted(static_cast<std::size_t>(next));
        }
        return true;
      }
      if (event.key == "Enter") {
        if (highlighted_ != kNoIndex && is_activatable(items_[highlighted_])) {
          event.handled = true;
          if (on_activate) on_activate(highlighted_);
          if (on_close) on_close();
          return true;
        }
        return true;  // 面板开着：Enter 无选中也不下沉
      }
      return false;
    }
    case EventKind::Wheel:
      return true;  // 面板不滚动（条目多时由数据方裁剪）
    default:
      return false;
  }
}

auto MenuPanel::semantics_text() const -> std::string {
  std::string out;
  for (std::size_t index = 0; index < items_.size(); ++index) {
    if (items_[index].separator) continue;
    if (!out.empty()) out.append(" | ");
    out.append(items_[index].label);
  }
  return out;
}

auto MenuPanel::semantics_flags() const -> SemanticsFlags {
  SemanticsFlags flags = Element::semantics_flags();
  flags.selected = highlighted_ != kNoIndex;
  return flags;
}

// —— MenuBar ——

MenuBar::MenuBar() {
  style_.direction = FlexDirection::Row;
  style_.background = math::Color{0, 0, 0, 0};
  set_focusable(true);
}

void MenuBar::set_menus(const std::vector<Menu>& menus) {
  menus_ = menus;
  title_widths_.assign(menus_.size(), 0.0f);
  open_index_ = kNoIndex;
  mark_layout_dirty();
}

auto MenuBar::menu_label(std::size_t index) const -> std::string_view {
  if (index >= menus_.size()) return {};
  return menus_[index].label;
}

auto MenuBar::menu_id(std::size_t index) const -> std::string_view {
  if (index >= menus_.size()) return {};
  return menus_[index].id;
}

auto MenuBar::title_rect(std::size_t index) const -> math::Rect {
  float x = bounds_.x;
  for (std::size_t position = 0; position < index && position < title_widths_.size(); ++position) {
    x += title_widths_[position];
  }
  if (index >= title_widths_.size()) return math::Rect{};
  return math::Rect{x, bounds_.y, title_widths_[index], bounds_.height};
}

void MenuBar::set_open_index(std::size_t index) {
  if (index >= menus_.size()) index = kNoIndex;
  if (open_index_ == index) return;
  open_index_ = index;
  mark_dirty();
}

auto MenuBar::make_panel(std::size_t index) -> std::unique_ptr<MenuPanel> {
  if (index >= menus_.size()) return nullptr;
  auto panel = std::make_unique<MenuPanel>(menus_[index].items);
  const std::string menu_id = menus_[index].id;
  const std::size_t menu_index = index;
  panel->on_activate = [this, menu_id, menu_index](std::size_t item_index) {
    if (item_index >= menus_[menu_index].items.size()) return;
    if (on_action) on_action(menu_id, menus_[menu_index].items[item_index].id);
  };
  // 关闭的两条路径（激活条目 / Esc / 点面板外）都走 `on_menu_close` 通知调用方
  // 摘掉 overlay。
  //
  // 这里踩过一个**真缺陷**：早期把 `panel->on_close` 直接接到 `set_open_index`，
  // 于是“关闭”只更新了菜单栏自己的状态，调用方**永远收不到通知** →
  // overlay 留在屏上（再加一次 `on_open_menu` 叠成两张，实测菜单选完不消失）。
  panel->on_close = [this, menu_index]() {
    if (open_index_ == menu_index) set_open_index(kNoIndex);
    if (on_menu_close) on_menu_close();
  };
  // 锚定：标题正下方、与标题左对齐。
  panel->set_anchor(title_rect(index));
  open_index_ = index;
  return panel;
}

void MenuBar::apply_theme(const Theme& theme) {
  const Metrics& metrics = theme.metrics();
  style_.background = math::Color{0, 0, 0, 0};
  style_.color = theme.colors().text;
  style_.font_size = metrics.font_base;
  style_.height = kBarHeight;
}

void MenuBar::measure(const RenderContext& context, const Constraints& constraints) {
  const TextPort& port = text_port_of(context);
  const Metrics& metrics = context.theme.metrics();
  title_widths_.assign(menus_.size(), 0.0f);
  float total = 0.0f;
  for (std::size_t index = 0; index < menus_.size(); ++index) {
    const float width = port.measure_width(menus_[index].label, metrics.font_base) +
                        kTitlePaddingX * 2.0f;
    title_widths_[index] = width;
    total += width;
  }
  if (constraints.max_width < kUnbounded) total = std::min(total, constraints.max_width);
  measured_ = math::Size{total, kBarHeight};
}

void MenuBar::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  const auto& colors = context.theme.colors();
  const auto& metrics = context.theme.metrics();
  for (std::size_t index = 0; index < menus_.size(); ++index) {
    const math::Rect title = title_rect(index);
    const bool open = index == open_index_;
    const bool hovered = static_cast<int>(index) == hover_index_;
    if (open) {
      fill_round_rect(canvas, title, metrics.radius_sm,
                      raster::Paint::solid(colors.primary_soft));
    } else if (hovered) {
      fill_round_rect(canvas, title, metrics.radius_sm, raster::Paint::solid(colors.surface_alt));
    }
    draw_line(context, canvas, menus_[index].label, title, metrics.font_base,
              open ? colors.primary : colors.text);
  }
}

auto MenuBar::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  switch (event.kind) {
    case EventKind::MouseMove: {
      const std::size_t index = [this](math::Point point) {
        for (std::size_t position = 0; position < menus_.size(); ++position) {
          if (title_rect(position).contains(point)) return position;
        }
        return kNoIndex;
      }(event.position);
      const int next = index == kNoIndex ? -1 : static_cast<int>(index);
      const bool changed = next != hover_index_;
      hover_index_ = next;
      if (changed) mark_dirty();
      // 已打开面板时，悬停到**另一个**标题就切过去（VSCode/浏览器菜单栏惯例）。
      // 没有这一步，用户从“文件”移到“编辑”时旧面板不换——只能点开新的（然后就叠了两张）。
      if (index != kNoIndex && open_index_ != kNoIndex && index != open_index_) {
        set_open_index(index);
        if (on_open_menu) on_open_menu(index);
      }
      return false;
    }
    case EventKind::MouseDown: {
      // 只做“按下”视觉态与命中标记：**打开面板的动作留给 Click**。
      //
      // 曾经把 Click 与 MouseDown 合在一个 case 里（两边都调 on_open_menu）——
      // 一次物理点击会触发**两次**回调（down + click），于是“打开 → 又切回关闭”，
      // 菜单面板永远不出现（实测：控制通道 input.mouse click 与真实鼠标都不出面板）。
      // Button 的约定也是这一套：MouseDown 只标脏，Click 才 activate。
      for (std::size_t index = 0; index < menus_.size(); ++index) {
        if (!title_rect(index).contains(event.position)) continue;
        event.handled = true;
        return true;
      }
      return false;
    }
    case EventKind::Click: {
      for (std::size_t index = 0; index < menus_.size(); ++index) {
        if (!title_rect(index).contains(event.position)) continue;
        event.handled = true;
        set_open_index(index);
        if (on_open_menu) on_open_menu(index);
        return true;
      }
      // 点菜单栏空白：视为关闭请求（否则面板留在屏上没人摘）
      if (open_index_ != kNoIndex) {
        set_open_index(kNoIndex);
        event.handled = true;
        return true;
      }
      return false;
    }
    case EventKind::KeyDown: {
      if (menus_.empty()) return false;
      if (event.key == "ArrowRight" || event.key == "ArrowLeft") {
        const std::ptrdiff_t step = event.key == "ArrowRight" ? 1 : -1;
        std::ptrdiff_t base = open_index_ == kNoIndex ? 0
                                                      : static_cast<std::ptrdiff_t>(open_index_);
        std::ptrdiff_t next = base + step;
        const auto size = static_cast<std::ptrdiff_t>(menus_.size());
        if (next < 0) next = size - 1;
        if (next >= size) next = 0;
        set_open_index(static_cast<std::size_t>(next));
        if (on_open_menu) on_open_menu(static_cast<std::size_t>(next));
        return true;
      }
      if (event.key == "Enter" || event.key == "ArrowDown") {
        const std::size_t index = open_index_ == kNoIndex ? 0 : open_index_;
        set_open_index(index);
        if (on_open_menu) on_open_menu(index);
        return true;
      }
      if (event.key == "Escape" || event.key == "Esc") {
        set_open_index(kNoIndex);
        return false;  // 不吞：调用方可能还要摘面板 overlay
      }
      return false;
    }
    default:
      return false;
  }
}

void MenuBar::activate() {
  set_focusable(true);
  if (open_index_ == kNoIndex && !menus_.empty()) {
    set_open_index(0);
    if (on_open_menu) on_open_menu(0);
  }
}

auto MenuBar::semantics_text() const -> std::string {
  std::string out;
  for (const auto& menu : menus_) {
    if (!out.empty()) out.append(" | ");
    out.append(menu.label);
  }
  return out;
}

auto MenuBar::semantics_flags() const -> SemanticsFlags {
  SemanticsFlags flags = Element::semantics_flags();
  flags.selected = open_index_ != kNoIndex;
  return flags;
}

// —— ContextMenu ——

ContextMenu::ContextMenu(math::Point anchor, std::vector<MenuItem> items)
    : anchor_(anchor), panel_(nullptr) {
  auto panel = std::make_unique<MenuPanel>(std::move(items));
  panel_ = panel.get();
  // 面板生命周期由本元素持有：ContextMenu 挂 FillViewport overlay（透明遮罩），
  // 面板是它的"内容"。不用 add_child（面板 bounds 由本类 arrange 直接指定，
  // 避免父容器 Flex 布局挪动面板位置）。
  owned_panel_ = std::move(panel);
  panel_->on_close = [this]() {
    if (on_close) on_close();
  };
  style_.background = math::Color{0, 0, 0, 0};  // 透明：不遮罩
}

auto ContextMenu::make(math::Point anchor, std::vector<MenuItem> items)
    -> std::unique_ptr<ContextMenu> {
  return std::make_unique<ContextMenu>(anchor, std::move(items));
}

void ContextMenu::apply_theme(const Theme& theme) {
  Element::apply_theme(theme);
  style_.background = math::Color{0, 0, 0, 0};
  if (panel_ != nullptr) panel_->apply_theme(theme);
}

void ContextMenu::measure(const RenderContext& context, const Constraints& constraints) {
  // 面板尺寸即内容尺寸；本体铺满视口（FillViewport 形态下约束即视口）。
  panel_->measure(context, constraints);
  const float width = constraints.max_width < kUnbounded ? constraints.max_width : 0.0f;
  const float height = constraints.max_height < kUnbounded ? constraints.max_height : 0.0f;
  measured_ = math::Size{width, height};
}

void ContextMenu::arrange(const RenderContext& context, math::Rect rect) {
  bounds_ = rect;
  // 面板定位：锚点右下展开 → 越界翻转（先水平后垂直）→ 仍越界夹入。
  const math::Size size = panel_->measured_size();
  float x = anchor_.x;
  float y = anchor_.y;
  if (x + size.width > rect.right()) x = std::max(rect.x, anchor_.x - size.width);
  if (y + size.height > rect.bottom()) y = std::max(rect.y, anchor_.y - size.height);
  panel_->arrange(context, math::Rect{x, y, size.width, size.height});
  layout_dirty_ = false;
}

void ContextMenu::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (panel_ == nullptr) return;
  // 只画面板（透明遮罩：不铺 overlay 色，面板外完全透出下层内容）。
  panel_->paint(context, canvas);
}

auto ContextMenu::on_event(const RenderContext& context, Event& event) -> bool {
  switch (event.kind) {
    case EventKind::KeyDown:
      if (event.key == "Escape" || event.key == "Esc") {
        event.handled = true;
        if (on_close) on_close();
        return true;
      }
      // 面板内的键盘导航（↑↓/Enter）转给面板。
      return panel_->on_event(context, event);
    case EventKind::MouseMove:
      (void)panel_->on_event(context, event);
      return false;  // 面板外移动穿透（hover 下层内容照常）
    case EventKind::Wheel:
      // 面板上的滚轮转面板；面板外穿透（滚动下层内容）。
      return panel_->bounds().contains(event.position) && panel_->on_event(context, event);
    case EventKind::Click:
    case EventKind::MouseDown: {
      // dismiss barrier：面板内走条目；面板外触发关闭。两者都消费（见类注释）。
      if (panel_->bounds().contains(event.position)) {
        return panel_->on_event(context, event);
      }
      event.handled = true;
      if (on_close) on_close();
      return true;
    }
    default:
      return false;
  }
}

}  // namespace st::ui

#include "st/ui/components/theme_picker.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "st/core/string.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/ui/builtin_themes.hpp"
#include "st/ui/icon.hpp"
#include "st/ui/text_port.hpp"

#include "components_internal.hpp"

namespace st::ui {

using components_internal::paint_focus_ring;
using components_internal::paint_outline;
using components_internal::text_port_of;
using st::ui::shadow_md;   // 主题阴影令牌（theme.hpp）

namespace {

// —— 色卡墙几何（骨架色/间距/圆角取当前主题 token；卡内容色取各主题自己的 token）——
constexpr float k_card_width = 64.0f;      // 色卡宽（含名字区）
constexpr float k_swatch_height = 48.0f;   // 色块高（三段拼色）
constexpr float k_label_height = 24.0f;    // 名字区高（垂直居中，上下呼吸位）
constexpr float k_card_gap = 10.0f;        // 卡间缝
constexpr std::size_t k_columns = 5;       // 网格列数（10 套主题 = 2 行）
constexpr float k_dialog_padding = 20.0f;  // 卡片内边距
constexpr float k_title_height = 32.0f;    // 卡片标题行高（“选择主题”）
constexpr float k_card_height = k_swatch_height + k_label_height;

/// 左对齐、垂直居中的单行文本（复用 Select 的画法）。
void paint_line(const RenderContext& context, raster::Surface& canvas, std::string_view text,
                math::Rect box, float size, math::Color color, bool center = true) {
  if (text.empty() || box.is_empty() || box.width <= 0.0f) return;
  const TextPort& port = text_port_of(context);
  const std::string clipped = port.ellipsize(text, size, box.width);
  if (clipped.empty()) return;
  const float line = port.line_height(size);
  const float x = center ? box.x + (box.width - port.measure_width(clipped, size)) * 0.5f : box.x;
  port.draw(canvas, clipped, math::Point{x, box.y + (box.height - line) * 0.5f}, size, color);
}

/// 单张主题色卡：大面积拼色样品（详见头文件说明；v3 评审 9.0/8.0 的形态）。
void paint_card(const RenderContext& context, raster::Surface& canvas, math::Rect rect,
                std::string_view name, bool selected, bool hovered) {
  const Palette& colors = context.theme.colors();
  const Metrics& metrics = context.theme.metrics();
  const float radius = metrics.radius_sm;

  canvas.fill_rect(rect, raster::Paint::solid(colors.surface_raised), radius);
  if (selected) {
    canvas.fill_rect(rect, raster::Paint::solid(colors.primary_soft), radius);
    paint_outline(canvas, rect, radius, colors.primary, metrics.border_width * 2.0f);
  } else if (hovered) {
    canvas.fill_rect(rect, raster::Paint::solid(colors.surface_hover), radius);
  }

  const auto preview = builtin_theme_preview_colors(name);
  const math::Color bg = preview ? preview->bg : colors.bg;
  const math::Color primary = preview ? preview->primary : colors.primary;
  std::optional<Theme> own_theme = builtin_theme(name);
  const math::Color raised =
      own_theme.has_value() ? own_theme->colors().surface_raised : colors.surface_raised;

  const float swatch_h = rect.height - k_label_height;
  const math::Rect top{rect.x, rect.y, rect.width, swatch_h * 0.66f};
  canvas.fill_rect(top, raster::Paint::solid(bg), radius);
  const math::Rect band{rect.x, top.bottom(), rect.width, 8.0f};
  canvas.fill_rect(band, raster::Paint::solid(primary));
  // 下段：primary 低透明洗色（暗卡的黑底 raised 与 bg 几乎同黑，拼色会退化成两段）
  const math::Color wash = raised.mix(primary, 0.18f);
  const math::Rect rest{rect.x, band.bottom(), rect.width, rect.y + swatch_h - band.bottom()};
  canvas.fill_rect(rest, raster::Paint::solid(wash));

  if (selected) {
    const math::Rect check{top.x + (top.width - 16.0f) * 0.5f,
                           top.y + (top.height - 16.0f) * 0.5f, 16.0f, 16.0f};
    Icon::draw(canvas, "check", check, primary);
  }

  const math::Rect label_box{rect.x, rect.y + swatch_h, rect.width, k_label_height};
  paint_line(context, canvas, builtin_theme_display_name(name), label_box, metrics.font_xs,
             selected ? colors.primary : colors.text_muted);
}

}  // namespace

// ————————————————— ThemePanel（模态色卡墙）————————————————

ThemePanel::ThemePanel(std::vector<std::string> theme_names, std::string current)
    : names_(std::move(theme_names)) {
  set_current(current);
  style_.direction = FlexDirection::Column;
}

void ThemePanel::set_current(const std::string& name) {
  for (std::size_t i = 0; i < names_.size(); ++i) {
    if (names_[i] == name) {
      selected_ = i;
      mark_dirty();
      return;
    }
  }
}

auto ThemePanel::columns() const -> std::size_t { return k_columns; }

auto ThemePanel::center_card() const -> math::Rect {
  // 卡片尺寸：网格 + 边距 + 标题行（居中卡片 = Dialog 同规格：radius_xl + shadow_lg）
  const std::size_t cols = columns();
  const std::size_t rows = (names_.size() + cols - 1) / cols;
  const float width =
      static_cast<float>(cols) * k_card_width + static_cast<float>(cols - 1) * k_card_gap +
      k_dialog_padding * 2.0f;
  const float height = static_cast<float>(rows) * k_card_height +
                       static_cast<float>(rows - 1) * k_card_gap + k_dialog_padding * 2.0f +
                       k_title_height;
  const float x = bounds_.x + (bounds_.width - width) * 0.5f;
  const float y = bounds_.y + (bounds_.height - height) * 0.5f;
  return math::Rect{x, y, width, height};
}

auto ThemePanel::card_rect(std::size_t index) const -> math::Rect {
  if (index >= names_.size()) return math::Rect{};
  const math::Rect card = center_card();
  const std::size_t cols = columns();
  const std::size_t row = index / cols;
  const std::size_t col = index % cols;
  return math::Rect{card.x + k_dialog_padding + static_cast<float>(col) * (k_card_width + k_card_gap),
                    card.y + k_dialog_padding + k_title_height +
                        static_cast<float>(row) * (k_card_height + k_card_gap),
                    k_card_width, k_card_height};
}

void ThemePanel::apply_theme(const Theme& theme) {
  const Palette& colors = theme.colors();
  const Metrics& metrics = theme.metrics();
  // 整体背景**必须透明**：FillViewport 形态下面板铺满视口，而真正的背板是
  // 居中卡片（paint_content 自己画）。若这里设不透明色，`paint_box` 会先把
  // 全视口填满——遮罩叠在这层不透明底上，原窗口内容就完全看不到了
  //（实测：遮罩 30% 黑却把整个画面刷成均匀灰，用户报“全部遮挡”）。
  style_.background = math::Color{0, 0, 0, 0};
  style_.radius = 0.0f;
  style_.shadow = Shadow{};
  style_.font_size = metrics.font_base;
  style_.color = colors.text;
  // 阴影/圆角属于居中卡片（paint_content 里用 shadow_md/radius_xl 画）。
  (void)colors;
}

void ThemePanel::measure(const RenderContext& context, const Constraints& constraints) {
  apply_theme(context.theme);
  // FillViewport 形态：占满分到的矩形（遮罩铺满、卡片自己居中）。
  float width = constraints.max_width;
  float height = constraints.max_height;
  if (width >= kUnbounded) {
    const math::Rect card = center_card();
    width = card.width + 80.0f;
  }
  if (height >= kUnbounded) {
    const math::Rect card = center_card();
    height = card.height + 80.0f;
  }
  measured_ = math::Size{width, height};
}

void ThemePanel::arrange(const RenderContext& context, math::Rect rect) {
  Element::arrange(context, rect);
  card_area_ = rect;
}

void ThemePanel::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (bounds_.is_empty()) return;
  const Palette& colors = context.theme.colors();
  const Metrics& metrics = context.theme.metrics();

  // —— 1. 遮罩：铺满视口、半透明（底层内容隐约可见）。
  //
  //    强度按背景亮度自适应：全局 `colors.overlay` 是模态强度（亮 ~30%、
  //    暗 ~65% 黑），但**暗背景叠黑遮罩几乎不可见却已死黑**（实测：qinhan
  //    下卡外与卡内像素几乎同色，背景内容完全看不出）——“模态感”没了。
  //    所以：亮背景用原强度；暗背景只轻压（12% 黑）——层次靠卡片阴影/描边，
  //    背景内容保持可辨认（用户主诉“不要全部遮挡原窗口内容”）。
  //    不改全局 overlay：file_dialog/overlay.cpp 要的是强模态，语义不同。
  const float bg_luma =
      (colors.bg.r * 0.299f + colors.bg.g * 0.587f + colors.bg.b * 0.114f) / 255.0f;
  const float dim_alpha = math::clampf(bg_luma * 0.30f, 0.12f, 0.30f);
  canvas.fill_rect(bounds_, raster::Paint::solid(colors.overlay.with_alpha_f(dim_alpha)));

  // —— 2. 居中卡片：`paint_raised_surface` 全套（辉光+阴影+底色+毛玻璃+边缘光）。
  //    面板整体透明（见 apply_theme），只有这块是实底。
  const math::Rect card = center_card();
  components_internal::paint_raised_surface(context, canvas, card, metrics.radius_xl,
                                            shadow_md(context.theme));
  paint_outline(canvas, card, metrics.radius_xl, colors.border, metrics.border_width);

  // 标题行（“选择主题”，font_lg）
  const math::Rect title_box{card.x + k_dialog_padding, card.y + metrics.space_xs,
                             card.width - k_dialog_padding * 2.0f, k_title_height - metrics.space_xs};
  paint_line(context, canvas, "选择主题", title_box, metrics.font_lg, colors.text, /*center=*/false);

  // —— 3. 色卡网格 ——
  if (names_.empty()) return;
  for (std::size_t index = 0; index < names_.size(); ++index) {
    paint_card(context, canvas, card_rect(index), names_[index],
               selected_.has_value() && *selected_ == index, hover_index_ == index);
  }
}

auto ThemePanel::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  const auto hit_card = [this](math::Point point) -> std::size_t {
    for (std::size_t index = 0; index < names_.size(); ++index) {
      if (card_rect(index).contains(point)) return index;
    }
 return static_cast<std::size_t>(-1);
  };
  switch (event.kind) {
    case EventKind::MouseMove: {
      const std::size_t now = hit_card(event.position);
      if (now != hover_index_) {
        hover_index_ = now;
        mark_dirty();
      }
      return true;
    }
    case EventKind::HoverOut:
      if (hover_index_ != static_cast<std::size_t>(-1)) {
        hover_index_ = static_cast<std::size_t>(-1);
        mark_dirty();
      }
      return false;
    case EventKind::MouseDown:
      // 只记按压位置，不出动作（结算在 Click——事件序列末尾）。
      // **卡外按下也要记**（区别于“没有先导 MouseDown”）：点遮罩是“我收下的
      // 按放”，Click 结算时据此关闭；而 “没有本面板的 down” 是标题栏开模态
      // 那一下的 Click 尾巴——不许关。
      pressed_inside_ = true;
      pressed_on_card_ = hit_card(event.position);
      return true;   // 遮罩吃掉一切点击（模态）
    case EventKind::MouseUp:
      // 只吃掉事件，**不动 pressed_on_card_**：up 之后马上是 click（合成点击 /
      // 后端 LBUTTONUP 补发），对账必须在 click 做——up 就重置的话，click 里
      // `index == pressed_on_card_` 恒不成立，真实点击永远走遮罩关闭分支
      //（实测：点色卡模态关了、卡没选中）。
      return true;
    case EventKind::Click:
    case EventKind::DoubleClick: {
      const std::size_t index = hit_card(event.position);
      // 同一张卡才选：按下与结算落在同一卡且不是卡外。
      const bool picked =
          pressed_inside_ && index != static_cast<std::size_t>(-1) &&
          index == pressed_on_card_ && pressed_on_card_ != static_cast<std::size_t>(-1);
      const bool had_press = pressed_inside_;
      pressed_inside_ = false;
      pressed_on_card_ = static_cast<std::size_t>(-1);
      if (picked) {
        // 先隐藏再回调（回调会立即摘本对象——UiRoot 延迟析构保栈安全）。
        set_visible(false);
        const auto pick = on_pick;
        if (pick) pick(names_[index]);
        return true;
      }
      // 点在卡外（含点遮罩）：关闭。只能在 Click 关——down/up 就摘的话，
      // 合成点击序列里后续事件会命中下层元素（穿透，实测踩到）。
      //
      // **只关“发生在本面板上的按放”**（`pressed_inside_`）：否则“标题栏开模态”
      // 那一下的 Click（挂载在 MouseUp 里、紧随的 Click 命中刚挂上的全视口面板）
      // 会把模态“刚打开立刻关掉”——实测用户看到的“主题框弹不出来”。
      if (had_press) {
        set_visible(false);
        const auto dismiss = on_dismiss;
        if (dismiss) dismiss();
      }
      return true;   // 模态：一切点击不落到下层
    }
    case EventKind::KeyDown:
      if (event.key == "Escape") {
        set_visible(false);
        const auto dismiss = on_dismiss;
        if (dismiss) dismiss();
        return true;
      }
      return false;
    default: return false;
  }
}

// ————————————————— ThemePicker（按钮 + 自模态）————————————————

ThemePicker::ThemePicker() {
  set_focusable(true);
  set_hover_effect(HoverEffect{.enabled = true, .background = true, .border = false,
                               .lift = false, .glow = false, .cursor = true});
  populate_builtins("light");
}

ThemePicker::~ThemePicker() { flush_dismiss(); }

void ThemePicker::populate_builtins(std::string_view current) {
  names_.clear();
  for (const auto name : builtin_theme_names()) names_.emplace_back(name);
  set_current(current);
}

void ThemePicker::set_current(std::string_view name) {
  current_ = std::string(name);
  if (panel_ != nullptr) panel_->set_current(current_);
  mark_dirty();
}

auto ThemePicker::open() -> bool {
  if (dismiss_pending_) flush_dismiss();
  if (open_ || names_.empty()) return open_;
  // 自模态：经宿主契约挂 FillViewport 浮层。未上树/宿主未接 → 如实 false。
  if (host() == nullptr) return false;
  auto panel = std::make_unique<ThemePanel>(names_, current_);
  panel->set_id(std::string(id().empty() ? "theme-panel" : std::string(id()) + "-panel"));
  panel->on_pick = [this](std::string_view name) { handle_pick(name); };
  panel->on_dismiss = [this]() { close(); };
  ThemePanel* raw = panel.get();
  if (!host()->host_add_overlay(std::move(panel), UiRoot::OverlayLayout::FillViewport)) {
    return false;
  }
  panel_ = raw;
  open_ = true;
  return true;
}

void ThemePicker::close() {
  if (!open_) return;
  open_ = false;
  dismiss_pending_ = false;   // 下面立即摘，不再需要延迟簿记
  if (panel_ != nullptr) {
    panel_->set_visible(false);
    // **立即从 overlay 摘除**（不能等绘制期 flush）：本组件在标题栏里是 invisible
    // 逻辑部件，`paint_content` 不会被调——延迟摘除永远不触发，遮罩就成了
    // 「挂着但 invisible」的壳：`intercepts_input()` 跟着变 false，下层内容
    // 全部可点——用户看到的就是“模态弹窗点击穿透到下方按钮”。
    // `host_remove_overlay` 在事件栈内也安全（UiRoot 延迟析构，见其注释）。
    if (host() != nullptr) {
      host()->host_remove_overlay(panel_);
      panel_ = nullptr;
    } else {
      dismiss_pending_ = true;   // 无宿主（未上树）：留簿记给 flush 兕底
    }
  }
  mark_dirty();
}

void ThemePicker::flush_dismiss() const {
  if (!dismiss_pending_) return;
  dismiss_pending_ = false;
  if (panel_ != nullptr && host() != nullptr) {
    host()->host_remove_overlay(panel_);
    panel_ = nullptr;
  }
}

void ThemePicker::handle_pick(std::string_view name) {
  current_ = std::string(name);
  open_ = false;
  mark_dirty();
  // 选定即收（同 close 的口径：立即摘，不依赖绘制期 flush——见 close 注释）。
  if (panel_ != nullptr) {
    panel_->set_visible(false);
    if (host() != nullptr) {
      host()->host_remove_overlay(panel_);
      panel_ = nullptr;
    }
  }
  // 真切换走宿主契约（UiRoot→Application::set_theme_name：合成/覆盖叠加/换调色一致）。
  // **on_change 是“选中通知”**（宿主自接，做持久化等副作用）——与切换是两件事：
  // 有宿主时两者都触发，无宿主时回落 on_change（宿主自负责切换）。
  if (host() != nullptr) (void)host()->host_apply_theme(name);
  const auto callback = on_change;
  if (callback) callback(name);
}

void ThemePicker::apply_theme(const Theme& theme) {
  Element::apply_theme(theme);
  if (panel_ != nullptr) panel_->apply_theme(theme);
}

void ThemePicker::measure(const RenderContext& context, const Constraints& constraints) {
  (void)context;
  const float side = 24.0f;
  float width = style_.width > 0.0f ? style_.width : side;
  float height = style_.height > 0.0f ? style_.height : side;
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
  if (constraints.max_height < kUnbounded) height = std::min(height, constraints.max_height);
  measured_ = math::Size{width, height};
}

void ThemePicker::arrange(const RenderContext& context, math::Rect rect) {
  Element::arrange(context, rect);
}

void ThemePicker::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  flush_dismiss();
  if (bounds_.is_empty()) return;
  const Palette& colors = context.theme.colors();
  const Metrics& metrics = context.theme.metrics();
  const float radius = style_.radius > 0.0f ? style_.radius : metrics.radius_sm;

  canvas.fill_rect(bounds_, raster::Paint::solid(open_ || focused_ ? colors.surface_alt
                                                                   : colors.surface),
                   radius);
  const float icon_size = 16.0f;
  const math::Rect icon_box{bounds_.x + (bounds_.width - icon_size) * 0.5f,
                            bounds_.y + (bounds_.height - icon_size) * 0.5f, icon_size,
                            icon_size};
  Icon::draw(canvas, "brush", icon_box, open_ ? colors.primary : colors.text_muted);
  if (focused_) paint_focus_ring(context, canvas, bounds_, radius);
}

auto ThemePicker::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  if (!enabled_) return false;
  switch (event.kind) {
    case EventKind::HoverIn:
    case EventKind::HoverOut:
      mark_dirty();
      return false;
    case EventKind::MouseDown:
    case EventKind::MouseUp:
    case EventKind::Click:
    case EventKind::DoubleClick:
      if (open_) close();
      else (void)open();
      return true;
    case EventKind::KeyDown: {
      const std::string& key = event.key;
      if (key == "Enter" || key == " " || key == "Space") {
        if (open_) close();
        else (void)open();
        return true;
      }
      if (key == "Escape" && open_) {
        close();
        return true;
      }
      return false;
    }
    default: return false;
  }
}
void ThemePicker::activate() {
  if (open_) close();
  else (void)open();
}

auto ThemePicker::semantics_text() const -> std::string { return "主题"; }

auto ThemePicker::semantics_value() const -> std::string {
  return std::string(builtin_theme_display_name(current_));
}

auto ThemePicker::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "value") return current_;
  if (name == "label" || name == "text") {
    return std::string(builtin_theme_display_name(current_));
  }
  if (name == "options") {
    std::string joined;
    for (const auto& theme_name : names_) {
      if (!joined.empty()) joined.push_back('|');
      joined.append(theme_name);
    }
    return joined;
  }
  if (name == "open") return std::string(open_ ? "true" : "false");
  return std::nullopt;
}

auto ThemePicker::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "value" || name == "label" || name == "text") {
    set_current(value);
    return true;
  }
  if (name == "open") {
    const auto parsed = st::parse_bool(value);
    if (!parsed.has_value()) return false;
    if (*parsed) return open();
    close();
    return true;
  }
  return false;
}

auto ThemePicker::property_names() const -> std::vector<std::string_view> {
  return {"value", "label", "text", "options", "open"};
}

auto ThemePicker::invoke_action(std::string_view action, std::string_view argument)
    -> bool {
  if (action == "change") {
    for (const auto& theme_name : names_) {
      if (theme_name == argument) {
        handle_pick(argument);
        return true;
      }
    }
    return false;
  }
  if (action == "open") return open();
  if (action == "close") {
    close();
    return true;
  }
  if (action == "toggle") {
    if (open_) close();
    else return open();
    return true;
  }
  return Element::invoke_action(action, argument);
}

}  // namespace st::ui
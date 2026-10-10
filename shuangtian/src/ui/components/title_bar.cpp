#include "st/ui/components/title_bar.hpp"

#include <algorithm>
#include <utility>

#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"
#include "st/ui/icon.hpp"
#include "st/ui/text_port.hpp"

#include "components_internal.hpp"

namespace st::ui {

using components_internal::text_port_of;

namespace {

/// 三个控制按钮的图标：最小化 / 最大化（未最大化）/ 还原（已最大化）/ 关闭。
/// 后两个是同一按钮的两种形态——**画面跟随窗口状态**，跨平台一致。
///
/// `restore` 是"两个叠放的方框"（与系统标题栏的还原图标同构）。
/// 它曾经与 `k_icon_maximize` 共用 `square`：两种形态像素上一模一样，
/// 用户看不出按钮此刻是哪个动作——图标墙（gallery「图标全集」）把图形与名字并排，
/// 这类"共用一个形状"的问题才显形。
constexpr std::string_view k_icon_minimize = "minus";
constexpr std::string_view k_icon_maximize = "square";
constexpr std::string_view k_icon_restore = "restore";
constexpr std::string_view k_icon_close = "close";

/// 控制按钮个数（最小化 / 最大化 / 关闭）。
constexpr std::size_t k_control_count = 3;

/// 控制按钮下标（可读性：命中与绘制都按它取）。
constexpr std::size_t k_button_minimize = 0;
constexpr std::size_t k_button_maximize = 1;
constexpr std::size_t k_button_close = 2;

/// 按钮图标边长（逻辑像素）：46×40 的按钮里居中放 14px 图标，命中区够大、观感不臃肿。
///
/// ⚠ 两个图标尺寸常量**分列**（而非共用）：按钮图标与标题图标虽然常取同一档 14/16px，
/// 但它们的角色不同（一个是控件图形、一个是品牌标识），合并成一个常量后
/// "调整标题图标"会静默改掉按钮图标——这正是同类缺陷的常见来源。
constexpr float k_icon_size = 14.0f;
/// 标题图标边长。
constexpr float k_title_icon_size = 16.0f;
/// 标题栏内边距（左右）。
constexpr float k_padding_x = 12.0f;
/// 图标与标题之间的间距。
constexpr float k_icon_gap = 8.0f;
/// 关闭按钮悬浮时的底色透明度（danger 色派生；系统惯例——它是最"危险"的那一个）。
constexpr float k_close_hover_alpha = 0.16f;
/// 普通控制按钮悬浮/按压底色透明度（`surface_pressed` 派生）。
constexpr float k_button_hover_alpha = 0.10f;
constexpr float k_button_press_alpha = 0.18f;

/// 悬停块的**纵向**内缩（逻辑 px）：横向贴满按钮，纵向各缩 4。
///
/// 为什么必须缩：按钮高 = 栏高（40），悬停块若同高就与栏的上下分界线贴死，
/// 看着像"栏被切了一刀"；缩 4 后块高 **32**——正好是 Windows 11 标题栏
/// 控制按钮的高度，块与栏边之间留出同一口气。
constexpr float k_control_hover_inset_y = 4.0f;
/// 悬停块圆角（逻辑 px）：块高 32 + 半径 4，是系统标题栏的观感。
constexpr float k_control_hover_radius = 4.0f;

/// 悬停中性底：在主题给的三档中性面里，挑**与标题栏底色反差最大、且方向正确**的一档。
///
/// 为何要"选"而不是写死一个 token——这里踩过一个坑，值得记下：
/// 原先用 `surface_pressed.with_alpha_f(0.10)` 叠层，而标题栏底色 `surface_alt`
/// 本身就是**不透明**的。alpha 不透明底上不产生任何差异：叠上去实测只差 **1/255**
/// （对比度 1.0055），等于**悬停完全没有反馈**；关闭按钮之所以看得见，
/// 纯粹因为它用的是危险色相（色相变化不吃明度）。
///
/// 修法两条：① **不透明铺**（直接给合成结果），不用罩层；
/// ② 取与底色反差最大的那一档。第二点是关键：`surface_hover`（"名义上的悬停档"）
/// 与亮色主题的 `surface_alt` 只差 3/255，本来就指望不上；而在深色主题下
/// `surface_sunken` 比栏更暗（看着像"挖了个洞"）——所以按底色亮度选方向：
/// 栏偏亮就取更暗的一档，栏偏暗就取更亮的一档。
[[nodiscard]] auto neutral_hover_tint(const Palette& colors) -> math::Color {
  const auto luma = [](math::Color color) -> float {
    return static_cast<float>(color.r) * 0.299f + static_cast<float>(color.g) * 0.587f +
           static_cast<float>(color.b) * 0.114f;
  };
  const float bar = luma(colors.surface_alt);
  const bool bar_is_light = bar >= 128.0f;
  const std::array<math::Color, 3> candidates{colors.surface_hover, colors.surface_pressed,
                                             colors.surface_sunken};
  math::Color best = colors.surface_pressed;
  float best_delta = 0.0f;
  for (const math::Color& candidate : candidates) {
    const float delta = luma(candidate) - bar;
    // 方向必须正确（与 `Element::paint_box` 的"同向提亮/压暗"同一口径）。
    if (bar_is_light ? delta >= 0.0f : delta <= 0.0f) continue;
    if (std::abs(delta) > best_delta) {
      best_delta = std::abs(delta);
      best = candidate;
    }
  }
  return best;
}

/// 圆角填充（按钮底色的统一画法；半径取 `min(radius_sm, 矩形短边一半)`）。
void fill_rounded(raster::Surface& canvas, math::Rect rect, float radius, math::Color color) {
  if (rect.is_empty() || color.a == 0U) return;
  raster::Path path;
  const float limit = std::min(rect.width, rect.height) * 0.5f;
  path.add_rounded_rect(rect, std::min(radius, limit));
  canvas.fill_path(path, raster::Paint::solid(color));
}

}  // namespace

TitleBar::TitleBar(std::string title) : title_(std::move(title)) {
  // 标题栏本身不参与 Tab 焦点环：它是窗框，键盘可达性由宿主（快捷键/菜单）负责。
  // （与系统标题栏一致：Alt+Space 打开系统菜单，而不是 Tab 到"最小化"上。）
  set_focusable(false);
  // **关掉基类的悬浮/按下回放**：本组件整条都是“可交互”的（拖动、双击、三个按钮），
  // 而 `pressed_` 会在**鼠标按在栏上任何位置**时置位（拖动窗口的第一步就是按在栏上）。
  // 基类的按下样式会压暗 `style_.background`，而那是**整条标题栏的底色**——
  // 实测：按住标题栏拖动时整条栏从 #EDEDF2 变成 #DADADF（x=400/1200 同时变），
  // 看着像“整个窗框闪了一下”。按钮自己的悬停/按下由 `paint_content` 逐按钮画。
  set_hover_effect(HoverEffect{.enabled = false});
}

void TitleBar::set_title(std::string title) {
  if (title_ == title) return;
  title_ = std::move(title);
  mark_dirty();
}

void TitleBar::set_icon(std::string icon) {
  if (icon_ == icon) return;
  icon_ = std::move(icon);
  mark_layout_dirty();
}

void TitleBar::set_show_controls(bool show) {
  if (show_controls_ == show) return;
  show_controls_ = show;
  mark_layout_dirty();
}

auto TitleBar::control_button_rect(std::size_t index) const -> math::Rect {
  const float size = control_button_size();
  // 从右缘往左排：关闭在最右，与系统标题栏的按钮顺序同一约定。
  const float right = bounds_.right() - (static_cast<float>(k_control_count - 1 - index) * size);
  return math::Rect{right - size, bounds_.y, size, std::min(bounds_.height, size * 2.0f)};
}

auto TitleBar::controls_rect() const -> math::Rect {
  if (!show_controls_) return {};
  const float width = control_button_size() * static_cast<float>(k_control_count);
  return math::Rect{bounds_.right() - width, bounds_.y, width, bounds_.height};
}

auto TitleBar::caption_rect() const -> math::Rect {
  // 标题**文字**区：从 `leading` 槽之后起（不盖住品牌名），到尾部槽为止。
  // 按需算而不是缓存矩形：右界依赖 `show_controls` 等属性，缓存了就会出现
  // "改属性未重排 → 几何还是旧的"（实测：单测正是这么撞出来的）。
  const float right = slots_left_ == 0.0f ? controls_left() : slots_left_;
  return math::Rect{title_left_, bounds_.y, std::max(0.0f, right - title_left_), bounds_.height};
}

auto TitleBar::drag_rect() const -> math::Rect {
  // 拖动带从窗口**左缘**起（与系统标题栏的 `HTCAPTION` 一致：图标/内边距那一段也算，
  // 用户不会精确点到文字上），右界到尾部槽之前。
  //
  // ⚠ 这只是**粗略**的拖动带：附属槽占住的那几块要从里面挖掉（`hits_caption` 负责），
  // 本函数保留“从左缘到尾部槽的连续带”语义（几何单调，供断言与视觉核对复用）。
  const float right = slots_left_ == 0.0f ? controls_left() : slots_left_;
  return math::Rect{bounds_.x, bounds_.y, std::max(0.0f, right - bounds_.x), bounds_.height};
}

auto TitleBar::hits_caption(math::Point point) const -> bool {
  if (!drag_rect().contains(point)) return false;
  // **附属槽不是拖动区**（头文件的契约；`leading` 与 `trailing` 同样适用）。
  //
  // 为什么必须逐块挖掉、而不是“只排除尾部槽”：`leading` 槽排在标题文字之前，
  // 正好落在“从左缘起”的拖动带里，于是挂在上面的宿主控件**双击**会冒泡到本组件，
  // 而双击的语义是“最大化/还原”。实测（gbcode 把菜单栏挂进 `leading` 槽）：
  // 双击「文件」菜单会把窗口最大化——单击/按下被控件自己消费，唯独双击漏出来。
  for (const Element* slot : leading_) {
    if (slot != nullptr && slot->bounds().contains(point)) return false;
  }
  for (const Element* slot : trailing_) {
    if (slot != nullptr && slot->bounds().contains(point)) return false;
  }
  return true;
}

auto TitleBar::add_trailing(std::unique_ptr<Element> child) -> Element* {
  if (child == nullptr) return nullptr;
  Element* raw = Element::add_child(std::move(child));
  trailing_.push_back(raw);
  mark_layout_dirty();
  return raw;
}

auto TitleBar::add_leading(std::unique_ptr<Element> child) -> Element* {
  if (child == nullptr) return nullptr;
  // leading 排在自持部件之前（它就在标题前），位置 = 当前 leading 数
  Element* raw = Element::insert_child(leading_.size(), std::move(child));
  leading_.push_back(raw);
  mark_layout_dirty();
  return raw;
}

auto TitleBar::add_child(std::unique_ptr<Element> child) -> Element* {
  // 默认归到**尾部槽**：声明式与协议里最常见的用法就是把控件挂进标题栏右侧。
  return add_trailing(std::move(child));
}

auto TitleBar::insert_child(std::size_t index, std::unique_ptr<Element> child) -> Element* {
  (void)index;
  return add_trailing(std::move(child));
}

/// 单行横排：`[标题带][leading][trailing][控制按钮]`（measure/arrange 共用）。
///
/// **为什么要自己排**：本组件既是窗框（要置顶、要全宽），又要**自带**一行横向布局
/// （标题 + 附属控件 + 控制按钮）。交给父容器的行布局意味着父容器必须是行布局、
/// 且知道"标题栏多高、内边距多少"这类约定——那正是"每个应用各写一套外壳"的来源。
void TitleBar::arrange(const RenderContext& context, math::Rect rect) {
  Element::arrange(context, rect);
  const float controls_left =
      show_controls_ ? bounds_.right() - control_button_size() * static_cast<float>(k_control_count)
                     : bounds_.right();
  const float head_left =
      bounds_.x + k_padding_x + (icon_.empty() ? 0.0f : k_title_icon_size + k_icon_gap);
  // 尾部槽（菜单/主题等宿主控件）：从控制按钮左侧往左排。
  float tail_cursor = controls_left;
  float slots_left = controls_left;
  for (auto iterator = trailing_.rbegin(); iterator != trailing_.rend(); ++iterator) {
    Element* slot = *iterator;
    if (slot == nullptr) continue;
    const float wanted = std::min(std::max(0.0f, slot->measured_size().width),
                                  std::max(0.0f, tail_cursor - head_left));
    tail_cursor -= wanted;
    slot->arrange(context, math::Rect{tail_cursor, bounds_.y, wanted, bounds_.height});
    slots_left = tail_cursor;
    tail_cursor -= k_icon_gap;   // 槽之间的间隙
  }
  // 前部槽：从**前置图标之后**往右排。
  //
  // 为何不从 `head_left` 起：`head_left` 已经把图标宽度算进去了（见其定义），
  // 从它起排就是“图标宽度被当作空白吐掉”——`leading` 非空时图标又恰好不画，
  // 结果是标题左侧留着一块看不见的空白（gbcode 的菜单栏正是这个形态）。
  const float slots_start = head_left + (icon_.empty() ? 0.0f : k_title_icon_size + k_icon_gap);
  float head_cursor = slots_start;
  for (Element* slot : leading_) {
    if (slot == nullptr) continue;
    const float wanted = std::min(std::max(0.0f, slot->measured_size().width),
                                  std::max(0.0f, slots_left - head_cursor));
    slot->arrange(context, math::Rect{head_cursor, bounds_.y, wanted, bounds_.height});
    head_cursor += wanted + k_icon_gap;
  }
  // 标题**文字**区：从 `leading` 槽之后起（不盖住品牌名），到尾部槽为止。
  // 只记两个标量（标题起点与尾部槽左缘），矩形由 `caption_rect()`/`drag_rect()` 按需算。
  // ⚠ 右界不能取"前部槽的右缘"：那样有前部槽时标题会被算成零宽（实测：标题直接不显示）。
  title_left_ = leading_.empty() ? slots_start : head_cursor;
  // `slots_left_` 用 **0 作"无尾部槽"哨兵**：有尾部槽时它是真实边界（通常 > 0），
  // 无尾部槽时靠 `controls_left()` 现算——否则关掉控制按钮后它仍是旧值（实测撞到）。
  slots_left_ = trailing_.empty() ? 0.0f : slots_left;
}

auto TitleBar::button_icon(std::size_t index) const -> std::string_view {
  if (index == k_button_minimize) return k_icon_minimize;
  if (index == k_button_maximize) {
    const bool maximized = control_ != nullptr && control_->window_maximized();
    return maximized ? k_icon_restore : k_icon_maximize;
  }
  return k_icon_close;
}

auto TitleBar::run_window_action(std::string_view action) -> bool {
  if (control_ == nullptr || !control_->window_control_available()) return false;
  if (action == "minimize") return control_->window_minimize();
  if (action == "maximize") {
    const bool changed = control_->window_toggle_maximize();
    // 按钮形态随窗口状态变（最大化 ↔ 还原）：切换后连布局都变了（图标不同），
    // 只 `mark_dirty` 会让语义树/视觉树停在旧图标上——自动化验证会读到错的按钮形态。
    if (changed) mark_layout_dirty();
    return changed;
  }
  if (action == "close") return control_->window_request_close();
  return false;
}

void TitleBar::apply_theme(const Theme& theme) {
  const Palette& colors = theme.colors();
  const Metrics& metrics = theme.metrics();
  style_.background = colors.surface_alt;
  // **不画边框**（用户 2026-10-08 要求“标题栏的边框线去掉”）。
  //
  // 原设计在这里给了一条 1px 分界线（“窗框与内容之间的视觉依据”）。实测下它
  // 是**四周环**（`Element::paint_box` 画的是 rounded_border_ring）——左右两条竖线
  // 在窗口边缘根本看不到，看得见的只有**下边**那一条；而它与活动栏/状态栏
  // 同色之后，这条线反而把本应连成一圈的“外壳”切成两段。
  //
  // 层次改由**底色本身**区分：`surface_alt`（外壳条带）与内容区的 `bg`/`surface`
  // 本来就不同档（见 `Palette` 的“表面阶梯逐级可辨”约束，`ui_theme_test` 有断言），
  // 不需要额外描边。
  style_.border_width = 0.0f;
  style_.border_color = math::Color{0, 0, 0, 0};
  style_.radius = 0.0f;
  style_.padding = math::Insets{0.0f, k_padding_x, 0.0f, k_padding_x};
  style_.font_size = metrics.font_sm;
  style_.font_weight = FontWeight::Medium;
  style_.color = enabled_ ? colors.text : colors.text_faint;
}

void TitleBar::measure(const RenderContext& context, const Constraints& constraints) {
  apply_theme(context.theme);
  // 铺满父级：用 `available_width`（`max_width` 在行布局里是 1e9，见 `fill_width`）。
  const float width = fill_width(constraints, style_.width, 0.0f);
  const float height = style_.has_explicit_height() ? style_.height : bar_height();
  measured_ = math::Size{std::max(width, 0.0f),
                         std::clamp(height, style_.min_height, style_.max_height)};
  // 附属槽逐个测量：它们在 `arrange` 里按 `measured_size().width` 入位，
  // 而**本组件自己不 measure 子树的话，槽位就会拿到 0 宽**——表现为"挂上去的按钮
  // 一个个都看不到"（子元素必须在父的 measure 里被测量，这是本框架的布局契约）。
  const Constraints slot_constraints{kUnbounded, measured_.height, kUnbounded, measured_.height};
  for (Element* slot : leading_) {
    if (slot != nullptr) slot->measure(context, slot_constraints);
  }
  for (Element* slot : trailing_) {
    if (slot != nullptr) slot->measure(context, slot_constraints);
  }
}

void TitleBar::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  const Palette& colors = context.theme.colors();
  const Metrics& metrics = context.theme.metrics();

  // —— 1. 标题：图标 + 文字（左对齐、垂直居中、超出以省略号收尾）——
  //
  // 文字**只能画到标题带右界**（`caption_rect()`）：标题带在 `arrange` 里已经让开了
  // 附属槽与控制按钮，不按它裁的话长标题会直接叠在宿主控件/按钮上。
  // 图标画在文字区**左侧**（有 `leading` 槽时图标让位给槽，两者都画会叠在一起）。
  const math::Rect caption = caption_rect();
  float text_left = caption.x;
  if (!icon_.empty()) {
    const math::Rect icon_box{bounds_.x + k_padding_x,
                              bounds_.y + (bounds_.height - k_title_icon_size) * 0.5f,
                              k_title_icon_size, k_title_icon_size};
    Icon::draw(canvas, icon_, icon_box, style_.color, 0.0f);
  }
  const float text_width = std::max(0.0f, caption.right() - text_left - metrics.space_sm);
  if (text_width > 1.0f) {
    const TextPort& port = text_port_of(context);
    const std::string clipped = port.ellipsize(title_, style_.font_size, text_width);
    if (!clipped.empty()) {
      const float line = port.line_height(style_.font_size);
      port.draw(canvas, clipped,
                math::Point{text_left, bounds_.y + (bounds_.height - line) * 0.5f},
                style_.font_size, style_.color);
    }
  }

  // —— 2. 窗口控制按钮（右端，最小化 / 最大化·还原 / 关闭）——
  if (!show_controls_) return;
  for (std::size_t index = 0; index < k_control_count; ++index) {
    const math::Rect rect = control_button_rect(index);
    const bool hovered = hovered_button_ == static_cast<int>(index);
    const bool pressed = pressed_button_ == static_cast<int>(index);
    const bool is_close = index == k_button_close;
    // 悬停块：横向贴满、纵向各缩 `k_control_hover_inset_y`（块高 32 = 系统按钮高）。
    const math::Rect block{rect.x, rect.y + k_control_hover_inset_y, rect.width,
                           std::max(0.0f, rect.height - k_control_hover_inset_y * 2.0f)};
    math::Color tone = style_.color;
    if (!block.is_empty() && (hovered || pressed)) {
      if (is_close) {
        // —— 关闭：**实心 danger 底 + 白叉**（Windows 11 的形态）——
        //
        // 为何不用半透明 danger 罩层：那是本组件此前的写法，实测对比度
        // 1.0564、视觉上就是"一抹淡红"；而实心 danger 与标题栏底反差 **2.11**，
        // 是全窗唯一不可逆动作应有的提示强度（色相与明度**两个正交量同时动**）。
        // 它也因此不依赖底色亮度——深色主题下同样成立。
        fill_rounded(canvas, block, k_control_hover_radius, colors.danger);
        tone = colors.on_primary;
        if (pressed) tone = tone.darken(0.08f);
      } else {
        // —— 普通按钮：不透明铺一档中性色（见 `neutral_hover_tint` 的说明）——
        const math::Color tint = neutral_hover_tint(colors);
        math::Color fill = tint;
        if (pressed) {
          // 按下比悬停再深一档，且不透明度抬满：否则"按下去了"看不出来。
          fill = tint.darken(0.08f);
          fill.a = 255U;
        }
        fill_rounded(canvas, block, k_control_hover_radius, fill);
      }
    }
    const math::Rect glyph{
        rect.x + (rect.width - k_icon_size) * 0.5f,
        rect.y + (rect.height - k_icon_size) * 0.5f, k_icon_size, k_icon_size};
    Icon::draw(canvas, button_icon(index), glyph, tone, 0.0f);
  }
}

auto TitleBar::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  switch (event.kind) {
    case EventKind::MouseDown: {
      if (event.button != 1) return false;
      // ① 控制按钮：记下按压（高亮在 paint 里出），松开时才真正触发——
      //    与系统标题栏一致（按错了可以拖出去取消）。
      if (show_controls_) {
        for (std::size_t index = 0; index < k_control_count; ++index) {
          if (control_button_rect(index).contains(event.position)) {
            pressed_button_ = static_cast<int>(index);
            mark_dirty();
            return true;
          }
        }
      }
      // ② 边缘带 → 缩放。**判定用纯函数**（与后端同一份 `ui::resize_edge_at`）。
      //    逻辑尺寸由本元素自己提供：组件不需要知道窗口是谁（跨平台无差异）。
      const math::Size size{bounds_.width, bounds_.height};
      const WindowEdge edge =
          resize_edge_at(math::Point{event.position.x - bounds_.x, event.position.y - bounds_.y},
                         size, kWindowResizeBorder);
      if (edge != WindowEdge::None && control_ != nullptr && control_->window_control_available()) {
        if (control_->window_begin_resize(edge)) return true;
        // 后端不支持（如 Win32：缩放由 `WM_NCHITTEST` 在窗口层接管）→ 落到拖动分支，
        // **不能就此吞掉**：那样边缘就成了"死区"，窗口反而变得拖不动。
      }
      // ③ 标题条带 → 拖动窗口。
      if (hits_caption(event.position)) {
        if (control_ != nullptr && control_->window_control_available()) {
          (void)control_->window_begin_move();
        }
        return true;   // 无论后端是否支持都消费：标题栏上的按下不该穿透到下层界面
      }
      return false;
    }
    case EventKind::MouseUp: {
      if (pressed_button_ < 0) return false;
      const auto pressed = static_cast<std::size_t>(pressed_button_);
      pressed_button_ = -1;
      mark_dirty();
      // 松开位置仍在该按钮内才触发（拖出去 = 取消，系统同款语义）。
      if (!control_button_rect(pressed).contains(event.position)) return true;
      if (pressed == k_button_minimize) {
        (void)run_window_action("minimize");
      } else if (pressed == k_button_maximize) {
        (void)run_window_action("maximize");
      } else {
        (void)run_window_action("close");
      }
      return true;
    }
    case EventKind::DoubleClick: {
      if (!hits_caption(event.position)) return false;
      if (on_double_click_) {
        on_double_click_();
      } else {
        (void)run_window_action("maximize");
      }
      return true;
    }
    case EventKind::MouseMove: {
      // 悬浮态：按钮高亮（`hovered_` 由 UiRoot 维护在元素级，按钮级需要自己算）。
      int hovered = -1;
      if (show_controls_) {
        for (std::size_t index = 0; index < k_control_count; ++index) {
          if (control_button_rect(index).contains(event.position)) {
            hovered = static_cast<int>(index);
            break;
          }
        }
      }
      if (hovered != hovered_button_) {
        hovered_button_ = hovered;
        mark_dirty();
      }
      return false;   // 不消费：移动事件继续冒泡（父容器/根仍要看到）
    }
    case EventKind::HoverOut: {
      if (hovered_button_ != -1) {
        hovered_button_ = -1;
        mark_dirty();
      }
      return false;
    }
    default: break;
  }
  return false;
}

auto TitleBar::hit_test(math::Point point) const noexcept -> bool {
  return bounds_.contains(point);
}

auto TitleBar::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "title" || name == "text") return title_;
  if (name == "icon") return icon_;
  if (name == "show_controls") return std::string(show_controls_ ? "true" : "false");
  // 窗口状态是"只能从像素看出来"的那类量：自动化验证需要能直接读到它
  // （`maximized` 决定最大化按钮此刻是"最大化"还是"还原"图标）。
  if (name == "maximized") {
    return std::string(control_ != nullptr && control_->window_maximized() ? "true" : "false");
  }
  if (name == "window_control_available") {
    return std::string(control_ != nullptr && control_->window_control_available() ? "true"
                                                                                   : "false");
  }
  return std::nullopt;
}

auto TitleBar::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "title" || name == "text") {
    set_title(std::string(value));
    return true;
  }
  if (name == "icon") {
    set_icon(std::string(value));
    return true;
  }
  if (name == "show_controls") {
    set_show_controls(value == "true" || value == "1" || value == "on");
    return true;
  }
  return false;
}

auto TitleBar::property_names() const -> std::vector<std::string_view> {
  return {"title", "text", "icon", "show_controls", "maximized", "window_control_available"};
}

auto TitleBar::invoke_action(std::string_view action, std::string_view argument) -> bool {
  (void)argument;
  // 动作返回值**如实反映能力**：无头/后端不支持时返回 false，调用方（协议 `invoke`、
  // 脚本、自动化验证）据此区分"点了没反应"与"环境没有这个能力"。
  return run_window_action(action);
}

}  // namespace st::ui

#include "st/ui/components/basic.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <functional>
#include <string>

#include "st/core/print.hpp"
#include "st/core/string.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"
#include "components_internal.hpp"
#include "st/ui/icon.hpp"
#include "st/ui/line_layout.hpp"
#include "st/ui/text_port.hpp"

namespace st::ui {
namespace {

using components_internal::paint_focus_ring;

[[nodiscard]] auto port_of(const RenderContext& context) -> const TextPort& {
  static const NullTextPort fallback;
  return context.text != nullptr ? *context.text : fallback;
}

/// 未知图标名**提示一次**（同一名字只报一次）。
///
/// 为什么需要（实测踩到）：图标名写错时 `Icon::has` 为假 → **静默不画**，
/// 按钮看着就是"空白/少一块"，没有任何线索指向"名字错了"。
/// 实测那次：活动栏用了 `diff`，而图标表里没有它——按钮位置空着，
/// 排查先怀疑渲染、再怀疑布局，最后才发现是"名字不存在"。
///
/// 报一次而不是每帧报：绘制每帧都跑，不去的化会把日志刷满。
/// 用 `set` 记已报过的名字（诊断基建，量级很小）。
void warn_unknown_icon_once(const std::string& name) {
  if (name.empty() || Icon::has(name)) return;
  // 去重状态用**函数内 atomic 位图**（图标名数量有限：`Icon` 表几十个）——
  // 避免可变全局状态（L8），也避免 `set` 的动态分配进入绘制热路径。
  //
  // 为什么不去重就用 `static bool`：绘制每帧都跑，不去重会把日志刷满
  //（实测：一帧一次，2 秒跑出几百行）。
  // lint-allow: L8 诊断去重（进程级且只看自己：128 位标志，无业务状态）。
  // 为什么不做成注入式：`paint_content` 是绘制热路径，为一条"名字写错"的提示
  // 给 `RenderContext` 加一个诊断槽位、再让每处调用点透传，代价远大于收益——
  // 而这段状态**不参与任何业务逻辑**，只影响"同一名字报几次"。
  constexpr std::size_t kMaxTracked = 128;
  static std::array<std::atomic<bool>, kMaxTracked> reported{};
  // 名字 → 槽位：用名字哈希（同一名字稳定落同一槽；碰撞只会"少报一次"，
  // 对诊断来说可接受——真正要紧的是"这个名字有问题"被说出来）。
  const std::size_t slot = std::hash<std::string>{}(name) % kMaxTracked;
  bool expected = false;
  if (!reported[slot].compare_exchange_strong(expected, true)) return;
  st::eprint("[ui] 未知图标名 `{}`（该按钮的图标不会绘制）——图标需登记在 `Icon` 表"
             "（src/ui/icon.cpp）里，`examples/*/assets/icons.svg` 那份是另一套",
             name);
}


}  // namespace

// —— Text ——

Text::Text(std::string content) : content_(std::move(content)) {}

void Text::set_content(std::string content) {
  if (content_ == content) return;
  content_ = std::move(content);
  mark_layout_dirty();
}

void Text::set_multiline(bool multiline) {
  if (multiline_ == multiline) return;
  multiline_ = multiline;
  mark_layout_dirty();
}

void Text::set_max_lines(std::size_t lines) {
  max_lines_ = lines;
  mark_layout_dirty();
}

void Text::set_tone(Tone tone) {
  tone_ = tone;
  mark_dirty();
}

void Text::set_font_size(float size) {
  font_size_override_ = size;
  mark_layout_dirty();
}

void Text::set_weight(FontWeight weight) {
  weight_ = weight;
  mark_dirty();
}

void Text::apply_theme(const Theme& theme) {
  style_.color = text_tone_override().has_value() ? tone_color(theme, *text_tone_override())
                                                  : tone_color(theme, tone_);
  style_.font_size = font_size_override_ > 0.0f ? font_size_override_ : theme.metrics().font_base;
  style_.font_weight = weight_;
  style_.background = math::Color{0, 0, 0, 0};
  // 显式排版覆盖（DSL `BoxProps::color/hex_color/size/weight`）**最后**回放——
  // 放末尾就不必逐个字段去问“这个是不是被显式设过”
  apply_text_overrides();
}

void Text::measure(const RenderContext& context, const Constraints& constraints) {
  const TextPort& port = port_of(context);
  const float size = style_.font_size;
  const float line_height = port.line_height(size);
  if (multiline_) {
    const float available = constraints.max_width < kUnbounded ? constraints.max_width : 480.0f;
    std::vector<std::string> lines = max_lines_ > 0
                                         ? port.wrap_limited(content_, size, available, max_lines_)
                                         : std::vector<std::string>{};
    if (max_lines_ == 0) {
      for (const auto& line : port.wrap(content_, size, available)) {
        lines.emplace_back(line);
      }
    }
    float widest = 0.0f;
    for (const auto& line : lines) widest = std::max(widest, port.measure_width(line, size));
    measured_ = math::Size{std::min(widest, available),
                           line_height * static_cast<float>(lines.empty() ? 1 : lines.size())};
    return;
  }
  const float width = port.measure_width(content_, size);
  measured_ = math::Size{std::min(width, constraints.max_width), line_height};
}

void Text::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (content_.empty()) return;
  const TextPort& port = port_of(context);
  const float size = style_.font_size;
  if (!multiline_) {
    paint_text(context, canvas, content_, bounds_);
    return;
  }
  const float available = bounds_.width;
  std::vector<std::string> lines;
  if (max_lines_ > 0) {
    lines = port.wrap_limited(content_, size, available, max_lines_);
  } else {
    for (const auto& line : port.wrap(content_, size, available)) lines.emplace_back(line);
  }
  const float line_height = port.line_height(size);
  float y = bounds_.y;
  for (const auto& line : lines) {
    const float width = port.measure_width(line, size);
    float x = bounds_.x;
    if (style_.text_align == TextAlign::Center) {
      x = bounds_.x + (bounds_.width - width) * 0.5f;
    } else if (style_.text_align == TextAlign::End) {
      x = bounds_.right() - width;
    }
    // **字重必须一起传**（与 `Element::paint_text` 同口径）：多行分支漏传会让
    // 段落/多行文本的粗体**静默退回 Regular**——实测踩到过。
    const float physical = size * canvas.device_scale();
    const bool real_bold = prefers_real_bold(style_.font_weight) && port.has_real_bold();
    const float embolden = real_bold ? 0.0f : embolden_radius(physical, style_.font_weight);
    port.draw(canvas, line, math::Point{x, y}, size, style_.color,
              text::FontRole::Proportional, embolden, real_bold);
    y += line_height;
  }
}

// —— Heading ——

auto Text::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "text" || name == "content") return content_;
  if (name == "multiline") return multiline_ ? "true" : "false";
  return std::nullopt;
}

auto Text::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "text" || name == "content") {
    set_content(std::string(value));
    return true;
  }
  if (name == "multiline") {
    set_multiline(value == "true" || value == "1");
    return true;
  }
  return false;
}

auto Text::property_names() const -> std::vector<std::string_view> {
  return {"text", "content", "multiline"};
}

Heading::Heading(std::string content, std::uint32_t level) : Text(std::move(content)) {
  set_level(level);
}

void Heading::set_level(std::uint32_t level) {
  level_ = level < 1 ? 1 : (level > 6 ? 6 : level);
  mark_layout_dirty();
}

void Heading::apply_theme(const Theme& theme) {
  const Metrics& metrics = theme.metrics();
  float size = metrics.font_2xl;
  switch (level_) {
    case 1: size = metrics.font_3xl; break;
    case 2: size = metrics.font_2xl; break;
    case 3: size = metrics.font_xl; break;
    case 4: size = metrics.font_lg; break;
    default: size = metrics.font_base; break;
  }
  style_.font_size = size;
  style_.font_weight = level_ <= 2 ? FontWeight::Bold : FontWeight::SemiBold;
  style_.color = theme.colors().text;
  style_.background = math::Color{0, 0, 0, 0};
  apply_text_overrides();
}

// —— IconView ——

IconView::IconView(std::string name, float size) : icon_(std::move(name)), size_(size) {}

void IconView::set_icon(std::string name) {
  icon_ = std::move(name);
  mark_dirty();
}

void IconView::set_size(float size) {
  size_ = size;
  mark_layout_dirty();
}

void IconView::set_tone(Tone tone) {
  tone_ = tone;
  mark_dirty();
}

void IconView::apply_theme(const Theme& theme) {
  style_.color = text_tone_override().has_value() ? tone_color(theme, *text_tone_override())
                                                  : tone_color(theme, tone_);
  apply_text_overrides();
}

void IconView::measure(const RenderContext& context, const Constraints& constraints) {
  (void)context;
  (void)constraints;
  measured_ = math::Size{size_, size_};
}

void IconView::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  (void)context;
  if (icon_.empty()) return;
  Icon::draw(canvas, icon_, bounds_, style_.color, 0.0f);
}

// —— Button ——

auto IconView::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "icon") return icon_;
  if (name == "size") return std::format("{}", size_);
  return std::nullopt;
}

auto IconView::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "icon") {
    set_icon(std::string(value));
    return true;
  }
  if (name == "size") {
    if (const auto parsed = parse_f64(value); parsed.has_value()) {
      set_size(static_cast<float>(*parsed));
      return true;
    }
    return false;
  }
  return false;
}

Button::Button(std::string label, Variant variant, Size size)
    : label_(std::move(label)), variant_(variant), size_(size) {
  set_focusable(true);
  // 悬浮/按下的**底色变化由基类通用层统一做**（`Element::paint_box`）：
  // 那里在同向提亮/压暗的幅度上做、而且**保持色相**（主按钮仍看得出是蓝的），
  // 按下比悬停再深一档。`Button::apply_theme` 只管各变体的**常态语气**。
  //
  // **不上浮**（`.lift = false`）：按钮的悬浮反馈是“悬停高亮”，而不是“抬起”——
  // 上浮会把整块（含文字与图标）在 hover 时上移 `metrics.hover_lift`（默认 1.5px），
  // 鼠标掠过时按钮看着在**抖/跳**（用户报「按钮悬浮时不要上移」）。
  // 反馈仍由 `background` + `border` 承担（实测两者都有可见变化）。
  // 卡片这类“浮起来”的容器仍可用 `lift`（那是它们该有的手感，不是按钮的）。
  //
  // 为什么必须分开：`apply_theme` **只在 layout 时跑**（见 `DESIGN.md` §4.2.6），
  // 而悬停/按下只标重绘、不触发布局——组件里那份 `hovered_ ? ... : ...` 分支
  // 算出的色会**永远停在旧状态**（实测：真实应用里按下态与悬停态像素完全一样）。
  // 两处同时做还会**叠乘**（组件一层 + 通用层）——`Soft` 因此被洗成中性灰。
  set_hover_effect(HoverEffect{.enabled = true, .background = true, .border = true,
                               .lift = false, .glow = false, .cursor = true});
}

void Button::set_label(std::string label) {
  label_ = std::move(label);
  mark_layout_dirty();
}

void Button::set_icon(std::string icon) {
  icon_ = std::move(icon);
  mark_layout_dirty();
}

void Button::set_variant(Variant variant) {
  variant_ = variant;
  mark_dirty();
}

void Button::set_size(Size size) {
  size_ = size;
  mark_layout_dirty();
}

void Button::set_tone(Tone tone) {
  tone_ = tone;
  mark_dirty();
}

void Button::apply_theme(const Theme& theme) {
  const Palette& colors = theme.colors();
  const Metrics& metrics = theme.metrics();
  style_.radius = metrics.radius_md;
  style_.font_size = size_ == Size::Small ? metrics.font_sm
                                          : (size_ == Size::Large ? metrics.font_lg : metrics.font_base);
  // 图标盒径：三档取自 `Metrics::icon_size*`（独立令牌，不从字号推——见其定义处）。
  icon_size_ = size_ == Size::Small ? metrics.icon_size_sm
                                    : (size_ == Size::Large ? metrics.icon_size_lg : metrics.icon_size);
  style_.font_weight = FontWeight::Medium;
  style_.border_width = 0.0f;
  style_.align_items = Align::Center;
  style_.justify = Justify::Center;
  style_.direction = FlexDirection::Row;
  style_.text_align = TextAlign::Center;

  // **悬浮/按下的底色变化交给基类的通用层**（`Element::paint_box`）——
  // 那里在同向提亮/压暗的幅度上做，且**保持色相**（主按钮仍看得出是蓝的）。
  // 本函数只管"**静态语气**"：每个变体的常态色。
  //
  // 为什么不在这里按 `hovered_`/`pressed_` 分支：`apply_theme` **只在 layout 时跑**
  // （见 `DESIGN.md` §4.2.6 与 `UiRoot::layout` 的早退条件），而悬停/按下只标
  // 重绘、不触发布局——于是那些分支算出来的色**永远停在旧状态上**，真实应用里
  // 按下态根本不生效（实测：按下与悬停像素完全一样）。
  // 两处同时做还会**叠乘**（组件一层 + 通用层）——Soft 变体因此被洗成中性灰。
  const bool inactive = !enabled_;
  switch (variant_) {
    case Variant::Primary:
      style_.background = inactive ? colors.primary_soft : colors.primary;
      style_.color = inactive ? colors.primary : colors.on_primary;
      break;
    case Variant::Secondary:
      style_.background = colors.surface;
      style_.color = inactive ? colors.text_faint : colors.text;
      style_.border_width = metrics.border_width;
      style_.border_color = colors.border;
      break;
    case Variant::Ghost:
      style_.background = math::Color{0, 0, 0, 0};
      style_.color = inactive ? colors.text_faint : colors.text_muted;
      break;
    case Variant::Soft:
      style_.background = colors.primary_soft;
      style_.color = inactive ? colors.text_faint : colors.primary;
      break;
    case Variant::Danger:
      style_.background = inactive ? colors.danger.with_alpha_f(0.2f) : colors.danger;
      style_.color = colors.on_primary;
      break;
  }
  if (tone_ != Tone::Default) {
    if (tone_ == Tone::OnPrimary) {
      style_.color = colors.on_primary;
    } else if (variant_ == Variant::Ghost || variant_ == Variant::Secondary) {
      style_.color = tone_color(theme, tone_);
    }
  }
  if (style_.shadow.color.a == 0U && variant_ == Variant::Primary && !inactive) {
    // 主按钮的轻微投影提升层次
  }
  // 显式排版覆盖最后回放（与 Text 同口径）：错误提示/危险按钮想标红时，
  // 不必再为“把颜色写到按钮上”去开后门
  apply_text_overrides();
}

void Button::measure(const RenderContext& context, const Constraints& constraints) {
  const Metrics& metrics = context.theme.metrics();
  const float height = size_ == Size::Small ? metrics.control_height_sm
                                            : (size_ == Size::Large ? metrics.control_height_lg
                                                                    : metrics.control_height);
  const float horizontal_padding = size_ == Size::Small ? metrics.space_md : metrics.space_lg;
  const TextPort& port = port_of(context);
  float width = port.measure_width(label_, style_.font_size) + horizontal_padding * 2.0f;
  warn_unknown_icon_once(icon_);
  // 图标宽度只在图标**真画得出来**时计入——与 `paint_content` 同一判据
  // （名字无效时那里不画、这里若算了宽度，按钮就会宽出一截且内容偏移）。
  // 间距只在**图标与文字同时存在**时计入（与 `paint_content` 的 `gap` 同规则）；
  // 纯图标按钮只有图标本身。
  //
  // ⚠ **按图标实际墨迹宽计，不是盒宽**：`Icon::path` 会把墨迹等比缩放并
  // **居中**到盒里，于是盒两侧有透明留白（描边式图标约 30%）。按盒宽排版
  // 会让那些留白也占位置——而绘制时墨迹居中，重心因此偏向文字一侧。
  // 实测（图标+文字按钮）：偏移恰为 `(盒宽 − 墨迹宽) / 4`，且**随盒变大而线性变大**
  // （图标盒 16→20 时偏差从 0.5 涨到 1.5px）。按墨迹排版后两个量同一个事实。
  if (!icon_.empty() && Icon::has(icon_)) {
    width += icon_ink_size().width;
    if (!label_.empty()) width += metrics.space_sm;
  }
  width = std::max(width, height);
  // **显式宽度优先**：与其它组件同口径（`style_.width` 是宿主/DSL 的明确意图）。
  if (style_.has_explicit_width()) width = style_.width;
  // **约束要听**：旧实现在这里 `(void)constraints`——宽度只看标签文本，
  // 于是窄容器里的长标签按钮**横向溢出**到相邻面板之上（实测：侧栏 Git 变更项
  // 叠在代码编辑器上面，看起来像绘制错乱，实质是测量忽略了可用宽度）。
  // 夹到 `constraints` 后，超长文本由 `paint_content` 末位省略号收束。
  if (constraints.max_width < kUnbounded) width = std::min(width, constraints.max_width);
  if (style_.min_width > 0.0f) width = std::max(width, style_.min_width);
  width = std::min(width, style_.max_width);
  measured_ = math::Size{width, height};
}

void Button::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  const Metrics& metrics = context.theme.metrics();
  const TextPort& port = port_of(context);
  const float font_size = style_.font_size;
  // 图标盒径：`apply_theme` 写入的档位值（与 `measure` 里**同一个量**，
  // 否则「量出来的宽度」与「画出来的图标」会对不上）。
  const float icon_size = icon_size_;
  // **图标名必须真的画得出来，才给它预留空间**。
  //
  // `icon_` 非空 ≠ 画得出来：名字不在内置表（也不是已装载的 SVG id）时，
  // `Icon::draw` 静默什么都不画，但这里仍按 `icon_size + gap` 预留了空位——
  // 文字于是被推到一侧。实测：画廊把 `btn-refresh` 的图标写成 `activity`
  // （内置 73 个图标里没有它，正确的是 `refresh`），结果**文字右偏 11.9px**。
  //
  // 判据用 `Icon::has`（与 `Icon::draw` 内部同一个查找）——
  // “算不算宽度”与“画不画得出”必须是同一个事实，否则这类静默偏移会反复出现。
  warn_unknown_icon_once(icon_);
  const bool draws_icon = !icon_.empty() && Icon::has(icon_);
  const float gap = (draws_icon && !label_.empty()) ? metrics.space_sm : 0.0f;
  // **按墨迹宽的排版占位**：盒里有透明留白（描边式图标约 30%），而绘制时
  // 墨迹是**居中**在盒里的——占位与墨迹同宽，内容才真的居中。
  // （与 `measure` 用的是同一个量；见那里的推导与实测数据。）
  const float icon_ink = draws_icon ? icon_ink_size().width : 0.0f;
  const float icon_extent = draws_icon ? icon_ink + gap : 0.0f;
  // **文本先按可用宽度截断再加省略号**：不截断时超长标签会画到按钮之外
  // （按钮被父容器夹窄了，但文本宽度还是它自己的量法），于是相邻面板上会多出
  // 一截看不清源头的字——实测就是侧栏 Git 变更项的路径叠到了代码上。
  const float available = std::max(0.0f, bounds_.width - icon_extent);
  const std::string clipped = port.ellipsize(label_, font_size, available);
  const float text_width = port.measure_width(clipped, font_size);
  const float total = text_width + icon_extent;
  float cursor = bounds_.x + (bounds_.width - total) * 0.5f;
  const float center_y = bounds_.center().y;

  if (draws_icon && icon_leading) {
    // 盒的左缘 = 当前光标 **减去墨迹在盒内的左边距**（`(盒 − 墨迹) / 2`，
    // 与 `Icon::path` 的居中同一口径）——墨迹才会落在 `cursor` 处。
    const float icon_left = cursor - (icon_size - icon_ink) * 0.5f;
    // 线宽传 0：`Icon::draw` 会按 `stroke/24 × 盒径` 随盒缩放（与 `IconView`、
    // 标题栏同口径）。固定 2px 会让大盒里笔画偏细、小盒里偏粗——同一个图标在
    // 不同档按钮里“看着不一样重”，而笔画粗细是另一个正交量。
    Icon::draw(canvas, icon_,
               math::Rect{icon_left, center_y - icon_size * 0.5f, icon_size, icon_size},
               style_.color, 0.0f);
    cursor += icon_extent;
  }
  if (!clipped.empty()) {
    // 垂直居中走**全仓统一口径**（`centered_line_top`：按墨迹区居中，不是行盒）。
    // 自己算一份的表现是"按钮字比菜单字偏一两像素"。
    const float y = centered_line_top(port, clipped, font_size, bounds_.y,
                                                           bounds_.height);
    port.draw(canvas, clipped, math::Point{cursor, y}, font_size, style_.color);
  }
  if (draws_icon && !icon_leading) {
    const float icon_left = cursor - (icon_size - icon_ink) * 0.5f;
    Icon::draw(canvas, icon_,
               math::Rect{icon_left, center_y - icon_size * 0.5f, icon_size, icon_size},
               style_.color, 0.0f);
  }
  if (focused_) {
    // **与其它组件走同一条焦点环**（`paint_focus_ring`），不再自己画一份。
    //
    // 旧实现在按钮**内部**缩进 2px 画一圈：不透明底（`Primary`/`Soft`/`Danger`）
    // 上环落在底色里，看着像“按钮里面又有个小框”；`Secondary` 则同时看到外框、
    // 2px 空白、内环三层。统一到“盖住自己描边的一道环”后，各变体都只是一道。
    paint_focus_ring(context, canvas, bounds_, style_.radius);
  }
}

auto Button::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  switch (event.kind) {
    case EventKind::HoverIn:
      mark_dirty();
      return false;
    case EventKind::HoverOut:
      mark_dirty();
      return false;
    case EventKind::MouseDown:
      mark_dirty();
      return true;
    case EventKind::Click:
    case EventKind::DoubleClick:
      mark_dirty();
      activate();
      return true;
    case EventKind::KeyDown: {
      if (event.key == "Enter" || event.key == " " || event.key == "Space") {
        activate();
        return true;
      }
      return false;
    }
    default:
      return false;
  }
}

void Button::activate() {
  if (!enabled_) return;
  if (on_click) on_click();
}

auto Button::semantics_flags() const -> SemanticsFlags {
  SemanticsFlags flags = Element::semantics_flags();
  flags.pressed = pressed_;
  return flags;
}

auto Button::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "label" || name == "text") return label_;
  if (name == "icon") return icon_;
  return std::nullopt;
}

auto Button::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "label" || name == "text") {
    set_label(std::string(value));
    return true;
  }
  if (name == "icon") {
    set_icon(std::string(value));
    return true;
  }
  return false;
}

auto Button::property_names() const -> std::vector<std::string_view> {
  return {"label", "text", "icon"};
}

auto Button::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "click" || action == "activate") {
    activate();
    return true;
  }
  return Element::invoke_action(action, argument);
}

// —— Card ——

Card::Card(float padding) : padding_(padding) {
  style_.padding = math::Insets::all(padding);
  // 卡片默认**不**带悬浮反馈：它多数是容器而非控件。需要交互时（可点卡片）
  // 调 `set_hover_effect` 打开——发光比背景提亮更适合卡片（不改变卡片自身的语气）。
  set_hover_effect(HoverEffect{.enabled = false});
}

void Card::set_padding(float padding) {
  padding_ = padding;
  style_.padding = math::Insets::all(padding);
  mark_layout_dirty();
}

void Card::set_radius(float radius) {
  radius_override_ = radius;
  mark_dirty();
}

void Card::set_shadow_level(std::uint8_t level) {
  shadow_level_ = level;
  mark_dirty();
}

void Card::set_elevated(bool elevated) {
  elevated_ = elevated;
  mark_dirty();
}

void Card::apply_theme(const Theme& theme) {
  style_.background = theme.colors().surface;
  style_.border_color = theme.colors().border;
  style_.border_width = theme.metrics().border_width;
  style_.radius = radius_override_ > 0.0f ? radius_override_ : theme.metrics().radius_lg;
  style_.padding = math::Insets::all(padding_);
  // 阴影档位：0=无、1=sm、≥2=md；`elevated_` 为真时一律 lg
  style_.shadow = shadow_level_ == 0 ? Shadow{}
                  : elevated_ ? shadow_lg(theme)
                  : shadow_level_ >= 2 ? shadow_md(theme)
                                       : shadow_sm(theme);
  // 卡片边框在浅色主题下**淡到几乎看不见**（它只用来在深色主题/高对比场景收边）：
  // 有阴影的卡片再配一道同等明显的描边，会变成“双层轮廓”，显得脏。
  style_.border_color = theme.colors().border.with_alpha_f(
      theme.mode() == ThemeMode::Dark ? 1.0f : 0.55f);
  // 亚克力的「玻璃边缘」：抬升的卡片顶部一道极弱亮线。
  // 只给**真抬升**的卡片（有阴影的那些）——平铺的内容块加边缘光会变成"到处在发光"。
  // 浅色档本令牌是透明的（见 `Theme::light()` 的说明），所以这里无需分模式判断。
  style_.top_highlight = (elevated_ || shadow_level_ > 0) ? theme.colors().highlight
                                                         : math::Color{0, 0, 0, 0};
}

// —— Divider ——

Divider::Divider(bool vertical) : vertical_(vertical) {}

void Divider::apply_theme(const Theme& theme) {
  style_.background = theme.colors().border;
  style_.height = vertical_ ? kAuto : theme.metrics().border_width;
  style_.width = vertical_ ? theme.metrics().border_width : kAuto;
}

void Divider::measure(const RenderContext& context, const Constraints& constraints) {
  if (vertical_) {
    measured_ = math::Size{context.theme.metrics().border_width, constraints.max_height};
    return;
  }
  measured_ = math::Size{constraints.max_width, context.theme.metrics().border_width};
}

void Divider::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  (void)context;
  canvas.fill_rect(bounds_, raster::Paint::solid(style_.background));
}

// —— KeyValueRow ——

KeyValueRow::KeyValueRow(std::string key, std::string value)
    : label_(std::move(key)), value_(std::move(value)) {
  style_.direction = FlexDirection::Row;
}

void KeyValueRow::set_value(std::string value) {
  value_ = std::move(value);
  mark_dirty();
}

void KeyValueRow::apply_theme(const Theme& theme) {
  style_.font_size = theme.metrics().font_sm;
  style_.background = math::Color{0, 0, 0, 0};
}

void KeyValueRow::measure(const RenderContext& context, const Constraints& constraints) {
  const TextPort& port = port_of(context);
  const float line_height = port.line_height(style_.font_size);
  measured_ = math::Size{constraints.max_width, line_height + 6.0f};
}

void KeyValueRow::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  const TextPort& port = port_of(context);
  const float size = style_.font_size;
  // 与 `Element::paint_text` / `draw_line` / `Button` 同一口径：按**墨迹区**居中。
  const float y = centered_line_top(port, label_, size, bounds_.y,
                                                         bounds_.height);
  port.draw(canvas, label_, math::Point{bounds_.x, y}, size, context.theme.colors().text_muted);
  const float value_width = port.measure_width(value_, size);
  port.draw(canvas, value_, math::Point{bounds_.right() - value_width, y}, size,
            context.theme.colors().text);
}

}  // namespace st::ui

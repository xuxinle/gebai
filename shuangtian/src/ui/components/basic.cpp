#include "st/ui/components/basic.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <functional>
#include <string>

#include "st/core/print.hpp"
#include "st/core/string.hpp"
#include "st/core/log.hpp"
#include "st/raster/paint.hpp"
#include "st/math/path.hpp"
#include "components_internal.hpp"
#include "st/ui/element.hpp"
#include "st/ui/icon.hpp"
#include "st/ui/line_layout.hpp"
#include "st/ui/text_port.hpp"

namespace st::ui {

namespace {

/// 选区哨兵（`Text` 用，见 `select_all` / `selected_text`）：
/// `kLineEnd` = “本行取到行尾”；`kLastLine` = “末行”（实际行数由排版缓存给）。
/// 为什么用哨兵而不是“当场算行数”：行数只能在有渲染上下文时（折行）才知道，
/// 而 `selected_text()` 是 const 无上下文接口。
constexpr std::size_t kLineEnd = std::numeric_limits<std::size_t>::max();
constexpr std::size_t kLastLine = std::numeric_limits<std::size_t>::max() / 2U;

}  // namespace
namespace {

using components_internal::paint_focus_ring;

/// 焦点环是否该画（**只在键盘导航时**）。
///
/// 为何需要：鼠标按下可聚焦元素也会给焦点，而点一下就在按钮上留一圈边框不对
///——Windows / VSCode 的惯例是“焦点环只在键盘导航时出现”（用户实测报过
/// “活动栏按钮不要点击时显示边框”）。
///
/// 判据问宿主（`HostFocus::focus_ring_visible`）而不是各自推断：
/// “上一次输入是键盘还是鼠标”是**全局**状态，组件无从知道；分散实现必然
/// 漂移成“有的按钮带框有的不带”。
///
/// ⚠ 但**组件可以否决**（`FocusRingPolicy::Never`）：容器类（滚动/面板/列表）
/// 要焦点只为接键，本身不是“可操作目标”；全局启发式盖不住它们的场景：
/// 窗口激活时焦点默认落在第一个可聚焦元素上，而那一下既非键盘也非鼠标，
/// `focus_ring_visible` 停在默认 true，一整圈灰框就出来了（相期实测：
/// 切回前台卡片区多一圈灰框，鼠标一动就消失）。把“要不要环”
/// 从全局启发式降维成组件声明，两类场景各得其所。
[[nodiscard]] auto focus_ring_wanted(const Element& element) -> bool {
  // 唯一判定处已上提到 `Element::wants_focus_ring()`（容器可以否决）。
  // 这里保留一个同名转发，是因为多个组件已经写了 `focus_ring_wanted(*this)`；
  // 新代码建议直接用 `element.wants_focus_ring()`。
  return element.wants_focus_ring();
}

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
  st::log::err("[ui] 未知图标名 `{}`（该按钮的图标不会绘制）——图标需登记在 `Icon` 表"
             "（src/ui/icon.cpp）里，`apps/*/assets/icons.svg` 那份是另一套",
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

void Text::set_scale(Scale scale) {
  if (scale_ == scale) return;
  scale_ = scale;
  mark_layout_dirty();
}

auto Text::font_size_for_scale(const Metrics& metrics, Scale scale) -> float {
  switch (scale) {
    case Scale::Override: return 0.0f;  // 调用方自己定（见 `apply_theme`）
    case Scale::Xs2: return metrics.font_2xs;
    case Scale::Xs: return metrics.font_xs;
    case Scale::Sm: return metrics.font_sm;
    case Scale::Base: return metrics.font_base;
    case Scale::Lg: return metrics.font_lg;
    case Scale::Xl: return metrics.font_xl;
  }
  return metrics.font_base;
}

void Text::set_weight(FontWeight weight) {
  weight_ = weight;
  mark_dirty();
}

void Text::apply_theme(const Theme& theme) {
  style_.color = text_tone_override().has_value() ? tone_color(theme, *text_tone_override())
                                                  : tone_color(theme, tone_);
  // 优先级：显式像素 > 语义档位 > 主题正文。
  // 显式优先是为了不破坏 `set_font_size` 的旧契约（既有调用方一大片）。
  if (font_size_override_ > 0.0f) {
    style_.font_size = font_size_override_;
  } else if (scale_ != Scale::Override) {
    style_.font_size = font_size_for_scale(theme.metrics(), scale_);
  } else {
    style_.font_size = theme.metrics().font_base;
  }
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
    // **折行结果按 (内容, 宽度, 字号) 缓存**：三者不变就复用。
    //
    // 为何必要：`measure` 在每帧的 `layout()` 里都会跑，而全量 `wrap` 是 O(长度)；
    // 把整份日志交给多行 `Text` 时，单是这一步就吃满了帧预算
    // （实测见 hpp 里 `display_line_widths_` 的说明）。
    if (layout_cache_content_ != content_ || layout_cache_width_ != available ||
        layout_cache_font_size_ != size || layout_cache_port_ != context.text) {
      std::vector<std::string> lines =
          max_lines_ > 0
              ? port.wrap_limited(content_, size, available, max_lines_)
              : std::vector<std::string>{};
      if (max_lines_ == 0) {
        for (const auto& line : port.wrap(content_, size, available)) {
          lines.emplace_back(line);
        }
      }
      display_lines_ = std::move(lines);
      display_line_widths_.assign(display_lines_.size(), 0.0F);
      layout_cache_content_ = content_;
      layout_cache_width_ = available;
      layout_cache_font_size_ = size;
      layout_cache_port_ = context.text;
    }
    // 行宽逐行缓存：只在对应的行还没量过时量（字符串可能因折行缓存复用而保留）。
    if (display_line_widths_.size() != display_lines_.size()) {
      display_line_widths_.assign(display_lines_.size(), 0.0F);
    }
    float widest = 0.0f;
    for (std::size_t index = 0; index < display_lines_.size(); ++index) {
      if (display_line_widths_[index] <= 0.0F) {
        display_line_widths_[index] = port.measure_width(display_lines_[index], size);
      }
      widest = std::max(widest, display_line_widths_[index]);
    }
    measured_ = math::Size{std::min(widest, available),
                           line_height *
                               static_cast<float>(display_lines_.empty() ? 1 : display_lines_.size())};
    return;
  }
  const float width = port.measure_width(content_, size);
  display_lines_.assign(1U, content_);
  display_line_widths_.assign(1U, width);
  measured_ = math::Size{std::min(width, constraints.max_width), line_height};
}

void Text::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  if (content_.empty()) return;
  const TextPort& port = port_of(context);
  const float size = style_.font_size;
  if (!multiline_) {
    // 单行：选区高亮画在文本下（复用同一份 `offset_to_x`，不会与字形错位）。
    if (selectable_ && has_selection()) {
      const float x0 = offset_to_x(context, 0U, select_begin_offset_);
      const float x1 = offset_to_x(context, 0U, select_end_offset_);
      // 垂直范围走**框架原语** `layout_line` 的 `glyph_top/glyph_height`
      //（字形墨迹带——高亮/光标专用；不要用行带 `ink_top/ink_height`，
      // 那是行背景口径，行距大时会让高亮多出一截）。
      // 旧实现写 `descent − ascent`：两者都是**正数**，相减恒为负，
      // 被 `max(..., size*0.6)` 兜住 ⇒ 高亮高永远是个固定值、与字体无关。
      const LineGeometry geom =
          layout_line(port, kInkReferenceSample, size, bounds_.height, text::FontRole::Proportional);
      canvas.fill_rect(math::Rect{std::min(x0, x1), bounds_.y + geom.glyph_top,
                                  std::max(x1 - x0, 1.0f), geom.glyph_height},
                       raster::Paint::solid(context.theme.colors().selection));
    }
    paint_text(context, canvas, content_, bounds_);
    return;
  }
  const float available = bounds_.width;
  // **复用排版缓存**（`measure` 已经算过——同一帧里它先跑），不再每帧重折行。
  // 仅当缓存与当前 (内容, 宽度, 字号) 不符时才现场重算（如从未走过 layout 的直绘路径）。
  if (layout_cache_content_ != content_ || layout_cache_width_ != available ||
      layout_cache_font_size_ != size || layout_cache_port_ != context.text ||
      display_line_widths_.size() != display_lines_.size()) {
    std::vector<std::string> lines;
    if (max_lines_ > 0) {
      lines = port.wrap_limited(content_, size, available, max_lines_);
    } else {
      for (const auto& line : port.wrap(content_, size, available)) lines.emplace_back(line);
    }
    display_lines_ = std::move(lines);
    display_line_widths_.assign(display_lines_.size(), 0.0F);
    layout_cache_content_ = content_;
    layout_cache_width_ = available;
    layout_cache_font_size_ = size;
    layout_cache_port_ = context.text;
  }
  const std::vector<std::string>& lines = display_lines_;
  const float line_height = port.line_height(size);

  // **逐行视口剔除**：只画落在裁剪区内的行。
  //
  // 为何必须（2026-10-14，实测）：旧实现对**全部行**做 `measure_width` + `draw`，
  // 完全不看视口——把整份日志交给一个多行 `Text`（`Terminal` 行模式正是如此）
  // 时，每帧成本直接等于内容长度：稳态帧 500 行 47 ms / 2000 行 705 ms /
  // 20000 行 3299 ms（`tools/ui_big_editors_bench.cpp`）。
  //
  // 几何与 `Element::paint`/`Tree` 同一口径（同一份 `clip_rect()` + `device_scale`
  // 折算）；竖直方向不需要保险外扩（行只画字形，不投阴影/发光）。
  const math::IntRect clip_physical = canvas.clip_rect();
  const float scale = canvas.device_scale();
  const float inv = scale > 0.0f ? 1.0f / scale : 1.0f;
  const float clip_top = static_cast<float>(clip_physical.y) * inv;
  const float clip_bottom = static_cast<float>(clip_physical.y + clip_physical.height) * inv;
  const float local_top = clip_top - bounds_.y;
  const float local_bottom = clip_bottom - bounds_.y;

  std::size_t first_row = 0;
  std::size_t last_row = lines.size();
  if (line_height > 0.0f && clip_bottom > clip_top) {
    if (local_bottom <= 0.0f || local_top >= static_cast<float>(lines.size()) * line_height) {
      return;  // 整块文本都在视口之外：一行也不画
    }
    first_row = static_cast<std::size_t>(std::floor(std::max(0.0F, local_top) / line_height));
    // 上界宽出一行：避免“裁剪区底边正落在行中”时漏画下半截。
    last_row = std::min(
        lines.size(),
        static_cast<std::size_t>(std::ceil(std::max(0.0F, local_bottom) / line_height)) + 1U);
  }

  float y = bounds_.y + static_cast<float>(first_row) * line_height;
  for (std::size_t row = first_row; row < last_row && row < lines.size(); ++row) {
    const auto& line = lines[row];
    // 行宽用缓存（`measure` 已量过）——不再每行现量。
    const float width = row < display_line_widths_.size() && display_line_widths_[row] > 0.0F
                            ? display_line_widths_[row]
                            : port.measure_width(line, size);
    float x = bounds_.x;
    if (style_.text_align == TextAlign::Center) {
      x = bounds_.x + (bounds_.width - width) * 0.5f;
    } else if (style_.text_align == TextAlign::End) {
      x = bounds_.right() - width;
    }
    // 选区高亮（本行内的选中区间），画在字形之下。
    if (selectable_ && has_selection() && row >= select_begin_line_ && row <= select_end_line_) {
      const std::size_t from = row == select_begin_line_ ? select_begin_offset_ : 0U;
      const std::size_t to = row == select_end_line_ ? select_end_offset_ : line.size();
      if (to > from) {
        const float head = x + port.measure_width(std::string_view(line).substr(0, std::min(from, line.size())), size);
        const float tail = x + port.measure_width(std::string_view(line).substr(0, std::min(to, line.size())), size);
        // 垂直同理走框架原语（行内 `y` 是本行行顶）。
        const LineGeometry geom = layout_line(port, kInkReferenceSample, size, line_height,
                                              text::FontRole::Proportional);
        canvas.fill_rect(math::Rect{head, y + geom.glyph_top, std::max(tail - head, 1.0f),
                                    geom.glyph_height},
                         raster::Paint::solid(context.theme.colors().selection));
      }
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

// —— 选区（2026-10-14）——

auto Text::rendered_lines(const RenderContext& context) const -> std::vector<RenderedLine> {
  const TextPort& port = port_of(context);
  const float size = style_.font_size;
  const float line_height = port.line_height(size);
  std::vector<std::string> texts;
  if (!multiline_) {
    texts.push_back(content_);
  } else if (max_lines_ > 0) {
    texts = port.wrap_limited(content_, size, bounds_.width, max_lines_);
  } else {
    for (const auto& line : port.wrap(content_, size, bounds_.width)) {
      texts.emplace_back(line);
    }
  }
  std::vector<RenderedLine> out;
  out.reserve(texts.size());
  float y = bounds_.y;
  for (auto& text : texts) {
    RenderedLine entry;
    entry.width = port.measure_width(text, size);
    entry.x = bounds_.x;
    if (style_.text_align == TextAlign::Center) {
      entry.x = bounds_.x + (bounds_.width - entry.width) * 0.5f;
    } else if (style_.text_align == TextAlign::End) {
      entry.x = bounds_.right() - entry.width;
    }
    entry.y = y;
    entry.text = std::move(text);
    out.push_back(std::move(entry));
    y += line_height;
  }
  // 缓存显示行文本（`selected_text()` 用它——见该函数说明）。
  // `paint_content` 是 const，故用 `mutable` 成员。
  display_lines_.clear();
  display_lines_.reserve(out.size());
  for (const RenderedLine& entry : out) display_lines_.push_back(entry.text);
  return out;
}

auto Text::offset_to_x(const RenderContext& context, std::size_t line,
                       std::size_t offset) const -> float {
  const TextPort& port = port_of(context);
  const float size = style_.font_size;
  const std::vector<RenderedLine> lines = rendered_lines(context);
  if (lines.empty()) return bounds_.x;
  const std::size_t row = std::min(line, lines.size() - 1U);
  const RenderedLine& entry = lines[row];
  const std::size_t bounded = std::min(offset, entry.text.size());
  return entry.x + port.measure_width(std::string_view(entry.text).substr(0, bounded), size);
}

auto Text::caret_at(const RenderContext& context, math::Point point) const
    -> std::pair<std::size_t, std::size_t> {
  const TextPort& port = port_of(context);
  const float size = style_.font_size;
  const std::vector<RenderedLine> lines = rendered_lines(context);
  if (lines.empty()) return {0U, 0U};
  const float line_height = port.line_height(size);
  // 行：按 y 算（line_height > 0 时）。
  std::size_t row = 0;
  if (line_height > 0.0f) {
    const float raw = std::floor((point.y - bounds_.y) / line_height);
    row = std::min(static_cast<std::size_t>(std::max(raw, 0.0f)), lines.size() - 1U);
  }
  const RenderedLine& entry = lines[row];
  // 行内：逐码点累加，取最接近的字符边界（与 `MarkdownView` 同一策略）。
  float pen = entry.x;
  const std::size_t count = st::utf8_length(entry.text);
  std::size_t best = 0U;
  for (std::size_t i = 0; i <= count; ++i) {
    if (i == count) { best = count; break; }
    const std::string_view one = st::utf8_slice(entry.text, i, 1);
    const float advance = port.measure_width(one, size);
    if (point.x < pen + advance * 0.5f) { best = i; break; }
    pen += advance;
  }
  return {row, st::utf8_offset(entry.text, best)};
}

auto Text::selected_text() const -> std::string {
  if (!selectable_ || select_begin_line_ == std::string::npos) return {};
  // 用**上次排版缓存**的显示行（`measure`/`paint_content` 时写入）：
  // `selected_text()` 是 const 无上下文接口，无上下文就无从折行。
  // 这也保证了“选中的字符”与“屏幕上看到的字”是同一份（若现场重折行，
  // 窗口尺寸一变就会选出别处的字）。
  if (display_lines_.empty()) return {};
  const std::size_t last = display_lines_.size() - 1U;
  const std::size_t from_line = std::min(select_begin_line_, last);
  const std::size_t to_line = std::min(select_end_line_, last);
  std::string out;
  for (std::size_t row = from_line; row <= to_line; ++row) {
    if (row > from_line) out += '\n';
    const std::string& text = display_lines_[row];
    const std::size_t from = row == from_line ? std::min(select_begin_offset_, text.size()) : 0U;
    // ⚠ 末端的偏移**只在末行生效**；其余行取整行。
    // 别写成 `min(select_end_offset_, text.size())`——`select_all()` 把末偏移
    // 置 0（表示“到行尾”），那样会选中空串（实测：全选拷出来是空的）。
    const std::size_t to =
        row == to_line ? (select_end_offset_ == kLineEnd ? text.size()
                                                         : std::min(select_end_offset_, text.size()))
                       : text.size();
    if (to > from) out += text.substr(from, to - from);
  }
  return out;
}

auto Text::has_selection() const -> bool {
  if (!selectable_ || select_begin_line_ == std::string::npos) return false;
  return select_end_line_ > select_begin_line_ || select_end_offset_ > select_begin_offset_;
}

void Text::clear_selection() noexcept {
  select_begin_line_ = std::string::npos;
  select_begin_offset_ = 0U;
  select_end_line_ = 0U;
  select_end_offset_ = 0U;
  selecting_ = false;
}

void Text::select_all() {
  if (!selectable_) return;
  // 全选 = 首行首字 .. 末行末字。
  // 行数需渲染上下文，故末行用一个**哨兵**，由 `selected_text()`/绘制按实际
  // 行数夹取；末偏移用 `kLineEnd`（= “取整行”）。
  select_begin_line_ = 0U;
  select_begin_offset_ = 0U;
  select_end_line_ = kLastLine;
  select_end_offset_ = kLineEnd;
  mark_dirty();
}

void Text::set_selectable(bool selectable) {
  if (selectable == selectable_) return;
  selectable_ = selectable;
  if (!selectable_) clear_selection();
  mark_dirty();
}

// —— Heading ——

auto Text::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "text" || name == "content") return content_;
  if (name == "multiline") return multiline_ ? "true" : "false";
  // 字号：与 `set_property` 对称（不报出去就改不了——`apply_properties` 的判据
  // 是“名字在元素自己声明的清单里”）。
  // ## 读的是"生效值"，但要考虑尚未经过 `apply_theme` 的显式设置
  //
  // `set_font_size` 写 `font_size_override_`，而 `style_.font_size` 只在
  // `apply_theme()` 里从它同步。于是"刚设完就读"会读到旧值——实测：
  // `set_property("font_size","18")` 返回 true，但 `get_property` 仍是 14。
  //
  // 这是一类真实的"改了没生效"缺陷（属性面往返断裂）。所以读的时候
  // 优先用 override（它是作者显式设定的那个值）。
  if (name == "font_size") {
    const float effective = font_size_override_ > 0.0f ? font_size_override_ : style_.font_size;
    return std::format("{}", effective);
  }
  if (name == "selectable") return selectable_ ? "true" : "false";
  if (name == "selection") return selected_text();
  if (name == "tone") return std::string(components_internal::tone_name(tone_));
  return std::nullopt;
}

auto Text::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "text" || name == "content") {
    set_content(std::string(value));
    return true;
  }
  if (name == "tone") {
    // 语义色（跟主题走）：XML/JS 里 `tone="primary"` 与硬编码 `text_color="#25.."`
    // 的区别在切主题时就显现——后者会在暗色主题上错层。
    const auto parsed = components_internal::tone_from_name(value);
    if (!parsed.has_value()) return false;
    set_tone(*parsed);
    return true;
  }
  if (name == "multiline") {
    set_multiline(value == "true" || value == "1");
    return true;
  }
  if (name == "font_size") {
    // 字号：接受 `18` / `18px`（属性面是文本，单位由调用方决定）。
    // 旧名 `size` 仍接受（兼容期），但新写法是 `font_size`。
    const std::string text(value);
    char* end = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);
    if (end == text.c_str()) return false;
    set_font_size(static_cast<float>(parsed));
    return true;
  }
  if (name == "selectable") {
    set_selectable(value == "true" || value == "1");
    return true;
  }
  return false;
}

auto Text::property_names() const -> std::vector<std::string_view> {
  // `size` 是字号（与 `Button` 的尺寸档位**同名不同义**——各自解释，见 `xml_ui.cpp`
  // 里 `normalize_property` 的说明）。
  // `selectable` 可读写（自动化/声明式都靠它开启选区）。
  return {"text", "content", "multiline", "font_size", "selectable", "tone"};
}

auto Text::readonly_property_names() const -> std::vector<std::string_view> {
  // `selection` 只读（`get` 能读、`set` 不生效），与其他文本组件同一组名字。
  return {"selection"};
}

auto Text::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "copy") {
    // 有选区 → 拷选区；无选区 → 拷整个 content（兼容旧语义）。
    // 空内容/无宿主不算复制成功（与 CodeEditor 空选区跳过同姿态）。
    if (host() == nullptr) return false;
    const std::string text = has_selection() ? selected_text() : content_;
    if (text.empty()) return false;
    return host()->host_set_clipboard_text(text);
  }
  if (action == "select_all") {
    if (!selectable_) return false;
    select_all();
    return true;
  }
  if (action == "clear_selection") {
    clear_selection();
    mark_dirty();
    return true;
  }
  return Element::invoke_action(action, argument);
}

auto Text::on_event(const RenderContext& context, Event& event) -> bool {
  // 默认（`selectable_ == false`）**不拦截任何事件**：静态标签遍布界面，
  // 抢拖选手势会变成干扰（见 `set_selectable` 的说明）。
  if (!selectable_ || !enabled()) return false;
  switch (event.kind) {
    case EventKind::MouseDown: {
      if (event.button != 1 || !bounds_.contains(event.position)) return false;
      const auto [row, offset] = caret_at(context, event.position);
      selecting_ = true;
      select_begin_line_ = row;
      select_begin_offset_ = offset;
      select_end_line_ = row;
      select_end_offset_ = offset;
      mark_dirty();
      event.handled = true;
      return true;
    }
    case EventKind::MouseMove: {
      if (!selecting_) return false;
      const auto [row, offset] = caret_at(context, event.position);
      // 归一化：保证 begin <= end（拖选方向任意）。
      const bool forward = row > select_begin_line_ ||
                           (row == select_begin_line_ && offset >= select_begin_offset_);
      select_end_line_ = row;
      select_end_offset_ = offset;
      if (!forward) {
        std::swap(select_begin_line_, select_end_line_);
        std::swap(select_begin_offset_, select_end_offset_);
      }
      mark_dirty();
      event.handled = true;
      return true;
    }
    case EventKind::MouseUp:
      selecting_ = false;
      return true;
    case EventKind::KeyDown: {
      const bool ctrl = event.ctrl || event.meta;
      if (ctrl && (event.key == "a" || event.key == "A")) {
        select_all();
        event.handled = true;
        return true;
      }
      if (ctrl && (event.key == "c" || event.key == "C")) {
        return invoke_action("copy", {});
      }
      if (event.key == "Escape" && has_selection()) {
        clear_selection();
        mark_dirty();
        event.handled = true;
        return true;
      }
      return false;
    }
    default: return false;
  }
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
  // 图标的 `size` 是**直径**（不是字号）——名字统一为 `diameter`，
  // 避免与 `font_size` 混淆（同一层里两个"大小"必须能区分开）。
  if (name == "diameter") return std::format("{}", size_);
  if (name == "tone") return std::string(components_internal::tone_name(tone_));
  return std::nullopt;
}

// 与 `get/set_property` 对称（见头文件里的说明）：不报名字 = 这条路命不中。
auto IconView::property_names() const -> std::vector<std::string_view> {
  return {"icon", "diameter", "tone"};
}

auto IconView::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "icon") {
    set_icon(std::string(value));
    return true;
  }
  if (name == "tone") {
    const auto parsed = components_internal::tone_from_name(value);
    if (!parsed.has_value()) return false;
    set_tone(*parsed);
    return true;
  }
  if (name == "diameter") {
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
  // 按钮 = 可点开 → 手形指针。这是“这块能点”最省事的全局提示，
  // 用户不必先悬停看有没有高亮才知道。
  set_cursor(MouseCursor::Hand);
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
  // **写进声明（`Appearance`），不是只改成员**（2026-11-10，词汇表统一）。
  //
  // 为何：`variant` 有两个来源——组件自己（构造参数/`set_variant`）与声明
  // （XML/协议的 `variant`）。旧实现只改 `variant_` 成员，而声明式那条
  // **只写 `Appearance` 从不回写成员**，于是“实际生效值”要看两条通道的叠加，
  // 而 `get_property` 只报一个——相期蓝底事故里 `get(variant)=primary`
  // 但屏幕上是 Ghost 色就是这样来的。
  //
  // 两条入口写到**同一个字段**（`extras().appearance.variant`）后：
  // `apply_theme`（`apply_declared_overrides` 回放）、`get_property`、主题切换
  // 三处看的是同一份事实，不可能再分叉。
  Appearance declared = appearance().value_or(Appearance{});
  declared.variant = variant;
  (void)apply_appearance(declared);
  variant_ = variant;   // 保留成员：`apply_theme` 的禁用态分支需要它做**无分配**判断
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
  // **生效档位来自声明优先**（2026-11-10，词汇表统一）。
  //
  // 为何不是直接用 `variant_` 成员：`variant` 有两条来源——组件自己
  // （构造参数/`set_variant`）与外观声明（XML/协议的 `variant` → `Appearance`）。
  // 渲染顺序是“组件默认 → 声明回放”（`refresh_theme` 的两步），
  // 所以**声明优先**是那个顺序的直接体现；本函数只负责拿“该画哪个档”
  // 去查 token，不再自己决定优先级。
  const Variant active_variant = [&] {
    if (const auto declared = appearance(); declared.has_value() && declared->variant.has_value()) {
      return *declared->variant;
    }
    return variant_;
  }();
  style_.radius = metrics.radius_md;
  style_.font_size = size_ == Size::Small ? metrics.font_sm
                                          : (size_ == Size::Large ? metrics.font_lg : metrics.font_base);
  // 图标盒径：三档取自 `Metrics::icon_size*`（独立令牌，不从字号推——见其定义处）。
  icon_size_ = size_ == Size::Small ? metrics.icon_size_sm
                                    : (size_ == Size::Large ? metrics.icon_size_lg : metrics.icon_size);
  // 字重 **Regular**：按钮标签是“控件文案”，不是标题。加重会让整排按钮看着
  // 挤、发脏（尤其小字号 + 合成加粗同时存在时）。需要突出的按钮靠变体色
  // （`Primary`/`Soft`/`Danger`）表达，不靠字重。
  style_.font_weight = FontWeight::Regular;
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
  // **先清渐变，再按变体写底**。
  //
  // 为何必须在 `switch` **之前**（2026-11-10，实测）：旧写法把 `reset()` 放在
  // `case Primary` 分支**内部**，于是从 Primary 切到其它变体的路径**永不清渐变**——
  // `set_variant(Ghost)` 只标脏、不重跑本函数以外的任何清理，而绘制时
  // 「渐变优先于纯色」⇒ 侧栏开关（`set_variant(on ? Soft : Ghost)`，初值 Primary）
  // 每个都带一块主色残影。这是同一类缺陷的 C++ 侧入口。
  style_.set_solid_background(math::Color{0, 0, 0, 0});

  // **取色统一走 `Tokens::variant_colors`**（不再在按钮里手拼一套）。
  //
  // 为何要统一（2026-11-10，词表统一）：按钮原先自己写了一份
  // “每个变体什么底/什么字”，而 `styling.cpp` 的 `variant_colors()` 是
  // 样式系统给**所有**组件用的同一份表。两份表并存时：
  //   · 同一档在两处颜色不同（“Secondary 和 Neutral 看着一样但实现两处”）；
  //   · 样式系统的表定义了 hover/pressed 色，按钮那份没有——
  //     而 `paint_box` 的通用悬浮层**读的就是这份色**，于是按钮的
  //     悬浮/按下只能靠“同向提亮”猜（这正是历史上“按下与悬停像素完全一样”的成因）。
  // 统一后按钮白得正确的 `background_hover`/`background_pressed` 语义。
  const Tokens tokens(theme);
  const Tokens::VariantColors tint = tokens.variant_colors(active_variant);
  // 底色与渐变**二选一**，由词表决定（`VariantColors::background_gradient`）：
  // 两个写入口各自把对面清掉，所以“透明底 + 残留渐变”在结构上不可能发生。
  if (tint.background_gradient.has_value()) {
    style_.set_gradient_background(*tint.background_gradient, tint.background);
  } else {
    style_.set_solid_background(tint.background);
  }
  style_.border_color = tint.border;
  style_.color = tint.text;
  // `border` 宽度：只有真给了描边色的变体才该画边。
  // 否则 `Neutral`（surface 底 + border 描边）会丢那一圈边，
  // 而 `Ghost`/`Primary` 会凭空多出一圈。
  style_.border_width = tint.border.a != 0U ? metrics.border_width : 0.0f;

  // 禁用态：降调（而不是把色直接换掉——换色会让禁用按钮看着像另一个变体）。
  // 每档的“降调色”各有其语义，所以仍按变体分支（与 `variant_colors` 正交）。
  if (inactive) {
    switch (active_variant) {
      case Variant::Primary: style_.set_solid_background(colors.primary_soft); break;
      case Variant::Soft:
      case Variant::Accent:
      case Variant::Success:
      case Variant::Warning: style_.set_solid_background(colors.primary_soft); break;
      case Variant::Danger: style_.set_solid_background(colors.danger.with_alpha_f(0.2f)); break;
      case Variant::Plain:
      case Variant::Neutral:
      case Variant::Ghost: break;   // 无底/中性底：底不变，字降调
    }
    style_.color = colors.text_faint;
    // 主色系变体的禁用字用主色（低饱和底上白字看不清）
    if (active_variant == Variant::Primary) style_.color = colors.primary;
  }

  // **渐变层已由词表负责**（`VariantColors::background_gradient`，见上方取色段）——
  // 这里不再重算一遍。旧的重复实现正是“同一件事两处写”的典型：
  // 两处的判据（`accent != primary`、`!inactive`）必须永远一致，否则
  // 一处画一处不画，而屏幕上看不出是哪一处决定的。
  // 禁用态已在上方通过 `set_solid_background` 把渐变清掉（禁用不画渐变）。
  if (tone_ != Tone::Default) {
    if (tone_ == Tone::OnPrimary) {
      style_.color = colors.on_primary;
    } else if (active_variant == Variant::Ghost || active_variant == Variant::Neutral ||
               active_variant == Variant::Plain) {
      // 只有“无底/中性底”的变体允许用 tone 把字标色（红/绿/黄…）——
      // 实底变体（Primary/Soft/Danger）的前景色是**底色的配对色**，改它会失去对比。
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
  // 省略位置：默认尾部（句子/标题）；**路径与长标识**用中段（保留文件名）。
  const std::string clipped =
      ellipsize_middle_ ? port.ellipsize_middle(label_, font_size, available)
                        : port.ellipsize(label_, font_size, available);
  const float text_width = port.measure_width(clipped, font_size);
  const float total = text_width + icon_extent;
  // 横向对齐：默认居中（既有界面零变化）；**列表行**用 `Start` 贴左缘。
  // 列表是拿来“扫”的，扫的是左缘——居中会让几十行参差不齐（实测截图）。
  // 贴左时带上左内边距：否则文字紧贴描边（居中时看不出来——居中本就不靠边）。
  float cursor = content_align_ == Align::Start ? bounds_.x + style_.padding.left
                                               : bounds_.x + (bounds_.width - total) * 0.5f;
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
    //
    // ⚠ 条件里还有 `focus_ring_wanted(*this)`：**鼠标点出来的焦点不画环**
    //（用户报过“活动栏按钮不要点击时显示边框”）；键盘一按就恢复。
    // 与 CSS `:focus-visible` 同一套启发式（程序化聚焦默认算“该画”）。
    if (focus_ring_wanted(*this)) {
      paint_focus_ring(context, canvas, bounds_, style_.radius);
    }
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
    case EventKind::KeyDown: {
      if (event.key == "Enter" || event.key == " " || event.key == "Space") {
        activate();
        return true;
      }
      return false;
    }
    default:
      // 点击族（三种同权）：**一次按放恰好一条**（后端契约，见 `shell/click_gesture.hpp`），
      // 所以这里就是“按一下就激活一次”。
      //
      // 旧写法只列 `Click` + `DoubleClick`，于是连点三下的**第三下静默丢失**
      // （实测：三连点只触发 2 次，用户看到“第三下没反应”）。
      if (!is_click_family(event.kind)) return false;
      mark_dirty();
      activate();
      return true;
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
  // 档位：`variant`（外观）/ `scale`（尺寸档）/ `hint`（悬停提示）。
  //
  // ⚠ `variant` 必须报**真正在画的那个档位**，而不是 `variant_` 成员。
  //
  // 为何（2026-11-10，相期蓝底事故的“读回说谎”那一半）：
  // `variant` 有两条通道——组件自己（`set_variant`/构造参数）与外观声明
  // （XML/协议的 `variant` → `Appearance`）。旧实现只报 `variant_` 成员，
  // 而声明式那条**从不回写成员**——于是 `<Button variant="ghost"/>` 在
  // `get` 里报 `primary`，而屏幕上是“主色渐变底 + Ghost 灰字”的缝合怪。
  //
  // 调试时这个谎最致命：它直接把人往“XML 没生效”的反方向指（实际生效了，
  // 只是生效在另一层）。读回口必须与**实际生效值**同一份事实。
  if (name == "variant") {
    // 声明优先（`apply_declared_overrides` 的回放顺序就是它的优先级）。
    if (const auto declared = appearance(); declared.has_value() && declared->variant.has_value()) {
      return std::string(st::ui::variant_name(*declared->variant));
    }
    return std::string(st::ui::variant_name(variant_));
  }
  // 按钮的 `size` 是**枚举**（small/medium/large），不是长度——
  // 改叫 `scale` 以免与长度类字段混淆。
  if (name == "scale") {
    switch (size_) {
      case Size::Small: return "small";
      case Size::Medium: return "medium";
      case Size::Large: return "large";
    }
    return "medium";
  }
  if (name == "hint") return hint();
  if (name == "tone") return std::string(components_internal::tone_name(tone_));
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
  if (name == "tone") {
    const auto parsed = components_internal::tone_from_name(value);
    if (!parsed.has_value()) return false;
    set_tone(*parsed);
    return true;
  }
  if (name == "variant") {
    // **组件不再接受 `variant` 写入，但也不装作不认识它**（2026-11-10）。
    //
    // 为何不能简单地 `return false`：`apply_properties` 的拒因分类靠
    // “名字在不在 `property_names()` 里”——返回 false 会让它报 `unknown_name`
    // （“这个名字不认识”），而事实是**名字认得、只是写入通道不是这一层**。
    // 那个错根因会把排查人指到“拼错属性名”的错方向（实测就把
    // `set_reports_rejected_properties` 的断言撞红了：期望 invalid_value，实得 unknown_name）。
    //
    // 真正的事实是：`variant` 的**写入通道是样式系统**（`Appearance`）——
    // XML/协议 那条路径里 `is_appearance_name` 分支**先于**本函数执行，
    // 所以正常链路根本到不了这。能到这里的只有直接调 `set_property("variant")` 的代码，
    // 而它应该被告知的是“这个值在这层不合法”，不是“这个名字不存在”。
    //
    // 所以：名字列入 `property_names()`（可读写契约），写入直接落到**声明**上——
    // 两条入口最终到同一个事实（`appearance().variant`），不再有“一个字段两条通道”。
    const std::string text(value);
    constexpr std::array kAll{Variant::Plain,  Variant::Neutral, Variant::Primary,
                              Variant::Accent, Variant::Success, Variant::Warning,
                              Variant::Danger, Variant::Ghost,   Variant::Soft};
    std::optional<Variant> parsed;
    for (const Variant candidate : kAll) {
      if (text == st::ui::variant_name(candidate)) {
        parsed = candidate;
        break;
      }
    }
    if (!parsed.has_value()) return false;   // 值不合法 → invalid_value（正确的拒因）
    // 写进**声明**（Appearance），不是 `variant_` 成员：
    // 与 XML/协议 完全同一条通道，于是 `get_property("variant")`、`apply_theme`、
    // 主题重放三处看的是**同一份事实**。
    Appearance declared = appearance().value_or(Appearance{});
    declared.variant = *parsed;
    (void)apply_appearance(declared);
    mark_dirty();
    return true;
  }
  if (name == "scale") {
    const std::string text(value);
    if (text == "small") {
      set_size(Size::Small);
    } else if (text == "medium") {
      set_size(Size::Medium);
    } else if (text == "large") {
      set_size(Size::Large);
    } else {
      return false;
    }
    return true;
  }
  if (name == "hint") {
    set_hint(std::string(value));
    return true;
  }
  return false;
}

auto Button::property_names() const -> std::vector<std::string_view> {
  // `variant` **在这张表里**（名字认得、可写），但它的写入落到**声明**
  // （`Appearance`）而不是组件自己的 `variant_` 成员——见 `set_property` 的说明。
  // 读回也走同一份事实（`get_property` 优先报声明的值）。
  return {"label", "text", "icon", "variant", "scale", "hint", "tone"};
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
  //
  // ⚠ **必须是 `scale_alpha`（乘）而不是 `with_alpha_f`（覆盖）**。
  // `border` 族令牌自带半透明 alpha（亚克力口径：它是一道“叠在任意底色上强度一致”的白光/黑光），
  // 深色下 23% 白、浅色下 11% 黑。要“在令牌基础上再调淡/调浓”只能**乘**：
  //   · 用 `with_alpha_f(1.0)` 会把 alpha **覆盖成 255** → 暗色卡片得一道**不透明的纯白 1px 实边**
  //     （实测四边中左/下正打在像素格上，合成后亮度 255/255：屏幕上是刺眼白框）；
  //   · 用 `with_alpha_f(0.55)` 是覆盖成 55% 黑 → 浅色卡片边框合成到 `surface` 后为 `#717172`，
  //     与 `surface` 对比 4.2:1，正是注释里要避免的“脏边”。
  // 两者都不是“乘”，因为**源色本身带 alpha 时两个字面量恰好都长得像倍率**，这是最容易被误读的地方。
  style_.border_color = theme.colors().border.scale_alpha(
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
  // **用最淡的一档**（`border_subtle`）而不是 `border`：
  // 分割线是“同组内的弱分隔”，职责是暗示分组，不应与控件边界争抢注意力
  //（用户 2026-10-21：“分割线改细点”）。
  // 主题里 `border_subtle` 的强度已被 `ui_theme_test` 锁在 `border` 之下。
  style_.background = theme.colors().border_subtle;
  // 线宽支持**亚像素**（< 1 逻辑 px）：`0.5f` 在 2 倍 DPI 下正好一个物理像素，
  // 在 1 倍下会被光栅器按覆盖率画成一条淡线——两种 DPI 都比 1px 实线更轻。
  style_.height = vertical_ ? kAuto : kLineThickness;
  style_.width = vertical_ ? kLineThickness : kAuto;
}

void Divider::measure(const RenderContext& context, const Constraints& constraints) {
  (void)context;
  if (vertical_) {
    measured_ = math::Size{kLineThickness, constraints.max_height};
    return;
  }
  measured_ = math::Size{constraints.max_width, kLineThickness};
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
  // 显式排版覆盖最后回放（与 Text/Button 同口径）：字号覆盖直接改写 style_
  //（measure/paint 都从 style_.font_size 取）；颜色覆盖存起来供 paint_content
  // 的左右两段取色用（两段各自有 muted/text 的语义分工，不能整体盖）。不设则
  // 沿用主题两段色——覆盖在这里只做"记录"，取色在绘制时问 text_color_override。
  apply_text_overrides();
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
  // 左右两段各有语义色（muted/text）；显式文字色覆盖（DSL `BoxProps::color`）
  // 只盖**值**一段——它是"这条信息的重点"，键保持 muted 才读得出主次。
  math::Color value_color = context.theme.colors().text;
  if (const auto& override_color = text_color_override(); override_color.has_value()) {
    value_color = *override_color;
  }
  port.draw(canvas, label_, math::Point{bounds_.x, y}, size, context.theme.colors().text_muted);
  const float value_width = port.measure_width(value_, size);
  port.draw(canvas, value_, math::Point{bounds_.right() - value_width, y}, size,
            value_color);
}

}  // namespace st::ui

// ── 组件登记（`st/ui/component_registry.hpp`）────────────────────────
//
// 一行一个组件，展开后包含**静态登记**（本 .o 进产物即在 main 前完成注册）
// 与**导入锚点**（`import_components<T>()` 取它的地址，把本 .o 拉进产物）。
// 所以：真的用到组件 → 自动注册；要按名字建 → 另调 import。
ST_UI_COMPONENT_IMPL(::st::ui::Text);
ST_UI_COMPONENT_IMPL(::st::ui::Heading);
ST_UI_COMPONENT_IMPL(::st::ui::IconView);
ST_UI_COMPONENT_IMPL(::st::ui::Button);
ST_UI_COMPONENT_IMPL(::st::ui::Card);
ST_UI_COMPONENT_IMPL(::st::ui::Divider);
ST_UI_COMPONENT_IMPL(::st::ui::KeyValueRow);


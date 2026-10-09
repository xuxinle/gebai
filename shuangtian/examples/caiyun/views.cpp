/// 裁云的自绘视图实现（见 `views.hpp`）。

#include "views.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"

namespace caiyun {

namespace {

using st::raster::Paint;
using st::raster::Path;
using st::ui::Event;
using st::ui::EventKind;

/// 时间轴各带的高度（逻辑像素）。
constexpr float kRulerHeight = 22.0f;
constexpr float kTrackHeight = 40.0f;
constexpr float kTrackGap = 6.0f;
constexpr float kEdgeGrabWidth = 6.0f;   ///< 修剪手柄的抓取宽度
constexpr float kLabelPad = 6.0f;

/// 片段块的配色按生成器区分（一眼能看出"这条轨道上是什么内容"）。
[[nodiscard]] auto generator_color(GeneratorKind kind) -> st::math::Color {
  switch (kind) {
    case GeneratorKind::ColorBars: return st::math::Color{78, 110, 180, 255};
    case GeneratorKind::Subtitle: return st::math::Color{150, 96, 160, 255};
    case GeneratorKind::Shapes: return st::math::Color{70, 140, 120, 255};
    case GeneratorKind::TestChart: return st::math::Color{150, 120, 70, 255};
  }
  return st::math::Color{90, 90, 90, 255};
}

[[nodiscard]] auto fnv1a(std::span<const std::uint32_t> pixels) noexcept -> std::uint64_t {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const std::uint32_t value : pixels) {
    hash ^= static_cast<std::uint64_t>(value);
    hash *= 1099511628211ULL;
  }
  return hash;
}

}  // namespace

// ── TimelineView ───────────────────────────────────────────────────────────

TimelineView::TimelineView() { set_focusable(true); }

void TimelineView::set_project(Project* project) {
  project_ = project;
  selected_ = project != nullptr ? project->selected : -1;
  mark_dirty();
}

auto TimelineView::time_to_x(double time) const -> float {
  if (project_ == nullptr || project_->duration <= 0.0) return bounds().x;
  const float usable = std::max(1.0f, bounds().width);
  return bounds().x + static_cast<float>(time / project_->duration) * usable;
}

auto TimelineView::x_to_time(float x) const -> double {
  if (project_ == nullptr || project_->duration <= 0.0) return 0.0;
  const float usable = std::max(1.0f, bounds().width);
  const double ratio = static_cast<double>(x - bounds().x) / static_cast<double>(usable);
  return std::clamp(ratio * project_->duration, 0.0, project_->duration);
}

auto TimelineView::track_rect(int track) const -> st::math::Rect {
  const float top = bounds().y + kRulerHeight + kTrackGap +
                    static_cast<float>(1 - track) * (kTrackHeight + kTrackGap);
  return st::math::Rect{bounds().x, top, bounds().width, kTrackHeight};
}

auto TimelineView::clip_rect(const Clip& clip) const -> st::math::Rect {
  const st::math::Rect track = track_rect(clip.track);
  const float x0 = time_to_x(clip.t_in);
  const float x1 = time_to_x(clip.t_out);
  return st::math::Rect{x0, track.y + 3.0f, std::max(2.0f, x1 - x0), track.height - 6.0f};
}

auto TimelineView::grab_at(st::math::Point point) const -> std::pair<int, TimelineGrab> {
  if (project_ == nullptr) return {-1, TimelineGrab::None};

  // 播放头优先（它在最上层，且很细——不优先就永远抓不到）。
  const float head_x = time_to_x(project_->playhead);
  if (std::abs(point.x - head_x) <= 4.0f && point.y >= bounds().y &&
      point.y <= bounds().bottom()) {
    return {-1, TimelineGrab::Playhead};
  }
  // 片段：**上层优先**（V2 盖住 V1 的部分该抓 V2）。
  for (int track = 1; track >= 0; --track) {
    for (const auto& clip : project_->clips) {
      if (clip.track != track) continue;
      const st::math::Rect rect = clip_rect(clip);
      if (!rect.contains(point)) continue;
      if (point.x - rect.x <= kEdgeGrabWidth) return {clip.id, TimelineGrab::ClipLeftEdge};
      if (rect.right() - point.x <= kEdgeGrabWidth) return {clip.id, TimelineGrab::ClipRightEdge};
      return {clip.id, TimelineGrab::ClipBody};
    }
  }
  // 标尺上任意点击 = 定位播放头。
  if (point.y <= bounds().y + kRulerHeight) return {-1, TimelineGrab::Playhead};
  return {-1, TimelineGrab::None};
}

void TimelineView::notify_changed() {
  mark_dirty();
  if (on_project_changed) on_project_changed();
}

void TimelineView::notify_seek() {
  mark_dirty();
  if (on_seek) on_seek();
  if (on_project_changed) on_project_changed();
}

void TimelineView::apply_theme(const st::ui::Theme& theme) {
  const auto& colors = theme.colors();
  ruler_bg_ = colors.surface_alt;
  track_bg_ = colors.surface_sunken;
  track_bg_alt_ = colors.surface;
  grid_ = colors.border;
  clip_border_ = colors.border_strong;
  playhead_ = colors.accent;
  text_ = colors.text;
  clip_selected_ = colors.accent;
}

void TimelineView::measure(const st::ui::RenderContext& context,
                           const st::ui::Constraints& constraints) {
  (void)context;
  float width = 480.0f;
  float height = kRulerHeight + (kTrackHeight + kTrackGap) * 2.0f + 8.0f;
  if (constraints.max_width < st::ui::kUnbounded) width = constraints.max_width;
  if (constraints.max_height < st::ui::kUnbounded) height = constraints.max_height;
  measured_ = st::math::Size{width, height};
}

void TimelineView::paint_content(const st::ui::RenderContext& context,
                                 st::raster::Surface& canvas) const {
  const st::math::Rect area = bounds();
  if (area.is_empty()) return;
  canvas.fill_rect(area, Paint::solid(track_bg_alt_));
  if (project_ == nullptr) return;

  // —— 标尺 ——
  const st::math::Rect ruler{area.x, area.y, area.width, kRulerHeight};
  canvas.fill_rect(ruler, Paint::solid(ruler_bg_));
  // 每秒一个刻度、每 5 秒一个长刻度 + 文字（标签密度随缩放自适应，
  // 否则窄视口上文字会糊成一片——这在小窗口下最容易出问题）。
  const float pixels_per_second = area.width / static_cast<float>(std::max(0.001, project_->duration));
  int step = 1;
  if (pixels_per_second < 40.0f) step = 5;
  if (pixels_per_second < 16.0f) step = 10;
  for (int second = 0; second <= static_cast<int>(project_->duration); second += step) {
    const float x = time_to_x(static_cast<double>(second));
    canvas.fill_rect(st::math::Rect{x, ruler.y + kRulerHeight - 6.0f, 1.0f, 6.0f},
                     Paint::solid(grid_));
    if (context.text != nullptr) {
      context.text->draw(canvas, format_short(static_cast<double>(second)),
                         st::math::Point{x + 3.0f, ruler.y + 12.0f}, 9.5f, text_);
    }
  }

  // —— 轨道 ——
  for (int track = 0; track <= 1; ++track) {
    const st::math::Rect line = track_rect(track);
    canvas.fill_rect(line, Paint::solid(track == 0 ? track_bg_ : track_bg_));
    if (context.text != nullptr) {
      context.text->draw(canvas, track == 0 ? "V1" : "V2",
                         st::math::Point{line.x + kLabelPad, line.y + 12.0f}, 9.5f, text_);
    }
  }

  // —— 片段块 ——
  for (const auto& clip : project_->clips) {
    const st::math::Rect rect = clip_rect(clip);
    const bool is_selected = clip.id == selected_;
    const bool is_hovered = clip.id == hover_clip_;
    st::math::Color fill = generator_color(clip.generator);
    if (is_hovered) fill = st::math::Color{
        static_cast<std::uint8_t>(std::min(255, fill.r + 18)),
        static_cast<std::uint8_t>(std::min(255, fill.g + 18)),
        static_cast<std::uint8_t>(std::min(255, fill.b + 18)), fill.a};
    canvas.fill_rect(rect, Paint::solid(fill), 4.0f);
    // 选中/悬浮的边框差异要**明显**（2px + 强调色），否则小片段上分不出来。
    if (is_selected || is_hovered) {
      const Path outline = st::raster::make_rounded_rect(rect, 4.0f);
      canvas.stroke_path(outline, Paint::solid(is_selected ? clip_selected_ : clip_border_),
                         is_selected ? 2.0f : 1.0f);
    }
    if (context.text != nullptr && rect.width > 28.0f) {
      // 标签：生成器名 + 时长；宽度不足时只画名字（截断由宽度判断兜住）。
      const std::string label = format_short(clip.t_in) + " " + to_string(clip.generator);
      context.text->draw(canvas, label, st::math::Point{rect.x + kLabelPad, rect.y + 14.0f}, 10.0f,
                         st::math::Color{255, 255, 255, 235});
    }
    // 修剪手柄：左右各一条竖线（**只在选中时显示**——全画会让时间轴很吵）。
    if (is_selected && rect.width > 12.0f) {
      canvas.fill_rect(st::math::Rect{rect.x, rect.y, 2.0f, rect.height},
                       Paint::solid(st::math::Color{255, 255, 255, 200}));
      canvas.fill_rect(st::math::Rect{rect.right() - 2.0f, rect.y, 2.0f, rect.height},
                       Paint::solid(st::math::Color{255, 255, 255, 200}));
    }
  }

  // —— 播放头（最后画，压在片段上）——
  const float head_x = time_to_x(project_->playhead);
  canvas.fill_rect(st::math::Rect{head_x - 1.0f, area.y, 2.0f, area.height},
                   Paint::solid(playhead_));
  // 播放头顶部的小三角（抓取提示）。
  Path head;
  head.move_to(st::math::Point{head_x - 5.0f, area.y});
  head.line_to(st::math::Point{head_x + 5.0f, area.y});
  head.line_to(st::math::Point{head_x, area.y + 7.0f});
  head.close();
  canvas.fill_path(head, Paint::solid(playhead_));
}

auto TimelineView::on_event(const st::ui::RenderContext& context, Event& event) -> bool {
  (void)context;
  if (project_ == nullptr) return false;
  switch (event.kind) {
    case EventKind::MouseMove: {
      const auto [id, grab] = grab_at(event.position);
      hover_clip_ = (grab == TimelineGrab::ClipBody || grab == TimelineGrab::ClipLeftEdge ||
                     grab == TimelineGrab::ClipRightEdge)
                        ? id
                        : -1;
      if (dragging_playhead_) {
        project_->playhead = project_->snap(x_to_time(event.position.x));
        notify_seek();
        return true;
      }
      if (grab_ == TimelineGrab::ClipBody && grab_clip_ >= 0) {
        const double target = x_to_time(event.position.x) - grab_offset_;
        (void)project_->move_clip(grab_clip_, project_->snap(target));
        notify_seek();
        return true;
      }
      if ((grab_ == TimelineGrab::ClipLeftEdge || grab_ == TimelineGrab::ClipRightEdge) &&
          grab_clip_ >= 0) {
        const double time = project_->snap(x_to_time(event.position.x));
        (void)project_->trim(grab_clip_, grab_ == TimelineGrab::ClipLeftEdge ? 0 : 1, time);
        notify_seek();
        return true;
      }
      mark_dirty();
      return false;
    }
    case EventKind::HoverOut:
      hover_clip_ = -1;
      mark_dirty();
      return false;
    case EventKind::MouseDown: {
      if (event.button != 1) return false;
      set_focused(true);
      const auto [id, grab] = grab_at(event.position);
      grab_ = grab;
      grab_clip_ = id;
      if (grab == TimelineGrab::Playhead) {
        dragging_playhead_ = true;
        project_->playhead = project_->snap(x_to_time(event.position.x));
        notify_seek();
        return true;
      }
      if (id >= 0) {
        selected_ = id;
        project_->selected = id;
        const Clip* clip = project_->find_clip(id);
        if (clip != nullptr && grab == TimelineGrab::ClipBody) {
          grab_offset_ = x_to_time(event.position.x) - clip->t_in;
        }
        notify_changed();
        return true;
      }
      return false;
    }
    case EventKind::MouseUp:
      dragging_playhead_ = false;
      grab_ = TimelineGrab::None;
      grab_clip_ = -1;
      return false;
    default:
      return false;
  }
}

auto TimelineView::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "selected") return std::to_string(selected_);
  if (name == "clip_count") {
    return std::to_string(project_ == nullptr ? 0U : project_->clips.size());
  }
  if (name == "playhead") {
    return project_ == nullptr ? std::string("0") : format_timecode(project_->playhead);
  }
  if (name == "transport") {
    return project_ != nullptr && project_->playing ? std::string("playing") : std::string("paused");
  }
  return std::nullopt;
}

auto TimelineView::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "selected") {
    try {
      selected_ = std::stoi(std::string(value));
      if (project_ != nullptr) project_->selected = selected_;
      mark_dirty();
      return true;
    } catch (...) {
      return false;
    }
  }
  return false;
}

auto TimelineView::property_names() const -> std::vector<std::string_view> {
  return {"selected", "clip_count", "playhead", "transport"};
}

auto TimelineView::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (project_ == nullptr) return false;

  if (action == "seek") {
    try {
      project_->playhead = std::clamp(std::stod(std::string(argument)), 0.0, project_->duration);
      notify_seek();
      return true;
    } catch (...) {
      return false;
    }
  }
  if (action == "select") {
    try {
      const int id = std::stoi(std::string(argument));
      if (project_->find_clip(id) == nullptr) return false;
      selected_ = id;
      project_->selected = id;
      notify_changed();
      return true;
    } catch (...) {
      return false;
    }
  }
  if (action == "split") {
    double at = project_->playhead;
    if (!argument.empty()) {
      try {
        at = std::stod(std::string(argument));
      } catch (...) {
        return false;
      }
    }
    const bool ok = project_->split_at(at);
    if (ok) notify_changed();
    return ok;
  }
  if (action == "ripple_delete") {
    const int id = argument.empty() ? selected_ : std::stoi(std::string(argument));
    const bool ok = project_->ripple_delete(id);
    if (ok) notify_changed();
    return ok;
  }
  if (action == "trim") {
    // 形如 `<clip_id> <edge 0/1> <time>`
    const std::size_t a = argument.find(' ');
    const std::size_t b = argument.find(' ', a + 1);
    if (a == std::string_view::npos || b == std::string_view::npos) return false;
    try {
      const int id = std::stoi(std::string(argument.substr(0, a)));
      const int edge = std::stoi(std::string(argument.substr(a + 1, b - a - 1)));
      const double time = std::stod(std::string(argument.substr(b + 1)));
      const bool ok = project_->trim(id, edge, time);
      if (ok) notify_changed();
      return ok;
    } catch (...) {
      return false;
    }
  }
  if (action == "move") {
    // 形如 `<clip_id> <new_t_in>`
    const std::size_t space = argument.find(' ');
    if (space == std::string_view::npos) return false;
    try {
      const int id = std::stoi(std::string(argument.substr(0, space)));
      const double t = std::stod(std::string(argument.substr(space + 1)));
      const bool ok = project_->move_clip(id, t);
      if (ok) notify_changed();
      return ok;
    } catch (...) {
      return false;
    }
  }
  if (action == "add_clip") {
    // 形如 `<generator 0..3> <track> <t_in> <t_out>`
    double values[4] = {0, 0, 0, 1};
    std::string_view rest = argument;
    for (int i = 0; i < 4; ++i) {
      const std::size_t space = rest.find(' ');
      const std::string_view token = space == std::string_view::npos ? rest : rest.substr(0, space);
      try {
        values[i] = std::stod(std::string(token));
      } catch (...) {
        return false;
      }
      if (space == std::string_view::npos) break;
      rest = rest.substr(space + 1);
    }
    const int kind = static_cast<int>(values[0]);
    if (kind < 0 || kind >= static_cast<int>(kGeneratorCount)) return false;
    const int id = project_->add_clip(kGenerators[kind], static_cast<int>(values[1]), values[2],
                                      values[3]);
    if (id < 0) return false;
    selected_ = id;
    notify_changed();
    return true;
  }
  if (action == "snap") {
    const bool wanted = argument.empty() ? !project_->snapping : argument != "0";
    if (project_->snapping == wanted) return false;
    project_->snapping = wanted;
    notify_changed();
    return true;
  }
  return false;
}

// ── MonitorView ────────────────────────────────────────────────────────────

MonitorView::MonitorView() : frame_(16, 9, 1.0f) {}

void MonitorView::set_project(Project* project) {
  project_ = project;
  frame_valid_ = false;
  mark_dirty();
}

void MonitorView::refresh() {
  frame_valid_ = false;
  mark_dirty();
}

void MonitorView::apply_theme(const st::ui::Theme& theme) {
  background_ = theme.colors().surface_sunken;
  border_ = theme.colors().border_strong;
}

void MonitorView::measure(const st::ui::RenderContext& context,
                          const st::ui::Constraints& constraints) {
  (void)context;
  float width = 480.0f;
  float height = 270.0f;
  if (constraints.max_width < st::ui::kUnbounded) width = constraints.max_width;
  if (constraints.max_height < st::ui::kUnbounded) height = constraints.max_height;
  // 监视器按 **16:9** 收高：给它多高的空间都不该拉变形（视频预览的常识）。
  const float max_by_ratio = width * 9.0f / 16.0f;
  measured_ = st::math::Size{width, std::min(height, max_by_ratio)};
}

void MonitorView::paint_content(const st::ui::RenderContext& context,
                                st::raster::Surface& canvas) const {
  const st::math::Rect area = bounds();
  if (area.is_empty()) return;
  canvas.fill_rect(area, Paint::solid(background_));
  if (project_ == nullptr) return;

  // 合成目标尺寸固定为 **320×180**（16:9）：与窗口大小解耦，
  // 于是"同一时刻两次渲染逐像素相同"这条判据不依赖窗口尺寸。
  constexpr int kWidth = 320;
  constexpr int kHeight = 180;
  const double time = project_->playhead;
  if (!frame_valid_ || frame_time_ != time) {
    // `resize` 返回 Status（`nodiscard`）；尺寸恒为 320×180、失败只可能是
    // 显存/分配问题——那时后续渲染会画到旧尺寸上，不该静默放过。
    const auto resized = frame_.resize(kWidth, kHeight);
    if (!resized.has_value()) return;
    frame_.clear(st::math::Color{0, 0, 0, 255});
    // ⚠ 合成时必须把**真实文本端口**传进去：不传的话字幕类片段退化为几何占位块
    //（实测：预览里字幕位置是一条灰条——初看像“字幕功能没做完”，实际是渲染时
    // 没给 text port）。端口来自本帧的渲染上下文（由 `UiRoot` 拥有）。
    project_->render_frame(frame_, time, context.text);
    // ⚠ **必须在渲染之后取像素**：`pixels()` 是只读视图，位置对了才有意义。
    // 为什么用 FNV-1a 而不是 crc32：无表、无状态、几行就能写对，且 e2e 侧
    // 用同一条常量即可独立复算。
    frame_hash_ = fnv1a(frame_.pixels());
    frame_time_ = time;
    frame_valid_ = true;
  }
  // ⚠ **必须保持 16:9**：把 320×180 直接拉到 `area` 会在宽面板里把画面横拉
  //（实测：预览区 3.3:1 时圆形变成椭圆、字幕拉伸）——视频预览的“纵横比失真”
  // 是最容易被一眼看出、又最容易在“先跑通”阶段被漏掉的一类错。
  const float scale = std::min(area.width / static_cast<float>(kWidth),
                               area.height / static_cast<float>(kHeight));
  const float draw_w = static_cast<float>(kWidth) * scale;
  const float draw_h = static_cast<float>(kHeight) * scale;
  const st::math::Rect fitted{area.x + (area.width - draw_w) * 0.5f,
                              area.y + (area.height - draw_h) * 0.5f, draw_w, draw_h};
  canvas.draw_canvas(frame_, fitted);
  // 边框：让"预览区到哪里为止"在浅底主题下也清楚——贴紧**实际画面**（不是面板
  // 区域），否则边框会把留白也算进去，看起来像画面没对齐。
  const Path outline = st::raster::make_rounded_rect(fitted, 2.0f);
  canvas.stroke_path(outline, Paint::solid(border_), 1.0f);
  (void)context;
}

auto MonitorView::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "frame_hash") return std::format("{:016x}", frame_hash_);
  if (name == "frame_time") return format_timecode(frame_time_ < 0.0 ? 0.0 : frame_time_);
  return std::nullopt;
}

auto MonitorView::property_names() const -> std::vector<std::string_view> {
  return {"frame_hash", "frame_time"};
}

auto MonitorView::invoke_action(std::string_view action, std::string_view argument) -> bool {
  // 让 e2e 能**强制**重合成（对照用：同一时刻渲染两次应得同一哈希）。
  if (action == "refresh") {
    (void)argument;
    refresh();
    return true;
  }
  return false;
}

}  // namespace caiyun

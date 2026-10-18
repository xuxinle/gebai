/// 裁云的视图实现（见 `views.hpp`）。
///
/// 时间轴段是**薄适配器**（caiyun 拆解步骤 6）：手绘的 450 行已删，几何/绘制/
/// 命中/拖拽全部来自内置 `st::ui::TimelineView`；本文件只做三件事——投影
///（Project→Model 重建）、编辑转发（EditDelegate→Project API）、控制协议薄壳
///（九动作四属性，e2e 契约字节不变）。

#include "views.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <span>
#include <string>

#include "motion.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"

namespace caiyun {

namespace {

using st::raster::Paint;
using st::raster::Path;
using st::ui::Event;
using st::ui::EventKind;

/// 时间轴各带的高度（逻辑像素）——与旧手写版同一套内容几何：两条 40px 轨带 +
/// 6px 间隙。标尺高度用内置件的 kRulerHeight（24px，旧版 22px——视觉差 2px，
/// 换件后时间轴区域布局允许的差异）。
constexpr float kTrackHeight = 40.0f;
constexpr float kTrackGap = 6.0f;
/// 换件后的缺省总高（内置件标尺 + 两条 (40+6) 轨带 + 4px 余量；main.cpp 用
/// 显式 height=132 覆盖，此值只兜未布局的量测）。
constexpr float kTimelineHeight = st::ui::TimelineView::kRulerHeight +
                                  (kTrackHeight + kTrackGap) * 2.0f + 4.0f;

/// 秒 → 毫秒（`st::ui::timeline::Model` 的时间单位；换算集中在此一处）。
[[nodiscard]] auto to_ms(double seconds) -> std::int64_t {
  return static_cast<std::int64_t>(std::llround(seconds * 1000.0));
}
/// 毫秒 → 秒（同上的反向；delegate 回调与播放头读回都经它）。
[[nodiscard]] auto to_seconds(st::ui::timeline::Millis ms) -> double {
  return static_cast<double>(ms) / 1000.0;
}

/// 片段块的配色：场景片段按**场景模板**区分（一眼看出这条轨道上是什么内容），
/// 生成器片段按生成器类型。换件后经 `timeline::Clip::tint` 投影进内置件。
[[nodiscard]] auto generator_color(GeneratorKind kind) -> st::math::Color {
  switch (kind) {
    case GeneratorKind::ColorBars: return st::math::Color{78, 110, 180, 255};
    case GeneratorKind::Subtitle: return st::math::Color{150, 96, 160, 255};
    case GeneratorKind::Shapes: return st::math::Color{70, 140, 120, 255};
    case GeneratorKind::TestChart: return st::math::Color{150, 120, 70, 255};
  }
  return st::math::Color{90, 90, 90, 255};
}

[[nodiscard]] auto scene_color(SceneKind kind) -> st::math::Color {
  switch (kind) {
    case SceneKind::Title: return st::math::Color{86, 122, 208, 255};
    case SceneKind::Terminal: return st::math::Color{64, 156, 132, 255};
    case SceneKind::Triptych: return st::math::Color{176, 124, 84, 255};
    case SceneKind::Metrics: return st::math::Color{112, 104, 196, 255};
    case SceneKind::Outro: return st::math::Color{132, 108, 168, 255};
    // 自定义布局用中性灰：它不代表某种内容类型，只是"这段是自由排版"。
    case SceneKind::Custom: return st::math::Color{110, 118, 130, 255};
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

// ── TimelineView（薄适配器）─────────────────────────────────────────────────

/// EditDelegate 实现：编辑全部转发 Project（真值源）；吸附按 Project 真值平移。
struct TimelineView::Delegate final : public st::ui::TimelineView::EditDelegate {
  TimelineView* owner{nullptr};
  /// 正被拖拽的 Project 片段 id（适配器在 MouseDown 选中回投时记下、MouseUp
  /// 清除）——snap 排除"被拖片段自身"要**精确身份**，而 delegate 的 snap 回调
  /// 只拿到时间（exclude 是拖拽前起点，Project 里片段已在前几帧移走，按时间
  /// 反查会扑空或误伤同刻度的其他片段）。
  int drag_id{-1};

  /// Model 下标 → Project 片段 id（id 是 Model 投影的 key：sync 时以字符串存入）。
  [[nodiscard]] auto clip_id(std::size_t track, std::size_t clip) const -> int {
    if (owner == nullptr || owner->project_ == nullptr) return -1;
    const st::ui::timeline::Clip* item = owner->view_->model().find_clip(track, clip);
    if (item == nullptr) return -1;
    try {
      return std::stoi(item->id);
    } catch (...) {
      return -1;
    }
  }

  auto move(std::size_t track, std::size_t clip,
            st::ui::timeline::Millis new_start) -> bool override {
    const int id = clip_id(track, clip);
    if (id < 0) return false;
    return owner->project_->move_clip(id, to_seconds(new_start));
  }

  auto trim(std::size_t track, std::size_t clip, Edge edge,
            st::ui::timeline::Millis time) -> bool override {
    const int id = clip_id(track, clip);
    if (id < 0) return false;
    // Project::trim 的口径：edge 0=左 1=右；time 左=新入点、右=新出点时刻——
    // 与 EditDelegate::trim 的口径逐字相同（换算只差秒↔毫秒）。
    const int project_edge = edge == Edge::Left ? 0 : 1;
    return owner->project_->trim(id, project_edge, to_seconds(time));
  }

  auto snap(st::ui::timeline::Millis candidate,
            st::ui::timeline::Millis exclude) -> st::ui::timeline::Millis override {
    (void)exclude;   // 排除身份走 drag_id_（见成员注释），时间口径不用于反查
    if (owner == nullptr || owner->project_ == nullptr) return candidate;
    if (!owner->project_->snapping) return candidate;
    // 阈值换算：Project 记秒（snap_threshold），Model 要像素——秒 × pps 等价
    // 旧手感（旧实际像素阈值 ≈ 0.12s × 48px/s ≈ 5.8px）。
    st::ui::timeline::SnapOptions options;
    options.threshold_px = static_cast<float>(
        owner->project_->snap_threshold *
        static_cast<double>(owner->view_->model().pixels_per_second()));
    // 本回调只在**片段拖拽**里被调（播放头不经 delegate）：片段可吸播放头
    //（旧版 exclude_id>=0 时 to_playhead=true 的同一语义）。
    options.to_playhead = true;
    // 排除被拖片段自身（不排会"永远吸在原位"——吸附实现最经典的错法）。
    std::size_t exclude_track = st::ui::timeline::Model::kNone;
    std::size_t exclude_clip = st::ui::timeline::Model::kNone;
    if (drag_id >= 0) {
      const auto [lane, index] =
          owner->view_->model().locate(std::to_string(drag_id));
      if (lane != st::ui::timeline::Model::kNone) {
        exclude_track = lane;
        exclude_clip = index;
      }
    }
    return owner->view_->model().snap(candidate, options, exclude_track, exclude_clip);
  }
};

// 析构留在此处（hpp 内联会在 Delegate 不完整处实例化 unique_ptr 删除器）。
TimelineView::~TimelineView() = default;

TimelineView::TimelineView() {
  set_focusable(true);
  // 紧凑双轨形态（方案 S4）：关掉 88px 轨道名前缀栏，内容占满整幅宽度。
  view_->set_show_track_headers(false);
  // **换件行为差异点，必须开**（方案步骤 6 验收清单）：caiyun 旧版"选中即画
  // 修剪手柄"；内置件默认不画——不补这行，换件后选中片段看不到修剪手柄，
  // 交互面静默缺失。
  view_->set_show_trim_handles(true);
  delegate_ = std::make_unique<Delegate>();
  delegate_->owner = this;
  view_->set_edit_delegate(delegate_.get());
}

void TimelineView::set_project(Project* project) {
  project_ = project;
  sync_from_project();
  mark_dirty();
}

void TimelineView::sync_from_project() const {
  if (project_ == nullptr) {
    view_->model() = {};
    return;
  }
  // 旧「全时长铺满视口」的换算语义经 Model 等价重建：pps = 视口宽 / 工程时长
  //（滚动恒 0）。视口宽取当前 bounds（未布局时兑底 480）。
  const float lane_width = bounds().width > 1.0f ? bounds().width : 480.0f;
  view_->model().set_viewport_width(lane_width);
  view_->model().set_pixels_per_second(static_cast<float>(
      static_cast<double>(lane_width) / std::max(0.001, project_->duration)));
  view_->model().set_scroll(0);
  view_->model().set_playhead(to_ms(project_->playhead));

  // 轨道按视觉序（V2 在上 = Model 下标 0，与 hit_test 的"上层先抓"一致）；
  // 带高含 6px 轨道间隙，与旧 track_rect 的阶梯几何一致。选中态与配色都从
  // Project 真值反推（L3：本类零自存选中）。
  std::vector<st::ui::timeline::Track> lanes;
  lanes.reserve(2);
  for (int visual = 0; visual < 2; ++visual) {
    st::ui::timeline::Track lane;
    lane.label = visual == 0 ? "V2" : "V1";
    lane.height = kTrackHeight + kTrackGap;
    for (const Clip& clip : project_->clips) {
      if (clip.track != (visual == 0 ? 1 : 0)) continue;
      // 场景片段的标签写场景模板名（比"色板/字幕"更能说明这段是什么）。
      const Scene* scene = project_->scene_of(clip);
      const std::string label =
          scene != nullptr ? std::string(scene_kind_name(scene->kind)) : to_string(clip.generator);
      const st::math::Color tint =
          scene != nullptr ? scene_color(scene->kind) : generator_color(clip.generator);
      lane.clips.push_back(st::ui::timeline::Clip{
          .id = std::to_string(clip.id),
          .label = label,
          .start = to_ms(clip.t_in),
          .duration = to_ms(clip.t_out) - to_ms(clip.t_in),
          .source_in = to_ms(clip.source_in),
          .selected = clip.id == project_->selected,
          .tint = tint,
      });
    }
    lanes.push_back(std::move(lane));
  }
  view_->model().set_tracks(std::move(lanes));
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

void TimelineView::apply_theme(const st::ui::Theme& theme) { view_->apply_theme(theme); }

void TimelineView::measure(const st::ui::RenderContext& context,
                           const st::ui::Constraints& constraints) {
  (void)context;
  float width = 480.0f;
  float height = kTimelineHeight;
  if (constraints.max_width < st::ui::kUnbounded) width = constraints.max_width;
  if (constraints.max_height < st::ui::kUnbounded) height = constraints.max_height;
  measured_ = st::math::Size{width, height};
}

void TimelineView::arrange(const st::ui::RenderContext& context, st::math::Rect rect) {
  Element::arrange(context, rect);
  view_->arrange(context, rect);
}

void TimelineView::paint_content(const st::ui::RenderContext& context,
                                 st::raster::Surface& canvas) const {
  if (project_ == nullptr) return;
  sync_from_project();
  view_->paint(context, canvas);
}

auto TimelineView::on_event(const st::ui::RenderContext& context, Event& event) -> bool {
  if (project_ == nullptr) return false;
  // 旧版不消费滚轮（工程恒铺满视口，无缩放/滚动语义）——内置件的滚轮分支
  //（缩放/平移）与本适配器的「每次交互后按 Project 重刷投影」相抵（下一帧
  // 就被 pps/scroll 复位打回），消费了也只有一帧抖动。直接放行冒泡，行为与
  // 旧版一致。
  if (event.kind == EventKind::Wheel) return false;

  sync_from_project();
  const double playhead_before = project_->playhead;
  const int selected_before = project_->selected;
  const bool handled = view_->on_event(context, event);
  if (!handled) return false;

  // ① 选中回投：内置件把选中写在**投影**里（点谁选谁），真值须回 Project
  //（L3 反向——检查器/selected 属性都读 Project，双源即缺陷面）。
  const auto picked = view_->model().selected_clips();
  if (picked.size() == 1) {
    const st::ui::timeline::Clip* item =
        view_->model().find_clip(picked[0].first, picked[0].second);
    if (item != nullptr) {
      try {
        project_->selected = std::stoi(item->id);
      } catch (...) {
      }
    }
  }
  if (event.kind == EventKind::MouseDown) {
    delegate_->drag_id = project_->selected;   // 潜在拖拽目标（snap 排除用）
  } else if (event.kind == EventKind::MouseUp) {
    delegate_->drag_id = -1;
  }

  // ② 播放头回投：内置件改的是投影 Model 的 playhead——投到 Project 真值
  //（含 [0, duration] 夹取，旧版 x_to_time 的夹取语义在此保留）。
  const double playhead_after = to_seconds(view_->model().playhead());
  const bool playhead_moved = playhead_after != playhead_before;
  if (playhead_moved) {
    project_->playhead = std::clamp(playhead_after, 0.0, project_->duration);
  }

  // ③ 投影按真值重刷：编辑已由 delegate 落进 Project（内置件的乐观写只是
  // 跟手中间态），播放头夹取、pan 的 scroll 复位也在此收口——绘制永远读
  // 真值投影。拖拽中重建投影是 delegate 形态的预期用法（S4 步骤 4 契约）。
  sync_from_project();

  if (playhead_moved) {
    notify_seek();   // 双通知：on_seek（监视器每帧跟手）+ on_project_changed
  } else if (project_->selected != selected_before) {
    notify_changed();   // 点选：检查器/素材箱刷新
  } else if (event.kind == EventKind::MouseMove) {
    notify_seek();   // 片段拖拽帧：每步通知（B2「监视器跟手」的旧语义）
  }
  mark_dirty();
  return true;
}

auto TimelineView::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "selected") {
    return std::to_string(project_ == nullptr ? -1 : project_->selected);   // L3：真值在 Project
  }
  if (name == "clip_count") {
    return std::to_string(project_ == nullptr ? 0U : project_->clips.size());
  }
  if (name == "playhead") {
    return project_ == nullptr ? std::string("0") : format_timecode(project_->playhead);
  }
  if (name == "transport") {
    return project_ != nullptr && project_->playing ? std::string("playing") : std::string("paused");
  }
  // —— 读面补齐：写得到就要读得回 ——
  //
  // 动因（实测）：`set_opacity`/`set_speed`/`trim`/`move` 都写得到，但属性面只有
  // selected/clip_count/playhead/transport——于是**人调过的值 AI 读不回来**，
  // “AI 为主、人机共用”就断在了共享真值无法被双方读出这一步。
  //
  // `clip` 用行式给整张片段表（AI 一次拉全，比逐 id 多轮往返省得多）；
  // `clip.<字段>` 是单片段快照。
  if (project_ == nullptr) return std::nullopt;
  if (name == "clip") return clip_table();
  if (name.starts_with("clip.")) {
    const int id = parse_id(name.substr(5));
    const Clip* clip = project_->find_clip(id);
    if (clip == nullptr) return std::nullopt;
    const std::string_view field = name.substr(5 + std::to_string(id).size() + 1);
    return clip_field(*clip, field);
  }
  return std::nullopt;
}

auto TimelineView::parse_id(std::string_view text) -> int {
  int value = 0;
  bool any = false;
  for (const char c : text) {
    if (c < '0' || c > '9') break;
    value = value * 10 + (c - '0');
    any = true;
  }
  return any ? value : -1;
}

auto TimelineView::clip_field(const Clip& clip, std::string_view field) -> std::optional<std::string> {
  if (field == "track") return std::to_string(clip.track);
  if (field == "in") return std::format("{:.3f}", clip.t_in);
  if (field == "out") return std::format("{:.3f}", clip.t_out);
  if (field == "duration") return std::format("{:.3f}", clip.duration());
  if (field == "source_in") return std::format("{:.3f}", clip.source_in);
  if (field == "opacity") return std::format("{:.3f}", clip.opacity);
  if (field == "speed") return std::format("{:.3f}", clip.speed);
  if (field == "scene") return std::to_string(clip.scene_index);
  return std::nullopt;
}

auto TimelineView::clip_table() const -> std::string {
  // 一行一片段、空格分隔字段、换行分段——比 JSON 好读，也比“只报个数”有信息量。
  // 字段：`id track in out opacity speed scene`（后三项是写入口能改的东西）。
  std::string out;
  for (const Clip& clip : project_->clips) {
    if (!out.empty()) out.push_back('\n');
    out += std::format("{} {} {:.3f} {:.3f} {:.3f} {:.3f} {}", clip.id, clip.track, clip.t_in,
                       clip.t_out, clip.opacity, clip.speed, clip.scene_index);
  }
  return out;
}

auto TimelineView::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "selected") {
    try {
      const int id = std::stoi(std::string(value));
      if (project_ == nullptr) return false;
      if (project_->find_clip(id) == nullptr) return false;   // 不存在的 id 如实拒绝
      project_->selected = id;                                // L3：选中真值只写 Project
      sync_from_project();
      mark_dirty();
      return true;
    } catch (...) {
      return false;
    }
  }
  return false;
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
      project_->selected = id;   // L3：真值留 Project，视图零自存
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
    const int id = argument.empty() ? project_->selected : std::stoi(std::string(argument));
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
    project_->selected = id;   // 新片段即选中（真值走 Project）
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
  if (action == "set_opacity" || action == "set_speed") {
    // 形式：`<片段 id> <数值>`——检查器的滑杆改的是同一个量，控制通道要有等价入口
    //（否则“AI 能改字幕却不能改透明度”，两个入口的能力就分叉了）。
    //
    // **必须走 Project 的 setter**（而不是直接写 `clip->opacity`）：AI 控制态的
    // 门禁长在 Project 里；绕过去就是“AI 在剪、用户还能把画面调没”的真实缺口
    //（实测：这里直接写字段时 `set_opacity` 在锁定态仍返回 true）。
    const std::size_t space = argument.find(' ');
    if (space == std::string_view::npos) return false;
    try {
      const int id = std::stoi(std::string(argument.substr(0, space)));
      const double value = std::stod(std::string(argument.substr(space + 1)));
      const bool ok = action == "set_opacity"
                          ? project_->set_opacity(id, static_cast<float>(value))
                          : project_->set_speed(id, static_cast<float>(value));
      if (ok) notify_changed();
      return ok;
    } catch (...) {
      return false;
    }
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

  // 合成目标保持 **16:9** 且与窗口大小解耦（“同一时刻两次渲染逐像素相同”这条
  // 判据不依赖窗口尺寸）；分辨率从 320×180 提到 **960×540**：320 宽的预览里
  // 设计字号 188 的大标题只剩 ~31px，且细线/字距看不出来——预览因此失去验收价值。
  constexpr int kWidth = 960;
  constexpr int kHeight = 540;
  const double time = project_->playhead;
  if (!frame_valid_ || frame_time_ != time) {
    const auto resized = frame_.resize(kWidth, kHeight);
    if (!resized.has_value()) return;
    frame_.clear(st::math::Color{0, 0, 0, 255});
    // ⚠ 合成时必须把**真实文本端口**传进去：不传的话文字类内容退化为几何占位块。
    project_->render_frame(frame_, time, context.text);
    // ⚠ **必须在渲染之后取像素**：`pixels()` 是只读视图，位置对了才有意义。
    frame_hash_ = fnv1a(frame_.pixels());
    frame_time_ = time;
    frame_valid_ = true;
  }
  // ⚠ **必须保持 16:9**：直接拉到 `area` 会在宽面板里把画面横拉
  //（实测：预览区 3.3:1 时圆形变成椭圆、字幕拉伸）。
  const float scale = std::min(area.width / static_cast<float>(kWidth),
                               area.height / static_cast<float>(kHeight));
  const float draw_w = static_cast<float>(kWidth) * scale;
  const float draw_h = static_cast<float>(kHeight) * scale;
  const st::math::Rect fitted{area.x + (area.width - draw_w) * 0.5f,
                              area.y + (area.height - draw_h) * 0.5f, draw_w, draw_h};
  canvas.draw_canvas(frame_, fitted);
  // 边框：让“预览区到哪里为止”在浅底主题下也清楚——贴紧**实际画面**（不是面板
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

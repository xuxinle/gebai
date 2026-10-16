#include "st/ui/components/timeline_view.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "st/raster/paint.hpp"
#include "st/ui/theme.hpp"

#include "components_internal.hpp"

namespace st::ui {

namespace {

using timeline::Millis;

/// 修剪时的最小片段长度（毫秒）。0 长度的片段无法再被抓住（命中判定要 `duration > 0`），
/// 而"拖成 0 就再也抓不住"比"拖不动"更糟——所以给一个下限。
constexpr Millis kMinClipDuration{16};
/// 中键（与视口容器同一约定）。
constexpr int kMiddleButton{3};

}  // namespace

TimelineView::TimelineView() {
  // 时间轴内容超出自身矩形（轨道多/时间长），必须裁剪——`clip_children` 同时决定
  // 绘制与命中（不设会出现"看不见却抢点击"）。
  style_.clip_children = true;
  set_focusable(true);
}

void TimelineView::notify_model_replaced() {
  // 投影刷新防线（见头文件）：旧快照指向旧投影，后续 move 会拿它改新下标的片段。
  // 拖拽中途换数据的正确语义是「松手」——拖拽回到无拖拽态，指针下的新内容
  // 由下一次 MouseDown 重新抓取。
  drag_ = Drag{};
  dragging_playhead_ = false;
  hover_track_ = kNoIndex;
  hover_clip_ = kNoIndex;
  mark_dirty();
}

void TimelineView::apply_theme(const Theme& theme) {
  const Palette& colors = theme.colors();
  surface_ = colors.surface;
  surface_alt_ = colors.surface_alt;
  surface_sunken_ = colors.surface_sunken;
  border_ = colors.border;
  text_color_ = colors.text;
  text_muted_ = colors.text_muted;
  accent_ = colors.primary;
  // 片段块与选中态都用主题令牌（硬编码在另一主题上会变成刺眼的块）。
  // 悬停色不在此存：它从基色现算（`base.lighten(0.10)`，见 paint_content）——
  // 片段 tint 可选化后每个片段的基色可能不同，悬停提亮跟着基色走而不是主题走。
  clip_fill_ = colors.primary_soft;
  clip_selected_ = colors.primary;
  playhead_color_ = colors.danger;
  muted_track_ = colors.surface_alt;
  font_size_ = theme.metrics().font_xs;
}

auto TimelineView::lane_rect() const -> math::Rect {
  const float top = bounds_.y + (show_ruler_ ? kRulerHeight : 0.0f);
  // 头部栏可关（caiyun 拆解步骤 5）：关掉后 lane 从 bounds 左缘起——所有几何面
  // （lane/标尺/轨道带/命中坐标）都经这里，开关只改这一个偏移量，不散落。
  const float header = show_track_headers_ ? kTrackHeaderWidth : 0.0f;
  return math::Rect{bounds_.x + header, top,
                    std::max(0.0f, bounds_.width - header),
                    std::max(0.0f, bounds_.bottom() - top)};
}

auto TimelineView::ruler_rect() const -> math::Rect {
  if (!show_ruler_) return {};
  const float header = show_track_headers_ ? kTrackHeaderWidth : 0.0f;
  return math::Rect{bounds_.x + header, bounds_.y,
                    std::max(0.0f, bounds_.width - header), kRulerHeight};
}

auto TimelineView::time_to_x(Millis time) const -> float {
  // 模型的 `time_to_x` 以"lane 左缘"为 0；屏幕位置再加上 lane 的 x。
  return lane_rect().x + model_.time_to_x(time);
}

auto TimelineView::x_to_time(float screen_x) const -> timeline::Millis {
  return model_.x_to_time(screen_x - lane_rect().x);
}

auto TimelineView::track_band(std::size_t index) const -> math::Rect {
  const auto [top, bottom] = model_.track_rect(index);
  const math::Rect lane = lane_rect();
  return math::Rect{bounds_.x, lane.y + top, bounds_.width, std::max(0.0f, bottom - top)};
}

auto TimelineView::clip_rect(std::size_t track, std::size_t clip) const -> math::Rect {
  const timeline::Clip* item = model_.find_clip(track, clip);
  if (item == nullptr || item->duration <= 0) return {};
  const auto [left, right] = model_.clip_x_range(track, clip);
  const math::Rect band = track_band(track);
  const float x0 = lane_rect().x + left;
  const float x1 = lane_rect().x + right;
  // 块内缩 2px：相邻片段之间留一道缝（贴在一起时看不出是两块）。
  return math::Rect{x0, band.y + 2.0f, std::max(0.0f, x1 - x0), std::max(0.0f, band.height - 4.0f)};
}

void TimelineView::measure(const RenderContext& context, const Constraints& constraints) {
  (void)context;
  const float width = style_.has_explicit_width() ? style_.width
                     : bounds_.width > 0.0f        ? bounds_.width
                                                   : constraints.max_width;
  const float height = style_.has_explicit_height() ? style_.height
                      : bounds_.height > 0.0f        ? bounds_.height
                                                     : constraints.max_height;
  measured_ = math::Size{std::max(0.0f, width), std::max(0.0f, height)};
}

void TimelineView::arrange(const RenderContext& context, math::Rect rect) {
  Element::arrange(context, rect);
  // 模型要知道视口宽度才能算可见区间与滚动上限。
  model_.set_viewport_width(lane_rect().width);
}

void TimelineView::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  const TextPort& port = components_internal::text_port_of(context);
  const math::Rect lane = lane_rect();
  const timeline::Millis visible_from = x_to_time(lane.x);
  const timeline::Millis visible_to = x_to_time(lane.right());

  // 底：轨道区用凹面色（与"画布外"区分开）。
  canvas.fill_rect(lane, raster::Paint::solid(surface_sunken_), 0.0f);

  // —— 轨道带（交替底色 + 名称 + 锁定/静音标记）——
  for (std::size_t index = 0; index < model_.track_count(); ++index) {
    const timeline::Track& track = model_.tracks()[index];
    const math::Rect band = track_band(index);
    if (band.y + band.height < bounds_.y || band.y > bounds_.bottom()) continue;
    const bool odd = index % 2U == 1U;
    const math::Color band_color = track.locked ? muted_track_ : (odd ? surface_alt_ : surface_);
    if (show_track_headers_) {
      canvas.fill_rect(math::Rect{bounds_.x, band.y, lane.x - bounds_.x, band.height},
                       raster::Paint::solid(band_color), 0.0f);
    }
    canvas.fill_rect(math::Rect{lane.x, band.y, lane.width, band.height},
                     raster::Paint::solid(odd ? surface_alt_ : surface_), 0.0f);
    // 轨道分隔线（1px 实心条——两个轨道挨着时靠它分辨）。
    canvas.fill_rect(math::Rect{bounds_.x, band.bottom() - 1.0f, bounds_.width, 1.0f},
                     raster::Paint::solid(border_), 0.0f);
    if (show_track_headers_) {
      const float line = port.line_height(font_size_);
      port.draw(canvas, track.label.empty() ? track.id : track.label,
                math::Point{bounds_.x + 8.0f, band.y + (band.height - line) * 0.5f}, font_size_,
                track.locked ? text_muted_ : text_color_);
      // 锁定标记（在名称右侧，用文字而不是图标：无图标后端也要能看）。
      if (track.locked) {
        const float width = port.measure_width("锁", font_size_);
        port.draw(canvas, "锁", math::Point{lane.x - width - 10.0f, band.y + 4.0f}, font_size_,
                  text_muted_);
      }
    }
  }

  // —— 片段 ——
  for (std::size_t track = 0; track < model_.track_count(); ++track) {
    for (std::size_t index = 0; index < model_.tracks()[track].clips.size(); ++index) {
      const timeline::Clip& clip = model_.tracks()[track].clips[index];
      if (clip.duration <= 0) continue;
      if (!clip.intersects(visible_from, visible_to)) continue;   // 视口剔除
      const math::Rect block = clip_rect(track, index);
      if (block.is_empty()) continue;
      const bool hovered = track == hover_track_ && index == hover_clip_;
      // 基色：片段 tint 可选（宿主按内容着色）——`nullopt` 走主题令牌（默认零变化），
      // 有值则 tint 作为常态基色参与：hover 在其上提亮一档、选中仍用主题选中色
      //（选中是组件状态语义，不让宿主配色夺走它——两段选色保持同一层次结构）。
      const math::Color base = clip.tint.value_or(clip_fill_);
      // 悬停态：基色同向提亮一档——不用 `mix` 掺 `surface_hover`（悬停稳定后那是
      // 完全替换，会把 primary_soft 洗成中性灰；Element 框体层 hover 实测踩过的坑，
      // 同法不同层不重蹈）。量级 0.10 与框体层同档，保证任何主题/任何 tint 下
      // 「提亮但不刺眼」——tint 是宿主配色，提亮系数是组件的，两者叠加不互相劫持。
      const math::Color hover_of_base = base.lighten(0.10f);
      // 选中态优先于悬停色（选中是持续状态）；选中+悬停叠加时再轻提一档
      //（0.05 < 0.10：选中色已比常态亮，再满幅提亮会过曝）。
      math::Color fill = clip.selected ? clip_selected_ : (hovered ? hover_of_base : base);
      if (clip.selected && hovered) fill = clip_selected_.lighten(0.05f);
      canvas.fill_rect(block, raster::Paint::solid(fill), 3.0f);
      components_internal::paint_outline(canvas, block, 3.0f,
                                          hovered ? accent_ : border_, hovered ? 1.5f : 1.0f);
      // 修剪手柄：`show_trim_handles_` 打开且选中时，左右缘各一道竖条
      //（不默认开——全时段可见会让时间轴很吵，与 caiyun 手写版同一取舍）。
      if (show_trim_handles_ && clip.selected && block.width > 12.0f) {
        canvas.fill_rect(math::Rect{block.x, block.y, 2.0f, block.height},
                         raster::Paint::solid(math::Color{255, 255, 255, 200}), 0.0f);
        canvas.fill_rect(math::Rect{block.right() - 2.0f, block.y, 2.0f, block.height},
                         raster::Paint::solid(math::Color{255, 255, 255, 200}), 0.0f);
      }
      // 标签：**装不下就不画**（截断成半句比不画更难读——与行尾注解同一约定）。
      const float available = block.width - 8.0f;
      if (available > 12.0f && !clip.label.empty()) {
        const std::string shown = port.ellipsize(clip.label, font_size_, available);
        if (!shown.empty() && port.measure_width(shown, font_size_) <= available) {
          port.draw(canvas, shown, math::Point{block.x + 4.0f, block.y + 3.0f}, font_size_,
                    text_color_);
        }
      }
    }
  }

  // —— 标尺 ——
  if (const math::Rect ruler = ruler_rect(); !ruler.is_empty()) {
    canvas.fill_rect(ruler, raster::Paint::solid(surface_alt_), 0.0f);
    canvas.fill_rect(math::Rect{ruler.x, ruler.bottom() - 1.0f, ruler.width, 1.0f},
                     raster::Paint::solid(border_), 0.0f);
    const Millis step = timeline::ruler_step(visible_to - visible_from,
                                             model_.pixels_per_second());
    if (step > 0) {
      const float line = port.line_height(font_size_);
      // 从可见区间的**第一步**开始（对齐到 step 的整数倍——不这样刻度会随滚动"漂移"）。
      Millis tick = (visible_from / step) * step;
      if (tick < visible_from) tick += step;
      int guard = 0;
      while (tick <= visible_to && guard < 512) {
        ++guard;
        const float x = time_to_x(tick);
        if (x >= ruler.x - 1.0f && x <= ruler.right() + 1.0f) {
          canvas.fill_rect(math::Rect{x, ruler.y + ruler.height * 0.5f, 1.0f,
                                      ruler.height * 0.5f},
                           raster::Paint::solid(text_muted_), 0.0f);
          const std::string label = timeline::format_timecode(tick, true);
          // 标签**右对齐到刻度左侧**（让它属于"这一刻"而不是"下一段"）。
          const float width = port.measure_width(label, font_size_);
          port.draw(canvas, label,
                    math::Point{std::min(x + 3.0f, ruler.right() - width - 2.0f),
                                ruler.y + (ruler.height * 0.5f - line) * 0.5f},
                    font_size_, text_muted_);
        }
        tick += step;
      }
    }
  }

  // —— 播放头（画在最上层：它细，被压住就抓不着）——
  const Millis head = model_.playhead();
  if (head >= visible_from && head <= visible_to) {
    const float x = time_to_x(head);
    canvas.fill_rect(math::Rect{x, bounds_.y, 2.0f, bounds_.height},
                     raster::Paint::solid(playhead_color_), 0.0f);
    // 顶部三角柄（拖拽的抓点；用一串小方块近似三角形——不依赖 Path 也不必引图标）。
    for (int row = 0; row < 6; ++row) {
      const float width = 12.0f - static_cast<float>(row) * 1.6f;
      canvas.fill_rect(math::Rect{x + 1.0f - width * 0.5f, bounds_.y + static_cast<float>(row),
                                  width, 1.0f},
                       raster::Paint::solid(playhead_color_), 0.0f);
    }
  }
}

auto TimelineView::on_event(const RenderContext& /*context*/, Event& event) -> bool {
  const math::Rect lane = lane_rect();
  switch (event.kind) {
    case EventKind::MouseDown: {
      const bool middle = event.button == kMiddleButton;
      // ① 标尺/播放头带：点它就跳转（"点标尺定位"是最直觉的手势）。
      if (event.button == 1 && show_ruler_) {
        const math::Rect ruler = ruler_rect();
        if (ruler.contains(event.position)) {
          model_.set_playhead(std::max<Millis>(0, x_to_time(event.position.x)));
          dragging_playhead_ = true;
          drag_.kind = Drag::Kind::Playhead;
          mark_dirty();
          if (on_playhead_changed) on_playhead_changed(model_.playhead());
          event.handled = true;
          return true;
        }
      }
      // ② 中键 = 平移（与 `Viewport` 同一手势；不与左键抢）。
      if (middle && lane.contains(event.position)) {
        drag_.kind = Drag::Kind::Pan;
        drag_.origin = event.position;
        drag_.scroll = model_.scroll();
        event.handled = true;
        return true;
      }
      if (event.button != 1 || !lane.contains(event.position)) return false;

      // ③ 命中（模型负责"点到什么"；组件只翻译）。
      const float local_x = event.position.x - lane.x;
      const float local_y = event.position.y - lane.y;
      const timeline::Hit hit = model_.hit_test(local_x, local_y, kEdgePx);
      if (hit.playhead) {
        dragging_playhead_ = true;
        drag_.kind = Drag::Kind::Playhead;
        event.handled = true;
        return true;
      }
      if (hit.clip == timeline::Hit::kNone) {
        // 空白处：平移（见头文件里选它的理由）。
        drag_.kind = Drag::Kind::Pan;
        drag_.origin = event.position;
        drag_.scroll = model_.scroll();
        event.handled = true;
        return true;
      }
      const timeline::Track& track = model_.tracks()[hit.track];
      // **锁定轨道不接受编辑**（但仍可选中查看）：锁定的语义是"别动它"。
      if (track.locked) {
        model_.clear_selection();
        if (timeline::Clip* clip = model_.find_clip(hit.track, hit.clip); clip != nullptr) {
          clip->selected = true;
        }
        mark_dirty();
        event.handled = true;
        return true;
      }
      const timeline::Clip* clip = model_.find_clip(hit.track, hit.clip);
      if (clip == nullptr) return false;
      drag_.track = hit.track;
      drag_.clip = hit.clip;
      drag_.origin = event.position;
      drag_.clip_start = clip->start;
      drag_.clip_duration = clip->duration;
      drag_.clip_source_in = clip->source_in;
      switch (hit.part) {
        case timeline::ClipPart::LeftEdge: drag_.kind = Drag::Kind::ClipLeft; break;
        case timeline::ClipPart::RightEdge: drag_.kind = Drag::Kind::ClipRight; break;
        default: drag_.kind = Drag::Kind::ClipBody; break;
      }
      // 选中（点谁选谁——多选留给将来的框选需求）。
      model_.clear_selection();
      if (timeline::Clip* target = model_.find_clip(hit.track, hit.clip); target != nullptr) {
        target->selected = true;
      }
      mark_dirty();
      event.handled = true;
      return true;
    }
    case EventKind::MouseMove: {
      if (drag_.kind == Drag::Kind::None) {
        // 无拖拽时的悬停追踪：命中即记入 hover_*，变化才标重绘（每帧 MouseMove
        // 都 mark_dirty 会让整条时间轴跟着鼠标走——无谓的全重绘）。
        const float local_x = event.position.x - lane.x;
        const float local_y = event.position.y - lane.y;
        const timeline::Hit hit = lane.contains(event.position)
                                      ? model_.hit_test(local_x, local_y, kEdgePx)
                                      : timeline::Hit{};
        const std::size_t track = hit.clip != timeline::Hit::kNone ? hit.track : kNoIndex;
        const std::size_t clip = hit.clip != timeline::Hit::kNone ? hit.clip : kNoIndex;
        if (track != hover_track_ || clip != hover_clip_) {
          hover_track_ = track;
          hover_clip_ = clip;
          mark_dirty();
        }
        return false;   // 不消费：悬停态更新不阻断后续事件（光标形态等由宿主定）
      }
      if (drag_.kind == Drag::Kind::Playhead) {
        model_.set_playhead(std::max<Millis>(0, x_to_time(event.position.x)));
        if (on_playhead_changed) on_playhead_changed(model_.playhead());
        mark_dirty();
        event.handled = true;
        return true;
      }
      if (drag_.kind == Drag::Kind::Pan) {
        // 平移：指针右移 = 看更早的内容（scroll 减小）——与"抓着内容拖"一致。
        const float delta_px = event.position.x - drag_.origin.x;
        const double delta_ms = static_cast<double>(delta_px) /
                                static_cast<double>(std::max(0.001f, model_.pixels_per_second())) *
                                1000.0;
        model_.set_scroll(drag_.scroll - static_cast<Millis>(std::llround(delta_ms)));
        mark_dirty();
        event.handled = true;
        return true;
      }
      // 投影刷新防线的兜底：拖拽中宿主重建了数据（忘调 `notify_model_replaced`）
      // 时，快照已指向旧投影——拿旧下标继续写会改到**新投影里同下标的另一个片段**
      //（陈旧下标改错对象，步骤 3 要抓的目标缺陷）。判据不是「快照 == 当前值」
      //（正常拖拽中每帧 move 都会偏离），而是「快照 != 上一帧应有的落点」——
      // 正常拖拽里上一帧 move 后组件自己写下的值必然与快照一致（被宿主改过
      // 才会不一致），所以这里能同时识别「宿主绕道改了几何」与「整个重建」。
      const auto snapshot_stale = [this]() -> bool {
        if (drag_.kind != Drag::Kind::ClipBody && drag_.kind != Drag::Kind::ClipLeft &&
            drag_.kind != Drag::Kind::ClipRight) {
          return false;
        }
        const timeline::Clip* dragged = model_.find_clip(drag_.track, drag_.clip);
        return dragged == nullptr || dragged->start != drag_.clip_start ||
               dragged->duration != drag_.clip_duration ||
               dragged->source_in != drag_.clip_source_in;
      }();
      if (snapshot_stale) {
        // 与 `notify_model_replaced()` 同一套复位（这里是兜底，那里是契约入口）。
        drag_ = Drag{};
        dragging_playhead_ = false;
        hover_track_ = kNoIndex;
        hover_clip_ = kNoIndex;
        mark_dirty();
        // 松手：本次 move 不改任何片段（吃了事件，拖拽到此为止）。
        event.handled = true;
        return true;
      }
      timeline::Clip* clip = model_.find_clip(drag_.track, drag_.clip);
      if (clip == nullptr) return false;
      const float delta_px = event.position.x - drag_.origin.x;
      const double delta_ms = static_cast<double>(delta_px) /
                              static_cast<double>(std::max(0.001f, model_.pixels_per_second())) *
                              1000.0;
      const Millis delta = static_cast<Millis>(std::llround(delta_ms));

      const auto snap_time = [this](Millis candidate, Millis fallback) -> Millis {
        if (!snap_enabled_) return candidate;
        if (edit_delegate_ != nullptr) {
          // 委托形态的吸附也走宿主：吸附点在真值源里，组件投影里那套候选
          //（快照时刻的）未必与真值源同步——吸附解释权随编辑权一起移交。
          return edit_delegate_->snap(candidate, drag_.clip_start);
        }
        // ⚠ **排除被拖的那个片段**：不排除的话它会永远吸在自己原来的位置
        //（表现是"拖不动"——吸附实现最经典的错法）。
        const Millis snapped =
            model_.snap(candidate, snap_options_, drag_.track, drag_.clip);
        return snapped == candidate ? fallback : snapped;
      };

      // —— 编辑委托通道（S1）：真值源在宿主手里时，拖拽目标交宿主裁决 ——
      // 帧序是下面三段注释钉死的契约（改顺序 = 改语义，同族用例会红）：
      //   ①算目标（吸附本身也是宿主回调，期间投影可能被重建——指针随后重取）；
      //   ②乐观写投影：先把目标写进自持 Model（拖拽跟手的绘制来源）；
      //   ③裁决：回调宿主 move/trim——宿主改真值源、重进投影（set_tracks）
      //     都发生在这里，它的值永远最后落笔；false 则把投影恢复到裁决前
      //     （本帧存档，不是拖拽起点——否则会撤销之前已接受的帧）。
      if (edit_delegate_ != nullptr) {
        using Edge = EditDelegate::Edge;
        const bool is_move = drag_.kind == Drag::Kind::ClipBody;
        Edge edge = Edge::Left;
        // move：新起点 / trim：新起点（Left）或新结束时刻（Right）。
        Millis new_start = 0;
        Millis new_time = 0;
        Millis new_duration = 0;
        Millis new_source_in = 0;
        if (is_move) {
          // **移动**：只改 start（duration 与 source_in 都不动——动 duration
          // 就不是移动而是缩放了）。
          const Millis target = std::max<Millis>(0, drag_.clip_start + delta);
          new_start = snap_time(target, target);
          new_time = new_start;
        } else if (drag_.kind == Drag::Kind::ClipRight) {
          // **修剪出点**：改 duration；source_in 不动。
          edge = Edge::Right;
          const Millis target = std::max(kMinClipDuration, drag_.clip_duration + delta);
          const Millis snapped = snap_time(drag_.clip_start + target, target);
          new_duration = std::max(kMinClipDuration, snapped);
          new_time = drag_.clip_start + new_duration;
        } else {   // ClipLeft：**修剪入点**，start 与 source_in 同步走。
          edge = Edge::Left;
          const Millis raw_start = std::max<Millis>(0, drag_.clip_start + delta);
          Millis start_candidate = snap_time(raw_start, raw_start);
          const Millis moved = start_candidate - drag_.clip_start;
          // 左边缘不能越过右边缘（否则 duration 变负）。
          const Millis max_moved = drag_.clip_duration - kMinClipDuration;
          if (moved > max_moved) start_candidate = drag_.clip_start + max_moved;
          const Millis final_moved = start_candidate - drag_.clip_start;
          new_start = start_candidate;
          new_time = start_candidate;
          new_duration = drag_.clip_duration - final_moved;
          // 源入点不能小于 0（素材没有负时间）。
          new_source_in = std::max<Millis>(0, drag_.clip_source_in + final_moved);
        }

        // 乐观写投影（指针在算完目标后才取——上面的 snap_time 已回调过宿主，
        // 取早了可能被宿主重建失效）。裁决前投影的完整存档也在此处。
        timeline::Clip* current = model_.find_clip(drag_.track, drag_.clip);
        const bool has_slot = current != nullptr;
        const timeline::Clip snapshot = has_slot ? *current : timeline::Clip{};
        if (has_slot) {
          if (is_move) {
            current->start = new_start;
          } else if (edge == Edge::Right) {
            current->duration = new_duration;
          } else {
            current->start = new_start;
            current->duration = new_duration;
            current->source_in = new_source_in;
          }
        }

        const bool accepted =
            is_move ? edit_delegate_->move(drag_.track, drag_.clip, new_start)
                    : edit_delegate_->trim(drag_.track, drag_.clip, edge, new_time);
        if (accepted) {
          // 接受：投影已是目标（或宿主重建的新投影），交互回调照旧——宿主侧的
          // 撤销栈/检查器刷新/双通知语义都在这条上。
          if (is_move) {
            if (on_clip_moved) on_clip_moved(drag_.track, drag_.clip);
          } else if (on_clip_trimmed) {
            on_clip_trimmed(drag_.track, drag_.clip);
          }
        } else if (has_slot) {
          // 拒绝：恢复裁决前投影。指针再取一次（裁决回调里可能 set_tracks
          // 重建过）；**id 守卫**——重建后同下标可能是另一个片段，把旧存档写
          // 到它头上正是「下标当身份用」的坑（PITFALLS 场景 5 同族）。下标
          // 失效或 id 不符时不动投影（宿主的重建结果就是它要的投影）。
          timeline::Clip* restore = model_.find_clip(drag_.track, drag_.clip);
          if (restore != nullptr && restore->id == snapshot.id) {
            *restore = snapshot;
          }
        }
        mark_dirty();
        event.handled = true;
        return true;
      }

      if (drag_.kind == Drag::Kind::ClipBody) {
        // **移动**：只改 `start`（`duration` 与 `source_in` 都不动）。
        // 动 duration 就不是移动而是缩放了——那是编辑器里最容易被"顺手"搞错的一处。
        const Millis target = drag_.clip_start + delta;
        clip->start = std::max<Millis>(0, snap_time(target, target));
        if (on_clip_moved) on_clip_moved(drag_.track, drag_.clip);
      } else if (drag_.kind == Drag::Kind::ClipRight) {
        // **修剪出点**：改 `duration`；`source_in` 不动（出点变化不改变素材起点）。
        const Millis target = drag_.clip_duration + delta;
        const Millis snapped = snap_time(drag_.clip_start + target, target);
        clip->duration = std::max(kMinClipDuration, snapped);
        if (on_clip_trimmed) on_clip_trimmed(drag_.track, drag_.clip);
      } else if (drag_.kind == Drag::Kind::ClipLeft) {
        // **修剪入点**：`start` 与 `source_in` 同步走，`duration` 反向变。
        //
        // 为什么不只改 start：那样等于"把内容往后挪"（画面里片段位移了，内容没变），
        // 而修剪的语义是"丢掉素材开头的一段"——素材入点必须跟着走。
        // `duration` 要减去位移量，否则片段会变长（右边缘不动而左边内缩）。
        const Millis raw_start = drag_.clip_start + delta;
        Millis new_start = std::max<Millis>(0, snap_time(raw_start, raw_start));
        const Millis moved = new_start - drag_.clip_start;
        // 左边缘不能越过右边缘（否则 duration 变负）。
        const Millis max_moved = drag_.clip_duration - kMinClipDuration;
        if (moved > max_moved) {
          new_start = drag_.clip_start + max_moved;
        }
        const Millis final_moved = new_start - drag_.clip_start;
        clip->start = new_start;
        clip->duration = drag_.clip_duration - final_moved;
        // 源入点跟着走，但**不能小于 0**（素材没有负时间）。
        clip->source_in = std::max<Millis>(0, drag_.clip_source_in + final_moved);
        if (on_clip_trimmed) on_clip_trimmed(drag_.track, drag_.clip);
      }
      mark_dirty();
      event.handled = true;
      return true;
    }
    case EventKind::MouseUp: {
      if (drag_.kind == Drag::Kind::None) return false;
      const bool was_playhead = drag_.kind == Drag::Kind::Playhead;
      drag_ = Drag{};
      if (was_playhead) {
        // 抬起后把待决定的吸附**不动**（播放头不做吸附——它常用于"我要精确到这里"）。
        dragging_playhead_ = false;
      }
      mark_dirty();
      event.handled = true;
      return true;
    }
    case EventKind::HoverOut: {
      // 离开组件：清悬停（不清的话片段会永远亮着——“鼠标走了还亮”是悬停态
      // 最经典的残留缺陷）。拖拽中不清——拖拽时指针常滑出组件，清了会闪烁。
      if (drag_.kind == Drag::Kind::None &&
          (hover_track_ != kNoIndex || hover_clip_ != kNoIndex)) {
        hover_track_ = kNoIndex;
        hover_clip_ = kNoIndex;
        mark_dirty();
      }
      return false;
    }
    case EventKind::Wheel: {
      if (!lane.contains(event.position) || event.wheel_delta == 0.0f) return false;
      if (event.ctrl) {
        // Ctrl+滚轮 = 缩放，**以指针为锚**（与 `Viewport` 同一约定）。
        // 锚点保持：缩放前后**指针下的那一刻时间不变**。
        // 先记下缩放前指针处的时间，缩放后再把它"拉回"指针下。
        const Millis before = x_to_time(event.position.x);
        const float factor = event.wheel_delta > 0.0f ? 1.25f : 1.0f / 1.25f;
        model_.set_pixels_per_second(model_.pixels_per_second() * factor);
        const Millis after = x_to_time(event.position.x);
        model_.set_scroll(model_.scroll() + (before - after));
      } else {
        // 普通滚轮 = 横向滚动（时间轴的主要"看别处"手势）。
        const double pps = static_cast<double>(std::max(0.001f, model_.pixels_per_second()));
        const double step_ms = 1000.0 / pps * 80.0;
        model_.set_scroll(model_.scroll() - static_cast<Millis>(
                                               std::llround(static_cast<double>(event.wheel_delta) *
                                                            step_ms)));
      }
      mark_dirty();
      event.handled = true;
      return true;
    }
    default:
      return false;
  }
}

auto TimelineView::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "playhead") return std::to_string(model_.playhead());
  if (name == "pixels_per_second") {
    return std::format("{:.3f}", static_cast<double>(model_.pixels_per_second()));
  }
  if (name == "scroll") return std::to_string(model_.scroll());
  if (name == "tracks") return std::to_string(model_.track_count());
  if (name == "duration") return std::to_string(model_.content_duration());
  if (name == "snap") return snap_enabled_ ? "true" : "false";
  if (name == "hover_clip") {
    // 悬停态的读回口：与 Element 框体层 `hovered` 同理——"只能看像素"的状态
    // 不给读口，自动化验证就只能截图猜。形态 "track:clip"，无悬停为 "-1:-1"。
    return std::format("{}:{}", hover_track_ == kNoIndex ? -1 : static_cast<int>(hover_track_),
                       hover_clip_ == kNoIndex ? -1 : static_cast<int>(hover_clip_));
  }
  if (name == "trim_handles") return show_trim_handles_ ? "true" : "false";
  if (name == "track_headers") return show_track_headers_ ? "true" : "false";
  return Element::get_property(name);
}

auto TimelineView::set_property(std::string_view name, std::string_view value) -> bool {
  const auto parse_float = [&value]() -> std::optional<float> {
    try {
      std::size_t used = 0;
      const float parsed = std::stof(std::string(value), &used);
      if (used != value.size()) return std::nullopt;
      return parsed;
    } catch (...) {
      return std::nullopt;
    }
  };
  if (name == "playhead") {
    if (const auto parsed = parse_float(); parsed.has_value()) {
      model_.set_playhead(static_cast<Millis>(std::llround(*parsed)));
      mark_dirty();
      if (on_playhead_changed) on_playhead_changed(model_.playhead());
      return true;
    }
    return false;
  }
  if (name == "pixels_per_second") {
    if (const auto parsed = parse_float(); parsed.has_value()) {
      model_.set_pixels_per_second(*parsed);
      mark_dirty();
      return true;
    }
    return false;
  }
  if (name == "scroll") {
    if (const auto parsed = parse_float(); parsed.has_value()) {
      model_.set_scroll(static_cast<Millis>(std::llround(*parsed)));
      mark_dirty();
      return true;
    }
    return false;
  }
  if (name == "snap") {
    // 只认明确的两个词：把乱值当 false 会让"设错了"看起来像"功能坏了"。
    if (value == "true") {
      snap_enabled_ = true;
      return true;
    }
    if (value == "false") {
      snap_enabled_ = false;
      return true;
    }
    return false;
  }
  if (name == "trim_handles") {
    // 与 `snap` 同一严格口径（明确两个词，乱值如实拒绝）。
    if (value == "true") {
      set_show_trim_handles(true);
      return true;
    }
    if (value == "false") {
      set_show_trim_handles(false);
      return true;
    }
    return false;
  }
  if (name == "track_headers") {
    // 与 `snap`/`trim_handles` 同一严格口径（明确两个词，乱值如实拒绝）。
    if (value == "true") {
      set_show_track_headers(true);
      return true;
    }
    if (value == "false") {
      set_show_track_headers(false);
      return true;
    }
    return false;
  }
  return Element::set_property(name, value);
}

auto TimelineView::property_names() const -> std::vector<std::string_view> {
  auto names = Element::property_names();
  names.insert(names.end(), {"playhead", "pixels_per_second", "scroll", "tracks", "duration",
                             "snap", "hover_clip", "trim_handles", "track_headers"});
  return names;
}

auto TimelineView::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "zoom_in" || action == "zoom_out") {
    const float factor = action == "zoom_in" ? 1.25f : 1.0f / 1.25f;
    model_.set_pixels_per_second(model_.pixels_per_second() * factor);
    mark_dirty();
    return true;
  }
  if (action == "play") {
    // 归零（"回到开头"——播放由宿主驱动，组件只管播放头位置）。
    model_.set_playhead(0);
    mark_dirty();
    if (on_playhead_changed) on_playhead_changed(0);
    return true;
  }
  if (action == "scroll_to") {
    try {
      std::size_t used = 0;
      const auto parsed = std::stoll(std::string(argument), &used);
      if (used != argument.size()) return false;
      model_.set_scroll(parsed);
      mark_dirty();
      return true;
    } catch (...) {
      return false;
    }
  }
  return Element::invoke_action(action, argument);
}

}  // namespace st::ui

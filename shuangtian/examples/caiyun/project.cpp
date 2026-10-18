/// 裁云工程模型的实现（见 `project.hpp`）。
///
/// 程序化片段全部用**本地光栅原语**画（无解码依赖）：矩形/圆/描边/文本。
/// 每个生成器都是 `local_time` 的纯函数——同一时刻渲染两次结果逐像素相同，
/// 这是 e2e 确定性判据的前提（设计文档 §7.1）。

#include "project.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "fx.hpp"
#include "motion.hpp"
#include "st/raster/path.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/timeline.hpp"  // format_timecode 收口（拆解方案步骤 1）

namespace caiyun {

namespace {

using st::raster::Paint;
using st::raster::Path;

/// 最小片段时长（秒）——修剪/分割的护栏。
/// 为什么需要：允许零长片段会让时间轴上出现"看不见的块"，之后**再也选不中、
/// 删不掉**（它的矩形宽度为 0）。这类"看不见但占着数据"的条目是最难查的一类。
constexpr double kMinClipDuration = 0.1;

[[nodiscard]] auto mix(Color a, Color b, float t) -> Color {
  const auto channel = [t](std::uint8_t x, std::uint8_t y) {
    const float value = static_cast<float>(x) + (static_cast<float>(y) - static_cast<float>(x)) * t;
    return static_cast<std::uint8_t>(std::clamp(value, 0.0f, 255.0f));
  };
  return Color{channel(a.r, b.r), channel(a.g, b.g), channel(a.b, b.b), channel(a.a, b.a)};
}

/// 色板：四条竖色带 + 一条横向扫描的柔光带（"在动"的可见证据）。
void render_color_bars(Canvas& canvas, double time, Color tint) {
  const float w = static_cast<float>(canvas.physical_width());
  const float h = static_cast<float>(canvas.physical_height());
  const Color palette[] = {Color{232, 88, 88, 255}, Color{240, 196, 96, 255},
                           Color{88, 200, 120, 255}, Color{88, 132, 232, 255}};
  const float band = w / 4.0f;
  for (int i = 0; i < 4; ++i) {
    canvas.fill_rect(Rect{band * static_cast<float>(i), 0.0f, band, h},
                     Paint::solid(mix(palette[i], tint, 0.25f)));
  }
  // 柔光带：位置随时间平移（`fmod` 保证周期性——导出逐帧时不会因浮点累积漂移）。
  const float sweep = static_cast<float>(std::fmod(time * 0.35, 1.0)) * (w + 160.0f) - 80.0f;
  canvas.fill_rect(Rect{sweep, 0.0f, 80.0f, h}, Paint::solid(Color{255, 255, 255, 40}));
}

/// 字幕：**透明底** + 中下方文字（带缓动入场）。
///
/// ⚠ 底必须是**透明**的，不能铺满不透明色：字幕片段在 V2 轨道上，下面通常压着
/// V1 的画面——铺满不透明底会把整条下层轨道盖掉（实测：预览里 V1 完全看不见，
/// 看起来像“多层合成没生效”）。字幕的职责只是“加一行字”，不是“换一屏”。
/// 可读性由文字后面的**深色底衬条**保证（那才是该不透明的地方）。
void render_subtitle(Canvas& canvas, double time, Color tint, const std::string& caption,
                     const st::ui::TextPort* text_port) {
  (void)tint;
  const float w = static_cast<float>(canvas.physical_width());
  const float h = static_cast<float>(canvas.physical_height());
  canvas.clear(Color{0, 0, 0, 0});

  // 入场缓动：前 0.6s 从下方 24px 升上来。
  const float enter = ease_in_out(static_cast<float>(std::min(time / 0.6, 1.0)));
  const float size = std::max(16.0f, h * 0.09f);
  const float baseline_y = h * 0.74f + (1.0f - enter) * 24.0f;

  if (text_port == nullptr || caption.empty()) {
    // 无文本端口时画一个**几何占位块**（"这里本该有字幕"是可见的，
    // 而不是静默什么都不画）。宽度按字数估，高度取字号。
    const float width = size * 0.95f * static_cast<float>(std::max<std::size_t>(1, caption.size()));
    canvas.fill_rect(Rect{(w - width) * 0.5f, baseline_y - size, width, size * 1.2f},
                     Paint::solid(Color{255, 255, 255, static_cast<std::uint8_t>(180.0f * enter)}));
    return;
  }
  const auto measured = text_port->measure(caption, size);
  const float x = (w - measured.width) * 0.5f;
  // 文字底衬：深色半透明一条，保证任何底色下都可读（字幕的实用要求）。
  canvas.fill_rect(Rect{x - 16.0f, baseline_y - size * 1.3f, measured.width + 32.0f, size * 1.7f},
                   Paint::solid(Color{0, 0, 0, static_cast<std::uint8_t>(120.0f * enter)}));
  const auto alpha = static_cast<std::uint8_t>(std::clamp(255.0f * enter, 0.0f, 255.0f));
  text_port->draw(canvas, caption, st::math::Point{x, baseline_y - size * 0.2f}, size,
                  Color{255, 255, 255, alpha});
}

/// 图形运动：三个形状按不同 easing 轨迹移动（各自相位不同，画面"活"）。
void render_shapes(Canvas& canvas, double time, Color tint) {
  const float w = static_cast<float>(canvas.physical_width());
  const float h = static_cast<float>(canvas.physical_height());
  canvas.fill_rect(Rect{0.0f, 0.0f, w, h}, Paint::solid(mix(Color{24, 26, 34, 255}, tint, 0.22f)));

  const float t = static_cast<float>(std::fmod(time * 0.5, 1.0));
  const float eased = ease_in_out(t);
  // 圆：水平往复。
  const float radius = h * 0.16f;
  const float cx = radius + eased * (w - radius * 2.0f);
  canvas.fill_circle(st::math::Point{cx, h * 0.42f}, radius,
                     Paint::solid(mix(Color{240, 196, 96, 255}, tint, 0.3f)));
  // 矩形：垂直往复（相位相反）。
  const float rect_w = w * 0.18f;
  const float rect_h = h * 0.14f;
  const float ry = h * 0.12f + (1.0f - eased) * (h - rect_h - h * 0.24f);
  canvas.fill_rect(Rect{w * 0.2f, ry, rect_w, rect_h}, Paint::solid(Color{88, 200, 120, 255}), 8.0f);
  // 描边圆环：从中心向外扩散（半径随时间增长并循环）。
  const float ring_t = static_cast<float>(std::fmod(time * 0.4, 1.0));
  const Path ring = st::raster::make_circle(st::math::Point{w * 0.72f, h * 0.62f},
                                            h * 0.08f + ring_t * h * 0.28f);
  canvas.stroke_path(ring, Paint::solid(Color{255, 255, 255, 90}), 3.0f);
}

/// 测试图：色条 + 网格 + 时码刻度（风格自定，不抄 SMPTE 布局）。
void render_test_chart(Canvas& canvas, double time, Color tint, const st::ui::TextPort* text_port) {
  const float w = static_cast<float>(canvas.physical_width());
  const float h = static_cast<float>(canvas.physical_height());
  canvas.fill_rect(Rect{0.0f, 0.0f, w, h}, Paint::solid(Color{16, 16, 16, 255}));

  // 顶部长条：八级灰阶（亮度均匀递增——可用它核验 gamma/量化）。
  const int steps = 8;
  const float bar_w = w / static_cast<float>(steps);
  for (int i = 0; i < steps; ++i) {
    const auto level = static_cast<std::uint8_t>(255.0f * static_cast<float>(i) /
                                                 static_cast<float>(steps - 1));
    canvas.fill_rect(Rect{bar_w * static_cast<float>(i), 0.0f, bar_w, h * 0.18f},
                     Paint::solid(Color{level, level, level, 255}));
  }
  // 网格（间距按画布宽度的 1/12，便于目测几何是否正确）。
  const float gap = w / 12.0f;
  for (int i = 1; i < 12; ++i) {
    canvas.fill_rect(Rect{gap * static_cast<float>(i), h * 0.18f, 1.0f, h * 0.82f},
                     Paint::solid(Color{255, 255, 255, 28}));
  }
  // 中下：一个随时间移动的刻度块（"在动"的判据）+ 圆点阵列。
  const float knob = static_cast<float>(std::fmod(time * 0.6, 1.0));
  canvas.fill_rect(Rect{knob * (w - 40.0f), h * 0.86f, 40.0f, h * 0.06f},
                   Paint::solid(mix(Color{232, 88, 88, 255}, tint, 0.3f)));
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 5; ++col) {
      const float cx = w * (0.18f + 0.16f * static_cast<float>(col));
      const float cy = h * (0.36f + 0.16f * static_cast<float>(row));
      canvas.fill_circle(st::math::Point{cx, cy}, 4.0f, Paint::solid(Color{200, 200, 200, 200}));
    }
  }
  if (text_port != nullptr) {
    const std::string label = format_timecode(time);
    text_port->draw(canvas, label, st::math::Point{w * 0.04f, h * 0.30f}, h * 0.07f,
                    Color{240, 240, 240, 255});
  }
}

}  // namespace

auto Project::apply_script(Script next) -> bool {
  if (next.scenes.empty()) return false;
  script = std::move(next);
  fps = script.fps;
  duration = std::max(0.2, script.total_duration());
  playhead = std::clamp(playhead, 0.0, duration);
  rebuild_from_script();
  return true;
}

void Project::rebuild_from_script() {
  const int keep_selected = selected;
  clips.clear();
  next_id = 1;
  double cursor = 0.0;
  for (std::size_t i = 0; i < script.scenes.size(); ++i) {
    const Scene& scene = script.scenes[i];
    const double length = std::max(0.2, scene.duration);
    Clip clip{};
    clip.id = next_id++;
    clip.track = 0;            // 场景串行铺在 V1（画中画才用 V2）
    clip.t_in = cursor;
    clip.t_out = cursor + length;
    clip.scene_index = static_cast<int>(i);
    clip.tint = scene.accent;
    // 反向验证用：把下面这行改成 `if (false)` 后重建，`caiyun_director_e2e.py` 的
    // “[2] 脚本即真值”立即变红（已实测：断言报“时间轴片段数应为 3，实为 0”）。
    clips.push_back(clip);
    cursor += length;
  }
  duration = std::max(0.2, cursor);
  // 选中态尽量保留（重建后 id 全变，按"第几段"恢复更符合直觉）。
  if (keep_selected >= 0 && !clips.empty()) {
    const std::size_t index = std::min<std::size_t>(static_cast<std::size_t>(keep_selected - 1),
                                                    clips.size() - 1);
    selected = clips[index].id;
  } else {
    selected = clips.empty() ? -1 : clips.front().id;
  }
}

auto Project::add_clip(GeneratorKind kind, int track, double t_in, double t_out) -> int {
  // **AI 控制态一律拒绝**（用户要求：AI 剪的时候人只能看）。守在这里而不是 UI 层，
  // 是因为编辑入口有六七个，逐个禁用必漏一个；漏掉的那个就是真实缺口。
  if (!can_edit()) return -1;

  if (track < 0 || track > 1) return -1;
  if (t_in < 0.0) t_in = 0.0;
  if (t_out - t_in < kMinClipDuration) return -1;
  if (t_out > duration) t_out = duration;
  Clip clip{};
  clip.id = next_id++;
  clip.track = track;
  clip.t_in = t_in;
  clip.t_out = t_out;
  clip.generator = kind;
  // 每个片段给一个可区分的基色（同一生成器的多个片段不至于完全一样）。
  const std::uint8_t hue_step = static_cast<std::uint8_t>((clip.id * 47) % 200);
  clip.tint = Color{static_cast<std::uint8_t>(60 + hue_step), 140,
                    static_cast<std::uint8_t>(180 - hue_step / 2), 255};
  clips.push_back(clip);
  selected = clip.id;
  return clip.id;
}

auto Project::remove_clip(int id) -> bool {
  // **AI 控制态一律拒绝**（用户要求：AI 剪的时候人只能看）。守在这里而不是 UI 层，
  // 是因为编辑入口有六七个，逐个禁用必漏一个；漏掉的那个就是真实缺口。
  if (!can_edit()) return false;

  for (auto it = clips.begin(); it != clips.end(); ++it) {
    if (it->id != id) continue;
    clips.erase(it);
    if (selected == id) selected = -1;
    return true;
  }
  return false;
}

auto Project::ripple_delete(int id) -> bool {
  // **AI 控制态一律拒绝**（用户要求：AI 剪的时候人只能看）。守在这里而不是 UI 层，
  // 是因为编辑入口有六七个，逐个禁用必漏一个；漏掉的那个就是真实缺口。
  if (!can_edit()) return false;

  const Clip* target = find_clip(id);
  if (target == nullptr) return false;
  const int track = target->track;
  const double gap = target->duration();
  const double removed_at = target->t_in;
  if (!remove_clip(id)) return false;
  // **波纹**：同轨道上、位于被删片段之后的片段整体前移一个"它的时长"。
  // 只动同轨道——跨轨道前移会把另一条轨道的对位打乱（那是另一类操作）。
  for (auto& clip : clips) {
    if (clip.track != track) continue;
    if (clip.t_in <= removed_at) continue;
    clip.t_in -= gap;
    clip.t_out -= gap;
    if (clip.t_in < 0.0) {
      clip.t_out -= clip.t_in;
      clip.t_in = 0.0;
    }
  }
  return true;
}

auto Project::split_at(double time) -> bool {
  // **AI 控制态一律拒绝**（用户要求：AI 剪的时候人只能看）。守在这里而不是 UI 层，
  // 是因为编辑入口有六七个，逐个禁用必漏一个；漏掉的那个就是真实缺口。
  if (!can_edit()) return false;

  // 优先切选中的（用户意图更明确）；没选中时切"最上层命中"的那个。
  int target_id = selected;
  if (const Clip* chosen = find_clip(target_id); chosen == nullptr || !chosen->covers(time)) {
    target_id = -1;
    for (const auto& clip : clips) {
      if (!clip.covers(time)) continue;
      // 上层优先：track 大的先切。
      if (target_id < 0 || clip.track >= find_clip(target_id)->track) target_id = clip.id;
    }
  }
  if (target_id < 0) return false;
  Clip* target = find_clip(target_id);
  if (target == nullptr || !target->covers(time)) return false;
  // 两侧都要满足最小时长，否则切出来一个"看不见的块"。
  if (time - target->t_in < kMinClipDuration) return false;
  if (target->t_out - time < kMinClipDuration) return false;

  Clip right = *target;
  right.id = next_id++;
  right.t_in = time;
  // 右半段的素材入点跟着推进（左边被切掉的部分不再属于它）。
  right.source_in = target->local_time(time);
  target->t_out = time;
  clips.push_back(right);
  return true;
}

auto Project::trim(int id, int edge, double time) -> bool {
  // **AI 控制态一律拒绝**（用户要求：AI 剪的时候人只能看）。守在这里而不是 UI 层，
  // 是因为编辑入口有六七个，逐个禁用必漏一个；漏掉的那个就是真实缺口。
  if (!can_edit()) return false;

  Clip* clip = find_clip(id);
  if (clip == nullptr) return false;
  if (edge == 0) {
    // 左边缘：入点右移时素材入点同步右移（**画面不跳**——这是修剪与"裁掉开头"
    // 的区别；若不同步，修剪会让画面内容突然前跳）。
    const double new_in = std::max(0.0, std::min(time, clip->t_out - kMinClipDuration));
    clip->source_in += (new_in - clip->t_in) * static_cast<double>(clip->speed);
    if (clip->source_in < 0.0) clip->source_in = 0.0;
    clip->t_in = new_in;
  } else {
    clip->t_out = std::max(clip->t_in + kMinClipDuration, std::min(time, duration));
  }
  return true;
}

auto Project::move_clip(int id, double new_t_in) -> bool {
  // **AI 控制态一律拒绝**（用户要求：AI 剪的时候人只能看）。守在这里而不是 UI 层，
  // 是因为编辑入口有六七个，逐个禁用必漏一个；漏掉的那个就是真实缺口。
  if (!can_edit()) return false;

  Clip* clip = find_clip(id);
  if (clip == nullptr) return false;
  const double length = clip->duration();
  double in = std::max(0.0, new_t_in);
  if (in + length > duration) in = std::max(0.0, duration - length);
  clip->t_in = in;
  clip->t_out = in + length;
  return true;
}

auto Project::set_opacity(int id, float value) -> bool {
  if (!can_edit()) return false;
  Clip* clip = find_clip(id);
  if (clip == nullptr) return false;
  clip->opacity = std::clamp(value, 0.0f, 1.0f);
  return true;
}

auto Project::set_speed(int id, float value) -> bool {
  if (!can_edit()) return false;
  Clip* clip = find_clip(id);
  if (clip == nullptr) return false;
  clip->speed = std::clamp(value, 0.25f, 4.0f);
  return true;
}

auto Project::set_scene_text(int scene_index, int field, std::string value) -> bool {
  if (!can_edit()) return false;
  if (scene_index < 0 || static_cast<std::size_t>(scene_index) >= script.scenes.size()) {
    return false;
  }
  Scene& scene = script.scenes[static_cast<std::size_t>(scene_index)];
  if (field == 0) {
    scene.title = std::move(value);
  } else if (field == 1) {
    scene.subtitle = std::move(value);
  } else {
    return false;
  }
  return true;
}

auto Project::set_ai_control(bool on) -> bool {
  if (ai_controlled == on) return false;
  ai_controlled = on;
  // AI 接管时以**脚本**为准重建片段：AI 改的是脚本，而 `clips` 还带着历史人工
  // 编辑；不重建就会出现“脚本早换了、时间轴还是手动剪过的样子”。
  if (on) rebuild_from_script();
  return true;
}

void Project::render_frame(Canvas& target, double time, const st::ui::TextPort* text_port) const {
  target.clear(Color{0, 0, 0, 255});
  // 自底向上：轨道 V1（track 0）在下、V2（track 1）在上。
  for (int track = 0; track <= 1; ++track) {
    for (const auto& clip : clips) {
      if (clip.track != track || !clip.covers(time)) continue;
      // **快路径**：本片段是唯一有内容且不透明的那个 → 直接画进目标画布。
      //
      // 为什么值得单独写：常规路径每帧要“渲染到临时画布 → draw_canvas_at 双线性
      // 重采样贴回”，而 1080p 的一次全屏重采样比渲染本身还贵（实测约 40ms/帧，
      // 是导出慢的主因）。成片里 90% 以上的帧恰好满足这个条件。
      bool sole_opaque = clip.opacity >= 0.999f;
      if (sole_opaque) {
        for (const auto& other : clips) {
          if (other.id == clip.id || !other.covers(time)) continue;
          if (other.opacity > 0.001f) {
            sole_opaque = false;
            break;
          }
        }
      }
      if (sole_opaque) {
        if (const Scene* scene = scene_of(clip); scene != nullptr) {
          render_scene(target, *scene, clip.local_time(time), scene->duration, text_port,
                        &assets_, asset_basis_, clip.scene_index,
                        static_cast<int>(script.scenes.size()));
        } else {
          render_generator(target, clip.generator, clip.local_time(time), clip.tint, script.name,
                           text_port);
        }
        continue;
      }
      // 片段渲染到临时画布再按 opacity 合成（这样透明度与"内容画了什么"解耦）。
      Canvas layer{target.physical_width(), target.physical_height(), 1.0f};
      layer.clear(Color{0, 0, 0, 0});
      if (const Scene* scene = scene_of(clip); scene != nullptr) {
        // 场景：`local_time` 已含素材入点与速度（修剪/调速都自动正确）。
        render_scene(layer, *scene, clip.local_time(time), scene->duration, text_port,
                     &assets_, asset_basis_, clip.scene_index,
                     static_cast<int>(script.scenes.size()));
      } else {
        render_generator(layer, clip.generator, clip.local_time(time), clip.tint,
                         script.name, text_port);
      }
      st::raster::DrawOptions options{};
      options.opacity = clip.opacity;
      target.draw_canvas_at(layer, 0, 0, options);
    }
  }

  // 转场：相邻场景在边界处交叉淡入（**在合成之后**按片段时间做，
  // 与片段怎么排布无关——所以人工移动/修剪后转场仍然跟着边界走）。
  if (script.transition > 0.001) {
    for (const auto& clip : clips) {
      const Scene* scene = scene_of(clip);
      if (scene == nullptr) continue;
      // 入场：跨过 `t_in` 后 transition 秒内，从**上一段场景的末帧**淡入。
      const double into = time - clip.t_in;
      if (into < 0.0 || into >= script.transition) continue;
      const Clip* previous = nullptr;
          for (const auto& other : clips) {
      if (other.id == clip.id || !other.is_scene()) continue;
      if (other.t_out <= clip.t_in + 1e-6 &&
          (previous == nullptr || other.t_out > previous->t_out)) {
        previous = &other;
      }
    }
      if (previous == nullptr) continue;
      const Scene* before = scene_of(*previous);
      if (before == nullptr) continue;
      Canvas bridge{target.physical_width(), target.physical_height(), 1.0f};
      bridge.clear(Color{0, 0, 0, 0});
      render_scene(bridge, *before, previous->local_time(previous->t_out), before->duration,
                   text_port, &assets_, asset_basis_);
      // 转场不只透明度交叉（那看起来"平"）：交出的一幕同时**微缩 + 上移**，
      // 叠在新的一幕上就有"镜头推进"的方向感。
      //
      // 幅度很小（3% 缩放 / 1.5% 位移）：转场是"让观众不迷失"不是炫技；
      // 幅度一大就变成 PPT 切换（廉价感的主要来源）。
      const float progress = static_cast<float>(
          std::clamp(into / std::max(0.001, script.transition), 0.0, 1.0));
      // 缓动：转场用 ease-out（快出慢收）——线性交叉会显得"匀速滑动"，
      // 而真实的镜头切换是"一下到位然后稳住"。
      const float eased = 1.0f - (1.0f - progress) * (1.0f - progress) * (1.0f - progress);
      const float shrink = 1.0f - 0.035f * eased;
      const float shift = static_cast<float>(target.physical_height()) * 0.016f * eased;
      const float box_w = static_cast<float>(target.physical_width()) * shrink;
      const float box_h = static_cast<float>(target.physical_height()) * shrink;
      const st::math::Rect box{(static_cast<float>(target.physical_width()) - box_w) * 0.5f,
                               (static_cast<float>(target.physical_height()) - box_h) * 0.5f - shift,
                               box_w, box_h};
      st::raster::DrawOptions options{};
      options.opacity = 1.0f - eased;
      target.draw_canvas(bridge, box, options);

      // ── 冲击闪白：转场前 30% 时间内叠一层高亮，纯黑场之间"撞击"用 ──
      //
      // 为何要有它：交叉溶解在任何两幕之间都"温和"，而预告片的节奏需要
      // **明确的断点**。闪白提供了这个断点，且成本极低（一次全幅 fill_rect）。
      if (progress < 0.30f) {
        const float flash = 1.0f - progress / 0.30f;
        const float alpha = flash * flash * 0.32f;
        if (alpha > 0.003f) {
          const auto a = static_cast<std::uint8_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
          target.fill_rect(st::math::Rect{0.0f, 0.0f,
                                          static_cast<float>(target.physical_width()),
                                          static_cast<float>(target.physical_height())},
                           st::raster::Paint::solid(st::math::Color{210, 232, 255, a}));
        }
      }

      // ── 光扫擦除：转场期间一道亮带扫过（"被光洗过"的换场感）──
      if (progress > 0.02f && progress < 0.92f) {
        const float sweep = (progress - 0.02f) / 0.90f;
        caiyun::paint_light_sweep(target, sweep, 0.22f, scene->accent, 1.1f,
                                  static_cast<float>(target.device_scale()));
      }

    }
  }

  // 全片淡入/淡出（黑场包边）。
  const auto fade_alpha = [&]() -> double {
    if (script.fade_in > 0.001 && time < script.fade_in) return 1.0 - time / script.fade_in;
    const double from_end = duration - time;
    if (script.fade_out > 0.001 && from_end < script.fade_out) {
      return 1.0 - std::max(0.0, from_end) / script.fade_out;
    }
    return 0.0;
  };
  const double black = std::clamp(fade_alpha(), 0.0, 1.0);
  if (black > 0.002) {
    target.fill_rect(Rect{0.0f, 0.0f, static_cast<float>(target.physical_width()),
                          static_cast<float>(target.physical_height())},
                     Paint::solid(Color{0, 0, 0, static_cast<std::uint8_t>(black * 255.0)}));
  }
}

void render_generator(Canvas& target, GeneratorKind kind, double local_time, Color tint,
                      const std::string& caption, const st::ui::TextPort* text_port) {
  switch (kind) {
    case GeneratorKind::ColorBars: render_color_bars(target, local_time, tint); break;
    case GeneratorKind::Subtitle: render_subtitle(target, local_time, tint, caption, text_port); break;
    case GeneratorKind::Shapes: render_shapes(target, local_time, tint); break;
    case GeneratorKind::TestChart: render_test_chart(target, local_time, tint, text_port); break;
  }
}

auto format_timecode(double seconds) -> std::string {
  // 收口到框架版 `timeline::format_timecode`（拆解方案步骤 1 / BACKLOG:3954）——
  // 时间码是「被标尺/片段标签/状态栏共用」的量，各写一份必然不一致（框架头文件
  // timeline.hpp:181-185 对此的论证）。本函数只剩秒→毫秒的换算：caiyun 的
  // 时间轴以秒为原生单位（Project 字段），而框架模型是毫秒制。
  // 舍入：+0.5 再取整与旧实现逐字符等价（18 覆盖值探针比对，含 61.4995
  // 舍入路径边界与负值 clamp——见 tmp/tc_probe/timecode_probe.cpp）。
  if (seconds < 0.0) seconds = 0.0;
  const auto ms = static_cast<std::int64_t>(seconds * 1000.0 + 0.5);
  return st::ui::timeline::format_timecode(ms);
}

// format_short 保留独立实现（**不**改调框架 compact 形态）——诚实边界：两者
// 形态不同，caiyun 的标尺要「mm:ss」（无毫秒），而 timeline::format_timecode
// 的 compact 分支是「mm:ss.mmm」（省小时但保留毫秒，src/ui/timeline.cpp:244-245）。
// 换过去标尺标签会从 "00:05" 变 "00:05.000"，属于可感知的视觉回归——不收口。
auto format_short(double seconds) -> std::string {
  if (seconds < 0.0) seconds = 0.0;
  const auto total_s = static_cast<std::int64_t>(seconds);
  return std::format("{:02}:{:02}", total_s / 60, total_s % 60);
}

}  // namespace caiyun

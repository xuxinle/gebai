/// `st::ui::timeline` 时间轴数据模型测试。
///
/// 三条最容易错的地方（都在这里被钉住）：
/// ① **时间↔像素换算**：标尺刻度与片段位置必须用同一个换算（各算一份必然差一像素）；
/// ② **吸附不能吸到自己**：不排除被拖的那个片段，它会永远吸在原位——"拖不动"；
/// ③ **命中优先级**：播放头 → 修剪边缘 → 片体；顺序错了就出现"抓不住播放头"
///    或"想移动却变成修剪"。

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "st/math/color.hpp"
#include "st/test/test.hpp"
#include "st/ui/timeline.hpp"

namespace {

using st::ui::timeline::Clip;
using st::ui::timeline::ClipPart;
using st::ui::timeline::format_timecode;
using st::ui::timeline::Millis;
using st::ui::timeline::Model;
using st::ui::timeline::ruler_step;
using st::ui::timeline::SnapOptions;
using st::ui::timeline::Track;

/// 两条轨道：视频（两个片段）+ 音频（一个片段）。
[[nodiscard]] auto make_tracks() -> std::vector<Track> {
  Track video;
  video.id = "v1";
  video.label = "视频";
  video.clips.push_back(Clip{.id = "a", .label = "A", .start = 0, .duration = 2000});
  video.clips.push_back(Clip{.id = "b", .label = "B", .start = 3000, .duration = 1000});
  Track audio;
  audio.id = "a1";
  audio.label = "音频";
  audio.height = 32.0f;
  audio.clips.push_back(Clip{.id = "c", .label = "C", .start = 1000, .duration = 500});
  return {video, audio};
}

[[nodiscard]] auto make_model() -> Model {
  Model model;
  model.set_tracks(make_tracks());
  model.set_pixels_per_second(100.0f);   // 1ms = 0.1px，方便心算
  model.set_viewport_width(600.0f);
  return model;
}

}  // namespace

ST_TEST(timeline_time_pixel_conversion_round_trips) {
  Model model = make_model();
  // 100 px/s：1000ms 应在 x=100。
  ST_CHECK(std::abs(model.time_to_x(1000) - 100.0f) < 0.01f);
  ST_CHECK(std::abs(model.time_to_x(0) - 0.0f) < 0.01f);
  // 往返
  for (const Millis time : {0, 1, 999, 1500, 7000}) {
    ST_CHECK_EQ(model.x_to_time(model.time_to_x(time)), time);
  }
  // 滚动参与换算。
  model.set_scroll(2000);
  ST_CHECK(std::abs(model.time_to_x(2000) - 0.0f) < 0.01f);
  ST_CHECK(std::abs(model.time_to_x(4000) - 200.0f) < 0.01f);
  // 可见区间：600px / 100pps = 6s。
  const auto [from, to] = model.visible_range();
  ST_CHECK_EQ(from, Millis{2000});
  ST_CHECK_EQ(to, Millis{8000});

  // 缩放：改变 pps 后往返仍成立。
  model.set_pixels_per_second(37.5f);
  ST_CHECK_EQ(model.x_to_time(model.time_to_x(1234)), Millis{1234});
  // 非法 pps 被夹取（0/负数会让换算除零或反向）。
  model.set_pixels_per_second(0.0f);
  ST_CHECK(model.pixels_per_second() > 0.0f);
  model.set_pixels_per_second(-5.0f);
  ST_CHECK(model.pixels_per_second() > 0.0f);
  // 滚动不为负。
  model.set_scroll(-100);
  ST_CHECK_EQ(model.scroll(), Millis{0});
}

ST_TEST(timeline_track_geometry_and_clip_ranges) {
  Model model = make_model();
  // 轨道纵向堆叠：v1 高 44，a1 从 44 开始、高 32。
  ST_CHECK(std::abs(model.track_top(0) - 0.0f) < 0.01f);
  ST_CHECK(std::abs(model.track_top(1) - 44.0f) < 0.01f);
  const auto [top, bottom] = model.track_rect(1);
  ST_CHECK(std::abs(top - 44.0f) < 0.01f);
  ST_CHECK(std::abs(bottom - 76.0f) < 0.01f);
  // 由 y 反查轨道。
  ST_CHECK_EQ(model.track_at_y(10.0f), std::size_t{0});
  ST_CHECK_EQ(model.track_at_y(50.0f), std::size_t{1});
  ST_CHECK_EQ(model.track_at_y(100.0f), Model::kNone);
  ST_CHECK_EQ(model.track_at_y(-1.0f), Model::kNone);

  // 片段横向范围（100pps）：A 从 0 到 200px。
  const auto [left, right] = model.clip_x_range(0, 0);
  ST_CHECK(std::abs(left - 0.0f) < 0.01f);
  ST_CHECK(std::abs(right - 200.0f) < 0.01f);
  // 越界下标返回零宽（不崩）。
  const auto [l2, r2] = model.clip_x_range(9, 9);
  ST_CHECK(std::abs(l2) < 0.01f);
  ST_CHECK(std::abs(r2) < 0.01f);

  // 内容总长 = 最晚结束时间（B 的 3000+1000 = 4000）。
  ST_CHECK_EQ(model.content_duration(), Millis{4000});
  // 按 id 定位。
  const auto [track, clip] = model.locate("c");
  ST_CHECK_EQ(track, std::size_t{1});
  ST_CHECK_EQ(clip, std::size_t{0});
  const auto [missing_track, missing_clip] = model.locate("不存在");
  ST_CHECK_EQ(missing_track, Model::kNone);
  ST_CHECK_EQ(missing_clip, Model::kNone);
}

ST_TEST(timeline_hit_test_priority_and_edges) {
  Model model = make_model();
  model.set_playhead(500);   // 500ms 处播放头 → x = 50

  // ① 播放头优先：x=50 落在 A 片段（0..200px）之内，但应当命中播放头。
  const auto on_playhead = model.hit_test(50.0f, 10.0f);
  ST_CHECK(on_playhead.playhead);
  ST_CHECK(!on_playhead.valid() || on_playhead.playhead);

  // ② 修剪边缘：A 的左缘在 x=0，点 x=2 应当命中 LeftEdge。
  const auto left_edge = model.hit_test(2.0f, 10.0f);
  ST_CHECK(!left_edge.playhead);
  ST_CHECK_EQ(left_edge.track, std::size_t{0});
  ST_CHECK_EQ(left_edge.clip, std::size_t{0});
  ST_CHECK(left_edge.part == ClipPart::LeftEdge);
  // 右缘在 x=200。
  const auto right_edge = model.hit_test(199.0f, 10.0f);
  ST_CHECK(right_edge.part == ClipPart::RightEdge);
  // 片体中部。
  const auto body = model.hit_test(120.0f, 10.0f);
  ST_CHECK(body.part == ClipPart::Body);

  // ③ 空白处（A 与 B 之间：2000..3000ms = 200..300px）。
  const auto blank = model.hit_test(250.0f, 10.0f);
  ST_CHECK(!blank.valid());

  // 轨道外的 y 不命中。
  const auto outside = model.hit_test(120.0f, 500.0f);
  ST_CHECK(!outside.valid());

  // 叠放：同一 x 上有两个片段时取**数组靠后**的（视觉上压在上面的）。
  // ⚠ 先把播放头移开：命中优先级里它最高，不移开就永远命中它（测试会假失败）。
  model.set_playhead(-100'000);
  Track overlay;
  overlay.id = "over";
  overlay.clips.push_back(Clip{.id = "under", .start = 0, .duration = 1000});
  overlay.clips.push_back(Clip{.id = "top", .start = 0, .duration = 1000});
  model.set_tracks({overlay});
  const auto stacked = model.hit_test(50.0f, 10.0f);
  ST_CHECK_EQ(stacked.clip, std::size_t{1});

  // 极窄片段：修剪边缘不能吃掉片体（否则"小片段拖不动"）。
  Track narrow;
  narrow.id = "n";
  narrow.clips.push_back(Clip{.id = "tiny", .start = 0, .duration = 20});   // 2px 宽
  model.set_tracks({narrow});
  const auto tiny = model.hit_test(1.0f, 10.0f);
  ST_CHECK(tiny.part != ClipPart::None);
  // 2px 宽的片段上，x=1 是正中——应当仍是 Body（边缘带被限制到片宽 1/3 = 0.67px）。
  ST_CHECK(tiny.part == ClipPart::Body);
}

ST_TEST(timeline_snap_ignores_excluded_clip_and_uses_pixel_threshold) {
  Model model = make_model();
  // 播放头在 2000（A 的右缘；也是候选）。
  model.set_playhead(2000);
  SnapOptions options;

  // 拖 A 时排除了它自己：2000 处还有播放头与 A 的右缘（被排除），
  // 所以仍能吸到 2000——但那来自播放头。
  const Millis near_head = model.snap(1990, options, 0, 0);
  ST_CHECK_EQ(near_head, Millis{2000});

  // **关键**：不排除自己时，把 A 的**右缘**拖到离原位 5ms 处会被吸回原位——
  // 这就是"拖不动"的成因。为了把"零点"这个候选也排除掉（0 是合法吸附点，
  // 会掩盖结论），这里用一个专门的场景：只有一条轨道、一个片段，且把
  // `to_zero` 关掉。
  SnapOptions no_zero;
  no_zero.to_zero = false;
  no_zero.to_playhead = false;
  // 不排除：拖 5ms 会被吸回 0（片段自己的起点）。
  ST_CHECK_EQ(model.snap(5, no_zero, Model::kNone, Model::kNone), Millis{0});
  // 排除自己：不该再被吸回去。
  ST_CHECK(model.snap(5, no_zero, 0, 0) != 0);

  // 吸附到其他片段边缘：B 的起点 3000 → 1595（阈值内 8px = 80ms）应吸到 3000。
  const Millis to_b = model.snap(2950, options, 0, 0);
  ST_CHECK_EQ(to_b, Millis{3000});
  // 超出阈值不吸。
  const Millis far = model.snap(2500, options, 0, 0);
  ST_CHECK_EQ(far, Millis{2500});

  // 阈值是**像素**：放大后 8px 只对应很小的毫秒数，于是"同样的毫秒距离不再吸"。
  // 1000pps 时 8px = 8ms ⇒ 距 2000 有 50ms 的地方不吸；10pps 时 8px = 800ms ⇒ 吸。
  Model zoomed_in = make_model();
  zoomed_in.set_playhead(2000);
  zoomed_in.set_pixels_per_second(1000.0f);
  ST_CHECK_EQ(zoomed_in.snap(1950, options, Model::kNone, Model::kNone), Millis{1950});
  ST_CHECK_EQ(zoomed_in.snap(1995, options, Model::kNone, Model::kNone), Millis{2000});

  Model zoomed_out = make_model();
  zoomed_out.set_playhead(2000);
  zoomed_out.set_pixels_per_second(10.0f);
  ST_CHECK_EQ(zoomed_out.snap(1950, options, Model::kNone, Model::kNone), Millis{2000});

  // 各开关能独立关掉。
  SnapOptions only_zero;
  only_zero.to_clip_edges = false;
  only_zero.to_playhead = false;
  ST_CHECK_EQ(model.snap(3005, only_zero, Model::kNone, Model::kNone), Millis{3005});
  // 标记点也是候选。
  SnapOptions with_marker;
  with_marker.to_clip_edges = false;
  with_marker.to_playhead = false;
  with_marker.markers = {5000};
  ST_CHECK_EQ(model.snap(4950, with_marker, Model::kNone, Model::kNone), Millis{5000});
}

ST_TEST(timeline_selection_helpers) {
  Model model = make_model();
  ST_CHECK(model.selected_clips().empty());
  model.find_clip(0, 1)->selected = true;
  model.find_clip(1, 0)->selected = true;
  const auto selected = model.selected_clips();
  ST_CHECK_EQ(selected.size(), std::size_t{2});
  ST_CHECK_EQ(selected[0].first, std::size_t{0});
  ST_CHECK_EQ(selected[0].second, std::size_t{1});
  model.clear_selection();
  ST_CHECK(model.selected_clips().empty());
}

ST_TEST(timeline_timecode_formatting) {
  ST_CHECK_EQ(format_timecode(0), std::string("00:00:00.000"));
  ST_CHECK_EQ(format_timecode(1234), std::string("00:00:01.234"));
  ST_CHECK_EQ(format_timecode(61'500), std::string("00:01:01.500"));
  ST_CHECK_EQ(format_timecode(3'600'000), std::string("01:00:00.000"));
  // 紧凑形：省略小时。
  ST_CHECK_EQ(format_timecode(61'500, true), std::string("01:01.500"));
  // 负数按 0 显示（时间轴上没有负时间；显示 `-00:00:01` 会让人以为数据坏了）。
  ST_CHECK_EQ(format_timecode(-500), std::string("00:00:00.000"));
}

ST_TEST(timeline_ruler_step_uses_nice_steps) {
  // 100 px/s、最小 72px ⇒ 至少 720ms ⇒ 跳到 1s。
  ST_CHECK_EQ(ruler_step(10'000, 100.0f), Millis{1000});
  // 放大到 1000 px/s：72px = 72ms ⇒ 跳到 100ms。
  ST_CHECK_EQ(ruler_step(2'000, 1000.0f), Millis{100});
  // 缩到 10 px/s：72px = 7200ms ⇒ 跳到 10s。
  ST_CHECK_EQ(ruler_step(120'000, 10.0f), Millis{10'000});
  // 步长永远是 1/2/5 × 10^n（保证刻度标签是整秒/整分，不用心算）。
  for (const float pps : {3.0f, 17.0f, 250.0f, 4096.0f}) {
    const Millis step = ruler_step(60'000, pps);
    const double magnitude = std::pow(10.0, std::floor(std::log10(static_cast<double>(step))));
    const double mantissa = static_cast<double>(step) / magnitude;
    const bool nice = std::abs(mantissa - 1.0) < 1.0e-6 || std::abs(mantissa - 2.0) < 1.0e-6 ||
                      std::abs(mantissa - 5.0) < 1.0e-6 ||
                      std::abs(mantissa - 10.0) < 1.0e-6;
    ST_CHECK(nice);
    // 且满足最小像素间距。
    ST_CHECK(static_cast<float>(step) / 1000.0f * pps >= 72.0f - 0.01f);
  }
  // 非法输入不崩。
  ST_CHECK(ruler_step(0, 100.0f) > 0);
  ST_CHECK(ruler_step(1000, 0.0f) > 0);
}

ST_TEST(timeline_clip_tint_is_optional_data_only) {
  // 片段着色（caiyun 拆解步骤 5，S5/G7）：tint 是**数据接口**不是绘制效果——
  // Model 层只存不解释；本用例钉住「默认 nullopt、显式赋值/复位往返、
  // 不参与任何几何/命中/吸附计算」三件事，绘制侧的消费在 view 测试。
  std::vector<Track> tracks = make_tracks();
  ST_CHECK(!tracks[0].clips[0].tint.has_value());   // 默认 nullopt（零破坏：不填它
  ST_CHECK(!tracks[1].clips[0].tint.has_value());   //  的存量代码与旧版数据形态一致）

  tracks[0].clips[0].tint = st::math::Color::rgb(200, 60, 30);
  Model model;
  model.set_tracks(std::move(tracks));
  const Clip* clip = model.find_clip(0, 0);
  ST_REQUIRE(clip != nullptr);
  ST_REQUIRE(clip->tint.has_value());
  ST_CHECK_EQ(clip->tint->r, 200);
  ST_CHECK_EQ(clip->tint->g, 60);
  ST_CHECK_EQ(clip->tint->b, 30);
  ST_CHECK_EQ(clip->tint->a, 255);

  // 可变访问同往返（宿主经 tracks() 改色）。
  Model mutable_model = make_model();
  mutable_model.tracks()[1].clips[0].tint = st::math::Color::rgb(1, 2, 3);
  const Clip* audio = mutable_model.find_clip(1, 0);
  ST_REQUIRE(audio != nullptr);
  ST_REQUIRE(audio->tint.has_value());
  ST_CHECK_EQ(audio->tint->g, 2);
  mutable_model.tracks()[1].clips[0].tint.reset();
  ST_CHECK(!mutable_model.find_clip(1, 0)->tint.has_value());

  // **默认零变化**：tint 不改任何几何/命中/吸附结果——nullopt 片段与有值片段
  // 走完全同一套换算（着色是纯数据面，本层不该因它长出任何分支）。
  Model plain = make_model();
  Model tinted = make_model();
  tinted.tracks()[0].clips[0].tint = st::math::Color::rgb(9, 9, 9);
  for (const Millis time : {0, 500, 1500, 3000}) {
    ST_CHECK(plain.time_to_x(time) == tinted.time_to_x(time));
  }
  const auto plain_left = plain.clip_x_range(0, 0);
  const auto tinted_left = tinted.clip_x_range(0, 0);
  ST_CHECK(plain_left == tinted_left);
  ST_CHECK(plain.hit_test(100.0f, 10.0f).clip == tinted.hit_test(100.0f, 10.0f).clip);
  ST_CHECK_EQ(plain.content_duration(), tinted.content_duration());
}

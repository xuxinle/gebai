/// `st::ui::TimelineView` 时间轴组件测试。
///
/// 数据层（换算/命中/吸附）已在 `ui_timeline_model_test` 里被 79 条断言钉住，
/// 所以本文件只测**组件那一层**：绘制真的发生、鼠标事件真的翻译成了模型操作、
/// 以及三条最容易错的交互——
/// ① 拖拽不吸到自己（否则"拖不动"）；
/// ② 修剪**动 source_in**（不动的话"修剪"会退化成"位移"）；
/// ③ 锁定轨道不接受编辑。

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "st/app/text_port.hpp"
#include "st/raster/canvas.hpp"
#include "st/test/test.hpp"
#include "st/text/text.hpp"
#include "st/ui/components/timeline_view.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::math::Color;
using st::math::Point;
using st::ui::Event;
using st::ui::EventKind;
using st::ui::RenderContext;
using st::ui::Theme;
using st::ui::TimelineView;
using st::ui::timeline::Clip;
using st::ui::timeline::Millis;
using st::ui::timeline::Track;

/// 刻意不在原点：坐标 bug 在原点会被 0 掩盖（这类组件最常见的假绿来源）。
constexpr float kOriginX = 53.0f;
constexpr float kOriginY = 37.0f;
constexpr float kWidth = 640.0f;
constexpr float kHeight = 180.0f;

struct Fixture {
  Theme theme{Theme::light()};
  std::shared_ptr<st::text::FontStack> stack{};
  std::unique_ptr<st::text::TextRenderer> renderer{};
  std::unique_ptr<st::ui::TextPort> port{};
  TimelineView view{};
  RenderContext context{theme, nullptr, 0.0};

  Fixture() {
    auto loaded = st::text::FontStack::system_default();
    if (loaded.has_value()) {
      stack = std::make_shared<st::text::FontStack>(std::move(*loaded));
      renderer = std::make_unique<st::text::TextRenderer>(*stack);
      port = st::app::make_text_port(*renderer);
      context.text = port.get();
    }
    view.set_id("timeline");
    view.apply_theme(theme);
    // 100 px/s：1000ms == 100px，便于手算期望值。
    view.model().set_pixels_per_second(100.0f);
    layout();
  }

  void set_tracks(std::vector<Track> tracks) {
    view.model().set_tracks(std::move(tracks));
    layout();
  }

  void layout() {
    view.measure(context, st::ui::Constraints{kWidth, kHeight});
    view.arrange(context, st::math::Rect{kOriginX, kOriginY, kWidth, kHeight});
  }

  /// 两条轨道：视频（0..2000ms 与 3000..4000ms）+ 音频（1000..1500ms）。
  void load_default() {
    Track video;
    video.id = "v1";
    video.label = "视频";
    video.clips.push_back(Clip{.id = "a", .label = "A", .start = 0, .duration = 2000});
    video.clips.push_back(Clip{.id = "b", .label = "B", .start = 3000, .duration = 1000});
    Track audio;
    audio.id = "a1";
    audio.label = "音频";
    audio.height = 36.0f;
    audio.clips.push_back(Clip{.id = "c", .label = "C", .start = 1000, .duration = 500});
    set_tracks({video, audio});
  }

  [[nodiscard]] auto render() const {
    st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(800, 400, 1.0f);
    canvas.clear(Color::rgb(1, 2, 3));
    view.paint(context, canvas);
    return canvas;
  }

  /// 某片段的**屏幕**矩形（组件几何，供命中测试用）。
  [[nodiscard]] auto clip_screen_rect(std::size_t track, std::size_t clip) const {
    // 与实现同一套换算：lane 的 x 加上模型给的相对位置。
    const float lane_x = kOriginX + TimelineView::kTrackHeaderWidth;
    const auto [left, right] = view.model().clip_x_range(track, clip);
    const auto [band_top, band_bottom] = view.model().track_rect(track);
    const float lane_y = kOriginY + TimelineView::kRulerHeight;
    return st::math::Rect{lane_x + left, lane_y + band_top + 2.0f, right - left,
                          std::max(0.0f, band_bottom - band_top - 4.0f)};
  }
};

[[nodiscard]] auto near(float a, float b, float tolerance = 1.5f) -> bool {
  return std::fabs(a - b) <= tolerance;
}

/// 造一个按下→移动→抬起的鼠标序列（只走组件，不依赖 UiRoot）。
[[nodiscard]] auto drag(TimelineView& view, const RenderContext& context, Point from, Point to,
                        int button = 1) -> bool {
  Event down;
  down.kind = EventKind::MouseDown;
  down.button = button;
  down.position = from;
  const bool grabbed = view.on_event(context, down);

  Event move;
  move.kind = EventKind::MouseMove;
  move.position = to;
  (void)view.on_event(context, move);

  Event up;
  up.kind = EventKind::MouseUp;
  up.position = to;
  (void)view.on_event(context, up);
  return grabbed;
}

}  // namespace

ST_TEST(timeline_view_paints_tracks_clips_and_playhead) {
  Fixture fx;
  fx.load_default();
  fx.view.model().set_playhead(1500);   // 1500ms ⇒ lane 内 150px
  fx.layout();
  const auto canvas = fx.render();

  // ① 片段块真的画了：块内与该轨道空白处的颜色不同。
  const auto block = fx.clip_screen_rect(0, 0);
  const Color in_clip = canvas.pixel_at(static_cast<int>(block.x + block.width * 0.5f),
                                        static_cast<int>(block.y + block.height * 0.5f));
  const Color in_gap = canvas.pixel_at(
      static_cast<int>(fx.clip_screen_rect(0, 1).x - 40.0f),
      static_cast<int>(block.y + block.height * 0.5f));
  ST_CHECK(in_clip != in_gap);

  // ② 播放头真的画了：它所在竖线处与左右 20px 处不同。
  const float head_x = kOriginX + TimelineView::kTrackHeaderWidth + 150.0f;
  const Color on_head = canvas.pixel_at(static_cast<int>(head_x),
                                        static_cast<int>(kOriginY + 100.0f));
  const Color next_to = canvas.pixel_at(static_cast<int>(head_x + 20.0f),
                                        static_cast<int>(kOriginY + 100.0f));
  ST_CHECK(on_head != next_to);

  // ③ 标尺真的画了：标尺带内（y 在 ruler 高度内）与轨道区底色不同。
  const Color ruler = canvas.pixel_at(static_cast<int>(kOriginX + 200.0f),
                                      static_cast<int>(kOriginY + 4.0f));
  const Color lane = canvas.pixel_at(static_cast<int>(kOriginX + 200.0f),
                                     static_cast<int>(kOriginY + TimelineView::kRulerHeight + 60.0f));
  ST_CHECK(ruler != lane);

  // ④ 轨道名前缀栏与轨道区不同（左栏是"名称区"，有一道分隔）。
  const Color header = canvas.pixel_at(static_cast<int>(kOriginX + 10.0f),
                                       static_cast<int>(kOriginY + TimelineView::kRulerHeight + 10.0f));
  ST_CHECK(header != lane);
}

ST_TEST(timeline_view_clicking_ruler_moves_playhead) {
  Fixture fx;
  fx.load_default();
  fx.view.model().set_playhead(0);
  fx.layout();
  int changes = 0;
  fx.view.on_playhead_changed = [&changes](Millis) { ++changes; };

  // 点标尺上 250px 处 ⇒ 2500ms（100px/s）。
  Event down;
  down.kind = EventKind::MouseDown;
  down.button = 1;
  down.position = Point{kOriginX + TimelineView::kTrackHeaderWidth + 250.0f, kOriginY + 6.0f};
  ST_CHECK(fx.view.on_event(fx.context, down));
  ST_CHECK(near(static_cast<float>(fx.view.model().playhead()), 2500.0f));
  ST_CHECK_EQ(changes, 1);

  // 拖到 100px 处 ⇒ 1000ms（拖动中持续更新）。
  Event move;
  move.kind = EventKind::MouseMove;
  move.position = Point{kOriginX + TimelineView::kTrackHeaderWidth + 100.0f, kOriginY + 6.0f};
  ST_CHECK(fx.view.on_event(fx.context, move));
  ST_CHECK(near(static_cast<float>(fx.view.model().playhead()), 1000.0f));
  ST_CHECK_EQ(changes, 2);

  // 抬起后移动不再改播放头。
  Event up;
  up.kind = EventKind::MouseUp;
  up.position = move.position;
  ST_CHECK(fx.view.on_event(fx.context, up));
  Event move2;
  move2.kind = EventKind::MouseMove;
  move2.position = Point{kOriginX + TimelineView::kTrackHeaderWidth + 300.0f, kOriginY + 6.0f};
  ST_CHECK(!fx.view.on_event(fx.context, move2));
  ST_CHECK(near(static_cast<float>(fx.view.model().playhead()), 1000.0f));
}

ST_TEST(timeline_view_dragging_clip_body_moves_without_resizing) {
  Fixture fx;
  fx.load_default();
  const auto block = fx.clip_screen_rect(0, 0);
  const Point from{block.x + block.width * 0.5f, block.y + block.height * 0.5f};
  const Point to{from.x + 40.0f, from.y};   // 右移 40px = +400ms

  const Millis duration_before = fx.view.model().find_clip(0, 0)->duration;
  const Millis source_before = fx.view.model().find_clip(0, 0)->source_in;
  int moved = 0;
  fx.view.on_clip_moved = [&moved](std::size_t, std::size_t) { ++moved; };

  // 关掉吸附：先验证"纯跟手"的位移（吸附单独用例测）。
  fx.view.set_snap_enabled(false);
  ST_CHECK(drag(fx.view, fx.context, from, to));
  ST_CHECK(near(static_cast<float>(fx.view.model().find_clip(0, 0)->start), 400.0f));
  // **移动不改时长**（改了就成缩放了——编辑器里最容易"顺手"搞错的一处）。
  ST_CHECK_EQ(fx.view.model().find_clip(0, 0)->duration, duration_before);
  // 移动也不改源入点（内容没被修剪）。
  ST_CHECK_EQ(fx.view.model().find_clip(0, 0)->source_in, source_before);
  ST_CHECK_EQ(moved, 1);
  // 拖动即选中。
  ST_CHECK(fx.view.model().find_clip(0, 0)->selected);
}

ST_TEST(timeline_view_trimming_right_edge_changes_duration_only) {
  Fixture fx;
  fx.load_default();
  fx.view.set_snap_enabled(false);
  const auto block = fx.clip_screen_rect(0, 0);
  // 抓**右边缘**（在边缘带内）。
  const Point from{block.right() - 2.0f, block.y + block.height * 0.5f};
  const Point to{from.x - 50.0f, from.y};   // 左移 50px = -500ms
  const Millis start_before = fx.view.model().find_clip(0, 0)->start;
  const Millis source_before = fx.view.model().find_clip(0, 0)->source_in;
  int trimmed = 0;
  fx.view.on_clip_trimmed = [&trimmed](std::size_t, std::size_t) { ++trimmed; };

  ST_CHECK(drag(fx.view, fx.context, from, to));
  ST_CHECK_EQ(fx.view.model().find_clip(0, 0)->duration, Millis{1500});   // 2000 - 500
  // 修剪出点：`start` 与 `source_in` 都**不动**。
  ST_CHECK_EQ(fx.view.model().find_clip(0, 0)->start, start_before);
  ST_CHECK_EQ(fx.view.model().find_clip(0, 0)->source_in, source_before);
  ST_CHECK_EQ(trimmed, 1);
}

ST_TEST(timeline_view_trimming_left_edge_moves_source_in) {
  // **本文件最重要的一条**：修剪左边缘必须同步 `source_in`。
  //
  // 不动它的话，"修剪"退化成"把内容往后挪"——画面里只是片段变短了，
  // 而素材内容整体错位（在那个尺度上肉眼看不出来，导出后才炸）。
  Fixture fx;
  fx.load_default();
  fx.view.set_snap_enabled(false);
  // 先给片段一个非零源入点（真实素材多半不是从 0 开始）。
  fx.view.model().find_clip(0, 0)->source_in = 1000;
  const auto block = fx.clip_screen_rect(0, 0);
  const Point from{block.x + 2.0f, block.y + block.height * 0.5f};   // 抓左边缘
  const Point to{from.x + 30.0f, from.y};                            // 右移 30px = +300ms

  ST_CHECK(drag(fx.view, fx.context, from, to));
  const Clip* clip = fx.view.model().find_clip(0, 0);
  ST_REQUIRE(clip != nullptr);
  ST_CHECK_EQ(clip->start, Millis{300});         // 起点右移
  ST_CHECK_EQ(clip->duration, Millis{1700});     // 时长相应变短（右边缘不动）
  ST_CHECK_EQ(clip->source_in, Millis{1300});    // 源入点跟着 +300（这是"修剪"的定义）
}

ST_TEST(timeline_view_locked_track_resists_editing) {
  Fixture fx;
  fx.load_default();
  fx.view.model().tracks()[0].locked = true;   // 锁定视频轨
  fx.layout();
  const auto block = fx.clip_screen_rect(0, 0);
  const Point from{block.x + block.width * 0.5f, block.y + block.height * 0.5f};
  const Millis start_before = fx.view.model().find_clip(0, 0)->start;

  // 拖动被拒（但**点击仍能选中**——锁定的语义是"别动它"，不是"看不见它"）。
  fx.view.set_snap_enabled(false);
  ST_CHECK(drag(fx.view, fx.context, from, Point{from.x + 60.0f, from.y}));
  ST_CHECK_EQ(fx.view.model().find_clip(0, 0)->start, start_before);
  ST_CHECK(fx.view.model().find_clip(0, 0)->selected);

  // 未锁定的轨道照常可编辑（确认"拒绝"不是全局的）。
  const auto audio_block = fx.clip_screen_rect(1, 0);
  const Point audio_from{audio_block.x + audio_block.width * 0.5f,
                         audio_block.y + audio_block.height * 0.5f};
  ST_CHECK(drag(fx.view, fx.context, audio_from, Point{audio_from.x + 20.0f, audio_from.y}));
  ST_CHECK(fx.view.model().find_clip(1, 0)->start > 1000);
}

ST_TEST(timeline_view_snap_pulls_to_neighbouring_clip) {
  Fixture fx;
  fx.load_default();
  fx.view.set_snap_enabled(true);
  fx.view.set_snap_options(st::ui::timeline::SnapOptions{.threshold_px = 8.0f});
  const auto block = fx.clip_screen_rect(0, 0);
  const Point from{block.x + block.width * 0.5f, block.y + block.height * 0.5f};
  // 把 A（0..2000）往右拖到"右缘接近 3000"的位置：右缘在 2000，需要 +1000ms。
  // 100px/s ⇒ 40px 位移 = 400ms ⇒ 右缘 2400（离 3000 太远，不该吸）。
  // 所以这里给一个"右缘距 3000 只差 30ms"的位移：2970-2000 = 970ms ⇒ 97px。
  const Point to{from.x + 97.0f, from.y};
  ST_CHECK(drag(fx.view, fx.context, from, to));
  // 吸附生效：A 的右缘落在 B 的起点 3000 上 ⇒ start = 3000 - 2000 = 1000。
  ST_CHECK_EQ(fx.view.model().find_clip(0, 0)->start, Millis{1000});

  // 关掉吸附后同样的拖动不再被拉走。
  Fixture plain;
  plain.load_default();
  plain.view.set_snap_enabled(false);
  const auto plain_block = plain.clip_screen_rect(0, 0);
  const Point plain_from{plain_block.x + plain_block.width * 0.5f,
                         plain_block.y + plain_block.height * 0.5f};
  ST_CHECK(drag(plain.view, plain.context, plain_from,
                Point{plain_from.x + 97.0f, plain_from.y}));
  ST_CHECK(plain.view.model().find_clip(0, 0)->start != 1000);
}

ST_TEST(timeline_view_zoom_and_scroll_actions) {
  Fixture fx;
  fx.load_default();
  const float pps_before = fx.view.model().pixels_per_second();
  ST_CHECK(fx.view.invoke_action("zoom_in", {}));
  ST_CHECK(fx.view.model().pixels_per_second() > pps_before);
  ST_CHECK(fx.view.invoke_action("zoom_out", {}));
  ST_CHECK(near(fx.view.model().pixels_per_second(), pps_before, 0.01f));

  // Ctrl+滚轮以**指针为锚**缩放：指针下的那一刻时间不变。
  fx.view.model().set_scroll(0);
  fx.layout();
  const float pointer_x = kOriginX + TimelineView::kTrackHeaderWidth + 200.0f;
  const Millis anchor_before =
      fx.view.model().x_to_time(200.0f);   // lane 内 200px 处
  Event wheel;
  wheel.kind = EventKind::Wheel;
  wheel.ctrl = true;
  wheel.wheel_delta = 1.0f;
  wheel.position = Point{pointer_x, kOriginY + 100.0f};
  ST_CHECK(fx.view.on_event(fx.context, wheel));
  const Millis anchor_after = fx.view.model().x_to_time(200.0f);
  ST_CHECK(std::llabs(anchor_after - anchor_before) <= 20);   // 锚点基本不动（整数舍入）

  // 普通滚轮 = 横向滚动。
  fx.view.model().set_scroll(1000);
  wheel.ctrl = false;
  wheel.wheel_delta = 1.0f;
  ST_CHECK(fx.view.on_event(fx.context, wheel));
  ST_CHECK(fx.view.model().scroll() < 1000);

  // 属性面与动作面（控制通道/脚本路径）。
  ST_CHECK(fx.view.set_property("playhead", "1234"));
  ST_CHECK_EQ(fx.view.model().playhead(), Millis{1234});
  ST_CHECK(fx.view.invoke_action("play", {}));
  ST_CHECK_EQ(fx.view.model().playhead(), Millis{0});
  ST_CHECK(!fx.view.set_property("snap", "maybe"));   // 非法值如实拒绝
  ST_CHECK(fx.view.set_property("snap", "false"));
  ST_CHECK(!fx.view.snap_enabled());
  const auto duration = fx.view.get_property("duration");
  ST_REQUIRE(duration.has_value());
  ST_CHECK_EQ(*duration, std::string("4000"));
}

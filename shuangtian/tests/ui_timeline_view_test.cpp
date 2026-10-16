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

ST_TEST(timeline_view_set_playhead_external_updates_silently) {
  // **S2 播放头静默写入口**（caiyun 换件方案步骤 2）：宿主主循环每帧推进播放头
  // 时，必须能「写进去且被重画」但**不回调**——否则形成「写→on_playhead_changed→
  // 宿主 refresh/再写」的推拉回环。三条断言分别钉住①值更新②回调不触发③重绘生效；
  // 末尾用 set_property 对照组证明判据真的能抓到「回调被触发了」。
  Fixture fx;
  fx.load_default();
  int external_changes = 0;
  int silent_changes = 0;
  fx.view.on_playhead_changed = [&silent_changes](Millis) { ++silent_changes; };

  // 先走到稳态：画一帧 + 清脏（dirty 标记断言的前提——Element 初始即脏）。
  const auto settle = [&fx]() {
    (void)fx.render();
    fx.view.clear_dirty();
  };
  settle();
  ST_CHECK(!fx.view.dirty());

  fx.view.set_playhead_external(2500);   // 2500ms ⇒ lane 内 250px

  // ① Model 值更新：属性面与模型面读到同一新值。
  ST_CHECK_EQ(fx.view.model().playhead(), Millis{2500});
  ST_CHECK_EQ(*fx.view.get_property("playhead"), std::string("2500"));

  // ② 回调不被触发（静默语义的全部内容）。
  ST_CHECK_EQ(silent_changes, 0);
  ST_CHECK_EQ(external_changes, 0);

  // ③ mark_dirty 生效：脏标记被置上（下一帧会重画）。
  ST_CHECK(fx.view.dirty());
  // 佐证（像素）：新位置的竖线真的画出来了——在旧位置采样不到它。
  {
    const auto canvas = fx.render();
    const float new_x = kOriginX + TimelineView::kTrackHeaderWidth + 250.0f;
    const int probe_y = static_cast<int>(kOriginY + TimelineView::kRulerHeight + 8.0f);
    const Color on_head = canvas.pixel_at(static_cast<int>(new_x), probe_y);
    const Color next_to = canvas.pixel_at(static_cast<int>(new_x + 30.0f), probe_y);
    ST_CHECK(on_head != next_to);
  }
  // 连写两次仍零回调（每帧推进的形态）。
  fx.view.set_playhead_external(2600);
  fx.view.set_playhead_external(2700);
  ST_CHECK_EQ(fx.view.model().playhead(), Millis{2700});
  ST_CHECK_EQ(silent_changes, 0);
  ST_CHECK_EQ(external_changes, 0);

  // —— 对照组：set_property("playhead") 是「对外通知」口径（控制协议/脚本），
  // 必须发回调——若静默入口的判据（计数器）抓不到回调，这条会先红。
  fx.view.set_property("playhead", "1234");
  ST_CHECK_EQ(fx.view.model().playhead(), Millis{1234});
  ST_CHECK_EQ(silent_changes, 1);

  // 对照组反向：静默写不干扰后续交互路径的正常回调（点标尺仍应通知）。
  Event down;
  down.kind = EventKind::MouseDown;
  down.button = 1;
  down.position = Point{kOriginX + TimelineView::kTrackHeaderWidth + 100.0f, kOriginY + 6.0f};
  (void)fx.view.on_event(fx.context, down);
  ST_CHECK_EQ(silent_changes, 2);
}

ST_TEST(timeline_view_set_playhead_external_marks_dirty_not_layout) {
  // S2 的重绘分寸：播放头只是绘制内容变化，不该牵动 measure/arrange（整帧重排）。
  // 语义基准：set_property("playhead")（同款写）在实现里也只 mark_dirty。
  Fixture fx;
  fx.load_default();
  (void)fx.render();
  fx.view.clear_dirty();
  ST_CHECK(!fx.view.dirty());
  ST_CHECK(!fx.view.layout_dirty());

  fx.view.set_playhead_external(1000);
  ST_CHECK(fx.view.dirty());
  ST_CHECK(!fx.view.layout_dirty());   // 播放头移动不需要重排
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

ST_TEST(timeline_view_hover_brightens_clip_and_reads_back) {
  Fixture fx;
  fx.load_default();
  const auto block = fx.clip_screen_rect(0, 0);
  const Point over{block.x + block.width * 0.5f, block.y + block.height * 0.5f};

  // 初始无悬停（读回口是属性面的可验证面，不止像素）。
  ST_CHECK_EQ(*fx.view.get_property("hover_clip"), std::string("-1:-1"));

  // 悬停前后的像素不同：hover 提亮真的画出来了。
  const Color before = fx.render().pixel_at(static_cast<int>(over.x), static_cast<int>(over.y));
  Event enter;
  enter.kind = EventKind::MouseMove;
  enter.position = over;
  (void)fx.view.on_event(fx.context, enter);
  ST_CHECK_EQ(*fx.view.get_property("hover_clip"), std::string("0:0"));
  const Color after = fx.render().pixel_at(static_cast<int>(over.x), static_cast<int>(over.y));
  ST_CHECK(before != after);

  // 移出片段（同轨道空白处）→ 悬停清除、像素回到常态。
  Event leave;
  leave.kind = EventKind::MouseMove;
  leave.position = Point{fx.clip_screen_rect(0, 1).x - 40.0f, over.y};
  (void)fx.view.on_event(fx.context, leave);
  ST_CHECK_EQ(*fx.view.get_property("hover_clip"), std::string("-1:-1"));
  ST_CHECK(fx.render().pixel_at(static_cast<int>(over.x), static_cast<int>(over.y)) == before);

  // HoverOut（指针离开组件）也是同一条清除路径。
  Event enter2;
  enter2.kind = EventKind::MouseMove;
  enter2.position = over;
  (void)fx.view.on_event(fx.context, enter2);
  ST_CHECK_EQ(*fx.view.get_property("hover_clip"), std::string("0:0"));
  Event out;
  out.kind = EventKind::HoverOut;
  (void)fx.view.on_event(fx.context, out);
  ST_CHECK_EQ(*fx.view.get_property("hover_clip"), std::string("-1:-1"));
}

ST_TEST(timeline_view_hover_survives_drag_and_clears_after) {
  Fixture fx;
  fx.load_default();
  const auto block = fx.clip_screen_rect(0, 0);
  const Point from{block.x + block.width * 0.5f, block.y + block.height * 0.5f};

  // 先悬停上（MouseMove 无拖拽分支才更新 hover_*），再从片段上按下拖走：
  // 拖拽中的 move 走拖拽分支、不碰 hover——指针滑出片段后状态仍保留
  //（拖拽中清会闪烁）。抬起后离开组件才清。
  Event enter;
  enter.kind = EventKind::MouseMove;
  enter.position = from;
  (void)fx.view.on_event(fx.context, enter);
  ST_CHECK_EQ(*fx.view.get_property("hover_clip"), std::string("0:0"));

  ST_CHECK(drag(fx.view, fx.context, from, Point{from.x + 100.0f, from.y}));
  ST_CHECK_EQ(*fx.view.get_property("hover_clip"), std::string("0:0"));

  Event out;
  out.kind = EventKind::HoverOut;
  (void)fx.view.on_event(fx.context, out);
  ST_CHECK_EQ(*fx.view.get_property("hover_clip"), std::string("-1:-1"));
}

ST_TEST(timeline_view_trim_handles_visible_only_when_enabled_and_selected) {
  Fixture fx;
  fx.load_default();
  // ⚠ 播放头必须移开：默认 0ms 的播放头竖线（x∈[lane.x, lane.x+2)）压在块 a
  // 的左缘上且画在最上层——手柄采样点（块左缘+1）会被它统一覆盖，
  // 开/关手柄都采到播放头色，断言必红（控制面实跑的教训，几何非色彩问题）。
  fx.view.model().set_playhead(1500);
  fx.layout();
  const auto block = fx.clip_screen_rect(0, 0);
  const Point center{block.x + block.width * 0.5f, block.y + block.height * 0.5f};
  const auto handle_pixel = [&fx, &block]() {
    return fx.render().pixel_at(static_cast<int>(block.x + 1.0f),
                                static_cast<int>(block.y + block.height * 0.5f));
  };

  // 默认关：点选片段后，左缘手柄位置仍是片段本体色（不是白色手柄）。
  Event down;
  down.kind = EventKind::MouseDown;
  down.button = 1;
  down.position = center;
  (void)fx.view.on_event(fx.context, down);
  ST_CHECK(fx.view.model().find_clip(0, 0)->selected);
  ST_CHECK(!fx.view.show_trim_handles());
  const Color body = handle_pixel();

  // 开关打开：同一位置像素变化（手柄画出来了；半透明白叠在块色上，
  // 不做精确色断言——只断言"变了"与后文"关掉变回来"，往返即证因果）。
  fx.view.set_show_trim_handles(true);
  ST_CHECK(fx.view.show_trim_handles());
  const Color handle = handle_pixel();
  ST_CHECK(handle != body);

  // 属性面同口径：set/get 与显式 setter 等价，乱值如实拒绝。
  ST_CHECK(fx.view.set_property("trim_handles", "false"));
  ST_CHECK(!fx.view.show_trim_handles());
  ST_CHECK_EQ(*fx.view.get_property("trim_handles"), std::string("false"));
  ST_CHECK(!fx.view.set_property("trim_handles", "maybe"));
  ST_CHECK(handle_pixel() == body);

  // 未选中片段不受开关影响：开关开、但没选中时手柄不画（左缘仍是块本体色）。
  // 基线取「未选中+开关关」的常态色，再开开关验证不变。
  fx.view.model().clear_selection();
  ST_CHECK(fx.view.set_property("trim_handles", "false"));
  const Color unselected = handle_pixel();
  ST_CHECK(fx.view.set_property("trim_handles", "true"));
  ST_CHECK(handle_pixel() == unselected);
}

ST_TEST(timeline_view_track_headers_toggle_collapses_prefix_column) {
  // 头部栏开关（caiyun 拆解步骤 5，S4/G11）：关掉后 lane 从组件左缘起，
  // 几何/命中坐标整体平移——方案验收：lane_rect().x == bounds().x（对外不可见，
  // 用「点同一 x 命中同一时刻」的等价判据）+ 开/关两态像素采样。
  Fixture fx;
  fx.load_default();
  ST_CHECK(fx.view.show_track_headers());   // 默认开（存量行为不变）

  // 播放头移到 A 中部：它在头部栏区域（默认在 lane 左缘 x=0 处）不可见，
  // 关掉头部栏后应出现——这是「lane 左缘左移了」的可见证据。
  fx.view.model().set_playhead(500);
  fx.layout();
  const float with_header_head_x = kOriginX + TimelineView::kTrackHeaderWidth + 50.0f;
  ST_CHECK(fx.render().pixel_at(static_cast<int>(with_header_head_x),
                                static_cast<int>(kOriginY + 100.0f)) !=
           fx.render().pixel_at(static_cast<int>(with_header_head_x + 20.0f),
                                static_cast<int>(kOriginY + 100.0f)));

  // 关掉：lane 左缘 = 组件左缘。播放头同一时刻右移了 kTrackHeaderWidth。
  fx.view.set_show_track_headers(false);
  ST_CHECK(!fx.view.show_track_headers());
  fx.layout();
  const float no_header_head_x = kOriginX + 50.0f;
  const auto canvas = fx.render();
  ST_CHECK(canvas.pixel_at(static_cast<int>(no_header_head_x),
                           static_cast<int>(kOriginY + 100.0f)) !=
           canvas.pixel_at(static_cast<int>(no_header_head_x + 20.0f),
                           static_cast<int>(kOriginY + 100.0f)));

  // 开/关两态像素对比：原头部栏区域（组件左缘内 10px）从名称栏色变为
  // 轨道内容色（这里应恰是 A 片段本体——A 从 0ms 起铺满 lane）。
  const Point header_zone{kOriginX + 10.0f, kOriginY + TimelineView::kRulerHeight + 10.0f};
  const Color clip_body = canvas.pixel_at(
      static_cast<int>(kOriginX + TimelineView::kTrackHeaderWidth + 10.0f),
      static_cast<int>(kOriginY + TimelineView::kRulerHeight + 10.0f));
  ST_CHECK(canvas.pixel_at(static_cast<int>(header_zone.x),
                           static_cast<int>(header_zone.y)) == clip_body);

  // 命中坐标同步平移：点组件内同一屏幕 x（头部栏区域里，lane 左缘左侧），
  // 开关前后命中**同一时刻**——方案验收「点击同一 x 命中同一时间」的等价判据：
  // 拿播放头当探针（标尺点击 = x_to_time，直接量坐标换算，不经过手势选择）。
  // 开态：该 x 在头部栏（lane 外）→ 不消费；关态：该 x 在 lane 内 → 消费且命中 0ms。
  const float probe_x = kOriginX + 40.0f;
  fx.view.set_show_track_headers(false);
  fx.layout();
  Event hit;
  hit.kind = EventKind::MouseDown;
  hit.button = 1;
  hit.position = Point{probe_x, kOriginY + 6.0f};   // 标尺带内
  ST_CHECK(fx.view.on_event(fx.context, hit));
  // 关态 lane 左缘 = 组件左缘：probe_x（组件内 40px）→ 40px / 100pps = 400ms。
  // 开态同一 x 在头部栏里，永远到不了这里——播放头位置的差值正是头部栏宽度
  // 所代表的时刻差（坐标换算的平移量被重接量化）。
  ST_CHECK_EQ(fx.view.model().playhead(), Millis{400});
  Event up;
  up.kind = EventKind::MouseUp;
  up.position = hit.position;
  (void)fx.view.on_event(fx.context, up);

  // 对照：重新打开后，同一 x 在头部栏（lane 外）→ 不消费、播放头不动。
  fx.view.set_show_track_headers(true);
  fx.layout();
  fx.view.model().set_playhead(777);
  Event hit2;
  hit2.kind = EventKind::MouseDown;
  hit2.button = 1;
  hit2.position = Point{probe_x, kOriginY + 6.0f};
  ST_CHECK(!fx.view.on_event(fx.context, hit2));
  ST_CHECK_EQ(fx.view.model().playhead(), Millis{777});
  Event out2;
  out2.kind = EventKind::HoverOut;
  (void)fx.view.on_event(fx.context, out2);

  // 属性面同口径：set/get 与显式 setter 等价，乱值如实拒绝（与 snap/trim_handles
  // 同一严格口径）。
  ST_CHECK(fx.view.set_property("track_headers", "false"));
  ST_CHECK(!fx.view.show_track_headers());
  ST_CHECK_EQ(*fx.view.get_property("track_headers"), std::string("false"));
  ST_CHECK(fx.view.set_property("track_headers", "true"));
  ST_CHECK_EQ(*fx.view.get_property("track_headers"), std::string("true"));
  ST_CHECK(!fx.view.set_property("track_headers", "maybe"));
}

ST_TEST(timeline_view_clip_tint_paints_and_default_stays_theme) {
  // 片段着色（caiyun 拆解步骤 5，S5/G7）：数据接口（timeline::Clip::tint）的
  // 绘制侧验证——方案验收：两片段不同 tint ⇒ 块内像素可区分；默认 nullopt
  // ⇒ 主题色（与改动前一致）。
  Fixture fx;
  fx.load_default();
  fx.view.set_snap_enabled(false);

  // 基线：两个无 tint 片段同一主题色（同轨道同状态）。采样避开标签行
  //（块顶 +3 画 label，+10y 会采到文字色），取块内下半的中带；
  // 两端各让开 4px（描边与内缩缝）。
  const auto sample = [&fx](std::size_t t, std::size_t c) {
    const auto block = fx.clip_screen_rect(t, c);
    return fx.render().pixel_at(static_cast<int>(block.x + 20.0f),
                                static_cast<int>(block.y + block.height * 0.7f));
  };
  const Color theme_a = sample(0, 0);
  const Color theme_b = sample(0, 1);
  ST_CHECK(theme_a == theme_b);

  // 给 A 一个与主题色差异明显的 tint（饱和红），B 保持 nullopt。采样点同上
  //（同一 lambda 同一座标系——采样点漂移会让前后像素不可比）。
  fx.view.model().find_clip(0, 0)->tint = Color::rgb(200, 40, 20);
  const auto canvas = fx.render();
  const auto block_a = fx.clip_screen_rect(0, 0);
  const auto block_b = fx.clip_screen_rect(0, 1);
  const Color tinted = canvas.pixel_at(
      static_cast<int>(block_a.x + 20.0f),
      static_cast<int>(block_a.y + block_a.height * 0.7f));
  const Color plain = canvas.pixel_at(
      static_cast<int>(block_b.x + 20.0f),
      static_cast<int>(block_b.y + block_b.height * 0.7f));
  // 两片段像素可区分：tint 生效且默认片段仍走主题色（回「默认零变化」）。
  ST_CHECK(tinted != plain);
  ST_CHECK(plain == theme_b);
  // tint 真的落到像素上：与主题基色本身不同（不是「着色被忽略」的假绿）。
  ST_CHECK(tinted != theme_a);

  // tint 不影响几何：块矩形/命中与着色前一致（数据接口非绘制效果的分界线）。
  const auto rect_before = fx.clip_screen_rect(0, 0);
  ST_CHECK(fx.view.model().find_clip(0, 0)->tint.has_value());
  const auto rect_after = fx.clip_screen_rect(0, 0);
  ST_CHECK(rect_before.x == rect_after.x);
  ST_CHECK(rect_before.width == rect_after.width);

  // hover 语义在 tint 上叠加：悬停 tint 片段 → 像素变化（提亮跟着基色走）。
  const Point over{block_a.x + 20.0f, block_a.y + block_a.height * 0.7f};
  Event enter;
  enter.kind = EventKind::MouseMove;
  enter.position = over;
  (void)fx.view.on_event(fx.context, enter);
  ST_CHECK_EQ(*fx.view.get_property("hover_clip"), std::string("0:0"));
  const Color hovered =
      fx.render().pixel_at(static_cast<int>(over.x), static_cast<int>(over.y));
  ST_CHECK(hovered != tinted);

  // 选中态仍用主题选中色（组件状态语义不被宿主配色夺走）。
  Event down;
  down.kind = EventKind::MouseDown;
  down.button = 1;
  down.position = over;
  (void)fx.view.on_event(fx.context, down);
  ST_CHECK(fx.view.model().find_clip(0, 0)->selected);
  const Color selected =
      fx.render().pixel_at(static_cast<int>(over.x), static_cast<int>(over.y));
  ST_CHECK(selected != tinted);
  ST_CHECK(selected != hovered);
}

// ============================================================================
// 编辑委托通道（caiyun 拆解步骤 4，S1）——「数据在别处」形态的用例组。
//
// 前面 13 例钉的是**自持直写**语义（不挂 delegate 时行为字节不变——那本身就是本步
// 的硬约束）；这里钉的是挂上 delegate 后的六件事：
// ① move 接受：投影按目标更新 + on_clip_moved 仍发；
// ② move 拒绝：投影回到裁决前（净效果 = Model 不动）+ 交互回调不发；
// ③④ trim 同型两例（出点接受/入点拒绝——入点连带 source_in，拒绝时三字段一起回）；
// ⑤ 投影重建：宿主在回调里改真值源后 set_tracks，宿主值存活、快照不除旧；
// ⑥ 吸附通道随编辑权一并移交（候选/排除口径可验证）+ 不挂 delegate 时零调用。
// ============================================================================
namespace {

using Edge = TimelineView::EditDelegate::Edge;

/// move 回调的记账（断言要能看见组件传了什么，而不是桩自己算一遍再比对——
/// PITFALLS 场景 2「假绿：桩把差异抹平」的反面写法）。
struct MoveCall {
  std::size_t track{};
  std::size_t clip{};
  Millis new_start{};
};
struct TrimCall {
  std::size_t track{};
  std::size_t clip{};
  Edge edge{};
  Millis time{};
};
struct SnapCall {
  Millis candidate{};
  Millis exclude{};
};

/// 测试桩：只记账 + 可配置的裁决/吸附行为 + 可选的重入钩子（用例⑤）。
class RecordingDelegate : public TimelineView::EditDelegate {
 public:
  std::vector<MoveCall> moves{};
  std::vector<TrimCall> trims{};
  std::vector<SnapCall> snaps{};
  /// 全局裁决开关（true = 接受所有编辑）。
  bool accept{true};
  /// 吸附行为：默认恒等（原样返回）；置真则返回 snap_override（吸到指定点）。
  bool override_snap{false};
  Millis snap_override{};
  /// 重入钩子（用例⑤）：首次 move 时模拟宿主改真值源后重建投影。
  /// `view` 为非拥有指针（桩不持有组件，生命周期由用例的 Fixture 保证）。
  TimelineView* view{nullptr};
  bool rebuild_once{false};
  bool rebuilt{false};
  Millis host_start{};   ///< 重建时真值源里的起点（模拟宿主另行裁决后的形态）

  auto move(std::size_t track, std::size_t clip, Millis new_start) -> bool override {
    moves.push_back(MoveCall{track, clip, new_start});
    if (rebuild_once && !rebuilt && view != nullptr) {
      rebuilt = true;
      // 宿主「真值源」里这个片段其实在 5000（例：被另一条编辑链波纹推过去了）。
      // 重建投影是预期用法——宿主的值必须最后落笔，组件的乐观目标不得残留。
      Track video;
      video.id = "v1";
      video.label = "视频";
      video.clips.push_back(Clip{.id = "a", .label = "A", .start = host_start,
                                 .duration = 2000});
      Track audio;
      audio.id = "a1";
      audio.label = "音频";
      audio.height = 36.0f;
      audio.clips.push_back(Clip{.id = "c", .label = "C", .start = 1000, .duration = 500});
      view->model().set_tracks({video, audio});
    }
    return accept;
  }

  auto trim(std::size_t track, std::size_t clip, Edge edge, Millis time) -> bool override {
    trims.push_back(TrimCall{track, clip, edge, time});
    return accept;
  }

  auto snap(Millis candidate, Millis exclude) -> Millis override {
    snaps.push_back(SnapCall{candidate, exclude});
    return override_snap ? snap_override : candidate;
  }
};

}  // namespace

ST_TEST(timeline_view_delegate_move_accepted_writes_model_and_notifies) {
  // ① 接受：投影按目标更新 + 交互回调照发（宿主的撤销栈/检查器刷新都挂在这条上）。
  Fixture fx;
  fx.load_default();
  RecordingDelegate delegate;
  fx.view.set_edit_delegate(&delegate);
  fx.view.set_snap_enabled(false);
  ST_CHECK(fx.view.edit_delegate() == &delegate);

  const auto block = fx.clip_screen_rect(0, 0);
  const Point from{block.x + block.width * 0.5f, block.y + block.height * 0.5f};
  const Point to{from.x + 40.0f, from.y};   // 右移 40px = +400ms
  int moved = 0;
  fx.view.on_clip_moved = [&moved](std::size_t, std::size_t) { ++moved; };

  // 佐证基底：目标区域在拖前是空白色（拖后应是片段色——绘制仍读自持投影）。
  const Point probe{to.x + 20.0f, from.y};
  const Color gap_before =
      fx.render().pixel_at(static_cast<int>(probe.x), static_cast<int>(probe.y));

  ST_CHECK(drag(fx.view, fx.context, from, to));

  // 投影更新到目标：move 收到的与写入的都是绝对目标 400（不是增量）。
  ST_CHECK_EQ(fx.view.model().find_clip(0, 0)->start, Millis{400});
  ST_REQUIRE(delegate.moves.size() == 1);
  ST_CHECK_EQ(delegate.moves[0].track, std::size_t{0});
  ST_CHECK_EQ(delegate.moves[0].clip, std::size_t{0});
  ST_CHECK_EQ(delegate.moves[0].new_start, Millis{400});
  // 移动不是缩放：duration/source_in 不动（委托通道同样遵守）。
  ST_CHECK_EQ(fx.view.model().find_clip(0, 0)->duration, Millis{2000});
  ST_CHECK_EQ(fx.view.model().find_clip(0, 0)->source_in, Millis{0});
  // 接受时交互回调仍发（一次拖拽一次）。
  ST_CHECK_EQ(moved, 1);
  // 像素佐证：新位置画出了片段（绘制读的就是刚写的投影）。
  const Color clip_after =
      fx.render().pixel_at(static_cast<int>(probe.x), static_cast<int>(probe.y));
  ST_CHECK(clip_after != gap_before);
}

ST_TEST(timeline_view_delegate_move_rejected_leaves_model_untouched) {
  // ② 拒绝：投影回到裁决前——净效果 = 自持 Model 不动；交互回调不发。
  Fixture fx;
  fx.load_default();
  RecordingDelegate delegate;
  delegate.accept = false;   // 宿主一概拒绝（例：只读预览形态/锁定语义在宿主侧）
  fx.view.set_edit_delegate(&delegate);
  fx.view.set_snap_enabled(false);

  const auto block = fx.clip_screen_rect(0, 0);
  const Point from{block.x + block.width * 0.5f, block.y + block.height * 0.5f};
  int moved = 0;
  fx.view.on_clip_moved = [&moved](std::size_t, std::size_t) { ++moved; };

  ST_CHECK(drag(fx.view, fx.context, from, Point{from.x + 40.0f, from.y}));

  // 三个字段全部回到拖拽前（乐观写被回滚，没有残留）。
  ST_CHECK_EQ(fx.view.model().find_clip(0, 0)->start, Millis{0});
  ST_CHECK_EQ(fx.view.model().find_clip(0, 0)->duration, Millis{2000});
  ST_CHECK_EQ(fx.view.model().find_clip(0, 0)->source_in, Millis{0});
  // 宿主确实被问过（拿到的是绝对目标 400）。
  ST_REQUIRE(delegate.moves.size() == 1);
  ST_CHECK_EQ(delegate.moves[0].new_start, Millis{400});
  // 拒绝时交互回调不发（没发生的编辑不该进宿主的撤销栈）。
  ST_CHECK_EQ(moved, 0);
}

ST_TEST(timeline_view_delegate_trim_right_accepted_writes_duration) {
  // ③a 出点修剪接受：duration 更新、start/source_in 不动；edge/time 口径可验证。
  Fixture fx;
  fx.load_default();
  RecordingDelegate delegate;
  fx.view.set_edit_delegate(&delegate);
  fx.view.set_snap_enabled(false);

  const auto block = fx.clip_screen_rect(0, 0);
  const Point from{block.right() - 2.0f, block.y + block.height * 0.5f};   // 抓右缘
  const Point to{from.x - 50.0f, from.y};                                 // -500ms
  int trimmed = 0;
  fx.view.on_clip_trimmed = [&trimmed](std::size_t, std::size_t) { ++trimmed; };

  ST_CHECK(drag(fx.view, fx.context, from, to));

  ST_CHECK_EQ(fx.view.model().find_clip(0, 0)->duration, Millis{1500});
  ST_CHECK_EQ(fx.view.model().find_clip(0, 0)->start, Millis{0});
  ST_CHECK_EQ(fx.view.model().find_clip(0, 0)->source_in, Millis{0});
  // trim 收到：Right 边 + 新**结束时刻**（start+duration = 1500，不是新时长）。
  ST_REQUIRE(delegate.trims.size() == 1);
  ST_CHECK(delegate.trims[0].edge == Edge::Right);   // 枚举无 formatter，用布尔断言
  ST_CHECK_EQ(delegate.trims[0].time, Millis{1500});
  ST_CHECK_EQ(trimmed, 1);
}

ST_TEST(timeline_view_delegate_trim_left_rejected_leaves_clip_intact) {
  // ③b 入点修剪拒绝：start/duration/**source_in** 三字段一起回滚（入点的乐观写
  // 连带源入点，拒绝时只回 start 不回 source_in 就会把「修剪」错留成半截位移）。
  Fixture fx;
  fx.load_default();
  RecordingDelegate delegate;
  delegate.accept = false;
  fx.view.set_edit_delegate(&delegate);
  fx.view.set_snap_enabled(false);
  fx.view.model().find_clip(0, 0)->source_in = 1000;   // 非零源入点

  const auto block = fx.clip_screen_rect(0, 0);
  const Point from{block.x + 2.0f, block.y + block.height * 0.5f};   // 抓左缘
  const Point to{from.x + 30.0f, from.y};                           // +300ms
  int trimmed = 0;
  fx.view.on_clip_trimmed = [&trimmed](std::size_t, std::size_t) { ++trimmed; };

  ST_CHECK(drag(fx.view, fx.context, from, to));

  ST_CHECK_EQ(fx.view.model().find_clip(0, 0)->start, Millis{0});
  ST_CHECK_EQ(fx.view.model().find_clip(0, 0)->duration, Millis{2000});
  ST_CHECK_EQ(fx.view.model().find_clip(0, 0)->source_in, Millis{1000});
  // trim 收到：Left 边 + 新**起点**（300，不是新时长/新结束）。
  ST_REQUIRE(delegate.trims.size() == 1);
  ST_CHECK(delegate.trims[0].edge == Edge::Left);   // 枚举无 formatter，用布尔断言
  ST_CHECK_EQ(delegate.trims[0].time, Millis{300});
  ST_CHECK_EQ(trimmed, 0);
}

ST_TEST(timeline_view_delegate_rebuild_projection_inside_callback_survives) {
  // ④ 投影语义：宿主在 move 回调里改真值源后 set_tracks 重建——宿主值必须
  // 存活（组件的乐观目标不残留）；且重建不断拖拽：下一次 MouseDown 的快照
  // 取自重建后的投影，move 收到的目标不再基于拖拽起点的旧值。
  Fixture fx;
  fx.load_default();
  RecordingDelegate delegate;
  delegate.rebuild_once = true;
  delegate.host_start = 5000;
  delegate.view = &fx.view;
  fx.view.set_edit_delegate(&delegate);
  fx.view.set_snap_enabled(false);

  const auto block = fx.clip_screen_rect(0, 0);
  const Point from{block.x + block.width * 0.5f, block.y + block.height * 0.5f};
  ST_CHECK(drag(fx.view, fx.context, from, Point{from.x + 40.0f, from.y}));

  // 宿主重建后的投影存活：乐观目标 400 被重建盖掉，起点是真值源里的 5000。
  ST_CHECK_EQ(fx.view.model().find_clip(0, 0)->start, Millis{5000});
  ST_CHECK_EQ(delegate.moves.size(), std::size_t{1});
  ST_CHECK_EQ(delegate.moves[0].new_start, Millis{400});   // 组件间的是它的目标

  // 同一次拖拽会话已结束；从**重建后的新位置**再拖一格（+100px = +1000ms）：
  // 快照必须取自重建后的投影（5000），move 收到绝对目标 6000——若组件用旧
  // 快照（拖拽起点的 0）就会报 1000，这正是「重建不断拖拽」要钉的语义。
  const auto moved_block = fx.clip_screen_rect(0, 0);
  const Point from2{moved_block.x + 20.0f, moved_block.y + moved_block.height * 0.5f};
  ST_CHECK(drag(fx.view, fx.context, from2, Point{from2.x + 100.0f, from2.y}));
  ST_REQUIRE(delegate.moves.size() == std::size_t{2});
  ST_CHECK_EQ(delegate.moves[1].new_start, Millis{6000});
  // 这次没有重建钩子（只一次）：接受后乐观写存活。
  ST_CHECK_EQ(fx.view.model().find_clip(0, 0)->start, Millis{6000});
}

ST_TEST(timeline_view_delegate_snap_channel_and_absent_delegate_baseline) {
  // ⑤ 吸附通道随编辑权一并移交（候选/排除口径可验证）+ 保底：不挂 delegate
  // 时它一次都不被调用，直写路径照旧。
  Fixture fx;
  fx.load_default();
  fx.view.set_snap_enabled(false);
  RecordingDelegate idle;   // 造出来但**不挂**——它的任何记账都是「不该发生」证据
  const auto block = fx.clip_screen_rect(0, 0);
  const Point from{block.x + block.width * 0.5f, block.y + block.height * 0.5f};
  ST_CHECK(drag(fx.view, fx.context, from, Point{from.x + 40.0f, from.y}));
  // 直写路径照旧（未挂 delegate）：Model 直写、零回调进入桩。
  ST_CHECK_EQ(fx.view.model().find_clip(0, 0)->start, Millis{400});
  ST_CHECK_EQ(idle.moves.size(), std::size_t{0});
  ST_CHECK_EQ(idle.trims.size(), std::size_t{0});
  ST_CHECK_EQ(idle.snaps.size(), std::size_t{0});

  // 同一 Fixture 换轨道：挂 delegate、开吸附，把片段 a 换到非零起点 1000
  //（exclude 口径要有非零值才验得出错——全 0 会把「传错」也藏成绿）。
  fx.load_default();
  fx.view.model().find_clip(0, 0)->start = 1000;
  fx.layout();
  RecordingDelegate delegate;
  delegate.override_snap = true;
  delegate.snap_override = 1500;   // 宿主吸附点：真值源里别的片段边缘
  fx.view.set_edit_delegate(&delegate);
  fx.view.set_snap_enabled(true);

  const auto shifted = fx.clip_screen_rect(0, 0);
  const Point from2{shifted.x + shifted.width * 0.5f,
                    shifted.y + shifted.height * 0.5f};
  ST_CHECK(drag(fx.view, fx.context, from2, Point{from2.x + 40.0f, from2.y}));

  // snap 收到：candidate = 拖拽目标（1000+400 = 1400），exclude = 快照起点 1000
  //（排除被拖片段的语义随编辑权一并移交，时间口径）。
  ST_REQUIRE(delegate.snaps.size() == std::size_t{1});
  ST_CHECK_EQ(delegate.snaps[0].candidate, Millis{1400});
  ST_CHECK_EQ(delegate.snaps[0].exclude, Millis{1000});
  // move 收到的是吸附后的目标（1500），投影也落在那。
  ST_REQUIRE(delegate.moves.size() == std::size_t{1});
  ST_CHECK_EQ(delegate.moves[0].new_start, Millis{1500});
  ST_CHECK_EQ(fx.view.model().find_clip(0, 0)->start, Millis{1500});
}

ST_TEST(timeline_view_model_replaced_mid_drag_releases_drag) {
  // 步骤 3 的核心用例：拖拽中途宿主 `set_tracks` 重建数据 + 调
  // `notify_model_replaced()` 后，继续移动鼠标**不得**改到新下标对应的片段
  //（悬置快照指向旧投影，陈旧下标会改错对象——本单要抓的目标缺陷）。
  Fixture fx;
  fx.load_default();
  fx.view.set_snap_enabled(false);
  const auto block = fx.clip_screen_rect(0, 0);
  const Point from{block.x + block.width * 0.5f, block.y + block.height * 0.5f};

  // 按下（不抬起）：进入拖拽态。
  Event down;
  down.kind = EventKind::MouseDown;
  down.button = 1;
  down.position = from;
  ST_CHECK(fx.view.on_event(fx.context, down));

  // 宿主整体重建：同下标 (0,0) 换成完全不同的几何（模拟「新投影」）。
  Track video;
  video.id = "v2";
  video.label = "视频";
  video.clips.push_back(Clip{.id = "x", .label = "X", .start = 0, .duration = 1000});
  fx.set_tracks({video});
  fx.view.notify_model_replaced();

  // 继续移动（旧快照下 +100px = +1000ms；若悬置态残留，新 (0,0) 的 start
  // 会被改成 1000）。复位后 move 走悬停分支**不消费**（返回 false）——
  // 与「抬起后移动不再消费」同一口径，消费了反证拖拽态还在。
  Event move;
  move.kind = EventKind::MouseMove;
  move.position = Point{from.x + 100.0f, from.y};
  ST_CHECK(!fx.view.on_event(fx.context, move));
  const Clip* clip = fx.view.model().find_clip(0, 0);
  ST_REQUIRE(clip != nullptr);
  ST_CHECK_EQ(clip->start, Millis{0});   // 新片段原封不动
  ST_CHECK_EQ(clip->duration, Millis{1000});

  // 悬置拖拽已复位：此后的 MouseMove 不再处于拖拽态（回到悬停追踪分支）。
  Event move2;
  move2.kind = EventKind::MouseMove;
  move2.position = Point{from.x + 120.0f, from.y};
  ST_CHECK(!fx.view.on_event(fx.context, move2));
  ST_CHECK_EQ(fx.view.model().find_clip(0, 0)->start, Millis{0});
}

ST_TEST(timeline_view_model_replaced_mid_trim_releases_drag) {
  // 修剪同型：拖左边缘中途重建数据，后续 move 不得拿旧快照去改新下标的片段
  //（ClipLeft 分支会同时写 start/duration/source_in 三个字段——改错对象的
  // 伤害面比移动更大）。
  Fixture fx;
  fx.load_default();
  fx.view.set_snap_enabled(false);
  fx.view.model().find_clip(0, 0)->source_in = 500;
  const auto block = fx.clip_screen_rect(0, 0);
  const Point from{block.x + 2.0f, block.y + block.height * 0.5f};   // 抓左缘

  Event down;
  down.kind = EventKind::MouseDown;
  down.button = 1;
  down.position = from;
  ST_CHECK(fx.view.on_event(fx.context, down));

  // 宿主重建：新 (0,0) 是不同几何的片段。
  Track video;
  video.id = "v2";
  video.label = "视频";
  video.clips.push_back(Clip{.id = "x", .label = "X", .start = 2000, .duration = 800,
                             .source_in = 300});
  fx.set_tracks({video});
  fx.view.notify_model_replaced();

  // 继续修（旧快照下 +30px = +300ms；残留态会把新片段的 start 改成 300）。
  // 复位后 move 不消费（同上一例的口径）。
  Event move;
  move.kind = EventKind::MouseMove;
  move.position = Point{from.x + 30.0f, from.y};
  ST_CHECK(!fx.view.on_event(fx.context, move));
  const Clip* clip = fx.view.model().find_clip(0, 0);
  ST_REQUIRE(clip != nullptr);
  ST_CHECK_EQ(clip->start, Millis{2000});
  ST_CHECK_EQ(clip->duration, Millis{800});
  ST_CHECK_EQ(clip->source_in, Millis{300});
}

ST_TEST(timeline_view_stale_snapshot_without_notify_still_releases) {
  // 兜底防线：宿主重建数据但**忘调** `notify_model_replaced()`——MouseMove 的
  // 快照自校验必须自行复位（步骤 3 计划原文：「若 drag_ 捕获的快照与当前 Model
  // 不符则复位 drag_」）。
  Fixture fx;
  fx.load_default();
  fx.view.set_snap_enabled(false);
  const auto block = fx.clip_screen_rect(0, 0);
  const Point from{block.x + block.width * 0.5f, block.y + block.height * 0.5f};

  Event down;
  down.kind = EventKind::MouseDown;
  down.button = 1;
  down.position = from;
  ST_CHECK(fx.view.on_event(fx.context, down));

  // 宿主重建且不调薄入口（新投影里 (0,0) 的几何与拖拽时捕获的快照不符）。
  Track video;
  video.id = "v2";
  video.label = "视频";
  video.clips.push_back(Clip{.id = "x", .label = "X", .start = 0, .duration = 1000});
  fx.set_tracks({video});

  // 继续移动：自校验发现快照陈旧 → 复位拖拽态并吃掉本次事件，新片段原封不动。
  Event move;
  move.kind = EventKind::MouseMove;
  move.position = Point{from.x + 100.0f, from.y};
  ST_CHECK(fx.view.on_event(fx.context, move));
  const Clip* clip = fx.view.model().find_clip(0, 0);
  ST_REQUIRE(clip != nullptr);
  ST_CHECK_EQ(clip->start, Millis{0});
  ST_CHECK_EQ(clip->duration, Millis{1000});

  // 与上一例同理：后续 move 回到无拖拽分支（不消费）。
  Event move2;
  move2.kind = EventKind::MouseMove;
  move2.position = Point{from.x + 120.0f, from.y};
  ST_CHECK(!fx.view.on_event(fx.context, move2));
  ST_CHECK_EQ(fx.view.model().find_clip(0, 0)->start, Millis{0});
}

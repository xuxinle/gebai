/// 组件动画引擎（CSS 式）：缓动 / 关键帧 / 过渡 / 元素落点 / 合成 / 几何平移。
///
/// ## 为什么这些用例是这几条
///
/// 动画的**验收标准长在用户身上**（“淡入是不是真的淡了”“滑入有没有残影”），
/// 而它能被机器钉住的只有三类量：
///
/// 1. **纯函数语义**（缓动端点/单调、关键帧的 delay/iteration/direction/fill）——
///    这些外部只有一条正确答案（CSS 规范），拿不准就是错的；
/// 2. **元素级可观测值**（`animation_opacity` / `animation_translate` / `running`）——
///    属性面是"这一帧到底算成什么"的直接读回，比数像素稳；
/// 3. **像素**（透明度真的让底色透出来、位移真的挪了墨迹）——属性全对而画面错，
///    在这条线上是真实存在的失败形态（绘制路径与样式路径是两条）。
///
/// 另外两条**反空转**判据必须钉住（它们对应的缺陷历史上真发生过）：
/// 播完必须不再请求续帧（否则静止画面按 60fps 空转，实测 6 秒耗 6.12 秒 CPU），
/// 以及 `fill: none` 结束之后必须回到元素自身样式（而不是永久停在终态）。

#include "st/test/test.hpp"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "st/raster/canvas.hpp"
#include "st/raster/translated_surface.hpp"
#include "st/ui/animation.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::math::Color;
using st::math::Point;
using st::math::Rect;
using st::raster::Canvas;
using st::ui::Element;
using st::ui::Panel;
using st::ui::Theme;
using st::ui::UiRoot;
using st::ui::anim::Animation;
using st::ui::anim::Easing;
using st::ui::anim::Keyframes;
using st::ui::anim::Props;
using st::ui::anim::Property;

constexpr float kEps = 1e-4F;

// ────────────────────────────────────────────────────────────────────────────
// 缓动
// ────────────────────────────────────────────────────────────────────────────

ST_TEST(animation_easing_hits_endpoints_exactly) {
  // 端点必须**精确**：t=0 若差一点点，动画首帧就跳一下；t=1 若不等于 1，
  // `opacity` 会永远差 1/255（看起来“淡入结束后还是有点透”）。
  const std::vector<Easing> all{Easing::linear(), Easing::ease(), Easing::ease_in(),
                                Easing::ease_out(), Easing::ease_in_out(),
                                Easing::bezier(0.34F, 1.56F, 0.64F, 1.0F),
                                Easing::step(4)};
  for (const Easing& easing : all) {
    ST_CHECK(std::abs(st::ui::anim::sample(easing, 0.0F)) <= 1e-6F);
    ST_CHECK(std::abs(st::ui::anim::sample(easing, 1.0F) - 1.0F) <= 1e-6F);
  }
}

ST_TEST(animation_easing_is_monotonic_and_clamped) {
  // 单调不减（回弹曲线除外）——单调性坏了的表现是“动画中途往回跳一帧”，
  // 而那在截图上根本抓不到。
  for (const Easing& easing :
       {Easing::linear(), Easing::ease(), Easing::ease_in(), Easing::ease_out(),
        Easing::ease_in_out(), Easing::bezier(0.42F, 0.0F, 0.58F, 1.0F)}) {
    float previous = -1.0F;
    for (int step = 0; step <= 40; ++step) {
      const float value = st::ui::anim::sample(easing, static_cast<float>(step) / 40.0F);
      ST_CHECK(value >= previous - kEps);
      previous = value;
    }
  }
  // 越界输入必须被夹住，且**不能出 NaN**：动画首尾常给 −1e-7 这类值，
  // 而 `pow(负数, 非整数次)` 是 NaN——NaN 混进布局会变成“元素凭空消失”。
  for (const float t : {-1.0F, -1e-7F, 1.5F, 1e9F}) {
    const float value = st::ui::anim::sample(Easing::ease_in_out(), t);
    ST_CHECK(!std::isnan(value));
    ST_CHECK(value >= -kEps);
    ST_CHECK(value <= 1.0F + kEps);
  }
}

ST_TEST(animation_named_easings_match_css_control_points) {
  // `ease` 是 `cubic-bezier(0.25,0.1,0.25,1)`：t=0.5 处应落在 0.80 附近
  // （CSS 的 `ease` 前段快、后段慢）。用“明显大于线性”这条粗判据，
  // 而不是精确值——精确值会把实现绑死在某个迭代次数上。
  const float ease = st::ui::anim::sample(Easing::ease(), 0.5F);
  ST_CHECK(ease > 0.7F);
  ST_CHECK(ease < 0.9F);
  // `ease-in` 起步慢：中点必须**低于**线性。
  ST_CHECK(st::ui::anim::sample(Easing::ease_in(), 0.5F) < 0.45F);
  // `ease-out` 起步快：中点必须**高于**线性。
  ST_CHECK(st::ui::anim::sample(Easing::ease_out(), 0.5F) > 0.55F);
  // `linear` 精确等于输入。
  ST_CHECK(std::abs(st::ui::anim::sample(Easing::linear(), 0.37F) - 0.37F) <= kEps);
}

ST_TEST(animation_steps_jump_at_correct_positions) {
  const Easing end = Easing::step(4, st::ui::anim::StepPosition::JumpEnd);
  // jump-end：0~0.25 都是 0，0.25 起跳到 0.25。
  ST_CHECK(std::abs(st::ui::anim::sample(end, 0.1F)) <= kEps);
  ST_CHECK(std::abs(st::ui::anim::sample(end, 0.25F) - 0.25F) <= kEps);
  const Easing start = Easing::step(4, st::ui::anim::StepPosition::JumpStart);
  ST_CHECK(std::abs(st::ui::anim::sample(start, 0.0F)) <= kEps);
  ST_CHECK(std::abs(st::ui::anim::sample(start, 0.01F) - 0.25F) <= kEps);
}

ST_TEST(animation_parse_easing_accepts_css_syntax) {
  const auto ease = st::ui::anim::parse_easing("ease-in-out");
  ST_CHECK(ease.has_value());
  ST_CHECK(ease->kind == st::ui::anim::EasingKind::EaseInOut);
  const auto bezier = st::ui::anim::parse_easing("cubic-bezier(0.34, 1.56, 0.64, 1)");
  ST_CHECK(bezier.has_value());
  ST_CHECK(std::abs(bezier->x1 - 0.34F) <= kEps);
  ST_CHECK(std::abs(bezier->y2 - 1.0F) <= kEps);
  const auto steps = st::ui::anim::parse_easing("steps(3, jump-start)");
  ST_CHECK(steps.has_value());
  ST_CHECK(steps->steps == 3);
  ST_CHECK(steps->step_position == st::ui::anim::StepPosition::JumpStart);
  // 认不出的**如实返回空**（不静默当线性：那会让“写错了”看起来像“动画没效果也很正常”）。
  ST_CHECK(!st::ui::anim::parse_easing("bouncy").has_value());
  ST_CHECK(!st::ui::anim::parse_easing("cubic-bezier(1,2)").has_value());
}

// ────────────────────────────────────────────────────────────────────────────
// 关键帧
// ────────────────────────────────────────────────────────────────────────────

namespace {

[[nodiscard]] auto make_fade_frames() -> Keyframes {
  Keyframes out{};
  out.name = "test-fade";
  Props head{};
  head.opacity = 0.0F;
  Props tail{};
  tail.opacity = 1.0F;
  out.frames.push_back(st::ui::anim::Keyframe{0.0F, head});
  out.frames.push_back(st::ui::anim::Keyframe{1.0F, tail});
  return out;
}

[[nodiscard]] auto make_animation(double duration, int iterations) -> Animation {
  Animation out{};
  out.name = "test-fade";
  out.duration = duration;
  out.iterations = iterations;
  out.easing = Easing::linear();   // 线性：让“时间 → 进度”的映射可以直接算
  out.fill = Animation::Fill::Forwards;
  return out;
}

}  // namespace

ST_TEST(animation_lerp_only_touches_declared_properties) {
  // `from {opacity:0}` 只该动 opacity：其余属性必须保持“没声明”。
  // 若给每个字段填默认值，`from` 帧会把背景色一起冲成默认色（整块变黑）。
  Props head{};
  head.opacity = 0.0F;
  Props tail{};
  tail.opacity = 1.0F;
  const Props middle = st::ui::anim::lerp_props(head, tail, 0.5F);
  ST_CHECK(middle.opacity.has_value());
  ST_CHECK(std::abs(*middle.opacity - 0.5F) <= kEps);
  ST_CHECK(!middle.background.has_value());
  ST_CHECK(!middle.color.has_value());
  ST_CHECK(!middle.translate_x.has_value());
}

ST_TEST(animation_sample_respects_fill_and_running) {
  const Keyframes frames = make_fade_frames();
  const Animation animation = make_animation(1.0, 1);
  // 0.5s 处：半透明、仍在跑。
  const auto middle = st::ui::anim::sample_animation(frames, animation, 0.5);
  ST_CHECK(middle.running);
  ST_CHECK(std::abs(*middle.props.opacity - 0.5F) <= 1e-3F);
  // 结束：`fill: forwards` 保留终态，但**不再续帧**（这是防空转的硬判据）。
  const auto done = st::ui::anim::sample_animation(frames, animation, 2.0);
  ST_CHECK(!done.running);
  ST_CHECK(std::abs(*done.props.opacity - 1.0F) <= kEps);

  // `fill: none`：结束后**不施加任何覆盖**（回到元素自身样式，而不是永久停在终态）。
  Animation plain = animation;
  plain.fill = Animation::Fill::None;
  const auto cleared = st::ui::anim::sample_animation(frames, plain, 2.0);
  ST_CHECK(!cleared.running);
  ST_CHECK(cleared.props.empty());
}

ST_TEST(animation_delay_and_backwards_fill) {
  const Keyframes frames = make_fade_frames();
  Animation animation = make_animation(1.0, 1);
  animation.delay = 0.5;
  animation.fill = Animation::Fill::Backwards;
  // 延迟期内：`backwards` 显示首帧（opacity=0），且仍在“播”（要续帧等它开始）。
  const auto waiting = st::ui::anim::sample_animation(frames, animation, 0.2);
  ST_CHECK(waiting.running);
  ST_CHECK(waiting.props.opacity.has_value());
  ST_CHECK(std::abs(*waiting.props.opacity) <= kEps);
  // 负延迟 = 从动画中途开始（CSS 同语义）。
  Animation early = make_animation(1.0, 1);
  early.delay = -0.5;
  const auto halfway = st::ui::anim::sample_animation(frames, early, 0.0);
  ST_CHECK(std::abs(*halfway.props.opacity - 0.5F) <= 1e-3F);
}

ST_TEST(animation_alternate_reverses_odd_iterations) {
  const Keyframes frames = make_fade_frames();
  Animation animation = make_animation(1.0, 2);
  animation.direction = Animation::Direction::Alternate;
  animation.fill = Animation::Fill::Both;
  // 第一次循环 0.5s：正放 → 0.5
  ST_CHECK(std::abs(*st::ui::anim::sample_animation(frames, animation, 0.5).props.opacity - 0.5F) <=
           1e-3F);
  // 第二次循环 0.5s（总 1.5s）：反放 → `local=0.5` 取 1-0.5=0.5，也是 0.5（对称数据，
  // 换个不对称的判定：反放时 `local=0.25` 应得 0.75）。
  ST_CHECK(std::abs(*st::ui::anim::sample_animation(frames, animation, 1.25).props.opacity -
                    0.75F) <=
           1e-3F);
}

ST_TEST(animation_infinite_never_finishes) {
  const Keyframes frames = make_fade_frames();
  const Animation animation = make_animation(1.0, 0);   // 0 = infinite
  ST_CHECK(animation.infinite());
  // 一万秒之后仍在跑（无限循环永不 `running=false`）。
  ST_CHECK(st::ui::anim::sample_animation(frames, animation, 10000.0).running);
}

ST_TEST(animation_keyframes_normalize_sorts_and_closes_ends) {
  Keyframes messy{};
  messy.name = "messy";
  Props low{};
  low.opacity = 0.2F;
  Props high{};
  high.opacity = 0.9F;
  // 故意乱序 + 缺端点。
  messy.frames.push_back(st::ui::anim::Keyframe{0.6F, high});
  messy.frames.push_back(st::ui::anim::Keyframe{0.3F, low});
  const Keyframes fixed = messy.normalized();
  ST_CHECK(fixed.frames.size() >= 4U);   // 补了 0 与 1
  for (std::size_t index = 1; index < fixed.frames.size(); ++index) {
    ST_CHECK(fixed.frames[index - 1].offset <= fixed.frames[index].offset);
  }
  ST_CHECK(std::abs(fixed.frames.front().offset) <= kEps);
  ST_CHECK(std::abs(fixed.frames.back().offset - 1.0F) <= kEps);
}

ST_TEST(animation_presets_exist_and_have_duration) {
  // 预设的**时长也必须给好**：让调用方各自传时长会立刻出现“同一屏里两个卡片节奏不同”。
  for (const std::string_view name : st::ui::anim::preset_names()) {
    const auto frames = st::ui::anim::preset_keyframes(name);
    const auto animation = st::ui::anim::preset_animation(name);
    ST_CHECK(frames.has_value());
    ST_CHECK(animation.has_value());
    ST_CHECK(animation->duration > 0.0);
    ST_CHECK(!frames->normalized().frames.empty());
    // 采样首帧不能崩、且确实给出一个动画属性（`shake` 只动位移，没有 opacity）。
    const auto sample = st::ui::anim::sample_animation(*frames, *animation, 0.0);
    ST_CHECK(!sample.props.empty());
  }
  // 不存在的名字如实返回空。
  ST_CHECK(!st::ui::anim::preset_animation("no-such-preset").has_value());
}

// ────────────────────────────────────────────────────────────────────────────
// 元素级落点
// ────────────────────────────────────────────────────────────────────────────

namespace {

constexpr int kWidth = 320;
constexpr int kHeight = 200;

/// 一个最小可跑的元素树（含一块自绘底色，方便像素断言）。
struct Fixture {
  Theme theme{Theme::light()};
  UiRoot root{};
  Panel* box{nullptr};
  double clock{1.0};   ///< 单调（真实时间只增不减；否则会走“静态帧落位”分支）

  Fixture() {
    root.set_theme(theme);
    auto owned = std::make_unique<Panel>();
    box = owned.get();
    box->set_id("box");
    box->style().background = Color::rgb(0, 0, 0);   // 纯黑：透明度变化最容易量
    box->style().width = 100.0F;
    box->style().height = 60.0F;
    root.set_content(std::move(owned));
    root.set_viewport(st::math::Size{static_cast<float>(kWidth), static_cast<float>(kHeight)});
  }

  [[nodiscard]] auto context(double time) const -> st::ui::RenderContext {
    return st::ui::RenderContext{theme, nullptr, time};
  }
  void layout() {
    root.layout();
    box->measure(context(clock), st::ui::Constraints{});
    box->arrange(context(clock), Rect{40.0f, 40.0f, 100.0f, 60.0F});
  }
  /// 推进到 `time` 并画一帧（动画在 `paint` 里推进，这是唯一的时间入口）。
  ///
  /// **必须跟着 `clear_dirty()`**：那是宿主帧循环的收尾（`Application::render_frame`
  /// 同样这么调），它把“上一帧还有谁在动”滚成 `followup_`——否则下一帧会因为
  /// 损坏区为空走“空帧”分支，动画就停在半程（这正是实测踩到的形态）。
  void frame(Canvas& canvas, double time) {
    clock = time;
    root.set_time(time);
    (void)root.paint_frame(canvas);
    root.clear_dirty();
  }
};

[[nodiscard]] auto pixel(const Canvas& canvas, int x, int y) -> Color {
  return canvas.pixel_at(x, y);
}

}  // namespace

ST_TEST(animation_element_opacity_reaches_pixels_and_settles) {
  Fixture fixture;
  fixture.layout();
  Canvas canvas = Canvas::for_logical_size(kWidth, kHeight, 1.0F);
  canvas.clear(Color::rgb(255, 255, 255));

  Animation animation{};
  animation.name = "fade";
  animation.duration = 1.0;
  animation.easing = Easing::linear();
  animation.fill = Animation::Fill::Forwards;
  Props head{};
  head.opacity = 0.0F;
  Props tail{};
  tail.opacity = 1.0F;
  Keyframes frames{};
  frames.name = "fade";
  frames.frames.push_back(st::ui::anim::Keyframe{0.0F, head});
  frames.frames.push_back(st::ui::anim::Keyframe{1.0F, tail});
  fixture.box->set_animation(animation, frames);

  // 起播那一帧（t=0）：完全透明 → 黑块画不出来，像素是清屏色。
  fixture.frame(canvas, 1.0);
  ST_CHECK(fixture.box->animation_running());
  const Color start_pixel = pixel(canvas, 80, 70);
  ST_CHECK(static_cast<int>(start_pixel.r) > 240);   // 仍是白底

  // 半程：像素必须是中间灰（**这是像素判据的意义**：属性面 0.5 也可能因为
  // 绘制路径没读 `paint_style()` 而完全不动）。
  fixture.frame(canvas, 1.5);
  const auto halfway = fixture.box->get_property("animation_opacity");
  ST_CHECK(halfway.has_value());
  ST_CHECK(std::abs(std::stod(*halfway) - 0.5) <= 0.02);
  const Color mid_pixel = pixel(canvas, 80, 70);
  ST_CHECK(static_cast<int>(mid_pixel.r) > 90);
  ST_CHECK(static_cast<int>(mid_pixel.r) < 165);
  ST_CHECK(static_cast<int>(mid_pixel.r) > 100);   // 0.5 不透明黑压白底 ≈ 127

  // 结束：完全不透明，且**不再请求续帧**。
  fixture.frame(canvas, 2.5);
  ST_CHECK(!fixture.box->animation_running());
  const Color done_pixel = pixel(canvas, 80, 70);
  ST_CHECK(static_cast<int>(done_pixel.r) < 12);
}

ST_TEST(animation_fill_none_returns_to_original_style) {
  // `fill: none` 结束后必须回到元素自身样式。漏了这条会变成“入场动画播完
  // 元素永久留在起始状态”（最惨的一种：opacity 停在 0，元素整个消失）。
  Fixture fixture;
  fixture.layout();
  Canvas canvas = Canvas::for_logical_size(kWidth, kHeight, 1.0F);
  canvas.clear(Color::rgb(255, 255, 255));
  Animation animation{};
  animation.name = "in";
  animation.duration = 0.5;
  animation.easing = Easing::linear();
  animation.fill = Animation::Fill::None;
  Props head{};
  head.opacity = 0.0F;
  Props tail{};
  tail.opacity = 1.0F;
  Keyframes frames{};
  frames.frames.push_back(st::ui::anim::Keyframe{0.0F, head});
  frames.frames.push_back(st::ui::anim::Keyframe{1.0F, tail});
  fixture.box->set_animation(animation, frames);
  fixture.frame(canvas, 1.0);
  fixture.frame(canvas, 3.0);   // 早已结束
  // 元素自身 `style_.opacity` 是 1 → 黑块必须完全不透明。
  ST_CHECK(!fixture.box->animation_props().opacity.has_value());
  const Color done = pixel(canvas, 80, 70);
  ST_CHECK(static_cast<int>(done.r) < 12);
}

ST_TEST(animation_translate_moves_drawn_pixels) {
  // 位移必须真的挪了墨迹（属性面 6px 不代表绘制挪了 6px——
  // 绘制走的是 `TranslatedSurface`，与样式路径是两套）。
  Fixture fixture;
  fixture.layout();
  Canvas canvas = Canvas::for_logical_size(kWidth, kHeight, 1.0F);
  canvas.clear(Color::rgb(255, 255, 255));
  Animation animation{};
  animation.name = "slide";
  animation.duration = 1.0;
  animation.easing = Easing::linear();
  animation.fill = Animation::Fill::Both;
  Props head{};
  head.translate_y = 40.0F;
  Props tail{};
  tail.translate_y = 0.0F;
  Keyframes frames{};
  frames.frames.push_back(st::ui::anim::Keyframe{0.0F, head});
  frames.frames.push_back(st::ui::anim::Keyframe{1.0F, tail});
  fixture.box->set_animation(animation, frames);

  // t=0：整块下移 40px → 原位置（y=70）是白底，新位置（y=110）是黑。
  fixture.frame(canvas, 1.0);
  ST_CHECK(std::abs(fixture.box->animation_translate().y - 40.0F) <= 0.01F);
  const Color original_spot = pixel(canvas, 80, 55);
  const Color moved_spot = pixel(canvas, 80, 110);
  ST_CHECK(static_cast<int>(original_spot.r) > 240);
  ST_CHECK(static_cast<int>(moved_spot.r) < 12);

  // 结束：回原位（`fill: both` 保留终态 = translate 0）。
  fixture.frame(canvas, 3.0);
  ST_CHECK(std::abs(fixture.box->animation_translate().y) <= 0.01F);
  ST_CHECK(static_cast<int>(pixel(canvas, 80, 55).r) < 12);
}

ST_TEST(animation_scaled_element_still_draws) {
  // 缩放走离屏合成：**最容易出的错是“什么都没画”**（scratch 尺寸取错、
  // 或者合成时用了空矩形）。这里只钉“缩放之后仍然有墨迹且位置大致对”。
  Fixture fixture;
  fixture.layout();
  Canvas canvas = Canvas::for_logical_size(kWidth, kHeight, 1.0F);
  canvas.clear(Color::rgb(255, 255, 255));
  Animation animation{};
  animation.name = "pop";
  animation.duration = 0.2;
  animation.easing = Easing::linear();
  animation.fill = Animation::Fill::Forwards;
  Props head{};
  head.scale = 0.5F;
  Props tail{};
  tail.scale = 1.0F;
  Keyframes frames{};
  frames.frames.push_back(st::ui::anim::Keyframe{0.0F, head});
  frames.frames.push_back(st::ui::anim::Keyframe{1.0F, tail});
  fixture.box->set_animation(animation, frames);
  fixture.frame(canvas, 1.0);
  ST_CHECK(std::abs(fixture.box->animation_scale() - 0.5F) <= 0.01F);
  // 中心必然还在（缩放以 bounds 中心为基准）。
  const Color center = pixel(canvas, 90, 70);
  ST_CHECK(static_cast<int>(center.r) < 40);
  // 结束：回到 1.0 且仍然画出来。
  fixture.frame(canvas, 2.0);
  ST_CHECK(std::abs(fixture.box->animation_scale() - 1.0F) <= 0.01F);
  ST_CHECK(static_cast<int>(pixel(canvas, 45, 45).r) < 40);
}

ST_TEST(animation_stops_requesting_frames_when_finished) {
  // **防空转判据**：播完必须不再请求续帧。
  // 历史上悬停过渡踩过一模一样的坑（静止元素每帧把自己标 animating →
  // 应用 100% 占一个核，实测 6 秒耗 6.12 秒 CPU）。
  Fixture fixture;
  fixture.layout();
  Canvas canvas = Canvas::for_logical_size(kWidth, kHeight, 1.0F);
  ST_CHECK(fixture.box->set_preset_animation("fadeInUp"));
  ST_CHECK(fixture.box->set_preset_animation("modalIn"));
  ST_CHECK(!fixture.box->set_preset_animation("no-such-preset"));
  fixture.frame(canvas, 1.0);
  ST_CHECK(fixture.box->animation_running());
  fixture.frame(canvas, 10.0);   // 远超任何预设时长
  ST_CHECK(!fixture.box->animation_running());
  // 再画一帧：根元素不该再要求帧。
  fixture.frame(canvas, 11.0);
  ST_CHECK(!fixture.box->animation_running());
  ST_CHECK(!fixture.root.needs_frame());
}

ST_TEST(animation_reduced_motion_jumps_to_end) {
  // 无障碍：`reduced_motion` 时不播中间帧，直接落终态。
  Fixture fixture;
  fixture.layout();
  Canvas canvas = Canvas::for_logical_size(kWidth, kHeight, 1.0F);
  canvas.clear(Color::rgb(255, 255, 255));
  Animation animation{};
  animation.name = "fade";
  animation.duration = 5.0;   // 故意很长：若不减弱，5s 内都不会结束
  animation.easing = Easing::linear();
  animation.fill = Animation::Fill::Forwards;
  Props head{};
  head.opacity = 0.0F;
  Props tail{};
  tail.opacity = 1.0F;
  Keyframes frames{};
  frames.frames.push_back(st::ui::anim::Keyframe{0.0F, head});
  frames.frames.push_back(st::ui::anim::Keyframe{1.0F, tail});
  fixture.box->set_animation(animation, frames);
  // 用 reduced_motion 的 context 画一帧：终态（opacity=1）且不续帧。
  st::ui::RenderContext reduced{fixture.theme, nullptr, 1.0};
  reduced.reduced_motion = true;
  fixture.box->paint(reduced, canvas);
  fixture.root.clear_dirty();
  ST_CHECK(!fixture.box->animation_running());
  const auto opacity = fixture.box->get_property("animation_opacity");
  ST_CHECK(opacity.has_value());
  ST_CHECK(std::abs(std::stod(*opacity) - 1.0) <= 0.01);
  ST_CHECK(static_cast<int>(pixel(canvas, 80, 70).r) < 12);
}

ST_TEST(animation_transition_interpolates_style_changes) {
  // 过渡：值变化时补间过去，而不是瞬变。
  Fixture fixture;
  fixture.layout();
  Canvas canvas = Canvas::for_logical_size(kWidth, kHeight, 1.0F);
  canvas.clear(Color::rgb(255, 255, 255));
  st::ui::anim::Transition transition{};
  transition.duration = 1.0;
  transition.easing = Easing::linear();
  transition.properties = {Property::Background};
  fixture.box->set_transition(transition);

  // 先画一帧记基准（opacity=1 的黑块）。
  fixture.frame(canvas, 1.0);
  ST_CHECK(static_cast<int>(pixel(canvas, 80, 70).r) < 12);

  // 改样式 → 下一帧开始过渡：半程必须是中间灰，而不是立刻变成白（瞬变）。
  //
  // ⚠ 改完必须 `mark_dirty()`：`paint_frame` 的**空帧闸门**（损坏区空 + 无续帧
  // + 无到期唤醒 → 不画）会让“悄悄改了样式却没标脏”的元素根本不进 `paint`，
  // 于是过渡也就永远开不了头。真实路径上这不会漏——`apply_theme` 每帧标脏、
  // 交互态变化也都标脏——但测试直接摸 `style()` 时要自己补上。
  fixture.box->style().background = Color::rgb(255, 255, 255);
  fixture.box->mark_dirty();
  fixture.frame(canvas, 1.0);
  fixture.frame(canvas, 1.5);
  ST_CHECK(fixture.box->transition_active());
  const Color mid = pixel(canvas, 80, 70);
  ST_CHECK(static_cast<int>(mid.r) > 90);
  ST_CHECK(static_cast<int>(mid.r) < 190);

  // 结束：完全变白，且不再续帧。
  fixture.frame(canvas, 3.0);
  ST_CHECK(!fixture.box->transition_active());
  ST_CHECK(static_cast<int>(pixel(canvas, 80, 70).r) > 240);
}

// ────────────────────────────────────────────────────────────────────────────
// 平移绘制面（`raster::TranslatedSurface`）
// ────────────────────────────────────────────────────────────────────────────

ST_TEST(animation_translated_surface_is_pixel_equivalent_to_manual_offset) {
  // **判据**：同一组绘制用 `TranslatedSurface` 画，与“手工把所有坐标加偏移”
  // 必须**逐像素相同**。这条等价式是平移包装唯一的正确性依据——
  // 若某个原语漏了平移，它会在这里露出来（而不是靠肉眼发现“有一类东西没动”）。
  Canvas reference = Canvas::for_logical_size(160, 120, 1.0F);
  Canvas shifted = Canvas::for_logical_size(160, 120, 1.0F);
  reference.clear(Color::rgb(255, 255, 255));
  shifted.clear(Color::rgb(255, 255, 255));
  const Color ink = Color::rgb(20, 40, 80);
  const Point offset{17.0F, -9.0F};
  for (Canvas* target : {&reference, &shifted}) {
    const bool manual = target == &reference;
    const Point shift = manual ? offset : Point{};
    target->fill_rect(Rect{10.0F + shift.x, 20.0F + shift.y, 40.0F, 30.0F},
                      st::raster::Paint::solid(ink));
    target->fill_rect(Rect{60.0F + shift.x, 40.0F + shift.y, 20.0F, 20.0F},
                      st::raster::Paint::solid(ink), 6.0F);
    target->fill_circle(Point{30.0F + shift.x, 80.0F + shift.y}, 8.0F,
                        st::raster::Paint::solid(ink));
    target->push_clip_rounded_rect(Rect{0.0F + shift.x, 0.0F + shift.y, 100.0F, 100.0F}, 4.0F);
    target->fill_rect(Rect{80.0F + shift.x, 10.0F + shift.y, 30.0F, 30.0F},
                      st::raster::Paint::solid(ink));
    target->pop_clip();
  }
  // 第二遍：用包装重画 shifted（与上面“手工加偏移”的结果必须一致）。
  shifted.clear(Color::rgb(255, 255, 255));
  {
    st::raster::TranslatedSurface wrapper(shifted, offset);
    wrapper.fill_rect(Rect{10.0F, 20.0F, 40.0F, 30.0F}, st::raster::Paint::solid(ink));
    wrapper.fill_rect(Rect{60.0F, 40.0F, 20.0F, 20.0F}, st::raster::Paint::solid(ink), 6.0F);
    wrapper.fill_circle(Point{30.0F, 80.0F}, 8.0F, st::raster::Paint::solid(ink));
    wrapper.push_clip_rounded_rect(Rect{0.0F, 0.0F, 100.0F, 100.0F}, 4.0F);
    wrapper.fill_rect(Rect{80.0F, 10.0F, 30.0F, 30.0F}, st::raster::Paint::solid(ink));
    wrapper.pop_clip();
  }
  int differences = 0;
  for (int y = 0; y < 120; ++y) {
    for (int x = 0; x < 160; ++x) {
      if (!(reference.pixel_at(x, y) == shifted.pixel_at(x, y))) ++differences;
    }
  }
  ST_CHECK_EQ(differences, 0);
}

ST_TEST(animation_translated_surface_compensates_clip_rect) {
  // `clip_rect()` 必须**反向补偿**：调用方（元素的视口剔除）拿它和未平移的
  // `bounds_` 比。返回平移后的域会让“滑入的元素里有一半不显示”。
  Canvas canvas = Canvas::for_logical_size(200, 100, 1.0F);
  const Point offset{25.0F, 10.0F};
  st::raster::TranslatedSurface wrapper(canvas, offset);
  const auto rect = wrapper.clip_rect();
  ST_CHECK_EQ(rect.x, -25);
  ST_CHECK_EQ(rect.y, -10);
}

}  // namespace

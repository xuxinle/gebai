#pragma once

/// **组件动画引擎**（CSS 式）：缓动 + 关键帧 + 过渡，与元素的生命周期解耦。
///
/// ## 为什么要一层统一引擎（此前是每个组件各写一套）
///
/// `BACKLOG` 里记着「统一动画系统（现每组件手工状态机）」这笔账：`Toggle` 的
/// 滑块位移、`Tabs` 的指示条、`ProgressRing` 的扫掠、`Skeleton` 的呼吸、`StatusDot`
/// 的脉冲，各自在 `paint_content` 里按 `context.time_seconds` 算相位、各自写
/// 「还在动就 `request_animation()`」、各自定义时长与曲线。后果与缓动曲线当年
/// 遇到的问题完全同构：
///
/// 1. **换不掉**：想把全应用动效统一调慢/换手感，得逐个组件改；
/// 2. **会漂**：同一语义（“淡入 180ms 缓出”）在两处写法略有不同，看片时就会
///    出现“这个卡片和那个卡片节奏不一样”；
/// 3. **不可复用**：应用想要“列表项依次淡入”这类效果，只能自己起定时器 + 自己插值，
///    而定时器一挂上去就与框架的续帧协议（`request_animation`/`request_wake_at`）
///    两套并存——CPU 空转的老坑（2026-10-29 实测 6 秒耗 6.12 秒）很容易重踩。
///
/// 本层只做**纯函数**：给「时间 + 缓动 + 关键帧」，算出「这一刻的属性值」。
/// 元素的运行态（起点时间、是否还在动、续帧请求）由 `Element` 管，
/// 绘制落点由 `Element::paint_box`/`paint_text`/`paint_impl` 消费。
///
/// ## 语义对齐 CSS 的程度（如实列全）
///
/// | CSS | 本层 | 备注 |
/// |---|---|---|
/// | `linear` / `ease` / `ease-in` / `ease-out` / `ease-in-out` | ✅ | 前四个是 `cubic-bezier` 的具名简写 |
/// | `cubic-bezier(x1,y1,x2,y2)` | ✅ | 牛顿迭代反解 x→t（见 `cubic_bezier`） |
/// | `steps(n, jump-*)` | ✅ | `jump-end`（默认）/ `jump-start` / `jump-none` |
/// | `@keyframes` + 百分比关键帧 | ✅ | `from`/`to`/`0%`…`100%`；段间用同一缓动 |
/// | `animation-delay` | ✅ | 负延迟按 CSS 语义（从动画中途开始） |
/// | `animation-iteration-count`（数字 / `infinite`） | ✅ | |
/// | `animation-direction`（normal/reverse/alternate/alternate-reverse） | ✅ | |
/// | `animation-fill-mode`（none/forwards/backwards/both） | ✅ | |
/// | `transition`（属性值变化时补间） | ✅ | 见 `Element::set_transition` |
/// | `animation-timing-function` **逐关键帧** | ❌ | 段间统一。CSS 允许每帧各带曲线，但那需要“段→曲线”映射与 `from` 缺省推导，需求未出现前不预加 |
/// | 动画事件的 JS 回调（`animationend`） | ❌ | 无脚本事件面；需要时读 `Element::animation_running()` |

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/color.hpp"
#include "st/math/geometry.hpp"

namespace st::ui {
namespace anim {

// ────────────────────────────────────────────────────────────────────────────
// 缓动
// ────────────────────────────────────────────────────────────────────────────

/// 缓动函数族。
///
/// **具名曲线是对齐 CSS 的四条**（`ease` = `(0.25,0.1,0.25,1)` 等），不是自创形状——
/// 这样从设计稿/网页抄过来的 `transition: all .2s ease` 能一比一落地，
/// 而不需要“翻译成框架的某个很像的曲线”。
enum class EasingKind : std::uint8_t {
  Linear,
  Ease,
  EaseIn,
  EaseOut,
  EaseInOut,
  CubicBezier,
  Steps,
};

/// `steps()` 的跳变位置。
enum class StepPosition : std::uint8_t {
  JumpEnd,    ///< 默认（`steps(4)`）：起点在 0，终点前跳到 1
  JumpStart,  ///< 起点立即跳到 1/n（`steps(4, jump-start)`）
  JumpNone,   ///< 两端都不跳（需 n ≥ 2）
};

struct Easing {
  EasingKind kind{EasingKind::Linear};
  /// `CubicBezier` 的控制点（x 必须在 [0,1]；y 不限，CSS 允许回弹）。
  float x1{0.25f};
  float y1{0.1f};
  float x2{0.25f};
  float y2{1.0f};
  /// `Steps` 的段数（< 1 时按 1 处理）。
  int steps{1};
  StepPosition step_position{StepPosition::JumpEnd};

  [[nodiscard]] static constexpr auto linear() noexcept -> Easing { return {}; }
  [[nodiscard]] static constexpr auto ease() noexcept -> Easing {
    return Easing{EasingKind::Ease, 0.25f, 0.1f, 0.25f, 1.0f, 1, StepPosition::JumpEnd};
  }
  [[nodiscard]] static constexpr auto ease_in() noexcept -> Easing {
    return Easing{EasingKind::EaseIn, 0.42f, 0.0f, 1.0f, 1.0f, 1, StepPosition::JumpEnd};
  }
  [[nodiscard]] static constexpr auto ease_out() noexcept -> Easing {
    return Easing{EasingKind::EaseOut, 0.0f, 0.0f, 0.58f, 1.0f, 1, StepPosition::JumpEnd};
  }
  [[nodiscard]] static constexpr auto ease_in_out() noexcept -> Easing {
    return Easing{EasingKind::EaseInOut, 0.42f, 0.0f, 0.58f, 1.0f, 1, StepPosition::JumpEnd};
  }
  /// 三次贝塞尔（`cubic-bezier(x1,y1,x2,y2)`）。
  [[nodiscard]] static constexpr auto bezier(float x1, float y1, float x2, float y2) noexcept
      -> Easing {
    return Easing{EasingKind::CubicBezier, x1, y1, x2, y2, 1, StepPosition::JumpEnd};
  }
  /// 阶跃（`steps(n, position)`）。
  [[nodiscard]] static constexpr auto step(int count, StepPosition position = StepPosition::JumpEnd)
      noexcept -> Easing {
    return Easing{EasingKind::Steps, 0.25f, 0.1f, 0.25f, 1.0f, count, position};
  }
};

/// 采样缓动：输入进度 `t`（会被夹到 `[0,1]`），输出缓动后的进度。
///
/// 输出**不夹取**：`cubic-bezier` 的 y 可以越界（CSS 同语义，回弹曲线靠它），
/// 把输出夹回 `[0,1]` 会让 `cubic-bezier(.34,1.56,.64,1)` 这类曲线退化成缓出。
/// 需要“值域必须在 [0,1]”的调用方（如 `opacity`）自己在落点处夹。
///
/// `t = 0` 与 `t = 1` 必须精确返回 0 / 1（哪怕是贝塞尔）：否则动画首帧会跳一下、
/// 末帧会差一点点，而“差一点点”在 `opacity` 上表现为常驻的 1/255 灰。
[[nodiscard]] auto sample(const Easing& easing, float t) noexcept -> float;

/// 具名解析（`"ease-out"` / `"cubic-bezier(...)"` / `"steps(4, jump-start)"`）。
/// 认不出返回 `nullopt`（**不静默当线性**：那会让“写错了”表现为“动画没效果也很正常”）。
[[nodiscard]] auto parse_easing(std::string_view text) -> std::optional<Easing>;
/// 稳定短名（控制通道属性面/诊断用）。
[[nodiscard]] auto easing_name(const Easing& easing) -> std::string;

// ────────────────────────────────────────────────────────────────────────────
// 可动画属性
// ────────────────────────────────────────────────────────────────────────────

/// **可动画的属性集合**（每一项都是“能连续插值、且落点真的会读它”的）。
///
/// 刻意**不含布局属性**（`width`/`padding`/`gap`）：动画布局会每帧触发重排，
/// 而重排会把整帧重绘（增量重绘的早退条件之一就是“没有布局脏”），
/// 一帧几十毫秒的代价换一点点视觉收益不值。位移/缩放由 `translate`/`scale`
/// 在**绘制期**表达（不动 `bounds_`，因此重排、命中、语义树都与静态时一致）。
enum class Property : std::uint8_t {
  Opacity,
  Background,
  BorderColor,
  Color,
  Radius,
  BorderWidth,
  FontSize,
  TranslateX,
  TranslateY,
  Scale,
  Count,
};

/// 属性名（CSS 口径）；未知名返回 `nullopt`。
[[nodiscard]] auto parse_property(std::string_view text) -> std::optional<Property>;
[[nodiscard]] auto property_name(Property property) -> std::string_view;

/// 一组**部分声明**的动画属性（缺省 = 本帧不动这一项）。
///
/// 用 `std::optional` 逐项表达“没声明”，而不是“声明了默认值”：关键帧
/// `from {opacity:0}` 只该动 opacity，其余属性必须沿用元素自己的样式——
/// 若给每个字段一个确定初值，`from` 帧会把背景色也一起冲成默认色。
struct Props {
  std::optional<float> opacity{};
  std::optional<math::Color> background{};
  std::optional<math::Color> border_color{};
  std::optional<math::Color> color{};
  std::optional<float> radius{};
  std::optional<float> border_width{};
  std::optional<float> font_size{};
  std::optional<float> translate_x{};
  std::optional<float> translate_y{};
  std::optional<float> scale{};

  [[nodiscard]] auto has(Property property) const noexcept -> bool;
  [[nodiscard]] auto empty() const noexcept -> bool;
};

/// `a → b` 按 `t` 插值；**只有两边都声明的属性**才出现在结果里
/// （单边声明时用那一边的值，因为“另一半”的语义是元素自己的样式，本层不知道）。
[[nodiscard]] auto lerp_props(const Props& a, const Props& b, float t) -> Props;

/// 把 `over` 里声明的属性覆盖到 `base` 上（其余保持 `base`）。
[[nodiscard]] auto overlay_props(Props base, const Props& over) -> Props;

/// 两个 `Props` 是否逐字段相等（判断“过渡是否已到位”用）。
[[nodiscard]] auto props_equal(const Props& a, const Props& b) noexcept -> bool;

// ────────────────────────────────────────────────────────────────────────────
// 关键帧动画
// ────────────────────────────────────────────────────────────────────────────

/// 一个关键帧（`offset ∈ [0,1]`）。
struct Keyframe {
  float offset{0.0f};
  Props props{};
};

/// 一组关键帧（`@keyframes name { … }`）。
///
/// `frames` 按 `offset` 升序保存（构造时排序，见 `Keyframes::normalized()`）：
/// 采样是**二分/线性扫描**，乱序会取到错的区间，而 CSS 作者写 `to` 在前的顺序
/// 并不违法。
struct Keyframes {
  std::string name{};
  std::vector<Keyframe> frames{};

  /// 排序 + 去重 + 补齐端点：
  /// - 只有一个关键帧时补一个同值帧（否则“动画”等于常量）；
  /// - 首个 `offset > 0` 时补 `0`（值取第一个帧，等价于 CSS 的“缺省从元素自身样式来”，
  ///   但这里没有元素上下文，退化为常量起点——比“从元素样式补间到首帧”更可预测）；
  /// - 末尾 `offset < 1` 时补 `1`。
  [[nodiscard]] auto normalized() const -> Keyframes;
  [[nodiscard]] auto empty() const noexcept -> bool { return frames.empty(); }
};

/// 一个动画的**播放参数**（`animation` 简写的结构化形式）。
struct Animation {
  /// 关键帧名（`""` = 没有动画）。
  std::string name{};
  /// 单次循环时长（秒；`<= 0` = 立即落终态）。
  double duration{0.0};
  /// 起始延迟（秒）。**负值合法**：等价于从动画中途开始（CSS 同语义），
  /// 常见用法是“入场动画别等，直接给个 -0.2s 让它看起来已经在播”。
  double delay{0.0};
  /// 循环次数；`<= 0` 表示无限循环（CSS `infinite`）。
  int iterations{1};
  Easing easing{Easing::ease()};

  enum class Direction : std::uint8_t { Normal, Reverse, Alternate, AlternateReverse };
  enum class Fill : std::uint8_t { None, Forwards, Backwards, Both };

  Direction direction{Direction::Normal};
  Fill fill{Fill::None};

  [[nodiscard]] auto infinite() const noexcept -> bool { return iterations <= 0; }
  [[nodiscard]] auto empty() const noexcept -> bool { return name.empty(); }
};

/// 一次采样结果。
struct Sample {
  /// 本帧的动画属性（已按 direction / fill 决定）。
  /// 空 = 本帧不该施加任何覆盖（如 `fill: none` 且已结束）。
  Props props{};
  /// 是否仍在进行（调用方据此续帧）。
  bool running{false};
};

/// 按**已经过的墙钟秒数**采样一个动画。
///
/// 纯函数：不给元素、不看 `style_`。`elapsed` 从动画**开始那一刻**算起
/// （由调用方减起点），负值表示还在延迟里。
///
/// 语义要点（都对齐 CSS）：
/// - 延迟期内：`fill: backwards/both` 时返回首帧值，否则**不施加覆盖**；
/// - 结束后：`fill: forwards/both` 时返回末帧值，否则不施加覆盖；
/// - `alternate`：奇数次循环反向（`alternate-reverse` 的第一次就反向）；
/// - 无限循环永不结束，`running` 恒真。
[[nodiscard]] auto sample_animation(const Keyframes& keyframes, const Animation& animation,
                                    double elapsed) -> Sample;

/// 采样到的整体进度（0..1，跨循环的“本次循环内进度”；诊断/测试用）。
[[nodiscard]] auto animation_progress(const Animation& animation, double elapsed) -> float;

// ────────────────────────────────────────────────────────────────────────────
// 过渡（transition）
// ────────────────────────────────────────────────────────────────────────────

/// 一条过渡：某些属性的值**变化时**补间过去，而不是瞬变。
struct Transition {
  /// 时长（秒；`<= 0` = 不补间，直接落位）。
  double duration{0.0};
  double delay{0.0};
  Easing easing{Easing::ease()};
  /// 作用的属性集合（空 = 全部 `Property`；与 CSS `transition: all` 同义）。
  std::vector<Property> properties{};

  [[nodiscard]] auto affects(Property property) const noexcept -> bool;
  [[nodiscard]] auto active() const noexcept -> bool { return duration > 0.0; }
};

/// 一条过渡的插值进度（0..1；`elapsed` 从值变化那一刻算起）。
[[nodiscard]] auto transition_progress(const Transition& transition, double elapsed) noexcept
    -> float;

// ────────────────────────────────────────────────────────────────────────────
// 预设（`@keyframes` 的内置集合）
// ────────────────────────────────────────────────────────────────────────────

/// 预设名 → 关键帧。**名字与常见 CSS 类库一致**（`fadeInUp` 是全世界用得最多
/// 的入场动画；`modalIn`/`shake`/`pulse` 同理），免得使用者自己翻译。
///
/// | 名字 | 用途 |
/// |---|---|
/// | `fadeIn` | 纯淡入（0.3s ease-out） |
/// | `fadeInUp` | 淡入 + 上移 24px（首屏/卡片依次出现） |
/// | `fadeOut` | 淡出 |
/// | `modalIn` | 弹窗进场：淡入 + 从 0.96 放大 + 上移 8px |
/// | `slideInLeft` / `slideInRight` | 抽屉/侧栏滑入（位移 + 淡入） |
/// | `pulse` | 呼吸（透明度的轻微起伏；无限循环） |
/// | `shake` | 抖动（表单校验失败；左右 6px 来回） |
/// | `spin` | 旋转 → **不支持**：本层没有旋转变换（`Surface` 无仿射原语），
///   旋转动画由组件自绘（`Spinner` 画弧段相位）表达。名字保留为不存在的预设，
///   查到空关键帧时如实返回，调用方据此回退。 |
[[nodiscard]] auto preset_keyframes(std::string_view name) -> std::optional<Keyframes>;
/// 预设名清单（`preset_keyframes` 能查到的全部名字）。
[[nodiscard]] auto preset_names() -> const std::vector<std::string_view>&;
/// 预设的**完整播放参数**（时长/循环次数/缓动也一并给好）：
/// 调用方只需 `set_animation(preset_animation("fadeInUp"))`。
///
/// 为什么时长也由预设定：入场动画的节奏是**成组设计**的（0.3~0.6s、`ease-out`、
/// 依次错开），让调用方各自传时长会立刻出现“同一屏里两个卡片节奏不同”。
[[nodiscard]] auto preset_animation(std::string_view name) -> std::optional<Animation>;

}  // namespace anim
}  // namespace st::ui

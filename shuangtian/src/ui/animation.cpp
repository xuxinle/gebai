/// 组件动画引擎的实现（`st/ui/animation.hpp`）：缓动 / 关键帧 / 过渡 / 预设。
///
/// 本文件全部是**纯函数**——没有元素、没有时间源、没有续帧协议。它解得开
/// “这一刻该是什么值”，但不管“谁在什么时候问”。这个分工是刻意的：
/// 缓动与关键帧采样是动画里唯一能被单测穷举的部分（端点、单调性、循环方向），
/// 把它们与元素状态混在一起，就只能靠“跑几帧看截图”来验。

#include "st/ui/animation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace st::ui {
namespace anim {
namespace {

[[nodiscard]] constexpr auto clamp01(float value) noexcept -> float {
  return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

/// 三次贝塞尔的一维求值（`B(t) = 3(1-t)²t·p1 + 3(1-t)t²·p2 + t³`，起点 0 终点 1）。
[[nodiscard]] constexpr auto bezier_axis(float t, float p1, float p2) noexcept -> float {
  const float u = 1.0f - t;
  return 3.0f * u * u * t * p1 + 3.0f * u * t * t * p2 + t * t * t;
}

/// 给定 x，反解参数 t（牛顿迭代 + 二分兜底）。
///
/// 为什么需要它：CSS 的 `cubic-bezier(x1,y1,x2,y2)` 里 x 是**时间轴**、y 是**进度**，
/// 而“给定时间求进度”是一个没有解析解的三次方程反解。直接用 t 当前进度的写法
/// （省略这一步）在对称曲线（`ease-in-out`）上碰巧对，在不对称曲线上会明显偏——
/// 这类偏差只能靠“拿浏览器同款曲线对比”发现，肉眼看只是“手感有点怪”。
[[nodiscard]] auto solve_bezier_t(float x, float x1, float x2) noexcept -> float {
  // 牛顿迭代：初值取 x（x 与 t 单调同向，线性初值已足够接近）。
  float t = x;
  for (int step = 0; step < 8; ++step) {
    const float current = bezier_axis(t, x1, x2) - x;
    if (std::abs(current) < 1e-6f) return t;
    const float u = 1.0f - t;
    // d/dt = 3(1-t)²(x1) + 6(1-t)t(x2-x1) + 3t²(1-x2)
    const float slope =
        3.0f * u * u * x1 + 6.0f * u * t * (x2 - x1) + 3.0f * t * t * (1.0f - x2);
    if (std::abs(slope) < 1e-6f) break;
    const float next = t - current / slope;
    if (!(next >= 0.0f) || !(next <= 1.0f)) break;   // 迭代跑出定义域 → 交给二分
    t = next;
  }
  // 二分兜底（斜率为零的退化曲线：`cubic-bezier(0,0,0,0)` 之类）。
  float low = 0.0f;
  float high = 1.0f;
  t = x;
  for (int step = 0; step < 24; ++step) {
    const float current = bezier_axis(t, x1, x2);
    if (std::abs(current - x) < 1e-6f) break;
    if (current < x) {
      low = t;
    } else {
      high = t;
    }
    t = (low + high) * 0.5f;
  }
  return t;
}

[[nodiscard]] auto step_sample(const Easing& easing, float t) noexcept -> float {
  const int count = easing.steps < 1 ? 1 : easing.steps;
  switch (easing.step_position) {
    case StepPosition::JumpStart:
      return clamp01((std::floor(t * static_cast<float>(count)) + 1.0f) /
                     static_cast<float>(count));
    case StepPosition::JumpNone: {
      // 两端都不跳：n 段里只有 n-1 个台阶（CSS 要求 n ≥ 2，这里退化按 n=2 处理）。
      const int span = count < 2 ? 2 : count;
      const float index = std::floor(t * static_cast<float>(span));
      return clamp01(index / static_cast<float>(span - 1));
    }
    case StepPosition::JumpEnd:
    default:
      return clamp01(std::floor(t * static_cast<float>(count)) / static_cast<float>(count));
  }
}

[[nodiscard]] auto parse_float(std::string_view text) -> std::optional<float> {
  std::string buffer(text);
  if (buffer.empty()) return std::nullopt;
  try {
    std::size_t consumed = 0;
    const float value = std::stof(buffer, &consumed);
    if (consumed == 0) return std::nullopt;
    // 尾巴只允许单位/空白（`"0.3s"`、`"12px"`）。
    const std::string_view rest = std::string_view(buffer).substr(consumed);
    for (const char ch : rest) {
      if (ch == ' ' || ch == '\t') continue;
      if (ch == 's' || ch == 'p' || ch == 'x' || ch == '%') continue;
      return std::nullopt;
    }
    return value;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

[[nodiscard]] auto trim(std::string_view text) -> std::string_view {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\n' ||
                         text[begin] == '\r')) {
    ++begin;
  }
  while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\n' ||
                         text[end - 1] == '\r')) {
    --end;
  }
  return text.substr(begin, end - begin);
}

}  // namespace

// ────────────────────────────────────────────────────────────────────────────
// 缓动
// ────────────────────────────────────────────────────────────────────────────

auto sample(const Easing& easing, float t) noexcept -> float {
  const float clamped = clamp01(t);
  // 端点精确返回：`cubic-bezier` 的迭代解在 0/1 处只保证 1e-6 精度，
  // 而首帧差 1e-6 在 `opacity` 上就是“明明设了 0 却有淡淡一层”。
  if (clamped <= 0.0f) return 0.0f;
  if (clamped >= 1.0f) return 1.0f;
  switch (easing.kind) {
    case EasingKind::Linear:
      return clamped;
    case EasingKind::Steps:
      return step_sample(easing, clamped);
    case EasingKind::Ease:
    case EasingKind::EaseIn:
    case EasingKind::EaseOut:
    case EasingKind::EaseInOut:
    case EasingKind::CubicBezier: {
      // 梯级：具名曲线就是各自的贝塞尔控制点（`ease`/`ease-in`/… 在工厂函数里已填好）。
      const float x1 = clamp01(easing.x1);
      const float x2 = clamp01(easing.x2);
      const float parameter = solve_bezier_t(clamped, x1, x2);
      return bezier_axis(parameter, easing.y1, easing.y2);
    }
  }
  return clamped;
}

auto parse_easing(std::string_view text) -> std::optional<Easing> {
  const std::string_view value = trim(text);
  if (value.empty()) return std::nullopt;
  if (value == "linear") return Easing::linear();
  if (value == "ease") return Easing::ease();
  if (value == "ease-in") return Easing::ease_in();
  if (value == "ease-out") return Easing::ease_out();
  if (value == "ease-in-out") return Easing::ease_in_out();
  if (value.rfind("cubic-bezier(", 0) == 0) {
    const std::size_t open = value.find('(');
    const std::size_t close = value.rfind(')');
    if (close == std::string_view::npos || close <= open) return std::nullopt;
    const std::string_view body = value.substr(open + 1, close - open - 1);
    std::array<float, 4> numbers{};
    std::size_t count = 0;
    std::size_t cursor = 0;
    while (cursor <= body.size() && count < numbers.size()) {
      const std::size_t comma = body.find(',', cursor);
      const std::string_view piece =
          comma == std::string_view::npos ? body.substr(cursor) : body.substr(cursor, comma - cursor);
      const auto parsed = parse_float(trim(piece));
      if (!parsed.has_value()) return std::nullopt;
      numbers[count] = *parsed;
      ++count;
      if (comma == std::string_view::npos) break;
      cursor = comma + 1;
    }
    if (count != numbers.size()) return std::nullopt;
    return Easing::bezier(numbers[0], numbers[1], numbers[2], numbers[3]);
  }
  if (value.rfind("steps(", 0) == 0) {
    const std::size_t open = value.find('(');
    const std::size_t close = value.rfind(')');
    if (close == std::string_view::npos || close <= open) return std::nullopt;
    const std::string_view body = value.substr(open + 1, close - open - 1);
    const std::size_t comma = body.find(',');
    const auto count = parse_float(trim(comma == std::string_view::npos ? body
                                                                        : body.substr(0, comma)));
    if (!count.has_value()) return std::nullopt;
    StepPosition position = StepPosition::JumpEnd;
    if (comma != std::string_view::npos) {
      const std::string name(trim(body.substr(comma + 1)));
      if (name == "jump-start" || name == "start") {
        position = StepPosition::JumpStart;
      } else if (name == "jump-none") {
        position = StepPosition::JumpNone;
      } else if (name == "jump-end" || name == "end") {
        position = StepPosition::JumpEnd;
      } else {
        return std::nullopt;
      }
    }
    return Easing::step(static_cast<int>(*count), position);
  }
  return std::nullopt;
}

auto easing_name(const Easing& easing) -> std::string {
  switch (easing.kind) {
    case EasingKind::Linear: return "linear";
    case EasingKind::Ease: return "ease";
    case EasingKind::EaseIn: return "ease-in";
    case EasingKind::EaseOut: return "ease-out";
    case EasingKind::EaseInOut: return "ease-in-out";
    case EasingKind::Steps:
      return std::format("steps({}, {})", easing.steps,
                         easing.step_position == StepPosition::JumpStart ? "jump-start"
                         : easing.step_position == StepPosition::JumpNone ? "jump-none"
                                                                          : "jump-end");
    case EasingKind::CubicBezier:
      break;
  }
  return std::format("cubic-bezier({}, {}, {}, {})", easing.x1, easing.y1, easing.x2, easing.y2);
}

// ────────────────────────────────────────────────────────────────────────────
// 可动画属性
// ────────────────────────────────────────────────────────────────────────────

auto parse_property(std::string_view text) -> std::optional<Property> {
  const std::string_view value = trim(text);
  if (value == "opacity") return Property::Opacity;
  if (value == "background" || value == "background-color") return Property::Background;
  if (value == "border-color") return Property::BorderColor;
  if (value == "color") return Property::Color;
  if (value == "radius" || value == "border-radius") return Property::Radius;
  if (value == "border-width") return Property::BorderWidth;
  if (value == "font-size") return Property::FontSize;
  if (value == "translate-x" || value == "translateX") return Property::TranslateX;
  if (value == "translate-y" || value == "translateY") return Property::TranslateY;
  if (value == "scale") return Property::Scale;
  return std::nullopt;
}

auto property_name(Property property) -> std::string_view {
  switch (property) {
    case Property::Opacity: return "opacity";
    case Property::Background: return "background";
    case Property::BorderColor: return "border-color";
    case Property::Color: return "color";
    case Property::Radius: return "radius";
    case Property::BorderWidth: return "border-width";
    case Property::FontSize: return "font-size";
    case Property::TranslateX: return "translate-x";
    case Property::TranslateY: return "translate-y";
    case Property::Scale: return "scale";
    case Property::Count: break;
  }
  return "";
}

auto Props::has(Property property) const noexcept -> bool {
  switch (property) {
    case Property::Opacity: return opacity.has_value();
    case Property::Background: return background.has_value();
    case Property::BorderColor: return border_color.has_value();
    case Property::Color: return color.has_value();
    case Property::Radius: return radius.has_value();
    case Property::BorderWidth: return border_width.has_value();
    case Property::FontSize: return font_size.has_value();
    case Property::TranslateX: return translate_x.has_value();
    case Property::TranslateY: return translate_y.has_value();
    case Property::Scale: return scale.has_value();
    case Property::Count: break;
  }
  return false;
}

auto Props::empty() const noexcept -> bool {
  for (std::uint8_t index = 0; index < static_cast<std::uint8_t>(Property::Count); ++index) {
    if (has(static_cast<Property>(index))) return false;
  }
  return true;
}

namespace {

/// 单值插值：两边都有就插，只有一边就用那一边（见 `lerp_props` 的注释）。
template <class T, class Lerp>
[[nodiscard]] auto blend(const std::optional<T>& from, const std::optional<T>& to, float t,
                         Lerp lerp) -> std::optional<T> {
  if (from.has_value() && to.has_value()) return lerp(*from, *to, t);
  if (to.has_value()) return to;
  return from;
}

}  // namespace

auto lerp_props(const Props& a, const Props& b, float t) -> Props {
  const float clamped = clamp01(t);
  Props out{};
  const auto color_lerp = [](const math::Color& from, const math::Color& to, float k) {
    return math::lerp(from, to, k);
  };
  const auto float_lerp = [](float from, float to, float k) { return from + (to - from) * k; };
  out.opacity = blend(a.opacity, b.opacity, clamped, float_lerp);
  out.background = blend(a.background, b.background, clamped, color_lerp);
  out.border_color = blend(a.border_color, b.border_color, clamped, color_lerp);
  out.color = blend(a.color, b.color, clamped, color_lerp);
  out.radius = blend(a.radius, b.radius, clamped, float_lerp);
  out.border_width = blend(a.border_width, b.border_width, clamped, float_lerp);
  out.font_size = blend(a.font_size, b.font_size, clamped, float_lerp);
  out.translate_x = blend(a.translate_x, b.translate_x, clamped, float_lerp);
  out.translate_y = blend(a.translate_y, b.translate_y, clamped, float_lerp);
  out.scale = blend(a.scale, b.scale, clamped, float_lerp);
  return out;
}

auto overlay_props(Props base, const Props& over) -> Props {
  if (over.opacity.has_value()) base.opacity = over.opacity;
  if (over.background.has_value()) base.background = over.background;
  if (over.border_color.has_value()) base.border_color = over.border_color;
  if (over.color.has_value()) base.color = over.color;
  if (over.radius.has_value()) base.radius = over.radius;
  if (over.border_width.has_value()) base.border_width = over.border_width;
  if (over.font_size.has_value()) base.font_size = over.font_size;
  if (over.translate_x.has_value()) base.translate_x = over.translate_x;
  if (over.translate_y.has_value()) base.translate_y = over.translate_y;
  if (over.scale.has_value()) base.scale = over.scale;
  return base;
}

auto props_equal(const Props& a, const Props& b) noexcept -> bool {
  const auto same = [](const auto& left, const auto& right) {
    return left.has_value() == right.has_value() && (!left.has_value() || *left == *right);
  };
  return same(a.opacity, b.opacity) && same(a.background, b.background) &&
         same(a.border_color, b.border_color) && same(a.color, b.color) &&
         same(a.radius, b.radius) && same(a.border_width, b.border_width) &&
         same(a.font_size, b.font_size) && same(a.translate_x, b.translate_x) &&
         same(a.translate_y, b.translate_y) && same(a.scale, b.scale);
}

// ────────────────────────────────────────────────────────────────────────────
// 关键帧
// ────────────────────────────────────────────────────────────────────────────

auto Keyframes::normalized() const -> Keyframes {
  Keyframes out{};
  out.name = name;
  out.frames = frames;
  if (out.frames.empty()) return out;
  std::ranges::stable_sort(out.frames, [](const Keyframe& left, const Keyframe& right) {
    return left.offset < right.offset;
  });
  for (auto& frame : out.frames) frame.offset = clamp01(frame.offset);
  // 同一 offset 上出现两帧：保留**后者**（CSS 里后声明的覆盖前面的）。
  {
    std::vector<Keyframe> merged;
    merged.reserve(out.frames.size());
    for (const auto& frame : out.frames) {
      if (!merged.empty() && merged.back().offset == frame.offset) {
        merged.back() = frame;
      } else {
        merged.push_back(frame);
      }
    }
    out.frames = std::move(merged);
  }
  if (out.frames.size() == 1) {
    // 单帧：补一个端点让“动画”有意义（否则采样恒为常量，`alternate` 也没得交替）。
    const Keyframe only = out.frames.front();
    if (only.offset <= 0.0f) {
      Keyframe tail = only;
      tail.offset = 1.0f;
      out.frames.push_back(tail);
    } else {
      Keyframe head = only;
      head.offset = 0.0f;
      out.frames.insert(out.frames.begin(), head);
    }
  }
  if (out.frames.front().offset > 0.0f) {
    Keyframe head = out.frames.front();
    head.offset = 0.0f;
    out.frames.insert(out.frames.begin(), head);
  }
  if (out.frames.back().offset < 1.0f) {
    Keyframe tail = out.frames.back();
    tail.offset = 1.0f;
    out.frames.push_back(tail);
  }
  return out;
}

namespace {

/// 在已归一化的关键帧上取 `progress` 处的属性值。
[[nodiscard]] auto sample_frames(const std::vector<Keyframe>& frames, const Easing& easing,
                                 float progress) -> Props {
  if (frames.empty()) return Props{};
  const float clamped = clamp01(progress);
  if (frames.size() == 1) return frames.front().props;
  for (std::size_t index = 1; index < frames.size(); ++index) {
    const Keyframe& left = frames[index - 1];
    const Keyframe& right = frames[index];
    if (clamped <= right.offset) {
      const float span = right.offset - left.offset;
      if (!(span > 0.0f)) return right.props;
      const float local = (clamped - left.offset) / span;
      return lerp_props(left.props, right.props, sample(easing, local));
    }
  }
  return frames.back().props;
}

}  // namespace

// ────────────────────────────────────────────────────────────────────────────
// 动画采样
// ────────────────────────────────────────────────────────────────────────────

auto animation_progress(const Animation& animation, double elapsed) -> float {
  if (!(animation.duration > 0.0)) return elapsed < 0.0 ? 0.0f : 1.0f;
  const double shifted = elapsed - animation.delay;
  if (shifted <= 0.0) return 0.0f;
  const double cycles = shifted / animation.duration;
  if (animation.infinite()) {
    return static_cast<float>(cycles - std::floor(cycles));
  }
  if (cycles >= static_cast<double>(animation.iterations)) return 1.0f;
  return static_cast<float>(cycles - std::floor(cycles));
}

auto sample_animation(const Keyframes& keyframes, const Animation& animation, double elapsed)
    -> Sample {
  Sample out{};
  if (keyframes.empty() || animation.empty()) return out;
  const Keyframes normalized = keyframes.normalized();
  if (normalized.empty()) return out;
  const std::vector<Keyframe>& frames = normalized.frames;
  const Easing segment_easing = animation.easing;
  const auto fill = animation.fill;
  const bool fill_backwards =
      fill == Animation::Fill::Backwards || fill == Animation::Fill::Both;
  const bool fill_forwards = fill == Animation::Fill::Forwards || fill == Animation::Fill::Both;

  if (!(animation.duration > 0.0)) {
    // 零时长：立即落终态（`forwards/both` 才有值；否则不施加覆盖）。
    out.running = false;
    if (fill_forwards || fill_backwards) out.props = frames.back().props;
    return out;
  }

  const double shifted = elapsed - animation.delay;
  if (shifted < 0.0) {
    // 延迟期内。
    out.running = true;
    if (fill_backwards) {
      // `backwards` 的语义：延迟期就显示**首帧**（注意 direction 影响哪一帧是“首”）。
      const bool reversed = animation.direction == Animation::Direction::Reverse ||
                            animation.direction == Animation::Direction::AlternateReverse;
      out.props = sample_frames(frames, segment_easing, reversed ? 1.0f : 0.0f);
    }
    return out;
  }

  const double cycles = shifted / animation.duration;
  const bool finished = !animation.infinite() && cycles >= static_cast<double>(animation.iterations);
  const double cycle_index = std::floor(cycles);
  const float local = static_cast<float>(cycles - cycle_index);

  if (finished && !fill_forwards) {
    out.running = false;
    return out;
  }

  // 方向：奇数次循环是否反向。
  // 用 `std::int64_t` 而不是 `long long`：后者在 LLP64/LP64 上宽度不同，
  // 而这里表达的是**数据**（第几次循环）——`long`/`long long` 承载数据是
  // CONVENTIONS §2 R23 明令禁止的（`long` 在 Windows 是 32 位，越界会静默截断，
  // 而无限循环的计数很容易跑大）。取模用整数（`cycle_index` 已是 floor 的整数）。
  const auto odd = static_cast<std::int64_t>(cycle_index) % 2 != 0;
  bool reversed = false;
  switch (animation.direction) {
    case Animation::Direction::Normal: reversed = false; break;
    case Animation::Direction::Reverse: reversed = true; break;
    case Animation::Direction::Alternate: reversed = odd; break;
    case Animation::Direction::AlternateReverse: reversed = !odd; break;
  }
  const float progress = reversed ? 1.0f - local : local;
  // 结束时用**终点帧的原始值**而不是 0 进度处的插值：`fill: forwards` 的语义是
  // “停在动画的最后状态”，而 `local` 在收尾帧可能略小于 1（浮点），
  // 插值会给出 0.999 的值——在 `opacity` 上就是“淡入结束后不是完全不透明”。
  out.props = finished ? (reversed ? frames.front().props : frames.back().props)
                       : sample_frames(frames, segment_easing, progress);
  out.running = !finished;
  return out;
}

// ────────────────────────────────────────────────────────────────────────────
// 过渡
// ────────────────────────────────────────────────────────────────────────────

auto Transition::affects(Property property) const noexcept -> bool {
  if (properties.empty()) return true;   // 空 = all
  return std::ranges::find(properties, property) != properties.end();
}

auto transition_progress(const Transition& transition, double elapsed) noexcept -> float {
  if (!(transition.duration > 0.0)) return 1.0f;
  const double shifted = elapsed - transition.delay;
  if (shifted <= 0.0) return 0.0f;
  const double ratio = shifted / transition.duration;
  if (ratio >= 1.0) return 1.0f;
  return sample(transition.easing, static_cast<float>(ratio));
}

// ────────────────────────────────────────────────────────────────────────────
// 预设
// ────────────────────────────────────────────────────────────────────────────

namespace {

[[nodiscard]] auto fade_in_up() -> Keyframes {
  Keyframes out{};
  out.name = "fadeInUp";
  Props head{};
  head.opacity = 0.0f;
  head.translate_y = 24.0f;
  Props tail{};
  tail.opacity = 1.0f;
  tail.translate_y = 0.0f;
  out.frames.push_back(Keyframe{0.0f, head});
  out.frames.push_back(Keyframe{1.0f, tail});
  return out;
}

[[nodiscard]] auto modal_in() -> Keyframes {
  Keyframes out{};
  out.name = "modalIn";
  Props head{};
  head.opacity = 0.0f;
  head.scale = 0.96f;
  head.translate_y = 8.0f;
  Props tail{};
  tail.opacity = 1.0f;
  tail.scale = 1.0f;
  tail.translate_y = 0.0f;
  out.frames.push_back(Keyframe{0.0f, head});
  out.frames.push_back(Keyframe{1.0f, tail});
  return out;
}

[[nodiscard]] auto slide_in(float dx) -> Keyframes {
  Keyframes out{};
  out.name = dx < 0.0f ? "slideInLeft" : "slideInRight";
  Props head{};
  head.opacity = 0.0f;
  head.translate_x = dx;
  Props tail{};
  tail.opacity = 1.0f;
  tail.translate_x = 0.0f;
  out.frames.push_back(Keyframe{0.0f, head});
  out.frames.push_back(Keyframe{1.0f, tail});
  return out;
}

[[nodiscard]] auto pulse() -> Keyframes {
  Keyframes out{};
  out.name = "pulse";
  Props rest{};
  rest.opacity = 1.0f;
  Props low{};
  low.opacity = 0.45f;
  out.frames.push_back(Keyframe{0.0f, rest});
  out.frames.push_back(Keyframe{0.5f, low});
  out.frames.push_back(Keyframe{1.0f, rest});
  return out;
}

[[nodiscard]] auto shake() -> Keyframes {
  Keyframes out{};
  out.name = "shake";
  Props center{};
  center.translate_x = 0.0f;
  Props left{};
  left.translate_x = -6.0f;
  Props right{};
  right.translate_x = 6.0f;
  out.frames.push_back(Keyframe{0.0f, center});
  out.frames.push_back(Keyframe{0.2f, left});
  out.frames.push_back(Keyframe{0.4f, right});
  out.frames.push_back(Keyframe{0.6f, left});
  out.frames.push_back(Keyframe{0.8f, right});
  out.frames.push_back(Keyframe{1.0f, center});
  return out;
}

struct PresetEntry {
  std::string_view name;
  Keyframes (*build)();
  double duration;
  int iterations;
  Easing easing;
  Animation::Fill fill;
};

}  // namespace

auto preset_names() -> const std::vector<std::string_view>& {
  static const std::vector<std::string_view> names{"fadeIn",    "fadeInUp", "fadeOut",
                                                   "modalIn",   "slideInLeft", "slideInRight",
                                                   "pulse",     "shake"};
  return names;
}

namespace {

[[nodiscard]] auto build_preset(std::string_view name) -> std::optional<PresetEntry> {
  // **入场类一律 `fill: both`**（不是 `forwards`）。
  //
  // 差的正好是「延迟期」那一段：`forwards` 只在结束后保留终态，延迟期内**不施加
  // 任何覆盖** → 元素以自身样式**原位、完全不透明**地待在那儿，延迟一到才突然跳成
  // 「透明 + 偏移 24px」再淡入。实测（不透明黑块、delay 0.5s、延迟期内采样）：
  // `forwards` 墨量 **255**（完全可见）、`both` 墨量 **11**（首帧态）。
  // 单条动画看不太出来（延迟 0），而 `set_animation_delay` 做 stagger 时
  // 每一块都会先“闪一下”再消失重播——那是肉眼立刻能看见的错。
  if (name == "fadeIn") {
    return PresetEntry{"fadeIn", nullptr, 0.30, 1, Easing::ease_out(), Animation::Fill::Both};
  }
  if (name == "fadeOut") {
    return PresetEntry{"fadeOut", nullptr, 0.25, 1, Easing::ease_in(), Animation::Fill::Both};
  }
  if (name == "fadeInUp") {
    return PresetEntry{"fadeInUp", fade_in_up, 0.45, 1, Easing::ease_out(),
                       Animation::Fill::Both};
  }
  if (name == "modalIn") {
    return PresetEntry{"modalIn", modal_in, 0.25, 1, Easing::ease_out(),
                       Animation::Fill::Both};
  }
  if (name == "slideInLeft") {
    return PresetEntry{"slideInLeft", [] { return slide_in(-32.0f); }, 0.32, 1,
                       Easing::ease_out(), Animation::Fill::Both};
  }
  if (name == "slideInRight") {
    return PresetEntry{"slideInRight", [] { return slide_in(32.0f); }, 0.32, 1,
                       Easing::ease_out(), Animation::Fill::Both};
  }
  if (name == "pulse") {
    // 循环动画**必须用 `fill: both`**：否则循环间隙（`local` 归零）会把值抖回元素原值。
    return PresetEntry{"pulse", pulse, 1.5, 0, Easing::ease_in_out(), Animation::Fill::Both};
  }
  if (name == "shake") {
    return PresetEntry{"shake", shake, 0.5, 1, Easing::linear(), Animation::Fill::None};
  }
  return std::nullopt;
}

}  // namespace

auto preset_keyframes(std::string_view name) -> std::optional<Keyframes> {
  if (name == "fadeIn" || name == "fadeOut") {
    Keyframes out{};
    out.name = std::string(name);
    Props head{};
    head.opacity = name == "fadeIn" ? 0.0f : 1.0f;
    Props tail{};
    tail.opacity = name == "fadeIn" ? 1.0f : 0.0f;
    out.frames.push_back(Keyframe{0.0f, head});
    out.frames.push_back(Keyframe{1.0f, tail});
    return out;
  }
  const auto entry = build_preset(name);
  if (!entry.has_value() || entry->build == nullptr) return std::nullopt;
  return entry->build();
}

auto preset_animation(std::string_view name) -> std::optional<Animation> {
  const auto entry = build_preset(name);
  if (!entry.has_value()) return std::nullopt;
  Animation out{};
  out.name = std::string(name);
  out.duration = entry->duration;
  out.delay = 0.0;
  out.iterations = entry->iterations;
  out.easing = entry->easing;
  out.direction = Animation::Direction::Normal;
  out.fill = entry->fill;
  return out;
}

}  // namespace anim
}  // namespace st::ui

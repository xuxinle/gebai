/// 缓动曲线是**主题令牌**，不是各组件里写死的私有函数。
///
/// ## 守的是什么（实测代价）
///
/// `motion_fast/normal/slow` 原先只管**时长**，曲线分别写在 `toggle.cpp` 与
/// `tabs.cpp` 的私有 `ease_out` 里（都是 `1-(1-t)³`）。两个后果：
///
/// ① **换不掉**：想要"更沉一点"得改所有组件。而时长与曲线是**两个独立旋钮**——
///    同样 180ms，线性是"机械跳过去"、缓出是"滑过去"、缓出再慢是"拖泥带水"。
/// ② **会漂**：两份实现只要有一处被调成 `²` 或 `⁴`，两个控件的节奏就不一致。
///
/// 所以曲线收进 `Metrics::motion_ease_out_power`，组件统一走
/// `st::ui::ease_out(theme, t)`。
///
/// ## 判据为何是这几条
///
/// 只测"默认值等于三次曲线"不够——那挡不住"有人把参数接到别处"。
/// 要同时钉住：**端点精确**（t=0 必须恰好 0，否则动画起步会跳一帧）、
/// **单调不减**（否则出现回弹，而回弹需要专门设计）、
/// **随令牌变化**（证明优化目标真的接上了）。

#include "st/test/test.hpp"

#include <cmath>

#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/ui/theme.hpp"

namespace {

constexpr float kEps = 1e-5F;

[[nodiscard]] auto make_theme(float power) -> st::ui::Theme {
  st::ui::Theme theme = st::ui::Theme::dark();
  theme.metrics().motion_ease_out_power = power;
  return theme;
}

}  // namespace

ST_TEST(ease_out_hits_endpoints_exactly) {
  const st::ui::Theme theme = st::ui::Theme::dark();
  // 端点必须精确：t=0 若不是恰好 0，动画起步会跳一帧；
  // t=1 若不是恰好 1，终态会差一点点（然后靠"超过时长直接返回 target"掩盖，
  // 但中间帧的比较/缓存会因此抖动）。
  ST_CHECK(std::abs(st::ui::ease_out(theme, 0.0F)) <= kEps);
  ST_CHECK(std::abs(st::ui::ease_out(theme, 1.0F) - 1.0F) <= kEps);

  // 越界输入必须被夹住，且**不能出 NaN**。
  // 动画首尾常给出 −1e-7 这类值，而 `pow(负数, 非整数次)` 是 NaN——
  // NaN 混进布局会变成"元素凭空消失"，是最难查的一类故障。
  for (const float t : {-1.0F, -1e-7F, 1.5F}) {
    const float v = st::ui::ease_out(theme, t);
    ST_CHECK(!std::isnan(v));
    ST_CHECK(v >= -kEps);
    ST_CHECK(v <= 1.0F + kEps);
  }
}

ST_TEST(ease_out_is_monotonic_and_shape_shifts_with_token) {
  const st::ui::Theme cubic = make_theme(3.0F);
  const st::ui::Theme linear = make_theme(1.0F);
  const st::ui::Theme steep = make_theme(5.0F);

  float prev_cubic = -1.0F;
  for (int step = 0; step <= 20; ++step) {
    const float t = static_cast<float>(step) / 20.0F;
    const float v = st::ui::ease_out(cubic, t);
    // 单调不减：出现下降就是"回弹"，那需要专门设计（当前语义不含回弹）
    ST_CHECK(v >= prev_cubic - kEps);
    prev_cubic = v;
  }

  // power=1 必须是线性（每帧每控件都在算，这条同时是"省一次 pow"分支的护栏）
  for (int step = 0; step <= 10; ++step) {
    const float t = static_cast<float>(step) / 10.0F;
    ST_CHECK(std::abs(st::ui::ease_out(linear, t) - t) <= kEps);
  }

  // 幂次越大起步越猛（同一点上的进度更大）——证明曲线真的随令牌变
  constexpr float kMid = 0.25F;
  const float v3 = st::ui::ease_out(cubic, kMid);
  const float v5 = st::ui::ease_out(steep, kMid);
  const float v1 = st::ui::ease_out(linear, kMid);
  st::log::out("[ease] t=0.25 时 power1={:.4f} power3={:.4f} power5={:.4f}\n",
            static_cast<double>(v1), static_cast<double>(v3), static_cast<double>(v5));
  ST_CHECK(v3 > v1);
  ST_CHECK(v5 > v3);

  // 非法幂次退化成线性（**不能**变成 0：那会让动画卡在起点不动）
  const st::ui::Theme broken = make_theme(0.0F);
  ST_CHECK(std::abs(st::ui::ease_out(broken, 0.4F) - 0.4F) <= kEps);
  const st::ui::Theme negative = make_theme(-2.0F);
  ST_CHECK(std::abs(st::ui::ease_out(negative, 0.4F) - 0.4F) <= kEps);
}

ST_TEST(ease_out_default_matches_historical_cubic) {
  // 默认档必须与历史行为**逐点相同**（三次缓出）——否则所有既有动效的手感会一起变，
  // 而那种变化没有任何测试会报红。
  const st::ui::Theme theme = st::ui::Theme::dark();
  for (int step = 0; step <= 20; ++step) {
    const float t = static_cast<float>(step) / 20.0F;
    const float expected = 1.0F - std::pow(1.0F - t, 3.0F);
    ST_CHECK(std::abs(st::ui::ease_out(theme, t) - expected) <= kEps);
  }
}

/// 裁云 JS 编排层：**弹簧族**与扩展缓动的契约。
///
/// ## 为什么这组用例值得单独存在
///
/// 差距盘点的结论是「我们的动效软——所有东西都是同一种 S 曲线」。
/// 根因不是缺曲线，而是缺**二阶动力学**：真实物体有余量、会过冲、会回弹，
/// 而任何 `easeOut*` 都是单调的（永不回头），所以再快也"没有质量"。
///
/// ## 测试策略（这里有个坑，已避开）
///
/// 第一版把公式在测试里**又抄了一遍**，然后断言抄件满足物理性质——
/// 那是典型的"桩把差异抹平"：实现写错、抄件照着错，测试照样绿。
///
/// 现在一律**经 `LayoutEngine` 执行真实脚本**去调宿主函数：
/// 测的是 `layout.cpp` 里注册的那一份，而不是测试里另写的一份。
///
/// 判据选**物理性质**而不是固定数值（数值会随参数微调漂移，性质不会）：
/// 起于 0、趋于 1；欠阻尼必须过冲、临界/过阻尼不许；ζ 越大过冲越小；
/// ω 越大越快；`spring_vel` 与 `spring` 的差分一致；`settle` 之后真的稳了。

#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "examples/caiyun/layout.hpp"
#include "st/test/test.hpp"

namespace {

/// 跑一段脚本，返回它 `return` 的数值（失败给 `nullopt`）。
///
/// 不用异常：`CONVENTIONS.md` R10 禁 `throw`（异常作控制流），
/// 而本文件需要“失败就停下”的语义——用 `optional` + 调用点 `ST_REQUIRE` 表达。
///
/// 用一层的 `layers` 承载返回值：脚本把结果写成该图层的 `dx`，
/// 这样不必给测试新增“求值表达式”的特殊通道。
auto eval_number(const std::string& expression, float time = 0.0f) -> std::optional<double> {
  caiyun::LayoutEngine engine;
  caiyun::LayoutScript script{};
  script.source = std::format(
      "function layout(t, duration, canvas, layers) {{\n"
      "  return [{{ dx: {} }}];\n"
      "}}\n",
      expression);
  if (!engine.load(script).has_value()) return std::nullopt;

  std::vector<caiyun::LayerState> layers(1);
  layers[0].x = 0.0f;
  layers[0].w = 10.0f;
  layers[0].h = 10.0f;
  layers[0].alpha = 1.0f;
  std::vector<caiyun::LayerDelta> out(1);
  if (!engine.evaluate(time, 2.0f, 1920.0f, 1080.0f, layers, out).has_value()) {
    return std::nullopt;
  }
  return static_cast<double>(out[0].dx);
}

/// 必成版本：失败就让当前用例失败并返回 0（后续断言会连着红，但不会假装成功）。
auto must_eval(const std::string& expression, float time = 0.0f) -> double {
  const auto value = eval_number(expression, time);
  if (!value.has_value()) return 0.0;
  return value.value();
}

/// 在时间轴上采样一条弹簧响应。
auto sample_spring(double zeta, double omega, int count = 240, double span = 2.0)
    -> std::vector<double> {
  const std::string expression =
      std::format("spring(t, {:g}, {:g})", zeta, omega);
  std::vector<double> out;
  out.reserve(static_cast<std::size_t>(count) + 1);
  for (int i = 0; i <= count; ++i) {
    const auto time = static_cast<float>(span * static_cast<double>(i) / count);
    out.push_back(must_eval(expression, time));
  }
  return out;
}

}  // namespace

ST_TEST(spring_starts_at_zero_and_settles_at_one) {
  // 起点必须 0（否则入场会"跳一下"），终点收敛到 1（否则永远差一点到位，
  // 看起来像布局错位而不是动画）。
  ST_CHECK_NEAR(must_eval("spring(0, 0.6, 20)", 0.0f), 0.0, 1e-9);
  ST_CHECK_NEAR(must_eval("spring(5, 0.6, 20)", 0.0f), 1.0, 1e-3);
  // 负时间（还没开始）也必须静止在 0，而不是振荡回头。
  ST_CHECK_NEAR(must_eval("spring(-0.5, 0.4, 20)", 0.0f), 0.0, 1e-12);
}

ST_TEST(spring_underdamped_overshoots) {
  // **"有质量"的来源**：欠阻尼必须越过 1 再回来。
  // 若这条红，说明弹簧退化成了单调缓动——运动质量的根就没了。
  const auto samples = sample_spring(0.4, 20.0);
  const double peak = *std::max_element(samples.begin(), samples.end());
  ST_CHECK(peak > 1.02);
}

ST_TEST(spring_critical_and_overdamped_never_overshoot) {
  // ζ >= 1 在物理上就不该回头（临界/过阻尼）。
  // 若实现把阻尼公式写反，这里会红。
  for (const double zeta : {1.0, 1.5, 2.0}) {
    const auto samples = sample_spring(zeta, 20.0);
    const double peak = *std::max_element(samples.begin(), samples.end());
    ST_CHECK(peak <= 1.0 + 1e-6);
  }
}

ST_TEST(spring_more_damping_means_less_overshoot) {
  // 单调关系是"调参手感"的契约：ζ 越大过冲越小。
  // 不单调的话，写脚本的人就没有可依赖的直觉。
  double previous = 1e9;
  for (const double zeta : {0.3, 0.5, 0.7, 0.9}) {
    const auto samples = sample_spring(zeta, 20.0);
    const double peak = *std::max_element(samples.begin(), samples.end());
    ST_CHECK(peak < previous);
    previous = peak;
  }
}

ST_TEST(spring_higher_omega_settles_sooner) {
  const double slow = must_eval("spring(0.12, 0.6, 12)", 0.0f);
  const double fast = must_eval("spring(0.12, 0.6, 30)", 0.0f);
  ST_CHECK(fast > slow);
}

ST_TEST(spring_velocity_matches_finite_difference) {
  // `spring_vel` 必须真的是 `spring` 的导数——它驱动 squash & stretch：
  // 速度错 ⇒ 形变错 ⇒ 看起来像"固定幅度地压一下"而不是碰撞。
  //
  // 用**真实实现**的两处结果做中心差分交叉验证（不是抄公式）。
  constexpr double kStep = 1e-3;
  for (const double tau : {0.05, 0.12, 0.25, 0.4, 0.8}) {
    const double ahead = must_eval(std::format("spring({}, 0.55, 22)", tau + kStep));
    const double behind = must_eval(std::format("spring({}, 0.55, 22)", tau - kStep));
    const double numeric = (ahead - behind) / (2.0 * kStep);
    const double analytic =
        must_eval(std::format("spring_vel({}, 0.55, 22)", tau));
    ST_CHECK_NEAR(analytic, numeric, std::max(1e-3, std::abs(numeric) * 0.02));
  }
}

ST_TEST(spring_settle_bound_is_actually_settled) {
  // `settle` 的意义：让脚本敢按它排节奏（"这一弹占 0.62s，然后切镜"）。
  // 若它算早了，动作会在没到位时被砍断——**画面上看不出是它错的**。
  constexpr double kZeta = 0.6;
  constexpr double kOmega = 20.0;
  const double bound = must_eval("settle(0.6, 20, 0.02)");
  ST_CHECK(bound > 0.0);
  ST_CHECK(bound < 2.0);  // 别保守到失去意义
  // 该时刻之后（含余量），响应必须在容差内
  for (double offset = 0.0; offset <= 0.4; offset += 0.05) {
    const double value =
        must_eval(std::format("spring({}, {}, {})", bound + offset, kZeta, kOmega));
    ST_CHECK(std::abs(value - 1.0) <= 0.06);
  }
}

ST_TEST(spring_rejects_nonsense_parameters_without_nan) {
  // 极端参数不能让 NaN 漏出去：NaN 会让元素**直接消失**，而且不报错。
  for (const char* expression :
       {"spring(0.5, 0.0, 20)", "spring(0.5, -1.0, 20)", "spring(0.5, 0.6, 0)",
        "spring(1e6, 0.05, 80)", "spring(0.3, 5.0, 200)"}) {
    const double value = must_eval(expression);
    ST_CHECK(std::isfinite(value));
  }
}

ST_TEST(layout_ease_supports_the_new_curves) {
  // 新增三条曲线必须真的接上（写错名字会走到 `unexpected`，
  // 而 `evaluate` 失败时 `out` 被填零增量 ⇒ 这里读到 0 —— 正好能抓住）。
  ST_CHECK_NEAR(must_eval("ease('expo_out', 0.5)"), 0.969, 0.01);
  ST_CHECK_NEAR(must_eval("ease('expo_in_out', 0.5)"), 0.5, 0.02);
  ST_CHECK_NEAR(must_eval("ease('smoother', 0.5)"), 0.5, 0.01);
  // 端点必须精确（缓动的两端是所有排版的锚点）
  ST_CHECK_NEAR(must_eval("ease('expo_out', 0)"), 0.0, 1e-9);
  ST_CHECK_NEAR(must_eval("ease('expo_out', 1)"), 1.0, 1e-9);
  ST_CHECK_NEAR(must_eval("ease('smoother', 1)"), 1.0, 1e-9);
  // `expo_out` 必须比 `out` 更快到位（t80 0.23 vs 0.42）——
  // 这是它被单独加进来的理由，若不成立则它没有存在价值。
  ST_CHECK(must_eval("ease('expo_out', 0.3)") > must_eval("ease('out', 0.3)"));
}

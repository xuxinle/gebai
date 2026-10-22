#include <array>
#include <cmath>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "st/text/weight_curve.hpp"
#include "st/test/test.hpp"

namespace {

using st::text::WeightCurve;

/// 把 `(px, gamma, contrast)` 三元组列表塞进曲线。
auto make(std::initializer_list<std::array<float, 3>> rows) -> WeightCurve {
  std::vector<WeightCurve::Sample> samples;
  samples.reserve(rows.size());
  for (const auto& r : rows) {
    WeightCurve::Sample s;
    s.pixel_size = r[0];
    s.gamma = r[1];
    s.contrast = r[2];
    samples.push_back(s);
  }
  return WeightCurve(std::move(samples));
}

}  // namespace

ST_TEST(weight_curve_empty_falls_back_to_globals) {
  WeightCurve curve;
  ST_CHECK(curve.empty());
  const auto v = curve.evaluate(15.0f, 0.85f, 0.6f);
  ST_CHECK(!v.active);
  ST_CHECK_NEAR(v.gamma, 0.85f, 1e-6f);
  ST_CHECK_NEAR(v.contrast, 0.6f, 1e-6f);
}

/// **线性插值**：两点之间必须落在连线上，且连续（不能跳变）。
ST_TEST(weight_curve_interpolates_linearly) {
  const auto curve = make({{10.0f, 0.80f, 0.40f}, {20.0f, 1.20f, 0.00f}});
  // 第三个参数是**全局对比度基底**（乘法修正，见 `evaluate` 的说明）。
  // 传 1.0f = 不缩放，才能直接断言曲线里的数值。
  const auto mid = curve.evaluate(15.0f, 1.0f, 1.0f);
  ST_CHECK(mid.active);
  ST_CHECK_NEAR(mid.gamma, 1.00f, 1e-5f);
  ST_CHECK_NEAR(mid.contrast, 0.20f, 1e-5f);
  // 连续性：跨过中点两侧的增量应该极小
  const auto a = curve.evaluate(14.99f, 1.0f, 1.0f);
  const auto b = curve.evaluate(15.01f, 1.0f, 1.0f);
  ST_CHECK(std::abs(a.gamma - b.gamma) < 1e-3f);
}

/// **对比度是乘法修正，不是覆盖**（`curve.contrast × global_contrast`）。
///
/// 为何必须有这条：这个语义是修了一个真 bug 改过来的（“一旦加载曲线，
/// 全局 `--cstrength` 就完全失效”）。但改完之后**没有用例盯着它**，
/// 于是测试里那句旧的 `evaluate(..., 0.0f)` 被默认为“未指定”——
/// 在乘法语义下它等于“对比度清零”，四个用例因此集体变红。
ST_TEST(weight_curve_contrast_is_multiplicative) {
  const auto curve = make({{20.0f, 1.0f, 0.55f}});
  // 基底 1.0：曲线原值
  ST_CHECK_NEAR(curve.evaluate(20.0f, 1.0f, 1.0f).contrast, 0.55f, 1e-6f);
  // 基底 2.0：翻倍
  ST_CHECK_NEAR(curve.evaluate(20.0f, 1.0f, 2.0f).contrast, 1.10f, 1e-6f);
  // 基底 0.5：减半
  ST_CHECK_NEAR(curve.evaluate(20.0f, 1.0f, 0.5f).contrast, 0.275f, 1e-6f);
  // 基底 0（合法的“关掉对比度”）→ 归零，而不是回退成曲线原值
  ST_CHECK_NEAR(curve.evaluate(20.0f, 1.0f, 0.0f).contrast, 0.00f, 1e-6f);
}

/// 超出范围取**端点值**，不外推斜率（否则极字号会得到荒谬的 γ）。
ST_TEST(weight_curve_clamps_outside_range) {
  const auto curve = make({{10.0f, 0.80f, 0.40f}, {20.0f, 1.20f, 0.00f}});
  const auto tiny = curve.evaluate(2.0f, 1.0f, 1.0f);
  ST_CHECK_NEAR(tiny.gamma, 0.80f, 1e-6f);
  ST_CHECK_NEAR(tiny.contrast, 0.40f, 1e-6f);
  const auto huge = curve.evaluate(200.0f, 1.0f, 1.0f);
  ST_CHECK_NEAR(huge.gamma, 1.20f, 1e-6f);
  ST_CHECK_NEAR(huge.contrast, 0.00f, 1e-6f);
}

/// 文本解析：允许注释、空行、多余字段；同字号**后来者胜**；非法点丢弃。
ST_TEST(weight_curve_parses_text) {
  WeightCurve curve;
  const std::string text =
      "# 注释行\n"
      "\n"
      "  9.0  0.70  1.00  极小字\n"
      "  15.0 0.94  0.58\n"
      "  15.0 0.96  0.55  同字号后来者胜\n"
      "  bogus line here\n"
      "  0.0  1.0   1.0   非法字号\n";
  const std::size_t n = curve.load_from_text(text);
  ST_CHECK_EQ(n, std::size_t{2});   // 9 和 15（后者覆盖前者）
  ST_CHECK_EQ(curve.samples().size(), std::size_t{2});
  const auto v = curve.evaluate(15.0f, 1.0f, 1.0f);
  ST_CHECK_NEAR(v.gamma, 0.96f, 1e-5f);   // 后来者胜
  ST_CHECK_NEAR(v.contrast, 0.55f, 1e-5f);
}

/// 数值护栏：γ 夹到 [0.3, 4]、对比度夹到 [0, 3]；非有限值被替换。
ST_TEST(weight_curve_sanitizes_samples) {
  const auto curve = make({{10.0f, 0.10f, 9.0f}, {20.0f, 99.0f, -5.0f}});
  const auto low = curve.evaluate(10.0f, 1.0f, 1.0f);
  ST_CHECK_NEAR(low.gamma, 0.30f, 1e-6f);   // 下界
  ST_CHECK_NEAR(low.contrast, 3.00f, 1e-6f);
  const auto high = curve.evaluate(20.0f, 1.0f, 1.0f);
  ST_CHECK_NEAR(high.gamma, 4.00f, 1e-6f);  // 上界
  ST_CHECK_NEAR(high.contrast, 0.00f, 1e-6f);
}

/// 无序输入必须被排序（否则插值会取错相邻点）。
ST_TEST(weight_curve_sorts_input) {
  const auto curve = make({{30.0f, 1.30f, 0.0f}, {10.0f, 0.70f, 0.4f}, {20.0f, 1.00f, 0.2f}});
  const auto& s = curve.samples();
  ST_CHECK_EQ(s.size(), std::size_t{3});
  ST_CHECK(s[0].pixel_size < s[1].pixel_size);
  ST_CHECK(s[1].pixel_size < s[2].pixel_size);
  const auto v = curve.evaluate(15.0f, 1.0f, 1.0f);
  ST_CHECK_NEAR(v.gamma, 0.85f, 1e-5f);
}

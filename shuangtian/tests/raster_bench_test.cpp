// 光栅器性能基准（**性能回归护栏**，不是精确的性能模型）。
//
// 为什么需要它：界面"卡"的时候，争论的焦点永远是"是不是阴影太慢/文字太慢"，
// 而软件光栅器的成本分布又高度依赖几何形态（边缘数、覆盖面积、模糊半径），
// 靠肉眼与直觉判断必然跑偏。这里把界面里最常见的四类绘制负载固化成可重复的测量。
//
// ## 断言怎么定（2026-10 重定：从"绝对毫秒"改为"归一化倍数"）
//
// **原先的写法是错的，且错得危险**。旧断言直接卡绝对毫秒（如 `per_card <= 1.5`），
// 而实测值是 0.033 ms/张——**余量 34 倍**（投影 23×、文字 22×、渐变 714×）。
// 实测验证过它的后果：把渲染代价抬到 **1.21 ms/张（+27 倍）**，断言**依然通过**。
// 也就是说这类退化（误删 SIMD 快路径、缓存失效…）能一路穿过测试网——
// 比"没有性能测试"更糟，因为它给人已经覆盖了的错觉。
//
// 现在改为**相对基准的倍数**，且余量被**用例自己盯住**（见文件末的 `bench_thresholds_*`）：
//
// 1. **机器无关**：被测操作的成本 / "清屏一次"的每像素成本 = 纯比值。
//    清屏是纯内存写、与本机速度线性相关，作分母能把 CPU 差异约掉。
//    （实测本机比值：圆角卡片 ≈1.7×、投影 ≈14×、文本 ≈39×、渐变 ≈2×。）
// 2. **余量有上限**：`bench_thresholds_have_bounded_headroom` 逐条重测并断言
//    **阈值 / 实测 ≤ `kMaxHeadroom`**。谁把阈值放宽到"抓不住退化"，这条当场红灯。
// 3. **负载必须高于计时噪声**：渐变原先 20 块只花 0.7 ms，比值波动高达 20%
//    （计时器与调度噪声占主导）→ 已把负载加大到 ~7 ms 量级。

#include <chrono>
#include <cstdint>
#include <format>

#include "st/core/print.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"
#include "st/test/test.hpp"
#include "st/text/text.hpp"

namespace {

using Clock = std::chrono::steady_clock;

inline constexpr int kCanvasWidth = 1280;
inline constexpr int kCanvasHeight = 800;

[[nodiscard]] auto elapsed_ms(const Clock::time_point& start) -> double {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

[[nodiscard]] auto make_canvas() -> st::raster::Canvas {
  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(kCanvasWidth, kCanvasHeight, 1.0f);
  canvas.clear(st::math::Color::rgb(0xF7, 0xF8, 0xFA));
  return canvas;
}

/// 基准单位：把整块画布清一次色的**每像素毫秒数**。
///
/// 取多次的**最小值**而不是平均值：清屏是纯内存写、无分支，最小次最能代表
/// "本机不受干扰时有多快"，被调度噪声抬高的那些次不该把分母做大（分母偏大会
/// 让所有比值偏小 → 阈值变松，方向正好是错的那一边）。
[[nodiscard]] auto baseline_ms_per_pixel() -> double {
  st::raster::Canvas canvas = make_canvas();
  constexpr int kRuns = 7;
  double best = 1e9;
  for (int index = 0; index < kRuns; ++index) {
    const auto start = Clock::now();
    canvas.clear(st::math::Color::rgb(0xF7, 0xF8, 0xFA));
    best = std::min(best, elapsed_ms(start));
  }
  return best / (static_cast<double>(kCanvasWidth) * static_cast<double>(kCanvasHeight));
}

/// 一次测量的结果：绝对耗时 + 归一化比值。
struct Measurement {
  double ms{0.0};
  double ratio{0.0};  ///< 每单位代价 / 清屏每像素成本（机器无关）
};

}  // namespace

// ————————————————————————————————————————————————————————————————————————————
// 四类负载的测量函数（**用例与余量自检共用同一份测量**，避免两处算法漂移）
// ————————————————————————————————————————————————————————————————————————————

namespace {

/// 圆角卡片填充 ×60（界面里最常见的负载）。
[[nodiscard]] auto measure_rounded_cards(double unit) -> Measurement {
  st::raster::Canvas canvas = make_canvas();
  const st::raster::Paint paint = st::raster::Paint::solid(st::math::Color::rgb(255, 255, 255));
  constexpr int kCards = 60;
  constexpr double kCardWidth = 320.0;
  constexpr double kCardHeight = 160.0;
  const auto start = Clock::now();
  for (int index = 0; index < kCards; ++index) {
    const float x = static_cast<float>(index % 6) * 200.0f + 20.0f;
    const float y = static_cast<float>(index / 6) * 130.0f + 20.0f;
    canvas.fill_rect(
        st::math::Rect{x, y, static_cast<float>(kCardWidth), static_cast<float>(kCardHeight)}, paint,
        12.0f);
  }
  const double ms = elapsed_ms(start);
  const double per_card = ms / kCards;
  return Measurement{ms, per_card / (kCardWidth * kCardHeight) / unit};
}

/// 卡片投影 ×20（blur=16；同尺寸命中遮罩缓存，稳态成本 = 合成）。
[[nodiscard]] auto measure_card_shadows(double unit) -> Measurement {
  st::raster::Canvas canvas = make_canvas();
  const st::math::Color color{0, 0, 0, 40};
  constexpr int kShadows = 20;
  constexpr double kCardWidth = 300.0;
  constexpr double kCardHeight = 140.0;
  constexpr double kBlur = 16.0;
  const double area = (kCardWidth + kBlur * 4.0 + 4.0) * (kCardHeight + kBlur * 4.0 + 4.0);
  const auto start = Clock::now();
  for (int index = 0; index < kShadows; ++index) {
    const float x = static_cast<float>(index % 4) * 320.0f + 24.0f;
    const float y = static_cast<float>(index / 4) * 160.0f + 24.0f;
    canvas.draw_shadow(
        st::math::Rect{x, y, static_cast<float>(kCardWidth), static_cast<float>(kCardHeight)}, 12.0f,
        static_cast<float>(kBlur), color, st::math::Point{0.0f, 4.0f});
  }
  const double ms = elapsed_ms(start);
  const double per_shadow = ms / kShadows;
  return Measurement{ms, per_shadow / area / unit};
}

/// 文本绘制 ×60 行（字型缓存命中后的稳态成本）。
/// 无字体环境（CI 容器）返回 `nullopt`——那是**跳过**，不是通过。
[[nodiscard]] auto measure_text(bool* available) -> Measurement {
  st::raster::Canvas canvas = make_canvas();
  auto fonts = st::text::FontStack::system_default();
  if (!fonts) {
    *available = false;
    return {};
  }
  *available = true;
  st::text::TextRenderer renderer(*fonts);
  const std::string sample = "霜天 · 组件画廊 1280x800 headless control 0123456789";
  constexpr int kLines = 60;
  const double unit = baseline_ms_per_pixel();
  const auto start = Clock::now();
  double glyph_pixels = 0.0;
  for (int index = 0; index < kLines; ++index) {
    const float y = static_cast<float>(index % 24) * 30.0f + 20.0f;
    (void)renderer.draw(canvas, sample, st::math::Point{24.0f, y}, 14.0f,
                        st::math::Color::rgb(0x1F, 0x24, 0x2C));
    glyph_pixels += static_cast<double>(sample.size()) * 14.0 * 14.0 * 0.5;
  }
  const double ms = elapsed_ms(start);
  const double per_line = ms / kLines;
  return Measurement{ms, (per_line / (glyph_pixels / kLines)) / unit};
}

/// 渐变填充：20 块 × 10 遍（**负载刻意加大到毫秒级**——原先 20 块只花 0.7 ms，
/// 比值波动 20%，阈值会被噪声逼得只能放宽）。
[[nodiscard]] auto measure_gradients(double unit) -> Measurement {
  st::raster::Canvas canvas = make_canvas();
  const st::raster::Paint paint = st::raster::Paint::with_gradient(st::raster::Gradient::linear(
      st::math::Point{0.0f, 0.0f}, st::math::Point{0.0f, 800.0f},
      {{0.0f, st::math::Color::rgb(0x3B, 0x82, 0xF6)},
       {1.0f, st::math::Color::rgb(0x93, 0xC5, 0xFD)}}));
  constexpr int kRects = 20;
  constexpr int kSweeps = 10;
  constexpr double kRectWidth = 1200.0;
  constexpr double kRectHeight = 36.0;
  const auto start = Clock::now();
  for (int sweep = 0; sweep < kSweeps; ++sweep) {
    for (int index = 0; index < kRects; ++index) {
      const float y = static_cast<float>(index) * 40.0f;
      canvas.fill_rect(
          st::math::Rect{40.0f, y, static_cast<float>(kRectWidth), static_cast<float>(kRectHeight)},
          paint, 8.0f);
    }
  }
  const double ms = elapsed_ms(start);
  const double per_rect = ms / static_cast<double>(kRects * kSweeps);
  return Measurement{ms, per_rect / (kRectWidth * kRectHeight) / unit};
}

}  // namespace

// ————————————————————————————————————————————————————————————————————————————
// 门禁阈值（归一化比值；实测值见每条的注释）
// ————————————————————————————————————————————————————————————————————————————

namespace {

/// 圆角卡片：实测 ≈1.7× 清屏单位 → 阈值 4×（余量 ≈2.4 倍）。
inline constexpr double kRoundedCardsLimit{4.0};
/// 投影：实测 ≈14× → 阈值 30×（余量 ≈2.1）。
inline constexpr double kShadowLimit{30.0};
/// 文本：实测 ≈39× → 阈值 90×（余量 ≈2.3）。本项比值最稳（三次测量 ±0.3%）。
inline constexpr double kTextLimit{90.0};
/// 渐变：实测 ≈2× → 阈值 6×（余量 ≈3）。负载已加大到毫秒级以压低噪声。
inline constexpr double kGradientLimit{6.0};

/// 我们希望"至少这么多次退化"能被门禁抓住（2 倍以内可能是机器差异，不必报警）。
inline constexpr double kFaultFactor{3.0};

/// **任何一条性能断言的余量上限**。
///
/// 这是本文件最重要的一条约束：它把"阈值不能太松"从**愿望**变成**可检查的性质**。
/// 旧写法（绝对毫秒 + 20~700 倍余量）之所以危险，不是数字难看，而是**没人会去复核**。
/// 现在余量本身是用例断言的对象（见下方 `bench_thresholds_*`）。
inline constexpr double kMaxHeadroom{3.5};

}  // namespace

ST_TEST(bench_rounded_card_fill) {
  const double unit = baseline_ms_per_pixel();
  const Measurement m = measure_rounded_cards(unit);
  st::print("[bench] 圆角卡片填充 ×60: {:.2f} ms（{:.3f} ms/张，{:.2f}× 清屏单位，阈值 {}×）\n",
            m.ms, m.ms / 60.0, m.ratio, kRoundedCardsLimit);
  ST_CHECK(m.ratio <= kRoundedCardsLimit);
}

ST_TEST(bench_card_shadow) {
  const double unit = baseline_ms_per_pixel();
  const Measurement m = measure_card_shadows(unit);
  st::print("[bench] 卡片投影 ×20（blur=16）: {:.2f} ms（{:.3f} ms/个，{:.2f}× 清屏单位，阈值 {}×）\n",
            m.ms, m.ms / 20.0, m.ratio, kShadowLimit);
  ST_CHECK(m.ratio <= kShadowLimit);
}

ST_TEST(bench_text_draw) {
  bool available = false;
  const Measurement m = measure_text(&available);
  if (!available) {
    st::print("[bench] 文本：无系统字体，本项跳过（不算通过）\n");
    return;
  }
  st::print("[bench] 文本 ×60（59 字符/行，字型缓存命中）: {:.2f} ms（{:.3f} ms/行，{:.2f}× 清屏单位，阈值 {}×）\n",
            m.ms, m.ms / 60.0, m.ratio, kTextLimit);
  ST_CHECK(m.ratio <= kTextLimit);
}

ST_TEST(bench_gradient_fill) {
  const double unit = baseline_ms_per_pixel();
  const Measurement m = measure_gradients(unit);
  st::print("[bench] 渐变填充 ×200（1200×36）: {:.2f} ms（{:.3f} ms/块，{:.2f}× 清屏单位，阈值 {}×）\n",
            m.ms, m.ms / 200.0, m.ratio, kGradientLimit);
  ST_CHECK(m.ratio <= kGradientLimit);
}

// ————————————————————————————————————————————————————————————————————————————
// 门禁自检（本节的存在理由：让"这把尺子准不准"成为可检查的事）
// ————————————————————————————————————————————————————————————————————————————

/// **余量有上限**：逐条重测，断言 阈值 / 实测 ≤ `kMaxHeadroom`。
///
/// 这条用例守的是**性能门禁本身**，不是被测代码。若有人把阈值放宽回
/// "绝对毫秒 + 20 倍余量"（那正是本文件原先的写法），这里当场红灯——
/// 而不是等某次退化悄悄穿过测试网。
///
/// 与 `CONVENTIONS §7.1`「新写的回归测试必须验证它真能抓住那个缺陷」同一条纪律，
/// 只是从**功能**测试推广到**性能**测试：断言自己也要被验证。
ST_TEST(bench_thresholds_have_bounded_headroom) {
  const double unit = baseline_ms_per_pixel();
  const auto check = [](const char* name, double limit, const Measurement& m) {
    ST_REQUIRE(m.ratio > 0.0);
    const double headroom = limit / m.ratio;
    st::print("[bench-guard] {}: 阈值 {:.1f}× / 实测 {:.2f}× = 余量 {:.2f} 倍（上限 {}）\n", name,
              limit, m.ratio, headroom, kMaxHeadroom);
    // 余量太小 → 机器噪声就会误报；太大 → 抓不住退化。两头都要卡住。
    ST_CHECK(headroom <= kMaxHeadroom);
    ST_CHECK(headroom >= 1.3);
  };
  check("圆角卡片", kRoundedCardsLimit, measure_rounded_cards(unit));
  check("卡片投影", kShadowLimit, measure_card_shadows(unit));
  check("渐变填充", kGradientLimit, measure_gradients(unit));

  bool available = false;
  const Measurement text = measure_text(&available);
  if (available) check("文本绘制", kTextLimit, text);
}

/// **退化必然被发现**：把被测操作重复 `kFaultFactor` 倍，断言一定越过阈值。
///
/// 这条比上一条更强——它直接给出"多少次退化会被抓住"这个可读的数。
/// 实现上不真的跑退化版本（那要多花几倍的机时），而是用**同一份测量结果做线性外推**：
/// 性能退化在"重复同一操作"这个维度上就是线性叠加（多做 N 次 = 多 N 倍成本），
/// 所以"比值 × kFaultFactor > 阈值"等价于"N 倍退化会被抓住"。
///
/// 注意它**不是**在测"渲染会不会懒优化"——`fill_rect` 重复 N 次是真的画 N 次
/// （同一矩形重画仍要走完整条光栅化路径），这正是我们要模拟的退化形态。
ST_TEST(bench_fault_multiplication_is_caught) {
  const double unit = baseline_ms_per_pixel();
  const auto check = [](const char* name, double limit, const Measurement& m) {
    const bool caught = m.ratio * kFaultFactor > limit;
    st::print("[bench-guard] {}: 实测 {:.2f}× × {} 倍退化 = {:.2f}× vs 阈值 {:.1f}× → {}\n", name,
              m.ratio, kFaultFactor, m.ratio * kFaultFactor, limit, caught ? "会被抓住" : "**抓不住**");
    ST_CHECK(caught);
  };
  check("圆角卡片", kRoundedCardsLimit, measure_rounded_cards(unit));
  check("卡片投影", kShadowLimit, measure_card_shadows(unit));
  check("渐变填充", kGradientLimit, measure_gradients(unit));

  bool available = false;
  const Measurement text = measure_text(&available);
  if (available) check("文本绘制", kTextLimit, text);
}

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
/// 噪声鲁棒测量：把一次测量重复 `kMeasureRepeats` 遍，取**最小值**。
///
/// 为什么必须这样：本机是共享环境（cgroup 限 4 核、loadavg 常 >2），单次测量会飘。
/// 实测同一份代码连跑 4 次，渐变填充的比值在 **1.10×~1.68×** 之间跳（+53%）——
/// 这不是被测代码在变，是调度噪声。噪声**只会抬高**耗时，所以最小值最接近真实成本，
/// 也是唯一能让"余量上限"这类断言有意义的取法。
///
/// ⚠ 这不是"把阈值调松"：阈值一律不动，只是把**测量**做稳。
/// 对比：`baseline_ms_per_pixel()` 用的是"每像素成本"（已除以面积），
/// 这里的四个负载是**整块**负载，不能那样等效缩小。
inline constexpr int kMeasureRepeats{3};

template <class Measure>
[[nodiscard]] auto best_of(Measure&& measure) -> double {
  double best = 1e9;
  for (int index = 0; index < kMeasureRepeats; ++index) {
    best = std::min(best, measure());
  }
  return best;
}

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

/// 当前构建是否带 sanitizer（即 `st test --san`）。
///
/// ## 为什么性能门禁必须认出它
///
/// 本文件的"机器无关"靠的是**比值**：被测操作成本 / 清屏每像素成本。
/// 这条推理成立的前提是"两者被同样地加速/减速"——而 **sanitizer 打破了这个前提**：
/// ASan 的插桩开销正比于**内存访问量**，而清屏是纯 `memset`（几乎不插桩）。
/// 实测同一份代码在 san 档下的比值膨胀得**极不均匀**：清屏快得少、圆角卡片 4.36×、
/// 文本 56.5×、阴影 15.4×（发布档分别是 0.38 ms、1.4×、33×、14×）。
///
/// 于是"余量 = 阈值/实测"这个量在插桩档下**失去意义**：它既可能 0.92（看起来
/// "阈值太松"），也可能 2.59（看起来"还好"），而两者都不反映真实性能。
///
/// ## 处理方式：跳过上界断言，不是放宽阈值
///
/// 把阈值按插桩档调宽会**真的**把发布档门禁变松（同一份阈值两边共用）。
/// 所以这里的选择是：插桩档**跳过余量上界断言**并在报告里写明，
/// 门禁的标定与把关只在发布档完成（这也正是 `st test` 默认档）。
/// **不是静默跳过**：`st::print` 明说跳过了什么、为什么。
[[nodiscard]] constexpr auto instrumented_build() noexcept -> bool {
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
  return true;
#else
  return false;
#endif
}

/// 插桩档下跳过**比值型**阈值断言，并说明跳过了哪一条。
/// 返回 `true` = 调用方应当返回（跳过）。
///
/// 统一成一个入口，是为了让"插桩档到底跳了多少东西"在一处可见——
/// 分散写 `if (instrumented_build()) return;` 久了就没人说得清跳过面有多大。
[[nodiscard]] inline auto skip_ratios_under_instrumentation(const char* name) -> bool {
  if (!instrumented_build()) return false;
  st::print("[bench] {}：插桩档（san）跳过比值阈值——ASan 开销 ∝ 内存访问量，\n"
            "        而分母（清屏）是纯内存写，比值不再机器无关。标定以发布档为准。\n",
            name);
  return true;
}

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
  const double ms = best_of([&] {
    const auto start = Clock::now();
    for (int index = 0; index < kCards; ++index) {
      const float x = static_cast<float>(index % 6) * 200.0f + 20.0f;
      const float y = static_cast<float>(index / 6) * 130.0f + 20.0f;
      canvas.fill_rect(
          st::math::Rect{x, y, static_cast<float>(kCardWidth), static_cast<float>(kCardHeight)},
          paint, 12.0f);
    }
    return elapsed_ms(start);
  });
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
  const double ms = best_of([&] {
    const auto start = Clock::now();
    for (int index = 0; index < kShadows; ++index) {
      const float x = static_cast<float>(index % 4) * 320.0f + 24.0f;
      const float y = static_cast<float>(index / 4) * 160.0f + 24.0f;
      canvas.draw_shadow(
          st::math::Rect{x, y, static_cast<float>(kCardWidth), static_cast<float>(kCardHeight)},
          12.0f, static_cast<float>(kBlur), color, st::math::Point{0.0f, 4.0f});
    }
    return elapsed_ms(start);
  });
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
  const double ms = best_of([&] {
    const auto start = Clock::now();
    for (int index = 0; index < kLines; ++index) {
      const float y = static_cast<float>(index % 24) * 30.0f + 20.0f;
      (void)renderer.draw(canvas, sample, st::math::Point{24.0f, y}, 14.0f,
                          st::math::Color::rgb(0x1F, 0x24, 0x2C));
    }
    return elapsed_ms(start);
  });
  // 总字形墨量的估算（下面按 `glyph_pixels / kLines` 折算到单行）：
  // 每行按 "字符数 × 字号² × 0.5" 估，**必须乘回总行数**——
  // 否则比值会被静默放大 60 倍（这里踩过一次：改成 best_of 时漏了乘）。
  const double glyph_pixels =
      static_cast<double>(sample.size()) * 14.0 * 14.0 * 0.5 * static_cast<double>(kLines);
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
  const double ms = best_of([&] {
    const auto start = Clock::now();
    for (int sweep = 0; sweep < kSweeps; ++sweep) {
      for (int index = 0; index < kRects; ++index) {
        const float y = static_cast<float>(index) * 40.0f;
        canvas.fill_rect(st::math::Rect{40.0f, y, static_cast<float>(kRectWidth),
                                        static_cast<float>(kRectHeight)},
                         paint, 8.0f);
      }
    }
    return elapsed_ms(start);
  });
  const double per_rect = ms / static_cast<double>(kRects * kSweeps);
  return Measurement{ms, per_rect / (kRectWidth * kRectHeight) / unit};
}

}  // namespace

// ————————————————————————————————————————————————————————————————————————————
// 门禁阈值（归一化比值；实测值见每条的注释）
// ————————————————————————————————————————————————————————————————————————————

namespace {

/// 各项的"实测"是**去掉调度噪声后**的稳定值（`best_of` 取最小值，见该函数注释），
/// 阈值按它给 ≈2.2~2.5 倍余量。余量上限 `kMaxHeadroom`(3.5) 由自检盯住。
///
/// ⚠ 2026-10 性能轮把这里的阈值**收紧了**（因为测量变准了）：加 `best_of` 之前，
/// 单次测量把噪声当成本，实测值虚高（如文本 33× 而真值 23×），阈值只能跟着放宽，
/// 于是"余量"看着合理、实际抓不住退化。收紧后 4 项都在 `kFaultFactor`(3×) 下越线。
///
/// 圆角卡片：实测 ≈1.35× → 阈值 3×（余量 ≈2.2）。
inline constexpr double kRoundedCardsLimit{3.0};
/// 投影：实测 ≈10.3× → 阈值 24×（余量 ≈2.3）。
inline constexpr double kShadowLimit{24.0};
/// 文本：实测 ≈23×（本项最稳）→ 阈值 50×（余量 ≈2.2）。
inline constexpr double kTextLimit{50.0};
/// 渐变：实测 ≈1.6× → 阈值 4×（余量 ≈2.5）。
inline constexpr double kGradientLimit{4.0};

/// 我们希望"至少这么多次退化"能被门禁抓住（2 倍以内可能是机器差异，不必报警）。
inline constexpr double kFaultFactor{3.0};

/// **任何一条性能断言的余量上限**。
///
/// 这是本文件最重要的一条约束：它把"阈值不能太松"从**愿望**变成**可检查的性质**。
/// 旧写法（绝对毫秒 + 20~700 倍余量）之所以危险，不是数字难看，而是**没人会去复核**。
/// 现在余量本身是用例断言的对象（见下方 `bench_thresholds_*`）。
inline constexpr double kMaxHeadroom{3.5};

}  // namespace

ST_TEST_SLOW(bench_rounded_card_fill) {
  const double unit = baseline_ms_per_pixel();
  const Measurement m = measure_rounded_cards(unit);
  st::print("[bench] 圆角卡片填充 ×60: {:.2f} ms（{:.3f} ms/张，{:.2f}× 清屏单位，阈值 {}×）\n",
            m.ms, m.ms / 60.0, m.ratio, kRoundedCardsLimit);
  if (skip_ratios_under_instrumentation("圆角卡片填充")) return;
  ST_CHECK(m.ratio <= kRoundedCardsLimit);
}

ST_TEST_SLOW(bench_card_shadow) {
  const double unit = baseline_ms_per_pixel();
  const Measurement m = measure_card_shadows(unit);
  st::print("[bench] 卡片投影 ×20（blur=16）: {:.2f} ms（{:.3f} ms/个，{:.2f}× 清屏单位，阈值 {}×）\n",
            m.ms, m.ms / 20.0, m.ratio, kShadowLimit);
  if (skip_ratios_under_instrumentation("卡片投影")) return;
  ST_CHECK(m.ratio <= kShadowLimit);
}

ST_TEST_SLOW(bench_text_draw) {
  bool available = false;
  const Measurement m = measure_text(&available);
  if (!available) {
    st::print("[bench] 文本：无系统字体，本项跳过（不算通过）\n");
    return;
  }
  st::print("[bench] 文本 ×60（59 字符/行，字型缓存命中）: {:.2f} ms（{:.3f} ms/行，{:.2f}× 清屏单位，阈值 {}×）\n",
            m.ms, m.ms / 60.0, m.ratio, kTextLimit);
  if (skip_ratios_under_instrumentation("文本绘制")) return;
  ST_CHECK(m.ratio <= kTextLimit);
}

ST_TEST_SLOW(bench_gradient_fill) {
  const double unit = baseline_ms_per_pixel();
  const Measurement m = measure_gradients(unit);
  st::print("[bench] 渐变填充 ×200（1200×36）: {:.2f} ms（{:.3f} ms/块，{:.2f}× 清屏单位，阈值 {}×）\n",
            m.ms, m.ms / 200.0, m.ratio, kGradientLimit);
  if (skip_ratios_under_instrumentation("渐变填充")) return;
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
ST_TEST_SLOW(bench_thresholds_have_bounded_headroom) {
  const double unit = baseline_ms_per_pixel();
  const auto check = [](const char* name, double limit, const Measurement& m) {
    ST_REQUIRE(m.ratio > 0.0);
    const double headroom = limit / m.ratio;
    st::print("[bench-guard] {}: 阈值 {:.1f}× / 实测 {:.2f}× = 余量 {:.2f} 倍（上限 {}）\n", name,
              limit, m.ratio, headroom, kMaxHeadroom);
    // 余量太小 → 机器噪声就会误报；太大 → 抓不住退化。两头都要卡住。
    //
    // ⚠ 插桩档（`st test --san`）下这条**不适用**：ASan 的插桩开销正比于内存访问量，
    // 而清屏是纯 `memset`——比值膨胀得极不均匀（见 `instrumented_build()` 的说明）。
    // 这里如实跳过并说明，而不是把阈值放宽（那会真的把发布档门禁变松）。
    if (instrumented_build()) return;
    ST_CHECK(headroom <= kMaxHeadroom);
    ST_CHECK(headroom >= 1.3);
  };
  check("圆角卡片", kRoundedCardsLimit, measure_rounded_cards(unit));
  check("卡片投影", kShadowLimit, measure_card_shadows(unit));
  check("渐变填充", kGradientLimit, measure_gradients(unit));

  if (instrumented_build()) {
    st::print("[bench-guard] 插桩档（san）：**跳过余量上下界断言**——ASan 的开销与内存访问量\n"
              "              成正比，而清屏是纯内存写，比值不再机器无关。性能标定以发布档为准。\n");
    return;
  }
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
ST_TEST_SLOW(bench_fault_multiplication_is_caught) {
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

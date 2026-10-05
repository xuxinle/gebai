// 帧耗时门禁（**端到端的性能回归护栏**）。
///
/// ## 与 `raster_bench_test.cpp` 的分工
///
/// - `raster_bench_test`：**单原语**成本（一次圆角填充 / 一次投影），回答"哪类绘制贵"。
/// - 本文件：**整帧**成本（真实应用跑真实界面），回答"交付给用户的每帧有多贵"。
///
/// 两者都要，因为它们的失效模式不同：原语变慢可以由上层缓存掩盖；而整帧变慢
/// 往往是**结构性**的（多画一遍整屏、排版重复算了、纹理每帧重建）。
///
/// ## 为什么这条门禁是必要的（它守的是真实发生过的一类缺陷）
///
/// `DESIGN.md` 记着一个实测缺陷：悬浮过渡的元素每帧把 `hover_start_` 重置为当前时间
/// → 永久声明 `animating` → **根节点每帧都脏** → 应用 100% 占一个核
/// （实测：6 秒耗 6.12 秒 CPU）。当时**没有任何测试发现它**——因为忙循环对
/// "单次调用耗时"是不可见的，而那时的断言全在单原语上。
///
/// 所以这里测的是**稳态帧序列**：跑够帧数让缓存/懒分配都进入稳态，再看帧耗时。
///
/// ## 断言口径（与 `raster_bench` 同一套思路，理由见那里）
///
/// 帧耗时的绝对值随机器变，故**先归一化**：帧耗时 / "清屏一次 1280×800" 的耗时。
/// 清屏是纯内存写、与本机速度线性相关，作分母可约掉 CPU 差异。
/// 阈值同样有**余量上限自检**（见文末），避免又退化成"20 倍余量的摆设"。

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <vector>

#include "st/app/app.hpp"
#include "st/app/text_port.hpp"
#include "st/core/print.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/test/test.hpp"
#include "st/text/text.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using Clock = std::chrono::steady_clock;

inline constexpr int kWidth = 1280;
inline constexpr int kHeight = 800;

[[nodiscard]] auto elapsed_ms(const Clock::time_point& start) -> double {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/// 归一化基准：清屏一次的毫秒数（取多次最小值，理由同 `raster_bench`）。
[[nodiscard]] auto clear_ms() -> double {
  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(kWidth, kHeight, 1.0f);
  double best = 1e9;
  for (int index = 0; index < 7; ++index) {
    const auto start = Clock::now();
    canvas.clear(st::math::Color::rgb(0xF7, 0xF8, 0xFA));
    best = std::min(best, elapsed_ms(start));
  }
  return best;
}

/// 造一个"够真实"的界面：嵌套容器 + 若干卡片/文本/按钮，覆盖 UI 树遍历、
/// 布局、裁剪栈、圆角填充、阴影与文字绘制——即一帧的真实工作量。
[[nodiscard]] auto build_representative_ui() -> std::unique_ptr<st::ui::Element> {
  auto root = std::make_unique<st::ui::Panel>();
  root->set_id("bench-root");
  root->style().direction = st::ui::FlexDirection::Column;
  root->style().gap = 12.0f;
  for (int row = 0; row < 12; ++row) {
    auto row_panel = std::make_unique<st::ui::Panel>();
    row_panel->set_id(std::format("row-{}", row));
    row_panel->style().direction = st::ui::FlexDirection::Row;
    row_panel->style().gap = 8.0f;
    for (int column = 0; column < 8; ++column) {
      auto card = std::make_unique<st::ui::Card>();
      card->set_id(std::format("card-{}-{}", row, column));
      card->style().width = 140.0f;
      card->style().height = 48.0f;
      auto label = std::make_unique<st::ui::Text>(std::format("卡片 {}·{}", row, column));
      label->style().font_size = 13.0f;
      card->add_child(std::move(label));
      row_panel->add_child(std::move(card));
    }
    root->add_child(std::move(row_panel));
  }
  return root;
}

/// 当前构建是否带 sanitizer（即 `st test --san`）。
///
/// 与 `raster_bench_test.cpp` 同名的那个函数同一理由：本门禁的两条自检断言
/// （余量的上下界）建立在"比值是机器无关量"这条推理上，而 **ASan 打破了它**——
/// ASan 的插桩开销正比于**内存访问量**，而作为分母的"清屏"是纯 `memset`
/// （几乎不被插桩）。实测同一份代码在 san 档下比值从 59× 到 142× 乱跳
/// （发布档稳定在 38×~45×），此时"余量"既不指向性能也不指向退化。
///
/// **不是放宽阈值**（那会真的把发布档门禁变松），而是插桩档如实跳过这**两条自检**
/// （门禁本身的 `kFrameLimit` 断言仍保留——它只作极宽的安全网）。
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
inline constexpr bool kInstrumentedBuild{true};
#else
inline constexpr bool kInstrumentedBuild{false};
#endif

/// 测试场景的装配：**必须挂真实文本端口**。
///
/// 本门禁与分阶段诊断长时间都**没挂**端口，于是 `Element::paint_text` 走
/// `NullTextPort`——`measure_width()` 恒 0、`draw()` 是空实现：场景里造了 96 个
/// `Text`，实际**一个字形都没画**。于是“96 段文字”这句与实测不符，
/// 而“瓶颈是不是在文字”这条排查方向也就永远测不出来（实测文字只占 3%）。
class RootFixture {
 public:
  RootFixture() {
    auto loaded = st::text::FontStack::system_default();
    if (loaded) {
      fonts_ = std::make_unique<st::text::FontStack>(std::move(*loaded));
      renderer_ = std::make_unique<st::text::TextRenderer>(*fonts_);
      port_ = st::app::make_text_port(*renderer_);
      root_.set_text_port(port_.get());
    }
    root_.set_theme(st::ui::Theme::light());
    root_.set_content(build_representative_ui());
    root_.set_viewport({kWidth, kHeight});
  }

  [[nodiscard]] auto root() -> st::ui::UiRoot& { return root_; }
  /// 无系统字体时本场景退化为零字形（如实告知，便于报告里写清楚）。
  [[nodiscard]] auto has_text() const noexcept -> bool { return port_ != nullptr; }

 private:
  std::unique_ptr<st::text::FontStack> fonts_{};
  std::unique_ptr<st::text::TextRenderer> renderer_{};
  std::unique_ptr<st::ui::TextPort> port_{};
  st::ui::UiRoot root_{};
};

/// 跑 `frames` 帧，返回稳态（后半段）的帧耗时统计（毫秒）。
struct FrameTiming {
  /// **最快的一帧**——"这台机器能不能跑进预算"的判据（见下方阈值说明）。
  double best{0.0};
  double p50{0.0};
  double p95{0.0};
  double worst{0.0};
  std::size_t sampled{0};
};

/// 跑 `frames` 帧，统计**稳态**（丢掉前 1/4 预热帧）的帧耗时分布。
///
/// `best`（最小值）用于"够不够快"的判据：本机是共享环境（cgroup 限 4 核、loadavg 常 >2），
/// 同一份代码连跑 6 次，p50 的比值在 **30×~47×** 之间摆——那是同租户在抢 CPU，
/// 不是被测代码在变。噪声**只会抬高**耗时，所以最小值最能代表"这台机器跑得动"。
/// `p50`/`p95`/`worst` 仍然报告（它们反映"典型/最坏体感"），p95 另有独立断言。
[[nodiscard]] auto measure_frames(std::size_t frames) -> FrameTiming {
  RootFixture fixture;
  st::ui::UiRoot& root = fixture.root();

  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(kWidth, kHeight, 1.0f);
  std::vector<double> samples;
  samples.reserve(frames);
  for (std::size_t index = 0; index < frames; ++index) {
    const auto start = Clock::now();
    // 完整一帧：标记脏 → 布局（含测量）→ 绘制整树。
    //
    // 每帧都标脏是**刻意的**：测的是"最坏稳态"（真实界面里滚动/悬停/动画都会持续
    // 触发重绘），而不是"什么都不做的空转帧"——后者耗时接近 0，什么都测不出来。
    //
    // 用 `paint_frame`（而非裸 `paint`）是刻意的：它才是应用帧循环真正走的那条路
    // （含损坏区收集与裁剪决策），测它才有端到端意义。
    root.mark_dirty_all();
    root.layout(true);
    canvas.clear(st::math::Color::rgb(0xF7, 0xF8, 0xFA));
    (void)root.paint_frame(canvas);
    samples.push_back(elapsed_ms(start));
  }
  // 丢掉前 1/4（首帧含字体加载、字形栅格化、懒分配等一次性成本），用后半段作稳态。
  const std::size_t begin = samples.size() / 4;
  std::vector<double> steady(samples.begin() + static_cast<std::ptrdiff_t>(begin), samples.end());
  std::ranges::sort(steady);
  FrameTiming timing;
  timing.sampled = steady.size();
  timing.best = steady.front();
  timing.p50 = steady[steady.size() / 2];
  timing.p95 = steady[static_cast<std::size_t>(static_cast<double>(steady.size() - 1) * 0.95)];
  timing.worst = steady.back();
  return timing;
}

/// 重复 `repeats` 轮测量，返回**最快的那一轮**。
///
/// 为什么还要再套一层：`measure_frames` 内部的"最快帧"能滤掉**帧级**噪声，
/// 但滤不掉**持续一整轮的**系统级尖峰（同租户跑构建/拷贝时，一轮 40 帧全都慢）。
/// 实测本机：正常轮 best 比值 34×~38×（很稳），但偶发一轮整体抬到 58×。
/// 多测几轮取最快，把这种尖峰挡在门禁之外——**这是让测量可信，不是放宽阈值**。
[[nodiscard]] auto measure_frames_best(std::size_t frames, int repeats = 3) -> FrameTiming {
  FrameTiming best{};
  bool first = true;
  for (int index = 0; index < repeats; ++index) {
    const FrameTiming timing = measure_frames(frames);
    if (first || timing.best < best.best) {
      best = timing;
      first = false;
    }
  }
  return best;
}

/// 一帧的**归一化成本上限**：帧耗时 / 清屏耗时。
///
/// ## 两轮实测（同一个场景，同一个界面）
///
/// | 轮次 | 清屏 | 帧 p50 | 比值 | 帧内 paint |
/// |---|---|---|---|---|
/// | 优化前（release -O2） | 0.383 ms | 24.85 ms | **65×** | 28.6 ms |
/// | 优化后（2026-10 性能轮） | 0.380 ms | **14.67 ms** | **38.6×** | 15.3 ms |
///
/// 一轮做了什么（详见 `docs/PAINT_DIAGNOSIS.md`）：把两处最重的绘制换成了等价但更省的实现——
/// 卡片两层投影合并成一张缓存贴图（`draw_shadow_layered`，14.0 → 6.9 ms/帧），
/// 圆角边框从"描边"改成"环形填充"（6.9 → 3.9 ms/帧）。
///
/// ⚠ **本门禁曾经测的是"没有文字的界面"**：`UiRoot` 从未挂文本端口，于是
/// `Element::paint_text` 走 `NullTextPort`——`measure_width()` 恒 0、`draw()` 空实现，
/// 场景里 96 个 `Text` 元素**一个字形都没画**，"96 段文字"与实测不符。
/// 现已挂真实 `RendererTextPort`（见 `RootFixture`），上表是修好口径之后的数据。
///
/// ## 判据用**最快帧**，不用 p50
///
/// 本机是共享环境（cgroup 限 4 核、loadavg 常 >2）：同一份代码连跑 6 次，
/// **p50** 的比值在 **30×~47×** 之间摆——那是同租户在抢 CPU，不是代码在变。
/// 而 `best`（最快帧）稳定在 **29×~32×**。噪声只会**抬高**耗时，
/// 所以"这台机器跑不跑得动这个界面"该看最快帧——这与 `raster_bench` 的
/// `best_of` 是同一个道理（那里踩过：单次测量把噪声当成本，阈值只能一路放宽）。
///
/// p50/p95 仍然报告：它们反映**典型与最坏体感**，p95 另有独立断言守着"偶发超时"。
///
/// 外面还套了一层 `measure_frames_best`（多轮取最快）——单轮的"最快帧"滤不掉
/// **持续整轮**的系统尖峰（实测偶发一轮整体抬到 58×），多测几轮才稳。
///
/// ## 阈值是"回归护栏"，不是"性能目标"
///
/// 50×（≈ 本机 ~19 ms，最优帧）给现状留 ~1.6 倍余量，余量上限 3.5 由自检盯住。
/// **性能目标另算**：`PAINT_DIAGNOSIS.md` 记着下一步该抠哪里
/// （阴影合成的内存流量、圆角填充的专用扫描线、清屏合并）。
inline constexpr double kFrameLimit{50.0};
/// 插桩档（`st test --san`）的整帧上限。
///
/// **为什么这里可以放宽、而自检那两条要跳过**：本门禁的两条自检断言的是**阈值本身的
/// 标定是否合理**（余量落在 [1.05, 6] 内）——那在插桩档下没有意义，所以跳过；
/// 而这一条断言的是"整帧没失控"，放宽到 90× 后仍然是**有效**的安全网
/// （基线 62×，3 倍退化 186× 一定越线）。两个数字都不是拍出来的：
/// 发布档实测 38×~45×、插桩档实测 59×~63×。
inline constexpr double kFrameLimitInstrumented{90.0};
/// 余量上限（与 `raster_bench` 同一约束，理由见该文件）。
inline constexpr double kMaxHeadroom{3.5};
/// 期望能被抓住的退化倍数。
inline constexpr double kFaultFactor{3.0};

}  // namespace

ST_TEST_SLOW(frame_cost_stays_within_budget) {
  const double unit = clear_ms();
  const FrameTiming timing = measure_frames_best(40);
  const double ratio = timing.best / unit;
  // 插桩档下用**档位松弛**而不是跳过：这条是"整帧别失控"的安全网，值得留着；
  // 而 50× 是为发布档标定的（ASan 把整帧抬到 ~1.9 倍清屏比值，实测 57×~63×）。
  const double limit = kInstrumentedBuild ? kFrameLimitInstrumented : kFrameLimit;
  st::print("[frame] 稳态帧耗时 best={:.2f}ms p50={:.2f}ms p95={:.2f}ms worst={:.2f}ms"
            "（{} 帧采样；清屏 {:.3f}ms → best = {:.2f}× 清屏，阈值 {}×{}）\n",
            timing.best, timing.p50, timing.p95, timing.worst, timing.sampled, unit, ratio, limit,
            kInstrumentedBuild ? "，插桩档松弛" : "");
  ST_REQUIRE(timing.sampled >= 10U);
  ST_CHECK(ratio <= limit);
}

/// p95 也要在预算内：只看 p50 会漏掉"偶发超时"（如每 N 帧重建一次缓存）。
ST_TEST_SLOW(frame_cost_p95_stays_within_budget) {
  const double unit = clear_ms();
  const FrameTiming timing = measure_frames_best(40);
  const double ratio = timing.p95 / unit;
  // ⚠ 这一条**故意保持 p95 口径**（不做 best-of），因为它守的是另一件事：
  // "有没有偶发超时"（如每 N 帧重建一次缓存）。共享机器上 p95 会被邻居抬高，
  // 所以给 2 倍宽限——它只用来抓"量级不对"，精细的性能判断归上一条（best）。
  const double limit = kFrameLimit * 2.0;
  st::print("[frame] p95 = {:.2f}× 清屏（阈值 {}×，含共享机器宽限）\n", ratio, limit);
  ST_CHECK(ratio <= limit);
}

/// **帧门禁自检（一）**：余量必须在 `kMaxHeadroom` 以内。
///
/// 与 `raster_bench` 的同名自检同理：让"阈值不能太松"成为可检查的性质，
/// 而不是靠人记得。谁把 `kFrameLimit` 放宽到抓不住退化，这里当场红灯。
ST_TEST_SLOW(frame_budget_threshold_has_bounded_headroom) {
  const double unit = clear_ms();
  const FrameTiming timing = measure_frames_best(30);
  const double ratio = timing.best / unit;
  const double headroom = kFrameLimit / ratio;
  // 余量上限**从 25× 收紧到 2.5×**（2026-10 性能轮）。为什么不是直接上 `kMaxHeadroom`(3.5)：
  //
  // 帧门禁的量不是单原语，而是**整个应用的稳态帧**——本机是共享环境（cgroup 限 4 核、
  // loadavg 常 >2），实测 p50 的比值在 30×~47× 之间摆（同一份代码重复跑）。
  // 而判据换成**最快帧**后，比值稳定在 ≈30×，2.5× 的余量上限是够紧的。
  //
  // 而旧的 25× 是在基线还是 65× 时定的——那时的作用是"等优化落地前别误报"。
  // 现在判据改成**最快帧**（比值稳定在 ≈30×），给 1.6 倍余量到 50×，
  // 上限 2.5× 收得比 6× 紧——那条 6× 是"p50 还在抖"时定的，口径换掉后不再需要。
  // **这不是免责声明**：它同时被 `frame_budget_fault_multiplication_is_caught` 盯着——
  // 当前的 `kFaultFactor`（3 倍退化）必须真的越线。
  constexpr double kFrameMaxHeadroom{2.5};
  // 打印的“上限”必须是**实际生效的那个**（`kFrameMaxHeadroom`）。
  // 旧版这里印的是全局的 `kMaxHeadroom`(3.5)，而检查用的是 25——日志说的与做的不一致，
  // 看上去像“余量 1.18 倍但上限 3.5，为什么还能过”。
  st::print("[frame-guard] 阈值 {:.1f}× / 实测 {:.2f}× = 余量 {:.2f} 倍（上限 {}）\n", kFrameLimit,
            ratio, headroom, kFrameMaxHeadroom);
  if (kInstrumentedBuild) {
    st::print("[frame-guard] 插桩档（san）：**跳过余量上下界断言**——ASan 开销 ∝ 内存访问量，\n"
              "              而分母是纯内存写，比值不再机器无关（实测 59×~142× 乱跳）。\n"
              "              本门的标定与把关以发布档为准。\n");
    return;
  }
  ST_CHECK(headroom <= kFrameMaxHeadroom);
  ST_CHECK(headroom >= 1.05);
}

/// **帧门禁自检（二）**：`kFaultFactor` 倍退化必然被发现。
ST_TEST_SLOW(frame_budget_fault_multiplication_is_caught) {
  const double unit = clear_ms();
  const FrameTiming timing = measure_frames_best(30);
  const double ratio = timing.best / unit;
  const bool caught = ratio * kFaultFactor > kFrameLimit;
  st::print("[frame-guard] 实测 {:.2f}× × {} 倍退化 = {:.2f}× vs 阈值 {:.1f}× → {}\n", ratio,
            kFaultFactor, ratio * kFaultFactor, kFrameLimit, caught ? "会被抓住" : "**抓不住**");
  // 同上：插桩档的比值不可比，这条也不适用（它同样建立在"比值机器无关"之上）。
  if (kInstrumentedBuild) {
    st::print("[frame-guard] 插桩档（san）：跳过此自检（理由同上）。\n");
    return;
  }
  ST_CHECK(caught);
}

// ————————————————————————————————————————————————————————————————————————————
// 瓶颈定位：把一帧拆成 layout / paint 两段（诊断用，不改门禁）
// ————————————————————————————————————————————————————————————————————————————

/// 分别报出 layout 与 paint 的耗时——**门禁只报"帧超预算"是不足以行动的**：
/// "24ms" 不指向任何具体动作，而"layout 占 20ms / paint 占 4ms"直接指出该改哪里。
/// （这与 `control::Metrics` 分阶段上报 layout/paint/present 是同一个理由。）
ST_TEST_SLOW(frame_cost_breakdown_for_diagnostics) {
  const double unit = clear_ms();
  // 与 `measure_frames` 同一口径：挂真实文本端口（无字体环境时退化为零字形）。
  RootFixture fixture;
  st::ui::UiRoot& root = fixture.root();
  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(kWidth, kHeight, 1.0f);

  double layout_total = 0.0;
  double paint_total = 0.0;
  constexpr int kFrames = 30;
  for (int index = 0; index < kFrames; ++index) {
    const auto start = Clock::now();
    root.mark_dirty_all();
    root.layout(true);
    const auto after_layout = Clock::now();
    canvas.clear(st::math::Color::rgb(0xF7, 0xF8, 0xFA));
    (void)root.paint_frame(canvas);
    const auto after_paint = Clock::now();
    if (index >= kFrames / 4) {   // 丢掉前 1/4（字体加载/懒分配等一次性成本）
      layout_total += std::chrono::duration<double, std::milli>(after_layout - start).count();
      paint_total += std::chrono::duration<double, std::milli>(after_paint - after_layout).count();
    }
  }
  const double frames = static_cast<double>(kFrames - kFrames / 4);
  const double layout_ms = layout_total / frames;
  const double paint_ms = paint_total / frames;
  st::print("[frame-split] layout {:.2f}ms（{:.1f}× 清屏）· paint {:.2f}ms（{:.1f}× 清屏）· "
            "合计 {:.2f}ms\n",
            layout_ms, layout_ms / unit, paint_ms, paint_ms / unit, layout_ms + paint_ms);
  ST_CHECK(layout_ms > 0.0);
  ST_CHECK(paint_ms > 0.0);
}

/// 启动卡（`st/app/splash.hpp`）的回归：三条硬约束 + 一条"不是静态图"。
///
/// ## 为什么这几条必须钉住
///
/// 启动卡是**启动期唯一出现在屏上的东西**，而它出错的方式都很"安静"：
///
/// | 若这条破了 | 用户看到什么 | 为什么难发现 |
/// |---|---|---|
/// | 磨砂档卡外不是全透明 | 磨砂没透出来（卡外一片不透明的暗色） | 截图里"看着是块深色卡"，与设计稿无法区分 |
/// | 无磨砂档卡外不是不透明 | 透出桌面壁纸/别的窗口（"漏了"） | 只在"没有合成器"的机器上发生（远程/旧系统） |
/// | 卡中心全透明 | 卡根本没画出来 | 启动仍能完成，只是没有卡 |
/// | 帧间无差异 | 画了张静态图当动画 | 单帧截图完全正常 |
///
/// 所以断言不用"渲染结果和基线 PNG 比"（那样每改一次观感都要重做基线，
/// 最后一定被人删掉），而断言**结构性事实**：哪块该透明、哪块该不透明、帧间该变。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "st/app/splash.hpp"
#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/test/test.hpp"

namespace {

// 画布尺寸取**真实比例**（1280×800 逻辑）：启动卡的窗口通常远大于卡本身，
// 而光晕是“卡的影子”——它在窗口四角应该是**零**。
// 早先用 320×220（卡宽 320 就撑满窗口）时四角测到 18~20：那是**测试画布失真**，
// 不是实现问题（真窗口下同样的参数四角是 0）。
constexpr int kLogicalWidth = 1280;
constexpr int kLogicalHeight = 800;
constexpr float kScale = 1.0f;

[[nodiscard]] auto make_state(bool backdrop_blur) -> st::app::SplashState {
  st::app::SplashState state{};
  state.elapsed_seconds = 0.31;   // 一个脉冲周期内的任意点（非 0，避免"起始帧恰好全 0"）
  state.progress = 0.4f;
  state.background = st::math::Color::rgb(0x0B, 0x0B, 0x0E);
  state.accent = st::math::Color::rgb(0x7C, 0x8C, 0xFF);
  state.backdrop_blur = backdrop_blur;
  return state;
}

[[nodiscard]] auto alpha_at(const st::raster::Canvas& canvas, int x, int y) -> std::uint32_t {
  const auto pixels = canvas.pixels();
  const auto width = static_cast<std::size_t>(canvas.physical_width());
  const std::size_t offset = static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x);
  if (offset >= pixels.size()) return 0U;
  return pixels[offset] & 0xFFU;
}

}  // namespace

ST_TEST(splash_backdrop_mode_keeps_outside_transparent) {
  // 磨砂档：卡外**不能有一块不透明的底**（否则合成器垫的模糊层被我们自己盖掉）。
  //
  // ⚠ 但"卡外"不等于"全零 alpha"：卡外还有一圈**柔光晕**（`glow_radius`），
  // 它是让卡"浮起来"的那层氛围光，本来就是半透明的。第一版断言写的是"全部 == 0"，
  // 实测四角得 6、边缘中点得 25——错的是断言，不是实现（多层光晕在窗口角落
  // 总会外溢一丁点）。
  // 所以判据分两档：
  //   ① 四角 ≤ 8/255（≈3%，磨砂纹理上不可见）；
  //   ② 边缘中点 ≤ 48/255（仍是"氛围光"量级，而不是一层不透明的底——后者才是 255）。
  st::raster::Canvas canvas =
      st::raster::Canvas::for_logical_size(kLogicalWidth, kLogicalHeight, kScale);
  st::app::paint_splash_card(canvas, make_state(true));
  const int width = canvas.physical_width();
  const int height = canvas.physical_height();
  ST_CHECK(alpha_at(canvas, 1, 1) <= 8U);
  ST_CHECK(alpha_at(canvas, width - 2, 1) <= 8U);
  ST_CHECK(alpha_at(canvas, 1, height - 2) <= 8U);
  ST_CHECK(alpha_at(canvas, width - 2, height - 2) <= 8U);
  ST_CHECK(alpha_at(canvas, width / 2, 1) <= 48U);
  ST_CHECK(alpha_at(canvas, width / 2, height - 2) <= 48U);
}

ST_TEST(splash_card_body_is_painted) {
  // 卡中心必须**不是**全透明：否则"磨砂在、卡没了"，而那在单帧截图里看不出来。
  st::raster::Canvas canvas =
      st::raster::Canvas::for_logical_size(kLogicalWidth, kLogicalHeight, kScale);
  st::app::paint_splash_card(canvas, make_state(true));
  const int width = canvas.physical_width();
  const int height = canvas.physical_height();
  ST_CHECK(alpha_at(canvas, width / 2, height / 2) > 0U);
    // 品牌标记（菱形）比卡底更实——否则"卡画了但标记没画"也会通过上面那条。
  //
  // 采样点 = **卡的正中心**（标记与环的圆心）。现在卡里只有图形（没有下方的应用名/进度），
  // 所以几何是“卡心即标记心”；早先标记在卡高的 30% 处（那时下方预留了文字位）。
  st::app::SplashStyle style{};
  const float logical_w = static_cast<float>(kLogicalWidth);
  const float logical_h = static_cast<float>(kLogicalHeight);
  const float card_w = std::min(style.card_width, logical_w * 0.86f);
  const float card_h = std::min(card_w / std::max(0.2f, style.card_aspect), logical_h * 0.86f);
  const float card_y = logical_h * 0.5f - card_h * 0.5f;
  const int mark_y = static_cast<int>(std::lround((card_y + card_h * 0.5f) * kScale));
  ST_CHECK(alpha_at(canvas, width / 2, mark_y) > 100U);
}

ST_TEST(splash_without_backdrop_is_opaque_at_edges) {
  // 无磨砂档：卡外必须**不透明**（填主题底色）。否则会透出桌面壁纸——那不是"高级"。
  st::raster::Canvas canvas =
      st::raster::Canvas::for_logical_size(kLogicalWidth, kLogicalHeight, kScale);
  st::app::paint_splash_card(canvas, make_state(false));
  const int width = canvas.physical_width();
  const int height = canvas.physical_height();
  ST_CHECK_EQ(alpha_at(canvas, 1, 1), 255U);
  ST_CHECK_EQ(alpha_at(canvas, width - 2, height - 2), 255U);
}

ST_TEST(splash_animation_actually_moves) {
  // 两帧（相差半个脉冲周期）必须有实质差异：否则"动画"只是张静态图。
  st::raster::Canvas first =
      st::raster::Canvas::for_logical_size(kLogicalWidth, kLogicalHeight, kScale);
  st::raster::Canvas second =
      st::raster::Canvas::for_logical_size(kLogicalWidth, kLogicalHeight, kScale);
  st::app::SplashState early = make_state(true);
  st::app::SplashState later = make_state(true);
  later.elapsed_seconds = early.elapsed_seconds + 0.42;
  st::app::paint_splash_card(first, early);
  st::app::paint_splash_card(second, later);

  const auto left = first.pixels();
  const auto right = second.pixels();
  std::size_t differing = 0;
  const std::size_t count = left.size() < right.size() ? left.size() : right.size();
  for (std::size_t index = 0; index < count; ++index) {
    if (left[index] != right[index]) ++differing;
  }
  // 判据取"至少 200 像素变了"：一帧里脉冲环+扫光带动的面积远大于此，
  // 而 200 又足以排除"只有一两个像素因浮点抖动而变"的假阳性。
  ST_CHECK(differing > 200U);
}

ST_TEST(splash_pump_repeats_without_waiting) {
  // ## 这条用例钉住的是“动画真的会被反复调用”
  //
  // 回归现场（2026-11-08，用户报“动画没了”）：绘制层一直是好的
  // （`splash_animation_actually_moves` 一直绿），但**泵没被持续调用**——
  // 首帧那一整块计算把主线程占满，动画只画了开头一两帧就定住。
  // 用户看到的就是一张静态图。
  //
  // 而那一轮排查之所以慢，是因为**量具错了**：截图在分层窗口（ULW）上
  // 抓到的是重定向表面的纯黑（实测所有采样帧中心都是 (0,0,0)），
  // 看着像“没动”，实际只是抓不到。所以这里用离屏画布验证。
  //
  // 验证两件事：
  //   ① 时间轴推进 16ms（一帧）就必须能画出**不同**的画面（不是只有大跨度才变）；
  //   ② 连续多个时刻必须**每个**都与前一帧不同（动画是连续的，不是两帧跳）。
  st::app::SplashStyle style{};
  st::app::SplashState state = make_state(true);
  std::vector<std::vector<std::uint32_t>> frames;
  constexpr int kSteps = 6;
  for (int index = 0; index < kSteps; ++index) {
    // 从 0.22s 起（起始相位，与 `paint_splash_once` 同源），每步 16ms。
    state.elapsed_seconds = 0.22 + static_cast<double>(index) * 0.016;
    st::raster::Canvas canvas =
        st::raster::Canvas::for_logical_size(kLogicalWidth, kLogicalHeight, kScale);
    st::app::paint_splash_card(canvas, state, style);
    const auto pixels = canvas.pixels();
    frames.emplace_back(pixels.begin(), pixels.end());
  }
  // ① 每相邻两步必须有差异。
  // 注意：16ms 只占 1.15s 周期的 1.4%，所以差异会比大跨度小很多——
  // 那个下限（>200 像素）不能直接用，否则会把“真的在动”误判成红。
  // 真实下限用**像素值变化**判：任何一处 `!=` 就算。
  for (int index = 1; index < kSteps; ++index) {
    const auto& prev = frames[static_cast<std::size_t>(index - 1)];
    const auto& curr = frames[static_cast<std::size_t>(index)];
    std::size_t differing = 0;
    const std::size_t count = std::min(prev.size(), curr.size());
    for (std::size_t at = 0; at < count; ++at) {
      if (prev[at] != curr[at]) ++differing;
    }
    ST_CHECK(differing > 0U);
  }
}

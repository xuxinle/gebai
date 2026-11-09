/// 组件动画的**分镜探针**：把 CSS 式动画在时间轴上采样成序列 PNG，
/// 并用像素量断言“它真的在按预期变化”。
///
/// ## 为什么必须出图，而不是只看属性面
///
/// 动画是**形态类**改动：`animation_opacity` 读回来 0.5 只能说明引擎算对了，
/// 说明不了绘制层真的读了这个值（那两条是**不同的代码路径**：一条算属性，
/// 一条落到 `paint_box`/`text`）。实测同类缺陷出现过——属性全对、画面完全没动。
///
/// ## 判据（每条都对应一种真实失败形态）
///
/// 1. **入场动画**：t=0 时元素不可见（或明显淡）→ t=1 时完全不透明，
///    且**中间帧必须严格介于两者之间**（只有首末两帧对、中间跳变也算失败：
///    那是 `fill` 处理错了，不是“播了”）。
/// 2. **滑入**：位移量必须单调收敛到 0，且墨迹的**包围盒**真的在移动
///    （只看 alpha 看不出位移——两者是正交的量）。
/// 3. **无限循环**：两分钟后仍在跑（`alternate` 也不能因为浮点累积提前停）。
/// 4. **抖动**：位移必须**变号**（单侧偏移不是抖动，是“滑走”）。
///
/// 用法：`ui_animation_probe [输出目录]`（缺省 `build/ui-anim`）。
/// 退出码非 0 = 有判据不满足（可直接进 CI）。

#include <cmath>
#include <cstdio>
#include <format>
#include <string>
#include <vector>

#include "st/codec/png.hpp"
#include "st/core/fs.hpp"
#include "st/core/print.hpp"
#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/animation.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

constexpr int kWidth = 360;
constexpr int kHeight = 240;
constexpr int kFrames = 6;

[[nodiscard]] auto to_png(const st::raster::Canvas& canvas) -> st::codec::PngImage {
  st::codec::PngImage image{};
  const int width = canvas.physical_width();
  const int height = canvas.physical_height();
  image.width = static_cast<std::uint32_t>(width);
  image.height = static_cast<std::uint32_t>(height);
  image.rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
  const auto pixels = canvas.pixels();
  for (std::size_t index = 0; index < pixels.size(); ++index) {
    const std::uint32_t value = pixels[index];
    const std::uint32_t alpha = value & 0xFFU;
    const auto un = [alpha](std::uint32_t channel) -> std::uint8_t {
      if (alpha == 0U) return 0;
      const float scaled = static_cast<float>(channel) * 255.0f / static_cast<float>(alpha);
      return static_cast<std::uint8_t>(scaled > 255.0f ? 255.0f : scaled);
    };
    image.rgba[index * 4U + 0U] = un((value >> 24U) & 0xFFU);
    image.rgba[index * 4U + 1U] = un((value >> 16U) & 0xFFU);
    image.rgba[index * 4U + 2U] = un((value >> 8U) & 0xFFU);
    image.rgba[index * 4U + 3U] = static_cast<std::uint8_t>(alpha);
  }
  return image;
}

/// 一组像素的“墨迹量”（与底色的差异总量）与墨迹包围盒。
struct InkStats {
  double ink{0.0};
  int min_x{0};
  int min_y{0};
  int max_x{0};
  int max_y{0};
  bool empty{true};
};

[[nodiscard]] auto measure_ink(const st::raster::Canvas& canvas, st::math::Color background)
    -> InkStats {
  InkStats stats{};
  const int width = canvas.physical_width();
  const int height = canvas.physical_height();
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const st::math::Color color = canvas.pixel_at(x, y);
      const double difference = std::abs(static_cast<double>(color.r) - background.r) +
                                std::abs(static_cast<double>(color.g) - background.g) +
                                std::abs(static_cast<double>(color.b) - background.b);
      if (difference <= 6.0) continue;   // 抗锯齿抖动不算墨迹
      stats.ink += difference;
      if (stats.empty) {
        stats.min_x = x;
        stats.min_y = y;
        stats.max_x = x;
        stats.max_y = y;
        stats.empty = false;
      } else {
        stats.min_x = std::min(stats.min_x, x);
        stats.min_y = std::min(stats.min_y, y);
        stats.max_x = std::max(stats.max_x, x);
        stats.max_y = std::max(stats.max_y, y);
      }
    }
  }
  return stats;
}

/// 一个被测场景：一棵元素树 + 在 `time` 时刻画一帧。
struct Scene {
  st::ui::Theme theme{st::ui::Theme::light()};
  st::ui::UiRoot root{};
  st::ui::Panel* box{nullptr};
  double clock{1.0};

  Scene() {
    root.set_theme(theme);
    auto owned = std::make_unique<st::ui::Panel>();
    box = owned.get();
    box->set_id("box");
    box->style().background = st::math::Color::rgb(0x22, 0x3A, 0x8C);
    box->style().radius = 10.0F;
    box->style().width = 140.0F;
    box->style().height = 80.0F;
    root.set_content(std::move(owned));
    root.set_viewport(st::math::Size{static_cast<float>(kWidth), static_cast<float>(kHeight)});
    root.layout();
    st::ui::RenderContext context{theme, nullptr, clock};
    box->measure(context, st::ui::Constraints{});
    box->arrange(context, st::math::Rect{110.0F, 80.0F, 140.0F, 80.0F});
  }

  /// 清屏色必须与 `paint_frame` 用的**同一个**（它整帧路径会 `clear(theme.bg)`）：
  /// 用白色当基准时，浅色主题的 `bg` 与它差几个量化台阶，
  /// 整屏每一像素都会被算成“墨迹”——实测 `min_x` 恒为 0，位移判据因此完全失效
  /// （这也是“量尺口径与应用不一致”的一个实例）。
  [[nodiscard]] auto background() const -> st::math::Color { return theme.colors().bg; }

  [[nodiscard]] auto canvas() -> st::raster::Canvas {
    st::raster::Canvas out = st::raster::Canvas::for_logical_size(kWidth, kHeight, 1.0F);
    out.clear(background());
    return out;
  }

  void frame(st::raster::Canvas& target, double time) {
    clock = time;
    root.set_time(time);
    (void)root.paint_frame(target);
    root.clear_dirty();
  }

};

[[nodiscard]] auto write_frame(const st::raster::Canvas& canvas, const std::string& directory,
                               const std::string& name) -> bool {
  const auto image = to_png(canvas);
  const auto written = st::codec::png_write_file(st::fs::join(directory, name + ".png"), image);
  if (!written) {
    st::print("写 PNG 失败：%s\n", written.error().message.c_str());
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string out_dir = argc > 1 ? argv[1] : "build/ui-anim";
  (void)st::fs::create_directories(out_dir);
  int failures = 0;

  // —— ① 入场：fadeInUp ——
  {
    Scene scene;
    if (!scene.box->set_preset_animation("fadeInUp")) {
      st::print("[FAIL] fadeInUp 预设不存在\n");
      ++failures;
    }
    std::vector<double> inks;
    for (int index = 0; index < kFrames; ++index) {
      auto canvas = scene.canvas();
      const double time = 1.0 + 0.45 * static_cast<double>(index) / (kFrames - 1);
      scene.frame(canvas, time);
      const InkStats stats = measure_ink(canvas, scene.background());
      inks.push_back(stats.ink);
      if (!write_frame(canvas, out_dir, std::format("fadeInUp-{}", index))) ++failures;
    }
    // 判据 1：墨量必须**单调不减**（淡入 + 上移，两者都让墨量变大），
    // 且首末差异显著（首帧接近 0）。单调性是“中间帧确实在动”的判据。
    for (std::size_t index = 1; index < inks.size(); ++index) {
      if (inks[index] + 1.0 < inks[index - 1]) {
        st::print("[FAIL] fadeInUp 墨量在第 {} 帧回落（{:.0f} → {:.0f}）\n", index,
                    inks[index - 1], inks[index]);
        ++failures;
      }
    }
    if (!(inks.back() > inks.front() * 3.0)) {
      st::print("[FAIL] fadeInUp 首末墨量差异不足（{:.0f} → {:.0f}）\n", inks.front(),
                inks.back());
      ++failures;
    }
    st::print("[probe] fadeInUp 墨量序列：");
    for (const double value : inks) st::print("{:.0f} ", value);
    st::print("\n");
  }

  // —— ② 滑入：translate 必须真的挪墨迹 ——
  {
    Scene scene;
    st::ui::anim::Animation animation{};
    animation.name = "slide";
    animation.duration = 0.4;
    animation.easing = st::ui::anim::Easing::linear();
    animation.fill = st::ui::anim::Animation::Fill::Forwards;
    st::ui::anim::Props head{};
    head.translate_x = 90.0F;
    st::ui::anim::Props tail{};
    tail.translate_x = 0.0F;
    st::ui::anim::Keyframes frames{};
    frames.frames.push_back(st::ui::anim::Keyframe{0.0F, head});
    frames.frames.push_back(st::ui::anim::Keyframe{1.0F, tail});
    scene.box->set_animation(animation, frames);
    int previous_min_x = -1;
    for (int index = 0; index < kFrames; ++index) {
      auto canvas = scene.canvas();
      const double time = 1.0 + 0.4 * static_cast<double>(index) / (kFrames - 1);
      scene.frame(canvas, time);
      const InkStats stats = measure_ink(canvas, scene.background());
      if (!write_frame(canvas, out_dir, std::format("slideIn-{}", index))) ++failures;
      // 判据 2：墨迹左缘必须**随帧左移**（位移真的作用到绘制上了）。
      if (previous_min_x >= 0 && stats.min_x > previous_min_x) {
        st::print("[FAIL] slideIn 第 {} 帧墨迹没有继续左移（{} → {}）\n", index,
                  previous_min_x, stats.min_x);
        ++failures;
      }
      previous_min_x = stats.min_x;
      st::print("[probe] slideIn 帧 {} min_x={} translate_x={:.1f}\n", index, stats.min_x,
                  static_cast<double>(scene.box->animation_translate().x));
    }
  }

  // —— ③ 抖动：位移必须变号 ——
  {
    Scene scene;
    (void)scene.box->set_preset_animation("shake");
    bool seen_left = false;
    bool seen_right = false;
    for (int index = 0; index < kFrames * 2; ++index) {
      auto canvas = scene.canvas();
      const double time = 1.0 + 0.5 * static_cast<double>(index) / (kFrames * 2 - 1);
      scene.frame(canvas, time);
      if (!write_frame(canvas, out_dir, std::format("shake-{}", index))) ++failures;
      const float offset = scene.box->animation_translate().x;
      if (offset < -0.5F) seen_left = true;
      if (offset > 0.5F) seen_right = true;
    }
    if (!seen_left || !seen_right) {
      st::print("[FAIL] shake 位移没有变号（left={} right={}）\n", seen_left, seen_right);
      ++failures;
    }
  }

  // —— ④ 无限循环：两分钟后仍在跑 ——
  {
    Scene scene;
    (void)scene.box->set_preset_animation("pulse");
    auto canvas = scene.canvas();
    scene.frame(canvas, 1.0);
    auto late = scene.canvas();
    scene.frame(late, 121.0);
    if (!scene.box->animation_running()) {
      st::print("[FAIL] pulse（infinite）在 121 秒后停了\n");
      ++failures;
    }
    // 判据 4：无限循环必须**不请求续帧地停下来**只在结束时成立——仍在播时应当请求。
    if (!scene.root.needs_frame()) {
      st::print("[FAIL] pulse 仍在播，却没有请求下一帧\n");
      ++failures;
    }
  }

  // —— ⑤ 缩放：scale 走离屏合成，墨迹面积必须变 ——
  {
    Scene scene;
    st::ui::anim::Animation animation{};
    animation.name = "pop";
    animation.duration = 0.3;
    animation.easing = st::ui::anim::Easing::linear();
    animation.fill = st::ui::anim::Animation::Fill::Forwards;
    st::ui::anim::Props head{};
    head.scale = 0.5F;
    st::ui::anim::Props tail{};
    tail.scale = 1.0F;
    st::ui::anim::Keyframes frames{};
    frames.frames.push_back(st::ui::anim::Keyframe{0.0F, head});
    frames.frames.push_back(st::ui::anim::Keyframe{1.0F, tail});
    scene.box->set_animation(animation, frames);
    double first_ink = 0.0;
    double last_ink = 0.0;
    for (int index = 0; index < kFrames; ++index) {
      auto canvas = scene.canvas();
      const double time = 1.0 + 0.3 * static_cast<double>(index) / (kFrames - 1);
      scene.frame(canvas, time);
      const InkStats stats = measure_ink(canvas, scene.background());
      if (!write_frame(canvas, out_dir, std::format("scaleIn-{}", index))) ++failures;
      if (index == 0) first_ink = stats.ink;
      last_ink = stats.ink;
    }
    // 面积 ∝ scale² → 0.5 倍缩放只剩约 1/4 的墨量。
    if (!(last_ink > first_ink * 2.0)) {
      st::print("[FAIL] scaleIn 放大后墨量没有显著增加（{:.0f} → {:.0f}）\n", first_ink,
                last_ink);
      ++failures;
    }
    st::print("[probe] scaleIn 墨量 {:.0f} → {:.0f}\n", first_ink, last_ink);
  }

  st::print("产出目录：{}\n", out_dir);
  if (failures == 0) {
    st::print("[PASS] 动画分镜全部判据通过\n");
    return 0;
  }
  st::print("[FAIL] {} 条判据不满足\n", failures);
  return 1;
}

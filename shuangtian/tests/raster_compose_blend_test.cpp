/// 图像合成的混合模式回归（`Canvas::draw_canvas`，即图层合成路径）。
///
/// ## 这组用例守的是什么
///
/// `draw_canvas` / `draw_canvas_at` 是**图层合成的唯一入口**——图像编辑器把
/// 一张图层的像素叠到目标缓冲上，全靠它。而它长期**完全忽略 `DrawOptions::blend`**：
/// 实现里写死 `over_premul(row[x], blended)`（= SrcOver）。
///
/// 后果的隐蔽程度值得记一笔：界面看不出异常——图层顺序对、不透明度生效、
/// 单层显示完全正常；**只有九种混合模式里的八种静默退化成普通叠加**。
/// 实测（镂月示例把九种模式各自导出后逐像素比对）：
/// `multiply` 的合成结果等于直接覆盖，即"乘算"这个功能从来没生效过。
///
/// 为什么单测没能拦住：既有的混合测试（`simd_blend_row_matches_scalar_reference`）
/// 测的是**行混合函数的 SIMD 与标量一致性**——两侧走同一个 `blend_pixel_premul`，
/// 于是"没传 mode"这个错误在两侧同样发生，比较依然通过。这是典型的
/// **桩把差异抹平了**：被测的是"谁把 mode 传给谁"，而参照实现与被测实现共用了
/// 同一个"忘记传"的路径。
///
/// 所以这里**不比对另一份实现**，而是直接比对**手算的期望值**（W3C 混合公式），
/// 并把"合成结果必须随 blend 变化"钉成断言。

#include <cstdint>
#include <vector>

#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/surface.hpp"
#include "st/test/test.hpp"

namespace {

using st::math::Color;
using st::math::Rect;
using st::raster::BlendMode;
using st::raster::Canvas;
using st::raster::DrawOptions;
using st::raster::Paint;

/// 一张 4×4 的纯色小图（避开边缘，采样点取中心）。
[[nodiscard]] auto solid(int size, Color color) -> Canvas {
  Canvas canvas{size, size, 1.0f};
  canvas.clear(color);
  return canvas;
}

/// 按混合模式把 `top` 叠到 `bottom` 上，返回中心像素。
[[nodiscard]] auto compose(int size, Color bottom, Color top, BlendMode mode) -> Color {
  Canvas target = solid(size, bottom);
  const Canvas layer = solid(size, top);
  DrawOptions options{};
  options.blend = mode;
  target.draw_canvas_at(layer, 0, 0, options);
  return target.pixel_at(size / 2, size / 2);
}

[[nodiscard]] auto near(Color a, Color expected, int tolerance) -> bool {
  const auto diff = [](std::uint8_t x, std::uint8_t y) {
    return x > y ? static_cast<int>(x) - static_cast<int>(y)
                 : static_cast<int>(y) - static_cast<int>(x);
  };
  return diff(a.r, expected.r) <= tolerance && diff(a.g, expected.g) <= tolerance &&
         diff(a.b, expected.b) <= tolerance;
}

}  // namespace

/// **合成必须尊重 `DrawOptions::blend`**（每档各按自己的公式）。
///
/// 期望值手算（底 `#33995F`，上 `#CC8080`，均为不透明）：
///   multiply = 逐通道相乘 → (0x33*0xCC, 0x99*0x80, 0x5F*0x80)/255
///   darken   = 逐通道取小 → (0x33, 0x80, 0x5F)
///   lighten  = 逐通道取大 → (0xCC, 0x99, 0x80)
/// 这三档的期望值**互不相同**，也就同时钉住了"模式确实被区分的"。
ST_TEST(canvas_compose_respects_blend_mode) {
  constexpr int kSize = 4;
  const Color bottom{0x33, 0x99, 0x5F, 255};
  const Color top{0xCC, 0x80, 0x80, 255};

  // ① 不透明上层 + Src：目标被完全替换。
  const Color src_result = compose(kSize, bottom, top, BlendMode::Src);
  ST_CHECK(near(src_result, top, 1));

  // ② Multiply：逐通道相乘（这是原先静默退化的那一档）。
  const Color multiply = compose(kSize, bottom, top, BlendMode::Multiply);
  const Color multiply_expected{
      static_cast<std::uint8_t>(0x33 * 0xCC / 255), static_cast<std::uint8_t>(0x99 * 0x80 / 255),
      static_cast<std::uint8_t>(0x5F * 0x80 / 255), 255};
  ST_CHECK(near(multiply, multiply_expected, 2));

  // ③ Darken / Lighten：逐通道取小 / 取大。两者互为对照——
  //    如果实现把 mode 丢了（退化成 SrcOver），这两档都会等于 `top`，断言当场红。
  const Color darken = compose(kSize, bottom, top, BlendMode::Darken);
  const Color darken_expected{0x33, 0x80, 0x5F, 255};
  ST_CHECK(near(darken, darken_expected, 1));

  const Color lighten = compose(kSize, bottom, top, BlendMode::Lighten);
  const Color lighten_expected{0xCC, 0x99, 0x80, 255};
  ST_CHECK(near(lighten, lighten_expected, 1));

  // ④ **区分性**：这三档结果两两不同。若实现丢了 mode，三者会全部等于 `top`。
  ST_CHECK(!near(darken, lighten, 1));
  ST_CHECK(!near(multiply, top, 1));
  ST_CHECK(!near(darken, top, 1));
}

/// **每档的合成结果都随模式变化**（结构性断言，与具体公式解耦）。
///
/// 与上一条互补：上一条钉住具体数值，这一条钉住"九档不全一样"——
/// 即便将来某档的公式被有意调整，只要它仍有区分性，本用例依然有效。
ST_TEST(canvas_compose_modes_are_distinct) {
  constexpr int kSize = 4;
  const Color bottom{0x33, 0x99, 0x5F, 255};
  const Color top{0xCC, 0x80, 0x80, 255};

  const BlendMode modes[] = {BlendMode::SrcOver, BlendMode::Src,     BlendMode::DstOver,
                             BlendMode::Multiply, BlendMode::Screen,  BlendMode::Overlay,
                             BlendMode::Darken,   BlendMode::Lighten, BlendMode::Add};
  std::vector<Color> results;
  results.reserve(std::size(modes));
  for (const BlendMode mode : modes) results.push_back(compose(kSize, bottom, top, mode));

  // 至少四组**互不相同**的结果（这九档里 SrcOver/Src 在不透明上层下相同，
  // 其余各不相同；取 4 作为保守下界，避免公式微调时误报）。
  int distinct_pairs = 0;
  for (std::size_t i = 0; i < results.size(); ++i) {
    for (std::size_t j = i + 1; j < results.size(); ++j) {
      if (!near(results[i], results[j], 1)) ++distinct_pairs;
    }
  }
  ST_CHECK(distinct_pairs >= 4);
}

/// 缩放合成（`draw_canvas` 的双线性路径）同样要尊重模式。
///
/// 为什么要单独测：`draw_canvas` 有两条路径——`1:1` 直贴与**缩放重采样**。
/// 实测缺陷在两条路径上是同源码行（都在采样后的混合那一步），但将来优化
/// 1:1 路径时很容易只改一处；把缩放的期望也钉住，改一处会当场红。
ST_TEST(canvas_scaled_compose_respects_blend_mode) {
  const Color bottom{0x33, 0x99, 0x5F, 255};
  const Color top{0xCC, 0x80, 0x80, 255};
  Canvas target = solid(8, bottom);
  const Canvas layer = solid(4, top);
  DrawOptions options{};
  options.blend = BlendMode::Darken;
  // 4×4 → 8×8：纯色图放大后中心仍是同一个颜色，不受插值影响。
  target.draw_canvas(layer, Rect{0.0f, 0.0f, 8.0f, 8.0f}, options);
  ST_CHECK(near(target.pixel_at(4, 4), Color{0x33, 0x80, 0x5F, 255}, 2));
}

/// 不透明度与混合模式**同时**生效（两件事不能互相盖掉）。
///
/// ⚠ 选色要小心：底白 + 顶黑，`multiply(白, 黑) = 黑`——乘法结果与透明度无关地
/// 会落在“黑”。第一版选的是**底黑 + 顶白**，那时乘法结果恒为黑、半透明叠黑
/// 仍是黑，断言“应当介于黑与白之间”**前提就不成立**（实测报假失败）。
ST_TEST(canvas_compose_opacity_and_blend_compose) {
  constexpr int kSize = 4;
  const Color white{0xFF, 0xFF, 0xFF, 255};
  const Color black{0x00, 0x00, 0x00, 255};

  const auto compose_with_opacity = [kSize, white, black](float opacity) {
    Canvas target = solid(kSize, white);
    const Canvas layer = solid(kSize, black);
    DrawOptions options{};
    options.blend = BlendMode::Multiply;
    options.opacity = opacity;
    target.draw_canvas_at(layer, 0, 0, options);
    return target.pixel_at(kSize / 2, kSize / 2);
  };

  // 不透明：乘法结果就是黑。
  ST_CHECK(near(compose_with_opacity(1.0f), black, 1));
  // 半透明：应当是“半途”——既不等于黑，也不等于白底。
  const Color half = compose_with_opacity(0.5f);
  ST_CHECK(half.r > 0x40U);
  ST_CHECK(half.r < 0xC0U);
  // 单调性：透明度越低越接近白底（这一条把“两件事互盖”直接钉死——
  // 若 `blend` 把 `opacity` 丢了，三档会全等；反之若 `opacity` 把 `blend` 丢了，
  // 完全不透明档也会是白的）。
  ST_CHECK(compose_with_opacity(0.25f).r > half.r);
}

/// 三维顶点色 / 纹理的**色彩空间口径**回归测试。
///
/// ## 这组用例钉的是什么
///
/// `math::Color` 是无标签的 8bit 字节，而几乎每个调用方传的都是**按 sRGB 选出来的色**
/// （设计工具里看到的那个）。光照计算必须在**线性**空间做，所以入口处要解码。
///
/// 漏了这一步的后果是"画面偏亮、材质发塑料感"——不会崩、不会报错，
/// 很容易被误认为"配色没调好"。实测中灰球体亮度冲到 216/255，几乎接白。
///
/// 三种写作方式**不能混**，混了就会出现"同一份颜色在不同接口下不一样亮"：
/// 1. `math::Color` 入参（`colored_sphere` / `box` / `plane` 等）→ 内部解码；
/// 2. `Mesh::vertices` 直接写 → **必须是线性值**（不做解码）；
/// 3. 纹理 RGBA8 字节（`set_texture`）→ 渲染时解码。
///
/// ## 判据为什么只能比数值
///
/// "偏亮"在单张图上看起来像口味问题。只有钉住**解码后的具体数值**，
/// 才能锁住"到底做没做解码"。

#include <array>
#include <cmath>
#include <cstddef>

#include "st/raster/color_pipeline.hpp"
#include "st/raster/mesh.hpp"
#include "st/test/test.hpp"

using st::math::Color;
using st::raster::Mesh;

namespace {

/// 从顶点缓冲里取第 index 个顶点的颜色（跳过前 6 个位置+法线分量）。
auto vertex_color(const Mesh& mesh, std::size_t index) -> std::array<float, 3> {
  const float* base = mesh.vertices.data() + index * Mesh::kVertexFloats + 6U;
  return {base[0], base[1], base[2]};
}

constexpr float kTol = 1e-5f;

}  // namespace

ST_TEST(mesh_vertex_colors_are_decoded_from_srgb) {
  // 传 sRGB 128（中灰）。线性值应 ≈ 0.2159，而**不是** 0.502。
  // 这是整条口径的分水岭：0.502 意味着"把 sRGB 当线性用了"。
  //
  // ⚠ `colored_sphere` 会在基础色上乘一个**竖向渐变**（`vertical_shade`，
  // 幅度很小但确实不是常量），所以不能断言"每个顶点都等于期望值"。
  // 正确做法：取全网格的**最大**通道值，它对应的渐变因子最接近 1。
  const Color grey{128, 128, 128, 255};
  const Mesh mesh = Mesh::colored_sphere(0.5f, 12, grey);
  ST_CHECK(mesh.vertex_count() > 0);

  float peak = 0.0f;
  for (std::size_t i = 0; i < mesh.vertex_count(); ++i) {
    peak = std::max(peak, vertex_color(mesh, i)[0]);
  }
  const float decoded = st::raster::srgb_to_linear(128.0f / 255.0f);   // ≈ 0.2159
  const float naive = 128.0f / 255.0f;                                // ≈ 0.502（未解码）
  // ## 为何要钉到 2% 以内（逆向验证抓出的漏洞）
  //
  // 原来用“在 decoded 的 0.9~1.15 倍之间”，但这拦不住"用纯幂函数 x^2.2
  // 代替 sRGB 分段"的错——两者在 128 处只差 **0.0037**（0.2159 vs 0.2195），
  // 完全落在这个区间里。实测该回退仍然绿。
  //
  // sRGB 分段曲线是**标准**（IEC 61966-2-1），没有“差不多”的余地。
  // 注意峰值乘了竖向渐变（上限 1.12），所以先归一化再比。
  constexpr float kShadeMax = 1.12f;   // `vertical_shade` 的最大因子
  ST_CHECK(std::abs(peak / kShadeMax - decoded) < decoded * 0.02f);
  // 与"未解码"保持明显距离（此时差异 ~0.29，一定有分辨力）
  ST_CHECK(peak < naive * 0.75f);

  // 暗部：sRGB 32 的正确线性值是 0.0144，而 x^2.2 给 0.0104（差 28%）。
  // 这条才是真正区分“分段曲线”与“纯幂函数”的判据。
  const Mesh dark_probe = Mesh::colored_sphere(0.5f, 8, Color{32, 32, 32, 255});
  float dark_peak = 0.0f;
  for (std::size_t i = 0; i < dark_probe.vertex_count(); ++i) {
    dark_peak = std::max(dark_peak, vertex_color(dark_probe, i)[0]);
  }
  const float dark_expect = st::raster::srgb_to_linear(32.0f / 255.0f);   // ≈ 0.0144
  ST_CHECK(std::abs(dark_peak / kShadeMax - dark_expect) < dark_expect * 0.08f);
}

ST_TEST(mesh_vertex_color_decoding_is_monotonic_and_bounded) {
  // 解码必须单调且值域 [0,1]：黑仍是黑（乘渐变后仍为 0），白接近 1。
  const Mesh black = Mesh::colored_sphere(0.5f, 8, Color{0, 0, 0, 255});
  const Mesh white = Mesh::colored_sphere(0.5f, 8, Color{255, 255, 255, 255});
  float black_peak = 0.0f;
  float white_peak = 0.0f;
  for (std::size_t i = 0; i < black.vertex_count(); ++i) {
    black_peak = std::max(black_peak, vertex_color(black, i)[0]);
  }
  for (std::size_t i = 0; i < white.vertex_count(); ++i) {
    white_peak = std::max(white_peak, vertex_color(white, i)[0]);
  }
  ST_CHECK(black_peak == 0.0f);
  // ## 为何上限是 1.12 而不是 1
  //
  // `colored_sphere` 会乘竖向渐变 `vertical_shade = 1 + y*0.12`，**最高 1.12**。
  // 所以纯白球体的顶点色峰值是 1.12（线性 HDR 值），不是 1.0。
  // 这是设计如此（渐变本身就要抬顶部），不是缺陷。
  // 判据：已解码且落在渐变上限附近。
  ST_CHECK(white_peak > 0.85f);
  ST_CHECK(white_peak < 1.13f);

  // 单调：更亮的 sRGB 输入 → 更大的峰值
  const Mesh dark = Mesh::colored_sphere(0.5f, 8, Color{64, 64, 64, 255});
  const Mesh light = Mesh::colored_sphere(0.5f, 8, Color{192, 192, 192, 255});
  float dark_peak = 0.0f;
  float light_peak = 0.0f;
  for (std::size_t i = 0; i < dark.vertex_count(); ++i) {
    dark_peak = std::max(dark_peak, vertex_color(dark, i)[0]);
  }
  for (std::size_t i = 0; i < light.vertex_count(); ++i) {
    light_peak = std::max(light_peak, vertex_color(light, i)[0]);
  }
  ST_CHECK(light_peak > dark_peak);
}

ST_TEST(cube_generator_decodes_its_base_color_too) {
  // `Mesh::cube` 走 `box()`，与 `colored_sphere` 是**不同的代码路径**——
  // 两处都要解码，否则"球对了但盒子还偏亮"，很难查。
  const Mesh box = Mesh::box(1.0f, 1.0f, 1.0f);
  ST_CHECK(box.vertex_count() > 0);
  // 默认色原本是偏亮的蓝；解码后蓝通道应显著小于 0.9（未解码时会接近 1）。
  const auto c = vertex_color(box, 0);
  ST_CHECK(c[2] > 0.0f);
  ST_CHECK(c[2] < 0.9f);
}

ST_TEST(plane_generator_produces_decoded_color) {
  // ## 为何用“比值”而不是绝对区间
  //
  // `Mesh::plane` / `textured_plane` 都乘竖向渐变（`vertical_shade`），
  // 而渐变因子的具体幅度是生成器内部细节。直接猜“峰值 = decoded × 1.12”
  // 会把测试绑到实现细节上（改渐变幅度就红，而那不该是测试关心的）。
  //
  // 可靠判据：**同一个生成器**传两个不同 sRGB 色，它们的比值就是
  // 两个解码值的比值（渐变因子相同，约掉了）。这条只依赖“解了码”，
  // 不依赖渐变的任何具体数值。
  const Color bright{200, 200, 200, 255};
  const Color dark{64, 64, 64, 255};
  const Mesh a = Mesh::plane(2.0f, 1.0f, bright);
  const Mesh b = Mesh::plane(2.0f, 1.0f, dark);
  ST_CHECK(a.vertex_count() > 0);
  float pa = 0.0f;
  float pb = 0.0f;
  for (std::size_t i = 0; i < a.vertex_count(); ++i) {
    pa = std::max(pa, vertex_color(a, i)[0]);
  }
  for (std::size_t i = 0; i < b.vertex_count(); ++i) {
    pb = std::max(pb, vertex_color(b, i)[0]);
  }
  const float expect_a = st::raster::srgb_to_linear(200.0f / 255.0f);   // 0.578
  const float expect_b = st::raster::srgb_to_linear(64.0f / 255.0f);    // 0.0513
  const float ratio = pa / pb;
  const float expect_ratio = expect_a / expect_b;                        // ≈ 11.3
  // 未解码时比值是 200/64 ≈ 3.1（差 3.6 倍，分辨力很强）
  ST_CHECK(std::abs(ratio - expect_ratio) < expect_ratio * 0.05f);
  ST_CHECK(std::abs(ratio - 3.125f) > 1.0f);
}

ST_TEST(textured_mesh_carries_linear_vertex_colors) {
  // 带 UV 的网格（贴图用）顶点色同样要解码——否则"贴图偏亮 + 顶点色偏亮"
  // 两个错误会叠在一起，而且都只能靠肉眼发现。
  // 判据同上（比值法，与渐变因子无关）。
  const Mesh a = Mesh::textured_plane(2.0f, 2.0f, Color{200, 200, 200, 255});
  const Mesh b = Mesh::textured_plane(2.0f, 2.0f, Color{64, 64, 64, 255});
  ST_CHECK(a.vertex_count() > 0);
  ST_CHECK(a.has_uvs());
  float pa = 0.0f;
  float pb = 0.0f;
  for (std::size_t i = 0; i < a.vertex_count(); ++i) {
    pa = std::max(pa, vertex_color(a, i)[0]);
  }
  for (std::size_t i = 0; i < b.vertex_count(); ++i) {
    pb = std::max(pb, vertex_color(b, i)[0]);
  }
  const float expect_ratio = st::raster::srgb_to_linear(200.0f / 255.0f) /
                             st::raster::srgb_to_linear(64.0f / 255.0f);
  const float ratio = pa / pb;
  ST_CHECK(std::abs(ratio - expect_ratio) < expect_ratio * 0.05f);
  ST_CHECK(std::abs(ratio - 3.125f) > 1.0f);
}

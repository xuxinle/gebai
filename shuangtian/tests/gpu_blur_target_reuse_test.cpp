/// 毛玻璃 / 折射的**离屏目标复用**回归。
///
/// ## 背景（2026-11-28，用户报「霜天最近的变更把背景渲染改坏了」）
///
/// `acquire_color_targets` 为了消除“每帧每块玻璃新建两张 RGBA 纹理且从不释放”
/// 的泄漏，改成了**跨调用复用、只增不减**（`60aee34c`）。复用本身对，
/// 但它引入了一个隐含前提：**纹理尺寸 == 本次区域尺寸**。
///
/// 而 `blur_region` / `refract_region` 的采样坐标全部是按“纹理尺寸 == 区域尺寸”
/// 推出来的：
///   · `copy.uv` 把画布上的 `area` 画到 RT 的**左上 area 块**（viewport = area）；
///   · 模糊 pass 用 `local01`（四边形内的 0..1 坐标）**直接当纹理 uv**；
///   · 贴回用 `0.5 / area.width` 做半纹素内缩，并把整张纹理铺满 `area`。
///
/// 于是**大目标配小区域**时：内容只占纹理左上角一小块，而采样/贴回按整张纹理解释
/// ⇒ 采到未写区域、内容被拉伸或缩放。表现是玻璃上一块**死色竖带**
/// （实测 xiaobai 的 user 气泡：左侧 45px 恒为 `#474747`，右侧正常）。
///
/// 判据（两条用例各自用自己的对照，关键是**逐位相等**）：
///   · 模糊：两张画布——A 只对小块模糊（首帧就是小尺寸 ⇒ 目标尺寸正确），
///     B 先对不重叠的大块模糊（把目标撑大）再对同一小块模糊；小块内必须 Δ0。
///   · 折射：同构，用折射参数。
/// 采用“自身对照”而不与软件腿比：两腿的模糊核形状本就不同（实测最大 Δ48），
/// 那个量级盖不住本缺陷；同构调用才能给出 Δ0 这种硬判据。
#include <cmath>
#include <memory>

#include "st/core/log.hpp"
#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/gpu.hpp"
#include "st/test/test.hpp"

namespace {

using st::math::Color;
using st::math::Rect;
using st::raster::Canvas;

[[nodiscard]] auto gpu_ready() -> bool { return st::raster::gpu::available(); }

/// 背景：满屏竖条纹（周期 16px，明暗强对比）。
/// 选条纹而不是匀色块：缺陷（内容被拉伸）在匀色区里看不见，
/// 条纹提供的是“**位置**编码”——明暗相位一错就立刻现形。
template <class SurfaceT>
void paint_scene(SurfaceT& canvas, int width, int height) {
  canvas.clear(Color{0x38, 0x48, 0x68, 0xFF});
  for (int x = 0; x < width; x += 16) {
    canvas.fill_rect(Rect{static_cast<float>(x), 0.0f, 8.0f, static_cast<float>(height)},
                     st::raster::Paint::solid(Color{0xF0, 0xE8, 0xD8, 0xFF}));
  }
}

/// 两块画布在 `area` 内的最大通道差（0xRRGGBBAA 打包）。
[[nodiscard]] auto max_delta_in(const st::raster::Surface& left, const st::raster::Surface& right,
                                st::math::IntRect area) -> int {
  int worst = 0;
  for (int y = area.y; y < area.y + area.height; ++y) {
    for (int x = area.x; x < area.x + area.width; ++x) {
      const Color a = left.pixel_at(x, y);
      const Color b = right.pixel_at(x, y);
      worst = std::max({worst, std::abs(static_cast<int>(a.r) - static_cast<int>(b.r)),
                        std::abs(static_cast<int>(a.g) - static_cast<int>(b.g)),
                        std::abs(static_cast<int>(a.b) - static_cast<int>(b.b))});
    }
  }
  return worst;
}

}  // namespace

ST_TEST(gpu_blur_after_larger_blur_matches_itself) {
  // 模糊也没有“同一块两次跑”可比（它就在地模糊画布）；用**两张画布**：
  //   A：只对小块模糊（首帧就是小尺寸 ⇒ 目标尺寸正确）；
  //   B：先对**不重叠**的大块模糊（把目标撑大），再对同一小块模糊。
  // 两者在小块内必须**逐位一致**——不一致就是目标尺寸变化后采样错了。
  if (!gpu_ready()) return;
  constexpr int kWidth = 400;
  constexpr int kHeight = 300;
  // 半径取 1：要测的是“目标尺寸变化后的采样映射”，大半径会把条纹抹平，
  // 相位信息一丢，缺陷就又藏起来了。
  //
  // ⚠ **几何必须保证真的复用**：`acquire_color_targets` 只在“两个维度都够大”时复用，
  // 任一维不够就**重建**。第一版写 `big` 高 120（ext 高 172），而小块 ext 高 174
  // ⇒ 每次小块都重建目标、复用从未发生，用例恒绿（逆向验证当场拓穿）。
  const Rect big{200.0f, 10.0f, 190.0f, 150.0f};   // ext ≈ 242×202
  const Rect small{20.0f, 160.0f, 130.0f, 90.0f};  // ext ≈ 192×174（两维都更小 ⇒ 复用）

  auto fresh = st::raster::gpu::create_canvas(kWidth, kHeight, 1.0f, {});
  auto reused = st::raster::gpu::create_canvas(kWidth, kHeight, 1.0f, {});
  ST_CHECK(fresh.has_value());
  ST_CHECK(reused.has_value());
  if (!fresh.has_value() || !reused.has_value()) return;

  paint_scene(**fresh, kWidth, kHeight);
  paint_scene(**reused, kWidth, kHeight);

  ST_REQUIRE((*fresh)->blur_region(small, 1.0f));
  ST_REQUIRE((*reused)->blur_region(big, 1.0f));
  ST_REQUIRE((*reused)->blur_region(small, 1.0f));

  const auto inner = st::math::IntRect{30, 170, 110, 70};
  const int delta = max_delta_in(**fresh, **reused, inner);
  st::log::out("[blur-reuse] 小块内最大 Δ{}\n", delta);
  for (const auto& [sx, sy] :
       {std::pair{32, 200}, std::pair{60, 200}, std::pair{100, 200}, std::pair{140, 200}}) {
    const Color a = (*fresh)->pixel_at(sx, sy);
    const Color b = (*reused)->pixel_at(sx, sy);
    st::log::out("[blur-reuse]  ({},{}): 首帧 #{:02X}{:02X}{:02X} · 复用后 #{:02X}{:02X}{:02X}\n", sx, sy,
                 a.r, a.g, a.b, b.r, b.g, b.b);
  }
  // 同一块、同一参数：结果必须**逐位一致**（两次调用完全同构，没有核形状差异）。
  ST_CHECK(delta == 0);
}

ST_TEST(gpu_blur_edge_does_not_pull_stale_content_from_bigger_target) {
  // ## 这条钉的是“复用的**另一半**”
  //
  // 上面两条钉的是“基准与步长要映射进子矩形”。但那只保证**中心**采对；
  // 模糊核会把采样点**推出 area**（`base + step * i`），而子矩形外存的是
  // **上一次更大区域遗留的旧像素**。
  //
  // 实测症状（用户报「右和下怎么有阴影」）：gbcode 先开大菜单再开小菜单，
  // 小菜单的**右边与下边各约 15px 内沿发暗**（面板内 248→234，面板外仍 255）。
  //
  // 判据：把大片**深色**目标撑开、再在小区域做模糊，小区域**靠近边缘的内沿**
  // 不得被旧内容染色。只量内沿一圈（不含中心）——中心本来就采不到外侧。
  if (!gpu_ready()) return;
  constexpr int kWidth = 400;
  constexpr int kHeight = 300;
  const Rect big{40.0f, 20.0f, 320.0f, 250.0f};
  const Rect small{40.0f, 20.0f, 130.0f, 90.0f};

  auto fresh = st::raster::gpu::create_canvas(kWidth, kHeight, 1.0f, {});
  auto reused = st::raster::gpu::create_canvas(kWidth, kHeight, 1.0f, {});
  ST_CHECK(fresh.has_value());
  ST_CHECK(reused.has_value());
  if (!fresh.has_value() || !reused.has_value()) return;

  // 画布：**匀色浅底**（内沿一旦被染色就看得出来）。
  const auto paint_flat = [](st::raster::Surface& canvas) {
    canvas.clear(Color{0xF0, 0xF0, 0xF0, 0xFF});
  };
  paint_flat(**fresh);
  paint_flat(**reused);

  // 先把**大块**填充成深色并模糊（把目标撑大、且旧内容留在纹理里）。
  (*reused)->fill_rect(big, st::raster::Paint::solid(Color{0x10, 0x10, 0x10, 0xFF}));
  ST_REQUIRE((*reused)->blur_region(big, 1.0f));
  // 再把大块涂回浅色（画布上已经是浅的了），只对小块模糊。
  (*reused)->fill_rect(big, st::raster::Paint::solid(Color{0xF0, 0xF0, 0xF0, 0xFF}));
  ST_REQUIRE((*reused)->blur_region(small, 1.0f));

  // 对照组：同尺寸目标（没有“更大目标”这回事）。
  ST_REQUIRE((*fresh)->blur_region(small, 1.0f));

  // 只量三条**内侧边**（距边 2~6px，避开圆角与抗锯齿过渡带）。
  const auto near = st::math::IntRect{static_cast<int>(small.x) + 2,
                                      static_cast<int>(small.y) + 2,
                                      static_cast<int>(small.width) - 4,
                                      static_cast<int>(small.height) - 4};
  int worst = 0;
  for (int y = near.y; y < near.y + near.height; ++y) {
    for (int x = near.x; x < near.x + near.width; ++x) {
      const bool on_edge_band = x < near.x + 8 || x >= near.x + near.width - 8 ||
                                y < near.y + 8 || y >= near.y + near.height - 8;
      if (!on_edge_band) continue;
      const Color a = (*fresh)->pixel_at(x, y);
      const Color b = (*reused)->pixel_at(x, y);
      worst = std::max({worst, std::abs(static_cast<int>(a.r) - static_cast<int>(b.r)),
                        std::abs(static_cast<int>(a.g) - static_cast<int>(b.g)),
                        std::abs(static_cast<int>(a.b) - static_cast<int>(b.b))});
    }
  }
  st::log::out("[blur-edge] 内沿最大 Δ{}\n", worst);
  // 匀色区域：采到旧内容（#101010）会被拉暗几十级；采对时两路逐位相同。
  ST_CHECK(worst == 0);
}

ST_TEST(gpu_refract_after_larger_refract_matches_itself) {
  // 折射也没有软件腿做参照，用**自身**做对照（同构调用必须 Δ0）：
  //   画布 A：只对小块做折射（首帧就是小尺寸 ⇒ 目标尺寸正确）；
  //   画布 B：先对大块折射（把目标撑大），再对同一小块折射。
  if (!gpu_ready()) return;
  constexpr int kWidth = 400;
  constexpr int kHeight = 300;
  // **两块区域必须不重叠**：折射会就地改写画布，后一次要从**未被动过**的背景里采。
  const Rect big{200.0f, 10.0f, 190.0f, 150.0f};   // ext ≈ 242×202
  const Rect small{20.0f, 160.0f, 130.0f, 90.0f};  // ext ≈ 192×174（两维都更小 ⇒ 复用）

  auto fresh = st::raster::gpu::create_canvas(kWidth, kHeight, 1.0f, {});
  auto reused = st::raster::gpu::create_canvas(kWidth, kHeight, 1.0f, {});
  ST_CHECK(fresh.has_value());
  ST_CHECK(reused.has_value());
  if (!fresh.has_value() || !reused.has_value()) return;

  paint_scene(**fresh, kWidth, kHeight);
  paint_scene(**reused, kWidth, kHeight);

  ST_REQUIRE((*fresh)->refract_region(small, 8.0f, 16.0f, 0.3f, 14.0f));
  ST_REQUIRE((*reused)->refract_region(big, 8.0f, 16.0f, 0.3f, 14.0f));
  ST_REQUIRE((*reused)->refract_region(small, 8.0f, 16.0f, 0.3f, 14.0f));
  const auto inner = st::math::IntRect{30, 170, 110, 70};
  const int delta = max_delta_in(**fresh, **reused, inner);
  st::log::out("[refract-reuse] 小块内最大 Δ{}\n", delta);
  // 同一块、同一参数：结果必须逐位一致（两次调用完全同构，没有核形状差异）。
  ST_CHECK(delta == 0);
}

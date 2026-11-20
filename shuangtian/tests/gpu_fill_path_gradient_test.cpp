/// GPU 侧「渐变填充**路径**」的回归（D3D11）。
///
/// ## 背景：一条只在 GPU 上出现、且与配置无关的缺陷
///
/// 用户报"暗色主题下气泡四边都有浅色亮边，配置明明是上下两道"。取证结果：
///
/// | 后端 | 左右（应暗） | 上下（应亮） |
/// |---|---|---|
/// | 软件（无头截图） | 20 / 27 ✔ | 218 ✔ |
/// | GPU（真实窗口） | **98 ✘** | 98 ✔ |
///
/// 根因在 `GpuCanvas::fill_path`：它无条件走 `paint.color()`，而
/// `Paint::with_gradient` 把 `color_` 设成**第一个色标**。于是
/// `edge → 透明` 的亮带取到满亮（整圈发亮），`透明 → 暗 → 透明` 的侧边
/// 取到全透明（完全不画）——两条叠起来正是"四边高光、左右无阴影"。
///
/// 软件腿走 `Canvas::fill_path`（逐像素 `Paint::sample`），从来没这个问题，
/// 所以**无头截图一直是对的、只有真实窗口错**——这是它难以定位的原因。
///
/// ## 这条测试钉住什么
///
/// 在 GPU 画布上画一条**竖直**的路径填充（`edge → 透明` 的线性渐变），断言：
///   ① 渐变**真的在渐变**（左右两端亮度不同）——回退 `paint.color()` 就会
///      整块同色，此断言立刻变红；
///   ② 路径之外的像素**没有被涂**——遮罩没接上时会溢出整个包围盒，
///      正是开发过程中实际踩到的第二个缺陷（圆角消失、变直角矩形）。
#include <memory>
#include <vector>

#include "st/core/log.hpp"
#include "st/math/color.hpp"
#include "st/math/path.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/gpu.hpp"
#include "st/raster/paint.hpp"
#include "st/test/test.hpp"

namespace {

using st::math::Color;
using st::math::Path;
using st::math::Point;
using st::math::Rect;
using st::raster::Canvas;
using st::raster::Gradient;
using st::raster::Paint;
using st::raster::Surface;
using st::raster::gpu::DeviceInfo;
using st::raster::gpu::Options;

constexpr int kWidth = 120;
constexpr int kHeight = 120;

[[nodiscard]] auto gpu_ready() -> bool {
  static const st::Result<DeviceInfo> info = st::raster::gpu::probe();
  return info.has_value();
}

/// 一块**带圆角**的矩形路径（x 40..80，y 20..100，圆角 14）。
///
/// 为什么必须带圆角：纯矩形的包围盒**等于形状本身**，所以“遮罩没接上”
/// 与“遮罩接上了”画出来完全一样——逆向验证时那条断言不会变红（实测）。
/// 圆角让包围盒四个角落在路径外，遮罩一旦失效，那四个角会被涂满。
[[nodiscard]] auto rounded_rect_path() -> Path {
  constexpr float kRadius = 14.0f;
  constexpr float kLeft = 40.0f;
  constexpr float kRight = 80.0f;
  constexpr float kTop = 20.0f;
  constexpr float kBottom = 100.0f;
  Path path;
  // 圆角用二次贝塞尔近似 90° 弧（控制点在圆角正方形顶点，误差 < 0.03r）——
  // `Path` 没有 `arc_to`，而 `quad_to` 是它提供的曲线手段。
  path.move_to(Point{kLeft + kRadius, kTop});
  path.line_to(Point{kRight - kRadius, kTop});
  path.quad_to(Point{kRight, kTop}, Point{kRight, kTop + kRadius});
  path.line_to(Point{kRight, kBottom - kRadius});
  path.quad_to(Point{kRight, kBottom}, Point{kRight - kRadius, kBottom});
  path.line_to(Point{kLeft + kRadius, kBottom});
  path.quad_to(Point{kLeft, kBottom}, Point{kLeft, kBottom - kRadius});
  path.line_to(Point{kLeft, kTop + kRadius});
  path.quad_to(Point{kLeft, kTop}, Point{kLeft + kRadius, kTop});
  path.close();
  return path;
}

/// 从左到右的线性渐变：左端亮白、右端全透明。
[[nodiscard]] auto horizontal_fade() -> Gradient {
  return Gradient::linear(Point{40.0f, 60.0f}, Point{80.0f, 60.0f},
                          {{0.0f, Color{0xFF, 0xFF, 0xFF, 0xFF}},
                           {1.0f, Color{0xFF, 0xFF, 0xFF, 0x00}}});
}

/// 在给定画布上画同一条路径 + 同一个渐变。
template <typename CanvasLike>
void paint_case(CanvasLike& canvas) {
  canvas.clear(Color{0, 0, 0, 255});
  canvas.fill_path(rounded_rect_path(), Paint::with_gradient(horizontal_fade()));
}

[[nodiscard]] auto luma_at(const Surface& surface, int x, int y) -> int {
  const Color c = surface.pixel_at(x, y);
  return (static_cast<int>(c.r) + static_cast<int>(c.g) + static_cast<int>(c.b)) / 3;
}

[[nodiscard]] auto luma_soft(const Canvas& canvas, int x, int y) -> int {
  const Color c = canvas.pixel_at(x, y);
  return (static_cast<int>(c.r) + static_cast<int>(c.g) + static_cast<int>(c.b)) / 3;
}

}  // namespace

/// ① GPU 上渐变路径填充**必须真的渐变**（而不是退化成单色）。
///
/// 回退 `fill_path` 到 `paint.color()` 时，整块都是第一个色标（纯白），
/// `left_luma - right_luma == 0` → 本用例变红。
ST_TEST(gpu_fill_path_gradient_actually_fades) {
  if (!gpu_ready()) return;   // 无 GPU 机器跳过
  auto surface = st::raster::gpu::create_canvas(kWidth, kHeight, 1.0f, Options{});
  if (!surface.has_value() || *surface == nullptr) return;
  Surface& gpu = **surface;
  paint_case(gpu);

  const int left = luma_at(gpu, 44, 60);    // 靠左端：接近满亮
  const int right = luma_at(gpu, 76, 60);   // 靠右端：接近透明
  // 渐变必须**单调且幅度显著**：从 >200 降到 <80（留足余量，不卡死具体数值）
  ST_CHECK(left > 200);
  ST_CHECK(right < 80);
  ST_CHECK(left - right > 150);
}

/// ② 路径**之外**不许被涂：遮罩必须接上。
///
/// 这条钉住开发中实际踩到的第二个缺陷：裁剪帧压栈晚于 `base_params` 时，
/// `params.mode[2]` 拿到上一个帧，着色器不做遮罩采样 → 整个包围盒被填满。
ST_TEST(gpu_fill_path_gradient_respects_path_mask) {
  if (!gpu_ready()) return;
  auto surface = st::raster::gpu::create_canvas(kWidth, kHeight, 1.0f, Options{});
  if (!surface.has_value() || *surface == nullptr) return;
  Surface& gpu = **surface;
  paint_case(gpu);

  // 路径外（左右留白与上下留白）必须是清屏色
  ST_CHECK(luma_at(gpu, 10, 60) == 0);    // 左外
  ST_CHECK(luma_at(gpu, 110, 60) == 0);   // 右外
  ST_CHECK(luma_at(gpu, 60, 5) == 0);     // 上外
  ST_CHECK(luma_at(gpu, 60, 115) == 0);   // 下外
  // **四个圆角外**：这是本用例真正的利刃——矩形的包围盒本来就与形状重合，
  // 只有圆角让包围盒四角落在路径外。遮罩失效时这四点会被涂亮。
  ST_CHECK(luma_at(gpu, 41, 21) == 0);    // 左上角外
  ST_CHECK(luma_at(gpu, 79, 21) == 0);    // 右上角外
  ST_CHECK(luma_at(gpu, 41, 99) == 0);    // 左下角外
  ST_CHECK(luma_at(gpu, 79, 99) == 0);    // 右下角外
  // 路径内必须是亮的（证明上面各条不是"什么都没画"）
  ST_CHECK(luma_at(gpu, 42, 60) > 200);
}

/// ③ 与软件腿同口径：同一路径 + 同一渐变，两条腿的**端点亮度**应接近。
///
/// 这条是"两腿同步"的守门：GPU 用了色带纹理采样、软件腿逐像素插值，
/// 实现不同但语义必须一致（允许 ±12 的量化/精度差）。
ST_TEST(gpu_fill_path_gradient_matches_software_leg) {
  if (!gpu_ready()) return;
  auto surface = st::raster::gpu::create_canvas(kWidth, kHeight, 1.0f, Options{});
  if (!surface.has_value() || *surface == nullptr) return;
  Surface& gpu = **surface;
  paint_case(gpu);

  Canvas software{kWidth, kHeight, 1.0f};
  paint_case(software);

  const std::vector<std::pair<int, int>> samples = {
      {44, 60}, {52, 60}, {60, 60}, {68, 60}, {76, 60}};
  for (const auto& [x, y] : samples) {
    const int g = luma_at(gpu, x, y);
    const int s = luma_soft(software, x, y);
    const int delta = g > s ? g - s : s - g;
    ST_CHECK(delta <= 12);
  }
}

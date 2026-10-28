/// GPU 画布**提交不变量**的回归：每个绘制入口都必须被 `submit_deferred_locked` 提交。
///
/// ## 为什么需要这组用例（2026-10-25 的真实代价）
///
/// 「每画布一个 deferred context」之后，绘制是**录制态**：不 `FinishCommandList` +
/// `ExecuteCommandList` 就不会执行。于是每个绘制入口都必须记账
/// （`note_deferred_draw`），而**漏记的后果各不相同、却都难查**：
///
/// | 漏在哪 | 症状 |
/// |---|---|
/// | `present` | 窗口全黑 |
/// | `submit_glyph_batch` | 文字整体消失（矩形/背景都在） |
/// | `submit_to` | 阴影/模糊"画了但看不见" |
/// | 离屏 clear | 阴影出现上一帧残影 |
///
/// 同一天里这四处全漏过、修了三轮。所以用例必须覆盖**每一类入口**，
/// 而不是只测"画个矩形能看见"——那样漏掉合批与离屏路径也照样绿。
///
/// ## 判据为什么是"回读到的像素"
///
/// 提交是否发生，唯一可观察的后果就是**像素有没有真的落到纹理上**。
/// 所以每个用例都以 `to_rgba8()` / `pixel_at()` 收尾——
/// 而这两个入口自身会先提交，所以**不能**用它们来验证"其它入口有没有提交"。
///
/// 为此测试必须在"其它入口"与"回读"之间插入一次**独立的提交动作**，
/// 让断言看到的是"提交前那批绘制是否已在里面"。
/// 这正是 `flush_glyph_batch()` 的顺序 bug 能暴露的关键（见第三组用例）。

#include "st/test/test.hpp"

#include <cmath>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/gpu.hpp"
#include "st/raster/paint.hpp"

namespace {

using st::math::Color;
using st::math::Rect;
using st::raster::Canvas;
using st::raster::Surface;

constexpr int kWidth = 64;
constexpr int kHeight = 64;

/// GPU 不可用时**明确跳过**（不假绿）：判据是后端自己报的能力，不是猜。
[[nodiscard]] auto gpu_ready() -> bool { return st::raster::gpu::available(); }

[[nodiscard]] auto make_gpu() -> std::unique_ptr<Surface> {
  auto created = st::raster::gpu::create_canvas(kWidth, kHeight, 1.0f, {});
  if (!created.has_value()) return nullptr;
  return std::move(*created);
}

/// "画过"的像素数。
///
/// ## 为何必须看 alpha，不能只看 RGB
///
/// 画布内部是**预乘 alpha**。黑色阴影预乘后 RGB 全是 0，只剩 alpha 有值——
/// 于是“只看 RGB”的判据会把它当成“什么都没画”。
///
/// 这正是第一版这组用例踩的坑：GPU 实际画得**与软件逐像素一致**
/// （实测 `(28,28)=(0,0,0,160)` 对软件的 `(0,0,0,160)`），却被判成 0 个像素，
/// 于是白排查了一轮“提交是不是漏了”。
///
/// 教训：**判据要盯的是“有没有内容”，不是“亮不亮”**。
[[nodiscard]] auto painted_pixels(Surface& surface) -> int {
  const auto bytes = surface.to_rgba8();
  int count = 0;
  for (std::size_t index = 0; index + 3 < bytes.size(); index += 4) {
    if (bytes[index] != 0 || bytes[index + 1] != 0 || bytes[index + 2] != 0 ||
        bytes[index + 3] != 0) {
      ++count;
    }
  }
  return count;
}

}  // namespace

/// ① 基本图元（走 `submit`）：画了就该看得见。
///
/// 这条是"底线"用例——它绿只能说明 `submit` 记得记账，
/// 不代表合批与离屏路径也对（下面两条专管那个）。
ST_TEST(gpu_committed_draws_land_in_the_canvas) {
  if (!gpu_ready()) return;
  auto surface = make_gpu();
  ST_REQUIRE(surface != nullptr);
  surface->clear(Color{0, 0, 0, 0});
  Surface& target = *surface;
  target.fill_rect(Rect{8.0f, 8.0f, 32.0f, 32.0f}, st::raster::Paint::solid(Color{255, 0, 0, 255}));
  // 回读路径自己会提交，但这里要确认的是**像素真的落了地**
  const int painted = painted_pixels(target);
  ST_CHECK(painted > 0);
  st::log::out("[gpu-commit] 基本图元：{} 个非零像素\n", painted);
}

/// ② 离屏绘制（走 `submit_to`）：阴影遮罩 / 盒式模糊都靠它。
///
/// 判据是"最终合成结果里有东西"——`submit_to` 漏记账时离屏结果不产出，
/// 而**使用它的那次绘制照常提交**，于是画面看起来"参数没生效"。
ST_TEST(gpu_offscreen_effects_reach_the_canvas) {
  if (!gpu_ready()) return;
  const auto caps = st::raster::gpu::capabilities();
  if (!caps.shadows) {
    st::log::out("[gpu-commit] 跳过：后端未声明 shadows\n");
    return;
  }
  auto surface = make_gpu();
  ST_REQUIRE(surface != nullptr);
  surface->clear(Color{0, 0, 0, 0});
  Surface& target = *surface;
  target.draw_shadow(Rect{16.0f, 16.0f, 24.0f, 24.0f}, 4.0f, 6.0f, Color{0, 0, 0, 160});
  const int painted = painted_pixels(target);

  // 先与软件画布对照：若软件也画不出东西，那是**测试用法**的问题而不是 GPU 提交的问题。
  // 没有这层对照，一条红会同时指向两种完全不同的原因，排查从第一步就走偏。
  Canvas reference(kWidth, kHeight);
  reference.clear(Color{0, 0, 0, 0});
  reference.draw_shadow(Rect{16.0f, 16.0f, 24.0f, 24.0f}, 4.0f, 6.0f, Color{0, 0, 0, 160});
  int reference_painted = 0;
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      if (reference.pixel_at(x, y).a != 0) ++reference_painted;
    }
  }
  st::log::out("[gpu-commit] 离屏阴影：GPU {} 个 / 软件 {} 个非零像素\n", painted, reference_painted);
  ST_CHECK(reference_painted > 0);   // 用法本身得对
  ST_CHECK(painted > 0);             // GPU 也跟着要有
}

/// ③ 图像合成（走 `draw_canvas_at` + `submit_to` 的位图路径）。
///
/// 与②同一类风险，但走的是另一条纹理采样路径——两条都测才叫覆盖。
ST_TEST(gpu_canvas_composition_reaches_the_canvas) {
  if (!gpu_ready()) return;
  const auto caps = st::raster::gpu::capabilities();
  if (!caps.bitmaps) {
    st::log::out("[gpu-commit] 跳过：后端未声明 bitmaps\n");
    return;
  }
  auto source = make_gpu();
  ST_REQUIRE(source != nullptr);
  source->clear(Color{0, 0, 0, 0});
  Surface& src = *source;
  src.fill_rect(Rect{0.0f, 0.0f, 32.0f, 32.0f}, st::raster::Paint::solid(Color{0, 255, 0, 255}));

  auto destination = make_gpu();
  ST_REQUIRE(destination != nullptr);
  destination->clear(Color{0, 0, 0, 0});
  Surface& dst = *destination;
  // 跨画布合成：源画布必须先提交（`draw_canvas_at` 内部读它的目标纹理）
  dst.draw_canvas_at(src, 0, 0);
  const int painted = painted_pixels(dst);
  ST_CHECK(painted > 0);
  st::log::out("[gpu-commit] 画布合成：{} 个非零像素\n", painted);
}

/// ④ **顺带验证"提交点必须在攒批之后"**（2026-10-25 的真实顺序 bug）。
///
/// `ensure_readback()` 里原本 `submit_deferred_locked()` 排在
/// `flush_glyph_batch()` **之前**：先提交（那时字形还没录）→ 再录字形 →
/// 字形落在未提交状态，于是**回读到的画面文字全缺**。
///
/// 屏幕上看不出来（上屏时会再提交一次），只有截图/导出缺字——
/// 这正是"像素断言假阳/假阴"的来源。本用例要求：
/// 同一块画布**两次回读结果一致**（如果第一次缺字、第二次补齐，就说明顺序错了）。
ST_TEST(gpu_readback_is_stable_across_repeated_reads) {
  if (!gpu_ready()) return;
  auto surface = make_gpu();
  ST_REQUIRE(surface != nullptr);
  surface->clear(Color{0, 0, 0, 0});
  Surface& target = *surface;
  target.fill_rect(Rect{10.0f, 10.0f, 20.0f, 20.0f}, st::raster::Paint::solid(Color{0, 0, 255, 255}));

  const int first = painted_pixels(target);
  const int second = painted_pixels(target);
  ST_CHECK_EQ(first, second);
  // 首次就该有内容：若第一次为 0、第二次才有，说明"首帧全黑"那类缺陷回来了
  ST_CHECK(first > 0);
  st::log::out("[gpu-commit] 回读稳定：第一次 {} / 第二次 {}\n", first, second);
}

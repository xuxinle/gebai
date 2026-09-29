/// GPU 与软件光栅器的**逐像素一致性**测试（容差口径）。
///
/// 为什么用容差而不是逐字节：GPU 与软件的抗锯齿与混合细节**不可能完全相同**
/// （SDF 解析覆盖率 vs 扫描线覆盖率；16 位中间运算 vs 浮点）。要求 bit-exact 等于
/// 给一个不可能达成的目标，只会让测试被删掉。真正要钉住的是三件事：
///
/// 1. **几何在同一位置**（偏移一个像素是灾难，颜色差 1/255 不是）；
/// 2. **颜色量级一致**（不允许整体偏色、通道互换、预乘/直通搞混——那些是量级差异）；
/// 3. **覆盖区域一致**（该画的地方要画，不该画的地方不能有东西）。
///
/// 因此判据写成：超差像素占比上限 + 最大通道差上限 + 覆盖率（非零 alpha）比例接近。
/// 一旦有人在 GPU 路径里把预乘写反、通道顺序搞错、坐标少乘一次 scale，这条会立刻红。

#include "st/test/test.hpp"

#include <cmath>
#include <format>
#include <string>
#include <vector>

#include "st/core/print.hpp"
#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/gpu.hpp"
#include "st/raster/paint.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/feedback.hpp"
#include "st/ui/components/input.hpp"
#include "st/ui/components/table.hpp"
#include "st/ui/components/toggle.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::math::Color;
using st::math::Insets;
using st::math::Rect;
using st::raster::Canvas;
using st::raster::Surface;
using st::ui::Align;
using st::ui::Button;
using st::ui::Card;
using st::ui::FlexDirection;
using st::ui::Input;
using st::ui::Panel;
using st::ui::Switch;
using st::ui::Table;
using st::ui::TableColumn;
using st::ui::Text;
using st::ui::Theme;
using st::ui::UiRoot;

inline constexpr int kWidth = 400;
inline constexpr int kHeight = 300;

/// 对比报告。
struct Comparison {
  double differing_ratio{0.0};   ///< 超差像素占比（0..1）
  int max_delta{0};              ///< 最大通道差
  double ink_ratio_left{0.0};    ///< 左侧覆盖率（非零 alpha 占比）
  double ink_ratio_right{0.0};   ///< 右侧覆盖率
  std::string first_offsets{};   ///< 前三处超差坐标（诊断用）
};

[[nodiscard]] auto ink_ratio(std::span<const std::uint32_t> pixels) -> double {
  if (pixels.empty()) return 0.0;
  std::size_t ink = 0;
  for (const std::uint32_t pixel : pixels) {
    if ((pixel & 0xFFU) != 0U) ++ink;
  }
  return static_cast<double>(ink) / static_cast<double>(pixels.size());
}

/// 逐像素对比（`max_channel_delta` 以内算一致）。
[[nodiscard]] auto compare(std::span<const std::uint32_t> left,
                           std::span<const std::uint32_t> right, int max_channel_delta,
                           int width) -> Comparison {
  Comparison result;
  result.ink_ratio_left = ink_ratio(left);
  result.ink_ratio_right = ink_ratio(right);
  if (left.size() != right.size() || left.empty()) {
    result.differing_ratio = 1.0;
    result.max_delta = 255;
    return result;
  }
  std::size_t differing = 0;
  int reported = 0;
  for (std::size_t index = 0; index < left.size(); ++index) {
    const std::uint32_t a = left[index];
    const std::uint32_t b = right[index];
    int delta = 0;
    for (unsigned shift = 0; shift < 32U; shift += 8U) {
      const int ca = static_cast<int>((a >> shift) & 0xFFU);
      const int cb = static_cast<int>((b >> shift) & 0xFFU);
      delta = std::max(delta, std::abs(ca - cb));
    }
    result.max_delta = std::max(result.max_delta, delta);
    if (delta > max_channel_delta) {
      ++differing;
      if (reported < 3) {
        const auto x = static_cast<int>(index % static_cast<std::size_t>(width));
        const auto y = static_cast<int>(index / static_cast<std::size_t>(width));
        result.first_offsets += std::format("({},{})Δ{} ", x, y, delta);
        ++reported;
      }
    }
  }
  result.differing_ratio = static_cast<double>(differing) / static_cast<double>(left.size());
  return result;
}

/// 同一棵界面分别画到软件画布与 GPU 画布。
struct Scene {
  Theme theme{Theme::light()};
  st::ui::RenderContext context{theme, nullptr, 0.0};
  Canvas software{kWidth, kHeight};
  std::unique_ptr<Surface> gpu{};

  explicit Scene(bool with_gpu = true) {
    if (with_gpu) {
      auto created = st::raster::gpu::create_canvas(kWidth, kHeight, 1.0f, {});
      if (created.has_value()) gpu = std::move(*created);
    }
  }

  void render(const st::ui::Element& root) {
    software.clear(Color{0x0A, 0x0F, 0x1A, 0xFF});
    root.paint(context, software);
    if (gpu != nullptr) {
      gpu->clear(Color{0x0A, 0x0F, 0x1A, 0xFF});
      root.paint(context, *gpu);
    }
  }
};

[[nodiscard]] auto gpu_ready() -> bool { return st::raster::gpu::available(); }

}  // namespace

ST_TEST(gpu_matches_software_for_solid_rounded_rect) {
  if (!gpu_ready()) return;
  Scene scene;
  if (scene.gpu == nullptr) return;

  Card card(18.0f);
  card.style().background = Color{0x22, 0x33, 0x44, 0xFF};
  card.measure(scene.context, st::ui::Constraints{.max_width = 200.0f, .max_height = 120.0f});
  card.arrange(scene.context, Rect{40.0f, 30.0f, 200.0f, 120.0f});

  scene.software.clear(Color{0x0A, 0x0F, 0x1A, 0xFF});
  card.paint(scene.context, scene.software);
  scene.gpu->clear(Color{0x0A, 0x0F, 0x1A, 0xFF});
  card.paint(scene.context, *scene.gpu);

  const Comparison result =
      compare(scene.software.pixels(), scene.gpu->pixels(), /*max_channel_delta=*/6, kWidth);
  // 形状内部必须一致；差异只允许出现在抗锯齿的边缘环上
  ST_CHECK(result.differing_ratio < 0.02);
  ST_CHECK(result.max_delta <= 40);
}

ST_TEST(gpu_matches_software_for_layout_of_real_widgets) {
  if (!gpu_ready()) return;
  // 这棵界面会用到**投影与描边**（卡片阴影、开关/输入框描边）——
  // 未落地前不跑它：否则测试只会因为“还没实现”而永远红，最终被人删掉。
  // 真实的进度由 `capabilities()` 报出，并在下方断言里打印出来。
  const st::raster::gpu::Capabilities caps = st::raster::gpu::capabilities();
  if (!caps.shadows || !caps.paths) {
    st::print("[gpu-diff] 跳过（待 M3b/M4）：shadows={} paths={}\n", caps.shadows, caps.paths);
    return;
  }
  Scene scene;
  if (scene.gpu == nullptr) return;

  // 一棵有代表性的小界面：卡片 + 文字 + 开关 + 输入框 + 表格
  Panel root(FlexDirection::Column);
  root.style().padding = Insets::all(20.0f);
  root.style().gap = 12.0f;
  auto heading = std::make_unique<Text>("GPU 对比");
  heading->set_font_size(20.0f);
  root.add_child(std::move(heading));
  auto toggle = std::make_unique<Switch>("启用");
  toggle->set_checked(true);
  root.add_child(std::move(toggle));
  auto input = std::make_unique<Input>();
  input->set_placeholder("输入…");
  root.add_child(std::move(input));
  auto button = std::make_unique<Button>("确定");
  root.add_child(std::move(button));

  root.measure(scene.context, st::ui::Constraints{.max_width = kWidth, .max_height = kHeight});
  root.arrange(scene.context, Rect{0.0f, 0.0f, kWidth, kHeight});
  scene.render(root);

  const Comparison result =
      compare(scene.software.pixels(), scene.gpu->pixels(), /*max_channel_delta=*/12, kWidth);
  st::print("[gpu-diff] 小界面：超差 {:.3f}% · 最大Δ{} · 覆盖 {:.3f} vs {:.3f} · {}\n",
            result.differing_ratio * 100.0, result.max_delta, result.ink_ratio_left,
            result.ink_ratio_right, result.first_offsets);
  // 几何必须落在同一位置：超差像素占比很小；覆盖率（该画的地方）必须接近
  ST_CHECK(result.differing_ratio < 0.05);
  ST_CHECK(std::abs(result.ink_ratio_left - result.ink_ratio_right) < 0.02);
  ST_CHECK(result.max_delta <= 96);
}

ST_TEST(gpu_text_matches_software_closely) {
  if (!gpu_ready()) return;
  Scene scene;
  if (scene.gpu == nullptr) return;

  // 文字是"覆盖率位图"路径：两边字形同源，只有边缘合成方式不同
  st::ui::RenderContext context{scene.theme, nullptr, 0.0};
  Panel root(FlexDirection::Column);
  auto text = std::make_unique<Text>("霜天 Shuangtian 0123");
  text->set_font_size(18.0f);
  root.add_child(std::move(text));
  root.measure(context, st::ui::Constraints{.max_width = kWidth, .max_height = kHeight});
  root.arrange(context, Rect{10.0f, 10.0f, 360.0f, 40.0f});
  scene.render(root);

  const Comparison result =
      compare(scene.software.pixels(), scene.gpu->pixels(), /*max_channel_delta=*/16, kWidth);
  st::print("[gpu-diff] 文字：超差 {:.3f}% · 最大Δ{} · 覆盖 {:.4f} vs {:.4f} · {}\n",
            result.differing_ratio * 100.0, result.max_delta, result.ink_ratio_left,
            result.ink_ratio_right, result.first_offsets);
  // 有字必须有墨：两边覆盖率同量级（字形缺失会让 GPU 侧覆盖率归零）
  ST_CHECK(result.ink_ratio_right > 0.0);
  ST_CHECK(std::abs(result.ink_ratio_left - result.ink_ratio_right) < 0.01);
  ST_CHECK(result.differing_ratio < 0.05);
}

ST_TEST(gpu_gradient_uses_same_lut_as_software) {
  if (!gpu_ready()) return;
  Scene scene;
  if (scene.gpu == nullptr) return;

  Card card(0.0f);
  card.style().background = Color{0, 0, 0, 0};
  card.measure(scene.context, st::ui::Constraints{.max_width = 256.0f, .max_height = 40.0f});
  card.arrange(scene.context, Rect{0.0f, 0.0f, 256.0f, 40.0f});

  st::raster::Paint paint = st::raster::Paint::with_gradient(st::raster::Gradient::linear(
      st::math::Point{0.0f, 0.0f}, st::math::Point{256.0f, 0.0f},
      {{0.0f, Color{0xFF, 0x00, 0x00, 0xFF}}, {1.0f, Color{0x00, 0x00, 0xFF, 0xFF}}}));

  scene.software.clear(Color{0, 0, 0, 0});
  scene.software.fill_rect(Rect{0.0f, 0.0f, 256.0f, 40.0f}, paint);
  scene.gpu->clear(Color{0, 0, 0, 0});
  scene.gpu->fill_rect(Rect{0.0f, 0.0f, 256.0f, 40.0f}, paint);

  const Comparison result =
      compare(scene.software.pixels(), scene.gpu->pixels(), /*max_channel_delta=*/12, kWidth);
  st::print("[gpu-diff] 渐变：超差 {:.3f}% · 最大Δ{} · {}\n", result.differing_ratio * 100.0,
            result.max_delta, result.first_offsets);
  ST_CHECK(result.differing_ratio < 0.05);
  ST_CHECK(result.max_delta <= 40);
}

ST_TEST(gpu_capabilities_are_consistent_with_probe) {
  // 能力声明必须与实际可用性一致：不可用时不能声称任何能力
  const st::raster::gpu::Capabilities caps = st::raster::gpu::capabilities();
  if (!gpu_ready()) {
    ST_CHECK(!caps.solid_shapes);
    ST_CHECK(!caps.complete());
    return;
  }
  ST_CHECK(caps.solid_shapes);
  ST_CHECK(caps.gradients);
  ST_CHECK(caps.coverage_masks);
  ST_CHECK(caps.clips);
  // 未落地的部分必须**报缺失**（如实优于好看）
  st::print("[gpu] 能力：shapes={} gradients={} masks={} bitmaps={} clips={} shadows={} paths={}\n",
            caps.solid_shapes, caps.gradients, caps.coverage_masks, caps.bitmaps, caps.clips,
            caps.shadows, caps.paths);
}

ST_TEST(gpu_clip_rect_stays_inside_bounds) {
  if (!gpu_ready()) return;
  auto gpu = st::raster::gpu::create_canvas(64, 64, 1.0f, {});
  ST_CHECK(gpu.has_value());
  if (!gpu.has_value()) return;
  Surface& target = **gpu;
  target.clear(Color{0, 0, 0, 0});
  target.push_clip_rect(Rect{16.0f, 16.0f, 16.0f, 16.0f});
  target.fill_rect(Rect{0.0f, 0.0f, 64.0f, 64.0f}, st::raster::Paint::solid(Color{0xFF, 0, 0, 0xFF}));
  target.pop_clip();
  // 裁剪外必须干净（这是矩形裁剪走剪裁矩形的验证）
  ST_CHECK_EQ(static_cast<int>(target.pixel_at(4, 4).a), 0);
  ST_CHECK_EQ(static_cast<int>(target.pixel_at(60, 60).a), 0);
  // 裁剪内必须着色
  ST_CHECK_EQ(static_cast<int>(target.pixel_at(24, 24).r), 0xFF);
}

ST_TEST(gpu_clip_rounded_rect_leaves_corners_clear) {
  if (!gpu_ready()) return;
  auto gpu = st::raster::gpu::create_canvas(64, 64, 1.0f, {});
  ST_CHECK(gpu.has_value());
  if (!gpu.has_value()) return;
  Surface& target = **gpu;
  target.clear(Color{0, 0, 0, 0});
  target.push_clip_rounded_rect(Rect{8.0f, 8.0f, 48.0f, 48.0f}, 16.0f);
  target.fill_rect(Rect{0.0f, 0.0f, 64.0f, 64.0f},
                   st::raster::Paint::solid(Color{0x00, 0xFF, 0x00, 0xFF}));
  target.pop_clip();
  // 圆角处的角必须被裁掉，中心与边中点必须留着
  ST_CHECK_EQ(static_cast<int>(target.pixel_at(9, 9).a), 0);
  ST_CHECK_EQ(static_cast<int>(target.pixel_at(32, 32).a), 0xFF);
  ST_CHECK_EQ(static_cast<int>(target.pixel_at(32, 10).a), 0xFF);
}

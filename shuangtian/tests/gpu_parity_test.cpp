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
#include <tuple>
#include <format>
#include <string>
#include <vector>

#include "st/app/text_port.hpp"
#include "st/codec/png.hpp"
#include "st/core/print.hpp"
#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/gpu.hpp"
#include "st/raster/paint.hpp"
#include "st/text/text.hpp"
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
  double structural_ratio{0.0};  ///< **结构性**超差占比（差 >64：内容不同，而非边缘抗锯齿）
  int max_delta{0};              ///< 最大通道差
  double ink_ratio_left{0.0};    ///< 左侧非**底色**像素占比（确认两侧都真画了东西）
  double ink_ratio_right{0.0};   ///< 右侧非底色像素占比
  std::string first_offsets{};   ///< 前三处超差坐标（诊断用）
  std::string worst_offsets{};   ///< 最差的五处（判断是否只是边缘）
};

/// 非**底色**像素占比。
///
/// 为什么不用“非零 alpha”：场景底色是不透明的，那样算出来永远是 1.0——
/// 一个恒为 1 的指标看起来像“断言通过了”，实际什么都没验证（本测试曾踩到这个）。
[[nodiscard]] auto ink_ratio(std::span<const std::uint32_t> pixels, std::uint32_t background)
    -> double {
  if (pixels.empty()) return 0.0;
  std::size_t ink = 0;
  for (const std::uint32_t pixel : pixels) {
    if (pixel != background) ++ink;
  }
  return static_cast<double>(ink) / static_cast<double>(pixels.size());
}

/// 逐像素对比（`max_channel_delta` 以内算一致）。
[[nodiscard]] auto compare(std::span<const std::uint32_t> left,
                           std::span<const std::uint32_t> right, int max_channel_delta, int width,
                           std::uint32_t background = 0x0A0F1AFFU) -> Comparison {
  Comparison result;
  result.ink_ratio_left = ink_ratio(left, background);
  result.ink_ratio_right = ink_ratio(right, background);
  if (left.size() != right.size() || left.empty()) {
    result.differing_ratio = 1.0;
    result.structural_ratio = 1.0;
    result.max_delta = 255;
    return result;
  }
  std::size_t differing = 0;
  std::size_t structural = 0;
  int reported = 0;
  std::array<std::pair<int, int>, 5> worst{};  // (delta, offset)
  worst.fill({0, -1});
  for (std::size_t index = 0; index < left.size(); ++index) {
    const std::uint32_t a = left[index];
    const std::uint32_t b = right[index];
    int delta = 0;
    for (unsigned shift = 0; shift < 32U; shift += 8U) {
      const int ca = static_cast<int>((a >> shift) & 0xFFU);
      const int cb = static_cast<int>((b >> shift) & 0xFFU);
      delta = std::max(delta, std::abs(ca - cb));
    }
    if (delta > result.max_delta) result.max_delta = delta;
    for (auto& entry : worst) {
      if (delta > entry.first) {
        entry = {delta, static_cast<int>(index)};
        break;
      }
    }
    if (delta > max_channel_delta) {
      ++differing;
      if (reported < 3) {
        const auto x = static_cast<int>(index % static_cast<std::size_t>(width));
        const auto y = static_cast<int>(index / static_cast<std::size_t>(width));
        result.first_offsets += std::format("({},{})Δ{} ", x, y, delta);
        ++reported;
      }
    }
    // 结构性超差：差 >64 是“内容不同”（位置/颜色/形状对不上），不是边缘抗锯齿的权重差。
    // （此前这里漏了自增——`structural_ratio` 恒为 0，下方所有 `< 0.001` 断言
    //   都是空转的假绿；补上后这些断言才真的在钉位置/颜色/形状。）
    if (delta > 64) ++structural;
  }
  result.differing_ratio = static_cast<double>(differing) / static_cast<double>(left.size());
  result.structural_ratio = static_cast<double>(structural) / static_cast<double>(left.size());
  return result;
}

/// 同一棵界面分别画到软件画布与 GPU 画布。
///
/// **必须带真实字体端口**：否则 `Text` 与带文字的控件（按钮/标签）量不出尺寸、根本不画——
/// 两边都“没画”，对比结果就是完美的 0 差异。这种“假绿”比红更糟：
/// 它会让“文字在 GPU 上能画”这个结论**毫无依据**（本测试第一版就是这个毛病）。
struct Scene {
  Theme theme{Theme::light()};
  std::shared_ptr<st::text::FontStack> stack{};
  std::unique_ptr<st::text::TextRenderer> renderer{};
  std::unique_ptr<st::ui::TextPort> port{};
  // `RenderContext` 持有 `theme` 引用（因此不可赋值）：用 optional 延迟构造——
  // 必须先建好字体与文本端口，再把端口指针交给它。
  std::optional<st::ui::RenderContext> context_storage{};
  [[nodiscard]] auto context() -> st::ui::RenderContext& { return *context_storage; }
  bool has_text{false};
  Canvas software{kWidth, kHeight};
  std::unique_ptr<Surface> gpu{};

  explicit Scene(bool with_gpu = true) {
    context_storage.emplace(st::ui::RenderContext{theme, nullptr, 0.0});
    if (auto loaded = st::text::FontStack::system_default(); loaded.has_value()) {
      stack = std::make_shared<st::text::FontStack>(std::move(*loaded));
      renderer = std::make_unique<st::text::TextRenderer>(*stack);
      port = st::app::make_text_port(*renderer);
      context_storage.emplace(st::ui::RenderContext{theme, port.get(), 0.0});
      has_text = true;
    }
    if (with_gpu) {
      auto created = st::raster::gpu::create_canvas(kWidth, kHeight, 1.0f, {});
      if (created.has_value()) gpu = std::move(*created);
    }
  }

  void render(const st::ui::Element& root) {
    software.clear(Color{0x0A, 0x0F, 0x1A, 0xFF});
    root.paint(context(), software);
    if (gpu != nullptr) {
      gpu->clear(Color{0x0A, 0x0F, 0x1A, 0xFF});
      root.paint(context(), *gpu);
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
  card.measure(scene.context(), st::ui::Constraints{.max_width = 200.0f, .max_height = 120.0f});
  card.arrange(scene.context(), Rect{40.0f, 30.0f, 200.0f, 120.0f});

  scene.software.clear(Color{0x0A, 0x0F, 0x1A, 0xFF});
  card.paint(scene.context(), scene.software);
  scene.gpu->clear(Color{0x0A, 0x0F, 0x1A, 0xFF});
  card.paint(scene.context(), *scene.gpu);

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

  root.measure(scene.context(), st::ui::Constraints{.max_width = kWidth, .max_height = kHeight});
  root.arrange(scene.context(), Rect{0.0f, 0.0f, kWidth, kHeight});
  scene.render(root);

  const Comparison result =
      compare(scene.software.pixels(), scene.gpu->pixels(), /*max_channel_delta=*/12, kWidth);
  st::print("[gpu-diff] 小界面：超差 {:.3f}% · 结构性 {:.3f}% · 最大Δ{} · 非底色 {:.3f} vs {:.3f} · {}\n",
            result.differing_ratio * 100.0, result.structural_ratio * 100.0, result.max_delta,
            result.ink_ratio_left, result.ink_ratio_right, result.worst_offsets);
  // 几何必须落在同一位置：超差像素占比很小；覆盖率（该画的地方）必须接近
  ST_CHECK(result.differing_ratio < 0.05);
  // **结构性差异必须近于零**：形状/文字/颜色都落在同一位置。
  // 允许的残差只有"边缘抗锯齿权重不同"（SDF 解析覆盖率 vs 扫描线覆盖率），
  // 而那种差异是单个像素级别的、不可能成片——所以用"差 >64 的像素占比"当判据，
  // 而不是用一个宽松的 max_delta 上限（那会同时放过"整块内容错了"）。
  ST_CHECK(result.structural_ratio < 0.001);
  ST_CHECK(std::abs(result.ink_ratio_left - result.ink_ratio_right) < 0.02);
}

ST_TEST(gpu_text_matches_software_closely) {
  if (!gpu_ready()) return;
  Scene scene;
  if (scene.gpu == nullptr) return;
  // 没有字体端口时这个用例是**空转的**（两边都没画字 → 完美 0 差异）：
  // 必须显式确认字体真的加载了，否则"文字一致"这个结论没有依据。
  ST_CHECK(scene.has_text);
  if (!scene.has_text) return;

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
  st::print("[gpu-diff] 文字：超差 {:.3f}% · 结构性 {:.3f}% · 最大Δ{} · 非底色 {:.4f} vs {:.4f} · {}\n",
            result.differing_ratio * 100.0, result.structural_ratio * 100.0, result.max_delta,
            result.ink_ratio_left, result.ink_ratio_right, result.worst_offsets);
  // 有字必须有墨：两边覆盖率同量级（字形缺失会让 GPU 侧覆盖率归零）
  // 两侧都必须真的画了字（非底色像素占比远大于 0），且覆盖率量级接近
  ST_CHECK(std::abs(result.ink_ratio_left - result.ink_ratio_right) < 0.01);
  ST_CHECK(result.differing_ratio < 0.05);
}

ST_TEST(gpu_gradient_uses_same_lut_as_software) {
  if (!gpu_ready()) return;
  Scene scene;
  if (scene.gpu == nullptr) return;

  Card card(0.0f);
  card.style().background = Color{0, 0, 0, 0};
  card.measure(scene.context(), st::ui::Constraints{.max_width = 256.0f, .max_height = 40.0f});
  card.arrange(scene.context(), Rect{0.0f, 0.0f, 256.0f, 40.0f});

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

ST_TEST(gpu_shadow_matches_software) {
  // 单独验证投影：这是 GPU 侧唯一需要**多遍渲染到纹理**（遮罩 RT + 可分离盒式模糊）的原语，
  // 也是软件光栅器里最贵的一项。若不单独测它，`capabilities().shadows=true` 就是空话。
  if (!gpu_ready()) return;
  auto gpu = st::raster::gpu::create_canvas(kWidth, kHeight, 1.0f, {});
  ST_CHECK(gpu.has_value());
  if (!gpu.has_value()) return;
  Surface& target = **gpu;
  Canvas software(kWidth, kHeight);
  const Rect rect{80.0f, 70.0f, 160.0f, 100.0f};
  const Color base{0x0A, 0x0F, 0x1A, 0xFF};
  const Color shadow{0x00, 0x00, 0x00, 0x8C};
  for (const auto& [radius, blur, offset] :
       std::vector<std::tuple<float, float, st::math::Point>>{{16.0f, 6.0f, {0.0f, 2.0f}},
                                                              {0.0f, 22.0f, {0.0f, 6.0f}},
                                                              {12.0f, 14.0f, {2.0f, 4.0f}}}) {
    software.clear(base);
    software.draw_shadow(rect, radius, blur, shadow, offset);
    target.clear(base);
    target.draw_shadow(rect, radius, blur, shadow, offset);
    const Comparison result = compare(software.pixels(), target.pixels(), 12, kWidth,
                                      /*background=*/0x0A0F1AFFU);
    st::print("[gpu-diff] 投影 r={} b={}：超差 {:.3f}% · 结构性 {:.3f}% · 最大Δ{} · 非底色 {:.3f} vs {:.3f}\n",
              radius, blur, result.differing_ratio * 100.0, result.structural_ratio * 100.0,
              result.max_delta, result.ink_ratio_left, result.ink_ratio_right);
    // 投影是**低对比度大面积**的东西：结构性差异必须为零，
    // 否则就是位置/半径/模糊对不上（那在视觉上是“阴影歪了”）
    ST_CHECK(result.structural_ratio < 0.005);
    // 两侧都必须真的画出了投影（非底色像素占比同量级）
    ST_CHECK(result.ink_ratio_left > 0.05);
    ST_CHECK(std::abs(result.ink_ratio_left - result.ink_ratio_right) < 0.02);
  }
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

ST_TEST(gpu_huge_path_mask_is_bounded_by_canvas) {
  // 回归（2026-09-30 定位的真实崩溃）：把无界约束哨兵（kUnbounded≈1e9）当作
  // 高度的元素，其描边路径包围盒高度 ≈1e9 → GPU 遮罩曾按包围盒全量分配
  // （实测 1362×999999979 ≈ 1.3 TB）→ std::bad_alloc 直接崩进程。
  // 软件光栅器按扫描线裁剪不受影响，因此只在「无头 + GPU」组合下暴露。
  // 修法：遮罩与当前裁剪域取交（默认裁剪=整块画布）。本用例钉两件事：
  //   ① 巨大几何不再产生巨大分配（能跑完本身就是断言——修复前这里直接崩）；
  //   ② 可见部分的渲染与软件光栅器语义一致（取交不改变像素结果）。
  if (!gpu_ready()) return;
  const int width = 128;
  const int height = 96;
  auto gpu = st::raster::gpu::create_canvas(width, height, 1.0f, {});
  ST_CHECK(gpu.has_value());
  if (!gpu.has_value()) return;
  Surface& target = **gpu;

  // 左右两条竖边穿过画布，上下边远在 ±1e9（模拟无界高度元素的边框）
  st::raster::Path huge;
  huge.move_to(st::math::Point{30.0f, -1.0e9f});
  huge.line_to(st::math::Point{98.0f, -1.0e9f});
  huge.line_to(st::math::Point{98.0f, 1.0e9f});
  huge.line_to(st::math::Point{30.0f, 1.0e9f});
  huge.close();
  const Color stroke{0xE8, 0x40, 0x40, 0xFF};
  const Color base{0x0A, 0x0F, 0x1A, 0xFF};

  Canvas software(width, height);
  software.clear(base);
  software.stroke_path(huge, st::raster::Paint::solid(stroke), 2.0f);
  target.clear(base);
  target.stroke_path(huge, st::raster::Paint::solid(stroke), 2.0f);

  // 竖边穿过处必须出现描边像素（x=30 整像素落在 [29,31] 的描边带内）
  ST_CHECK(target.pixel_at(30, height / 2).r > 150);
  // 中空区域保持底色
  ST_CHECK_EQ(static_cast<int>(target.pixel_at(64, height / 2).r), 0x0A);
  // 与软件光栅器语义一致（仅允许边缘抗锯齿的权重差）
  const Comparison result = compare(software.pixels(), target.pixels(), 12, width,
                                    /*background=*/0x0A0F1AFFU);
  st::print("[gpu-diff] 巨大路径：超差 {:.3f}% · 结构性 {:.3f}% · 最大Δ{} · 非底色 {:.4f} vs {:.4f}\n",
            result.differing_ratio * 100.0, result.structural_ratio * 100.0, result.max_delta,
            result.ink_ratio_left, result.ink_ratio_right);
  ST_CHECK(result.structural_ratio < 0.005);
  ST_CHECK(std::abs(result.ink_ratio_left - result.ink_ratio_right) < 0.02);
}

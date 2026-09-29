#pragma once

/// 软件光栅画布：预乘 RGBA8 像素缓冲 + 全自绘绘制原语 + 裁剪栈。
///
/// 定位：**默认兼底与语义真相源**——无 GPU、无显示服务、非 Windows 平台都靠它，
/// 且回归测试以它为准。GPU 实现（`raster::gpu::GpuCanvas`）在容差内对齐它。
///
/// 设计要点（`DESIGN.md` §4.2）：
/// - 抗锯齿为**扫描线覆盖率**（非 MSAA），1px 发丝边框在任何 DPI 下平滑；
/// - 裁剪：矩形裁剪走快速路径（区间交集），圆角/路径裁剪走 8 位遮罩；
/// - 混合在预乘空间做 16 位中间运算，避免 8 位往返误差；
/// - **软硬件共用接口**：绘制原语全部来自 `Surface`（UI 层只认接口，
///   这样同一条绘制路径既能落到 CPU 也能落到显卡）。

#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"
#include "st/raster/surface.hpp"

namespace st::raster {

/// 8 位覆盖率遮罩（圆角/路径裁剪、阴影模糊）。
/// 一行的覆盖率**运行段**：`[x0, x1)` 上的覆盖率为 `weight`（带符号；非零环绕规则下
/// 顺时针 +、逆时针 −，重叠处的和正好抵消出孔洞）。
///
/// 为什么不用逐像素覆盖率数组：那样每行都要把整个宽度跑一遍（还要乘上子采样数），
/// 一个 460×150 的圆角卡片就是 55 万次浮点累加——实测这是路径填充最大的开销。
/// 运行段表示下，每行的代价是 O(段数) + 段内 SIMD 混合。
///
/// 注：这是**软件光栅器内部**的表示，不属于 `Surface` 接口。
struct CoverageRun {
  float x0{0.0f};
  float x1{0.0f};
  float weight{0.0f};
};

class Mask {
 public:
  Mask(int width, int height);

  [[nodiscard]] auto width() const noexcept -> int { return width_; }
  [[nodiscard]] auto height() const noexcept -> int { return height_; }
  [[nodiscard]] auto empty() const noexcept -> bool { return width_ <= 0 || height_ <= 0; }
  [[nodiscard]] auto at(int x, int y) const noexcept -> std::uint8_t;
  void set(int x, int y, std::uint8_t value) noexcept;
  [[nodiscard]] auto values() noexcept -> std::span<std::uint8_t>;
  [[nodiscard]] auto values() const noexcept -> std::span<const std::uint8_t>;
  /// 覆盖率抽样（双线性，坐标为遮罩局部坐标）。
  [[nodiscard]] auto sample(float x, float y) const noexcept -> float;

 private:
  int width_{0};
  int height_{0};
  std::vector<std::uint8_t> values_{};
};

class Canvas final : public Surface {
 public:
  /// 直接指定**物理**缓冲尺寸（`device_scale` 默认 1.0，此时逻辑坐标 = 物理像素）。
  Canvas(int physical_width, int physical_height, float device_scale = 1.0f);
  /// 按**逻辑**尺寸与 DPI 缩放建画布：物理缓冲 = round(logical × scale)。
  [[nodiscard]] static auto for_logical_size(int logical_width, int logical_height,
                                             float device_scale) -> Canvas;
  ~Canvas();
  Canvas(const Canvas&) = delete;
  auto operator=(const Canvas&) -> Canvas& = delete;
  Canvas(Canvas&& other) noexcept;
  auto operator=(Canvas&& other) noexcept -> Canvas&;

  /// 逻辑尺寸（UI 视角；= 物理尺寸 / device_scale）——由 `Surface` 基类从物理尺寸折算。
  /// 物理缓冲尺寸（像素存取/编码视角）。
  [[nodiscard]] auto physical_width() const noexcept -> int override { return physical_width_; }
  [[nodiscard]] auto physical_height() const noexcept -> int override { return physical_height_; }
  /// DPI 缩放（物理像素 / 逻辑像素）。
  [[nodiscard]] auto device_scale() const noexcept -> float override { return scale_; }
  void set_device_scale(float scale) noexcept override;
  /// 逻辑矩形 → 物理整数矩形（向外取整，覆盖完整像素）。
  [[nodiscard]] auto to_physical(math::Rect rect) const noexcept -> math::IntRect override;
  [[nodiscard]] auto to_physical(math::Point point) const noexcept -> math::Point override;

  void clear(math::Color color) override;

  // —— 绘制剖析（可选；`set_profiler(nullptr)` 即关闭，无额外开销） ——
  void set_profiler(PaintProfiler* profiler) noexcept override { profiler_ = profiler; }
  [[nodiscard]] auto profiler() const noexcept -> PaintProfiler* override { return profiler_; }
  /// 手动记账（文本等跨模块绘制路径用）：把一段耗时记到指定原语。
  void add_profile(PaintOp op, double ms, std::uint64_t pixels = 0) noexcept override {
    if (profiler_ != nullptr) profiler_->add(op, ms, pixels);
  }

  // —— 绘制原语 ——
  void fill_rect(math::Rect rect, const Paint& paint, float radius = 0.0f,
                 DrawOptions options = {}) override;
  void fill_path(const Path& path, const Paint& paint, DrawOptions options = {}) override;
  /// 描边（圆头圆角连接；宽度为总宽）。
  void stroke_path(const Path& path, const Paint& paint, float width,
                   DrawOptions options = {}) override;
  void fill_circle(math::Point center, float radius, const Paint& paint,
                   DrawOptions options = {}) override;
  /// 投影（半径 `blur` 的近似高斯模糊，偏移 `offset`）。
  void draw_shadow(math::Rect rect, float radius, float blur, math::Color color,
                   math::Point offset = {}, DrawOptions options = {}) override;
  /// 位图合成（双线性缩放）。
  void draw_canvas(const Surface& source, math::Rect destination, DrawOptions options = {}) override;
  void draw_canvas_at(const Surface& source, int x, int y, DrawOptions options = {}) override;
  /// 覆盖率位图混合（文字/遮罩）：软件实现逐行走 `blend_coverage_row`。
  void blend_coverage_bitmap(int x, int y, std::span<const float> coverage, int width, int height,
                             const Paint& paint, float opacity, BlendMode blend) override;

  // —— 裁剪 ——
  /// 裁剪（逻辑坐标入参；内部按 `device_scale` 换算到物理像素）。
  void push_clip_rect(math::Rect rect) override;
  /// 圆角矩形裁剪（**逻辑坐标**；半径同样按 DPI 缩放）。
  void push_clip_rounded_rect(math::Rect rect, float radius) override;
  /// 路径裁剪（**路径按物理像素解释**——与 raster 层其余 API 一致；
  /// 持有逻辑坐标路径时先 `path.scaled(canvas.device_scale())`）。
  void push_clip_path(const Path& path) override;
  void pop_clip() override;
  [[nodiscard]] auto clip_rect() const noexcept -> math::IntRect override;
  [[nodiscard]] auto has_mask_clip() const noexcept -> bool override;

  // —— 像素访问（物理像素；逻辑坐标访问用 pixel_at_point）——
  [[nodiscard]] auto pixel_at(int x, int y) const -> math::Color override;
  void set_pixel(int x, int y, math::Color color) override;
  /// 逻辑坐标取色（控制通道/测试用）。
  [[nodiscard]] auto pixel_at_point(math::Point point) const -> math::Color override;
  [[nodiscard]] auto pixels() const noexcept -> std::span<const std::uint32_t> override {
    return pixels_;
  }
  [[nodiscard]] auto pixels() noexcept -> std::span<std::uint32_t> override { return pixels_; }
  /// 导出为直通 RGBA8 字节流（PNG 编码/截图用；物理分辨率）。
  [[nodiscard]] auto to_rgba8() const -> std::vector<std::uint8_t> override;
  /// 非全透明像素的最小包围盒（**物理**像素；测试与截图裁剪用）。
  [[nodiscard]] auto content_bounds() const noexcept -> math::IntRect override;
  /// 同上，但换算到逻辑像素。
  [[nodiscard]] auto content_bounds_logical() const noexcept -> math::IntRect override;

  // —— 软件光栅器专用（**不属于 `Surface` 接口**）——
  //
  // 这些是 CPU 光栅化的内部机制：路径光栅化逐行交付覆盖率运行段、阴影遮罩构建、
  // 逐像素混合细节。它们与“绘制一个形状”这个**语义**无关，而是本实现的做法。
  // 把它们留在接口上会逼 GPU 实现去模拟 CPU 的数据结构（那是错的方向）。

  /// 低层：以覆盖率调制画笔颜色混合一行（栅格化器与自定义绘制使用）。
  /// `coverage[0]` 对应该行 `x_begin` 像素。
  void blend_coverage_row(int y, int x_begin, std::span<const float> coverage, const Paint& paint,
                          float opacity, BlendMode blend);
  /// 低层：按**覆盖率运行段**混合一行（路径光栅化的主路径）。
  /// 与逐像素覆盖率数组语义一致（端点像素按小数分摊），但只会碰“真的有覆盖”的像素。
  void blend_coverage_runs(int y, std::span<const CoverageRun> runs, const Paint& paint,
                           float opacity, BlendMode blend);

 private:
  struct ClipFrame {
    math::IntRect rect{};  ///< 物理像素
    std::shared_ptr<const Mask> mask{};  ///< 空表示仅矩形裁剪
    int mask_origin_x{0};
    int mask_origin_y{0};
  };

  [[nodiscard]] auto effective_alpha(int x, int y, float coverage) const noexcept -> float;
  void blend_span(int y, int x_begin, int x_end, math::Color color, float opacity, BlendMode mode);
  [[nodiscard]] auto current_clip() const noexcept -> const ClipFrame&;
  /// 取（或生成并缓存）阴影遮罩。
  ///
  /// 阴影形状 + 模糊结果**只取决于几何参数**（宽/高/圆角/模糊半径/偏移），与画布内容、
  /// 绘制位置无关：同一张卡片每帧重新光栅化 + 模糊同一张遮罩是纯浪费（实测阴影是
  /// 单帧最重的绘制项，而卡片尺寸在整个界面里往往只有三五种）。
  /// 参数按 0.25 像素量化后做键——几何量在帧间有浮点噪声，不量化则永远命不中。
  [[nodiscard]] auto shadow_mask(float width, float height, float radius, float blur,
                                 math::Point offset) -> std::shared_ptr<const Mask>;
  /// 物理像素坐标 → 逻辑坐标（画笔/渐变采样用）。
  [[nodiscard]] auto to_logical_point(float physical_x, float physical_y) const noexcept
      -> math::Point;

  int physical_width_{0};
  int physical_height_{0};
  float scale_{1.0f};
  float inverse_scale_{1.0f};
  std::vector<std::uint32_t> pixels_{};
  std::vector<ClipFrame> clip_stack_{};
  /// 阴影遮罩缓存（键 → 遮罩）。上限 `kShadowMaskCacheLimit`，超限丢弃最早的一项。
  std::vector<std::pair<std::uint64_t, std::shared_ptr<const Mask>>> shadow_masks_{};
  PaintProfiler* profiler_{nullptr};  ///< 空 = 不剖析
};

/// 盒式模糊 ×3 近似高斯（阴影/毛玻璃用）；就地修改遮罩。
void blur_mask(Mask& mask, float radius);

}  // namespace st::raster

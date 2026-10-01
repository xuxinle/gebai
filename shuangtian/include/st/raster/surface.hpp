#pragma once

/// 绘制目标抽象（`Surface`）：UI/文本层只依赖它，不依赖任何具体光栅化实现。
///
/// 为什么要有这一层：框架原先只有软件光栅器（`raster::Canvas`），它与渲染后端是绑死的。
/// 引入 GPU 后，同一条绘制路径必须能落到两种实现上——UI 层不该知道"这一帧是 CPU 画的
/// 还是显卡画的"。`Surface` 就是那个边界：
///
/// | 实现 | 定位 |
/// |---|---|
/// | `raster::Canvas` | 软件光栅器。**语义真相源**：无 GPU / 非支持平台 100% 可用，测试以它为准 |
/// | `raster::gpu::GpuCanvas` | D3D11（硬件 → WARP 回退）。默认首选，能力与软件对齐 |
///
/// 两条约束：
/// 1. **接口只放"两边都能做"的原语**。像"按覆盖率运行段混合一行"这种软件内部机制
///    留在 `Canvas` 上（`Surface` 不暴露），否则 GPU 实现被迫模拟 CPU 的内部数据结构。
/// 2. **像素访问是接口的一部分**（`pixels`/`to_rgba8`/`pixel_at`）：控制通道的截图、
///    回归测试的逐像素断言都依赖它。GPU 实现走回读，慢但正确——它只在"要看像素"时才走。

#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"

namespace st::raster {

/// 绘制原语分类（绘制剖析用）。
///
/// 为什么要分到这么细：绘制的代价高度不均——一次阴影（遮罩光栅化 + 模糊 + 逐像素混合）
/// 可能贵过一百次纯色填充。只报“一帧 30ms”无法推出该改哪里。
enum class PaintOp : std::uint8_t {
  Clear,      ///< 整屏清色
  FillRect,   ///< 轴向对齐矩形填充
  FillRoundRect,  ///< 圆角矩形（走路径光栅化）
  FillPath,   ///< 任意路径填充（图标、自绘形状）
  Stroke,     ///< 描边
  Shadow,     ///< 投影（含遮罩光栅化与模糊）
  Image,      ///< 位图合成（纹理/离屏画布）
  ClipMask,   ///< 非矩形裁剪（圆角/路径裁剪的遮罩构建）
  Text,       ///< 文本（字形位图混合）
  Count,      ///< 哨兵
};

[[nodiscard]] auto paint_op_name(PaintOp op) -> std::string_view;

/// 单类原语的累计统计。
struct PaintOpStat {
  std::uint64_t calls{0};
  /// 覆盖的像素数（近似：调用时按各原语的输出面积估算）
  std::uint64_t pixels{0};
  double ms{0.0};
};

/// 绘制剖析器：**默认不挂到画布上**（挂了才计时，不挂零开销）。
struct PaintProfiler {
  std::array<PaintOpStat, static_cast<std::size_t>(PaintOp::Count)> ops{};

  void add(PaintOp op, double ms, std::uint64_t pixels = 0);
  [[nodiscard]] auto op(PaintOp which) const noexcept -> const PaintOpStat&;
  [[nodiscard]] auto total_ms() const noexcept -> double;
  void clear() noexcept;
};

/// 覆盖率位图的**通道布局**（决定 `blend_coverage_bitmap` 如何解释那张图）。
///
/// 为什么要把它带进接口：同一张「覆盖率图」有两种物理含义完全不同的排布，
/// 而它们的**混合公式不一样**（灰度是单 α，亚像素是逐通道 α）——
/// 把它留成调用方的口头约定，后端就只能靠猜，猜错的表现是「字变糊」或「字变形」。
enum class CoverageFormat : std::uint8_t {
  /// 每像素 1 个覆盖率（`w × h`）——灰度抗锯齿（路径遮罩、无头/截图口径）。
  Grayscale,
  /// 每像素 R/G/B 三个覆盖率（`w × h × 3`，行优先、像素内 R→G→B 交错）——
  /// LCD 亚像素（ClearType 类）渲染。
  ///
  /// 为什么是「交错三值」而不是三条平面：GPU 侧要把它直接传成一张 RGB 纹理
  /// （采样一次就拿到三个通道），交错是唯一能零拷贝对齐纹理格式的排布。
  Lcd,
};

/// 绘制目标：所有绘制原语、裁剪与像素读回的统一入口。
class Surface {
 public:
  Surface(const Surface&) = delete;
  auto operator=(const Surface&) -> Surface& = delete;
  // 接口本身无数据，移动就是“能力转移”（具体实现自己决定要不要真移动缓冲）
  Surface(Surface&&) = default;
  auto operator=(Surface&&) -> Surface& = default;
  virtual ~Surface() = default;

  // —— 尺寸与坐标换算 ——

  /// 逻辑尺寸（UI 视角；= 物理尺寸 / device_scale）。
  [[nodiscard]] auto width() const noexcept -> int {
    return static_cast<int>(std::lround(static_cast<float>(physical_width()) / device_scale()));
  }
  [[nodiscard]] auto height() const noexcept -> int {
    return static_cast<int>(std::lround(static_cast<float>(physical_height()) / device_scale()));
  }
  [[nodiscard]] virtual auto physical_width() const noexcept -> int = 0;
  [[nodiscard]] virtual auto physical_height() const noexcept -> int = 0;
  /// DPI 缩放（物理像素 / 逻辑像素）。
  [[nodiscard]] virtual auto device_scale() const noexcept -> float = 0;
  /// 是否支持**区域重绘**（清局部 + 按裁剪域局部重绘）。
  ///
  /// 软件画布可以直接寻址缓冲，局部路径成本与区域成正比；GPU 画布整帧仅 ~1ms，
  /// 且局部路径需要另维护一套状态，因此默认不支持（调用方回落整帧语义）。
  [[nodiscard]] virtual auto supports_partial_repaint() const noexcept -> bool { return false; }
  /// 运行时切换 DPI（重建后端缓冲；软件与 GPU 语义一致）。
  virtual void set_device_scale(float scale) = 0;
  [[nodiscard]] auto logical_bounds() const noexcept -> math::Rect {
    return math::Rect{0.0f, 0.0f, static_cast<float>(width()), static_cast<float>(height())};
  }
  [[nodiscard]] auto bounds() const noexcept -> math::Rect { return logical_bounds(); }
  [[nodiscard]] auto int_bounds() const noexcept -> math::IntRect {
    return math::IntRect{0, 0, width(), height()};
  }
  [[nodiscard]] auto physical_bounds() const noexcept -> math::IntRect {
    return math::IntRect{0, 0, physical_width(), physical_height()};
  }
  /// 逻辑矩形 → 物理整数矩形（向外取整，覆盖完整像素）。
  [[nodiscard]] virtual auto to_physical(math::Rect rect) const noexcept -> math::IntRect = 0;
  [[nodiscard]] virtual auto to_physical(math::Point point) const noexcept -> math::Point = 0;
  /// 逻辑尺寸 → 物理像素数。
  [[nodiscard]] auto scale_length(float logical) const noexcept -> float {
    return logical * device_scale();
  }

  // —— 绘制剖析（可选；`set_profiler(nullptr)` 即关闭，无额外开销） ——
  virtual void set_profiler(PaintProfiler* profiler) noexcept = 0;
  [[nodiscard]] virtual auto profiler() const noexcept -> PaintProfiler* = 0;
  /// 手动记账（文本等跨模块绘制路径用）：把一段耗时记到指定原语。
  virtual void add_profile(PaintOp op, double ms, std::uint64_t pixels = 0) noexcept = 0;

  // —— 绘制原语 ——
  virtual void clear(math::Color color) = 0;
  virtual void fill_rect(math::Rect rect, const Paint& paint, float radius = 0.0f,
                         DrawOptions options = {}) = 0;
  virtual void fill_path(const Path& path, const Paint& paint, DrawOptions options = {}) = 0;
  /// 描边（圆头圆角连接；宽度为总宽）。
  virtual void stroke_path(const Path& path, const Paint& paint, float width,
                           DrawOptions options = {}) = 0;
  virtual void fill_circle(math::Point center, float radius, const Paint& paint,
                           DrawOptions options = {}) = 0;
  /// 投影（半径 `blur` 的近似高斯模糊，偏移 `offset`）。
  virtual void draw_shadow(math::Rect rect, float radius, float blur, math::Color color,
                           math::Point offset = {}, DrawOptions options = {}) = 0;
  /// 位图合成（双线性缩放；`source` 可为软件画布或 GPU 目标）。
  virtual void draw_canvas(const Surface& source, math::Rect destination,
                           DrawOptions options = {}) = 0;
  virtual void draw_canvas_at(const Surface& source, int x, int y, DrawOptions options = {}) = 0;

  /// **覆盖率位图混合**：把一张 8 位覆盖率的图（每项 0..1）以 `paint` 混到物理坐标 `(x, y)`。
  ///
  /// 为什么它是接口的一部分（而不是让文字去调软件内部的"按行混合"）：
  /// 字形渲染产出的就是**覆盖率位图**，而这是"CPU 与 GPU 都能做"的语义——
  /// 软件实现逐行混合，GPU 实现把它上传成 A8 纹理再画一个四边形。
  /// 若让文字直接依赖软件的行混合 API，GPU 路径就没法画字（而文字是界面里最常见的原语）。
  ///
  /// `coverage` 为**行优先**的紧凑数组（`width × height` 项）。
  /// `cache_key`：这份覆盖率位图的**稳定身份**（同一形状/字形跨帧、跨缓存重建都不变）。
  /// GPU 后端据此缓存上传好的纹理——**不能按指针缓存**：
  /// 位图的宿主容器可能被清空并释放，新位图复用同一地址，于是"按指针命中"
  /// 会把**上一个形状的纹理**当成这个形状的（实测症状：界面文字间歇性变成别的字）。
  /// 传 0 表示"没有稳定身份"——此时后端不得缓存，只能每次重建（正确性优先）。
  ///
  /// `format`：覆盖率的通道布局（见 `CoverageFormat`）。`Lcd` 时每像素三个值，
  /// 混合按**逐通道 α**做：`out_c = S_c·α_c + D_c·(1 - a_s·α_c)`（`S` 预乘源色、
  /// `a_s` 源 alpha）——这正是亚像素渲染的彩边来源，`α_c` 不能退化成标量
  /// （黑字压白底时彩边全在 `D_c·(1-α_c)` 那一项上，退化了就等于什么都没做）。
  virtual void blend_coverage_bitmap(int x, int y, std::span<const float> coverage, int width,
                                     int height, const Paint& paint, float opacity,
                                     BlendMode blend, std::uint64_t cache_key = 0,
                                     CoverageFormat format = CoverageFormat::Grayscale) = 0;

  // —— 裁剪（逻辑坐标入参；内部按 `device_scale` 换算到物理像素） ——
  virtual void push_clip_rect(math::Rect rect) = 0;
  virtual void push_clip_rounded_rect(math::Rect rect, float radius) = 0;
  /// 路径裁剪（**路径按物理像素解释**——与 raster 层其余 API 一致；
  /// 持有逻辑坐标路径时先 `path.scaled(surface.device_scale())`）。
  virtual void push_clip_path(const Path& path) = 0;
  virtual void pop_clip() = 0;
  [[nodiscard]] virtual auto clip_rect() const noexcept -> math::IntRect = 0;
  [[nodiscard]] virtual auto has_mask_clip() const noexcept -> bool = 0;

  // —— 像素访问（物理像素；逻辑坐标访问用 pixel_at_point） ——
  [[nodiscard]] virtual auto pixel_at(int x, int y) const -> math::Color = 0;
  virtual void set_pixel(int x, int y, math::Color color) = 0;
  /// 逻辑坐标取色（控制通道/测试用）。
  [[nodiscard]] virtual auto pixel_at_point(math::Point point) const -> math::Color = 0;
  /// 预乘 RGBA8 像素（**可能触发 GPU 回读**；只在"要看像素"时用）。
  [[nodiscard]] virtual auto pixels() const noexcept -> std::span<const std::uint32_t> = 0;
  [[nodiscard]] virtual auto pixels() noexcept -> std::span<std::uint32_t> = 0;
  /// 导出为直通 RGBA8 字节流（PNG 编码/截图用；物理分辨率）。
  [[nodiscard]] virtual auto to_rgba8() const -> std::vector<std::uint8_t> = 0;
  /// 非全透明像素的最小包围盒（**物理**像素；测试与截图裁剪用）。
  [[nodiscard]] virtual auto content_bounds() const noexcept -> math::IntRect = 0;
  /// 同上，但换算到逻辑像素。
  [[nodiscard]] virtual auto content_bounds_logical() const noexcept -> math::IntRect = 0;

 protected:
  Surface() = default;
};

}  // namespace st::raster

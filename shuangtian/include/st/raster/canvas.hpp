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

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/paint.hpp"
#include "st/math/path.hpp"
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
  /// 软件光栅器支持“区域重绘”：清局部 + 按裁剪域局部重绘（缓冲可直接寻址）。
  /// GPU 画布暂不接（整帧仅 ~1ms，局部路径要另维护一套状态，收益不划算）。
  [[nodiscard]] auto supports_partial_repaint() const noexcept -> bool override { return true; }
  void set_device_scale(float scale) noexcept override;

  /// 原地改尺寸：**只重开像素缓冲与基础裁剪帧**，阴影遮罩缓存保留。
  ///
  /// 为什么阴影缓存要留：那些遮罩只取决于几何参数（卡片尺寸/圆角/模糊半径），
  /// 与画布尺寸无关。拖动缩放时每换一次尺寸就丢掉它们，会让每一帧都为同样的
  /// 卡片重新光栅化 + 模糊（实测阴影是单帧最重的绘制项）。
  /// 注意：裁剪栈必须重置为"整块新画布"——旧裁剪帧的矩形是按旧尺寸算的。
  [[nodiscard]] auto resize(int physical_width, int physical_height) -> Status override;
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
  /// 描边（宽度为总宽；线帽/连接由 `style` 决定，缺省 butt/miter = SVG 默认）。
  void stroke_path(const Path& path, const Paint& paint, float width,
                   const StrokeStyle& style = StrokeStyle{},
                   DrawOptions options = {}) override;
  void fill_circle(math::Point center, float radius, const Paint& paint,
                   DrawOptions options = {}) override;
  /// 投影（半径 `blur` 的近似高斯模糊，偏移 `offset`）。
  void draw_shadow(math::Rect rect, float radius, float blur, math::Color color,
                   math::Point offset = {}, DrawOptions options = {}) override;
  /// **两层投影合一**：把两层预合成成**一张**缓存贴图，只做一次逐像素合成。
  ///
  /// 收益与正确性论证见 `Surface::draw_shadow_layered`（接口处）与
  /// `docs/PAINT_DIAGNOSIS.md` §3.1：合成核从"每层读遮罩 + 查表 + `over_premul`"（4.82 ns/px）
  /// 退化成纯 `over_premul`（2.17 ns/px），且两层的非零区域并集只有各自之和的 ~57%。
  void draw_shadow_layered(math::Rect rect, float radius, math::Color key_color, float key_blur,
                           math::Point key_offset, math::Color ambient_color,
                           float ambient_blur, math::Point ambient_offset,
                           DrawOptions options = {}) override;
  /// 位图合成（双线性缩放）。
  void draw_canvas(const Surface& source, math::Rect destination, DrawOptions options = {}) override;
  void draw_canvas_at(const Surface& source, int x, int y, DrawOptions options = {}) override;
  /// 覆盖率位图混合（文字/遮罩）：软件实现逐行走 `blend_coverage_row`
  /// （`CoverageFormat::Lcd` 时走 `blend_coverage_row_subpixel`）。
  void blend_coverage_bitmap(int x, int y, std::span<const float> coverage, int width, int height,
                             const Paint& paint, float opacity, BlendMode blend,
                             std::uint64_t cache_key = 0,
                             CoverageFormat format = CoverageFormat::Grayscale) override;
  /// 逐行灰度混合（软件内部机制；`Surface` 默认实现按行驱动它）。
  void blend_coverage_row(int y, int x_begin, std::span<const float> coverage, const Paint& paint,
                          float opacity, BlendMode blend) override;
  /// 逐行亚像素混合（转发到已有的 `blend_coverage_row_subpixel`，算法不变）。
  void blend_coverage_row_lcd(int y, int x_begin, std::span<const float> coverage,
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
  /// **回写整体像素**（直通 RGBA8、非预乘、行优先，尺寸必须与画布一致）。
  ///
  /// 用途：后处理类操作把“已经算好的整帧”放回画布。
  /// 典型场景是**运动模糊**：把快门区间多个子帧在线性光里平均之后，
  /// 结果已经是一整帧，需要有地方“放回去”（不能把每个子帧再画一遍——那又变回多层叠了）。
  ///
  /// 预乘约定：内部缓冲是预乘的，所以这里会把直通值乘上 alpha 再存。
  /// 运动模糊只处理不透明画面（alpha=255），乘不乘都一样，但保持语义一致。
  auto replace_rgba8(std::span<const std::uint8_t> bytes, int width, int height) -> bool;
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

  /// 低层：**亚像素**版——每像素三个覆盖率（`[R,G,B]` 交错）。
  /// `coverage[0..2]` 对应该行 `x_begin` 像素的三个子像素。
  ///
  /// 与单通道版的唯一区别是混合公式：`out_c = S_c·α_c + D_c·(1 - a_s·α_c)`，
  /// 即**目标衰减也逐通道**。非 `SrcOver` 模式没有逐通道语义，本函数退化为
  /// 三通道均值（等价于灰度路径）并在文档里如实说明，不假装支持。
  void blend_coverage_row_subpixel(int y, int x_begin, std::span<const float> coverage,
                                   const Paint& paint, float opacity, BlendMode blend);
  /// 低层：按**覆盖率运行段**混合一行（路径光栅化的主路径）。
  /// 与逐像素覆盖率数组语义一致（端点像素按小数分摊），但只会碰“真的有覆盖”的像素。
  void blend_coverage_runs(int y, std::span<const CoverageRun> runs, const Paint& paint,
                           float opacity, BlendMode blend);
  /// 把路径覆盖率写入画布（`fill_path` / `fill_circle` / 描边等所有路径绘制的共同出口）。
  ///
  /// 为何包一层而不是各处直接调 `detail::fill_path_aa`：`clear` 后的“画布还没被碰过”
  /// 标记必须**在任何一次像素写入时失效**，否则“跳过重复铺底”会把已画的内容当不存在。
  /// 实测：`fill_circle` 曾经直接调内部函数、绕过 `fill_path`，于是“先画圆再铺同色底”
  /// 两块输出差 **2040 像素**。
  void rasterize_path_runs(const Path& path, const Paint& paint, DrawOptions options);

  /// 几何 sprite 缓存的命中/未命中计数（进程内累计，供 `metrics` 回答
  /// 「缓存到底有没有生效」——实测过“加了缓存反而更慢”时，第一步就是查命中率）。
  struct GeometryStats {
    std::uint64_t hits{0};
    std::uint64_t misses{0};
    std::uint64_t rejected{0};  ///< 因硬条件不满足而完全没进缓存路径
    std::size_t entries{0};
  };
  [[nodiscard]] auto geometry_stats() const noexcept -> GeometryStats;
  /// 运行时开关几何 sprite 缓存（默认开）。
  ///
  /// 为何要可运行时切换：验证“缓存不改像素”的唯一干净办法是**同一进程里把两侧都跑一遍**
  /// （同一块画布、同一份代码、同一批路径），只差这个开关。
  /// 换成“开/关两个进程各跑一次”会引入动画/时序/首次预热差异（实测：同配置两次截图
  /// 本来就不一致），差异无法归因。
  void set_geometry_cache_enabled(bool enabled) noexcept { geometry_cache_enabled_ = enabled; }

  /// 运行时开关「阴影按同 α 游程分组」（默认开）。
  ///
  /// **验证必备**：这条优化把行内连续同遮罩值的像素当作一段，长段走 SIMD。
  /// 它很容易写错游程边界（少一像素 / 多一像素 / 漏最后一段），而错误只在
  /// 半透明边缘可见——少画一次只是“阴影淡一点”，肉眼几乎看不出。
  /// 零容差的验证只能靠“开与不开逐像素相同”（同 `set_geometry_cache_enabled` 的理由）。
  void set_shadow_run_grouping_enabled(bool enabled) noexcept {
    shadow_run_grouping_enabled_ = enabled;
  }

  /// 运行时开关「`blend_coverage_row` 用查表代替逐像素 `scale_premul`」（默认开）。
  ///
  /// **验证必备**：表的量化口径必须与逐像素**逐字相同**。一旦把 opacity 也烧进表，
  /// 量化就从一次变两次，实测（`tools/alpha_table_equiv_probe.cpp`）在
  /// opacity=0.9/0.75/0.5 时有 **19~25%** 的覆盖率组合差 1 LSB。
  /// 零容差的验证只能靠“开与不开逐像素相同”（同 `set_geometry_cache_enabled`）。
  void set_alpha_table_enabled(bool enabled) noexcept { alpha_table_enabled_ = enabled; }

  [[nodiscard]] auto geometry_cache_enabled() const noexcept -> bool {
    return geometry_cache_enabled_;
  }
  /// 运行时开关「清屏 + 首层同色不透明整屏底」的合并（默认开）。
  ///
  /// **验证必备**：这条快路径的正确性只能靠“开与不开逐像素相同”证明，
  /// 而那必须同一份二进制、同一块画布上跑两遍（换进程比会被预热/时序污染）。
  void set_skip_redundant_clear_fill_enabled(bool enabled) noexcept {
    skip_redundant_clear_fill_ = enabled;
  }
  /// 清空几何 sprite 缓存（`resize` / 帧循环重启时调）。
  void clear_geometry_sprites();

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
  /// 取（或生成并缓存）**两层合一的阴影贴图**（预乘 RGBA + 每行非零跨度）。
  ///
  /// 与 `shadow_mask` 同一思路（键 = 几何 + 两层的模糊/偏移/颜色，按 0.25 像素量化），
  /// 但把两层**预合成**在一张中性缓冲上：于是往画布上贴只是一次 `over_premul`
  /// （不需要逐像素读遮罩 + 查表），而且两层的非零区域取并集后重叠部分只合成一次。
  /// 贴图存的是**阴影自身的预乘色**（不含目标），因此对任何底色都正确——
  /// `src-over` 满足结合律，"先合并两层再贴" 与 "依次贴两次" 逐像素等价。
  struct LayeredShadow {
    std::shared_ptr<const std::vector<std::uint32_t>> pixels{};   ///< 预乘，行优先
    /// 每行 [first, last] 的非零跨度（相对贴图原点）；`first < 0` = 该行全零。
    std::shared_ptr<const std::vector<std::pair<int, int>>> spans{};
    int x{0};
    int y{0};
    int width{0};
    int height{0};
  };
  [[nodiscard]] auto layered_shadow(float width, float height, float radius,
                                    math::Color key_color, float key_blur, math::Point key_offset,
                                    math::Color ambient_color, float ambient_blur,
                                    math::Point ambient_offset) -> const LayeredShadow*;
  /// 物理像素坐标 → 逻辑坐标（画笔/渐变采样用）。
  [[nodiscard]] auto to_logical_point(float physical_x, float physical_y) const noexcept
      -> math::Point;

  /// **几何 sprite 缓存**（单色图案的预烘坝贴图）。
  ///
  /// ## 为何要它（2026-10 性能轮的最大一项）
  ///
  /// 图标与环形边框这类图形有两个特征：① 尺寸小、次数多（gallery 一帧 46 次描边）；
  /// ② **几何与颜色每帧完全相同**（同一批图标的位置变了又变，但形状/颜色/DPI 不变）。
  /// 而 `fill_path`/`stroke_path` 每次都重走整条管线：
  /// `flatten → 边表排序 → 4× 子采样 → 按行 merge_runs → 按像素混合`。
  /// 实测一次 20×20 图标的描边是 **91 µs**，而它的逐像素混合不到 1 µs——
  /// 也就是说 **>99% 的工作是重建已经知道的东西**。
  ///
  /// ## 何时命中（硬条件，全在 `try_geometry_sprite`）
  ///
  /// - 纯色画笔 + `SrcOver`/`Src` + `antialias` 开 + 无遮罩裁剪；
  /// - 路径的扁平化点集与参数（颜色/DPI/描边样式）哈希一致；
  /// - 绘制面积小于 `kGeometrySpriteMaxPixels`（大面积图案重建一次不算亏，而缓存
  ///   会持续吃内存）。
  ///
  /// 注：性能门禁与探针都用**同一批流程**绘制，所以缓存对它们同样生效——
  /// 那正是“用户实际感受到的每帧成本”。首次建贴图的一次性成本由 `fill_path`/
  /// `stroke_path` 的剖析条目如实体现（它属于首帧，不属于稳态）。
  ///
  /// ## 与直接绘制等价
  ///
  /// 贴图存的是「该图案在**无背景**上、以 `Src` 写入的预乘色」，键包含颜色与
  /// 不透明度；命中后按 `SrcOver` 贴到目标上。于是与“先用遮罩算出覆盖率、
  /// 再 `scale_premul` + `over_premul`”**逐位相同**（覆盖率量化口径一致；
  /// `src-over` 对 `Src` 预乘色成立）。线条绕向/孔洞由带符号覆盖率保留。
  struct GeometrySprite {
    std::shared_ptr<const std::vector<std::uint32_t>> pixels{};  ///< 预乘，行优先
    /// 每行 [first, last] 的非零跨度（相对贴图原点）；`first < 0` = 该行全零。
    std::shared_ptr<const std::vector<std::pair<int, int>>> spans{};
    int x{0};
    int y{0};
    int width{0};
    int height{0};
  };
  /// 以**物理坐标路径**尝试几何贴图缓存（命中或建图后已交付则返回 true）。
  ///
  /// `source` 用来算几何键（必须是**未缩放的源路径**——键里混了 `scale`，
  /// 同一路径在 1x/2x 下必须是不同条目），`physical` 用来建贴图（已乘 `scale_`）。
  ///
  /// 抽出来的理由：`fill_rect` 的圆角分支建路径时**已经是物理坐标**（它开头已乘过
  /// `scale_`），直接再交给 `fill_path` 会被再缩一次（双重缩放）。
  auto fill_physical_path_with_cache(const Path& source, const Path& physical, const Paint& paint,
                                     DrawOptions options) -> bool;

  /// 尝试命中几何 sprite 并直接贴到画布（返回 true = 已交付）。
  ///
  /// ## 为何键建在**源路径**上而不是扁平化结果上
  ///
  /// 描边的最贵一步是 `stroke_to_path`（扁平化 + 每段四边形 + 顶点补圆）——
  /// 如果只对“扩展后的轮廓”做键，这一步永远跑在最前面，缓存就只省了扫描线。
  /// 所以键直接取**命令流的控制点**（不扁平化，O(命令数) 且无浮点开销），
  /// 未命中时才调 `build`（= `stroke_to_path` 或 DPI 缩放）。
  ///
  /// `build` 只在未命中时被调用一次，它返回的路径既是贴图来源、也是本帧的交付物。
  auto try_geometry_sprite(std::uint64_t geometry_key, const Paint& paint, float opacity,
                           BlendMode blend, const std::function<Path()>& build,
                           math::Point draw_origin) -> bool;
  /// 把“源路径（**已平移到控制点包围盒原点**）+ 缩放”混成键（供 `fill_path` 用）。
  ///
  /// 为何要先平移：贴图必须与**绘制位置无关**，否则第一次画在 (100,50) 的图标
  /// 被复用时会被贴回 (100,50)（实测踩过：两个簇 449 像素错位）。
  /// 平移量取控制点包围盒的整数原点（O(命令数)，无需扁平化），因此同一条
  /// 几何在任何位置都会得到**同一个键**。
  [[nodiscard]] static auto path_geometry_key(const Path& path, float scale, std::uint64_t extra,
                                             math::Point origin) -> std::uint64_t;
  /// 控制点包围盒的**整数原点**（向外留 `pad`）：贴图的平移基准。
  ///
  /// 只取决于「形状 + 缩放 + pad」，与绘制位置无关——因此同一条几何在任何位置
  /// 都得到同一个原点，贴图才能跨位置复用。
  [[nodiscard]] static auto path_control_origin(const Path& path, float scale, float pad = 1.0f)
      -> math::Point;

  /// 把一张已建好的几何贴图贴到画布（`try_geometry_sprite` 的交付半步）。
  /// `origin` 是本次绘制的位置（贴图内部坐标相对它）。
  auto blit_geometry_sprite(const GeometrySprite& sprite, math::Point origin) -> bool;

  int physical_width_{0};
  int physical_height_{0};
  float scale_{1.0f};
  float inverse_scale_{1.0f};
  std::vector<std::uint32_t> pixels_{};
  std::vector<ClipFrame> clip_stack_{};
  /// 阴影遮罩缓存（键 → 遮罩）。上限 `kShadowMaskCacheLimit`，超限丢弃最早的一项。
  std::vector<std::pair<std::uint64_t, std::shared_ptr<const Mask>>> shadow_masks_{};
  /// 两层合一阴影贴图缓存（键 → 贴图）。上限同 `shadow_mask`，超限丢弃最早一项。
  std::vector<std::pair<std::uint64_t, LayeredShadow>> layered_shadows_{};
  /// 几何 sprite 缓存（键 → 贴图）。上限 `kGeometrySpriteLimit`，超限丢弃最早一项。
  /// 为何用 FIFO 而不是 LRU：界面里的图标集合在**帧间**高度稳定（命中路径永不移到表尾），
  /// 而在**页面切换**时成批更换——FIFO 天然匹配这个分布，且不需维护链表。
  struct GeometryCacheEntry {
    std::uint64_t key{0};
    GeometrySprite sprite{};
  };
  std::vector<GeometryCacheEntry> geometry_sprites_{};
  mutable GeometryStats geometry_stats_{};
  bool geometry_cache_enabled_{true};
  /// 阴影的“按同 α 游程分组”（`draw_shadow`）。见 `set_shadow_run_grouping_enabled`。
  bool shadow_run_grouping_enabled_{true};
  /// `blend_coverage_row` 用查表代替逐像素 `scale_premul`。见 `set_alpha_table_enabled`。
  bool alpha_table_enabled_{true};
  /// 扫描线覆盖率的**逐像素累加缓冲**（`blend_coverage_runs` 用）。
  ///
  /// 复用同一个 buffer 而非每行分配：一段路径会对本函数调用几百次（每行一次），
  /// 每次 `resize` 一个几十元素的 vector 不是可忽略的开销。
  std::vector<float> coverage_accum_{};

  /// 倾斜/横向线性渐变的**预乘源色行缓冲**（复用，不每行分配）。
  ///
  /// 横向渐变里“同一行的源色”只随 x 变，可以只算一遍交给 `simd::over_row`
  /// 整段混合；逐像素 `blend_one` 对同样面积要慢 40 倍以上
  /// （实测 1080p 115×1080 一条带：4.22ms → 0.19ms）。
  std::vector<std::uint32_t> gradient_row_{};

  /// 行缓冲的**缓存键**（横向渐变整条带只需算一次）。
  ///
  /// `gdy≈0` 时颜色的 y 分量在整条带内恒定，只有 x 跨度/位置会变；
  /// 三者任一不同就重算。用 int 键（无分配、可整体比较），不引入哈希表。
  int gradient_cache_key_{-1};
  float gradient_cache_to_{0.0f};
  float gradient_cache_len_{0.0f};

  /// 「预乘色 × 覆盖度字节」的查表（256 项），按 `(premul, opacity)` 缓存。
  ///
  /// 为何要缓存而不是行内建：`blend_coverage_row` 是**逐行调用**的。
  /// 把表建在行内时，小字形每行只画几个像素却要初始化 256×4 通道——
  /// 实测整帧 `text` 0.68 → 0.67 ms（等于白改）。按颜色缓存后，
  /// 同一段文字的几十个字形只建一次。
  using AlphaTable = std::array<std::uint32_t, 256>;
  [[nodiscard]] auto alpha_table(std::uint32_t premul) const -> const AlphaTable&;
  /// 缓存槽：单槽足够（同一段文字内颜色不变；变了就重建，仍然划）。
  /// `mutable` 是因为它在 `const` 的绘制路径上被惰性填充。
  mutable AlphaTable alpha_table_{};
  mutable std::uint32_t alpha_table_premul_{0xFFFFFFFFU};

  /// `clear()` 刚把整张画布变成 `cleared_color_`、且尚未被任何绘制碰过。
  ///
  /// 用于**消除“清屏 + 首层不透明底”的重复写**：`UIView::paint` 每帧 `clear(bg)`
  /// 后第一件事往往是根容器用同一个颜色 `fill_rect` 铺满整屏——那一遍 `clear`
  /// 纯属白写（1440p 上它单独就 **1.09 ms**，两项相加约 1.7 ms）。
  ///
  /// 仅在 `cleared_fresh_` 为真时才能跳；**任何一次绘制 / 推描裁剪 / resize 都会清掉它**
  /// （跳过的前提就是“那些像素还没被别的东西碰过”）。
  math::Color cleared_color_{};
  bool cleared_fresh_{false};
  /// 是否启用「清屏 + 首层同色整屏底」合并（默认开；关掉可供 A/B 验证）。
  bool skip_redundant_clear_fill_{true};
  PaintProfiler* profiler_{nullptr};  ///< 空 = 不剖析
};

/// 盒式模糊 ×3 近似高斯（阴影/毛玻璃用）；就地修改遮罩。
void blur_mask(Mask& mask, float radius);

}  // namespace st::raster

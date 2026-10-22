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
/// 2. **像素访问是接口的一部分**（`pixels`/`pixel_view`/`to_rgba8`/`pixel_at`）：控制通道的截图、
///    回归测试的逐像素断言都依赖它。GPU 实现走回读，慢但正确——它只在"要看像素"时才走。
///
/// `pixels` 与 `pixel_view` 的分工：前者是一维线形视图（既有代码/协议用），后者是
/// **二维带边界检查的视图**（`std::mdspan`）——多了一眼就能看出"行列对不对"的能力。
/// 两者指向**同一块内存**（不拷贝）：`pixel_view` 只是把 `pixels()` 重新解释成 `height × width`，
/// 所以不存在"两个视图不同步"的问题。

#include <array>
#include <cmath>
#include <cstdint>
#include <mdspan>
#include <span>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"
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
  Surface(Surface&&) = default;  // lint-allow: L10 移动构造需隐式（return 依赖它，加 explicit 有害）
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

  /// **原地改尺寸**：保留设备/上下文/缓存，只重开与尺寸相关的缓冲。
  ///
  /// 为什么它是接口的一部分，而不是"再 `create_canvas` 一个"：
  /// 拖动窗口边框时每个中间尺寸都要换缓冲，而"换缓冲"与"重建整套设备资源"是两件事——
  /// 后者要建 D3D11 纹理/渲染目标、换 swapchain、换 DIB，还要把渲染器选型重跑一遍
  /// （实测 gallery 1600×1000：一帧 31.9ms 里绘图只占 3.1ms，**剩下 ~29ms 是重建**）。
  /// 把前者抽成接口，拖动才能逐帧跟上；这也是“条图不开 GL/不重建设备”的前提。
  ///
  /// 契约：成功后 `physical_width()/physical_height()` 即为新值，内容**不保证保留**
  /// （调用方必须整帧重绘）；`device_scale()` 不变。
  /// 不支持的实现如实返回 `Unsupported`——调用方据此回落到"新建一个面"。
  [[nodiscard]] virtual auto resize(int physical_width, int physical_height) -> Status {
    (void)physical_width;
    (void)physical_height;
    return unexpected(ErrorCode::Unsupported, "该绘制面不支持原地改尺寸");
  }
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
  /// 描边（宽度为总宽；线帽/连接由 `style` 决定，缺省 butt/miter = SVG 默认）。
  virtual void stroke_path(const Path& path, const Paint& paint, float width,
                           const StrokeStyle& style = StrokeStyle{},
                           DrawOptions options = {}) = 0;
  virtual void fill_circle(math::Point center, float radius, const Paint& paint,
                           DrawOptions options = {}) = 0;
  /// 投影（半径 `blur` 的近似高斯模糊，偏移 `offset`）。
  virtual void draw_shadow(math::Rect rect, float radius, float blur, math::Color color,
                           math::Point offset = {}, DrawOptions options = {}) = 0;

  /// **两层投影（关键光 + 环境光）**——主题的 `Shadow` 就是这个形状，卡片/弹层都要它。
  ///
  /// 为什么合并成一个原语而不是"调两次 `draw_shadow`"：这两层的**遮罩都是几何参数的
  /// 纯函数**（尺寸/圆角/模糊/偏移），合起来可以缓存成**一张**预乘贴图并只做**一次**
  /// 逐像素合成。实测（`docs/PAINT_DIAGNOSIS.md` §3.1，96 张卡片场景）：两层各自合成
  /// 14.0 ms/帧 → 合并 **6.9 ms/帧**（端到端 7.15 ms 里有 5.56 ms 是纯内存流量的下界），因为
  /// ① 两层的非零区域高度重叠（并集只有各自之和的 57%）；
  /// ② 合并后在**中性缓冲**里做一次"源合成"，贴图存的是**阴影自身的预乘 RGBA**，
  ///    往目标上贴只是一次 `src-over`——比"每层都要读遮罩 + 查表 + 合成"便宜一半以上。
  ///
  /// `offset2` 等参数对应 `Shadow::blur2/blur2...`；只给第一层时行为与单层 `draw_shadow` 一致。
  /// 默认实现退化为两次 `draw_shadow`（**顺序与 `Element::paint_box` 相同**：先环境层、
  /// 后关键层——反过来的话紧层会被大而淡的层盖住），因此各后端不实现也能正确工作。
  ///
  /// 顺序之所以要写进接口语义：调用方（UI 层）不该知道"哪层先画"，那是后端的实现细节；
  /// 而"环境层在下、关键层在上"是**视觉定义**，必须两端一致。
  virtual void draw_shadow_layered(math::Rect rect, float radius, math::Color key_color, float key_blur,
                                   math::Point key_offset, math::Color ambient_color,
                                   float ambient_blur, math::Point ambient_offset,
                                   DrawOptions options = {}) {
    if (ambient_color.a != 0U && ambient_blur > 0.0f) {
      draw_shadow(rect, radius, ambient_blur, ambient_color, ambient_offset, options);
    }
    if (key_color.a != 0U && key_blur > 0.0f) {
      draw_shadow(rect, radius, key_blur, key_color, key_offset, options);
    }
  }

  /// 位图合成（双线性缩放；`source` 可为软件画布或 GPU 目标）。
  virtual void draw_canvas(const Surface& source, math::Rect destination,
                           DrawOptions options = {}) = 0;
  virtual void draw_canvas_at(const Surface& source, int x, int y, DrawOptions options = {}) = 0;

  /// **不透明纹理句柄**：把“本 Surface 的像素”以不透明句柄交给同后端的上层合成。
  ///
  /// 存在的理由（实测数据说话）：三维 GPU 腿的光栅只要 **0.06 ms**，而把它交回一张
  /// 软件画布要 **100 ms**（1080p）——那不是计算成本，是“显卡→内存”的一次往返。
  /// 只要上层也是 GPU 画布，这次往返完全可以不发生（纹理直接当 shader 资源采样）。
  ///
  /// 为何不把它设计成 `ID3D11ShaderResourceView*`：`Surface` 是**平台中立**的公共面，
  /// 所有后端与测试都依赖它。把 D3D 类型写进接口等于让整树依赖一个平台。
  /// 所以句柄是 `const void*` + 一个**后端标签**，由“同一后端的两方”自己解释；
  /// 标签不匹配或返回空 = 没有零拷贝路径，调用方走普通 `draw_canvas`。
  ///
  /// 默认返回空（软件画布、未来别的后端都是这个答案）——**不实现就没有这条快路**，
  /// 而不是逼每个后端都去实现一套它用不上的机制。
  struct NativeTexture {
    const void* handle{nullptr};   ///< 后端自有对象（D3D11 上是 `ID3D11ShaderResourceView*`）
    const char* backend{nullptr};  ///< 后端标签（如 `"d3d11"`）；不匹配即不可用
    int width{0};
    int height{0};

    [[nodiscard]] auto valid() const noexcept -> bool { return handle != nullptr; }
  };
  [[nodiscard]] virtual auto native_texture() const noexcept -> NativeTexture { return {}; }

  /// 用**同后端的原生纹理**做合成（零拷贝快路）。
  ///
  /// 默认返回 `false` = “我不支持/这条纹理不是我的后端”，调用方据此回退到
  /// `draw_canvas`（要先把对方的内容取成像素，那就贵了）。
  /// 返回 `true` 时**必须真的画上了**（而不是“接受但忽略”）。
  [[nodiscard]] virtual auto composite_native_texture(const NativeTexture& texture,
                                                     math::Rect destination,
                                                     DrawOptions options = {}) -> bool {
    (void)texture;
    (void)destination;
    (void)options;
    return false;
  };

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
  /// ## 为什么它是**非纯虚**（结构评审 A4：这是本文件自己第 15 行明令禁止的那一类）
  ///
  /// 本头第 15 行写着「接口只放『两边都能做』的原语，像『按覆盖率运行段混合一行』这种
  /// **软件内部机制**留在 `Canvas` 上」——而 `blend_coverage_bitmap` 恰恰是那条规则的反例：
  ///
  /// - 软件侧它只是「逐行走 `blend_coverage_row`」的一层循环，**不需要** `cache_key`；
  /// - GPU 侧却要为它维护一套 `MaskCacheEntry` 纹理缓存（按稳定身份换键），
  ///   也就是被迫实现一份由 CPU 数据结构定义的机制。
  ///
  /// 而它的消费方只有 3 处（`text` 的字形位图、`shell/backend` 的自检图案、
  /// 以及 `Canvas` 自己的覆盖实现），**没有一处是"两者都要做不同事"**。
  ///
  /// 所以这里给出**默认实现**：拆成 `blend_coverage_row` / `blend_coverage_row_lcd`
  /// 两个逐行原语（**两侧都天然做得到**：软件直接走行混合，GPU 把行作为
  /// 一行宽的纹理片上传），默认实现按行驱动它们。后端**可以**覆写本方法走更快的
  /// 整块路径（GPU 现在是这么做的），但**不再被要求**实现一套 CPU 内部机制。
  /// `cache_key` 保留在参数里：它是给"选择覆盖实现的后端"用的**提示**，
  /// 默认实现忽略它（见 `Canvas::blend_coverage_bitmap` 里的同类说明）。
  virtual void blend_coverage_bitmap(int x, int y, std::span<const float> coverage, int width,
                                     int height, const Paint& paint, float opacity,
                                     BlendMode blend, std::uint64_t cache_key = 0,
                                     CoverageFormat format = CoverageFormat::Grayscale) {
    (void)cache_key;
    if (width <= 0 || height <= 0 || opacity <= 0.0f) return;
    const std::size_t channels = format == CoverageFormat::Lcd ? 3U : 1U;
    const std::size_t stride = static_cast<std::size_t>(width) * channels;
    if (coverage.size() < static_cast<std::size_t>(height) * stride) return;
    for (int row = 0; row < height; ++row) {
      const auto line = coverage.subspan(static_cast<std::size_t>(row) * stride, stride);
      if (format == CoverageFormat::Lcd) {
        blend_coverage_row_lcd(y + row, x, line, paint, opacity, blend);
      } else {
        blend_coverage_row(y + row, x, line, paint, opacity, blend);
      }
    }
  }

  /// 混合**一行**灰度覆盖率（`coverage` 为 `width` 个值）。两侧都必须实现。
  virtual void blend_coverage_row(int y, int x_begin, std::span<const float> coverage,
                                  const Paint& paint, float opacity, BlendMode blend) = 0;
  /// 混合**一行**亚像素（LCD）覆盖率（`coverage` 为 `width * 3` 个值，逐通道 α）。
  virtual void blend_coverage_row_lcd(int y, int x_begin, std::span<const float> coverage,
                                      const Paint& paint, float opacity, BlendMode blend) = 0;

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
  /// 二维像素视图（`std::mdspan`，行主序、**物理**尺寸）：与 `pixels()` 同一块内存，不拷贝。
  ///
  /// 返回 `matrix_view<const uint32_t>`：`view[y, x]` 直接取像素，行列写反了">尺寸对不上"
  /// （而非静默读到邻行）——这是换成 `mdspan` 的全部理由。
  /// 尺寸由 `pixels().size()` 与物理宽高共同决定：两者不一致（未初始化的后端）时返回空视图，
  /// 而不是把错误尺寸传给 `mdspan`（后者是硬 UB，不是可捕获的错误）。
  [[nodiscard]] auto pixel_view() const noexcept
      -> std::mdspan<const std::uint32_t, std::dextents<std::size_t, 2>>;
  [[nodiscard]] auto pixel_view() noexcept
      -> std::mdspan<std::uint32_t, std::dextents<std::size_t, 2>>;
  /// 导出为直通 RGBA8 字节流（PNG 编码/截图用；物理分辨率）。
  [[nodiscard]] virtual auto to_rgba8() const -> std::vector<std::uint8_t> = 0;
  /// 非全透明像素的最小包围盒（**物理**像素；测试与截图裁剪用）。
  [[nodiscard]] virtual auto content_bounds() const noexcept -> math::IntRect = 0;
  /// 同上，但换算到逻辑像素。
  [[nodiscard]] virtual auto content_bounds_logical() const noexcept -> math::IntRect = 0;

 protected:
  Surface() = default;

  /// 由（像素跨度、物理宽高）构造二维视图：尺寸不匹配时返回空视图。
  ///
  /// 抽成静态模板是为了让 const / 非 const 两个重载共用同一道**尺寸一致性检查**——
  /// 这道检查是必要的：`mdspan` 对越界尺寸不做验证，传错就是直接踩内存。
  template <class Element>
  [[nodiscard]] static auto make_pixel_view(std::span<Element> flat, int physical_width_value,
                                            int physical_height_value) noexcept
      -> std::mdspan<Element, std::dextents<std::size_t, 2>> {
    using View = std::mdspan<Element, std::dextents<std::size_t, 2>>;
    if (physical_width_value <= 0 || physical_height_value <= 0) return View{};
    const auto columns = static_cast<std::size_t>(physical_width_value);
    const auto rows = static_cast<std::size_t>(physical_height_value);
    if (flat.size() != columns * rows) return View{};
    return View(flat.data(), rows, columns);
  }
};  // class Surface

// —— 内联实现（依赖 `physical_width()/physical_height()` 与 `pixels()` 两个虚函数）——

inline auto Surface::pixel_view() const noexcept
    -> std::mdspan<const std::uint32_t, std::dextents<std::size_t, 2>> {
  return make_pixel_view(pixels(), physical_width(), physical_height());
}

inline auto Surface::pixel_view() noexcept
    -> std::mdspan<std::uint32_t, std::dextents<std::size_t, 2>> {
  // 非 const 重载要调**非 const** `pixels()`（它可能触发 GPU 回读）：
  // 若这里调了 const 版，GPU 后端的可写视图会拿到一份“只读副本”并将其写丢。
  return make_pixel_view(pixels(), physical_width(), physical_height());
}

}  // namespace st::raster

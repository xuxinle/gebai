#pragma once

/// 文本整形与渲染：字体回退链（拉丁 → CJK → 符号）、度量、绘制、折行、省略。
///
/// **DPI 策略（关键）**：
/// - 排版（`measure`/`shape`/`wrap`）一律在**逻辑单位**下进行，与 DPI 无关 —— 布局稳定；
/// - 绘制时字形按 **物理分辨率** 栅格化（`画布 device_scale × supersample`），因此 2x/HiDPI 屏上
///   字形边缘同样锐利（覆盖率在物理像素空间计算，非放大插值）；
/// - 字形位图按 `(face, glyph, 物理尺寸, 超采样)` 缓存，命中为常数时间。

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/font.hpp"
#include "st/text/grid_fit.hpp"

namespace st::text {

/// 一段同字体的连续字形（整形结果单元）。
struct TextRun {
  const FontFace* face{nullptr};  ///< 非拥有（指向 FontStack 内的 face）
  GlyphId glyph{0};
  std::uint32_t codepoint{0};
  float x{0.0f};        ///< 逻辑单位：相对行首的笔位
  float advance{0.0f};  ///< 逻辑单位
};

/// 整形后的文本（逻辑单位）。
struct ShapedText {
  std::vector<TextRun> runs{};
  float width{0.0f};
  float ascent{0.0f};
  float descent{0.0f};
  float line_height{0.0f};
};

/// 字体回退链：按顺序查找首个覆盖该码点的 face。
/// 字形角色：同一段文本可以要求"正文字体"或"等宽字体"。
///
/// 存在的理由：代码（Markdown 代码块 / 行内码）必须对齐——等宽字体里 `i` 与 `M` 同宽，
/// 用比例字体渲染代码会让缩进与列对齐全部失真。
/// 回退策略：等宽库里找不到该码点时**回退到正文字体**（汉字在多数等宽字体里没有，
/// 而中文代码注释必须能显示——宁可混排，不可缺字）。
enum class FontRole : std::uint8_t { Proportional, Monospace };

/// 字体回退链：按顺序查找首个覆盖该码点的 face。
class FontStack {
 public:
  /// `mono_faces` 可以为空：此时等宽角色完全回退正文字体（代码块仍可读）。
  explicit FontStack(std::vector<FontFace> faces, std::vector<FontFace> mono_faces = {});

  /// 系统默认回退链（拉丁 + CJK；探测系统字体目录）。
  /// @return 失败：`NotFound` 未找到任何可用字体。
  [[nodiscard]] static auto system_default() -> Result<FontStack>;
  /// 显式指定字体文件（按给出顺序即回退优先级）。
  [[nodiscard]] static auto from_files(const std::vector<std::string>& paths) -> Result<FontStack>;

  /// 覆盖该码点的 face（无覆盖返回 nullptr）。
  [[nodiscard]] auto find_face(char32_t codepoint) const -> const FontFace*;
  /// 按角色选 face：`Monospace` 优先等宽库，找不到则**回退正文字体**。
  [[nodiscard]] auto find_face(char32_t codepoint, FontRole role) const -> const FontFace*;
  /// 是否真的探测到等宽字体（没有时等宽角色 = 正文字体，调用方可如实告知）。
  [[nodiscard]] auto has_monospace() const noexcept -> bool { return !mono_faces_.empty(); }
  [[nodiscard]] auto primary() const -> const FontFace& { return faces_.front(); }
  [[nodiscard]] auto faces() const noexcept -> std::span<const FontFace> { return faces_; }
  [[nodiscard]] auto monospace_faces() const noexcept -> std::span<const FontFace> {
    return mono_faces_;
  }
  [[nodiscard]] auto empty() const noexcept -> bool { return faces_.empty(); }

  /// 供渲染器使用的共享字节大小（缓存键的一部分）。
  [[nodiscard]] auto fingerprint() const noexcept -> std::uint64_t { return fingerprint_; }

 private:
  std::vector<FontFace> faces_{};
  /// 等宽库（代码用）。可以为空——没探到等宽字体时等宽角色完全回退正文字体。
  std::vector<FontFace> mono_faces_{};
  std::uint64_t fingerprint_{0};
};

/// 文本渲染器：整形 + 度量 + 绘制（带字形位图缓存）。
class TextRenderer {
 public:
  explicit TextRenderer(const FontStack& stack, float supersample = 1.0f);
  ~TextRenderer();
  TextRenderer(const TextRenderer&) = delete;
  auto operator=(const TextRenderer&) -> TextRenderer& = delete;

  /// 整形（逻辑单位；`role` 选字体库，代码用 `Monospace`）。
  [[nodiscard]] auto shape(std::string_view utf8, float size,
                           FontRole role = FontRole::Proportional) const -> ShapedText;
  /// 整形（共享指针版：命中缓存零拷贝；调用方可持有到缓存淘汰之后）。
  [[nodiscard]] auto shape_cached(std::string_view utf8, float size,
                                  FontRole role = FontRole::Proportional) const
      -> std::shared_ptr<const ShapedText>;
  /// 度量：宽 × 行高（逻辑单位）。
  [[nodiscard]] auto measure(std::string_view utf8, float size,
                             FontRole role = FontRole::Proportional) const -> math::Size;
  [[nodiscard]] auto measure_width(std::string_view utf8, float size,
                                   FontRole role = FontRole::Proportional) const -> float;
  [[nodiscard]] auto line_height(float size) const -> float;
  /// 基线相对行顶的偏移（逻辑单位）。
  [[nodiscard]] auto ascent(float size) const -> float;

  /// 绘制：`origin` 为**逻辑坐标**下的行左上角；字形按画布 DPI 物理栅格化。
  ///
  /// `embolden` 为合成加粗的**笔画外扩半径（物理像素）**，0 = 不加粗。
  /// 存在的理由：框架没有独立字重的字体面（字体栈是单面的），而界面里的标题/按钮/
  /// 大数字都标了非 Regular 字重——不提供这条路，「字重」就只是一个落不到像素上的属性
  /// （实测：`Element::paint_text` 原先完全不读 `style_.font_weight`，
  /// SemiBold 与 Regular 渲染逐像素相同）。
  /// 做法与 Skia `SkFont::setEmbolden` / `FT_GlyphSlot_Embolden` 同一取向：
  /// 同一份轮廓沿水平正方向按**采样格**平移后重复填充，再一起栅格化。  /// `embolden` 为**合成加粗的笔画外扩半径（物理像素）**，0 = 不加粗。
  ///
  /// 用**步数**而不是像素半径作为渲染器接口：步数是整数，能直接进缓存键，
  /// 也能保证平移落在采样格上（像素半径在两种模式下换算出的格宽不同，
  /// 由 `embolden_steps()` 统一换算）。
  auto draw(raster::Surface& canvas, std::string_view utf8, math::Point origin, float size,
            math::Color color, FontRole role = FontRole::Proportional,
            float embolden = 0.0f) const -> Status;

  /// 折行（按空格与 CJK 断点；返回各行原文区间）。
  [[nodiscard]] auto wrap(std::string_view utf8, float size, float max_width) const
      -> std::vector<std::string_view>;
  /// 截断加省略号。
  [[nodiscard]] auto ellipsize(std::string_view utf8, float size, float max_width) const
      -> std::string;
  /// 单个字符的推进宽度（用于光标定位）。
  [[nodiscard]] auto advance_of(char32_t codepoint, float size,
                                FontRole role = FontRole::Proportional) const -> float;
  /// 光标 x 偏移（逻辑单位）。
  [[nodiscard]] auto cursor_x(std::string_view utf8, float size, std::size_t codepoint_index) const
      -> float;
  /// 命中测试：给定局部 x 求最近的码点索引。
  [[nodiscard]] auto index_at_x(std::string_view utf8, float size, float local_x) const
      -> std::size_t;

  void set_supersample(float factor);
  [[nodiscard]] auto supersample() const noexcept -> float { return supersample_; }

  /// **亚像素（LCD）文字渲染开关**。
  ///
  /// 开启后字形按 **3× 水平超采样**光栅化，再按每像素的 R/G/B 三个子像素分别聚合出
  /// 覆盖率（等价于 FreeType 的 `FT_RENDER_MODE_LCD`）：水平分辨率变成 3 倍，
  /// 笔画边缘能落在 1/3 像素上（带 RGB 彩边）——这是桌面系统文字“看着锐”的来源。
  /// 关闭 = 灰度抗锯齿：像素对像素可与截图/回归断言直接比对。
  ///
  /// **位图网格与开关无关**：两种模式算出的 `width/height/offset_x/offset_y` 完全相同，
  /// 所以开与关只改变边缘合成方式，不会让文字挪位（排版稳定是硬约束）。
  /// 缓存键含模式位：两种位图**共存而不混淆**（切换无需清缓存）。
  void set_subpixel(bool enabled) noexcept { subpixel_ = enabled; }
  [[nodiscard]] auto subpixel() const noexcept -> bool { return subpixel_; }
  /// 亚像素的 **5-tap 低通滤波**（FreeType `FT_LCD_FILTER_DEFAULT` 权重 `{8,77,86,77,8}/256`）。
  ///
  /// 存在的理由：三通道独立采样会让笔画边缘出现强烈的彩色条纹；
  /// 这个滤波器在**子像素轴**上做一次带内平滑，把“红边/蓝边”压到接近 ClearType 的观感。
  /// 关掉它彩边更浓、单像素对比更硬（对照实验用；默认开）。
  void set_subpixel_filter(bool enabled) noexcept { subpixel_filter_ = enabled; }
  [[nodiscard]] auto subpixel_filter() const noexcept -> bool { return subpixel_filter_; }

  /// **字形网格拟合（grid fitting / hinting）模式**。
  ///
  /// 存在的理由（实测，见 `DESIGN.md §4.3.1` 与 `include/st/text/grid_fit.hpp`）：
  /// 13.5px 正文的笔画边缘 100% 落在分数相位上（91.8% 是“每边各一个过渡像素”的缓坡），
  /// 所以小字“看着糊”——这是**与抗锯齿模式无关**的主因（灰度/亚像素都过不了这一关）。
  /// 而实测又证明读字体自带 hinting 指令等于没做（中文是 CFF，根本没有那套指令），
  /// 唯一有效的是**从轮廓几何自推笔画位置再吸附**（对齐 FreeType auto-hinter 的口径）。
  ///
  /// 默认 `Off`：拟合会微调字形（这是它的目的），而**无头截图/回归断言需要一个
  /// 可逐像素复现的基准**——两者不能兼顾，所以默认关、由应用按“有无窗口”开。
  /// 缓存键含该模式位（拟合前后是两份不同的位图，不能混用）。
  void set_grid_fit(GridFitMode mode) noexcept { grid_fit_ = mode; }
  [[nodiscard]] auto grid_fit() const noexcept -> GridFitMode { return grid_fit_; }
  [[nodiscard]] auto stack() const noexcept -> const FontStack& { return *stack_; }
  /// 合成加粗的**档位数**（`embolden_steps` 的上限）。
  ///
  /// 单位是**采样格**，不是物理像素：亚像素 + supersample=2 时一个采样格只有
  /// 1/6 物理像素。上限取 16 才能覆盖最大的 Bold（1/24 em）在 48px 上的外扩量；
  /// 它只是防荒唐值的护栏，实际步数由 `embolden_steps(radius)` 算出。
  static constexpr int kMaxEmboldenSteps = 16;
  /// 把「合成加粗半径（**物理像素**）」换算成当前模式下可生效的**采样格步数**。
  [[nodiscard]] auto embolden_steps(float pixel_radius) const noexcept -> int;
  /// 字形位图缓存条目数（诊断用）。
  [[nodiscard]] auto cache_entries() const noexcept -> std::size_t;

  /// 字形覆盖率位图。
  ///
  /// 与 `glyph_bitmap_of` 一起**公开**：字形位图是"字对不对"的第一现场，
  /// 藏在私有实现里就只能靠截图比对，写不成可重复的检查。
  struct GlyphBitmap {
    int width{0};
    int height{0};
    int offset_x{0};  ///< 相对笔位的物理像素偏移（左上角）
    int offset_y{0};  ///< 相对基线的物理像素偏移（向上为负）
    /// 通道布局（与 `coverage` 的长度一一对应）：
    /// `Grayscale` = `width×height` 项；`Lcd` = `width×height×3` 项（像素内 R→G→B 交错）。
    /// 它同时是**混合公式的开关**：调用方必须把它原样交给 `blend_coverage_bitmap`。
    raster::CoverageFormat format{raster::CoverageFormat::Grayscale};
    std::vector<float> coverage{};  ///< 物理像素覆盖率（已按超采样下采样）
    /// 该字形的**稳定身份**（face + 字形号 + 字号档 + 超采样）。
    ///
    /// 为什么必须带上它：GPU 后端要把覆盖率上传成纹理并缓存，而缓存键若用
    /// "位图地址"就不成立——字体引擎的缓存会被清空（超过上限时 `clear()`），
    /// 位图内存随之释放并被**新字形复用同一地址**，于是 GPU 端"按地址命中"
    /// 会把上一个字形的纹理当成这个字形的。实测症状就是界面文字间歇性地变成别的字。
    /// 稳定身份不受内存生命周期影响，重算同一个字形得到同一个键。
    std::uint64_t cache_key{0};
    /// 位图原点在**采样空间**的坐标（= 输出像素 X 平均的采样列起点）。
    ///
    /// 本字段存在的唯一理由是**可断言性**：该值必须是 `supersample` 的整数倍
    /// （否则输出像素平均的采样窗口横跨两个物理像素，等于把墨迹糊开），
    /// 而不看内部值就无法写出钉住它的回归测试——实测踩到过
    /// 「凭外观写的测试回退修复后依然全绿」。
    int origin_x{0};
    int origin_y{0};
  };

  /// 取某个**码点**的字形位图（公开的诊断入口）。
  ///
  /// 用途：验证"字形没有被裁切"——位图四周必须留白（生成时 padding=1），
  /// 墨迹一旦贴边就说明包围盒算小了（曲线极值被切掉），
  /// 而**字宽不变**（advance 不受影响）→ 现象就是"排版完好、字却变了样"。
  /// 返回 `nullptr` 表示该码点在字体栈里没有对应字形。
  [[nodiscard]] auto glyph_bitmap_of(char32_t codepoint, float pixel_size,
                                     FontRole role = FontRole::Proportional) const
      -> std::shared_ptr<const GlyphBitmap>;

 private:
  /// 取字形覆盖率位图（按 face/字形/物理尺寸/超采样 缓存）。
  /// 返回 `shared_ptr`：即使该条目随后被淘汰，调用方手里的位图依然有效
  /// （曾因缓存「插入后淘汰」并返回裸指针导致 use-after-free，见 text.cpp 注释）。
  [[nodiscard]] auto glyph_bitmap(const FontFace& face, GlyphId glyph, float pixel_size,
                                  int embolden_steps) const
      -> std::shared_ptr<const GlyphBitmap>;
  /// 无缓存版整形（`shape_cached` 未命中时的计算体）。
  [[nodiscard]] auto shape_uncached(std::string_view utf8, float size, FontRole role) const
      -> ShapedText;
  /// 淘汰超出预算的字形条目（**调用方须持有锁**，且在插入之前调用）。
  /// `incoming_bytes` 是即将插入条目的内存量——先腾出它的位置。
  void trim_cache(std::size_t incoming_bytes) const;

  const FontStack* stack_{nullptr};
  float supersample_{1.0f};
  /// 亚像素（LCD）渲染开关；默认**关**——灰度是可逐像素断言的参考口径。
  bool subpixel_{false};
  /// 亚像素 5-tap 低通滤波开关（见 `set_subpixel_filter`）。
  bool subpixel_filter_{true};
  /// 网格拟合模式（见 `set_grid_fit`）；默认关，保证无头/回归的可复现性。
  GridFitMode grid_fit_{GridFitMode::Off};
  struct Cache;
  std::unique_ptr<Cache> cache_{};
};

}  // namespace st::text

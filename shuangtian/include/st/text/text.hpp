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

/// 字形类：**覆盖率分档**的维度。
///
/// 为什么需要它（2026-10-06 实测）：全量逐字形与真窗口浏览器比对后发现——在
/// **应用默认档**（拟合 light + 墨量补偿 + 笔画加墨）下，字母类的墨量比已经在
/// **0.94~0.96**（判定为"已对齐"，用户明确要求**不要动**），而**数字类只有 0.88~0.92**，
/// 且在 10/11/12/13px 上**符号一致**（四个字号都偏轻）。
/// 实测（只压 γ、其余档位不动）：**γ≈0.90 让数字落到 0.963~0.978**，
/// 而同批字母只从 0.939~0.961 抬到 0.999~1.016（过冲 2%）——
/// 全局 γ 做不到"只抬数字、不动字母"，所以需要**按类**这一层。
///
/// 判据用**字符类**而不是字号：实测各字类的偏差**在各自字号范围内恒定**
/// （数字、字母、汉字三条线各自成立），所以"按类"才是正确的分档维度
/// （"按字号分档"那条已被 DESIGN §4.3.7.17 的标定证明不成立）。
///
/// 汉字单独成类（2026-10-06 加）：用**一行一字**的专项页测得它在 10~20px 上
/// **稳定偏轻 5~7%**（总墨量比 0.929~0.949），且复杂字比简单字更轻
/// （复杂 0.90~0.93、简单 0.93~1.02）。它此前被并进 `Default`（连同标点/符号），
/// 而这两者的观感诉求不同，所以拆开各定一档。
enum class GlyphClass : std::uint8_t {
  Default,   ///< 其它一切字形（标点、符号…）
  Digit,     ///< ASCII 数字 `0`~`9`
  Letter,    ///< ASCII 拉丁字母 `A`~`Z` / `a`~`z`
  Han,       ///< CJK 汉字（含扩展 A/B 与兼容区）
};

/// 字形类的数量（用于按类保存覆盖参数的数组下标）。
inline constexpr std::size_t kGlyphClassCount = 4;

/// 类 → 数组下标。
[[nodiscard]] constexpr auto glyph_class_index(GlyphClass glyph_class) noexcept -> std::size_t {
  return static_cast<std::size_t>(glyph_class);
}

/// 码点 → 字形类（ASCII 数字单独一类，其余归 `Default`）。
[[nodiscard]] auto glyph_class_of(char32_t codepoint) noexcept -> GlyphClass;

struct TextRun {
  const FontFace* face{nullptr};  ///< 非拥有（指向 FontStack 内的 face）
  GlyphId glyph{0};
  std::uint32_t codepoint{0};
  /// 该字形所属的**覆盖率类**（ASCII 数字单独一类）。整形时定好、**绘制时照它取位图**
  /// ——这一条漏了，"按类覆盖 γ"就只在诊断接口上生效、实际绘制仍用默认档。
  GlyphClass glyph_class{GlyphClass::Default};
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
  /// `mono_faces` 可以为空；`bold_faces` 为空时粗体回退常规库（此时由调用方决定
  /// 是否再用合成加粗补足——`App` 的做法是：没有真粗体面才用合成加粗）。
  explicit FontStack(std::vector<FontFace> faces, std::vector<FontFace> mono_faces = {},
                     std::vector<FontFace> bold_faces = {});

  /// 系统默认回退链（拉丁 + CJK；探测系统字体目录）。
  /// @return 失败：`NotFound` 未找到任何可用字体。
  [[nodiscard]] static auto system_default() -> Result<FontStack>;
  /// 显式指定字体文件（按给出顺序即回退优先级）。
  [[nodiscard]] static auto from_files(const std::vector<std::string>& paths) -> Result<FontStack>;

  /// 覆盖该码点的 face（无覆盖返回 nullptr）。
  [[nodiscard]] auto find_face(char32_t codepoint) const -> const FontFace*;
  /// 按角色选 face：`Monospace` 优先等宽库，找不到则**回退正文字体**。
  [[nodiscard]] auto find_face(char32_t codepoint, FontRole role) const -> const FontFace*;

  /// **粗体面**版本的查找：`bold=true` 且粗体库里有该码点时用它，否则回退常规库。
  ///
  /// 为什么要有独立的粗体库（2026-10-04 实测）：此前框架**没有独立字重的字体面**，
  /// 粗体靠**合成加粗**（同一轮廓沿水平平移后重复填充）——结果是用户看到的
  /// 「中文粗体有点糊、英文不够均匀锐利」。把字体栈指向系统里**真实的**粗体面
  /// （`msyhbd.ttc` / `segoeuib.ttf`）后实测：中文过渡带 0.246→**0.155**（锐 37%），
  /// **英文字间离散 14.1%→0.0%**，英文过渡带 0.204→0.131。
  /// 真粗体的笔画与相位是**设计出来的**，不需要靠 smear 去撑。
  [[nodiscard]] auto find_face(char32_t codepoint, FontRole role, bool bold) const
      -> const FontFace*;
  /// 是否探测到粗体库（没有时粗体沿用常规面 + 合成加粗，调用方可如实告知）。
  [[nodiscard]] auto has_bold() const noexcept -> bool { return !bold_faces_.empty(); }
  [[nodiscard]] auto bold_faces() const noexcept -> std::span<const FontFace> {
    return bold_faces_;
  }
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
  /// 粗体库。可以为空——没探到粗体面时粗体回退常规库（并可由调用方合成加粗）。
  std::vector<FontFace> bold_faces_{};
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
                           FontRole role = FontRole::Proportional, bool bold = false) const
      -> ShapedText;
  /// 整形（共享指针版：命中缓存零拷贝；调用方可持有到缓存淘汰之后）。
  [[nodiscard]] auto shape_cached(std::string_view utf8, float size,
                                  FontRole role = FontRole::Proportional, bool bold = false) const
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
  /// `bold=true` 时**优先用真粗体字体面**（见 `FontStack::find_face(…, bold)`）——
  /// 实测比合成加粗锐 36~37%，并把英文字间离散从 14.1% 降到 **0%**；
  /// 探不到真粗体面时自动回退常规面（此时 `embolden` 仍可按需叠加）。
  auto draw(raster::Surface& canvas, std::string_view utf8, math::Point origin, float size,
            math::Color color, FontRole role = FontRole::Proportional,
            float embolden = 0.0f, bool bold = false) const -> Status;

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

  /// **字形覆盖率的 gamma 预校正指数**（在**位图生成时**施加，不是贴图时）。
  ///
  /// 映射：`α' = 1 − (1−α)^(1/γ)`（黑字白底的码值即 `(1−α)^(1/γ)`）。
  /// **γ > 1 提亮/减墨，γ < 1 压黑/加墨**（γ = 1 是恒等）。
  ///
  /// 存在的问题：霜天在 sRGB **编码空间**直接做 alpha 混合（`out = bg + (fg−bg)·α`），
  /// 而屏幕是 sRGB 非线性——同一覆盖率在两种合成空间下码值不同。
  /// 物理上正确的是**线性空间合成**（γ = 2.2），但“物理正确”不等于“好看”：
  /// 该不该走这一步、走多少，取决于**与界面自认的参照（浏览器/系统渲染器）的观感差距**。
  ///
  /// ⚠ **本参数的默认值被推翻过一次，方向教训比数字重要**（2026-10-04）：
  ///
  /// 第一轮：拿无头 Edge 截图当参照，用**覆盖率**口径（把像素投影到背景→前景的连线上反解 α），
  /// 得出「霜天墨量 +13.8%、实心像素 +16.4% = 发胖」，于是默认 γ = 2.2（完整线性）。
  ///
  /// 第二轮（用户反馈「优化后代码编辑器反而不如以前」）：换**朴素像素口径**
  /// （直接比亮度，不做任何反解）重测，结论**反转**：
  ///
  /// | 量（文字区） | 浏览器 | 霜天 γ=1 | 霜天 γ=2.2 |
  /// |---|---|---|---|
  /// | 平均亮度（越低越实） | **0.376** | 0.412 | 0.450 |
  /// | 亮度 <50% 的墨像素占比 | **65.2%** | 58.0% | 57.0% |
  /// | 代码编辑器：<50% 占比 | — | **10.8%** | 8.8%（**少 19%**） |
  /// | 代码编辑器：50~90% 过渡带 | — | **13.1%** | 13.8%（**更宽**） |
  ///
  /// 即：浏览器比霜天**更黑更实**，而 γ 提亮把霜天推得更远——
  /// “变灰、变虚、实心像素减少、过渡带变宽”正是用户看到的现象。
  ///
  /// **第一轮错在哪**（两条都记在下面，因为同类陷阱还会再来）：
  /// 1. **用带口径假设的量当基准**：覆盖率那个投影隐含“屏幕是线性的”这个假设；
  ///    朴素亮度口径没有假设，给出相反且更接近眼睛的答案。两个口径冲突时，
  ///    先信**假设少**的那个；
  /// 2. **参照选错了**：无头 `msedge --headless --screenshot` 不带桌面浏览器的
  ///    GPU 合成与字体渲染链路，它渲染出的字比用户实际看到的浏览器字**更浅**
  ///    （即使同一台机器、同一 DPI）——拿它当“浏览器基准”本身就偏。
  ///
  /// 所以默认值一度回退到 **1.0（不做校正）**，随后又根据**可信参照**（真机非无头浏览器）
  /// 定为 **0.6**——加墨方向的部分校正。当前证据：
  ///
  /// | 行带 | 参照 平均亮度 / <50% | 霜天 γ=1 | 霜天 γ=0.6 |
  /// |---|---|---|---|
  /// | 14px CJK | 0.306 / 75.6% | 0.376 / 67.0% | 0.344 / **72.3%** |
  /// | 13.5px 拉丁 | 0.338 / 67.5% | 0.443 / 58.3% | 0.402 / **61.1%** |
  /// | 12px 路径 | 0.281 / 78.2% | 0.518 / 50.8% | 0.464 / **56.1%** |
  /// | 13.5px 等宽 | 0.263 / 79.4% | 0.388 / 65.6% | 0.348 / **71.3%** |
  ///
  /// 12px 那行即使加墨仍差 21 个百分点——那部分已属**几何**（笔画未落网格 +
  /// 落点相位抖动 0.406px，见 `tools/glyph_phase_probe.cpp` 与 BACKLOG 的“子像素定位”条），
  /// 不是加墨能补的；不要为了追它就继续压 γ（会在低覆盖率区把边缘推成实心而走样）。
  ///
  /// **为什么放在位图生成时**：覆盖率位图既进 CPU 的逐行混合、也进 GPU 的遮罩纹理，
  /// 是两条渲染路径唯一的共同输入；在这一处校正，软件与 GPU 天然同源，
  /// 也不需要给 `Surface` 接口再加一个只在文字上用得到的参数。
  /// 缓存键含该值：不同校正指数是两份不同的位图，不能混用。
  void set_coverage_gamma(float gamma) noexcept;
  [[nodiscard]] auto coverage_gamma() const noexcept -> float { return coverage_gamma_; }

  /// 该**物理字号**实际生效的 gamma：小字号分档命中则用分档值，否则用 `coverage_gamma_`。
  /// 缓存键与位图映射都必须用它——只用 `coverage_gamma_` 无法区分两个字号档。
  [[nodiscard]] auto effective_gamma(float pixel_size) const noexcept -> float {
    return fitted_gamma_size_ > 0.0f && pixel_size <= fitted_gamma_size_ ? fitted_gamma_
                                                                        : coverage_gamma_;
  }

  void set_class_gamma(GlyphClass glyph_class, float gamma) noexcept {
    class_gamma_[glyph_class_index(glyph_class)] = gamma;
  }
  [[nodiscard]] auto class_gamma(GlyphClass glyph_class) const noexcept -> float {
    return class_gamma_[glyph_class_index(glyph_class)];
  }


  /// 覆盖率预校正模式（默认 `Gamma`）。
  ///
  /// `Gamma`：整体映射 `α' = 1 − (1−α)^(1/γ)`（见 `set_coverage_gamma`）。
  /// `Skia`：**逐颜色方向性曲线**（复刻 `skia@8643b1d` 的
  /// `SkTMaskGamma_build_correcting_lut`；逐位对照见 `tools/skia_lut_compare.py`）。
  /// 两者**互斥**——它们是两条完整的映射，不是叠加关系。
  ///
  /// | 输入 α | Skia 黑墨/白底 | Skia 白墨/黑底 | 霜天 γ=0.6 |
  /// |---|---|---|---|
  /// | 64 | 31 (contrast=0) → 58 (contrast=1) | **137** | 97 |
  /// | 128 | 68 → 119 | **188** | 175 |
  /// | 192 | 119 → 185 | **225** | 230 |
  ///
  /// 三条实测结论：
  /// 1. Skia 的**黑墨/白底 contrast=0 就是我们的 γ=2.2**（31/68/119 vs 31/69/120）——
  ///    上一轮被驳回的方向确实是它链条里的**一截**；它紧接着用 `apply_contrast`
  ///    （`a + (1−a)·c·a`）把墨补回来。
  /// 2. **`adjustedContrast = contrast · linDst` 只服务深字浅底**：`dst = 1 − src`
  ///    是“对背景的猜测”，黑字时 `linDst = 1`（全量生效）、白字时 `linDst = 0`
  ///    （**完全失效**，实测三个 contrast 输出逐位相同）。
  /// 3. **浅字深底走的是完全另一条曲线**（64→137、128→188）。用户的编辑器是
  ///    **深色主题（浅字深底）**——这正是两者分岔之处，也是全局 γ 表达不了的方向性。
  ///
  /// `mode = Skia` 时 `coverage_gamma` 不生效。缓存键含模式与对比度分档。
  enum class CoverageCorrect : std::uint8_t { Gamma, Skia };
  void set_coverage_correct(CoverageCorrect mode) noexcept;
  [[nodiscard]] auto coverage_correct() const noexcept -> CoverageCorrect {
    return coverage_correct_;
  }
  /// Skia 模式的对比度 `[0, 1]`（见上表）。**只影响深字浅底**。
  void set_coverage_contrast(float contrast) noexcept;
  [[nodiscard]] auto coverage_contrast() const noexcept -> float { return coverage_contrast_; }
  /// 文字色 / 背景色（`0xRRGGBB`）。Skia 模式按**文字色**索引 LUT、
  /// 按 `dst = 1 − src`（感知反色）猜背景——与 `SkTMaskGamma::preBlend` 同口径。
  void set_text_colors(std::uint32_t text, std::uint32_t background) noexcept {
    text_color_ = text;
    background_color_ = background;
  }

  /// 出厂默认的覆盖率预校正指数。
  ///
  /// **1.10 是拿真窗口浏览器的整幅总墨量标定的**（`tools/gen_realwin_page.py` +
  /// `tools/realwin_ink.py`：真窗口、`device-scale-factor 1.5`、一框一行、行投影分带）。
  /// 各字号在 γ=1.10 时墨量比落在 **0.99~1.03**；小字号端与正文端的理想 γ 中位数
  /// 只差 **−0.034**，即**理想 γ 与字号无关**——所以只有这一个全局值，没有字号分档。
  ///
  /// ⚠ **改动它之前先读 `set_coverage_gamma` 的事故记录**：本参数曾被设成 2.2（提亮、
  /// “物理上正确的线性合成”）并被用户实测驳回。
  ///
  /// ⚠ **基准必须用真窗口，不能用 headless**：本参数曾在 headless Chromium 上被标定为
  /// 0.6，并据此推出「小字号要更亮、需要字号分档」——拿到真窗口复核时**全部不成立**
  /// （headless 不做 ClearType 调校、默认灰度抗锯齿，笔画覆盖率分布与真窗口不同；
  /// 参照物一变，推出的「理想 γ」跟着变）。见 `DESIGN.md §4.3.7.17`。
  static constexpr float kDefaultCoverageGamma = 1.10f;

  /// **深底（暗色主题）下的默认值**——与浅底**不是同一个数**。
  ///
  /// 预校正的作用是 `α' = 1 − (1−α)^(1/γ)`：γ<1 把中间调推向**满墨**、γ>1 推向**空**。
  /// 这个方向是按**黑字白底**定的。白字黑底时，"看起来够不够实"由反方向的对比决定，
  /// 所以浅底标出来的 γ 不能直接沿用。
  ///
  /// 真窗口实测（`tools/calibrate_text_gamma.py measure --theme dark`；底色 `#0A0F1A`、字色 `#E8EEF9`）：
  ///
  /// | 逻辑px | 10 | 11 | 12 | 14 | 16 |
  /// |---|---|---|---|---|---|
  /// | **深底理想 γ** | 0.58 | 0.57 | 0.65 | 0.77 | <0.55 |
  /// | 浅底理想 γ | 1.05 | 1.08 | 1.4 | 1.6 | 1.20 |
  ///
  /// 两个主题相差近 2 倍——**单一默认值必然错一个主题**（这正是旧默认 0.6 在亮色下
  /// 被用户判为"偏重"、而在深色下却接近正确的由来）。
  static constexpr float kDefaultCoverageGammaOnDark = 0.60f;

  /// **按物理字号覆盖 gamma**（可选）：非零时，`pixel_size` 用 fitted 值。
  ///
  /// **默认不启用，且当前没有已知适用的档位**：真窗口实测显示理想 γ 与字号无关
  /// （见 `kDefaultCoverageGamma` 的标定说明），全局单一值就够。
  ///
  /// 保留本接口是为了**可调**：将来若在别的 DPI / 字体 / 后端组合上量出字号依赖，
  /// 不必改渲染器内部就能分档。归零用 `set_fitted_gamma(0, 0)`（= 落回 `coverage_gamma_`）。
  void set_fitted_gamma(float pixel_size, float gamma) noexcept;
  [[nodiscard]] auto fitted_gamma() const noexcept -> float { return fitted_gamma_size_; }
  [[nodiscard]] auto fitted_gamma_value() const noexcept -> float { return fitted_gamma_; }

  /// 把任意输入夹取到合法区间（`[0.3, 4]`；NaN 取默认值）。
  ///
  /// **区间双向都要用**：γ<1 是把字**压黑/加墨**（`α' = 1−(1−α)^(1/γ)`，γ<1 时 α' > α），
  /// γ>1 是提亮/减墨。哪个方向对由**真机参照 + 朴素像素口径**定（见 `kDefaultCoverageGamma`）。
  /// 下界 0.3 是护栏：再小会在低覆盖率区直接把边缘推成实心（形状走样）。
  [[nodiscard]] static auto sanitize_coverage_gamma(float gamma) noexcept -> float {
    if (!(gamma == gamma)) return kDefaultCoverageGamma;  // NaN：没有方向，取默认
    if (gamma < 0.3f) return 0.3f;
    return gamma > 4.0f ? 4.0f : gamma;
  }

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

  /// 拟合时认定的**直线边最大横向斜率**（物理像素，整条边的横跨量）。
  ///
  /// 这是“线条粗细不均匀”的关键旋钮（2026-10-04）：阈值内紧时，CJK 里大量微斜直线
  /// 不被当成笔画，于是**没被吸附**、留着分数相位——同字里就出现“有的笔画实、有的灰”。
  /// 默认用 `GridFitOptions::max_edge_slant` 的值；量尺：`tools/stroke_uniformity_probe.cpp`
  /// （看 `crisp` 与**墨量直方图的非整数堆积**）与 `tools/fit_reject_probe.cpp`。
  void set_fit_slant(float pixels) noexcept { fit_slant_ = pixels; }
  [[nodiscard]] auto fit_slant() const noexcept -> float { return fit_slant_; }
  /// 拟合的**最低笔画抽取覆盖率**（见 `GridFitOptions::min_stem_coverage`）。
  ///
  /// 低于它就把该字形整体放弃（不产出“吸一半”的混合字形）——量尺：
  /// `tools/grid_fit_coverage_scan.cpp`（同时看应用率与双峰组占比）。
  /// 负值 = 用 `GridFitOptions` 的默认值。
  void set_min_stem_coverage(float coverage) noexcept { min_stem_coverage_ = coverage; }
  /// **拟合墨量补偿**开关（默认关；`app.cpp` 按需打开）。
  ///
  /// 开启后：把「拟合造成的逐字墨量变化」归一化回不拟合基准——几何（边缘相位）不动、
  /// 只改墨色深浅；**只提亮不压暗**（拟合主要让字变轻，反向压暗会弄坏本来就对的字）。
  /// 依据与应用层数据见 `app.cpp` 的 `resolve_text_fit` 与 DESIGN「字间不一致」一节。
  void set_ink_compensation(bool enabled) noexcept;
  /// **笔画加墨（stem darkening）开关**（默认关；`app.cpp` 按需开）。
  ///
  /// 开启后，细笔画的两侧各自向**外**挪半个「该笔画的加墨量」——
  /// 曲线与 FreeType CFF 引擎的 `darkening-parameters` 一致（见 `GridFitOptions`）。
  /// 它是**几何外扩**而非覆盖率映射：因此与 gamma 正交，两者可以叠加，
  /// 而且**对小字有效、多大字自动归零**（加墨量随笔画宽衰减到 0）。
  /// 只在拟合开启时生效（它需要笔画列表）。
  void set_stem_darkening(bool enabled) noexcept { stem_darkening_ = enabled; }
  [[nodiscard]] auto stem_darkening() const noexcept -> bool { return stem_darkening_; }
  [[nodiscard]] auto ink_compensation() const noexcept -> bool { return ink_compensation_; }
  [[nodiscard]] auto min_stem_coverage() const noexcept -> float { return min_stem_coverage_; }
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
    /// 网格拟合**因预算不足被拒**的笔画数（诊断口径，2026-10-04 新增）。
    ///
    /// 被拒的笔画 = 没被吸附到网格的那一根 = 同字里“看着更细更灰”的笔画，
    /// 也就是用户反馈的「线条粗细不均匀」。与 `origin_x` 同理：不看内部值，
    /// 这类“本可以对齐却没对”的漏网只能靠肉眼发现——所以把它报出来。
    /// 拟合关时为 0。
    int fit_rejected_stems{0};
    /// 拟合**找到并参与**的笔画数（竖 + 横），以及抽取漏斗（回答“为什么没找到”）。
    ///
    /// 四个数一起看才能定位“不均匀”的类型：
    /// `funnel.straight` 小 ⇒ **识别不足**（边大多不是直线，或不够直/不够长）；
    /// `funnel.pairs_failed` 大 ⇒ **配对失败**（宽度超 `max_stem_width` / 跨度不重叠）；
    /// `fit_rejected_stems` 大 ⇒ **预算不足**（调 `max_shift`）。
      int fit_stems{0};
    /// 其中来自**字体自带 hints** 的笔画数。
    ///
    /// 为什么要单列：它把“找不到笔画”（几何法召回不足，`fit_stems` 小且本值为 0）
    /// 与“找到了但推不动”（`fit_rejected_stems` 大）分开，而且能直接验收 hint 路径
    /// 是否真的生效（CFF 字体上本值应接近该字形的实际笔画数）。
    int fit_hint_stems{0};
    /// 诊断：字体给出的 hint 笔画数 / 其中成功接上轮廓点的数（方案 B 的验收入口）。
    int fit_hint_seen{0};
    int fit_hint_bound{0};
  /// 本次渲染**是否真的做了拟合**（`GridFitResult::applied`）。
  ///
  /// 与 `fit_stems` 的区别很重要：`fit_stems` 是“找到多少条笔画”，而它才是“这个字形
  /// 最后到底被拟合了没有”——护栏（一未动 / 平移超限 / 覆盖率不足）都会把它置 false
  /// 而 `fit_stems` 依旧非零。量“应用率”必须用本字段。
  bool fit_applied{false};
  /// 格子补偿的**幂指数**（1.0 = 未补偿；见 `text.cpp` 的实现说明）。
  /// 供量尺/诊断回答“这个字被调了多少”——没有它，“字变黑了”无法归因。
  float ink_compensation{1.0f};
  st::text::GridFitResult::Funnel fit_funnel{};
    /// 被拒笔画里最坏的单边位移需求（**物理像素**）——`max_shift` 要放到多大才收得下它。
    float fit_worst_rejected_shift{0.0f};
  };

  /// 取某个**码点**的字形位图（公开的诊断入口）。
  ///
  /// 用途：验证"字形没有被裁切"——位图四周必须留白（生成时 padding=1），
  /// 墨迹一旦贴边就说明包围盒算小了（曲线极值被切掉），
  /// 而**字宽不变**（advance 不受影响）→ 现象就是"排版完好、字却变了样"。
  /// 返回 `nullptr` 表示该码点在字体栈里没有对应字形。
    /// `bold=true` 时**用真粗体字体面**（见 `FontStack::find_face(…, bold)`）。
    [[nodiscard]] auto glyph_bitmap_of(char32_t codepoint, float pixel_size,
                                       FontRole role = FontRole::Proportional,
                                       int embolden_steps = 0, bool bold = false) const
      -> std::shared_ptr<const GlyphBitmap>;

 private:
  /// 取字形覆盖率位图（按 face/字形/物理尺寸/超采样 缓存）。
  /// 返回 `shared_ptr`：即使该条目随后被淘汰，调用方手里的位图依然有效
  /// （曾因缓存「插入后淘汰」并返回裸指针导致 use-after-free，见 text.cpp 注释）。
  [[nodiscard]] auto glyph_bitmap(const FontFace& face, GlyphId glyph, float pixel_size,
                                  int embolden_steps,
                                  GlyphClass glyph_class = GlyphClass::Default) const
      -> std::shared_ptr<const GlyphBitmap>;
  /// 无缓存版整形（`shape_cached` 未命中时的计算体）。
  [[nodiscard]] auto shape_uncached(std::string_view utf8, float size, FontRole role,
                                    bool bold) const -> ShapedText;
  /// 淘汰超出预算的字形条目（**调用方须持有锁**，且在插入之前调用）。
  /// `incoming_bytes` 是即将插入条目的内存量——先腾出它的位置。
  void trim_cache(std::size_t incoming_bytes) const;

  const FontStack* stack_{nullptr};
  float supersample_{1.0f};
  /// 亚像素（LCD）渲染开关；默认**关**——灰度是可逐像素断言的参考口径。
  bool subpixel_{false};
  /// 亚像素 5-tap 低通滤波开关（见 `set_subpixel_filter`）。
  bool subpixel_filter_{true};
  /// 覆盖率 gamma 预校正指数（见 `set_coverage_gamma`）；1.0 = 关（旧行为）。
  float coverage_gamma_{kDefaultCoverageGamma};
  /// 按物理字号覆盖 gamma 的阈值（0 = 不覆盖）与取值，见 `set_fitted_gamma`。
  float fitted_gamma_size_{0.0f};
  float fitted_gamma_{0.0f};
  /// 按**字形类**覆盖 gamma（0 = 不覆盖），见 `set_class_gamma`。
  std::array<float, kGlyphClassCount> class_gamma_{0.0f, 0.0f, 0.0f, 0.0f};
  /// 覆盖率预校正模式（见 `set_coverage_correct`）。
  CoverageCorrect coverage_correct_{CoverageCorrect::Gamma};
  /// Skia 模式的对比度（只影响深字浅底）。
  float coverage_contrast_{1.0f};
  /// 文字色 / 背景色（仅 Skia 模式用于索引 LUT；默认黑字白底）。
  std::uint32_t text_color_{0xFF000000U};
  std::uint32_t background_color_{0xFFFFFFFFU};
  /// 网格拟合模式（见 `set_grid_fit`）；默认关，保证无头/回归的可复现性。
  GridFitMode grid_fit_{GridFitMode::Off};
  /// 拟合的直线边最大横向斜率（物理像素；见 `set_fit_slant`）。
  /// 0 = 用 `GridFitOptions` 的默认值（不在渲染器里另立一份常数）。
  float fit_slant_{0.0f};
  /// 拟合的最低笔画抽取覆盖率（<0 = 用 `GridFitOptions` 的默认值）。
  float min_stem_coverage_{-1.0f};
  /// 拟合墨量补偿开关（见 `set_ink_compensation`）。
  bool ink_compensation_{false};
  bool stem_darkening_{false};
  struct Cache;
  std::unique_ptr<Cache> cache_{};
};

}  // namespace st::text

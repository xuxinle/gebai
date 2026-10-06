#pragma once

/// 字体解析（text 层）：TrueType（`glyf`/`loca`/`cmap`/`hmtx`/`kern`）、OpenType CFF（`CFF ` +
/// Type2 charstring）、集合字体 TTC/OTC（`ttcf` 多 face）、CID-keyed CFF（`FDArray`/`FDSelect`/
/// `Private`/`Subrs`）。全自研、零第三方依赖。
///
/// 约定：
/// - **安全失败**：任何长度不足/偏移越界/结构损坏的字体一律返回 `Error`（`Parse`/`Invalid`），
///   绝不越界读；`FontFace::load` 失败时对象不可用。
/// - **坐标**：`glyph_outline` 返回**字体单位**、**y 向上**、原点在**基线笔位**（pen origin）的路径；
///   缩放（`size / units_per_em`）与 y 轴翻转（画布 y 向下）由 `TextRenderer` 负责。
/// - **缓存**：字形轮廓按 face 缓存（`GlyphId → Path`），重复取同一字形为常数时间。
/// - **拷贝语义**：`FontFace` 可拷贝，副本共享不可变的字体数据与轮廓缓存（内部 `shared_ptr`）。

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "st/core/error.hpp"
#include "st/raster/path.hpp"

namespace st::text {

/// 字形索引（字体内部编号；0 恒为 `.notdef`）。
using GlyphId = std::uint32_t;

/// 字体级度量（除 `glyph_count` 外均为**字体单位**）。
struct FontMetrics {
  float units_per_em{1000.0f};
  float ascender{0.0f};
  float descender{0.0f};  ///< 负数
  float line_gap{0.0f};
  std::uint32_t glyph_count{0};
};

/// 字体**自带**的笔画提示（CFF `hstem`/`vstem`/`hstemhm`/`vstemhm`，字体单位）。
///
/// 为什么需要它：CFF（OTF）字体把"笔画在哪、多宽"直接写在 charstring 的 stem hints 里，
/// 而按轮廓几何反推笔画（两两配对直线边）的**召回只有 ~19%**（实测，见
/// `docs/BACKLOG.md` 的拟合漏斗）——因为汉字大量笔画带微斜度或由曲线构成，配不上对，
/// 于是同一字里一部分笔画被吸附、另一部分留着分数相位，正是"线条粗细不均匀"的来源。
/// 有了自带 hints，笔画位置与宽度都是**字体设计者给的**，召回不再靠猜。
///
/// `lo`/`hi` 是横轴（vstem 为 x、hstem 为 y）上的两个边缘；**顺序不保证**（取 `min`/`max`）。
/// 无 hint 的字体（TrueType `glyf` 轮廓）返回空——那时仍走几何拟合那条路。
struct StemHint {
  float lo{0.0f};
  float hi{0.0f};
  bool vertical{true};  ///< true = `vstem`（竖笔画，横轴是 x）
};

/// 单字形度量（字体单位）。
struct Glyph {
  GlyphId id{0};
  float advance{0.0f};
  float bearing_x{0.0f};
  float bearing_y{0.0f};
  /// 字形包围盒的**底边**（字体单位，基线上为正）。
  ///
  /// 与 `bearing_y` 配对才拿得到墨迹的垂直跨度——而那是"把一行字在一片高度里
  /// 真正居中"的唯一依据（行盒/ascent 都含字体预留的头尾空间，见
  /// `TextRenderer::ink_metrics`）。
  /// 字母通常为 0（坐在基线上），`g`/`，` 这类下探字形为负。
  float ink_bottom{0.0f};
  bool empty{true};
};

/// 单个字体文件（或集合字体的单个 face）。
class FontFace {
 public:
  FontFace() = default;

  /// 加载字体文件/face。
  /// @param path 字体文件路径（TTF/OTF/TTC/OTC）。
  /// @param face_index 集合字体的 face 序号（0 起；非集合字体只接受 0）。
  /// @return 失败：`Io` 读文件失败、`Parse` 非字体/结构损坏/截断、`NotFound` face 序号越界。
  [[nodiscard]] static auto load(std::string_view path, int face_index = 0) -> Result<FontFace>;

  /// 码点 → 字形索引（经 `cmap` 格式 4/6/12/0）。
  /// @return 失败：`NotFound` 字体不含该码点。
  [[nodiscard]] auto glyph_index(char32_t codepoint) const -> Result<GlyphId>;

  /// 字形度量（`advance`/`bearing` 为字体单位；`empty` 表示无轮廓，如空格）。
  /// @return 失败：`NotFound` 字形索引越界。
  [[nodiscard]] auto glyph(GlyphId id) const -> Result<Glyph>;

  /// 码点 → 字形度量（`glyph_index` + `glyph` 的便捷组合）。
  [[nodiscard]] auto glyph_for(char32_t codepoint) const -> Result<Glyph>;

  /// 是否覆盖该码点。
  [[nodiscard]] auto has_glyph(char32_t codepoint) const -> bool;

  /// 字体度量（默认构造的 face 返回零值）。
  [[nodiscard]] auto metrics() const noexcept -> const FontMetrics&;

  /// 字体名（`name` 表的 full name / family name；缺表时为空串）。
  [[nodiscard]] auto name() const -> const std::string&;

  /// 加载时使用的字体文件路径。
  [[nodiscard]] auto path() const -> const std::string&;

  /// 是否是 **CFF（OTF）轮廓**字体（`glyf` 缺席且有 `CFF ` 表）。
  ///
  /// 为什么需要它：**笔画加墨（stem darkening）只该对 CFF 字体做**——
  /// 这是 FreeType 自身的规则（`cff` 引擎的 `no-stem-darkening` 默认为 FALSE；
  /// 而 TrueType 的指令本身就保证笔画不为零宽，再加墨会过粗）。
  /// 实测（2026-10-06）：同字号下 DejaVu（真型）已经比浏览器**深 1.09**，
  /// 而 Noto Sans CJK（CFF）只有 0.89——不对字体类型分档就会"补了 CFF、过深了 TTF"。
  [[nodiscard]] auto is_cff() const noexcept -> bool;

  /// 加载时使用的 face 序号。
  [[nodiscard]] auto face_index() const noexcept -> int;

  /// 字形轮廓（**字体单位、y 向上、原点在基线笔位**）。
  /// @return 失败：`NotFound` 索引越界、`Parse` 轮廓数据损坏、`Unsupported` 不支持的轮廓字符
  ///         串操作（如 Type2 `seac` 组合字形）。
  [[nodiscard]] auto glyph_outline(GlyphId id) const -> Result<raster::Path>;

  /// `kern` 表（格式 0）字距调整，**字体单位**；无该对返回 0（`noexcept`，不报错）。
  [[nodiscard]] auto kerning(GlyphId left, GlyphId right) const noexcept -> float;

  /// 该字形**字体自带**的笔画提示（字体单位；顺序即 charstring 里的出现顺序）。
  ///
  /// 仅 CFF（OTF）字体有；`glyf` 轮廓或解释失败时返回空表（调用方回退到几何拟合）。
  /// 结果按字形缓存（与轮廓缓存同一生命周期）。
  [[nodiscard]] auto stem_hints(GlyphId id) const -> const std::vector<StemHint>&;

 private:
  struct Data;  ///< 内部实现（见 `src/text/font.cpp`；不可变解析结果 + 缓存）
  std::shared_ptr<Data> data_{};
};

}  // namespace st::text

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

/// 单字形度量（字体单位）。
struct Glyph {
  GlyphId id{0};
  float advance{0.0f};
  float bearing_x{0.0f};
  float bearing_y{0.0f};
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

  /// 加载时使用的 face 序号。
  [[nodiscard]] auto face_index() const noexcept -> int;

  /// 字形轮廓（**字体单位、y 向上、原点在基线笔位**）。
  /// @return 失败：`NotFound` 索引越界、`Parse` 轮廓数据损坏、`Unsupported` 不支持的轮廓字符
  ///         串操作（如 Type2 `seac` 组合字形）。
  [[nodiscard]] auto glyph_outline(GlyphId id) const -> Result<raster::Path>;

  /// `kern` 表（格式 0）字距调整，**字体单位**；无该对返回 0（`noexcept`，不报错）。
  [[nodiscard]] auto kerning(GlyphId left, GlyphId right) const noexcept -> float;

 private:
  struct Data;  ///< 内部实现（见 `src/text/font.cpp`；不可变解析结果 + 缓存）
  std::shared_ptr<Data> data_{};
};

}  // namespace st::text

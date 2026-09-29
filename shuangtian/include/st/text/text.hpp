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
class FontStack {
 public:
  explicit FontStack(std::vector<FontFace> faces);

  /// 系统默认回退链（拉丁 + CJK；探测系统字体目录）。
  /// @return 失败：`NotFound` 未找到任何可用字体。
  [[nodiscard]] static auto system_default() -> Result<FontStack>;
  /// 显式指定字体文件（按给出顺序即回退优先级）。
  [[nodiscard]] static auto from_files(const std::vector<std::string>& paths) -> Result<FontStack>;

  /// 覆盖该码点的 face（无覆盖返回 nullptr）。
  [[nodiscard]] auto find_face(char32_t codepoint) const -> const FontFace*;
  [[nodiscard]] auto primary() const -> const FontFace& { return faces_.front(); }
  [[nodiscard]] auto faces() const noexcept -> std::span<const FontFace> { return faces_; }
  [[nodiscard]] auto empty() const noexcept -> bool { return faces_.empty(); }

  /// 供渲染器使用的共享字节大小（缓存键的一部分）。
  [[nodiscard]] auto fingerprint() const noexcept -> std::uint64_t { return fingerprint_; }

 private:
  std::vector<FontFace> faces_{};
  std::uint64_t fingerprint_{0};
};

/// 文本渲染器：整形 + 度量 + 绘制（带字形位图缓存）。
class TextRenderer {
 public:
  explicit TextRenderer(const FontStack& stack, float supersample = 1.0f);
  ~TextRenderer();
  TextRenderer(const TextRenderer&) = delete;
  auto operator=(const TextRenderer&) -> TextRenderer& = delete;

  /// 整形（逻辑单位）。
  [[nodiscard]] auto shape(std::string_view utf8, float size) const -> ShapedText;
  /// 度量：宽 × 行高（逻辑单位）。
  [[nodiscard]] auto measure(std::string_view utf8, float size) const -> math::Size;
  [[nodiscard]] auto measure_width(std::string_view utf8, float size) const -> float;
  [[nodiscard]] auto line_height(float size) const -> float;
  /// 基线相对行顶的偏移（逻辑单位）。
  [[nodiscard]] auto ascent(float size) const -> float;

  /// 绘制：`origin` 为**逻辑坐标**下的行左上角；字形按画布 DPI 物理栅格化。
  auto draw(raster::Surface& canvas, std::string_view utf8, math::Point origin, float size,
            math::Color color) const -> Status;

  /// 折行（按空格与 CJK 断点；返回各行原文区间）。
  [[nodiscard]] auto wrap(std::string_view utf8, float size, float max_width) const
      -> std::vector<std::string_view>;
  /// 截断加省略号。
  [[nodiscard]] auto ellipsize(std::string_view utf8, float size, float max_width) const
      -> std::string;
  /// 单个字符的推进宽度（用于光标定位）。
  [[nodiscard]] auto advance_of(char32_t codepoint, float size) const -> float;
  /// 光标 x 偏移（逻辑单位）。
  [[nodiscard]] auto cursor_x(std::string_view utf8, float size, std::size_t codepoint_index) const
      -> float;
  /// 命中测试：给定局部 x 求最近的码点索引。
  [[nodiscard]] auto index_at_x(std::string_view utf8, float size, float local_x) const
      -> std::size_t;

  void set_supersample(float factor);
  [[nodiscard]] auto supersample() const noexcept -> float { return supersample_; }
  [[nodiscard]] auto stack() const noexcept -> const FontStack& { return *stack_; }
  /// 字形位图缓存条目数（诊断用）。
  [[nodiscard]] auto cache_entries() const noexcept -> std::size_t;

 private:
  struct GlyphBitmap {
    int width{0};
    int height{0};
    int offset_x{0};  ///< 相对笔位的物理像素偏移（左上角）
    int offset_y{0};  ///< 相对基线的物理像素偏移（向上为负）
    std::vector<float> coverage{};  ///< 物理像素覆盖率（已按超采样下采样）
  };

  /// 取字形覆盖率位图（按 face/字形/物理尺寸/超采样 缓存）。
  /// 返回 `shared_ptr`：即使该条目随后被淘汰，调用方手里的位图依然有效
  /// （曾因缓存「插入后淘汰」并返回裸指针导致 use-after-free，见 text.cpp 注释）。
  [[nodiscard]] auto glyph_bitmap(const FontFace& face, GlyphId glyph, float pixel_size) const
      -> std::shared_ptr<const GlyphBitmap>;
  void trim_cache() const;

  const FontStack* stack_{nullptr};
  float supersample_{1.0f};
  struct Cache;
  std::unique_ptr<Cache> cache_{};
};

}  // namespace st::text

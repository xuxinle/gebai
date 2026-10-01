#include "st/text/text.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <list>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "st/core/fs.hpp"
#include "st/core/hash.hpp"
#include "st/core/string.hpp"
#include "st/core/time.hpp"
#include "st/raster/paint.hpp"

namespace st::text {
namespace {

/// 无限宽哨兵（与 `ui::kUnbounded` 同值；text 层不依赖 ui 层）。
inline constexpr float kUnbounded = 1.0e9f;

/// CJK 断行机会（可在任意汉字之间断行；避头尾暂未实现）。
[[nodiscard]] auto is_cjk(char32_t codepoint) noexcept -> bool {
  return (codepoint >= 0x2E80U && codepoint <= 0x9FFFU) ||
         (codepoint >= 0xF900U && codepoint <= 0xFAFFU) ||
         (codepoint >= 0xFF00U && codepoint <= 0xFFEFU) ||
         (codepoint >= 0x20000U && codepoint <= 0x3FFFFU);
}

[[nodiscard]] auto is_break_space(char32_t codepoint) noexcept -> bool {
  return codepoint == U' ' || codepoint == U'\t' || codepoint == U'\n' || codepoint == U'\r' ||
         codepoint == U'\u3000';
}

/// 系统字体候选（按回退优先级：拉丁 → CJK → 通用）。
struct FontCandidate {
  std::string path;
  bool prefer_cjk_face;
};

[[nodiscard]] auto font_candidates() -> std::vector<FontCandidate> {
  std::vector<FontCandidate> candidates;
  const auto push = [&candidates](std::string path, bool cjk) {
    if (path.empty()) return;
    if (!fs::is_regular_file(path)) return;
    candidates.push_back(FontCandidate{std::move(path), cjk});
  };
  if (const auto custom = fs::read_env("ST_FONT_LATIN"); custom.has_value()) {
    push(*custom, false);
  }
  if (const auto custom = fs::read_env("ST_FONT_CJK"); custom.has_value()) {
    push(*custom, true);
  }
  for (const auto* path : {
           "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
           "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
           "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
           "C:/Windows/Fonts/segoeui.ttf",
           "C:/Windows/Fonts/arial.ttf",
           "/System/Library/Fonts/SFNS.ttf",
           "/System/Library/Fonts/Helvetica.ttc",
       }) {
    push(path, false);
  }
  for (const auto* path : {
           "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
           "/usr/share/fonts/opentype/noto/NotoSansCJKsc-Regular.otf",
           "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
           "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
           "C:/Windows/Fonts/msyh.ttc",
           "C:/Windows/Fonts/simhei.ttf",
           "/System/Library/Fonts/PingFang.ttc",
       }) {
    push(path, true);
  }
  return candidates;
}

/// 等宽候选（代码用）。与正文档**分开探测**：等宽字体失败只是"没有等宽"
/// （代码块退化成正文字体仍可读），不该让整个字体栈失败。
[[nodiscard]] auto mono_font_candidates() -> std::vector<FontCandidate> {
  std::vector<FontCandidate> candidates;
  const auto push = [&candidates](const std::string& path) {
    if (path.empty() || !fs::is_regular_file(path)) return;
    candidates.push_back(FontCandidate{path, false});
  };
  if (const auto custom = fs::read_env("ST_FONT_MONO"); custom.has_value()) push(*custom);
  for (const auto* path : {
           "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
           "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
           "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf",
           "C:/Windows/Fonts/CascadiaMono.ttf",
           "C:/Windows/Fonts/consola.ttf",
           "C:/Windows/Fonts/cour.ttf",
           "/System/Library/Fonts/Menlo.ttc",
           "/System/Library/Fonts/SFNSMono.ttf",
       }) {
    push(path);
  }
  return candidates;
}


/// 载入字体：CJK 集合字体挑出真正覆盖汉字的 face（NotoSansCJK OTC 的 SC face 常非 0 号）。
[[nodiscard]] auto load_face(const FontCandidate& candidate) -> Result<FontFace> {
  constexpr char32_t kProbe = U'霜';
  constexpr int kMaxFaces = 12;
  if (candidate.prefer_cjk_face) {
    // 集合字体（Noto Sans CJK OTC 等）内含 JP/KR/SC/TC 多个 face，字形码位相同但**字形形态不同**：
    // 优先选简体（SC）face，避免中文界面出现日文字形变体；无 SC 时取首个覆盖汉字的 face。
    std::optional<FontFace> first_covering;
    for (int index = 0; index < kMaxFaces; ++index) {
      auto face = FontFace::load(candidate.path, index);
      if (!face) break;
      if (!face->has_glyph(kProbe)) continue;
      const std::string name = face->name();
      if (name.find("SC") != std::string::npos || name.find("Simplified") != std::string::npos) {
        return face;
      }
      if (!first_covering.has_value()) first_covering = std::move(*face);
    }
    if (first_covering.has_value()) return *first_covering;
  }
  return FontFace::load(candidate.path, 0);
}

[[nodiscard]] auto size_key(float size) noexcept -> std::uint32_t {
  const float clamped = size < 1.0f ? 1.0f : size;
  return static_cast<std::uint32_t>(std::lround(clamped * 4.0f));
}

/// 亚像素光栅化的水平采样倍数（每个物理像素 3 个子像素：R/G/B）。
inline constexpr int kSubpixelColumns = 3;

/// 亚像素 scratch 的宽度上限（防荒诞字号把内存吃光；超出则该字形退回灰度——
/// 位图的 `format` 字段会随之写成 Grayscale，所以混合端不会按 3 通道去读）。
inline constexpr int kMaxSubpixelWidth = 4096;

/// 子像素轴低通滤波（权重同 FreeType `FT_LCD_FILTER_DEFAULT`：`{8,77,86,77,8}/256`）。
///
/// 输入/输出都是**子像素序列**（三值交错、长度 = 3 × 像素数），就地修改；
/// 边缘按夹取处理（FreeType 同样复制边界，否则笔画端部会凭空变暗）。
void apply_lcd_filter(std::span<float> subpixels) {
  constexpr int kWeights[5] = {8, 77, 86, 77, 8};
  const std::size_t count = subpixels.size();
  if (count == 0) return;
  const std::vector<float> source(subpixels.begin(), subpixels.end());
  const auto last = static_cast<std::ptrdiff_t>(count) - 1;
  for (std::size_t index = 0; index < count; ++index) {
    float sum = 0.0f;
    for (int tap = -2; tap <= 2; ++tap) {
      const std::ptrdiff_t position = static_cast<std::ptrdiff_t>(index) + tap;
      const std::ptrdiff_t clamped = std::clamp(position, std::ptrdiff_t{0}, last);
      sum += source[static_cast<std::size_t>(clamped)] * static_cast<float>(kWeights[tap + 2]);
    }
    subpixels[index] = sum / 256.0f;
  }
}

}  // namespace

// —— FontStack ——

FontStack::FontStack(std::vector<FontFace> faces, std::vector<FontFace> mono_faces)
    : faces_(std::move(faces)), mono_faces_(std::move(mono_faces)) {
  std::uint64_t fingerprint = 1469598103934665603ULL;
  for (const auto& face : faces_) {
    fingerprint ^= st::hash::fnv1a64(face.path());
    fingerprint *= 1099511628211ULL;
    fingerprint ^= static_cast<std::uint64_t>(face.face_index());
  }
  // 指纹要覆盖等宽库：字形缓存按指纹分桶，漏掉它会让两套字体互相污染。
  for (const auto& face : mono_faces_) {
    fingerprint ^= st::hash::fnv1a64(face.path());
    fingerprint *= 1099511628211ULL;
    fingerprint ^= static_cast<std::uint64_t>(face.face_index()) + 0x9E3779B9ULL;
  }
  fingerprint_ = fingerprint;
}

auto FontStack::system_default() -> Result<FontStack> {
  std::vector<FontFace> faces;
  std::string last_error;
  for (const auto& candidate : font_candidates()) {
    auto face = load_face(candidate);
    if (!face) {
      last_error = std::format("{}: {}", candidate.path, face.error().message);
      continue;
    }
    faces.push_back(std::move(*face));
  }
  if (faces.empty()) {
    return unexpected(ErrorCode::NotFound,
                      std::format("未找到可用系统字体（最后错误：{}）", last_error));
  }
  // 等宽库单独探测：失败只是"没有等宽"（代码块退化成正文字体仍可读），
  // 不该让整个字体栈失败——完全没字体才是什么都渲染不出来。
  std::vector<FontFace> mono_faces;
  for (const auto& candidate : mono_font_candidates()) {
    if (auto face = load_face(candidate); face.has_value()) {
      mono_faces.push_back(std::move(*face));
    }
  }
  return FontStack(std::move(faces), std::move(mono_faces));
}

auto FontStack::from_files(const std::vector<std::string>& paths) -> Result<FontStack> {
  std::vector<FontFace> faces;
  for (const auto& path : paths) {
    FontCandidate candidate{path, false};
    auto face = load_face(candidate);
    if (!face) return forward_error(face.error());
    faces.push_back(std::move(*face));
  }
  if (faces.empty()) return unexpected(ErrorCode::Invalid, "字体列表为空");
  std::vector<FontFace> mono_faces;
  for (const auto& candidate : mono_font_candidates()) {
    if (auto face = load_face(candidate); face.has_value()) {
      mono_faces.push_back(std::move(*face));
    }
  }
  return FontStack(std::move(faces), std::move(mono_faces));
}

auto FontStack::find_face(char32_t codepoint, FontRole role) const -> const FontFace* {
  if (role == FontRole::Monospace) {
    // 先等宽库，再回退正文档：汉字在多数等宽字体里没有，而代码块里的中文注释
    // 必须能显示——宁可混排，不可缺字。
    for (const auto& face : mono_faces_) {
      if (face.has_glyph(codepoint)) return &face;
    }
  }
  return find_face(codepoint);
}

auto FontStack::find_face(char32_t codepoint) const -> const FontFace* {
  for (const auto& face : faces_) {
    if (face.has_glyph(codepoint)) return &face;
  }
  return nullptr;
}

// —— TextRenderer ——

struct TextRenderer::Cache {
  mutable std::mutex mutex{};
  /// 字形位图缓存条目。
  /// 值用 `shared_ptr` 而非 `unique_ptr`——淘汰时仍持有该位图的调用方不受影响
  /// （曾因"插入后清空缓存 + 返回裸指针"导致 heap-use-after-free，ASan 实测定位）。
  struct GlyphEntry {
    std::shared_ptr<const GlyphBitmap> bitmap{};
    std::size_t bytes{0};                  ///< 位图内存估计（淘汰预算用）
    std::list<std::uint64_t>::iterator lru{};  ///< 指向 `lru_order` 中自己的位置
  };
  std::unordered_map<std::uint64_t, GlyphEntry> glyphs{};
  /// LRU 顺序：front = 最近使用。
  ///
  /// 旧实现超限时 `glyphs.clear()` 全清——CJK 大文档（>上限字形）滚动时
  /// 周期性"全清 → 全部重栅格化"造成帧尖峰。真 LRU 后淘汰是增量的。
  std::list<std::uint64_t> lru_order{};
  std::size_t glyph_bytes{0};
  std::unordered_map<std::uint64_t, float> widths{};  ///< 文本宽度缓存（键：文本 + 字号档）
  /// 整形结果缓存（键：文本 + 字号档 + role + 字体栈指纹，与 widths 同构）。
  ///
  /// 为什么值得缓存：`draw()` 每帧对每段文本重新整形（逐码点 face 查找 + 字距 +
  /// runs 向量分配）。静态界面的文字跨帧不变，重复整形是纯浪费（§4.3 声称的
  /// 「缓存：整形结果」此前并未实现）。
  struct ShapedEntry {
    std::shared_ptr<const ShapedText> text{};
    std::size_t bytes{0};
    std::list<std::uint64_t>::iterator lru{};
  };
  std::unordered_map<std::uint64_t, ShapedEntry> shaped{};
  std::list<std::uint64_t> shaped_lru{};
  std::size_t shaped_bytes{0};
  static constexpr std::size_t kShapedMaxBytes = 16 * 1024 * 1024;
  static constexpr std::size_t kShapedMaxEntries = 4096;
  /// 淘汰预算：按**内存**而不是条目数（字形位图大小差异极大——空白字形与 64px 汉字
  /// 差三个数量级；512 条目的旧上限对 16px 汉字只值 ~800 KiB，对 64px 却要 32 MiB）。
  static constexpr std::size_t kMaxBytes = 24 * 1024 * 1024;
  /// 条目数硬上限（防小位图海量时的 map 开销）。
  static constexpr std::size_t kMaxEntries = 32768;
};

TextRenderer::TextRenderer(const FontStack& stack, float supersample)
    : stack_(&stack), supersample_(supersample < 1.0f ? 1.0f : supersample),
      cache_(std::make_unique<Cache>()) {
  // 亚像素低通滤波可由环境变量关掉（对照实验/排障用）：`ST_TEXT_LCD_FILTER=0`。
  if (const auto value = fs::read_env("ST_TEXT_LCD_FILTER"); value.has_value()) {
    subpixel_filter_ = !(*value == "0" || *value == "off" || *value == "false");
  }
}

TextRenderer::~TextRenderer() = default;

void TextRenderer::set_supersample(float factor) {
  supersample_ = factor < 1.0f ? 1.0f : factor;
  const std::scoped_lock lock(cache_->mutex);
  cache_->glyphs.clear();
  cache_->lru_order.clear();
  cache_->glyph_bytes = 0;
}

auto TextRenderer::cache_entries() const noexcept -> std::size_t { return cache_->glyphs.size(); }

auto TextRenderer::ascent(float size) const -> float {
  if (stack_->empty()) return size * 0.8f;
  const FontMetrics& metrics = stack_->primary().metrics();
  const float units = metrics.units_per_em > 0.0f ? metrics.units_per_em : 1000.0f;
  return metrics.ascender / units * size;
}

auto TextRenderer::line_height(float size) const -> float {
  if (stack_->empty()) return size * 1.45f;
  const FontMetrics& metrics = stack_->primary().metrics();
  const float units = metrics.units_per_em > 0.0f ? metrics.units_per_em : 1000.0f;
  const float span = metrics.ascender - metrics.descender + metrics.line_gap;
  const float height = span / units * size;
  return height > size ? height : size * 1.2f;
}

auto TextRenderer::shape(std::string_view utf8, float size, FontRole role) const -> ShapedText {
  // 拷贝返回（公开 API 保持值语义）；缓存命中时省下的是整形本身，
  // 拷贝只是一次 runs 向量复制（远低于逐码点 face 查找 + 字距的开销）。
  return *shape_cached(utf8, size, role);
}

auto TextRenderer::shape_cached(std::string_view utf8, float size, FontRole role) const
    -> std::shared_ptr<const ShapedText> {
  const std::uint64_t key = st::hash::fnv1a64(utf8) ^
                            (static_cast<std::uint64_t>(size_key(size)) << 32U) ^
                            (static_cast<std::uint64_t>(role) << 56U) ^ stack_->fingerprint();
  {
    const std::scoped_lock lock(cache_->mutex);
    if (const auto iterator = cache_->shaped.find(key); iterator != cache_->shaped.end()) {
      // 命中：提到 LRU 头部
      cache_->shaped_lru.splice(cache_->shaped_lru.begin(), cache_->shaped_lru,
                                iterator->second.lru);
      return iterator->second.text;
    }
  }
  auto shaped = std::make_shared<ShapedText>(shape_uncached(utf8, size, role));
  const std::size_t bytes = shaped->runs.size() * sizeof(TextRun) + utf8.size() + 64;
  if (bytes <= Cache::kShapedMaxBytes) {  // 超大文本不入缓存（一跳进就会出现"刚插就淘汰"）
    const std::scoped_lock lock(cache_->mutex);
    while (!cache_->shaped_lru.empty() &&
           (cache_->shaped_bytes + bytes > Cache::kShapedMaxBytes ||
            cache_->shaped.size() >= Cache::kShapedMaxEntries)) {
      const std::uint64_t victim = cache_->shaped_lru.back();
      cache_->shaped_lru.pop_back();
      const auto iterator = cache_->shaped.find(victim);
      if (iterator == cache_->shaped.end()) continue;
      cache_->shaped_bytes -= iterator->second.bytes;
      cache_->shaped.erase(iterator);
    }
    cache_->shaped_lru.push_front(key);
    cache_->shaped_bytes += bytes;
    cache_->shaped.emplace(key, Cache::ShapedEntry{shaped, bytes, cache_->shaped_lru.begin()});
  }
  return shaped;
}

auto TextRenderer::shape_uncached(std::string_view utf8, float size, FontRole role) const
    -> ShapedText {
  ShapedText shaped;
  if (stack_->empty() || utf8.empty() || size <= 0.0f) {
    shaped.line_height = line_height(size);
    shaped.ascent = ascent(size);
    return shaped;
  }
  const FontFace* previous_face = nullptr;
  GlyphId previous_glyph = 0;
  float pen = 0.0f;
  float max_ascent = 0.0f;
  float max_descent = 0.0f;

  std::size_t index = 0;
  while (index < utf8.size()) {
    const Codepoint codepoint = decode_utf8(utf8, index);
    if (codepoint.bytes == 0) break;
    const FontFace* face = stack_->find_face(codepoint.value, role);
    if (face == nullptr) {
      // 无字体覆盖：按空格宽度占位（保证排版不塌陷）
      pen += size * 0.5f;
      previous_face = nullptr;
      continue;
    }
    auto glyph = face->glyph_for(codepoint.value);
    if (!glyph) {
      pen += size * 0.5f;
      previous_face = nullptr;
      continue;
    }
    const FontMetrics& metrics = face->metrics();
    const float units = metrics.units_per_em > 0.0f ? metrics.units_per_em : 1000.0f;
    float kerning = 0.0f;
    if (previous_face == face && previous_glyph != 0) {
      kerning = face->kerning(previous_glyph, glyph->id) / units * size;
    }
    pen += kerning;
    TextRun run;
    run.face = face;
    run.glyph = glyph->id;
    run.codepoint = codepoint.value;
    run.x = pen;
    run.advance = glyph->advance / units * size;
    pen += run.advance;
    max_ascent = std::max(max_ascent, metrics.ascender / units * size);
    max_descent = std::max(max_descent, -metrics.descender / units * size);
    shaped.runs.push_back(run);
    previous_face = face;
    previous_glyph = glyph->id;
  }
  shaped.width = pen;
  shaped.ascent = max_ascent > 0.0f ? max_ascent : ascent(size);
  shaped.descent = max_descent;
  shaped.line_height = line_height(size);
  return shaped;
}

auto TextRenderer::measure_width(std::string_view utf8, float size, FontRole role) const -> float {
  if (utf8.empty()) return 0.0f;
  // 缓存键**必须带 role**：同一段文字在等宽/比例下的宽度不同，
  // 漏掉它会让代码块与正文互相污染（宽度随机对不上）。
  const std::uint64_t key = st::hash::fnv1a64(utf8) ^
                            (static_cast<std::uint64_t>(size_key(size)) << 32U) ^
                            (static_cast<std::uint64_t>(role) << 56U) ^ stack_->fingerprint();
  {
    const std::scoped_lock lock(cache_->mutex);
    if (const auto iterator = cache_->widths.find(key); iterator != cache_->widths.end()) {
      return iterator->second;
    }
  }
  const float width = shape_cached(utf8, size, role)->width;
  {
    const std::scoped_lock lock(cache_->mutex);
    if (cache_->widths.size() > 4096) cache_->widths.clear();
    cache_->widths[key] = width;
  }
  return width;
}

auto TextRenderer::measure(std::string_view utf8, float size, FontRole role) const -> math::Size {
  return math::Size{measure_width(utf8, size, role), line_height(size)};
}

auto TextRenderer::advance_of(char32_t codepoint, float size, FontRole role) const -> float {
  if (stack_->empty()) return size * 0.5f;
  const FontFace* face = stack_->find_face(codepoint, role);
  if (face == nullptr) return size * 0.5f;
  auto glyph = face->glyph_for(codepoint);
  if (!glyph) return size * 0.5f;
  const FontMetrics& metrics = face->metrics();
  const float units = metrics.units_per_em > 0.0f ? metrics.units_per_em : 1000.0f;
  return glyph->advance / units * size;
}

auto TextRenderer::cursor_x(std::string_view utf8, float size, std::size_t codepoint_index) const
    -> float {
  const std::string_view prefix = utf8_slice(utf8, 0, codepoint_index);
  return measure_width(prefix, size);
}

auto TextRenderer::index_at_x(std::string_view utf8, float size, float local_x) const -> std::size_t {
  const std::size_t total = utf8_length(utf8);
  if (local_x <= 0.0f) return 0;
  float pen = 0.0f;
  std::size_t index = 0;
  while (index < total) {
    const std::string_view slice = utf8_slice(utf8, index, 1);
    const float advance = measure_width(slice, size);
    if (local_x < pen + advance * 0.5f) return index;
    pen += advance;
    ++index;
  }
  return total;
}

/// 淘汰超出预算的条目（**调用方须持有锁**，且在插入之前调用）。
///
/// 按 LRU 从尾部逐出；预算按字节（见 `Cache::kMaxBytes`）。
/// `incoming_bytes` 是即将插入条目的内存量——先腾出它的位置再插入，
/// 保持"先淘汰、后插入"的顺序（反过来曾出现"插入后清空"把新位图一起销毁，
/// `return slot.get()` 返回悬垂指针，ASan 实测 heap-use-after-free）。
void TextRenderer::trim_cache(std::size_t incoming_bytes) const {
  while (!cache_->lru_order.empty() &&
         (cache_->glyph_bytes + incoming_bytes > Cache::kMaxBytes ||
          cache_->glyphs.size() >= Cache::kMaxEntries)) {
    const std::uint64_t victim = cache_->lru_order.back();
    cache_->lru_order.pop_back();
    const auto iterator = cache_->glyphs.find(victim);
    if (iterator == cache_->glyphs.end()) continue;
    cache_->glyph_bytes -= iterator->second.bytes;
    cache_->glyphs.erase(iterator);
  }
}

auto TextRenderer::glyph_bitmap_of(char32_t codepoint, float pixel_size, FontRole role) const
    -> std::shared_ptr<const GlyphBitmap> {
  const FontFace* face = stack_->find_face(codepoint, role);
  if (face == nullptr) return nullptr;
  const auto id = face->glyph_index(codepoint);
  if (!id.has_value()) return nullptr;
  return glyph_bitmap(*face, *id, pixel_size);
}

auto TextRenderer::glyph_bitmap(const FontFace& face, GlyphId glyph, float pixel_size) const
    -> std::shared_ptr<const GlyphBitmap> {
  const std::uint32_t size_bucket = size_key(pixel_size);
  // 缓存键 = 逐字段 FNV-1a 混合，而不是"移位后 XOR 拼装"。
  //
  // 两者看起来等价，实际不然：XOR 拼装**只有位段互不重叠**时才等价于元组，
  // 一旦重叠（或字段被移出 64 位）不同的 (face, glyph, size) 就会落到同一个键，
  // 症状是**取到别人的字形**——字宽不变（CJK 全角等宽）、肉眼看到的就是"字变了"。
  // 实测就踩到过：`supersample*8 << 60` 在 supersample ≥ 4 时会整体移出 64 位，
  // 也就是"没有参与键"。混合式写法对任意位宽的字段都安全，也不需要人肉核对位段。
  const auto mix = [](std::uint64_t seed, std::uint64_t value) noexcept -> std::uint64_t {
    return (seed ^ value) * 1099511628211ULL;
  };
  const auto supersample_bucket = static_cast<std::uint64_t>(std::lround(supersample_ * 8.0f));
  // 渲染模式**必须进键**：灰度与亚像素的覆盖率位图排布不同（1 项/像素 vs 3 项/像素），
  // 混用等于按错误长度解读（表现是"字缺一块"或"字整片消失"，且只在切换模式的那一刻出现）。
  const bool lcd = subpixel_;
  const std::uint64_t key = mix(mix(mix(mix(st::hash::fnv1a64(face.path()),
                                            static_cast<std::uint64_t>(face.face_index())),
                                        static_cast<std::uint64_t>(glyph)),
                                    static_cast<std::uint64_t>(size_bucket)),
                                mix(supersample_bucket, lcd ? 1ULL : 0ULL));
  {
    const std::scoped_lock lock(cache_->mutex);
    if (const auto iterator = cache_->glyphs.find(key); iterator != cache_->glyphs.end()) {
      // 命中：提到 LRU 头部（真 LRU——被复用的字形永远不会被"周期性全清"波及）
      cache_->lru_order.splice(cache_->lru_order.begin(), cache_->lru_order,
                               iterator->second.lru);
      return iterator->second.bitmap;
    }
  }

  const FontMetrics& metrics = face.metrics();
  const float units = metrics.units_per_em > 0.0f ? metrics.units_per_em : 1000.0f;
  const float effective_size = static_cast<float>(size_bucket) / 4.0f;
  const int supersample = std::max(1, static_cast<int>(std::lround(supersample_)));
  const float scale = effective_size * static_cast<float>(supersample) / units;

  auto bitmap = std::make_shared<GlyphBitmap>();
  bitmap->cache_key = key;   // 稳定身份 = 上面那份缓存键（与内存地址无关）
  // 通道布局先按当前模式写好；下面若因尺寸护栏退回灰度，这里会同步改掉
  // （blend 端读的是**位图自带的** format，不是渲染器的当前模式，所以两者必须一致）。
  bitmap->format = lcd ? raster::CoverageFormat::Lcd : raster::CoverageFormat::Grayscale;
  auto outline = face.glyph_outline(glyph);
  const bool blank = !outline || outline->is_empty();

  if (!blank) {
    // 字体单位（y 向上）→ 物理像素（y 向下，笔位为原点）。
    // `horizontal`：亚像素模式下水平坐标再乘 3——同一份几何、两种采样密度。
    const auto build_path = [&outline, scale](float horizontal) {
      raster::Path path;
      const auto map_point = [scale, horizontal](math::Point point) noexcept -> math::Point {
        return math::Point{point.x * scale * horizontal, -point.y * scale};
      };
      for (const auto& command : outline->commands()) {
        switch (command.kind) {
          case raster::PathCommand::Kind::MoveTo:
            path.move_to(map_point(command.p1));
            break;
          case raster::PathCommand::Kind::LineTo:
            path.line_to(map_point(command.p1));
            break;
          case raster::PathCommand::Kind::QuadTo:
            path.quad_to(map_point(command.p1), map_point(command.p2));
            break;
          case raster::PathCommand::Kind::CubicTo:
            path.cubic_to(map_point(command.p1), map_point(command.p2), map_point(command.p3));
            break;
          case raster::PathCommand::Kind::Close:
            path.close();
            break;
        }
      }
      return path;
    };
    const raster::Path transformed = build_path(1.0f);

    const math::Rect bounds = transformed.flattened_bounds(0.2f);
    const int padding = 1;
    const auto min_x = static_cast<int>(std::floor(bounds.x)) - padding;
    const auto min_y = static_cast<int>(std::floor(bounds.y)) - padding;
    const auto max_x = static_cast<int>(std::ceil(bounds.right())) + padding;
    const auto max_y = static_cast<int>(std::ceil(bounds.bottom())) + padding;
    const int width = max_x - min_x;
    const int height = max_y - min_y;
    if (width > 0 && height > 0 && width <= 4096 && height <= 4096) {
      const int out_width = std::max(1, width / supersample);
      const int out_height = std::max(1, height / supersample);
      bitmap->width = out_width;
      bitmap->height = out_height;
      bitmap->offset_x = min_x / supersample;
      bitmap->offset_y = min_y / supersample;
      const std::size_t output_pixels =
          static_cast<std::size_t>(out_width) * static_cast<std::size_t>(out_height);

      if (!lcd || width * kSubpixelColumns > kMaxSubpixelWidth) {
        if (lcd) {
          // 荒唐字号（子像素 scratch 会超宽）→ 退回灰度：**如实退回**，
          // 连 `format` 一起改成灰度，混合端才不会按 3 通道去读一张单通道图。
          bitmap->format = raster::CoverageFormat::Grayscale;
        }
        raster::Canvas scratch(width, height);
        raster::Path local =
            transformed.translated(static_cast<float>(-min_x), static_cast<float>(-min_y));
        scratch.fill_path(local, raster::Paint::solid(math::Color::rgb(255, 255, 255)));
        bitmap->coverage.assign(output_pixels, 0.0f);
        for (int y = 0; y < out_height; ++y) {
          for (int x = 0; x < out_width; ++x) {
            float total = 0.0f;
            int samples = 0;
            for (int sy = 0; sy < supersample; ++sy) {
              for (int sx = 0; sx < supersample; ++sx) {
                const int px = x * supersample + sx;
                const int py = y * supersample + sy;
                const math::Color pixel = scratch.pixel_at(px, py);
                total += static_cast<float>(pixel.a) / 255.0f;
                ++samples;
              }
            }
            bitmap->coverage[static_cast<std::size_t>(y) * static_cast<std::size_t>(out_width) +
                             static_cast<std::size_t>(x)] =
                samples > 0 ? total / static_cast<float>(samples) : 0.0f;
          }
        }
      } else {
        // —— 亚像素（LCD）分支 ——
        // 几何按 x×3 光栅化到 **3×supersample 列 × supersample 行** 的 scratch：
        // 每个物理像素横跨 3·supersample 列，其中第 c 个子像素占连续 supersample 列。
        // 覆盖率逐子像素聚合 ⇒ 水平分辨率 3 倍（这就是 ClearType 类渲染的全部机理）。
        //
        // 包围盒**按灰度口径 ×3 推导**（而不是把 3 倍坐标重新算一遍 bounds）：
        // 两种模式的位图网格由此**逐像素重合**——开/关亚像素只改边缘合成方式，不挪字。
        raster::Canvas scratch(width * kSubpixelColumns, height);
        raster::Path local = build_path(static_cast<float>(kSubpixelColumns))
                                 .translated(static_cast<float>(-min_x * kSubpixelColumns),
                                             static_cast<float>(-min_y));
        scratch.fill_path(local, raster::Paint::solid(math::Color::rgb(255, 255, 255)));

        bitmap->coverage.assign(
            output_pixels * static_cast<std::size_t>(kSubpixelColumns), 0.0f);
        const float sample_count =
            static_cast<float>(supersample) * static_cast<float>(supersample);
        std::vector<float> row_subpixels(static_cast<std::size_t>(out_width) *
                                         static_cast<std::size_t>(kSubpixelColumns));
        for (int y = 0; y < out_height; ++y) {
          for (int x = 0; x < out_width; ++x) {
            for (int channel = 0; channel < kSubpixelColumns; ++channel) {
              float total = 0.0f;
              for (int sy = 0; sy < supersample; ++sy) {
                const int py = y * supersample + sy;
                for (int sx = 0; sx < supersample; ++sx) {
                  const int px = (x * kSubpixelColumns + channel) * supersample + sx;
                  total += static_cast<float>(scratch.pixel_at(px, py).a) / 255.0f;
                }
              }
              row_subpixels[static_cast<std::size_t>(x) * static_cast<std::size_t>(kSubpixelColumns) +
                            static_cast<std::size_t>(channel)] =
                  sample_count > 0.0f ? total / sample_count : 0.0f;
            }
          }
          // 滤波在**子像素轴**上做（像素边界对它无意义），所以整行 3×out_width 一起过。
          if (subpixel_filter_) apply_lcd_filter(row_subpixels);
          std::copy(row_subpixels.begin(), row_subpixels.end(),
                    bitmap->coverage.begin() +
                        static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y) *
                                                        static_cast<std::size_t>(out_width) *
                                                        static_cast<std::size_t>(kSubpixelColumns)));
        }
      }
    }
  }

  // **先淘汰、后插入**：反过来会在插入后清空缓存，把刚插入的位图一起销毁，
  // 于是 `return slot.get()` 返回悬垂指针（ASan 实测 heap-use-after-free）。
  const std::scoped_lock lock(cache_->mutex);
  const std::size_t entry_bytes =
      bitmap->coverage.size() * sizeof(float) + sizeof(Cache::GlyphEntry) + 32;
  trim_cache(entry_bytes);
  cache_->lru_order.push_front(key);
  cache_->glyph_bytes += entry_bytes;
  cache_->glyphs.emplace(key, Cache::GlyphEntry{bitmap, entry_bytes, cache_->lru_order.begin()});
  return bitmap;
}

auto TextRenderer::draw(raster::Surface& surface, std::string_view utf8, math::Point origin,
                        float size, math::Color color, FontRole role) const -> Status {
  if (stack_->empty() || utf8.empty() || size <= 0.0f || color.a == 0U) return ok();
  // 文本是跨模块的绘制路径（字形位图直接按覆盖率行混合），在画布上单独记一笔：
  // 否则“绘制 30ms”里看不出文字占多少（界面里文字往往是第一位的调用次数大户）。
  const bool profiling = surface.profiler() != nullptr;
  const std::int64_t profile_start = profiling ? st::time::now_ns() : 0;
  std::uint64_t profile_pixels = 0;
  const std::shared_ptr<const ShapedText> shaped_ptr = shape_cached(utf8, size, role);
  const ShapedText& shaped = *shaped_ptr;
  const float device_scale = surface.device_scale();
  const float baseline = (origin.y + shaped.ascent) * device_scale;
  const raster::Paint paint = raster::Paint::solid(color);
  const float opacity = static_cast<float>(color.a) / 255.0f;

  for (const auto& run : shaped.runs) {
    if (run.face == nullptr) continue;
    const float pixel_size = size * device_scale;
    const std::shared_ptr<const GlyphBitmap> bitmap =
        glyph_bitmap(*run.face, run.glyph, pixel_size);
    if (bitmap == nullptr || bitmap->coverage.empty()) continue;
    profile_pixels += static_cast<std::uint64_t>(bitmap->width) *
                      static_cast<std::uint64_t>(bitmap->height);
    const auto x_begin =
        static_cast<int>(std::lround((origin.x + run.x) * device_scale)) + bitmap->offset_x;
    const auto y_begin = static_cast<int>(std::lround(baseline)) + bitmap->offset_y;
    // 经**接口**提交字形覆盖率位图（而不是直接调软件内部的行混合 API）：
    // 这样同一条文字路径在 CPU 与 GPU 上都能画（GPU 把它当纹理贴一个四边形）。
    // 文字是界面里最常见的原语，若它只能走软件，GPU 渲染就名存实亡。
    //
    // `bitmap->format` 必须原样传出（而不是看渲染器当前模式）：灰度是 1 项/像素、
    // 亚像素是 3 项/像素，混合公式与纹理格式都随它变——传错就是“按错误长度解读”。
    surface.blend_coverage_bitmap(x_begin, y_begin, bitmap->coverage, bitmap->width,
                                  bitmap->height, paint, opacity, raster::BlendMode::SrcOver,
                                  bitmap->cache_key, bitmap->format);
  }
  if (profiling) {
    surface.add_profile(raster::PaintOp::Text,
                        static_cast<double>(st::time::now_ns() - profile_start) / 1'000'000.0,
                        profile_pixels);
  }
  return ok();
}

auto TextRenderer::wrap(std::string_view utf8, float size, float max_width) const
    -> std::vector<std::string_view> {
  std::vector<std::string_view> lines;
  if (utf8.empty()) {
    lines.push_back(utf8);
    return lines;
  }
  if (max_width <= 0.0f || max_width >= kUnbounded) {
    lines.push_back(utf8);
    return lines;
  }

  const std::size_t total = utf8_length(utf8);
  std::size_t line_start = 0;
  std::size_t last_break = std::string_view::npos;  // 码点索引（可断处之后）
  float pen = 0.0f;

  for (std::size_t index = 0; index < total; ++index) {
    const std::string_view slice = utf8_slice(utf8, index, 1);
    const char32_t codepoint = utf8_decode(slice).front();
    if (codepoint == U'\n') {
      lines.push_back(utf8_slice(utf8, line_start, index - line_start));
      line_start = index + 1;
      last_break = std::string_view::npos;
      pen = 0.0f;
      continue;
    }
    const float advance = measure_width(slice, size);
    if (is_break_space(codepoint)) {
      last_break = index + 1;
    } else if (is_cjk(codepoint) || (index > 0 && is_cjk(utf8_decode(utf8_slice(utf8, index - 1, 1)).front()))) {
      if (last_break == std::string_view::npos) last_break = index;
    }
    if (pen + advance > max_width && index > line_start) {
      const std::size_t break_at =
          last_break != std::string_view::npos && last_break > line_start ? last_break : index;
      lines.push_back(utf8_slice(utf8, line_start, break_at - line_start));
      line_start = break_at;
      pen = measure_width(utf8_slice(utf8, line_start, index + 1 - line_start), size);
      last_break = std::string_view::npos;
      continue;
    }
    pen += advance;
  }
  if (line_start <= total) lines.push_back(utf8_slice(utf8, line_start, total - line_start));
  return lines;
}

auto TextRenderer::ellipsize(std::string_view utf8, float size, float max_width) const
    -> std::string {
  if (utf8.empty()) return {};
  if (max_width <= 0.0f || max_width >= kUnbounded) return std::string(utf8);
  if (measure_width(utf8, size) <= max_width) return std::string(utf8);

  constexpr std::string_view kEllipsis = "…";
  const float ellipsis_width = measure_width(kEllipsis, size);
  if (ellipsis_width > max_width) return {};

  const std::size_t total = utf8_length(utf8);
  std::size_t low = 0;
  std::size_t high = total;
  while (low < high) {
    const std::size_t middle = (low + high + 1) / 2;
    const std::string_view prefix = utf8_slice(utf8, 0, middle);
    if (measure_width(prefix, size) + ellipsis_width <= max_width) {
      low = middle;
    } else {
      high = middle - 1;
    }
  }
  std::string out(utf8_slice(utf8, 0, low));
  out.append(kEllipsis);
  return out;
}

}  // namespace st::text

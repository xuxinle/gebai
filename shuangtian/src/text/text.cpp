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

void TextRenderer::set_coverage_correct(CoverageCorrect mode) noexcept {
  if (mode == coverage_correct_) return;
  coverage_correct_ = mode;
  // 与 `set_coverage_gamma` 同理：位图随映射变，旧位图留着只会白占预算。
  const std::scoped_lock lock(cache_->mutex);
  cache_->glyphs.clear();
  cache_->lru_order.clear();
  cache_->glyph_bytes = 0;
}

void TextRenderer::set_coverage_contrast(float contrast) noexcept {
  const float clamped = std::clamp(contrast, 0.0f, 1.0f);
  if (clamped == coverage_contrast_) return;
  coverage_contrast_ = clamped;
  const std::scoped_lock lock(cache_->mutex);
  cache_->glyphs.clear();
  cache_->lru_order.clear();
  cache_->glyph_bytes = 0;
}

void TextRenderer::set_coverage_gamma(float gamma) noexcept {
  const float clamped = sanitize_coverage_gamma(gamma);
  if (clamped == coverage_gamma_) return;
  coverage_gamma_ = clamped;
  // 覆盖率位图随指数变（见头文件），旧位图不能留着——留着不会出错（键已区分），
  // 但会白占最多 24 MiB 预算把当前指数下的热字形挤出去。
  const std::scoped_lock lock(cache_->mutex);
  cache_->glyphs.clear();
  cache_->lru_order.clear();
  cache_->glyph_bytes = 0;
}

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
  return glyph_bitmap(*face, *id, pixel_size, 0);
}

/// 合成加粗的步数换算：把物理像素半径换成**当前模式下的采样格步数**。
///
/// 采样格 = 一次叠填位移的最小有效量。水平方向的采样密度在两种模式下不同：
/// 灰度是 `supersample`（1 物理像素 / supersample），亚像素是 `3 × supersample`
/// （每物理像素三个子像素）。按采样格取整才能保证位移真的生效
/// （否则会被栅格化取整吃成 0——症状是「字重档位看起来没有任何区别」）。
auto TextRenderer::embolden_steps(float pixel_radius) const noexcept -> int {
  if (pixel_radius <= 0.0f) return 0;
  const int supersample = std::max(1, static_cast<int>(std::lround(supersample_)));
  const int cells_per_pixel = supersample * (subpixel_ ? kSubpixelColumns : 1);
  const int steps =
      static_cast<int>(std::lround(pixel_radius * static_cast<float>(cells_per_pixel)));
  return std::clamp(steps, 0, TextRenderer::kMaxEmboldenSteps);
}

auto TextRenderer::glyph_bitmap(const FontFace& face, GlyphId glyph, float pixel_size,
                                int embolden_steps) const -> std::shared_ptr<const GlyphBitmap> {
  const std::uint32_t size_bucket = size_key(pixel_size);
  const int steps = std::clamp(embolden_steps, 0, TextRenderer::kMaxEmboldenSteps);
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
  // 覆盖率 gamma 同样进键：它是对**同一字形**的位图做不同映射，不进键就会取到上一个指数的字
  // （症状是“改了参数却看不出变化”，直到某个字形被淘汰才“突然生效”）。
  const auto gamma_bucket = static_cast<std::uint64_t>(std::lround(coverage_gamma_ * 100.0f));
  // 校正模式与 Skia 模式的对比度也要进键：同理由——不同映射 = 不同位图。
  const auto correct_bucket =
      static_cast<std::uint64_t>(coverage_correct_) * 1000U +
      (coverage_correct_ == CoverageCorrect::Skia
           ? static_cast<std::uint64_t>(std::lround(coverage_contrast_ * 100.0f))
           : 0U);
  // 渲染模式**必须进键**：灰度与亚像素的覆盖率位图排布不同（1 项/像素 vs 3 项/像素），
  // 混用等于按错误长度解读（表现是"字缺一块"或"字整片消失"，且只在切换模式的那一刻出现）。
  const bool lcd = subpixel_;
  // 网格拟合模式同样要进键：拟合前后是两份不同的位图（边缘相位不同），
  // 与亚像素同理——混用会取到“不符合当前模式”的字形（症状是“开关看起来没生效”）。
  const auto fit_bucket = static_cast<std::uint64_t>(grid_fit_);
  // **合成加粗步数必须进键**：它是同一字形的不同笔画宽度版本，
  // 混用等于把 Regular 的位图当成 SemiBold 的（症状：“字重一会儿生效一会儿不生效”）。
  // **逐字段顺序混合**（不是“移位后 XOR 拼装”）——六层嵌套的 `mix(mix(...))` 写起来
  // 极易多一个或少一个括号（改这条链时就当场出现过：多了一层）。折成列表既不可能数错括号，
  // 也让“键里到底有哪几个字段”一眼可数——而这份字段表就是缓存正确性的全部依据。
  std::uint64_t key = st::hash::fnv1a64(face.path());
  for (const std::uint64_t field :
       {static_cast<std::uint64_t>(face.face_index()), static_cast<std::uint64_t>(glyph),
                         static_cast<std::uint64_t>(size_bucket), supersample_bucket, gamma_bucket,
                 correct_bucket, lcd ? 1ULL : 0ULL, fit_bucket,
                 static_cast<std::uint64_t>(steps)}) {
    key = mix(key, field);
  }
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
  // **降采样在 gamma 空间做**（见 `set_coverage_gamma`）：子样本平均是 gamma 空间的线性操作，
  // 若先把每个子样本拉到线性光再平均、再回 gamma 空间，两次变换的开销就落到字形栅格化的内层循环
  // （一个 CJK 字形 ss=2 时有 3×2×2 = 12 次每像素）。两种口径的差异在 α∈[0.2,0.8] 处最大约 3%，
  // 低于本校正要消的 13.6% 一个数量级，所以取便宜的那个。
  /// Skia 模式的 LUT（256 项，`[0,1]` 浮点）。逐行复刻 `skia@8643b1d`
  /// `SkTMaskGamma_build_correcting_lut`，与 `tools/skia_lut_compare.py` 同口径。
  ///
  /// 与 gama 模式的根本区别：**它是方向性的**。`dst = 1 − src` 是“对背景的猜测”，
  /// 所以黑字（src=0）走「线性反解 + 对比度补回」、白字（src=255）只走线性反解。
  /// 实测：黑字 contrast=0 时输出 31/68/119（≈ 我们的 γ=2.2）；
  /// 白字时输出 137/188/225（另一条曲线，且 contrast 完全无效）。
  const auto build_skia_lut = [](std::uint32_t argb, float contrast) noexcept
      -> std::array<float, 256> {
    const auto srgb_to_linear = [](float u) noexcept -> float {
      return u <= 0.04045f ? u / 12.92f : std::pow((u + 0.055f) / 1.055f, 2.4f);
    };
    const auto linear_to_srgb = [](float u) noexcept -> float {
      return u <= 0.0031308f ? u * 12.92f : 1.055f * std::pow(u, 1.0f / 2.4f) - 0.055f;
    };
    // 只用**绿通道**当亮度索引（SkTMaskGamma<3,3,3> 也是每通道一张小表；
    // 而覆盖率是三通道同一个标量，按亮度选表才是正确口径，见 `correct` 的说明）。
    const float src = static_cast<float>((argb >> 8) & 0xFFU) / 255.0f;
    const float lin_src = srgb_to_linear(src);
    const float dst = 1.0f - src;
    const float lin_dst = srgb_to_linear(dst);
    const float adjusted = contrast * lin_dst;   // ← 只对深字浅底生效的那一项
    std::array<float, 256> lut{};
    for (int i = 0; i < 256; ++i) {
      const float raw = static_cast<float>(i) / 255.0f;
      const float srca = raw + (1.0f - raw) * adjusted * raw;   // apply_contrast
      const float dsta = 1.0f - srca;
      const float out = linear_to_srgb(lin_src * srca + dsta * lin_dst);
      const float result = (out - dst) / (src - dst);            // 反解 blit 会做的事
      lut[static_cast<std::size_t>(i)] = std::clamp(result, 0.0f, 1.0f);
    }
    return lut;
  };
  const std::array<float, 256> skia_lut =
      coverage_correct_ == CoverageCorrect::Skia ? build_skia_lut(text_color_, coverage_contrast_)
                                                 : std::array<float, 256>{};
  const bool use_skia_lut = coverage_correct_ == CoverageCorrect::Skia;
  const auto correct = [this, &skia_lut, use_skia_lut](float value) noexcept -> float {
    if (value <= 0.0f || value >= 1.0f) return value;
    if (use_skia_lut) {
      const float scaled = value * 255.0f;
      const auto index = static_cast<std::size_t>(std::clamp(scaled, 0.0f, 255.0f));
      const std::size_t next = std::min<std::size_t>(index + 1U, 255U);
      const float frac = scaled - static_cast<float>(index);
      return skia_lut[index] * (1.0f - frac) + skia_lut[next] * frac;
    }
    if (coverage_gamma_ == 1.0f) return value;
    // 1 − (1−α)^(1/g)：黑字白底时码值 code' = (1−α)^(1/g) = linear_to_srgb(1−α)（近似）。
    // 即把“code 空间混合”的结果换成“线性空间混合”（从而更浅/更细）——方向见头文件说明。
    return 1.0f - std::pow(1.0f - value, 1.0f / coverage_gamma_);
  };
  const float scale = effective_size * static_cast<float>(supersample) / units;
  /// 宽度量化的适用上限（**物理像素**）。
  ///
  /// 量化（把宽度取到整数像素 + 放宽远边位移预算）的收益是「整根笔画落成满黑像素」——
  /// 这只在笔画本身细到无法自己盖满一个像素时才有意义。笔画粗了之后自己就有满黑像素，
  /// 量化只剩代价（墨量偏差最大 0.5px，实测 22px CJK 会从 +0.4% 升到 +3.5%）。
  ///
  /// 边界值 21 不是拍的：UI 正文最大 14 逻辑 px × 1.5 DPI = **21 物理像素**，
  /// 而 22 以上（大标题/大数字）已经不需要量化——两个区间的实测各有依据。
  constexpr float kQuantizeBelowPx = 21.0f;

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

    // 包围盒**必须在拟合之前算**：拟合会把墨迹边界推到新的整数位置，
    // 于是 `floor`/`ceil` 的结果可能差 1px——位图尺寸随“拟合开关”跳变，
    // 就是“开一下同一个字就挪了半像素”的来源。**网格稳定是硬不变式**
    // （排版与缓存都靠它），所以网格取自未拟合的轮廓。
    //
    // **位图原点必须锢定物理像素边界**（本函数里最容易被忽略的一条硬约束）。
    // 坐标处在**超采样空间**（1 物理像素 = `supersample` 单位）：若原点只是
    // `floor(bounds) - padding`，它就落在采样单位上，而 `supersample > 1` 时
    // 它多半**不是** `supersample` 的整数倍——后果有两个，都不是小事：
    // ① 输出像素 x 平均的是采样列 `[x·ss, (x+1)·ss)`，当原点错半个物理像素时
    //    这个窗口**横跨两个物理像素**，等于把墨迹向水平方向糊开；
    // ② `offset_x = min_x / supersample` 是整数截断（向零），负的奇数原点会与
    //    `floor` 差 1，位图落点再错一像素。
    // 症状：同一字形的锐度随字号**奇偶交替**（实测「三」在 21.5px 的中间调占比
    // 1.788、22.0px 只有 0.037），且与抗锯齿模式、网格拟合全都无关——
    // 所以它必须在这里修，而不是在拟合里修。
    const math::Rect bounds = transformed.flattened_bounds(0.2f);
    const int padding = 1;
    // 向下/向上对齐到 `supersample` 的整数倍（负数也按 floor 语义，不用整数截断）。
    const auto align_down = [supersample](int value) -> int {
      const int remainder = value % supersample;
      return remainder == 0 ? value : value - (remainder < 0 ? remainder + supersample : remainder);
    };
    const auto align_up = [&align_down, supersample](int value) -> int {
      const int down = align_down(value);
      return down == value ? value : down + supersample;
    };
    const int pad = padding * supersample;
    const auto min_x = align_down(static_cast<int>(std::floor(bounds.x)) - pad);
    const auto min_y = align_down(static_cast<int>(std::floor(bounds.y)) - pad);
    const auto max_x = align_up(static_cast<int>(std::ceil(bounds.right())) + pad);
    const auto max_y = align_up(static_cast<int>(std::ceil(bounds.bottom())) + pad);
    const int width = max_x - min_x;
    const int height = max_y - min_y;
    // **网格拟合**：把笔画边缘吸附到像素网格。在像素空间做（坐标变换之后、
    // 栅格化之前），且用上一步的稳定网格——拟合只改边缘相位，不改位图尺寸。
    //
    // 在**输出位图局部坐标**里拟合（先平移 `-min_x/-min_y`）：吸附格点因此锚定在
    // **位图自己的物理像素边界**上。直接在轮廓坐标里用整数格点会错位——
    // 位图原点是 `floor(bounds)-1`，与轮廓坐标的整数格点相差一个任意小数部分。
    const raster::Path local_unfitted =
        transformed.translated(static_cast<float>(-min_x), static_cast<float>(-min_y));
    const st::text::GridFitOptions fit_options{.mode = grid_fit_,
                                               // **细笔画才量化宽度**：量化的收益（根笔画落成满黑像素）只存在于
                                               // 「宽度 < 2 物理像素」的字号区间；粗笔画自己就有满黑像素，
                                               // 量化只剩墨量偏差（实测 22px CJK 从 +2.5% 升到 +3.5%）。
                                               .quantize_width = effective_size <= kQuantizeBelowPx,
                                               .grid = static_cast<float>(supersample)};
    // `fit_slant_ == 0` = 不覆盖，用头文件的默认值（不在两处各写一份常数）。
    st::text::GridFitOptions effective_options = fit_options;
    if (fit_slant_ > 0.0f) effective_options.max_edge_slant = fit_slant_;
    if (min_stem_coverage_ >= 0.0f) effective_options.min_stem_coverage = min_stem_coverage_;
    const st::text::GridFitResult fit_result = st::text::grid_fit(local_unfitted, effective_options);
    const raster::Path& fitted = fit_result.path;
    // 把“本可以对齐却没对”的漏网报出来（见 `GlyphBitmap::fit_rejected_stems`）：
    // 被拒笔画多的字形就是“同一字里粗细不一”的现场。
    bitmap->fit_rejected_stems = fit_result.rejected_stems;
    bitmap->fit_worst_rejected_shift = fit_result.worst_rejected_shift;
    bitmap->fit_stems = fit_result.vertical_stems + fit_result.horizontal_stems;
    bitmap->fit_applied = fit_result.applied;
    bitmap->fit_funnel = fit_result.funnel;
    if (width > 0 && height > 0 && width <= 4096 && height <= 4096) {
      const int out_width = std::max(1, width / supersample);
      const int out_height = std::max(1, height / supersample);
      bitmap->width = out_width;
      bitmap->height = out_height;
      bitmap->offset_x = min_x / supersample;
      bitmap->offset_y = min_y / supersample;
      // 把**采样空间原点**一并报出：它必须是 `supersample` 的整数倍，
      // 否则输出像素平均的采样窗口横跨两个物理像素（诊断字段见头文件）。
      bitmap->origin_x = min_x;
      bitmap->origin_y = min_y;
      const std::size_t output_pixels =
          static_cast<std::size_t>(out_width) * static_cast<std::size_t>(out_height);

      if (!lcd || width * kSubpixelColumns > kMaxSubpixelWidth) {
        if (lcd) {
          // 荒唐字号（子像素 scratch 会超宽）→ 退回灰度：**如实退回**，
          // 连 `format` 一起改成灰度，混合端才不会按 3 通道去读一张单通道图。
          bitmap->format = raster::CoverageFormat::Grayscale;
        }
        raster::Canvas scratch(width, height);
        raster::Paint fill = raster::Paint::solid(math::Color::rgb(255, 255, 255));
        scratch.fill_path(fitted, fill);
        // 合成加粗（同亚像素分支：位图内按采样格平移叠填）——
        // 灰度模式下 1 个采样格 = 1/supersample 物理像素。
        for (int step = 1; step <= steps; ++step) {
          scratch.fill_path(fitted.translated(static_cast<float>(step), 0.0f), fill);
        }
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
                samples > 0 ? correct(total / static_cast<float>(samples)) : 0.0f;
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
        // **只能水平放大**：用 `build_path(horizontal=3)` 重新构一次，
        // 而不是 `Path::scaled(3)`——后者会把 x 与 y **一起**乘 3，
        // 于是字形被纵向拉到 3 倍高、只有上半部分落在画布里
        // （症状：文字像被切成两半。实测就踩到了，靠 1:1 截图对比定位）。
        // 只因为“反正是同一个轮廓”而换成 scaled 是错的：那两个函数的语义不同。
        raster::Path local = build_path(static_cast<float>(kSubpixelColumns));
        // 网格拟合的位移是**灰度口径**的，水平放大后同步 ×3；竖直方向不动。
        // （不这么做的话，亚像素路径会把拟合后的轮廓直接放大，
        //   位移也跟着被乘 3——相位就完全错了。）
        if (grid_fit_ != GridFitMode::Off) {
          const auto base_points = local_unfitted.raw_points();
          const auto fitted_points = fitted.raw_points();
          if (base_points.size() == fitted_points.size() &&
              fitted_points.size() == local.raw_points().size()) {
            // `fit_dx` 是**位图局部**位移（拟合在局部坐标里做的），与轮廓坐标
            // 只差平移：delta 逐点相减后平移量自然消去。水平放大 3 倍同步。
            const float horizontal = static_cast<float>(kSubpixelColumns);
            auto scaled_points = local.raw_points();
            for (std::size_t index = 0; index < scaled_points.size(); ++index) {
              scaled_points[index].x +=
                  (fitted_points[index].x - base_points[index].x) * horizontal;
              scaled_points[index].y += fitted_points[index].y - base_points[index].y;
            }
            (void)local.set_raw_points(scaled_points);
          }
        }
        local = local.translated(static_cast<float>(-min_x * kSubpixelColumns),
                                 static_cast<float>(-min_y));
        // **合成加粗**：把同一份轮廓沿水平正方向按**采样格**平移后重复填充。
        //
        // 为什么在**位图内**做而不是在贴图外层叠绘：位图的采样格固定（1 个 scratch 列
        // = 1 个子像素的 supersample 分之一），位移落在格点上、结果精确可缓存；
        // 而且**灰度与亚像素两条分支各做一次**，不会出现「亚像素下生效、
        // 灰度下被取整吃成 0」这类静默失效。
        scratch.fill_path(local, raster::Paint::solid(math::Color::rgb(255, 255, 255)));
        for (int step = 1; step <= steps; ++step) {
          const float shift = static_cast<float>(step);  // 采样格
          scratch.fill_path(
              local.translated(shift, 0.0f),
              raster::Paint::solid(math::Color::rgb(255, 255, 255)));
        }

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
          // **gamma 校正整像素施加，且用同一个标量**（见下方说明）。
          // 标量取三通道均值而不是逐通道各自映射：逐通道会把彩边信息“各自拉直”，
          // 破坏“三通道覆盖率按同一比例缩放”的前提（而那个前提正是本模式的物理含义——
          // 一个像素只有一个几何覆盖率，R/G/B 只是同一条边的三种采样）。
          if (coverage_gamma_ != 1.0f || use_skia_lut) {
            for (int x = 0; x < out_width; ++x) {
              const std::size_t base = static_cast<std::size_t>(x) * kSubpixelColumns;
              const float mean = (row_subpixels[base] + row_subpixels[base + 1U] +
                                  row_subpixels[base + 2U]) /
                                 3.0f;
              const float gain =
                  mean > 0.0f ? correct(mean) / mean : 0.0f;  // 0 → 保持全 0
              for (int channel = 0; channel < kSubpixelColumns; ++channel) {
                row_subpixels[base + static_cast<std::size_t>(channel)] *= gain;
              }
            }
          }
          // 滤波在**子像素轴**上做（像素边界对它无意义），所以整行 3×out_width 一起过。
          // 顺序：先 gamma 后滤波（不是相反）——滤波是“把墨在相邻子像素间摊开”的物理过程，
          // 发生在我们已经决定每个子像素该多黑的**之后**；反过来做会先把彩边摊平再映射，
          // 得到的不是任何一个真实混合空间的结果。
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
                        float size, math::Color color, FontRole role, float embolden) const
    -> Status {
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
  // 半径 → **采样格步数**：换算依赖当前渲染模式（亚像素/灰度）与超采样倍率，
  // 而这两种状态都只存在于渲染器内部——调用方无从自算，所以放在这里。
  const int embolden_steps = this->embolden_steps(embolden);

  // **合成加粗已在字形位图内部完成**（见 `glyph_bitmap` 的两条分支）：
  // 同一份轮廓沿水平正方向按**采样格**平移后重复填充，再一起栅格化。
  //
  // 为什么不在贴图时叠绘多次：那会让**灰度模式静默失效**（位移被栅格化取整吃成 0，
  // 表现为「字重档位完全一样」），而且每条文本要多次跨界提交。位图内做则两条模式
  // 各做一次、位移精确落在格点上，代价只是缓存多一份带加粗的条目
  // （缓存键含步数，不会与 Regular 的位图混用）。
  for (const auto& run : shaped.runs) {
    if (run.face == nullptr) continue;
    const float pixel_size = size * device_scale;
    const std::shared_ptr<const GlyphBitmap> bitmap =
        glyph_bitmap(*run.face, run.glyph, pixel_size, embolden_steps);
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

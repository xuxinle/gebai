#include "st/text/text.hpp"

#include <algorithm>
#include <cmath>
#include <format>
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
  /// 字形位图缓存：值用 `shared_ptr` 而非 `unique_ptr`——淘汰时仍持有该位图的调用方不受影响
  /// （曾因"插入后清空缓存 + 返回裸指针"导致 heap-use-after-free，ASan 实测定位）。
  std::unordered_map<std::uint64_t, std::shared_ptr<const GlyphBitmap>> glyphs{};
  std::unordered_map<std::uint64_t, float> widths{};  ///< 文本宽度缓存（键：文本 + 字号档）
  static constexpr std::size_t kMaxEntries = 512;
};

TextRenderer::TextRenderer(const FontStack& stack, float supersample)
    : stack_(&stack), supersample_(supersample < 1.0f ? 1.0f : supersample),
      cache_(std::make_unique<Cache>()) {}

TextRenderer::~TextRenderer() = default;

void TextRenderer::set_supersample(float factor) {
  supersample_ = factor < 1.0f ? 1.0f : factor;
  const std::scoped_lock lock(cache_->mutex);
  cache_->glyphs.clear();
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
  const float width = shape(utf8, size, role).width;
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

/// 淘汰超出上限的条目（**调用方须持有锁**，且在插入之前调用）。
void TextRenderer::trim_cache() const {
  if (cache_->glyphs.size() <= Cache::kMaxEntries) return;
  cache_->glyphs.clear();
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
  const std::uint64_t key = mix(mix(mix(mix(st::hash::fnv1a64(face.path()),
                                            static_cast<std::uint64_t>(face.face_index())),
                                        static_cast<std::uint64_t>(glyph)),
                                    static_cast<std::uint64_t>(size_bucket)),
                                supersample_bucket);
  {
    const std::scoped_lock lock(cache_->mutex);
    if (const auto iterator = cache_->glyphs.find(key); iterator != cache_->glyphs.end()) {
      return iterator->second;
    }
  }

  const FontMetrics& metrics = face.metrics();
  const float units = metrics.units_per_em > 0.0f ? metrics.units_per_em : 1000.0f;
  const float effective_size = static_cast<float>(size_bucket) / 4.0f;
  const int supersample = std::max(1, static_cast<int>(std::lround(supersample_)));
  const float scale = effective_size * static_cast<float>(supersample) / units;

  auto bitmap = std::make_shared<GlyphBitmap>();
  auto outline = face.glyph_outline(glyph);
  const bool blank = !outline || outline->is_empty();

  if (!blank) {
    // 字体单位（y 向上）→ 物理像素（y 向下，笔位为原点）
    raster::Path transformed;
    const auto map_point = [scale](math::Point point) noexcept -> math::Point {
      return math::Point{point.x * scale, -point.y * scale};
    };
    for (const auto& command : outline->commands()) {
      switch (command.kind) {
        case raster::PathCommand::Kind::MoveTo:
          transformed.move_to(map_point(command.p1));
          break;
        case raster::PathCommand::Kind::LineTo:
          transformed.line_to(map_point(command.p1));
          break;
        case raster::PathCommand::Kind::QuadTo:
          transformed.quad_to(map_point(command.p1), map_point(command.p2));
          break;
        case raster::PathCommand::Kind::CubicTo:
          transformed.cubic_to(map_point(command.p1), map_point(command.p2), map_point(command.p3));
          break;
        case raster::PathCommand::Kind::Close:
          transformed.close();
          break;
      }
    }

    const math::Rect bounds = transformed.flattened_bounds(0.2f);
    const int padding = 1;
    const auto min_x = static_cast<int>(std::floor(bounds.x)) - padding;
    const auto min_y = static_cast<int>(std::floor(bounds.y)) - padding;
    const auto max_x = static_cast<int>(std::ceil(bounds.right())) + padding;
    const auto max_y = static_cast<int>(std::ceil(bounds.bottom())) + padding;
    const int width = max_x - min_x;
    const int height = max_y - min_y;
    if (width > 0 && height > 0 && width <= 4096 && height <= 4096) {
      raster::Canvas scratch(width, height);
      raster::Path local =
          transformed.translated(static_cast<float>(-min_x), static_cast<float>(-min_y));
      scratch.fill_path(local, raster::Paint::solid(math::Color::rgb(255, 255, 255)));

      const int out_width = std::max(1, width / supersample);
      const int out_height = std::max(1, height / supersample);
      bitmap->width = out_width;
      bitmap->height = out_height;
      bitmap->offset_x = min_x / supersample;
      bitmap->offset_y = min_y / supersample;
      bitmap->coverage.assign(
          static_cast<std::size_t>(out_width) * static_cast<std::size_t>(out_height), 0.0f);

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
    }
  }

  // **先淘汰、后插入**：反过来会在插入后清空缓存，把刚插入的位图一起销毁，
  // 于是 `return slot.get()` 返回悬垂指针（ASan 实测 heap-use-after-free）。
  const std::scoped_lock lock(cache_->mutex);
  trim_cache();
  cache_->glyphs[key] = bitmap;
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
  const ShapedText shaped = shape(utf8, size, role);
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
    // 这样同一条文字路径在 CPU 与 GPU 上都能画（GPU 把它当 A8 纹理贴）。
    // 文字是界面里最常见的原语，若它只能走软件，GPU 渲染就名存实亡。
    surface.blend_coverage_bitmap(x_begin, y_begin, bitmap->coverage, bitmap->width,
                                  bitmap->height, paint, opacity, raster::BlendMode::SrcOver);
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

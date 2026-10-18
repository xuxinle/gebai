/// 场景渲染引擎（见 `motion.hpp`）。
///
/// 排版常数全部在 1920×1080 设计坐标里；`Painter` 负责把它们换算到目标画布的物理像素。
///
/// 全文件统一用 **float**（设计坐标本就是像素量级，float 有 7 位有效数字，够）：
/// 混用 double 会触发 `-Wdouble-promotion`（本仓 `-Werror`），而逐个 `static_cast`
/// 只会把表达式写成一团——统一量纲比逐点转换可读得多。

#include "motion.hpp"

#include "assets_lib.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/time.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"

namespace caiyun {

namespace {

using st::math::Color;
using st::raster::Canvas;
using st::raster::Paint;
using st::raster::Path;
using st::ui::TextPort;

constexpr float kPi = 3.14159265f;

/// 背景合成缓冲（半分辨率；见 render_scene 里的说明）。
///
/// 为什么是**静态缓冲**而不是每次现建：每帧建一个 960×540 的画布要走一次分配
/// （~2MB），300 帧就是 600MB 的反复分配/释放；静态缓冲让这件事归零。
/// 它只在渲染线程/主线程各自的调用里用到，且**每帧被 clear**——不跨帧保留内容。
thread_local st::raster::Canvas backdrop_canvas{2, 2, 1.0f};

[[nodiscard]] auto clamp01f(float value) -> float {
  return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}
[[nodiscard]] auto mix_color(Color a, Color b, float t) -> Color {
  const auto channel = [t](std::uint8_t from, std::uint8_t to) {
    const float value =
        static_cast<float>(from) + (static_cast<float>(to) - static_cast<float>(from)) * t;
    return static_cast<std::uint8_t>(std::clamp(value, 0.0f, 255.0f));
  };
  return Color{channel(a.r, b.r), channel(a.g, b.g), channel(a.b, b.b), channel(a.a, b.a)};
}
[[nodiscard]] auto with_alpha(Color color, float alpha) -> Color {
  const float scaled = std::clamp(static_cast<float>(color.a) * alpha, 0.0f, 255.0f);
  color.a = static_cast<std::uint8_t>(scaled);
  return color;
}

/// 按 UTF-8 码点切分（不能按字节切：中文一个字三字节，切错就是乱码方框）。
[[nodiscard]] auto split_utf8(std::string_view text) -> std::vector<std::string_view> {
  std::vector<std::string_view> glyphs;
  for (std::size_t i = 0; i < text.size();) {
    const auto byte = static_cast<unsigned char>(text[i]);
    std::size_t length = 1;
    if ((byte & 0x80U) == 0x00U) {
      length = 1;
    } else if ((byte & 0xE0U) == 0xC0U) {
      length = 2;
    } else if ((byte & 0xF0U) == 0xE0U) {
      length = 3;
    } else {
      length = 4;
    }
    length = std::min(length, text.size() - i);
    glyphs.push_back(text.substr(i, length));
    i += length;
  }
  return glyphs;
}

/// 设计坐标 → 画布的画笔包装（唯一的坐标换算点）。
///
/// 为什么不让各模板自己乘 `scale`：五个模板 × 十几种绘制调用，漏乘一处就是
/// "预览对、成片错位"这类最难查的差异（且只在导出时才现形）。
class Painter {
 public:
  Painter(Canvas& canvas, const TextPort* port)
      : canvas_(canvas), port_(port),
        scale_(static_cast<float>(canvas.physical_width()) / kDesignWidth),
        width_(static_cast<float>(canvas.physical_width())),
        height_(static_cast<float>(canvas.physical_height())) {}

  [[nodiscard]] auto scale() const noexcept -> float { return scale_; }
  [[nodiscard]] auto width() const noexcept -> float { return width_; }
  [[nodiscard]] auto height() const noexcept -> float { return height_; }
  [[nodiscard]] auto port() const noexcept -> const TextPort* { return port_; }

  [[nodiscard]] auto rect(float x, float y, float w, float h) const -> st::math::Rect {
    return st::math::Rect{x * scale_, y * scale_, w * scale_, h * scale_};
  }
  [[nodiscard]] auto point(float x, float y) const -> st::math::Point {
    return st::math::Point{x * scale_, y * scale_};
  }
  [[nodiscard]] auto design(float value) const noexcept -> float { return value * scale_; }

  void clear(Color color) { canvas_.clear(color); }
  void fill(float x, float y, float w, float h, Color color, float radius = 0.0f) {
    canvas_.fill_rect(rect(x, y, w, h), Paint::solid(color), design(radius));
  }
  void circle(float cx, float cy, float r, Color color) {
    canvas_.fill_circle(point(cx, cy), design(r), Paint::solid(color));
  }
  void stroke_circle(float cx, float cy, float r, Color color, float width) {
    const Path ring = st::raster::make_circle(point(cx, cy), design(r));
    canvas_.stroke_path(ring, Paint::solid(color), design(width));
  }
  void line(float x1, float y1, float x2, float y2, Color color, float width) {
    const Path path = st::raster::make_line(point(x1, y1), point(x2, y2));
    const st::raster::StrokeStyle style{.cap = st::raster::LineCap::Round,
                                        .join = st::raster::LineJoin::Round};
    canvas_.stroke_path(path, Paint::solid(color), design(width), style);
  }
  /// 圆角矩形描边环（`make_rounded_border_ring`：比 `stroke_path` 几何量少一半）。
  void border(float x, float y, float w, float h, float radius, Color color, float width) {
    const Path ring =
        st::raster::make_rounded_border_ring(rect(x, y, w, h), design(radius), design(width));
    canvas_.fill_path(ring, Paint::solid(color));
  }

  /// 排版：按**设计字号**量宽（返回设计坐标下的宽度）。
  [[nodiscard]] auto text_width(std::string_view text, float size, bool mono = false) const
      -> float {
    if (port_ == nullptr || text.empty()) return 0.0f;
    const float width = port_->measure_width(text, design(size),
                                             mono ? st::text::FontRole::Monospace
                                                  : st::text::FontRole::Proportional);
    return width / scale_;
  }

  /// 画一行文字：`anchor` 0=左 0.5=居中 1=右；`y` 是**基线**的 y（设计坐标）。
  ///
  /// 为什么要做基线换算：`TextPort::draw` 的 `origin` 是**行盒左上角**，而排版
  /// 位置几乎总是按基线想的（"标题坐在 y=470 这条线上"）。不换算的话每处都会
  /// 系统性偏低一个 `ascent`（≈0.93×字号）——大标题上就是 100+px 的错位。
  void text(std::string_view content, float size, float x, float y, Color color,
            float anchor = 0.0f, bool bold = false, bool mono = false) {
    if (port_ == nullptr || content.empty()) return;
    const float advance = text_width(content, size, mono);
    const float left = x - advance * anchor;
    const float top = y - port_->ascent(design(size)) / scale_;
    port_->draw(canvas_, content, point(left, top), design(size), color,
                mono ? st::text::FontRole::Monospace : st::text::FontRole::Proportional,
                bold ? design(size) / 20.0f : 0.0f, bold);
  }

  /// 带字距的文本总宽（与 `spaced_text` 同一口径——两者分开写必然漂移）。
  [[nodiscard]] auto spaced_width(std::string_view content, float size, float tracking) const
      -> float {
    const auto glyphs = split_utf8(content);
    float total = 0.0f;
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
      total += text_width(glyphs[i], size);
      if (i + 1 < glyphs.size()) total += tracking;
    }
    return total;
  }

  /// 带**字距**的文字（大标题用；CJK 逐字加距是"高级感"最省力的一招）。
  /// 返回总宽（设计坐标）。
  auto spaced_text(std::string_view content, float size, float x, float baseline, Color color,
                   float tracking) -> float {
    if (port_ == nullptr || content.empty()) return 0.0f;
    const auto glyphs = split_utf8(content);
    const float top = baseline - port_->ascent(design(size)) / scale_;
    float cursor = x;
    float total = 0.0f;
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
      port_->draw(canvas_, glyphs[i], point(cursor, top), design(size), color);
      const float advance = text_width(glyphs[i], size);
      const float gap = (i + 1 < glyphs.size()) ? tracking : 0.0f;
      cursor += advance + gap;
      total += advance + gap;
    }
    return total;
  }

  /// 垂直渐变底（**真正的线性渐变**）。
  ///
  /// 依据（`tools/gradient_fill_probe.cpp` 实测，1080p）：整幅**线性**渐变 0.6ms/帧
  /// （与纯色 0.44ms 同量级），而整幅**径向**渐变 97ms（逐像素算距离，223×）。
  /// 所以“渐变贵”这个直觉只对径向成立——线性渐变可以放心用。
  void vertical_gradient(Color top, Color bottom) {
    st::raster::Gradient gradient = st::raster::Gradient::linear(
        st::math::Point{0.0f, 0.0f}, st::math::Point{0.0f, height_},
        {st::raster::GradientStop{0.0f, top}, st::raster::GradientStop{1.0f, bottom}});
    canvas_.fill_rect(st::math::Rect{0.0f, 0.0f, width_, height_},
                      Paint::with_gradient(std::move(gradient)));
  }

  /// 坚向铺光（**线性、竖直**，整幅一遍）。
  ///
  /// 为何限定“竖直”：本框架的光栅化器对**竖向**线性渐变有逐行 SIMD 快路径
  /// （整行同色 → `simd::blend_row`），实测 1080p 整幅 0.6ms；而**倾斜/横向**且
  /// 带 alpha 的线性渐变落到逐像素递推 + 标量混合，实测 66ms（110×，见
  /// `tools/gradient_fill_probe.cpp`）。背景铺光是低频内容，竖直与斜向在观感上
  /// 几乎无差——选便宜的。
  void wash_vertical(Color from_color, Color to_color) {
    st::raster::Gradient gradient = st::raster::Gradient::linear(
        st::math::Point{0.0f, 0.0f}, st::math::Point{0.0f, height_},
        {st::raster::GradientStop{0.0f, from_color}, st::raster::GradientStop{1.0f, to_color}});
    canvas_.fill_rect(st::math::Rect{0.0f, 0.0f, width_, height_},
                      Paint::with_gradient(std::move(gradient)));
  }

  /// 径向柔光（**只在必须“圆形光斑”的地方用**：背景中心光斑、片尾标志光晕）。
  ///
  /// 成本：包围盒内逐像素算距离（1080p 全屏量级 98ms，见 `gradient_fill_probe`）。
  /// 所以它的包裹盒必须**远小于画布**；大面积层一律用 `wash_vertical`。
  void glow(float cx, float cy, float radius, Color color, float strength) {
    if (strength <= 0.002f || radius <= 0.5f) return;
    const float px = cx * scale_;
    const float py = cy * scale_;
    const st::math::Rect box =
        st::math::Rect{px - radius, py - radius, radius * 2.0f, radius * 2.0f}
            .intersect(st::math::Rect{0.0f, 0.0f, width_, height_});
    if (box.is_empty()) return;
    st::raster::Gradient gradient = st::raster::Gradient::radial(
        point(cx, cy), radius,
        {st::raster::GradientStop{0.0f, with_alpha(color, strength)},
         st::raster::GradientStop{0.45f, with_alpha(color, strength * 0.35f)},
         st::raster::GradientStop{1.0f, with_alpha(color, 0.0f)}});
    canvas_.fill_rect(box, Paint::with_gradient(std::move(gradient)));
  }

  /// 暗角（四边压暗，把视线收到中心）。
  ///
  /// 用四条**渐变带**（每条一次 fill_rect，带高 120 设计像素），而不是 N 层实心叠色：
  /// 多层实心在 1080p 上是百万次像素混合，而渐变带只需 4 次、过渡也更均匀。
  void vignette(float strength) {
    if (strength <= 0.002f) return;
    const float band = design(120.0f) * (0.6f + 0.4f * strength);
    const Color edge = with_alpha(Color{0, 0, 0, 255}, strength * 0.55f);
    const Color clear = with_alpha(Color{0, 0, 0, 255}, 0.0f);
    const auto band_fill = [this](st::math::Rect box, st::math::Point from,
                                  st::math::Point to, Color from_color, Color to_color) {
      st::raster::Gradient gradient =
          st::raster::Gradient::linear(from, to, {st::raster::GradientStop{0.0f, from_color},
                                                  st::raster::GradientStop{1.0f, to_color}});
      canvas_.fill_rect(box, Paint::with_gradient(std::move(gradient)));
    };
    const float w = width_;
    const float h = height_;
    band_fill(st::math::Rect{0.0f, 0.0f, w, band}, st::math::Point{0.0f, 0.0f},
              st::math::Point{0.0f, band}, edge, clear);
    band_fill(st::math::Rect{0.0f, h - band, w, band}, st::math::Point{0.0f, h},
              st::math::Point{0.0f, h - band}, edge, clear);
    band_fill(st::math::Rect{0.0f, 0.0f, band, h}, st::math::Point{0.0f, 0.0f},
              st::math::Point{band, 0.0f}, edge, clear);
    band_fill(st::math::Rect{w - band, 0.0f, band, h}, st::math::Point{w, 0.0f},
              st::math::Point{w - band, 0.0f}, edge, clear);
  }
 public:
  /// 素材绘制用：`blit_cover`/`blit_contain` 是自由函数（在 Painter 外部），
  /// 需要 `Canvas&`。不给 getter 就只能靠"再开一条绘制路径"——两条路径必漂移。
  [[nodiscard]] auto canvas() -> Canvas& { return canvas_; }
  [[nodiscard]] auto design_w() const noexcept -> float { return kDesignWidth; }
  [[nodiscard]] auto design_h() const noexcept -> float { return kDesignHeight; }

 private:
  Canvas& canvas_;
  const TextPort* port_;


  float scale_;
  float width_;
  float height_;
};

/// 背景图（素材）：铺满全画面 + 压暗 + 缓慢推拉（Ken Burns）。
///
/// 为何用 cover 而非 stretch：素材长宽比与 16:9 几乎不会一致，拉伸会把
/// 人脸/产品拉变形（一眼假）。cover 裁掉多余部分，永远是“填满且不变形”。
///
/// 为何要压暗（`image_dim`）：白字压在亮图上根本读不出来。压暗不是“滤镜”，
/// 是**可读性前提**——所以它不是可选装饰，而是素材背景的默认行为。
///
/// 为何要 Ken Burns（`image_ken_burns`）：完全静止的图在视频里显得像幻灯片；
/// 缓慢推（6% 幅度，整段线性）就足以让它“呼吸”起来，且不会引人注意。
void paint_background_image(Painter& p, const Scene& scene, float local_time, float duration,
                            const AssetLibrary& assets, std::string_view basis) {
  if (scene.image.empty()) return;
  const auto path = assets.resolve(scene.image, basis);
  const float w = p.design_w();
  const float h = p.design_h();
  if (!path) {
    // **占位框**：不静默跳过——否则"图没进来"看起来像设计就是没图。
    p.fill(0.0f, 0.0f, w, h, Color{40, 20, 24, 255});
    p.text("素材缺失", 48.0f, w * 0.5f, h * 0.5f, Color{255, 180, 180, 255});
    return;
  }
  const auto image = assets.image(*path);   // shared_ptr
  if (!image) {
    p.fill(0.0f, 0.0f, w, h, Color{40, 20, 24, 255});
    p.text("素材无法解码", 48.0f, w * 0.5f, h * 0.5f, Color{255, 180, 180, 255});
    return;
  }
  // Ken Burns：整段内从 1.0 推到 (1 + ken_burns)，以画面中心为锚点。
  const float phase = duration > 0.01f ? std::clamp(local_time / duration, 0.0f, 1.0f) : 0.0f;
  const float grow = 1.0f + std::max(0.0f, scene.image_ken_burns) * phase;
  const float draw_w = w * grow;
  const float draw_h = h * grow;
  const st::math::Rect area{(w - draw_w) * 0.5f, (h - draw_h) * 0.5f, draw_w, draw_h};
  blit_cover(p.canvas(), *image, area, 1.0f, 0.0f);
  // 压暗（纯色叠层；alpha 由 `image_dim` 控）。
  const float dim = std::clamp(scene.image_dim, 0.0f, 1.0f);
  if (dim > 0.001f) {
    p.fill(area.x, area.y, area.width, area.height,
           Color{6, 9, 18, static_cast<std::uint8_t>(dim * 255.0f)});
  }
}

/// 卡片插图（素材）：在给定设计坐标框内 contain 贴图（完整可见，不裁）。
void paint_card_image(Painter& p, const Scene& scene, st::math::Rect area,
                      const AssetLibrary& assets, std::string_view basis) {
  if (scene.image_card.empty()) return;
  const auto path = assets.resolve(scene.image_card, basis);
  if (!path) {
    p.fill(area.x, area.y, area.width, area.height, Color{40, 20, 24, 255});
    return;
  }
  const auto image = assets.image(*path);
  if (!image) {
    p.fill(area.x, area.y, area.width, area.height, Color{40, 20, 24, 255});
    return;
  }
  blit_contain(p.canvas(), *image, area, 1.0f);
}

/// 背景：底色渐变 + 两束缓慢漂移的彩色铺光 + 暗角。
///
/// 三层的分工：底色给基调，铺光给"光从哪来"，暗角把视线收回中心。漂移量很小
/// （相位 0→1 只挪 6% 画宽），是为了让静止画面不"死"，而不是做运动。
void paint_backdrop(Painter& p, const Scene& scene, float local_time, float duration) {
  // 底色（**线性**渐变，整幅 0.6ms：与纯色同量级，见 gradient_fill_probe）。
  p.vertical_gradient(scene.bg_top, scene.bg_bottom);
  const float phase = duration > 0.01f ? local_time / duration : 0.0f;
  const float drift = 0.5f - 0.5f * std::cos(phase * kPi);   // 0 → 1 → 0
  // 两束**竖向铺光**：一束自上压下、一束自下而上，强弱跟着相位呼吸。
  // 只用竖向（原因见 `wash_vertical`）——斜向/径向在本框架里都是逐像素慢路径。
  p.wash_vertical(with_alpha(scene.accent, 0.26f + 0.08f * (1.0f - drift)),
                  with_alpha(scene.accent, 0.0f));
  p.wash_vertical(with_alpha(scene.accent, 0.0f),
                  with_alpha(mix_color(scene.accent, Color{255, 255, 255, 255}, 0.5f),
                             0.14f + 0.05f * (1.0f - drift)));
  // **不加全幅径向光斑**：实测它的包裹盒（直径 1340px）单帧就要 84ms——径向渐变
  // 在本框架里是逐像素算距离（47 ns/px，见 `tools/gradient_fill_probe.cpp`）。
  // 光斑留给小半径场合（片尾标志），背景层只用竖向渐变。
  p.vignette(0.9f);
}
/// 卡片（三联卡/终端共用）：半透明底 + 低透明描边 + 柔和投影。
void paint_card(Painter& p, float x, float y, float w, float h, float radius, Color fill_color,
                Color border_color, float shadow_alpha) {
  if (shadow_alpha > 0.002f) {
    p.fill(x + 6.0f, y + 14.0f, w, h, with_alpha(Color{0, 0, 0, 255}, shadow_alpha * 0.5f), radius);
  }
  p.fill(x, y, w, h, fill_color, radius);
  p.border(x, y, w, h, radius, border_color, 1.4f);
}

/// 片头：大标题升起 + 强调细线展开 + 副标题与两行说明错峰。
void paint_title(Painter& p, const Scene& scene, float local_time) {
  const float enter = ease_out_expo(clamp01f(local_time / 0.9f));
  const float rise = (1.0f - enter) * 64.0f;
  const float center_x = kDesignWidth * 0.5f;
  const float title_size = 188.0f;

  // 标题：整块升起 + 逐字加距（字距是 CJK 大标题最省力的"高级感"来源）。
  const float tracking = title_size * 0.10f;
  const float spaced = p.spaced_width(scene.title, title_size, tracking);
  const float title_baseline = 470.0f + rise;
  p.spaced_text(scene.title, title_size, center_x - spaced * 0.5f, title_baseline,
                with_alpha(Color{245, 248, 255, 255}, clamp01f(enter * 1.15f)), tracking);

  // 强调线：从中心向两侧展开（入场后 0.35s 开始）。
  const float line_t = ease_out_expo(clamp01f((local_time - 0.35f) / 0.7f));
  const float line_width = 420.0f * line_t;
  if (line_width > 1.0f) {
    p.line(center_x - line_width, 536.0f, center_x + line_width, 536.0f,
           with_alpha(scene.accent, 0.85f), 3.0f);
  }

  // 英文小标。
  const float sub_t = ease_out_cubic(clamp01f((local_time - 0.30f) / 0.8f));
  p.text(scene.subtitle, 40.0f, center_x, 610.0f + (1.0f - sub_t) * 22.0f,
         with_alpha(mix_color(scene.accent, Color{255, 255, 255, 255}, 0.55f), sub_t * 1.05f), 0.5f);

  // 两行说明：错峰上浮。
  for (std::size_t i = 0; i < scene.lines.size() && i < 3; ++i) {
    const float index = static_cast<float>(i);
    const float t = ease_out_cubic(clamp01f((local_time - 0.55f - 0.16f * index) / 0.9f));
    p.text(scene.lines[i], 46.0f, center_x, 720.0f + index * 68.0f + (1.0f - t) * 26.0f,
           with_alpha(Color{196, 210, 236, 255}, t * 0.95f), 0.5f);
  }
}

/// 终端：深色代码卡 + 逐字符敲出 + 光标闪烁 + 标题条（三个圆点）。
void paint_terminal(Painter& p, const Scene& scene, float local_time, float duration) {
  const float enter = ease_out_expo(clamp01f(local_time / 0.7f));
  const float card_x = 300.0f;
  const float card_y = 236.0f + (1.0f - enter) * 90.0f;

  const float card_w = 1320.0f;
  // 高度按**全部行**预留（不随敲字增长）：卡片高度一帧一变会让整个卡片"抽动"，
  // 而预留底部空白在敲字过程里反而像“待写入的区域”。留白控制在约一行高。
  const float card_h = 168.0f + 76.0f * static_cast<float>(std::max<std::size_t>(1, scene.lines.size()));

  paint_card(p, card_x, card_y, card_w, card_h, 28.0f,
             with_alpha(Color{12, 16, 22, 255}, 0.92f * enter),
             with_alpha(scene.accent, 0.35f * enter), 0.55f * enter);

  // 标题条：三个圆点 + 文件名。
  const float bar_y = card_y + 46.0f;
  const Color dot_colors[] = {Color{255, 96, 92, 255}, Color{255, 196, 86, 255},
                              Color{86, 208, 120, 255}};
  for (int i = 0; i < 3; ++i) {
    p.circle(card_x + 44.0f + static_cast<float>(i) * 30.0f, bar_y, 8.0f * enter,
             with_alpha(dot_colors[i], 0.9f * enter));
  }
  p.text(scene.title, 30.0f, card_x + 160.0f, bar_y + 10.0f,
         with_alpha(Color{170, 186, 210, 255}, 0.9f * enter));
  p.fill(card_x + 30.0f, bar_y + 34.0f, card_w - 60.0f, 1.6f,
         with_alpha(Color{255, 255, 255, 255}, 0.10f * enter));

  // 逐字符敲出：整体铺满场景的**前 55%**（固定每字 0.035s 会超出短场景的时长，
  // 于是"打完才出现的副标题"永远不出现——实测就是这么漏的）。
  std::size_t total_chars = 0;
  for (const std::string& line : scene.lines) total_chars += line.size();
  const float typing_span =
      std::clamp(0.03f * static_cast<float>(std::max<std::size_t>(1, total_chars)), 0.55f,
                 std::max(0.55f, duration * 0.55f));
  float typed = clamp01f((local_time - 0.45f) / typing_span);
  std::size_t budget = static_cast<std::size_t>(typed * static_cast<float>(total_chars));

  float cursor_y = card_y + 140.0f;
  float cursor_x = card_x + 56.0f;
  for (std::size_t i = 0; i < scene.lines.size(); ++i) {
    const std::string& line = scene.lines[i];
    std::size_t taken = std::min(budget, line.size());
    // 按 UTF-8 边界回退（切在半个汉字上会画出方框）。
    while (taken > 0 && taken < line.size() &&
           (static_cast<unsigned char>(line[taken]) & 0xC0U) == 0x80U) {
      --taken;
    }
    const std::string fragment = line.substr(0, taken);
    if (!fragment.empty()) {
      p.text(fragment, 38.0f, card_x + 56.0f, cursor_y + 34.0f, Color{214, 232, 255, 255}, 0.0f,
             false, true);
    }
    cursor_x = card_x + 56.0f + p.text_width(fragment, 38.0f, true) + 8.0f;
    // 右侧行号（低透明大数字：终端卡的"层次"，也让空行不显得空）。
    p.text(std::format("{:02}", i + 1), 30.0f, card_x + card_w - 70.0f, cursor_y + 34.0f,
           with_alpha(Color{255, 255, 255, 255}, 0.12f), 1.0f, false, true);
    if (taken < line.size()) break;
    budget -= taken;
    cursor_y += 76.0f;
  }

  // 光标：闪烁（1.0s 周期，占空 55%）。
  if (typed > 0.0f && std::fmod(local_time, 1.0f) < 0.55f) {
    p.fill(cursor_x, cursor_y + 8.0f, 16.0f, 40.0f, with_alpha(scene.accent, 0.9f));
  }

  // 底部说明（打完才出现）。
  const float foot = ease_out_cubic(clamp01f((typed - 0.85f) / 0.15f));
  p.text(scene.subtitle, 40.0f, kDesignWidth * 0.5f, 920.0f,
         with_alpha(Color{200, 214, 238, 255}, foot * 0.95f), 0.5f);
}

/// 三联卡：三张卡错峰升入，每张有图标（几何画）+ 标签 + 一句说明。
///
/// 设了 `image_card` 时用**素材图**代替几何图标（图文卡片）；否则仍是圆环+内点。
void paint_triptych(Painter& p, const Scene& scene, float local_time,
                    const AssetLibrary* assets, std::string_view basis) {
  const float head = ease_out_cubic(clamp01f(local_time / 0.8f));
  p.text(scene.title, 78.0f, kDesignWidth * 0.5f, 250.0f + (1.0f - head) * 26.0f,
         with_alpha(Color{246, 249, 255, 255}, head), 0.5f, true);
  p.text(scene.subtitle, 38.0f, kDesignWidth * 0.5f, 312.0f,
         with_alpha(Color{190, 204, 230, 255}, head * 0.9f), 0.5f);

  const float card_w = 470.0f;
  const float card_h = 400.0f;
  const float gap = 56.0f;
  const float total = card_w * 3.0f + gap * 2.0f;
  const float start_x = (kDesignWidth - total) * 0.5f;
  for (std::size_t i = 0; i < 3; ++i) {
    const float index = static_cast<float>(i);
    const float t = ease_out_back(clamp01f((local_time - 0.22f - 0.14f * index) / 0.85f));
    if (t <= 0.001f) continue;
    const float x = start_x + index * (card_w + gap);
    const float y = 430.0f + (1.0f - t) * 70.0f;
    paint_card(p, x, y, card_w, card_h, 26.0f, with_alpha(Color{255, 255, 255, 255}, 0.055f * t),
               with_alpha(scene.accent, 0.30f * t), 0.5f * t);
    // 图标：有素材则贴图（图文卡片），否则几何语言（不引图标集）。
    const float icon_y = y + 120.0f;
    const bool has_img = assets != nullptr && !scene.image_card.empty();
    if (has_img) {
      const st::math::Rect box{x + 40.0f, icon_y - 90.0f, card_w - 80.0f, 180.0f};
      paint_card_image(p, scene, box, *assets, basis);
    } else {
      p.stroke_circle(x + card_w * 0.5f, icon_y, 46.0f * t,
                      with_alpha(scene.accent, 0.85f * t), 3.0f);
      p.circle(x + card_w * 0.5f, icon_y, 14.0f * t, with_alpha(scene.accent, 0.9f * t));
    }
    const std::string label =
        i < scene.lines.size() ? scene.lines[i] : std::format("要点 {}", i + 1);
    // **自动缩字号**：卡片宽度固定，而文案长度是外部输入（AI 写的）。
    // 固定 50 号字遇到「融合旧世界 IT」这类长句会直接溢出卡片边界
    //（实测：文字压到卡片外，看着像排版坏了）。
    // 先量宽度，超过可用宽就按比例缩——最小 30 号（再小就不是标题了，
    // 干脆换行更难看，所以宁可变小）。
    const float label_max_w = card_w - 48.0f;   // 左右各留 24 的内边距
    float label_size = 50.0f;
    const float measured = p.text_width(label, label_size, true);
    if (measured > label_max_w && measured > 0.0f) {
      label_size = std::max(30.0f, label_size * (label_max_w / measured));
    }
    p.text(label, label_size, x + card_w * 0.5f, y + 268.0f,
           with_alpha(Color{244, 247, 255, 255}, t), 0.5f, true);
    const char* note = i == 0 ? "声明即刻生效" : (i == 1 ? "统一裁决留痕" : "全程可回滚");
    p.text(note, 30.0f, x + card_w * 0.5f, y + 320.0f,
           with_alpha(Color{180, 196, 224, 255}, t * 0.9f), 0.5f);
  }
}

/// 数字：三个大数字滚动到位（"数据感"最直接的来源）。
void paint_metrics(Painter& p, const Scene& scene, float local_time) {
  const float head = ease_out_cubic(clamp01f(local_time / 0.8f));
  p.text(scene.title, 72.0f, kDesignWidth * 0.5f, 230.0f + (1.0f - head) * 24.0f,
         with_alpha(Color{246, 249, 255, 255}, head), 0.5f, true);
  p.text(scene.subtitle, 38.0f, kDesignWidth * 0.5f, 292.0f,
         with_alpha(Color{190, 204, 230, 255}, head * 0.9f), 0.5f);

  const std::size_t count =
      std::min<std::size_t>(3, std::max(scene.values.size(), scene.lines.size()));
  const float slot = kDesignWidth / 3.0f;
  for (std::size_t i = 0; i < count; ++i) {
    const float index = static_cast<float>(i);
    const float t = ease_out_expo(clamp01f((local_time - 0.30f - 0.12f * index) / 1.1f));
    const float target =
        i < scene.values.size() ? static_cast<float>(scene.values[i]) : index + 1.0f;
    const float shown = target * t;
    const float cx = slot * (index + 0.5f);
    const bool integral = std::abs(target - std::round(target)) < 1e-4f;
    std::string number =
        integral ? std::format("{:.0f}", shown) : std::format("{:.1f}", shown);
    // “+” 接在数字本体后面（不单独定位）：单独定位时它会被推到列外、
    // 与邻列相撞——实测 2628+ 那一列右缘直接出了画布。
    if (integral && target >= 1000.0f) number += "+";
    // 长数字缩小一级：四位数在 168px 下宽约 660px，会压到列边界（列宽 640）。
    const float number_size = number.size() <= 3 ? 168.0f : 132.0f;
    p.text(number, number_size, cx, 600.0f,
           with_alpha(mix_color(scene.accent, Color{255, 255, 255, 255}, 0.35f), t), 0.5f, true);
    const std::string label =
        i < scene.lines.size() ? scene.lines[i] : std::format("指标 {}", i + 1);
    p.text(label, 38.0f, cx, 690.0f, with_alpha(Color{200, 214, 238, 255}, t * 0.95f), 0.5f);
  }
  // 分割细线（数字之间）：让三列有结构而不是三坨文字。
  for (int i = 1; i < 3; ++i) {
    const float t = ease_out_cubic(clamp01f((local_time - 0.5f) / 0.8f));
    p.fill(slot * static_cast<float>(i), 470.0f, 1.6f, 260.0f * t,
           with_alpha(Color{255, 255, 255, 255}, 0.10f));
  }
}

/// 片尾：标志（圆环 + 中心点，纯几何）+ slogan。
void paint_outro(Painter& p, const Scene& scene, float local_time) {
  const float enter = ease_out_expo(clamp01f(local_time / 0.9f));
  const float cx = kDesignWidth * 0.5f;
  const float cy = kDesignHeight * 0.5f - 40.0f;
  // 双环：外环缓慢呼吸，内点脉动。
  const float breathe = 1.0f + 0.035f * std::sin(local_time * 2.4f);
  p.stroke_circle(cx, cy, 150.0f * enter * breathe, with_alpha(scene.accent, 0.75f), 3.5f);
  p.stroke_circle(cx, cy, 104.0f * enter * breathe, with_alpha(Color{255, 255, 255, 255}, 0.22f),
                  1.6f);
  p.circle(cx, cy, 34.0f * enter, with_alpha(scene.accent, 0.95f));
  p.glow(cx, cy, p.design(420.0f), with_alpha(scene.accent, 255.0f), 0.30f * enter);

  const float text_t = ease_out_cubic(clamp01f((local_time - 0.35f) / 0.8f));
  p.text(scene.title, 96.0f, cx, cy + 250.0f + (1.0f - text_t) * 20.0f,
         with_alpha(Color{246, 249, 255, 255}, text_t), 0.5f, true);
  p.text(scene.subtitle, 34.0f, cx, cy + 316.0f,
         with_alpha(Color{190, 204, 230, 255}, text_t * 0.92f), 0.5f);
}

}  // namespace

auto ease_out_expo(float t) -> float {
  const float value = clamp01f(t);
  return value >= 1.0f ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * value);
}

auto ease_out_cubic(float t) -> float {
  const float value = clamp01f(t);
  const float inverse = 1.0f - value;
  return 1.0f - inverse * inverse * inverse;
}

auto ease_in_out_cubic(float t) -> float {
  const float value = clamp01f(t);
  if (value < 0.5f) return 4.0f * value * value * value;
  const float inverse = -2.0f * value + 2.0f;
  return 1.0f - inverse * inverse * inverse / 2.0f;
}

auto ease_out_back(float t) -> float {
  const float value = clamp01f(t);
  constexpr float c1 = 1.70158f;
  constexpr float c3 = c1 + 1.0f;
  const float inverse = value - 1.0f;
  return 1.0f + c3 * inverse * inverse * inverse + c1 * inverse * inverse;
}

void render_scene(st::raster::Canvas& target, const Scene& scene, double local_time,
                  double scene_duration, const st::ui::TextPort* text_port,
                  const AssetLibrary* assets, std::string_view asset_basis) {
  Painter p(target, text_port);
  const float duration = static_cast<float>(scene_duration > 0.01 ? scene_duration : 2.0);
  const float time = static_cast<float>(local_time < 0.0 ? 0.0 : local_time);
  const bool profile = std::getenv("CAIYUN_PROFILE") != nullptr;
  const auto now = [] { return st::time::now_ns(); };
  const std::int64_t t0 = profile ? now() : 0;

  // 背景全用**纯色填充**拼（底色条带 + 二维格子铺光 + 渐变暗角带）：整幅渐变填充
  // 在本框架里是逐像素采样（实测 1080p 约 25 ns/px，一次全幅 ≈ 50ms），而纯色
  // fill_rect 走覆盖运行段 + SIMD 混合，同样的观感成本低一个数量级。
  paint_backdrop(p, scene, time, duration);
  // 素材背景**叠在底色之上、文字之下**：底色仍是兜底（图缺失时有基调可用）。
  if (assets != nullptr) paint_background_image(p, scene, time, duration, *assets, asset_basis);
  const std::int64_t t1 = profile ? now() : 0;
  switch (scene.kind) {
    case SceneKind::Title: paint_title(p, scene, time); break;
    case SceneKind::Terminal: paint_terminal(p, scene, time, duration); break;
    case SceneKind::Triptych: paint_triptych(p, scene, time, assets, asset_basis); break;
    case SceneKind::Metrics: paint_metrics(p, scene, time); break;
    case SceneKind::Outro: paint_outro(p, scene, time); break;
  }
  if (profile) {
    st::print("[profile] bg={:.1f}ms content={:.1f}ms\n",
              static_cast<double>(t1 - t0) / 1.0e6, static_cast<double>(now() - t1) / 1.0e6);
  }
}

}  // namespace caiyun

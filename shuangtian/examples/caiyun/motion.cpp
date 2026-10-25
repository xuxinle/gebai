/// 场景渲染引擎（见 `motion.hpp`）。
///
/// 排版常数全部在 1920×1080 设计坐标里；`Painter` 负责把它们换算到目标画布的物理像素。
///
/// 全文件统一用 **float**（设计坐标本就是像素量级，float 有 7 位有效数字，够）：
/// 混用 double 会触发 `-Wdouble-promotion`（本仓 `-Werror`），而逐个 `static_cast`
/// 只会把表达式写成一团——统一量纲比逐点转换可读得多。

#include "motion.hpp"

#include "assets_lib.hpp"
#include "brand.hpp"
#include "carry.hpp"
#include "fx.hpp"
#include "layer.hpp"
#include "kin_text.hpp"
#include "layout.hpp"
#include "model3d.hpp"
#include "st/core/log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <string>
#include <functional>
#include <string_view>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/time.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"

namespace caiyun {

namespace {

using st::math::Color;
using st::math::Point;
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
  Painter(st::raster::Surface& canvas, const TextPort* port)
      : canvas_(canvas), port_(port),
        scale_(static_cast<float>(canvas.physical_width()) / kDesignWidth),
        width_(static_cast<float>(canvas.physical_width())),
        height_(static_cast<float>(canvas.physical_height())) {}

  /// 设置矢量相机（缺省 zoom=1 = 完全不变，老路径零成本）。
  void set_camera(const VectorCamera& camera) {
    camera_ = camera;
    // 锚点缺省值随画布尺寸走——`kDesignWidth/2` 在竖版画幅下不是画面中心。
    if (camera.anchor_x < 0.0f) camera_.anchor_x = kDesignWidth * 0.5f;
    if (camera.anchor_y < 0.0f) camera_.anchor_y = kDesignHeight * 0.5f;
  }
  [[nodiscard]] auto camera() const noexcept -> const VectorCamera& { return camera_; }

  [[nodiscard]] auto scale() const noexcept -> float { return scale_; }
  [[nodiscard]] auto width() const noexcept -> float { return width_; }
  [[nodiscard]] auto height() const noexcept -> float { return height_; }
  [[nodiscard]] auto port() const noexcept -> const TextPort* { return port_; }

  [[nodiscard]] auto rect(float x, float y, float w, float h) const -> st::math::Rect {
    const auto [px, py] = camera_.apply(x, y);
    // 宽高按**缩放倍数**变换（不做旋转时就是等比）——旋转不影响轴对齐矩形尺寸，
    // 本框架的图元只支持轴对齐，旋转的矩形会被当成包围盒（可接受：相机旋转
    // 建议 ≤1 度，包围盒误差在该量级下不可见）。
    const auto [px2, py2] = camera_.apply(x + w, y + h);
    return st::math::Rect{px * scale_, py * scale_, (px2 - px) * scale_, (py2 - py) * scale_};
  }
  [[nodiscard]] auto point(float x, float y) const -> st::math::Point {
    const auto [px, py] = camera_.apply(x, y);
    return st::math::Point{px * scale_, py * scale_};
  }
  /// 物理像素坐标 → 物理像素（只过相机，不再乘 scale）。
  ///
  /// ## 为何需要这个入口
  ///
  /// `path` 图元的点存的是**设计像素**，但绘制时要先变到物理像素再交给 canvas。
  /// 早先那里自己写 `lx + points[i] * scale`，**绕开了相机**——于是矢量相机
  /// 对路径图元无效（实测：一条 loss 曲线在 240× 时完全不动，而同一画面的
  /// rect/text 都在正常放大，很难看出是哪类图元漏了）。
  ///
  /// 这个入口把“已到物理像素的坐标”再过一次相机，避免在业务代码里
  /// 先除再乘（除乘往返会引入浮点误差，且看代码的人很难确认对不对）。
  [[nodiscard]] auto point_physical(float px, float py) const -> st::math::Point {
    const auto [x, y] = camera_.apply_physical(px, py, scale_);
    return st::math::Point{x, y};
  }
  /// 标量（半径/线宽/字号/圆角）按相机的**缩放倍数**变换。
  ///
  /// 这是矢量重绘的关键：字号也跟着变，文字才会按新尺寸重新光栅化（锐利），
  /// 而不是把旧位图拉大（糊）。
  [[nodiscard]] auto design(float value) const noexcept -> float {
    return value * scale_ * camera_.zoom;
  }
  void clear(Color color) { canvas_.clear(color); }
  void fill(float x, float y, float w, float h, Color color, float radius = 0.0f) {
    canvas_.fill_rect(rect(x, y, w, h), Paint::solid(color), design(radius));
  }
  /// 任意路径填充（路径点必须**已过相机**——用 `point()` 建路径）。
  ///
  /// 多边形/旋转矩形只能走这条路：`fill_rect` 只能画轴对齐矩形，
  /// 无论怎么调参数都画不出斜的。
  void fill_path(const Path& path, Color color) {
    if (path.is_empty()) return;
    canvas_.fill_path(path, Paint::solid(color));
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

  /// 排版：量宽并返回**世界坐标**下的宽度。
  ///
  /// 两处除以必须一致：`measure_width` 用的是**已乘 zoom 的字号**，
  /// 所以除回去时要除 `scale * zoom`。只除 `scale` 会让返回值虚肿 zoom 倍，
  /// 而调用方拿它去算居中/换行——**字越多偏得越远**，且小倍率下看不出来。
  [[nodiscard]] auto text_width(std::string_view text, float size, bool mono = false) const
      -> float {
    if (port_ == nullptr || text.empty()) return 0.0f;
    const float width = port_->measure_width(text, design(size),
                                             mono ? st::text::FontRole::Monospace
                                                  : st::text::FontRole::Proportional);
    return width / (scale_ * camera_.zoom);
  }

  /// 画一行文字：`anchor` 0=左 0.5=居中 1=右；`y` 是**基线**的 y（设计坐标）。
  ///
  /// 为什么要做基线换算：`TextPort::draw` 的 `origin` 是**行盒左上角**，而排版
  /// 位置几乎总是按基线想的（"标题坐在 y=470 这条线上"）。不换算的话每处都会
  /// 系统性偏低一个 `ascent`（≈0.93×字号）——大标题上就是 100+px 的错位。
  ///
  /// ## 粗体：只走真粗体面，**不做合成加粗**（本框架的硬性取向）
  ///
  /// 合成加粗是“把同一份轮廓水平平移后重复填充”，与真粗体面相比：
  /// 中文过渡带 0.246 vs **0.155**、英文字间离散 14.1% vs **0.0%**
  /// （实测见 `st/ui/text_port.hpp` 的 `prefers_real_bold`）。
  /// 对动效片尤其致命：大字号下“糊而脏”的过渡带会被放大到整屏可见。
  ///
  /// 所以这里**恒传 `embolden = 0`**，粗体完全由 `bold=true` 交给字体栈
  /// 去选粗体面。探不到粗体面的环境里，`find_face` 会自动回退常规面——
  /// 此时字重退化成 Regular（可接受：**宁可不粗，不要脏**）。
  void text(std::string_view content, float size, float x, float y, Color color,
            float anchor = 0.0f, bool bold = false, bool mono = false) {
    if (port_ == nullptr || content.empty()) return;
    const float advance = text_width(content, size, mono);
    const float left = x - advance * anchor;
    // ascent 也是“已乘 zoom 的像素量 → 世界坐标”，与 text_width 同口径
    const float top = y - port_->ascent(design(size)) / (scale_ * camera_.zoom);
    port_->draw(canvas_, content, point(left, top), design(size), color,
                mono ? st::text::FontRole::Monospace : st::text::FontRole::Proportional,
                0.0f, bold);
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
  ///
  /// ## `bold` 必须透传（这是一个真实缺陷的修复）
  ///
  /// 早先这里只传 5 个参数，**漏了 `bold`**——于是所有带字距的文本都退回
  /// Regular。而字距恰恰是**大标题**才会用的（`tracking` 在版式里就配给标题），
  /// 所以症状是“标题该粗却不粗”，看起来像字重设置没生效。
  ///
  /// 与 `text()` 同一取向：`embolden` 恒传 0，粗体只走真粗体面。
  auto spaced_text(std::string_view content, float size, float x, float baseline, Color color,
                   float tracking, bool bold = false) -> float {
    if (port_ == nullptr || content.empty()) return 0.0f;
    const auto glyphs = split_utf8(content);
    const float top = baseline - port_->ascent(design(size)) / scale_;
    float cursor = x;
    float total = 0.0f;
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
      port_->draw(canvas_, glyphs[i], point(cursor, top), design(size), color,
                  st::text::FontRole::Proportional, 0.0f, bold);
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
  // —— 品牌与结构原语 ——
  //
  // 产品的"产品感"主要来自**反复出现的结构元素**（识标、角标、页脚、进度），
  // 而不是单帧的特效。这些方法全部接受 design 坐标，内部换算到物理像素。

  /// 标志：同心环 + 中心点（几何画，任何 DPI 下都锐）。
  void brand_glyph(float cx, float cy, float radius, Color color, float alpha) {
    if (alpha <= 0.004f || radius <= 0.5f) return;
    stroke_circle(cx, cy, radius, with_alpha(color, alpha * 0.9f),
                  std::max(1.6f, radius * 0.085f));
    stroke_circle(cx, cy, radius * 0.62f, with_alpha(color, alpha * 0.42f),
                  std::max(1.0f, radius * 0.05f));
    circle(cx, cy, radius * 0.24f, with_alpha(color, alpha));
  }

  /// 品牌角标（左上角：标志 + 字标 + 拉丁名）。
  ///
  /// 位置由调用方给（版式的 brand_origin），不在这里硬编码——
  /// 版式常量集中在一处才能整体调。
  void brand_lockup(float x, float y, float glyph_radius, std::string_view wordmark,
                    std::string_view latin, Color color, float alpha) {
    if (alpha <= 0.004f) return;
    brand_glyph(x + glyph_radius, y + glyph_radius, glyph_radius, color, alpha);
    const float text_x = x + glyph_radius * 2.0f + glyph_radius * 0.62f;
    const float mark_size = glyph_radius * 0.92f;
    if (!wordmark.empty()) {
      text(wordmark, mark_size, text_x, y + glyph_radius - mark_size * 0.82f,
           with_alpha(Color{246, 249, 255, 255}, alpha), 0.0f, true);
    }
    if (!latin.empty()) {
      spaced_text(latin, mark_size * 0.36f, text_x, y + glyph_radius + mark_size * 0.82f,
                  with_alpha(color, alpha * 0.72f), mark_size * 0.14f);
    }
  }

  /// 页脚：细横线 + 一行小字 + 右侧页码。
  ///
  /// 单帧看不出作用，但**连续看下来**它提供了"这是一个完整作品"的暗示——
  /// 没有页脚的片子读起来像"幻灯片拼接"。
  void footer(float margin, float y, float width, std::string_view note,
              std::string_view page, Color color, float alpha) {
    if (alpha <= 0.004f) return;
    fill(margin, y, width - margin * 2.0f, 1.2f, with_alpha(color, alpha * 0.20f));
    const float text_size = 26.0f;
    // 页脚文字的**对比度下限**：包装层在任意背景上都要能读出来。
    // 早先 note 用 alpha*0.60，压在深色素材上直接消失了（实测）。
    // 这里不用强调色，而用**近白**——强调色在深色底上饱和度不够，
    // 小字会"糊"进背景。
    const Color ink{214, 226, 244, 255};
    if (!note.empty()) {
      text(note, text_size, margin, y + text_size * 0.9f,
           with_alpha(ink, alpha * 0.88f), 0.0f);
    }
    if (!page.empty()) {
      const float w = text_width(page, text_size, true);
      text(page, text_size, width - margin - w, y + text_size * 0.9f,
           with_alpha(ink, alpha * 0.95f), 0.0f, true);
    }
  }

  /// 进度指示：一排短段，已过的段高亮（比一条细进度条更有设计感，也更省绘制）。
  void progress_dots(float cx, float y, int count, int active, float size, Color color,
                     float alpha) {
    if (count <= 0 || alpha <= 0.004f) return;
    const float gap = size * 1.9f;
    const float total = static_cast<float>(count - 1) * gap;
    for (int i = 0; i < count; ++i) {
      const float x = cx - total * 0.5f + static_cast<float>(i) * gap;
      const bool on = i <= active;
      fill(x - size * 0.5f, y, size, size * 0.28f,
           with_alpha(color, alpha * (on ? 0.92f : 0.26f)));
    }
  }

  /// 四角对齐角标（印刷/影视包装里最常见的结构元素）。
  ///
  /// 它让画面看起来**被设计过**，而不是元素随机浮在背景上。
  void corner_ticks(float inset, float length, float thickness, Color color, float alpha) {
    if (alpha <= 0.004f) return;
    const float w = design_w();
    const float h = design_h();
    const Color c = with_alpha(color, alpha);
    // 左上
    fill(inset, inset, length, thickness, c);
    fill(inset, inset, thickness, length, c);
    // 右上
    fill(w - inset - length, inset, length, thickness, c);
    fill(w - inset - thickness, inset, thickness, length, c);
    // 左下
    fill(inset, h - inset - thickness, length, thickness, c);
    fill(inset, h - inset - length, thickness, length, c);
    // 右下
    fill(w - inset - length, h - inset - thickness, length, thickness, c);
    fill(w - inset - thickness, h - inset - length, thickness, length, c);
  }

  /// 素材绘制用：blit_cover/blit_contain 是自由函数（在 Painter 外部），
  /// 需要 Canvas&。不给 getter 就只能靠"再开一条绘制路径"——两条路径必漂移。
  [[nodiscard]] auto canvas() -> st::raster::Surface& { return canvas_; }
  [[nodiscard]] auto design_w() const noexcept -> float { return kDesignWidth; }
  [[nodiscard]] auto design_h() const noexcept -> float { return kDesignHeight; }

 private:
  st::raster::Surface& canvas_;
  const TextPort* port_;


  float scale_;
  float width_;
  float height_;
  /// 矢量相机（缺省 zoom=1：`apply` 早退，老路径零成本）。
  VectorCamera camera_{};
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

    // 背景亮度（两色取亮者）——决定铺光与暗角要不要上。
  //
  // 为何按亮度自适应而不是给脚本加开关：**“浅底不该有铺光/暗角”是普适的**，
  // 而让每个写米白片的人都记得关掉它是又一处“必然漏”的开关。
  // 实测代价：米白日历场景四角被暗角刷成四个大黑角、中间被铺光揩成脏灰，
  // 而所有数字（亮度/变化强度/静止占比）都毫无异常——根本看不出是哪一层画的。
  const bool light = backdrop_is_light(scene.bg_top, scene.bg_bottom);

  // 两束**竖向铺光**：一束自上压下、一束自下而上，强弱跟着相位呼吸。
  //
  // 只用竖向（原因见 `wash_vertical`）——斜向/径向在本框架里都是逐像素慢路径。
  // 铺光是“光从哪来”，它在深色底上给出体积感；浅底上只会把白揩成脏灰。
  if (!light) {
    p.wash_vertical(with_alpha(scene.accent, 0.26f + 0.08f * (1.0f - drift)),
                    with_alpha(scene.accent, 0.0f));
    p.wash_vertical(with_alpha(scene.accent, 0.0f),
                    with_alpha(mix_color(scene.accent, Color{255, 255, 255, 255}, 0.5f),
                               0.14f + 0.05f * (1.0f - drift)));
  }
  // **不加全幅径向光斑**：实测它的包裹盒（直径 1340px）单帧就要 84ms——径向渐变
  // 在本框架里是逐像素算距离（47 ns/px，见 `tools/gradient_fill_probe.cpp`）。
  // 光斑留给小半径场合（片尾标志），背景层只用竖向渐变。
  //
  // 暗角把视线收到中心，对深色底是加分；浅底上就是四个大黑角。
  if (!light) {
    p.vignette(0.9f);
  }
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
/// 按图层参数画文字（支持多行、对齐、字距）。
///
/// 为什么要单独一个 helper：`Painter::text` 只画单行、且 x 是中心锚点。
/// 图层需要**左/右对齐**与**换行**——这两个能力在通用版式里是必需项
/// （左边图片、右边一段说明文字，是宣传片最常见的构图）。
void draw_text_lines(Painter& p, const Layer& layer, float lx, float ly, float lw, float lh,
                     float alpha, float scale, const st::ui::TextPort* port) {
  const Color color = with_alpha(layer.color, alpha);
  const float size = layer.size * scale;
  const float tracking = layer.tracking * scale;
  const float line_step = size * std::max(1.0f, layer.line_height);

  // 按 `\n` 拆行（脚本里用 `\\n` 换行；不做自动折行——折行需要断词规则，
  // 中文/英文/混排各不相同，让调用方显式断行更可控）。
  std::vector<std::string> lines;
  std::size_t begin = 0;
  while (begin <= layer.text.size()) {
    const std::size_t end = layer.text.find('\n', begin);
    if (end == std::string::npos) {
      lines.push_back(layer.text.substr(begin));
      break;
    }
    lines.push_back(layer.text.substr(begin, end - begin));
    begin = end + 1;
  }

  float cursor_y = ly;
  for (const std::string& line : lines) {
    if (!line.empty()) {
      // 量宽用于对齐；**超出图层框就自动缩字号**。
      //
      // 为什么必须缩：图层框宽是用户给的确定值，而文字长度是外部输入。
      // 不缩就会溢出到框外（实测右侧说明文字超出画面被裁掉一半）。
      // 缩字号比换行安全：换行需要断词规则（中文/英文/混排各不同），
      // 而脚本里的 `\n` 已经让用户显式控制换行——这里只保证"不溢出"。
      float line_size = size;
      float width = 0.0f;
      const auto measure = [&](float candidate) {
        const float base = port->measure_width(line, candidate);
        return tracking <= 0.01f
                   ? base
                   : base + tracking * static_cast<float>(line.size() - 1) *
                                (candidate / std::max(1.0f, size));
      };
      width = measure(line_size);
      if (lw > 1.0f && width > lw) {
        line_size = std::max(8.0f, size * (lw / width));
        width = measure(line_size);
      }
      float draw_x = lx;
      if (layer.align == 0) {
        draw_x = lx + (lw - width) * 0.5f;   // 居中（在图层框内，而不是画面内）
      } else if (layer.align == 2) {
        draw_x = lx + lw - width;            // 右对齐
      }
      // ⚠ `Painter::text` 的 anchor 参数：0=左对齐（x 是左边界）、0.5=居中（x 是中心）。
      //
      // 早先这里传 0.0f 又把 x 算成 `draw_x + width*0.5`，等于"按中心算位置、
      // 却按左边界画" —— 文字整体右移半个宽度，超出图层框后被裁（实测三栏卡片
      // 的标题偏右 100px、右侧被切）。居中必须传 0.5f。
      const float step_ratio = line_size / std::max(1.0f, size);
      // ⚠ **坐标必须换回设计坐标再交给 `Painter`**。
      //
      // 本函数里的 `lx/ly/lw/lh`、`width`、`cursor_y` 都是**物理像素**
      //（它们由 `evaluate_layer` 按画布尺寸算出来）；而 `Painter::text` /
      // `spaced_text` 的入参是**设计坐标**（内部会乘 `scale_`）。
      // 直接把物理像素传进去会**再乘一次**，于是文字位置与字号都被缩 `scale` 倍：
      // 1080p 导出（scale=1）下看不出来，而预览（960×540，scale=0.5）与
      // 导出帧序列（320×180，scale=0.167）下文字全部堆到左上角。
      //
      // 实测症状：三幕文字全挤在左上角、且字号小到读不出——但 1080p 成片是对的，
      // 所以这个缺陷只在“用低分辨率抽帧验收”时才暴露（而那正是推荐的自检方式）。
      const float design_x = (draw_x + width * 0.5f) / scale;
      const float design_y = cursor_y / scale;
      if (tracking <= 0.01f) {
        // draw_x 是左边界；居中时传 0.5 让它以 (draw_x + width/2) 为中心画。
        p.text(line, line_size / scale, design_x, design_y, color, 0.5f, layer.bold);
      } else {
            p.spaced_text(line, line_size / scale, draw_x / scale, design_y, color,
                  layer.tracking * step_ratio, layer.bold);
      }
    }
    cursor_y += line_step;
  }
  (void)lh;
}

/// 画一组通用图层（`Scene::layers`）。
///
/// ## 为什么单独一个函数而不是塞进场景模板
///
/// 图层是**数据驱动**的：任何场景（包括五个内置模板）都可以在下面叠图层。
/// 塞进模板会让"自定义版式"与"内置版式"变成两条代码路径，
/// 而两条路径必然漂移（本项目在别的功能上付过这个代价）。
///
/// ## 绘制顺序
///
/// 按数组顺序（先列出的在下层）。这与镂月的块顺序语义一致——
/// 用户学一次就够，不必记两套。
/// 轨迹采集开关：由环境变量 `CAIYUN_TRACK` 控制。
///
/// ## 为何用全局缓冲而不是逐层传参
///
/// `render_frame` 的调用链是 `render.cpp → project.cpp → motion.cpp → paint_layers`，
/// 中间还有快/慢两条路径与运动模糊的多次采样。要让轨迹一路传下去，
/// 得改四处签名 + 每条路径都记得带上——而漏一处就是“某些帧没轨迹”，
/// 极难发现。
///
/// 轨迹只是一份诊断输出（默认关闭，零开销），用线程局部的全局缓冲更稳：
/// 每线程一份（导出并行安全），由 `dump_trajectory()` 一次性写出。
struct TrajectoryCell {
  std::vector<ElementTrack>* sink{nullptr};
};

[[nodiscard]] auto trajectory_cell() -> TrajectoryCell& {
  static thread_local TrajectoryCell cell{};
  return cell;
}

/// 本帧要写入的轨迹目标（空 = 不采集）。
[[nodiscard]] auto trajectory_sink() -> std::vector<ElementTrack>* {
  return trajectory_cell().sink;
}

void paint_layers(Painter& p, const Scene& scene, float local_time, const AssetLibrary* assets,
                  std::string_view basis, std::vector<ElementTrack>* trajectory = nullptr) {
  if (scene.layers.empty()) return;
  const float canvas_w = p.width();
  const float canvas_h = p.height();
  const float design_w = p.design_w();
  const float design_h = p.design_h();
  const float scale = p.scale();

  // ── 先把**所有**图层的基准状态算出来 ──
  //
  // 为何先算一遍、而不是在绘制循环里边算边画：
  // 脚本编排层是**整帧一次求值**（`layout(t, duration, canvas, layers)` 拿到
  // 全部图层、返回全部增量）。每图层一次跨界会把导出拖慢几个量级
  // （342 帧 × 20 图层 × 几十微秒 ≈ 十几秒纯开销）。
  std::vector<LayerState> states;
  states.reserve(scene.layers.size());
  for (const Layer& layer : scene.layers) {
    states.push_back(evaluate_layer(layer, local_time, canvas_w, canvas_h, design_w, design_h));
  }

  // ── 脚本编排层（可选）：一次调用算出整帧增量 ──
  //
  // **没写 `layout` 时这条分支完全跳过**（零开销），老脚本行为一字不变。
  //
  // 装载时机：首次遇到带脚本的场景时编一次（每线程一个实例）。
  // 为何不在 `parse_script` 里编：QuickJS 运行时非线程安全，
  // 而导出开 16 路并行——共享就得全程加锁，那比多编几次贵得多。
  //
  // 失败时**降级而不报错**：脚本抛异常/超时 ⇒ 本帧零增量（= 纯 JSON 行为）。
  // 让它崩，等于“一处脚本错误毁掉整片”；让它静默空白，等于“坏了不知道”。
  // 所以选中间：**降级 + 累计失败计数**，导出结束时汇总报告。
  LayoutEngine& engine = layout_engine();
  if (!scene.layout.empty()) {
    // 本线程上次装的是不是这个脚本？
    //
    // ⚠ **必须用内容哈希**，不能用「文件名 + 长度」。
    // 实测踩过：“改脚本内容不改长度”时指纹不变 → 引擎不会重装 →
    // 渲染用的是**旧脚本**，而所有输出看起来“完全正常”（只是没变）。
    // 那个现象极难查：帧字节完全一致、有效变化率一模一样、零报错。
    static thread_local std::string loaded_fingerprint{};
    const std::string fingerprint = std::format(
        "{}#{}", scene.layout.filename, std::hash<std::string>{}(scene.layout.source));
    if (loaded_fingerprint != fingerprint) {
      const auto loaded = engine.load(scene.layout);
      if (!loaded) {
        ST_LOG_WARN("脚本编排层装载失败，已降级为纯 JSON：{}", loaded.error().message);
        loaded_fingerprint.clear();
      } else {
        loaded_fingerprint = fingerprint;
      }
    }
    if (engine.loaded()) {
      std::vector<LayerDelta> deltas;
      if (engine.evaluate(local_time, static_cast<float>(scene.duration), canvas_w, canvas_h,
                          states, deltas)) {
        for (std::size_t index = 0; index < states.size() && index < deltas.size(); ++index) {
          apply_delta(states[index], deltas[index]);
        }
      }
    }
  }

  std::size_t layer_index = 0;
  for (const Layer& layer : scene.layers) {
    const LayerState state = states[layer_index++];
    if (std::getenv("CAIYUN_LAYER_DEBUG") != nullptr) {
      st::print("[layer] kind={} x={} y={} w={} h={} (raw x={} y={}) alpha={:.2f}\n",
                layer_kind_name(layer.kind), state.x, state.y, state.w, state.h, layer.x,
                layer.y, state.alpha);
    }
    if (state.alpha <= 0.004f) continue;

    // 元素位置上报：供连贯性验收做“按元素追踪”。
    //
    // ## 为何需要它（画面级量尺的盲区）
    //
    // 只比“亮块有没有留下”无法区分两种情况：
    // · 同一个点从左上滑到右下（真连贯）；
    // · 左上换了一个新东西、右下也换了一个（幻灯片），
    // 两者在网格亮度上可能完全一样。
    // 要分开必须知道“这个元素是谁、它去哪了”——只有渲染器知道。
    //
    // 只记 alpha>0 的（看不见的元素不应影响判定），
    // 并只记名字（不记地址），导出后就是一份纯文本轨迹。
    if (state.alpha > 0.004f) {
      if (std::vector<ElementTrack>* sink = trajectory_sink(); sink != nullptr) {
        sink->push_back(ElementTrack{std::string(layer.name), state.x, state.y, state.w,
                                     state.h, state.alpha});
      }
      if (trajectory != nullptr) {
        trajectory->push_back(ElementTrack{std::string(layer.name), state.x, state.y, state.w,
                                           state.h, state.alpha});
      }
    }
    // 三维组：首层负责把**整组**画进一个共享世界（同一相机/光照/纹理）。
    // 判定“首层”看它是不是组内第一个出现的——组内顺序就是绘制顺序（先画桌面、再画图标）。
    bool is_group_head = false;
    if (!layer.model_group.empty()) {
      is_group_head = true;
      for (const Layer& previous : scene.layers) {
        if (&previous == &layer) break;
        if (previous.model_group == layer.model_group) {
          is_group_head = false;
          break;
        }
      }
      if (is_group_head) {
        // 收集组内成员（含 head 自己），交给组渲染器一次画完。
        std::vector<const Layer*> members;
        for (const Layer& candidate : scene.layers) {
          if (candidate.model_group == layer.model_group) members.push_back(&candidate);
        }
        const LayerState head_state = state;   // head 的动画状态（决定画面框）
        paint_model3d_group(p.canvas(), layer, members, local_time,
                            static_cast<float>(scene.duration), basis, head_state,
                            canvas_w, canvas_h, design_w, design_h);
        continue;   // 组内成员已一并画完
      }
    }

    // 入场缩放以层中心为锚点。
    float lx = state.x;
    float ly = state.y + state.offset_y;
    float lw = state.w;
    float lh = state.h;
    if (state.scale != 1.0f) {
      const float cx = lx + lw * 0.5f;
      const float cy = ly + lh * 0.5f;
      lw *= state.scale;
      lh *= state.scale;
      lx = cx - lw * 0.5f;
      ly = cy - lh * 0.5f;
    }

    // （矢量相机的处理见各图元：`p.fill()`/`p.line()`/`p.text()` 这些
    //  已经在 `Painter` 内部过相机；直画 `p.canvas()` 的则由各 case 调
    //  `p.point_physical()` 自行过。）

    switch (layer.kind) {
      case LayerKind::Rect: {
        if (layer.fill_alt.a == 0) {
          p.fill(lx, ly, lw, lh, with_alpha(layer.fill, state.alpha), layer.radius * scale);
        } else {
          // 渐变填充：走 canvas 直接填（Painter 的 fill 只有纯色）。
          st::raster::Gradient gradient =
              layer.gradient_dir == 1
                  ? st::raster::Gradient::linear(
                        st::math::Point{lx, ly}, st::math::Point{lx + lw, ly},
                        {st::raster::GradientStop{0.0f, with_alpha(layer.fill, state.alpha)},
                         st::raster::GradientStop{1.0f, with_alpha(layer.fill_alt, state.alpha)}})
                  : st::raster::Gradient::linear(
                        st::math::Point{lx, ly}, st::math::Point{lx, ly + lh},
                        {st::raster::GradientStop{0.0f, with_alpha(layer.fill, state.alpha)},
                         st::raster::GradientStop{1.0f, with_alpha(layer.fill_alt, state.alpha)}});
          // `p.canvas().fill_path()` **不过相机**，所以这里手动过一次。
          // （早先完全没过——矢量相机对路径图元无效，实测 240× 时曲线不动。）
          const auto tl = p.point_physical(lx, ly);
          const auto br = p.point_physical(lx + lw, ly + lh);
          st::raster::Path path;
          const float r = layer.radius * scale * (1.0f + std::log(p.camera().zoom));
          if (r > 0.5f) {
            path.add_rounded_rect(st::math::Rect{tl.x, tl.y, br.x - tl.x, br.y - tl.y}, r);
          } else {
            path.add_rect(st::math::Rect{tl.x, tl.y, br.x - tl.x, br.y - tl.y});
          }
          p.canvas().fill_path(path, Paint::with_gradient(std::move(gradient)));
        }
        if (layer.stroke.a != 0 && layer.stroke_width > 0.01f) {
          p.border(lx, ly, lw, lh, layer.radius * scale, with_alpha(layer.stroke, state.alpha),
                   layer.stroke_width * scale);
        }
        break;
      }
      case LayerKind::Ellipse: {
        // 用 fill_path 画椭圆（Painter 只有正圆；椭圆需要 path）。
        // path 直画 canvas ⇒ 手动过相机。
        const auto tl = p.point_physical(lx, ly);
        const auto br = p.point_physical(lx + lw, ly + lh);
        st::raster::Path path;
        path.add_ellipse(st::math::Rect{tl.x, tl.y, br.x - tl.x, br.y - tl.y});
        p.canvas().fill_path(path, Paint::solid(with_alpha(layer.fill, state.alpha)));
        break;
      }
      case LayerKind::Line: {
        // Line 用 x,y 作为起点、w,h 作为终点增量（而不是宽高）——
        // 这是图层语义里唯一的特例，写进文档免得误用。
        p.line(lx, ly, lx + lw, ly + lh, with_alpha(layer.stroke.a != 0 ? layer.stroke : layer.fill,
                                                    state.alpha),
               std::max(1.0f, layer.stroke_width * scale));
        break;
      }
      case LayerKind::Text: {
        if (layer.text.empty()) break;
        const st::ui::TextPort* port = p.port();
        if (port == nullptr) break;
        draw_text_lines(p, layer, lx, ly, lw, lh, state.alpha, scale, port);
        break;
      }
      case LayerKind::KinText: {
        // 逐字排版：自己负责排版，每个字独立驱动（见 `kin_text.cpp`）。
        if (layer.text.empty()) break;
        const st::ui::TextPort* port = p.port();
        if (port == nullptr) break;
        paint_kin_text(p, layer, state, scale, port, local_time);
        break;
      }
      case LayerKind::Image: {
        if (assets == nullptr || layer.src.empty()) {
          // 素材缺失：画占位框（不静默跳过——静默会让"图没进来"看起来像设计）。
          p.fill(lx, ly, lw, lh, Color{40, 20, 24, static_cast<std::uint8_t>(state.alpha * 255.0f)});
          break;
        }
        const auto path = assets->resolve(layer.src, basis);
        if (!path) {
          p.fill(lx, ly, lw, lh, Color{40, 20, 24, static_cast<std::uint8_t>(state.alpha * 255.0f)});
          break;
        }
        const auto image = assets->image(*path);
        if (!image) {
          p.fill(lx, ly, lw, lh, Color{40, 20, 24, static_cast<std::uint8_t>(state.alpha * 255.0f)});
          break;
        }
        const st::math::Rect box{lx, ly, lw, lh};
        if (layer.contain) {
          blit_contain(p.canvas(), *image, box, state.alpha);
        } else {
          blit_cover(p.canvas(), *image, box, state.alpha, layer.radius * scale);
        }
        // 压暗：叠一层半透明底色（cover 没有 tint 参数，独立叠一次更灵活——
        // 用户的压暗意图与"圆角裁切"是两件事，能分开控制）。
        const float dim = std::clamp(layer.dim, 0.0f, 1.0f);
        if (dim > 0.004f) {
          p.fill(lx, ly, lw, lh, Color{6, 9, 18, static_cast<std::uint8_t>(dim * 255.0f)},
                 layer.radius * scale);
        }
        break;
      }
      case LayerKind::Path: {
        if (layer.points.size() < 4) break;
        st::raster::Path path;
        bool first = true;
        for (std::size_t i = 0; i + 1 < layer.points.size(); i += 2) {
          // path 直画 canvas ⇒ 手动过相机（点与线宽都要）。
          // 早先绕过了相机，实测：一条 loss 曲线在 240× 时完全不动，
          // 而同一画面的 rect/text 都在正常放大，很难看出是哪类图元漏了。
          const auto pt = p.point_physical(lx + layer.points[i] * scale,
                                          ly + layer.points[i + 1] * scale);
          const st::math::Point point{pt.x, pt.y};
          if (first) {
            path.move_to(point);
            first = false;
          } else {
            path.line_to(point);
          }
        }
        if (layer.closed) {
          path.close();
          p.canvas().fill_path(path, Paint::solid(with_alpha(layer.fill, state.alpha)));
        }
        if (layer.stroke.a != 0 && layer.stroke_width > 0.01f) {
          // 线宽随缩放**亚线性**增长，用 `1 + ln(zoom)` 而不是 `zoom` 或 `√zoom`。
          //
          // ## 为何不是线性 `* zoom`
          // 实测：240× 时 `* zoom` 让一条 2.6px 的曲线变成 624px 的**粗带子**，
          // 完全读不出“这是条曲线”。
          //
          // ## 为何也不是 `√zoom`
          // 实测：28× 时 `√zoom` 是 5.3 倍 ⇒ 2.2px 变 11.6px。而深层的
          // 微步抖动 @28× 只有 5px——线宽比抖动还宽，**细节被自己的线盖住**。
          //
          // `1 + ln(zoom)`：28× 时 4.3 倍（9.5px），且增长越来越慢，
          // 既看得出“变近了”，又不会吃掉深层细节。
          const float growth = 1.0f + std::log(p.camera().zoom);
          p.canvas().stroke_path(path, Paint::solid(with_alpha(layer.stroke, state.alpha)),
                                 layer.stroke_width * scale * growth);
        }
        break;
      }
      case LayerKind::Glow: {
        GlowSpec spec{};
        spec.cx = canvas_w > 0.0f ? (lx + lw * 0.5f) / canvas_w : 0.5f;
        spec.cy = canvas_h > 0.0f ? (ly + lh * 0.5f) / canvas_h : 0.5f;
        spec.radius = layer.spread;
        spec.intensity = layer.intensity;
        spec.color = layer.fill;
        paint_glow(p.canvas(), spec, scale, state.alpha);
        break;
      }
      case LayerKind::Particles: {
        ParticleField field{};
        field.count = layer.count;
        field.alpha = state.alpha * layer.intensity;
        field.color = layer.fill;
        paint_particles(p.canvas(), field, static_cast<double>(local_time), scale);
        break;
      }
      case LayerKind::Model3d: {
        // 三维图层：矩形与入场动画（位移/缩放/透明度）由上面的通用逻辑算好，
        // 这里只负责把“这一块矩形里该画什么”交给 `model3d.cpp`。
        //
        // 为何传 `box` 而不是让 model3d 自己算坐标：设计像素→物理像素的换算
        // 只在 `Painter` 里做一次（否则两处换算必然漂移，而它只在导出时现形）。
        //
        // **组内的非首层跳过**：它们由首层统一画（见下方 `paint_model3d_group`）。
        // 不跳会画两遍：一遍在共享世界里（对）、一遍孤立（错，而且能看见）。
        if (layer.model_group.empty() || is_group_head) {
          paint_model3d(p.canvas(), layer, st::math::Rect{lx, ly, lw, lh}, local_time,
                        static_cast<float>(scene.duration), state.alpha, basis);
        }
        break;
      }
    }
  }
}

/// 版式槽位（全片共用一套：换画幅时所有场景一起跟着变）。
[[nodiscard]] auto make_slots() -> LayoutSlots {
  LayoutSlots slots{};
  slots.grid = Grid::make(kDesignWidth, kDesignHeight);
  return slots;
}

/// 全片共用的一层「包装」：品牌角标 + 四角标 + 页脚 + 进度。
///
/// ## 为什么抽成一个函数
///
/// 包装元素必须**逐帧一致**（位置、透明度、字号完全一样），否则连续看下来
/// 会"抖"——而人眼对"本该固定却每帧微动"的东西极其敏感。
/// 各场景各写一遍必然漂移，所以只留一个入口。
///
/// ## 为什么这些元素能解决"太简单"
///
/// "简单"不是元素少，而是**没有系统**：单帧里几个大字看着没问题，
/// 连起来看就觉得"随便摆的"。包装层提供三件产品物料必有的事：
/// - 识标：观众知道这是谁做的；
/// - 结构（四角标/页脚线）：画面"被设计过"的暗示；
/// - 进度：观众知道自己在片子的哪里。
void paint_wrap(Painter& p, const Scene& scene, float local_time, int scene_index,
                int scene_total) {
  const LayoutSlots slots = make_slots();
  const Grid& grid = slots.grid;
  // 入场稍微滞后于内容（0.15s）：包装先到会抢内容的第一印象。
  const float t = ease_out_cubic(clamp01f((local_time - 0.12f) / 0.85f));
  if (t <= 0.004f) return;

  p.corner_ticks(grid.margin * 0.62f, 46.0f, 2.0f, scene.accent, t * 0.13f);
  // 识标：品牌名为空则**不画**。
  //
  // 这是通用工具，不该预设某个产品的名字。默认空串→不画，
  // 比印一个占位品牌好：假品牌会被误认为"模板没改干净"。
  if (!scene.brand_wordmark.empty() || !scene.brand_latin.empty()) {
    p.brand_lockup(grid.margin, grid.margin, slots.brand.glyph_radius, scene.brand_wordmark,
                   scene.brand_latin, scene.accent, t * 0.9f);
  }
  p.progress_dots(grid.center_x(), grid.height - grid.margin * 1.55f, scene_total, scene_index,
                  26.0f, scene.accent, t * 0.85f);
  // 页脚：左下角小字同样**空则不画**（不塞默认品牌）。
  p.footer(grid.margin, grid.height - grid.margin * 0.92f, grid.width, scene.footer_note,
           std::format("{:02d} / {:02d}", scene_index + 1, scene_total), scene.accent,
           t * 0.9f);
}

void paint_title(Painter& p, const Scene& scene, float local_time) {
  const LayoutSlots slots = make_slots();
  const Grid& grid = slots.grid;
  const float enter = ease_out_expo(clamp01f(local_time / 0.9f));
  const float rise = (1.0f - enter) * 64.0f;
  const float center_x = grid.center_x();
  const float title_size = 188.0f;

  // 标题落点走版式槽位，不写死像素。
  const float tracking = title_size * 0.10f;
  const float spaced = p.spaced_width(scene.title, title_size, tracking);
  const float title_top = slots.title_y() + rise;
  p.spaced_text(scene.title, title_size, center_x - spaced * 0.5f, title_top,
                with_alpha(Color{245, 248, 255, 255}, clamp01f(enter * 1.15f)), tracking);

  const float rule_y = title_top + title_size * 0.78f;
  const float line_t = ease_out_expo(clamp01f((local_time - 0.35f) / 0.7f));
  const float line_width = grid.content_width() * 0.22f * line_t;
  if (line_width > 1.0f) {
    p.line(center_x - line_width, rule_y, center_x + line_width, rule_y,
           with_alpha(scene.accent, 0.85f), 3.0f);
  }

  const float sub_t = ease_out_cubic(clamp01f((local_time - 0.30f) / 0.8f));
  const float sub_size = 42.0f;
  const float sub_y = rule_y + sub_size * 1.35f;
  p.spaced_text(scene.subtitle, sub_size,
                center_x - p.spaced_width(scene.subtitle, sub_size, 10.0f) * 0.5f,
                sub_y + (1.0f - sub_t) * 22.0f,
                with_alpha(mix_color(scene.accent, Color{255, 255, 255, 255}, 0.62f),
                           sub_t * 1.05f),
                10.0f);

  // ── 下半部信息结构 ──
  //
  // 早先画面下半 500px 是空的，整帧读起来像"标题 + 一大片背景"。
  // 这里补三样**有信息量的**结构（不是装饰）：
  //   ① 三条能力短语（分隔线 + 小字，横排）——给人"这是个什么东西"的抓手；
  //   ② 一条细分隔线，把标题区与信息区分开（版式的"分隔"职责）；
  //   ③ 页脚与进度（片子的连续性）。
  const float facts_y = grid.height * 0.665f;
  const float facts_t = ease_out_cubic(clamp01f((local_time - 0.62f) / 0.9f));
  if (facts_t > 0.01f) {
    // 分隔线：从中心向两侧展开，比标题线更细更淡（层级：主 > 次）。
    p.line(grid.margin, facts_y - 34.0f, grid.margin + grid.content_width() * facts_t,
           facts_y - 34.0f, with_alpha(scene.accent, facts_t * 0.18f), 1.2f);
    // 三条短语取自场景的 `lines`（空则该行不画）。
    //
    // 早先这里硬编码了三个词，等于把某个具体产品的内容烧进了模板——
    // 这是"示例"而不是"工具"的典型症状。模板只负责版式，文案由脚本给。
    std::vector<std::string> facts;
    for (const std::string& line : scene.lines) {
      if (!line.empty()) facts.push_back(line);
      if (facts.size() >= 3) break;
    }
    const float cell = grid.content_width() / 3.0f;
    for (std::size_t i = 0; i < facts.size(); ++i) {
      const float index = static_cast<float>(i);
      const float t = ease_out_cubic(clamp01f((local_time - 0.68f - 0.12f * index) / 0.85f));
      const float cx = grid.margin + cell * (index + 0.5f);
      // 小方块标记 + 文字（几何标记而不是项目符号——本项目没有图标集）。
      //
      // 纵向对齐：文字 y 是**行盒顶部**，行高约 1.25 倍字号；
      // 方块要对的是行的视觉中心（顶部 + 行高*0.42），不是基线——
      // 早先写 +8 让它挂在字的下沿外（一眼看出"没对齐"）。
      const float mark_size = 8.0f;
      const float row_mid = facts_y + 30.0f * 0.42f - mark_size * 0.5f;
      const float label_left = cx - p.text_width(facts[i], 30.0f) * 0.5f;
      p.fill(label_left - mark_size - 14.0f, row_mid, mark_size, mark_size,
             with_alpha(scene.accent, t * 0.85f));
      p.text(facts[i], 30.0f, cx, facts_y + (1.0f - t) * 14.0f,
             with_alpha(Color{206, 220, 244, 255}, t * 0.88f), 0.5f);
      // 列间细分隔（只在中间两列之间画，首尾不画）
      if (i < 2) {
        const float sep_x = grid.margin + cell * (index + 1.0f);
        p.fill(sep_x, facts_y - 8.0f, 1.0f, 34.0f, with_alpha(scene.accent, t * 0.16f));
      }
    }
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
  //
  // 位置要**夹在卡片内**：卡片高度按"全部行"预留（`168 + 76*n`），而敲完最后一行后
  // `cursor_y` 已经走到 `140 + 76*n`——直接画就落在卡片下边框之外 20px（实测：
  // 一条蓝色竖块悬在卡片底边外面，看着像渲染错位）。夹一行内边距即可。
  if (typed > 0.0f && std::fmod(local_time, 1.0f) < 0.55f) {
    const float cursor_top = std::min(cursor_y, card_y + card_h - 64.0f);
    p.fill(cursor_x, cursor_top + 8.0f, 16.0f, 40.0f, with_alpha(scene.accent, 0.9f));
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
  // 三列落在**版心内**（不是铺满整幅画）：kDesignWidth / 3 会让最左/最右的数字
  // 中心贴到画面边（大字一半在安全区外），叠加运镜放大后直接顶到边缘。
  // 实测数据页的「100」「10」被裁到几乎贴边——那是"排版坏了"而不是"镜头推进"。
  const LayoutSlots slots = make_slots();
  const Grid& grid = slots.grid;
  const float slot = grid.content_width() / 3.0f;
  for (std::size_t i = 0; i < count; ++i) {
    const float index = static_cast<float>(i);
    const float t = ease_out_expo(clamp01f((local_time - 0.30f - 0.12f * index) / 1.1f));
    const float target =
        i < scene.values.size() ? static_cast<float>(scene.values[i]) : index + 1.0f;
    const float shown = target * t;
    // 补上版心左边距：否则整组内容仍然贴着画布边。
    const float cx = grid.margin + slot * (index + 0.5f);
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

/// 跨场承接图元的绘制（声明在 `carry.hpp`）。
///
/// 放在这里而不是 `carry.cpp`：它需要使用本文件的 `Painter`（设计坐标入、
/// 内部乘 `scale_`）。若在 `carry.cpp` 里另写一套坐标换算，两处必然漂移——
/// 而漂移在 1080p 导出（scale=1）下看不出来，只在预览与帧序列里现形。
void paint_carry(st::raster::Surface& target, const CarryOut& carry, const CarryPose& pose,
                 const st::ui::TextPort* text_port, float alpha) {
  if (!pose.active || alpha <= 0.004f) return;
  if (pose.w < 1.0f || pose.h < 1.0f) return;
  Painter p(target, text_port);

  const auto with_alpha = [alpha](Color color) -> Color {
    const auto a = static_cast<std::uint8_t>(
        std::clamp(static_cast<float>(color.a) * alpha, 0.0f, 255.0f));
    return Color{color.r, color.g, color.b, a};
  };

  // 底色：承接图元必须**不透明**才有“同一块东西”的实体感。
  // 半透明的承接会被读成“一层渐变的蒙版”，carry 的意图就丢了。
  if (carry.fill.a != 0) {
    p.fill(pose.x, pose.y, pose.w, pose.h, with_alpha(carry.fill), pose.radius);
  }
  if (carry.stroke.a != 0 && carry.stroke_width > 0.01f) {
    p.border(pose.x, pose.y, pose.w, pose.h, pose.radius, with_alpha(carry.stroke),
             carry.stroke_width);
  }

  // 文字：字号按**短边**缩放，让“同一块东西长开”时字的相对占比稳定。
  //
  // 为何不固定字号：承接的本质是这个矩形的**尺寸在变**；若字号不动，
  // 从小卡长到整页时字会变成角落里的小字，读起来像两个画面叠在一起。
  if (!carry.text.empty() && carry.text_size > 0.5f) {
    const float from_h = resolve_carry_dim(carry.from.h, 1.0f, 1.0f);
    const float from_w = resolve_carry_dim(carry.from.w, 1.0f, 1.0f);
    const float base_short = std::max(1.0f, std::min(from_w, from_h));
    const float now_short = std::max(1.0f, std::min(pose.w, pose.h));
    const float font = std::clamp(carry.text_size * (now_short / base_short), 6.0f,
                                  std::max(8.0f, pose.h * 0.9f));
    // `Painter::text` 的 `anchor` 0.5 = x 是中心；y 是**基线**。
    // 基线取盒中心 + 0.34 倍字号（cap-height 的一半的近似），大字不会偏低。
    const float baseline = pose.y + pose.h * 0.5f + font * 0.34f;
    p.text(carry.text, font, pose.x + pose.w * 0.5f, baseline, with_alpha(carry.text_color),
           0.5f);
  }
}

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

// 相机进度的实现见 `fx.cpp`（跟 `apply_camera` 同一个单元）：
// 那边不依赖资源库/文本排版，测试可以直接把 `.cpp` 拉进去验证真实实现。

void set_trajectory_sink(std::vector<ElementTrack>* sink) { trajectory_cell().sink = sink; }

void paint_script_shapes(Painter& p) {
  const LayoutEngine& engine = layout_engine();
  if (!engine.loaded()) return;
  const auto& shapes = engine.shapes();
  if (std::getenv("CAIYUN_SHAPES_DEBUG") != nullptr) {
    st::print("[shapes] loaded={} count={}\n", engine.loaded(), shapes.size());
    if (!shapes.empty()) {
      const auto& s = shapes[0];
      st::print("[shapes] #0 kind={} x={:.1f} y={:.1f} w={:.1f} rgba=({},{},{},{})\n", s.kind,
                static_cast<double>(s.x), static_cast<double>(s.y),
                static_cast<double>(s.w), s.color.r, s.color.g, s.color.b, s.color.a);
    }
  }
  if (shapes.empty()) return;
  // ## 坐标系契约（实测踩过一次，两种错法）
  //
  // `shape.x/y/w/h/stroke/h` 全是**设计像素**（kDesignWidth=1920 的坐标系）。
  // 渲染时画布的 `device_scale` 恒为 1（`render.cpp` 用 `Canvas{w,h,1.0f}` 建），
  // 所以 `p.scale()` 也是 1——**它不能当“设计→物理”的换算因子**。
  // 真正的换算是 `物理/设计 = 画布宽 / kDesignWidth`。
  //
  // 两种错法都踩过：
  //   ① 直接传 `shape.x`（设计值当物理值）⇒ 1280×720 下圆心落在 (960,540)
  //      而不是 (640,360)，画面偏右下、图元偏大（实测直径 179 应为 267）。
  //   ② 用 `p.point()`：它按 `device_scale` 乘，而那是 1，结果同①。
  //
  // 所以自己算换算率。正文下面统一用它。
  const float s = p.width() / kDesignWidth;
  const auto S = [s](float v) { return v * s; };

  // 旋转与多边形顶点的几何在 `fx` 里（那里可以写单测，这里只能靠渲帧）。
  const auto polygon_path = [S](const std::vector<Point>& pts) -> Path {
    Path path;
    if (pts.empty()) return path;
    path.move_to(Point{S(pts[0].x), S(pts[0].y)});
    for (std::size_t i = 1; i < pts.size(); i++) {
      path.line_to(Point{S(pts[i].x), S(pts[i].y)});
    }
    path.close();
    return path;
  };

  for (const ScriptShape& shape : shapes) {
    const float alpha = static_cast<float>(shape.color.a) / 255.0f;
    if (alpha <= 0.004f) continue;
    const Color color{shape.color.r, shape.color.g, shape.color.b,
                      static_cast<std::uint8_t>(std::lround(alpha * 255.0f))};
    switch (shape.kind) {
      case 0:
        p.circle(S(shape.x), S(shape.y), S(shape.w * 0.5f), color);
        break;
      case 2:
        p.line(S(shape.x), S(shape.y), S(shape.x2), S(shape.y2), color,
               std::max(0.5f, S(shape.stroke)));
        break;
      case 3:
        if (!shape.text.empty()) {
          // ## 文字与几何图元的坐标契约**不一样**（实测踩两次）
          //
          // · `p.circle` / `p.fill` / `p.fill_path` / `p.line` 收**物理像素**
          //   （它们不再过相机）；
          // · `p.text` 收**设计坐标**——它内部自己 `point(left, top)`，
          //   而过相机时会乘 `scale_`。
          //
          // 于是“一律乘 s”会让文字**双重缩小**（实测：导出帧里所有标签全消失，
          // 因为 56px × 0.5 × 0.5 = 14px 再叠加位置偏移）。
          // 在 1920×1080 下 `scale_ == 1`，两者恰好相同，所以这个差异只在
          // 非设计分辨率下才现形。
          if (shape.rotate != 0.0f) {
            // 文字旋转只能逐字画（一个字符串是一个绘制单元，转不了单字）。
            // 字宽按 0.58 em 估算（与 `spaced_text` 同一口径），够用于倾斜的标签/水印。
            const float font = std::max(1.0f, shape.h);
            const float cx = shape.x;
            const float cy = shape.y + font * 0.5f;
            float advance = 0.0f;
            for (const char ch : shape.text) {
              const Point pt = rotate_point_deg(Point{cx + advance, shape.y},
                                                Point{cx, cy}, shape.rotate);
              p.text(std::string(1, ch), font, pt.x, pt.y, color);
              advance += font * (ch == ' ' ? 0.30f : 0.58f);
            }
          } else {
            p.text(shape.text, std::max(1.0f, shape.h), shape.x, shape.y, color);
          }
        }
        break;
      case 4: {
        // 多边形：点已在设计坐标里。少于 3 个顶点**不画**（而不是退化成一个点/一条线）。
        const std::size_t n = shape.points.size() / 2;
        if (n < 3) break;
        std::vector<Point> pts;
        pts.reserve(n);
        float minx = shape.points[0], maxx = minx;
        float miny = shape.points[1], maxy = miny;
        for (std::size_t i = 0; i < n; i++) {
          const float px = shape.points[i * 2], py = shape.points[i * 2 + 1];
          pts.push_back(Point{px, py});
          minx = std::min(minx, px); maxx = std::max(maxx, px);
          miny = std::min(miny, py); maxy = std::max(maxy, py);
        }
        if (shape.rotate != 0.0f) {
          rotate_points_about_bounds(pts.data(), pts.size(), shape.rotate);
        }
        p.fill_path(polygon_path(pts), color);
        break;
      }
      default:
        if (shape.rotate != 0.0f) {
          // 矩形旋转：四个角各转一次再用多边形填——轴对齐的 `p.fill()`
          // 无论怎么调都画不出斜的矩形。
          std::vector<Point> pts{
              Point{shape.x, shape.y},
              Point{shape.x + shape.w, shape.y},
              Point{shape.x + shape.w, shape.y + shape.h},
              Point{shape.x, shape.y + shape.h},
          };
          rotate_points_about_bounds(pts.data(), pts.size(), shape.rotate);
          p.fill_path(polygon_path(pts), color);
        } else {
          // 矩形：`p.fill` 收物理像素。
          p.fill(S(shape.x), S(shape.y), S(shape.w), S(shape.h), color, S(shape.radius));
        }
        break;
    }
  }
}

void render_scene(st::raster::Surface& target, const Scene& scene, double local_time,
                  double scene_duration, const st::ui::TextPort* text_port,
                  const AssetLibrary* assets, std::string_view asset_basis, int scene_index,
                  int scene_total, std::vector<ElementTrack>* trajectory) {
  Painter p(target, text_port);
  const float duration = static_cast<float>(scene_duration > 0.01 ? scene_duration : 2.0);
  const float time = static_cast<float>(local_time < 0.0 ? 0.0 : local_time);
  const bool profile = std::getenv("CAIYUN_PROFILE") != nullptr;
  const auto now = [] { return st::time::now_ns(); };
  const std::int64_t t0 = profile ? now() : 0;

  // 背景全用**纯色填充**拼（底色条带 + 二维格子铺光 + 渐变暗角带）：整幅渐变填充
  // 在本框架里是逐像素采样（实测 1080p 约 25 ns/px，一次全幅 ≈ 50ms），而纯色
  // fill_rect 走覆盖运行段 + SIMD 混合，同样的观感成本低一个数量级。
  // 轨迹采集：把本幕的轨迹缓冲挂上（外部传了才采）。
  //
  // 为何在这里挂而不往下传参：调用链是 render.cpp → project.cpp → motion.cpp，
  // 中间还有快/慢两条路径与运动模糊的多次采样，逐层传参要改四处签名，
  // 而漏一处就是“某些帧没轨迹”——极难发现。线程局部缓冲更稳。
  //
  // ⚠ 不覆盖已有的 sink：`render.cpp` 会在更外层设置（它知道帧号与 worker），
  // 而 `project.cpp` 调 `render_scene` 时不会转传 trajectory，
  // 所以这里如果写成“无条件赋值”，外层的设置会被 nullptr 抹掉（已踩过）。
  if (trajectory != nullptr) {
    trajectory_cell().sink = trajectory;
  }
  paint_backdrop(p, scene, time, duration);
  // 背景颗粒：紧贴底色之上、所有内容之下。
  //
  // 为何放在这里而不是靠 JS 逐点画：颗粒要几百个点、每帧重算，
  // 跨 C++/JS 边界传一次的开销会盖过渲染本身。它是**场景级质感**，不是内容。
  //
  // 它解决的是「静止段看起来像卡住」——安静不等于冻结。
  // 实测：加入后「变化强度中位数」从 0.006 提到 0.19 量级（对标成熟动效片）。
  if (scene.grain > 0) {
    paint_grain(target, scene.grain, scene.grain_alpha, static_cast<double>(time), 30.0,
                target.device_scale());
  }
  // 素材背景**叠在底色之上、文字之下**：底色仍是兜底（图缺失时有基调可用）。
  if (assets != nullptr) paint_background_image(p, scene, time, duration, *assets, asset_basis);
  // ── 氛围层：背景之上、主体之下 ──
  //
  // 顺序有讲究：光晕/体积光要**压在背景上**才有"从背景里透出来"的感觉；
  // 粒子要在主体之下（浮尘在前景会挡字）。景深作用于背景，
  // 所以它在主体绘制**之前**——主体必须保持锐利（那是观众要读的东西）。
  if (scene.dof > 0.001f) {
    // 只对"背景"做失焦：先把当前画布（底色+素材）模糊，再整体贴回。
    // 主体随后绘制，因此仍是锐的——这就是"景深"在本渲染器里的实现方式。
    const auto blurred = depth_of_field(target, scene.dof);
    target.clear(Color{0, 0, 0, 255});
    st::raster::DrawOptions options{};
    options.opacity = 1.0f;
    target.draw_canvas(blurred,
                       st::math::Rect{0.0f, 0.0f,
                                      static_cast<float>(target.physical_width()),
                                      static_cast<float>(target.physical_height())},
                       options);
  }
  const float dscale = p.scale();
  if (scene.glow > 0.001f) {
    GlowSpec spec{};
    spec.cx = scene.glow_cx;
    spec.cy = scene.glow_cy;
    spec.radius = scene.glow_radius;
    spec.intensity = scene.glow;
    spec.color = scene.accent;
    paint_glow(target, spec, dscale);
  }
  if (scene.god_ray > 0.001f) {
    paint_god_ray(target, scene.glow_cx, scene.glow_cy * 0.35f, scene.god_ray_angle, 0.30f,
                  1.05f, scene.accent, scene.god_ray, dscale);
  }
  if (scene.particles > 0) {
    ParticleField field{};
    field.count = scene.particles;
    field.alpha = scene.particle_alpha;
    field.color = Color{210, 228, 255, 255};
    paint_particles(target, field, static_cast<double>(time), dscale);
  }
  const std::int64_t t1 = profile ? now() : 0;

  // ── 矢量相机：在**绘制期**施加缩放（而非画完再重采样）──
  //
  // ## 为何需要两条相机路径
  //
  // `apply_camera`（整幅重采样）适合**小幅度运镜**：一次重采样、
  // 所有图元（含文字、三维、粒子）一起动，语义一致，代价 8~12ms/帧。
  //
  // 但它的**细节天花板就是源分辨率**：放大 200× 时一个源像素变成 200px
  // 方块，双线性插值把这个方块抹匀 ⇒ 必糊。
  // 参考片 clearing 的 300× 钻穿**完全不糊**（实测抽帧：深钻阶段
  // 月份标签、格子边缘都锐利）——因为它每帧按当前尺度**重新绘制**。
  //
  // 所以给了 `cam_vector: true` 就走矢量路径：`Painter` 的坐标入口
  // 直接乘上相机变换，图元（含文字）按放大后的尺寸**重新光栅化**。
  //
  // ## 矢量路径的代价（为何不让它当默认）
  //
  // · 粒子/三维/辉光这些**屏幕空间效果**不经过 Painter 坐标，不会跟着缩放；
  // · 大幅缩放时超大字号的文字光栅化成本上升；
  // · 旋转与震动的包围盒只能是近似。
  // 所以它是**“钻穿镜头”的专用模式**，而不是运镜的通用替代。
  const bool camera_moves = scene.cam_zoom_from != scene.cam_zoom_to ||
                            scene.cam_rotate != 0.0f || scene.cam_pan_x != 0.0f ||
                            scene.cam_pan_y != 0.0f || scene.cam_shake > 0.0001f ||
                            scene.cam_anchor_x >= 0.0f || scene.cam_anchor_y >= 0.0f;
  const bool vector_camera = camera_moves && scene.cam_vector &&
                             scene.cam_rotate == 0.0f && scene.cam_shake <= 0.0001f;
  if (vector_camera) {
    const float cam_progress =
        camera_progress(time, duration, scene.cam_window_start, scene.cam_window_end);
    const float base_zoom =
        scene.cam_zoom_from + (scene.cam_zoom_to - scene.cam_zoom_from) * cam_progress;
    VectorCamera vcam{};
    vcam.zoom = std::max(0.02f, base_zoom);
    // 锚点从归一化坐标换到设计坐标（`cam_anchor` 是 0~1，负值 = 画面中心）。
    vcam.anchor_x = scene.cam_anchor_x >= 0.0f ? scene.cam_anchor_x * kDesignWidth
                                               : kDesignWidth * 0.5f;
    vcam.anchor_y = scene.cam_anchor_y >= 0.0f ? scene.cam_anchor_y * kDesignHeight
                                               : kDesignHeight * 0.5f;
    p.set_camera(vcam);
  }

  switch (scene.kind) {
    case SceneKind::Title: paint_title(p, scene, time); break;
    case SceneKind::Terminal: paint_terminal(p, scene, time, duration); break;
    case SceneKind::Triptych: paint_triptych(p, scene, time, assets, asset_basis); break;
    case SceneKind::Metrics: paint_metrics(p, scene, time); break;
    case SceneKind::Outro: paint_outro(p, scene, time); break;
    case SceneKind::Custom: break;   // 完全由以下图层绘制（无内置模板）
  }
  // 通用图层：任何场景都可叠加（含 Custom）。
  if (!scene.layers.empty()) paint_layers(p, scene, time, assets, asset_basis, trajectory);
  // ── 脚本临时图元（本帧才存在的形状）──
  //
  // 这是“结构变化”的唯一通道（见 `ScriptShape` 的说明）：
  // 点分裂成四个、珠子沿曲线跑、批量错峰——都靠它。
  //
  // 画在图层**之上**：脚本图元通常是前景装饰（珠子、分裂出的点、连线），
  // 盖在上面才对；要盖在下面就把它们写成背景图层的 `shapes`。
  paint_script_shapes(p);
  // 包装层统一在这里加：所有场景共用同一套识标/结构/页脚。
  //
  // 放这里而不是各场景内各调一次，是因为包装必须**逐帧一致**——
  // 各写一遍必然漂移（实测：只有 title 有品牌角标、其余四个场景裸着，
  // 连起来看就是不统一）。
  // 光扫在主体**之上**（它是"打过来的光"，要能扫过文字）
  if (scene.light_sweep > 0.001f) {
    // 每段内扫一次：进度按段内时间归一到 0~1（前 45% 时间完成扫过）。
    const float sweep_t = clamp01f(time / std::max(0.2f, duration * 0.45f));
    paint_light_sweep(target, sweep_t, 0.16f, scene.accent, scene.light_sweep, p.scale());
  }
  // ── 运镜层：只作用在**内容**上，包装层不参与 ──
  //
  // 为何包装层（识标/页脚/进度/四角标）不参与运镜：它们是"画面上的固定装置"，
  // 就像电视转播里的台标——镜头推进时台标不会跟着放大。
  // 实测把整幅画面一起变换后，识标被放大、页脚被推出画面、四周出现黑边
  // （拉远类运镜尤其明显：边缘直接露底）。
  //
  // 所以顺序是：内容 → 运镜 → 把运镜结果贴回 → 包装层。
  //
  // 矢量路径（`cam_vector`）下**不做这一步**：缩放已经在绘制期施加了，
  // 再做一次整幅重采样等于把刚重绘出来的锐利细节又抹一遍。
  if (camera_moves && !vector_camera) {
    CameraMove move{};
    move.zoom_from = scene.cam_zoom_from;
    move.zoom_to = scene.cam_zoom_to;
    move.rotate_deg = scene.cam_rotate;
    move.pan_x = scene.cam_pan_x;
    move.pan_y = scene.cam_pan_y;
    move.shake = scene.cam_shake;
    move.anchor_x = scene.cam_anchor_x;
    move.anchor_y = scene.cam_anchor_y;
    if (scene.cam_zoom_curve == "expo_in") {
      move.zoom_curve = ZoomCurve::ExpoIn;
    } else if (scene.cam_zoom_curve == "expo_out") {
      move.zoom_curve = ZoomCurve::ExpoOut;
    }
    // 相机进度：默认整段线性；给了窗口就只为窗口那段推进（窗口外夹在 0/1）。
    const float cam_progress =
        camera_progress(time, duration, scene.cam_window_start, scene.cam_window_end);
    const auto moved = apply_camera(target, move, cam_progress, static_cast<double>(time),
                                    target.device_scale());
    target.clear(Color{0, 0, 0, 255});
    st::raster::DrawOptions cam_options{};
    cam_options.opacity = 1.0f;
    target.draw_canvas_at(moved, 0, 0, cam_options);
  }
  // 包装层在运镜**之后**画：它固定在画面上，不随镜头动。
  //
  // `scene.wrap` 可关掉它：包装层（识标/四角标/页脚/进度）是“宣传片”的语言，
  // 而有些片要的是**纯画面**（例如“桌面上一块扁平块”这种模拟界面的镜头）——
  // 那时四角标记与 `01 / 03` 反而跳戏。默认 true 保持旧行为不变。
  if (scene.wrap) paint_wrap(p, scene, time, scene_index, scene_total);
  if (profile) {
    st::print("[profile] bg={:.1f}ms content={:.1f}ms\n",
              static_cast<double>(t1 - t0) / 1.0e6, static_cast<double>(now() - t1) / 1.0e6);
  }
}

}  // namespace caiyun

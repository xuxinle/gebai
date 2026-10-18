/// 裁云 · 影像特效层实现（见 `fx.hpp`）。

#include "fx.hpp"

#include <algorithm>
#include <cmath>

namespace caiyun {

auto hash01(std::uint32_t a, std::uint32_t b) -> float {
  // 32 位整数哈希（Wang hash 的变体）→ 取低位做 [0,1)。
  //
  // 选它的理由：够快（几个乘加）、无状态、雪崩性够好（相邻 index 得到的值
  // 在视觉上不相关）。**不要**换成 `% N` 之类的取模——相邻 index 会得到相邻值，
  // 粒子会排成整齐的行列（一眼假）。
  std::uint32_t h = a * 0x9E3779B1u ^ (b + 0x85EBCA77u + (a << 6) + (a >> 2));
  h ^= h >> 16;
  h *= 0x7FEB352Du;
  h ^= h >> 15;
  h *= 0x846CA68Bu;
  h ^= h >> 16;
  return static_cast<float>(h & 0x00FFFFFFu) / 16777216.0f;
}

auto depth_of_field(const st::raster::Canvas& source, float strength) -> st::raster::Canvas {
  const int width = source.physical_width();
  const int height = source.physical_height();
  st::raster::Canvas out{width, height, source.device_scale()};
  if (width <= 0 || height <= 0) return out;
  if (strength <= 0.002f) {
    // 0 = 不处理：直接拷一份（调用方拿到同形状画布，不必分支）。
    st::raster::DrawOptions options{};
    options.opacity = 1.0f;
    out.draw_canvas(source, st::math::Rect{0.0f, 0.0f, static_cast<float>(width),
                                           static_cast<float>(height)},
                    options);
    return out;
  }

  // 降采样 → 双线性放大 = 低通滤波（廉价"失焦"）。
  //
  // 采样倍率随强度在 2~6 之间：强度越大、采样越粗，放大后越糊。
  // 只降一次不循环（循环降采样会让画面变成色块，不像光学虚化）。
  const float sample_div = 2.0f + strength * 4.0f;
  const int small_w = std::max(4, static_cast<int>(static_cast<float>(width) / sample_div));
  const int small_h = std::max(4, static_cast<int>(static_cast<float>(height) / sample_div));
  st::raster::Canvas small{small_w, small_h, source.device_scale()};
  st::raster::DrawOptions options{};
  options.opacity = 1.0f;
  small.draw_canvas(source,
                    st::math::Rect{0.0f, 0.0f, static_cast<float>(small_w),
                                   static_cast<float>(small_h)},
                    options);
  out.draw_canvas(small,
                  st::math::Rect{0.0f, 0.0f, static_cast<float>(width),
                                 static_cast<float>(height)},
                  options);
  return out;
}

void paint_particles(st::raster::Canvas& target, const ParticleField& field, double time,
                     float device_scale) {
  if (field.count <= 0 || field.alpha <= 0.004f) return;
  const float width = static_cast<float>(target.physical_width());
  const float height = static_cast<float>(target.physical_height());
  if (width < 1.0f || height < 1.0f) return;
  const float scale = device_scale > 0.01f ? device_scale : 1.0f;
  const float t = static_cast<float>(time);

  for (int i = 0; i < field.count; ++i) {
    const auto index = static_cast<std::uint32_t>(i);
    // 每个粒子的属性都用**独立哈希**取值：这样粒子可以任意裁减而不影响其余粒子。
    const float base_x = hash01(index, 1u);
    const float base_y = hash01(index, 2u);
    const float size_mix = hash01(index, 3u);
    const float depth_mix = hash01(index, 4u);
    const float phase = hash01(index, 5u) * 6.2831853f;
    const float flicker_speed = 0.35f + hash01(index, 6u) * 0.9f;

    // 漂移：包络取模让粒子从对侧循环进入（画面不会"用完"）。
    const float drift_x = field.drift_x * t * (0.4f + depth_mix * 1.2f);
    const float drift_y = field.drift_y * t * (0.4f + depth_mix * 1.2f);
    float x = (base_x * width + drift_x * scale);
    float y = (base_y * height + drift_y * scale);
    x = std::fmod(x, width);
    if (x < 0.0f) x += width;
    y = std::fmod(y, height);
    if (y < 0.0f) y += height;

    // 尺寸与亮度都随"深度"变：小且淡 = 远。
    const float depth = 1.0f - field.depth_factor * depth_mix;   // 0.15~1
    const float size = (field.size_min + (field.size_max - field.size_min) * size_mix) *
                       depth * scale;
    if (size < 0.35f) continue;
    // 闪烁：正弦调制（不是随机跳变——跳变会像噪点）。
    const float flicker = 0.55f + 0.45f * std::sin(t * flicker_speed * 3.1f + phase);
    const float alpha = field.alpha * depth * flicker;
    if (alpha <= 0.004f) continue;

    const auto a = static_cast<std::uint8_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
    st::raster::DrawOptions options{};
    options.opacity = 1.0f;
    target.fill_circle(st::math::Point{x, y}, size * 0.5f,
                       st::raster::Paint::solid(Color{field.color.r, field.color.g,
                                                      field.color.b, a}),
                       options);
    // 亮核：近处的粒子加一个小白点，看起来像"有光的尘埃"而不是纯色圆点。
    if (depth > 0.72f && size > 1.6f * scale) {
      const auto core = static_cast<std::uint8_t>(std::clamp(alpha * 1.35f, 0.0f, 1.0f) * 255.0f);
      target.fill_circle(st::math::Point{x, y}, size * 0.22f,
                         st::raster::Paint::solid(Color{255, 255, 255, core}), options);
    }
  }
}

void paint_glow(st::raster::Canvas& target, const GlowSpec& spec, float device_scale,
                float alpha_mul) {
  if (spec.intensity <= 0.004f || alpha_mul <= 0.004f) return;
  (void)device_scale;
  const float width = static_cast<float>(target.physical_width());
  const float height = static_cast<float>(target.physical_height());
  const float short_side = std::min(width, height);
  const float radius = spec.radius * short_side;
  if (radius < 1.0f) return;
  const float cx = spec.cx * width;
  const float cy = spec.cy * height;

  // ⚠ 这里**不能**用"多层同心圆叠出径向渐变"：
  // 每层圆的边缘都会留下一个可见的环（实测：14 层叠出来是一圈圈同心圆环，
  // 一眼就看出是程序画的，比不加光晕还难看）。
  //
  // 正确做法是逐像素算径向衰减——但要**限制在光晕的包围盒内**：
  // 全画面逐像素采样在 1080p 是 ~50ms/帧，只算光晕范围（通常 60% 面积）
  // 可以接受，而且光晕外本来就是纯透明、无需计算。
  const int x0 = std::max(0, static_cast<int>(cx - radius));
  const int x1 = std::min(target.physical_width(), static_cast<int>(cx + radius) + 1);
  const int y0 = std::max(0, static_cast<int>(cy - radius));
  const int y1 = std::min(target.physical_height(), static_cast<int>(cy + radius) + 1);
  if (x1 <= x0 || y1 <= y0) return;

  const float inv_r2 = 1.0f / (radius * radius);
  const float stretch = std::max(0.25f, spec.stretch);
  for (int y = y0; y < y1; ++y) {
    const float dy = (static_cast<float>(y) + 0.5f - cy);
    for (int x = x0; x < x1; ++x) {
      const float dx = (static_cast<float>(x) + 0.5f - cx) / stretch;
      const float d2 = (dx * dx + dy * dy) * inv_r2;
      if (d2 >= 1.0f) continue;
      // 平滑衰减：1-d² 的平方再做一次缓和，接近真实光晕的柔和边缘。
      const float falloff = (1.0f - d2);
      const float value = falloff * falloff * falloff;
      const float alpha = spec.intensity * value * alpha_mul;
      if (alpha <= 0.002f) continue;
      const auto a = static_cast<std::uint8_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
      // 单像素 fill_rect：走的是**混合**（不是覆盖），与其它图元语义一致。
      target.fill_rect(st::math::Rect{static_cast<float>(x), static_cast<float>(y), 1.0f, 1.0f},
                       st::raster::Paint::solid(Color{spec.color.r, spec.color.g, spec.color.b, a}));
    }
  }
}

void paint_light_sweep(st::raster::Canvas& target, float progress, float width_ratio,
                       Color color, float intensity, float device_scale) {
  if (progress <= 0.0f || progress >= 1.0f || intensity <= 0.004f) return;
  const float width = static_cast<float>(target.physical_width());
  const float height = static_cast<float>(target.physical_height());
  const float band_w = width * std::max(0.02f, width_ratio);
  // 光带从画面左外扫到右外：用进度的斜角，让光带斜着过去（垂直扫像"加载条"）。
  const float cx = -band_w + (width + band_w * 2.0f) * progress;
  constexpr int kSteps = 9;
  st::raster::DrawOptions options{};
  options.opacity = 1.0f;
  for (int i = 0; i < kSteps; ++i) {
    const float k = static_cast<float>(i) / static_cast<float>(kSteps - 1);
    // 中间亮、两边淡：k 从 0→1 走过光带宽度。
    const float taper = std::sin(k * 3.14159265f);
    const float alpha = intensity * taper * taper * 0.16f * device_scale;
    if (alpha <= 0.003f) continue;
    const auto a = static_cast<std::uint8_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
    const float x = cx + (k - 0.5f) * band_w;
    // 斜切：用平行四边形（两端错开）表现"扫过"的方向感。
    st::raster::Path path;
    path.move_to(st::math::Point{x - band_w * 0.06f, 0.0f});
    path.line_to(st::math::Point{x + band_w * 0.06f, 0.0f});
    path.line_to(st::math::Point{x + band_w * 0.06f + band_w * 0.22f, height});
    path.line_to(st::math::Point{x - band_w * 0.06f + band_w * 0.22f, height});
    path.close();
    target.fill_path(path,
                     st::raster::Paint::solid(Color{color.r, color.g, color.b, a}), options);
  }
}

auto apply_camera(const st::raster::Canvas& source, const CameraMove& move, float progress,
                  double time, float device_scale) -> st::raster::Canvas {
  const int width = source.physical_width();
  const int height = source.physical_height();
  st::raster::Canvas out{width, height, source.device_scale()};
  if (width <= 0 || height <= 0) return out;
  const float t = std::clamp(progress, 0.0f, 1.0f);
  (void)device_scale;

  // 缩放/平移/旋转按**整段进度**线性推进；只有震动按时间衰减。
  // 用缓动会让"镜头运动"变成"动画"——镜头是匀速的（这是摄影与动画的区别）。
  const float zoom = move.zoom_from + (move.zoom_to - move.zoom_from) * t;
  const float pan_x = move.pan_x * t * static_cast<float>(width);
  const float pan_y = move.pan_y * t * static_cast<float>(height);
  const float rotate = move.rotate_deg * t * 3.14159265f / 180.0f;

  // 冲击震动：正弦 × 指数衰减（同一时刻两次渲染相同，无随机）。
  float shake_x = 0.0f;
  float shake_y = 0.0f;
  if (move.shake > 0.0001f) {
    const float decay = std::exp(-move.shake_decay * static_cast<float>(time));
    if (decay > 0.002f) {
      const float short_side = static_cast<float>(std::min(width, height));
      const float amp = move.shake * short_side * decay;
      const float phase = static_cast<float>(time) * move.shake_hz * 6.2831853f;
      // 两个不同频率叠加：单一频率像"整体晃动"，叠加后像"被撞了一下"。
      shake_x = amp * (std::sin(phase) * 0.7f + std::sin(phase * 2.37f) * 0.3f);
      shake_y = amp * (std::cos(phase * 1.13f) * 0.6f + std::sin(phase * 3.11f) * 0.4f);
    }
  }

  const float cx = static_cast<float>(width) * 0.5f + pan_x + shake_x;
  const float cy = static_cast<float>(height) * 0.5f + pan_y + shake_y;

  // ⚠ 为什么"逐像素逆映射"而不是"画一个旋转四边形再填充"：
  //
  // 本框架的 `fill_path` 只接受 `Paint`（纯色/线性渐变），**不接受图像图案**；
  // `draw_canvas` 只支持轴对齐的目标矩形（没有旋转，也不支持源矩形裁切）。
  // 所以旋转只能自己做：对每个输出像素，算出它在**源图里的位置**再采样。
  // 这是标准做法，也顺带把缩放与平移合并在同一次变换里——
  // 只重采样一次，不会像"先缩放再旋转"那样糊两遍。
  const float inv_zoom = 1.0f / std::max(0.05f, zoom);
  const float cos_r = std::cos(rotate);
  const float sin_r = std::sin(rotate);
  const auto& src_pixels = source.pixels();   // 预乘 RGBA（uint32）
  if (src_pixels.empty()) return out;
  auto dst_pixels = out.pixels();
  const float src_w = static_cast<float>(source.physical_width());
  const float src_h = static_cast<float>(source.physical_height());

  for (int y = 0; y < height; ++y) {
    const float dy = static_cast<float>(y) + 0.5f - cy;
    for (int x = 0; x < width; ++x) {
      const float dx = static_cast<float>(x) + 0.5f - cx;
      // 逆旋转 + 逆缩放 → 源坐标（以画面中心为原点）
      const float sx = (dx * cos_r + dy * sin_r) * inv_zoom + src_w * 0.5f - 0.5f;
      const float sy = (-dx * sin_r + dy * cos_r) * inv_zoom + src_h * 0.5f - 0.5f;
      if (sx < -0.5f || sy < -0.5f || sx >= src_w || sy >= src_h) continue;   // 画面外留空

      // 双线性采样（4 邻域加权）。
      const int x0 = static_cast<int>(std::floor(sx));
      const int y0 = static_cast<int>(std::floor(sy));
      const float fx = sx - static_cast<float>(x0);
      const float fy = sy - static_cast<float>(y0);
      const int cx0 = std::clamp(x0, 0, source.physical_width() - 1);
      const int cy0 = std::clamp(y0, 0, source.physical_height() - 1);
      const int cx1 = std::clamp(x0 + 1, 0, source.physical_width() - 1);
      const int cy1 = std::clamp(y0 + 1, 0, source.physical_height() - 1);
      const auto i00 = static_cast<std::size_t>(cy0) * static_cast<std::size_t>(src_w) +
                       static_cast<std::size_t>(cx0);
      const auto i10 = static_cast<std::size_t>(cy0) * static_cast<std::size_t>(src_w) +
                       static_cast<std::size_t>(cx1);
      const auto i01 = static_cast<std::size_t>(cy1) * static_cast<std::size_t>(src_w) +
                       static_cast<std::size_t>(cx0);
      const auto i11 = static_cast<std::size_t>(cy1) * static_cast<std::size_t>(src_w) +
                       static_cast<std::size_t>(cx1);
      const auto unpack = [](std::uint32_t v, int shift) {
        return static_cast<float>((v >> shift) & 0xFFu);
      };
      const std::uint32_t p00 = src_pixels[i00];
      const std::uint32_t p10 = src_pixels[i10];
      const std::uint32_t p01 = src_pixels[i01];
      const std::uint32_t p11 = src_pixels[i11];
      const float w00 = (1.0f - fx) * (1.0f - fy);
      const float w10 = fx * (1.0f - fy);
      const float w01 = (1.0f - fx) * fy;
      const float w11 = fx * fy;
      std::uint32_t blended = 0;
      for (int shift = 0; shift < 32; shift += 8) {
        const float value = unpack(p00, shift) * w00 + unpack(p10, shift) * w10 +
                            unpack(p01, shift) * w01 + unpack(p11, shift) * w11;
        blended |= static_cast<std::uint32_t>(std::clamp(value, 0.0f, 255.0f) + 0.5f) << shift;
      }
      dst_pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                 static_cast<std::size_t>(x)] = blended;
    }
  }
  return out;
}

void paint_god_ray(st::raster::Canvas& target, float origin_x, float origin_y, float angle,
                   float width_ratio, float length_ratio, Color color, float intensity,
                   float device_scale) {
  if (intensity <= 0.004f) return;
  (void)device_scale;   // 几何全按对角线比例算，与设备缩放无关
  const float width = static_cast<float>(target.physical_width());
  const float height = static_cast<float>(target.physical_height());
  const float diagonal = std::sqrt(width * width + height * height);
  const float beam_w = diagonal * std::max(0.01f, width_ratio);
  const float beam_len = diagonal * std::max(0.05f, length_ratio);
  const float ox = origin_x * width;
  const float oy = origin_y * height;
  const float dx = std::cos(angle);
  const float dy = std::sin(angle);
  // 垂直于光柱方向的单位向量（用于把光柱展开成矩形）。
  const float px = -dy;
  const float py = dx;

  // 沿横向叠若干条窄带：边缘淡、中心亮——这就是"边缘柔和"的来历。
  constexpr int kSteps = 11;
  st::raster::DrawOptions options{};
  options.opacity = 1.0f;
  for (int i = 0; i < kSteps; ++i) {
    const float k = static_cast<float>(i) / static_cast<float>(kSteps - 1);
    const float offset = (k - 0.5f) * beam_w;
    const float taper = std::sin(k * 3.14159265f);
    // 体积光的能量沿长度衰减（近光源亮、远端淡）。
    const float alpha = intensity * taper * taper * 0.075f;
    if (alpha <= 0.003f) continue;
    const auto a = static_cast<std::uint8_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
    const float band = beam_w / static_cast<float>(kSteps) * 1.6f;
    st::raster::Path path;
    path.move_to(st::math::Point{ox + px * (offset - band * 0.5f),
                                 oy + py * (offset - band * 0.5f)});
    path.line_to(st::math::Point{ox + px * (offset + band * 0.5f),
                                 oy + py * (offset + band * 0.5f)});
    path.line_to(st::math::Point{ox + dx * beam_len + px * (offset + band * 0.5f),
                                 oy + dy * beam_len + py * (offset + band * 0.5f)});
    path.line_to(st::math::Point{ox + dx * beam_len + px * (offset - band * 0.5f),
                                 oy + dy * beam_len + py * (offset - band * 0.5f)});
    path.close();
    target.fill_path(path,
                     st::raster::Paint::solid(Color{color.r, color.g, color.b, a}), options);
  }
}

}  // namespace caiyun

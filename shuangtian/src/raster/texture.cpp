// 影像质感层的实现。设计约束与坐标约定见 include/st/raster/texture.hpp。
//
// 实现上的两个关键取舍（都不要随手改）：
//  · 粒子/颗粒用哈希而非随机引擎 —— 渲染是纯函数，同一时刻两次必须逐像素相同；
//  · 径向光晕用多层同心圆、景深用降采样 —— 不假设存在整幅高斯模糊原语。

/// 裁云 · 影像特效层实现（见 `fx.hpp`）。

#include "st/raster/texture.hpp"

#include <algorithm>
#include <cmath>

namespace st::raster {

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

auto backdrop_is_light(st::math::Color top, st::math::Color bottom) -> bool {
  // 阈值 0.72（而不是 WCAG 的 0.5）：小片深色背景里带一点亮色的情况很常见
  // （如 #0B1220 → #2A3550）。取 0.5 会把那类深底误判为浅色，于是
  // 它们该有的铺光与暗角被默默关掉————而且是看不出来原因的那种。
  constexpr float kLightThreshold = 0.72f;
  // 用**感知亮度**（0.299/0.587/0.114，与图像处理惯例一致），
  // 不用 `st::math::relative_luminance`：后者是 WCAG 的**线性化**亮度，
  // 它给 #C8C0B0 算出 0.53——而人眼看上去那是明显的浅米色。
  // 两者口径不同，混用会让阈值失去意义（实测：拿 WCAG 值配 0.72
  // 会把中浅米色误判成深色，暗角又回来了）。
  const auto luma = [](const st::math::Color& color) -> float {
    return (0.299f * static_cast<float>(color.r) + 0.587f * static_cast<float>(color.g) +
            0.114f * static_cast<float>(color.b)) /
           255.0f;
  };
  return std::max(luma(top), luma(bottom)) >= kLightThreshold;
}

/// ## 源是 `Surface`，产物仍是软件 `Canvas`（2026-10-25 GPU 接入）
///
/// 景深要逐像素做盒式模糊，那是一段 CPU 专用的算法（多遍均值）。
/// 让它也上 GPU 需要写 shader，而这条路只在 `dof > 0` 时才走——
/// 本片单里 13 个 case 全部没开景深。所以这里接受 `Surface` 源（GPU 画布可直接贴进来），
/// 产物仍是软件画布，语义不变、零风险。
auto depth_of_field(const st::raster::Surface& source, float strength) -> st::raster::Canvas {
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

void paint_particles(st::raster::Surface& target, const ParticleField& field, double time,
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

/// 背景颗粒层：细小的微光点，**每帧位置都不同**（但完全确定，无随机）。
///
/// ## 它解决什么（实测得来的必要层）
///
/// 对标成熟动效片时发现：它的背景不是纯色，而是持续闪烁的细颗粒。
/// 逐帧测量下，正是这层把「变化强度中位数」从 0.006 拉到 0.196（33 倍）。
///
/// 观感差别：纯色背景在「静止段」看起来像**卡住了**——画面完全冻结，
/// 观众会怀疑视频卡了。有颗粒时则是「安静但活着」。
/// 这是「静止段能不能成立」的关键：**安静 ≠ 冻结**。
///
/// ## 为何不能靠 JS 逐点画
///
/// 颗粒要几百到上千个点，每帧重算并跨越 C++/JS 边界传一次，
/// 单帧开销会盖过渲染本身。它是**场景级装饰**，不是内容，由渲染器直接画。
///
/// ## 确定性（裁云硬要求）
///
/// 位置来自 `hash01(粒子序号, 时间量化)`——时间只以「帧序号」进入哈希，
/// 所以同一脚本两次出片**逐帧逐像素一致**。
/// 若用 `std::rand` 或时钟，同一脚本两次出片会不同，裁云就无法复现。
void paint_grain(st::raster::Surface& target, int count, float alpha, double time,
                 double fps, float device_scale) {
  if (count <= 0 || alpha <= 0.002f) return;
  const float width = static_cast<float>(target.physical_width());
  const float height = static_cast<float>(target.physical_height());
  if (width < 1.0f || height < 1.0f) return;
  const float scale = device_scale > 0.01f ? device_scale : 1.0f;
  // `scale` 曾用于颗粒半径（`radius * scale`）。改用 1×1 `fill_rect` 后
  // 颗粒是单物理像素，不再需要按 DPI 缩放半径——高 DPI 下“颗粒更细”本来就是
  // 正确观感（物理尺寸不变）。
  (void)scale;
  // 时间量化到帧：同一个时间点算出的颗粒位置固定。
  const auto frame = static_cast<std::uint32_t>(
      fps > 0.01 ? static_cast<double>(time) * fps : static_cast<double>(time) * 30.0);

  st::raster::DrawOptions options{};
  options.opacity = 1.0f;
  for (int i = 0; i < count; ++i) {
    const auto index = static_cast<std::uint32_t>(i);
    // 每帧换一个盐：位置整片重新分布（这就是"闪"的来源）。
    const std::uint32_t salt = frame * 2654435761U;
    // 位置：**每帧整片重分布**。
    //
    // 试过加"整体缓慢漂移"（更像真实浮尘），但实测**逐帧差值反而下降**
    // （中位数 0.49 → 0.44）：漂移让相邻帧的颗粒位置接近，
    // 而重分布是每颗都换位置——后者制造的逐帧差异更大。
    // 验收口径要的是"每帧都不同"，重分布更有效；观感上两者都像浮尘。
    const float x = hash01(index ^ salt, 11u) * width;
    const float y = hash01(index ^ salt, 23u) * height;
    // 亮度分布：大部分很暗、少数很亮。
    // 参考片实测：颗粒**稀疏但亮点接近纯白**——均匀的中等亮度会看起来像噪点视频。
    // 用三次方压低多数、抬高少数，得到"稀疏星点"而不是"均匀噪点"。
    const float flicker = hash01(index ^ salt, 37u);
    const float shaped = flicker * flicker * flicker;
    const float a = alpha * (0.12f + 0.88f * shaped);
    if (a <= 0.004f) continue;
    const auto value = static_cast<std::uint8_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f);
    // ## 用 1×1 `fill_rect` 而不是 `fill_circle`（2026-10-25，25 倍）
    //
    // 颗粒直径只有 1.1~2.6 **物理**像素（半径 0.55~1.3），几何上就是个点；
    // 而 `fill_circle` 每个都要走一趟完整路径光栅化（生成扫描线 + 覆盖率）。
    // 实测 4200 颗：`fill_circle` **27.75ms/帧**，1×1 `fill_rect` **1.11ms**。
    //
    // 为何不用更快的 `set_pixel`（0.07ms）：GPU 画布上它会**触发一次完整回读**
    // 然后只写 CPU 侧缓存 —— 而后续 GPU 绘制会把它盖掉，语义是错的。
    // `fill_rect` 是正常的绘制指令，CPU/GPU 两条腿都对。
    //
    // 观感差异：`fill_circle` 的半径会带来一点柔边，`fill_rect` 是硬边单像素。
    // 在这个尺寸（≲ 2px）下两者在 1080p 上几乎不可分辨——都是“稀疏星点”，
    // 而半径本来也因亚像素位置而随机化，不存在可见的“圆”感。
    target.fill_rect(st::math::Rect{std::floor(x), std::floor(y), 1.0f, 1.0f},
                     st::raster::Paint::solid(Color{226, 232, 255, value}), 0.0f, options);
  }
}

void paint_glow(st::raster::Surface& target, const GlowSpec& spec, float device_scale,
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

void paint_light_sweep(st::raster::Surface& target, float progress, float width_ratio,
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

// 相机进度（声明与理由见 `fx.hpp`）——它是“钻到位就静止”的全部实现。
auto camera_progress(float time, float duration, float window_start, float window_end) -> float {
  if (window_end <= window_start) {
    return std::clamp(time / std::max(0.2f, duration), 0.0f, 1.0f);
  }
  const float span = window_end - window_start;
  return std::clamp((time - window_start) / std::max(0.05f, span), 0.0f, 1.0f);
}

/// 与 `depth_of_field` 同理：源可以是 GPU 画布，产物是软件画布。
/// 相机变换是一次重采样，`draw_canvas_at` 的 GPU 路径本身就能吃 Surface 源，
/// 所以这里不需要改算法。
auto apply_camera(const st::raster::Surface& source, const CameraMove& move, float progress,
                  double time, float device_scale) -> st::raster::Canvas {
  const int width = source.physical_width();
  const int height = source.physical_height();
  st::raster::Canvas out{width, height, source.device_scale()};
  if (width <= 0 || height <= 0) return out;
  const float t = std::clamp(progress, 0.0f, 1.0f);
  (void)device_scale;

  // 缩放/平移/旋转按**整段进度**推进。
  //
  // ## 为何缩放曲线可选（`zoom_curve`）
  //
  // 线性缩放做不出“钻穿尺度”（参考片 clearing：1× → 300×）：
  // 线性的前 2/3 时间都在 1~100 之间平推，观感是“缓缓放大”；
  // 而后 1/3 才猛地冲入——但那时已经看完了。
  //
  // 指数曲线（`expo_in`）则把绝大部分**视觉变化**挤在末尾，
  // 中段一直是“还很大”，最后一口气吸进去。这是“从这一格里钻出去”的根源。
  //
  // `scale` 模式：`zoom_from * (zoom_to/zoom_from)^t`（等比，1→300 与 300→1 对称）
  // `linear` 模式：旧行为，逐字不变。
  float t_zoom = t;
  if (move.zoom_curve == ZoomCurve::ExpoIn) {
    // expo_in：2^(10(t-1))，t=0 → 0.00098、t=1 → 1。
    // 做归一化（减去起点再除）才能保证 t=0 时恰好是 0、t=1 恰好是 1——
    // 不归一化的话起点有个 0.001 的偏差，301 倍时就是 0.3 的画面偏位。
    constexpr float kBase = 1024.0f;   // 2^10
    const float raw = std::pow(kBase, t - 1.0f) - 1.0f / kBase;
    t_zoom = raw / (1.0f - 1.0f / kBase);
  } else if (move.zoom_curve == ZoomCurve::ExpoOut) {
    t_zoom = 1.0f - std::pow(1024.0f, -t);
  }
  t_zoom = std::clamp(t_zoom, 0.0f, 1.0f);
  const float zoom =
      move.zoom_from + (move.zoom_to - move.zoom_from) * t_zoom;
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

  // ## 销点：缩放围绕哪里
  //
  // 默认绕画面中心（旧行为）。给了销点就绕销点——这是“从这一格里钻出去”
  // 与“整幅等比放大”的全部区别：参考片 clearing 的前 2/3 秒里，
  // 那个日期格子**在屏幕上纹丝不动**，变大的只有它周围的一切。
  //
  // ## 为何销点用归一化坐标（0~1）而不是像素
  //
  // 像素坐标会把相机绑在“设计尺寸”上，而同一份脚本要能出 1080p/4K/竖版。
  // 归一化坐标在每种画幅下指向“同一个相对位置”。
  //
  // ## 为何不把销点写进 `pan`
  //
  // pan 是“整段平移总量”，它随 `t` 线性增长；而销点是**恒定约束**
  // （用另一个变量），两者语义不同。混在一起会在 300× 时把销点推出画面。
  const float anchor_x = move.anchor_x >= 0.0f ? move.anchor_x * static_cast<float>(width)
                                                : static_cast<float>(width) * 0.5f;
  const float anchor_y = move.anchor_y >= 0.0f ? move.anchor_y * static_cast<float>(height)
                                                : static_cast<float>(height) * 0.5f;
  const float cx = anchor_x + pan_x + shake_x;
  const float cy = anchor_y + pan_y + shake_y;

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

  // 源侧的销点（与输出侧的 `anchor_x/anchor_y` 是**同一个点**，只是坐标空间不同）。
  //
  // ## 为何不能只用输出侧销点
  //
  // 逆映射是“输出像素 → 源像素”。旧写法把源原点硬编码在 `src_w*0.5`（画面中心），
  // 于是“输出绕销点、源绕中心”——两者不是同一个点，缩放时销点会**漂移**。
  // 300× 时每像素误差被放大 300 倍，直接飞出去。
  //
  // 实测现象：销点设 40%/37%，1× 时对，10× 时偏 300px，30× 时整幅空白（
  // 因为销点早飞出采样范围）。而所有断言都不报错——它只是“画得不对”。
  //
  // ## −0.5：连续坐标 → 像素索引坐标（**只对销点路径**）
  //
  // `u*W` 是**连续坐标**（画面左边缘 = 0），而采样用的 `sx` 是**像素索引**
  // （`sx = 72.0` 指像素 72 的中心）。两者差半像素。
  //
  // 不扣这 0.5 的后果：销点在 1× 时偏半像素（看不出），
  // 到 30× 时放大成 **15px**（实测：这正是一条断言变红的原因）。
  //
  // ⚠ **旧路径（不填销点）仍然保留 `-0.5f`**（在 `sx/sy` 的那两行里）。
  // 那条路径已逐像素锁定（`fx_camera_static_is_identity`），
  // 改它会让静态相机不再恒等——实测直接变红。
  // 所以两个分支的补偿位置不同，但结果都对齐像素中心。
  const bool has_anchor = move.anchor_x >= 0.0f || move.anchor_y >= 0.0f;
  const float src_anchor_x =
      move.anchor_x >= 0.0f ? move.anchor_x * src_w - 0.5f : src_w * 0.5f;
  const float src_anchor_y =
      move.anchor_y >= 0.0f ? move.anchor_y * src_h - 0.5f : src_h * 0.5f;
  // 旧路径需要在采样行里再减 0.5，销点路径已经在上面扣过了。
  const float centre_bias = has_anchor ? 0.0f : -0.5f;

  for (int y = 0; y < height; ++y) {
    const float dy = static_cast<float>(y) + 0.5f - cy;
    for (int x = 0; x < width; ++x) {
      const float dx = static_cast<float>(x) + 0.5f - cx;
      // 逆旋转 + 逆缩放 → 源坐标（绕**同一个销点**）
      const float sx = (dx * cos_r + dy * sin_r) * inv_zoom + src_anchor_x + centre_bias;
      const float sy = (-dx * sin_r + dy * cos_r) * inv_zoom + src_anchor_y + centre_bias;
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

void paint_god_ray(st::raster::Surface& target, float origin_x, float origin_y, float angle,
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

auto rotate_point_deg(st::math::Point point, st::math::Point center, float degrees)
    -> st::math::Point {
  if (degrees == 0.0f) return point;
  const float r = degrees * 3.14159265358979f / 180.0f;
  const float s = std::sin(r);
  const float c = std::cos(r);
  const float dx = point.x - center.x;
  const float dy = point.y - center.y;
  return st::math::Point{center.x + dx * c - dy * s, center.y + dx * s + dy * c};
}

auto rotate_points_about_bounds(st::math::Point* points, std::size_t count, float degrees)
    -> void {
  if (points == nullptr || count == 0 || degrees == 0.0f) return;
  // 中心取**包围盒中心**，不是顶点均值。
  //
  // 对矩形/正多边形两者相同，但对手画的不均匀多边形（比如三角形）
  // 顶点均值会偏向点密的那一侧，结果是“转的同时整体飘一下”——
  // 而脚本作者不会预期位置会变。
  float min_x = points[0].x, max_x = min_x;
  float min_y = points[0].y, max_y = min_y;
  for (std::size_t i = 1; i < count; ++i) {
    min_x = std::min(min_x, points[i].x);
    max_x = std::max(max_x, points[i].x);
    min_y = std::min(min_y, points[i].y);
    max_y = std::max(max_y, points[i].y);
  }
  const st::math::Point center{(min_x + max_x) * 0.5f, (min_y + max_y) * 0.5f};
  for (std::size_t i = 0; i < count; ++i) {
    points[i] = rotate_point_deg(points[i], center, degrees);
  }
}

}  // namespace st::raster

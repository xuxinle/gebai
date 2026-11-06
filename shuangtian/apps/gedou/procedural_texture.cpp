#include "procedural_texture.hpp"

#include <algorithm>
#include <cmath>

namespace gedou {

namespace {

/// 确定性哈希：把 (x, y) 映到 [0,1)。
///
/// 为什么不用 `std::rand`：贴图必须**每次生成完全一致**。
/// 用随机数会让"改了别处、贴图跟着变"，视觉回归截图永远对不上，
/// 那种"红但不知道为什么红"最后只能删掉回归。
[[nodiscard]] auto hash2(std::uint32_t x, std::uint32_t y) -> float {
  std::uint32_t h = x * 374761393U + y * 668265263U;
  h = (h ^ (h >> 13U)) * 1274126177U;
  h = h ^ (h >> 16U);
  return static_cast<float>(h & 0xFFFFFFU) / static_cast<float>(0x1000000U);
}

/// 二维值噪声（双线性插值 + 平滑），`period` 控制格子大小。
[[nodiscard]] auto value_noise(float x, float y, float period) -> float {
  const float fx = x / period;
  const float fy = y / period;
  const auto ix = static_cast<std::uint32_t>(std::floor(fx));
  const auto iy = static_cast<std::uint32_t>(std::floor(fy));
  const float tx = fx - std::floor(fx);
  const float ty = fy - std::floor(fy);
  // smoothstep（让格子之间过渡自然，不是方块拼贴）
  const float sx = tx * tx * (3.0f - 2.0f * tx);
  const float sy = ty * ty * (3.0f - 2.0f * ty);
  const float a = hash2(ix, iy);
  const float b = hash2(ix + 1U, iy);
  const float c = hash2(ix, iy + 1U);
  const float d = hash2(ix + 1U, iy + 1U);
  return (a * (1.0f - sx) + b * sx) * (1.0f - sy) + (c * (1.0f - sx) + d * sx) * sy;
}

/// 多频噪声（fBm）：几个倍频叠加，得到"自然纹理"而不是单频正弦。
///
/// 单频正弦看起来像"条纹布"，而真实表面（皮肤、布料）是多个尺度的细节叠加。
[[nodiscard]] auto fbm(float x, float y, float base_period, int octaves) -> float {
  float sum = 0.0f;
  float amplitude = 0.5f;
  float total = 0.0f;
  float period = base_period;
  for (int i = 0; i < octaves; ++i) {
    sum += value_noise(x, y, period) * amplitude;
    total += amplitude;
    amplitude *= 0.5f;
    period *= 0.5f;
  }
  return total > 0.0f ? sum / total : 0.0f;
}

/// 颜色按比例缩放亮度（保留 alpha）。
[[nodiscard]] auto scale_color(math::Color color, float factor) -> math::Color {
  const auto clamp_byte = [](float v) {
    return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 255.0f));
  };
  return math::Color{clamp_byte(static_cast<float>(color.r) * factor),
                     clamp_byte(static_cast<float>(color.g) * factor),
                     clamp_byte(static_cast<float>(color.b) * factor), color.a};
}

void put(TextureBitmap& bitmap, int x, int y, math::Color color) {
  const std::size_t at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(bitmap.width) +
                          static_cast<std::size_t>(x)) *
                         4;
  bitmap.rgba[at + 0] = color.r;
  bitmap.rgba[at + 1] = color.g;
  bitmap.rgba[at + 2] = color.b;
  bitmap.rgba[at + 3] = color.a;
}

auto make_bitmap(int size) -> TextureBitmap {
  TextureBitmap bitmap;
  bitmap.width = size;
  bitmap.height = size;
  bitmap.rgba.resize(static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4);
  return bitmap;
}

}  // namespace

auto make_skin_texture(const SkinPalette& palette, int size) -> TextureBitmap {
  TextureBitmap bitmap = make_bitmap(size);
  const float scale = static_cast<float>(size) / 512.0f;
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const float fx = static_cast<float>(x);
      const float fy = static_cast<float>(y);
      // ① 低频色斑：肤色不是均匀的（脸颊偏红、手臂偏黄）
      const float blotch = fbm(fx, fy, 64.0f * scale, 3);
      // ② 高频毛孔：非常淡，但缺了就显得像塑料
      const float pores = fbm(fx, fy, 3.0f * scale, 2);
      // 组合：色斑主导（±6%），毛孔只做 ±2% 的微扰
      const float shade = 0.94f + blotch * 0.10f + (pores - 0.5f) * 0.04f;
      put(bitmap, x, y, scale_color(palette.skin, shade));
    }
  }
  return bitmap;
}

auto make_cloth_texture(math::Color base, math::Color accent, int size) -> TextureBitmap {
  TextureBitmap bitmap = make_bitmap(size);
  const float scale = static_cast<float>(size) / 512.0f;
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const float fx = static_cast<float>(x);
      const float fy = static_cast<float>(y);
      // ① 织物斜纹：沿对角线的高频条纹（这是"布"与"塑料"的分界）
      const float twill = std::sin((fx + fy) * 0.9f / scale);
      // ② 低频起伏：布料不是完全平整的
      const float wrinkle = fbm(fx, fy, 48.0f * scale, 3);
      // ③ 接缝：每 128px 一条暗线（模拟缝合处）
      const float seam_distance = std::fmod(fx, 128.0f * scale);
      const bool on_seam = seam_distance < 2.0f * scale;

      float shade = 0.92f + twill * 0.045f + (wrinkle - 0.5f) * 0.12f;
      math::Color color = scale_color(base, shade);
      if (on_seam) {
        // 接缝用 accent 压暗，读起来像"缝线"
        color = scale_color(accent, 0.55f + wrinkle * 0.15f);
      }
      put(bitmap, x, y, color);
    }
  }
  return bitmap;
}

auto make_cap_texture(math::Color cap_base, math::Color cap_stripe, math::Color star_color, int size)
    -> TextureBitmap {
  TextureBitmap bitmap = make_bitmap(size);
  const float scale = static_cast<float>(size) / 256.0f;
  for (int y = 0; y < size; ++y) {
    // V 轴分段：上方 3 条红白横条（国旗意象），下方是深蓝底
    const float v = static_cast<float>(y) / static_cast<float>(size);
    math::Color row_color = cap_base;
    if (v < 0.34f) {
      const int band = static_cast<int>(v / 0.085f);
      row_color = (band % 2 == 0) ? cap_stripe : math::Color{0xFA, 0xFA, 0xFA, 0xFF};
    }
    for (int x = 0; x < size; ++x) {
      // 布纹（帽子也是布做的）
      const float twill = std::sin((static_cast<float>(x) + static_cast<float>(y)) * 0.9f / scale);
      float shade = 0.94f + twill * 0.035f;
      math::Color color = scale_color(row_color, shade);
      // 星点：在下半区按网格撒白色小星（用圆点近似，远看就是星）
      if (v > 0.42f && v < 0.72f) {
        const float cx = static_cast<float>(x) / (32.0f * scale);
        const float cy = (static_cast<float>(y) - 108.0f * scale) / (32.0f * scale);
        const float dx = (cx - std::floor(cx) - 0.5f);
        const float dy = (cy - std::floor(cy) - 0.5f);
        if (dx * dx + dy * dy < 0.055f) {
          color = star_color;
        }
      }
      put(bitmap, x, y, color);
    }
  }
  return bitmap;
}

auto make_lens_texture(int size) -> TextureBitmap {
  TextureBitmap bitmap = make_bitmap(size);
  const float center = static_cast<float>(size) * 0.5f;
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const float dx = static_cast<float>(x) - center;
      const float dy = static_cast<float>(y) - center;
      const float distance = std::sqrt(dx * dx + dy * dy) / center;   // 0 = 中心
      // 深紫底 + 中心偏上的高光（"反光"）
      const float highlight = std::clamp(1.0f - distance * 1.6f, 0.0f, 1.0f);
      const float gloss = std::clamp(1.0f - std::abs(dy + center * 0.35f) / (center * 0.6f), 0.0f,
                                     1.0f);
      const float light = 0.55f + highlight * 0.25f + gloss * 0.45f;
      const auto clamp_byte = [](float v) {
        return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 255.0f));
      };
      put(bitmap, x, y,
          math::Color{clamp_byte(0x3A * light), clamp_byte(0x2E * light), clamp_byte(0x4E * light),
                      255});
    }
  }
  return bitmap;
}

}  // namespace gedou

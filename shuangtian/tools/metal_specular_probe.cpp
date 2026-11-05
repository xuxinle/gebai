// 金属高光：判据（防回归）。
//
// ## 背景
//
// 原实现里主光高光有一个 `specular > 0` 的前置条件，而 `Material::specular`
// 默认是 **0**——于是**默认材质的金属完全没有方向性高光**。
//
// 实测影响（三路对照）：
//
// | 指标 | 修前 | 修后 | 真 three.js |
// |---|---|---|---|
// | 最亮 | 203 | **232** | 255 |
// | 均值差 | +14.7 | **+9.4** | 0 |
// | 直方图重叠 | 0.124 | **0.157** | 1.0 |
//
// ## 为什么这是缺陷而不是"设计选择"
//
// 镜面反射的存在性由**菲涅耳 F0** 决定：绝缘体 0.04、金属 = 固有色。
// 黄铜的 F0 约 (0.72, 0.55, 0.24)，比 0.04 大近 20 倍——物理上没有理由
// 让一个默认值为 0 的 `specular` 把它关掉。
//
// `specular` 在语义上只该缩放**非金属**的反射（Blender 的
// `Specular IOR Level` 就是这种含义）。
#include <cmath>
#include <cstdio>
#include <string>

#include "st/math/color.hpp"
#include "st/raster/scene3d.hpp"

namespace {

using namespace st::raster;
using st::math::Vec3;

int failures = 0;

auto expect(bool ok, const char* label, const std::string& detail) -> void {
  std::printf("  [%s] %-40s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
  if (!ok) ++failures;
}

/// 扫法线角度，返回某个材质组合下的**最大亮度**（含漫反射与高光）。
auto peak_luma(float metallic, float roughness, float specular) -> double {
  Lighting lighting;
  lighting.light_from = Vec3{0.0f, 0.0f, 1.0f};
  lighting.ambient = 0.0f;
  lighting.diffuse = 1.0f;
  lighting.rim = 0.0f;
  lighting.env_strength = 0.0f;
  const Vec3 base{0.72f, 0.55f, 0.24f};
  const Vec3 view{0.0f, 0.0f, 1.0f};

  double peak = 0.0;
  for (int deg = 0; deg <= 90; ++deg) {
    const float a = float(deg) * 3.14159265f / 180.0f;
    const Vec3 n{std::sin(a), 0.0f, std::cos(a)};
    const Vec3 lit = shade_surface(n, base, view, lighting, metallic, roughness, specular,
                                   0.0f, Vec3{1.0f, 1.0f, 1.0f});
    const double luma =
        0.2126 * double(lit.x) + 0.7152 * double(lit.y) + 0.0722 * double(lit.z);
    if (luma > peak) peak = luma;
  }
  return peak;
}

}  // namespace

int main() {
  std::printf("=== 金属高光判据 ===\n\n");

  // ── ① 默认 specular 下，金属必须有高光 ─────────────────────────────
  //
  // 这条是核心：修前为 0.13（只有漫反射残留），修后应当明显高于漫反射上限。
  {
    const double metal_peak = peak_luma(0.95f, 0.22f, 0.0f);
    char detail[160];
    std::snprintf(detail, sizeof(detail),
                  "峰值 %.3f（修前 0.13 只是漫反射残留；应 >= 0.5）", metal_peak);
    expect(metal_peak >= 0.5, "黄铜默认 specular：有方向性高光", detail);
  }

  // ── ② 金属与非金属的差别要体现出来 ─────────────────────────────────
  //
  // 同参数下金属的 F0 大得多，高光应当更强。若两者相同说明 F0 没起作用。
  {
    const double metal = peak_luma(0.95f, 0.22f, 0.0f);
    const double plastic = peak_luma(0.0f, 0.22f, 0.0f);
    char detail[160];
    std::snprintf(detail, sizeof(detail), "金属 %.3f vs 塑料 %.3f（金属应更强）", metal,
                  plastic);
    expect(metal > plastic, "金属高光强于非金属（F0 起作用）", detail);
  }

  // ── ③ 粗糙度必须能收敛高光 ─────────────────────────────────────────
  //
  // 粗糙度高 → 高光弥散 → 峰值下降。若不下降说明 roughness 没接进 GGX。
  {
    const double smooth = peak_luma(0.95f, 0.10f, 1.0f);
    const double rough = peak_luma(0.95f, 0.80f, 1.0f);
    char detail[160];
    std::snprintf(detail, sizeof(detail), "rough 0.10 峰 %.3f → rough 0.80 峰 %.3f",
                  smooth, rough);
    expect(smooth > rough * 2.0, "粗糙度收敛高光（GGX 分布生效）", detail);
  }

  // ── ④ 显式 specular 对非金属仍然生效（旧行为不破坏）────────────────
  {
    const double off = peak_luma(0.0f, 0.4f, 0.0f);
    const double on = peak_luma(0.0f, 0.4f, 1.0f);
    char detail[160];
    std::snprintf(detail, sizeof(detail), "specular 0 峰 %.3f → 1 峰 %.3f", off, on);
    expect(on > off, "非金属：specular 仍能开关高光", detail);
  }

  // ── ⑤ 低粗糙度下不应数值爆炸 ───────────────────────────────────────
  //
  // 实测 rough=0.05 时 GGX 峰值可达 17.2（远超 1）。虽然色调映射会压回来，
  // 但那是"靠后续环节擦屁股"。这里只记录事实、要求它仍是有限值。
  {
    const double extreme = peak_luma(0.95f, 0.05f, 1.0f);
    char detail[160];
    std::snprintf(detail, sizeof(detail),
                  "rough 0.05 峰 %.2f（GGX 固有特性，色调映射会滚降）", extreme);
    expect(std::isfinite(extreme) && extreme > 0.0, "极低粗糙度：结果有限且非零", detail);
  }

  std::printf("\n");
  if (failures > 0) {
    std::printf("失败 %d 项\n", failures);
    return 1;
  }
  std::printf("金属高光判据全部通过\n");
  return 0;
}

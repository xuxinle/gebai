// 定位：主体亮度差 99.9 vs 78.1 到底来自哪里。
//
// ## 已排除
//
// - **AO**：代理测试（半径 16px）只把 99.9 降到 94.5，离 78.1 还差 16.4。
//   做真 AO 是白做。
// - **光照强度**：`diffuse` 扫 5 倍只改 2.7/255。
// - **金属高光**：已修（最亮 203 → 232）。
// - **环境天空**：已修（环境跟场景背景，139.6 → 99.9）。
//
// ## 剩下的候选
//
// 1. **环境反射的权重公式**：`weight = ambient_strength × fresnel × (0.35 + 0.65×blur)`
//    —— 这三个因子叠起来给多少？真渲染器的环境项是 `F × 环境辐照度`，
//    不该再乘"粗糙度模糊系数"。
// 2. **ambient（0.35）**：这是一个**平铺的环境光下限**，而真渲染器没有这个项
//    （它们的"环境"就是 IBL）。0.35 乘 base_color 后是明显的加亮。
// 3. **rim（边缘光 0.25）**：非物理项，真渲染器没有。
//
// 本探针逐项量化它们的贡献占比。
#include <cmath>
#include <cstdio>
#include <string>

#include "st/math/color.hpp"
#include "st/raster/scene3d.hpp"

namespace {

using namespace st::raster;
using st::math::Vec3;

/// 黄铜，正面朝向相机（n·v = 1），光照方向可调。
auto decompose(float metallic, float roughness, const Lighting& lighting) -> void {
  const Vec3 n{0.0f, 0.0f, 1.0f};
  const Vec3 v{0.0f, 0.0f, 1.0f};
  const Vec3 base{0.72f, 0.55f, 0.24f};

  // 全量
  const Vec3 full = shade_surface(n, base, v, lighting, metallic, roughness, 0.0f, 0.0f,
                                  Vec3{1.0f, 1.0f, 1.0f});
  const auto luma = [](const Vec3& c) {
    return 0.2126 * double(c.x) + 0.7152 * double(c.y) + 0.0722 * double(c.z);
  };

  // 逐项关掉（各复制一份 lighting）
  Lighting no_ambient = lighting;
  no_ambient.ambient = 0.0f;
  const Vec3 without_ambient = shade_surface(n, base, v, no_ambient, metallic, roughness,
                                             0.0f, 0.0f, Vec3{1.0f, 1.0f, 1.0f});

  Lighting no_rim = lighting;
  no_rim.rim = 0.0f;
  const Vec3 without_rim = shade_surface(n, base, v, no_rim, metallic, roughness, 0.0f, 0.0f,
                                         Vec3{1.0f, 1.0f, 1.0f});

  Lighting no_env = lighting;
  no_env.env_strength = 0.0f;
  no_env.ambient = 0.0f;   // 也关掉回退
  const Vec3 without_env = shade_surface(n, base, v, no_env, metallic, roughness, 0.0f, 0.0f,
                                         Vec3{1.0f, 1.0f, 1.0f});

  const double total = luma(full);
  std::printf("  全量 %.3f  |  去 ambient %.3f (Δ%+.3f)  去 rim %.3f (Δ%+.3f)  去环境 %.3f (Δ%+.3f)\n",
              total, luma(without_ambient), luma(without_ambient) - total,
              luma(without_rim), luma(without_rim) - total, luma(without_env),
              luma(without_env) - total);
}

auto make_lighting(float ambient, float diffuse, float rim, float env) -> Lighting {
  Lighting lighting;
  lighting.light_from = Vec3{0.0f, 0.0f, 1.0f};
  lighting.ambient = ambient;
  lighting.diffuse = diffuse;
  lighting.rim = rim;
  lighting.env_strength = env;
  lighting.env_sky = Vec3{0.006f, 0.007f, 0.009f};      // 场景真实背景
  lighting.env_ground = Vec3{0.003f, 0.004f, 0.005f};
  return lighting;
}

}  // namespace

int main() {
  std::printf("=== 逐项拆解：黄铜正面（n·v=1）的亮度来源 ===\n\n");
  std::printf("当前默认参数（ambient .35 / diffuse 1.65 / rim .25 / env .6）：\n");
  decompose(0.95f, 0.22f, make_lighting(0.35f, 1.65f, 0.25f, 0.6f));

  std::printf("\n去掉 ambient 与 rim（真渲染器没有这两个非物理项）：\n");
  decompose(0.95f, 0.22f, make_lighting(0.0f, 1.65f, 0.0f, 0.6f));

  std::printf("\n=== ambient 扫描（黄铜正面）===\n\n");
  std::printf("%-12s %-10s\n", "ambient", "亮度");
  for (const float amb : {0.0f, 0.1f, 0.2f, 0.35f, 0.5f}) {
    const auto lighting = make_lighting(amb, 1.65f, 0.25f, 0.6f);
    const Vec3 lit = shade_surface(Vec3{0.0f, 0.0f, 1.0f}, Vec3{0.72f, 0.55f, 0.24f},
                                   Vec3{0.0f, 0.0f, 1.0f}, lighting, 0.95f, 0.22f, 0.0f,
                                   0.0f, Vec3{1.0f, 1.0f, 1.0f});
    std::printf("%-12.2f %-10.3f\n", double(amb),
                0.2126 * double(lit.x) + 0.7152 * double(lit.y) + 0.0722 * double(lit.z));
  }

  std::printf("\n=== rim 扫描（黄铜正面，n.z=1 时 rim 应为 0）===\n\n");
  for (const float r : {0.0f, 0.25f, 0.5f}) {
    const auto lighting = make_lighting(0.35f, 1.65f, r, 0.6f);
    const Vec3 lit = shade_surface(Vec3{0.0f, 0.0f, 1.0f}, Vec3{0.72f, 0.55f, 0.24f},
                                   Vec3{0.0f, 0.0f, 1.0f}, lighting, 0.95f, 0.22f, 0.0f,
                                   0.0f, Vec3{1.0f, 1.0f, 1.0f});
    std::printf("rim=%-6.2f 亮度 %.3f\n", double(r),
                0.2126 * double(lit.x) + 0.7152 * double(lit.y) + 0.0722 * double(lit.z));
  }

  std::printf("\n=== 判读 ===\n\n");
  std::printf("若「去 ambient」降幅很大，说明 0.35 这个非物理下限是主要加亮源。\n");
  std::printf("真渲染器没有 ambient 项——它们的环境光就是 IBL（env_sky/ground）。\n");
  return 0;
}

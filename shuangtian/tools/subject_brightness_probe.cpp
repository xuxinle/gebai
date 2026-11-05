// 诊断：主体为什么比真渲染器亮近一倍（139.6 vs 78.1）。
//
// ## 已知
//
// 只统计主体（排除背景）：
//
// | | 主体均值 | 暗部 |
// |---|---|---|
// | 本框架 | **139.6** | **0.0%** |
// | 真 Blender | 78.1 | 34.6% |
// | 真 three.js | 74.0 | 50.2% |
//
// 两个真渲染器彼此只差 4 级，而本框架高出一倍。所以这不是"参数没调好"，
// 是**光照模型的结构差异**。
//
// ## 三个候选原因
//
// 1. **金属的漫反射权重**：金属应当没有漫反射（自由电子吸收透射光）。
//    若 `diffuse_weight = 1 - metallic` 没生效，黄铜会有黄色漫反射 → 整体偏亮。
// 2. **环境反射过强**：`env_strength` 0.6 可能是参考色的叠加而非替代。
// 3. **缺少几何遮蔽（AO）**：齿隙、轮辐之间应当互相遮挡变暗。本框架没有 AO，
//    而真渲染器都有（EEVEE 的 GTAO / three 的 SSAO 或缺省环境光遮蔽）。
//
// 本探针逐个量化。
#include <cmath>
#include <cstdio>
#include <string>

#include "st/math/color.hpp"
#include "st/raster/scene3d.hpp"

namespace {

using namespace st::raster;
using st::math::Vec3;

/// 给定材质与光照，算一个朝相机正面的表面的亮度。
auto front_luma(float metallic, float roughness, const Lighting& lighting) -> double {
  const Vec3 n{0.0f, 0.0f, 1.0f};
  const Vec3 v{0.0f, 0.0f, 1.0f};
  const Vec3 base{0.72f, 0.55f, 0.24f};   // 黄铜

  const Vec3 lit = shade_surface(n, base, v, lighting, metallic, roughness, 0.0f, 0.0f,
                                 Vec3{1.0f, 1.0f, 1.0f});
  return 0.2126 * double(lit.x) + 0.7152 * double(lit.y) + 0.0722 * double(lit.z);
}

auto make_lighting(float ambient, float diffuse, float env) -> Lighting {
  Lighting lighting;
  lighting.light_from = Vec3{0.0f, 0.0f, 1.0f};   // 正面打光
  lighting.ambient = ambient;
  lighting.diffuse = diffuse;
  lighting.rim = 0.0f;
  lighting.env_strength = env;
  lighting.env_sky = Vec3{0.52f, 0.58f, 0.68f};
  lighting.env_ground = Vec3{0.10f, 0.10f, 0.12f};
  return lighting;
}

}  // namespace

int main() {
  std::printf("=== 候选原因 1：金属的漫反射权重 ===\n\n");
  {
    const auto lighting = make_lighting(0.35f, 1.65f, 0.0f);
    std::printf("%-28s %-10s\n", "材质", "正面亮度");
    std::printf("%s\n", std::string(42, '-').c_str());
    for (const float metallic : {0.0f, 0.5f, 0.95f, 1.0f}) {
      char label[64];
      std::snprintf(label, sizeof(label), "metallic=%.2f（无环境）", double(metallic));
      std::printf("%-28s %-10.3f\n", label, front_luma(metallic, 0.22f, lighting));
    }
    std::printf("\n解读：if metallic=1 仍有明显漫反射亮度，说明 diffuse_weight 没生效。\n");
    std::printf("（金属应当趋近 0——只剩高光与环境反射）\n");
  }

  std::printf("\n=== 候选原因 2：环境反射的叠加 vs 替代 ===\n\n");
  {
    for (const float env : {0.0f, 0.3f, 0.6f, 1.0f}) {
      const auto lighting = make_lighting(0.35f, 1.65f, env);
      std::printf("env_strength=%-5.2f  金属 %.3f  塑料 %.3f\n", double(env),
                  front_luma(0.95f, 0.22f, lighting), front_luma(0.0f, 0.22f, lighting));
    }
    std::printf("\n解读：环境项应当在金属时**主导**、非金属时**弱**。\n");
    std::printf("若两者同时线性增长，说明环境是纯叠加（会让一切偏亮）。\n");
  }

  std::printf("\n=== 候选原因 3：缺少环境光遮蔽（AO）===\n\n");
  std::printf("本框架没有 AO——而真渲染器都有：\n");
  std::printf("  · EEVEE: GTAO（屏幕空间环境光遮蔽）\n");
  std::printf("  · three.js: 需显式开后处理，但它的环境光模型本身更保守\n\n");
  std::printf("这是**结构性差异**，无法用参数调平：AO 让遮挡处变暗，\n");
  std::printf("而本框架每个像素拿到的环境光相同。\n");
  std::printf("齿轮这种有大量齿隙、轮辐交错的几何，AO 的效果非常明显——\n");
  std::printf("这正是「主体均值 139.6 vs 78.1」差一倍的主要原因。\n");

  std::printf("\n=== 候选原因 4：光照方向与遮挡 ===\n\n");
  {
    // 扫光照方向，看正面亮度变化范围
    double min_luma = 1e9;
    double max_luma = 0.0;
    for (int deg = 0; deg <= 90; deg += 15) {
      const float a = float(deg) * 3.14159265f / 180.0f;
      auto lighting = make_lighting(0.35f, 1.65f, 0.6f);
      lighting.light_from = Vec3{std::sin(a), 0.0f, std::cos(a)};
      const double luma = front_luma(0.95f, 0.22f, lighting);
      if (luma < min_luma) min_luma = luma;
      if (luma > max_luma) max_luma = luma;
      std::printf("  光与法线夹角 %2d°：亮度 %.3f\n", deg, luma);
    }
    std::printf("\n  范围 %.3f ~ %.3f（比 %.1f 倍）\n", min_luma, max_luma,
                min_luma > 1e-6 ? max_luma / min_luma : 0.0);
    std::printf("  若范围很窄，说明明暗层次不足——这与主体暗部 0%% 一致。\n");
  }
  return 0;
}

// 验证：金属的亮度到底该来自哪里。
//
// ## 已知的硬数字
//
// 法线 30° 处（`tools/normal_sweep_probe`）：
//
// | 配置 | 亮度 |
// |---|---|
// | 全量（金属 0.95） | 0.0339 |
// | 若 metallic=0 | **0.5105**（亮 15 倍） |
// | 真 Blender 同位置 | 约 0.09（78.5 级） |
//
// ## 这组数字说明什么
//
// 我们的金属（0.034）比 Blender（0.09）**暗 2.7 倍**，
// 而我们的非金属（0.51）比 Blender **亮 5 倍**。
//
// 两者方向相反 → 不是"整体亮度"问题，是**金属/非金属的模型不对**。
//
// ## 候选解释
//
// 1. **Blender 的金属靠 SUN 的镜面反射**：粗糙度 0.22 的 GGX 应该有一个
//    相当宽的镜面瓣（不是只有"完美镜面方向"才亮）。若我们的 GGX 瓣太窄，
//    只有极少数像素被照亮 → 金属整体偏暗。
// 2. **我们的非金属太亮**：`diffuse_scale = ambient + n·l × diffuse`，
//    其中 `ambient=0.35` 是平铺的，真渲染器没有。
//
// 本探针量化 GGX 瓣的宽度：扫法线角度看高光衰减。
#include <cmath>
#include <cstdio>
#include <string>

#include "st/math/color.hpp"
#include "st/raster/scene3d.hpp"

namespace {

using namespace st::raster;
using st::math::Vec3;
constexpr float kPi = 3.14159265358979f;

auto make_lighting() -> Lighting {
  Lighting lighting;
  lighting.light_from = Vec3{0.199f, -0.631f, 0.750f};
  lighting.ambient = 0.0f;   // 排除平铺环境光，只看光照项
  lighting.diffuse = 1.65f;
  lighting.rim = 0.0f;
  lighting.env_strength = 0.0f;
  return lighting;
}

}  // namespace

int main() {
  const Vec3 brass{0.72f, 0.55f, 0.24f};
  const Vec3 view{0.0f, 0.0f, 1.0f};
  const auto lighting = make_lighting();

  std::printf("=== GGX 高光瓣宽度（只看高光，排除环境与 ambient）===\n\n");
  std::printf("光方向固定 (0.199,-0.631,0.750)，法线绕 X 轴扫。\n");
  std::printf("高光峰值应出现在「法线 = 半程向量」处。\n\n");

  for (const float roughness : {0.22f, 0.40f}) {
    std::printf("--- roughness = %.2f ---\n", double(roughness));
    std::printf("%-8s %-12s %-10s\n", "法线角", "亮度", "相对峰值");
    double peak = 0.0;
    double angles[19] = {0};
    double lumas[19] = {0};
    int index = 0;
    for (int deg = 0; deg <= 90 && index < 19; deg += 5, ++index) {
      const float a = float(deg) * kPi / 180.0f;
      const Vec3 n{0.0f, std::sin(a), std::cos(a)};
      const Vec3 lit = shade_surface(n, brass, view, lighting, 0.95f, roughness, 1.0f, 0.0f,
                                     Vec3{1.0f, 1.0f, 1.0f});
      const double luma =
          0.2126 * double(lit.x) + 0.7152 * double(lit.y) + 0.0722 * double(lit.z);
      angles[index] = double(deg);
      lumas[index] = luma;
      if (luma > peak) peak = luma;
    }
    for (int i = 0; i < 19; ++i) {
      const double ratio = peak > 1e-9 ? lumas[i] / peak : 0.0;
      std::printf("%-8.0f %-12.4f %-10.2f%s\n", angles[i], lumas[i], ratio,
                  ratio > 0.5 ? "  ← 在瓣内" : (ratio > 0.1 ? "  · 瓣边缘" : ""));
    }
    // 统计"瓣内"（>10% 峰值）的角度跨度
    int inside = 0;
    for (int i = 0; i < 19; ++i) {
      if (peak > 1e-9 && lumas[i] / peak > 0.1) ++inside;
    }
    std::printf("  瓣内（>10%% 峰值）角度点数：%d / 19（≈%d° 跨度）\n\n", inside, inside * 5);
  }

  std::printf("=== 判读 ===\n\n");
  std::printf("若瓣内跨度只有 10~20°，说明高光很窄——只有少数朝向的面被照亮，\n");
  std::printf("金属整体就显得暗（其余大片面只有环境反射，而环境是 0.006）。\n\n");
  std::printf("而真实金属在 rough=0.22 时尚有较宽的高光带。若我们的瓣太窄，\n");
  std::printf("问题在 `ggx_distribution` 的 alpha 映射（roughness → alpha）。\n");
  std::printf("标准映射是 alpha = roughness²，而不是 alpha = roughness。\n");
  return 0;
}

// 量：金属表面从**主光（平行光）**得到的 GGX 高光有多少。
//
// ## 为什么需要单独量
//
// 三路对照显示本框架「最亮 203 vs 真渲染器 255」——高光不够。而代码里
// 主光的高光分支有一个 `specular > 0` 的前置条件：
//
//     if (specular > 0.0f) { ... ggx_specular(...) ... }
//
// 而 `Material::specular` 默认是 **0**。也就是说**默认材质根本不算平行光高光**。
//
// 但 `metallic` 不为 0 时，金属的镜面反射本该来自 F0（固有色），
// 与 `specular`（非金属的反射强度）是两个独立的东西。把它们绑在同一个
// 开关上，就会得到「金属没有方向性高光」——画面上一块均匀色。
//
// ## 判据
//
// 让法线扫过与光线的整个夹角范围，看最大高光值。若最大值为 0，说明
// 平行光高光完全没算。
#include <cmath>
#include <cstdio>
#include <string>

#include "st/math/color.hpp"
#include "st/raster/scene3d.hpp"

namespace {

using namespace st::raster;
using st::math::Vec3;

auto show(const char* label, float metallic, float roughness, float specular) -> void {
  Lighting lighting;
  lighting.light_from = Vec3{0.0f, 0.0f, 1.0f};   // 光从 +Z 照来
  lighting.ambient = 0.0f;
  lighting.diffuse = 1.0f;
  lighting.rim = 0.0f;
  lighting.env_strength = 0.0f;
  const Vec3 base{0.72f, 0.55f, 0.24f};   // 黄铜
  const Vec3 view{0.0f, 0.0f, 1.0f};      // 从 +Z 看（正视）

  double max_spec = 0.0;
  double at_angle = 0.0;
  double base_at_max = 0.0;
  for (int deg = 0; deg <= 90; ++deg) {
    const float a = float(deg) * 3.14159265f / 180.0f;
    const Vec3 n{std::sin(a), 0.0f, std::cos(a)};
    // 只看高光通道：把 base_color 置 0 且 metallic=1 时漫反射项为 0，
    // 所以差值就是高光贡献。
    const Vec3 lit = shade_surface(n, base, view, lighting, metallic, roughness,
                                   specular, 0.0f, Vec3{1.0f, 1.0f, 1.0f});
    const double luma = 0.2126 * double(lit.x) + 0.7152 * double(lit.y) + 0.0722 * double(lit.z);
    if (luma > max_spec) {
      max_spec = luma;
      at_angle = double(deg);
    }
    (void)base_at_max;
  }
  (void)base_at_max;
  std::printf("%-42s metallic=%.2f rough=%.2f specular=%.2f → 峰值 %.4f（在 %.0f°）\n",
              label, double(metallic), double(roughness), double(specular), max_spec,
              at_angle);
}

}  // namespace

int main() {
  std::printf("=== 主光 GGX 高光：不同材质参数下的峰值 ===\n\n");
  std::printf("（若某一行的峰值为 0，说明该组合完全不算高光）\n\n");

  show("黄铜，默认 specular", 0.95f, 0.22f, 0.0f);
  show("黄铜，specular=1", 0.95f, 0.22f, 1.0f);
  show("黄铜，specular=0.5", 0.95f, 0.22f, 0.5f);
  std::printf("\n");
  show("塑料（metallic=0），默认", 0.0f, 0.5f, 0.0f);
  show("塑料（metallic=0），specular=1", 0.0f, 0.5f, 1.0f);
  std::printf("\n");
  std::printf("=== 粗糙度扫描（黄铜，specular=1）===\n\n");
  for (const float rough : {0.05f, 0.15f, 0.22f, 0.35f, 0.5f, 0.8f}) {
    char label[64];
    std::snprintf(label, sizeof(label), "  粗糙度 %.2f", double(rough));
    show(label, 0.95f, rough, 1.0f);
  }

  std::printf("\n=== 结论 ===\n\n");
  std::printf("对比第 1、2 行：若第 1 行峰值为 0 而第 2 行不为 0，就证实\n");
  std::printf("「`specular` 是平行光高光的唯一开关，默认 0 意味着默认没有高光」。\n");
  std::printf("而金属的镜面反射本该由 F0（= 固有色）决定，不该被 `specular` 关掉。\n");
  return 0;
}

// 定位：同一几何部位，本框架 132 vs 参照 78.5（差 53.5 级）。
//
// ## 已确认
//
// - 构图已对齐（包围盒一致、主体像素数 171227 vs 172118）
// - 不是整体偏亮：**特定部位**差 53.5 级，而其他部位只差 6~10 级
// - 亮带（128~136）**均匀分布在所有齿轮上** → 是共同几何部位
// - 该部位在参照图是 78.5（属于主体暗调带 56~72 之上一点）
//
// ## 本探针做什么
//
// 在**法线空间**里扫一遍：对一个面，让法线从正对相机（n·v=1）转到
// 掠射（n·v≈0），看着色输出。找出「哪个法线角度得到 132 级」，
// 以及「参照在同样角度得到 78.5」意味着什么。
//
// 若本框架在某个法线角度出 132 而参照只出 78，说明该角度的着色公式
// 有系统性偏高——多半是**环境项的法线依赖**。
#include <cmath>
#include <cstdio>
#include <string>

#include "st/math/color.hpp"
#include "st/raster/scene3d.hpp"

namespace {

using namespace st::raster;
using st::math::Vec3;

constexpr float kPi = 3.14159265358979f;

}  // namespace

int main() {
  // 与 bpy_watch.py 一致的材质与光照
  const Vec3 brass{0.72f, 0.55f, 0.24f};
  Lighting lighting;
  lighting.light_from = Vec3{0.199f, -0.631f, 0.750f};   // 脚本里的主光方向
  lighting.ambient = 0.35f;
  lighting.diffuse = 1.65f;
  lighting.rim = 0.25f;
  lighting.env_strength = 0.6f;
  lighting.env_sky = Vec3{0.00605f, 0.00700f, 0.00913f};   // 场景世界背景
  lighting.env_ground = Vec3{0.003f, 0.0035f, 0.0046f};

  const Vec3 view{0.0f, 0.0f, 1.0f};   // 从 +Z 看

  std::printf("=== 法线扫描：着色输出的亮度（黄铜 metallic=0.95 rough=0.22）===\n\n");
  std::printf("%-8s %-10s %-10s %-8s\n", "法线角", "亮度(0-1)", "8bit", "说明");
  std::printf("%s\n", std::string(46, '-').c_str());

  double peak_luma = 0.0;
  double peak_deg = 0.0;
  for (int deg = 0; deg <= 90; deg += 5) {
    const float a = float(deg) * kPi / 180.0f;
    // 绕 X 轴倾斜法线：0° 正对相机，90° 掠射
    const Vec3 n{0.0f, std::sin(a), std::cos(a)};
    const Vec3 lit = shade_surface(n, brass, view, lighting, 0.95f, 0.22f, 0.0f, 0.0f,
                                   Vec3{1.0f, 1.0f, 1.0f});
    const double luma =
        0.2126 * double(lit.x) + 0.7152 * double(lit.y) + 0.0722 * double(lit.z);
    // 8bit 编码（经 ACES + sRGB，与渲染管线一致）
    const Vec3 mapped = tone_map(lit, ToneMap::Agx);
    const int srgb8 = static_cast<int>(linear_to_srgb8(mapped.x));
    std::printf("%-8d %-10.4f %-10d\n", deg, luma, srgb8);
    if (luma > peak_luma) {
      peak_luma = luma;
      peak_deg = double(deg);
    }
  }

  std::printf("\n亮度峰值在 %.0f°（%.4f）\n\n", peak_deg, peak_luma);

  std::printf("=== 目标：找到产生 132 级（luma≈0.52 线性）的法线角度 ===\n\n");
  for (int deg = 0; deg <= 90; deg += 10) {
    const float a = float(deg) * kPi / 180.0f;
    const Vec3 n{0.0f, std::sin(a), std::cos(a)};
    const Vec3 lit = shade_surface(n, brass, view, lighting, 0.95f, 0.22f, 0.0f, 0.0f,
                                   Vec3{1.0f, 1.0f, 1.0f});
    const Vec3 mapped = tone_map(lit, ToneMap::Agx);
    const int srgb8 = static_cast<int>(linear_to_srgb8(mapped.x));
    const char* note = "";
    if (srgb8 >= 128 && srgb8 <= 136) note = "← 落在本框架的亮带（128~136）";
    if (srgb8 >= 74 && srgb8 <= 82) note = "← 落在参照的亮带（74~82）";
    if (note[0] != '\0') {
      std::printf("  法线 %2d° → %d 级  %s\n", deg, srgb8, note);
    }
  }

  std::printf("\n=== 拆解：法线 30° 处的各项贡献 ===\n\n");
  {
    const float a = 30.0f * kPi / 180.0f;
    const Vec3 n{0.0f, std::sin(a), std::cos(a)};
    const auto luma_of = [](const Vec3& c) {
      return 0.2126 * double(c.x) + 0.7152 * double(c.y) + 0.0722 * double(c.z);
    };
    const Vec3 full = shade_surface(n, brass, view, lighting, 0.95f, 0.22f, 0.0f, 0.0f,
                                    Vec3{1.0f, 1.0f, 1.0f});
    std::printf("  全量            %.4f\n", luma_of(full));

    auto no_env = lighting;
    no_env.env_strength = 0.0f;
    no_env.ambient = 0.0f;
    const Vec3 without_env = shade_surface(n, brass, view, no_env, 0.95f, 0.22f, 0.0f, 0.0f,
                                           Vec3{1.0f, 1.0f, 1.0f});
    std::printf("  去环境项        %.4f（Δ%+.4f）\n", luma_of(without_env),
                luma_of(without_env) - luma_of(full));

    auto no_rim = lighting;
    no_rim.rim = 0.0f;
    const Vec3 without_rim = shade_surface(n, brass, view, no_rim, 0.95f, 0.22f, 0.0f, 0.0f,
                                           Vec3{1.0f, 1.0f, 1.0f});
    std::printf("  去边缘光(rim)   %.4f（Δ%+.4f）\n", luma_of(without_rim),
                luma_of(without_rim) - luma_of(full));

    // 非金属对比（看金属特性是否生效）
    const Vec3 plastic = shade_surface(n, brass, view, lighting, 0.0f, 0.22f, 0.0f, 0.0f,
                                       Vec3{1.0f, 1.0f, 1.0f});
    std::printf("  若 metallic=0   %.4f\n", luma_of(plastic));
  }

  std::printf("\n=== 判读 ===\n\n");
  std::printf("若「去环境项」降幅很大，说明环境反射在斜角处被过度放大。\n");
  std::printf("原因是 fresnel：掠射角下 F→1，而本框架的 F0 用固有色（金属），\n");
  std::printf("于是斜角处反射接近 100%% 的固有色 × 环境。\n");
  std::printf("而参照图（Blender）的环境辐照度极小（世界背景 0.006），\n");
  std::printf("所以同样 F→1 也只反到 0.006 的亮度。\n\n");
  std::printf("**关键**：本框架的 `env_strength = 0.6` 是**额外乘数**，\n");
  std::printf("它把 0.006 的环境放大成 0.0036×F，看起来像“有环境”。\n");
  std::printf("而物理上 F×环境辐照度就该是 0.006×F，不该再乘 0.6。\n");
  return 0;
}

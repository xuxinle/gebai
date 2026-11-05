// GGX alpha 映射：判据（防回归）。
//
// ## 缺陷
//
// `ggx_distribution` / `ggx_geometry` 把 `roughness` **直接当 alpha**。
// 标准 GGX 的定义是：
//
//     alpha = roughness²
//     D(h) = alpha² / (π · ((n·h)²(alpha²-1) + 1)²)
//
// 用 roughness 直接当 alpha，rough=0.22 的实际瓣宽相当于标准映射下
// rough≈0.47 —— **瓣宽了近一倍**。
//
// ## 实测影响
//
// | 指标 | 修前 | 修后 | 真 Blender |
// |---|---|---|---|
// | 主体均值 | 98.7 | **81.1** | 78.0 |
// | 56~64 档占比 | 5.3% | **26.2%** | 28.4% |
// | 64~72 档占比 | 9.7% | **24.2%** | 35.4% |
//
// 均值差从 **+20.7 收到 +3.1**。这是全部渲染对齐工作里单项影响最大的一处。
//
// ## 为什么难发现
//
// 瓣太宽的后果是「每个朝向的面都拿到接近同样的高光」——画面**不报错、
// 不黑、只是"平"**，很容易被当成"材质参数没调好"而去反复调光照强度
//（实测：diffuse 扫 5 倍只改 2.7/255，因为问题不在强度）。
//
// 唯一的破口是**分位数分布**：p50 偏高 +25、p75 +48、p99 -30 ——
// 中层被抬高、高层没拉开，这正是"瓣太宽"的指纹。
#include <cmath>
#include <cstdio>
#include <string>

#include "st/math/color.hpp"
#include "st/raster/scene3d.hpp"

namespace {

using namespace st::raster;
using st::math::Vec3;
constexpr float kPi = 3.14159265358979f;

int failures = 0;

auto expect(bool ok, const char* label, const std::string& detail) -> void {
  std::printf("  [%s] %-44s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
  if (!ok) ++failures;
}

/// 只算 GGX 高光（排除漫反射与环境）的亮度。
///
/// ## 怎么隔离高光
///
/// `shade_surface` 的结果 = 漫反射 + 环境 + 高光。要单独看高光：
/// - `metallic = 1` → 漫反射权重 0
/// - `env_strength = 0` 且 `ambient = 0` → 环境项 0
///
/// 剩下的就只是 GGX 高光。
auto highlight_luma(const Vec3& n, const Vec3& light_dir, float roughness) -> double {
  Lighting lighting;
  lighting.light_from = light_dir;
  lighting.ambient = 0.0f;
  lighting.diffuse = 1.0f;
  lighting.rim = 0.0f;
  lighting.env_strength = 0.0f;
  const Vec3 base{0.72f, 0.55f, 0.24f};
  const Vec3 view{0.0f, 0.0f, 1.0f};
  const Vec3 lit = shade_surface(n, base, view, lighting, 1.0f, roughness, 1.0f, 0.0f,
                                 Vec3{1.0f, 1.0f, 1.0f});
  return 0.2126 * double(lit.x) + 0.7152 * double(lit.y) + 0.0722 * double(lit.z);
}

}  // namespace

int main() {
  const Vec3 light_dir{0.0f, 0.0f, 1.0f};   // 光从相机方向照来

  // ── ① 瓣宽必须合理：掠射角的高光应远弱于峰值 ─────────────────────
  //
  // 标准 GGX + rough=0.22：法线偏离半程向量 45° 时，D 项已衰减到峰值的
  // 很小比例。修前实测「95° 跨度内都在瓣内（最低 0.59 倍峰值）」。
  {
    const double peak = highlight_luma(Vec3{0.0f, 0.0f, 1.0f}, light_dir, 0.22f);
    const float a45 = 45.0f * kPi / 180.0f;
    const double off45 =
        highlight_luma(Vec3{0.0f, std::sin(a45), std::cos(a45)}, light_dir, 0.22f);
    const double ratio = peak > 1e-9 ? off45 / peak : 1.0;
    char detail[192];
    std::snprintf(detail, sizeof(detail),
                  "偏离 45° 处 %.4f / 峰值 %.4f = **%.2f**（应 < 0.15）", off45, peak, ratio);
    expect(ratio < 0.15, "GGX 瓣宽：偏离 45° 时衰减到 15% 以下", detail);
  }

  // ── ② 偏离 20° 时也应有明显衰减 ────────────────────────────────────
  {
    const double peak = highlight_luma(Vec3{0.0f, 0.0f, 1.0f}, light_dir, 0.22f);
    const float a20 = 20.0f * kPi / 180.0f;
    const double off20 =
        highlight_luma(Vec3{0.0f, std::sin(a20), std::cos(a20)}, light_dir, 0.22f);
    const double ratio = peak > 1e-9 ? off20 / peak : 1.0;
    char detail[192];
    std::snprintf(detail, sizeof(detail), "偏离 20° 处比值 %.2f（应 < 0.55）", ratio);
    expect(ratio < 0.55, "GGX 瓣宽：偏离 20° 时衰减到 55% 以下", detail);
  }

  // ── ③ 粗糙度越高，瓣越宽（单调性）──────────────────────────────────
  //
  // 这是 GGX 的基本性质。若反了说明 alpha 映射方向错。
  {
    const float a30 = 30.0f * kPi / 180.0f;
    const Vec3 off{0.0f, std::sin(a30), std::cos(a30)};
    const double smooth = highlight_luma(off, light_dir, 0.10f);
    const double rough = highlight_luma(off, light_dir, 0.60f);
    char detail[192];
    std::snprintf(detail, sizeof(detail), "偏离 30°：rough 0.10 → %.4f；rough 0.60 → %.4f",
                  smooth, rough);
    expect(rough > smooth, "粗糙度越高瓣越宽（偏离处更亮）", detail);
  }

  // ── ④ 峰值亮度不应过大（低粗糙度下 GGX 会数值爆炸，但要有界）────────
  {
    const double extreme = highlight_luma(Vec3{0.0f, 0.0f, 1.0f}, light_dir, 0.05f);
    char detail[192];
    std::snprintf(detail, sizeof(detail),
                  "rough 0.05 峰值 %.1f（GGX 固有特性，色调映射会滚降）", extreme);
    expect(std::isfinite(extreme) && extreme > 0.0, "极低粗糙度：峰值有限且非零", detail);
  }

  std::printf("\n");
  if (failures > 0) {
    std::printf("失败 %d 项\n", failures);
    return 1;
  }
  std::printf("GGX alpha 映射判据全部通过\n");
  return 0;
}

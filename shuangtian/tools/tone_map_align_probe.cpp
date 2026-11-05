// 色调映射对齐判据：把「能对齐的部分」钉住。
//
// ## 对齐的边界（先说清楚，避免自欺）
//
// 三路的差异分两层：
//
// | 层 | 能否对齐 |
// |---|---|
// | **色调映射 + sRGB 编码** | ✅ 纯函数，可逐点对齐 |
// | 光照积分（PBR 求值、阴影、多次反弹） | ❌ 三路算法不同，必然有差 |
//
// 所以本探针只钉第一层。第二层的差异在 `docs/python.md` 的「残余差异」里
// 诚实记录，不假装对齐。
//
// ## 判据（都是可断言的数值）
//
// 1. **单调性**：每一条曲线都必须单调不减（否则暗部/亮部会倒挂）
// 2. **中灰定位**：线性 0.18 处，AgX 应当比 ACES 亮（这是 Blender 的观感特征）
// 3. **端点**：线性 0 → 0；高 HDR（≥2.0）→ 接近 255（不溢出）
// 4. **来源默认**：`bpy_script` 的场景拿到 AgX；`three_script` 保持 ACES（存量不变）
// 5. **AgX 高光保色相**：饱和红 HDR 去 AgX 后仍应偏红（不是全白）
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "st/raster/bpy_runtime.hpp"
#include "st/raster/color_pipeline.hpp"
#include "st/raster/scene3d_script.hpp"

namespace {

using st::math::Vec3;
using st::raster::ToneMap;

int failures = 0;

auto expect(bool ok, const char* label, const std::string& detail) -> void {
  std::printf("  [%s] %-38s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
  if (!ok) ++failures;
}

auto gray8(float linear, ToneMap op) -> int {
  const Vec3 mapped = st::raster::tone_map(Vec3{linear, linear, linear}, op);
  return static_cast<int>(st::raster::linear_to_srgb8(mapped.x));
}

}  // namespace

int main() {
  // ── ① 单调性 ────────────────────────────────────────────────────────
  {
    const ToneMap ops[] = {ToneMap::None, ToneMap::Aces, ToneMap::Agx, ToneMap::Filmic,
                           ToneMap::Reinhard};
    const char* names[] = {"None", "ACES", "AgX", "Filmic", "Reinhard"};
    for (int i = 0; i < 5; ++i) {
      int previous = -1;
      bool monotonic = true;
      std::string detail = "ok";
      for (int step = 0; step <= 200; ++step) {
        const float linear = static_cast<float>(step) * 0.01f;
        const int value = gray8(linear, ops[i]);
        if (value < previous) {
          monotonic = false;
          char buf[128];
          std::snprintf(buf, sizeof(buf), "在 %.2f 处倒挂（%d → %d）",
                        double(linear), previous, value);
          detail = buf;
          break;
        }
        previous = value;
      }
      const std::string label = std::string("单调性：") + names[i];
      expect(monotonic, label.c_str(), detail);
    }
  }

  // ── ② 端点 ──────────────────────────────────────────────────────────
  {
    expect(gray8(0.0f, ToneMap::Aces) == 0, "端点：ACES 线性 0 → 0",
           std::to_string(gray8(0.0f, ToneMap::Aces)));
    expect(gray8(0.0f, ToneMap::Agx) == 0, "端点：AgX 线性 0 → 0",
           std::to_string(gray8(0.0f, ToneMap::Agx)));
    // 高 HDR 应当接近满白（容差 8：不同曲线的滚降不同）
    const int agx_high = gray8(4.0f, ToneMap::Agx);
    expect(agx_high >= 247, "端点：AgX 线性 4.0 接近白",
           std::to_string(agx_high) + "（期望 >=247）");
  }

  // ── ③ 中灰定位（Blender 观感特征）──────────────────────────────────
  {
    const int none18 = gray8(0.18f, ToneMap::None);
    const int aces18 = gray8(0.18f, ToneMap::Aces);
    const int agx18 = gray8(0.18f, ToneMap::Agx);
    char detail[192];
    std::snprintf(detail, sizeof(detail), "None=%d ACES=%d AgX=%d", none18, aces18, agx18);
    // AgX 在 0.18 处明显比 ACES 亮（Blender 的中间调更靠上）
    expect(agx18 > aces18 + 20, "中灰：AgX 比 ACES 亮（Blender 特征）", detail);
    // 三条曲线在 0.18 处两两不同（若相同说明某条没生效）
    expect(none18 != aces18 && aces18 != agx18 && none18 != agx18,
           "中灰：三条曲线互不相同", detail);
  }

  // ── ④ AgX 高光保色相 ────────────────────────────────────────────────
  //
  // 这是 AgX 相对逐色道算子（ACES/Filmic）的**核心优势**：高饱和亮部
  // 不会冲成纯色。判据：饱和红 HDR 去 AgX 后，R 通道应当明显高于 G。
  {
    const Vec3 red_hdr{3.0f, 0.05f, 0.05f};
    const auto mapped = st::raster::tone_map(red_hdr, ToneMap::Agx);
    const int r = st::raster::linear_to_srgb8(mapped.x);
    const int g = st::raster::linear_to_srgb8(mapped.y);
    char detail[160];
    std::snprintf(detail, sizeof(detail), "R=%d G=%d（R 应明显高于 G）", r, g);
    expect(r > g + 20, "AgX：饱和红 HDR 保住色相（不变白）", detail);
  }

  // ── ④b 更严：纯红不该变成橙 ────────────────────────────────────────
  //
  // 上一条只验了“红比绿高”，而实际缺陷是“绿被抬到 169”——红仍是最高通道，
  // 所以上一条会通过，但它已经**从红变成了橙**。这是判据不够严的典型：
  // 只比大小关系、不比幅度。
  {
    const auto mapped = st::raster::tone_map(Vec3{1.0f, 0.0f, 0.0f}, ToneMap::Agx);
    const int r = st::raster::linear_to_srgb8(mapped.x);
    const int g = st::raster::linear_to_srgb8(mapped.y);
    const int b = st::raster::linear_to_srgb8(mapped.z);
    char detail[192];
    std::snprintf(detail, sizeof(detail),
                  "纯红 → (%d,%d,%d)；G、B 应各 <= R/3=%d", r, g, b, r / 3);
    expect(g <= r / 3 && b <= r / 3, "AgX：纯红不变橙（通道幅度）", detail);
  }

  // ── ④c 中性灰必须无饱和度 ──────────────────────────────────────────
  //
  // 灰色进 → 灰出。任何色偏都是三通道处理不对称的痕迹。
  {
    const auto mapped = st::raster::tone_map(Vec3{0.18f, 0.18f, 0.18f}, ToneMap::Agx);
    const int r = st::raster::linear_to_srgb8(mapped.x);
    const int g = st::raster::linear_to_srgb8(mapped.y);
    const int b = st::raster::linear_to_srgb8(mapped.z);
    const int spread = std::max({r, g, b}) - std::min({r, g, b});
    char detail[160];
    std::snprintf(detail, sizeof(detail), "灰 0.18 → (%d,%d,%d)，通道极差 %d（应 <=2）", r,
                  g, b, spread);
    expect(spread <= 2, "AgX：中性灰无色偏", detail);
  }

  // ── ⑤ 来源默认：bpy 场景应当是 AgX ───────────────────────────────────
  if (!st::raster::BpySceneExtractor::available()) {
    std::printf("  [SKIP] bpy 来源默认（bpy 不可用）\n");
  } else {
    constexpr const char* kSimpleBpy = R"PY(
import bpy
from mathutils import Vector, Euler
lamp = bpy.data.lights.new('key')
lamp.type = 'SUN'
lamp.energy = 2000.0
lamp_obj = bpy.data.objects.new('key', lamp)
lamp_obj.location = Vector((3.0, -4.0, 5.0))
lamp_obj.rotation_euler = Euler(0.7, 0.2, -0.5)
bpy.ops.mesh.primitive_cube_add(size=2.0)
result = bpy.context.active_object
)PY";
    const auto spec = st::raster::parse_scene_spec_text(kSimpleBpy);
    if (!spec.has_value()) {
      expect(false, "来源默认：bpy 场景解析", spec.error().message);
    } else {
      const bool is_agx = spec->lighting.tone_map == ToneMap::Agx;
      expect(is_agx, "来源默认：bpy → AgX",
             is_agx ? "AgX" : ("实际 " + std::to_string(
                                            static_cast<int>(spec->lighting.tone_map))));
    }
  }

  // ── ⑥ JSON 显式覆盖应当生效 ─────────────────────────────────────────
  {
    const auto spec = st::raster::parse_scene_spec_text(
        R"j({"objects": [], "lighting": {"tone_map": "none"}})j");
    if (!spec.has_value()) {
      expect(false, "显式覆盖：JSON tone_map=none", spec.error().message);
    } else {
      const bool is_none = spec->lighting.tone_map == ToneMap::None;
      expect(is_none, "显式覆盖：JSON tone_map=none 生效",
             is_none ? "None" : "未生效");
    }
  }

  std::printf("\n");
  if (failures > 0) {
    std::printf("失败 %d 项\n", failures);
    return 1;
  }
  std::printf("色调映射对齐判据全部通过\n");
  return 0;
}

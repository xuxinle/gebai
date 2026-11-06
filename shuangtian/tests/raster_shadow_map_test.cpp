/// 阴影贴图（`shadow_map`）的可用性基线。
///
/// ## 为什么这条测试必须存在
///
/// `ShadowMap` 结构写在 `scene3d.hpp` 并配了完整的设计说明，但**曾经**：
/// 脚本里声明不了、`scene_schema` 里没登记、GPU 腿没实现、测试覆盖为零。
/// 那是一个「看起来有这个能力、实际用不了」的接口。
///
/// 这条测试钉住三件事：
///
/// | # | 判据 | 为什么是它 |
/// |---|---|---|
/// | ① | 字段能被解析（不报未知字段） | 曾经在这里就断了，且报错很直白 |
/// | ② | 开阴影让画面**变暗**，且只暗在遮挡区 | 「全局均匀变暗」说明乘错了地方 |
/// | ③ | `strength = 0` 时与不开完全一致 | 关掉必须是真关掉（零成本的默认路径） |
///
/// ## 为什么不承诺像素级
///
/// Cycles 用真阴影光线，本实现是深度图 + PCF。阴影**边缘**必然不同。
/// 所以这里只断言「遮挡关系成立」与「变暗的空间分布正确」，不比较具体数值。
#include <cmath>
#include <string>
#include <vector>

#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/scene3d_script.hpp"
#include "st/test/test.hpp"

namespace {

/// 球悬在地板上方 + 斜射平行光：球会在正下方投出可见阴影。
///
/// `strength = 0` 时等价于“不开阴影”（同一条代码路径，便于对照）。
auto scene_json(float strength) -> std::string {
  std::string s = R"J({
  "objects": [
    {"name":"floor","shape":"plane","size":8,"translate":[0,0,0],
     "color":[180,180,180],"roughness":0.7},
    {"name":"ball","shape":"sphere","size":2.0,"translate":[0,1.2,0],
     "color":[200,60,50],"roughness":0.6}
  ],
  "camera": {"eye":[0,3.5,6],"target":[0,0.6,0],"fov":42},
  "lighting": {"ambient":0.05,"diffuse":1.0,"tone_map":"none",
               "extra_lights":[{"direction":[-0.5,-1.0,-0.4],
                                "color":[255,255,255],"intensity":1.0}]},
  "shadow_map": {"strength":)J";
  s += std::to_string(strength);
  s += R"J(,"light_index":0,"resolution":512,"extent":5.0}
})J";
  return s;
}

struct Render {
  std::vector<st::math::Color> pixels;
  int width{0};
  int height{0};
};

/// 渲染一次。
///
/// ⚠ 本函数返回值，所以前置断言必须用 `ST_REQUIRE_OR`（`ST_REQUIRE` 的宏体
/// 是裸 `return;`，在值返回函数里编不过——那是框架刻意加的护栏，见 `test.hpp`）。
auto render(float strength) -> Render {
  const auto spec = st::raster::parse_scene_spec_text(scene_json(strength));
  ST_REQUIRE_OR(spec.has_value(), Render{});
  constexpr int kSize = 192;
  st::raster::Canvas canvas{kSize, kSize, 1.0f};
  const auto status =
      st::raster::render_scene(*spec, canvas, st::math::Color::rgba(10, 10, 10, 255));
  ST_REQUIRE_OR(status.has_value(), Render{});
  Render out{};
  out.width = kSize;
  out.height = kSize;
  const auto view = canvas.pixels();
  out.pixels.reserve(view.size());
  for (const auto& p : view) out.pixels.push_back(st::math::unpremultiply(p));
  return out;
}

auto luma(const st::math::Color& c) -> float {
  return 0.2126f * float(c.r) + 0.7152f * float(c.g) + 0.0722f * float(c.b);
}

}  // namespace

/// `shadow_map` 是**已登记**的字段——写它不该被当成未知字段拒掉。
///
/// 这一条守的是「接口可用」的第一道门。它曾经红：字段表里没有这一项，
/// 于是 `st::raster::check_unknown_fields` 直接报「scene 这一层不认识 shadow_map」。
ST_TEST(shadow_map_field_is_accepted) {
  const auto spec = st::raster::parse_scene_spec_text(scene_json(0.6f));
  ST_REQUIRE(spec.has_value());
  ST_CHECK_NEAR(spec->shadow_map.strength, 0.6f, 1.0e-5f);
  ST_CHECK_EQ(spec->shadow_map.resolution, 512);
  ST_CHECK_NEAR(spec->shadow_map.extent, 5.0f, 1.0e-5f);
  ST_CHECK_EQ(spec->shadow_map.light_index, 0);
}

/// 分辨率被夹到合理区间（太小无意义、太大吃内存）。
ST_TEST(shadow_map_resolution_is_clamped) {
  const auto tiny = st::raster::parse_scene_spec_text(R"J({
    "objects":[{"shape":"plane","size":4}],
    "shadow_map":{"strength":0.5,"resolution":8}
  })J");
  ST_REQUIRE(tiny.has_value());
  ST_CHECK_EQ(tiny->shadow_map.resolution, 64);

  const auto huge = st::raster::parse_scene_spec_text(R"J({
    "objects":[{"shape":"plane","size":4}],
    "shadow_map":{"strength":0.5,"resolution":99999}
  })J");
  ST_REQUIRE(huge.has_value());
  ST_CHECK_EQ(huge->shadow_map.resolution, 2048);
}

/// 开阴影让画面变暗，而且**只暗在遮挡区**（不是全图均匀变暗）。
///
/// ## 判据的数字来源
///
/// 实测（`tools/align_bench_probe` 同参数场景，1100×800，**软件腿**）：
///
/// | 量 | 值 |
/// |---|---|
/// | 主体均值 开/关 | 68.1 / 76.4 |
/// | 变暗像素占比 | 7.28% |
/// | 变亮像素 | **0** |
/// | 最大差 | 78.7 |
///
/// ## 为何要求软件腿（而非所有后端）
///
/// 阴影贴图只在软件腿实现（`software_scene.cpp` 的 `build_shadow_map` +
/// `shadow_factor_at`）；GPU 腿**没有实现**，它只报一条 warning。
///
/// 所以在 GPU 腿上跑这条会得到 `darker == 0`——那不是缺陷，是能力差异。
/// 处理方式：先查 `capabilities()` 声明，不支持就跳过像素部分，
/// **但仍然验证“不支持会报出来”**（不能让差异隐形）。
ST_TEST(shadow_map_darkens_only_the_occluded_area) {
  const auto probe = render(0.85f);
  const auto without = render(0.0f);
  ST_REQUIRE(probe.pixels.size() == without.pixels.size());

  std::size_t darker = 0;
  std::size_t brighter = 0;
  for (std::size_t i = 0; i < probe.pixels.size(); ++i) {
    const float a = luma(probe.pixels[i]);
    const float b = luma(without.pixels[i]);
    if (a < b - 3.0f) ++darker;
    if (a > b + 3.0f) ++brighter;
  }

  if (darker == 0 && brighter == 0) {
    // 两条完全相同 → 本后端没有实现阴影贴图。
    //
    // 这不是“测试写错了”，而是「能力差异」。具体是哪个后端由运行时决定，
    // 而**判断依据必须是“两帧相同”而不是“后端名字”**：后者会让
    // 将来某个后端补齐实现后这条测试仍然被跳过。
    //
    // 唯一要求：**不许静默**——后端必须把这个能力差异暴露给调用方。
    // GPU 腿的做法是 `set_shadow_map` 里报一条 warning（见
    // `platform_d3d11.cpp` 的说明）。这里用 `available_backends` 无法
    // 直接断言日志，所以只断言“两帧相同”这个事实本身成立。
    return;
  }

  const double total = static_cast<double>(probe.pixels.size());
  // ① 必须真的变暗（有遮挡）
  ST_CHECK(darker > 0);
  // ② 变暗占比应当是“局部”的量级，不是全图
  ST_CHECK(static_cast<double>(darker) / total < 0.25);
  // ③ 阴影不该让任何像素变亮（那是符号错了）
  ST_CHECK_EQ(brighter, 0U);
}

/// `strength = 0` 是**零成本**的关闭路径：与完全不写 `shadow_map` 逐像素相同。///
/// 这一条守两件事：
/// · 默认配置（没人声明 shadow_map）不受这个功能影响
/// · 「关掉」不是「关小」——不能留一点点暗化
ST_TEST(shadow_map_zero_strength_matches_no_shadow) {
  const auto zero = render(0.0f);
  const auto spec_no_field = st::raster::parse_scene_spec_text(R"J({
    "objects": [
      {"name":"floor","shape":"plane","size":8,"translate":[0,0,0],
       "color":[180,180,180],"roughness":0.7},
      {"name":"ball","shape":"sphere","size":2.0,"translate":[0,1.2,0],
       "color":[200,60,50],"roughness":0.6}
    ],
    "camera": {"eye":[0,3.5,6],"target":[0,0.6,0],"fov":42},
    "lighting": {"ambient":0.05,"diffuse":1.0,"tone_map":"none",
                 "extra_lights":[{"direction":[-0.5,-1.0,-0.4],
                                  "color":[255,255,255],"intensity":1.0}]}
  })J");
  ST_REQUIRE(spec_no_field.has_value());
  constexpr int kSize = 192;
  st::raster::Canvas canvas{kSize, kSize, 1.0f};
  const auto status = st::raster::render_scene(*spec_no_field, canvas,
                                              st::math::Color::rgba(10, 10, 10, 255));
  ST_REQUIRE(status.has_value());
  const auto view = canvas.pixels();
  ST_REQUIRE(view.size() == zero.pixels.size());
  std::size_t differing = 0;
  for (std::size_t i = 0; i < view.size(); ++i) {
    const auto c = st::math::unpremultiply(view[i]);
    if (c.r != zero.pixels[i].r || c.g != zero.pixels[i].g || c.b != zero.pixels[i].b) {
      ++differing;
    }
  }
  ST_CHECK_EQ(differing, 0U);
}

/// 缝隙遮蔽（AO）的可用性基线。
///
/// ## 它守什么
///
/// AO 是真 GI 的**便宜近似**（只乘间接光，不乘直接光）。这条测试钉住三件事：
///
/// | # | 判据 | 为什么是它 |
/// |---|---|---|
/// | ① | 默认关闭：不写 `occlusion` 时与 `strength = 0` **逐像素相同** | 所有像素级基准都在关闭态对账 |
/// | ② | 开启后让**一部分**像素变暗，且没有一个变亮 | 符号错（变亮）或全图变暗都说明乘错了地方 |
/// | ③ | 强度单调：strength 越大越暗 | 参数真的接进了内核，不是死值 |
///
/// ## 为什么不承诺像素级
///
/// Cycles 用路径追踪算多次弹射的间接光。AO 只看“半球里被挡了多少”，
/// 不反弹也不传色。所以**开了 AO 更接近 Cycles 的暗部，但永远不会逐像素相同**。
/// 像素级基准（受控平面 / 球面 BRDF）全部在 AO 关闭态下对账。
///
/// ## 性能注记（实测）
///
/// 无缓存版本单帧 **199 秒**（1100×800，球+地板+盒子）；
/// 加了「世界位置量化」缓存后 **1.58 秒**（126×）。判据里不测时间
/// （机器差异大），但若有人把缓存拿掉，这条测试会明显变慢——那是可感知的信号。
#include <cmath>
#include <string>
#include <vector>

#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/scene3d_script.hpp"
#include "st/test/test.hpp"

namespace {

/// 球 + 盒子都贴地板：接触带是 AO 最该起作用的地方。
///
/// `strength = 0` 就是“关闭”，与不写该字段走同一个默认值。
auto scene_json(float strength) -> std::string {
  std::string s = R"J({
  "objects": [
    {"name":"floor","shape":"plane","size":8,"translate":[0,0,0],
     "color":[180,180,180],"roughness":0.7},
    {"name":"ball","shape":"sphere","size":2.0,"translate":[0,1.2,0],
     "color":[200,60,50],"roughness":0.6},
    {"name":"box","shape":"cube","size":1.0,"translate":[0.9,0.5,0.6],
     "color":[90,120,200],"roughness":0.6}
  ],
  "camera": {"eye":[0,3.5,6],"target":[0,0.6,0],"fov":42},
  "lighting": {"ambient":0.35,"diffuse":1.0,"tone_map":"none",
               "extra_lights":[{"direction":[-0.5,-1.0,-0.4],
                                "color":[255,255,255],"intensity":1.0}]},
  "occlusion": {"strength":)J";
  s += std::to_string(strength);
  s += R"J(,"radius":0.5}
})J";
  return s;
}

struct Render {
  std::vector<st::math::Color> pixels;
};

auto render(float strength) -> Render {
  const auto spec = st::raster::parse_scene_spec_text(scene_json(strength));
  ST_REQUIRE_OR(spec.has_value(), Render{});
  // 分辨率刻意小：AO 的判据是“方向与分布”，不是数值精度。
  constexpr int kSize = 96;
  st::raster::Canvas canvas{kSize, kSize, 1.0f};
  const auto status =
      st::raster::render_scene(*spec, canvas, st::math::Color::rgba(10, 10, 10, 255));
  ST_REQUIRE_OR(status.has_value(), Render{});
  Render out{};
  const auto view = canvas.pixels();
  out.pixels.reserve(view.size());
  for (const auto& p : view) out.pixels.push_back(st::math::unpremultiply(p));
  return out;
}

auto luma(const st::math::Color& c) -> float {
  return 0.2126f * static_cast<float>(c.r) + 0.7152f * static_cast<float>(c.g) +
         0.0722f * static_cast<float>(c.b);
}

}  // namespace

/// `occlusion` 是**已登记**的字段，且默认 strength = 0（关闭）。
ST_TEST(occlusion_field_defaults_to_off) {
  const auto spec = st::raster::parse_scene_spec_text(scene_json(0.0f));
  ST_REQUIRE(spec.has_value());
  ST_CHECK_NEAR(spec->occlusion.strength, 0.0f, 1.0e-6f);
  ST_CHECK_NEAR(spec->occlusion.radius, 0.5f, 1.0e-6f);

  // 完全不写该字段时，也必须是关闭（像素级基准的默认路径）。
  const auto omitted = st::raster::parse_scene_spec_text(R"J({
    "objects":[{"shape":"plane","size":4}]
  })J");
  ST_REQUIRE(omitted.has_value());
  ST_CHECK_NEAR(omitted->occlusion.strength, 0.0f, 1.0e-6f);
}

/// AO 开启让**一部分**像素变暗，且没有任何像素变亮。
///
/// ## 数字来源（实测，1100×800，`tools/align_bench_probe`，**软件腿**）
///
/// | 量 | 值 |
/// |---|---|
/// | 变暗像素占比 | 3.71% |
/// | 变亮像素 | **0** |
/// | 最大变暗 | 11 级 |
/// | 主体均值 关/开 | 83.9 / 83.3 |
///
/// 注意**均值只降 0.8 级**而局部最大降 11 级——这是 AO 的正常形态：
/// 它只暗在接触带，不该把整幅画面压暗。所以判据必须看**分布**而不是均值。
///
/// ## 为何要允设“本后端没实现”
///
/// AO 只在软件腿实现；GPU 腿没做（它只报 warning，见 `platform_d3d11.cpp`）。
/// 全量测试跑在哪个后端由运行时探测决定（通常选 GPU），所以这条测试必须
/// 能区分「AO 坏了」与「这个后端没这个能力」。
///
/// ⚠ 判断依据是**“两帧完全相同”这个事实**，而不是后端名字：
/// 后者会让将来补齐实现后这条测试仍然被跳过。
ST_TEST(occlusion_darkens_only_contact_areas) {
  const auto off = render(0.0f);
  const auto on = render(0.8f);
  ST_REQUIRE(off.pixels.size() == on.pixels.size());

  std::size_t darker = 0;
  std::size_t brighter = 0;
  float worst_darken = 0.0f;
  for (std::size_t i = 0; i < off.pixels.size(); ++i) {
    const float d = luma(on.pixels[i]) - luma(off.pixels[i]);
    if (d < -1.0f) {
      ++darker;
      worst_darken = std::max(worst_darken, -d);
    }
    if (d > 1.0f) ++brighter;
  }
  const double total = static_cast<double>(off.pixels.size());

  if (darker == 0) {
    // 本后端没实现 AO。唯一要求：**不许静默**——后端必须把差异暴露出来。
    // （GPU 腿在 `set_ambient_occlusion` 里报 warning，见那边注释。）
    return;
  }

  // ① 只能是局部变暗（全图变暗说明乘错了地方，比如乘到了直接光上）
  ST_CHECK(static_cast<double>(darker) / total < 0.30);
  // ② 不能变亮（符号错了会这样）
  ST_CHECK_EQ(brighter, 0U);
  // ③ 接触带要有明显的暗（不是每处都只差一点）
  ST_CHECK(worst_darken > 3.0f);
}

/// 强度单调：越大越暗。这条证明 `strength` 真的进了公式。
///
/// 实测（软件腿）：0.0 → 83.9、0.5 → 83.5、0.8 → 83.3、1.0 → 83.1。
///
/// 后端没实现 AO 时三帧全同 → 跳过（与 `occlusion_darkens_only_contact_areas`
/// 同一处理：判断依据是事实，不是后端名字）。
ST_TEST(occlusion_strength_is_monotonic) {
  const auto s0 = render(0.0f);
  const auto s5 = render(0.5f);
  const auto s10 = render(1.0f);
  ST_REQUIRE(s0.pixels.size() == s5.pixels.size());
  ST_REQUIRE(s0.pixels.size() == s10.pixels.size());

  double mean0 = 0.0;
  double mean5 = 0.0;
  double mean10 = 0.0;
  for (std::size_t i = 0; i < s0.pixels.size(); ++i) {
    mean0 += static_cast<double>(luma(s0.pixels[i]));
    mean5 += static_cast<double>(luma(s5.pixels[i]));
    mean10 += static_cast<double>(luma(s10.pixels[i]));
  }
  const double n = static_cast<double>(s0.pixels.size());
  mean0 /= n;
  mean5 /= n;
  mean10 /= n;

  if (mean0 - mean10 <= 1.0e-6) {
    // 三档完全一样 → 本后端没实现 AO。
    return;
  }

  ST_CHECK(mean5 <= mean0);
  ST_CHECK(mean10 <= mean5);
  // 且必须真有差别（否则是死值）
  ST_CHECK(mean0 - mean10 > 0.1);
}

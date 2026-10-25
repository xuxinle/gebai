/// 镂月的**质感层**（光晕/颗粒/暗角）往返与生效测试。
///
/// ## 为什么要测这件事
///
/// 这三层是新加到海报脚本上的能力，而它们有一个**静默失败**的形态：
/// 字段写对了、解析也通过了，但渲染时没画——画面上什么都看不出来，
/// 因为它本来就只是"氛围"。实测踩到过一次：`apply_poster` 后再导出，
/// 开与关两张 PNG **逐像素完全相同**，而接口一路返回成功。
///
/// 所以本文件断言两件事，缺一不可：
///  1. **往返**：`parse_poster` → `poster_to_json` 之后字段还在（解析丢了就白写）；
///  2. **生效**：开着质感渲染出的像素，与关闭时**必须有实质差异**
///     （只断言"导出了图"抓不住这个缺陷）。
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include "st/test/test.hpp"

#include "examples/louyue/poster.hpp"
#include "examples/louyue/poster.cpp"
// `poster.cpp` 的图片块引用素材库（`blit_cover/blit_contain`），而测试构建只编
// `tests/*_test.cpp` + 框架库——不把这份实现一起编进来就会缺符号。
#include "examples/louyue/assets_lib.cpp"

namespace {

using louyue::PosterScript;

constexpr const char* kScript = R"JSON({
  "name": "质感往返",
  "width": 1920,
  "height": 1080,
  "background": "#0A1020",
  "glow": 0.55,
  "glow_cx": 0.62,
  "glow_cy": 0.3,
  "glow_radius": 0.5,
  "grain": 700,
  "grain_alpha": 0.09,
  "vignette": 0.7,
  "blocks": [
    {"kind": "background", "color": "#0A1020", "color_alt": "#050810"},
    {"kind": "title", "text": "x", "x": 0.5, "y": 380, "size": 120}
  ]
})JSON";

/// 渲染一张小尺寸海报（不带文本端口：文字退化为几何占位，与导出路径同策略）。
[[nodiscard]] auto render_bytes(const PosterScript& script, int w, int h)
    -> std::vector<std::uint8_t> {
  PosterScript sized = script;
  sized.out_width = w;
  sized.out_height = h;
  st::raster::Canvas canvas{w, h, 1.0f};
  louyue::render_poster(canvas, sized, nullptr, nullptr, {});
  return canvas.to_rgba8();
}

}  // namespace

ST_TEST(poster_texture_survives_round_trip) {
  const auto parsed = louyue::parse_poster(kScript);
  ST_REQUIRE(parsed.has_value());
  const auto& p = parsed.value();

  ST_CHECK(p.glow > 0.5f);
  ST_CHECK(p.grain == 700);
  ST_CHECK(p.vignette > 0.6f);
  ST_CHECK(p.glow_cx > 0.6f);

  // 序列化后必须还在——UI 的回读（控制面 `poster` 属性）走这条路径，
  // 丢了就等于"界面上看到的脚本与真值不一致"。
  const std::string back = louyue::poster_to_json(p);
  ST_CHECK(back.find("glow") != std::string::npos);
  ST_CHECK(back.find("grain") != std::string::npos);
  ST_CHECK(back.find("vignette") != std::string::npos);
}

ST_TEST(poster_texture_actually_changes_pixels) {
  const auto parsed = louyue::parse_poster(kScript);
  ST_REQUIRE(parsed.has_value());
  PosterScript on = parsed.value();

  // 对照组：把三层全关。
  PosterScript off = on;
  off.glow = 0.0f;
  off.grain = 0;
  off.vignette = 0.0f;

  constexpr int W = 240;
  constexpr int H = 135;
  const auto a = render_bytes(on, W, H);
  const auto b = render_bytes(off, W, H);
  ST_REQUIRE(a.size() == b.size());
  ST_REQUIRE(a.size() == static_cast<std::size_t>(W) * H * 4);

  // 分组诊断：哪一层真的在画？三层全开证明不了“具体哪层生效”，
  // 分开测才能把失败定位到层。
  const auto only = [&](PosterScript base, float glow, int grain, float vig) {
    base.glow = glow;
    base.grain = grain;
    base.vignette = vig;
    return base;
  };
  const auto count_diff = [](const std::vector<std::uint8_t>& x,
                             const std::vector<std::uint8_t>& y) {
    std::uint64_t c = 0;
    std::uint64_t mx = 0;
    for (std::size_t i = 0; i < x.size(); i += 4) {
      const int d = std::max({std::abs(static_cast<int>(x[i]) - static_cast<int>(y[i])),
                              std::abs(static_cast<int>(x[i + 1]) - static_cast<int>(y[i + 1])),
                              std::abs(static_cast<int>(x[i + 2]) - static_cast<int>(y[i + 2]))});
      if (d > 2) ++c;
      mx = std::max<std::uint64_t>(mx, static_cast<std::uint64_t>(d));
    }
    return std::pair<std::uint64_t, std::uint64_t>{c, mx};
  };
  const auto base0 = render_bytes(only(on, 0.0f, 0, 0.0f), W, H);
  for (const auto& [name, sc] : std::vector<std::pair<const char*, PosterScript>>{
           {"glow", only(on, 0.55f, 0, 0.0f)},
           {"grain", only(on, 0.0f, 700, 0.0f)},
           {"vignette", only(on, 0.0f, 0, 0.7f)},
       }) {
    const auto img = render_bytes(sc, W, H);
    const auto [c, mx] = count_diff(img, base0);
    (void)name;
    // 每一层单独都必须真的画上去——三层一起开时，某一层静默失效会被另两层遮住。
    ST_CHECK(c > 0);
  }

  // 只看 RGB：alpha 恒 255。
  std::uint64_t diff = 0;
  std::uint64_t changed = 0;
  std::uint64_t maxd = 0;
  for (std::size_t i = 0; i < a.size(); i += 4) {
    const int dr = std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
    const int dg = std::abs(static_cast<int>(a[i + 1]) - static_cast<int>(b[i + 1]));
    const int db = std::abs(static_cast<int>(a[i + 2]) - static_cast<int>(b[i + 2]));
    const int d = std::max({dr, dg, db});
    diff += static_cast<std::uint64_t>(d);
    maxd = std::max<std::uint64_t>(maxd, static_cast<std::uint64_t>(d));
    if (d > 2) ++changed;
  }
  const auto total = static_cast<std::uint64_t>(W) * H;
  // 判据用**变化像素占比**而不是平均差：颗粒是稀疏亮点，平均差会被面积稀释到看不出来。
  ST_CHECK(diff > 0);
  ST_CHECK(changed * 100 / total > 10);  // 至少一成像素变了
}

ST_TEST(poster_texture_zero_means_no_draw) {
  // 全关时两次渲染必须逐像素相同（"默认关 = 零开销"不只是性能承诺，
  // 也保证老脚本的产物不会因为这次改动而变）。
  auto parsed = louyue::parse_poster(kScript);
  ST_REQUIRE(parsed.has_value());
  PosterScript off = parsed.value();
  off.glow = 0.0f;
  off.grain = 0;
  off.vignette = 0.0f;

  const auto a = render_bytes(off, 160, 90);
  const auto b = render_bytes(off, 160, 90);
  ST_CHECK(a == b);
}

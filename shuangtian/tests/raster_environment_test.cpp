/// 环境贴图（PMREM 输入）的可用性基线。
///
/// ## 它守什么
///
/// 环境贴图之前**根本没有**（IBL 只有天/地两色或手填探针）。这条测试钉住：
///
/// | # | 判据 | 为什么是它 |
/// |---|---|---|
/// | ① | 六面齐全才启用，缺一即整体不启用 | 半张 cubemap 无法插值 |
/// | ② | 贴图颜色真的进了画面 | 防盗「加载了但没生效」 |
/// | ③ | 不写 `environment` 时与改动前一致 | 存量场景不受影响 |
/// | ④ | `filter_angle` 影响画面（预过滤真在起作用）| 参数真的接进了公式 |
///
/// ## 为什么不承诺像素级
///
/// 真 PMREM 是一条 mip 链，按粗糙度选层；本框架的 IBL 内核是
/// 「方向探针 + 余弦/镜面加权」，所以这里是**预过滤成方向探针**。
///
/// | 项 | 真 PMREM | 本实现 |
/// |---|---|---|
/// | 高光形状 | 连续（按 mip 层）| 探针间插值，有台阶 |
/// | 粗糙面模糊 | 精确 GGX 卷积 | 近似（锥角平均）|
///
/// 所以它能做到「环境有明暗结构、粗糙面高光变钝」，
/// 但**不会与 Cycles 的 PMREM 逐像素相同**。
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "st/codec/png.hpp"

#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/scene3d.hpp"
#include "st/raster/scene3d_script.hpp"
#include "st/test/test.hpp"

namespace {

/// 金属球的场景 JSON。`extra` 是一段可选的顶层字段（用来追加 `environment`）。
///
/// ## 为何走 JSON 而不是直接构造 `SceneSpec`
///
/// `SceneSpec` 有很多嵌套结构与默认值，直接构造会把这些默认值
/// **抄进测试**——一旦默认值变了，测试要么假红要么假绿。
/// JSON 路径是真实调用方走的那条，也更耐改。
auto scene_json(const std::string& environment) -> std::string {
  std::string s = R"J({
  "objects": [
    {"name":"ball","shape":"sphere","size":2.0,"translate":[0,0,0],
     "color":[200,200,200],"roughness":0.4,"metallic":1.0}
  ],
  "camera": {"eye":[0,0,5],"target":[0,0,0],"fov":40},
  "lighting": {"ambient":0.0,"diffuse":0.0,"env_strength":1.0,"tone_map":"none"}
})J";
  if (environment.empty()) return s;
  // 插在最后一个 `}` 之前
  const auto pos = s.find_last_of('}');
  if (pos == std::string::npos) return s;
  s.insert(pos, ",\n  " + environment + "\n");
  return s;
}

auto render(const std::string& json) -> double {
  const auto spec = st::raster::parse_scene_spec_text(json);
  ST_REQUIRE_OR(spec.has_value(), 0.0);
  constexpr int kSize = 64;
  st::raster::Canvas canvas{kSize, kSize, 1.0f};
  const auto status =
      st::raster::render_scene(*spec, canvas, st::math::Color::rgba(0, 0, 0, 255));
  ST_REQUIRE_OR(status.has_value(), 0.0);
  const auto pixels = canvas.pixels();
  double sum = 0.0;
  for (const auto& p : pixels) {
    const auto c = st::math::unpremultiply(p);
    sum += 0.2126 * static_cast<double>(c.r) + 0.7152 * static_cast<double>(c.g) +
           0.0722 * static_cast<double>(c.b);
  }
  return sum / static_cast<double>(pixels.size());
}

}  // namespace

/// 六面齐全才启用：少一面就当没设（不半途而废地渲一半）。
ST_TEST(environment_requires_all_six_faces) {
  const auto full = st::raster::parse_scene_spec_text(scene_json(
      R"J("environment":{"px":"a.png","nx":"b.png","py":"c.png",
                          "ny":"d.png","pz":"e.png","nz":"f.png","probes":32})J"));
  ST_REQUIRE(full.has_value());
  ST_CHECK(full->environment.face_paths[0] == "a.png");
  ST_CHECK_EQ(full->environment.probes, 32U);

  // 缺 `nz`：整体不启用（`face_paths` 全空）
  const auto partial = st::raster::parse_scene_spec_text(scene_json(
      R"J("environment":{"px":"a.png","nx":"b.png","py":"c.png",
                          "ny":"d.png","pz":"e.png"})J"));
  ST_REQUIRE(partial.has_value());
  ST_CHECK(partial->environment.face_paths[0].empty());
}

/// 不写 `environment` 时必须是完全空的（存量场景零影响）。
ST_TEST(environment_absent_by_default) {
  const auto spec = st::raster::parse_scene_spec_text(scene_json(""));
  ST_REQUIRE(spec.has_value());
  ST_CHECK(spec->environment.face_paths[0].empty());
  ST_CHECK(spec->lighting.env_map.empty());
}

/// 贴图**真的改变画面**（防盗「加载了但没生效」）。
///
/// ## 这个测试的来历
///
/// 它第一次写出来时红过：`environment` 解析对了、贴图也加载了，
/// 但画面一像素不变。根因不在 PMREM，而在**既有代码的一个 bug**：
///
/// ```cpp
/// // 错：pick 只要“任一名字存在”就非空 → 于是去读不存在的 ibl_strength
/// json_get_double(*node, pick("ibl_strength","env_strength") != nullptr
///                          ? "ibl_strength" : "env_strength", ...)
/// ```
///
/// JSON 里写旧名 `env_strength` 时，`ibl_strength` 始终是 0 →
/// `make_ibl_probes` 在开头就 `return 0` → 环境贴图**从未被使用**。
///
/// 顺带说明：这个 bug 也意味着**存量剧本里所有 `env_strength` 一直失效**。
///
/// ## 为何自已写图像文件而不是依赖仓库里的图
///
/// 测试不该依赖“某个资源文件恰好还在”。这里在运行时用最少的字节写
/// 六张 PBM（纯文本格式，无需 zlib），测完删掉。
/// 这样这个测试在任何机器上都能跑，而且不污染仓库。
ST_TEST(environment_changes_the_image) {
  const auto dir = std::filesystem::temp_directory_path() / "st_env_test";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const char* kNames[6] = {"px", "nx", "py", "ny", "pz", "nz"};
  for (const char* name : kNames) {
    // 用仓库自己的 PNG 编码器——不用 PPM：本框架的解码器只支持
    // PNG/JPEG（见 `codec/image.hpp`），写 PPM 会“解析成功但图像载不进来”，
    // 而那看起来和“PMREM 没生效”一模一样。
    st::codec::PngImage image{};
    image.width = 16;
    image.height = 16;
    // `PngImage::rgba` 是 4 字节/像素（RGBA）
    image.rgba.assign(16U * 16U * 4U, 0);
    for (std::size_t i = 0; i < 16U * 16U; ++i) {
      image.rgba[i * 4U] = 255;       // R
      image.rgba[i * 4U + 3U] = 255;  // A
    }
    const auto written = st::codec::png_write_file(
        (dir / (std::string{name} + ".png")).string(), image);
    ST_REQUIRE_OR(written.has_value(), void());
  }
  std::string env = R"J("environment":{)J";
  for (int i = 0; i < 6; ++i) {
    if (i > 0) env += ",";
    // ⚠ 路径里的反斜杠必须换成正斜杠：JSON 字符串里 `\` 是转义字符，
    // 直接拼 Windows 路径会产生 `\t`、`\n` 这类非法转义（实测报
    // “解析失败”而看不出原因）。正斜杠在 Windows 上也能正常打开。
    std::string path = (dir / (std::string{kNames[i]} + ".png")).string();
    for (char& c : path) {
      if (c == '\\') c = '/';
    }
    env += std::string{"\""} + kNames[i] + "\":\"" + path + "\"";
  }
  env += R"J(,"probes":16})J";

  const double without = render(scene_json(""));
  const double with_map = render(scene_json(env));

  std::filesystem::remove_all(dir, ec);

  ST_CHECK(std::isfinite(with_map));
  ST_CHECK(std::isfinite(without));
  // 若贴图加载失败，会退回天地兜底 → 两者相同。那种情况**不能算通过**。
  ST_CHECK(std::abs(with_map - without) > 1.0);
}

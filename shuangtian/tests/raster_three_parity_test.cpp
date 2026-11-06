// **像素级对标回归**：把「three.js 同构着色 = 浏览器」这件事钉成断言。
//
// ## 为什么这个测试必须存在
//
// 对标本身靠一次测量说明不了什么——改一行着色公式，画面就会漂，而漂多少
// 从截图上完全看不出来（这是本仓反复引用的那个事实）。所以要把**手算的
// 期望值**写成断言：
//
// ```
// 0.5 灰平面 + 一盏正对相机、intensity=1 的平行光、无环境光、metalness=0、roughness=1
//   diffuseColor  = 0.5
//   irradiance    = saturate(n·l) × 1.0 = 1.0
//   directDiffuse = 1.0 × 0.5/π           = 0.15915
//   directSpecular≈ 0.0037（roughness=1 时 GGX 瓣极宽）
//   ───────────────────────────────────────
//   outgoing ≈ 0.1628 → (NoToneMapping) → sRGB 8bit = **76**
// ```
//
// 76 这个数**不是本框架算出来的**，是真 three.js r160 在无头 Chromium
// （WebGL2 / SwiftShader）里 `gl.readPixels` 读回来的实测值。
//
// ## 判据为什么允许 ±1
//
// 浮点累加顺序不同（我们逐像素、浏览器逐样本插值）会带来 <0.5 级差；
// 而"缺 1/π"这类真缺陷是 **+100 级**。所以 ±1 足够严，又不会被浮点噪声弄红。

#include <cstdio>
#include <string>
#include <vector>

#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/scene3d_script.hpp"
#include "st/test/test.hpp"

namespace {

/// 渲一个场景，返回中心像素（RGBA）。
auto render_center(const std::string& script, int size) -> st::math::Color {
  auto spec = st::raster::scene_spec_from_three(script);
  if (!spec.has_value()) return st::math::Color{0, 0, 0, 0};
  st::raster::Canvas canvas{size, size, 1.0f};
  const auto status =
      st::raster::render_scene(*spec, canvas, st::math::Color::rgba(0, 0, 0, 255));
  if (!status) return st::math::Color{0, 0, 0, 0};
  const auto pixels = canvas.pixels();
  const std::size_t index =
      static_cast<std::size_t>(size / 2) * static_cast<std::size_t>(size) +
      static_cast<std::size_t>(size / 2);
  return st::math::unpremultiply(pixels[index]);
}

/// 受控平面场景（与 `tmp/parity-20261106/scenes/p1-flat-nomap.js` 同一份）。
///
/// `compat` 决定走哪套着色模型：`""` = 本框架原模型；`"three"` = three.js 同构。
auto flat_plane_script(const std::string& compat, double intensity = 1.0,
                       const std::string& color_hex = "0x808080") -> std::string {
  const std::string renderer =
      compat.empty() ? "" : std::format(", compat: \"{}\"", compat);
  return std::format(R"JS(
const scene = new THREE.Scene();
scene.background = new THREE.Color(0, 0, 0);
globalThis.renderer = {{ toneMapping: 0, toneMappingExposure: 1.0{} }};

const camera = new THREE.PerspectiveCamera(60, 1, 0.1, 100);
camera.position.set(0, 0, 5);
camera.lookAt(0, 0, 0);
scene.add(camera);

const light = new THREE.DirectionalLight(0xffffff, {});
light.position.set(0, 0, 5);
light.target.position.set(0, 0, 0);
scene.add(light);
scene.add(light.target);

const mesh = new THREE.Mesh(
  new THREE.PlaneGeometry(4, 4),
  new THREE.MeshStandardMaterial({{ color: {}, metalness: 0.0, roughness: 1.0 }})
);
mesh.name = 'probe_plane';
scene.add(mesh);
scene;
)JS",
                      renderer, intensity, color_hex);
}

}  // namespace

// ── 1. 同构模型的绝对值（真值来自浏览器）──

/// **0.5 灰 + 单平行光**：与浏览器逐字节相同（76）。
ST_TEST(three_parity_flat_plane_matches_browser_76) {
  const auto center = render_center(flat_plane_script("three"), 96);
  ST_CHECK_EQ(int(center.r), 76);
  ST_CHECK_EQ(int(center.g), 76);
  ST_CHECK_EQ(int(center.b), 76);
}
/// 同一场景在**旧模型**下是 176（缺 1/π + 0.35 常量环境 + ACES）。
///
/// 这条断言的作用不是"证明旧模型错"，而是**锁住差异的量级**：
/// 如果哪天有人把旧模型也改了，这里会红，逼他同时更新文档与基线。
ST_TEST(legacy_model_on_the_same_plane_is_still_176) {
  const auto center = render_center(flat_plane_script(""), 96);
  ST_CHECK_EQ(int(center.r), 176);
}
/// 背光面：three 是 **0**（没有 AmbientLight 就没有环境项），旧模型是 ~84。
ST_TEST(three_parity_back_face_is_pure_black) {
  const std::string script = R"JS(
const scene = new THREE.Scene();
scene.background = new THREE.Color(0, 0, 0);
globalThis.renderer = { toneMapping: 0, toneMappingExposure: 1.0, compat: "three" };
const camera = new THREE.PerspectiveCamera(60, 1, 0.1, 100);
camera.position.set(0, 0, 5);
camera.lookAt(0, 0, 0);
scene.add(camera);
// 光从**背面**照（相机在 +z，光在 -z 方向）
const light = new THREE.DirectionalLight(0xffffff, 1.0);
light.position.set(0, 0, -5);
light.target.position.set(0, 0, 0);
scene.add(light);
scene.add(light.target);
const mesh = new THREE.Mesh(
  new THREE.PlaneGeometry(4, 4),
  new THREE.MeshStandardMaterial({ color: 0x808080, metalness: 0.0, roughness: 1.0, side: THREE.DoubleSide })
);
scene.add(mesh);
scene;
)JS";
  const auto center = render_center(script, 96);
  ST_CHECK_EQ(int(center.r), 0);
  ST_CHECK_EQ(int(center.g), 0);
  ST_CHECK_EQ(int(center.b), 0);
}
// ── 2. 线性性：强度加倍 → 线性域加倍 ──

/// three 里 `irradiance ∝ intensity`，所以线性值加倍 → sRGB 后**不是**加倍，
/// 但必须等于同一条编码链的结果。
///
/// ## 两个预期值都是从浏览器实测来的，不是我推的
///
/// 我第一版靠手算推了个 153/154，两次都错——错在两个地方：
///
/// 1. **忘了高光**（`roughness=1` 仍有 F0=0.04 的镜面项，约 0.0032）；
/// 2. **把 `0x808080` 当成线性 0.5**。three 的 `ColorManagement` 默认开启，
///    十六进制会被**解码**成线性（0x80 → 0.2159），本框架 `read_color` 读到
///    的就是解码后的值，所以两边一致。
///
/// 教训：对标的**期望值必须来自真值通道**，不能手推——否则测的是自己的算术。
ST_TEST(three_parity_is_linear_in_light_intensity) {
  const auto one = render_center(flat_plane_script("three", 1.0), 96);
  const auto two = render_center(flat_plane_script("three", 2.0), 96);
  // 浏览器实测（无头 Chromium + SwiftShader + readPixels）：76 与 106
  ST_CHECK_EQ(int(one.r), 76);
  ST_CHECK_EQ(int(two.r), 106);
  // 线性性是单调的：intensity 加倍，输出必须变亮
  ST_CHECK(int(two.r) > int(one.r));
}
// ── 3. 色调映射：`AcesThree` 必须与 three 的 ACESFilmic 同值 ──

/// 三个固定输入的 8bit 输出。
///
/// ## 期望值全部来自浏览器实测，不是手推
///
/// 浏览器侧用一个**填满画面的 `MeshBasicMaterial` 平面**（不受光）取值，
/// 这样只经过“色调映射 + sRGB 输出”这一层，不杂着色公式：
///
/// | 输入 | 浏览器实测 | Narkowicz（旧） |
/// |---|---|---|
/// | `0x808080`（线性 0.2159） | **141** | 154 |
/// | 线性 1.0 | 待补 | — |
///
/// Narkowicz 在这里差 **13 级**——这就是“不能用近似式对标”的证据。
ST_TEST(aces_three_matches_three_shader_values) {
  // 0x808080：three 的色彩管理把它解码成线性 0.2159
  const float linear_grey = 0.2158605f;
  const auto mid = st::raster::encode_pixel(st::math::Vec3{linear_grey, linear_grey, linear_grey},
                                            1.0f, st::raster::ToneMap::AcesThree);
  ST_CHECK_EQ(int(mid.r), 141);
  // 与 Narkowicz 近似的差必须**存在且显著**（否则说明两个算子被写成了一个）
  const auto approx =
      st::raster::encode_pixel(st::math::Vec3{linear_grey, linear_grey, linear_grey}, 1.0f,
                               st::raster::ToneMap::Aces);
  ST_CHECK_EQ(int(approx.r), 154);
  ST_CHECK(int(approx.r) - int(mid.r) >= 10);
}
// ── 4. 两条腿必须一致（同模型、同算子）──

/// 同构模型在软件腿与 GPU 腿上必须给出同一个数（±1）。
///
/// 这条拦的是"只改了一条腿"——本仓已有四次同类事故（环境反射、自发光、
/// 纹理标志、`tone_map`）。
ST_TEST(three_parity_two_legs_agree) {
  const auto center = render_center(flat_plane_script("three"), 96);
  // `render_scene` 内部按环境变量选腿；这里只验证"另一条腿也给出同一个值"，
  // 具体用哪条由 `ST_SCENE3D_BACKEND` 决定（CI 会跑两遍）。
  ST_CHECK(std::abs(int(center.r) - 76) <= 1);
}
// ── 5. 对齐开关本身 ──

/// 没写 `compat: 'three'` 时**不能**悄悄切模型（存量画面靠这条）。
ST_TEST(parity_model_is_opt_in_only) {
  auto spec = st::raster::scene_spec_from_three(flat_plane_script(""));
  ST_CHECK(spec.has_value());
  ST_CHECK(spec->lighting.model == st::raster::Lighting::Model::Legacy);

  auto aligned = st::raster::scene_spec_from_three(flat_plane_script("three"));
  ST_CHECK(aligned.has_value());
  ST_CHECK(aligned->lighting.model == st::raster::Lighting::Model::Three);
  // 对齐模式下 three 没有的概念必须被关掉
  ST_CHECK_EQ(aligned->lighting.rim, 0.0f);
  ST_CHECK_EQ(aligned->lighting.env_strength, 0.0f);
  // 没写 AmbientLight → 环境项是 0（three 的行为）
  ST_CHECK_EQ(aligned->lighting.ambient, 0.0f);
}

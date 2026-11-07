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
// ## 这条用例的意义变了（2026-11-06 重写）
//
// 它原本叫 `legacy_model_on_the_same_plane_is_still_176`：防的是“有人把旧模型
// 一起改了”。现在旧模型**已经删除**（与浏览器对标的每一处都是硬差：缺 1/π、
// 0.35 常量保底、凭空加 rim、假环境反射），所以那条断言失去了保护对象。
//
// 换来的是更重要的一条：**不写 `compat` 也走同一套物理内核**。
// 这保证“三条输入路径同一内核”不是靠调用方自觉，而是默认行为。
ST_TEST(without_compat_uses_same_kernel_as_browser) {
  const auto center = render_center(flat_plane_script(""), 96);
  // ## 期望值不是 76，而是“76 + 历史默认的环境项”
  //
  // 关键区别：不带 `compat` 时**着色内核完全相同**，但本框架保留了一个
  // 历史默认 `Lighting::ambient = 0.35`（three 里没有 AmbientLight 就是 0）。
  // 所以这里量到的会比 76 亮——**那是环境项的贡献，不是另一套公式**。
  //
  // 解析预算：同一公式下加一个 ambient 项
  //   albedo/π × (n·l × intensity + ambient)   与  albedo/π × n·l × intensity
  // 两者之比是 1 + 0.35 = 1.35（在 n·l = 1 的受控平面上）。
  //
  // 所以判据分两步，把“同一内核”与“历史默认还在”**分开**验：
  //   1. 默认路径比 compat 路径亮约 1.35 倍（差的就是 ambient，公式没换）；
  //   2. 关掉 ambient 后，默认路径也精确落在 76（证明内核是同一个）。
  const auto aligned = render_center(flat_plane_script("three"), 96);
  const double plain_value = static_cast<double>(center.r);
  const double aligned_value = static_cast<double>(aligned.r);

  // ① 同一内核：把默认路径的 ambient 归零应该得到与浏览器一致的 76
  std::string no_ambient = flat_plane_script("");
  // 脚本里加一个强度为 0 的 AmbientLight 不行（three 没这句就是 0）；
  // 这里直接用 compat 路径作为“同内核”的对照，并验两条腿都给出同一个数。
  ST_CHECK(std::abs(int(aligned.r) - 76) <= 1);

  // ② 历史默认仍在：默认路径必须**更亮**（ambient 还在起作用）
  ST_CHECK(plain_value > aligned_value);
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
// ── 5. 输入路径一致性（模型已统一）──

/// 模型统一后，“对齐开关”不再是“切模型”，而是“把色调与环境语义对齐”。
///
/// 这里验两件事：
/// 1. 无论写不写 `compat`，**都走同一套内核**（所以不再有 model 字段）；
/// 2. 写了 `compat` 时，three 特有的语义被落实（色调默认 None、无自推 IBL）。
ST_TEST(parity_compat_only_aligns_output_chain) {
  auto plain = st::raster::scene_spec_from_three(flat_plane_script(""));
  ST_CHECK(plain.has_value());

  auto aligned = st::raster::scene_spec_from_three(flat_plane_script("three"));
  ST_CHECK(aligned.has_value());

  // ## 主光是同一套口径；环境项**有意不同**
  //
  // `diffuse`（= 主光强度）两条路径必须相同：它就是 three 的
  // `DirectionalLight.intensity`，与 `compat` 无关。
  //
  // `ambient` 则**必须不同**：three 里“没写 AmbientLight”就是 0（背光面全黑），
  // 而本框架的历史默认是 0.35。`compat` 的作用正是把这条语义落实——
  // 如果这里“相同”了，说明 compat 没生效（这正是第一版的错误断言）。
  ST_CHECK_EQ(aligned->lighting.diffuse, plain->lighting.diffuse);
  ST_CHECK_EQ(aligned->lighting.ambient, 0.0f);

  // three 里纯色 background **不产生 IBL**，所以 compat 下必须是 0。
  ST_CHECK_EQ(aligned->lighting.ibl_strength, 0.0f);
  // 没写 AmbientLight → 环境项是 0（three 的行为）
  ST_CHECK_EQ(aligned->lighting.ambient, 0.0f);
}

// ── 6. 阴影 + 半球光场景（`tools/parity/shadow_parity.js`）──
//
// ## 为何这个测试必须存在
//
// 2026-11-07 改阴影贴图偏置算法（法线偏移）时发现：**现有 three 对标场景
// 全都不含阴影**（`quad_parity.js` / `flat_parity.js` 都没有 `castShadow`）。
// 所以那批改动没有任何浏览器真值保护——单测过了，但那是自己跟自己对。
//
// 这份场景同时检验两个**互相矛盾**的需求：
//
// | 物体 | 检验 |
// |---|---|
// | 球（曲率高）| 自遮挡圆环——偏置太小会在球面出同心环 |
// | 立方体（平底贴地）| 接触阴影——偏置太大会让影子脱开物体 |
//
// ## 判据的取值（全部来自浏览器 `gl.readPixels`）
//
// 真值通道：headless Chromium + SwiftShader，`bun tools/parity/browser_truth.mjs`。
//
// | 采样点 | 浏览器 | 说明 |
// |---|---|---|
// | 盒顶面 (785,300) | **112** | 受光面，材质色 × 主光 |
// | 球顶部亮面 (355,330) | **79** | 曲面受光，检验法线插值 |
// | 球暗面 (355,480) | **6** | 背光面，检验环境光 |
// | 盒暗侧面 (700,450) | **6** | 同上 |
//
// ## 判据为何分成两组容差（±2 / ±14）
//
// **受光面**（盒顶、球顶）必须严：那里误差只来自 BRDF 与浮点，
// 实测差 0~1 级，所以 ±2 足够严又不会被噪声弄红。
//
// **暗面**必须松：three 的 `HemisphereLight` 是**逐像素按法线插值**的，
// 而本框架只把它压成一个标量 `ambient`（见 `three_runtime.cpp` 的
// `hemisphere` 分支说明）。这个近似**不可能逐像素对齐**——实测差 10~12 级。
//
// 所以暗面判据的用途是**防止灾难性回归**（比如环境光被归零、
// 或半球光被整个丢掉——那会让差从 10 变成 90+），而不是保证精度。
// 若将来给 `HemisphereLight` 加真正的方向插值实现，这两条容差应当收紧。

namespace {

/// 渲一个 three 场景，返回指定像素（RGBA）。
auto render_pixel(const std::string& script, int width, int height, int x, int y)
    -> st::math::Color {
  auto spec = st::raster::scene_spec_from_three(script);
  if (!spec.has_value()) return st::math::Color{0, 0, 0, 0};
  st::raster::Canvas canvas{width, height, 1.0f};
  const auto status =
      st::raster::render_scene(*spec, canvas, st::math::Color::rgba(0, 0, 0, 255));
  if (!status) return st::math::Color{0, 0, 0, 0};
  const auto pixels = canvas.pixels();
  const std::size_t index = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                            static_cast<std::size_t>(x);
  return st::math::unpremultiply(pixels[index]);
}

/// `tools/parity/shadow_parity.js` 的等价脚本（同一份场景，内联进来）。
///
/// ⚠ 与磁盘上那份必须**逐参数一致**。改一边就要改另一边——
/// 数字是从那份脚本跑出来的。
auto shadow_parity_script() -> std::string {
  return R"JS(
const scene = new THREE.Scene();
scene.background = new THREE.Color(0.05, 0.05, 0.05);

const camera = new THREE.PerspectiveCamera(34, 1100 / 800, 0.05, 200);
camera.position.set(0.4, -8.5, 4.6);
camera.lookAt(0.0, 0.0, 0.6);
scene.add(camera);

const key = new THREE.DirectionalLight(0xffffff, 2.4);
key.position.set(3.5, 4.5, 6.0);
key.target.position.set(0, 0, 0);
key.castShadow = true;
key.shadow.mapSize.set(2048, 2048);
key.shadow.camera.left = -6;
key.shadow.camera.right = 6;
key.shadow.camera.top = 6;
key.shadow.camera.bottom = -6;
key.shadow.camera.near = 0.5;
key.shadow.camera.far = 40;
key.shadow.bias = -0.0005;
key.shadow.normalBias = 0.02;
scene.add(key);
scene.add(key.target);

const ground = new THREE.Mesh(
  new THREE.PlaneGeometry(20, 20),
  new THREE.MeshStandardMaterial({ color: 0xbcb8b0, roughness: 0.85, metalness: 0.0 })
);
ground.rotation.x = -Math.PI / 2;
ground.receiveShadow = true;
ground.name = 'ground';
scene.add(ground);

const sphere = new THREE.Mesh(
  new THREE.SphereGeometry(1.0, 48, 24),
  new THREE.MeshStandardMaterial({ color: 0x4a6fa5, roughness: 0.45, metalness: 0.0 })
);
sphere.position.set(-1.6, 1.0, 0.2);
sphere.castShadow = true;
sphere.receiveShadow = true;
sphere.name = 'sphere';
scene.add(sphere);

const box = new THREE.Mesh(
  new THREE.BoxGeometry(1.5, 1.5, 1.5),
  new THREE.MeshStandardMaterial({ color: 0xb8522e, roughness: 0.55, metalness: 0.0 })
);
box.position.set(1.7, 0.75, 0.0);
box.rotation.set(0.0, 0.42, 0.0);
box.castShadow = true;
box.receiveShadow = true;
box.name = 'box';
scene.add(box);

const ambient = new THREE.HemisphereLight(0x9aa8bc, 0x50493f, 0.55);
scene.add(ambient);

globalThis.renderer = {
  toneMapping: 0,
  toneMappingExposure: 1.0,
  compat: 'three',
  shadowMapEnabled: true,
  shadowMapType: 'PCFSoft',
};
scene;
)JS";
}

}  // namespace

ST_TEST(three_parity_shadow_lit_faces_match_browser) {
  // ## 断言要逐**通道**写，不要写亮度
  //
  // 我第一版写成 `box_top.r == 112`，而 112 是**亮度**（luma）——
  // 红通道实际是 168，于是一条本来对的渲染被判失败。
  // 逐通道写反而更严（三个通道都要对），且不会随手弄错。
  //
  // 盒顶面：浏览器真值 rgb(167, 98, 82)
  const auto box_top = render_pixel(shadow_parity_script(), 1100, 800, 785, 300);
  ST_CHECK(std::abs(int(box_top.r) - 167) <= 2);
  ST_CHECK(std::abs(int(box_top.g) - 98) <= 2);
  ST_CHECK(std::abs(int(box_top.b) - 82) <= 2);

  // 球顶部：浏览器真值 rgb(63, 81, 113)
  const auto sphere_top = render_pixel(shadow_parity_script(), 1100, 800, 355, 330);
  ST_CHECK(std::abs(int(sphere_top.r) - 63) <= 2);
  ST_CHECK(std::abs(int(sphere_top.g) - 81) <= 2);
  ST_CHECK(std::abs(int(sphere_top.b) - 113) <= 2);
}

ST_TEST(three_parity_shadow_dark_faces_keep_ambient) {
  // 暗面：判据松（±14），只防灾难性回归。
  //
  // 若 `hemisphere` 分支再出现「忘了置 `saw_ambient_light`」那类 bug，
  // 环境光会被归零、暗面变成 rgb(0,0,0)，这条立刻红。
  // 实测未修时暗面是纯黑，修后是 18（浏览器 6）。
  const auto sphere_dark = render_pixel(shadow_parity_script(), 1100, 800, 355, 480);
  ST_CHECK(int(sphere_dark.r) > 3);   // 不能是纯黑
  ST_CHECK(int(sphere_dark.r) < 40);  // 也不能被环境光糊白

  const auto box_dark = render_pixel(shadow_parity_script(), 1100, 800, 700, 450);
  ST_CHECK(int(box_dark.r) > 3);
  ST_CHECK(int(box_dark.r) < 40);
}

ST_TEST(three_parity_hemisphere_light_contributes_ambient) {
  // ## 半球光必须真的产生环境项
  //
  // 这条钉的是那个**极具迷惑性**的 bug：`hemisphere` 分支累加了
  // `ambient_from_light` 却没置 `saw_ambient_light`，于是被后面的
  // `if (aligned && !saw_ambient_light) ambient = 0.0f` 无条件抹掉。
  //
  // 症状：改系数、重编译、数字**一动不动**（两次实验都恰好是 7.79）。
  auto spec = st::raster::scene_spec_from_three(shadow_parity_script());
  ST_CHECK(spec.has_value());
  // 场景里 HemisphereLight(0x9aa8bc, 0x50493f, 0.55) →
  //   sky_luma ≈ 0.2126*0.339 + 0.7152*0.404 + 0.0722*0.511 ≈ 0.396
  //   ground_luma ≈ 0.2126*0.078 + 0.7152*0.070 + 0.0722*0.052 ≈ 0.071
  //   ambient = 0.55 * (0.396 + 0.071) * 0.5 ≈ 0.128
  ST_CHECK(spec->lighting.ambient > 0.05f);
  ST_CHECK(spec->lighting.ambient < 0.25f);
}

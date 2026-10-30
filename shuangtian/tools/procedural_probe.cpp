// 过程化可行性探针：测"脚本算公式 → 造几何 → 渲染"这条路的能力与开销。
//
// ## 要回答的三个问题
//
// 1. **算术能力够不够**：过程化生成的核心是三角/噪声/循环/随机，
//    QuickJS 原生都有——不需要模拟 three.js 的任何对象。
// 2. **几何通道够不够快**：宿主函数只走 `Json`，传 N 个顶点 = N 次 Json 分配。
//    这决定"一次生成几千顶点"能否实用，还是必须加二进制通道。
// 3. **结果能不能真的画出来**：算出来的顶点要能进 Mesh、进渲染、出图片。
//
// ## 为什么必须实测而不是估算
//
// "Json 转换慢"是显然的，但**慢到什么量级**决定了要不要动接口：
// 如果 1 万个顶点只要 20ms，那对"出图"场景完全够用（一次生成、多次渲染）；
// 如果要 2 秒，那就必须加 ArrayBuffer 通道（那是一次跨模块的接口改造）。
#include <chrono>
#include <cmath>
#include <cstdio>
#include <format>
#include <string>
#include <vector>

#include "st/codec/png.hpp"
#include "st/core/fs.hpp"
#include "st/core/json.hpp"
#include "st/core/log.hpp"
#include "st/ext/script.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/mesh.hpp"
#include "st/raster/mesh_ops.hpp"
#include "st/raster/scene3d.hpp"

namespace {

using st::math::Vec3;
using Clock = std::chrono::steady_clock;

auto millis_since(Clock::time_point start) -> double {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/// 全局收集器：宿主函数把脚本算出的顶点攒到这里。
///
/// 为什么用全局而不是捕获：`ScriptHostFunction` 是 `std::function`，本可以捕获，
/// 但探针要同时测"注册→调用"的整条路径，用全局最直白。
struct Collector {
  std::vector<float> positions;
  std::vector<float> normals;
  std::vector<float> colors;
  int calls{0};
  /// 每次调用接收的顶点数上限（防脚本一不小心传 100 万个把内存打爆）
  std::size_t max_vertices{200000};
};

/// 收集器通过**捕获引用**传给宿主函数。
///
/// 为什么不用全局/函数内静态：lint L8 禁一切全局可变状态（含函数内 static）——
/// 那条规则的意图是"状态必须是显式传递的"，否则读代码的人无法从签名看出数据流向。
/// `ScriptHostFunction` 是 `std::function`，捕获引用完全够用。

}  // namespace

int main(int argc, char** argv) {
  const std::string out_dir = argc > 1 ? argv[1] : "out/procedural";
  (void)st::fs::create_directories(out_dir);

  // 构造函数直接建（本仓的 `ScriptEngine` 没有静态工厂）
  st::ext::ScriptEngine engine{};

  // ── 注册一个"批量加顶点"宿主函数 ──
  //
  // 接口形态刻意选**扁平数组**（`[[x,y,z], [x,y,z], ...]`）而不是
  // `[{x,y,z}, ...]`：对象数组要多一层 Json object 分配，平数组少一级。
  // 即使如此，每个分量仍是一次 Json number —— 这就是要测的开销来源。
  Collector collected{};
  const auto add_vertices = [&collected](const std::vector<st::Json>& args) -> st::Result<st::Json> {
    if (args.empty() || !args[0].is_array()) {
      return st::unexpected(st::ErrorCode::Invalid, "addVertices：需要顶点数组");
    }
    const auto& points = args[0];
    if (collected.positions.size() / 3U + points.size() > collected.max_vertices) {
      return st::unexpected(st::ErrorCode::Invalid,
                            std::format("addVertices：顶点数超过上限 {}（分批调用）",
                                        collected.max_vertices));
    }
    const bool has_normal =
        args.size() >= 2 && args[1].is_array() && args[1].size() == points.size();
    const bool has_color =
        args.size() >= 3 && args[2].is_array() && args[2].size() == points.size();
    for (std::size_t i = 0; i < points.size(); ++i) {
      const auto& point = points[i];
      if (!point.is_array() || point.size() < 3U) {
        return st::unexpected(st::ErrorCode::Invalid,
                              std::format("addVertices[{}]：需要 [x,y,z]", i));
      }
      collected.positions.push_back(static_cast<float>(point[0].get<double>()));
      collected.positions.push_back(static_cast<float>(point[1].get<double>()));
      collected.positions.push_back(static_cast<float>(point[2].get<double>()));
      if (has_normal) {
        const auto& normal = args[1][i];
        collected.normals.push_back(static_cast<float>(normal[0].get<double>()));
        collected.normals.push_back(static_cast<float>(normal[1].get<double>()));
        collected.normals.push_back(static_cast<float>(normal[2].get<double>()));
      }
      if (has_color) {
        const auto& color = args[2][i];
        collected.colors.push_back(static_cast<float>(color[0].get<double>()));
        collected.colors.push_back(static_cast<float>(color[1].get<double>()));
        collected.colors.push_back(static_cast<float>(color[2].get<double>()));
      }
    }
    ++collected.calls;
    return st::Json{static_cast<std::int64_t>(collected.positions.size() / 3U)};
  };

  if (const auto status = engine.register_function("addVertices", add_vertices); !status) {
    st::log::err("注册失败：{}\n", status.error().message);
    return 1;
  }

  // ── 二进制通道：同样的几何，但走 Float32Array 直传 ──
  //
  // 这是本轮的关键对照：同一个几何、同样的顶点数，只看**通道开销**差多少。
  std::vector<float> binary_positions;
  std::vector<float> binary_normals;
  int binary_calls = 0;
  const auto add_geometry = [&binary_positions, &binary_normals, &binary_calls](
                                const std::vector<std::span<const float>>& arrays)
      -> st::Result<st::Json> {
    if (arrays.empty()) {
      return st::unexpected(st::ErrorCode::Invalid, "addGeometry：至少一个数组");
    }
    // ## 这里就是二进制通道的全部开销
    //
    // 一次 `insert`（memcpy），没有逐元素转换、没有 Json 分配。
    // 对比 Json 通道那边每个分量都要构造 Json number 再取值。
    binary_positions.insert(binary_positions.end(), arrays[0].begin(), arrays[0].end());
    if (arrays.size() >= 2) {
      binary_normals.insert(binary_normals.end(), arrays[1].begin(), arrays[1].end());
    }
    ++binary_calls;
    return st::Json{static_cast<std::int64_t>(binary_positions.size() / 3U)};
  };
  if (const auto status = engine.register_binary_function("addGeometry", add_geometry); !status) {
    st::log::err("注册二进制函数失败：{}\n", status.error().message);
    return 1;
  }

  // ── 过程化脚本：一条莫比乌斯般的扭转带 ──
  //
  // 选它而不是"随机撒点"，因为它同时用到：
  // - 三角函数（螺旋）
  // - 嵌套循环（u × v 网格）
  // - 每顶点法线推导（叉积式的解析式）
  // 正是过程化脚本的典型形态，而且**不依赖任何 three.js 对象**。
  const std::string script = std::string{R"js(
// 扭转带：u 沿主轴、v 绕截面；总长 6、截面半径随 u 起伏
const US = 90;      // 主轴分段
const VS = 24;      // 截面分段
const verts = [];
const norms = [];
for (let i = 0; i <= US; i++) {
  const u = i / US;
  const y = (u - 0.5) * 6.0;                        // 高度 -3..3
  const twist = u * Math.PI * 3.0;                  // 扭转 3π
  const radius = 0.55 + 0.22 * Math.sin(u * Math.PI * 2.0);   // 起伏
  for (let j = 0; j < VS; j++) {
    const v = j / VS * Math.PI * 2.0;
    // 截面是一个被压扁的圆（宽 1.6 倍），再绕 Y 扭转
    const px = Math.cos(v) * radius * 1.6;
    const pz = Math.sin(v) * radius;
    const cx = px * Math.cos(twist) - pz * Math.sin(twist);
    const cz = px * Math.sin(twist) + pz * Math.cos(twist);
    verts.push([cx, y, cz]);
    // 法线：截面中心的径向（近似，够用）
    const nl = Math.hypot(px, pz) || 1;
    norms.push([cx / nl * 0.8, 0.15, cz / nl * 0.8]);
  }
}
// 顶层不能用 `return`（QuickJS 按脚本求值）——把结果作为最后一个表达式
addVertices(verts, norms) + 0;
)js"};
  // ── 执行计时 ──
  const auto start = Clock::now();
  const auto result = engine.eval(script, "procedural.js");
  const double elapsed = millis_since(start);
  if (!result.has_value()) {
    st::log::err("脚本失败：{}\n", result.error().message);
    return 1;
  }
  const std::size_t vertex_count = collected.positions.size() / 3U;
  st::log::out("[proc] 脚本执行 + 通道传输：{:.1f} ms，接收 {} 个顶点，{} 次调用\n",
               elapsed, vertex_count, collected.calls);
  st::log::out("[proc] 每顶点开销 {:.1f} us（据此推算 10 万顶点约 {:.0f} ms）\n",
               vertex_count > 0 ? elapsed * 1000.0 / static_cast<double>(vertex_count) : 0.0,
               vertex_count > 0 ? elapsed * 1000.0 / static_cast<double>(vertex_count) * 100000.0 /
                                      1000.0
                               : 0.0);

  // ── 用收到的顶点造 Mesh：按 (i,j) 网格连三角形 ──
  st::raster::Mesh mesh{};
  constexpr int kU = 90;
  constexpr int kV = 24;
  for (std::size_t i = 0; i < vertex_count; ++i) {
    const Vec3 position{collected.positions[i * 3U], collected.positions[i * 3U + 1U],
                        collected.positions[i * 3U + 2U]};
    const Vec3 normal = i * 3U + 2U < collected.normals.size()
                            ? Vec3{collected.normals[i * 3U], collected.normals[i * 3U + 1U],
                                   collected.normals[i * 3U + 2U]}
                            : Vec3{0.0f, 1.0f, 0.0f};
    st::raster::push_vertex(mesh.vertices, position, normal.normalized(),
                            Vec3{0.82f, 0.66f, 0.42f});
  }
  for (int i = 0; i < kU; ++i) {
    for (int j = 0; j < kV; ++j) {
      const auto at = [&](int row, int column) {
        return static_cast<std::uint32_t>(row * kV + (column % kV));
      };
      const std::uint32_t a = at(i, j);
      const std::uint32_t b = at(i + 1, j);
      const std::uint32_t c = at(i + 1, j + 1);
      const std::uint32_t d = at(i, j + 1);
      mesh.indices.insert(mesh.indices.end(), {a, b, c, a, c, d});
    }
  }
  st::log::out("[proc] 网格：{} 顶点 · {} 三角形\n", mesh.vertex_count(),
               mesh.indices.size() / 3U);

  // ── 渲染 ──
  constexpr int kWidth = 360;
  constexpr int kHeight = 360;
  auto scene = st::raster::Scene3D::create(kWidth, kHeight);
  if (!scene.has_value()) {
    st::log::err("无法创建场景\n");
    return 1;
  }
  st::raster::Camera camera{};
  // 拉远到能看全这条 6 米高的扭转带（初版 3.4 米处只能看到局部）
  camera.eye = Vec3{6.2f, 1.4f, 6.2f};
  camera.target = Vec3{0.0f, 0.0f, 0.0f};
  camera.up = Vec3{0.0f, 1.0f, 0.0f};
  camera.fov_y_degrees = 42.0f;

  st::raster::Lighting lighting{};
  lighting.key_direction = Vec3{0.4f, 0.85f, 0.5f};
  lighting.ambient = 0.3f;
  lighting.diffuse = 0.85f;

  st::raster::Material material{};
  material.metallic = 0.35f;
  material.roughness = 0.42f;

  (*scene)->begin_frame(st::math::Color{16, 18, 24, 255});
  (*scene)->set_camera(camera);
  (*scene)->set_lighting(lighting);
  (*scene)->set_cull_mode(st::raster::CullMode::None);
  (*scene)->set_material(material);
  (*scene)->draw_mesh(mesh, st::math::Mat4::identity());

  st::raster::Canvas canvas(kWidth, kHeight);
  (*scene)->end_frame(canvas, st::math::Rect{0.0f, 0.0f, static_cast<float>(kWidth),
                                             static_cast<float>(kHeight)});
  (*scene)->flush();

  // 存 PNG
  st::codec::PngImage image{};
  image.width = kWidth;
  image.height = kHeight;
  image.rgba.resize(static_cast<std::size_t>(kWidth) * static_cast<std::size_t>(kHeight) * 4U);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const auto pixel = canvas.pixel_at(x, y);
      const float alpha = static_cast<float>(pixel.a) / 255.0f;
      const auto unpremultiply = [alpha](std::uint8_t value) {
        if (alpha <= 0.0f) return static_cast<std::uint8_t>(0);
        const float scaled = static_cast<float>(value) / alpha;
        return static_cast<std::uint8_t>(std::lround(std::min(scaled, 255.0f)));
      };
      const std::size_t at =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(kWidth) +
           static_cast<std::size_t>(x)) * 4U;
      image.rgba[at + 0] = unpremultiply(pixel.r);
      image.rgba[at + 1] = unpremultiply(pixel.g);
      image.rgba[at + 2] = unpremultiply(pixel.b);
      image.rgba[at + 3] = pixel.a;
    }
  }
  const std::string png_path = st::fs::join(out_dir, "procedural-ribbon.png");
  const auto written = st::codec::png_write_file(png_path, image);

  // 判据：有几何、有像素、耗时可接受
  int inked = 0;
  const auto background = canvas.pixel_at(0, 0);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const auto pixel = canvas.pixel_at(x, y);
      const int dr = static_cast<int>(pixel.r) - static_cast<int>(background.r);
      const int dg = static_cast<int>(pixel.g) - static_cast<int>(background.g);
      const int db = static_cast<int>(pixel.b) - static_cast<int>(background.b);
      if (dr * dr + dg * dg + db * db > 400) ++inked;
    }
  }
  const bool geometry_ok = vertex_count > 2000 && mesh.indices.size() / 3U > 4000;
  const bool rendered = inked > 4000;
  // 通道开销判据：10 万顶点推算耗时 < 500ms（出图场景可接受）
  const double per_vertex_us =
      vertex_count > 0 ? elapsed * 1000.0 / static_cast<double>(vertex_count) : 1.0e9;
  const bool fast_enough = per_vertex_us * 100000.0 / 1000.0 < 500.0;

  // ── 二进制通道对照测量 ──
  //
  // 同一几何用 Float32Array 传一遍。这是"过程化能不能每帧重算"的唯一判据。
  const std::string binary_script = std::string{R"js(
// 换一套变量名：同一引擎里 `const` 不能重复声明（QuickJS 与浏览器一致）
const BUS = 900, BVS = 24;   // 放大 10 倍：21840 顶点，观察通道占比是否随规模变化
const bcount = (BUS + 1) * BVS;
const positions = new Float32Array(bcount * 3);
const normals = new Float32Array(bcount * 3);
let idx = 0;
for (let a = 0; a <= BUS; a++) {
  const u = a / BUS;
  const y = (u - 0.5) * 6.0;
  const twist = u * Math.PI * 3.0;
  const radius = 0.55 + 0.22 * Math.sin(u * Math.PI * 2.0);
  for (let b = 0; b < BVS; b++) {
    const v = b / BVS * Math.PI * 2.0;
    const px = Math.cos(v) * radius * 1.6;
    const pz = Math.sin(v) * radius;
    const cx = px * Math.cos(twist) - pz * Math.sin(twist);
    const cz = px * Math.sin(twist) + pz * Math.cos(twist);
    positions[idx * 3] = cx;
    positions[idx * 3 + 1] = y;
    positions[idx * 3 + 2] = cz;
    const nl = Math.hypot(px, pz) || 1;
    normals[idx * 3] = cx / nl * 0.8;
    normals[idx * 3 + 1] = 0.15;
    normals[idx * 3 + 2] = cz / nl * 0.8;
    idx++;
  }
}
addGeometry(positions, normals);
)js"};

  // ## 先测"纯计算"基线：同样循环，但**不调宿主函数**
  //
  // 这一步是为了把总耗时拆成"JS 算"与"通道传"两部分。
  // 不拆的话，看到 2.3x 提速会误判成"通道没优化好"，
  // 而真相可能是"瓶颈已经转移到 JS 自己的循环"（那说明通道已经不再是瓶颈）。
  const std::string compute_only_script = std::string{R"js(
const CUS = 90, CVS = 24;
const ccount = (CUS + 1) * CVS;
const cpos = new Float32Array(ccount * 3);
let cidx = 0;
for (let a = 0; a <= CUS; a++) {
  const u = a / CUS;
  const y = (u - 0.5) * 6.0;
  const twist = u * Math.PI * 3.0;
  const radius = 0.55 + 0.22 * Math.sin(u * Math.PI * 2.0);
  for (let b = 0; b < CVS; b++) {
    const v = b / CVS * Math.PI * 2.0;
    const px = Math.cos(v) * radius * 1.6;
    const pz = Math.sin(v) * radius;
    cpos[cidx * 3] = px * Math.cos(twist) - pz * Math.sin(twist);
    cpos[cidx * 3 + 1] = y;
    cpos[cidx * 3 + 2] = px * Math.sin(twist) + pz * Math.cos(twist);
    cidx++;
  }
}
ccount;
)js"};
  const auto compute_start = Clock::now();
  const auto compute_result = engine.eval(compute_only_script, "compute-only.js");
  const double compute_elapsed = millis_since(compute_start);
  if (!compute_result.has_value()) {
    st::log::err("纯计算脚本失败：{}\n", compute_result.error().message);
  }

  const auto binary_start = Clock::now();
  const auto binary_result = engine.eval(binary_script, "procedural-binary.js");
  const double binary_elapsed = millis_since(binary_start);
  double binary_per_vertex = 1.0e9;
  double speedup = 0.0;
  if (!binary_result.has_value()) {
    st::log::err("二进制脚本失败：{}\n", binary_result.error().message);
  } else {
    const std::size_t binary_vertices = binary_positions.size() / 3U;
    binary_per_vertex =
        binary_vertices > 0 ? binary_elapsed * 1000.0 / static_cast<double>(binary_vertices) : 0.0;
    const double json_per_vertex =
        vertex_count > 0 ? elapsed * 1000.0 / static_cast<double>(vertex_count) : 1.0;
    speedup = binary_per_vertex > 0.0 ? json_per_vertex / binary_per_vertex : 0.0;
    st::log::out("[proc] 二进制通道：{:.1f} ms，{} 个顶点，每顶点 {:.3f} us\n", binary_elapsed,
                 binary_vertices, binary_per_vertex);
    st::log::out("[proc] 通道提速 {:.1f}x（Json {:.2f} → 二进制 {:.3f} us/顶点）\n", speedup,
                 json_per_vertex, binary_per_vertex);
    st::log::out("[proc] 10 万顶点：Json {:.0f} ms → 二进制 {:.1f} ms\n", json_per_vertex * 100.0,
                 binary_per_vertex * 100.0);
    // ## 拆分：纯计算 vs 通道
    //
    // 若"纯计算"已接近"二进制通道"，说明通道不再是瓶颈——
    // 继续优化通道毫无意义，该优化的是 JS 循环本身（或改用 C++ 侧生成）。
    const double compute_per_vertex =
        vertex_count > 0 ? compute_elapsed * 1000.0 / static_cast<double>(vertex_count) : 0.0;
    st::log::out("[proc] 拆解：纯 JS 计算 {:.3f} us/顶点，通道 {:.3f} us/顶点（占 {:.0f}%）\n",
                 compute_per_vertex, binary_per_vertex - compute_per_vertex,
                 binary_per_vertex > 0.0 ? (binary_per_vertex - compute_per_vertex) /
                                               binary_per_vertex * 100.0
                                         : 0.0);
  }

  st::log::out("\n判据：几何生成={} 渲染出图={}（{} 像素）通道够快={}（10万顶点推算 {:.0f} ms）\n",
               geometry_ok ? "ok" : "FAIL", rendered ? "ok" : "FAIL", inked,
               fast_enough ? "ok" : "FAIL", per_vertex_us * 100.0);
  if (written) st::log::out("产物：{}\n", png_path);
  return (geometry_ok && rendered) ? 0 : 2;
}

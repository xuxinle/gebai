/// AgX LUT 实现（见 `agx_lut.hpp` 的设计说明）。
#include "st/raster/agx_lut.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>

#include "st/core/log.hpp"

namespace st::raster::agx {
namespace {

// ── 1. 从 Rec.709 到 E-Gamut 的矩阵 ──────────────────────────────────────
//
// 数值由 `tools/agx_chain_dump.py` 让 **OCIO 自己**算出来（不是手抄
// `config.ocio` 的间接定义，那样要乘三次矩阵、错一位就差几级）：
//
//     config.getProcessor('Linear Rec.709 (sRGB)', 'Linear FilmLight E-Gamut')
//
// 那一脚本的实测输出与 Blender 渲染真值逐字节一致（58/118/170/197/217
// 以及红绿蓝的 219,57,33 / 110,197,81 / 20,86,209），所以这个矩阵可信。
constexpr float kRec709ToEGamut[3][3] = {
    {0.5593711138f, 0.3047833443f, 0.1358455569f},
    {0.0762207061f, 0.7879717946f, 0.1358074695f},
    {0.0655267090f, 0.1645467579f, 0.7699264884f},
};

// ── 2. lg2 分配区间 ────────────────────────────────────────────────────
//
// OCIO `AllocationTransform {allocation: lg2, vars: [-12.47393, 12.5260688117]}`
// 对应 25 档动态范围（2^-12.47 ≈ 0.00018 到 2^12.53 ≈ 5900）。
//
// 语义：把 [2^lo, 2^hi] **对数映射**到 [0,1]。低于下限钳制到 0，
// 高于上限钳制到 1（OCIO 本身也是这么做的）。
constexpr float kLogMin = -12.47393f;
constexpr float kLogMax = 12.5260688117f;

/// 把线性值映射到 lg2 分配后的 [0,1]。
auto lg2_allocate(float x) noexcept -> float {
  const float lo = std::exp2(kLogMin);
  const float clamped = x > lo ? x : lo;
  const float t = (std::log2(clamped) - kLogMin) / (kLogMax - kLogMin);
  return std::clamp(t, 0.0f, 1.0f);
}

// ── 3. LUT 容器 ────────────────────────────────────────────────────────

struct LutTable {
  int size{0};
  /// RGB 三元组，索引顺序与 `.cube` 文件一致：
  ///   index = ((r * size + g) * size + b) * 3
  /// （`.cube` 规范：**红最外层、蓝最内层**，且数据按 b 递增排列）
  std::vector<float> data{};
};

/// 单例（只读、惰性构造）。
///
/// 为何不用可变的跨 TU 全局：本仓禁止。`static const` 在函数内构造是
/// 线程安全且只执行一次的（C++11 magic statics），符合「无可变全局」的立场。
struct LutState {
  std::optional<LutTable> table{};
  std::string reason{};
  bool probed{false};
};

auto state() -> LutState& {
  // lint-allow: L8 AgX LUT 是进程级不可变查表数据（只增不减），与日志 sink / 测试注册表同类：
  // 调用方是整个进程的每一个渲染调用，没有 Context 可注入。2.2MB 只解析一次。
  static LutState s;
  return s;
}

/// 按候选路径列表尝试加载。
auto try_paths(const std::vector<std::string>& candidates) -> std::optional<LutTable>;

/// `.cube` 解析。
///
/// ## 只实现本仓需要的子集（并明确报出超出部分）
///
/// | 项 | 支持 |
/// |---|---|
/// | `TITLE` / `#` 注释 | 跳过 |
/// | `DOMAIN_MIN` / `DOMAIN_MAX` | 读入并**校验**必须为 0/1（Blender 的这两份都是） |
/// | `LUT_3D_SIZE` | 必须（只支持 3D LUT） |
/// | `LUT_1D_SIZE` | **不支持**，报错 |
/// | 数据行 | `r g b` 三个 float |
///
/// 不做通用 `.cube` 解析器：通用支持需要处理 1D/3D 组合、任意 domain、
/// 以及 `LUT_3D_INPUT_RANGE` 等扩展。本仓只对 Blender 这一个文件负责，
/// 但要能**明确报错**而不是悄悄读错。
auto parse_cube(std::istream& in, const std::string& path) -> std::optional<LutTable> {
  std::string line;
  int size = 0;
  bool have_size = false;
  std::vector<float> values;
  bool domain_ok = true;

  while (std::getline(in, line)) {
    // 去掉行尾 \r（Windows 换行）
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    if (line[0] == '#') continue;
    if (line.rfind("TITLE", 0) == 0) continue;

    if (line.rfind("LUT_3D_SIZE", 0) == 0) {
      size = std::atoi(line.c_str() + 11);
      have_size = true;
      continue;
    }
    if (line.rfind("LUT_1D_SIZE", 0) == 0) {
      st::log::err("[agx] {} 是 1D LUT，不支持（只支持 LUT_3D_SIZE）\n", path);
      return std::nullopt;
    }
    if (line.rfind("DOMAIN_MIN", 0) == 0 || line.rfind("DOMAIN_MAX", 0) == 0) {
      float a = 0.0f;
      float b = 0.0f;
      float c = 0.0f;
      const bool is_max = line.rfind("DOMAIN_MAX", 0) == 0;
      const char* p = line.c_str() + 10;
      if (std::sscanf(p, "%f %f %f", &a, &b, &c) == 3) {
        const float expect = is_max ? 1.0f : 0.0f;
        if (std::fabs(a - expect) > 1e-6f || std::fabs(b - expect) > 1e-6f ||
            std::fabs(c - expect) > 1e-6f) {
          domain_ok = false;
        }
      }
      continue;
    }
    // 数据行：三个 float
    float a = 0.0f;
    float b = 0.0f;
    float c = 0.0f;
    if (std::sscanf(line.c_str(), "%f %f %f", &a, &b, &c) == 3) {
      values.push_back(a);
      values.push_back(b);
      values.push_back(c);
    }
  }

  if (!have_size || size <= 0) {
    st::log::err("[agx] {} 缺少 LUT_3D_SIZE\n", path);
    return std::nullopt;
  }
  if (!domain_ok) {
    // 本实现假定 domain 是 [0,1]；不满足就明确拒绝（而不是读出错位的值）
    st::log::err("[agx] {} 的 DOMAIN 不是 [0,1]，本实现不支持\n", path);
    return std::nullopt;
  }
  const std::size_t expect = static_cast<std::size_t>(size) * size * size * 3U;
  if (values.size() != expect) {
    st::log::err("[agx] {} 数据量不符：期望 {} 个 float，实得 {}\n", path, expect,
                 values.size());
    return std::nullopt;
  }
  LutTable t{};
  t.size = size;
  t.data = std::move(values);
  return t;
}

auto try_paths(const std::vector<std::string>& candidates) -> std::optional<LutTable> {
  for (const auto& path : candidates) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) continue;
    std::ifstream in(path, std::ios::binary);
    if (!in) continue;
    if (auto t = parse_cube(in, path)) {
      st::log::info("[agx] 已加载 LUT {}（{}³）\n", path, t->size);
      return t;
    }
  }
  return std::nullopt;
}

/// 生成候选路径（不写死版本号：扫 Blender 安装目录下的各版本）。
auto candidate_paths() -> std::vector<std::string> {
  std::vector<std::string> out;
  if (const char* env = std::getenv("ST_AGX_LUT")) {
    if (*env != '\0') out.emplace_back(env);
  }
  out.emplace_back("resources/AgX_Base_sRGB.cube");

  const char* roots[] = {
      "C:/Program Files/Blender Foundation",
      "/usr/share/blender",
      "/Applications/Blender.app/Contents/Resources",
  };
  for (const char* root : roots) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) continue;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
      if (ec) break;
      if (!entry.is_directory()) continue;
      // 两个常见布局：
      //    win: <root>/Blender 5.2/5.2/datafiles/colormanagement/luts/
      //    linux: <root>/<ver>/datafiles/colormanagement/luts/
      const auto base = entry.path();
      const auto luts = base / "datafiles" / "colormanagement" / "luts";
      out.push_back((luts / "AgX_Base_sRGB.cube").string());
      // win 布局多一层版本号
      std::error_code ec2;
      if (std::filesystem::is_directory(base, ec2)) {
        for (const auto& sub : std::filesystem::directory_iterator(base, ec2)) {
          if (ec2) break;
          if (!sub.is_directory()) continue;
          out.push_back(
              (sub.path() / "datafiles" / "colormanagement" / "luts" / "AgX_Base_sRGB.cube")
                  .string());
        }
      }
    }
  }
  return out;
}

/// 确保探测已执行（只执行一次）。
auto ensure_probed() noexcept -> LutState& {
  LutState& s = state();
  if (s.probed) return s;
  // lint-allow: L8 惰性探测的单次锁（与上面 `state()` 同一生命周期）。
  static std::mutex m;
  const std::lock_guard<std::mutex> lock(m);
  if (s.probed) return s;
  auto table = try_paths(candidate_paths());
  if (table.has_value()) {
    s.table = std::move(table);
  } else {
    s.reason =
        "找不到 AgX_Base_sRGB.cube（试过 ST_AGX_LUT、resources/、以及 Blender 安装目录）";
  }
  s.probed = true;
  return s;
}

/// 取 LUT 格子值（内部用，调用方保证 i 在范围内）。
///
/// ## 索引次序：**r 变化最快**（`.cube` 规范）
///
/// 实测踩过：第一版写成 `(r*N+g)*N+b`（b 最快），中性灰看不出差异
/// （三坐标相等，两种索引等价），**纯色完全错**：
///
/// | 样本 | 错索引 | 对索引 | 真值 |
/// |---|---|---|---|
/// | 纯红 (1,0,0) | rgb(0,113,186) 蓝 | rgb(210,100,48) 红 | rgb(219,57,33) |
/// | 纯蓝 (0,0,1) | 偏红 | 偏蓝 | rgb(20,86,209) |
///
/// 这类 bug 的特点是「灰阶全绿、彩色全错」，所以**基线必须包含纯色样本**
/// （只测中性灰会让它一路通过）。
auto cell(const LutTable& t, int r, int g, int b, int ch) noexcept -> float {
  const std::size_t idx =
      (static_cast<std::size_t>(r) +
       static_cast<std::size_t>(g) * static_cast<std::size_t>(t.size) +
       static_cast<std::size_t>(b) * static_cast<std::size_t>(t.size) *
           static_cast<std::size_t>(t.size)) *
          3U +
      static_cast<std::size_t>(ch);
  return t.data[idx];
}

/// tetrahedral 插值（OCIO `FileTransform {interpolation: tetrahedral}`）。
///
/// ## 为什么用 tetrahedral 而不是三线性
///
/// OCIO 的 config 在 AgX 的 `FileTransform` 上**显式**写了
/// `interpolation: tetrahedral`。两者在 57³ 的格子里差别很小，但在
/// **格子对角面**上会差 1~2 级——既然要像素级，就用与 Blender 相同的方式。
///
/// ## 算法
///
/// 把单位立方体按对角面切成 6 个四面体，按 (rx,ry,rz) 的大小关系选一个，
/// 再在那个四面体内做重心插值。标准实现（Kuhn 分解）。
auto sample(const LutTable& t, float r, float g, float b, int ch) noexcept -> float {
  const int n = t.size - 1;
  const float fr = std::clamp(r, 0.0f, 1.0f) * float(n);
  const float fg = std::clamp(g, 0.0f, 1.0f) * float(n);
  const float fb = std::clamp(b, 0.0f, 1.0f) * float(n);
  int i0 = int(fr);
  int j0 = int(fg);
  int k0 = int(fb);
  if (i0 >= n) i0 = n - 1;
  if (j0 >= n) j0 = n - 1;
  if (k0 >= n) k0 = n - 1;
  const float dr = fr - float(i0);
  const float dg = fg - float(j0);
  const float db = fb - float(k0);

  const auto c000 = cell(t, i0, j0, k0, ch);
  const auto c111 = cell(t, i0 + 1, j0 + 1, k0 + 1, ch);

  // 6 种排序，每种用 4 个顶点（tetrahedral）
  if (dr > dg) {
    if (dg > db) {
      // r > g > b
      const auto c100 = cell(t, i0 + 1, j0, k0, ch);
      const auto c110 = cell(t, i0 + 1, j0 + 1, k0, ch);
      return c000 + (c100 - c000) * dr + (c110 - c100) * dg + (c111 - c110) * db;
    }
    if (dr > db) {
      // r > b > g
      const auto c100 = cell(t, i0 + 1, j0, k0, ch);
      const auto c101 = cell(t, i0 + 1, j0, k0 + 1, ch);
      return c000 + (c100 - c000) * dr + (c111 - c101) * dg + (c101 - c100) * db;
    }
    // b > r > g
    const auto c001 = cell(t, i0, j0, k0 + 1, ch);
    const auto c101 = cell(t, i0 + 1, j0, k0 + 1, ch);
    return c000 + (c101 - c001) * dr + (c111 - c101) * dg + (c001 - c000) * db;
  }
  if (db > dg) {
    // b > g > r
    const auto c001 = cell(t, i0, j0, k0 + 1, ch);
    const auto c011 = cell(t, i0, j0 + 1, k0 + 1, ch);
    return c000 + (c111 - c011) * dr + (c011 - c001) * dg + (c001 - c000) * db;
  }
  if (db > dr) {
    // g > b > r
    const auto c010 = cell(t, i0, j0 + 1, k0, ch);
    const auto c011 = cell(t, i0, j0 + 1, k0 + 1, ch);
    return c000 + (c011 - c010) * dr + (c010 - c000) * dg + (c111 - c011) * db;
  }
  // g > r > b
  const auto c010 = cell(t, i0, j0 + 1, k0, ch);
  const auto c110 = cell(t, i0 + 1, j0 + 1, k0, ch);
  return c000 + (c110 - c010) * dr + (c010 - c000) * dg + (c111 - c110) * db;
}

}  // namespace

auto available() noexcept -> bool { return ensure_probed().table.has_value(); }

auto unavailable_reason() -> const char* { return ensure_probed().reason.c_str(); }

auto lut_size() noexcept -> int {
  const auto& s = ensure_probed();
  return s.table.has_value() ? s.table->size : 0;
}

auto invalidate_cache() noexcept -> void {
  LutState& s = state();
  s = LutState{};
}

auto apply(math::Vec3 linear_rec709) noexcept -> math::Vec3 {
  const auto& s = ensure_probed();
  if (!s.table.has_value()) return analytic(linear_rec709);
  const auto& t = *s.table;

  // ① Rec.709 线性 → E-Gamut 线性
  const float er = kRec709ToEGamut[0][0] * linear_rec709.x +
                   kRec709ToEGamut[0][1] * linear_rec709.y +
                   kRec709ToEGamut[0][2] * linear_rec709.z;
  const float eg = kRec709ToEGamut[1][0] * linear_rec709.x +
                   kRec709ToEGamut[1][1] * linear_rec709.y +
                   kRec709ToEGamut[1][2] * linear_rec709.z;
  const float eb = kRec709ToEGamut[2][0] * linear_rec709.x +
                   kRec709ToEGamut[2][1] * linear_rec709.y +
                   kRec709ToEGamut[2][2] * linear_rec709.z;

  // ② lg2 分配到 [0,1]
  const float tr = lg2_allocate(er);
  const float tg = lg2_allocate(eg);
  const float tb = lg2_allocate(eb);

  // ③ LUT（tetrahedral）
  const float lr = sample(t, tr, tg, tb, 0);
  const float lg = sample(t, tr, tg, tb, 1);
  const float lb = sample(t, tr, tg, tb, 2);

  // ④ LUT 输出是 **Rec.1886（伽马 2.4）编码**，要先反线性化再套 sRGB 编码
  //
  // ## 完整推导（实测逐点验证，2026-11-06）
  //
  // `.cube` 头注释：「the end image formation will be from -10 to 6.5
  // encoded in power 2.4」——那份 LUT 的产物是 Rec.1886 编码（显示 EOTF
  // 为伽马 2.4），**不是** sRGB。而 Blender 的 `display_colorspace: sRGB`
  // 要求在 `AgX Base Rec.1886` 之后再转一次到 sRGB。
  //
  // 所以正确链是：
  //
  //     display = sRGB_encode( pow(LUT_out, 2.4) )
  //
  // 拿 0.18 灰对账（真值 0.461320）：
  //   LUT_out = 0.489409
  //   pow(0.489409, 2.4) = 0.179975      ← 反线性化（Rec.1886 的 EOTF）
  //   sRGB_encode(0.179975) = 0.461320   ← 与真值**逐位相同**
  //
  // ## 两个走过的弯路（记在这里）
  //
  // | 写法 | 0.18 得 | 错在哪 |
  // |---|---|---|
  // | `pow(v, 1/2.4)` | 189 | 方向反了 |
  // | 直接 `v` | 125 | 少一步 sRGB 编码 |
  // | **`sRGB_encode(pow(v, 2.4))`** | **118** | 正解 |
  const auto pow24_decode = [](float v) {
    const float c = v > 0.0f ? v : 0.0f;
    return std::pow(c, 2.4f);
  };
  const auto srgb_encode = [](float v) {
    const float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
  };
  return math::Vec3{srgb_encode(pow24_decode(lr)), srgb_encode(pow24_decode(lg)),
                    srgb_encode(pow24_decode(lb))};
}

auto analytic(math::Vec3 linear_rec709) noexcept -> math::Vec3 {
  // 旧的解析拟合（Troy Sobotka 简化版）。**只在 LUT 不可用时使用**。
  //
  // 这里保留原来的数学形式：Rec.2020 色度旋转 → 对数编码 → sigmoid → 去饱和。
  // 它与真值的差距（0.18 灰 +70 级）已记录在头文件里——不是"差不多"，
  // 是"降级后画面不至于变成另一种长相"。
  constexpr float kR2020[3][3] = {
      {0.6274f, 0.3293f, 0.0433f},
      {0.0691f, 0.9195f, 0.0114f},
      {0.0164f, 0.0880f, 0.8956f},
  };
  const float r = kR2020[0][0] * linear_rec709.x + kR2020[0][1] * linear_rec709.y +
                  kR2020[0][2] * linear_rec709.z;
  const float g = kR2020[1][0] * linear_rec709.x + kR2020[1][1] * linear_rec709.y +
                  kR2020[1][2] * linear_rec709.z;
  const float b = kR2020[2][0] * linear_rec709.x + kR2020[2][1] * linear_rec709.y +
                  kR2020[2][2] * linear_rec709.z;
  const auto curve = [](float v) {
    const float log_v = std::log2(std::max(v, 1e-10f));
    const float t = (log_v + 12.47393f) / (12.52606881f + 12.47393f);
    const float x = std::clamp(t, 0.0f, 1.0f);
    return std::pow(x, 1.0f / 2.4f);
  };
  return math::Vec3{curve(r), curve(g), curve(b)};
}

}  // namespace st::raster::agx

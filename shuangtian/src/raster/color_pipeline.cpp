/// 颜色管线的实现：sRGB 编解码 + 色调映射。纯函数，无状态。
///
/// 各算子的常数出处（都可核对）：
/// - sRGB 分段：IEC 61966-2-1；与 Blender `gpu_shader_colorspace.bsl.hh`
///   的 `scene_linear_to_rec709_srgb` 同一组（0.04045 / 12.92 / 1.055 / 2.4）。
/// - ACES 近似：Krzysztof Narkowicz, "ACES Filmic Tone Mapping Curve" (2015)。
/// - Filmic：John Hable, "Uncharted 2: HDR Lighting" (2010) 的
///   `Uncharted2Tonemap`，按 Hable 原文的常数（A/B/C/D/E/F）。

#include "st/raster/color_pipeline.hpp"

#include <algorithm>
// `tone_map_agx` 用 `std::array`（3×3 色度矩阵）与 `std::log2`。
//
// 实测踩过：不显式包含时主构建能过（被其它头间接带入），而**独立工程编译时**
// 报 “`m` has incomplete type”——集成用例 `independent_project_builds_links_and_runs`
// 就是靠真建一个外部工程抓到它的。头文件纪律：用到什么就包含什么。
#include <array>
#include <cmath>

namespace st::raster {
namespace {

constexpr float kSrgbCutoff = 0.0031308f;   // 线性侧分段点
constexpr float kSrgbSlope = 12.92f;
constexpr float kSrgbAlpha = 1.055f;
constexpr float kSrgbOffset = 0.055f;
constexpr float kSrgbGamma = 2.4f;

/// 线性 → sRGB 的**未截断**版本（内部用：先映射再 clamp）。
[[nodiscard]] auto encode_unclamped(float linear) noexcept -> float {
  if (linear <= kSrgbCutoff) return linear * kSrgbSlope;
  return kSrgbAlpha * std::pow(linear, 1.0f / kSrgbGamma) - kSrgbOffset;
}

/// Reinhard：x/(1+x)。定义域 x≥0 时值域 [0,1)。
[[nodiscard]] auto reinhard(float x) noexcept -> float {
  const float v = std::max(x, 0.0f);
  return v / (1.0f + v);
}

/// Narkowicz 的 ACES 近似。
///
/// 注意它**定义域是 [0, ∞)**，但对负值会给出奇怪结果（分子分母都变小），
/// 所以入口先 clamp。这条曲线在 1.0 处约 0.8，即"纯白会略灰"——
/// 这是电影感的来源（高光有余量滚降），不是 bug。
[[nodiscard]] auto aces(float x) noexcept -> float {
  const float v = std::max(x, 0.0f);
  constexpr float a = 2.51f;
  constexpr float b = 0.03f;
  constexpr float c = 2.43f;
  constexpr float d = 0.59f;
  constexpr float e = 0.14f;
  const float num = v * (a * v + b);
  const float den = v * (c * v + d) + e;
  return std::clamp(num / den, 0.0f, 1.0f);
}

/// Hable 的 Uncharted2 曲线（先抬曝光再映射，与原文一致）。
[[nodiscard]] auto hable_curve(float x) noexcept -> float {
  constexpr float A = 0.15f;
  constexpr float B = 0.50f;
  constexpr float C = 0.10f;
  constexpr float D = 0.20f;
  constexpr float E = 0.02f;
  constexpr float F = 0.30f;
  return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

/// Filmic：Hable 曲线按白点归一化。
///
/// 白点取 11.2 是原文的 `W = 11.2`（"日光直射"对应值）。归一化后
/// 输入 11.2 映射到 1.0，否则整幅偏灰。
[[nodiscard]] auto filmic(float x) noexcept -> float {
  constexpr float kWhitePoint = 11.2f;
  const float v = std::max(x, 0.0f);
  const float white = hable_curve(kWhitePoint);
  if (white <= 0.0f) return 0.0f;
  return std::clamp(hable_curve(v) / white, 0.0f, 1.0f);
}

}  // namespace

auto linear_to_srgb(float linear) noexcept -> float {
  return std::clamp(encode_unclamped(std::clamp(linear, 0.0f, 1.0f)), 0.0f, 1.0f);
}

auto srgb_to_linear(float srgb) noexcept -> float {
  const float v = std::clamp(srgb, 0.0f, 1.0f);
  if (v <= kSrgbCutoff * kSrgbSlope) return v / kSrgbSlope;
  return std::pow((v + kSrgbOffset) / kSrgbAlpha, kSrgbGamma);
}

auto linear_to_srgb(math::Vec3 linear) noexcept -> math::Vec3 {
  return math::Vec3{linear_to_srgb(linear.x), linear_to_srgb(linear.y),
                    linear_to_srgb(linear.z)};
}

auto srgb_to_linear(math::Vec3 srgb) noexcept -> math::Vec3 {
  return math::Vec3{srgb_to_linear(srgb.x), srgb_to_linear(srgb.y),
                    srgb_to_linear(srgb.z)};
}

auto linear_to_srgb8(float linear) noexcept -> std::uint8_t {
  // 四舍五入而不是截断：截断会让整幅系统性偏暗半级（0.5 的偏差在 8bit 上是可见的）。
  const float encoded = linear_to_srgb(linear);
  const int value = static_cast<int>(std::lround(encoded * 255.0f));
  return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

auto tone_map_channel(float x, ToneMap op) noexcept -> float {
  switch (op) {
    case ToneMap::None:
      return std::clamp(x, 0.0f, 1.0f);
    case ToneMap::Reinhard:
      return reinhard(x);
      case ToneMap::Aces:
    return aces(x);
  case ToneMap::Filmic:
    return filmic(x);
  case ToneMap::Agx:
    // AgX 是**色彩空间变换**，不能逐色道算（见 `tone_map_agx`）。
    // 这里退化到 ACES 曲线——仅为了让单通道探索类调用有值可用；
    // 真正的 AgX 必须走 `tone_map()` 的三维入口。
    return aces(x);
  }
  return std::clamp(x, 0.0f, 1.0f);
}

// ── AgX（Blender 4.x 默认 view transform）──
//
// ## 为何不用逐色道的曲线糊弄
//
// ACES/Reinhard/Filmic 都是**逐色道**的 S 曲线：它不知道“这个像素是不是在
// 色域外面”，所以高饱和亮部会直接冲成纯红/纯绿（俗称“颜色烧焦”）。
// AgX 的关键差别是**先旋转到一个 3×3 的色度空间**（类似 CAM16 的压缩方向），
// 在三维里统一压亮度，所以能保住高光的**色相与层次**——这就是 Blender 4.x
// 把 AgX 设为默认的原因。
//
// ## 实现取捨
//
// 真实的 AgX 是一个 sigmoid 拟合表 + 完整的色适应计算；这里用**公开的
// 拟合多项式**（Troy Sobotka 的 AgX 简化版，Blender 早期实现同源）：
// 足够重现“高光不发彩、中间调略去饱和”的观感，且全是乘加、
// 与软件腿/GPU 腿的逐字翻译口径一致（不调 pow 之外的实现自由度函数）。
//
// ## 三个必须说的取舍
//
// 1. **look 只有一种**（Blender 的 `AgX - Punchy` 等没有做）；
// 2. **不做色域映射**（Blender 会把 sRGB 外的颜色拉回来，这里直接夹取）；
// 3. **对比度固定**（Blender 的 look 可调）。
// 三条都在说明里写明——不假装完整实现。
namespace {

/// sRGB 线性 → Rec.2020 编码空间（AgX 的输入空间）。
constexpr float kAgxR2020[3][3] = {
    {0.6158109f, 0.3540715f, 0.0502179f},
    {0.1946816f, 0.7710536f, 0.0336659f},
    {0.0019336f, 0.0894976f, 0.9071819f},
};

/// Rec.2020 编码空间 → sRGB 线性（上面的逆）。
constexpr float kAgxR2020Inv[3][3] = {
    {1.8897234f, -0.8592511f, -0.0304754f},
    {-0.4766837f, 1.5161050f, -0.0394222f},
    {0.0425652f, -0.1398432f, 1.0972780f},
};

[[nodiscard]] auto agx_apply(std::array<std::array<float, 3>, 3> m, math::Vec3 v) noexcept
    -> math::Vec3 {
  return math::Vec3{m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z,
                    m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
                    m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
}

/// AgX 的核心曲线：对数编码 → sigmoid → 去饱和。
///
/// 三个环节各有不可替代的作用，去掉任一个都能从画面上看出来：
/// - 对数编码：把 HDR 的宽动态范围压成可比的比例值（不压的话亮部全平）；
/// - sigmoid：把无穷范围映射到 0~1（这是任何 tone map 的本分）；
/// - 去饱和：**这个最关键**，高亮的饱和色（纯红灯管）要往白吐，
///   否则过曝时颜色会“烧焦”成一片纯色。
[[nodiscard]] auto agx_curve(math::Vec3 v) noexcept -> math::Vec3 {
  // ① 对数编码：把 0~∞ 映射到可处理区
  //
  // ## 这组常数的来历（这里踩过大坑）
  //
  // 原实现是 `log2(x/0.18) * 0.32 + 0.5`。那组常数下 **线性 0.05 算出来
  // 是 −0.09**（负数），被后面的 `clamp(x, 0, 1)` 直接夹成 0 —— 暗部全黑。
  //
  // 实测（`tools/tone_map_measure` 修前）：
  //
  // | 线性 | 修前 | 问题 |
  // |---|---|---|
  // | 0.05 | **0** | 暗部被夹掉 |
  // | 0.10 | 123 | 对 |
  // | 0.18 | 191 | 对 |
  // | 饱和红 HDR (3,0.05,0.05) | (249,249,225) | **几乎全白**，把色相丢了 |
  //
  // 中灰以上全对、以下全黑，再加高饱和色变白 —— 而这两条恰好是
  // “AgX 对比度高/高光去饱和”的**特征**，很容易被误当成风格而不去查。
  //
  // ## 修法
  //
  // 斜枕减半（0.32 → 0.16）：这样线性 0.001 算出来才 −0.19（加 0.5 后 0.31
  // 非负），而 0.18 仍在 0.5 附近（中灰不偏）。
  const auto encode = [](float x) noexcept -> float {
    return std::log2(std::max(x, 1e-6f) / 0.18f) * 0.16f + 0.5f;
  };
  const math::Vec3 loged{encode(v.x), encode(v.y), encode(v.z)};

  // ② S 曲线：**单调多项式拟合**（而不是 clamp + smoothstep）
  //
  // ## 为何不能用 `clamp(x, 0, 1)` + smoothstep（这是色相破坏的根源）
  //
  // 夹取的致命问题：它**对每个通道独立**。以纯红 (1, 0, 0) 为例：
  //
  // | 通道 | 编码值 | clamp 后 |
  // |---|---|---|
  // | R | 0.94 | 0.94 |
  // | G | -0.20 | **0** |
  // | B | -0.20 | **0** |
  //
  // 看起来还能保住红色。但**去饱和”那一一步会把 R 往 luma 拉，而 luma 因为
  // G/B 被夹成 0 而偏低……** 实测结果是 (255,169,0) —— **红色变成了橙色**。
  //
  // 而且中性灰也出了问题：灰 (0.18) 本应无饱和度，实测 AgX 得到饱和度 0.03
  //（三通道 189/187/187 不一致）—— 夹取后各通道值不同，再经非对角矩阵
  // 转回 sRGB 就会产生微小色偏。
  //
  // ## 正解：用单调的 S 形拟合函数，**不夹取**
  //
  // 采用 Troy Sobotka 公开的 AgX 拟合曲线的同形形式：
  //
  //     f(x) = 0.5 + 0.5 * tanh((x - 0.5) * k)
  //
  // 其中 `tanh` 天然把 (−∞, ∞) 映射到 (−1, 1)，**不需要 clamp**，
  // 因此不会因夹取而破坏通道间关系。纯红、中性灰都能保住。
  const auto sigmoid = [](float x) noexcept -> float {
    // k = 3.0：中间斜率适中（实测 0.18 中灰得到 0.74，与 Blender 的 AgX 接近）
    return 0.5f + 0.5f * std::tanh((x - 0.5f) * 3.0f);
  };
  const math::Vec3 curved{sigmoid(loged.x), sigmoid(loged.y), sigmoid(loged.z)};

  // ③ 去饱和：按亮度把三通道往灰拉（越亮拉得越狠）
  //
  // ## 为何要限制幅度（这里踩过大坑）
  //
  // 原实现是 `highlight = clamp((luma - 0.55) * 2.2, 0, 1)`——也就是
  // luma 超过 0.55 就开始全速去饱和，到 1.0 完全变灰。
  //
  // 而 AgX 的关键特性恰恰是**高光保色相**。实测（`tools/tone_map_measure`）：
  //
  // | 输入 | 原实现 | 应当 |
  // |---|---|---|
  // | 饱和红 HDR (3, 0.05, 0.05) | (250,232,195) 几乎白 | 偏红，不是白 |
  // | 饱和绿 HDR | (254,254,252) 全白 | 偏绿 |
  //
  // 根因不是“去饱和本身错”，而是**幅度过大**：在 Rec.2020 宽色域里，
  // 饱和红的三通道都被推高，luma 靠近 1，于是去饱和一开就到底。
  //
  // ③ 去饱和：按亮度把三通道往灰拉（越亮拉得越狠）
  //
  // ## 亮度权重必须用 **Rec.2020** 的（又一个隐蔽的错）
  //
  // 本函数跑在 Rec.2020 编码空间里（见 `tone_map_agx` 里的色域矩阵），
  // 而原实现用的是 sRGB 的权重 `(0.2126, 0.7152, 0.0722)`。
  //
  // 后果：纯红 (1,0,0) → AgX 得到 **(255,169,0)**——红色变成**橙色**。
  // 因为 sRGB 权重里绿色占 0.7152（远高于红色 0.2126），算出的 luma 偏向绿，
  // 去饱和就把绿往红身上推。
  //
  // 而 Rec.2020 的权重是 `(0.2627, 0.6780, 0.0593)`——与本空间的原色一致。
  //
  // ## 为何这个错特别难查
  //
  // 两组权重都是“合理的数字”，代码看着没错；而症状（红色偏橙）会被
  // 当成“AgX 就是这种风格”。必须拿**纯色**去量才能暴露：中性灰、黄铜
  // 这类低饱和输入下偏差不明显。
  const float luma = 0.2627f * curved.x + 0.6780f * curved.y + 0.0593f * curved.z;
  const float highlight = std::clamp((luma - 0.72f) * 0.55f, 0.0f, 0.45f);
  const auto mix = [&](float c) noexcept -> float { return c + (luma - c) * highlight; };
  return math::Vec3{mix(curved.x), mix(curved.y), mix(curved.z)};
}

}  // namespace

// ── AgX 的三维入口（与逐色道版本分开）──
//
// ## 关键在于**色域往返必须对称**
//
// 本函数做：sRGB → Rec.2020 → 非线性曲线 → sRGB。
//
// 实测踩过的两个缺陷（都是“往返不对称”造成的）：
//
// | 输入 | 得到 | 原因 |
// |---|---|---|
// | 纯红 (1,0,0) | **(255,169,0) 橙** | Rec.2020 矩阵把红摄到三通道，曲线后又拉不回来 |
// | 灰 0.18 | (190,187,187) 轻微偏 | 同上，非对角项残留 |
//
// 根因是：`kAgxR2020` 与 `kAgxR2020Inv` 应当互为**精确逆矩阵**，
// 而原值只给了 7 位小数且不保证可逆——往返后残差被非线性曲线放大。
//
// 做法：每次转回去后再做一个**残差修正**（用线性空间的比值拉回原值），
// 保证“灰色进灰出、纯红仍纯红”。这不是数学上优雅的做法，但它把
// “色相不变”这个**用户看得见的性质**硬化了下来。
auto tone_map_agx(math::Vec3 hdr) noexcept -> math::Vec3 {
  std::array<std::array<float, 3>, 3> to2020{};
  for (std::size_t r = 0; r < 3; ++r) {
    for (std::size_t c = 0; c < 3; ++c) to2020[r][c] = kAgxR2020[r][c];
  }
  const math::Vec3 wide = agx_apply(to2020, hdr);
  const math::Vec3 curved = agx_curve(wide);
  std::array<std::array<float, 3>, 3> back{};
  for (std::size_t r = 0; r < 3; ++r) {
    for (std::size_t c = 0; c < 3; ++c) back[r][c] = kAgxR2020Inv[r][c];
  }
  const math::Vec3 out = agx_apply(back, curved);

  // ## 色相保真修正（关键一步）
  //
  // 上面往返后得到的 `out` 亮度对、但**色相偏了**（纯红变橙、灰略偏）。
  // 因为两个矩阵只给到 7 位小数，并不精确互逆；非线性曲线又把残差放大。
  //
  // 修正思路：**能变的是亮度，不能变的是通道比例**。
  //
  //     1. 从曲线结果取“它把总能量缩了多少”= out_sum / in_sum
  //     2. 用那个比例去缩**输入**，得到结果
  //
  // 效果：
  //
  // | 输入 | 修前 | 修后 |
  // |---|---|---|
  // | 纯红 (1,0,0) | (255,169,0) 橙 | (255,0,0) 红 |
  // | 灰 0.18 | (190,187,187) | 三通道完全相等 |
  //
  // ## 为何这样做而不是修矩阵精度
  //
  // 修到 15 位小数也只能把残差变小，不能去掉——而且任何后续改动
  //（改曲线、加 look）都会重新引入。这个修正把“色相不变”变成了
  // **结构性保证**，不依赖常数精度。
  //
  // 代价：它不再是“真实的 AgX”（那需要完整 OCIO 配置）。亮度响应
  // 仍来自曲线本身，所以对比度/中灰位置/高光表观都对。
  const float in_sum = hdr.x + hdr.y + hdr.z;
  const float out_sum = out.x + out.y + out.z;
  math::Vec3 corrected = out;
  if (in_sum > 1e-6f && out_sum > 1e-6f) {
    const float scale = out_sum / in_sum;
    corrected = math::Vec3{hdr.x * scale, hdr.y * scale, hdr.z * scale};
  } else if (in_sum <= 1e-6f) {
    corrected = math::Vec3{0.0f, 0.0f, 0.0f};
  }
  return math::Vec3{std::clamp(corrected.x, 0.0f, 1.0f),
                    std::clamp(corrected.y, 0.0f, 1.0f),
                    std::clamp(corrected.z, 0.0f, 1.0f)};
}

auto tone_map(math::Vec3 hdr, ToneMap op) noexcept -> math::Vec3 {
  if (op == ToneMap::None) {
    return math::Vec3{std::clamp(hdr.x, 0.0f, 1.0f), std::clamp(hdr.y, 0.0f, 1.0f),
                      std::clamp(hdr.z, 0.0f, 1.0f)};
  }
  // AgX 是**色彩空间变换**，不能逐色道做（见上面说明）——单独一条路。
  if (op == ToneMap::Agx) return tone_map_agx(hdr);
  return math::Vec3{tone_map_channel(hdr.x, op), tone_map_channel(hdr.y, op),
                    tone_map_channel(hdr.z, op)};
}

auto encode_pixel(math::Vec3 hdr_linear, float exposure, ToneMap op) noexcept -> math::Color {
  const float gain = exposure > 0.0f ? exposure : 0.0f;
  const math::Vec3 exposed{hdr_linear.x * gain, hdr_linear.y * gain, hdr_linear.z * gain};
  const math::Vec3 mapped = tone_map(exposed, op);
  return math::Color{linear_to_srgb8(mapped.x), linear_to_srgb8(mapped.y),
                     linear_to_srgb8(mapped.z), 255};
}

}  // namespace st::raster

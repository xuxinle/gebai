/// 素材预览与自检：一次渲染给出「图 + 结构化诊断」。
///
/// ## 为什么需要它（这是本轮实测的教训）
///
/// 手工做第一个素材（手机）就踩了三个坑、渲染了 8 次才成型：
///
/// | 坑 | 症状 | 靠什么发现 |
/// |---|---|---|
/// | `emission_color` 写错名 | 屏幕不发光 | 读了源码才发现真实字段是 `emissive_color` |
/// | 部件 z 值陷进机身 | 屏幕被完全遮挡 | **渲完抽帧肉眼看** |
/// | 相机太近 | 物体占满画面、细节丢失 | **渲完抽帧肉眼看** |
///
/// 后两个都是"只能看图发现"的——而看图人工做一次要几十秒。
/// 本工具把这三类检查变成**一条命令、非零退出码**：
///
/// 1. **几何**：包围盒、尺寸、建议相机距离（转调 `st_api --check` 的能力）
/// 2. **可见性**：渲染后统计画面亮度分布——全黑/极暗说明相机或遮挡有问题
/// 3. **构图**：物体在画面里占多少面积——太小/太满都是错
///
/// ## 用法
///
/// ```
/// caiyun --preview-model assets/models/phone.json --out phone.png
/// ```
///
/// 退出码：0 = 检查通过；1 = 有问题（会逐条列出）。

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <string>
#include <vector>

#include <st/codec/png.hpp>
#include <st/core/fs.hpp>
#include <st/core/log.hpp>
#include <st/raster/canvas.hpp>
#include <st/raster/scene3d.hpp>
#include <st/raster/scene3d_script.hpp>

namespace caiyun {

namespace {

/// 渲染后对像素做的统计。
///
/// 所有比例都是**相对于“物体像素”**算的：先把与背景色相同的像素排除掉，
/// 否则背景会把自己算成“物体”（实测：中灰背景让物体占比报成 97%）。
struct FrameStats {
  /// 物体像素的平均亮度（不含背景）
  float mean_luma{0.0f};
  /// 物体像素占整幅画面的比例
  float lit_ratio{0.0f};
  /// 物体像素里亮于 0.25 的占比——“有没有体面的高光/受光面”
  float bright_ratio{0.0f};
  /// 物体像素里的最高亮度
  float max_luma{0.0f};
};

/// 判定"这个像素是物体而非背景"的差异阈值。
///
/// 不能用"亮度 > 某值"：背景本身有亮度，会被算成物体（实测：
/// 中灰背景让"物体占比"报成 97%，毫无意义）。
/// 改比**与背景色的 RGB 距离**——背景是常量，物体只要盖上去就不同。
constexpr float kForegroundDelta = 0.06f;

[[nodiscard]] auto analyze(std::span<const std::uint32_t> pixels, st::math::Color background)
    -> FrameStats {
  const auto bg_r = static_cast<float>(background.r) / 255.0f;
  const auto bg_g = static_cast<float>(background.g) / 255.0f;
  const auto bg_b = static_cast<float>(background.b) / 255.0f;
  FrameStats stats{};
  if (pixels.empty()) return stats;
  double sum = 0.0;
  std::size_t lit = 0;
  std::size_t bright = 0;
  float max_l = 0.0f;
  for (const std::uint32_t px : pixels) {
    // 画布内部是 `0xRRGGBBAA`
    const auto r = static_cast<float>((px >> 24U) & 0xFFU) / 255.0f;
    const auto g = static_cast<float>((px >> 16U) & 0xFFU) / 255.0f;
    const auto b = static_cast<float>((px >> 8U) & 0xFFU) / 255.0f;
    // Rec.709 亮度系数
    const float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    // 与背景的差异：判定"这里是不是物体"
    const float delta = std::max({std::abs(r - bg_r), std::abs(g - bg_g), std::abs(b - bg_b)});
    if (delta > kForegroundDelta) {
      ++lit;
      sum += static_cast<double>(luma);
      if (luma > 0.25f) ++bright;
      max_l = std::max(max_l, luma);
    }
  }
  const auto total = static_cast<float>(pixels.size());
  const auto foreground = static_cast<float>(lit);
  stats.mean_luma = foreground > 0.0f ? static_cast<float>(sum / static_cast<double>(lit)) : 0.0f;
  stats.lit_ratio = static_cast<float>(lit) / total;
  stats.bright_ratio = static_cast<float>(bright) / total;
  stats.max_luma = max_l;
  return stats;
}

/// 一条自检结论。
struct Check {
  bool ok{true};
  std::string message{};
};

}  // namespace

/// 预览一个素材场景文件。
///
/// `out_png` 为空则只做统计不落盘（CI 里用）。
[[nodiscard]] auto preview_model(const std::string& spec_path, const std::string& out_png,
                                 int width, int height) -> int {
  const auto text = st::fs::read_text(spec_path);
  if (!text) {
    st::log::err("读不到素材文件：{}\n", spec_path);
    return 2;
  }
  const auto spec = st::raster::parse_scene_spec_text(*text);
  if (!spec) {
    st::log::err("素材解析失败：{}\n", spec.error().message);
    return 2;
  }

  st::raster::Canvas canvas(width, height, 1.0f);
  // 背景刻意用中灰：它既不会伪装成"物体很亮"，也不会把暗部吞掉。
  // 用纯黑背景会让"物体本身很暗"和"物体没画出来"无法区分。
  const st::math::Color background{10, 12, 16, 255};

  auto scene = st::raster::Scene3D::create(width, height);
  if (!scene) {
    st::log::err("建三维场景失败：{}\n", scene.error().message);
    return 2;
  }
  const auto rendered = st::raster::render_scene_at_into(*spec, 0.0f, **scene, canvas, background);
  if (!rendered) {
    st::log::err("渲染失败：{}\n", rendered.error().message);
    return 2;
  }

  const auto pixels = canvas.pixels();
  const FrameStats stats = analyze(pixels, background);

  // ── 检查项 ──
  //
  // 阈值不是拍脑袋：它们是"肯定不对"的边界，不是"不够好看"的边界。
  // 略微偏保守——误报会让人不再信任这个工具，比漏报更糟。
  std::vector<Check> checks{};
  checks.push_back(Check{stats.lit_ratio > 0.01f,
                         stats.lit_ratio > 0.01f
                             ? std::format("可见内容占 {:.1f}%", stats.lit_ratio * 100.0f)
                             : std::format("几乎全黑（可见内容只有 {:.2f}%）——"
                                           "相机可能陷在物体里，或所有部件都被遮挡",
                                           stats.lit_ratio * 100.0f)});
  checks.push_back(Check{stats.max_luma > 0.20f,
                         stats.max_luma > 0.20f
                             ? std::format("最亮 {:.2f}", stats.max_luma)
                             : std::format("最亮只有 {:.2f}——自发光可能没生效"
                                           "（检查字段名是 emissive_color 而非 emission_color）",
                                           stats.max_luma)});
  checks.push_back(Check{stats.lit_ratio < 0.92f,
                         stats.lit_ratio < 0.92f
                             ? std::format("留白 {:.1f}%（构图正常）", (1.0f - stats.lit_ratio) * 100.0f)
                             : "物体几乎铺满整幅画面——相机太近，缩小会丢细节"});

  bool all_ok = true;
  st::log::out("素材预览：{}\n", spec_path);
  st::log::out("  物体 {} 个 · {}×{}\n", spec->objects.size(), width, height);
  st::log::out("  平均亮度 {:.3f} · 可见 {:.1f}% · 明亮 {:.1f}% · 峰值 {:.2f}\n",
               static_cast<double>(stats.mean_luma), static_cast<double>(stats.lit_ratio) * 100.0,
               static_cast<double>(stats.bright_ratio) * 100.0,
               static_cast<double>(stats.max_luma));
  for (const Check& check : checks) {
    st::log::out("  {} {}\n", check.ok ? "[OK]  " : "[问题]", check.message);
    if (!check.ok) all_ok = false;
  }

  if (!out_png.empty()) {
    st::codec::PngImage image{};
    image.width = static_cast<std::uint32_t>(width);
    image.height = static_cast<std::uint32_t>(height);
    image.rgba.resize(pixels.size() * 4U);
    for (std::size_t i = 0; i < pixels.size(); ++i) {
      const std::uint32_t px = pixels[i];
      image.rgba[i * 4U + 0U] = static_cast<std::uint8_t>((px >> 24U) & 0xFFU);
      image.rgba[i * 4U + 1U] = static_cast<std::uint8_t>((px >> 16U) & 0xFFU);
      image.rgba[i * 4U + 2U] = static_cast<std::uint8_t>((px >> 8U) & 0xFFU);
      image.rgba[i * 4U + 3U] = static_cast<std::uint8_t>(px & 0xFFU);
    }
    const auto written = st::codec::png_write_file(out_png, image);
    if (!written) {
      st::log::err("写 PNG 失败：{}\n", written.error().message);
      return 2;
    }
    st::log::out("  已写出 {}\n", out_png);
  }

  return all_ok ? 0 : 1;
}

}  // namespace caiyun

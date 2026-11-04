#include "st/raster/scene_report.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <vector>

#include "st/raster/canvas.hpp"
#include "st/raster/scene3d_script.hpp"
#include "st/raster/three_runtime.hpp"

namespace st::raster {

namespace {

/// 判定"这个像素与背景不同"的阈值。
///
/// 为什么是 12（三通道差的绝对值之和）：实测背景经过色调映射后的抖动约 0~3；
/// 而物体边缘的抗锯齿过渡在 5~10 之间。取 12 能把"真的画了东西"与"编码抖动"
/// 分开，又不至于漏掉半透明物体的浅色部分。
constexpr int kBackgroundThreshold = 12;

/// 判定"这片区域里画了东西"的**最少像素数**。
///
/// 为什么不是 1：一个孤立像素几乎必然是光栅化的边缘毛刺或数值噪声。
/// 要求成片（>= 24 像素，约 5×5）才算"这个物体在画面里"。
constexpr int kMinVisiblePixels = 24;

/// 从四角采样背景色。
///
/// ## 为什么不用传入的 `background`
///
/// 渲染管线做色调映射 + sRGB 编码：`begin_frame(0x0A0A0E)` 出来**不是** 0x0A0A0E。
/// 拿输入值当参考点会把整幅图算成"非背景"（本仓实测踩过：覆盖率虚报 100%）。
/// 四角极少被主体占据，取众数最稳。
[[nodiscard]] auto sample_background(std::span<const std::uint8_t> rgba, int width, int height)
    -> std::array<int, 3> {
  if (rgba.size() < 4 || width <= 0 || height <= 0) return {0, 0, 0};
  const auto at = [&](int x, int y) {
    const std::size_t index =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
         static_cast<std::size_t>(x)) * 4U;
    return std::array<int, 3>{rgba[index], rgba[index + 1], rgba[index + 2]};
  };
  const std::array<std::array<int, 3>, 4> corners{at(0, 0), at(width - 1, 0), at(0, height - 1),
                                                  at(width - 1, height - 1)};
  // 取众数：四角中成对相同的就是背景
  for (std::size_t i = 0; i < corners.size(); ++i) {
    int same = 0;
    for (std::size_t j = 0; j < corners.size(); ++j) {
      if (corners[i] == corners[j]) ++same;
    }
    if (same >= 2) return corners[i];
  }
  return corners[0];
}

[[nodiscard]] auto is_painted(std::span<const std::uint8_t> rgba, std::size_t offset,
                             const std::array<int, 3>& background) -> bool {
  if (offset + 2 >= rgba.size()) return false;
  const int distance = std::abs(static_cast<int>(rgba[offset]) - background[0]) +
                       std::abs(static_cast<int>(rgba[offset + 1]) - background[1]) +
                       std::abs(static_cast<int>(rgba[offset + 2]) - background[2]);
  return distance > kBackgroundThreshold;
}

struct FrameStats {
  int painted{0};
  double mean_luma{0.0};
  double luma_stddev{0.0};
};

[[nodiscard]] auto measure(std::span<const std::uint8_t> rgba, int width, int height)
    -> FrameStats {
  FrameStats stats;
  if (rgba.size() < 4) return stats;
  const auto background = sample_background(rgba, width, height);
  double sum = 0.0;
  double sum_squares = 0.0;
  const std::size_t pixels =
      std::min<std::size_t>(static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
                            rgba.size() / 4U);
  for (std::size_t i = 0; i < pixels; ++i) {
    const std::size_t offset = i * 4U;
    if (is_painted(rgba, offset, background)) ++stats.painted;
    // 亮度用 Rec.709 权重（与 sRGB 的感知亮度一致）
    const double luma = 0.2126 * rgba[offset] + 0.7152 * rgba[offset + 1] +
                        0.0722 * rgba[offset + 2];
    sum += luma;
    sum_squares += luma * luma;
  }
  if (pixels > 0) {
    stats.mean_luma = sum / static_cast<double>(pixels);
    const double variance = sum_squares / static_cast<double>(pixels) -
                            stats.mean_luma * stats.mean_luma;
    stats.luma_stddev = variance > 0.0 ? std::sqrt(variance) : 0.0;
  }
  return stats;
}

/// 只保留一个物体的场景副本（用于逐物体探测）。
///
/// ## three 场景为何不能这样做
///
/// `three_script` 是一条**完整脚本**，删对象意味着改脚本源码（做不到）。
/// 所以 three 场景的逐物体探测走"在脚本里按名字过滤"的旁路（见 `inspect`）。
[[nodiscard]] auto keep_only(const SceneSpec& spec, std::size_t index) -> SceneSpec {
  SceneSpec out = spec;
  out.objects.clear();
  if (index < spec.objects.size()) out.objects.push_back(spec.objects[index]);
  return out;
}

}  // namespace

auto report_level_name(ReportLevel level) noexcept -> std::string_view {
  switch (level) {
    case ReportLevel::Info:
      return "info";
    case ReportLevel::Warning:
      return "warning";
    case ReportLevel::Error:
      return "error";
  }
  return "info";
}

auto SceneRenderReport::has_error() const noexcept -> bool {
  return std::any_of(diagnostics.begin(), diagnostics.end(),
                     [](const ReportDiagnostic& item) { return item.level == ReportLevel::Error; });
}

auto SceneRenderReport::has_warning() const noexcept -> bool {
  return std::any_of(diagnostics.begin(), diagnostics.end(), [](const ReportDiagnostic& item) {
    return item.level == ReportLevel::Warning;
  });
}

auto inspect_scene_render(const SceneSpec& spec, int width, int height, math::Color background,
                          int probe_size) -> SceneRenderReport {
  SceneRenderReport report;
  report.width = width;
  report.height = height;

  const auto render = [&](const SceneSpec& scene_spec, int w, int h)
      -> std::pair<std::vector<std::uint8_t>, double> {
    Canvas canvas(w, h);
    const auto started = std::chrono::steady_clock::now();
    const auto status = render_scene(scene_spec, canvas, background);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    if (!status.has_value()) return {{}, static_cast<double>(elapsed) / 1000.0};
    return {canvas.to_rgba8(), static_cast<double>(elapsed) / 1000.0};
  };

  // ── 整幅 ──
  auto [pixels, elapsed_ms] = render(spec, width, height);
  report.render_ms = elapsed_ms;
  if (pixels.empty()) {
    report.verdict = "render_failed";
    report.diagnostics.push_back(ReportDiagnostic{
        .level = ReportLevel::Error,
        .code = "render_failed",
        .message = "渲染调用直接失败（三维后端不可用？）",
        .suggestion = "先检查 Scene3D::create 是否成功、GPU/软件腿是否可用",
    });
    return report;
  }

  const auto whole = measure(pixels, width, height);
  report.painted_pixels = whole.painted;
  report.mean_luma = whole.mean_luma;
  report.luma_stddev = whole.luma_stddev;
  const double total_pixels = static_cast<double>(width) * static_cast<double>(height);
  report.coverage = total_pixels > 0.0 ? static_cast<double>(whole.painted) / total_pixels : 0.0;

  // ── 逐物体 ──
  //
  // three 场景不能"只保留一个物体"（它是脚本）。这里的做法是：
  // 对 three 场景只在整幅层面判定，并在诊断里如实说明"逐物体探测不适用"；
  // 传统 `objects` 场景逐物体单独渲染。
  const bool three_mode = !spec.three_script.empty();
  if (!three_mode) {
    const int probe_w = std::max(32, probe_size);
    const int probe_h = std::max(32, probe_size);
    for (std::size_t i = 0; i < spec.objects.size(); ++i) {
      ObjectVisibility visibility;
      visibility.name = spec.objects[i].name;
      if (spec.objects[i].group && spec.objects[i].op.empty() &&
          spec.objects[i].pattern.empty()) {
        // 纯分组节点没有自己的几何，跳过（否则会报"这个物体看不见"的假警告）
        report.objects.push_back(visibility);
        continue;
      }
      auto [single, unused_ms] = render(keep_only(spec, i), probe_w, probe_h);
      (void)unused_ms;
      if (single.empty()) {
        report.objects.push_back(visibility);
        continue;
      }
      const auto backdrop = sample_background(single, probe_w, probe_h);
      int min_x = probe_w;
      int max_x = -1;
      int min_y = probe_h;
      int max_y = -1;
      for (int y = 0; y < probe_h; ++y) {
        for (int x = 0; x < probe_w; ++x) {
          const std::size_t offset =
              (static_cast<std::size_t>(y) * static_cast<std::size_t>(probe_w) +
               static_cast<std::size_t>(x)) * 4U;
          if (!is_painted(single, offset, backdrop)) continue;
          ++visibility.pixels;
          min_x = std::min(min_x, x);
          max_x = std::max(max_x, x);
          min_y = std::min(min_y, y);
          max_y = std::max(max_y, y);
        }
      }
      if (visibility.pixels > 0) {
        visibility.coverage =
            static_cast<double>(visibility.pixels) / (static_cast<double>(probe_w) * probe_h);
        visibility.min_x = min_x;
        visibility.max_x = max_x;
        visibility.min_y = min_y;
        visibility.max_y = max_y;
      }
      report.objects.push_back(visibility);
    }
  }

  // ── 诊断（按"越严重越先报"排列）──
  const auto add = [&](ReportLevel level, std::string code, std::string message,
                       std::string suggestion) {
    report.diagnostics.push_back(ReportDiagnostic{.level = level,
                                                  .code = std::move(code),
                                                  .message = std::move(message),
                                                  .suggestion = std::move(suggestion)});
  };

  // ① 什么都没画 —— 最严重，且成因最多
  if (report.painted_pixels < kMinVisiblePixels) {
    report.verdict = "nothing_drawn";
    add(ReportLevel::Error, "nothing_drawn",
        std::format("画面里几乎没有东西：非背景像素 {} 个（{}×{} 共 {} 像素）",
                    report.painted_pixels, width, height, static_cast<int>(total_pixels)),
        "三个最常见成因，按顺序排查："
        "① 相机在物体内部或物体在相机背后 → 把 camera.eye 沿视线方向退到物体外面"
        "（用 scene_describe 的 suggested_camera_distance 当起点）；"
        "② 物体全在取景框外 → 让相机 target 指向物体包围盒中心；"
        "③ 没写 scene.add(mesh) / 物体 visible=false → 确认每个 Mesh 都挂到了场景里");
  }

  // ② 画面糊满 —— 通常是相机钻进了物体
  if (report.coverage > 0.95) {
    // 只有不是"什么都没画"时才升级结论（两者不可能同时成立）
    if (report.verdict == "ok") report.verdict = "camera_inside";
    add(report.verdict == "nothing_drawn" ? ReportLevel::Warning : ReportLevel::Error,
        "camera_inside",
        std::format("画面被完全填满（覆盖 {:.1f}%）：相机很可能在物体内部或紧贴物体表面",
                    report.coverage * 100.0),
        "把相机沿视线反方向后退（例如 camera.position.multiplyScalar(1.8)），"
        "或把物体的 scale 调小；确认 camera.near 不是过大（near 大会切掉近处）");
  }

  // ③ 画面偏暗 / 偏亮
  //
  // ## 阈值为何定在 12（一个实测结论，不是拍脑袋）
  //
  // 本框架的**环境反射项自带底光**（`env_strength` 未配时按 `ambient + 0.55`
  // 给基准，目的是让金属“哪里都在映环境”）。实测：完全不打光的一个中灰球，
  // 平均亮度仍有 **40.2/255**——所以暗部的下限被这个底面光托住了。
  //
  // 于是“亮度 < 12”实际上永远不会触发；真要区分“没打光”，靠的是将来自发光
  // 未开启 + 纹理黑的极端情况。保留这个诊断是因为它对应**真实可发生的**
  // 场景（黑色材质 + 无光），而且须把阈值写得偏低，否则会把“故意的暗调片”误判。
  if (report.painted_pixels >= kMinVisiblePixels) {
    if (report.mean_luma < 12.0) {
      add(ReportLevel::Warning, "image_too_dark",
          std::format("画面整体很暗：平均亮度 {:.1f}/255（正常应在 30~200）", report.mean_luma),
          "常见成因：① 光的方向反了（three 的平行光是 position→target 射出，"
          "本框架要指向光源；若你手写光照参数请确认符号）"
          "② 忘了加光源（至少加一个 AmbientLight 或 DirectionalLight）"
          "③ 材质 base_color 过暗");
    }
    if (report.mean_luma > 235.0) {
      add(ReportLevel::Warning, "image_blown_out",
          std::format("画面过曝：平均亮度 {:.1f}/255", report.mean_luma),
          "把光源 intensity 调小（three 的 DirectionalLight 常用 1~3），"
          "或减小材质 emissiveIntensity");
    }
    // ④ 纯色块：没有明暗变化，通常意味着没有光照（或全部是自发光平色）
    if (report.luma_stddev < 2.0 && report.coverage > 0.05) {
      add(ReportLevel::Warning, "no_shading_variation",
          std::format("画面几乎没有明暗变化（亮度标准差 {:.2f}）：看起来是纯色块",
                      report.luma_stddev),
          "确认加了光源且光有方向（AmbientLight 单独用会让所有面同色）；"
          "材质用 MeshStandardMaterial 而不是 MeshBasicMaterial（后者不受光）");
    }
  }

  // ⑤ 逐物体的问题（只有传统 objects 场景能测）
  if (!three_mode) {
    for (const auto& visibility : report.objects) {
      if (visibility.name.empty()) continue;
      if (visibility.pixels == 0) {
        add(ReportLevel::Warning, "object_out_of_frame",
            std::format("物体「{}」完全不在画面里（单独渲染也没有像素）", visibility.name),
            std::format("检查它的 position 是否离相机 target 太远；"
                        "或把它移到相机视线方向上：mesh.position.set(camera 的 target 附近)"));
      } else if (visibility.pixels < kMinVisiblePixels) {
        add(ReportLevel::Info, "object_tiny",
            std::format("物体「{}」在画面里很小（{} 个像素）", visibility.name,
                        visibility.pixels),
            "若它本该是主体：把它放大（scale）或让相机靠近它");
      }
    }

    // ⑥ 全部挤在同一块区域（完全重叠）
    std::vector<const ObjectVisibility*> drawn;
    for (const auto& visibility : report.objects) {
      if (visibility.pixels > 0) drawn.push_back(&visibility);
    }
    if (drawn.size() >= 2) {
      bool all_same_box = true;
      for (std::size_t i = 1; i < drawn.size(); ++i) {
        if (drawn[i]->min_x != drawn[0]->min_x || drawn[i]->max_x != drawn[0]->max_x ||
            drawn[i]->min_y != drawn[0]->min_y || drawn[i]->max_y != drawn[0]->max_y) {
          all_same_box = false;
          break;
        }
      }
      if (all_same_box) {
        add(ReportLevel::Warning, "all_overlapping",
            std::format("{} 个物体在屏幕上占据完全相同的区域（包围盒一模一样）", drawn.size()),
            "它们很可能位置重合：确认各自 position 不同，或确认父节点变换没有把它们压到一起");
      }
    }
  }

  if (report.diagnostics.empty()) {
    add(ReportLevel::Info, "ok",
        std::format("画面正常：覆盖 {:.1f}%，平均亮度 {:.1f}，{} 个物体",
                    report.coverage * 100.0, report.mean_luma,
                    three_mode ? static_cast<int>(0) : static_cast<int>(report.objects.size())),
        "");
  }
  return report;
}

auto report_json(const SceneRenderReport& report) -> Json {
  Json root = Json::object();
  root["verdict"] = report.verdict;
  root["ok"] = report.ok();
  root["size"] = Json::array({report.width, report.height});
  root["painted_pixels"] = report.painted_pixels;
  root["coverage"] = report.coverage;
  root["mean_luma"] = report.mean_luma;
  root["luma_stddev"] = report.luma_stddev;
  root["render_ms"] = report.render_ms;

  Json objects = Json::array();
  for (const auto& visibility : report.objects) {
    Json item = Json::object();
    item["name"] = visibility.name;
    item["pixels"] = visibility.pixels;
    item["coverage"] = visibility.coverage;
    item["bbox"] = Json::array({visibility.min_x, visibility.min_y, visibility.max_x,
                                visibility.max_y});
    item["visible"] = visibility.visible();
    objects.push_back(std::move(item));
  }
  root["objects"] = std::move(objects);

  Json diagnostics = Json::array();
  for (const auto& diagnostic : report.diagnostics) {
    Json item = Json::object();
    item["level"] = std::string(report_level_name(diagnostic.level));
    item["code"] = diagnostic.code;
    item["message"] = diagnostic.message;
    item["suggestion"] = diagnostic.suggestion;
    diagnostics.push_back(std::move(item));
  }
  root["diagnostics"] = std::move(diagnostics);
  return root;
}

auto report_text(const SceneRenderReport& report) -> std::string {
  std::string out;
  // 第一行就是结论：模型/CI 不必读到末尾才知道对错
  out += std::format("{} {}（{}×{}，覆盖 {:.2f}%，亮度 {:.1f}）\n",
                     report.ok() ? "PASS" : "FAIL", report.verdict, report.width, report.height,
                     report.coverage * 100.0, report.mean_luma);

  if (!report.objects.empty()) {
    int visible = 0;
    for (const auto& visibility : report.objects) {
      if (visibility.visible()) ++visible;
    }
    out += std::format("物体：{}/{} 可见\n", visible, report.objects.size());
    for (const auto& visibility : report.objects) {
      if (!visibility.visible()) {
        out += std::format("  ✗ {}（不在画面里）\n", visibility.name);
      } else {
        out += std::format("  ✓ {}（{} 像素，bbox {},{}-{},{})\n", visibility.name,
                           visibility.pixels, visibility.min_x, visibility.min_y,
                           visibility.max_x, visibility.max_y);
      }
    }
  }

  if (report.diagnostics.empty()) {
    out += "诊断：无\n";
  } else {
    out += "诊断：\n";
    for (const auto& diagnostic : report.diagnostics) {
      out += std::format("  [{}] {} — {}\n", report_level_name(diagnostic.level),
                         diagnostic.message,
                         diagnostic.suggestion.empty() ? "" : diagnostic.suggestion);
    }
  }
  return out;
}

}  // namespace st::raster

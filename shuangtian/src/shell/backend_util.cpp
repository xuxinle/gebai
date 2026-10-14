#include "backend_util.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <vector>

#include "st/core/time.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/gpu.hpp"

namespace st::shell::detail {

/// 合成负载：真实画布尺寸上跑一组有代表性的原语（圆角矩形 / 渐变 / 覆盖率遮罩），
/// 取中位数。**`auto` 的判据本体**：窗口与离屏、四个后端共用同一份，
/// 否则"更快"会得出不同结论（那正是这套选型最不该出现的事）。
[[nodiscard]] auto benchmark_surface_impl(raster::Surface& target, int runs) -> double {
  const float width = static_cast<float>(target.width());
  const float height = static_cast<float>(target.height());
  if (width < 32.0f || height < 32.0f) return 0.0;
  const raster::Paint solid = raster::Paint::solid(math::Color{0x40, 0x80, 0xFF, 0xFF});
  const raster::Paint gradient = raster::Paint::with_gradient(raster::Gradient::linear(
      math::Point{0.0f, 0.0f}, math::Point{width, 0.0f},
      {{0.0f, math::Color{0xFF, 0x00, 0x80, 0xFF}}, {1.0f, math::Color{0x00, 0x80, 0xFF, 0xFF}}}));
  // 小覆盖率位图（模拟字形遮罩：上传 + 采样，这是文字路径的成本特征）
  std::vector<float> mask(16U * 16U, 0.5f);

  std::vector<double> samples;
  samples.reserve(static_cast<std::size_t>(runs));
  for (int run = 0; run < runs; ++run) {
    const std::int64_t start = st::time::now_ns();
    target.clear(math::Color{0x10, 0x10, 0x18, 0xFF});
    for (int index = 0; index < 400; ++index) {
      const float x = static_cast<float>(index % 20) * (width / 20.0f);
      const float y = static_cast<float>(index / 20) * (height / 20.0f);
      target.fill_rect(math::Rect{x + 1.0f, y + 1.0f, width / 25.0f, height / 25.0f}, solid, 4.0f);
    }
    for (int index = 0; index < 40; ++index) {
      target.fill_rect(math::Rect{0.0f, static_cast<float>(index) * (height / 40.0f), width,
                                  height / 40.0f},
                       gradient);
    }
    for (int index = 0; index < 200; ++index) {
      target.blend_coverage_bitmap(index % 20 * 17, index / 20 * 17, mask, 16, 16, solid, 0.8f,
                                   raster::BlendMode::SrcOver);
    }
    samples.push_back(static_cast<double>(st::time::now_ns() - start) / 1'000'000.0);
  }
  std::sort(samples.begin(), samples.end());
  return samples[samples.size() / 2U];  // 中位数：忽略冷启动与偶发抖动
}

/// 建绘制面：按 `renderer` 选择软件或 GPU，失败**如实回退**并记下原因。
///
/// 为什么把选择放在后端而不是应用：面（surface）由后端持有，应用只拿 `Surface&`；
/// 让后端决定它是什么类型，应用与控制通道都不需要知道这一帧是谁画的。
///
/// `auto` 的语义是"**取更快的那条**"，但渲染器的优劣与机器强相关（GPU 弱、驱动差、
/// 或呈现路径仍需 CPU 拷贝时，软件反而更快），所以这里不预设答案，一律实测。
[[nodiscard]] auto create_surface(const WindowOptions& options, std::string_view renderer)
    -> SurfaceChoice {
  SurfaceChoice choice;
  const int logical_width = options.width > 0 ? options.width : 1280;
  const int logical_height = options.height > 0 ? options.height : 720;
  const float scale = options.scale > 0.0f ? options.scale : 1.0f;
  const int physical_width = static_cast<int>(
      std::lround(static_cast<double>(logical_width) * static_cast<double>(scale)));
  const int physical_height = static_cast<int>(
      std::lround(static_cast<double>(logical_height) * static_cast<double>(scale)));

  // `auto` = 实测：两条都建出来，各跑一遍同样的负载，取更快的那条。
  // 不做"GPU 优先"的硬编码——GPU 弱、驱动差、或呈现仍需 CPU 拷贝时，软件反而更快。
  //
  // **测量用缩尺画布**：选型要的是「谁更快」这个序关系，不是绝对帧耗时。
  // 早先按真实尺寸（1920×1200）各跑 3 轮：软件光栅 86ms/轮 → 仅这一步就 261ms，
  // 而它全在**窗口可见之前**（实测启动延迟 680ms 的主体，用户看到的是白底+黑框）。
  // 光栅与合成的耗时对面积基本是线性的，缩小尺寸不改变两条的相对次序，
  // 代价却从几百毫秒降到几毫秒——**启动路径上不该为选型付这种代价**。
  if (renderer == "auto") {
    // 长边夹取到 320：足够盖住固定开销（状态切换/提交/遮罩上传）与面积的转折点，
    // 又不至于把时间花在像素上。保持长宽比，否则测的是另一种负载形状。
    constexpr int kProbeLongEdge = 320;
    const double long_edge = static_cast<double>(std::max(physical_width, physical_height));
    const double shrink = long_edge > static_cast<double>(kProbeLongEdge)
                              ? static_cast<double>(kProbeLongEdge) / long_edge
                              : 1.0;
    const int probe_width = std::max(32, static_cast<int>(std::lround(physical_width * shrink)));
    const int probe_height = std::max(32, static_cast<int>(std::lround(physical_height * shrink)));
    std::unique_ptr<raster::Surface> probe_software =
        std::make_unique<raster::Canvas>(probe_width, probe_height, scale);
    const double software_ms = benchmark_surface_impl(*probe_software, 3);
    std::unique_ptr<raster::Surface> probe_gpu{};
    std::string gpu_label{"不可用"};
    if (raster::gpu::available()) {
      if (auto created = raster::gpu::create_canvas(probe_width, probe_height, scale, {});
          created.has_value()) {
        probe_gpu = std::move(*created);
        const auto info = raster::gpu::probe();
        gpu_label = info.has_value() ? info->adapter : std::string("D3D11");
      }
    }
    const double gpu_ms = probe_gpu != nullptr ? benchmark_surface_impl(*probe_gpu, 3) : 0.0;
    bool pick_gpu = probe_gpu != nullptr && gpu_ms > 0.0 && gpu_ms < software_ms;
    // ⚠ `had_gpu` 要在探测阶段就记下：它是「这台机器上 GPU 能不能用」的事实，
    // 与「最后选了谁」是两件事（早先拿交付指针反推，move 之后必然得到相反的分支，
    // 于是出现"名字说 gpu、理由说 GPU 不可用"的自相矛盾）。
    const bool had_gpu = probe_gpu != nullptr;
    // 测速画布丢弃，交付**真实尺寸**的那块。
    if (pick_gpu) {
      if (auto full = raster::gpu::create_canvas(physical_width, physical_height, scale, {});
          full.has_value()) {
        choice.surface = std::move(*full);
        choice.name = "gpu";
      } else {
        pick_gpu = false;   // 真实尺寸建不出来 → 退软件（不影响已测出的序关系）
      }
    }
    if (!pick_gpu) {
      choice.surface = std::make_unique<raster::Canvas>(
          raster::Canvas::for_logical_size(logical_width, logical_height, scale));
      choice.name = "software";
    }
    choice.note = !had_gpu
                      ? std::format("auto：GPU 不可用，选软件（缩尺实测软件 {:.2f} ms）", software_ms)
                      : std::format("auto 实测（缩尺合成负载 {}x{}，不含呈现）：软件 {:.2f} ms vs GPU {} {:.2f} ms → 选{}",
                                    probe_width, probe_height, software_ms, gpu_label, gpu_ms,
                                    pick_gpu ? "GPU" : "软件");
    return choice;
  }

  if (renderer == "gpu") {
    if (!raster::gpu::available()) {
      const auto probe = raster::gpu::probe();
      choice.note = std::format("GPU 不可用（{}），已回退软件光栅器", probe.error().message);
    } else {
      auto created = raster::gpu::create_canvas(physical_width, physical_height, scale, {});
      if (created.has_value()) {
        choice.surface = std::move(*created);
        choice.name = "gpu";
        const auto info = raster::gpu::probe();
        choice.note = info.has_value()
                          ? std::format("D3D11 · {} · {}", info->adapter, info->feature_level)
                          : std::string("D3D11");
        return choice;
      }
      choice.note = std::format("GPU 画布创建失败（{}），已回退软件光栅器",
                                created.error().message);
    }
  } else if (renderer == "software") {
    choice.note = "显式指定软件光栅器";
  }
  choice.surface = std::make_unique<raster::Canvas>(
      raster::Canvas::for_logical_size(logical_width, logical_height, scale));
  return choice;
}

}  // namespace st::shell::detail

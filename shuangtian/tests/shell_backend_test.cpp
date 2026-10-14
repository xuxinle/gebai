/// 窗口/绘制面尺寸的不变量。
///
/// 起因是一个**真实缺陷**：GPU 呈现器把画布纹理送上屏时，若"画布尺寸"与"窗口客户区"
/// 不一致，DXGI 会**静默缩放**（不报错）——表现是整屏发糊。
/// 两条触发路径都存在过：
/// ① 启动时窗口按"请求的 scale"建、缓冲按"窗口实际 DPI"建（两者可能不同）；
/// ② 运行时改 DPI 只重建了缓冲、没同步窗口尺寸。
///
/// 因此这里钉住**不变量**：`物理尺寸 == 逻辑尺寸 × device_scale`，且改 DPI 时逻辑尺寸不变。
/// 窗口那一半需要真实窗口（在 `--backend=win32` 下才能测），但这条不变量对
/// 离屏后端同样成立——而它正是被破坏的那条。

#include "st/test/test.hpp"

#include <memory>

#include "st/shell/shell.hpp"

namespace {

using st::shell::Backend;
using st::shell::WindowOptions;

[[nodiscard]] auto make_headless() -> std::unique_ptr<Backend> {
  auto backend = st::shell::create_backend("headless");
  return backend.has_value() ? std::move(*backend) : nullptr;
}

}  // namespace

ST_TEST(backend_surface_size_always_matches_logical_times_scale) {
  auto backend = make_headless();
  ST_CHECK(backend != nullptr);
  if (backend == nullptr) return;

  for (const float scale : {1.0f, 1.5f, 2.0f, 1.25f}) {
    for (const auto& [logical_w, logical_h] : {std::pair{1280, 800}, std::pair{640, 480}}) {
      WindowOptions options;
      options.width = logical_w;
      options.height = logical_h;
      options.scale = scale;
      // 本用例只关心尺寸不变量：显式 software，避免 `auto` 每次都跑微基准
      // （那会让这个用例从毫秒级变成 7 秒级——测试的耗时也是要维护的成本）
      options.renderer = "software";
      if (auto status = backend->create_window(options); !status) continue;

      st::raster::Surface& surface = backend->framebuffer();
        const int expected_w = static_cast<int>(std::lround(static_cast<float>(logical_w) * scale));
  const int expected_h = static_cast<int>(std::lround(static_cast<float>(logical_h) * scale));
      // 这条断言就是"不缩放"的前提：物理截图尺寸必须等于渲染尺寸
      ST_CHECK_EQ(surface.physical_width(), expected_w);
      ST_CHECK_EQ(surface.physical_height(), expected_h);
      // 逻辑尺寸必须原样保留（UI 布局用的是它）
      ST_CHECK_EQ(backend->logical_size().width, static_cast<float>(logical_w));
      ST_CHECK_EQ(backend->logical_size().height, static_cast<float>(logical_h));
    }
  }
}

ST_TEST(backend_dpi_change_keeps_logical_size_and_rescales_physical) {
  auto backend = make_headless();
  ST_CHECK(backend != nullptr);
  if (backend == nullptr) return;

  WindowOptions options;
  options.width = 1280;
  options.height = 800;
  options.scale = 1.0f;
  options.renderer = "software";
  if (auto status = backend->create_window(options); !status) return;

  // 逐档改 DPI：**逻辑尺寸不变、物理尺寸按比例变**。
  // 早先的缺陷正是"只改了其中一半"——窗口那侧没动，于是两者的比例关系被破坏。
  for (const float scale : {1.5f, 2.0f, 1.0f, 1.25f}) {
    if (auto status = backend->set_device_scale(scale); !status) {
      ST_CHECK(false);   // 改 DPI 不该失败（除非参数非法，而这里都是合法值）
      continue;
    }
    ST_CHECK_EQ(backend->device_scale(), scale);
    ST_CHECK_EQ(backend->logical_size().width, 1280.0f);
    ST_CHECK_EQ(backend->logical_size().height, 800.0f);
    const int expected_w = static_cast<int>(std::lround(1280.0 * static_cast<double>(scale)));
    const int expected_h = static_cast<int>(std::lround(800.0 * static_cast<double>(scale)));
    ST_CHECK_EQ(backend->framebuffer().physical_width(), expected_w);
    ST_CHECK_EQ(backend->framebuffer().physical_height(), expected_h);
  }
}

ST_TEST(backend_rejects_invalid_scale) {
  auto backend = make_headless();
  if (backend == nullptr) return;
  WindowOptions options;
  options.width = 320;
  options.height = 240;
  if (auto status = backend->create_window(options); !status) return;
  ST_CHECK(!backend->set_device_scale(0.0f).has_value());
  ST_CHECK(!backend->set_device_scale(-1.0f).has_value());
}

ST_TEST(backend_reports_renderer_honestly) {
  // 渲染器必须可查：`auto` 是按实测选的，用户有权知道这一帧谁画的。
  // （窗口后端曾完全忽略 `renderer` 选项——真实桌面下永远走软件。）
  for (const char* requested : {"software", "auto"}) {
    WindowOptions options;
    options.width = 320;
    options.height = 240;
    options.renderer = requested;
    auto backend = st::shell::create_backend("headless");
    if (!backend.has_value()) continue;
    if (auto status = (*backend)->create_window(options); !status) continue;
    const std::string_view name = (*backend)->renderer_name();
    ST_CHECK(!name.empty());
    if (std::string_view(requested) == "software") {
      ST_CHECK_EQ(std::string(name), std::string("software"));
    }
    // 选择理由不该是空的——不知道"为什么选它"就没法排查性能问题
    ST_CHECK(!(*backend)->renderer_note().empty());
  }
}

ST_TEST(headless_defaults_to_software_renderer) {
  // **无头不该为 GPU 探测付代价**（2026-10-14）。
  //
  // 起因（实测）：`auto` 在无头模式下**照样探测 GPU**——建 probe D3D11 设备 +
  // 一块全尺寸 GPU 画布。无头（CI / 截图 / 自动化）根本不需要 GPU，却付出
  // RTX 驱动进程内常驻 ~53MB / ~55 线程 / ~4.6GB 虚拟地址，且全部付在
  // "任何像素产生之前"。
  //
  // 这条用例钉住的是"**缺省路径不得探测**"这个事实：不传 renderer（或传 `auto`）时
  // 必须直接给软件光栅器。反向验证：把 `HeadlessBackend::create_window` 里的
  // requested_renderer 分叉去掉（改回直传 options.renderer），本用例在
  // "缺省不给软件"这一支上变红。
  //
  // ⚠ 只断言"缺省是 software"，**不**假设机器上有没有 GPU：显式 `auto`
  // 在本机（无 GPU）与报告机器（RTX 4080）上都会走探测分支，而那是另一条契约。
  for (const char* requested : {"", "auto"}) {
    WindowOptions options;
    options.width = 320;
    options.height = 240;
    options.renderer = requested;
    auto backend = st::shell::create_backend("headless");
    if (!backend.has_value()) continue;
    if (auto status = (*backend)->create_window(options); !status) continue;
    ST_CHECK_EQ(std::string((*backend)->renderer_name()), std::string("software"));
  }
  // 显式 `gpu` 仍会被尊重（无 GPU 时如实回退软件，但**理由**要说清是回退，
  // 而不是"缺省就是这么定的"）——两条理由不能混为一谈。
  WindowOptions explicit_gpu;
  explicit_gpu.width = 320;
  explicit_gpu.height = 240;
  explicit_gpu.renderer = "gpu";
  if (auto backend = st::shell::create_backend("headless"); backend.has_value()) {
    if (auto status = (*backend)->create_window(explicit_gpu); status) {
      ST_CHECK(!(*backend)->renderer_note().empty());
    }
  }
}

ST_TEST(backend_headless_clipboard_roundtrip) {
  // headless 后端的剪贴板**读写闭环**（2026-10-12 补齐读侧）：
  // `set_clipboard_text` 一直在存 `clipboard_` 成员，但读侧报 Unsupported——
  // 写进去的东西读不出来，「复制→粘贴」永远不通。闭环是无头与有窗口
  // 行为一致的前提（e2e 的复制粘贴断言全靠它）。
  auto backend = make_headless();
  ST_CHECK(backend != nullptr);
  if (backend == nullptr) return;

  // 写 → 读：原样读回（含中文与换行——UTF-8 往返不损坏）。
  ST_CHECK(backend->set_clipboard_text("你好 clipboard\n第二行").has_value());
  const auto read = backend->clipboard_text();
  ST_CHECK(read.has_value());
  if (read.has_value()) {
    ST_CHECK_EQ(*read, std::string("你好 clipboard\n第二行"));
  }
  // 覆写 → 读最新（不残留旧值）。
  ST_CHECK(backend->set_clipboard_text("new").has_value());
  const auto second = backend->clipboard_text();
  ST_CHECK(second.has_value());
  if (second.has_value()) ST_CHECK_EQ(*second, std::string("new"));
}

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

#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

#include "st/shell/shell.hpp"
#include "st/ui/window_control.hpp"

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
  // 起因（实测）：缺省路径在无头模式下**照样探测 GPU**——建 probe D3D11 设备 +
  // 一块全尺寸 GPU 画布。无头（CI / 截图 / 自动化）根本不需要 GPU，却付出
  // RTX 驱动进程内常驻 ~53MB / ~55 线程 / ~4.6GB 虚拟地址，且全部付在
  // "任何像素产生之前"。
  //
  // 本用例钉住的是「**缺省路径不得探测**」这个事实：**不传 renderer**（空串）时
  // 必须直接给软件光栅器。反向验证：把 `HeadlessBackend::create_window` 里的
  // `options.renderer.empty()` 分叉去掉（改回直传），本用例变红。
  //
  // ⚠ **空串与 `auto` 必须分开测**（2026-10-14 修）：
  // 本用例原先写成 `for (const char* requested : {"", "auto"})`，
  // 于是把“显式传了 `--renderer auto`”也按“缺省”对待。那与代码注释、note 文案
  // 三方矛盾（注释/文案都承诺“显式 auto 照旧探测”），而且真出了事故：
  // `--renderer auto` 的无头 GPU 基准**静默跑在软件路径上**（1.44 ms → 11.49 ms），
  // 差点被当成 8.8 倍性能回归。
  //
  // 所以这里拆成两条断言：缺省 → software（不探测）；显式 `auto` → 走探测分支
  // （**不断言探测结果**：无 GPU 的机器与有 RTX 的机器都合法，那是另一条契约）。
  {
    WindowOptions options;
    options.width = 320;
    options.height = 240;
    options.renderer.clear();  // 空串 = 未指定
    if (auto backend = st::shell::create_backend("headless"); backend.has_value()) {
      if (auto status = (*backend)->create_window(options); status) {
        ST_CHECK_EQ(std::string((*backend)->renderer_name()), std::string("software"));
        // 理由要**如实**说是“缺省”，不是“实测选的”（两者混为一谈会让排查的人去翻微基准）。
        ST_CHECK((*backend)->renderer_note().find("缺省") != std::string::npos);
      }
    }
  }
  // 显式 `auto` 必须走**探测**分支（而不是被当成缺省改写成 software）。
  //
  // 不假设探测结果（无 GPU 机器选软件、有 RTX 的选 GPU 都合法）：只断言**理由**不是
  // “缺省”——这是机器无关的，也是旧 bug 的直接体征（旧代码下它会被写成
  // “无头缺省软件光栅器…”）。反向验证：把 backend.cpp 的判据改回
  // `empty() || == "auto"`，本断言立刻变红。
  {
    WindowOptions options;
    options.width = 320;
    options.height = 240;
    options.renderer = "auto";
    if (auto backend = st::shell::create_backend("headless"); backend.has_value()) {
      if (auto status = (*backend)->create_window(options); status) {
        ST_CHECK((*backend)->renderer_note().find("缺省") == std::string::npos);
      }
    }
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

// ————————————————————————————————————————————————————————————————————————————
// 平台后端的**契约面**（2026-10-15，Linux 双后端落地时补）
//
// 为什么这些用例值得写（它们不测像素，只测"会不会撒谎"）：
// 窗口后端的能力是靠**运行时探测**得到的（`dlopen` libX11 / libwayland-client），
// 而"探测到了但用不了"与"压根没探测到"是两件事。若后端在后者上返回一个
// 看似正常的对象，应用会拿到一个**永远不显示任何东西**的窗口——比启动失败难查得多。
// 因此这里钉住三条不变量：
//   ① 工厂在**不支持**时必须如实失败（而不是给个假对象）；
//   ② `name()` 与请求的后端一致（"我明明指定了 x11"不能被静默换成别的）；
//   ③ 无头后端永不声称支持窗口控制（UI 据此如实禁用按钮，而不是画个点不动的装饰）。
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(platform_backends_report_unavailable_honestly) {
  // 在**任意**机器上都成立的断言：不存在"探测到了库、却连窗口都不用建就报成功"这回事。
  // x11/wayland 在无对应显示服务时必须失败；win32 在非 Windows 上必须失败。
  // 反向验证：把 `create_backend` 里的 x11/wayland 分支改回"返回桩对象"，
  // 在无 DISPLAY/WAYLAND_DISPLAY 的环境下本用例变红。
  struct Case {
    const char* backend;
    bool requires_display;
  };
  const Case cases[] = {{"x11", true}, {"wayland", true}, {"win32", false}};
  for (const Case& item : cases) {
    const bool has_x11_display = std::getenv("DISPLAY") != nullptr;
    const bool has_wayland_display = std::getenv("WAYLAND_DISPLAY") != nullptr;
    const bool display_present = std::string_view(item.backend) == "x11" ? has_x11_display
                                 : std::string_view(item.backend) == "wayland" ? has_wayland_display
                                                                              : true;
    auto backend = st::shell::create_backend(item.backend);
    if (item.requires_display && !display_present) {
      // 无显示服务：**必须如实失败**（而不是给一个永远不显示的窗口）
      ST_CHECK(!backend.has_value());
    } else if (backend.has_value()) {
      // 有显示服务（或 win32 in Windows）：名字必须与请求一致——
      // 静默换成别的后端会让"我指定了 x11"变成谜案。
      ST_CHECK_EQ(std::string((*backend)->name()), std::string(item.backend));
    }
#if !defined(_WIN32)
    // 非 Windows 上 win32 后端只能是"如实失败"
    if (std::string_view(item.backend) == "win32") ST_CHECK(!backend.has_value());
#endif
  }
}

ST_TEST(headless_backend_never_claims_window_control) {
  // 无头后端 `supports_window_control()` 必须是 false：UI 据此**如实禁用**窗框按钮
  // （`ui::WindowControl::window_control_available`），而不是画一个点不动的装饰。
  // 反过来，若它报 true 而动作全部 `Unsupported`，用户看到的是"按钮能点但没反应"。
  auto backend = st::shell::create_backend("headless");
  ST_CHECK(backend.has_value());
  if (!backend.has_value()) return;
  WindowOptions options;
  options.width = 320;
  options.height = 240;
  options.renderer = "software";
  if (auto status = (*backend)->create_window(options); !status) return;
  ST_CHECK(!(*backend)->supports_window_control());
  ST_CHECK(!(*backend)->headless() == false);   // 无头就是无头（不得谎报有窗口）
  // 窗口动作一律如实拒绝（不是"静默无效"）
  ST_CHECK(!(*backend)->minimize().has_value());
  ST_CHECK(!(*backend)->toggle_maximize().has_value());
  ST_CHECK(!(*backend)->begin_move().has_value());
  ST_CHECK(!(*backend)->begin_resize(st::ui::WindowEdge::Right).has_value());
  ST_CHECK(!(*backend)->maximized());
}

ST_TEST(unknown_backend_name_is_rejected_not_silently_ignored) {
  // 拼错的后端名必须**报错**："我明明写了 x11 "（带空格）静默落回自动探测，
  // 会让人以为后端工作正常，实际跑的是另一个后端。
  auto backend = st::shell::create_backend("x11 ");
  ST_CHECK(!backend.has_value());
  auto second = st::shell::create_backend("nope");
  ST_CHECK(!second.has_value());
}

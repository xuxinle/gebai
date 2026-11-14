/// 歌白桌面端 · 霜天原生宿主（**替代原 Rust tao/wry 启动器**）。
///
/// ## 它做四件事
///
/// 1. 把**内嵌的 gebai 服务端二进制**物化到用户数据目录（长度一致时再比内容哈希，
///    相同就跳过重写——防"同尺寸不同版本的陈旧侧车"）；
/// 2. 用 `st::process::StreamHandle` 异步 spawn 它（`GEBAI_NO_OPEN=1`，不弹控制台），
///    工作线程逐行读输出，解析出 `[gebai] listening on http://HOST:PORT`；
/// 3. 端口到手后在**主线程**把整窗 `ui::WebView` 导航到 `http://127.0.0.1:{port}`，
///    并在页面就绪后注入桌面形态标记 `window.__GEBAI_DESKTOP__ = 1`（前端据此在下载
///    等场景补 toast——WebView 的下载指示不明显）；
/// 4. 关窗时终止侧车（含它起的子孙进程），不留孤儿。
///
/// ## 为什么读日志必须放在工作线程
///
/// `StreamHandle::read_line` 是**阻塞**的（见 `st/core/process.hpp`），在 GUI 主线程里调
/// 等于把界面冻在"启动中"。所以读端跑在独立线程，端口经 `Tasks::post_ui` 交回主线程——
/// 那是工作线程碰界面的唯一合法路径。
///
/// ## 为什么必须自己画一块占位界面
///
/// 网页的像素**不进 `framebuffer`**（由平台合成器直接上屏），所以服务端起不来时
/// 屏幕上什么都不会有。占位块（`ui::WebView` 自带）与状态行是"启动失败"唯一的出口——
/// 不写它，用户看到的就是一个纯黑窗口，而日志在别处。
///
/// ## 分层责任
///
/// 网页那一半由 `ui::WebView` + `Application::webview_port()` 承担；本文件只管
/// **侧车生命周期**（物化 / 拉起 / 退出回收）与**导航时机**。

#include <cstdint>
#include <cstdio>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "battery/embed.hpp"

#include "st/app/app.hpp"
#include "st/app/cli.hpp"
#include "st/codec/png.hpp"
#include "st/core/entry.hpp"
#include "st/core/fs.hpp"
#include "st/core/hash.hpp"
#include "st/core/json.hpp"
#include "st/core/log.hpp"
#include "st/core/process.hpp"
#include "st/core/time.hpp"
#include "st/ui/components/panel.hpp"
#include "st/ui/components/text.hpp"
#include "st/ui/components/webview.hpp"
#include "st/ui/components/window_frame.hpp"
#include "st/ui/svg.hpp"

namespace {

using st::app::Application;
using st::ui::Panel;
using st::ui::Text;
using st::ui::WebView;

/// 内嵌侧车的**发布形态**：payload 挂在本 exe 文件末尾（打包脚本追加），
/// 运行时由 `read_payload_from_self()` 读回。开发期用 `--server-exe` 直接指一件现成二进制。
///
/// 为何不把侧车用 `b::embed` 编进来：270MB 的字节数组会生成 ~1.1GB C++ 源码，
/// 每次构建全量重编（实测不可接受），而且改动后端就要重编前端；挂尾则两个产物各自独立构建。

/// 应用标识（物化目录名 / 侧车文件名前缀）：构建期可覆盖（场景变体各自独立的数据目录）。
#ifndef GEBAI_DESKTOP_APP_NAME
#define GEBAI_DESKTOP_APP_NAME "gebai"
#endif
/// 窗口标题。
#ifndef GEBAI_DESKTOP_TITLE
#define GEBAI_DESKTOP_TITLE "歌白"
#endif
/// 场景变体的固定端口（空 = 不指定，服务端自己取默认 47896）。
///
/// 为何要固定：localStorage 按 **origin** 隔离，端口一变，用户的环境变量/主题/快捷键
/// 全部"凭空消失"。变体与完整桌面端各占一个端口，两边互不干扰且各自 origin 稳定。
#ifndef GEBAI_DESKTOP_PORT
#define GEBAI_DESKTOP_PORT ""
#endif

constexpr const char* kAppName = GEBAI_DESKTOP_APP_NAME;
constexpr const char* kAppTitle = GEBAI_DESKTOP_TITLE;
constexpr const char* kAppPort = GEBAI_DESKTOP_PORT;

/// 侧车监听日志里的端口标记（与服务端 `boot/serve.ts` 那一行**同一字面量**）。
constexpr std::string_view kPortMarker{"listening on http://"};

/// 桌面形态标记：前端 `packages/web/src/state.ts` 读它决定下载等场景的 toast 提示。
///
/// 框架没有暴露 WebView2 的初始化脚本接口，所以在"页面已就绪"的帧上执行一次——
/// 纯赋值，重复执行无副作用。
constexpr std::string_view kDesktopMarkScript{
    "window.__GEBAI_DESKTOP__=1;"
    "document.documentElement.setAttribute('data-gebai-desktop','1');"};

/// 从一行日志里取端口：`[gebai] listening on http://127.0.0.1:47896 (...)`。
///
/// 只认标记之后的 `host:port`，在空白或 `/` 处收尾——不猜、不跨行。
[[nodiscard]] auto parse_port(const std::string& line) -> std::optional<std::uint16_t> {
  const std::size_t marker = line.find(kPortMarker);
  if (marker == std::string::npos) return std::nullopt;
  const std::size_t begin = marker + kPortMarker.size();
  std::size_t end = begin;
  while (end < line.size() && line[end] != ' ' && line[end] != '\t' && line[end] != '\r' &&
         line[end] != '/') {
    ++end;
  }
  if (end <= begin) return std::nullopt;
  const std::string authority = line.substr(begin, end - begin);
  const std::size_t colon = authority.rfind(':');
  if (colon == std::string::npos || colon + 1 >= authority.size()) return std::nullopt;
  int value = 0;
  for (std::size_t index = colon + 1; index < authority.size(); ++index) {
    const char character = authority[index];
    if (character < '0' || character > '9') return std::nullopt;
    value = value * 10 + (character - '0');
    if (value > 65535) return std::nullopt;
  }
  if (value <= 0) return std::nullopt;
  return static_cast<std::uint16_t>(value);
}

/// 用户数据目录（Windows `%LOCALAPPDATA%\<app>`；其它平台退到 home 下的同名目录）。
///
/// 不放在 exe 同目录：安装位可能只读（Program Files），那儿写侧车会直接失败。
[[nodiscard]] auto resolve_data_dir() -> std::string {
  std::string base = st::fs::read_env("LOCALAPPDATA").value_or(std::string{});
  if (base.empty()) base = st::fs::read_env("APPDATA").value_or(std::string{});
  if (base.empty()) base = st::fs::home_dir();
  return st::fs::join(base, kAppName);
}

/// 尾部 trailer 的长度（写死在两侧）：魔数 8 + payload 长度 8 + 载运体长度 8 + FNV 8。
///
/// 这个结构**必须与打包脚本完全一致**（`packages/desktop/scripts/pack-desktop.ts`）。
/// 用固定长度而不用“扫魔数找末尾”：扫描要读整个 270MB，而固定长度能直接 seek 尾读。
inline constexpr std::size_t kTrailerSize = 32;
/// 魔数（小端 ASCII `GEBAIPAY`）。
inline constexpr std::string_view kTrailerMagic{"GEBAIPAY"};

/// 读本 exe 尾部的侧车 payload（没有则返回 `nullopt`）。
///
/// ## 为何要读「自己」
///
/// 发布形态是**单个 exe**：侧车二进制就挂在自己文件末尾。运行时用
/// `st::process::executable_path()` 找到自己，seek 到尾部读 trailer，再按长度读回 payload
/// ——不需要任何外部文件，也不需要把 270MB 变成 C++ 源码。
///
/// 不做缓存：它只在启动时调一次（`materialize_server`），而“函数内 static 缓存”
/// 会变成进程级共享态（值本身不变，但那类形态一旦开了头就会有人往里塞可变数据）。
[[nodiscard]] auto read_payload_from_self() -> std::optional<std::vector<std::uint8_t>> {
  const auto self = st::process::executable_path();
  if (!self) return std::nullopt;
  const auto size = st::fs::file_size(*self);
  if (!size || *size < kTrailerSize) return std::nullopt;
  const auto trailer = st::fs::read_span(*self, *size - kTrailerSize, kTrailerSize);
  if (!trailer || trailer->size() != kTrailerSize) return std::nullopt;
  const std::span<const std::uint8_t> raw = *trailer;
  // 魔数逐字节比：不把 `uint8_t*` 转成 `char*`（那是一次只为比较的 reinterpret_cast）。
  for (std::size_t index = 0; index < kTrailerMagic.size(); ++index) {
    if (raw[index] != static_cast<std::uint8_t>(kTrailerMagic[index])) {
      return std::nullopt;  // 未打包（开发形态：exe 后面没有 payload）
    }
  }
  const auto read_u64 = [raw](std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
      value |= static_cast<std::uint64_t>(raw[offset + index]) << (index * 8U);
    }
    return value;
  };
  const std::uint64_t payload = read_u64(8);
  const std::uint64_t image = read_u64(16);
  const std::uint64_t expected = read_u64(24);
  // 三道自洽检查：payload 不为空、两者之和等于文件长度、且绝不越过文件头（防被截断的 exe）。
  if (payload == 0 || payload + image + kTrailerSize != *size) {
    st::log::err("歌白桌面端：尾部 payload 结构与文件长度不符（可能被截断/篡改），忽略\n");
    return std::nullopt;
  }
  const auto body = st::fs::read_span(*self, image, static_cast<std::size_t>(payload));
  if (!body || body->size() != payload) return std::nullopt;
  const std::uint64_t actual = st::hash::fnv1a64_bytes(*body);
  if (actual != expected) {
    st::log::err("歌白桌面端：尾部 payload 哈希不符（期望 {:016x}、实得 {:016x}）\n", expected,
                 actual);
    return std::nullopt;
  }
  return std::vector<std::uint8_t>(body->begin(), body->end());
}

/// 物化侧车到数据目录，返回可执行文件路径（空串 = 找不到可用的侧车二进制）。
///
/// ## 两种来源，优先级固定
///
/// 1. **本 exe 尾部挂的 payload**（发布形态）：打包脚本
///    `packages/desktop/scripts/pack-desktop.ts` 把侧车二进制追加到宿主 exe 末尾并写一段
///    固定长度的 trailer（魔数 + 长度 + 哈希）。运行时从**自身**读回来——单文件分发因此保留，
///    而构建期不必把 270MB 变成 C++ 字面量（那样生成 ~1.1GB 源码、每次全量重编，不可接受）。
/// 2. **`--server-exe`**（开发/调试）：直接指一件现成的二进制，不读尾部。
///
/// ## 为何要哈希而不只看长度
///
/// 目标文件可能是**上一版**的（同尺寸不同内容）。只看长度就会把陈旧侧车当成新的，
/// 而症状是“界面起来但功能是老版本”——比直接失败难查得多。
[[nodiscard]] auto materialize_server(const std::string& data_dir) -> std::string {
  const std::string dest = st::fs::join(data_dir, std::format("{}-server.exe", kAppName));
  const auto payload = read_payload_from_self();
  if (!payload.has_value() || payload->empty()) return {};
  const std::span<const std::uint8_t> bytes(*payload);

  bool mismatch = true;
  if (const auto current_size = st::fs::file_size(dest); current_size.has_value()) {
    if (*current_size == static_cast<std::uint64_t>(bytes.size())) {
      if (const auto current = st::fs::read_bytes(dest); current.has_value()) {
        mismatch = st::hash::fnv1a64_bytes(*current) != st::hash::fnv1a64_bytes(bytes);
      }
    }
  }
  if (!mismatch) return dest;
  (void)st::fs::create_directories(data_dir);
  if (const auto written = st::fs::write_bytes(dest, bytes); !written) {
    if (!st::fs::is_regular_file(dest)) {
      st::log::err("歌白桌面端：侧车写盘失败（{}）：{}\n", dest, written.error().message);
      return {};
    }
    st::log::out("歌白桌面端：侧车被占用，回退既有文件 {}\n", dest);
  }
  return dest;
}

/// 32px 原始 RGBA → PNG（交给后端做窗口/任务栏图标）。
///
/// 为何要有它：`AppOptions::window_icon_png` 收的是 **PNG 字节**（各系统 API 都吃它），
/// 而仓库里与 favicon 同源的那份是原始 RGBA（`packages/desktop/icons/icon32.rgba`）。
/// 在宿主里编一次比在外层多造一份资产可靠（多一份资产就多一处会漂）。
///
/// 入参是 `string_view`（`b::embed` 给的就是这个形态）：逐字节 `static_cast` 成
/// `uint8_t`，比转指针干净（也不需要 `reinterpret_cast`）。
[[nodiscard]] auto make_icon_png(std::string_view rgba, std::uint32_t size)
    -> std::vector<std::uint8_t> {
  st::codec::PngImage image;
  image.width = static_cast<int>(size);
  image.height = static_cast<int>(size);
  image.rgba.reserve(rgba.size());
  for (const char value : rgba) image.rgba.push_back(static_cast<std::uint8_t>(value));
  auto encoded = st::codec::png_encode(image);
  if (!encoded) return {};
  return *encoded;
}

/// 侧车运行期状态（工作线程写、主线程读）。
///
/// 为什么用 `shared_ptr` 而不是栈对象：读线程活得比主循环的某些分支长（它要读到
/// EOF 才退出），而主循环退出时会先 `terminate()`——共享所有权让两边都不必猜对方
/// 何时结束。字段本身只由一侧写（`port`/`ready`/`exited` 由 `post_ui` 在主线程写，
/// `log_tail` 由读线程写），因此不需要锁。
struct SidecarState {
  std::string program{};                 ///< 侧车可执行文件（诊断用）
  std::string workdir{};                 ///< 启动时的工作目录
  std::vector<std::string> extra_args{}; ///< 透传参数
  std::uint16_t port{0};                 ///< 解析出的端口（0 = 还没有）
  bool ready{false};                     ///< 已报端口
  bool exited{false};                    ///< 读线程结束（含"从未报端口"）
  std::string log_tail{};                ///< 尾部日志（失败时写进状态行）
};

}  // namespace

auto run_app(int argc, char** argv) -> int {
  // ① 命令行：框架通用开关 + 本应用的两条（开发期替换侧车、给侧车透传参数）。
  static const std::vector<st::app::AppOption> kOptions{
      st::app::takes_value("--server-exe",
                           "覆盖内嵌侧车：直接用这个 gebai 可执行文件（开发/调试用）"),
      st::app::takes_value("--server-arg",
                           "透传给服务端的一条参数（可重复，如 --server-arg=--reload）"),
      st::app::takes_value("--port",
                           "覆盖侧车固定端口（缺省 47896；同机多实例/e2e 用，例如 --port 48901）"),
  };
  st::app::CommonOptions common;
  st::app::AppOptionValues values;
  if (auto parsed = st::app::parse_options(argc, argv, kOptions, common, values); !parsed) {
    st::log::err("gebai_desktop: {}\n", parsed.error().to_string());
    return 2;
  }
  if (common.show_help) {
    st::log::out("{}", st::app::options_usage(kAppName, kOptions));
    return 0;
  }

  st::app::AppOptions options = std::move(common.app);
  options.title = kAppTitle;
  // 桌面端是"整个应用"，不是贴角面板：铺开工作区（框架默认行为，显式写出以免被误改）。
  options.fill_work_area = true;
  if (options.backend.empty() && options.headless) options.backend = "headless";
  // 窗口/任务栏图标：与 exe 资源同一记号（32px 原始 RGBA，编译期嵌入）。
  // 为何不现画：这个宿主的身份就是"歌白本身"，图标应当与 `gebai.exe`、Web UI favicon 一致。
  if (const auto icon = b::embed<"apps/gebai_desktop/assets/gebai-icon32.rgba">();
      icon.length() == 32U * 32U * 4U) {
    options.window_icon_png = make_icon_png(icon.str(), 32U);
  }

  Application app(kAppName, "0.1.0", options);

  // ② 界面：整窗一块 WebView（网页就是界面本身）。占位块保留——服务端起不来时
  // 它是唯一能显示原因的出口（网页像素不进 framebuffer）。
  // 外壳：`WindowFrame`（自绘标题栏 + 内容槽 + 八向缩放边缘），与其他霜天应用同形态。
  //
  // 为什么整窗 WebView 不能直接当根：窗口装饰一律自绘（CONVENTIONS.md §10 第 7 条），
  // 无边框窗口的拖动/缩放/最小化/最大化/关闭**只能**由界面自己提供；而网页像素不进
  // UI 树（平台合成器直接上屏），纯 WebView 根等于整个窗口没有任何窗口控制区——
  // 用户只剩 Alt+F4 一条路。
  auto frame = std::make_unique<st::ui::WindowFrame>(kAppTitle);
  frame->set_id("gebai-root");
  frame->set_window_control(&app);  // 三控制按钮 + 八向边缘条接真实后端（无头如实拒绝）
  // 框架自带的主题按钮关掉：主题由网页自己的 🎨 面板管（窗框色已随网页主题联动），
  // 框架主题按钮在这里既重复又割裂（两套主题状态各切各的）。
  if (frame->title_bar() != nullptr) frame->title_bar()->set_show_theme_button(false);

  // 内容主体（WebView + 状态行）进窗框的**内容槽**：标题栏占顶部一条，其余全是它。
  auto root = std::make_unique<Panel>(st::ui::FlexDirection::Column);
  root->set_id("gebai-content");
  root->style().grow = true;

  auto view = std::make_unique<WebView>();
  view->set_id("gebai-web");
  view->set_placeholder_text("歌白正在启动…");
  view->style().grow = true;
  auto* view_ptr = view.get();
  root->add_child(std::move(view));

  // 状态行：平时隐藏（不占视觉），出问题时把"侧车路径 + 失败原因 + 日志尾巴"写在这里。
  auto status = std::make_unique<Text>("");
  status->set_id("gebai-status");
  status->set_scale(Text::Scale::Xs);
  status->set_multiline(true);
  status->set_visible(false);
  auto* status_ptr = status.get();
  root->add_child(std::move(status));

  frame->content()->add_child(std::move(root));
  app.set_content(std::move(frame));

  // ②.4 窗框随网页主题联动：网页侧 `theme-core.ts` 在主题变化（切主题/换面额/黑白切换）
  // 时经 `chrome.webview.postMessage` 推 `{gebaiDesktop:true,type:"theme",theme,frameColor,light}`。
  // 这里解析后：明暗档切框架主题（标题栏文字/按钮前景自适应对比度），frameColor 染窗框
  //（标题栏条带 + 窗口底色，WebView 盖不满的边缘不再露出框架默认底色）。
  //
  // 为何放在 `on_event`：`WebMessageReceived` 是页面→宿主的唯一推送通路（导航完成后
  // 主动查询拿不到“变化时机”）；回调在 UI 线程派发，直接改组件安全。
  // 侧车未起/页面未导航时消息不会来，窗框保持框架默认——不额外发查询。
  st::ui::WindowFrame* frame_ptr_for_theme =
      dynamic_cast<st::ui::WindowFrame*>(app.root().find("gebai-root"));
  view_ptr->on_event = [frame_ptr_for_theme, &app](const st::ui::WebViewEvent& event) {
    if (event.kind != st::ui::WebViewEvent::Kind::WebMessageReceived) return;
    if (event.message.find("\"gebaiDesktop\"") == std::string::npos) return;  // 快速巷选
    const auto parsed = st::json_parse(event.message);
    if (!parsed) return;
    const st::Json& info = *parsed;
    if (!info.contains("type") || info["type"] != "theme") return;
    // 明暗档：切框架主题，标题栏文字/按钮色随主题体系（底色跟网页、前景跟框架）。
    if (info.contains("light")) {
      app.set_theme_mode(info["light"] == true ? st::ui::ThemeMode::Light : st::ui::ThemeMode::Dark);
    }
    // 窗框色：`#rrggbb` 染标题栏与窗口底色。
    if (info.contains("frameColor") && frame_ptr_for_theme != nullptr) {
      const std::string color = info.value("frameColor", std::string{});
      if (const auto parsed_color = st::ui::svg::parse_color(color)) {
        frame_ptr_for_theme->set_frame_color(parsed_color);
      }
    }
  };

  if (auto started = app.start(); !started) {
    std::fprintf(stderr, "启动失败：%s\n", started.error().message.c_str());
    return 1;
  }

  // ②.5 桌面形态标记：必须在**文档创建时**注入（`add_init_script`）——
  // 前端 `packages/web/src/state.ts` 在模块求值时就读它，导航完成后再注入已经晚了一步。
  //
  // 为何注册在这里而不是建组件时：浏览器宿主在 `app.start()` 里才建（它要窗口句柄），
  // 之前取端口只会拿到空（实测：早一步注册会报“宿主不支持”）。
  const bool init_script_ok = view_ptr->add_init_script(kDesktopMarkScript);

  const auto show_failure = [&](std::string text) {
    st::log::err("歌白桌面端：{}\n", text);
    if (status_ptr != nullptr) {
      status_ptr->set_content(std::move(text));
      status_ptr->set_visible(true);
    }
    if (view_ptr != nullptr) view_ptr->set_placeholder_text("歌白启动失败");
    app.request_repaint();
  };

  // ③ 侧车：优先 `--server-exe`（开发期指 dist/gebai.exe），否则从**本 exe 尾部**读 payload。
  std::string server_exe = values.get("--server-exe");
  const std::string data_dir = resolve_data_dir();
  if (server_exe.empty()) server_exe = materialize_server(data_dir);
  if (!init_script_ok) {
    st::log::err("歌白桌面端：文档初始化脚本注册失败（桌面形态标记将不生效）\n");
  }

  auto state = std::make_shared<SidecarState>();
  state->program = server_exe;
  state->workdir = server_exe.empty() ? data_dir : st::fs::parent(server_exe);
  for (const auto& item : values.values) {
    if (item.first != "--server-arg") continue;
    for (const auto& argument : item.second) state->extra_args.push_back(argument);
  }

  auto sidecar = std::make_shared<st::process::StreamHandle>();
  bool spawned = false;
  if (server_exe.empty()) {
    show_failure(
        "找不到 gebai 服务端二进制：本 exe 尾部没有 payload，且未给 --server-exe。\n"
        "发布形态应由打包脚本产出（bun run --cwd packages/desktop build）；"
        "开发期传 --server-exe 指向一个已存在的 gebai 可执行文件");
  } else {
    // 环境只对**这一个**子进程生效：`GEBAI_NO_OPEN=1` 阻止服务端自己开浏览器
    //（界面已经在我们的窗口里）；`GEBAI_PORT` **总是显式给**——
    //
    // 为何不能“不设就继承”：宿主自己常扮在另一个 gebai 进程的环境里（例如由服务端
    // 会话/CI 拉起），那个环境的 `GEBAI_PORT` 会被子进程原样继承 → 侧车去抢一个
    // 已被占用的端口，启动即以“端口被占用”失败，而界面只看到一句“未报出监听端口”。
    // 写死桌面形态的默认端口（与服务端 `DESKTOP_PORT` 同一值）就消除这个不确定；
    // `--port` 是运行时覆盖（同机多实例/e2e），优先级最高。
    const std::string port_override = values.get("--port");
    const std::string port_value = !port_override.empty()
                                       ? port_override
                                       : (kAppPort[0] != '\0' ? std::string(kAppPort)
                                                              : std::string("47896"));
    std::map<std::string, std::string> child_env{{"GEBAI_NO_OPEN", "1"}};
    child_env.emplace("GEBAI_PORT", port_value);
    sidecar->open(server_exe, state->extra_args, state->workdir, true, child_env);
    if (!sidecar->valid()) {
      show_failure(std::format("拉起侧车失败：{}\n{}", sidecar->error(), server_exe));
    } else {
      spawned = true;
      st::log::out("歌白桌面端：侧车已启动 {}（工作目录 {}）\n", server_exe, state->workdir);
    }
  }

  // ④ 读线程：逐行取日志，端口出现即经 `post_ui` 通知主线程。
  //
  // 线程由 `Tasks::run_async` 管（`Application` 析构时统一 join，不 detach）。
  if (spawned) {
    st::process::StreamHandle* handle = sidecar.get();
    (void)app.tasks().run_async([handle, state, &app](std::stop_token token) {
      std::string line;
      std::string tail;
      while (!token.stop_requested() && handle->read_line(line)) {
        tail.append(line).push_back('\n');
        // 尾部只留最后 ~8KB：完整日志在服务端自己的落盘里，这里只为"为什么没起来"留证据。
        if (tail.size() > 8192) tail.erase(0, tail.size() - 8192);
        if (const auto port = parse_port(line); port.has_value()) {
          const std::uint16_t value = *port;
          app.tasks().post_ui([state, value] {
            state->port = value;
            state->ready = true;
          });
          break;   // 端口已到手：不再逐行搬运（下面继续排空，防管道写满阻塞子进程）
        }
      }
      // 排空余下的输出：子进程常驻时会持续写，不读会让管道写满 → 客户端卡死。
      while (!token.stop_requested() && handle->read_line(line)) {
        tail.append(line).push_back('\n');
        if (tail.size() > 8192) tail.erase(0, tail.size() - 8192);
      }
      app.tasks().post_ui([state, tail] {
        state->log_tail = tail;
        state->exited = true;
      });
    });
  }

  // ⑤ 主循环：把"侧车报了端口"翻译成"导航 WebView"。
  //
  // ## 为什么导航放在帧循环而不是在 `post_ui` 里直接做
  //
  // `WebView::navigate` 首次会**建页面对象**（`ensure_page`），它要求 UI 线程、
  // 且需要当前 bounds 已经排布好——两者只在帧内成立。放进帧循环意味着"条件满足后
  // 下一帧自然发生"，不必在回调里抢时序。
  bool navigated = false;
  bool failure_shown = false;
  bool gave_up = false;
  const std::int64_t started_ms = st::time::now_ms();
  std::uint64_t frames = 0;
  while (!app.quit_requested()) {
    if (!navigated && !gave_up && state->ready && view_ptr != nullptr) {
      // 先问“本机支不支持”，而不是直接 navigate 再看返回值：
      // 不支持的宿主（无头 / 非 Windows / 未装 Runtime）下 navigate 永远不会成功，
      // 而帧循环每帧都会再试一次——实测表现为日志里同一个失败原因刷上千行、
      // 状态行反复重建。**一次判定后就放弃重试**。
      if (!view_ptr->supported()) {
        gave_up = true;
        failure_shown = true;
        show_failure(std::format("嵌入浏览器不可用：{}", view_ptr->status_text()));
      } else {
        const std::string url = std::format("http://127.0.0.1:{}", state->port);
        if (view_ptr->navigate(url)) {
          navigated = true;
          st::log::out("歌白桌面端：已导航 {}\n", url);
        } else {
          gave_up = true;
          failure_shown = true;
          show_failure(std::format("嵌入浏览器不可用：{}", view_ptr->status_text()));
        }
      }
    }
    // 侧车结束且从未报端口 / 60 秒还没就绪：把日志尾巴写成"人话失败原因"。
    if (!navigated && !gave_up && !failure_shown) {
      const bool exited_early = state->exited && !state->ready;
      const bool timed_out = st::time::now_ms() - started_ms > 60'000;
      if (exited_early || timed_out) {
        failure_shown = true;
        show_failure(std::format("服务端{}（侧车：{}）\n工作目录：{}\n{}",
                                 exited_early ? "未报出监听端口就退出了" : "60 秒内未就绪",
                                 state->program, state->workdir, state->log_tail));
      }
    }
    app.tick();
    ++frames;
    // `--frames` / `--ms` 是框架通用开关：无头冒烟/e2e 靠它们自停。
    // 统一入口之后它们对本应用也生效——主循环必须真遵守，否则“传了但不停”
    // 就是谎报退出语义（无头冒烟会永远挂着）。
    if (common.max_frames > 0 && frames >= common.max_frames) break;
    if (common.max_ms > 0 &&
        st::time::now_ms() - started_ms >= static_cast<std::int64_t>(common.max_ms)) {
      break;
    }
    app.pace_loop(st::time::now_ms());
  }

  // ⑥ 退出：先停侧车（`terminate` 会关读端，让阻塞中的 `read_line` 立刻返回 false），
  // 再回收退出码。顺序不能反：先 `finish()` 会一直等一个常驻进程。
  //
  // 两条路都会走到这里：用户关窗（`Application::tick` 把 `close_requested` 转成 `quit`）与
  // `--frames/--ms` 自停。一行日志把“帧数与是否导航过”落盘——出问题时它是分辩
  // “卡在启动”还是“卡在退出”的唯一线索（窗口已消失但进程还在时尤其重要）。
  st::log::out("歌白桌面端：主循环结束（帧 {}，导航 {}）\n", frames, navigated ? "已" : "未");
  if (spawned) {
    sidecar->terminate();
    const int code = sidecar->finish();
    st::log::out("歌白桌面端退出：侧车回收（退出码 {}）\n", code);
  }
  return 0;
}

ST_MAIN(run_app)

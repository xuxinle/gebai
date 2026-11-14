#pragma once

/// 可编程嵌入浏览器：**URL 导航** 与 **资源内嵌** 两种内容源，同一套控制面。
///
/// ## 它解决什么问题
///
/// `NativeHost`（见 `st/ui/components/native_host.hpp`）是"把外部程序的窗口嵌进来"的
/// 逃生门，但它只能**看着**网页：外部浏览器进程的导航、脚本执行、页面消息都在别人的
/// 进程里，宿主拿不到。真正要"用网页做界面的一部分"，需要的是**可编程控制**：
///
/// ```
///   navigate("https://内网系统/看板")        ← URL 内容源
///   load_embedded("index.html", assets)      ← 资源内嵌内容源（整前端打进 exe）
///   execute_script("document.title")  → 回读  ← 宿主 → 页面（有返回值）
///   post_web_message("…")                    ← 宿主 → 页面（事件）
///   on web_message_received                  ← 页面 → 宿主
/// ```
///
/// 本接口就是那条通路的**平台中立**形状：`shell.hpp` 不认识 `HWND`，这里也不认识
/// `ICoreWebView2`。平台实现只出现在 `platform_win32_webview.cpp`（`CONVENTIONS.md`
/// §10 第 1 条）。
///
/// ## 两种内容源，为什么必须并列
///
/// 只做 URL 等于只有半个浏览器——"把整个前端打包进 exe"才是本框架最常用的形态
///（无外部文件依赖、离线可用、双击就跑）。而两种源**共用同一套控制面**：
/// 导航历史、执行脚本、双向消息、缩放对两者一视同仁。
///
/// | 内容源 | 入口 | 能做什么 | 不能做什么 |
/// |---|---|---|---|
/// | URL | `navigate(url)` | 真站点、内网系统、跨域资源 | 依赖网络/外部服务 |
/// | 单文件 HTML | `load_html(html)` | 一段自包含 HTML | **相对路径子资源不可用**（没有 origin） |
/// | **内嵌资源包** | `load_embedded(entry, assets)` | 相对路径 / 同源 `fetch` / ES module / CSS 全部可用 | 需要把资源注册进 `WebAssets` |
///
/// 内嵌资源包之所以能"全部可用"，靠的是**虚拟 origin**：页面地址是一个虚构的
/// `https://<host>/`，其下所有请求都被拦下来，从内存里的「路径 → 字节」表取。
/// 于是页面是一个**真正的 Web 原点**（同源策略、模块解析、缓存语义照常），
/// 而字节全在 exe 里。
///
/// ## 不可用时的行为（**关键契约**）
///
/// 与 `NativeHost` 同一条契约：**任何**环境下都不会让界面崩坏或报错。
///
/// | 环境 | 行为 |
/// |---|---|
/// | 后端不支持（无头 / 非 Windows / 未装 Runtime） | `supported()` 恒 false；一切控制方法返回 `Unsupported`，不假装成功 |
/// | `load_*` 的入口资源不存在 | 如实返回 `NotFound`（**不**落一个空白页让你以为是加载慢） |
/// | 页面执行脚本抛错 | `execute_script` 的回调里带回错误（不是静默空串） |
/// | 嵌入物被切页/滚出可见区 | 自动隐藏（与 `NativeHost` 同一机械：裁到 `bounds ∩ clip`） |
///
/// 这条契约是**结构保证**而不是靠调用方自觉：`WebViewHost` 的每个方法都有默认实现
/// （`Unsupported` / 空操作），所以没接 provider 的宿主天然走降级路径。
///
/// ## `capture` 看不到它（能力表必须写清）
///
/// 与 `NativeHost` 完全相同的口径：嵌入内容的像素**不进 `framebuffer()`**，由平台
/// 合成器直接上屏。所以**任何以 `capture` / `visual_diff` 为判据的回归都看不见网页**。
/// 要确认"页面真的在跑、地址真的变了"，只能读本接口暴露的状态面
/// （`WebViewState`：`url` / `title` / `loading` / `ready` / `can_go_back`…）。
///
/// ## Windows 实测记录（写在这里，免得后来人重踩）
///
/// 1. **DPI 会被重置**：`put_RasterizationScale` 在**导航后**被 WebView2 改回 1.0
///    （实测 1.25 → 1.000）。所以缩放必须在 `RasterizationScaleChanged` 事件里重新下发，
///    否则高 DPI 下网页缩成一半——而属性面读数一切"正常"。
/// 2. **`ExecuteScript` 不解析 Promise**：执行 `async` 函数回来的是 `{}`。异步结果只能
///    由页面经 `post_web_message` 回推（再发一次同步读取也拿不到——两段之间没有时序保证，
///    实测拿到 `"pending"`）。
/// 3. **Loader 只能运行时加载**：上游 `WebView2LoaderStatic.lib` 是 MSVC 产物，
///    MinGW 链不上（`__security_cookie` / `IID_*` 重定义）。这恰好符合 §10 第 1 条。

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"
#include "st/math/geometry.hpp"

namespace st::shell::webview {

/// 内嵌资源**包**里的一条（路径 → 内容）。
///
/// `path` 是**页面内可见的路径**，以 `/` 开头（如 `/index.html`、`/assets/app.js`）。
/// 入口页的路径由 `load_embedded` 的 `entry` 指定。
struct Asset {
  /// 页面内路径（`/` 起）。查询串与锚点会被忽略（`/a.css?v=1` 命中 `/a.css`）。
  std::string path{};
  /// MIME 类型（`text/html` / `text/css` / `application/javascript` / `application/json` /
  /// `image/png`…）。**不能留空**：留空时浏览器会按文本猜，模块脚本会被当成普通文本拒收。
  std::string mime{};
  /// 内容字节。
  std::vector<std::uint8_t> bytes{};
};

/// 内嵌资源表：路径 → 内容。装配期填充，运行期只读。
///
/// 为何用独立类型而不是 `std::map<std::string, Asset>` 直接传：它要带**查询语义**
/// （归一化、忽略查询串、缺失的诊断信息），而这些语义如果散在调用方，两处实现迟早不一致
///（"为什么这个页面在白名单里还是 404"那类问题的根源）。
class WebAssets {
 public:
  /// 加入一条。路径重复时**以最后一次为准**（装配期覆盖比静默保留旧值更可预期）。
  void add(Asset asset);
  /// 便捷形态：文本内容（`mime` 必填，理由见 `Asset::mime`）。
  void add_text(std::string path, std::string mime, std::string_view body);
  /// 便捷形态：从磁盘文件读（读失败返回错误，**不**静默跳过）。
  auto add_file(std::string path, std::string mime, const std::string& disk_path) -> Status;

  /// 按页面内路径查（已归一化：忽略 `?`/`#`，补前导 `/`）。
  [[nodiscard]] auto find(std::string_view path) const -> const Asset*;
  /// 条目数。
  [[nodiscard]] auto size() const noexcept -> std::size_t { return entries_.size(); }
  [[nodiscard]] auto empty() const noexcept -> bool { return entries_.empty(); }
  /// 全部路径（诊断与自检用；顺序稳定）。
  [[nodiscard]] auto paths() const -> std::vector<std::string>;

  /// 路径归一化（**唯一口径**：查表与注册都走它）。
  ///
  /// 规则：丢弃 `?`/`#` 之后；确保以 `/` 开头；空串归为 `/`。
  static auto normalize(std::string_view path) -> std::string;

  /// 按扩展名推 MIME（推不出时返回 `application/octet-stream`）。
  static auto guess_mime(std::string_view path) -> std::string;

 private:
  std::map<std::string, Asset> entries_{};
};

/// 内容源的种类（自检读数用："这页是外网的还是内嵌的"）。
enum class SourceKind : std::uint8_t {
  None,      ///< 还没有内容
  Url,       ///< 外网/内网地址
  Html,      ///< 单文件 HTML（无 origin）
  Embedded,  ///< 内嵌资源包（虚拟 origin）
};

[[nodiscard]] constexpr auto to_string(SourceKind kind) noexcept -> std::string_view {
  switch (kind) {
    case SourceKind::None: return "none";
    case SourceKind::Url: return "url";
    case SourceKind::Html: return "html";
    case SourceKind::Embedded: return "embedded";
  }
  return "none";
}

/// 导航完成的原因（只有 `Completed` 和 `Failed` 值得应用层区分）。
enum class NavigationOutcome : std::uint8_t {
  Completed,
  Failed,
};

/// 页面事件：实现方在平台回调整理成这些，上层（组件/应用）按需订阅。
///
/// ## 为何用「一个事件结构 + kind」而不是每种事件一个回调注册点
///
/// 事件有六七种，各自一个 `add_xxx` / `remove_xxx` 会让"取消订阅"变成六份样板
/// （且极易漏一个就泄漏）。统一成一条队列由调用方按 `kind` 分派，
/// 取消订阅只有一个动作。
struct Event {
  enum class Kind : std::uint8_t {
    NavigationStarting,
    NavigationCompleted,
    TitleChanged,
    WebMessageReceived,  ///< 页面 → 宿主
    ProcessFailed,
    RasterizationScaleChanged,  ///< DPI 变了（**实现方已自行重下发**，这里只是告知）
  };

  Kind kind{Kind::NavigationCompleted};
  /// `NavigationStarting`/`Completed` 的目标地址；其余为空。
  std::string url{};
  /// `TitleChanged` 的标题。
  std::string title{};
  /// `WebMessageReceived` 的消息文本（JSON 或纯文本，由页面决定）。
  std::string message{};
  /// `NavigationCompleted` 的结果。
  NavigationOutcome outcome{NavigationOutcome::Completed};
  /// `ProcessFailed` 的简短原因（已转成人类可读）。
  std::string detail{};
};

/// 页面状态快照（属性面的数据源）。
///
/// ## 为何是"快照"而不是几个 getter
///
/// 这组值只有**合起来读**才有意义："页面到底好了没有"需要
/// `ready && !loading && url 非空` 三个条件一起看。分散成 getter 会让调用方自己拼口径，
/// 而口径一错就会得出"看起来好了其实还白屏"的结论——那正是这类集成最难查的缺陷。
struct State {
  /// 是否已创建出可用的页面对象（Runtime 缺失 / 非 Windows 时为 false）。
  bool attached{false};
  /// 是否已隐藏（被可见区裁空；与 `NativeHost` 同一口径）。
  bool visible{false};
  /// 当前内容源种类。
  SourceKind source{SourceKind::None};
  /// 当前地址（内嵌包时是虚拟 `https://<host>/…`；单文件 HTML 时为空）。
  std::string url{};
  std::string title{};
  /// 是否正在加载（含首帧白屏期）。
  bool loading{false};
  /// **首帧内容是否已就绪**（第一次导航成功完成）。它比 `loading` 更能回答
  /// "现在这块能不能看"——`loading=false` 也可能是"还没开始导航"。
  bool ready{false};
  bool can_go_back{false};
  bool can_go_forward{false};
  /// 当前缩放（1.0 = 100%）。**实现方负责在 DPI 变化后修正它**（见文件头第 1 条）。
  double zoom{1.0};
  /// 页面对象的 DPI 缩放（物理/逻辑）。自检读数：高 DPI 下它若停在 1.0 就是那个已知缺陷。
  double rasterization_scale{1.0};
  /// 内嵌资源包的条目数（其余内容源为 0）。
  std::size_t asset_count{0};
};

/// 可编程嵌入浏览器的平台接口（`Backend` 实现它，`Application` 转发）。
///
/// 所有方法默认**如实降级**：不支持的后端不必覆写任何一条。
class WebViewHost {
 public:
  virtual ~WebViewHost() = default;

  /// 本机此刻能不能做（Runtime 已安装 + 平台已实现 + 窗口已创建）。
  ///
  /// 为何不是编译期常量：WebView2 Runtime 是**独立于本程序安装**的东西——
  /// 同一个 exe 在这台机器能跑、在另一台没装 Runtime 的机器不能。
  [[nodiscard]] virtual auto webview_supported() const noexcept -> bool { return false; }
  /// 不支持时的**人话原因**（"没装 WebView2 Runtime" / "非 Windows" / "窗口未创建"）。
  ///
  /// 为何必须有：`supported()==false` 只说明"不行"，而"到底缺什么"决定了用户
  /// 该装 Runtime 还是该换平台。让调用方自己猜是这类可选能力最常见的折磨。
  [[nodiscard]] virtual auto webview_unsupported_reason() const -> std::string { return {}; }

  /// 创建页面对象，落在客户区的 `bounds`（**逻辑像素**）。
  ///
  /// 创建是**异步**的（WebView2 要先起浏览器进程），所以这里返回 `ok` 只代表"请求已发出"；
  /// 真正的就绪看 `state().attached`。失败原因经 `webview_unsupported_reason()` 可读。
  [[nodiscard]] virtual auto webview_create([[maybe_unused]] math::Rect bounds) -> Status {
    return unexpected(ErrorCode::Unsupported, "该后端不支持嵌入浏览器");
  }
  /// 销毁页面对象（幂等；未创建时为空操作）。
  virtual void webview_destroy() {}

  /// 布局变了：挪到 `bounds`，并且**只显示 `clip` 这一块有交集的部分**。
  ///
  /// 语义与 `Backend::sync_child_view` **逐字一致**（同一个"平台子窗口不吃 UI 裁剪栈"
  /// 的问题）：交集为空 ⇒ 隐藏；否则只露出交集。两者共用同一份几何决策。
  virtual void webview_sync([[maybe_unused]] math::Rect bounds, [[maybe_unused]] math::Rect clip) {}

  // ── 内容源 ──

  /// ① URL：导航到一个真实地址（`http(s)://` / `file://` / `data:` 均可）。
  [[nodiscard]] virtual auto webview_navigate([[maybe_unused]] std::string_view url) -> Status {
    return unexpected(ErrorCode::Unsupported, "该后端不支持嵌入浏览器");
  }
  /// ② 单文件 HTML：把一段自包含 HTML 当页面加载（**没有 origin**，相对子资源不可用）。
  [[nodiscard]] virtual auto webview_load_html([[maybe_unused]] std::string_view html) -> Status {
    return unexpected(ErrorCode::Unsupported, "该后端不支持嵌入浏览器");
  }
  /// ③ 内嵌资源包：以虚拟 origin 提供 `assets`，并导航到其中的 `entry`。
  ///
  /// `entry` 是 `assets` 里的一个路径（如 `/index.html`）；不存在时**如实返回
  /// `NotFound`**，不落空白页。
  ///
  /// 为何把整张表传进来而不是逐条 `add`：虚拟 origin 的过滤器要在导航**之前**装好，
  /// 而"表还会变"是装配期的事——运行期改表会让"这一页用的是哪版资源"无法回答。
  [[nodiscard]] virtual auto webview_load_embedded(
      [[maybe_unused]] std::string_view entry, [[maybe_unused]] const WebAssets& assets)
      -> Status {
    return unexpected(ErrorCode::Unsupported, "该后端不支持嵌入浏览器");
  }

  // ── 导航控制 ──
  [[nodiscard]] virtual auto webview_reload() -> Status {
    return unexpected(ErrorCode::Unsupported, "该后端不支持嵌入浏览器");
  }
  [[nodiscard]] virtual auto webview_stop() -> Status {
    return unexpected(ErrorCode::Unsupported, "该后端不支持嵌入浏览器");
  }
  [[nodiscard]] virtual auto webview_go_back() -> Status {
    return unexpected(ErrorCode::Unsupported, "该后端不支持嵌入浏览器");
  }
  [[nodiscard]] virtual auto webview_go_forward() -> Status {
    return unexpected(ErrorCode::Unsupported, "该后端不支持嵌入浏览器");
  }

  // ── 编程控制（这是本能力存在的理由）──

  /// 注册一段**文档创建时**执行的脚本（每次新文档都跑，在任何页面脚本之前）。
  ///
  /// ## 为何不能用 `execute_script` 代替（实测代价）
  ///
  /// 页面的模块体在**文档解析阶段**求值——前端常在那里读全局标记
  ///（典型：`const isDesktop = !!window.__SOME_HOST__`）。等
  /// `NavigationCompleted` 再 `execute_script` 已经晚了：那个模块体早已跑完，
  /// 读到的永远是 `undefined`。实测症状：注入脚本返回 `window.__X__ === 1` 为真，
  /// 而依赖它的界面行为（下载提示等）完全不生效——两边都是“成功”，只有结果不对。
  ///
  /// 平台语义：WebView2 的 `AddScriptToExecuteOnDocumentCreated`（对**之后**的每个文档生效）。
  /// 必须在首次 `webview_navigate` **之前**注册。
  [[nodiscard]] virtual auto webview_add_init_script([[maybe_unused]] std::string_view script)
      -> Status {
    return unexpected(ErrorCode::Unsupported, "该后端不支持嵌入浏览器");
  }

  /// 执行一段 JS，**同步**回读其求值结果的 JSON 表示。
  ///
  /// ⚠ **不解析 Promise**（见文件头第 2 条）：`async` 函数的返回值是 `{}`。
  /// 异步流程用 `webview_post_message` 让页面回推。
  ///
  /// `result` 的形态：结果是可 JSON 化的值时是 `JSON.stringify` 的产物（字符串、数字、
  /// 对象）；`undefined` 时为空串。**执行出错**时返回 `Parse`（消息里带 JS 异常文本），
  /// 不返回空串——两者必须分得开。
  [[nodiscard]] virtual auto webview_execute_script([[maybe_unused]] std::string_view script,
                                                    [[maybe_unused]] std::string& result) -> Status {
    return unexpected(ErrorCode::Unsupported, "该后端不支持嵌入浏览器");
  }

  /// 宿主 → 页面：发一条字符串消息（页面侧 `chrome.webview` / `window.chrome.webview` 收到）。
  [[nodiscard]] virtual auto webview_post_message([[maybe_unused]] std::string_view text) -> Status {
    return unexpected(ErrorCode::Unsupported, "该后端不支持嵌入浏览器");
  }

  /// 缩放（`1.0` = 100%）。**实现方负责在 DPI 变化后重新下发它**。
  [[nodiscard]] virtual auto webview_set_zoom([[maybe_unused]] double factor) -> Status {
    return unexpected(ErrorCode::Unsupported, "该后端不支持嵌入浏览器");
  }
  /// 是否允许页面跑 JS（关掉是"只渲染"模式，给不可信页面用）。
  [[nodiscard]] virtual auto webview_set_script_enabled([[maybe_unused]] bool enabled) -> Status {
    return unexpected(ErrorCode::Unsupported, "该后端不支持嵌入浏览器");
  }

  // ── 观测 ──

  /// 当前状态快照（属性面的数据源；每次现读，不缓存）。
  [[nodiscard]] virtual auto webview_state() const -> State { return {}; }

  /// 取走本帧累积的事件（取走即清空）。
  ///
  /// 为何是"取走"而不是逐条推送：事件要在 UI 线程按帧消费，回调方（COM）不能直接
  /// 改界面树。攒一帧再交给帧循环，与 `Backend::poll_event` 同一姿态。
  [[nodiscard]] virtual auto webview_take_events() -> std::vector<Event> { return {}; }

  /// 找一个已创建页面的**原生子窗口句柄**（不透明；`nullptr` = 没有）。
  ///
  /// 用途：调试与第三方工具定位；正常路径不需要它。
  [[nodiscard]] virtual auto webview_native_handle() -> void* { return nullptr; }
};

/// 平台工厂：创建 WebView2 宿主（仅 Windows 提供实现；其余平台返回 `nullptr`）。
///
/// ## 为何要单独的工厂而不是塞进 `create_backend`
///
/// 窗口后端（`win32`/`x11`/`wayland`/`headless`）回答的是"怎么开窗"，而浏览器宿主
/// 是**挂在某个窗口上**的附加能力。混进 `create_backend` 会让 X11/Wayland 也必须
/// 提供一份"不支持"的实现，而它们的窗口对象完全不同构（XID vs 无）。
///
/// ## 为何参数是 `void*` + `float` 而不是 `HWND`
///
/// 与本层其他接口同一口径：`webview.hpp` 不认识 `HWND`。`parent` 是**顶层窗口的
/// 原生句柄**（由 `Backend::native_window()` 给出），`device_scale` 是逻辑→物理的
/// 倍数（由 `Backend::device_scale()` 给出）。两者都是上层已经拿到的不透明值。
///
/// 返回 `nullptr` 表示"本平台/本次没有实现"（`parent` 为空、或非 Windows），
/// **不是错误**——调用方据此如实降级。
[[nodiscard]] auto create_win32_webview(void* parent, float device_scale)
    -> std::unique_ptr<WebViewHost>;

}  // namespace st::shell::webview

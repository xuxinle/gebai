#pragma once

/// @file webview_port.hpp
/// @brief `ui` 层的**嵌入浏览器端口**：组件侧唯一的依赖面（依赖方向的落点）。
///
/// ## 为什么需要这个文件（依赖方向）
///
/// 分层的既有口径是 `core ← … ← ui ← app`，而 **`shell` 依赖 `ui`**
///（`shell.hpp` 包含 `ui/element.hpp`）。所以 `ui` 不能包含 `shell/webview.hpp`——
/// 那会让方向反过来成环。
///
/// 但组件（`ui::WebView`）确实需要"导航、执行脚本、读状态"这些能力。
/// 解法与 `Element::ChildViewProbe` 完全同一个先例：**在 `ui` 侧定义窄接口与纯数据结构**，
/// 由 `Application`（在 `app` 层，两边都看得到）做折接。
///
/// ```
///   ui::WebView  ──uses──▶  ui::WebViewPort   ← 本文件（窄接口，无 HWND/无 COM）
///                                ▲
///                                │ implements（折接 shell::webview::* → ui::*）
///                        Application
///                                ▲
///                                │ uses
///                     shell::webview::WebViewHost
/// ```
///
/// ## 为何是"一个接口"而不是十几个 `HostFocus` 虚函数
///
/// 能力有十几条（三种内容源 + 五种导航 + 三种控制 + 状态 + 事件）。平铺到 `HostFocus`
/// 上会让那个接口变成杂物间，而每个 `HostFocus` 实现（含每个测试桩）都得跟着加。
/// 一个 `WebViewPort*` 让"没有浏览器能力"的宿主只需返回 `nullptr`——**零成本降级**。
///
/// ## 不可用时的行为（**关键契约**）
///
/// | 环境 | 行为 |
/// |---|---|
/// | 宿主没提供端口（无头 / 非 Windows / 未装 Runtime） | `host()->host_webview_port()` 返回 `nullptr`；组件的每个动作**如实返回 false / Unsupported**，界面照常 |
/// | 端口在但页面没建起来 | `state().attached == false`；动作返回失败原因，不假装成功 |
/// | 页面加载失败 | `state().ready` 保持 false；`loading` 归 false；事件里有 `Failed` |
///
/// ## `capture` 看不到它
///
/// 与 `NativeHost` 同一条口径：嵌入内容的像素**不进 `framebuffer()`**。
/// 判据只能走 `state()` 与事件流，不能靠像素回归。

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"
#include "st/math/geometry.hpp"

namespace st::ui {

/// 内嵌资源包里的一条（`ui` 侧的镜像结构；与 `shell::webview::Asset` 字段一一对应）。
///
/// 为何两边各留一份：见文件头的依赖方向说明。转换只在 `Application` 一处发生。
struct WebViewAsset {
  /// 页面内路径（以 `/` 开头；查询串与锚点会被忽略）。
  std::string path{};
  /// MIME（不能留空——模块脚本被当文本会直接拒收）。
  std::string mime{};
  std::vector<std::uint8_t> bytes{};
};

/// 内容源种类（自检读数用）。
enum class WebViewSource : std::uint8_t {
  None,
  Url,
  Html,
  Embedded,
};

[[nodiscard]] constexpr auto to_string(WebViewSource source) noexcept -> std::string_view {
  switch (source) {
    case WebViewSource::None: return "none";
    case WebViewSource::Url: return "url";
    case WebViewSource::Html: return "html";
    case WebViewSource::Embedded: return "embedded";
  }
  return "none";
}

/// 页面状态快照（组件属性面的数据源）。
///
/// 为何是快照：这组值只有**合起来读**才有意义（"好了没有" = `ready && !loading && url 非空`）。
/// 分散成 getter 会让调用方自己拼口径，而口径一错就会得出"看起来好了其实白屏"的结论。
struct WebViewState {
  bool attached{false};
  bool visible{false};
  WebViewSource source{WebViewSource::None};
  std::string url{};
  std::string title{};
  bool loading{false};
  /// 首帧内容是否已就绪（第一次导航成功完成）。比 `loading` 更能回答"这块能不能看"。
  bool ready{false};
  bool can_go_back{false};
  bool can_go_forward{false};
  double zoom{1.0};
  /// 页面对象的 DPI 缩放（高 DPI 下它若停在 1.0 就是那个已知缺陷）。
  double rasterization_scale{1.0};
  std::size_t asset_count{0};
};

/// 页面事件（组件与上层按 `kind` 分派）。
struct WebViewEvent {
  enum class Kind : std::uint8_t {
    NavigationStarting,
    NavigationCompleted,
    TitleChanged,
    WebMessageReceived,  ///< 页面 → 宿主
    ProcessFailed,
    RasterizationScaleChanged,
  };

  Kind kind{Kind::NavigationCompleted};
  std::string url{};
  std::string title{};
  std::string message{};
  bool succeeded{true};
  std::string detail{};
};

/// 嵌入浏览器端口（宿主实现它；组件只认它）。
class WebViewPort {
 public:
  virtual ~WebViewPort() = default;

  [[nodiscard]] virtual auto webview_supported() const noexcept -> bool { return false; }
  [[nodiscard]] virtual auto webview_unsupported_reason() const -> std::string { return {}; }

  [[nodiscard]] virtual auto webview_create([[maybe_unused]] math::Rect bounds) -> Status {
    return unexpected(ErrorCode::Unsupported, "该宿主不支持嵌入浏览器");
  }
  virtual void webview_destroy() {}
  virtual void webview_sync([[maybe_unused]] math::Rect bounds, [[maybe_unused]] math::Rect clip) {}

  [[nodiscard]] virtual auto webview_navigate([[maybe_unused]] std::string_view url) -> Status {
    return unexpected(ErrorCode::Unsupported, "该宿主不支持嵌入浏览器");
  }
  [[nodiscard]] virtual auto webview_load_html([[maybe_unused]] std::string_view html) -> Status {
    return unexpected(ErrorCode::Unsupported, "该宿主不支持嵌入浏览器");
  }
  /// 内嵌资源包：入口 + 整张表（表在装配期给全，运行期不改）。
  [[nodiscard]] virtual auto webview_load_embedded(
      [[maybe_unused]] std::string_view entry, [[maybe_unused]] const std::vector<WebViewAsset>& assets)
      -> Status {
    return unexpected(ErrorCode::Unsupported, "该宿主不支持嵌入浏览器");
  }

  [[nodiscard]] virtual auto webview_reload() -> Status {
    return unexpected(ErrorCode::Unsupported, "该宿主不支持嵌入浏览器");
  }
  [[nodiscard]] virtual auto webview_stop() -> Status {
    return unexpected(ErrorCode::Unsupported, "该宿主不支持嵌入浏览器");
  }
  [[nodiscard]] virtual auto webview_go_back() -> Status {
    return unexpected(ErrorCode::Unsupported, "该宿主不支持嵌入浏览器");
  }
  [[nodiscard]] virtual auto webview_go_forward() -> Status {
    return unexpected(ErrorCode::Unsupported, "该宿主不支持嵌入浏览器");
  }

  /// 执行 JS 并**同步**回读结果（不解析 Promise——异步结果由页面 `post_message` 回推）。
  [[nodiscard]] virtual auto webview_execute_script([[maybe_unused]] std::string_view script,
                                                    [[maybe_unused]] std::string& result) -> Status {
    return unexpected(ErrorCode::Unsupported, "该宿主不支持嵌入浏览器");
  }
  [[nodiscard]] virtual auto webview_post_message([[maybe_unused]] std::string_view text) -> Status {
    return unexpected(ErrorCode::Unsupported, "该宿主不支持嵌入浏览器");
  }
  [[nodiscard]] virtual auto webview_set_zoom([[maybe_unused]] double factor) -> Status {
    return unexpected(ErrorCode::Unsupported, "该宿主不支持嵌入浏览器");
  }
  [[nodiscard]] virtual auto webview_set_script_enabled([[maybe_unused]] bool enabled) -> Status {
    return unexpected(ErrorCode::Unsupported, "该宿主不支持嵌入浏览器");
  }

  [[nodiscard]] virtual auto webview_state() const -> WebViewState { return {}; }
  [[nodiscard]] virtual auto webview_take_events() -> std::vector<WebViewEvent> { return {}; }
};

}  // namespace st::ui

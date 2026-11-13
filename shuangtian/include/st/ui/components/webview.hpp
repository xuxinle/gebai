#pragma once

/// @file webview.hpp
/// @brief `ui::WebView`：一块**可编程控制**的嵌入浏览器区域。
///
/// ## 它与 `NativeHost` 的分工（关键，别混）
///
/// | | `NativeHost` | `WebView`（本组件） |
/// |---|---|---|
/// | 嵌入的是什么 | **别人的**窗口（外部浏览器进程） | **自己的**页面对象（进程内 WebView2） |
/// | 能做什么 | 只能摆位置、裁剪、还原 | 导航、执行 JS、双向消息、缩放、读标题/地址 |
/// | 谁来管页面 | 外部程序 | 我们（`shell::webview::WebViewHost`） |
///
/// 需要"嵌一个我控制不了的窗口"用 `NativeHost`；需要"用网页做界面的一部分"用本组件。
///
/// ## 两种内容源（并列，都是一等能力）
///
/// ```cpp
/// // ① URL：真站点/内网系统
/// webview->navigate("https://内网/看板");
///
/// // ② 内嵌资源包：整前端打进 exe（相对路径 / 同源 fetch / ES module 全部可用）
/// WebViewAssets assets;
/// assets.add_html("/index.html", b::embed<"web/index.html">().str());
/// assets.add_css("/app.css", b::embed<"web/app.css">().str());
/// webview->load_embedded("/index.html", assets);
///
/// // ③ 单文件 HTML（没有 origin，相对子资源不可用）
/// webview->load_html("<h1>hello</h1>");
/// ```
///
/// ## 不可用时的行为（**关键契约**）
///
/// 与 `NativeHost` 同一条契约——**任何**环境下都不会让界面崩坏或报错：
///
/// | 环境 | 行为 |
/// |---|---|
/// | 宿主没给端口（无头 / 非 Windows / 未装 Runtime） | 退化为**占位块**（占位、画提示底色、`supported` 恒 false）；每个动作返回 false |
/// | 页面还没建起来 | 动作如实返回失败（**不假装成功**），占位块显示原因 |
/// | 加载失败 | `ready` 保持 false、`loading` 归 false，占位块显示失败原因 |
/// | 切页 / 滚出可见区 | 自动隐藏（与 `NativeHost` 同一机械：裁到 `bounds ∩ clip`） |
///
/// ## `capture` 看不到它（能力表必须写清）
///
/// 嵌入内容的像素**不进 `framebuffer()`**（由平台合成器直接上屏）。所以
/// **任何以 `capture` / `visual_diff` 为判据的回归都看不见网页**——它白了、冻了、
/// 渲染进程崩了，像素回归仍然全绿。要确认"页面真的在跑"，只能读本组件的属性面
/// （`attached` / `ready` / `url` / `title` / `loading` / `rasterization_scale`…）。
///
/// ## 生命周期责任
///
/// - 页面对象由宿主（`shell::webview`）拥有；本组件**不拥有**它。组件析构时调用
///   `webview_destroy()`，页面随之销毁（与 `NativeHost` 相反：那个不拥有外部窗口）。
/// - 一个宿主当前只支持**一个**页面对象（`WebView2` 一个 controller 对一个页面）。
///   同窗口多个 `WebView` 时，先附着者生效，其余如实报不可用。

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/ui/component_registry.hpp"
#include "st/ui/element.hpp"
#include "st/ui/webview_port.hpp"

namespace st::ui {

/// 内嵌资源包的装配期容器（组件侧的便利封装）。
///
/// 它只做"收集 + 推 MIME + 与 `b::embed` 对接"，**不是**运行期的表——
/// 传给 `load_embedded` 后由宿主侧转成自己的表。
class WebViewAssets {
 public:
  /// 文本资源（`mime` 留空时按路径扩展名推）。
  void add(std::string path, std::string_view body, std::string mime = {});
  /// HTML（`text/html`）。
  void add_html(std::string path, std::string_view body);
  /// CSS（`text/css`）。
  void add_css(std::string path, std::string_view body);
  /// JS / ES module（`text/javascript`）。
  void add_js(std::string path, std::string_view body);
  /// JSON（`application/json`）。
  void add_json(std::string path, std::string_view body);
  /// 任意字节（图片/字体/wasm 等；`mime` 必填，留空会按扩展名推）。
  void add_bytes(std::string path, std::vector<std::uint8_t> bytes, std::string mime = {});

  [[nodiscard]] auto empty() const noexcept -> bool { return assets_.empty(); }
  [[nodiscard]] auto size() const noexcept -> std::size_t { return assets_.size(); }
  [[nodiscard]] auto list() const -> const std::vector<WebViewAsset>& { return assets_; }

 private:
  std::vector<WebViewAsset> assets_{};
};

/// 可编程嵌入浏览器组件。见文件头注释。
class WebView : public Element {
 public:
  WebView();
  ~WebView() override;
  WebView(const WebView&) = delete;
  auto operator=(const WebView&) -> WebView& = delete;

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "WebView"; }

  /// 期望尺寸（逻辑像素）。宽度未指定时撑满可用宽度；高度未指定时用 `kDefaultHeight`。
  void set_preferred_size(math::Size size);
  [[nodiscard]] auto preferred_size() const noexcept -> math::Size { return preferred_; }

  /// 占位块是否显示（未就绪/不支持时画一块提示底色）。默认 `true`。
  ///
  /// 为什么默认显示：留空看起来像"组件没渲染"（用户与智能体都会误判为缺陷）；
  /// 显示一块明确的占位块才是"这里本该有内容，但当前给不了"的诚实表达。
  void set_placeholder_visible(bool visible);
  [[nodiscard]] auto placeholder_visible() const noexcept -> bool { return placeholder_; }
  void set_placeholder_text(std::string text);
  [[nodiscard]] auto placeholder_text() const -> const std::string& { return placeholder_text_; }

  // ── 内容源（三种，都是一等能力）──

  /// ① URL 导航。`false` = 不可用/参数非法（原因见 `status_text()`）。
  auto navigate(std::string_view url) -> bool;
  /// ② 单文件 HTML（**没有 origin**：相对路径子资源不可用）。
  auto load_html(std::string_view html) -> bool;
  /// ③ 内嵌资源包：以虚拟 origin 提供 `assets`，入口为 `entry`（如 `/index.html`）。
  ///
  /// 这条是"整前端打进 exe"的形态：相对路径、同源 `fetch`、ES module、CSS 全都可用
  ///（因为页面有真正的 origin，只是所有请求都被我们拦下从内存取）。
  auto load_embedded(std::string_view entry, const WebViewAssets& assets) -> bool;

  // ── 导航控制 ──
  auto reload() -> bool;
  auto stop() -> bool;
  auto go_back() -> bool;
  auto go_forward() -> bool;

  // ── 编程控制 ──

  /// 执行 JS 并回读结果（`result` 是 JSON 表示；`undefined` 时为空串）。
  ///
  /// ⚠ **不解析 Promise**：`async` 函数的返回是 `{}`。异步流程用 `post_message`
  /// 让页面回推，并在 `on_message` 里收。
  auto execute_script(std::string_view script, std::string& result) -> bool;
  /// 宿主 → 页面：发一条字符串消息。
  auto post_message(std::string_view text) -> bool;
  auto set_zoom(double factor) -> bool;
  auto set_script_enabled(bool enabled) -> bool;

  // ── 观测 ──
  [[nodiscard]] auto state() const -> WebViewState;
  /// 本机此刻能不能用（宿主有端口 + 端口自报支持）。
  [[nodiscard]] auto supported() const -> bool;
  /// 当前状态的人话描述（占位块与状态栏用；不可用时说明**缺什么**）。
  [[nodiscard]] auto status_text() const -> std::string;

  /// 事件回调（页面 → 宿主；`WebMessageReceived` / 导航完成 / 标题变化…）。
  ///
  /// 为何用单个回调而不是每种事件一个字段：事件有六种，六个 `std::function`
  /// 会让"只关心一条"的调用方写五行空 lambda；而它们的**触发点相同**
  ///（都在事件派发处），按 `kind` 分派更直接。
  std::function<void(const WebViewEvent&)> on_event{};

  // ── 属性面（控制通道 `get` / 面板生成）──
  [[nodiscard]] auto readonly_property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  /// 动作面（控制通道 `invoke`）：
  ///
  /// | 动作 | argument | 说明 |
  /// |---|---|---|
  /// | `navigate` | URL | 导航 |
  /// | `load_html` | HTML | 单文件 |
  /// | `reload` / `stop` / `back` / `forward` | — | 导航控制 |
  /// | `exec` | JS | 执行并**在返回值属性 `last_result` 里**留结果 |
  /// | `post` | 文本 | 发消息给页面 |
  /// | `zoom` | 倍数 | 缩放 |
  /// | `set_script` | `true`/`false` | 脚本开关 |
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument) -> bool override;

  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  void on_visibility_changed() override;
  void apply_theme(const Theme& theme) override;

  /// 收事件（由 `UiRoot::dispatch_webview_events` 调；也允许应用直接调）。
  void handle_webview_event(const WebViewEvent& event);

 private:
  [[nodiscard]] auto port() const -> WebViewPort*;
  /// 确保页面对象已建（首次 `arrange` 时建；已是幂等）。
  ///
  /// 返回 `false` = 建不出来（无端口 / 端口不支持 / 创建失败）。
  /// **调用方必须据此提前返回**：不支持时继续往下走就会变成「看着在做事、实际什么都不会发生」。
  [[nodiscard]] auto ensure_page() -> bool;
  /// 把当前矩形（与可见区）同步给宿主。
  void sync_bounds();
  void set_status(std::string text);

  static constexpr float kDefaultHeight{360.0f};

  math::Size preferred_{0.0f, kDefaultHeight};
  bool placeholder_{true};
  std::string placeholder_text_{"网页区域"};
  /// 最近一次动作的状态（成功/失败原因）；占位块与属性面读它。
  std::string status_{"未加载"};
  /// 最近一次 `exec` 的回读结果（`invoke` 动作面用；属性面 `last_result`）。
  std::string last_result_{};
  /// 已请求建页面（避免每帧重复请求）。
  bool page_requested_{false};
  bool has_synced_{false};
  math::Rect last_synced_{};
  math::Rect last_clip_{};
  math::Color placeholder_fill_{0, 0, 0, 0};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::WebView, "WebView");

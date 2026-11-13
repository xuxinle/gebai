// `ui::WebView`（可编程嵌入浏览器）的回归测试。
//
// ## 这些用例在钉什么
//
// 与 `ui_native_host_test.cpp` 同一套哲学：这个能力是**可选**的（大部分环境没有
// 浏览器宿主：CI 无头、Linux、未装 Runtime 的机器）。所以第一条要钉死的不变量不是
// "能加载网页"，而是：
//
//   **不可用时，界面其余部分完全不受影响；每个动作如实失败，不假装成功。**
//
// 第二条是**内容源的契约差异**——单文件 HTML 没有 origin，这条如果不在文档/属性面
// 里说清，用的人会以为"相对路径为什么加载不到"是缺陷。
//
// 第三条是**异步结果的真实现状**：`ExecuteScript` 不解析 Promise，异步结果必须由页面
// `postMessage` 回推。这是实测出来的平台行为，钉住它是为了别让后来人再造一个
// "猜延时"的读取路径。
//
// ## 为什么不用像素断言
//
// 嵌入内容的像素**不进 `framebuffer`**（由平台合成器上屏），`capture` 拍不到。
// 与 `NativeHost` 完全同一条口径：判据只能走属性面与调用记录。

#include "st/test/test.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/shell/webview.hpp"
#include "st/ui/components/panel.hpp"
#include "st/ui/components/webview.hpp"
#include "st/ui/element.hpp"
#include "st/ui/ui_root.hpp"
#include "st/ui/webview_port.hpp"

#include "tests/support/text_port_fixtures.hpp"

using st::math::Rect;
using st::math::Size;
using st::ui::Element;
using st::ui::WebView;
using st::ui::WebViewAsset;
using st::ui::WebViewAssets;
using st::ui::WebViewEvent;
using st::ui::WebViewPort;
using st::ui::WebViewSource;
using st::ui::WebViewState;

namespace {

[[nodiscard]] auto rect_near(Rect a, Rect b) -> bool {
  constexpr float kEpsilon = 0.01F;
  return std::abs(a.x - b.x) < kEpsilon && std::abs(a.y - b.y) < kEpsilon &&
         std::abs(a.width - b.width) < kEpsilon && std::abs(a.height - b.height) < kEpsilon;
}

/// 可编程的浏览器端口桩：记录所有调用，并允许每个用例摆出不同的失败形态。
///
/// ## 桩必须复现被测的那一段真实行为（`CONVENTIONS.md` §7.2）
///
/// 这里最容易写成"永远返回 ok"的假桩——那样"组件有没有真的检查返回值"就测不出来。
/// 所以 `supports` / `*_ok` / `attached` / `ready` 都是**可编程**的，各用例自己摆。
struct PortStub final : WebViewPort {
  bool supports{false};
  bool create_ok{true};
  bool navigate_ok{true};
  bool embedded_ok{true};
  bool html_ok{true};
  bool exec_ok{true};
  std::string exec_result{"\"ok\""};
  std::string exec_error{"脚本抛异常：boom"};

  WebViewState state{};
  std::vector<WebViewEvent> pending{};

  std::vector<Rect> created{};
  std::vector<Rect> synced_bounds{};
  std::vector<Rect> synced_clips{};
  std::vector<std::string> navigated{};
  std::vector<std::string> html_loaded{};
  std::vector<std::string> embedded_entries{};
  std::vector<std::size_t> embedded_asset_counts{};
  std::vector<std::string> scripts{};
  std::vector<std::string> posted{};
  std::vector<double> zooms{};
  int destroyed{0};

  [[nodiscard]] auto webview_supported() const noexcept -> bool override { return supports; }
  [[nodiscard]] auto webview_unsupported_reason() const -> std::string override {
    return supports ? std::string{} : std::string{"未安装 WebView2 Runtime"};
  }

  [[nodiscard]] auto webview_create(Rect bounds) -> st::Status override {
    created.push_back(bounds);
    if (!create_ok) return st::unexpected(st::ErrorCode::Io, "建页面失败");
    // 真实后端建完是 attached=true；这里同步摆上（桩不模拟异步创建，
    // 那条时序由真窗口 e2e 覆盖——桩里模拟异步只会让断言变成"等几帧"）。
    state.attached = true;
    return st::ok();
  }
  void webview_destroy() override {
    ++destroyed;
    state = WebViewState{};
  }
  void webview_sync(Rect bounds, Rect clip) override {
    synced_bounds.push_back(bounds);
    synced_clips.push_back(clip);
    state.visible = !(clip.width <= 0.0F || clip.height <= 0.0F);
  }

  [[nodiscard]] auto webview_navigate(std::string_view url) -> st::Status override {
    navigated.emplace_back(url);
    if (!navigate_ok) return st::unexpected(st::ErrorCode::Io, "导航失败");
    state.url = std::string(url);
    state.source = WebViewSource::Url;
    state.ready = true;
    return st::ok();
  }
  [[nodiscard]] auto webview_load_html(std::string_view html) -> st::Status override {
    html_loaded.emplace_back(html);
    if (!html_ok) return st::unexpected(st::ErrorCode::Io, "加载 HTML 失败");
    state.source = WebViewSource::Html;
    state.ready = true;
    return st::ok();
  }
  [[nodiscard]] auto webview_load_embedded(std::string_view entry,
                                           const std::vector<WebViewAsset>& assets)
      -> st::Status override {
    embedded_entries.emplace_back(entry);
    embedded_asset_counts.push_back(assets.size());
    // 入口检查**总是**做（真实后端就是这么做的）——不随 `embedded_ok` 开关。
    //
    // 这是 `CONVENTIONS.md` §7.2 的又一个变体：若把“入口不存在”藏在开关后面，
    // 默认路径（`embedded_ok=true`）就会永远成功 ——「入口必须存在」这条契约于是**零覆盖**
    //（实测：`load_embedded("/missing.html")` 返回 true，断言全红）。
    if (std::none_of(assets.begin(), assets.end(),
                     [entry](const WebViewAsset& asset) { return asset.path == entry; })) {
      return st::unexpected(st::ErrorCode::NotFound,
                            std::format("内嵌资源包里没有入口 {}", entry));
    }
    if (!embedded_ok) return st::unexpected(st::ErrorCode::Io, "加载内嵌资源失败");
    state.source = WebViewSource::Embedded;
    state.url = std::format("https://app.st.invalid{}", entry);
    state.ready = true;
    state.asset_count = assets.size();
    return st::ok();
  }

  [[nodiscard]] auto webview_reload() -> st::Status override { return st::ok(); }
  [[nodiscard]] auto webview_stop() -> st::Status override { return st::ok(); }
  [[nodiscard]] auto webview_go_back() -> st::Status override { return st::ok(); }
  [[nodiscard]] auto webview_go_forward() -> st::Status override { return st::ok(); }

  [[nodiscard]] auto webview_execute_script(std::string_view script, std::string& result)
      -> st::Status override {
    scripts.emplace_back(script);
    if (!exec_ok) return st::unexpected(st::ErrorCode::Parse, exec_error);
    result = exec_result;
    return st::ok();
  }
  [[nodiscard]] auto webview_post_message(std::string_view text) -> st::Status override {
    posted.emplace_back(text);
    return st::ok();
  }
  [[nodiscard]] auto webview_set_zoom(double factor) -> st::Status override {
    zooms.push_back(factor);
    state.zoom = factor;
    return st::ok();
  }
  [[nodiscard]] auto webview_set_script_enabled(bool enabled) -> st::Status override {
    (void)enabled;
    return st::ok();
  }

  [[nodiscard]] auto webview_state() const -> WebViewState override { return state; }
  [[nodiscard]] auto webview_take_events() -> std::vector<WebViewEvent> override {
    std::vector<WebViewEvent> out = std::move(pending);
    pending.clear();
    return out;
  }
};

/// 夹具：`UiRoot` + 一块 `WebView` + 装在 root 上的端口。
///
/// ## 为何走 `set_webview_port` 而不是自己 `set_host(&stub)`
///
/// 与 `NativeHost` 的夹具同一个理由：`UiRoot::set_content` 会把**它自己**作为宿主契约
/// 写到整棵子树。绕过它直接 `set_host` 测到的就不是生产路径，而且会被重新覆盖。
struct Fixture {
  /// 声明顺序刻意如此：C++ 逆序析构 ⇒ `stub` 必须比 `root` 活得久。
  ///
  /// 原因与 `NativeHost` 实测过的一样：`root` 析构 → 销毁内容树 → `WebView` 析构
  /// → `webview_destroy()` → 端口回调 → 读写 `stub`。若 `stub` 先销，就是访问已销毁对象
  ///（表现是堆损坏退出，没有断言、没有栈线索）。
  PortStub stub{};
  st::test::FixedAdvanceTextPort port{};
  st::ui::UiRoot root{};
  WebView* view{nullptr};

  explicit Fixture(bool supported = true) : Fixture(supported, /*create_ok=*/true) {}

  /// `create_ok = false` 用来测“建页面失败”那条契约。
  ///
  /// ## 为何必须在**构造前**就摆好
  ///
  /// 夹具构造会 `layout(true)`，而布局会触发难建页面（`ensure_page`）。所以“构造完
  /// 再把 `create_ok` 摆成 false”是无效的——页面已经建好了，断言测到的是成功路径
  ///（实测：`navigate` 返回 true，三条断言全红）。
  Fixture(bool supported, bool create_ok) {
    stub.supports = supported;
    stub.create_ok = create_ok;
    root.set_text_port(&port);
    root.set_viewport(Size{800.0F, 600.0F});

    auto panel = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
    auto owned = std::make_unique<WebView>();
    view = owned.get();
    panel->add_child(std::move(owned));
    root.set_content(std::move(panel));
    // 与 `Application` 的装配同构：一条端口指针。
    root.set_webview_port(&stub);
    root.layout(true);
  }
};

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 第一组：**不可用时不影响基础功能**（本组件最重要的契约）
// ─────────────────────────────────────────────────────────────────────────────

ST_TEST(webview_without_port_degrades_to_placeholder) {
  // 完全没有端口（离屏渲染 / 未上树 / 无头）：每个动作如实 false，且不崩。
  st::test::FixedAdvanceTextPort port;
  st::ui::UiRoot root;
  root.set_text_port(&port);
  root.set_viewport(Size{640.0F, 480.0F});
  auto owned = std::make_unique<WebView>();
  WebView* view = owned.get();
  root.set_content(std::move(owned));
  root.layout(true);

  ST_CHECK(!view->supported());
  ST_CHECK(!view->navigate("https://example.com"));
  ST_CHECK(!view->load_html("<h1>x</h1>"));
  ST_CHECK(!view->reload());

  // 布局必须照常完成：组件有尺寸、在正确位置（"不影响界面其余部分"）。
  ST_CHECK(!view->bounds().is_empty());
  ST_CHECK(view->bounds().width == 640.0F);

  // 状态必须诚实说明"缺什么"，而不是一句"不可用"。
  const std::string status = view->status_text();
  ST_CHECK(status.find("无头") != std::string::npos ||
           status.find("非 Windows") != std::string::npos);
}

ST_TEST(webview_port_not_supporting_degrades_and_does_not_call) {
  Fixture fx(/*supported=*/false);

  ST_CHECK(!fx.view->supported());
  ST_CHECK(!fx.view->navigate("https://example.com"));
  ST_CHECK(!fx.view->load_embedded("/index.html", WebViewAssets{}));

  // 关键：**不支持时不该发出任何建页面/导航请求**（组件连试都不试）。
  ST_CHECK(fx.stub.created.empty());
  ST_CHECK(fx.stub.navigated.empty());
  ST_CHECK(fx.stub.embedded_entries.empty());

  // 原因要能读出来（"没装 Runtime" vs "非 Windows" 决定用户该做什么）。
  ST_CHECK(fx.view->status_text().find("Runtime") != std::string::npos);
}

ST_TEST(webview_create_failure_is_reported_not_faked) {
  // 建页面从**第一次**就失败（夹具构造前就摆好，见 `Fixture` 的注释）。
  Fixture fx(/*supported=*/true, /*create_ok=*/false);
  ST_CHECK(!fx.view->navigate("https://example.com"));
  // 建页面失败后**不该**再发出导航（半状态比失败本身更糟）。
  ST_CHECK(fx.stub.navigated.empty());
  ST_CHECK(fx.view->status_text().find("建页面失败") != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// 第二组：三种内容源的契约差异（尤其是单文件 HTML 没有 origin）
// ─────────────────────────────────────────────────────────────────────────────

ST_TEST(webview_navigate_records_url_and_state) {
  Fixture fx(/*supported=*/true);
  ST_CHECK(fx.view->navigate("https://example.com/a"));
  ST_CHECK(fx.stub.navigated.size() == 1);
  ST_CHECK(fx.stub.navigated.front() == "https://example.com/a");
  const WebViewState state = fx.view->state();
  ST_CHECK(state.source == WebViewSource::Url);
  ST_CHECK(state.url == "https://example.com/a");
  // 属性面与状态同源（控制通道 `get` 读的就是它）。
  ST_CHECK(fx.view->get_property("url").value_or("") == "https://example.com/a");
  ST_CHECK(fx.view->get_property("source").value_or("") == "url");
}

ST_TEST(webview_load_embedded_requires_existing_entry) {
  Fixture fx(/*supported=*/true);
  WebViewAssets assets;
  assets.add_html("/index.html", "<h1>x</h1>");
  assets.add_css("/a.css", "body{}");

  // 入口不存在：**如实失败**，不落空白页。
  ST_CHECK(!fx.view->load_embedded("/missing.html", assets));
  ST_CHECK(fx.view->status_text().find("没有入口") != std::string::npos);

  // 入口存在：成功，且资源条数带过去。
  ST_CHECK(fx.view->load_embedded("/index.html", assets));
  ST_CHECK(fx.stub.embedded_entries.size() == 2);   // 含失败那一次与成功那一次
  ST_CHECK(fx.stub.embedded_entries.back() == "/index.html");
  ST_CHECK(fx.stub.embedded_asset_counts.back() == 2);
  ST_CHECK(fx.view->state().source == WebViewSource::Embedded);
  ST_CHECK(fx.view->state().asset_count == 2);
}

ST_TEST(webview_load_html_marks_source_html_and_clears_url) {
  Fixture fx(/*supported=*/true);
  ST_CHECK(fx.view->load_html("<h1>inline</h1>"));
  ST_CHECK(fx.stub.html_loaded.size() == 1);
  const WebViewState state = fx.view->state();
  ST_CHECK(state.source == WebViewSource::Html);
  // 单文件 HTML **没有 origin**：地址必须是空，而不是编一个假地址出来。
  ST_CHECK(state.url.empty());
  ST_CHECK(fx.view->get_property("source").value_or("") == "html");
}

// ─────────────────────────────────────────────────────────────────────────────
// 第三组：编程控制面（执行脚本 / 消息 / 缩放）
// ─────────────────────────────────────────────────────────────────────────────

ST_TEST(webview_execute_script_exposes_result_via_property) {
  Fixture fx(/*supported=*/true);
  fx.stub.exec_result = "\"Example Domain\"";
  std::string result;
  ST_CHECK(fx.view->execute_script("document.title", result));
  ST_CHECK(result == "\"Example Domain\"");
  ST_CHECK(fx.stub.scripts.size() == 1);
  ST_CHECK(fx.stub.scripts.front() == "document.title");
  // `exec` 动作面必须把结果留在属性里——控制通道 `invoke` 之后才能读到。
  ST_CHECK(fx.view->get_property("last_result").value_or("") == "\"Example Domain\"");
}

ST_TEST(webview_execute_script_failure_is_not_an_empty_result) {
  Fixture fx(/*supported=*/true);
  fx.stub.exec_ok = false;
  std::string result;
  ST_CHECK(!fx.view->execute_script("boom()", result));
  // 关键：失败与"结果为空"必须分得开——空串是合法的 `undefined` 结果。
  ST_CHECK(fx.view->status_text().find("脚本抛异常") != std::string::npos);
  ST_CHECK(fx.view->get_property("last_result").value_or("x").empty());
}

ST_TEST(webview_invoke_exec_returns_result_and_handles_bad_zoom) {
  Fixture fx(/*supported=*/true);
  fx.stub.exec_result = "42";
  ST_CHECK(fx.view->invoke_action("exec", "1+1"));
  ST_CHECK(fx.view->get_property("last_result").value_or("") == "42");

  // 缩放参数非法要如实报（`stod` 抛异常被吞，不许变成"没反应"）。
  ST_CHECK(!fx.view->invoke_action("zoom", "abc"));
  ST_CHECK(fx.view->status_text().find("不是数字") != std::string::npos);
  ST_CHECK(fx.stub.zooms.empty());

  ST_CHECK(fx.view->invoke_action("zoom", "1.5"));
  ST_CHECK(fx.stub.zooms.size() == 1);
  ST_CHECK(fx.stub.zooms.front() == 1.5);
}

ST_TEST(webview_post_message_goes_to_port) {
  Fixture fx(/*supported=*/true);
  ST_CHECK(fx.view->post_message("hello"));
  ST_CHECK(fx.stub.posted.size() == 1);
  ST_CHECK(fx.stub.posted.front() == "hello");
}

// ─────────────────────────────────────────────────────────────────────────────
// 第四组：几何同步 —— 与 `NativeHost` 同一套"bounds ∩ clip"机械
// ─────────────────────────────────────────────────────────────────────────────

ST_TEST(webview_syncs_bounds_and_clip_to_port) {
  Fixture fx(/*supported=*/true);
  // 首次布局就把矩形发给端口（建页面用的是同一块矩形）。
  ST_CHECK(!fx.stub.synced_bounds.empty());
  ST_CHECK(rect_near(fx.stub.synced_bounds.back(), fx.view->bounds()));
  // 可见区必须一起发（平台子窗口不吃 UI 的裁剪栈，靠宿主自己裁）。
  ST_CHECK(!fx.stub.synced_clips.empty());

  // 视口变了：必须重新下发。
  //
  // 为何必须改尺寸：组件有一层“值没变就跳过”的早退（`sync_bounds` 的 `rect_equals`），
  // 不真变的话第二次下发永远不会发生——而那条早退本身是对的保护（免得每帧重排嵌入内容）。
  fx.stub.synced_bounds.clear();
  fx.root.set_viewport(Size{1000.0F, 700.0F});
  fx.root.layout(true);
  ST_CHECK(!fx.stub.synced_bounds.empty());
  ST_CHECK(rect_near(fx.stub.synced_bounds.back(), fx.view->bounds()));
}

ST_TEST(webview_hidden_by_ancestor_sends_empty_clip) {
  Fixture fx(/*supported=*/true);
  ST_CHECK(fx.view->state().visible);

  // 祖先隐藏：`visible_rect()` 变空 ⇒ 端口收到空 clip ⇒ 隐藏。
  //
  // 这条是"切页后网页仍盖在新页面上"那个实测缺陷的护栏（`NativeHost` 同一处）。
  fx.root.content()->set_visible(false);
  fx.root.layout(true);
  ST_CHECK(!fx.stub.synced_clips.empty());
  const Rect clip = fx.stub.synced_clips.back();
  ST_CHECK(clip.width <= 0.0F || clip.height <= 0.0F);
  ST_CHECK(!fx.view->state().visible);
  ST_CHECK(fx.view->get_property("shown").value_or("true") == "false");
}

// ─────────────────────────────────────────────────────────────────────────────
// 第五组：事件与资源表（口径一致性）
// ─────────────────────────────────────────────────────────────────────────────

ST_TEST(webview_events_reach_component_callback) {
  Fixture fx(/*supported=*/true);
  std::vector<WebViewEvent> seen;
  fx.view->on_event = [&seen](const WebViewEvent& event) { seen.push_back(event); };

  WebViewEvent message;
  message.kind = WebViewEvent::Kind::WebMessageReceived;
  message.message = "page-ready";
  fx.stub.pending.push_back(message);
  // 走与 `Application::tick` 完全相同的路径：取走 → `UiRoot::dispatch_webview_events`。
  fx.root.dispatch_webview_events(fx.stub.webview_take_events());

  ST_CHECK(seen.size() == 1);
  ST_CHECK(seen.front().kind == WebViewEvent::Kind::WebMessageReceived);
  ST_CHECK(seen.front().message == "page-ready");
  // 状态行也该反映出"收到了消息"（否则界面上看不出页面主动说话了）。
  ST_CHECK(fx.view->status_text().find("页面消息") != std::string::npos);
}

ST_TEST(webview_assets_guess_mime_from_extension) {
  // MIME 推错会让模块脚本被当普通文本拒收——而控制台只报一句含糊的 MIME 错误。
  WebViewAssets assets;
  assets.add("/index.html", "<h1>x</h1>");           // 留空 mime ⇒ 推
  assets.add("/app.js", "export const a = 1;");
  assets.add("/a.css", "body{}");
  assets.add("/data.json", "{}");
  assets.add_bytes("/logo.png", {0x89, 0x50});

  ST_CHECK(assets.size() == 5);
  const auto& list = assets.list();
  ST_CHECK(list[0].mime == "text/html");
  ST_CHECK(list[1].mime == "text/javascript");
  ST_CHECK(list[2].mime == "text/css");
  ST_CHECK(list[3].mime == "application/json");
  ST_CHECK(list[4].mime == "image/png");
}

ST_TEST(webview_assets_mime_matches_shell_side) {
  // 两侧各有一份 MIME 推断（`ui` 不能包含 `shell`，依赖方向）。
  // 口径分家是这类缺陷的典型形状，所以这里对齐一遍关键扩展名。
  const auto check = [](std::string_view path, std::string_view expected) {
    WebViewAssets assets;
    assets.add(std::string(path), "x");
    ST_CHECK(assets.list().front().mime == expected);
    st::shell::webview::WebAssets shell_assets;
    shell_assets.add_text(std::string(path), {}, "x");
    const auto* found = shell_assets.find(path);
    ST_CHECK(found != nullptr);
    if (found != nullptr) ST_CHECK(found->mime == expected);
  };
  check("/a.html", "text/html");
  check("/a.js", "text/javascript");
  check("/a.mjs", "text/javascript");
  check("/a.css", "text/css");
  check("/a.json", "application/json");
  check("/a.wasm", "application/wasm");
  check("/a.svg", "image/svg+xml");
  check("/a.woff2", "font/woff2");
}

// ============================================================================
// 能力级拆链的自动推导——回归用例
//
// ## 钉住什么
//
// "应用不用某项能力，产物里就不该有它"（pay-for-what-you-use）。判据是
// `mentions_identifier`（按词边界找标识符），它决定是否给目标加 `ST_NO_WEBVIEW`。
//
// ## 为何必须按词边界（实测代价）
//
// 纯子串匹配会让 `WebViewPort` 命中 `WebView` —— 而 `WebViewPortAdapter` 是
// **框架侧装配件**（`app.cpp` 自用），不是"应用要用浏览器"。判错方向的后果不对称：
// - 误判"要用" ⇒ 每个应用白背 191 KB 的 `WebView2Loader.dll` 嵌入数据；
// - 误判"不用" ⇒ 真正要用浏览器的应用拿到"未编入"，**功能没了**。
// 所以两条都要钉：边界必须挡掉 `WebViewPort`，也必须放行 `new WebView`。
// ============================================================================

#include <string>

#include "st/pkg/build.hpp"
#include "st/test/test.hpp"

namespace {
using st::pkg::mentions_identifier;
}  // namespace

ST_TEST(capability_pruning_mentions_real_use) {
  // 真实引用的各种写法都要命中。
  ST_CHECK(mentions_identifier("auto v = std::make_unique<WebView>();", "WebView"));
  ST_CHECK(mentions_identifier("using st::ui::WebView;", "WebView"));
  ST_CHECK(mentions_identifier("class WebView : public Element {", "WebView"));
  ST_CHECK(mentions_identifier("WebView", "WebView"));  // 整段就是它
  ST_CHECK(mentions_identifier("st::ui::WebView* p = nullptr;", "WebView"));
  // 行首/行尾/标点邻接都算边界。
  ST_CHECK(mentions_identifier("(WebView)", "WebView"));
  ST_CHECK(mentions_identifier("new WebView\n", "WebView"));
}

ST_TEST(capability_pruning_ignores_longer_identifiers) {
  // 这是缺陷的真实形态：`WebViewPort` / `WebViewAdapter` 含 `WebView` 但不是它。
  // 若这里判真，每个应用都白背 191 KB（正是本次要消灭的那笔负担）。
  ST_CHECK(!mentions_identifier("ui::WebViewPort* port;", "WebView"));
  ST_CHECK(!mentions_identifier("class WebViewPortAdapter final {", "WebView"));
  ST_CHECK(!mentions_identifier("host_webview_port()", "WebView"));
  ST_CHECK(!mentions_identifier("MyWebView", "WebView"));
  ST_CHECK(!mentions_identifier("WebViewX", "WebView"));
}

ST_TEST(capability_pruning_edge_cases) {
  // 空文本 / 空标识符 / 文本比标识符短：都不命中，且不越界。
  ST_CHECK(!mentions_identifier("", "WebView"));
  ST_CHECK(!mentions_identifier("Web", "WebView"));
  ST_CHECK(!mentions_identifier("WebView", ""));
  // 多次出现里只有一处是真引用时仍要命中（不能只检查第一处）。
  ST_CHECK(mentions_identifier("WebViewPort a; WebViewPort b; new WebView();", "WebView"));
  // 反复扫描不应死循环（同一位置多次命中带前缀的情况）。
  ST_CHECK(!mentions_identifier("WebViewWebViewWebView", "WebView"));
}

ST_TEST(capability_pruning_distinguishes_other_capabilities) {
  // 同一套判据也用于其他能力名（如 ScriptHost）。互不串味。
  ST_CHECK(mentions_identifier("ui::ScriptHost host;", "ScriptHost"));
  ST_CHECK(!mentions_identifier("ui::ScriptHostConfig c;", "ScriptHost"));
  ST_CHECK(!mentions_identifier("ui::ScriptHost host;", "WebView"));
}

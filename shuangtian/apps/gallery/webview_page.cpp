/// @file
/// @brief gallery 的「嵌入浏览器（WebView2）」页：**可编程控制**的网页区域演示。
///
/// ## 与「原生嵌入」页的分工
///
/// 那一页演示 `NativeHost`——把**别人的**窗口摆进来（只能看，不能控）。
/// 这一页演示 `ui::WebView`——用**自己的**页面对象做界面的一部分（能导航、
/// 执行脚本、双向消息）。两者的像素都不进 `framebuffer`，判据都只能读属性面。
///
/// ## 页面自己承担的三件事（为什么不能交给应用）
///
/// 1. 装配**内嵌资源包**（纯数据，`b::embed` 或字面量都可）；
/// 2. 把"三种内容源"各跑一次（URL / 内嵌包 / 单文件 HTML）；
/// 3. 把自检读数贴出来（`ready` / `url` / `title` / `rasterization_scale`）。
///
/// 页面**不**知道 `Application` 的存在：能力经组件的端口取（`WebView::supported()`）。

#include "webview_page.hpp"

#include <format>
#include <string>
#include <utility>

#include "st/core/json.hpp"

#include "st/ui/components/button.hpp"
#include "st/ui/components/card.hpp"
#include "st/ui/components/heading.hpp"
#include "st/ui/components/input_box.hpp"
#include "st/ui/components/key_value_row.hpp"
#include "st/ui/components/panel.hpp"
#include "st/ui/components/text.hpp"
#include "st/ui/components/webview.hpp"

namespace gallery {
namespace {

/// 「宿主函数」——页面 `stBridge.call('greet', {name})` 最终执行的就是它。
///
/// 这里刻意做得真实一点：入参检查、数值计算、带时区的数据都上，
/// 因为它们才是实际用途（网页调宿主的能力）与“传个字符串回去”的区别。
[[nodiscard]] auto host_greet(const st::Json& args) -> st::Json {
  const std::string name = st::json_get_string(args, "name", "匿名");
  st::Json reply = st::Json::object();
  reply["message"] = "你好，" + name + "！—— 这条消息由 **C++ 函数**生成";
  reply["from"] = "c++";
  // 回一些页面自己拿不到的东西：宿主进程的真实信息。
  reply["pointer_bytes"] = static_cast<std::int64_t>(sizeof(void*));
  reply["cxx_standard"] = static_cast<std::int64_t>(__cplusplus);
  reply["args_echo"] = args;
  return reply;
}

using st::math::Insets;
using st::ui::Button;
using st::ui::Card;
using st::ui::Element;
using st::ui::FlexDirection;
using st::ui::Input;
using st::ui::KeyValueRow;
using st::ui::Panel;
using st::ui::Text;
using st::ui::Tone;
using st::ui::WebView;
using st::ui::WebViewAssets;

[[nodiscard]] auto make_page(std::string_view id, std::string_view title,
                             std::string_view subtitle) -> std::unique_ptr<Panel> {
  auto page = std::make_unique<Panel>(FlexDirection::Column);
  page->set_id(std::format("page-{}", id));
  page->style().padding = Insets::all(24.0f);
  page->style().gap = 16.0f;
  page->add_child(std::make_unique<st::ui::Heading>(std::string(title), 2));
  auto sub = std::make_unique<Text>(std::string(subtitle));
  sub->set_tone(Tone::Muted);
  page->add_child(std::move(sub));
  return page;
}

[[nodiscard]] auto make_card(std::string_view id, std::string_view title) -> std::unique_ptr<Card> {
  auto card = std::make_unique<Card>(18.0f);
  card->set_id(std::string(id));
  card->style().direction = FlexDirection::Column;
  card->style().gap = 12.0f;
  if (!title.empty()) {
    auto heading = std::make_unique<Text>(std::string(title));
    heading->set_weight(st::ui::FontWeight::SemiBold);
    card->add_child(std::move(heading));
  }
  return card;
}

[[nodiscard]] auto make_row(bool grow = false) -> std::unique_ptr<Panel> {
  auto row = std::make_unique<Panel>(FlexDirection::Row);
  row->style().gap = 8.0f;
  row->style().align_items = st::ui::Align::Center;
  row->style().grow = grow;
  return row;
}

/// 演示用的**内嵌资源包**（整个"网站"在 exe 里，离线可用）。
///
/// ## 为什么要给三个文件而不是一个 HTML
///
/// 单个 HTML 用 `load_html()` 就够，但那样**用不上本能力的核心**：内嵌包的价值在于
/// 相对路径、同源 `fetch`、ES module、CSS 全都照常工作——而那必须有虚拟 origin。
/// 所以这个演示刻意把样式、脚本、数据拆成三个文件，页面里用相对路径引用它们：
/// 只要这三类都正常，就证明"虚拟 origin 这条路是通的"。
/// ## 模拟的「宿主函数」（JS 调 C++）
///
/// 回环：页面 `await window.stBridge.call('greet', {name})` → 页面 `postMessage`
/// 一个 JSON 请求给宿主 → 宿主 `on_event` 收到、执行**这个 C++ 函数**、用
/// `post_message` 把结果回推 → 页面 resolve。
///
/// 为何不用 `AddHostObjectToScript`（COM）：那条路要 IDispatch + 注册表 +
/// 跨进程代理，页面侧还只在 **HTTPS origin** 下可用（虚拟 origin 不一定满足）——
/// 而本仓的承载方式是“页面完全内嵌”，请求/响应回环更简单也更可控，
/// 且同一条路在 POSIX 上照样成立。
///
/// 为何需要注入的 SDK：悬空调用会抛错，且页面要在**请求发出去之前**
/// 记住 pending（否则同步回来的响应先到、没人收）。SDK 自动判断桥是否就绪，
/// 未就绪时等一次 `page-ready` 再重试。
[[nodiscard]] auto demo_site() -> WebViewAssets {
  WebViewAssets assets;
  assets.add_html("/index.html", R"html(<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<title>内嵌资源演示</title>
<link rel="stylesheet" href="./style.css">
</head><body>
<main>
  <h1 id="heading">资源全部在 exe 里</h1>
  <p class="sub">相对路径 · 同源 fetch · ES module · CSS 四样都可用</p>
  <ul id="facts"></ul>
  <button id="ping">向宿主发一条消息</button>
  <p id="pong" class="pong">（还没收到宿主消息）</p>
  <hr>
  <h2>JS 调用 C++ 函数</h2>
  <p class="sub">页面发请求 → 宿主执行 C++ → 回推结果（请求/响应回环）</p>
  <button id="call-cpp">调用 C++ 的 greet()</button>
  <pre id="cpp-result" class="pong">（还没调用过）</pre>
</main>
<script type="module" src="./app.js"></script>
</body></html>)html");
  assets.add_css("/style.css", R"css(:root { color-scheme: light; }
body { margin: 0; font: 15px/1.65 system-ui, -apple-system, "Segoe UI", sans-serif;
       color: #1d2330; background: linear-gradient(135deg, #eef2ff, #f8fafc 45%, #eefcf6); }
main { padding: 28px 32px; }
h1 { margin: 0 0 4px; font-size: 26px; letter-spacing: -.4px; }
p.sub { margin: 0 0 18px; color: #5b6478; }
ul#facts { margin: 0 0 20px; padding-left: 18px; }
ul#facts li { margin: 4px 0; }
ul#facts li.ok { color: #0d7a4a; }
ul#facts li.bad { color: #b42318; }
button { font: inherit; padding: 7px 14px; border-radius: 8px; cursor: pointer;
         border: 1px solid #c8d2e4; background: #fff; }
button:hover { background: #f2f6ff; }
p.pong { margin: 14px 0 0; padding: 10px 12px; border-radius: 8px;
         background: #eef6ff; color: #1f4a8a; }
pre#cpp-result { margin: 12px 0 0; padding: 12px 14px; border-radius: 8px;
                 background: #f3f5f9; color: #23293a; white-space: pre-wrap;
                 font: 13px/1.55 ui-monospace, Consolas, monospace; }
hr { margin: 24px 0; border: 0; border-top: 1px solid #d8dfec; }
h2 { margin: 0 0 2px; font-size: 18px; }
)css");
  assets.add_js("/app.js", R"js(// ES module：能被 `<script type=module>` 加载，就说明 MIME 与虚拟 origin 都对。
async function probe() {
  const facts = document.getElementById('facts');
  const add = (ok, text) => {
    const li = document.createElement('li');
    li.className = ok ? 'ok' : 'bad';
    li.textContent = (ok ? '✓ ' : '✗ ') + text;
    facts.appendChild(li);
  };
  // ① 相对路径的子资源：样式表若加载失败，getComputedStyle 取不到我们的渐变背景。
  const bg = getComputedStyle(document.body).backgroundImage;
  add(bg && bg !== 'none', '相对路径 CSS 生效');
  // ② 同源 fetch：这是"内嵌包是一个真 origin"的关键证据。
  try {
    const response = await fetch('./data.json');
    const data = await response.json();
    add(data.ok === true, '同源 fetch 拿到 ' + JSON.stringify(data.values));
  } catch (error) {
    add(false, '同源 fetch 失败：' + error);
  }
  // ③ ES module 本身就是本文件（能执行到这里即通过）。
  add(true, 'ES module 加载并执行');
  // ④ 宿主发来的消息（双向通道的另一半）。
  const pong = document.getElementById('pong');
  document.getElementById('ping').addEventListener('click', () => {
    window.chrome.webview.postMessage('hello-from-page');
    pong.textContent = '已向宿主发送 hello-from-page';
  });
  if (window.chrome && window.chrome.webview) {
    window.chrome.webview.addEventListener('message', (event) => {
      pong.textContent = '收到宿主消息：' + event.data;
      // 消息可能是**回环响应**（字段 `st_reply_to`）：交给桥接层清算。
      if (window.stBridge) window.stBridge._receive(event.data);
    });
    // 页面就绪后主动报一次，宿主侧据此把 ready 置真（异步结果的唯一可靠路径）。
    window.chrome.webview.postMessage('page-ready');
  } else {
    add(false, '宿主消息通道不可用');
  }

  // ════════════════════════════════════════════════════════════════════
  // JS 调用 C++：请求/响应回环（宿主函数名就是这个桥的名字）
  // ════════════════════════════════════════════════════════════════════
  //
  // 页面侧写起来就像调本地异步函数：
  //     const reply = await window.stBridge.call('greet', { name: 'WebView' });
  //
  // 协议（两端都只认这三个字段）：
  //     请求  {st_call:"greet", args:{...}, st_id:"<唯一>"}
  //     响应  {st_reply_to:"<同一个 st_id>", ok:true, value:{...}}
  //     出错  {st_reply_to:"<同一个 st_id>", ok:false, error:"<中文原因>"}
  //
  // 为何要把 st_id 传过去：页面可以并发发多个请求，宿主可以乱序回；
  // 没有 id 就只能靠“谁先回来先算哪个”，那在真实界面里是定时炸弹。
  const pending = new Map();
  let next_id = 1;
  window.stBridge = {
    call(name, args = {}) {
      const id = 'c' + (next_id++);
      return new Promise((resolve, reject) => {
        pending.set(id, { resolve, reject });
        const post = (payload) => window.chrome.webview.postMessage(JSON.stringify(payload));
        post({ st_call: name, args, st_id: id });
      });
    },
    _receive(text) {
      let msg;
      try { msg = JSON.parse(text); } catch { return; }
      if (!msg || !msg.st_reply_to) return;
      const slot = pending.get(msg.st_reply_to);
      if (!slot) return;
      pending.delete(msg.st_reply_to);
      if (msg.ok) slot.resolve(msg.value);
      else slot.reject(new Error(msg.error || '宿主报告失败'));
    },
  };

  document.getElementById('call-cpp').addEventListener('click', async () => {
    const out = document.getElementById('cpp-result');
    out.textContent = '调用中…';
    try {
      const reply = await window.stBridge.call('greet', { name: 'WebView' });
      out.textContent = JSON.stringify(reply, null, 2);
    } catch (error) {
      out.textContent = '调用失败：' + error.message;
    }
  });
}
probe();
)js");
  assets.add_json("/data.json", R"json({"ok":true,"values":[1,2,3],"note":"embedded"})json");
  return assets;
}

}  // namespace

auto build_webview_page() -> std::unique_ptr<Element> {
  auto page = make_page(
      "webview", "嵌入浏览器",
      "可编程控制的网页区域：URL / 内嵌资源包 / 单文件 HTML；不进 framebuffer");

  // ⚠ 与「原生嵌入」页同一个坑：页面在 `app.start()` **之前**装配，那时页面对象还没建，
  // `supported()` 必然 false。所以按钮恒可点，真实结果由状态行如实报出。

  auto about = make_card("card-wv-about", "它是什么");
  auto about_text = std::make_unique<Text>(
      "自己拥有一个页面对象（Windows 上为 WebView2）：可以导航、执行 JS、收发消息、缩放。"
      "与「原生嵌入」不同——那个只能看别人窗口，这个能真正控制。");
  about_text->set_tone(Tone::Muted);
  about->add_child(std::move(about_text));
  about->add_child(std::make_unique<KeyValueRow>("两种内容源", "URL / 内嵌资源包（虚拟 origin）"));
  about->add_child(
      std::make_unique<KeyValueRow>("像素可见性", "由平台合成器上屏，capture 拍不到"));
  about->add_child(std::make_unique<KeyValueRow>(
      "判据", "只能读属性面（attached / ready / url / title / rasterization_scale）"));
  page->add_child(std::move(about));

  // —— 浏览器区 ——
  auto view_card = make_card("card-wv-view", "浏览器区");
  auto view = std::make_unique<WebView>();
  view->set_id("webview-demo");
  view->set_preferred_size({0.0f, 420.0f});
  view->set_placeholder_text("点「加载内嵌站点」或「打开网址」");
  WebView* view_ptr = view.get();
  view_card->add_child(std::move(view));
  page->add_child(std::move(view_card));

  // —— 自检读数 ——
  auto probe_card = make_card("card-wv-probe", "自检读数");
  const auto make_probe_row = [&probe_card](std::string_view label) -> Text* {
    auto row = make_row();
    auto name = std::make_unique<Text>(std::string(label));
    name->set_tone(Tone::Muted);
    name->set_scale(Text::Scale::Xs);
    auto value = std::make_unique<Text>("—");
    value->set_scale(Text::Scale::Xs);
    Text* value_ptr = value.get();
    row->add_child(std::move(name));
    row->add_child(std::move(value));
    probe_card->add_child(std::move(row));
    return value_ptr;
  };
  Text* status_ptr = make_probe_row("状态");
  Text* ready_ptr = make_probe_row("内容就绪");
  Text* url_ptr = make_probe_row("地址");
  Text* title_ptr = make_probe_row("标题");
  Text* scale_ptr = make_probe_row("DPI 栅格化");
  Text* source_ptr = make_probe_row("内容源");
  page->add_child(std::move(probe_card));

  // 把组件的属性面读出来贴到界面上——这是**唯一**能看见"网页真的在跑"的通道。
  //
  // 为什么读属性面而不是读组件内部状态：属性面就是控制通道 `get` 的同一份数据，
  // 于是"界面上看到的"与"自动化读到的"永远不会分家（两处各读一份是本仓反复踩过的坑）。
  const auto refresh = [view_ptr, status_ptr, ready_ptr, url_ptr, title_ptr, scale_ptr,
                        source_ptr]() {
    const auto read = [view_ptr](std::string_view name) {
      return view_ptr->get_property(name).value_or(std::string{"—"});
    };
    status_ptr->set_content(read("status"));
    ready_ptr->set_content(read("ready"));
    url_ptr->set_content(read("url"));
    title_ptr->set_content(read("title"));
    scale_ptr->set_content(read("rasterization_scale"));
    source_ptr->set_content(read("source"));
  };

  // 事件驱动刷新：页面自己回推 `page-ready` 时把读数刷一遍（不靠猜延时）。
  //
  // 同时这里就是**宿主侧的请求分发**：页面发来的 `{st_call, args, st_id}` 在这里
  // 找到对应 C++ 函数、执行、把结果用同一个 `st_id` 回推。
  // 组件不认识这些字段——它只管“收到一条字符串消息”，语义完全在宿主层。
  view_ptr->on_event = [view_ptr, refresh, status_ptr](const st::ui::WebViewEvent& event) {
    refresh();
    if (event.kind != st::ui::WebViewEvent::Kind::WebMessageReceived) return;

    const st::Result<st::Json> parsed = st::json_parse(event.message);
    if (!parsed.has_value() || !parsed->is_object()) {
      // 不是 JSON（例如演示页的 `page-ready`）：当普通消息处理。
      status_ptr->set_content("页面消息：" + event.message);
      return;
    }
    const st::Json& request = *parsed;
    const std::string call = st::json_get_string(request, "st_call");
    const std::string id = st::json_get_string(request, "st_id");
    if (call.empty() || id.empty()) {
      status_ptr->set_content("页面消息（非宿主调用）：" + event.message);
      return;
    }

    st::Json reply = st::Json::object();
    reply["st_reply_to"] = id;
    if (call == "greet") {
      const st::Json* args = st::json_find(request, "args");
      reply["ok"] = true;
      reply["value"] = host_greet(args != nullptr ? *args : st::Json::object());
    } else {
      // 未知函数要如实报错，不能静默空转（否则页面那边永远 pending）。
      reply["ok"] = false;
      reply["error"] = "宿主没有这个函数：" + call;
    }
    (void)view_ptr->post_message(st::json_dump(reply));
    status_ptr->set_content("已处理宿主调用：" + call);
  };

  // —— 操作 ——
  auto url_row = make_row();
  auto url_input = std::make_unique<Input>();
  url_input->set_id("wv-url");
  url_input->set_placeholder("https://example.com");
  url_input->style().grow = 1.0f;
  Input* url_ptr_input = url_input.get();
  auto open_button = std::make_unique<Button>("打开网址", Button::Variant::Primary);
  open_button->set_id("btn-wv-open");
  Button* open_ptr = open_button.get();
  url_row->add_child(std::move(url_input));
  url_row->add_child(std::move(open_button));
  page->add_child(std::move(url_row));

  auto actions = make_row();
  auto embedded_button = std::make_unique<Button>("加载内嵌站点", Button::Variant::Primary);
  embedded_button->set_id("btn-wv-embedded");
  auto html_button = std::make_unique<Button>("加载单文件 HTML", Button::Variant::Ghost);
  html_button->set_id("btn-wv-html");
  auto exec_button = std::make_unique<Button>("执行 JS", Button::Variant::Ghost);
  exec_button->set_id("btn-wv-exec");
  auto back_button = std::make_unique<Button>("后退", Button::Variant::Ghost);
  back_button->set_id("btn-wv-back");
  Button* embedded_ptr = embedded_button.get();
  Button* html_ptr = html_button.get();
  Button* exec_ptr = exec_button.get();
  Button* back_ptr = back_button.get();
  actions->add_child(std::move(embedded_button));
  actions->add_child(std::move(html_button));
  actions->add_child(std::move(exec_button));
  actions->add_child(std::move(back_button));
  page->add_child(std::move(actions));

  auto exec_row = make_row();
  auto exec_input = std::make_unique<Input>();
  exec_input->set_id("wv-script");
  exec_input->set_text("document.title");
  exec_input->style().grow = 1.0f;
  Input* exec_input_ptr = exec_input.get();
  auto exec_result = std::make_unique<Text>("结果：—");
  exec_result->set_scale(Text::Scale::Xs);
  Text* exec_result_ptr = exec_result.get();
  exec_row->add_child(std::move(exec_input));
  exec_row->add_child(std::move(exec_result));
  page->add_child(std::move(exec_row));

  open_ptr->on_click = [view_ptr, url_ptr_input, refresh]() {
    const std::string url = url_ptr_input->value();
    if (url.empty()) return;
    (void)view_ptr->navigate(url);
    refresh();
  };
  embedded_ptr->on_click = [view_ptr, refresh]() {
    const WebViewAssets assets = demo_site();
    (void)view_ptr->load_embedded("/index.html", assets);
    refresh();
  };
  html_ptr->on_click = [view_ptr, refresh]() {
    (void)view_ptr->load_html(
        "<!doctype html><meta charset=utf-8><title>单文件</title>"
        "<body style=\"margin:0;font:15px/1.6 system-ui;padding:32px;"
        "background:#fff8e7;color:#463214\">"
        "<h1>单文件 HTML</h1>"
        "<p>这条路<b>没有 origin</b>：相对路径的子资源不可用。"
        "需要相对路径/fetch/module 时用「加载内嵌站点」。</p></body>");
    refresh();
  };
  exec_ptr->on_click = [view_ptr, exec_input_ptr, exec_result_ptr, refresh]() {
    std::string result;
    if (view_ptr->execute_script(exec_input_ptr->value(), result)) {
      exec_result_ptr->set_content("结果：" + (result.empty() ? std::string{"(undefined)"} : result));
    } else {
      exec_result_ptr->set_content("执行失败（见状态行）");
    }
    refresh();
  };
  back_ptr->on_click = [view_ptr, refresh]() {
    (void)view_ptr->go_back();
    refresh();
  };

  // —— 已知边界 ——
  auto limits = make_card("card-wv-limits", "已知边界");
  limits->add_child(std::make_unique<KeyValueRow>(
      "平台", "Windows（WebView2）；X11 / Wayland 未实现，如实报 Unsupported"));
  limits->add_child(
      std::make_unique<KeyValueRow>("截图", "capture / visual_diff 拍不到嵌入内容"));
  limits->add_child(std::make_unique<KeyValueRow>(
      "异步脚本", "ExecuteScript 不解析 Promise；异步结果靠页面 postMessage 回推"));
  limits->add_child(std::make_unique<KeyValueRow>(
      "单文件 HTML", "没有 origin：相对路径子资源、fetch、ES module 都不行"));
  page->add_child(std::move(limits));

  return page;
}

}  // namespace gallery

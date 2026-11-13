#include "st/ui/components/webview.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

#include "st/raster/paint.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"

#include "components_internal.hpp"

namespace st::ui {

using components_internal::text_port_of;

namespace {

/// 浮点矩形比较（判断"要不要真下发一次布局"）。
///
/// 与 `NativeHost` 同一理由：每帧都可能进 `arrange`，但真正变了的次数很少；
/// 每次都下发会让嵌入内容持续重排。
[[nodiscard]] auto rect_equals(math::Rect a, math::Rect b) noexcept -> bool {
  constexpr float kEpsilon = 0.01f;
  return std::abs(a.x - b.x) < kEpsilon && std::abs(a.y - b.y) < kEpsilon &&
         std::abs(a.width - b.width) < kEpsilon && std::abs(a.height - b.height) < kEpsilon;
}

/// 按扩展名推 MIME（与 `shell::webview::WebAssets::guess_mime` **同一口径**的镜像）。
///
/// 为何要在组件侧也有一份：`ui` 不能包含 `shell`（依赖方向），而"注册资源时 mime 留空"
/// 是常见用法——留空就必须有人来推。两份实现的一致性由
/// `webview_assets_guess_mime_matches_shell` 钉住（口径分家是这类缺陷的典型形状）。
[[nodiscard]] auto mime_of(std::string_view path) -> std::string {
  const std::size_t dot = path.rfind('.');
  if (dot == std::string_view::npos) return "application/octet-stream";
  std::string ext;
  for (const char ch : path.substr(dot + 1)) {
    ext.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  if (ext == "html" || ext == "htm") return "text/html";
  if (ext == "css") return "text/css";
  if (ext == "js" || ext == "mjs") return "text/javascript";
  if (ext == "json" || ext == "map") return "application/json";
  if (ext == "wasm") return "application/wasm";
  if (ext == "svg") return "image/svg+xml";
  if (ext == "png") return "image/png";
  if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
  if (ext == "gif") return "image/gif";
  if (ext == "webp") return "image/webp";
  if (ext == "ico") return "image/x-icon";
  if (ext == "woff") return "font/woff";
  if (ext == "woff2") return "font/woff2";
  if (ext == "ttf") return "font/ttf";
  if (ext == "otf") return "font/otf";
  if (ext == "txt") return "text/plain";
  if (ext == "xml") return "application/xml";
  return "application/octet-stream";
}

}  // namespace

// ── WebViewAssets ───────────────────────────────────────────────────────────

void WebViewAssets::add(std::string path, std::string_view body, std::string mime) {
  WebViewAsset asset;
  asset.path = std::move(path);
  asset.mime = mime.empty() ? mime_of(asset.path) : std::move(mime);
  asset.bytes.assign(body.begin(), body.end());
  assets_.push_back(std::move(asset));
}

void WebViewAssets::add_html(std::string path, std::string_view body) {
  add(std::move(path), body, "text/html");
}

void WebViewAssets::add_css(std::string path, std::string_view body) {
  add(std::move(path), body, "text/css");
}

void WebViewAssets::add_js(std::string path, std::string_view body) {
  // `text/javascript` 是 ES module 能被接受的那一档；`text/plain` 会让
  // `<script type=module>` 直接被拒——而这个错误在控制台里只报一句含糊的 MIME 错误。
  add(std::move(path), body, "text/javascript");
}

void WebViewAssets::add_json(std::string path, std::string_view body) {
  add(std::move(path), body, "application/json");
}

void WebViewAssets::add_bytes(std::string path, std::vector<std::uint8_t> bytes, std::string mime) {
  WebViewAsset asset;
  asset.path = std::move(path);
  asset.mime = mime.empty() ? mime_of(asset.path) : std::move(mime);
  asset.bytes = std::move(bytes);
  assets_.push_back(std::move(asset));
}

// ── WebView ─────────────────────────────────────────────────────────────────

WebView::WebView() {
  // 内容型组件：不参与焦点（键盘事件应当继续落在界面自己的控件上）。
  style_.background = math::Color{0, 0, 0, 0};
}

WebView::~WebView() {
  // 页面对象由宿主拥有；组件析构时请求销毁它——否则它会挂在一个正在消失的
  // 窗口下，或（更糟）让浏览器进程留到进程退出。
  if (WebViewPort* owner = port(); owner != nullptr) owner->webview_destroy();
}

auto WebView::port() const -> WebViewPort* {
  const HostFocus* owner = host();
  if (owner == nullptr) return nullptr;
  return owner->host_webview_port();
}

auto WebView::supported() const -> bool {
  const WebViewPort* owner = port();
  return owner != nullptr && owner->webview_supported();
}

void WebView::set_preferred_size(math::Size size) {
  preferred_ = size;
  mark_dirty();
}

void WebView::set_placeholder_visible(bool visible) {
  if (placeholder_ == visible) return;
  placeholder_ = visible;
  mark_dirty();
}

void WebView::set_placeholder_text(std::string text) {
  if (placeholder_text_ == text) return;
  placeholder_text_ = std::move(text);
  mark_dirty();
}

void WebView::set_status(std::string text) {
  status_ = std::move(text);
  mark_dirty();
}

auto WebView::state() const -> WebViewState {
  const WebViewPort* owner = port();
  if (owner == nullptr) return {};
  return owner->webview_state();
}

auto WebView::status_text() const -> std::string {
  const WebViewPort* owner = port();
  if (owner == nullptr) return "宿主没有浏览器能力（无头 / 非 Windows）";
  if (!owner->webview_supported()) {
    const std::string reason = owner->webview_unsupported_reason();
    return reason.empty() ? "本机不支持嵌入浏览器" : reason;
  }
  return status_;
}

auto WebView::ensure_page() -> bool {
  if (page_requested_) return true;
  WebViewPort* owner = port();
  if (owner == nullptr) return false;
  // **支持性检查放在这里，而不是靠调用方**：
  //
  // 一个端口存在但自报不支持（没装 Runtime / 非 Windows）时，任何建页面/导航都是
  // 白跑一次——而白跑的代价不只是开销：它会把一个“本机根本做不了”的动作变成
  // “看着在做事、实际什么都不会发生”（测试实测到过：断言 `stub.navigated.empty()` 红）。
  if (!owner->webview_supported()) return false;
  // 尺寸未知时给一个合理初值：建页面必须有矩形，而 0×0 会让 WebView2 建出一块
  // “看不见的空”（且不报错——那种失败最难查）。
  const math::Rect target = bounds_.is_empty()
                                ? math::Rect{0.0f, 0.0f,
                                             preferred_.width > 0.0F ? preferred_.width : 640.0f,
                                             preferred_.height > 0.0F ? preferred_.height
                                                                      : kDefaultHeight}
                                : bounds_;
  page_requested_ = true;
  const Status created = owner->webview_create(target);
  if (!created) {
    // **不假装成功**：把原因留在状态里，占位块会显示它。
    page_requested_ = false;
    set_status(created.error().message);
    return false;
  }
  set_status("页面容器已创建");
  sync_bounds();
  return true;
}

void WebView::sync_bounds() {
  WebViewPort* owner = port();
  if (owner == nullptr) return;
  const math::Rect clip = visible_rect();
  const bool changed = !has_synced_ || !rect_equals(last_synced_, bounds_) ||
                       !rect_equals(last_clip_, clip);
  if (!changed) return;
  owner->webview_sync(bounds_, clip);
  last_synced_ = bounds_;
  last_clip_ = clip;
  has_synced_ = true;
}

auto WebView::navigate(std::string_view url) -> bool {
  WebViewPort* owner = port();
  if (owner == nullptr) {
    set_status("宿主没有浏览器能力（无头 / 非 Windows）");
    return false;
  }
  if (!ensure_page()) {
    set_status(owner->webview_supported() ? status_ : owner->webview_unsupported_reason());
    return false;
  }
  const Status done = owner->webview_navigate(url);
  if (!done) {
    set_status(done.error().message);
    return false;
  }
  set_status(std::format("正在加载 {}", url));
  return true;
}

auto WebView::load_html(std::string_view html) -> bool {
  WebViewPort* owner = port();
  if (owner == nullptr) {
    set_status("宿主没有浏览器能力（无头 / 非 Windows）");
    return false;
  }
  if (!ensure_page()) {
    set_status(owner->webview_supported() ? status_ : owner->webview_unsupported_reason());
    return false;
  }
  const Status done = owner->webview_load_html(html);
  if (!done) {
    set_status(done.error().message);
    return false;
  }
  set_status("正在加载内联 HTML");
  return true;
}

auto WebView::load_embedded(std::string_view entry, const WebViewAssets& assets) -> bool {
  WebViewPort* owner = port();
  if (owner == nullptr) {
    set_status("宿主没有浏览器能力（无头 / 非 Windows）");
    return false;
  }
  if (assets.empty()) {
    set_status("内嵌资源表为空");
    return false;
  }
  if (!ensure_page()) {
    set_status(owner->webview_supported() ? status_ : owner->webview_unsupported_reason());
    return false;
  }
  const Status done = owner->webview_load_embedded(entry, assets.list());
  if (!done) {
    set_status(done.error().message);
    return false;
  }
  set_status(std::format("正在加载内嵌资源 {}（共 {} 条）", entry, assets.size()));
  return true;
}

auto WebView::reload() -> bool {
  WebViewPort* owner = port();
  if (owner == nullptr) return false;
  const Status done = owner->webview_reload();
  if (!done) {
    set_status(done.error().message);
    return false;
  }
  return true;
}

auto WebView::stop() -> bool {
  WebViewPort* owner = port();
  if (owner == nullptr) return false;
  const Status done = owner->webview_stop();
  if (!done) {
    set_status(done.error().message);
    return false;
  }
  return true;
}

auto WebView::go_back() -> bool {
  WebViewPort* owner = port();
  if (owner == nullptr) return false;
  const Status done = owner->webview_go_back();
  if (!done) {
    set_status(done.error().message);
    return false;
  }
  return true;
}

auto WebView::go_forward() -> bool {
  WebViewPort* owner = port();
  if (owner == nullptr) return false;
  const Status done = owner->webview_go_forward();
  if (!done) {
    set_status(done.error().message);
    return false;
  }
  return true;
}

auto WebView::execute_script(std::string_view script, std::string& result) -> bool {
  WebViewPort* owner = port();
  if (owner == nullptr) {
    set_status("宿主没有浏览器能力（无头 / 非 Windows）");
    return false;
  }
  const Status done = owner->webview_execute_script(script, result);
  if (!done) {
    set_status(done.error().message);
    return false;
  }
  last_result_ = result;
  mark_dirty();
  return true;
}

auto WebView::post_message(std::string_view text) -> bool {
  WebViewPort* owner = port();
  if (owner == nullptr) return false;
  const Status done = owner->webview_post_message(text);
  if (!done) {
    set_status(done.error().message);
    return false;
  }
  return true;
}

auto WebView::set_zoom(double factor) -> bool {
  WebViewPort* owner = port();
  if (owner == nullptr) return false;
  const Status done = owner->webview_set_zoom(factor);
  if (!done) {
    set_status(done.error().message);
    return false;
  }
  return true;
}

auto WebView::set_script_enabled(bool enabled) -> bool {
  WebViewPort* owner = port();
  if (owner == nullptr) return false;
  const Status done = owner->webview_set_script_enabled(enabled);
  if (!done) {
    set_status(done.error().message);
    return false;
  }
  return true;
}

void WebView::handle_webview_event(const WebViewEvent& event) {
  switch (event.kind) {
    case WebViewEvent::Kind::NavigationStarting:
      set_status(std::format("正在加载 {}", event.url));
      break;
    case WebViewEvent::Kind::NavigationCompleted:
      set_status(event.succeeded ? "已加载" : std::format("加载失败：{}", event.detail));
      break;
    case WebViewEvent::Kind::TitleChanged:
      // 标题变化不改 `status_`（那是"动作反馈"；标题是页面自己的能力）。
      break;
    case WebViewEvent::Kind::WebMessageReceived:
      // 页面主动调宿主：只在状态栏留一条短痕迹，完整内容交给 `on_event` 订阅者。
      set_status(std::format("收到页面消息（{} 字节）", event.message.size()));
      break;
    case WebViewEvent::Kind::ProcessFailed:
      set_status(std::format("渲染进程失败（{}）——需要重新加载", event.detail));
      break;
    case WebViewEvent::Kind::RasterizationScaleChanged:
      // DPI 变了：宿主已自行重下发，这里只需重绘（读数在属性面）。
      break;
  }
  if (on_event) on_event(event);
  mark_dirty();
}

void WebView::measure(const RenderContext& context, const Constraints& constraints) {
  (void)context;
  const float width = preferred_.width > 0.0f
                          ? preferred_.width
                          : (constraints.max_width > 0.0f ? constraints.max_width : 320.0f);
  const float height = preferred_.height > 0.0f ? preferred_.height : kDefaultHeight;
  measured_ = math::Size{width, height};
}

void WebView::arrange(const RenderContext& context, math::Rect rect) {
  (void)context;
  bounds_ = rect;
  // 页面对象在**首次真正布局**时创建：那时才有确定的矩形，而建页面必须有矩形。
  (void)ensure_page();
  sync_bounds();
}

void WebView::on_visibility_changed() {
  // 隐藏子树不会走 `arrange`，所以"被藏起来"只能从这里得知。
  // 不收尾的后果与 `NativeHost` 实测过的一样：切到别的页后网页仍盖在新页面上。
  sync_bounds();
}

void WebView::apply_theme(const Theme& theme) {
  placeholder_fill_ = theme.colors().surface_sunken;
  mark_dirty();
}

auto WebView::readonly_property_names() const -> std::vector<std::string_view> {
  auto names = Element::readonly_property_names();
  for (const std::string_view each :
       {"attached", "supported", "shown", "ready", "loading", "url", "title", "source",
        "can_back", "can_forward", "zoom", "rasterization_scale", "assets", "status",
        "last_result", "unsupported_reason", "placeholder_visible"}) {
    names.push_back(each);
  }
  return names;
}

auto WebView::get_property(std::string_view name) const -> std::optional<std::string> {
  if (auto inherited = Element::get_property(name); inherited.has_value()) return inherited;
  if (name == "placeholder_visible") return placeholder_ ? "true" : "false";
  if (name == "supported") return supported() ? "true" : "false";
  if (name == "unsupported_reason") {
    const WebViewPort* owner = port();
    return owner == nullptr ? "" : owner->webview_unsupported_reason();
  }
  if (name == "status") return status_;
  if (name == "last_result") return last_result_;
  const WebViewState now = state();
  if (name == "attached") return now.attached ? "true" : "false";
  // `shown`（真实可见）与 `attached`（已建）必须分开报：切页/滚出视口时它仍建着
  // 但看不见，只报 `attached` 会让自动化以为"它是好的"。
  if (name == "shown") return (now.attached && now.visible) ? "true" : "false";
  if (name == "ready") return now.ready ? "true" : "false";
  if (name == "loading") return now.loading ? "true" : "false";
  if (name == "url") return now.url;
  if (name == "title") return now.title;
  if (name == "source") return std::string(to_string(now.source));
  if (name == "can_back") return now.can_go_back ? "true" : "false";
  if (name == "can_forward") return now.can_go_forward ? "true" : "false";
  if (name == "zoom") return std::format("{:.3f}", now.zoom);
  if (name == "rasterization_scale") return std::format("{:.3f}", now.rasterization_scale);
  if (name == "assets") return std::to_string(now.asset_count);
  return std::nullopt;
}

auto WebView::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "navigate") return navigate(argument);
  if (action == "load_html") return load_html(argument);
  if (action == "reload") return reload();
  if (action == "stop") return stop();
  if (action == "back") return go_back();
  if (action == "forward") return go_forward();
  if (action == "post") return post_message(argument);
  if (action == "zoom") {
    // 参数解析失败要如实报（`std::stod` 会抛，本仓禁异常作控制流）。
    try {
      const double factor = std::stod(std::string(argument));
      return set_zoom(factor);
    } catch (...) {
      set_status(std::format("zoom 参数不是数字：{}", argument));
      return false;
    }
  }
  if (action == "set_script") {
    const bool enable = argument == "true" || argument == "1";
    return set_script_enabled(enable);
  }
  if (action == "exec") {
    std::string result;
    if (!execute_script(argument, result)) return false;
    set_status(std::format("脚本结果：{}", result));
    return true;
  }
  return Element::invoke_action(action, argument);
}

void WebView::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  // 已就绪且可见：这块区域的像素由平台合成器画（我们什么都不画）。
  //
  // 为什么不画底：画了会被页面盖住，纯属浪费；更关键的是——若哪天页面没显示出来，
  // 留空能立刻看出"这层没有像素"，而画一层"看起来正常"的底色会掩盖问题。
  const WebViewState now = state();
  if (now.attached && now.visible) return;

  if (!placeholder_) return;
  const math::Rect box = bounds_.inset(style_.padding);
  if (box.is_empty()) return;

  const math::Color fill =
      placeholder_fill_.a > 0 ? placeholder_fill_ : context.theme.colors().surface_sunken;
  canvas.fill_rect(box, raster::Paint::solid(fill), style_.radius);

  // 两行文字：标题（这段该是什么）+ 状态（现在为什么不是）。
  //
  // 为何必须显示状态：不可用的原因分很多种（没装 Runtime / 非 Windows / 入口资源缺失 /
  // 页面还在加载），而它们对用户意味着完全不同的动作。只写"网页不可用"等于没说。
  const TextPort& port_ref = text_port_of(context);
  const float font_size = style_.font_size > 0.0f ? style_.font_size : 13.0f;
  const std::string line1 = placeholder_text_;
  const std::string line2 = status_text();
  const float line_height = font_size * 1.6f;
  const float total = line_height * 2.0f;
  const float start_y = box.y + std::max(0.0f, (box.height - total) * 0.5f);

  const auto draw_centered = [&](const std::string& text, float y, math::Color color,
                                 float size) {
    if (text.empty()) return;
    const float width = port_ref.measure_width(text, size);
    const float x = box.x + std::max(0.0f, (box.width - width) * 0.5f);
    port_ref.draw(canvas, text, math::Point{x, y}, size, color);
  };
  draw_centered(line1, start_y, context.theme.colors().text_muted, font_size);
  draw_centered(line2, start_y + line_height, context.theme.colors().text_muted, font_size * 0.9f);
}

}  // namespace st::ui

// ── 组件登记 ──
ST_UI_COMPONENT_IMPL(::st::ui::WebView);

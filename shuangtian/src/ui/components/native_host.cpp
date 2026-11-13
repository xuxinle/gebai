#include "st/ui/components/native_host.hpp"

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

/// 浮点矩形比较（判断"要不要真的下发一次 SetWindowPos"）。
///
/// 为什么要比较而不是每帧都下发：`SetWindowPos` 会触发外部窗口重绘，而外部窗口
/// （浏览器）重绘不是免费的——每帧无谓地摆一次会让嵌入内容持续闪。布局每帧都会
/// 调 `arrange`，但真正变了的次数很少，所以这里做"值没变就跳过"。
[[nodiscard]] auto rect_equals(math::Rect a, math::Rect b) noexcept -> bool {
  constexpr float kEpsilon = 0.01f;
  return std::abs(a.x - b.x) < kEpsilon && std::abs(a.y - b.y) < kEpsilon &&
         std::abs(a.width - b.width) < kEpsilon && std::abs(a.height - b.height) < kEpsilon;
}

}  // namespace

NativeHost::NativeHost() {
  // 内容型组件：不参与焦点（键盘事件应当继续落在界面自己的控件上，
  // 而不是被一个"容器"截走）。
  style_.background = math::Color{0, 0, 0, 0};
}

NativeHost::~NativeHost() {
  // 析构必须解除嵌入：否则外部窗口会挂在一个正在消失的窗口下，
  // 变成画不出来、也点不到的孤儿（或被父窗口连带销毁）。
  detach();  // lint-allow: L9 本类方法（解除嵌入），非 std::thread::detach
}

void NativeHost::set_preferred_size(math::Size size) {
  preferred_ = size;
  mark_dirty();
}

void NativeHost::set_placeholder_visible(bool visible) {
  if (placeholder_ == visible) return;
  placeholder_ = visible;
  mark_dirty();
}

void NativeHost::set_placeholder_text(std::string text) {
  if (placeholder_text_ == text) return;
  placeholder_text_ = std::move(text);
  mark_dirty();
}

auto NativeHost::probe() const -> Element::ChildViewProbe {
  const HostFocus* owner = host();
  if (owner == nullptr) return {};
  return owner->host_child_view_probe();
}

auto NativeHost::child_view_probe() const -> Element::ChildViewProbe { return probe(); }

auto NativeHost::attached() const -> bool {
  // 读**宿主侧真实状态**而不是本地 `child_ != nullptr`：
  // 外部程序退出后句柄失效，本地标志仍是"已嵌入"，只有后端读数能反映真相。
  if (child_ == nullptr) return false;
  return probe().attached;
}

auto NativeHost::attach(void* child) -> bool {
  if (child == nullptr) return false;

  HostFocus* owner = host();
  if (owner == nullptr) {
    // 没有宿主（离屏渲染 / 未上树）：如实失败，不进半嵌入状态。
    return false;
  }
  if (!owner->host_supports_child_view()) {
    // 后端不支持（无头 / 未实现平台）：同样如实失败。
    // 组件保持占位块外观，界面其余部分完全不受影响。
    return false;
  }

  // 已嵌同一个句柄：幂等（重新摆一下位置即可）
  if (child_ == child) {
    sync_bounds();
    return true;
  }
  // 已嵌别的窗口：先还原旧的（一个组件只有一个嵌入位）
  if (child_ != nullptr) detach();  // lint-allow: L9 本类方法，非线程 detach

  const math::Rect target = bounds_;
  if (!owner->host_adopt_child_view(child, target)) {
    // 嵌入失败：**不留半嵌入状态**（后端保证要么完整嵌好、要么已还原）。
    child_ = nullptr;
    has_synced_ = false;
    return false;
  }
  child_ = child;
  last_synced_ = target;
  has_synced_ = true;
  // 嵌入成功后只有**读数还没到位**才需要短收敛（真实后端下这是常态：
  // 外部窗口要异步重排）。已经就位时不要白白多跑几帧。
  if (!content_fits()) {
    refine_frames_ = kMaxRefineFrames;
    request_animation();
  }
  mark_dirty();
  return true;
}

void NativeHost::detach() {
  if (child_ == nullptr) return;
  HostFocus* owner = host();
  if (owner != nullptr) owner->host_dismiss_child_view(child_);
  child_ = nullptr;
  has_synced_ = false;
  refine_frames_ = 0;
  mark_dirty();
}

void NativeHost::sync_bounds() {
  if (child_ == nullptr) return;
  HostFocus* owner = host();
  if (owner == nullptr) return;
  if (!attached()) {
    // 外部窗口已经没了（进程退出/被关）：清理本地状态，回到占位块。
    // 不再调 dismiss——句柄已失效，让后端去碰它是无意义的（后端会安全忽略，
    // 但这里更清楚：先自己收敛，避免每帧反复尝试）。
    child_ = nullptr;
    has_synced_ = false;
    mark_dirty();
    return;
  }
  // 可见矩形：已隐藏（切页 / 任一祖先 `set_visible(false)`）或已滚出裁剪区时，
  // 必须让后端把外部窗口藏起来——它不经过 `paint` 的 clip 栈，自己不会消失
  //（实测：切到别的页后，嵌入的网页仍盖在新页面上）。
  const math::Rect clip = visible_rect();
  const bool bounds_changed = !has_synced_ || !rect_equals(last_synced_, bounds_);
  if (!bounds_changed && rect_equals(last_clip_, clip)) return;
  owner->host_sync_child_view(child_, bounds_, clip);
  last_synced_ = bounds_;
  last_clip_ = clip;
  has_synced_ = true;
  // **只在矩形变了时**进入短收敛。
  //
  // 仅可见区变化（切页恢复/滚动）只需重新摆宿主，不必重测标题栏——那时它的
  // 布局没动过，重测只会白跑几帧（并在每次滚动时都启动一轮）。
  //
  // 而尺寸变了就**必须**收敛，且**不能**改成“读一次 `content_fits()` 再决定”：
  // 刚下发新矩形时后端还没量（量出来的是旧值），那一刻读数恒为“就位”，
  // 收敛就永远起不来——实测：resize 后内容停在旧尺寸，而所有断言都绿（典型假绿）。
  if (bounds_changed) refine_frames_ = kMaxRefineFrames;
}

/// 内容是否已经落在 (0,0) 且尺寸等于宿主（“嵌入物真的就位了”的唯一判据）。
auto NativeHost::content_fits() const -> bool {
  const auto now = probe();
  if (!now.attached) return false;
  if (now.host_width <= 0.0F || now.host_height <= 0.0F) return false;   // 宿主还没建好
  return std::abs(now.content_x) < 1.0F && std::abs(now.content_y) < 1.0F &&
         std::abs(now.content_width - now.host_width) < 1.0F &&
         std::abs(now.content_height - now.host_height) < 1.0F;
}

void NativeHost::refine_bounds() {
  if (refine_frames_ <= 0 || child_ == nullptr) return;
  --refine_frames_;
  HostFocus* owner = host();
  if (owner == nullptr) return;
  const auto now = probe();
  if (!now.attached) {
    // 外部窗口没了：交给 `sync_bounds` 同一套收敛（它下一帧会清本地状态）。
    refine_frames_ = 0;
    return;
  }
  // 判据：内容是否真的落在 (0,0) 且尺寸等于宿主。满足就停。
  if (content_fits()) {
    refine_frames_ = 0;
    return;
  }
  // 还没到位：把同一块矩形再同步一次（后端会重测标题栏高度并重新落位）。
  owner->host_sync_child_view(child_, bounds_, visible_rect());
  request_animation();   // 请求下一帧继续（否则重排完成后没人再量）
}

void NativeHost::measure(const RenderContext& context, const Constraints& constraints) {
  (void)context;
  // 宽度：显式指定则用指定值，否则撑满可用宽度。
  const float width =
      preferred_.width > 0.0f ? preferred_.width
                              : (constraints.max_width > 0.0f ? constraints.max_width : 320.0f);
  // 高度：显式指定则用指定值，否则用默认高度（**不**按宽度推——嵌入内容的高度
  // 由调用方决定，猜一个比例只会让"看起来差不多但不对"）。
  const float height = preferred_.height > 0.0f ? preferred_.height : kDefaultHeight;
  measured_ = math::Size{width, height};
  // 收敛阶段：在 `measure` 里做而不是在 `arrange`——`arrange` 只在矩形真变时被
  // 调到（`sync_bounds` 有相等早退），而重排完成不需要矩形变化也能推进。
  // 每帧一次（`measure` 每帧都会跑），直到读数稳定或次数用完。
  refine_bounds();
}

void NativeHost::arrange(const RenderContext& context, math::Rect rect) {
  (void)context;
  bounds_ = rect;
  // 布局定了才同步位置：`arrange` 拿到的就是最终矩形。
  sync_bounds();
}

void NativeHost::on_visibility_changed() {
  // 隐藏子树不会走 `arrange`，所以“被藏起来”只能从这里得知。
  // 不在这里收尾的后果实测过：切到别的页后，嵌入的网页仍盖在新页面上。
  sync_bounds();
}

void NativeHost::apply_theme(const Theme& theme) {
  placeholder_fill_ = theme.colors().surface_sunken;
  mark_dirty();
}

auto NativeHost::readonly_property_names() const -> std::vector<std::string_view> {
  auto names = Element::readonly_property_names();
  for (const std::string_view each : {"attached", "shown", "title_bar_height", "content_x",
                                     "content_y", "content_width", "content_height",
                                     "host_width", "host_height", "placeholder_visible"}) {
    names.push_back(each);
  }
  return names;
}

auto NativeHost::get_property(std::string_view name) const -> std::optional<std::string> {
  if (auto inherited = Element::get_property(name); inherited.has_value()) return inherited;
  if (name == "placeholder_visible") return placeholder_ ? "true" : "false";
  // `attached` 必须与 `attached()` **同一个判据**（先看有没有句柄）：
  // 否则没嵌入时会把宿主侧的“上一次读数”误报成已嵌入——一条口径分两处写
  // 就是这个下场。
  if (name == "attached") return attached() ? "true" : "false";
  // 剩下全部是宿主侧读数：每次现读（外部窗口随时可能没），不缓存。
  const auto probe_now = probe();
  // `shown`（真实可见）与 `attached`（已嵌入）必须分开报：切页/滚出视口时
  // 它仍嵌着但看不见，只报 `attached` 会让自动化以为“它是好的”。
  if (name == "shown") return (attached() && probe_now.visible) ? "true" : "false";
  const auto number = [](float value) { return std::format("{:.1f}", value); };
  if (name == "title_bar_height") return number(probe_now.title_bar_height);
  if (name == "content_x") return number(probe_now.content_x);
  if (name == "content_y") return number(probe_now.content_y);
  if (name == "content_width") return number(probe_now.content_width);
  if (name == "content_height") return number(probe_now.content_height);
  if (name == "host_width") return number(probe_now.host_width);
  if (name == "host_height") return number(probe_now.host_height);
  return std::nullopt;
}

void NativeHost::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  // 已嵌入：这块区域的像素由平台合成器画（我们什么都不画）。
  //
  // 为什么不画底：画了会被外部窗口盖住，纯属浪费；更关键的是——
  // 若哪天外部窗口没显示出来，留空能立刻看出"这层没有像素"，
  // 而画一层"看起来正常"的底色会掩盖问题。
  if (attached()) return;

  // 未嵌入（无能力 / 未调用 attach / 外部已退出）：画占位块。
  //
  // 这是本组件最重要的一条"不可用不影响基础功能"体现：
  // 界面照常排版、主题照常生效、其余控件照常工作，这里只是如实显示
  // "原生窗口当前不可用"，而不是报错、空白或崩溃。
  if (!placeholder_) return;
  const math::Rect box = bounds_.inset(style_.padding);
  if (box.is_empty()) return;

  const math::Color fill =
      placeholder_fill_.a > 0 ? placeholder_fill_ : context.theme.colors().surface_sunken;
  canvas.fill_rect(box, raster::Paint::solid(fill), style_.radius);
  if (placeholder_text_.empty()) return;

  // 文字居中：占位块的唯一作用是"说明这里本该有东西"，所以必须看得懂。
  const TextPort& port = text_port_of(context);
  const float font_size = style_.font_size > 0.0f ? style_.font_size : 13.0f;
  const float text_width = port.measure_width(placeholder_text_, font_size);
  const float x = box.x + std::max(0.0f, (box.width - text_width) * 0.5f);
  const float y = box.y + std::max(0.0f, (box.height - font_size) * 0.5f);
  port.draw(canvas, placeholder_text_, math::Point{x, y}, font_size,
            context.theme.colors().text_muted);
}

}  // namespace st::ui

// ── 组件登记（`st/ui/component_registry.hpp`）────────────────────────
// 一行一个组件，展开后包含静态登记（本 .o 进产物即在 main 前完成注册）
// 与导入锚点（`import_components<T>()` 取它的地址，把本 .o 拉进产物）。
ST_UI_COMPONENT_IMPL(::st::ui::NativeHost);

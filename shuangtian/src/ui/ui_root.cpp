#include "st/ui/ui_root.hpp"

#include "st/ui/input_coalescer.hpp"

#include <algorithm>
#include <cmath>
#include <bit>
#include <cctype>
#include <cstdint>
#include <format>
#include <ranges>

#include "st/core/log.hpp"
#include "st/core/print.hpp"
#include "st/ui/components/feedback.hpp"   // Tooltip（共享悬停提示气泡）

namespace st::ui {

// 提示计时的**诊断计数**（文件作用域：只为“延时有没有生效”提供可读证据，
// 不参与任何业务逻辑；量级是进程内两个标量）。
//
// lint-allow: L8 这两项是**诊断计数器**（与 `log.cpp` 的进程级配置同类）：
// 它们只被读来回答“延时到底生效了没”（有一轮悬停提示延时缺陷就靠它们定位），
// 不参与任何业务判断——注入 Context 会让每个构造点都得传一对“只给调试看”的数。
static std::uint64_t g_hint_update_calls = 0;
static double g_hint_last_delta = -1.0;

auto UiRoot::hint_update_calls() const noexcept -> std::uint64_t { return g_hint_update_calls; }
auto UiRoot::hint_last_delta_ms() const noexcept -> double { return g_hint_last_delta; }

UiRoot::UiRoot() : theme_(Theme::light()) {}

UiRoot::~UiRoot() = default;

void UiRoot::wire_owner(Element& element) {
  // 把宿主契约写到整棵子树：子组件（如命令面板要“打开即拿到焦点”）需要主动
  // 调 `UiRoot::set_focus`，而焦点簿记归根所有。边走边写，幂等。
  element.set_host(this);
  for (std::size_t index = 0; index < element.child_count(); ++index) {
    if (Element* child = element.child_at(index); child != nullptr) wire_owner(*child);
  }
}

void UiRoot::set_content(std::unique_ptr<Element> content) {
  content_ = std::move(content);
  if (content_ != nullptr) {
    wire_owner(*content_);
    content_->mark_layout_dirty();
    assign_ids(*content_, "root");
  }
  focused_ = nullptr;
  hovered_ = nullptr;
  pressed_ = nullptr;
  mark_dirty_all();
}

auto UiRoot::take_content() -> std::unique_ptr<Element> {
  // 取出根内容：不清焦点/不标脏（调用方会立即重新 set_content，避免中间态闪烁）。
  // 注意：不动 `assign_ids`（元素 id 保持——重新挂回时 setId 已非空，不会被重写）。
  return std::move(content_);
}

void UiRoot::set_theme(Theme theme) {
  theme_ = std::move(theme);
  mark_dirty_all();
}

void UiRoot::set_text_port(const TextPort* port) {
  text_port_ = port;
  mark_dirty_all();
}

void UiRoot::set_viewport(math::Size size) {
  if (size.width == viewport_.width && size.height == viewport_.height) return;
  viewport_ = size;
  mark_dirty_all();
}

void UiRoot::assign_ids(Element& element, const std::string& prefix) {
  const std::string path = element.id().empty() ? prefix : element.id();
  if (element.id().empty()) element.set_id(path);
  for (std::size_t index = 0; index < element.children().size(); ++index) {
    Element* child = element.child_at(index);
    if (child == nullptr) continue;
    assign_ids(*child, std::format("{}/{}[{}]", path, child->type(), index));
  }
}

auto UiRoot::render_context() const -> RenderContext {
  RenderContext context{theme_, text_port_, time_seconds_};
  return context;
}

void UiRoot::layout(bool force) {
  if (content_ == nullptr) return;
  // **主题传播放在早退之前**（2026-10-18）。
  //
  // 为何与重排解耦：`apply_theme_tree` 原本在 `dirty_/tree_layout_dirty()`
  // 守卫**之内**，于是“主题下发”变成了“重排的副作用”——只要这一帧没人要求重排，
  // 主题就不往下走。声明显式界面会**新建元素**（命令面板/对话框都是条件声明），
  // 新元素本该在“上树后的第一次 layout”里拿到当前主题；它能不能拿到，
  // 取决于当帧恰好有没有别的东西也在要求重排。这是隐式耦合，早晚出问题。
  //
  // ⚠ 诚实记录（反向验证的结果）：排查“命令面板配色不对”时我先怀疑这里，
  // 但改完它并不足以修好那个缺陷，回退后实测面板颜色依旧正确——真正的原因是
  // `CommandPalette::apply_theme` **没把色写回 style**（见那边注释）。
  // 保留本修改的理由不是“它修了那个 bug”，而是它把一条隐式耦合改成了显式契约：
  // 代价是每个元素一次颜色赋值 + `mark_dirty`（实测 90 节点树为微秒级），
  // 换来的是“上树就一定拿到当前主题”不依赖 luck。
  const auto apply_theme_tree = [](auto&& self, Element& element, const Theme& theme) -> void {
    element.apply_theme(theme);
    for (std::size_t index = 0; index < element.children().size(); ++index) {
      self(self, *element.child_at(index), theme);
    }
  };
  apply_theme_tree(apply_theme_tree, *content_, theme_);
  for (auto& overlay : overlays_) apply_theme_tree(apply_theme_tree, *overlay, theme_);

  if (!force && !dirty_ && !tree_layout_dirty()) return;
  const RenderContext context = render_context();
  Constraints constraints;
  constraints.max_width = viewport_.width;
  constraints.max_height = viewport_.height;
  constraints.available_width = viewport_.width;
  constraints.available_height = viewport_.height;

  content_->measure(context, constraints);
  const math::Size measured = content_->measured_size();
  // 根内容按"**至少铺满视口**"排布（内容更高时保持自然高度，不外溢裁剪）：
  // 这样根页面里的 `style().grow` 子项（如撑满窗口的编辑器/滚动区）能真正拿到剩余空间；
  // 若按内容自然高度排布，`grow` 在根层就退化成无效属性（页面永远"上半截"）。
  const float width = std::max(measured.width, viewport_.width);
  const float height = std::max(measured.height, viewport_.height);
  layout_subtree(*content_, math::Rect{0.0f, 0.0f, width, height});

  float overlay_offset = 0.0f;
  for (std::size_t index = 0; index < overlays_.size(); ++index) {
    Element& overlay = *overlays_[index];
    overlay.measure(context, constraints);
    if (index < overlay_layouts_.size() &&
        overlay_layouts_[index] == OverlayLayout::FillViewport) {
      // 铺满视口：遮罩/命令面板自定位形态——组件在 arrange 里自行计算卡片矩形，
      // 不再需要调用方注入视口尺寸（每个应用重复造轮子的历史缺口）。
      layout_subtree(overlay, math::Rect{0.0f, 0.0f, viewport_.width, viewport_.height});
      continue;
    }
    const math::Size overlay_size = overlay.measured_size();
    layout_subtree(overlay,
                   math::Rect{0.0f, overlay_offset, overlay_size.width, overlay_size.height});
    overlay_offset += overlay_size.height;
  }

  dirty_ = false;
  // 重排会挪动任意兄弟（波及范围难界定）→ 保守整帧重绘（与旧行为一致）。
  //
  // **但只在几何真的变了时才整帧**：`layout()` 的进入条件是“树里有人置过 layout 脏标记”，
  // 而现实应用里这个标记**处处会亮**——文本内容变、列表项刷新、标签条同步修改点……
  // 每一次都会让整棵树重排一遍，哪怕所有元素最后都落在**完全相同的矩形**上。
  // 无条件 `pending_full_ = true` 的代价是：任何一次内容变更都退化成整帧重绘
  // （实测 gbcode 1280×800：整帧 paint **11.5 ms**，增量重绘形同虚设；
  //   控制通道上表现为写操作 p50 **15.8 ms** vs 读操作 **4.1 ms**）。96 卡片基准
  //   场景的量化对照见 `script_write_partial_vs_full_frame_cost`（局部帧中位
  //   **0.69 ms** vs 整帧 **15.0 ms**，单次 JS 写文本的差距 ~22×）。
  //
  // 判据用**几何签名**：重排前后各走一遍树，把所有元素的矩形摊平比较——90 个节点两次
  // 遍历是微秒级，换来的是一条正确的分界线：**没挑动任何东西的重排不该触发重画**。
  const auto geometry_signature = [](auto&& self, const Element& element,
                                     std::uint64_t& out) -> void {
    const math::Rect& rect = element.bounds();
    const auto mix = [&out](float value) {
      out = out * 1099511628211ULL ^
            static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(value));
    };
    mix(rect.x);
    mix(rect.y);
    mix(rect.width);
    mix(rect.height);
    for (std::size_t index = 0; index < element.child_count(); ++index) {
      if (const Element* child = element.child_at(index); child != nullptr) self(self, *child, out);
    }
  };
  constexpr std::uint64_t kSignatureSeed = 1469598103934665603ULL;
  std::uint64_t before = kSignatureSeed;
  geometry_signature(geometry_signature, *content_, before);
  layout_subtree(*content_, math::Rect{0.0f, 0.0f, width, height});
  std::uint64_t after = kSignatureSeed;
  geometry_signature(geometry_signature, *content_, after);
  // 抽开单独比对覆层：覆层重排同样只会影响它自己（保守起见一并纳入签名）
  if (after != before) pending_full_ = true;
  ++version_;
}

void UiRoot::layout_subtree(Element& element, math::Rect rect) {
  const RenderContext context = render_context();
  element.arrange(context, rect);
}

/// 绘制后扫一遍：还有元素在悬浮过渡中就请求下一帧。
///
/// 为什么要这一次遍历：过渡动画需要**连续帧**，而帧末 `clear_dirty()` 会清脏。
/// 遍历只做指针判读（元素数量级几百），相对一次绘制可以忽略；
/// 换来的是"淡入真的是淡入"而不是卡在第一格。
void UiRoot::collect_animation_requests() {
  animation_pending_ = false;
  // **悬停提示在计时中 → 必须要下一帧**。
  //
  // 为何必须在这里：提示是“悬停满 N 毫秒才弹”，而主循环在静止时不开帧
  //（`animation_pending_` 为假就等事件）。不在这里报“还要一帧”，计时就永远
  // 停在第一帧——现象是“鼠标停半天，提示怎么都不出现”（实测）。
  if (hint_target_ != nullptr && !hint_visible_) animation_pending_ = true;
  const auto walk = [this](auto&& self, Element& element) -> void {
    if (element.animation_requested()) {
      animation_pending_ = true;
      element.clear_animation_request();  // 消费：元素在下一次绘制里重新置位
      return;
    }
    for (std::size_t index = 0; index < element.children().size(); ++index) {
      self(self, *element.child_at(index));
      if (animation_pending_) return;
    }
  };
  if (content_ != nullptr) walk(walk, *content_);
  for (const auto& overlay : overlays_) {
    if (animation_pending_) break;
    if (overlay != nullptr) walk(walk, *overlay);
  }
}

void UiRoot::paint(raster::Surface& canvas) {
  layout();
  RenderContext context = render_context();
  painted_elements_ = 0;
  context.painted_elements = &painted_elements_;
  // 叠加层在内容之后绘制（浮层在景上；与 paint_frame 同一 z 序，见其注释）。
  if (content_ != nullptr) paint_subtree(context, *content_, canvas);
  paint_overlays(context, canvas);
  // ⚠ 必须在**所有绘制之后**汇总。
  //
  // 早先这一行放在内容绘制之前（插在了浮层绘制后面）——于是它看不到本帧刚产生的
  // 动画请求（`hover_animating_` 是绘制时才置位的），下一帧 `dirty_` 被清零后
  // **永不再重绘**：过渡永久冻结在当时的进度上。
  // 现象：鼠标划过某项时过渡走到 1.0，移开后开始淡出却停在 1.0 → **该项永久高亮**，
  // 侧栏看起来有两个"选中项"（实测现象）。
  collect_animation_requests();
}

/// 逐个绘制叠加层，并在**两个绘制之间**回收上一轮摘除的对象。
///
/// 为什么不能直接 `for (auto& overlay : overlays_) overlay->paint(...)`：
/// 叠加层的 `paint` **可以摘除自己**（单帧不再重绘的约定）——`Toast` 的自动消失就是
/// 在 `paint` 里置 `expired_`、`record_damage()`、调 `on_dismiss()`，而宿主的
/// `on_dismiss` 会 `remove_overlay`。若那时 `dispatching_` 为假（帧循环里就是如此），
/// `remove_overlay` 会**立即释放**该对象，而它的 `paint` 还在栈上——
/// 回调返回后继续读成员就是 use-after-free。
///
/// 这不是假想：ASan 实测（2026-10-06 主题轮，`tools/` 里的重现脚本）给出的现场是
/// `Toast::paint → on_dismiss → 宿主 lambda 读已释放的 Toast` 与
/// `paint_frame 的 overlays 循环 → Toast::paint → on_dismiss` 两条都指着同一块内存。
/// 症状是偶发 SIGSEGV（依赖编译器把窄回调优化成内联还是真调用，因而是脆弱的）。
///
/// 修法与事件派发**同源**（`dispatch` 的 `graveyard_`）：绘制期间摘除的一律进墓场，
/// 延到本轮绘制结束再统一释放。这样指针在整个绘制过程里始终有效。
void UiRoot::paint_overlays(const RenderContext& context, raster::Surface& canvas) {
  // 只对**当轮**的叠加层遍历：paint 内部新加的（如弹层链式打开）下一帧才画，
  // 与 `dispatch` 的"派发期间不改容器"同一约定。
  //
  // 游标不用 `for (auto& overlay : overlays_)`：叠加层的 `paint` 可能经
  // `remove_overlay` 改动容器（见 `remove_overlay` 的注释），范围 for 的迭代器
  // 会当场失效。按下标 + 每轮重查 `size()` 是安全的形式。
  const bool was_painting = painting_overlays_;
  painting_overlays_ = true;
  const std::size_t count = overlays_.size();
  for (std::size_t index = 0; index < count; ++index) {
    if (index >= overlays_.size()) break;  // 自己或别人把它摘了
    overlays_[index]->paint(context, canvas);
    // 本叠加层摘除了自己（或别人）：先回收墓场再继续。
    // 回收必须在**当前对象已绘制完**之后（此时它的栈帧已退，不再有人持其指针）。
    reap_overlays();
  }
  painting_overlays_ = was_painting;
  // 统一收尾：`paint` 里摘除的都延到这里才真的析构（与事件派发同源）。
  reap_overlays();
}

void UiRoot::paint_subtree(const RenderContext& context, Element& element, raster::Surface& canvas) {
  element.paint(context, canvas);
}

auto UiRoot::hit_test(math::Point point) -> Element* {
  for (auto iterator = overlays_.rbegin(); iterator != overlays_.rend(); ++iterator) {
    if (Element* hit = hit_test_subtree(**iterator, point); hit != nullptr) return hit;
  }
  if (content_ == nullptr) return nullptr;
  return hit_test_subtree(*content_, point);
}

auto UiRoot::hit_test_pointer_move(math::Point point) -> Element* {
  // 与 `hit_test` 同构，只在**浮层那一层**换成 `hit_test_pointer_move`：
  // 浮层内容自身的命中不变（子树递归仍用普通命中），只有“宿主矩形算不算命中”
  // 这一条判据按事件类型区分。见 `Element::hit_test_pointer_move` 的说明。
  for (auto iterator = overlays_.rbegin(); iterator != overlays_.rend(); ++iterator) {
    Element& overlay = **iterator;
    if (!overlay.visible() || !overlay.intercepts_input()) continue;
    if (!overlay.hit_test_pointer_move(point)) continue;
    if (Element* hit = hit_test_subtree(overlay, point); hit != nullptr) return hit;
  }
  if (content_ == nullptr) return nullptr;
  return hit_test_subtree(*content_, point);
}

auto UiRoot::hit_test_subtree(Element& element, math::Point point) -> Element* {
  if (!element.visible() || !element.intercepts_input()) return nullptr;
  // **裁剪器件的命中也要裁**：`clip_children` 为真的元素（`ScrollView` 的内层、
  // `CodeEditor` 等）把子元素画到自己的矩形外——那些像素根本不存在，自然不可点。
  //
  // 反例（实测，gbcode 的终端滚回）：滚动后滚回 `Text` 的 bounds 是
  // `y=-119.3 / height=851.3`（内容远高于视口、被排到负坐标），而它的祖先链上
  // 只有绘制裁剪——于是**点标题栏/菜单栏**时命中的是这个看不见的 `#terminal-output`。
  // 用户感受：“顶部那条点不动了、菜单打不开”。
  //
  // 这里逐层收紧：子元素先要落在当前元素的矩形内（`clip_children` 时），递归下钻时
  // 矩形自然逐层收窄——与绘制时逐层 `push_clip_rounded_rect` 同口径（只差圆角：
  // 命中用包围矩形，比像素级圆角宽松一点点，与系统控件的命中语义一致）。
  const bool clip = element.style().clip_children;
  if (clip && !element.bounds().contains(point)) return nullptr;
  // **子元素与自身的命中优先级可以倒置**（2026-10-18）。
  //
  // 默认语义是“子元素优先”（下面的循环先递归、最后才问自己）——那对绝大多数
  // 容器是对的。但存在一类控件，它自己拥有一个**与子元素几何重叠**的交互区：
  // `SplitView` 的分隔手柄就是（贴边重叠，两侧各张开半个手柄，否则会留一条
  // 露出页面底色的缝）。这种重叠区如果仍按“子元素优先”，手柄永远收不到
  // MouseDown——拖拽直接失效（实测：侧栏完全拖不动）。
  //
  // 所以给出一个显式钩子：`claims_hit_before_children(point)` 返回 true 的元素
  // 在递归子元素**之前**先拿命中。默认 false，行为与既有一致。
  if (element.claims_hit_before_children(point) && element.hit_test(point)) return &element;
  const auto children = element.children();
  for (auto iterator = children.rbegin(); iterator != children.rend(); ++iterator) {
    if (*iterator == nullptr || !(*iterator)->visible()) continue;
    if (Element* hit = hit_test_subtree(**iterator, point); hit != nullptr) return hit;
  }
  return element.hit_test(point) ? &element : nullptr;
}

auto UiRoot::register_shortcut(const std::string& key, Shortcut mods,
                               std::function<bool()> handler) -> bool {
  if (key.empty() || !handler) return false;
  std::string normalized(key);
  std::ranges::transform(normalized, normalized.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  for (auto iterator = shortcuts_.rbegin(); iterator != shortcuts_.rend(); ++iterator) {
    if (iterator->first == normalized &&
        iterator->second.mods.ctrl == mods.ctrl && iterator->second.mods.shift == mods.shift &&
        iterator->second.mods.alt == mods.alt && iterator->second.mods.meta == mods.meta) {
      iterator->second.handler = std::move(handler);  // 同键位重复注册：覆盖（不叠加）
      return true;
    }
  }
  shortcuts_.emplace_back(std::move(normalized), ShortcutEntry{mods, std::move(handler)});
  return true;
}

void UiRoot::unregister_shortcut(const std::string& key, Shortcut mods) {
  std::string normalized(key);
  std::ranges::transform(normalized, normalized.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  std::erase_if(shortcuts_, [&](const auto& entry) {
    return entry.first == normalized &&
           entry.second.mods.ctrl == mods.ctrl && entry.second.mods.shift == mods.shift &&
           entry.second.mods.alt == mods.alt && entry.second.mods.meta == mods.meta;
  });
}

auto UiRoot::shortcuts() const -> std::vector<ShortcutInfo> {
  // 值快照（只读枚举面）：见 ui_root.hpp 的 ShortcutInfo 说明。
  //
  // 为何不返回 span/引用：①枚举面不得交出可变视图（硬约束：只读不可变）；
  // ②注册表随时可能被 register/unregister 改动，调用方拿着引用就是悬垂窗口。
  // 快照的成本是一次 vector 拷贝（帮助卡只在开卡/对账时调，非热路径）。
  std::vector<ShortcutInfo> out;
  out.reserve(shortcuts_.size());
  for (const auto& entry : shortcuts_) {
    // 存偅形状是 pair<string, ShortcutEntry>（first=登记名）；快照里把
    // Shortcut 的 key 字段同步为登记名（mods.key 原本就存注册时传的值，
    // 与登记名同源——但历史注册点存在两者不同字的写法，如 gbcode 的
    // "alt-up"（登记名）vs "ArrowUp"（mods.key）：枚举面以登记名为准，
    // 它才是派发比对真正用的那个）。
    ShortcutInfo info;
    info.name = entry.first;
    info.mods = entry.second.mods;
    info.mods.key = entry.first;
    out.push_back(std::move(info));
  }
  return out;
}

auto UiRoot::dispatch_to(Element& element, Event& event) -> bool {
  const RenderContext context = render_context();
  bool handled = false;
  for (Element* current = &element; current != nullptr; current = current->parent()) {
    if (current->on_event(context, event)) {
      handled = true;
      break;
    }
    // 行为注入的 handler 在**组件自身未消费之后**给机会（免子类化的小交互）。
    // 放在这一层而不是让每个组件覆写自己调：覆写点有二十多个，漏一个就是
    // “设了处理器、无报错、无效果”的静默失效（实测：`Input` 的 ↑↓ 历史就是这样丢的）。
    if (current->invoke_event_handler(event)) {
      handled = true;
      break;
    }
  }
  // 脚本桥：在**元素自身处理之后**通知观察者（各分支都经此函数，命中元素即 `element`）。
  //
  // 顺序很关键：若在 C++ 处理**之前**通知，脚本写入会被随后的 C++ 处理器覆盖，
  // 表现为"用 JS 改了界面却没生效"（实测踩过：语言标签点击后状态栏仍是 C++ 写的文案）。
  // 放在之后 = 脚本看到的是处理后的状态，且它的写入是最终态。
  if (event_observer_) event_observer_(event, element);
  return handled;
}

void UiRoot::update_hover(Element* target) {
  if (hovered_ == target) return;
  prune_stale_pointers();  // `hovered_` 可能已悬垂（子树被重建）
  // 悬停对象变了 → 提示重新计时（见 `update_hint`）。
  set_hint_target(target);
  const RenderContext context = render_context();
  Element* const previous = hovered_;   // 旧目标：尾部要给它标脏（见那里的说明）
  if (hovered_ != nullptr) {
    hovered_->set_hovered(false);
    Event event;
    event.kind = EventKind::HoverOut;
    hovered_->notify_hover(false);  // 回调与事件并行：局部逻辑不必自己去解析事件流
    (void)dispatch_to(*hovered_, event);
  }
  hovered_ = target;
  if (hovered_ != nullptr) {
    hovered_->set_hovered(true);
    Event event;
    event.kind = EventKind::HoverIn;
    hovered_->notify_hover(true);
    (void)dispatch_to(*hovered_, event);
  }
  // ⚠ **新旧目标都要标脏**。
  //
  // 漏掉旧目标会留下"卡住的悬浮高亮"：它的 `hover_t_` 还停在 1.0，而悬浮过渡是在
  // `paint` 里推进的——不重绘就永远停在悬浮外观上。现象是侧栏出现**两个**"选中项"
  // （一个真选中、一个卡住的悬浮），看起来像"导航自己在变来变去"。实测踩过。
  //
  // 只标**两个元素本身**：`mark_dirty()` 内部会把脏标记冒泡到全部祖先（不必逐个链上
  // 调用）；而逐个链上调用会把**根元素**也记进损坏区——根=整窗口，增量重绘直接
  // 退化成每帧整帧（实测踩过：悬停一次，全帧重画）。
  if (previous != nullptr) previous->mark_dirty();
  if (target != nullptr) target->mark_dirty();
  ++version_;
  (void)context;
}

// ════════════════════════════════════════════════════════════════════════════
// 悬停提示（共享气泡）
// ════════════════════════════════════════════════════════════════════════════
//
// 设计要点（为何这样做）：
//
// · **一个共享气泡**，而不是每个元素一个：悬停的对象每次至多一个，而界面里有
//   几百个图标按钮。每个元素自建一个 `Tooltip` 子元素 = 几百个隐藏元素参与
//   测量/遍历/对账（内存与帧时间都白付）。共享气泡只多一个元素，且只在需要时
//   插进叠加层。
// · **延时是必需的**（默认 450ms）：“鼠标扫过一排按钮”时立刻弹会闪出一串气泡——
//   那是干扰而不是提示。
// · **提示挂在叠加层**：它是“浮在最上”的东西，挂进内容树会被父容器的裁剪/滚动
//   切掉（实测：侧栏里的气泡被裁到只剩半行）。
// · **属于哪个元素**：从悬停元素沿父链向上找第一个 `hint()` 非空的——这样
//   点侧栏一行、点行里的图标、点行里的文本，三者得到同一个提示（与 Web 的
//   `title` 冒泡语义一致）；否则调用方得在每一层都写一遍。

void UiRoot::set_hint_target(Element* target) {
  Element* owner = nullptr;
  for (Element* node = target; node != nullptr; node = node->parent()) {
    if (!node->hint().empty()) {
      owner = node;
      break;
    }
  }
  if (owner == hint_target_) return;
  hide_hint();   // 新目标先收掉上一个的气泡（否则会挂在错误的位置上）
  hint_target_ = owner;
  hint_elapsed_ms_ = 0.0;
  // 基准**不在这里立**，而是交给第一次 `update_hint`（`hint_base_pending_`）。
  //
  // 本函数跑在**事件分发**期，而 `time_seconds_` 是上一帧帧首写入的
  //——此刻读到的是**旧值**。拿它当基准的话，下一帧的 delta 就是“从上一个帧首到现在”，
  // 鼠标在空白处停了一会儿再移上按钮时这个值可以很大（实测 **816ms**），
  // 450ms 的延时被整帧跳过（现象：“一悬停就弹”）。
  hint_base_pending_ = true;
}

void UiRoot::update_hint(double delta_ms) {
  if (hint_target_ == nullptr) {
    return;
  }
  ++g_hint_update_calls;
  g_hint_last_delta = delta_ms;
  // **第一次推进只立基准**（见 `set_hint_target` 的说明）：
  // 悬停刚发生的那一帧不知道“已经停了多久”，从这一帧起才算计时。
  if (hint_base_pending_) {
    hint_base_pending_ = false;
    hint_time_seconds_ = time_seconds_;
    return;
  }
  prune_stale_pointers();
  if (hint_target_ == nullptr) {
    hint_target_ = nullptr;
    hide_hint();
    return;
  }
  if (!hint_visible_) {
    hint_elapsed_ms_ += delta_ms;
    if (hint_elapsed_ms_ < static_cast<double>(hint_delay_ms_)) return;
  }
  const std::string& text = hint_target_->hint();
  if (text.empty()) {
    hide_hint();
    return;
  }
  if (hint_bubble_raw_ == nullptr) {
    auto bubble = std::make_unique<Tooltip>();
    bubble->set_id("hint-bubble");
    bubble->set_focusable(false);   // 只看的：不参与 Tab 焦点环
    hint_bubble_raw_ = bubble.get();
    add_overlay(std::move(bubble), OverlayLayout::Stack);
  }
  // ⚠ **显示时要让元素真的可见**（与 `hide_hint` 里的 `set_visible(false)` 对）：
  // `Tooltip` 自己的命中测试看 `active_`，而控制通道 `find`/语义树看 `visible()`
  //——两者必须一起翻转，否则“弹着”与“收着”在自动化眼里没区别。
  hint_bubble_raw_->set_visible(true);
  Tooltip* bubble = dynamic_cast<Tooltip*>(hint_bubble_raw_);
  if (bubble == nullptr) return;
  bubble->set_text(text);
  bubble->set_target_rect(hint_target_->bounds());
  bubble->set_active(true);
  hint_text_cache_ = text;
  if (!hint_visible_) {
    hint_visible_ = true;
    mark_dirty_all();
  }
}

void UiRoot::hide_hint() {
  hint_elapsed_ms_ = 0.0;
  if (!hint_visible_) return;
  hint_visible_ = false;
  hint_text_cache_.clear();
  if (hint_bubble_raw_ != nullptr) {
    if (auto* bubble = dynamic_cast<Tooltip*>(hint_bubble_raw_); bubble != nullptr) {
      bubble->set_active(false);
    }
    // **也置不可见**：只清 `active_` 的话，气泡在树上依旧 `visible`，
    // `find #hint-bubble` 永远命中——“收没收起”变得不可断言。
    hint_bubble_raw_->set_visible(false);
  }
  mark_dirty_all();
}

auto UiRoot::dispatch_key_into(Element* root, Event& event) -> bool {
  // 顺序：**先问本元素自身 → 焦点所在的子元素链 → 其余子元素**。
  //
  // ① 先问本元素：方向键/Enter/Esc 在浮层里是**容器级语义**（命令面板的 ↑↓ 移高亮、
  //    菜单面板的 ↑↓ 选项），而焦点子元素（过滤输入框）会先把 ArrowDown 当
  //    “光标移动”吞掉——“面板的方向键导航永远不生效”就是这么来的。容器先拿，
  //    它不认（返回 false）才继续下沉。
  //
  // ② 焦点链必须排在其他子元素**之前**：早期实现只是**逆序问所有子元素**，
  //    于是“谁被最后声明”谁先抢键——浮层里带输入框时，尾部的「×」按钮
  //    （`Button` 对 Enter 会 `activate()`）把本该属于输入框的 Enter 吃掉了。
  //    实测（gbcode 的“转到行”浮层）：在行号输入框里敲 Enter，结果是**浮层被关掉**、
  //    `on_submit` 根本没跑；查找条（尾部也有「×」）同理。
  //    这不是某个组件的问题：浮层的按键必须先交给**焦点元素**。
  if (root == nullptr) return false;
  // **不可见子树不接键**：焦点元素随面板隐藏后（如收起的终端），它的 bounds
  // 还停在上一帧的位置——不拦的话，隐藏面板里的组件会继续吃键盘
  //（实测风险：收起终端后按快捷键，先被隐藏的 Terminal 翻成字节流）。
  if (!root->visible()) return false;
  const RenderContext context = render_context();
  if (root->on_event(context, event)) return true;
  if (focused_ != nullptr) {
    // 只在“焦点确实在这棵子树里”时才走链，否则会把键送给不相干的元素。
    for (Element* walk = focused_; walk != nullptr; walk = walk->parent()) {
      if (walk != root) continue;
      Event focused_event = event;   // 副本：消费后把 `handled` 回写给调用方
      if (dispatch_to(*focused_, focused_event)) {
        if (focused_event.handled) event.handled = true;
        return true;
      }
      break;
    }
  }
  for (std::size_t index = root->child_count(); index > 0; --index) {
    Element* child = root->child_at(index - 1);
    if (child == nullptr || !child->visible()) continue;
    if (dispatch_key_into(child, event)) return true;
  }
  return false;
}

auto UiRoot::coalesced_move_count() const noexcept -> std::size_t {
  return moves_.merged_count();
}

auto UiRoot::feed_pointer_move(Event& event) -> bool {
  // **由宿主在"知道帧边界"的地方调用**（见 `input_coalescer.hpp` 的说明）。
  //
  // 为何不在 `dispatch` 里自动合并：`dispatch` 的调用者有三类——真窗口帧循环、
  // 测试、控制通道脚本。后两者**期望"调一次就派发一次"**（它们没有帧的概念）。
  // 在 `dispatch` 里扣住会让 `root.dispatch(move)` 之后状态还没变，于是
  // `opened[0]` 那类断言直接越界崩溃（实测：整套单测在菜单用例处 0xC0000005）。
  // 所以合并是**宿主的显式选择**：调它 = "我现在在帧循环里"。
  if (event.kind != EventKind::MouseMove) return false;
  if (pressed_ != nullptr) {
    // 按下期间不合并：拖拽（画线、拖滑块）是连续几何，合并掉中间的会断笔。
    moves_.reset();
    return false;
  }
  moves_.feed(event.position, event.ctrl, event.shift, event.alt, event.meta);
  return true;   // 已收下：宿主不必再 dispatch 它
}

auto UiRoot::flush_coalesced_moves() -> std::size_t {
  const auto move = moves_.take();
  if (!move.has_value()) return 0;
  if (move->left && !move->moved() && move->merged == 0) {
    Event leave;
    leave.kind = EventKind::HoverOut;
    leave.position = move->latest;
    (void)dispatch(leave);
    return 0;
  }
  Event event;
  event.kind = EventKind::MouseMove;
  event.position = move->latest;
  event.ctrl = move->ctrl;
  event.shift = move->shift;
  event.alt = move->alt;
  event.meta = move->meta;
  (void)dispatch(event);
  return 1 + move->merged;
}

auto UiRoot::refresh_cursor(math::Point point) -> void {
  MouseCursor wanted = MouseCursor::Arrow;
  Element* hit = hit_test_pointer_move(point);
  // 沿 parent 链向上找第一个“有自己意见”的那层。
  for (Element* node = hit; node != nullptr; node = node->parent()) {
    const MouseCursor candidate = node->cursor_at(point);
    if (candidate != MouseCursor::Arrow) {
      wanted = candidate;
      break;
    }
  }
  // 没有命中任何元素（点在窗口空白处）时保持 Arrow——上面的初值就是它。
  if (wanted == cursor_) return;
  cursor_ = wanted;
  // 后端可能为空（测试里 UiRoot 单独使用）；空实现的后端在这里什么也不做。
  if (cursor_sink) cursor_sink(cursor_);
}

auto UiRoot::dispatch(Event& event) -> bool {
  // 分发前先清悬垂指针：界面每帧都可能重建子树（列表刷新、页面替换），
  // 而焦点/悬停/按压指针可能正指着已被销毁的元素
  prune_stale_pointers();
  // 分发期间摘除的叠加层走**延迟析构**（见 `remove_overlay`）：
  // 分发栈里可能还持着该子树内元素的裸指针（`dispatch_to` 沿 parent 链回溯），
  // 立即释放就是 use-after-free。这里标记 + 分发结束后统一回收。
  dispatching_ = true;
  struct DispatchGuard {
    bool& flag;
    ~DispatchGuard() { flag = false; }
  } guard{dispatching_};
  layout();
  // 任何输入都先把提示收起来（见 `update_hint`）：
  // 鼠标动/点击/按键之后还挂着上次的提示，是“黏在屏幕上”的感觉。
  // 若这个事件又是“悬停在带提示的元素上”，下一帧的推进会把计时重新拉起。
  hide_hint();
  bool handled = false;

  switch (event.kind) {
    case EventKind::MouseMove: {
      // 拖拽归属：按下时锁定的元素在释放前持续接收 move（即使指针已拖出它）——
      // 否则 CodeEditor 拖选、ScrollBar 拖滑块一出边界就断（事件按命中转发，
      // 而拖拽语义属于"按下的那个元素"，与 Web/Qt 的隐式捕获一致）。
      if (pressed_ != nullptr) {
        // 拖拽中光标**锁住不动**：拉分隔线时指针很容易跑出那条 12px 的手柄，
        // 若每帧重新解析，光标会在 Resize 与 Arrow 之间抳动——用户正在拖的东西
        // 看起来“丢了”。这是原生拖拽的惯例（拖到哪都是拖拽光标）。
        handled = dispatch_to(*pressed_, event);
        break;
      }
      // 光标解析放在**命中之前**（它只看位置、不改任何状态，先算不影响派发）。
      refresh_cursor(event.position);
      // 指针移动走**移动专用命中**：屏障类元素只拦点击、放行移动，
      // 否则宿主（菜单栏/工具栏）收不到 hover（实测：面板开着时菜单栏无法悬停切换）。
      Element* target = hit_test_pointer_move(event.position);
      update_hover(target);
      handled = dispatch_to(target != nullptr ? *target : *content_, event);
      break;
    }
    case EventKind::MouseDown: {
      Element* target = hit_test(event.position);
      if (target == nullptr) {
        // ⚠ **命中不到任何元素时不要动 `pressed_`**（把上一次的所有权留给 MouseUp）。
        //
        // 为何：指针按下在控件外、再移入后释放（或本帧布局还没完成导致命中
        // 未命中）时，若在这里清空 `pressed_`，**后续的 MouseMove 会丢掉拖拽
        // 归属**——旧实现只在这一支里赋值，而按下命中不到就不该改变已有状态。
        // 实测：滑块拖拽用例在一次 `MouseDown`（命中不到）之后，
        // 后续 `MouseMove` 全部失效（值停在 0.5 不动），看着像“滑块坏了”。
        break;
      }
      pressed_ = target;
      pressed_at_ = event.position;   // 拖动判定基准（见 Click 分支的兑底拦截）
      if (target->focusable() && focused_ != target) set_focus(target);
      // 鼠标按下**不给焦点环**（见 `focus_ring_visible()`）：点一下就在按钮上
      // 留一圈边框不是任何人要的，而且它会一直留着。键盘一按就回来
      //（Tab/任何 KeyDown），需要时仍能看到焦点在哪。
      set_focus_ring_visible(false);
      target->mark_dirty();
      // ⚠ `set_pressed(true)` 不能漏：组件的“按住拖动中”靠它判（滑块/滚动条/
      // 拖选都要），子元素自己的 `pressed_` 与 UiRoot 的 `pressed_`（指针归属）
      // 是**两个**东西。
      target->set_pressed(true);
      handled = dispatch_to(*target, event);
      break;
    }
    case EventKind::MouseUp: {
      // **拖拽归属（释放侧）**：按下时锁定的元素**先**收 MouseUp——
      // 拖出手柄/滑块后释放不再丢失（SplitView 拖分栏、ScrollBar 拖滑块同属此类；
      // 与上方 MouseMove 的 `pressed_` 分支同一契约：拖拽语义属于「按下的那个元素」）。
      // 它不处理时回落命中元素（普通点击的释放路径完全不变）。
      //
      // 释放时结算「本次按放是否拖动过」（见 Click 分支的兜底拦截）——
      // 放在这里而不是 Click 里读 `pressed_at_`：后端补发的 Click 到达时 `pressed_`
      // 已清空，而**只有 Click、没有 down/up**的派发（控制通道/测试常态）里
      // `pressed_at_` 是无效默认值，用它判会把普通点击也误判成拖动（实测踩到）。
      last_release_dragged_ =
          std::abs(event.position.x - pressed_at_.x) > kDragThreshold ||
          std::abs(event.position.y - pressed_at_.y) > kDragThreshold;
      Element* target = hit_test(event.position);
      Element* pressed = pressed_;
      if (pressed_ != nullptr) {
        pressed_->set_pressed(false);
        pressed_->mark_dirty();
      }
      pressed_ = nullptr;
      if (pressed != nullptr) {
        handled = dispatch_to(*pressed, event);
        if (!handled && target != nullptr && target != pressed) {
          handled = dispatch_to(*target, event);
        }
      } else {
        handled = target != nullptr && dispatch_to(*target, event);
      }
      break;
    }
    case EventKind::Click:
    case EventKind::DoubleClick:
    case EventKind::TripleClick: {
      Element* target = hit_test(event.position);
      if (pressed_ != nullptr) {
        pressed_->set_pressed(false);
        pressed_->mark_dirty();
      }
      pressed_ = nullptr;
      handled = target != nullptr && dispatch_to(*target, event);
      // ⚠ 拖动过的松手**不是点击**：后端在 `LBUTTONUP` 后无条件补发 `Click`
      //（`platform_win32.cpp`），而本分支的兑底 `activate()` 不加区分的话，
      // 「拖 SplitView 手柄松手」会被当成激活——`SplitView::activate()` 是双击归位
      // （0.5），于是面板拖完就回弹到一半（实测正是用户报的缺陷）。
      //
      // 判据：按下→本次事件的位移超过阈值 = 拖动过，不再兑底激活。
      // 这是**手势语义**的账，只能记在 UiRoot：拖拽归属已在本类（`pressed_`），
      // 每个组件自己猜会重复踩坑（ScrollBar 拖完、未来任何可拖组件）。
      // 组件显式消费 `Click` 的不受影响（那条路在上面 `dispatch_to` 已返回）。
      // 「刚结束的这次按放拖动过」才拦（`last_release_dragged_` 由 MouseUp 结算；
      // 纯 Click 派发没有先导 down/up，`pressed_at_` 无效——那种情况不拦，兜底照常）。
      const bool dragged = event.kind == EventKind::Click && last_release_dragged_;
      last_release_dragged_ = false;   // 一次性：下一次按放重新结算
      if (!handled && !dragged && target != nullptr && event.kind == EventKind::Click) {
        target->activate();
        handled = true;
      }
      break;
    }
    case EventKind::Wheel: {
      Element* target = hit_test(event.position);
      handled = target != nullptr && dispatch_to(*target, event);
      break;
    }
    case EventKind::KeyDown: {
      // 键盘一到就恢复焦点环（见 `focus_ring_visible()`）：用键盘的人需要看到
      // 焦点在哪；用鼠标的人不该被一圈边框打扰。
      set_focus_ring_visible(true);
      // ① 全局快捷键**最先**：编辑器形态的 Ctrl+S/Ctrl+W 需要先于一切组件的落点
      //    （文本组件合法吞键时，快捷键仍是全局语义；handler 返回 false 则放弃下沉继续）。
      if (!shortcuts_.empty()) {
        std::string key(event.key);
        std::ranges::transform(key, key.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (auto iterator = shortcuts_.rbegin(); iterator != shortcuts_.rend(); ++iterator) {
          if (iterator->first != key) continue;
          const Shortcut& mods = iterator->second.mods;
          if (mods.ctrl != event.ctrl || mods.shift != event.shift || mods.alt != event.alt ||
              mods.meta != event.meta) {
            continue;
          }
          if (iterator->second.handler && iterator->second.handler()) {
            handled = true;
            break;
          }
        }
        if (handled) break;
      }
      // ② 模态语义：有叠加层（Dialog/Toast/下拉面板）时键盘事件先给最上层浮层——
      //    否则「Esc 关对话框」永远送不进去（焦点还在被遮住的内容元素上）。
      //    浮层不处理再回落焦点元素（浅层浮层如 Toast 不拦截正常输入）。
      //    不拦截命中的浮层（`intercepts_input()` 为假，如已隐藏的命令面板）跳过。
      //
      //    为什么是**递归下钻**而不是只问最上层：声明式浮层的根是一个铺满视口的
      //    宿主容器（它自己不会处理 Esc），真正认 Esc 的 `MenuPanel`/`CommandPalette`
      //    在其子树里。只问宿主 → 宿主返回 false → 整个浮层链被跳过，
      //    「Esc 关面板」永远失效（实测：菜单/命令面板都关不掉）。
      //    深度优先（后声明者优先）与命中测试同一序：最上面的浮层先拿。
      for (auto iterator = overlays_.rbegin(); iterator != overlays_.rend() && !handled; ++iterator) {
        Element* overlay = iterator->get();
        if (overlay == nullptr || !overlay->visible()) continue;
        if (!overlay->intercepts_input()) continue;
        handled = dispatch_key_into(overlay, event);
      }
      if (handled) break;
      // ③ 焦点元素优先处理（含冒泡）：组件对自己认识的键返回 true。
      if (focused_ != nullptr && dispatch_to(*focused_, event)) {
        handled = true;
        break;
      }
      // ④ Tab 焦点环**最后**：焦点元素未消费（如文本编辑器声明 Tab 自含、或无焦点）时，
      //    Tab 才作为全局焦点遍历语义生效；带 Shift 的 Tab 同样先给焦点元素机会
      //    （反向切标签等用户语义优先）。
      if (event.key == "Tab" && focused_ != nullptr) {
        focus_next(event.shift);
        handled = true;
      }
      break;
    }
    case EventKind::KeyUp:
    case EventKind::TextInput: {
      set_focus_ring_visible(true);
      handled = focused_ != nullptr && dispatch_to(*focused_, event);
      break;
    }
    case EventKind::FocusIn:
    case EventKind::FocusOut:
    case EventKind::HoverIn:
    case EventKind::HoverOut:
      handled = false;
      break;
  }
  if (handled) ++version_;
  reap_overlays();
  return handled;
}

auto UiRoot::find(std::string_view id) -> Element* {
  // 叠加层（Dialog/Toast/Select 面板）与内容树同属语义面：按 id 定位必须两者都搜——
  // 只搜内容会让"打开的对话框/轻提示"从协议 `get/set/invoke` 里消失
  // （2026-09-30 审视发现：gallery 打开 Dialog 后 find/#demo-dialog 永远 not_found）。
  //
  // ⚠ 同 id 多实例时**优先可见者**（2026-10-13，gbcode 亲历）：「常驻+隐藏」的侧栏
  // 视图会让同一 id 有两份（一份可见、一份 0x0 的隐藏残留），盲取第一个意味着
  // `get`/按 id 点击命中哪份取决于声明顺序——自动化会点到隐藏那份且表现为
  // 「点了没反应」。两遍口径：可见实例优先；全部不可见时退回盲取第一个
  // （兼容「真隐藏但就是要读它属性」的场景）。
  Element* found_visible = nullptr;
  Element* found_any = nullptr;
  const auto walk = [&](auto&& self, Element& element) -> void {
    if (found_visible != nullptr) return;
    if (element.derived_id() == id) {
      if (element.visible()) {
        found_visible = &element;
        return;
      }
      if (found_any == nullptr) found_any = &element;
    }
    for (std::size_t index = 0; index < element.children().size(); ++index) {
      self(self, *element.child_at(index));
      if (found_visible != nullptr) return;
    }
  };
  const auto overlays_walk = [&] {
    for (auto& overlay : overlays_) {
      if (overlay == nullptr) continue;
      walk(walk, *overlay);
      if (found_visible != nullptr) return;
    }
  };
  if (content_ != nullptr) walk(walk, *content_);
  if (found_visible == nullptr) overlays_walk();
  return found_visible != nullptr ? found_visible : found_any;
}

auto UiRoot::query(const Selector& selector, std::size_t limit) -> std::vector<Element*> {
  // 同 find(id)：选择器也要覆盖叠加层，否则 `Dialog`/`Toast`/`Select 面板` 全部选不到。
  std::vector<Element*> matches;
  const auto walk = [&](auto&& self, Element& element) -> void {
    if (limit != 0 && matches.size() >= limit) return;
    if (selector.matches_with_ancestors(element)) matches.push_back(&element);
    for (std::size_t index = 0; index < element.children().size(); ++index) {
      self(self, *element.child_at(index));
      if (limit != 0 && matches.size() >= limit) return;
    }
  };
  if (content_ != nullptr) walk(walk, *content_);
  for (auto& overlay : overlays_) {
    if (overlay == nullptr) continue;
    walk(walk, *overlay);
  }
  return matches;
}

void UiRoot::prune_stale_pointers() {
  if (focused_ == nullptr && hovered_ == nullptr && pressed_ == nullptr &&
      hint_target_ == nullptr && hint_bubble_raw_ == nullptr) {
    return;
  }
  // 只做指针相等比较：候选指针可能已指向销毁的元素，**绝不能解引用**
  const auto on_tree = [this](const Element* candidate) -> bool {
    if (candidate == nullptr) return false;
    const auto walk = [&candidate](auto&& self, const Element& node) -> bool {
      if (&node == candidate) return true;
      for (std::size_t index = 0; index < node.children().size(); ++index) {
        if (self(self, *node.child_at(index))) return true;
      }
      return false;
    };
    if (content_ != nullptr && walk(walk, *content_)) return true;
    for (const auto& overlay : overlays_) {
      if (overlay != nullptr && walk(walk, *overlay)) return true;
    }
    return false;
  };
  if (!on_tree(focused_)) focused_ = nullptr;
  if (!on_tree(hovered_)) hovered_ = nullptr;
  if (!on_tree(pressed_)) pressed_ = nullptr;
  // 提示的两个观察者：目标（在内容树）与气泡（在叠加层——上面的 `on_tree`
  // 也遍历 `overlays_`，所以同一条判据能用）。气泡被外部摘掉（`clear_overlays`）
  // 之后不能继续用，否则下一帧 `set_active` 就是 use-after-free。
  if (!on_tree(hint_target_)) {
    hint_target_ = nullptr;
    hint_elapsed_ms_ = 0.0;
    hint_visible_ = false;
  }
  if (!on_tree(hint_bubble_raw_)) hint_bubble_raw_ = nullptr;
}

auto UiRoot::focused() -> Element* {
  // 协议/脚本/动作层都靠这个访问器拿焦点元素——它们会**直接解引用**返回值，
  // 所以悬垂判断必须在这里做（不能指望调用方自己检查）
  prune_stale_pointers();
  return focused_;
}

auto UiRoot::set_focus(Element* element) -> bool {
  // 不可聚焦元素拒绝接受焦点（见头文件声明处）：`focusable()` 是「能否持有焦点」的
  // 契约，Tab 焦点环按它筛选——无条件赋值会产生「焦点在这、Tab 环里没它」的状态分裂。
  if (element != nullptr && !element->focusable()) return false;
  if (focused_ == element) return true;
  prune_stale_pointers();  // `focused_` 可能已悬垂：清掉再走 FocusOut 通告
  const RenderContext context = render_context();
  if (focused_ != nullptr) {
    focused_->set_focused(false);
    Event event;
    event.kind = EventKind::FocusOut;
    (void)dispatch_to(*focused_, event);
    focused_->mark_dirty();
  }
  focused_ = element;
  if (focused_ != nullptr) {
    focused_->set_focused(true);
    Event event;
    event.kind = EventKind::FocusIn;
    (void)dispatch_to(*focused_, event);
    focused_->mark_dirty();
  }
  // 焦点变更是状态变更源（"哪个元素被选中/可输入"是界面状态的一部分）：登记进变更清单。
  if (element != nullptr) note_changed(*element);
  ++version_;
  (void)context;
  return true;
}

void UiRoot::collect_focus_order(Element& element, std::vector<Element*>& order) {
  if (!element.visible() || !element.enabled()) return;
  if (element.focusable()) order.push_back(&element);
  for (std::size_t index = 0; index < element.children().size(); ++index) {
    collect_focus_order(*element.child_at(index), order);
  }
}

void UiRoot::focus_next(bool backwards) {
  if (content_ == nullptr) return;
  std::vector<Element*> order;
  collect_focus_order(*content_, order);
  if (order.empty()) return;
  if (focused_ == nullptr) {
    set_focus(order.front());
    return;
  }
  const auto iterator = std::ranges::find(order, focused_);
  if (iterator == order.end()) {
    set_focus(order.front());
    return;
  }
  const auto index = static_cast<std::ptrdiff_t>(std::distance(order.begin(), iterator));
  const auto size = static_cast<std::ptrdiff_t>(order.size());
  const std::ptrdiff_t next = backwards ? (index - 1 + size) % size : (index + 1) % size;
  set_focus(order[static_cast<std::size_t>(next)]);
}

auto UiRoot::semantics(std::uint32_t max_depth) const -> SemanticsNode {
  SemanticsNode root;
  root.id = "root";
  root.type = "Root";
  root.role = Role::Panel;
  root.bounds = math::Rect{0.0f, 0.0f, viewport_.width, viewport_.height};
  root.flags = SemanticsFlags{};

  if (content_ != nullptr) {
    SemanticsNode node;
    content_->collect_semantics(node);
    root.children.push_back(std::move(node));
  }
  for (const auto& overlay : overlays_) {
    SemanticsNode node;
    overlay->collect_semantics(node);
    root.children.push_back(std::move(node));
  }

  if (max_depth > 0) {
    const auto prune = [](auto&& self, SemanticsNode& node, std::uint32_t depth,
                          std::uint32_t limit) -> void {
      if (depth + 1 >= limit) {
        node.children.clear();
        return;
      }
      for (auto& child : node.children) self(self, child, depth + 1, limit);
    };
    prune(prune, root, 0, max_depth);
  }
  return root;
}

auto UiRoot::visual_tree() const -> VisualNode {
  VisualNode root;
  root.id = "root";
  root.type = "Root";
  root.bounds = math::Rect{0.0f, 0.0f, viewport_.width, viewport_.height};
  root.fill = theme_.colors().bg.to_css();
  if (content_ != nullptr) {
    VisualNode node;
    node.depth = 1;
    content_->collect_visual(node);
    root.children.push_back(std::move(node));
  }
  for (const auto& overlay : overlays_) {
    VisualNode node;
    node.depth = 1;
    overlay->collect_visual(node);
    root.children.push_back(std::move(node));
  }
  return root;
}

void UiRoot::mark_dirty_all() {
  dirty_ = true;
  // 整树重排/主题切换等全局变化：整帧重绘（局部路径不适用）。
  pending_full_ = true;
  ++version_;
  // **必须走全全部子树**：内容树 **+ 每个覆盖层**（2026-10-18 修，
  // 用户报“Ctrl+P 面板配色不对”）。
  //
  // 旧实现只走了 `content_`。而 `layout()` 的进入条件是
  // “`dirty_` 或 `tree_layout_dirty()`”，且 `apply_theme_tree`
  // **只在 `layout()` 里**跑。于是只要内容树干净，切主题就会跳过整个 layout——
  // 覆盖层的 `apply_theme` 永远不执行，配色停在它第一次被排出来时那个主题上。
  // 实测症状：暗色界面里命令面板是白底（拿到的是浅色令牌），切到浅色反而变暗。
  // 这类“某个子树收不到主题”的缺陷在内容树里看不出来（内容树自己是对的）。
  const auto walk = [](auto&& self, Element& element) -> void {
    element.mark_layout_dirty();
    for (std::size_t index = 0; index < element.children().size(); ++index) {
      self(self, *element.child_at(index));
    }
  };
  if (content_ != nullptr) walk(walk, *content_);
  for (const auto& overlay : overlays_) {
    if (overlay != nullptr) walk(walk, *overlay);
  }
}

void UiRoot::note_changed(const Element& element) {
  if (changed_ids_.size() >= kMaxChangedIds) return;
  const ElementId id = element.derived_id();
  if (id.empty()) return;
  // 去重：同一元素在一帧内多次变化只报一次（"哪些元素变了"是集合语义，不是计数）
  if (std::ranges::find(changed_ids_, id) != changed_ids_.end()) return;
  changed_ids_.push_back(id);
  // 状态变更也是版本变更：`ui.changed` 事件与 `wait for=stable` 都靠版本号
  // 感知"界面变了"——只登记清单而不动版本，事件永远发不出去。
  ++version_;
}

auto UiRoot::take_changed_ids() -> std::vector<ElementId> {
  std::vector<ElementId> taken = std::move(changed_ids_);
  changed_ids_.clear();
  return taken;
}

void UiRoot::clear_dirty() noexcept {
  // 动画未结束就**保持脏**：元素在本次绘制里声明的"还在动"是下一帧的依据。
  // 清掉它会让动画停在第一帧；而每帧重新声明，所以动画结束后重绘会自然停下。
  //
  // `animation_pending_` 由绘制阶段汇总（遍历一次），这里只消费；
  // 注意**不动**元素侧新累积的损坏区：绘制期间新产生的（`request_animation`）
  // 要留给下一帧做增量重绘（帧首由 `paint_frame` 收集消费）。
  followup_ = animation_pending_;
  animation_pending_ = false;
  // 帧级"整帧"标记的兜底清理：正常流程里 `paint_frame` 已消费（帧首快照）；
  // 直接调 `layout()/paint()` 的路径（测试/手写循环）没有消费者，留着会让
  // `dirty()` 永不安静。
  pending_full_ = false;
}

auto UiRoot::tree_layout_dirty() const noexcept -> bool {
  // 元素 mark_layout_dirty 会把标记沿 parent 链冒泡到所在树的根（content/overlay 根），
  // 因此根上的标记就是"树里有人要重排"的 O(1) 汇总。
  if (content_ != nullptr && content_->layout_dirty()) return true;
  for (const auto& overlay : overlays_) {
    if (overlay != nullptr && overlay->layout_dirty()) return true;
  }
  return false;
}

auto UiRoot::needs_frame() const noexcept -> bool {
  return dirty_ || followup_ || pending_full_ || !pending_damage_.is_empty() ||
         tree_layout_dirty();
}

void UiRoot::collect_tree_damage() {
  const auto merge = [this](const Element* root) {
    if (root == nullptr) return;
    const Element::DamageReport report = root->take_damage();
    if (!report.valid) return;
    if (report.needs_full) {
      pending_full_ = true;
      return;
    }
    pending_damage_ = pending_damage_.is_empty() ? report.rect
                                                 : pending_damage_.union_with(report.rect);
  };
  merge(content_.get());
  for (const auto& overlay : overlays_) merge(overlay.get());
}

auto UiRoot::paint_frame(raster::Surface& canvas) -> bool {
  // 0) 推进悬停提示的计时（提示是“延后出现”的，跟 `context.time_seconds` 同源；
  //    没有提示在计时时这件事是空操作）。放在损坏区收集**之前**：
  //    提示刚弹出/刚收起都要进本帧的损坏区，否则要等下一帧才看得见。
  {
    const double previous = hint_time_seconds_;
    hint_time_seconds_ = time_seconds_;
    if (hint_target_ != nullptr) {
      update_hint(std::max(0.0, (time_seconds_ - previous) * 1000.0));
    }
  }
  // 1) 收集元素上报的损坏区（上一帧之后到现在的所有变更）。
  collect_tree_damage();
  // 2) 必要时重排；真跑了重排 → 保守整帧（见 `layout` 尾部说明）。
  layout();
  // 3) 快照并消费本次帧的损坏区（绘制期间新记录的走元素，留给下一帧）。
  const math::Rect damage = pending_damage_;
  const bool full_flag = pending_full_;
  pending_damage_ = math::Rect{};
  pending_full_ = false;

  const math::Rect viewport_rect{0.0f, 0.0f, viewport_.width, viewport_.height};
  const math::Rect clipped_damage = damage.intersect(viewport_rect);
  const float viewport_area =
      std::max(1.0f, viewport_rect.width * viewport_rect.height);
  const float damage_area =
      clipped_damage.is_empty() ? 0.0f : clipped_damage.width * clipped_damage.height;
  // 全量条件：显式整帧标记 / 画布不支持局部 / 损坏区为空（保守地"什么都画"）/
  // 损坏区过大（超过视口 55% 时整帧更划算——跳过裁剪开销与小块拼接）。
  const bool use_full = full_flag || !canvas.supports_partial_repaint() ||
                        clipped_damage.is_empty() || damage_area > viewport_area * 0.55f;

  painted_elements_ = 0;
  RenderContext context = render_context();
  context.painted_elements = &painted_elements_;

  if (use_full) {
    // 整帧：清屏 + 绘制整树（GPU 画布恒走这条，与旧行为完全一致）。
    // ⠇叠加层在内容之后绘制（浮层在景上：遮罩/对话框/轻提示必须盖住内容）。
    //   旧序（先 overlay 后 content）会让内容把 Dialog 卡片/遮罩全部盖住
    //   （2026-09-30 画廊补全时实测发现：dialog-open 截图里什么都看不到）。
    canvas.clear(theme_.colors().bg);
    if (content_ != nullptr) paint_subtree(context, *content_, canvas);
    for (auto& overlay : overlays_) overlay->paint(context, canvas);
    last_frame_partial_ = false;
    dirty_rect_ = math::IntRect{0, 0, static_cast<int>(viewport_.width),
                                static_cast<int>(viewport_.height)};
  } else {
    // 局部：清损坏区 + 推裁剪 + 绘制整树。
    //
    // 为什么"重画整树"而不是只重画变化的元素：区域内可能叠着其他元素（z 序在后的
    // 邻层），只画变化元素会把邻居盖掉；而 `Element::paint` 自带**按裁剪域剔除**
    // （与视口剔除同一机制，含阴影/发光外扩），整树遍历里越界分支全部 O(1) 跳过，
    // 留下的正好是"与损坏区相交的全部元素"——z 序自然正确。
    canvas.push_clip_rect(clipped_damage);
    canvas.fill_rect(clipped_damage, raster::Paint::solid(theme_.colors().bg), 0.0f,
                     raster::DrawOptions{.blend = raster::BlendMode::Src});
    if (content_ != nullptr) paint_subtree(context, *content_, canvas);
    // 叠加层绘制与回收走 `paint_overlays`（`paint` 里摘除自己的那条路要延后析构，
    // 见其注释——范围 for 在这里会因容器被改而迭代器失效）。
    paint_overlays(context, canvas);
    canvas.pop_clip();
    last_frame_partial_ = true;
    dirty_rect_ = clipped_damage.round_out();
  }
  // 绘制后汇总"还有谁在动"（与 `paint()` 同一时机：必须在所有绘制之后）。
  collect_animation_requests();
  return last_frame_partial_;
}

void UiRoot::add_overlay(std::unique_ptr<Element> overlay, OverlayLayout layout) {
  if (overlay == nullptr) return;
  wire_owner(*overlay);
  assign_ids(*overlay, std::format("overlay[{}]", overlays_.size()));
  overlays_.push_back(std::move(overlay));
  overlay_layouts_.push_back(layout);  mark_dirty_all();
}

auto UiRoot::overlay_at(std::size_t index) const noexcept -> Element* {
  return index < overlays_.size() ? overlays_[index].get() : nullptr;
}

void UiRoot::register_declarative_host(std::function<bool()> advance) {
  if (!advance) return;
  declarative_hosts_.push_back(std::move(advance));
}

auto UiRoot::tick_declarative_hosts() -> std::size_t {
  // **索引式遍历 + 条数快照**（2026-10-14）：旧实现先把整个 vector **拷一份**再遍历
  //（`const auto hosts = declarative_hosts_;`）——每帧一次堆分配 + 拷贝，宿主数多时不小。
  //
  // 为何不能改成直接范围 for：推进过程中组件可能重建子树、甚至**登记新的宿主**
  //（如页面切换）——`advance` 里 `push_back` 会让范围 for 的迭代器失效。
  // 索引式 + 条数快照兼容了这个矛盾：
  //   * 已存在的宿主在原位（不搬动，引用稳定）；
  //   * 本轮新登记的宿主排在末尾，而 `index < count` 挡住了它——**本轮不跑**
  //     （与旧快照实现“快照里没有它”逐帧等价），下一帧自然会轮到；
  //   * 三个条件的 `index < declarative_hosts_.size()` 是**防御性**的：
  //     万一将来出现“推进中移除宿主”（当前无此路径），也不会越界读。
  std::size_t rebuilt = 0;
  const std::size_t count = declarative_hosts_.size();
  for (std::size_t index = 0; index < count && index < declarative_hosts_.size(); ++index) {
    if (declarative_hosts_[index]()) ++rebuilt;
  }
  return rebuilt;
}

void UiRoot::remove_overlay(Element* overlay) {
  for (auto iterator = overlays_.begin(); iterator != overlays_.end(); ++iterator) {
    if (iterator->get() == overlay) {
      // **延迟析构**：本函数可能在**事件分发栈内部**被调用（被分发的那棵子树自己要求
      // 摘除自己，如 `Select` 选完之后收起面板）——此时 `dispatch_to` 正持着该子树内
      // 元素的裸指针沿 `parent()` 链回溯，立即析构就是 use-after-free。
      //
      // 实测踩到（本仓库 `ui_select_opens_via_overlay_host` 当场 SIGSEGV）：
      // 改了主循环的“空闲等待”后，每帧都能跑 `layout`，于是一向靠“下一帧布局才摘除”
      // 才侥幸不崩的路径被提前执行，悬垂指针立刻暴露。
      //
      // 修法：从激活列表里立即摘掉（`find/query/绘制/命中` 当场看不到它），
      // 但**对象延到事件派发结束或下一帧布局前再释放**——指针保持有效，
      // 回溯链上的任何访问都不会踩到已释放内存。
      //
      // **叠加层自身也可能主动摘除自己**（`Toast` 在 `paint` 里到期）——那条路径
      // 上 `dispatching_` 为假，曾经就是**立即 delete**，而它的 `paint` 栈帧
      // 还在用 `this`（ASan 现场见 `paint_overlays` 的注释）。现在只要处于
      // "正在绘制叠加层"或"正在派发事件"，就一律进墓场延后释放。
      if (dispatching_ || painting_overlays_) {
        graveyard_.push_back(std::move(*iterator));
      }
      const auto index = static_cast<std::size_t>(std::distance(overlays_.begin(), iterator));
      overlays_.erase(iterator);
      if (index < overlay_layouts_.size()) {
        // `begin() + index` 要求 `difference_type`（ptrdiff_t），而 `index` 是 size_t——
        // GCC 静默接受（同一宽度下的重解释），**clang 的 `-Wsign-conversion` 直接报错**。
        // 这里显式转换，把“确实是有符号偏移”写在代码里而不是依赖编译器宽容。
        overlay_layouts_.erase(overlay_layouts_.begin() +
                               static_cast<std::ptrdiff_t>(index));
      }
      mark_dirty_all();
      return;
    }
  }
}

void UiRoot::reap_overlays() {
  graveyard_.clear();
}

void UiRoot::clear_overlays() {
  overlays_.clear();
  overlay_layouts_.clear();
  mark_dirty_all();
}

auto UiRoot::overlay_layout(const Element* overlay) const -> OverlayLayout {
  for (std::size_t index = 0; index < overlays_.size(); ++index) {
    if (overlays_[index].get() == overlay) {
      return index < overlay_layouts_.size() ? overlay_layouts_[index] : OverlayLayout::Stack;
    }
  }
  return OverlayLayout::Stack;
}

}  // namespace st::ui


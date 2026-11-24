#pragma once

/// 宿主能力契约：组件向宿主（`UiRoot`/`Application`）要东西的**唯一窄接口**。
///
/// ## 为什么要从 `element.hpp` 抽出来（2026-11）
///
/// 它原先定义在 `Element` 类体内部（作为嵌套类），115 行、11 个虚函数，而它的
/// **唯一实现方是 `UiRoot`**。嵌在 `Element` 里有三个坏处：
///
/// 1. **`element.hpp` 被 78 个 TU 包含**，每个都白读这 115 行与它需要的完整类型；
/// 2. `HostFocus` 要用 `Element*`、`WebViewPort*`、`ChildViewProbe`、`OverlayLayout`，
///    想把它移出去就只能前向声明——而**嵌套类没法前向声明**（必须先有完整宿主类）；
/// 3. 契约与实现混在一起：改宿主能力（加一个 `host_*`）会触碰 UI 基类头。
///
/// 抽出来之后 `element.hpp` 只需 `HostFocus*` 指针 + 前向声明——**契约独立、可单独阅读**。
///
/// ## 依赖方向（刻意的）
///
/// 本头**不认识 `Element` 的实现**（只用前置声明），因此它是纯契约层：
/// `Element` 依赖它，它不依赖 `Element` 的任何细节。

#include <memory>
#include <string>
#include <string_view>

#include "st/math/geometry.hpp"

namespace st::ui {

class Element;
class WebViewPort;
struct Theme;

/// 叠加层排布形态（`HostFocus::host_add_overlay` 与 `UiRoot::add_overlay` 共用）。
///
/// 为何是顶层枚举：嵌套枚举在仅**前向声明** `UiRoot` 时不可用，而本头（以及
/// `UiRoot` 的实现分片）都要在只有前置声明的场合引用它。
enum class OverlayLayout { Stack, FillViewport };

/// 嵌入式子视图的自检读数（`HostFocus::host_child_view_probe` 的返回）。
///
/// 与 `shell::ChildViewProbe` 字段一一对应；`ui` 层不能包含 `shell` 头
///（依赖方向），所以两边各留一份、在 `Application` 处折。
struct ChildViewProbe {
  bool attached{false};
  float title_bar_height{0.0f};
  float content_x{0.0f};
  float content_y{0.0f};
  float content_width{0.0f};
  float content_height{0.0f};
  float host_width{0.0f};
  float host_height{0.0f};
  /// 嵌入物当前是否**真的看得到**（被可见区裁空时为假；见 `shell::ChildViewProbe`）。
  bool visible{false};
  /// 嵌入内容是否还在推进（网页死掉后它停住，而 `attached` 仍为真）。
  bool alive{false};
  /// 嵌入物最近一次上报的状态文本（诊断用；空 = 无）。
  std::string status{};
};

/// 宿主能力契约（`Element::host()` 返回它）。
///
/// ## 为什么是窄接口而不是 `void*`（历史与理由，别退回）
///
/// 原先这里存的是**类型擦除的 `void*`**，靠 `owner_as<T>()`（内部 `static_cast`）
/// 强转回来。它确实躲开了 `element.hpp ↔ ui_root.hpp` 的循环依赖，**但代价是
/// 类型系统完全失效**：转到错误类型是静默 UB，编译器（`-Wall -Wextra -Werror` 全开）
/// 一句话都不会说。
///
/// 现在换成**小契约 `HostFocus`**（与 `shell` 的 `WindowControl` 同一手法：
/// 上层/平台中立的窄接口下沉，实现方去实现它）：
///   · 依赖方向仍然单向（`ui_root` 包含 `element`，反向只认这个界面）；
///   · `static_cast` 到错类型这类错误**变成编译错误**；
///   · 没宿主时**如实返回 `false`**（“没宿主”与“要了但没成”两件事可分）。
///
/// 为什么最初只放“焦点”一个能力、不把 `UiRoot` 整个暴露成接口：界面越窄越好
/// （消费者只有一个：`CommandPalette::grab_focus`）。将来真需要更多宿主能力时，
/// 应**继续往这个界面加纯虚函数**，而不要退回 `void*`。
///
/// ## 设计纪律
///
/// 能力多时**返回端口指针**而不是平铺虚函数——`host_webview_port()` 就是范例：
/// 浏览器有十几条能力，平铺会让本类变成杂物间，且**每个实现（含每个测试桩）都得跟着加**。
/// 返回 `nullptr` = 宿主没有这个能力，组件自己退化为占位块。
class HostFocus {
 public:
  HostFocus() = default;
  virtual ~HostFocus() = default;
  HostFocus(const HostFocus&) = delete;
  auto operator=(const HostFocus&) -> HostFocus& = delete;
  HostFocus(HostFocus&&) = delete;
  auto operator=(HostFocus&&) -> HostFocus& = delete;

  /// 把键盘焦点转到 `element`（等价 `UiRoot::set_focus`）；不可聚焦/不在树上时为 `false`。
  virtual auto set_keyboard_focus(Element* element) -> bool = 0;

  /// 读宿主剪贴板（无剪贴板能力/读失败时为空串，不抢错）。
  /// 终端粘贴（`Ctrl+Shift+V`）等组件能力从这取——剪贴板是**平台**能力，
  /// 组件不该也不必知道后端长什么样（实现在 `UiRoot`→`Application`→backend）。
  [[nodiscard]] virtual auto host_clipboard_text() -> std::string { return {}; }

  /// **写**宿主剪贴板（2026-10-12 补齐写侧——此前只有读，组件的 copy 动作面
  /// 接了也没路可走：终端的 `invoke copy` 一直空转返回 false）。
  /// 返回 false = 无宿主/写入失败（调用方如实上报，不假装成功）。
  /// 与读侧对称地走窄接口：实现在 `UiRoot`→`Application`→backend
  ///（win32 真剪贴板 / headless 进程内模拟），headless 与有窗口行为一致。
  virtual auto host_set_clipboard_text([[maybe_unused]] std::string_view text) -> bool {
    return false;
  }

  /// **焦点环是否该画**（只在键盘导航时为真）。
  ///
  /// 为何放在宿主契约里而不是各组件自己推断：鼠标按下可聚焦元素**也会给焦点**
  /// （`UiRoot` 的 `MouseDown` 分支），而点一下就在按钮上留一圈边框不对
  ///——Windows / VSCode 的惯例都是「焦点环只在键盘导航时出现」。
  /// 而“上一次输入是键盘还是鼠标”是**全局**状态，组件无从知道；
  /// 分散实现必然漂移（看上去就是“有的按钮带框有的不带”）。
  /// 默认真：**没有宿主**时按“该画”处理（离屏/测试渲染保持旧观感）。
  [[nodiscard]] virtual auto focus_ring_visible() const -> bool { return true; }

  /// 焦点环可见性变了——组件据此重绘（`UiRoot` 在切换时调）。
  virtual void on_focus_ring_changed() {}

  /// 宿主的当前主题（样式系统 `apply_appearance` 立即落位用）。
  ///
  /// 为什么进契约：外观规则要解析成具体值（颜色/圆角/阴影），而真值源在
  /// UiRoot——元素自己不存主题（存了就是影子状态，换肤会分岔）。
  /// 无宿主/未实现时返回空指针（调用方退回内置亮色主题）。
  [[nodiscard]] virtual auto host_theme() const -> const Theme* { return nullptr; }

  /// 换主题（按内置主题库名字）。**唯一真切换点**：组件不自己实现换肤，
  /// 而是问宿主要——宿主的实现（Application）负责合成/持久化/通知。
  /// 名字无效/宿主不支持 → 返回 false（调用方如实回落，不假装成功）。
  virtual auto host_apply_theme(std::string_view /*name*/) -> bool { return false; }

  /// 挂一个浮层（模态对话框/菜单/主题选择器用）。返回 false = 无宿主/不支持。
  virtual auto host_add_overlay(std::unique_ptr<Element> /*overlay*/,
                                OverlayLayout /*layout*/) -> bool {
    return false;
  }
  /// 摘一个浮层（对应用户点击遮罩/Esc 关闭）。返回 false = 无宿主/不在场。
  virtual auto host_remove_overlay(Element* /*overlay*/) -> bool { return false; }

  /// **树结构变了**（挂入/摘下子元素）：命中链可能因此改变，而指针位置没动。
  ///
  /// ## 为何必须主动上报、不靠“事后发现”
  ///
  /// 元素被摘下的那一刻指针还指着它，而它将要被销毁。宿主若只在下次用到指针态时
  /// 用“还在不在树上”去猜，**地址复用**会把新元素认成旧元素（`clear_children` +
  /// 逐项重建正好是这个形态：新按钮常拿到刚释放的地址）——于是“指针仍在树上”
  /// 成立，悬浮被误判为没变（`update_hover` 的 `hovered_ == target` 早退），
  /// 新实例永远收不到 HoverIn。实测代价：相期的卡片列表刷新后，“悬停才显示”
  /// 的控件一直不出来，直到鼠标动一下。
  ///
  /// `removed` 区分“只是多了东西”与“有人被拿走了”：前者指针下的命中未必变
  ///（多一张卡不该让现有悬浮抖动），后者必然要作废旧答案。
  /// 默认空实现：不关心指针态的宿主（离屏/测试桩）无需理会。
  virtual void host_structure_changed(bool /*removed*/) {}

  // ── 原生子窗口嵌入（可选能力）──
  //
  // 供 `NativeHost` 组件使用：把外部程序的窗口嵌进界面里（典型：内嵌浏览器）。
  // 三个方法**全部默认降级**（`false` / 空操作），因此：
  //   · 不支持这个能力的后端（无头/未实现平台）上，组件退化为占位块；
  //   · 已有的桩宿主、离屏渲染、测试代码**一行不用改**；
  //   · 基础功能（布局/主题/事件/其余组件）**完全不受影响**。
  //
  // 这就是“WebView 不可用时不影响基础功能”的结构保证：降级是**默认路径**，
  // 而不是每个后端各自去写一遍 `if (不支持) return;`。

  /// 宿主是否能把外部窗口嵌进来。`false` 时组件不应尝试嵌入（也不会报错）。
  [[nodiscard]] virtual auto host_supports_child_view() const -> bool { return false; }

  /// 把外部窗口 `child` 嵌到当前布局里的 `bounds`（**逻辑像素**）。
  /// 返回 `false` = 无此能力 / 失败（调用方如实回落，不假装成功）。
  virtual auto host_adopt_child_view([[maybe_unused]] void* child,
                                     [[maybe_unused]] math::Rect bounds) -> bool {
    return false;
  }
  /// 布局变化时同步位置。未嵌入时为空操作（**不报错**：重排是常态）。
  ///
  /// `clip` = 从根到本组件**所有祖先的矩形与裁剪区的交集**（由 UI 层算好）。
  /// 为何必须传：平台子窗口由合成器直接上屏，**不遵守 `visible()` 也不遵守
  /// 祖先的 `clip_children`**——切页后它会盖在新页面上，滚出视口也不会消失
  ///（两条都实测到过）。语义：与 `bounds` 无交集 ⇒ 隐藏；否则只露交集。
  virtual void host_sync_child_view([[maybe_unused]] void* child,
                                    [[maybe_unused]] math::Rect bounds,
                                    [[maybe_unused]] math::Rect clip) {}
  /// 解除嵌入（**必须**把外部窗口还原成独立顶层窗口）。
  virtual void host_dismiss_child_view([[maybe_unused]] void* child) {}

  /// 嵌入物的自检读数（属性面与测试的数据源）。
  ///
  /// 为何需要：嵌入内容**不进 `capture`**，像素回归看不见它。
  /// 没有这组读数，“网页死掉了”会静默通过所有像素回归。
  [[nodiscard]] virtual auto host_child_view_probe() const -> ChildViewProbe { return {}; }

  /// 嵌入浏览器端口（`ui::WebView` 唯一的能力来源）。
  ///
  /// 为何返回指针而不是把十几条能力平铺成虚函数：能力有十几条（三种内容源 +
  /// 五种导航 + 三种控制 + 状态 + 事件），平铺会让本类变成杂物间，
  /// 而**每个实现（含每个测试桩）都得跟着加**。返回 `nullptr` = 本宿主没有这个能力
  /// ——组件自己退化为占位块，界面其余部分完全不受影响（与 `NativeHost` 同一契约）。
  ///
  /// 契约与折接见 `st/ui/webview_port.hpp`。
  [[nodiscard]] virtual auto host_webview_port() const -> WebViewPort* { return nullptr; }
};

}  // namespace st::ui

#pragma once

/// 组件画廊的页面模块。
///
/// 为什么从 `main.cpp` 拆出来：画廊要展示的组件已经从 4 个涨到十几个，
/// 内容量远超"装配一个应用"的量级。`main.cpp` 只保留**装配**（顶部栏/侧栏/滚动容器/
/// 状态栏/导航），页面内容在这里按页组织，一页一个函数、互不干扰。
///
/// 页面切换用 `Element::set_visible` —— 框架在 `measure`/`arrange`/`paint`/`hit_test`/
/// `semantics`/`visual` 六处全部尊重 `visible`，因此**隐藏页完全不参与布局与绘制**
/// （切页成本 = 标脏 + 重排当前页），比"销毁重建"简单得多，也不会让组件丢状态
/// （输入框里的文字、表格选中行在切页后仍然保留）。
///
/// 页面清单与组件分类在 `gallery_model.hpp`（数据），本头只声明"怎么建页"。

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include "audit.hpp"
#include "gallery_model.hpp"
#include "native_host_page.hpp"
#include "webview_page.hpp"  // `NativeHostSource`（页面的外部窗口来源端口）
#include "st/math/geometry.hpp"
#include "st/ui/element.hpp"
#include "st/ui/window_control.hpp"  // 窗框动作出口（`ui::WindowControl` 端口）

namespace st::ui::dsl {
class Component;   // 声明式组件基类（完整定义在 st/ui/dsl.hpp；头文件只用到指针）
}

namespace gallery {

/// 页面 → 应用的回调：页面内交互统一经它触达应用。
/// 页面不持有 `Application`（否则每个页面都要知道应用生命周期），只依赖这几个函数。
///
/// `add_overlay`/`remove_overlay`/`viewport` 是给浮层类组件（下拉面板、对话框、提示）的：
/// 这些组件**必须挂到内容之上**（`UiRoot` 的叠加层），自己无法完成——
/// 组件提供 `overlay_host` 注入点，宿主由调用方提供，这里就是那条链路。
struct PageHooks {
  std::function<void(std::string)> set_status{};
  /// 挂叠加层：应用侧统一用 `FillViewport` 形态（模态遮罩/宽通知的标准形态）——
  /// 组件在 `arrange` 里自行定位卡片，页面不必注入视口尺寸。
  std::function<void(std::unique_ptr<st::ui::Element>)> add_overlay{};
  std::function<void(st::ui::Element*)> remove_overlay{};

  /// 页面声明"这里需要应用提供的实时值"（如 `"dpi"`、`"frames"`）。
  ///
  /// 为什么用"注册"而不是让页面去反查指标：页面不该知道 `Application` 的存在，
  /// 应用也不必知道每个值被画在哪一个控件上——两边各自只做自己知道的事。
  std::function<void(std::string_view field, std::function<void(std::string)> setter)>
      register_runtime_field{};
  /// 窗口控制端口（自绘窗框的动作出口；无窗口时为空）。
  ///
  /// 为什么走 hooks 而不是让页面自己去找：与 `add_overlay` 同理——页面不该知道
  /// `Application`/后端的存在（依赖方向），而"窗口动作"只有应用側做得了。
  st::ui::WindowControl* window_control{nullptr};

  /// 原生子窗口嵌入的外部窗口来源（页面用它判断"能不能嵌" + 取窗口 + 读状态）。
  ///
  /// 为何直接给一个接口指针而不散成几个 `std::function`：它有状态——一个长驻
  /// 子进程、一个窗口句柄、"还在不在跑"的判定。散成回调后页面得自己拼
  /// "进程死了但句柄还在"这种组合，很容易拼漏。
  ///
  /// 为何不让页面直接问后端：与 `window_control` 同理——页面不该知道
  /// `Application`/`shell::Backend` 的存在（依赖方向）。而且这个能力在无头/
  /// 未实现平台上是**不存在的**，页面必须能分辨"没能力"与"嵌失败"。
  /// `nullptr` = 应用没提供（页面如实显示"没有可用的外部窗口来源"）。
  NativeHostSource* native_host_source{nullptr};

  /// 自检台用：跑一次全量自检并返回报告（应用侧实现——自检要遍历真值树，而页面
  /// 对 `UiRoot` 只有只读能力；由应用把结果交给页面）。
  ///
  /// 返回**结构化报告**而不是一段文本：页面要按状态上色、按行展开明细，拿字符串
  /// 就只能再解析一次（两处各自格式化同一份数据必然漂移）。
  ///
  /// `AuditReport` 在页面翻译单元里是**不完整类型**：页面只把它传给
  /// `fill_audit_view`（实现在 audit_page.cpp，那里 include audit.hpp）。
  std::function<AuditReport()> run_self_audit{};
  /// 跳到另一页（页内快捷入口用；应用侧实现导航）。
  std::function<void(std::string_view)> navigate{};
};

/// 构建每个页面（返回 `Panel(Column)`，自带页标题与内边距）。
[[nodiscard]] auto build_page(std::size_t index, const PageHooks& hooks)
    -> std::unique_ptr<st::ui::Element>;

/// 声明式页的演示组件（≈ Compose / ArkTS 风格的 |状态 → 界面| 描述）。
///
/// 为什么由页面模块提供：它演示的就是「一页界面用声明式写」——内容属页面，
/// 挂载由应用做（`dsl::mount_into`，见 `apps/gallery/main.cpp`）：
/// 框架的 `mount()` 是**单根语义**（会替掉 `UiRoot::content()`），
/// 而 gallery 是手搭壳 + 声明式页的混合形态。
[[nodiscard]] auto make_declarative_component() -> std::shared_ptr<st::ui::dsl::Component>;

/// 属性面页的检视器组件：由 `PropertyDesc` 自动生成面板（`build_property_panel`）。
///
/// 与声明式页同一个形态（`mount_into` 子树），但它演示的是**属性自省**这条通路：
/// 组件声明 `property_schema()` → 面板自动出现对应控件 → 改动经 `set_property`
/// 落到真实元素上。属性面有缺口时这一页会**看起来不对**（控件缺失/拖了不动），
/// 这正是它作为质检点的价值。
[[nodiscard]] auto make_property_inspector_component() -> std::shared_ptr<st::ui::dsl::Component>;

/// 同上，但把“找靶子元素 / 读 / 写”接到指定的 UI 根上（应用侧建页后调用）。
///
/// 为何要单独一个重载：`make_property_inspector_component()` 不带参数（保持
/// "页面工厂"的纯净形态），而检视器要真的读写树上的元素，必须拿到 `UiRoot`。
[[nodiscard]] auto make_property_inspector_component(st::ui::UiRoot& root)
    -> std::shared_ptr<st::ui::dsl::Component>;

/// 把一份自检报告填进自检台页的表格与明细容器（`audit_page.cpp` 实现）。
///
/// 为什么不在 `build_page` 里就地填：自检要**随叫随跑**（按钮/控制通道都能触发），
/// 而建页只发生一次。所以把"渲染报告"单独抽成一个函数，建立时跑一次、
/// 之后每次跑完再调一次——同一段渲染逻辑，两个时机。
void fill_audit_view(st::ui::Element& table, st::ui::Element& details,
                     const AuditReport& report);

/// 把自检台页的按钮与两个容器接上（建页后由应用调用）。
void wire_audit_page(st::ui::Panel& page, const PageHooks& hooks);

/// 构建「能力矩阵」页（三维 / 关系图 / 异步排版 / 通路型能力的展位与判据）。
[[nodiscard]] auto build_capabilities_page(const PageHooks& hooks)
    -> std::unique_ptr<st::ui::Panel>;

/// 构建「自检台」页（结论表 + 明细 + 跑一次按钮）。
[[nodiscard]] auto build_audit_page(const PageHooks& hooks) -> std::unique_ptr<st::ui::Panel>;

/// 构建「属性面」页（含靶子区、检视器宿主 `#property-host`、说明卡）。
[[nodiscard]] auto build_properties_page(const PageHooks& hooks)
    -> std::unique_ptr<st::ui::Panel>;

}  // namespace gallery

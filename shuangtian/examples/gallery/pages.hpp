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

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include "st/math/geometry.hpp"
#include "st/ui/element.hpp"

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
};

/// 页面清单（导航、切页、自动验证共用同一份，避免"两处各写一遍"走偏）。
struct PageSpec {
  std::string_view id{};        ///< 页面容器 id：`page-<id>`（语义树据此判断当前页）
  std::string_view icon{};      ///< 导航图标名
  std::string_view label{};     ///< 导航文字与页标题
  std::string_view subtitle{};  ///< 页副标题
};

inline constexpr std::size_t kPageCount = 5;

[[nodiscard]] auto page_specs() -> const std::array<PageSpec, kPageCount>&;

/// 构建每个页面（返回 `Panel(Column)`，自带页标题与内边距）。
[[nodiscard]] auto build_page(std::size_t index, const PageHooks& hooks)
    -> std::unique_ptr<st::ui::Element>;

}  // namespace gallery

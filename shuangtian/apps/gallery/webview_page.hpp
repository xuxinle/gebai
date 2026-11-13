#pragma once

/// @file
/// @brief gallery 的「嵌入浏览器（WebView2）」页。
///
/// 页面只做三件事：说明能力、放一块 `ui::WebView`、把自检读数贴出来。
/// 它**不**知道 `Application` 的存在——能力全部经组件的端口取
///（`WebView::supported()` / `state()` / `get_property()`），
/// 这正是 `PageHooks` 的既有约定（页面不知道应用/后端）。

#include <memory>

#include "st/ui/element.hpp"

namespace gallery {

/// 构建该页。
[[nodiscard]] auto build_webview_page() -> std::unique_ptr<st::ui::Element>;

}  // namespace gallery

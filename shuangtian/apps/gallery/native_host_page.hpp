#pragma once

/// @file
/// @brief gallery 的“原生嵌入”页：把外部程序的窗口嵌进界面里演示。
///
/// 页面本身只做三件事：说明能力、放一块 `NativeHost`、把自检读数贴出来。
/// “起浏览器 + 找它的窗口”这件事不在页面里做——它要 `process` / `shell` 两层，
/// 而页面不该知道 `Application`（`PageHooks` 的既有约定）。

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include "st/ui/element.hpp"

namespace gallery {

/// 外部窗口的所有者（应用侧装配，页面只读用它）。
///
/// 为什么用一个接口类而不是几个 `std::function`：它有**状态**——
/// 一个长驻子进程、一个窗口句柄、一个“还在不在跑”的判定。把这三样散成回调，
/// 页面侧要自己拼“进程死了但句柄还在”这种组合，很容易拼漏。
class NativeHostSource {
 public:
  virtual ~NativeHostSource() = default;

  /// 本机此刻能不能做嵌入（后端支持 + 平台有实现）。
  [[nodiscard]] virtual auto supported() const -> bool = 0;

  /// 取一个可嵌入的外部窗口句柄（`nullptr` = 还没有）。
  ///
  /// 实现方**只负责取**：首次调用时起进程、等窗口出现；后续调用直接给缓存
  /// （外部窗口没了就重新起）。为什么允许“起进程”而不只是“查句柄”：
  /// 一个演示页要能自己动起来，否则用户得先在别处手动开个浏览器再把句柄告诉它。
  [[nodiscard]] virtual auto acquire() -> void* = 0;

  /// 人类可读的状态（“正在启动浏览器…”/“已就绪”/“后端不支持”）。
  [[nodiscard]] virtual auto status_text() const -> std::string = 0;
};

/// 构建该页。`source` 可为空——那时页面如实显示“没有可用的外部窗口来源”。
[[nodiscard]] auto build_native_host_page(NativeHostSource* source)
    -> std::unique_ptr<st::ui::Element>;

}  // namespace gallery

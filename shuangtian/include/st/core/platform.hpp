#pragma once

/// **平台级系统能力**（与 UI 无关的宿主交互）：目前只有「用系统默认程序打开 URL」。
///
/// 为何单独立一个模块而不是塞进 `st::process`：`process` 的语义是「**我们**起一个
/// 子进程并管它的输入输出」（构建驱动、终端 PTY），而「打开浏览器」是**把动作
/// 交给桌面环境**——我们既不关心它的输出，也不该等它结束（Windows 上
/// `ShellExecute` 根本不返回进程句柄）。两者的失败语义与生命周期都不同。
///
/// 平台实现集中在 `src/core/platform_shell.cpp`（Linux `xdg-open` / Windows
/// `ShellExecuteW` / macOS `open`）——业务代码只调这里的函数，不碰系统头
///（`CONVENTIONS.md` §10：系统 API 只能出现在 `platform_*` 里）。
///
/// 为什么框架要提供它：Markdown 的链接、终端里的 URL、关于页的官网链接
/// 都要「点一下就用系统程序打开」；没有这条通道，每个应用都会自己写一遍
/// `system("start ...")`——而那在 Windows 上会闪一个黑框、且路径/引号处理极不安全。

#include <string>
#include <string_view>

#include "st/core/error.hpp"

namespace st::platform {

/// 用系统默认程序打开 `url`（浏览器/邮件客户端等）。
///
/// 返回 false = 无法启动（无桌面环境 / 找不到 `xdg-open` / 系统调用失败）。
/// **不等**目标程序结束——它通常比我们活得久（浏览器窗口）。
/// 空 URL 直接返回 false（不做无意义的启动）。
///
/// 安全性：实现一律把 URL 作为**单个参数**传给系统 API（Windows 走
/// `ShellExecuteW`，不经 shell），因此不存在参数注入；调用方仍应只传
/// 自己构造/校验过的 URL（不要把用户输入的整行当成 URL）。
[[nodiscard]] auto open_external(std::string_view url) -> bool;

}  // namespace st::platform

#pragma once

/// 反馈组件（进度条 / 加载 / 徽标 / 标签片 / 头像 / 悬浮提示）
///
/// ## 这个头现在是**聚合头**（只 include 各子头）
///
/// 原先它把 6 个组件塞在一个文件里——那是「按需导入」做不到的根因之一：
/// include 一个组件就等于 include 全部，依赖关系也藏在这份清单里而不是组件自己身上。
///
/// 现在**一组件一头**（规则见 `st/ui/components/README.md`）。本文件保留为**便利聚合**：
/// - 想用一个组件：include 它自己的头（如 `st/ui/components/button.hpp`）——
///   只有它进产物；
/// - 想要"全部基础组件"：include 本头（开发期方便，交付前建议改回按需）。
#include "st/ui/components/progress_bar.hpp"
#include "st/ui/components/spinner.hpp"
#include "st/ui/components/badge.hpp"
#include "st/ui/components/chip.hpp"
#include "st/ui/components/avatar.hpp"
#include "st/ui/components/tooltip.hpp"

// 自注册锚点随各子头到达（`ST_UI_COMPONENT`）；本文件不再持有任何类型定义。

#pragma once

// @deprecated 已提升为公开头 `st/app/icon.hpp`（外部工程也需要它）。
//
// 保留这个转发头只是为了不让既有调用点一次性全改——迁移只需把
// `#include "../app_icon.hpp"` 换成 `#include "st/app/icon.hpp"`。
//
// 为何当初放在这里：它最早只是几个内置应用互相抄的一段代码，放在 `apps/` 下够用。
// 直到写独立工程时才发现那三行依赖三个只在内仓可见的约定
// （`uint8_t` vs `char` / 注册必须早于首帧 / `svg:` 前缀），
// 而外部工程根本 include 不到 `apps/` 下的相对路径——只能自己再抄一遍并重踩。
#include "st/app/icon.hpp"

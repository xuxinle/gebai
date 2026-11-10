---
title: 跨平台 · 写代码时逐条自查
summary: 目标是三平台；本机看不出跨平台问题，所以规则要写进红线而不是靠发现。
---

# 跨平台

## 四条红线（`CONVENTIONS.md` §10）

1. **系统头/平台宏/平台 `char*` API 只能出现在 `platform_*` 里**。业务代码需要系统能力时，
   先加平台无关封装（如 `process::current_id()`），不要在业务里直接调 `getpid`/`dlopen`。
2. **路径一律 UTF-8 且经 `st::fs`**。不要手写 `base + "/" + leaf`，不要 `ifstream(std::string)`
   （Windows 按 ANSI 解释，中文路径必坏）。
3. **入口用 `ST_MAIN(fn)`**（Windows 的 `argv` 是 ANSI，中文参数会乱）。
4. **不用 `long` 表示字节数/偏移**（Windows 上 32 位）；打印一律 `std::format`。

## 交叉编译：先看宿主，别机械套

判据是「本机原生编译器的目标三元组是不是那个平台」（`g++ -dumpmachine`）。

| 宿主 | 情况 |
|---|---|
| **Windows**（本仓库开发机） | 原生工具链本就是 mingw64（`x86_64-w64-mingw32`），普通 `st build` 已经在编 Windows 分支；再跑 `--toolchain=mingw` 是同一件事做第二遍，白花几分钟 |
| **Linux** | 本机编不到 Windows 分支，`--toolchain=mingw` 是必做项 |

真正该问的是：**除了宿主，还有哪个平台的分支我改过却没编过？**

## 三平台的坑（各自最贵的一条）

| 平台 | 坑 |
|---|---|
| Windows | 分层窗口（`WS_EX_LAYERED`）截图是黑；`PrintWindow` 会触发重绘把瞬态修好；`%VAR%` 在 `cmd /c "set X=1 && app"` 里不展开 |
| Linux | 无显示服务时后端要能回退无头；X11 上屏路径依赖 MIT-SHM（不可用时性能落差明显） |
| macOS | 后端能力差异（合成器语义不同），像素口径要按平台分别标定 |

## 更细

- 详细版 + 逐条自查清单：`docs/cross_platform.md`
- 权威条款（含"机械套用 mingw 三次全是空转"的教训）：`CONVENTIONS.md` §10 第 6 条
- 后端与上屏路径设计：`DESIGN.md` §4.6（`shell`）· §8.3（GPU）

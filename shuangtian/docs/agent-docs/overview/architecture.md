---
title: 架构 · 分层、目录与依赖方向
summary: 唯一硬约束是"下层不得依赖上层"；模块按层号排布，另有 8 处已知违规与它们为什么不改。
---

# 架构：分层与目录

## 唯一硬约束

**下层不得依赖上层**。层号越小越底层；同层之间可以互相依赖，跨层只能向下。

| 层 | 模块 | 一句话职责 |
|---|---|---|
| 0 | `core` | 基础设施：字符串、文件、日志、进程、线程、时间、内存、平台适配 |
| 1 | `math` `codec` `xml` `net` `edit` `md` `lsp` `pkg` | 数学/几何 · 编解码（PNG/JPEG/…）· XML 树 · 网络与帧协议 · 编辑模型（撤销栈/文本模型）· Markdown 解析与高亮 · LSP 客户端 · 包管理与构建驱动 |
| 2 | `text` | 字体解析与整形、文本布局、字形栅格化（依赖 raster 的**接口**） |
| 3 | `raster` `ext` | 光栅画布与绘制原语（软件为真相源，GPU 对齐它）· 扩展（JSON / 脚本） |
| 4 | `control` | 控制通道（协议 `st-control/1`，TCP） |
| 5 | `ui` | 元素树、布局、样式、组件库、声明式、主题、窗口框架 |
| 6 | `app` `shell` | 应用骨架（启动、帧循环、参数、图标）· 窗口后端（Win32 / X11 / Wayland / 无头） |
| 8 | `probe` `test` | 工具性质，不受层序约束 |

**`st_core` 库目标** = 层 0–1 里不（传递地）触及 `ui`/`raster` 的九个模块（core / codec / math / net /
xml / test / edit / lsp / pkg）——给"只要逻辑、不要界面"的程序用。

## 目录快速对照

```
include/st/<模块>/…    公开头（模块名即目录名）
src/<模块>/…           实现（与头一一对应，文件名同名）
src/ui/components/…    组件实现（一个组件族的 .cpp 可含多个小组件）
apps/<应用>/…          示例应用（同时是验收靶场）
tests/…                单元测试（每条 ST_TEST 名字就是一条不变量）
tools/…                探针与脚本（诊断、量测、e2e 驱动）
docs/                  文档（本树在 docs/agent-docs/）
```

## 8 处已知违规（都**不打算改**，理由已论证）

| 违规 | 处数 | 为什么不改 |
|---|---|---|
| `control(4) → ui(5)` | 5 | 控制通道的价值就是"能操作真实元素"；抽接口等于复制一份 `Element` 公开面，制造必然漂移的镜像 |
| `text(2) → raster(3)` | 2 | 一处是纯数据（`Paint`/`BlendMode`），一处是冷路径且带 LRU 缓存——都不是性能问题 |
| `md(1) → text(2)` | 1 | 语法高亮需要文本类型；抽象出去只是归属调整，没有收益 |

> 判断标准（三条同时成立才做归位）：① 该文件不引用目标层任何符号；② 只依赖更底层；
> ③ 现状属**分类错误**而非真实需求。
> 完整论证与已做过的三次归位（json/async/Path）见 `docs/STRUCTURE.md`。

## 三条必须知道的跨层约定

1. **绘制只认接口**：UI 层只调 `raster::Surface`（`virtual` 的 `blend_coverage_bitmap` 等），
   因此同一条绘制路径既能落 CPU 也能落显卡——**改绘制不要绕过 `Surface`**。
2. **平台差异只许出现在 `platform_*`**：系统头、平台宏、平台 `char*` API 只在那里；
   业务代码要系统能力先加平台无关封装（如 `process::current_id()`）。
3. **路径一律 UTF-8 且经 `st::fs`**：不要手写 `base + "/" + leaf`，也不要用
   `ifstream(std::string)`（Windows 上按 ANSI 解释，中文路径必坏）。

## 更细

- 每模块的接口清单：`DESIGN.md` §4（`core`/`raster`/`text`/`md`/`ui`/`shell`/`control`/`pkg` 逐节）
- 分层违规实证与复现：`docs/STRUCTURE.md`
- 当作通用 C++ 库用：`docs/AS_LIBRARY.md`
- 跨平台红线逐条：`overview/cross-platform.md`

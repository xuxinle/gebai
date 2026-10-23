# 歌白（GEBAI Agent）设计文档

> **融合旧世界 IT 的所有技术，打造新世界智能的躯体。**
>
> 本文件是**索引与核心不变式**。完整正文按主题拆为分册（见下方地图）——
> **章节号在所有分册与主文件之间保持一致**：源码注释与测试按 `DESIGN.md §x.y` 引用章节号，
> 拆册不改号（改号会静默打断论证链，见 `shuangtian/docs/README.md`「章节号是接口」）。

## 读法：先看你要回答什么问题

| 我要知道…… | 去哪 |
|---|---|
| 项目定位、GEBAI_HOME、技术栈、包结构 | [`design/01-overview.md`](design/01-overview.md) |
| 核心不变式、运行形态、核心模块接口、子Agent 扩展机制、子会话运行 | [`design/02-architecture-core.md`](design/02-architecture-core.md) |
| 某个内置子 Agent 具体怎么工作（工具表/审批/适用场景） | [`design/02b-builtin-agents.md`](design/02b-builtin-agents.md) |
| 对外动作怎么抽象到渠道（发消息/提工单/写记忆同一入口） | [`design/02-architecture-core.md`](design/02-architecture-core.md)「对外动作：渠道抽象」 |
| 功能列表、桌面端、构建发布、实施路线、测试策略、常量参考 | [`design/03-features.md`](design/03-features.md) |
| 快速上手与命令 | [`README.md`](README.md) |
| 编码约定与提交要求 | [`AGENTS.md`](AGENTS.md) |
| 霜天框架（C++ 原生 UI / 裁云 / 镂月） | [`shuangtian/DESIGN.md`](shuangtian/DESIGN.md) |

## 核心不变式（完整论证见分册）

- **能力外置、权力内守**：计算、协议、工具实现可外置到任意语言、任意进程；但**裁决权留在 TS 引擎**——工具可见性、审批姿态、安全模式、嵌套与深度规则、会话真相、进程生命周期全由引擎判定。
- **智能无状态、责任在智体**：模型每次调用都是白纸，上下文由智体奉上；需跨轮次/跨会话保留的结论写入文件或会话记录。
- **代码即一切**：子 Agent 文件、工具实现、系统提示词本身就是能力载体——可 diff、可测试、可回滚。不引入 memory / skill 这类运行时注入的不稳定能力。
- **测试是唯一准入凭证**：任何自我修改必须过 `test` / `typecheck` / `lint` 三件套，失败即回滚。

## 分册清单

| 分册 | 主题 | 行数 |
|---|---|---|
| [`design/01-overview.md`](design/01-overview.md) | 定位、`GEBAI_HOME`、技术栈、软件包结构 | ~434 |
| [`design/02-architecture-core.md`](design/02-architecture-core.md) | 架构：不变式、运行形态、服务端、模块接口、主循环、多模态、隔离、总Agent、子Agent 机制、子会话、自我优化、会话管理、配置、文件工作台、任务与待办、工具执行、上下文保护、消息模型 | ~1695 |
| [`design/02b-builtin-agents.md`](design/02b-builtin-agents.md) | 内置子 Agent 逐个详解（工具/审批/依赖/适用） | ~757 |
| [`design/03-features.md`](design/03-features.md) | 功能列表、桌面端架构、构建与发布、实施路线、测试策略、常量参考 | ~1242 |

## 维护约定

1. **改行为必须同步回写本档与对应分册**——文档与代码不一致属缺陷。
2. **章节号不重编号**：源码注释（如 `shuangtian/include/st/text/text.hpp` 引用 `§4.3.7.17`）与测试按号寻址；需要新增章节时**追加**，不要重排。
3. **数字必须实测**：子 Agent 数、工具数、测试文件数这类计数随代码变化，写入前先核对；宁可写「未测」也不填猜测值。
4. **产物纯净**：不留「何时发现/修复了什么」这类历史痕迹——缘由归 git 提交说明与自我优化 journal。

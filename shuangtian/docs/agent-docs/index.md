---
title: 霜天开发文档（智能体入口）
summary: 唯一入口。按"我要做什么"找到第一份该读的页，再顺着每页末尾的「更细」逐层下钻。
---

# 霜天开发文档 · 入口

**这份文档是给"要动手"的人/智能体写的**：每一项能力都落在一个**路径**上，用
`shuangtian_doc(path="…")` 读它，读完页尾会给出更深一层的路径。

两条约定：

1. **路径是本树的唯一键**。读文档只写路径（相对本文件所在目录，如 `overview/architecture.md`），
   不要再猜文件系统位置。
2. **一页只答一件事**。答案不在这一页时，页尾「更细」一定给出下一跳；不必通读整个目录。

## 30 秒：我该读哪一页

| 我要…… | 读这里 |
|---|---|
| 第一次接触，先搞清这套东西是什么、能干什么 | `overview/introduction.md` |
| 知道代码怎么分层、模块怎么放、依赖往哪边走 | `overview/architecture.md` |
| 建工程 / 构建 / 跑测试 / 打 lint | `overview/build-and-test.md` |
| 用控制通道让程序动起来（tree/find/get/set/invoke/capture） | `overview/control-protocol.md` |
| 写界面（声明式 / XML+JS / 手搭三条路怎么选） | `overview/ui-writing.md` |
| 写属性、读属性、通用属性有哪些 | `overview/properties.md` |
| 挑一个现成组件（按钮/表格/编辑器/终端…） | `components/index.md` |
| 收单前自查该验什么 | `overview/verification.md` |
| 别重蹈覆辙（踩过的坑） | `overview/pitfalls.md` |
| 三维场景 / 裁云 / 镂月 | `overview/three-d.md` |

## 本树的形状

```
（本文件）        入口：意图索引 + 约定
overview/         横向能力，每个主题一页（架构/构建/协议/界面/属性/验收/坑）
components/
  index.md        组件总表（按用途分组，带"何时用"）
  <族>.md         族页：同类控件怎么选、组合套路、坑
  <族>/<组件>.md  组件页：构造/属性面/动作面/源码位置/逐条取舍
```

- `overview/` 与 `components/<族>.md` 是**手写**的（写给"要做决定的人"）。
- `components/<族>/<组件>.md` 是**从源码生成**的（`tools/agent_docs_gen.py`），
  事实永远是当前代码的事实；改代码后重跑一次即可同步，不要手改这些页。

## 与旧文档的关系（别在两边各读一半）

| 你要找的东西 | 在本树 | 在旧文档（更全，但长） |
|---|---|---|
| 组件怎么用、有哪些属性 | `components/` | `DESIGN.md` §4.5（`ui` 层逐节） |
| 为什么这样设计、踩过什么坑 | `overview/pitfalls.md`（场景索引） | `docs/PITFALLS.md`（按场景）· `DESIGN.md` §8.2（缺陷清单） |
| 完整接口/协议字段 | `overview/control-protocol.md`（够用版） | `DESIGN.md` §6（权威版） |
| 还没做什么 | —— | `docs/BACKLOG.md` 待做段 |

> 本树是**入口与路由**，不是旧文档的替代品。它把"要读哪一节"这条路径固定下来，
> 免得每次都在 60 万字的 `DESIGN.md` 里重新找一遍。
> 冲突时**以旧文档与代码为准**（本树是摘要，摘要有滞后风险）。

## 维护（改框架就必须动文档）

本树分**两层**，职责不混：

| 层 | 谁维护 | 何时动 |
|---|---|---|
| `components/<族>/<组件>.md` | 生成器 [`tools/agent_docs_gen.py`](../../tools/agent_docs_gen.py) | 组件的属性面/动作面/构造/头注释变了 → **重跑生成器**（手改会被静默覆盖） |
| `index.md` · `overview/` · `components/index.md` · `components/<族>.md` | 手写 | 能力/流程/约定变了就改（生成器不碰这些） |

两道只读校验（跑一下就知道有没有漂）：

```sh
python tools/agent_docs_gen.py --check        # 生成层与源码是否一致
python tools/agent_docs_normalize.py --check  # 每一跳链接能否真解析
```

再往上一层：新增文档还要在 `docs/README.md` 的地图上登记（否则 `docs_map_covers_every_file` 会红）；
本树自身也在测试里被钉住（无孤儿页、组件全覆盖、族页能下钻到每个组件页、无页超预算）。

**分工原则**（避免两处同义漂移）：会随代码变的**机械事实**不写进提示词，一律走本树；
**开工即用、不能查也不能错**的纪律留在提示词；**长篇方法论与历史取证**（`docs/perceptual_changes.md`、
`docs/PITFALLS.md`、`docs/BACKLOG.md`…）本树只做索引。

## 下一跳

- 不知道怎么读这套框架 → `overview/introduction.md`
- 已经在写代码，只想知道规则 → `overview/properties.md` · `components/index.md`

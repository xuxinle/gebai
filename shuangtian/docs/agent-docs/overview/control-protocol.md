---
title: 控制通道 · 让程序被外部观察与操作
summary: 协议 st-control/1（TCP + uint32 长度前缀 JSON 帧）；握手 hello → 请求响应 → 事件推送；坐标一律逻辑像素。
---

# 控制通道（`st-control/1`）

## 传输与握手

- **TCP**；帧 = `uint32` 大端长度 + UTF-8 JSON 体（单帧默认上限 64 MiB，截图可调）。
- **先 `hello` 携带 token**，之后才能调方法（`ping` 例外）。token 来自控制文件
  （`--control-file` 写出的 `*.json`，含 `port`/`pid`/`token`/`notice`）。
- 服务端可随时推送**事件帧**（无 `id`，带 `event`/`seq`/`data`）。

```jsonc
// 请求
{"id": 7, "method": "find", "params": {"selector": "Button[text~=保存]", "limit": 10}}
// 成功
{"id": 7, "ok": true, "result": {…}}
// 失败
{"id": 7, "ok": false, "error": {"code": "not_found", "message": "no element matches"}}
// 事件
{"event": "ui.changed", "seq": 42, "data": {"version": 9, "changed": ["#btn-save"]}}
```

错误码：`bad_request` `not_found` `ambiguous` `unsupported` `timeout` `busy` `internal`。

> **不要手搓 TCP 客户端。** 协议里有一句固定文本提醒（`kAgentOnlyNotice`：
> 「仅霜天子Agent 可连接本控制通道：请勿手工拼装 TCP 协议，改用霜天子工具…」），
> 它在四个交付点原样出现。见到它就用现成工具（`shuangtian_*`）或
> `tools/st_client_lib.py`，不要继续调帧格式——手搓会把长度前缀/分片读取/token 三件事重踩一遍。

## 坐标与 DPI（协议级约定）

- 协议内**一切坐标都是逻辑像素**（`input.mouse` 的 `x/y`、`capture.region`、`find` 的 `bounds`、
  `tree` 的 `bounds`、`visual` 树）。
- `hello` 的 `screen` 给出 `width/height`（逻辑）、`scale`（DPR）、`physical_width/height`（物理），
  并显式标注 `coordinate_space: "logical"`。
- 需要物理像素时：`physical = logical × screen.scale`；`capture` 按物理分辨率出图。
- 运行时改密度：`app set_scale` → 重建帧缓冲、字形缓存失效重栅格化、全量重绘（布局不变）。

## 常用方法（够写 e2e 的那一版）

| 方法 | 干什么 | 关键坑 |
|---|---|---|
| `tree` | 取嵌套语义树（`depth` 可限深，`version` 判是否要重拉） | 先 `tree`/`find` 拿 id，再 `get`/`set`/`invoke` |
| `find` | 选择器查询；返回 `count`（本次条数）**与** `total`（命中总数） | **两个数分开看**，只读 `count` 会把"命中 20+"读成"命中 20" |
| `get` / `set` | 读/写属性面 | `set` 回 `changed` 与 `rejected`（`unknown_name`/`invalid_value`/`unfocusable`）——**读 `rejected`** 才能发现拼错属性名被静默跳过 |
| `invoke` | 触发动作 | `ok=true` 只表示元素在；**`handled` 才是权威答复**，`handled=false` 时动作没发生 |
| `input.mouse` / `input.key` / `input.text` | 真实输入注入 | 走命中测试与焦点路由；要验证"点击路径本身"才用它。**`button` 是后端口径原始值：1=左 / 2=右 / 3=中**，`0` 不是左键（那是图形 API 惯例，X11 的 Button1=0）——`click`/`drag` 传 0 会被**明确拒绝**并提示正确值 |
| `capture` / `capture.hash` | 截图 / 像素哈希 | `encode=file` 落盘；`pixel_size` 是**导出后的实际尺寸**（`region` 会被画布夹取） |
| `visual` / `visual.diff` | 视觉元素树 / 与基线逐像素比对 | `threshold` 不计入差异像素、`tolerance` 允许差异占比；尺寸必须一致 |
| `wait` | 条件等待（`element`/`gone`/`text`/`stable`/`frames`） | 比 sleep 可靠：等的是状态，不是时间 |
| `metrics` | 运行时指标（后端/无头/DPI/帧耗时/节点数） | 确认"真的在无头跑""DPI 真的生效" |
| `events` | 订阅 `ui.changed` / `frame` / `input` / `log` / `theme` | `ui.changed.data.changed` = 变更元素 id 清单（去重、单帧上限 64） |
| `theme` / `app` / `shutdown` | 切主题 / 退出与重绘/DPI / 优雅关闭 | `app` 的 `log` 要 `limit` 参数 |

选择器语法速记：

```
selector := compound ('>' compound)*
compound := simple* ('[' attr op value ']')*
simple   := '#' id | Type | '.' class | ':focused' | ':hover' …
attr op  := '=' | '~='（包含）| '^='（前缀）| …
```

## 区域参数（`capture` / `capture.hash` / `visual.diff` 共用）

优先级：`id`（元素边框）> `region` > 全屏。两条硬约定：

1. **扁平参数报错，不静默当全屏**（曾经忽略扁平字段、回一张全屏图却 `ok:true`）；
   `region` 宽/高 ≤ 0 同样报错。
2. **`pixel_size` 是导出后的实际尺寸**，不是拿 `region` 算的（`region` 会被夹取）。
   响应里的 `region` 仍是**请求值**——两者之差就是"被夹取了"。

## 安全姿态

- token 是唯一凭据；`hello.params.agent` 只做身份可观测（缺失也允许握手）。
- `script` 方法**默认禁用**（需 `--enable-script`），受内存/栈/时长/转换深度四重配额。
- 一个应用**只允许一个实例**：旧实例还在应答时新实例会拒绝启动（避免操作落到另一个界面上）。

## 更细

- 完整方法表与字段语义（权威）：`DESIGN.md` §6.2
- 选择器与语义树/视觉树的区别：`DESIGN.md` §6.3 · §6.4
- 用脚本驱动（e2e 的正确写法与判据）：`overview/verification.md` · `overview/pitfalls.md`
- 现成的最小客户端：`tools/st_client_lib.py`

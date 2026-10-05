# 霜天结构优化 — 进度与交接

> 分支：`optimize/structure-2026`（基于 `4b656d5`）。基线快照见 `docs/OPTIMIZATION_BASELINE.md`。
> 每阶段独立提交，可单独 `git revert`。

## 已完成

### P0 基线固化
- 度量探针与门禁基线落盘 `docs/OPTIMIZATION_BASELINE.md`。
- 门禁：700 测试 / 0 lint 违反 / 增量构建 9.5s。

### P2 巨型函数拆分（进行中：2/5，均为收益最高项）

| 目标 | 改动前 | 改动后 | 提交 |
|---|---|---|---|
| `text/highlight_builtin.cpp` | **511 行单函数**（占全文件 93%） | 30 个具名函数 + 一份可读清单 | `3dde940` |
| `control/server.cpp` `handle()` | **840 行** if 链，23 个方法 | **29 行**纯分派 + 22 个方法处理函数 | `fb28ba2` |
| `pkg/build.cpp`（345 行 compile_units） | 未动 | — | — |
| `pkg/lint.cpp`（check_mutable_globals） | 已随 P1④ 重写（343 行 → 拆出精确判据） | 部分完成 | `15c879f` |
| `ui/components/code_editor.cpp` | 未动 | — | — |

**零行为差异的证据**：语言表用探针逐字 diff（30 种语言的全部字段完全一致）；
`handle()` 靠 8 个协议端到端用例逐一单跑 + 全量 713 用例。

**先补护栏再动结构**：拆 `handle()` 之前先加了「能力清单 ↔ 实现」漂移护栏
（`ac29735`）——那个 840 行函数里有两套「有哪些方法」的真源（if 链 + `hello` 手写的
capabilities 19 项），会静默漂移。护栏按 §7.1 验证过真能抓住缺陷（注入假方法 → 当场红灯）。

### P3 组件类型身份单一真源（提交 `ee700f9`，**收益最大**）

| | 改动前 | 改动后 |
|---|---|---|
| 类型身份 | **两套独立表**（38 行 `type_name` 特化 + 42 行 `make_element` if 链）互不相识 | 一份清单 `ST_COMPONENT_LIST`，两处展开 |
| 新增组件 | 改 **3 处**（漏一处即静默失效） | 加 **1 行** |
| 一致性 | 无 | 4 个单测钉住（名称唯一 / 构造结果 `type()` 匹配 / 未知返回空 / `custom<T>` 端到端）|

静默失效的具体形态：漏改特化表 → `custom<T>` 报「未注册」；漏改 if 链 → 返回空 →
**声明式节点凭空消失**（界面缺块而不报错）。编译器两者都不提醒。

### P1 真实缺陷 + 纪律缺口（提交 `15c879f`）

| # | 问题 | 修法 |
|---|---|---|
| ① | `dsl.cpp:42` `active_composers` 无锁遍历（迭代中 erase = UB） | 容器加锁 + 快照遍历；`notify_state_written` 三线程分流（重组线程/UI 线程直接标脏，其他线程投递回 UI 线程——`pump_async` 排在 `reconcile` 之前，仍「下一帧可见」）。**主线程行为逐位不变**（断言数与基线完全相同） |
| ② | `highlight.cpp:730` 全局语言注册表作为函数内静态对象 → 跨 TU 析构顺序未定义 | 进程级永不析构单例（`new` 不 delete）+ 说明为何这样是标准做法 |
| ③ | `CONVENTIONS.md:315` 声称 lint 检查禁用 include，**实现里没有** | 新增规则 **L14**（系统头 + `dlopen`/`dlsym` 仅限 `platform_*`）；`entry.cpp` 走**清单** `lint.exempt` 具名豁免 |
| ④ | L8 只判「花括号深度 0」→ **匿名命名空间里的全局全部漏网**（L8 近乎失效） | 改用作用域标签栈（只由 namespace 组成即全局；+ 函数内 `static`）；三条误报防线（净代码视图 / 签名续行 / 前置作用域豁免）；`thread_local` 排除 |
| ⑤ | 附带发现：`dsl.cpp` **20 处** `static Element* none; return *none;` = 解引用空指针（UB） | 改为带类型名的即时失败 `fail_missing_element_factory(...)` |
| ⑥ | 仓库根 45 个 core dump（424 MB）+ `_mesh_block.txt` 残片 | 清理；git 历史可追溯 |
| ⑦ | `st.pkg` 的 `modules` 漏 `ext`（`st tree` 报 11 个模块，实际 12 个） | 补上；新增 `lint.exempt` 段 |

**验证**：708 passed / 0 failed / 17290 assertions；`st lint` 290 文件 **0 违反**（豁免 29 = 行内 26 + 清单 3）。
新增 10 个回归用例（L8 六条：匿名命名空间/具名命名空间/函数内 static/三条反例·豁免语义；L14 三条）。

**本轮最有价值的发现**：L8 收紧后**同一次扫描**从「0 违反」变成 6 条真问题——
包括 `active_composers`、`log.cpp` 的全局 sink、`test_runner` 的注册表。
这实证了一条判断：**「文档写着已检查、工具实际没检查」比「不检查」更危险**
（它让绿灯覆盖不到它声称覆盖的范围）。

## 待办（P2~P7）

按「收益/风险比」排序，建议逐阶段推进（每阶段跑 `st test` + `st lint` + `st build` 三件套）：

- **P2 巨型函数拆分**（纯机械，测试兜底）：`control/server.cpp` `handle()` 840 行 / 23 个 `method ==` 分派 → 方法表驱动；`text/highlight_builtin.cpp` 511 行单函数 → 语言数据分文件；`pkg/build.cpp` 匿名命名空间 1100 行拆分；`pkg/lint.cpp` `check_mutable_globals`（本轮已重写，可再拆）；`code_editor.cpp` 超长函数。
- **P3 组件类型身份单一真源**（A1，收益最大）：引入组件自注册描述符（name/factory/属性表/DSL 入口一处定义），消掉 `dsl.cpp` 里 `ST_DSL_TYPE` 表 + `make_element` 40 条 if 链 + 20 处 `fail_missing_element_factory` 入口的机械重复。**新增组件从「改 4 处」变「改 1 处」**。
- **P4 Element 瘦身 + owner 类型化**（A2+A3）：`sizeof(Element)` 432B → 目标 ~120B（低频状态移入惰性分配的 `ElementExtras`，公共 API 保持兼容）；`owner_` 的 `void*` + `static_cast` 逃生舱 → 类型化宿主查询契约（照 `WindowControl` 正面样板）。注意：改基类会触发 76~100 个 TU 重编。**风险最高的一项**——动的是 42 个组件共用的基类。
- **P2 剩余**：`pkg/build.cpp`（345 行 `compile_units`）、`ui/components/code_editor.cpp`（超长编辑函数）。
- **P5 后端接口分层 + json 解耦**（A4+B5）：`raster::Surface` 34 个纯虚按关注点拆为 `PathRasterizer`/`ClipStack`/`Compositor`/`PixelAccess`；`nlohmann/json.hpp`（25526 行）经 8 个公共头泄漏 → 前向声明 + 仅 `.cpp` 包含。
- **P6 文档同步 + 全量验证**：更新 `DESIGN.md`（新契约）、`CONVENTIONS.md`（已随 P1 更新 §8）、`docs/BACKLOG.md`；跑 `st test --san`、无头启动冒烟、控制通道连通性。
- **P7 收尾对比报告**：对象尺寸 / TU 重编数 / 巨型函数数 / 重复代码行数的前后对比。

## 复现命令

```bash
cd /workspace/gebai/shuangtian
./build/bin/st build st --profile dev && cp build/dev/bin/st build/bin/st   # 改了 lint/st 自身后需重建
./build/bin/st lint                  # 期望：291 文件 0 违反
./build/bin/st test                  # 期望：713 passed / 0 failed
./build/bin/st test lint             # 18 个 lint 用例（L8/L14 回归）
./build/bin/st test dsl_registry     # 4 个注册表一致性用例（P3 护栏）
./build/bin/st test capabilities_list_matches_implemented_methods   # 能力清单漂移护栏
./build/bin/st build gallery --profile dev && ./build/dev/bin/gallery --headless --frames 3 --control-port 0
```

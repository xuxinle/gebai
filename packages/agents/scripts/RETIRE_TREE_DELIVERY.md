# 交付说明：`retire mode=stop` 连带回收子孙进程树 + 回退态告警

## 交付物

| 文件 | 角色 |
|---|---|
| `packages/agents/src/agents/shengsheng/proc-tree.ts`（新增 596 行） | 判据与执行：进程表读取、环境标记归因、`planReap`（纯函数）、`reapProcesses`（SIGTERM→SIGKILL）、pid 复用防护、`detectRollbackResidue`（`*.revbak` 只读扫描） |
| `shengsheng.ts`（`collect` 的 `retire mode=stop` 分支 + 派工 env 标记） | stop 的完整语义接线 + `dry_run` 参数 + 残留告警 |
| `shengsheng.md` | 「收/退役」段落补 stop 的三件事与「无法定位必须如实报」 |
| `DESIGN.md` | `collect` 节补 `mode=stop` 的完整语义与三条硬边界 |
| `shengsheng-retire-tree.test.ts`（新增 17 条） | 纯函数层 / 真进程层 / 反向用例 / 干跑 / 回退态告警 |
| `scripts/reverse_verify_retire_tree.py`（新增） | 逆向验证两条（各自杀死一条硬约束） |
| `scripts/retire_tree_demo.ts`（新增） | 真场景取证脚本（可复现的原始回显） |

## 归因方式（为什么不是「会话进程 pid → 子孙」）

歌白里**没有「会话进程」**——所有会话的任务都跑在同一个服务进程里，子进程是**那个进程**的子进程，
「拿会话进程 pid 再往下爬」没有起点可用。故改用**环境标记**：

```
SHENGSHENG_WORKER_SESSION=<会话 id>   # 派工时写入 worker 会话 env，其 sh/py/js 子进程继承
```

读到该标记的进程必属该会话，读不到的必不属于；再叠一层**子孙递归**兜住 `env -i` 洗掉标记的。
**绝不按命令行/名字猜**——那正是「把全机同名的都杀了」的来源。

## 四条边界（每条都有用例兜）

1. **只杀该会话派生**：标记属别的会话、或无标记且不在其子孙里 → 一律不碰，并在回显里点名「看到了但为什么没杀」。
2. **永不误杀自己与宿主**：自身 pid + 祖先链 + 1 号进程进保护集（`protectLineage`）。
3. **pid 复用防护**：补杀前重读 `/proc/<pid>/stat` 第 22 字段（启动时刻），变了就当它已死——绝不把 SIGKILL 发给复用了该 pid 的无关进程。
4. **无法定位如实报**：非 Linux / 无 `/proc` / 权限不足 → 回显写「**无法定位**进程树」，**绝不静默当成「已清理」**。

## 验证证据

### 1. 单测（先红后绿）

- 新增用例**先确认红**：能力未实现时 `renderPlan` / `planReap` 不存在，`bun test` 直接报模块缺失（`17 fail` / 0 pass）。
- 实现后：**17 pass / 0 fail（73 expect）**。

### 2. 逆向验证（`reverse_verify_retire_tree.py`，两条各自杀死一条硬约束）

```
基线：17 passed / 0 failed
[通过] 只杀会话：不回收子孙进程树：回退后 1 条变红，命中期望 1 条
[通过] 去掉跨会话保护：别人的进程一并杀掉：回退后 1 条变红，命中期望 1 条
全部 2 条逆向验证通过；已恢复基线（17 passed / 0 failed）
```

### 3. 真场景原始回显（`bun run scripts/retire_tree_demo.ts`）

```
项目根：/tmp/gebai-retire-demo-0sPruZ/proj
会话标记：SHENGSHENG_WORKER_SESSION=0123456789abcdef0123456789abcdef

=== retire 前：现场 ===
pid 125893｜sleep 600｜SHENGSHENG_WORKER_SESSION=0123456789abcdef0123456789abcdef
pid 125894｜sh -c sleep 600 & wait｜SHENGSHENG_WORKER_SESSION=0123456789abcdef0123456789abcdef
pid 125895｜sleep 600｜（无标记）
pid 125896｜sleep 600｜SHENGSHENG_WORKER_SESSION=ffffffffffffffffffffffffffffffff
pid 125897｜sleep 600｜（无标记）
孙进程（pgrep -P 125894）：125898

=== retire 动作前打印的计划清单（回显原文） ===
已终止并退役：build-1｜会话 0123456789abcdef0123456789abcdef（保留可查）…（在途方向已退回种子队列）。原因：第 12 息止损…
回收计划（归因：SHENGSHENG_WORKER_SESSION 标记 + 子孙递归；扫过 421 个进程，来源 procfs /proc）
- 要杀的进程（3 个，pid+命令行）：
  · 125893｜sleep 600
  · 125894｜sh -c sleep 600 & wait
  · 125898｜sleep 600            ← 孙进程（靠子孙递归进来）
子孙进程树：已回收 3/3 个
- 处理记录：pid 125893 ← SIGTERM；pid 125894 ← SIGTERM；pid 125898 ← SIGTERM
⚠ **源码可能停在回退态**（1 处 `.revbak` 与源文件不一致；扫过 3 个文件）：
- …/proj/src/shengsheng.ts ← …/proj/src/shengsheng.ts.revbak
  核对：`diff '…shengsheng.ts.revbak' '…shengsheng.ts'`——确认哪一边该留，再手工 `cp` 或 `git checkout -- '…shengsheng.ts'`
（本工具**只告警、不自动恢复**：自动恢复可能覆盖别人正在验的回退态。）

=== retire 后：现场 ===
pid 125893｜（已不在）
pid 125894｜（已不在）
pid 125895｜sleep 600｜（无标记）                                  ← 无标记的无关进程，活着 ✓
pid 125896｜sleep 600｜SHENGSHENG_WORKER_SESSION=ffffffff…         ← 别的会话，活着 ✓
pid 125897｜sleep 600｜（无标记）                                  ← 活着 ✓
孙进程（pgrep -P 125894）：（无）                                   ← 孙进程已回收 ✓

会话是否被终止：是（engine.cancel 已调）
```

### 4. `bun test` + `tsc`

```
bun test packages/agents/src/agents/shengsheng/    → 100 pass / 0 fail（1351 expect）
bun test …/shengsheng-retire-tree.test.ts          →  17 pass /  0 fail（73 expect）
tsc --noEmit (packages/agents)                     → 0 error
shuangtian: st test → 184 passed, 0 failed；st lint → 违反 0 项
```

## 退化行为（拿不到真值时）

| 情形 | 行为 |
|---|---|
| 无 `/proc`（Windows / 极简容器） | `readProcTable` 返回 `error` → 回显「**无法定位**进程树——只终止了会话本身，它的子进程可能仍在改同一棵树」 |
| 该会话没有带标记的进程 | `via="none"`，回显「**无法定位**（该会话没有带 `SHENGSHENG_WORKER_SESSION` 的进程…）」+ 扫过多少进程，**不谎报**「已清理」 |
| 旧派工（标记生效之前的 worker） | 同上：归因不到就如实报，不猜 |
| `SIGKILL` 后仍存活 | `outcome="partial"`，逐条列出「仍在运行（需人工）」 |

## 需要重启才生效

`shengsheng.ts` / `proc-tree.ts` 是子Agent 的**辅助模块**（`import` 后进程内无法失效），
本次改动**必须重启歌白服务**才生效——热重载只覆盖子Agent 入口文件，新模块与新 env 标记注入不会热更新。

## 与另一条演进线的交叉（诚实记录）

开工时 `git status` 显示同目录已有别人的未提交改动（`shengsheng.md` 的租约段落）。
开发过程中那条线在我写入后再改同一批文件数次（`shengsheng.ts` / `shengsheng.test.ts` / `DESIGN.md`），
最终它把我这单的实现**一并提交**进了 `856ba9bb`（其提交说明已注明「本提交的 shengsheng.ts 同时含另一条
演进线的在做改动…故一并提交以保持树自洽」）。

我的两次提交：
- `a6556005`：`DESIGN.md` 里**只属于本单**的那一段 + 两个脚本（`retire_tree_demo.ts` / `reverse_verify_retire_tree.py`）。
- `3eec3ac3`：门禁脚本退出时释放并发锁（实跑发现自身缺陷：`--force` 只跳过并发判定，`preflight` 仍自持锁）。

**过程中我犯过一个错并已修正**：为把 `DESIGN.md` 的改动按 hunk 切开，我用 `git show HEAD:DESIGN.md`
重建了该文件，把另一条线**尚未提交**的两段（独占资源租约节、回退型门禁并发防护节）一并覆盖掉了。
已按本会话捕获的原始 `git diff` 全文**逐字恢复**，并核对：工作区 `DESIGN.md` = HEAD 版 + 那两段原样
（`grep -c` 各为 1，且这两段未进我的提交）。**教训**：同树多线并行时，不能拿 `HEAD` 版本当"干净基线"
去重建工作区文件——那会把别人未提交的改动当成噪声删掉。

## 未竟事项

1. **`engine.cancel` 后的正常收尾窗口**：现在 stop 是「终止会话 → 立刻回收子进程」。如果会话正在收尾
   （把回退的文件写回），而它起的子进程恰在此刻被杀，仍可能留下半截写入。方向：`cancel` 后给一个
   可配的短宽限（如 `SHENGSHENG_STOP_GRACE_MS`，缺省 0）再开杀。
2. **`sh-tasks` 后台任务记录与杀进程的联动**：被杀的 `sh async` 任务在 `tasks.json` 里仍是 `running`，
   下次 `bg_task status` 要靠 pid 失活才判 `lost`。方向：回收后顺手 `refresh` 一次该会话的任务表。
3. **标记的继承断点**：`env -i` / 显式清环境 / `sudo` 会丢标记，只剩「父进程带标记」一条路兜。
   更彻底的方案是进程组（`setsid` + pgid 归属），但那要把 `Sandbox.spawnBackground` 的进程组语义
   一起改（已有 `detached` + `kill(-pid)`），影响面大，宜单独立项。
4. **非 Linux 平台**：本模块不做 Windows 实现（只如实报「无法定位」）。Windows 有 `taskkill /T` 与
   Job Object 两条路，但需要真机验证，且本仓无 Windows CI。

## 建议下一条

**把「工作树独占租约」扩到「进程」维度**：现在 `cpu-exclusive` 只挡**申请方**，而本单证明真正的破坏
来自**没人申请**的野进程（会话死了，它的子进程还在写树）。方向：派工时登记「本 worker 会写哪些路径」
（工单字段 `writes: [...]`），`state` 读态把「有 worker 声称在写 X 但已无标记进程」报成**孤儿写者**——
比事后回收早一步（在它还是空转进程时就看见），而且能覆盖「生命周期超过会话」那类后台任务。

# Windows 上子进程「静默 exit 49」：PATH 键名大小写与商店别名存根

> 一次真实排障的完整记录（2026-10-28）。现象是一行没有任何线索的 `[exit 49]`，
> 根因绕了「Python 坏了 → WindowsApps 别名 → env 键名大小写」三层。
> 结论与修法可直接用于任何「自拼 env 再 spawn 裸命令名」的代码（本仓集中在 sh / py / js
> 脚本子进程与后台任务，见 `packages/server/src/core/support/child-env.ts`）。

## 1. 现象

`@gebai/server` 的测试在 Windows 上红了一片，失败信息都长这样：

```
error: expect(received).toContain(expected)
Expected to contain: "hello-python"
Received: "[exit 49]"
```

- **覆盖范围**：`py` 工具桥的全部 18 个用例、`py` 安全模式审计钩子，以及 `keqing-python` 包装测试。
- **误导点**：`stderr` 完全为空，`stdout` 也为空，只有一个退出码。看上去像「Python 装了但坏了」。
- 同一份代码在 Linux/macOS 上全绿，因此长期只表现为「某台 Windows 机器上莫名红了」。

## 2. 定位过程（关键在把「猜」换成「量」）

### 2.1 `49` 是什么：`9009 & 0xFF`

Windows 的 `9009` = 「命令找不到」。Node/Bun 把退出码放进 8 位字段，9009 截断即 **49**。
所以第一个问题不是「Python 为什么返回 49」，而是「**谁**返回了 9009」。

实测：直接执行 `%LOCALAPPDATA%\Microsoft\WindowsApps\python.exe`（微软商店的「应用执行别名」存根，
0 字节重解析点）——`9009`。指向成立。

### 2.2 为什么偏偏解析到存根：env 的 `PATH` 键名大小写

决定性对照（同一台机、同一个裸命令名 `python`，只改传给 `spawn` 的 env）：

| 传入的 env | 结果 |
|---|---|
| `{ PATH: <真值> }` | ✅ 真 Python 3.12 |
| `{ Path: <真值> }` | ❌ 存根 → 9009 → 49 |
| 不传 env（继承） | ✅ 真 Python 3.12 |

原因：Windows 上 `process.env` 的 PATH 键名就是 **`Path`**（系统规范写法）。
而 `spawn(file)` 在 `file` 不含路径、需按名解析时，**只去传入的 env 里找精确大写的 `PATH`**；
只有 `Path` 时视同「没有 PATH」，**回退到父进程 PATH**——父进程 PATH 里
`%LOCALAPPDATA%\Microsoft\WindowsApps` 排在真 Python 之前，于是命中存根。

> 这也解释了一个容易误判的现象：**在 Windows 的 PowerShell 里直接敲 `python` 同样是 49**
> （shell 自己命中存根）。「PowerShell 里 python 不通」**不能**作为「本机没装 Python」的证据；
> `py`（官方启动器）通常才是能用的那个。

### 2.3 为什么产品实际能用、而测试全红

同一件事在两个地方的口径不一致：

- **产品**（`py` 工具）探测解释器走 `ctx.runCommand` → `Sandbox.exec`，最终经 **PowerShell** 执行，
  而沙箱在拼 env 时**归一过 PATH**，所以 `python3`/`python` 探测失败、`py` 成功，
  产品选中 `py`，一切正常。
- **测试替身**探测用 `Bun.spawnSync([cand, "--version"])`（不传 env，继承）→ 命中真 `python`；
  真正执行时却传了 `env: {...process.env}`（含 `Path` 键）→ 落到存根 → 49。

即：**测试替身的探测与执行用了两套 env 口径**。修法因此是让替身与产品同口径，
而不是去「让产品兼容 49」。

### 2.4 顺带查出的第二个同类缺陷

“解释器不可用就降级为纯脚本执行”那一例，原设计是「把 PATH 指向不存在目录」来断掉桥的 spawn 路径，
同时期望**降级后的纯脚本执行仍能跑通**——两者在产品里靠同一套 env 不可能同时成立
（PATH 坏则全都坏）。这是测试自身的口径矛盾：它想验的是「降级分支」，
却顺手把「降级后还能执行」也押在了坏 PATH 上。改为：**对桥是否可用的判断走真实解析**
（跑不通即降级），**降级后的执行用已知可用的绝对路径**。

## 3. 修法

新增 `packages/server/src/core/support/child-env.ts` 的 `withCanonicalPath(env)`：

1. **PATH 类键去重为大写 `PATH`**（Windows 环境变量名本就不区分大小写，PowerShell 的
   `$env:Path`、cmd 的 `%PATH%` 照常命中）。大小写变体并存时**后者胜**（与 JS 展开
   `{...process.env, PATH: x}` 的覆盖直觉一致）。
2. **空串 PATH 视为「有」**：显式收紧搜索路径不能被当成「没这个键」（那会回退父进程 PATH）。
3. **丢弃 `undefined` 值**（spawn 的 env 值必须是字符串）。

接入点（全部在**最终落地前**归一）：

| 位置 | 说明 |
|---|---|
| `core/exec/js-tool.ts` `scriptChildEnv` | js 桥与 py 桥共用的子进程 env |
| `core/security/sandbox.ts` `exec` / `spawnBackground` | sh 工具、后台任务（含 `taskkill` 的 spawn） |
| `core/exec/py-tool.ts` `resolvePythonCmd` | 补文档：**探测必须与执行同口径**（两处都归一） |

测试侧同步归一：`py-tool.test.ts`、`tools.test.ts` 的 `runCommand` 桩。
另修三处测试自身的跨平台缺陷（与 PATH 无关，但同一轮暴露）：

| 用例 | 问题 | 修法 |
|---|---|---|
| `keqing-python.test.ts` | 硬编码 `python3`；Windows 上该名是存根，`Bun.spawnSync` 直接抛 ENOENT（不是返回非 0），「缺解释器即跳过」的初衷变成整例报错 | 按 `python3 → python → py` 逐个探测（与 py 工具同口径），`.which` 取绝对路径，探测异常按不可用处理 |
| `profile.test.ts` | 断言 `resolveProfilePath("/tmp/p.json")` 原样返回，但实现走 `resolve()`，Windows 上归一成 `C:\tmp\p.json` | 改为对 `resolve("/tmp/p.json")` 取齐（断言的仍是「没被当档案名拼进 profiles/」），两平台都成立 |
| `engine.test.ts`（任务 env 用例） | `echo flag=$TASK_FLAG` 是 POSIX 语法；Windows 的 sh 走 PowerShell，`$TASK_FLAG` 是未定义的 PowerShell 变量 → 空值，看起来像「任务 env 没注入」 | 按平台分：Windows 用 `$env:TASK_FLAG`（仓内 `sandbox.test.ts` 的 envProbe 已有先例） |

还有一处**过宽断言**：`engine.test.ts` 用 `not.toContain("待办")` 表达「子会话内部 todo 结果不进主上下文」，
而系统提示词本身就会提「待办」——它守的不是行为而是提示词的措辞，提示词一改就无意义地红。
改为锁 todo 工具输出里的专属文案。

## 4. 验证

- `bun run test`：**7/7 任务通过、0 fail**（server 1886 + web 1302 + agents 1700 + channels 70
  + sdk 127 + persona 260 + desktop 1 = **5346 pass**）。
- `bun run typecheck`：通过（顺带修掉 `packages/channels/src/channels/feishu.ts` 的
  `chatType` 类型缺失——它不只是类型问题：`extractMessage` 没把 `chatType` 带出来，
  下游 `msg.chatType !== "p2p"` 恒为真，单聊也会去查群成员表）。
- 新增 `child-env.test.ts` 8 例（含幂等、空串、无 PATH 不凭空造、`undefined` 丢弃、不修改入参）。

## 5. 可复用的判据（下次直接照用）

1. **退出码先还原**：Windows 上 `49` 优先怀疑 `9009 & 0xFF`（命令找不到），别先怀疑程序逻辑。
2. **stderr 为空 ≠ 程序没报错**：可能是根本没跑起来（存根/别名层就返回了）。
3. **「探测」与「执行」必须同口径**——包括 env、cwd、shell、绝对路径/裸名。
   口径不一致时症状恒为「探到的能用，跑起来是另一个东西」。
4. **自己拼 env 再 spawn 裸命令名 → 一律归一 PATH 键名**（本仓 `withCanonicalPath`）。
   更好的做法是**优先用绝对路径**（`Bun.which` / `which()`）spawn，从根上绕开搜索顺序。
5. **测试替身不要顺手把产品的坑复制一份**：替身若用 `{...process.env}` 包 env，就必须同样归一；
   否则替身会稳定失败在产品完全正常的地方。
6. **断言要锁行为、不锁措辞**：`not.toContain("待办")` 这类断言在提示词一改就会无意义地红。

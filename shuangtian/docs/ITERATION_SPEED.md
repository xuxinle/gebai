# 迭代速度基线（2026-10-26）

> 本文件是 `docs/BUILD_TEST_PERF.md` 的**续篇**：那份记编译器对比与测试分片调参，
> 这份记「改一行 → 看到结果」的端到端链路，以及为压缩它做的四类改动。
> 口径遵守 `BUILD_TEST_PERF.md` §0 三条测量纪律（输出落盘 / 先刷新工具链 / 多轮取最小值）。

## 1. 度量器与口径
`tools/bench.ps1`（新增）——每次运行自动落盘，只报关键数字。

| 指标 | 定义 |
|---|---|
| 墙钟 | `Stopwatch` 包住整个子进程（含进程启动/退出） |
| CPU 秒 | 每 100ms 枚举 `st`/`st_tests`/`g++`/`cc1plus`/`cc1`/`as`/`ld`/`collect2`/`objcopy`，累加各 PID 的 `TotalProcessorTime`；**进程退出后保留最后值**（不漏计已跑完的单元） |
| 平均并行度 | CPU 秒 ÷ 墙钟秒（1.0 = 单核满载） |
| 逻辑核利用率 | 平均并行度 ÷ 逻辑核数（本机 28） |
| 峰值并行度 | 采样期间最大「**两轮都在**的进程」CPU 增量 ÷ 采样间隔 |

> ⚠ **两个量尺坑（都踩过，都会导出相反结论）**：
> 1. 首版漏了 `st_tests` 进程名 → `test` 的 CPU 只采到 11 s（并行度 0.44），
>    看起来"测试只用半个核"，而真相是**尺子没量到**。这就是 §0 第 2 条那类错。
> 2. 首版峰值算「全部见过的 pid」的增量 → 新启动的 `cc1plus` 首轮就带着累计 CPU，
>    算出一秒跑 **8530 核**这种不可能的峰值。改为只算「两端都在」的进程后回到可信值。

## 2. 基线 → 交付（本机 28 逻辑核 / 32 GB / GCC 16.2.0）

| 操作 | 基线 | 交付 | 关键变化 |
|---|---|---|---|
| `st build`（空，全命中） | 0.22 s | 0.22 s | — |
| **`st build --force`（全量 54 单元）** | **42.29 s / 并行度 5.59（20.0%）** | **35.29 s / 26.94（96.2%）** | **并行度 ×4.8** |
| `st test`（全量，16 片） | 24.70 s / 26.55（94.8%） | 24.70 s / 26.55 | 本就吃满，未动 |
| `st lint` | 22.80 s / 0.99（3.5%） | 22.80 s | 单线程，未动（见 §6） |
| **改一行 `src/pkg/*` → `st` 可用** | **55.5 s** | **6.84 s** | **8.1×** |
| **`bootstrap.ps1` 后第一次 `st build`** | 重编 45（32.36 s） | **重编 0（1.08 s）** | **30×** |
| `st rebuild`（热态） | 48.2 s | 0.91 s | `force=false` |

> 并行度 96.2% 的那轮：墙钟 35.29 s、CPU 950.7 s、峰值 **28.3**（即某一刻 28 个逻辑核全部在编）。
> 三轮流钟 41.44 / 37.22 / **35.29**——本机有背景负载，故取最小值（§0 第 3 条）。

## 3. 四类改动与各自的实测依据

### 3.1 日志落盘（默认开）

每次 `build`/`test`/`lint`/`rebuild` 的**完整输出**自动写到
`.st/logs/<action>-<target>-<profile>-<YYYYmmdd-HHMMSS>-<pid>.log`，控制台照旧显示。
读：`st log [--tail N] [--grep X] [--list] [--full] [--action X]`。

**为什么做在 `st` 里而不是包装脚本**：实测代价——一次 `duplicate symbol` 失败，
`build/bin/logs/st.log` 里只有一行「日志落盘：…」，真正的错误一行也没有，
**必须重跑一遍**才能看见。构建是秒级到分钟级操作，这种重跑是直接的迭代时间损失。

实现：`st::print`/`st::eprint` 是**唯一输出出口**，在那里挂 tee
（`include/st/core/console_log.hpp`），stdout 与 stderr 汇到同一文件、
进程内串行化——不依赖 `2>&1` 的 shell 拼法（PowerShell 与 cmd 写法不同，跨平台不一致）。
在 `run_app` 分发命令**之前**打开 ⇒ 不存在「新加一个命令忘了接日志」。

`--no-log` / `ST_NO_CONSOLE_LOG=1` 可关（只读文件系统、基准测量排除 IO 影响时用）。

### 3.2 自举增量（最大的单点收益）

| 改动 | 位置 |
|---|---|
| `force = true` → `false`（`command_rebuild` 早已改，自动路径漏了） | `tools/stpm/main.cpp` `rebuild_self_and_reexec` |
| 可信校验开关 `ST_VERIFY_SELF_REBUILD=1` + `ST_VERIFY_TOKEN=<串>` | 同上 |
| **bootstrap 与 `st build` 的缓存对齐** | `bootstrap.ps1` / `.sh` |

缓存对齐的三个不一致（这才是 55.5 s 的主因，不是 `force`）：

1. 对象目录：`build/obj/bootstrap` vs `build/dev/obj`
2. 对象命名：`src_pkg_semver.cpp.o`（相对路径）vs `C__Users_…_src_pkg_semver.cpp.o`（绝对路径归一化）
3. **PCH**：bootstrap 完全不用 PCH（每个单元重新解析 143.9 MB 头）

**逆向验证**（必须做，否则是"恒绿的测试"）：

第一轮用 `[[maybe_unused]] const char* const kProbe = "…"` 注入 —— **失败**。
它在匿名命名空间且无人引用，被编译器**完全消除**，产物里当然搜不到。
那是**量尺的错**（判据分辨不出被测特征），不是假增量。

第二轮改为「文件末尾追加 `extern "C" const char st_…[] = "token";`」
——外部链接符号是 ABI 的一部分，不可被 DCE：

```
✅ .o 含 token —— 增量确实重编了这个单元
✅ st.exe 含 token —— 链接也带上了
✅ .o / exe 已无 token（还原后）
```

即**新改动真的编进去了，还原也真的编进去了**。

### 3.3 吃满 CPU（并行度 20% → 96.2%）

两处叠加缺陷：

1. **大单元被排到最后**（`build.cpp` 原 `stable_partition`，注释理由是"大块头收尾时独占闸门"）——
   后果相反：50 个小单元 2 s 跑完，剩下 26 核看两个巨无霸单独吃一个核。
   改为 **LPT 调度**（按源文件字节数降序提交；超大单元恒排最前）。
2. **`jobs_large` 固定 2** —— 两个巨无霸被闸门卡成串行。改为
   `clamp(预算 / 大单元估算, 2, min(8, jobs))`（本机 2 → 8）。

并发档位对照（各 3 轮取最小）：

| 并发 | 墙钟 | 并行度 | 结论 |
|---|---|---|---|
| **28（默认，最终复核）** | **35.29 s** | **26.94（96.2%）** | **最优** |
| 28（第一轮测） | 34.86 s | 24.03 | 同档噪声 |
| 14 | 44.21 s | 18.97 | 更差 |
| 8 | 52.54 s | 15.94 | 明显更差 |
| 28 不带 PCH | 44.19 s | 20.41 | 更差（PCH 必须留） |

⇒ **满核仍是最优，32 GB / 28 核不该为"防过载"降并发。**

### 3.4 单元耗时分布（长尾的量化）

单线程串行全量（54 单元共 194.8 s）：

| 单元 | 串行耗时 |
|---|---|
| `third_party/sqlite/sqlite3.c` | **20.1 s** |
| `third_party/quickjs/quickjs.c` | **14.2 s** |
| `src/pkg/build.cpp` | 9.6 s |
| `src/pkg/lint.cpp` | 6.6 s |
| `src/pkg/registry.cpp` | 6.3 s |

完美均衡（28 核）理论墙钟 **6.96 s**，实际 **35.29 s**。剩余差距的两项**已量**原因：

1. **并发膨胀 1.41×**（标定实验：同一文件 `build.cpp` 单线程 15.34 s，
   **2 路并发就 1.34×**，28 路 1.41×）⇒ 真实下界 ≈ 194.8×1.41/28 = **9.8 s**。
2. **PCH 是串行前置**：143.9 MB、构建 4.26 s，且 28 个进程各自 mmap + 反序列化
   ——既是一段纯串行前置，也是 1.41× 膨胀的主要来源。

## 4. PCH 瘦身：已量出收益，未改默认（有前置条件）

按头文件分组实测（PCH 构建时间 / 体积）：

| 组 | 增量体积 | 累计 |
|---|---|---|
| G1 核心（无可避免） | +57.1 MB | 59.4 MB |
| G2 容器+算法 | +27.6 MB | 87.0 MB |
| G3 format | +15.9 MB | 102.9 MB |
| G4 filesystem | +11.6 MB | 114.5 MB |
| **G5 并发（chrono/mutex/thread/future/atomic）** | **+24.6 MB** | 139.0 MB |
| G6 C++23 设施 | +4.0 MB | 143.0 MB |

移出 G5 的对照（PCH 重建 + 3 个代表单元各 2 轮取最小）：

| 变体 | PCH 体积 | PCH 构建 | 三单元合计 |
|---|---|---|---|
| 现状 | 143.9 MB | 4.97 s | 18.27 s |
| **去并发组** | **119.1 MB（−17%）** | **4.79 s** | **16.61 s（−9.1%）** |

**两个正交量同向下降 ⇒ 是真收益。但未改默认**，前置条件是先修自包含：
有 7 个 `.cpp` 与 1 个头文件**依赖 PCH 提供并发头**（违反 CONVENTIONS §6
「每个 `.cpp` 自包含、不得依赖 PCH」）。这会变成一次扫尾式的结构整理，
已记入 `BACKLOG.md`，不塞进本轮。

## 4.5 `.d` 解析：Windows 盘符被当成分隔符（本轮发现并修的**静默缺陷**）

**这是本轮代价最大的一个发现**——它不是“某个功能不对”，而是“增量判定在 Windows 上部分失灵”，
**不报任何错**。

### 现象

改了 `tests/pkg_memory_test.cpp` 后，`.o` 不重编（mtime 19:50 的源文件 vs 19:46 的对象），
`st test` 也不重建测试二进制——**连 `--force` 都治不好**。

### 根因

`parse_depfile` 把 `.d` 里的 `C:/Users/...` 解析成 `/Users/...`：

```cpp
if (raw == ':') { token.clear(); seen_colon = true; continue; }   // 无条件
```

Windows 路径的**盘符冒号**命中这一行，`C:` 被当成“目标/依赖分隔符”丢掉。
后果链：`.meta` 第 4 列记录的是**读不到的坏路径** → `deps_entry_matches` 要么恒判“变了”
（白重编）要么恒判“没变”（漏重编）→ **`include/st/**` 的头文件改动检测一并失效**。

### 为什么难发现（三重掩盖）

1. **不报错**：只是“不重编”，构建照样报“完成”；
2. **`--force` 也治不好**：重编后又写回同样的坏记录（同一个函数生成）；
3. **旧实现“看起来对”**：它对测试用例的**个数**是对的（靠“顺手把目标行也吞掉”蒙的），
   错的只是**路径内容**。

### 修法与验证

判据改为：**分隔符后面一定跟空白/行尾/续行；盘符冒号后面紧跟路径内容**。

```cpp
if (raw == ':' && !seen_colon && is_target_separator(index)) { ... }
```

验证（三格式 + 逆向）：

| 输入格式 | 修复前 | 修复后 |
|---|---|---|
| Windows 盘符 + 续行 | 2 条，路径 `/a/...`（丢盘符） | **2 条，`C:/a/...`** |
| POSIX 绝对路径 | 2 条，正确 | 2 条，正确 |
| 单行无续行 | 2 条，路径丢盘符 | **2 条，正确** |

**逆向验证**（两次，第一次不合格）：

- 第一次回退后 `st build` **编译失败**（`is_target_separator` 未使用 → `-Werror`）
  ⇒ 测试跑的是**上一份二进制** ⇒ 那个“红”不算证据（PITFALLS §7.3 的变体）。
- 第二次回退时加 `(void)is_target_separator;` 保证能编译，结果：
  **回退版 exit=0 编译成功 + 用例 `0 passed, 1 failed`** ⇒ 真抓住了。

回归测试：`tests/pkg_deps_hash_test.cpp` 的 `depfile_parse_preserves_windows_drive_letter`——
断言不只比字符串，而是要求**解析出的路径真能 `is_regular_file`**（这才是增量判定实际要做的事）。
为此把 `parse_depfile` 从匿名命名空间提到头文件（解析层缺陷跑一遍完整构建看不出来）。

## 5. UTF-8：PowerShell 的四条独立通道（`sh` 工具）

`packages/server/src/core/security/sandbox.ts` 的 `PS_PREAMBLE` 原来只设了两条，
实测缺第三条时**读日志得到乱码**：

| 通道 | 机制 | 缺失时的症状 |
|---|---|---|
| ① 控制台输出 | `[Console]::OutputEncoding` | 原生命令（g++/st.exe/git）中文乱码 |
| ② 控制台输入 | `[Console]::InputEncoding` | 交互/管道 stdin 多字节丢 |
| ③ 管道数据 | `$OutputEncoding` | 传给原生命令的中文丢（默认 ASCII） |
| **④ 文件读写默认编码** | **`$PSDefaultParameterValues['*:Encoding']`** | **`Get-Content` 读 UTF-8 文件→`鏋勫缓瀹屾垚`** |

**④ 与 ①②③ 完全无关**：实测（Windows 11 + PowerShell **5.1**）设了 ①② 之后
`Get-Content` 读合法 UTF-8 文件仍得到 `鏋勫缓瀹屾垚`（按 ANSI 代码页解码）。
而 **pwsh 7 默认就是 UTF-8**，所以装了 pwsh 的机器上完全看不出来——
偏偏 PS 5.1 是没装 pwsh 的机器上的回落路径（= 大多数用户）。

### 一个反直觉的取舍（逆向验证抓出来的）

补 ④ 时不能写 `[Text.UTF8Encoding]::new($false)` 做"无 BOM"：
PS 5.1 的 `Out-File -Encoding` 是 `ValidateSet(unknown,string,unicode,…,utf8,…)`，
传对象会**绑定失败并静默回落到默认 UTF-16**（实测写出 `ff fe` 开头）。

所以值只能写字符串 `'utf8'`。代价：PS 5.1 下带 BOM（`ef bb bf`），pwsh 7 下不带。
取"编码对"而非"无 BOM"——读侧 `st::fs::read_text` 已剥 BOM，而乱码不可接受。

### 验收与回归

- 端到端 8 条路径（原生命令 / cmd 回落 / 分块切断 / 非控制台 / 读文件 / 写文件）
- 正式回归 `sandbox.test.ts`：引导段四通道断言 + **真实执行**读/写，
  **同时覆盖 PS 5.1 与 pwsh 7**（只测一条会恒绿，把 5.1 上的缺陷完全掩住）
- **逆向验证通过**：移除 ④ 段后 PS 5.1 的真实执行用例变红，其余全绿

## 3.5 `st watch`：让「保存 → 结果」不再有等待（新增）

### 它解决什么、不解决什么（先把预期说准）

实测基线（本机 28 核，`st` 目标 54 单元）：

| 场景 | 墙钟 |
|---|---|
| 零改动 | 0.25 s |
| 改 1 个 `.cpp` | 3.49 s |
| 改 10 个 `.cpp` | 6.80 s |
| 改 `error.hpp` | 20.24 s |
| 全量 `--force` | 28.67 s（PCH 瘦身后） |

⇒ **编译本身的时间是真成本，watch 不会让它变快**。它省的是**等待**：
你改完最后一笔时它已经在编了，你切回来时产物往往已好。

### 实现选择：轮询，而不是 inotify/FSEvents/ReadDirectoryChangesW

三条理由（写进了 `watch.hpp` 文件头）：
1. 事件 API 要平台分支（`CONVENTIONS §10` 限制系统头只在 `platform_*` 里）；
2. 各平台语义差异大（inotify 逐文件 add watch、RDCW 有缓冲区溢出与重命名语义）；
3. **轮询的判据与增量判定同口径**——`deps_entry_matches` 也是看 `(mtime, size)`，
   所以「watch 认为变了」与「构建会重编」天然一致；事件 API 会多一层偏差。

代价实测：545 个文件一轮 `stat` 约 **0.2 s**（按 300 ms 间隔轮询）。

### 性能优化：1.45 s → 0.34 s（三个静默失效）

初版 `watch --once` **1.45 s**，而构建只要 **124 ms** ⇒ 扫描占 92%。
三个 bug **都不报错**，且都表现为“改了文件不触发构建”：

| # | 缺陷 | 症状 |
|---|---|---|
| 1 | 手写 glob 的 `**` 语义错 | 监视列表 650 → 397（**漏 110 个**）——两指针+回溯把「跨不跨目录」状态藏在回溯变量里，回溯到更早的星就丢 |
| 2 | `fs::walk` 路径基准误解 | 列表**恒为空**——`walk(root/src)` 返回 `src/a.cpp`（已相对根），我又拼一次 `dir` → `src/src/a.cpp` |
| 3 | stat 用相对路径 | 列表恒为空——`modified_ns(entry.path)` 读不到（cwd ≠ root）⇒ 静默跳过 |

修复后：**1.45 s → 0.34 s（4.3 倍）**，其中扫描 ~0.2 s、构建 ~0.13 s。

### 一个纪律实例

修 bug #1 时发现：我**手写了 80 行 glob**，而 `fs::match_glob` 早就在
`include/st/core/fs.hpp` 里（有文档、有测试、支持 `*`/`?`/`**`）。
改用现成实现后代码少 80 行，15 个边界用例全过。
——「先查有没有现成实现」这条不是口号，是真省时间的。

### 用法

```powershell
st watch                     # 常驻，保存即增量构建
st watch gallery             # 盯指定目标
st watch --once              # 构建一次就退（脚本/CI 用）
st watch --interval 500 --debounce 400
```

### 测试与逆向验证

6 个用例（`tests/pkg_watch_test.cpp`），其中两条是**相反断言**：
`watch_glob_double_star_crosses_dirs_single_star_does_not` 既要求 `**` 跨目录、
又要求 `*` **不**跨目录——只测前者会让“`*` 也跨目录”的实现通过。

逆向验证 3 项、**2 项通过**：stat 路径与 build/ 排除都能被抓住；
第三项（`relative_slash`）复测发现 `fs::relative_to` 对相对输入**本就**直接返回原路径，
所以那个 `is_absolute` 判断是**纯防御、不是缺陷修复**，无法逆向验证（如实记下）。

## 3.6 `st lint` 并行化：22.8 s → 1.30 s（2026-10-27）

### 摸底

764 文件、CPU 22.7 s / 墙钟 22.8 s ⇒ **并行度 0.99/28（3.5% 利用率）**。
28 个核里只有 1 个在干活。

### 关键判断：那个“跨文件规则”的顾虑不成立

BACKLOG 原先记着“部分规则跨文件（L16 层方向、清单豁免），分片需重新设计语义”。
**读代码后发现三条都不成立**：

| 顾虑 | 实际 |
|---|---|
| L16 需全局视图 | `check_layer_includes()` **只看本文件自己的路径**（`path.contains("src/core/")`）|
| 清单豁免需全局视图 | `exemptions` 在外层解析好、**只读传入** |
| 跨文件状态多 | `suppressed` / `manifest_suppressed` 是**每文件局部计数器** |

⇒ **所有规则都是单文件规则**。（这是“读代码而不是读待办”的价值：待办里的顾虑常是当时的猜测。）

### 实现的两个要点

1. **先读进内存、再并行扫**。在并行任务里读盘会让几千文件同时压在磁盘队列上；
   带宽固定，寻道竞争反而变慢。串行读（页缓存友好）+ 并行 CPU 密集正则。
   代价：764 × 约 20 KB ≈ 15 MB 峰值。
2. **失败语义变了**。旧实现遇到读失败**立刻 return**；并行下无法中途放弃，
   改成“全部跑完后报下标最小的那个”。副产品：用户能一次看到**所有**读失败的文件。

### 结果

| 指标 | 改前 | 改后 |
|---|---|---|
| 墙钟 | 22.8 s | **1.30 s**（5 轮最小）|
| CPU | 22.7 s | 25.7 s |
| 并行度 | 0.99/28 | **19.2/28** |
| 违规结果 | 69 项 | **69 项，逐条一致** |

加速约 **17×**。CPU 总量略升（25.7 vs 22.7）是因为多线程下有缓存竞争，不影响墙钟。

### 我写出的一个 bug（逆向验证抓到的）

初版写：

```cpp
report.files_scanned = jobs.size();   // ← 无条件赋成总数
```

分片漏掉文件时报告**仍然显示“扫了 N 个”**。改 `if (index >= jobs.size())` 成
`index + 1 >=` 后，`scan_counts` 用例**依然绿**——逆向验证当场揭穿了它。
正确写法是统计 `scanned.scanned == true` 的条目。

### 回归与逆向验证

4 个用例（`tests/pkg_lint_parallel_test.cpp`）：

| 用例 | 钉住什么 |
|---|---|
| `scan_counts_every_file` | 并行分发不漏下标 |
| `agrees_with_per_file_scan` | **并行结果 == N 次单文件扫**（不靠硬编码期望值）|
| `is_deterministic_across_runs` | 两次扫逐条相同 + 确实有序 |
| `keeps_exemption_counters` | 豁免计数不丢（并行下最易漏的共享状态）|

逆向验证 3 项、**2 项变红**：分片漏文件 ✅、豁免计数不汇总 ✅。
第三项（去掉末尾排序）**实测仍绿**——因为汇总时按**下标顺序**遍历 `results`、
而下标来自有序的 `fs::walk`，输出天然有序，排序只是冗余保险。
这本身是好设计（可复现性不依赖排序），但如实记下：
那条断言无法逆向验证。

### 写测试时的一个坑

第一版把 `lint-allow` 注释放在**上一行**，`suppressed` 恒为 0。
`has_allow_comment()` 拿**单行原文**找标记，没有“上一行覆盖下一行”的语义。
**那是用例错，不是实现错**——分清楚这两件事很关键（否则会去改一个本来正确的实现）。

## 6. 已知未做（都不塞进本轮）

| 项 | 实测 | 为什么没做 |
|---|---|---|
| **`st buildd` 守护进程** | 常驻能省的只是「进程启动 + 清单解析 = **0.122 s**」（`st version` 实测），而 `st watch` 已把「保存 → 构建」变成自动 | 要付守护进程生命周期/控制通道/多客户端/崩溃恢复一整套新失效模式，风险 > 0.12 s 收益 |
| 拆 `error.hpp` / `fs.hpp` 减爆炸半径 | `error.hpp` 33 个单元 / 20.2 s、`fs.hpp` 24 / 17.7 s | `Result<T>`/`Status` 全是模板+内联，`std::expected` 的类型参数必须完整可见——拆不了；拆成“前向声明+实现”会把内联变真实调用 |
| PCH 再移出 `filesystem` / `format` | `filesystem` +11.6 MB、`format` +15.9 MB | `format` 被 89/147 个单元直接用；`filesystem` 在路径密集的代码里到处是 |
| `st lint` 并行化 | 22.8 s 单线程（3.5% 利用率），759 文件 | **✅ 已完成（2026-10-27）**：22.8 s → 1.30 s（约 17×），并行度 19.2/28。原文担心的“跨文件规则”实测不成立 |
| `sqlite3.c` / `quickjs.c` 拆分 | 20.1 s / 14.2 s，合计占串行总量 17.6% | 第三方源码，改动风险高于收益 |
| 15 处 `#include "examples/*.cpp"` | 改一个实现连带重编多个测试单元 | 需改 `st.pkg` 的 target 划分 |
| **为 `text` 抽光栅化接口（A2）** | 实测发现**那个抽象早就在了**：`TextRenderer::draw` 收的是 `raster::Surface&`（接口），而 `Surface::blend_coverage_bitmap` **本来就是 `virtual`**，软件 Canvas 与 GPU GpuCanvas 各一份实现 | 不存在“新增虚调用”的风险（那次虚调用早就在热路径上）；真正的问题只是**类型定义在 `raster` 命名空间里**。纯归属问题，无性能收益——按“性能优先”不做 |
| **`text → raster` 的 2 处违规** | 一处是 `Canvas scratch`（字形位图生成，**有 LRU 缓存，只冷启动某字形才走**），一处是 `Surface`（虚接口） | 都不是热路径问题，也不影响正确性；仅模块归属 |
| **`control → ui`（5 处）** | 控制通道要操作 `ui::actions` / `ui::dsl` / `ui::selector` | 见 `docs/STRUCTURE.md` 的论证：抽出接口会让控制通道失去直接操作能力（UI 元素是自绘的，没有稳定 DOM 可依）|

## 7. 复现命令

```powershell
# 刷新工具链（§0 第 2 条：先确认尺子是新的）
.\build\bin\st.exe build st --profile dev

# 度量（自动落盘 + 并行度/利用率）
pwsh -File tools\bench.ps1 -Label build-full -Cmd "build st --profile dev --no-self-rebuild --no-tests --force"
pwsh -File tools\bench.ps1 -Label test-full  -Cmd "test"
pwsh -File tools\bench.ps1 -Label lint       -Cmd "lint"

# 读日志（不必知道路径）
.\build\bin\st.exe log --grep error
.\build\bin\st.exe log --tail 60 --action build

# 可信增量校验（改完构建逻辑后确认新 st 真是新逻辑）
$env:ST_VERIFY_SELF_REBUILD=1; $env:ST_VERIFY_TOKEN="<本次新加的字符串>"
.\build\bin\st.exe build st --profile dev
```

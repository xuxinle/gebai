# 霜天构建/测试性能 — 基线（编译器与并行口径）

> 用途：编译器对比（gcc vs clang）与测试并行提速的**唯一基准**。全部数字为本机实测，
> 每项都写清口径（几轮、取什么统计量、输出去哪），否则数字不可复核。
> 快照时间：2026-10-06（§7 为 2026-11-22 增补）。机器：**28 逻辑核 / 32 GB / Windows 11**。

## 7. 测试构建/运行提速（2026-11-22）

口径：`cmd /c "st ... > tmp\perf\m.txt 2>&1"`（输出必须重定向，见 §0 第 1 条），
3 轮取最小值；`st` = `build/bin/st.exe`（自举产物，dev 档）。

### 7.1 结果

| 操作 | 改前 | 改后 | 倍数 | 省下的时间去哪了 |
|---|---|---|---|---|
| `st test <过滤词>`（无改动） | **5.42 s** | **1.14 s** | **4.8×** | 2.9 s 链接 + 1.5 s objcopy 分离 → 0 |
| `st test <过滤词> --no-build` | 0.70 s | **0.65 s** | — | 本来就是纯跑 |
| `st test`（全量，16 片） | **32.3 s** | **12.3 s** | **2.6×** | 集成用例 18 s + git 夹具 5 s + 分片失衡 |
| `st build st`（无改动） | 2.67 s | **0.35 s** | 7.6× | 同第一条（链接跳过） |
| `st build`（无改动，含测试刷新） | 0.63 s | 0.63 s | — | 一直很快 |

关于最值钱的那一条（**改一行测试 → 看到结果**）：

| | 改前 | 改后 |
|---|---|---|
| 改一个测试文件 → 重编 + 重链 + 跑该族 | ~14.5 s | **9.5 s** |

剩下的 9.5 s 拆开：编译 1 单元 ~1.2 s、链接 ~2.9 s（静态链 766 个对象）、
分离调试信息 **~4.0 s**（`objcopy` 三步共处理 ~200 MB 产物）、跑用例 ~0.6 s。
分离那 4 s 看起来可省（增量时复用上轮 `.debug`），但那会让调试符号指向旧二进制
——**正确性降级换速度**，明确不做（`build.cpp` 那段注释记了理由）。

### 7.2 四条根因（每条都有实测证据）

**① 测试二进制每次都重链（占内循环 81%）**

`build()` 有「链接指纹 + 跳过」机制，而 `build_test_binary()` **直接调 `link()`、
从不读写 `.link` 戳**——`build/dev/bin/st_tests.exe.link` 在改前**从未存在过**
（ls 一下就是根因证据）。于是「0 单元重编」的 `st test` 仍要付 2.9 s 链接 + 1.5 s
分离，而它真正想跑的用例只占 0.6 s。

修法：把指纹计算抽成 `link_fingerprint_of()`（编译器族 / 链接集合 / 每对象的 `.meta`
内容哈希 / 档位标志 / `--define` / 链接旗标 / 尾部参数 / 目标名 / `system` / 图标哈希 /
**`strip_debug`**），两侧共用一份；`build_test_binary()` 在 `compile_units` 之后先比指纹。

**② 集成用例被当成普通用例（独占一片 18 s）**

`independent_project_builds_links_and_runs` 用 `ST_TEST_WITH_TIMEOUT` 注册，
而 `src/test/test_main.cpp` 与 `tests/skip_visibility_test.cpp` 的注释**都按它是 SLOW 写的**
（“默认不跑慢用例……需要真编译的集成用例”）。文档说 A、代码写 B——代码才是运行时真相。
于是它每轮全量都跑；16 片并行时其余 15 片 6 s 就完了，它还在自己那一片里编译。

修法：新增 `ST_TEST_WITH_TIMEOUT_SLOW`（slow 与 timeout 是两个正交维度，交集必须有宏），
改用它 → 默认全量不跑，`--slow` 与显式指名照常跑（仍吃 60 s 超时）。
回归：`tests/skip_visibility_test.cpp` 的 `slow_set_contains_integration_case`
（**查注册表**的 `Case::slow` 与 `timeout_ms`，不是查 `--list`——`list_cases` 不过滤慢用例）。

**③ git 夹具在子进程上白烧时间**

实测单次 `git` spawn **50-65 ms**（Windows 进程创建 + MSYS 启动 + 配置链）。
`git_service` 家族 19-24 个仓库 × 每仓 6 次（init/config×2/add/commit）= ~5-7 s 纯开销。

两刀：
- `--template=` 置空（跳过 hooks 模板，85→64 ms）+ 身份用 `-c user.email=… user.name=…`
  注入（`git config` 两次 → 0 次，**加在建仓与本夹具 `run()` 两处**，后者保证后续 `commit` 也不缺身份）；
- **种子仓复用**：全族用例对初始仓库的要求完全一致，进程内建一次、其余 `copy` 一份
  （`git_service` 3 spawn/仓 → 1；`gbcode_branches` 11 spawn/仓 → 1）。

实测：`git_service` **18.19 s → 10.85 s**（−40%）；`gbcode_branches` 单例 1.14 s → **0.10 s**。

**④ 分片按「条数」装箱，与耗时基本不相关**

旧算法用「用例条数」当负载。实测 16 片最重 **16.9 s**、最轻 **0.7 s**，理想值 6.8 s
——其余 15 个核在等最重那一片。

修法：权重换成**上一轮实测耗时**（侧车 `build/<档位>/bin/test-times.txt`，由 `st test`
自动采集合并），**TU 原子性不变**（同族用例必须同片，那是 git_service 随机误伤的根修）。

实测片内耗时分布：

| 口径 | 最重片 | 最轻片 | 理想 |
|---|---|---|---|
| 按条数（旧） | 16.9 s | 0.7 s | 6.8 s |
| 按耗时（新） | 11.6 s | 6.0 s | 7.2 s |

剩下那片 11.6 s 是 `git_service` **单个 TU 本身**的耗时——不拆 TU 就下不了限，
而拆分正是当初随机误伤的**反面做法**（见 `assign_shards_by_tu` 的注释与
`docs/BACKLOG.md` 的「git_service 家族分片并行随机误伤」条目）。

删掉侧车文件即退回按条数：实测 13.82 s（vs 11.86 s），这条是逆向验证。

### 7.3 正确性与稳定性证据

- **串行 vs 16 片逐用例比对**：`missing=0 extra=0 failDiff=0`（两份 JUnit 各 3095 例）。
  注意必须用 `--format junit --junit-out <路径>`——分片模式下 `ST_JUNIT_XML` 会被父进程
  **每片覆写**，外层的值不生效（踩过一次：拿到的是一份 819 例的旧报告）。
- **稳定性**：连跑 5 轮，每次 `exit=0`、`3095 passed / 0 failed`。
- **逆向验证**（逐条回退，确认对应判据变红）：
  - 把集成用例改回 `ST_TEST_WITH_TIMEOUT` → `slow_set_contains_integration_case` 在
    `found->slow` 处红；
  - 改测试源 → 必须重链并跑到新代码（`exe.last_write_time` 变化）；
  - `--no-strip` / 回默认 → 各判一次必须重链（`strip_debug` 入了指纹）；
  - `st build --define A=1` → 重链；同 define 再来一次 → 跳过；回退 → 重链；
  - `--force` → 重链；
  - 删耗时侧车 → 退回按条数且变慢（13.82 s vs 11.86 s）。

### 7.4 复现命令

```powershell
# 基线（必须重定向到文件，否则终端重绘污染数字）
cmd /c "build\bin\st.exe test code_editor              > tmp\perf\a.txt 2>&1"
cmd /c "build\bin\st.exe test code_editor --no-build   > tmp\perf\b.txt 2>&1"
cmd /c "build\bin\st.exe test                          > tmp\perf\c.txt 2>&1"
cmd /c "build\bin\st.exe build st                      > tmp\perf\d.txt 2>&1"

# 逐用例一致性（串行 vs 16 片）
build\bin\st.exe test --no-build --test-jobs 1 --format junit --junit-out tmp\cmp-serial.xml
build\bin\st.exe test --no-build              --format junit --junit-out tmp\cmp-shard16.xml
# 然后比对两份 XML 的用例名集合与 pass/fail 状态

# 看分片模式（有没有用耗时表）
get-content .st\logs\test-lib---*.log | select-string "按实测耗时装箱|按用例条数装箱"
# 看片内耗时和（逐片日志里累加 PASS/FAIL/SKIP 行的耗时）
get-childitem .st\logs\test-shard-*-of-16.log | sort-object LastWriteTime -descending | select -first 16
```


## 0. 测量纪律（三条，踩过才写下来）

1. **输出必须重定向到文件，不能进终端/管道**。实测：同一个空构建，输出到终端时
   每次 `print` 约 **37 ms**（终端重绘），14 行输出 = **+0.9 s**；重定向到文件后
   每次约 **0.1 ms**。对着终端量构建耗时等于把渲染时间算进编译时间。
2. **先刷新工具链再用它量**。`build/dev/bin/st.exe` 曾被当成"当前工具链"用了三天——
   它是 **10-02 的旧二进制**（早于 glob 下钻与测试默认档修复），于是"空构建 6 s"
   被测出来，而真相是 **0.28 s**。任何时候 `st` 自己也是被测对象：先
   `st build st`（或用另一个档的 `st`）确认手里这把尺子是新的。
3. **多轮取最小值**，且每轮之间不要改代码（本机有背景负载，单次测量会被污染）。

## 1. 被测对象与编译器

| 项 | 值 |
|---|---|
| 框架 | 霜天 `shuangtian`（自研构建驱动 `st`；C++23，PCH，97 单元库 + 87 个测试文件） |
| GCC | `g++` **16.2.0**（MinGW-w64，x86_64-win32-seh-rev1） |
| Clang（旧） | `clang++` **18.1.8**（LLVM 官方 Windows 版，**不可用作本机构建器**：见 §4） |
| Clang（新） | `clang++` **21.x**（LLVM.LLVM winget 包；MSVC STL 要求 Clang ≥ 20） |
| MSVC | 18 / cl 14.51（回退路径，未纳入本轮对比） |

## 2. 基线：构建与测试（修复/提速前）

口径：`cmd /c "st ... > 文件 2>&1"`，3 轮取最小值，工具链为**当时的最新 dev 档 `st`**。

| 操作 | 墙钟 | 说明 |
|---|---|---|
| `st build`（dev 档，无改动） | **0.28 s** | 全部 97 单元命中缓存；`build()` 内部仅 154 ms |
| `st build st`（改 tools/stpm） | 6.5 s | 1 单元重编（3.8 s）+ 链接（0.25 s）+ 固定成本 |
| `st test`（**单进程**，全量 819 例） | **24.5 s** | 测试执行占 24.3 s，编译/链接几乎为 0 |
| `st_tests.exe` 进程自身 | CPU **27.5 s** / 墙钟 **33.6 s** | 单核打满、其余 27 核空转 |

### 提速后的最终数字（本轮交付）

| 操作 | 提速前 | 提速后 | 倍数 |
|---|---|---|---|
| `st test`（全量 828 例） | 24.8 s | **5.0 s** | **5.0×** |
| `st build`（无改动） | 0.28 s | 0.31 s | —（已是缓存命中态） |
| 全套件结果与串行一致性 | — | **逐用例零差异** | — |

### 用例耗时结构（JUnit 逐用例口径，819 例共 23.6 s）

| 分位 | 用例数 | 累计耗时 |
|---|---|---|
| ≥ 500 ms | 2 | 1.4 s |
| ≥ 200 ms | 6 | 3.0 s |
| ≥ 100 ms | ~25 | 8.4 s |
| < 2 ms | **516** | 0.11 s |
| < 1 ms | **402** | ~0 s |

即：**402 个用例本身不到 1 毫秒**，而套件总时长 24 s——时间全在少数重型用例
（文本光栅化、控制通道 TCP 等待、DSL 组合）与**每例的观测开销**（独立线程 + 软超时等）。

## 3. 测试并行（分片）实测

`st test --test-jobs N` = 把用例按注册顺序分成 N 片，**每片一个独立测试进程**。

| N | 墙钟（含构建） | 纯执行 | 机时（用例耗时和） | 备注 |
|---|---|---|---|---|
| 1（等价分片前） | 24.1 s | 22.7 s | 23.7 s | 单核（纯执行基准 22.9 s） |
| 2 | 14.6 s | 13.3 s | — | |
| 4 | 9.0 s | 7.7 s | 29.0 s（1.22×） | |
| 8 | 6.3 s | 4.9 s | 34.6 s（1.46×） | |
| 12 | 5.7 s | 4.4 s | — | |
| **16** | **4.8 s** | 3.8 s | 50.5 s（2.13×） | 默认（28/2 夹到 16） |
| 20 | 5.2 s | 4.0 s | — | 已过最优 |
| 24 | 5.0 s | 3.6 s | 71.2 s（3.00×） | |
| 28 | 5.3 s | 3.9 s | 77.7 s（3.28×） | 墙钟反超 16 片 |

- **构建侧固定 1.34 s**（`st test --list`，无改动增量）；上表"墙钟"含它。
- 16 片与 20/24/28 各跑 4 轮取最小值：**16 片 4.75 s 最优**，
  20 片 5.18 s（+9%）、24 片 4.97 s（+5%）、28 片 5.30 s（+12%）。
  即默认档（16）确实在拐点上，再往上没有收益。

**加速比 6.0×（纯执行 22.9 s → 3.8 s），16 片是拐点**。
并行的代价是**机时**（用例耗时之和）：4 片 1.22×、8 片 1.46×、16 片 2.13×、28 片 3.28×。
即 16 片用 **2.1 倍机时换 6 倍墙钟**（并行效率 ~245%，因多核同时吃满）。

膨胀主要落在**真光栅化的 `text_*` 用例**上（CPU 竞争敏感），中位膨胀：
4 片 1.31×、**8 片 1.53×**、**16 片 2.31×**、20 片 2.61×、**28 片 3.20×**。
> 早先版本这里写的是"单例最多膨胀 24×"——那是把**某一条用例的偶发尖峰**
> 当成了稳定量。改为：单个最长用例（4 轮取最小值）28 片 1.5×、
> 24 片及以下 1.0×；上表的中位数才是有代表性的量。

**片数公式**（`tools/stpm/main.cpp`）：`min(16, max(1, 核数 / 2))`。

### 正确性证据（"并行 = 串行"不是想当然）

1. **逐用例结果比对**：串行报告 vs 4/8/16/28 片合并报告，
   `missing=0 extra=0 failDiff=0 dupes=0`（用例名集合与通过状态完全一致）。
2. **稳定性**：8/16/28 各连跑 3 次，每次退出码都是 0、各片计数与断言数一致。
3. **片数/结果一致性**：各片用例数 = ⌈819/N⌉ 或 ⌊819/N⌋，合计恒为 819。

**隔离边界（写下来，因为它是分片方案唯一的薄弱处）**：
- **内存状态**天然隔离（每片一个进程，与串行语义等价，片内顺序不变）。
- **文件系统不隔离**：写测试产物的用例必须按 `st::test::shard_suffix()` 分目录
  （已改 `tests/raster_software_scene_test.cpp`；其余用例走 `make_temp_dir`，天然唯一）。
- **端口**由系统分配（`port = 0`），无冲突。

## 4. Clang 可用性（本机）

| 路径 | 结论 |
|---|---|
| `clang++`（18.1.8，默认 MSVC 目标） | **不可用**：MSVC STL 14.51 显式要求 Clang ≥ 20（`STL1000`，`yvals_core.h:917` 的 static_assert） |
| `clang++ 18.1.8 --target=x86_64-w64-windows-gnu`（吃 MinGW libstdc++） | 能编译/链接，但 libstdc++ 头**编不过**（`__normal_iterator`「use of undeclared identifier」）；且生成的 `.exe` 在本机直接**崩溃**（退出码 0xC0000005，静态初始化期）⇒ 不可用于跑测试 |
| `clang++ 23.1.2 --target=x86_64-w64-windows-gnu` | **可用**（本轮接入）：libstdc++ 16.2 的头编得过，产物能跑 |

**为什么选 GNU 目标而不是 MSVC 目标**：走 MSVC 目标等于把"第二编译器"绑死在
Visual Studio 上（要它的头、库、CRT），而本框架在 Windows 上的默认口径本来就是
"GCC 风格 + MinGW"。走 GNU 目标后**两族共用同一份 libstdc++**——既没有额外的
工具链依赖，也让编译器对比真正只剩一个变量。

**代价（已处理）**：GNU 目标下 clang 暴露了一批 GCC 不报的**自家代码真缺陷**
（死函数、未使用的 lambda 捕获、未使用的私有字段、`size_type→difference_type` 隐式变号）。
这些逐个修了代码而不是加抑制；只有 vendored 第三方头的告警（`sqlite3.h` 的 `__int64`、
`quickjs.h` 的旧式转换与 `NAN`）才进清单的 `suppressions`。

## 5. gcc vs clang 对比（2026-10-06 实测）

### 5.1 结论先行

**在"两族共用同一份 STL（libstdc++）"这个正确口径下，全量构建两族持平**
（gcc 36.3/36.5 s，clang 37.3/38.7 s；差 2~6%，落在本机噪声内）。

| 口径 | gcc 16.2 | clang 23.1 | 说明 |
|---|---|---|---|
| **全量重建**（`st build gallery --profile dev --force`，28 路并行） | **36.3 / 36.5 s** | 37.3 / 38.7 s | 差 2~6%（两轮） |
| — 其中编译 | 35.6 / 36.7 s | 36.8 / 38.2 s | 同上 |
| — 其中链接 | 0.65 s | **0.25 s** | clang 侧走 LLVM 链接器，快 2.6× |
| **PCH 产物体积**（`.gch`） | 114 MB | **30 MB** | clang 小 3.8× |
| 库产物（`libst.a`） | 64.6 MB | 62.1 MB | 相差 4% |

**默认仍是 g++**，但理由与"clang 慢"无关：
1. g++ 是 MinGW 发行版自带，装一个包即可；clang 需要另装 LLVM；
2. 全量持平而 gcc 略快（且两轮都在同侧），没有换默认档的收益；
3. clang 的价值在**第二编译器门禁**——它拓出了一批 GCC 不报的自家代码缺陷
   （死函数、未使用的 lambda 捕获、未使用的私有字段、`size_type→difference_type`
   隐式变号），这类差异在 GCC 上永远不会报。

### 5.2 ⚠ 曾经得出的**相反结论**（保留为口径教训）

第一轮对比用的是 **clang + MSVC STL**（clang 默认目标）对 **gcc + libstdc++**，
测出"全量重建 clang 慢 15%"，并据此写下了"clang 的 PCH 反序列化更慢"的成因解释。

**那个结论是口径造出来的**：两边跑的是**两份不同的标准库实现**，差值里混着
"模板实例化成本"。改用同一份 libstdc++ 后，15% 的差距消失了。

这类错误的杀伤力在于它**看起来很有解释力**：当时还找到了一条自洽的旁证
（"clang 的 PCH 小 3.8 倍却更慢，说明不是加载大小的问题"）——一条错误结论
配一条自洽的解释，比一条裸错误更难被怀疑。

⇒ **教训**：比"编译器 A 与 B 谁快"时，除编译器外的一切都必须相同
（STL、ABI、目标三元组、命令行）。这条与 `docs/perceptual_changes.md` 的
"参照链路"是同一条原则，只是换到了构建领域。

### 5.3 逐单元（单线程、不带 PCH、同 STL，3 轮取最小值）

| 源文件 | 体积 | gcc | clang | clang/gcc |
|---|---|---|---|---|
| `src/pkg/build.cpp` | 108 KB | 7.31 s | **6.39 s** | 0.87 |
| `src/ui/dsl.cpp` | 90 KB | 8.08 s | **6.62 s** | 0.82 |
| `src/ui/components/code_editor.cpp` | 89 KB | **3.56 s** | 3.77 s | 1.06 |
| `src/control/server.cpp` | 88 KB | 6.37 s | **5.98 s** | 0.94 |
| `src/text/font.cpp` | 83 KB | **3.98 s** | 4.01 s | 1.01 |
| `src/text/text.cpp` | 83 KB | **3.58 s** | 3.80 s | 1.06 |
| `src/raster/canvas.cpp` | 69 KB | **2.77 s** | 2.40 s | 0.87 |
| `src/core/fs.cpp` | 25 KB | **3.81 s** | 3.36 s | 0.88 |
| `src/md/markdown.cpp` | 44 KB | **1.99 s** | 1.54 s | 0.77 |
| `tests/ui_table_test.cpp` | 12 KB | **3.13 s** | 3.53 s | 1.13 |

**逐单元看 clang 略占优（合计 clang/gcc = 0.93）**，而全量并行却持平——两者不矛盾：
全量受**内存带宽与调度**约束（28 路并行时瓶颈不在单核前端），单线程才看得到前端差异。
这也是"单线程最小值"与"全量墙钟"必须并列报告的原因：**换一个口径就能得到相反的方向**。

### 5.4 口径与可靠性

- 每个数字是 **3 轮最小值**（本机有背景负载，均值会被污染）；
- 输出全部重定向到文件（终端重绘会让每次 `print` 多付 ~37 ms）；
- 两族跑**同一条命令行**（仅编译器路径与各自的 `suppressions` 不同），
  同一份 libstdc++，同一个 `--target` 口径——所以差异只归给编译器。
- 全量重建两轮：gcc 36.28/36.47 s；clang 37.32/38.71 s（离散度 ~4%）。

### 5.5 接入方式（可选，不是构建前置）

`st.pkg` 的 `toolchains.clang` 指向一份本地 LLVM 安装，并声明
`"target_triple": "x86_64-w64-windows-gnu"`（走 libstdc++ 那条口径，不依赖 VS）。
**没装 clang 的机器不受影响**（默认档仍走 g++）。
跑第二编译器门禁：`st build gallery --toolchain clang --profile dev`。

## 6. 复现命令

> **迭代速度（改一行 → 看到结果）的基线、四类改动与口径**见 `docs/ITERATION_SPEED.md`。
> 那份记的是日志落盘、自举增量、并行度、UTF-8，与本文件的编译器对比/分片调参互补。

```powershell
# 基线（必须重定向到文件，否则终端重绘会污染数字）
cmd /c "build\dev\bin\st.exe build > tmp\b.txt 2>&1"
cmd /c "build\dev\bin\st.exe test --test-jobs 1 > tmp\s1.txt 2>&1"   # 单进程
cmd /c "build\dev\bin\st.exe test              > tmp\s16.txt 2>&1"   # 默认分片

# 逐用例画像（写 JUnit 后统计）
$env:ST_JUNIT_XML="tmp\tests.xml"; cmd /c "build\dev\bin\st.exe test --test-jobs 1 > tmp\t.txt 2>&1"

# 分片与串行结果一致性
python tools/compare_junit_shards.py tmp/serial.xml tmp/shard16.xml

# 第二编译器（clang）门禁
cmd /c "build\dev\bin\st.exe build gallery --toolchain clang --profile dev > tmp\clang.txt 2>&1"

# 逐单元编译器对比（单线程 3 轮取最小值；带/不带 PCH 两个口径）
python tmp/compiler_bench.py        # 不带 PCH
python tmp/compiler_bench_pch.py    # 带 PCH（日常路径）
```

> `tmp/compiler_bench*.py` 是一次性对比脚本（不进交付）：它们把编译器的完整命令行
> 写死在文件里，与构建器自己的标志集是**两份**。要长期持有这两把尺子，应把它们改成
> 从 `st` 导出命令行（如 `st build --verbose` 的 dump）再执行——那是一个独立小课题。

# 霜天文档地图

**哪份文档答哪个问题、它还跟不跟着代码变、读它多少成本。**

> 这份地图是**入口**：除了它自己，`docs/` 下**全部 49 份** `.md` 都在下表里。
> 完整性由测试 `docs_map_covers_every_file` 保证——**新增文档忘了登记会立刻红**，
> 而不是等半年后有人发现地图不准（本文件曾长期只收了 26/49 份）。

## 怎么用这份地图（三种读法）

| 你的处境 | 读法 |
|---|---|
| **知道要做什么** | 跳到下面的「按意图索引」——那张表的主轴是**你想干什么**，不是文件名 |
| **知道要找哪份** | 看 §A–§E 的分类表（按文档性质分组） |
| **只有 30 秒** | 只看每份标的**体量**档：`速查`（<5k 字，通读）· `详读`（5–25k）· `档案`（>25k，**只读指定节**） |

## 两个必须先知道的约定

**① 寿命契约**——决定你该信多少：

| 契约 | 含义 | 怎么用 |
|---|---|---|
| **活** | 随代码同步更新，**当前真相** | 与代码冲突时以代码为准，但文档不一致属缺陷，应修 |
| **实测** | 内容**冻结**在某一时刻（某次实测的原始数据） | 数据仍有效但**环境相关**；结论可能已被后续改动超越 |
| **记录** | 已落地的设计/修复记录，讲**为什么这么做** | 不要按它改代码（它描述过去）；要改就看它记的边界与代价 |
| **接口** | 被**源码注释按章节号引用** | **章节号是稳定接口，禁止重编号**——改之前先 grep `§` |

> ⚠ **源码引用 = 硬接口**。以下文档的章节号被 `src/`、`include/`、`tests/` 的注释直接引用，
> 重编号会静默打断论证链（读者按注释去查，查到的是别的内容）：
> `BACKLOG.md`、`declarative.md`、`PAINT_DIAGNOSIS.md`（各 4~6 处源码引用）。

**② 体量档**——决定你怎么读：

| 档 | 字数 | 建议 |
|---|---|---|
| `速查` | < 5k | 通读，它就是为了被一口气读完 |
| `详读` | 5k–25k | 按需读相关章节 |
| `档案` | > 25k | **不要通读**，按下面标的节号定点查 |

---

## 按意图索引（主入口）

> 标注的 `§` 是文档内章节号。**这些编号是稳定接口**——代码注释、测试、其它文档都在引用它们
> （例如 `src/ui/components/slider.cpp` 写着"同 `DESIGN.md` §8.2 第 26 条"），不要重编号。

### 我要开始

| 我想…… | 去哪 | 体量 |
|---|---|---|
| 跑起来看效果 | `../README.md`「快速开始」（Windows / Linux 两版） | 详读 |
| 建一个自己的应用工程 | `independent_project.md`（清单/构建/运行全流程） | 详读 |
| 把霜天当通用 C++ 库用 | `AS_LIBRARY.md` | 详读 |
| 知道模块怎么分层、有哪些已知违规 | `STRUCTURE.md` | 速查 |
| 知道有哪些示例、怎么跑全量 e2e | `EXAMPLES_E2E.md` | 速查 |

### 我要画东西

| 我想…… | 去哪 | 体量 |
|---|---|---|
| **画三维场景**（图元/组合/动画/蒙皮/glTF/拾取/贴图/过程化） | **`geometry.md`**（唯一权威；能力全在这份） | **档案**（26k，按 §4.x 定点查） |
| 知道三维能力对标 Blender 还差什么 | `scene3d_capability_plan.md`（差距与路线） | 详读 |
| 让大模型写三维脚本（机读清单/自检） | 跑 `build/dev/bin/st_api.exe`（`--example` / `--check` / `--normalize`） | —— |
| 做视觉上"讲究"的三维画面 | `visual_craft.md`（三维画面的制作手册） | 速查 |
| 用三维做视频（裁云） | `caiyun_ONBOARDING.md` → `caiyun_layout_script.md` → `caiyun_motion_acceptance.md` | 详读/详读/详读 |
| 知道裁云能做什么、边界在哪 | `caiyun_capability_ledger.md`（能力台账） | 速查 |
| 写剧本（裁云/镂月通用模板） | `script_design.md`（先写剧本再写脚本的六部分模板） | 详读 |
| 看两个现成的能力展示片子怎么做的 | `剧本-歌白收编.md`（96 行）· `剧本-裁云一镜到底.md`（63 行） | 速查/速查 |
| 对标 onetake 的能力差距 | `onetake_gap_analysis.md`（全链路盘点）· `onetake_13case_benchmark.md`（13 条逐条对标） | 速查/详读 |
| 知道为什么"一镜到底"要那样设计 | `caiyun_onedot_concept.md` · `caiyun_carry.md`（连贯度验收） | 速查/速查 |
| 看镂月裁云两示例的设计规约 | `LOUYUE_CAIYUN.md`（权威现状见 `../README.md`「示例」节） | 详读 |

### 我要写代码

| 我想…… | 去哪 | 体量 |
|---|---|---|
| **动手前必读**（禁用特性/强制特性/lint 规则） | `../CONVENTIONS.md` | 详读 |
| 改观感（字体/配色/动画/手感） | **`perceptual_changes.md`**（先读它：正交量、量尺三坑、八步循环） | 详读 |
| 量"指标测不到的那一点精致度" | `polish.md` | 速查 |
| 写跨平台代码 | `../CONVENTIONS.md` §10（红线）→ `cross_platform.md`（详细版 + 自查清单） | 详读/速查 |
| 写声明式界面 | `declarative.md`（**§5.5「调用方必须知道的三件事」是硬契约**） | 详读·接口 |
| 用 XML + JS 写应用 | `xml-ui.md`（`examples/xmlapp` 通路） | 详读 |
| 改文件即生效（不重编不重启） | `hot-reload.md`（`st::ui::HotReload`；`../DESIGN.md` §4.2.28） | 速查 |
| 写测试 | `../CONVENTIONS.md` §7；"假绿测试"的教训在 `../DESIGN.md` §8.3.2 | —— |
| 用真按键验界面交互（输入/快捷键/回显） | `../CONVENTIONS.md` §7.7.1（三条注入通道怎么选）+ §7.7.2 | —— |
| 判断一条结论可不可信（判据自己会不会错） | `../CONVENTIONS.md` §7.8（判据先在已知样本上自验） | —— |
| 搞懂包管理器 / 构建系统 | `../DESIGN.md` §7；"改头文件不重编"那个坑在 §8.2 第 30 条 | —— |
| 用跨编译器拓缺陷（GCC 不报的） | `BUILD_CHECK.md`；`st check --toolchain=clang` | 速查 |
| 知道构建为什么这样隔离（多 worker 互踩） | `BUILD_ISOLATION_PLAN.md`（**方案未落地**，只分析） | 详读 |
| 知道 SQLite 怎么内置进来的 | `sqlite_integration.md` | 详读 |
| 知道字体渲染的像素级对齐怎么做的 | `DWRITE_PIXEL_BENCHMARK.md`（Windows 原生链路像素对照） | 详读 |
| 知道字重曲线怎么映射（跨字号连续） | `WEIGHT_CURVE.md` | 速查 |

### 我要验收 / 自查

| 我想…… | 去哪 | 体量 |
|---|---|---|
| **收任何一单之前：逐项该查什么** | **`VERIFY-PLAYBOOK.md`**（判据总表：按交付类型分行，每道门一句话 + 命令） | 详读 |
| **别重蹈覆辙** | 三处按场景查：`PITFALLS.md`（**坑位速查**，按"你此刻在做什么"索引）→ `../DESIGN.md` §8.2（60 条真实缺陷）→ `BACKLOG.md`「已完成」段（完整取证过程） | 档案/——/档案 |
| 知道某个行为是不是被保证的 | **grep `tests/` 的用例名**——每条 `ST_TEST` 就是一条不变量（173 个 .cpp；文档会漂，测试会红） | —— |
| 知道改动前后的性能/内存/产物大小 | `BASELINE_METRICS_20261014.md`（可复跑数字 + 内存去向分解） | 详读 |
| 知道迭代速度基线（构建/测试多久） | `ITERATION_SPEED.md` | 详读 |
| 知道构建/测试性能的编译器差异 | `BUILD_TEST_PERF.md` | 详读 |
| 知道软件渲染路径怎么从 14.9ms 优化到 3.95ms | `PERF_2026-10-19.md` | 详读 |
| 知道整帧 paint 成本归因到哪个原语 | `PAINT_DIAGNOSIS.md`（§2.2/§3.1 被源码引用） | 详读·接口 |
| 知道字体渲染与浏览器的差距 | `TEXT_AB_REPORT.md`（全字符集 + 全字号）· `SKIA_TEXT_RENDERING_STUDY.md`（Skia 覆盖率的调研） | 详读/详读 |
| 知道"记录过的知识为什么还是没被用上" | `DOC_USABILITY_FINDINGS.md`（**文档可用性实测诊断**） | 速查 |
| 知道接手文档是否真的够用 | `_ONBOARDING_CHECK.md`（接手检查记录） | 速查 |

### 我要定方向 / 查历史

| 我想…… | 去哪 | 体量 |
|---|---|---|
| **还没做什么** | `BACKLOG.md`「待做」段（**唯一权威清单**；「已完成」段是历史考古层） | 档案（29 万字，**只查不读**） |
| 某次修复为什么这么做、付出了什么代价 | `BACKLOG.md`「已完成」段（按缺陷名 grep） | 档案 |
| 知道结构优化那轮做了什么 | `OPTIMIZATION_BASELINE.md`（改动前）→ `OPTIMIZATION_PROGRESS.md`（过程）→ `OPTIMIZATION_REPORT.md`（收尾，17 个提交） | 速查×3 |
| 看一次大分支合并前的复核范式 | `MERGE_REVIEW.md`（16 笔逐笔：改动/验证/风险/回滚点） | 速查 |
| 知道终端那轮修了什么 | `TERMINAL_FIXES_20261008.md`（光标错位/输入回显/多会话串台的根因表） | 速查 |
| 知道片指标不达标时怎么救 | `ot_bench_fixes.md`（实测轨迹） | 详读 |
| 改文档 | 本文件末「写作约定」与「新知识该写哪份」 | —— |

---

## A. 活文档 —— 写代码前后的必读

| 文档 | 回答的问题 | 体量 | 契约 |
|---|---|---|---|
| [`../README.md`](../README.md) | 它是什么、怎么跑起来、现在支持什么 | 详读 | 活 |
| [`../DESIGN.md`](../DESIGN.md) | 为什么这样设计、接口长什么样、踩过哪些坑 | 档案 | 活 |
| [`../CONVENTIONS.md`](../CONVENTIONS.md) | 写代码必须遵守什么（禁用/强制特性、lint） | 详读 | 活 |
| [`geometry.md`](geometry.md) | **三维建模能力**（几何层）：图元/组合/动画/蒙皮/glTF/拾取/贴图/过程化/生态素材/大模型可用性 | 档案 | 活 |
| [`cross_platform.md`](cross_platform.md) | 跨平台会怎么错、提交前逐条自查什么 | 速查 | 活 |
| [`declarative.md`](declarative.md) | 声明式 UI（`st::ui::dsl` + `declarative.js`）的设计与**调用方契约** | 详读 | 活·**接口** |
| [`perceptual_changes.md`](perceptual_changes.md) | 观感类改动怎么做：正交量、量尺三坑、八步循环 | 详读 | 活 |
| [`PITFALLS.md`](PITFALLS.md) | **坑位速查**：按**发生场景**索引"这类事以前踩过什么" | 档案 | 活·索引 |
| [`VERIFY-PLAYBOOK.md`](VERIFY-PLAYBOOK.md) | **收单前逐项勾的验收判据总表** | 详读 | 活·索引 |
| [`EXAMPLES_E2E.md`](EXAMPLES_E2E.md) | 五示例 e2e 驱动入口表 | 速查 | 活 |
| [`STRUCTURE.md`](STRUCTURE.md) | 模块分层与已知违规 | 速查 | 活 |
| [`AS_LIBRARY.md`](AS_LIBRARY.md) | 把霜天当通用 C++ 库用 | 详读 | 活 |
| [`independent_project.md`](independent_project.md) | 怎么用本框架建**自己的**应用工程 | 详读 | 活 |
| [`hot-reload.md`](hot-reload.md) | 改文件即生效（原子替换、指纹判变、清场语义） | 速查 | 活 |
| [`script_design.md`](script_design.md) | **剧本设计模板**（裁云/镂月通用六部分） | 详读 | 活 |
| [`visual_craft.md`](visual_craft.md) | 视觉制作手册（裁云 · 三维画面） | 速查 | 活 |
| [`polish.md`](polish.md) | 精致度：指标测不到的那一点 | 速查 | 活 |
| [`WEIGHT_CURVE.md`](WEIGHT_CURVE.md) | 字重曲线（跨字号连续映射） | 速查 | 活 |
| [`README.md`](README.md) | 本文件 | 详读 | 活 |

## B. 待办台账

| 文档 | 回答的问题 | 体量 | 契约 |
|---|---|---|---|
| [`BACKLOG.md`](BACKLOG.md) | **还没做什么**（待做段）+ 已做过什么（含每条根因与代价） | 档案 | 活·**接口** |

> **本文件是待办的唯一权威清单**。「已完成」段是历史考古层——它被源码按章节号引用，
> 记的是"这个缺陷的根因、修的时候付出了什么代价"，不要当现状读。
> **29 万字，不要通读**：按缺陷名 grep。

## C. 实测依据 —— 数字的来源，**冻结但必须可寻址**

数据是某次实测的快照，但被别的文档与源码引用（"这个数的出处"）。
不要因为"时间久了"就删——删了论证就断了。要更新就在文内追加新一轮，**不要覆盖旧数字**。

| 文档 | 内容 | 体量 | 契约 |
|---|---|---|---|
| [`PAINT_DIAGNOSIS.md`](PAINT_DIAGNOSIS.md) | 整帧 paint 成本分解（归因到原语）+ 测量口径缺陷 | 详读 | 实测·**接口** |
| [`BASELINE_METRICS_20261014.md`](BASELINE_METRICS_20261014.md) | 基础指标基线（性能/产物大小/内存）+ 内存去向分解 | 详读 | 实测 |
| [`BUILD_TEST_PERF.md`](BUILD_TEST_PERF.md) | 构建/测试性能基线（gcc vs clang、并行提速） | 详读 | 实测 |
| [`ITERATION_SPEED.md`](ITERATION_SPEED.md) | 迭代速度基线（2026-10-26） | 详读 | 实测 |
| [`PERF_2026-10-19.md`](PERF_2026-10-19.md) | 渲染性能专项：软件路径 14.9 → 3.95 ms | 详读 | 实测 |
| [`DWRITE_PIXEL_BENCHMARK.md`](DWRITE_PIXEL_BENCHMARK.md) | 字体渲染 × Windows 原生链路像素级对标 | 详读 | 实测 |
| [`TEXT_AB_REPORT.md`](TEXT_AB_REPORT.md) | 与浏览器字体渲染对照（全字符集 + 全字号） | 详读 | 实测 |
| [`SKIA_TEXT_RENDERING_STUDY.md`](SKIA_TEXT_RENDERING_STUDY.md) | Skia/Chromium 覆盖率与网格拟合怎么做的 | 详读 | 实测 |
| [`ot_bench_fixes.md`](ot_bench_fixes.md) | 把片子从「指标不达标」救回来：实测轨迹 | 详读 | 实测 |
| [`DOC_USABILITY_FINDINGS.md`](DOC_USABILITY_FINDINGS.md) | 文档可用性实测诊断：记录过的知识为何仍没被用上 | 速查 | 实测 |
| [`_ONBOARDING_CHECK.md`](_ONBOARDING_CHECK.md) | 接手文档检验记录（只读文件作答案） | 速查 | 实测 |

## D. 设计与修复记录 —— 读它是为了**别重蹈**

| 文档 | 讲的是什么 | 体量 | 契约 |
|---|---|---|---|
| [`sqlite_integration.md`](sqlite_integration.md) | SQLite 源码内置：为什么这么接、边界划在哪 | 详读 | 记录（已落地） |
| [`BUILD_CHECK.md`](BUILD_CHECK.md) | `st check` 跨编译器检查：为什么需要它 | 速查 | 记录（已落地） |
| [`xml-ui.md`](xml-ui.md) | 纯 XML + JS 通路（`examples/xmlapp`） | 详读 | 记录（已落地） |
| [`TERMINAL_FIXES_20261008.md`](TERMINAL_FIXES_20261008.md) | 终端修复轮：光标错位/输入回显/多会话串台 | 速查 | 记录（已落地） |
| [`OPTIMIZATION_BASELINE.md`](OPTIMIZATION_BASELINE.md) | 结构优化**改动前**快照 | 速查 | 记录 |
| [`OPTIMIZATION_PROGRESS.md`](OPTIMIZATION_PROGRESS.md) | 结构优化过程与交接 | 速查 | 记录 |
| [`OPTIMIZATION_REPORT.md`](OPTIMIZATION_REPORT.md) | 结构优化收尾（17 个提交，每阶段可回滚） | 速查 | 记录 |
| [`MERGE_REVIEW.md`](MERGE_REVIEW.md) | `optimize/ui-viewport-2026` 合并前复核（16 笔逐笔） | 速查 | 记录（分支已并入） |
| [`BUILD_ISOLATION_PLAN.md`](BUILD_ISOLATION_PLAN.md) | **构建隔离方案（未落地）**：多 worker 互踩的四层机制 + 改动面与代价 | 详读 | 方案（只分析未落地） |

## E. 专题：三维 / 裁云 / 镂月

| 文档 | 讲的是什么 | 体量 | 契约 |
|---|---|---|---|
| [`scene3d_capability_plan.md`](scene3d_capability_plan.md) | 三维渲染能力：对标 Blender 的差距与路线 | 详读 | 活（路线部分） |
| [`LOUYUE_CAIYUN.md`](LOUYUE_CAIYUN.md) | 「镂月裁云」两示例的设计规约 | 详读 | 记录（权威现状见 `../README.md`「示例」节） |
| [`caiyun_ONBOARDING.md`](caiyun_ONBOARDING.md) | 裁云片子制作 · 接手须知（新人从这里开始） | 详读 | 活 |
| [`caiyun_capability_ledger.md`](caiyun_capability_ledger.md) | 裁云能力台账（Capability Ledger） | 速查 | 活 |
| [`caiyun_layout_script.md`](caiyun_layout_script.md) | 裁云 · 脚本编排层（`layout.js`） | 详读 | 活 |
| [`caiyun_motion_acceptance.md`](caiyun_motion_acceptance.md) | 裁云 · 动效验收手册（三层判据） | 详读 | 活 |
| [`caiyun_carry.md`](caiyun_carry.md) | 裁云 · 跨场承接（carry）与连贯度验收 | 速查 | 活 |
| [`caiyun_onedot_concept.md`](caiyun_onedot_concept.md) | 歌白 · 一镜到底（对标 onetake 的 one-dot） | 速查 | 记录 |
| [`onetake_gap_analysis.md`](onetake_gap_analysis.md) | 对标 onetake：能力差距全链路盘点 | 速查 | 活 |
| [`onetake_13case_benchmark.md`](onetake_13case_benchmark.md) | 裁云 × onetake：13 条示例逐条对标 | 详读 | 实测 |
| [`剧本-歌白收编.md`](剧本-歌白收编.md) | 剧本 · 歌白能力片「收编」 | 速查 | 记录 |
| [`剧本-裁云一镜到底.md`](剧本-裁云一镜到底.md) | 剧本 · 裁云「一镜到底」能力展示（16 秒） | 速查 | 记录 |

## F. 各目录里的文档

| 文档 | 回答的问题 |
|---|---|
| [`../tools/README.md`](../tools/README.md) | 探针/脚本怎么用、依赖放哪（代码进仓库 / 二进制走资源管理） |
| [`../third_party/SOURCES.md`](../third_party/SOURCES.md) | 随仓库分发的第三方源码、版本/来源/许可、动了哪些上游代码 |
| [`../third_party/sqlite/PROVENANCE.md`](../third_party/sqlite/PROVENANCE.md) | SQLite 的取数路径、改动面、裁剪结果 |
| [`../../resources/models/`](../../resources/models/)（主仓库） | 模型与运行时资源的实际存放地（二进制资源不进本仓库） |

---

## 非文档形态的知识（同样要找得到）

知识不只在 `.md` 里。三类"活"的沉淀：

**① 探针与脚本（`tools/`）**——用途见 [`../tools/README.md`](../tools/README.md)，按用途分：

- **e2e 驱动**（`*_e2e.py`）：控制通道端到端（`gbcode`/`gallery`/`louyue`/`caiyun`/`split_view`/`theme_file`/`input_action`/`visual_assert`/`events`/`framework_gaps`）
- **逆向验证**（`*_reverse_verify.py`）：把修复回退、断言变红——证明测试**真能抓住**缺陷
- **像素/文字量测**（`*_probe.py`、`*_ink*.py`、`text_*.py`）：从截图里量几何与墨量
- **诊断探针**（`tools/*.cpp`）：`crash_probe`、`icon_metric_probe`、`make_sample_png` 等
- **三维与生态**（`tools/*.cpp`）：`real_asset_probe`（真实素材渲染成图）· `ecosystem_probe`（素材保留率总账）· `jpeg_probe` · `procedural_probe`（过程化 + 通道开销）· `pick_probe` · `texture_probe` · `llm_usability_probe`（模拟模型犯错量报错质量）
- **给模型用的入口**：`st_api`（能力清单 / `--check` 自检 / `--normalize` 看理解）· `tools/fetch_assets.ps1`（取生态素材）
- **文档/结构核对**（`check_docs.py`、`check_selfcontained.py`）

**② 测试即规格（`tests/`）**——每条 `ST_TEST` 的名字就是一条**不变量**。
想确认"某个行为是不是被保证的"，grep 用例名比读文档更准（文档会漂，测试会红）。

**③ 源码注释里的成因**——最靠近代码、最不容易漂的一层。本仓约定：
注释写**为什么**（尤其是"为什么不那样写"），并在注释里给出来源的文档章节号。

---

## 写作约定（改文档时守这几条）

1. **数字必须实测**。性能、用例数、断言数、文件数都写"实测值"并注明口径与环境；
   没有实测的宁可写"*未测*"，也不填猜测值（`../DESIGN.md` §11 就是这么处理的）。
   **口径不同要写明**（例：用例数有"源码声明"与"实跑"两个口径，差在慢用例跳过）。
2. **状态以代码为准，不以计划为准**。写着"待做"却已完成的条目会误导读者——这类漂移
   在本仓库真实发生过（GPU 行曾长期写"⏳ M3+ 待做"而 M1–M6 早已落地）。
3. **章节号是接口**。要重编号就得同步改代码注释与其它文档里的引用，先 grep `§` 再动。
4. **教训要保留成因**。只写"要用 A 不用 B"没用；写上"因为 B 会静默给出错误结果"才不会
   被人"顺手改回去"。
5. **区分"设计意图"与"当前实现"**。两者不一致时要显式说明是哪一种。
6. **仓库根**：路径分两套——本工程（`shuangtian/`）与主仓库（其上一级）。
   写引用时用**带目录名的相对路径**（`docs/<文件名>.md` 的形态），两套基准都能解析；
   不要写没有目录前缀的裸文件名。**示例里的占位符会被 `tools/check_docs.py` 当成实引用
   而报错**——那是设计如此：宁可报错，也不要一个查不出来的假路径。

## 新知识该写哪份

写下来之前先问一句：**这条知识会跟着什么一起变？** 答案是分流的依据。

| 知识的性质 | 写到哪 | 为什么 |
|---|---|---|
| **框架的缺陷/缺口**（该有没有、行为与文档不符） | `docs/BACKLOG.md` 待做段 | 它是唯一权威清单 |
| **可复用的方法/纪律**（怎么量、怎么验、怎么避免某类错） | 对应**专题手册**（观感 → `perceptual_changes.md`；跨平台 → `cross_platform.md`；声明式契约 → `declarative.md`；三维 → `geometry.md`） | 方法要成体系才有用；散在提交说明里等于没有 |
| **子Agent 必知的纪律**（何时用、去看哪一份） | 子Agent **提示词** | 只写导引，**不复述**文档内容（两处同义必然漂移） |
| **具体改动的缘由**（这次为什么这么改） | **git 提交说明** | 与代码同生共死；`git log` 就是它的检索入口 |
| **实测数据**（性能基线、对照实验） | `docs/` 下**新开一份实测报告**，并在地图上登记 | 报告要可寻址；写进地图才不会变成孤儿文件 |
| **反直觉的写法**（为什么这里不能那样写） | **源码注释**（就近）+ 必要时文档章节号 | 最靠近代码，改的人一定会看到 |
| **一次性的过程记录**（本次怎么定位的） | 提交说明或 `BACKLOG` 已完成段 | 不要单开文件——孤儿文档是地图腐烂的主因 |

**判断口诀**：写得下（一处）就近写；写得散（多处要引用）才开新文档，**并登记到本地图**
（漏登记会被 `docs_map_covers_every_file` 这条测试抓住）。

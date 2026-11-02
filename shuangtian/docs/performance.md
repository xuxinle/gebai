# 性能分析体系（面向**执行者**的使用手册）

> **这份文档的读者是模型/脚本/CI，不是人。** 所以它按「我想知道 X → 跑这条 → 读这几个字段」
> 组织，而不是按模块介绍。每个结论都要求带**统计量**与**口径**——裸数字在本体系里是缺陷。

## 0. 一句话

```bash
st perf gallery            # → 一条命令，拿到"该改哪里"的结论
```

它做四件事：增量构建 → 无头启动（带剖析）→ 采样 → 输出诊断。**不需要知道端口、协议、探针名字。**

---

## 1. 三条命令，覆盖三类问题

| 我想…… | 命令 | 返回 |
|---|---|---|
| 知道"它现在慢在哪" | `st perf <target>` | `findings[]`（每条=判据+证据+位置+建议） |
| 知道"这次改动有没有让它变慢" | `st perf <target> --baseline 旧.json` | 逐指标 `当前/基线−1` + 变慢项计数 |
| 只比两份已存报告（毫秒级） | `st perf --compare 旧.json,新.json` | 同上，不构建不运行 |

可选参数：

| 参数 | 默认 | 说明 |
|---|---|---|
| `--frames N` | 200 | 剖析采样帧数。**≥30 才有统计意义**（报告会自己说样本够不够） |
| `--bench N` | 同 frames | 让应用每帧强制全量重绘。**默认开启**——静止界面第二帧起不重绘，拿不到样本 |
| `--bench 0` | — | 显式关闭 → 量**真实交互**（含局部重绘），帧数会少很多 |
| `--profile NAME` | release | 构建/运行档。**用 dev/debug 的数字不能和 release 比** |
| `--out PATH` | `tmp/perf-<target>.json` | 报告落盘位置 |
| `--json` | 关 | 报告原样打到 stdout（自动化读取） |

---

## 2. 报告怎么读（字段语义）

报告是 JSON，`schema` 固定为 `st.perf.report/1`（**字段语义由它保证**）。

```
context   测量环境（后端/渲染器/尺寸/DPI/构建档/帧数）—— 没有它，数字不可比
caveats[] 口径自检：ok=false 就是"这份数据不能用来下结论"
findings[]结论：severity/kind/title/evidence/where/action
sections  证据层：metrics（可比较标量）/ phases / elements / hot_elements / frames
```

### 2.1 `caveats`：先看这里，再看结论

| kind | ok=false 的含义 | 怎么补救 |
|---|---|---|
| `profile` | 跑的是 debug 档（慢 2~4 倍），**不能与其他档比** | `--profile release` 重跑 |
| `samples` | 稳态样本 < 30，分位不可作为结论 | 加大 `--frames` |
| `cold_start` | （信息，恒 ok）首 N 帧已排除出分位，另计 | — |
| `clock` | 无 CPU 刻度，元素级读数含读表开销 | 相对排序仍有效，绝对值偏高 |
| `coverage` | 逐帧序列被截断（只留最后 N 帧） | 加大 `--frames` |
| `instrumentation` | 埋点异常（栈溢出/未配对 end），读数会偏小 | 查埋点 |

**规则：任何 `ok=false` 的条目，都要在结论里提一句。** 这是本体系存在的意义之一——
模型极易把 debug 档的数字当 release 用。

### 2.2 `findings`：按 `kind` 分流，不要读标题文案

| kind | 含义 | 该看哪里 |
|---|---|---|
| `frame_budget` | 帧内 p95 与 16.67ms 预算的关系 | `sections.metrics.frame_p95_ms` |
| `cold_start` | 首帧耗时（含字体首栅格化/缓存建表） | `--perf-startup` / `ST_PERF_STARTUP=1` |
| `pacing` | 墙钟 ≫ 帧内 = **被限帧**，不是慢 | 无需处理（用 `--bench` 才量上限） |
| `jitter` | p95/p50 大 → 有毛刺 | `where` 里列了最贵的几帧 |
| `idle_churn` | 大量空帧（无像素变化仍走帧路径） | 查谁在每帧请求重绘 |
| `dominant_paint` / `dominant_layout` / `dominant_present` | 某段占比 ≥60% | 对应分段与元素热点 |
| `element_class_hotspot` | 某**类**元素每帧自持耗时占比高 | `sections.elements` |
| `element_hotspot` | 某个**具体**元素最贵 | `where` = 元素 id |
| `startup_total` / `startup_dominant` | 冷启动总时长与其最贵一步 | `sections.steps` |

### 2.3 `sections.hot_elements` 的口径（**易读错，务必看这段**）

```jsonc
{"id": "input-search", "type": "Input", "mean_ms": 2.10, "total_ms": 2.10, "n": 41}
```

- `mean_ms` = **自持**耗时（不含子孙），**按该元素自己出现的帧数**平均；
- `total_ms` = 含子孙的耗时；
- **排序按 `mean_ms`**。

为什么必须分两个：只报含子孙的耗时，排序**永远落在根容器上**（它自己没有几行代码，
代价全是子元素之和）。实测踩过：首版报告把 `app-root` 报成"每帧 55.9ms"，
读者只能得出"整个界面慢"这个已知事实——**这条就是"看不出该改哪里"的成因**。

---

## 3. 埋点：怎么给新代码加剖析

```cpp
#include "st/perf/scope.hpp"

void paint_expensive_thing(...) {
  // 阶段计时（嵌套安全：parent.self 不含 child，不会凭空多出"未归因"时间）
  st::perf::Scope scope{"paint.shadow"};   // 作用域结束自动记账
  ...
}
```

- **名字用 `模块.动作` 形式**（`paint.shadow` / `layout.list`），报告按名字聚合；
- **调用点自动记录**（`std::source_location` 默认实参）：同一名字的几十处调用会自动
  分成几十个调用点桶，报告直接给出最贵的那一处 `file:line`；
- **关闭时零成本**：`Scope` 构造第一件事就是查 `enabled()`，关掉时不读时钟。

启动分解：

```cpp
st::perf::startup_mark("font.load");   // 关键里程碑，同名多次会合并
```

`ST_PERF_STARTUP=1` 时退出会打印「冷启动分解：总 200.8ms（最贵前 5 步）」。

**框架已内置的里程碑**（不需要自己加）：
`app.start` → `backend.create` → `window.create` → `before_first_frame` → `first_frame`。

---

## 4. 六类常见误读（每条都踩过）

| # | 误读 | 真相 | 判据 |
|---|---|---|---|
| 1 | "墙钟 19ms，渲染好慢" | 帧内只有 3.9ms，**被帧预算限帧** | `findings` 里的 `pacing`；看 `wall_*` vs `frame_*` |
| 2 | "p95 19ms，超预算了" | 混进了 200ms 的**冷启动帧** | `caveats` 的 `cold_start`；`sections.metrics.steady_frames` |
| 3 | "dev 档 10ms，比 release 慢 3 倍" | 档位不同，**根本不可比** | `caveats` 的 `profile`；`--compare` 会直接拒绝比 |
| 4 | "`app-root` 每帧 56ms" | 那是**含子孙**的耗时，它自己几乎不花时间 | 看 `mean_ms`（自持）不是 `total_ms` |
| 5 | "3 个样本的中位数是 4.5ms" | 样本不足，分位没有意义 | `caveats` 的 `samples`；`n < 30` |
| 6 | "`--bench` 的数字就是真实体验" | 那是**最坏情况**（每帧全量重绘） | 真实交互用 `--bench 0` |

---

## 5. 当前基线（2026-11-02 实测，dev 档 / headless / DPI 1.5 / 1178×736）

> **契约：实测（冻结）**。要更新就追加新一轮，**不要覆盖旧数字**。

```bash
st perf gallery --frames 80      # 复现
```

| 项 | 值 | 说明 |
|---|---|---|
| 稳态帧 p50 / p95 | **9.98 / 10.37 ms** | `--bench` 模式（每帧全量重绘，最坏情况） |
| 冷启动首帧 | **~202 ms** | 其中 `first_frame` 185.8ms（字体首栅格化 + 图元缓存建表） |
| 启动分解（ST_PERF_STARTUP=1） | 总 200.8ms | `first_frame` 185.8 / `before_first_frame` 11.8 / `window.create` 3.2 |
| 最贵元素（自持） | `input-search` **2.10ms** | `Input` 类两个实例各约 2ms |
| paint 段占比 | **97%** | layout 0.27ms / paint 9.84ms / present 0 |

**已知的真实性能结论**（由本体系首次量化出来）：
- 首帧 202ms 里 **185.8ms 在 `first_frame`**——即首次绘制时的字体光栅化与图元缓存建表。
  这是一次性成本，但它决定了"用户多久看到界面"。
- 稳态 paint 9.84ms 中，`Input` 类两个实例合计约 4.1ms（占 42%）。

---

## 6. 与既有探针/文档的关系

| 想知道 | 用什么 |
|---|---|
| **一次拿到结论** | `st perf <target>` |
| 逐绘制原语分解 + 最贵调用点 | `ST_PAINT_PROFILE=1 <app> --bench N` |
| 与历史基线对比 | `st perf --compare 旧.json,新.json` |
| 三维/视频渲染性能 | `docs/perf-benchmark.md`（裁云出片与 Blender 对比） |
| 绘制原语的深度诊断 | `docs/PAINT_DIAGNOSIS.md` |
| 内存与产物大小 | `docs/BASELINE_METRICS_20261014.md` |
| 构建/测试耗时 | `docs/BUILD_TEST_PERF.md` |

**一次性的 `tools/*_probe.cpp` 仍然保留**，但它们的存在理由是"验某个具体假设"，
不是"日常看一眼性能"。日常入口是 `st perf`。

# 基础指标基线（性能 / 产物大小 / 内存）— 2026-10-14 实测

> **契约：实测（冻结）**。本文是架构层基础指标的**实测快照**，被 `DESIGN.md` §11.1
> 与 `docs/BACKLOG.md` 引用。数字冻结在测量时刻——**要更新就追加新一轮，不要覆盖旧数字**。
>
> 测量环境（本轮）：
> - **Linux 容器**（8 核 / 8 GiB cgroup / 无 GPU / g++ 13.3 / glibc）
> - 系统字体：`NotoSansCJK-Regular.ttc` **19.5 MB**（CJK，映射改造的主对象）
> - 复测命令见文末「怎么重现」。
>
> ---
>
> ## 追加一轮（2026-10-16）：上屏路径实测（Linux 本机 GNOME/X11）
>
> 环境：Ubuntu / g++ 15.2 / 8 核 / GNOME 45 (Wayland 会话 + Xwayland :0) / 软件光栅器。
> 量尺：`tools/frame_breakdown.py`（按需帧）与 gallery 自带 `--bench N`（连续强制全量重绘）。
> 后者更能反映动画/拖动时的真实上屏压力（前者只画 5~7 帧就静止，socket 缓冲会吸收峰值）。
>
> | 场景（150 帧 p50） | 总帧 | 排版 | 绘制 | **送显** | 送显占比 |
> |---|---|---|---|---|---|
> | headless 1280×720 | 11.10 | 0.19 | 10.90 | 0.00 | — |
> | headless 1920×1080 | 17.72 | 0.19 | 17.52 | 0.00 | — |
> | x11 1280×720（改前） | 14.27 | 0.20 | 11.20 | **2.88** | 20% |
> | x11 1280×720（改后） | **12.40** | 0.19 | 11.23 | **0.96** | 8% |
> | x11 1920×1080（改前） | 24.82 | 0.21 | 17.87 | **6.71** | 27% |
> | x11 1920×1080（改后） | **20.28** | 0.20 | 17.94 | **2.14** | 11% |
>
> 交互场景（局部改动，非 `--bench` 全量）：present **0.19~0.22 ms**（送显 196×101）。
>
> 独立探针 `tools/x11_present_probe.cpp`（40 帧端到端中位，1920×1080）：
>
> | 上屏方式 | 1280×720 | 1920×1080 |
> |---|---|---|
> | `XPutImage` 整屏 | 17.5 ms/帧 | 18.2 ms/帧 |
> | `XShmPutImage` 整屏 | 1.46（12×） | 2.42（7.5×） |
> | `XShmPutImage` 每帧 `XSync` | 3.37 | 5.45 |
> | `XShmPutImage` 局部 400×300 | — | 0.85 |
> | 像素转换（标量 / SIMD） | 0.39 / 0.28 ms | 0.96 / 0.68 ms |
>
> **结论**：上屏在 X11 上是**与绘制同量级**的一大项（1080p 占 27%），而它在 headless 是 0；
> MIT-SHM 把这一段降到 1/3，“只送损坏区”再把它按面积缩小。措施与理由见 `DESIGN.md` §4.6.2。
>
> ---
>
> **未复测**：报告里的 Windows 数字（RTX 4080 SUPER / MinGW-w64）本会话无法复现，
> 表中标注来源；改动按跨平台实现，Windows 侧等效指标需在 Windows 机器上复测。

## 一、本轮改动前 → 改动后（同一环境，可复跑）

| 维度 | 指标 | 改动前 | 改动后 | 变化 |
|---|---|---|---|---|
| **内存** | gallery 无头软件路径 RSS 峰值 | **137.6 MiB** | **31.7 MiB** | **−77%** |
| | 其中：字体原始文件字节形态 | 15 个 face **整读进私有堆**，永不释放 | **15/15 走内存映射**，0 回退 | 文件页可回收 |
| | 线程数 | 18 | 18 | 无头本就不建 GPU 线程（Windows 侧差异见下） |
| | `ST_MEM_BUDGET=low` | — | 32.2 MiB（缓存上限 ×0.25） | 声明式收缩生效 |
| **产物** | `st`（dev 档 `-O1 -g1`） | **70.1 MB** | **4.59 MB**（+ `st.debug` 66 MB） | **−93%** |
| | `gallery`（dev 档） | 29.7 MB | **6.87 MB**（+ `gallery.debug` 22.9 MB） | −77% |
| | 符号可用性 | 产物内 `.debug_*` | `addr2line -f -C -e gallery.debug <addr>` 仍出**函数名 + 源码行号**；`.gnu_debuglink` 段已写入（gdb 自动认） | 不丢 |
| **性能** | 无头软件路径帧耗时 p50/p95 | （见 DESIGN §11） | 31.0 / 33.3 ms（gallery 全组件页，本容器 8 核无 GPU） | — |
| | 空闲 CPU（无头 10 s） | 0.6%（单核） | 0.01%（单核，Linux 实测） | 无回归 |
| | 性能门禁（`bench_*`，`--slow`） | **FAIL**（因为真实的圆角回归） | **全绿**（release 档；见下） | 修了回归 |

### 性能门禁：本轮修了一个被门禁抓着但没人看的真回归

`bench_rounded_card_fill` 的红**不是环境问题，是真回归**：`b420909`（2026-10-06 修“圆角发虚”）
在 `blend_coverage_runs` 里加回了“整行逐像素累加”，它**遮蔽了 `simd::blend_row` 的
full-span 快路径**（而 `b9166d5` 的渲染 5× 轮本已用“运行段 + merge_runs”取代了累加）。

| 圆角卡片（release，`tools/rounded_fill_probe.cpp`）| 比值 |
|---|---|
| `b420909` 之前 | **1.32×** |
| `b420909` 之后（回归）| **26.7×** |
| 本轮修复后 | **1.70~2.14×**（阈值 3×）|

修复保持“同一像素只混一次”的正确语义（斜带 45° 比 1.0017、环墨量 1.0000），
但累加后**按 alpha 字节分组**，同值极大段走 SIMD；并只在相邻 run 可能共享像素时才累加。
像素级 A/B：修复版 vs 旧版同一场景截图 **248 像素差**，而同一二进制自身两次截图的噪声是 **232**。

另：比值不跨优化档稳定（release 圆角 2.03× / dev 3.23×），已按既有 san 档口径收口——
非 `release` 档**如实跳过比值型断言并说明**。**跑性能门禁用 `st test --profile release --slow`。**

## 二、内存去向（Windows 实测 176 MB 的分解 —— 报告数据 + 本轮处置）

| 构成 | 估算 | 本轮处置 |
|---|---|---|
| 进程基线（MinGW CRT + libstdc++ 静态） | ~6.4 MB | —（不可压缩） |
| **字体文件整读驻留**（15 个 face） | **~86 MB（49%）** | ✅ 改为内存映射；`font_face_count` 改只读 512 B 头 |
| 帧缓冲（1536×960@DPI1.2） | ~5.9 MB | —（必需） |
| Tasks 池 8 线程栈 | ~8 MB | — |
| 字形 24 + 整形 16 + SVG 16 MiB | 56 MiB（预留上限） | ✅ 收敛到 `st::memory`；`low` 档 ×0.25 |
| GPU 驱动进程内常驻（仅 `auto`/`gpu`） | **+53 MB / +55 线程 / +4.6 GB VA** | ✅ 无头缺省 `software`（不再探测） |
| 其余（exe 映像、解析表、arena…） | ~30–40 MB | — |

**结构性结论**：问题不在「应用规模」而在「框架基座」——最小应用（3 个元素）与
gallery（20+ 页全组件）实测几乎同值（183 vs 184 MB，Windows）。

## 三、为何把「内存 < 20 MiB」改成分档口径

那份目标写于 v0.1 设计期，而实测最小应用 176 MB（超 8 倍）。分解后可见：

- **不可压缩项**：CRT 基线 6.4 MB + 帧缓冲 5.9 MB + 线程栈 8 MB ≈ 20 MB；
- **平台项**：Windows GPU 驱动进程内常驻 53 MB（只要用 D3D11 就有）；
- **框架自设项**：缓存预留 56 MiB。

也就是说「< 20 MiB」在**现代 Windows 进程**上本来就不可能达成，而那不是缺陷。
真正的缺陷是两处**可以拿掉的**大项（字体整读 86 MB、无头探测 GPU 53 MB），本轮已修。

新口径（`DESIGN.md` §11.1）：**无头软件路径 < 120 MiB**、**窗口 GPU 路径 < 250 MiB**，
其中「字体原始字节驻留 < 5 MiB」为硬指标（本轮已达成，Linux 侧 −77% 实测）。

## 四、怎么重现

```bash
# 内存/线程采样（Linux /proc；Windows 侧请用 ProcessStartInfo 读 WorkingSet64）
python3 tools/mem_sample.py --exe build/dev/bin/gallery --hold 5 -- \
    --headless --control-port 0 --frames 100000 --renderer software

# 内存预算低档
ST_MEM_BUDGET=low python3 tools/mem_sample.py --exe build/dev/bin/gallery --hold 4 -- \
    --headless --control-port 0 --frames 100000 --renderer software

# 产物分离前后
ls -l build/dev/bin/gallery build/dev/bin/gallery.debug
objdump -h build/dev/bin/gallery | grep -i debug      # 只剩 .gnu_debuglink
addr2line -f -C -e build/dev/bin/gallery.debug 0x1234 # 出函数名 + 行号

# 字体加载方式（控制通道可直接读）
# metrics 字段：font_faces_loaded / font_mapped_loads / font_read_fallbacks
```

## 五、本轮落地清单（细节见 `docs/BACKLOG.md` 待做章顶部）

| # | 改动 | 类型 |
|---|---|---|
| 1 | `separate_debug_info`（objcopy 三步）+ `--no-strip` + 自举脚本同步 + 崩溃器指向 `.debug` | 产物 |
| 2 | 无头缺省 `software` 渲染器（显式 `auto`/`gpu` 才探测） | 内存/启动 |
| 3 | `st::fs::MappedFile`（`platform_fs_map.cpp` 单点）+ `read_span`；`FontFace::load` 映射优先 | 内存/启动 |
| 4 | `load_face_cached` 去 `thread_local` + 计数器进程级 `atomic` | 内存 |
| 5 | `st::memory` 缓存预算单点 + `ST_MEM_BUDGET=low\|normal` | 内存 |
| 6 | 帧时序/日志环形缓冲 + `tick_declarative_hosts` 去拷贝 | 帧循环 |
| 7 | `DESIGN.md` §11.1（本文件为其实测依据） | 文档 |

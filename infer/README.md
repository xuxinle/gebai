# infer — GEBAI 本地推理引擎

在 **RTX 4080 SUPER 16GB + 32GB 内存** 的消费级机器上，把 **Qwen-AgentWorld-35B-A3B**（35B 总参 / 3B 激活的混合线性注意力 MoE）
跑到"日常可用甚至好用"的速度。两条不可妥协的原则：

1. **最强性能** —— 每个决策都以实测 tok/s 与显存/内存占用说话，不以"能跑起来"为满足。
2. **完全可控** —— 自持引擎 fork、自有量化配方、自有服务层；不依赖任何云、不依赖黑盒二进制、每项优化可复现可回退。

---

## 一、硬件基线（实测）

| 项 | 实测值 | 对推理的含义 |
|---|---|---|
| GPU | RTX 4080 SUPER **16GB**（Ada, sm_89） | 装不下 35B 任何 ≥IQ3 全量权重 → **必须异构分层** |
| 显存带宽 | ~736 GB/s | 非专家权重（~3B）轻松容纳，是解码速度的"快车道" |
| 内存 | **32GB DDR5-4800 双通道** | 有效带宽 ~50–60GB/s，成为异构解码的**主瓶颈** |
| CPU | i7-14700KF，20C/28T | 专家层 GEMM 有充裕并行度（P-core 8 + E-core 12） |
| 磁盘 | NVMe，余 1.38TB | 模型加载与 checkpoint 快照充裕 |

**结论**：瓶颈不在算力，而在**内存带宽**。解码时每 token 需从内存搬运被激活的 8 个专家权重
（40 层 × 8 专家 × 512 中间维 × 3 矩阵 ≈ 每 token 十几 MB 级），
所以性能工程的核心是：**把尽量多的专家层塞进 GPU，把 CPU 侧流量压到最小，并用投机解码摊薄每 token 的搬运量**。

---

## 二、模型特性（`Qwen/Qwen-AgentWorld-35B-A3B`，modelscope 官方 Qwen 空间）

```
architectures: ["Qwen3_5MoeForConditionalGeneration"]
model_type:    qwen3_5_moe        → llama.cpp 架构名 qwen35moe（b11100 起原生支持）
```

| 结构 | 取值 | 对部署的意义 |
|---|---|---|
| 层数 | 40（+1 层 MTP） | MTP 层可作投机解码头 |
| 注意力 | **混合**：每 4 层中 3 层线性注意力（Gated-DeltaNet）+ 1 层 full attention | **KV cache 只有 10/40 层** → 128K 上下文仅 ~1.3GB，长上下文几乎是免费的 |
| 专家 | **256 选 8**，moe_intermediate 512，另加共享专家 | 专家参数占 ~32B/35B → **异构分层的甜点区**（每 token 只读 8/256） |
| hidden / vocab | 2048 / 248320 | 非专家部分很小（~3B），可全驻 GPU |
| 上下文 | 262144 | 远超日常所需；压缩/量化 KV 的收益有限，不必激进 |
| 权重 | BF16 21 shard ≈ 68GB | 必须量化；用 unsloth UD 量化档（含 imatrix） |
| 附带 | 视觉塔（config 标 `language_model_only: true`） | 本阶段走纯文本，视觉留待后续 |

### 实测张量布局（IQ4_XS 档，由 `scripts/inspect-gguf.py` 直接从 GGUF 读出）

```
GGUF v3 | 733 张量 | 架构 qwen35moe | chat_template 8044 字符
总 16.55 GiB (17.77 GB)

  expert(ffn_down)     40 张量   5.529 GiB   ████████████████████ 33%
  expert(ffn_gate)     40 张量   4.322 GiB   ███████████████      26%
  expert(ffn_up)       40 张量   4.322 GiB   ███████████████      26%
  attn_linear         370 张量   1.285 GiB   ████                  8%
  token_embd            1 张量   0.503 GiB   ██                    3%
  output                2 张量   0.389 GiB   █                     2%
  shared_expert       160 张量   0.125 GiB                         1%
  ffn_other            40 张量   0.078 GiB                         0%

  专家合计 14.174 GiB（85.6%），非专家 2.38 GiB（14.4%）
  每层专家 362.9 MiB  →  -ncmoe 每 +1 ≈ 省 363 MiB 显存
```

**由此得到的显存规划（先验，待实测校准）**：

| 项 | MiB |
|---|---|
| 显存总量 | 16376 |
| 系统/桌面占用 | ~900 |
| KV cache（32K, q8_0, 仅 10 层 full attn） | ~320 |
| 计算缓冲 | ~1500 |
| **可用于权重** | **~13656** |
| 非专家（必须驻 GPU） | 2437 |
| 可容纳专家 | 11219 → **约 31 层** |
| **推荐 ncmoe 起点** | **9 ~ 10**（40 − 31） |

CPU 侧每 token 流量（ncmoe=10）：`10 层 × 8 专家 × 1.384 MiB ≈ 111 MiB`，
在有效内存带宽 ~45GB/s 下约 2.6ms → **理论上限约 380 tok/s**（实际受 CPU GEMM / 内存延迟制约，会显著更低）。

> **两个由布局推出的优化方向**（P3 验证）：
> 1. **张量粒度 offload**：`ffn_down` 用了更高位宽（0.540 vs 0.422 MiB/专家，unsloth 认为 down 更重要）。
>    只把 `ffn_down_exps` 放 CPU（`-ot` 而非 `-ncmoe`）可让 40 层 gate/up 全驻 GPU = 8.64 GiB，
>    但 CPU 流量升至 173 MiB/token —— 与 ncmoe=10 的 111 MiB 对比，**需实测择优**。
> 2. **自有量化可能省出空间**：把 down 压到与 gate/up 同位宽可省约 1.2GB → 多放 1–2 层专家进显存。

**为什么它是 16GB 卡的最佳拍档**：MoE 稀疏 + 线性注意力，使得"小显存 + 大内存"的组合不至于崩盘——
对比同规模的稠密 35B，激活参数量小一个数量级，且 KV 压力几乎消失。

---

## 三、技术路线

```
┌──────────────────────────── RTX 4080 SUPER 16GB ────────────────────────────┐
│  tok_embd / output / attn / DeltaNet / 共享专家 / 前 N 层之外的专家权重        │
│  KV cache（仅 10 层 full attention）                                         │
└──────────────────────────────────┬──────────────────────────────────────────┘
                                   │ PCIe 4.0 x16 (~25GB/s，单向实测)
┌──────────────────────────────────┴──────────────────────────────────────────┐
│  32GB DDR5：被 offload 的 MoE 专家权重（-ncmoe N / -ot 张量级指定）           │
│  每 token 只解引用被路由到的 8 个专家 → 稀疏访问，不像稠密那样等比搬运          │
└─────────────────────────────────────────────────────────────────────────────┘
```

**为什么选 llama.cpp 而不是 vLLM/SGLang**（已逐一核实，2026-09）：

| 引擎 | 结论 |
|---|---|
| **llama.cpp** | ✅ 原生 `qwen35moe`（PR #19435 已合并，含 Gated-DeltaNet 内核），且有 `-ot/--override-tensor`、`-cmoe/-ncmoe`、`--spec-type` 全套异构与投机开关 → **唯一能在 16GB 显存跑 35B 的成熟引擎** |
| vLLM | ✅ 已支持 `qwen3_5`，但权重必须全驻显存 → INT4 也要 ~20GB > 16GB，**结构性不可行** |
| SGLang | ❌ 无 qwen3_5 支持 |

### 调优维度（性能工程的主战场）

| 维度 | 手段 | 预期作用 |
|---|---|---|
| 显存规划 | `-ncmoe N`（前 N 层专家留 CPU）/ `-ot` 张量级覆盖（可细到 `ffn_down_exps` 单张量）/ `-ngl` | 结论已定：**全部专家驻显存**（`-ncmoe 0`）压倒性最优，offload 只在显存装不下时才用 |
| 投机解码 | `--spec-type`（`draft-mtp` / `ngram-mod` / `ngram-cache` 等） | **已实测排除**：自由文本缺可预测重复模式，命中率过低反低于无投机基线——不启用 |
| KV 精度 | `-ctk q8_0 / -ctv q8_0` | KV 本就小，收益有限但释放的显存可换更多专家层上 GPU |
| 计算图 | `-fa on`、`-ub/-b` 批大小、`--no-op-offload` | 提升 prompt processing 与批吞吐 |
| CPU 侧 | 线程数/亲和性 | 仅在启用 offload 时才有意义（全 GPU 配置下不影响） |
| 量化 | 换档位；自有 imatrix 重配比（非专家高精度 + 专家低精度） | **最高优先级旋钮**：能否全量驻显存由它决定，直接决定 3 倍速差 |

---

## 四、目录结构

```
infer/
  README.md              本文件：设计、实测数据、复现步骤
  config/
    hardware.json        本机硬件基线（bootstrap 自检写入）
    profiles.json        运行档位（模型 / 显存规划 / 上下文 / 并行度 / 投机）
    assets.manifest.json 非入库资产清单（大小 + sha256 + 多来源；restore.ps1 读取）
    model-layout-*.json  GGUF 结构与张量布局导出（inspect-gguf 产出，显存规划依据）
  scripts/
    restore.ps1          按清单校验/补齐非入库资产（-List / -Check / -All / -Only / -Proxy）
    bootstrap.ps1        环境自检（GPU/CPU/内存/磁盘/引擎/模型/构建工具链）
    fetch.ps1            单线程健壮下载（重试 + 续传 + 大小校验）
    fetch-parallel.ps1   前台并发分片下载（大文件用）
    build-engine.ps1     自建引擎（cpu/cuda/vulkan，含工具链兼容性门禁）
    run-server.ps1       启动 OpenAI 兼容服务（按档位，启动记录留档）
    bench.ps1            基准测试（吞吐 + 报告落盘）
    bench-concurrent.ps1 并发吞吐测试（多 slot 聚合）
    plan-memory.ps1      显存规划扫描（ncmoe 求 Pareto 点）
    smoke-test.ps1       端到端冒烟（引擎→加载→生成→API，小模型秒级回归）
    verify.ps1           质量基线回归（固定种子/提示词，配置间输出对比）
    inspect-gguf.py      GGUF 结构与张量布局解析（支持未下载完成的文件）
    fetch-hf-parallel.py 大仓库并发分片下载（断点续传 + 预算分批；`--only` 选单文件）
    run-convert.py       HF→GGUF 转换入口（自动挂 gguf-py 与源码树到 PYTHONPATH）
    prepare-convert-mirror.py  分片命名不规范的 HF 仓库 → 转换器友好镜像（硬链接 + 重写索引）
  bench/reports/         基准报告与启动记录（argv 全量留档，可审计可回放）
  quant/                 自有量化配方（imatrix 重配比）
  engine/                引擎 fork、构建说明与补丁
  vendor/                引擎二进制（CUDA / Vulkan / CPU 多形态并存，便于 A/B 与回退）
```

模型权重落 `{GEBAI_HOME}/resources/models/infer/`（与仓库资源子仓库约定一致）。

运维入口是歌白的 `local_infer` 子Agent（`local_infer_status` / `local_infer_models` / `local_infer_start` / `local_infer_stop` / `local_infer_bench` / `local_infer_inspect`），
它复用本目录脚本而不复制逻辑；服务是标准 OpenAI 兼容端点，可直接接成 `GEBAI_LLM_ROUTES` 中的一路本地算力
（配置示例见根目录 `.env.example` 的 `LOCAL_INFER_*` 区块）。

---

## 五、快速开始

### 5.1 在另一台机器上从零还原（先做这一步）

模型权重（43.5 GB）、引擎二进制（~8 GB）、llama.cpp 源码与构建工具链体积大且可从上游重新获取，
**不入 git**。清单与还原脚本把它们变成一条命令：

```powershell
# 查看清单（不联网、不落盘）
pwsh -File infer/scripts/restore.ps1 -List

# 只校验现状（只读；-Quick 仅比大小更快，全量 sha256 校验读 45.8 GB）
pwsh -File infer/scripts/restore.ps1 -Check

# 校验并补齐**必需项**（引擎 + fast 档模型 ≈ 13.0 GB）
pwsh -File infer/scripts/restore.ps1 -Proxy http://<proxy-host>:<port>

# 连同可选资产（其余量化档 / WSL 对照构建 / 工具链，总计 45.8 GB）
pwsh -File infer/scripts/restore.ps1 -All

# 只要某一项（**显式点名即下载，不看 required**）
pwsh -File infer/scripts/restore.ps1 -Only model-iq4xs
```

清单在 `config/assets.manifest.json`（与主仓库 `scripts/resources.manifest.json` 同约定：
`path` / `size` / `sha256` / `required` / `license` / `description` / `sources[]`）。

| id | 大小 | 必需 | 说明 |
|---|---|---|---|
| `engine-cuda-win` | 0.24 GB | ✅ | llama.cpp b11100 Windows CUDA（默认后端） |
| `model-iq3xxs` | 12.80 GB | ✅ | IQ3_XXS —— fast/concurrent/long-context/throughput 档默认模型 |
| `model-q2kxl` | 11.41 GB | — | **Q2_K_XL —— concurrent-max 档（GEBAI 多会话）默认模型**，支撑 np=4（见 6.9.1） |
| `model-iq2xxs` | 10.71 GB | — | IQ2_XXS —— 显存更紧时的回退（价值只在腾显存换并发） |
| `model-iq3s` | 13.96 GB | — | IQ3_S —— balanced 档（质量更优） |
| `model-iq4xs` | 16.56 GB | — | IQ4_XS —— quality 档 |
| `model-qwen36-ud-iq3xxs` | 13.10 GB | — | **Qwen3.6-35B-A3B（unsloth UD-IQ3_XXS）—— `qwen36-a3b` 档**（见 6.14） |
| `imatrix-unsloth` | 0.18 GB | — | 重要性矩阵（自有量化配方用） |
| `engine-vulkan-win` / `engine-cpu-win` | 0.03 / 0.02 GB | — | 回退与对照后端 |
| `engine-cuda-wsl` / `engine-cudart-wsl` | 0.16 / 0.55 GB | — | WSL2 跨平台对照（解压在 WSL 侧，见 `scripts/wsl-setup-linux.sh`） |
| `src-llamacpp` | 0.04 GB | — | llama.cpp 源码（自建 CUDA 引擎用） |
| `toolchain-clang` / `toolchain-llvm-exe` / `toolchain-vsbuildtools` | 0.9 / 0.37 / 0.004 GB | — | 构建工具链（仅重编引擎时需要） |

**设计要点**：

- **只下载必需项即可开工**：13 GB 而不是 45.8 GB；其余按需 `-All` 或 `-Only` 拉取。
- **每个文件多来源顺序尝试**，下载后逐一校验 size + sha256（大文件走 `fetch-parallel.ps1` 前台并发分片；
  本机实测后台任务会被系统挂起，故不丢后台）。
- **`-Proxy` 支持代理**：本机环境 GitHub 直连间歇可达，脚本内已带重试与续传。
- 退出码：`0` 全部就绪 / `1` 有失败 / `2` 校验不通过（适合接入 CI 或开机自检）。

### 5.2 日常使用

```powershell
# 1) 环境自检（GPU/CPU/内存/磁盘/引擎/模型/工具链）
pwsh -File infer/scripts/bootstrap.ps1

# 2) 启动服务（默认档位）
pwsh -File infer/scripts/run-server.ps1

# 3) 基准测试
pwsh -File infer/scripts/bench.ps1 -Contexts 4096,32768 -Runs 3

# 4) 显存规划搜索（找出本机最优 ncmoe）
pwsh -File infer/scripts/plan-memory.ps1
```

### 5.3 跨平台与跨设备（引擎矩阵）

推理**不绑定 Windows、不绑定 NVIDIA**。引擎清单在 `config/engines.json`（矩阵：linux/win32/darwin × cpu/cuda/vulkan/metal/rocm/sycl/openvino，资产名逐条核对自 llama.cpp 发行版），按本机 `platform+arch` 过滤、按实测设备探测（`nvidia-smi` / `/dev/nvidia*` / `vulkaninfo` / `/dev/dri` / darwin 判 Metal）挑后端：

```powershell
# 在 GEBAI 里（local_infer 子Agent）
local_infer_engines(action="detect")          # CPU 核数 / CUDA / Vulkan / Metal 可用性 + 探测依据 + 推荐引擎
local_infer_engines(action="list")             # 23 条矩阵 × 本机平台 × 已安装状态
local_infer_engine_fetch(id="linux-cpu-x64")   # 下载安装（断点续传 + 校验 + 解压 + 递归定位 llama-server + 写安装标记）
local_infer_model_fetch(preset="qwen2.5-0.5b") # 小模型（CPU 档位可用；已有则幂等跳过）
local_infer_start(profile="cpu-small", port=19080)   # 无 GPU 也能起
```

安装落点 `vendor/<engine-id>/`（标记文件 `.engine.json` 记 tag/资产/exe；CUDA 引擎的 cudart 运行时一并装到同目录），下载缓存在 `vendor/.cache/`。归档内部布局各平台不同（Linux 为 `<tag>/llama-server`、Windows 在 `build/bin` 下），因此**递归定位可执行文件**而不是写死路径。

无 GPU 机器上实测（Linux x86_64 / 8 核 / Qwen2.5-0.5B-Q4_K_M）：单流解码 **15.7–28 t/s**（短 prompt 更快），`json_schema` 约束解码与结构化校验全通。注意 **CPU 上并发无增益**（4 路各约 2.5 t/s、聚合持平——CPU 算力被 slot 平分），与 GPU 场景结论相反。

### 5.4 内网/离线：发现源码自动编译

**预编译包需要外网**（GitHub 发行版）。内网/air-gapped 机器改为**随包带入源码，就地编译**：

```powershell
# ① 联网机器：把源码归档拉进资源目录（清单已登记该条目，size/sha256 校验）
bun run resources:download --only="src/**"     # → resources/src/llama.cpp-b11175.tar.gz（37.6MB，自带 vendor/ 依赖）
# ② 内网机器：发现 → 看工具链 → 编译（产物与下载安装的引擎同一落点）
local_infer_sources(action="list")                              # 列出候选：含归档预览、CMakeLists 与 vendor/ 依赖的有无
local_infer_sources(action="toolchain")                          # 工具链（cmake/ninja/gcc/nvcc/hipcc…）与各设备就绪 + 内网补齐办法
local_infer_source_build(source="llama.cpp-b11175", device="cpu", offline=true)   # 真实编译安装（长编译加 background=true）
local_infer_start(engine="linux-cpu-src", profile="cpu-small")  # 用自编译引擎起服务
```

- **发现根**（按优先级）：`LOCAL_INFER_SOURCE_DIRS`（环境变量，路径分隔符分隔）→ `{GEBAI_HOME}/resources/src/` → `{GEBAI_HOME}/resources/engines/` → `<infer>/engine/`。目录与归档（`.tar.gz`/`.zip`）都支持；归档**只预览不解压**（条目数/顶层/是否含 `CMakeLists.txt`/是否自带 `vendor/`），解压延迟到编译时（落 `vendor/.cache/src/<id>/`，幂等可复用）。
- **离线可行性判定**：llama.cpp 的 CMake 在缺 `vendor/` 依赖时会尝试联网取（内网表现为长时间挂起）——因此发现层会**全量过滤**归档条目给出「是否自带 vendor/」的结论（按采样判定会漏——实测该归档的 vendor 在 3999 条里的最后几十条），并在缺失时明确提示换用完整源码包。`offline=false` 才允许 FetchContent 联网。
- **编译安装**：cmake 预设按后端只开目标开关（cpu/cuda/vulkan/metal/rocm）+ `-DLLAMA_BUILD_SERVER=ON -DLLAMA_CURL=OFF`（去 libcurl 硬依赖）；`Ninja` 优先、否则 `Unix Makefiles`/`MinGW Makefiles`；产物复制到 `vendor/<engine-id>/bin/`（**解引用符号链接**，安装目录自包含——`build/bin/` 里是 `libggml-base.so → .so.0 → .so.0.25.3` 这类链，漏拷就起不来），并写同一格式的 `.engine.json`（额外带 `build.{from_source,source_dir,build_dir,device,offline,log,commit}`）。
- **本机实测**（Linux x86_64 / 8 核 / 无 GPU / Qwen2.5-0.5B-Q4_K_M）：`resources/src/llama.cpp-b11175.tar.gz` → 零外网 cmake 配置 **1.5 s** → 全量编译 **308 s**（Ninja / 8 并行，271 个目标，exit 0）→ `vendor/linux-cpu-src/bin/llama-server` 可跑，同模型同参数解码 **25.8–32.7 t/s**（3 轮）；**预编译包对照组 28.2–30.4 t/s**——两者同档，即**自建不损失性能**（本机编译带 `-march=native`，在别的机器/量化上可能更明显，以本机实测为准）。
- **编译任务可管**：每次编译落 `run/builds/<build_id>.json`（步骤/阶段/日志/属主 PID，与批量同款存活判定）与 `bench/reports/build-<id>-<stamp>.log`；`background=true` 后台编译（CPU 数分钟、CUDA 等十几分钟以上），`status`/`log`/`list`/`cancel` 随时管；取消在**阶段之间**生效（不强杀 cmake，避免留下半成品构建目录）。`engines(action="list")` 会把这类**不在矩阵里的已装引擎**（源码编译产物）单独列一段，可直接被 `start` 的 `engine` 参数引用。
- **工具链路径也认非 PATH 位置**：探测结果同时给出每个工具的**绝对路径**（`ToolchainInfo.paths`），便携工具链（如放进 `resources/toolchain/bin`）不必先改 PATH 就能被编译层直接调用。

- **一条命令冒烟**（脚本直接调用上面这些工具，因此验证的就是模型走的那条路径；零重复实现）：

```bash
# Linux / macOS（POSIX）
bash infer/scripts/smoke-source-build.sh                       # 发现 → 工具链 → 编译 → 起服务 → 结构化推理 → 批量 → 停止
bash infer/scripts/smoke-source-build.sh --skip-build          # 只验下游链路（用已安装引擎，几十秒）
bash infer/scripts/smoke-source-build.sh --device cuda --items 4 --keep
# Windows
pwsh -File infer/scripts/smoke-source-build.ps1 -SkipBuild     # 同一实现的包装（参数透传）
```

  共 8 步、每步做**真实断言**（不是只看输出）：候选存在 / 后端就绪 / 产物 exe 落盘 / 引擎在清单可见 / `/health`+`/props` 直连复验 n_ctx / 结构化 JSON 可解析且满足必填字段 / `results.jsonl` 行数与成功数一致 / 停服务后端口真释放。退出码 0/1 可接 CI；`--engine-id` 同 id 重跑=幂等跳过编译（首次验编译，之后秒验下游）。

### 5.5 服务进程管理（状态文件 / 日志 / 跨平台启动）

`run-server.ps1 -Background` 启动成功后会写两份记录（旧版脚本只写前者，子Agent 会按端口反查 PID 补写后者）：
| 文件 | 内容 | 用途 |
|---|---|---|
| `bench/reports/launch-<时间戳>.json` | 本次启动的完整 argv、模型字节数、显存规划 | 审计与回放 |
| `run/server-<端口>.json` | PID、端口、档位、模型、日志路径、argv | **进程管理的唯一事实来源**（status/stop/restart/logs 据此精确定位实例） |

```powershell
# 看实例与 PID 存活（服务崩溃后状态文件会显示「已退出」并附日志疑似错误）
pwsh -File infer/scripts/run-server.ps1 -Profile fast -Background -NoWait
# 换档位/换模型 = 重启（llama-server 不支持热切换）：停掉该端口实例后再启
```

日志落 `bench/reports/server-<时间戳>.log`（stdout）与 `.log.err`（stderr）。`local_infer_logs` 工具从尾部按块回退读取（大日志不整文件入内存）并给出失败特征摘要；**不做流式跟随**（跟随会挂住会话）。

**启动路径跨平台**（不再依赖 PowerShell）：子Agent 缺省走 TS 层的 launcher —— `detached` 拉起（脱离调用方进程树，否则工具会挂在长驻子进程上）+ stdout/stderr 重定向到日志文件 + `/health` 就绪轮询 + 按 PID/端口精确终止；平台命令构造均为「显式平台参数的纯函数」（win32/POSIX 双轨可测）。Windows 上无已装引擎且未显式要求 launcher 时自动回退 `run-server.ps1`，也可用 `LOCAL_INFER_LAUNCH=script` 强制。启动记录 `bench/reports/launch-<stamp>.json` 记完整 argv（含引擎 id、启动方式、可执行文件路径），可审计可回放。

在 GEBAI 里这一切由 `local_infer` 子Agent 承担（工具复用上述脚本与状态文件，不复制逻辑）：

| 工具 | 作用 | 审批 |
|---|---|---|
| `local_infer_engines` | 引擎矩阵 × 本机平台 × 已安装状态；设备探测（CPU/CUDA/Vulkan/Metal + 依据 + 推荐） | 免 |
| `local_infer_engine_fetch` / `local_infer_model_fetch` | 下载安装引擎 / 下载模型（断点续传 + 校验，幂等） | 需 |
| `local_infer_status` | 实例列表（PID + 存活校验）/进程/端口与占用者/`/health`+`/props`（n_ctx、**slot 数=并发上限**）/显存/引擎/最近启动 | 免 |
| `local_infer_start` / `local_infer_stop` / `local_infer_restart` | 启停与重启（`engine` 选引擎、`mode` 选启动方式）；stop/restart 支持按 PID、端口、档位精确终止进程树并清理状态文件 | 需 |
| `local_infer_logs` | 日志尾读 + 关键字过滤 + 错误特征摘要 + 文件清单 | 免 |
| `local_infer_generate` | 单条推理（含结构化输出：JSON Schema 约束 + 工具侧校验与回灌重试） | 免 |
| `local_infer_batch` | 批量提交与断点续跑（`background=true` 随服务进程后台执行、立即返回 job_id） | 免 |
| `local_infer_jobs` | 批次 list/status/results/cancel | 免 |
| `local_infer_bench` / `local_infer_inspect` / `local_infer_models` | 基准 / GGUF 结构 / 档位清单 | bench 需 |

### 5.6 批量提交推理任务（+ 结构化输出 + 统一推理目标）

单条与批量都走标准 `/v1/chat/completions`，批量**不引入任何第三方依赖**（自实现 worker pool：限流、退避重试、逐条落盘），产物落 `bench/runs/<job_id>/`：

| 文件 | 内容 |
|---|---|
| `items.jsonl` | 输入副本（含 id 与 meta；给定同 `job_id` 即续跑该任务） |
| `results.jsonl` | **逐条 append** 的结果（可实时 tail；进度以此为准，不信状态文件快照） |
| `job.json` | 任务快照（状态/并发/总条数/已处理/失败/产物路径；后台批次另记 `background`/`owner_pid` 与终态 `summary`） |

```
# 在 GEBAI 里（local_infer 子Agent）
local_infer_generate(prompt="把这句话翻成英文：…", schema={...})      # 单条 + 结构化
local_infer_batch(items=[{"id":"q1","prompt":"…"}, …], schema={...})  # 批量（同步）
local_infer_batch(items_file="bench/runs/questions.jsonl", job_id="kb-2026-09", background=true)  # 后台跑
local_infer_batch(job_id="kb-2026-09", resume=true)                   # 同 id 续跑剩余条目
local_infer_jobs(action="status", job_id="kb-2026-09")                 # 进度（+ 存活判定）
local_infer_jobs(action="results", job_id="kb-2026-09", only="failed")# 只看失败条目
local_infer_jobs(action="cancel", job_id="kb-2026-09")                 # 真中止在跑的后台批次
```

**并发怎么定**：`concurrency` 缺省 1（串行）；给定值会与档位 `parallel` 比对并夹取。依据是实测结论——**并发只在「slot 数匹配的短 prompt」且瓶颈在 GPU 时有效**（np=16 短 prompt 聚合 627.8 t/s = 4.30×）；长 prompt（如 GEBAI 会话的 15K 系统提示 + 工具定义）下预填充占主导、并发无增益（见 6.9/6.10）；**纯 CPU 上并发同样无增益**（0.5B 实测：单流 15.7 t/s，4 路各约 2.5 t/s、聚合持平——CPU 算力被 slot 平分）。所以：短 prompt 大批量用 `throughput` 档（np=16），长 prompt 用 `concurrent` 档（np=2/4），CPU 保持 1；并发上限就取 `local_infer_status` 报出的 slot 数。

**推理目标（不限于本机）**：`generate`/`batch` 的 `target` 参数选目标——缺省 `local`（本机受管实例，端口取状态文件 > `LOCAL_INFER_PORT` > 8080）；也可直连 `http://…` 或用在 `LOCAL_INFER_TARGETS` 里声明的命名目标（含 `api_key`）。`local_infer_targets(probe=true)` 列出全部目标并探活（n_ctx/slot/模型名）。密钥在输出与 `job.json` 里一律只出现掩码，产物只记目标名与不含密钥的 base_url。

**结构化输出**：`schema`（JSON Schema）交给服务端转 GBNF 约束解码，工具侧再做「抽取 JSON + 轻量 Schema 校验（type/required/enum/嵌套）」，校验不过**把错误回灌给模型重试一次**（`retry_on_invalid=false` 可关）；`grammar` 为 GBNF 直传兜底（注意与 `-bs` 后端采样不兼容，引擎会自动回退 CPU 采样）；`json_object` 只要求合法 JSON，是最弱的降级手段（服务端不认识约束时工具会自动降级为它并给出预警）。结果同时给到 `content`/`reasoning_content`、`usage`、服务端 timings 换算的实测 t/s。

**同步还是后台**：`background` 缺省 **false**（同步执行——本次工具调用等批次跑完，条目多时会长时间占住本轮，可用 `max_items` 分片推进）。**条目多时用 `background=true`**：前置处理（解析条目/断点跳过/分片/并发夹取）完成后立即返回 job_id，批次在服务进程内继续跑、逐条 append 到 `results.jsonl`，随后用 `local_infer_jobs(action="status")` 查进度、`action="results"` 取结果。

- 会话中断或工具调用结束**不会**杀掉后台批次（批次的信号刻意不绑会话取消）；真中止用 `local_infer_jobs(action="cancel")`——对在本服务进程跑的后台批次写取消标记并 `abort()`，在飞条目跑完后停止。
- **服务重启**会让在跑的后台批次变成「已中断」：`jobs(action="status")` 按 `owner_pid` 探活并幂等改判状态、给出续跑指引；**不会自动续跑**（避免重启后无人值守地占满 GPU）。用 `local_infer_batch(job_id=…, resume=true)` 接续。
- 同一 `job_id` 已有在跑批次时重复提交会被拒绝（不会改写正在跑任务的输入副本）。
- 落盘语义：被取消时**尚未开始**的条目只作占位、**不写入** `results.jsonl`（产物只含真实产出），resume 时自然重跑；被中止的在飞条目如实记为一次失败，resume 会重做。所以 `results.jsonl` 的行数在「取消过」的任务里可能少于条目数，这是预期行为。
- 批量工具类任务建议 `enable_thinking=false`（实测省 91% token 且工具调用结果一致）；条目多时可用 `LOCAL_INFER_BATCH_MAX_ITEMS` 设一个默认分片上限。

---

## 六、实测数据（全部为本机真实测量）

**测试环境**：llama.cpp b11100 官方预编译｜模型 `Qwen-AgentWorld-35B-A3B`（unsloth UD 量化，sha256 已校验）
｜RTX 4080 SUPER 16GB + i7-14700KF + 32GB DDR5｜KV `q8_0`｜`-fa on`｜`-t 14`｜上下文 32768

### 6.1 后端对比——**决定性的 2.6 倍**

同一模型（IQ3_XXS）、同一参数（ncmoe=0，全部专家在显存）下：

| 后端 | pp512 t/s | tg256 t/s | GPU 利用率 | SM 时钟 | 功耗 |
|---|---|---|---|---|---|
| Vulkan | 2 696 | 54.4 | 93% | 2745 MHz | 117 W |
| **CUDA** | **4 391** | **143.4** | 88% | — | 202 W |
| 提升 | **1.6×** | **2.6×** | — | — | — |

> **结论**：本机场景（256 专家 / 40 层 → 每 token 320 次小 GEMM）下，CUDA 的 MoE 内核效率
> 远高于 Vulkan。**性能工程的首要决策是选 CUDA**，而不是调参。Vulkan 作为无 CUDA Toolkit 时的回退。

### 6.2 完整配置矩阵（CUDA）

| 配置 | 量化 | 专家驻 GPU | pp512 | tg256 | 显存 |
|---|---|---|---|---|---|
| **IQ3_XXS 全 GPU** | 3.06 bpw | 40/40 | **4 391** | **143.4** | 13.2 GB |
| IQ3_XXS ncmoe=6 | 3.06 bpw | 34/40 | 1 416 | 96.8 | ~11.0 GB |
| IQ4_XS ncmoe=8 | 4.25 bpw | 32/40 | 943 | 82.3 | 14.8 GB |
| IQ4_XS ncmoe=10 | 4.25 bpw | 30/40 | 677 | 74.8 | ~14.0 GB |
| IQ4_XS ncmoe=14 | 4.25 bpw | 26/40 | 493 | 63.1 | ~12.6 GB |

**核心规律**：
1. **全 GPU（ncmoe=0）压倒性最优**。CUDA 下把 6 层专家丢给 CPU 就损失 32%（143→97），
   远比 Vulkan 场景严重——因为 GPU 太快，CPU 一点点拖后腿就成瓶颈。
2. 策略因此明确：**选能把全部专家塞进 16GB 显存的最小量化**，而非“高量化+部分 offload”。
3. 每层专家 363 MiB（IQ4_XS）/ 320 MiB（IQ3_XXS）——`-ncmoe` 每 +1 就是确定的显存开销。

### 6.3 服务级实景（`llama-server` + OpenAI 兼容端点）

```
模型加载：12 秒（IQ3_XXS 全 GPU）

prompt eval： 37 tokens /  464 ms
     eval：400 tokens / 3139 ms  →  127.1 t/s
    total：        3.6 s / 437 tokens
---
1+1等于几？  →  content="1+1等于2。"
                reasoning_content="Thinking Process: 1. Identify the core question..."
                predicted_per_second = 126.3 t/s
```

**这是推理型模型**（带思维链）：正文在 `content`，思考过程在 `reasoning_content`。
消费类 API 必须同时处理两个字段，否则只拿到半截输出（调 180 token 时正文可能被思维链挤空）。

> **实用提醒**：该模型的思维链相当冗长（实测一个简单问题耗掉 950+ token，其中正文仅 1 句）。
> 因此 `max_tokens` 必须给足（建议 ≥1500，否则正文会被思维链挤空——表现为"有 usage 但 content 为空"）；
> 需要低延迟时应按需关闭思考（chat template 支持 `--no-reasoning-preserve` 相关开关）。

### 6.4 无效优化（务实的负结果）

| 手段 | 结果 | 结论 |
|---|---|---|
| **MTP 投机解码**（`--spec-type draft-mtp`） | 无法使用 | 该 GGUF **未包含 MTP 张量**（已用 `inspect-gguf.py` 验证） |
| **ngram-mod 投机** | 29.7 t/s（基准 40.3） | ❌ 反而变慢 |
| **ngram-cache 投机** | 37.7 t/s（基准 40.3） | ❌ 无收益（draft 16 个仅接受 7 个） |

**原因**：自由文本生成缺少可预测的重复模式，n-gram 投机命中率过低，抵消不了草稿开销。
→ **投机解码在本场景关闭**（已在 `profiles.json` 中全部置 null）。

### 6.5 量化档位对比与显存余量（决定档位制的依据）

三档均为实测（llama-bench 256-token / 服务模式 32K 上下文）：

| 档位 | 位宽 | 大小 | 专家位置 | bench 解码 | 服务解码 | 服务显存 | 余量 |
|---|---|---|---|---|---|---|---|
| `fast`（默认） | 3.06 bpw | 13.7 GB | 全 GPU | **143.6** | ~127 | 13.2 GB | 舒适（~3 GB） |
| `balanced` | 3.96 bpw | 15.0 GB | 全 GPU | 137.5 | 117.8 | **15.8 GB** | ⚠️ 仅 ~565 MB |
| `quality` | 4.25 bpw | 16.6 GB | 8 层在 CPU | 82.3 | — | 14.8 GB | 舒适 |

**两个关键权衡**：

1. **`balanced` 的价值与风险并存**：IQ3_S 的 `ffn_down` 与 IQ4_XS 同款位宽（unsloth 的质量倾斜：
   down 矩阵给高精度，gate/up 压得更狠），因此**质量高于 IQ3_XXS 而只需全 GPU 驻留**——
   同全 GPU 配置下，它把「质量档」从 82 t/s 抬到 137.5 t/s（+68%）。
   代价是显存余量只剩 565 MB：**长上下文或并发场景应改用 `fast`**（或降低 ctx），否则会 OOM。
2. **`quality` 档是唯一必须 offload 的档位**——IQ4_XS 的 16.6 GB 放不进 16 GB 显存，
   8 层专家落 CPU 后解码腰斩（143.6 → 82.3）。仅在确需最高保真度且能接受一半速度时使用。

> 由布局数据可解释为何 `balanced` 只慢 4%：三档的 `gate`/`up`/`attn_linear` 张量位宽相同
> （3.230 / 1.002 GiB），差异全部集中在 `ffn_down`（4.373 → 5.529 GiB，+1.16 GiB）与专家总字节。
> 解码是访存受限，权重总量只增 8.5%，速度自然只降 4%。

### 6.6 参数扫描：已验证无剩余空间（负结果）

在 `fast` 档（全 GPU）上扫描常见调优旋钮，每项 `-p 512 -n 256 -r 2`：

| 配置 | pp512 | tg256 |
|---|---|---|
| 基线 `t=14 b=2048 ub=512` | 4 618 | 143.0 |
| `t=20` / `t=10` | 4 639 / 4 683 | 143.7 / 142.6 |
| `ub=1024` / `b=4096 ub=1024` / `b=8192 ub=2048` | 4 500 / 4 744 / 4 480 | 142.1 / 143.2 / 143.8 |
| `fa off` | 4 537 | 143.3 |
| KV `f16`（替代 `q8_0`） | 4 710 | 143.6 |

**全部落在 142.1–143.8 t/s 的噪声区间内，无任何实质差异。** 结论：

- 线程数不影响解码——全 GPU 时 CPU 只做调度，不是瓶颈；
- batch/ubatch 只影响预填充（且本场景预填充已远超需求），对解码无用；
- FA 开关与 KV 精度对速度无影响（KV 本就只占 10/40 层）。

**唯一有效的调用参数是 `-bs`（后端采样）**：129.1 → 136.4 t/s（**+5.7%**），
temperature=0 下**输出逐字节一致**（已合入全部非对照档位；验证脚本 `scripts/verify-bs.ps1`）。
它消除的是每步的 logits 回传（248320×4 B）。注意与语法约束（grammar）不兼容——
带 grammar 的请求引擎会自动回退到 CPU 采样，不影响正确性。

→ **除 `-bs` 外参数层已榨干**：140 t/s 量级是所选后端的稳态上限，进一步提升必须改引擎代码（见下节）。

### 6.7 最终推荐配置

```jsonc
// profiles.json → "fast"（默认）
engine : vendor/llama-b11100-win-cuda12.4   // 已自包含（cudart+cublas+cublasLt 已入目录）
model  : Qwen-AgentWorld-35B-A3B-UD-IQ3_XXS.gguf
-ncmoe 0  -ngl 99  -fa on  -ctk q8_0 -ctv q8_0  -c 32768  -t 14  --jinja  -bs
→  解码 143 t/s（+bs 后服务实测 134.7）｜ 预填充 4 391 t/s ｜ 显存 13.2 GB
// 多子会话并行：concurrent 档（np=2, ctx 65536）—— GEBAI prompt 实测 15K token，每 slot 需 ≥24K
// 短 prompt 批量：throughput 档（np=16）→ 聚合 627.8 t/s（4.30×）
```

**从初始的 37 t/s 到 143 t/s（3.9× 提升）**，路径是：换后端（Vulkan→CUDA）+ 换量化（让全部专家进显存），
两者都不是“调参”，而是结构性决策。

### 6.8 相对硬件上限的位置

| 项 | 数值 | 含义 |
|---|---|---|
| 显存带宽 | ~736 GB/s | 硬件上限 |
| 每 token 需读权重 | 12.79 GiB / 8 专家激活 ≈ 0.42 GiB | 25% 专家占比 |
| **纯带宽理论上限** | **≈ 1750 t/s** | 736 ÷ 0.42 |
| **实测** | **143 t/s（8.2% 利用率）** | 剩余空间在前端调度 / 内核效率 |

→ 账面还有 12 倍空间，但**这不等于“努力就能拿到”**：参数层已验证无空间（见 6.6），
剩余差距只可能来自引擎内部结构——当前受限于**每 token 上千个微小内核（1 425 个，均 3.87 µs）的串行执行**
（填不满 SM，占用率仅 18–38%），而非带宽，也**不是内核启动**（剖析实测内核启动数 0.00/token）。
要触及它必须改引擎代码，风险与成本都显著上台阶（见「引擎改造的启动条件」）。

> **一个反直觉的实测佐证**：`balanced` 档权重比 `fast` 多 8.5%，解码只慢 4%。
> 若解码纯受带宽支配，应当同比例变慢。这说明**每步固定开销（内核碎片化 / 图调度）占了相当比重**——
> 这既是带宽利用率只有 8% 的原因，也是唯一值得改代码的方向。

### 6.9 长上下文 / 并发 / CPU 对照

**长上下文（KV q8_0，只有 10/40 层是 full attention）**：

| 上下文 | 显存 | 解码 | 结论 |
|---|---|---|---|
| 32 768 | 15 052 MiB | 95.7 t/s | 基准 |
| 65 536 | 15 523 MiB | 93.8 t/s | 可用 |
| **131 072（128K）** | **15 869 MiB** | **93.3 t/s** | ✅ **实测硬上限**（速度几乎不衰减） |
| 163 840（160K） | 16 021 MiB | 22.2 t/s | ❌ 性能断崖 |
| 196 608（192K） | 15 956 MiB | 38.5 t/s | ❌ |
| 229 376（224K） | 15 994 MiB | 28.0 t/s | ❌ |
| 262 144（256K，模型训练上限） | 16 027 MiB | 20.9 t/s | ❌ |

**两个关键结论**：

1. **128K 以内，长上下文几乎免费**——从 32K 到 128K，显存仅增 817 MiB（KV 只占 10/40 层），
   解码速度从 95.7 掉到 93.3 t/s（-2.5%）。这是混合线性注意力（3 层 DeltaNet + 1 层 full attention）的直接红利。
2. **128K 是硬边界，且原因不是显存**：160K 以上显存占用与 128K 几乎相同（都在 15.9–16.0 GB），
   但解码崩到 20–38 t/s。说明逼近显存上限后引擎走入了低效路径（而非简单 OOM）。
   → **不要试图用 KV 量化换更大上下文**：KV 总量本就很小，压它换不来跨过这个边界。

128K 下的真实表现（8K 提示词实测）：

```
提示 2409 token  →  预填充 3 579.7 t/s  |  解码 112.7 t/s  |  峰值显存 15 646 MiB
```

**实践提示**：128K 时显存仅余约 730 MiB，请勿与其他 GPU 任务并用；日常交互用 `fast` 档（32K、余量充裕），
需要长文档处理时切 `long-context` 档。

**并发（`-np N` → 每 slot 分得 ctx/N）**：

用 Bun 原生并发 + **服务端内部计时（`timings`）+ 预热**测得（100 token/请求，np=16，ctx 32768）：

| 并发请求数 | 总 token | 聚合吞吐 | 单请求 | 相对单流 |
|---|---|---|---|---|
| 1 | 100 | 143.1 t/s | 143.1 | 1.02× |
| 2 | 200 | 225.1 | 112.5 | 1.61× |
| 4 | 400 | 329.4 | 82.4 | 2.35× |
| 8 | 800 | 447.3 | 55.9 | 3.19× |
| **16** | **1 600** | **642.1** | 40.1 | **4.59×** |

**627.8 t/s 已超过统一批处理的理论值**（`llama-batched-bench` B=8 → ≈510 t/s）——
连续批处理的动态填充优于固定批。

> **测量提醒（踩过的坑）**：并发测速**必须先做并发预热**。首轮含 CUDA 图对目标 batch size 的捕获，
> 会让结果严重偏低（实测 Q2_K_XL np=4：首轮 225.5 → 稳定后 327.5 t/s）。只预热单请求不够。

#### 6.9.1 显存换并发：本次实测最有效的吞吐提升（+46%）

**背景**：GEBAI 的 prompt（系统提示词 + 工具定义）实测 15 352 token，故每 slot 必须 ≥ 24K。
用 IQ3_XXS（12.80 GB）时 ctx 65536 只能开 **np=2**——再大就撞显存硬边界。

**做法**：换用更低位宽的量化腾出显存，换取更高并发（而不是指望量化本身提速）。

| 模型 | 位宽 | 大小 | 配置 | 显存 | 单流 | 并发聚合（纯 decode） |
|---|---|---|---|---|---|---|
| IQ3_XXS | 3.06 bpw | 12.80 GB | np=2, ctx 65536 | 15 550 MiB | 146 t/s | 218–222 t/s |
| IQ2_XXS | 2.06 bpw | 10.71 GB | np=4, ctx 131072 | 13 805 MiB | 148.9 | 311–319 |
| **Q2_K_XL** | **2.74 bpw** | **11.41 GB** | **np=4, ctx 131072** | **14 427 MiB** | **152.3** | **317–327** |

**Q2_K_XL 是本场景最优解**：位宽高于 IQ2_XXS（质量更好）、单流与并发都更快、显存仍余 1.9 GB。
相对原 `concurrent` 档（np=2）**提升 46%**。对应档位 `concurrent-max`。

**关键机理（反直觉，但实测确凿）**：

- **单纯减小量化不会更快**。IQ2_XXS 单流 148.9 vs IQ3_XXS 146（噪声级），
  2 并发 198–205 vs IQ3 的 218–222——**反而略慢**。因为 GPU 不是带宽瓶颈
  （带宽利用率仅 4–25%，见 6.8），更低位宽反而增加解包开销。
- **收益全部来自显存释放换取的并发度**。并发是唯一被证明有效的吞吐手段（见 6.9.2）。

**极限与边界**：np=8 + ctx 262144 实测 368–378 t/s，但显存 **15 637 MiB 已超出安全线 15 600**——
不建议作为默认。`np=4` 是收益与安全的平衡点。

#### 6.9.2 投机解码：全路径实测否定（勿重试）

投机解码是单流场景下唯一的理论出路（既打破自回归串行，又能把 matvec 变成 matmul），
因此做了完整验证——**五条独立证据全部否定**：

| # | 方案 | 实测结果 |
|---|---|---|
| ① | ngram 系（ngram-mod / ngram-cache） | 29.7 / 37.7 t/s vs 无投机 40.3 —— 自由文本无重复模式 |
| ② | ngram-map-k（理想场景：要求重复文本） | 接受率仅 13%，**105.9 t/s**（基线 146） |
| ③ | 外部 draft 模型（Qwen3.5-2B，同词表 248320） | draft 单独 315.8 t/s（= target 的 2.16×，前置条件满足）；接受率 49–64% 正常；但速度 **33–38 t/s（慢 4 倍）** |
| ④ | 合成接受率（`--spec-synth-len 5`，模拟完美投机） | 仍只有 **52–57 t/s** |
| ⑤ | MTP 头 | 权重中不存在 |

**③④ 是决定性的**：④ 证明即使接受率接近 100% 也翻不了盘——瓶颈是**框架层交替开销**：
每轮投机实测 ~96 ms，而 draft 生成 + target 验证的理论成本仅 ~29 ms。

**为什么 draft 模型路线从一开始就注定**：target 本身是 MoE（激活仅 3B），已经是高效引擎；
draft 速度优势只有 2.16×，远不足以覆盖交替开销。投机解码适合「target 很慢 + draft 很快」
（如 dense 70B + dense 1B），不适用于「target 已是快 MoE」。

> 词表核对方法（投机的前置条件）：直接读 GGUF 元数据里的 `tokenizer.ggml.tokens` 长度与
> 首尾 token，而不是只看 config.json 的 `vocab_size`。本次核对结果：两模型均 248320 token、
> gpt2 词表、eos=248046、首尾 token 一致。

**slot 数存在精确最优（短 prompt 批量场景，同一并发数下扫描）**：

| slot 数 | 显存 | 单请求（无并发时） | 16 路并发聚合 |
|---|---|---|---|
| 1 | 14 862 MiB | 143.1 | — |
| 8 | 14 862 MiB | 143.1 | 447.3 |
| **16** | **15 222 MiB** | **142.6（不降！）** | **642.1** |
| 24 | 15 606 MiB | 85.8（开始劣化） | 482.0 |
| 32 | 15 985 MiB | 11.9（崩塌） | — |

**两条关键结论**：

1. **增大 slot 数不牺牲单请求性能**（np=16 时单请求仍 142.6，与 np=1 持平）——
   slot 是按需分配的，只要显存有余量就可以放心调大。
2. **但存在硬边界**：np=24 起劣化、np=32 崩塌（单请求 11.9），
   与显存逼近上限（15 985 / 16 376 MiB）同步发生——与 6.9 长上下文“超 128K 崩塌”同一现象。

> ⚠️ **测量方法教训（必读，否则会得到完全错误的数字）**：
> ① 本模型首请求含 **CUDA graph 捕获（约 10–12 s）**，必须**先预热**再取数；
> ② **`Start-Process` + `Wait-Process` 本身就引入约 12 s 偏差**——用墙钟会得到「与规模无关的恒定值」
> （曾因此把 8 路并发的 349.5 t/s 误算为 37.5 t/s）。
> 正确做法：用原生并发（Bun `Promise.all`）+ **服务端 `timings` 内部计时**
> （`scripts/bench-concurrency-precise.ps1` 与本文数据均按此口径）。

→ **集成建议**：见下节「GEBAI 场景的真实约束」——并非“无脑 np=16”，
slot 数必须按 prompt 大小反推。

**GEBAI 场景的真实约束（关键，决定了上述高并发不可直接套用）**：

用 GEBAI 真实请求实测发现其 prompt（系统提示词 + 工具定义）达 **15 352 token**：

```
request (15352 tokens) exceeds the available context size (4096 tokens)
```

而每 slot 窗口 = ctx / np，所以：

| 档位 | ctx | np | 每 slot | 能否跑 GEBAI | 聚合（16 并发） |
|---|---|---|---|---|---|
| throughput | 65 536 | 16 | 4 096 | ❌ 报 exceed_context | 627.8 t/s（仅短 prompt） |
| **concurrent** | **65 536** | **2** | **32 768** | ✅ | 114.7 t/s（大 prompt 无增益） |

**两条结论**：

1. **GEBAI 场景下高并发拿不到**：prompt 15K + 显存有限 ⇒ 每 slot 需 ≥ 24K ⇒ np ≤ 2。
   且大 prompt 时预填充占主导，**并发本身也无增益**（120.1 → 114.7）。
2. **真正重要的是 prompt caching**（默认已启用）：实测 3 603 token 固定前缀的第二次请求
   直接命中缓存（`cache_n=3603, prompt_n=4`），**墙钟 0.83 → 0.46 s（−45%）**。
   GEBAI 的固定系统提示词只需算一次——这比提并发对体验的改善大得多。

> 验证脚本：`scripts/bench-concurrency-precise.ps1`（并发上限）、
> `scripts/verify-engine-equiv.ps1`（引擎等价性）、`scripts/verify-bs.ps1`（-bs 正确性）。

**CPU 对照（`-ngl 0`，全 CPU）**：

| 指标 | GPU 全量 | CPU 全量 | 加速比 |
|---|---|---|---|
| 预填充 | 4 391 t/s | 53.8 t/s（pp64） | **81.6×** |
| 解码 | 143.4 t/s | 13.7 t/s（tg16） | **10.4×** |

### 6.10 单流性能上限的量化归因（剖析结论）

前面所有配置扫描都是负结果，因此做了量化剖析定位真正的上限。

**① 批量解码实际能线性扩展**

`llama-batched-bench -npp 128 -ntg 64`（全 GPU）：

| 批大小 | 聚合解码 t/s | 相对单流 |
|---|---|---|
| 1 | 94.96 | 1.0× |
| 2 | 187.53 | 2.0× |
| 4 | 268.46 | 2.8× |
| **8** | **508–512** | **≈3.6×** |

> 早期曾把 B=8 记为 391 t/s，那是冷启动/环境干扰下的偏差；交替 A/B 复核（各两次）
> 得 508.30 / 512.05（official）与 496.53 / 503.32（selfbuilt）。

预填充同样受益（B=4 时 3800 t/s，单流 742）。→ MoE 的权重读取确实能被批量摊薄。

**② 任意两批大小即可反解出「固定开销」与「边际成本」**

设每次 decode 调用固定开销 O、每 token 边际成本 C，则：
- B=1：O + C = 7.3 ms/token（实测 tg256 = 137 t/s）
- B=8：O + 8C = 15.7 ms / 8 token（实测 ≈510 t/s）

解得 **O ≈ 6.1 ms（每次调用）**、**C ≈ 1.20 ms（每 token）**。

**→ 单流时 84% 的时间是固定开销；GPU 真正干活只占约 16%**（C = 1.2 ms）。

这个结果与独立的剖析数据自洽：O ≈ 6.1 ms ≈ 采样等待 4.6 ms + graph launch 0.7 ms + 其它（见 6.11–6.12）。

> 注意：服务端连续批处理的实测聚合（ctx 32768、np=16、并发 16 → **642 t/s**；
> ctx 65536 时为 627.8 t/s，见下节 GEBAI 约束）**超过**同步批处理的
> B=8 上限（≈510 t/s）——因为连续批处理能在请求完成时动态补位，利用率高于同步推进。
> 所以服务端的真实上限应以 6.9 的实测为准，不能用 batched-bench 直接外推。

**③ 实测确认：GPU 时间上很忙，但硬件填不满**

生成期间 `nvidia-smi dmon`：SM 占用率 18–38%、显存带宽 4–25%。
但 nsys 同时给出：**decode 期 GPU 时间利用率 88%**（每 100 ms 有 88 ms 有内核在跑）。
两者不矛盾——SM% 是 **warp 活跃度**，不是时间占用率。

真正的病理是**内核碎片化**：

| 指标 | 实测 |
|---|---|
| decode 期内核数 | **136 779 / 96 token = 1 425 个/token** |
| 内核平均时长 | **3.87 µs**（最短 0.77 µs） |
| 单个采样同步（4.7 ms）内的内核忙碌 | **4.4 ms（93%）** |

→ 解码**既不是算力/带宽瓶颈，也不是“等主机”**（GPU 大部分时间在跑内核）；
而是**每 token 上千个微小内核串行执行，单个内核填不满 SM**（18–38%）。
这既是批量能摊薄的原因（batch=8 时每个内核算 8 个序列，时间只增 ~2.2× 而吞吐 8×），
也指向真正的优化方向：**算子融合 / 增大内核粒度**，而不是“消除往返”。

> **复现方法**（判断“同步期间 GPU 是在算还是在等”）：
> 1. `nsys profile --cuda-graph-trace=node -o sample-wait <llama-bench ...>`；
> 2. 对每个 `cudaStreamSynchronize` 区间，与 `CUPTI_ACTIVITY_KIND_KERNEL` 求重叠：
>    `SELECT COUNT(*), SUM(MIN(k.end,r.end)-MAX(k.start,r.start)) FROM ... WHERE k.start < r.end AND k.end > r.start`；
> 3. 重叠忙碌 / 同步时长 ≈ 93% → GPU 在真算（而非等主机）。
> **注意**：WDDM 下内核**时长**会失真，但“是否有内核重叠”这个存在性判断仍然可靠。

**④ 已穷尽的旋钮（全部无实质效果，供后人不再重复）**

| 类别 | 测过 | 结果 |
|---|---|---|
| 线程 | 8 / 14 / 20 / 28 | 140.1–143.7（噪声） |
| 同步与优先级 | `--poll 0/100`、`--prio 2/3`、`--cpu-range` | 无改善（prio=3 反降） |
| 批次 | `-b/-ub` 各组、`--no-host` | 无改善（后者不兼容） |
| 注意力 | `-fa on/off`、KV `q8_0`/`f16` | 无差异 |
| CUDA 后端 | `GGML_CUDA_PDL`、`FORCE_CUBLAS`、`F16` | 无差异 |
| 采样 | `-bs`（后端采样） | **+5%**（130.5 → 136.9 t/s，非主因） |
| **CUDA graphs** | 禁用对比 | **关键：141 → 59 t/s**，必须开启（已默认） |

**⑤ 投机解码在「本模型的官方 GGUF」上不可行（已定论；注意适用范围）**

- ngram 类自投机：实测低于无投机基线（自由文本缺重复模式）；
- MTP 头：config 虽声明 `mtp_num_hidden_layers: 1`，但**官方原始权重就没有 MTP 张量**
  （索引 693 张量 / 40 层，无 mtp/nextn，也无视觉塔）——无米之炊，彻底排除；
- 外部草稿模型：词表不匹配（248320），不可用。
- ⚠ **适用范围的修正（2026-09-26）**：以上三条都以「**该 GGUF 里没有 MTP 张量**」为前提。
  换一个带 MTP 张量的 GGUF，MTP 投机就成立——Qwen3.8-27B 实测 **+42%~78%**（见 §6.15）。
  **判据是 GGUF 里有没有 MTP 张量（`local_infer_inspect` 的「MTP 头」一栏），不是模型名。**

**⑥ 下一步方向（含一个已否证的假设，避免重走）**

O ≈ 6.1 ms 的量级远超 CUDA API 应在的开销（nsys 实测 `cudaGraphLaunch` 641 µs、
`cudaStreamSynchronize` 290 µs，合计不到 1 ms），因此曾怀疑是 **Windows WDDM** 的计算命令延迟
（本卡以 WDDM 模式运行，非 TCC）。

**验证结果：假设不成立。** 在 WSL2 下用官方 Linux CUDA 构建跑同一模型、同参数：

| 环境 | pp512 t/s | tg256 t/s |
|---|---|---|
| Windows（原生 CUDA 构建） | 4 391 | **140.98** |
| **WSL2 / Linux（官方 Linux CUDA 构建）** | 3 947 | **137.46** |

两平台几乎一致 ⇒ **限制不在操作系统/驱动路径，而在 llama.cpp 自身解码循环的结构**。

### 6.11 解码期内核级剖析（含 CUDA graph 内节点）

> **剖析前提**：解码路径全程在 CUDA graph 内，**必须加 `--cuda-graph-trace=node`** 才能看到图内节点；
> 不加时内核数是 5 942（只含 prefill 与图外内核），加了是 **83 014**（+13 倍）——两者的结论完全不同。

采集：`nsys profile --cuda-graph-trace=node -p 8 -n 48`（自建 CUDA 引擎，b11100 源）

**解码期 Top 内核**（按总耗时）：

| 内核 | 调用 | 总耗时 | 占比 | 平均 | 最大 | 网格 |
|---|---|---|---|---|---|---|
| `mul_mat_vec_q<Q6_K,1>` | 5 784 | 119.9 ms | 21.4% | 20.7 µs | 1.72 ms | 248320×1 |
| **`mul_mat_vec_q_moe<IQ2_S>`** | **78** | **87.5 ms** | **15.6%** | **1.12 ms** | **12.09 ms** | 256×8 |
| `mul_mat_vec_q<Q6_K,8>` | 486 | 76.0 ms | 13.6% | 156 µs | 7.15 ms | 4096×1 |
| **`mul_mat_vec_q_moe<IQ3_S>`** | **74** | **53.8 ms** | **9.6%** | **728 µs** | 7.77 ms | 1024×8 |

**MoE 专家内核合计 25%**，且**平均 1.12 ms / 最大 12.09 ms（10 倍方差）**。
每层专家权重约 86 MiB，按 736 GB/s 应在 **117 µs** 完成，实测 **1 120 µs → 约 10% 带宽效率**。

**`nsight_findings` 定级（按可回收时间）**：

| 严重度 | 问题 | 关键证据 | 可回收 |
|---|---|---|---|
| 高 | 小内核过多 | **72 833 次调用 < 10 µs（87.7%）**；同流相邻内核平均间隔 929.55 µs（含主机侧启动） | 128 ms |
| 高 | 同步等待 | Stream wait sync **1 917 次** / 640 ms | 640 ms |
| 中 | 网格并行度未填满 | 20 个内核网格仅 512–1024 块（≈65K 线程，硬件可容十万级） | 314 ms |
| 中 | 寄存器占用偏高 | `mul_mat_vec_q_moe` 每线程 72 寄存器、块 32×8 | — |

**最关键的线索（已量化坐实）**：解码期的流同步随 token 数线性增长（受控实验）：

| 运行 | 同步次数 | 内核数 |
|---|---|---|
| `-n 8` | 997 | 5 192 |
| `-n 64` | 2 285 | 5 192 |
| **差值 / 56 token** | **+1 288 → 23.0 次/token** | **+0** |

```
23 次同步/token × 306 µs = 7.0 ms/token
实测每 token（B=1）：     7.3 ms/token   ← 吻合
```

同类受控对比得出每 token 的完整开销清单：

| API | 每 token 增量 | 含义 |
|---|---|---|
| `cudaGraphLaunch` | 1.00 | 图启动（正常，每步一次） |
| `cudaLaunchKernel` | **0.00** | 不随 token 增长——**不是内核启动问题** |
| `cudaMemcpyAsync` | **10.00** | 每步 10 次主机↔设备拷贝 |
| `cudaStreamSynchronize` | **23.00** | 每步 23 次流同步 |

**注意区分「次数多」与「代价大」**：同步与拷贝的次数确实多（23 + 10），但**代价很小**——
两者合计仅 **0.27 ms/token**（占 7.1 ms 的 3.8%，见 6.12 末）。
单流的真正主体是**每步上千个微小内核的串行执行时间**（1 425 个/token，均 3.87 µs）——
它们填不满 SM（占用率 18–38%），但 GPU 时间上确实在忙（利用率 88%）。

### 6.12 进一步定位（一个被证否的候选，记录以免重试）

高详细度日志显示**每次 decode 的计算图被切成两段**：

```
sched_reserve: graph: nodes = 3907, splits = 2, input objects = 4, input tensors = 9
sched_reserve:      CUDA0 compute buffer size =     4.86 MiB
sched_reserve:  CUDA_Host compute buffer size =     0.33 MiB
done_getting_tensors: tensor 'token_embd.weight' (q6_K) cannot be used with
                      preferred buffer type CUDA_Host, using CPU instead
llama_context:      CUDA_Host  output buffer size =     0.95 MiB   (= 248320×4，logits)
```

嵌入表驻 CPU + logits 输出缓冲在主机，使图被切为两段。

**但把切分消除后并没有提速——所以切分不是瓶颈**：

| 配置 | 图切分 | tg256 |
|---|---|---|
| 基线 | **splits = 2** | 138.81 |
| `-ot token_embd.weight=CUDA0` | **splits = 1** | 140.10 |

（`-ot` 生效已被日志确认：`buffer type overridden to CUDA0` → `splits = 1`。）
切分从 2 降到 1 而吞吐持平 ⇒ **每步 23 次同步的成本不在图切分上**，
而在每步的**采样路径与状态管理**（其中 `CUDA_Host output buffer 0.95 MiB` 始终存在——
CPU 采样必须把 logits 回传；这正是 `-bs` 后端采样能消除的那一次）。

**已逐一排除的候选修法**（全部无效，供后人不再重复）：

| 尝试 | 结果 |
|---|---|
| `-ot token_embd.weight=CUDA0`（嵌入表推 GPU，图切分 2→1） | 140.10 vs 138.81 —— **噪声内，无改善** |
| `--no-host` | 参数被拒（实现不完整） |
| 线程/批/FA/KV/CUDA 环境变量全旋钮 | 均落在噪声区间（见 6.6） |
| 换平台（WSL/Linux 官方构建） | 137.46 vs 140.98 —— 一致，非平台问题 |
| `-bs` 后端采样（消除 logits 回传） | 130.5 → 136.9 t/s，**+5%**（唯一有效但幅度小） |

**因此剩下两个真正有效的方向**：

1. **批处理（已落地）**：微小内核在 batch>1 时被填得更满（batch=8 时每个内核算 8 个序列，
   时间只增 ~2.2× 而吞吐 8×），`throughput` 档（np=16，短 prompt）聚合 627.8 t/s（**4.30×**，
   超过统一批处理 B=8 的 ≈510）——见 6.9。
2. **算子融合 / 增大内核粒度（需改引擎）**：把每 token 上千个微内核合并成更少的、填得满 SM 的内核。
   这才是单流的根本出路。

**为什么不能靠“减少同步次数”拿到大幅提速**（定量依据）：explore 给出的时间预算显示，
23 次同步 + 10 次拷贝合计仅 **0.27 ms/token**（占 7.1 ms 的 3.8%）。
而采样等待的 4.7 ms 经实测**是 GPU 真的在内核算**（重叠 4.4 ms，93%）——
**不是等主机，也不是往返延迟**。

> **一个被实测否决的诱人方向**：曾推测“把采样完全搬到显卡、消除主机往返”能大幅提速。
> 但 4.7 ms 里 93% 是 GPU 繁忙，往返只占 ~0.3 ms——**上界约 5%**，
> 正好解释 `-bs` 为何只涨 5.7%（它已把回传数据量降了四个数量级，993 KB → 4 B）。
> 结论：改采样架构**不值得**，真正方向是减少内核数量（算子融合）。

> 本节的结论修正过一次：最初把图切分当作根因，经验证（消除切分后无提速）后推翻。
> 保留此过程是因为它排除了一条看似成立的路径——避免后人重走。

### 6.13 待测

| 项 | 说明 |
|---|---|
| 首 token 延迟（长提示词） | 当前仅测了短提示 |
| 质量标尺 | 三档的困惑度/任务级对比（`verify.ps1` 已备好固定种子回归），用于把「档位制」建立在质量数据上而非位宽直觉 |
| IQ4_XS 的 `-ot` 张量级 offload | 只把 `ffn_down_exps` 留 CPU（而非按层整块），CPU 侧流量 173 MiB/token——需实测是否优于按层方案 |

### 6.14 Qwen3.6-35B-A3B：第二个可用模型（2026-09-25 落地）

同一张 16GB 卡上跑通的**另一个** 35B-A3B MoE（`qwen35moe` 同架构族，官方 Qwen3.6 发布），
档位 `qwen36-a3b`。与 `fast` 档同量级、可直接互换，便于按任务挑选。

| 项 | 实测值 |
|---|---|
| 模型 | `Qwen3.6-35B-A3B-UD-IQ3_XXS.gguf`（13.10 GB，unsloth UD 现成档，来源见 `assets.manifest.json`） |
| 架构 | 41 层（40 主 + 1 MTP）/ 256 专家激活 8+1 / 上下文原生 262144 |
| 显存 | **14 779 / 16 376 MiB**（余量约 1.6 GB） |
| 加载 | **11 s** |
| 解码 | **127.6 ~ 141.3 t/s**（服务端计时；短输出 127.6、带思维链 141.3） |
| 中文问答 | ✅ 正常 |
| 结构化输出 | ✅ schema→GBNF 约束解码，一次通过 |
| 工具调用 | ✅ `finish=tool_calls`，参数 `{"city":"北京"}` 正确 |
| 思维链 | ✅ `reasoning_content` 2250 字符，与正文分离 |
| 数学推理 | ✅ 鸡兔同笼答对（23 鸡 / 12 兔） |

**它也是推理型模型**：思维链可吃掉大部分输出预算（实测 900 token 全被思考占满、正文为空），
所以 `enable_thinking=false` 仍应是默认（与第十章同结论）。

**选型建议**：`qwen36-a3b` 与 `fast`（AgentWorld）互换；前者为通用/编码向，后者为 AgentWorld 工具调用向。

### 6.15 Qwen3.8-27B：第三个可用模型（dense 27B 视觉语言模型，2026-09-26 落地）

档位 `qwen38-27b`。与前面两个 35B-A3B **MoE** 不同，这是 **dense** 模型
（64 层混合 Gated-DeltaNet + Gated Attention，27B 全参激活）——每 token 都要过全部权重，
解码天然慢于 MoE 档（受显存带宽限），换来的是原生视觉与更强的长程能力。

16GB 显存下的关键取舍：**能整卡装进显存的最高质量档是 `UD-IQ4_XS`（13.27 GiB）**，
再高一档 `UD-Q4_K_XL`（16.35 GiB）必然溢出到 CPU。可选档共 27 个（`UD-IQ1_S` 5.77 → `UD-Q8_K_XL` 29.3 GiB），
IQ4_XS 之后显存就装不下了，Q4_K_XL 只在「接受 CPU offload 掉速」时才值得。

| 项 | 实测值 |
|---|---|
| 模型 | `Qwen3.8-27B-UD-IQ4_XS.gguf`（13.27 GiB = 14 252 845 984 B，sha256 `40fac405…` **逐位校验通过**） |
| 来源 | modelscope `unsloth/Qwen3.8-27B-GGUF`（README 的 base_model 为 `Qwen/Qwen3.8-27B`） |
| 视觉 | `mmproj-F16.gguf`（0.86 GiB，sha256 `cbb841a9…` 已校验；`--mmproj` 走 mtmd 路径。**本档暂未挂载**） |
| 架构 | `qwen35`（`model_type=qwen3_5`，`Qwen3_5ForConditionalGeneration`）；64 层 = 16×(3×线性注意力 + 1×full attention)；原生上下文 262144 |
| 显存 | **16 006 / 16 376 MiB**（含 MTP draft context；**余量仅约 370 MiB**，勿与该卡上其它 GPU 任务并用） |
| 加载 | **11 s** |
| 解码 | **60.0 ~ 79.0 t/s**（开 MTP）／**42.2 t/s**（关 MTP，同 prompt 对照） |
| 预填充 | **pp512 1951.9 / pp2048 1986.9 t/s**（llama-bench 干净基线）；server 实测 15.3K token 长 prompt **997.6 t/s** |
| 中文问答 | ✅ 正常（200 字说明文，内容准确） |
| 思维链 | ✅ `reasoning_content` 与正文分离（thinking 默认开，`enable_thinking=false` 可关） |
| 数学推理 | ✅ 概率题答对（3 红 5 蓝取 2 皆为红 = 3/28） |

**⚠ MTP 投机：短请求有效，但在本档的长 prompt 场景下会拖垮 prefill（已关闭）**

这个 GGUF **确实带 MTP 张量**（18 个，`local_infer_inspect` 的「MTP 头」一栏直接可见），
`--spec-type draft-mtp --spec-draft-n-max 3` 实测（服务端计时）：

| 输出长度 | 关 MTP | 开 MTP | 增益 | 接受率 / 平均投机长度 |
|---|---|---|---|---|
| 140 token（同一 prompt） | 42.2 t/s | **60.0 t/s** | **+42%** | 0.50 / 2.50 |
| 400 token | — | **75.3 t/s** | — | 0.67 / 3.00 |

机理与 §6.12 的⑤对照起来很清楚：那条「投机不可行」的结论建立在「GGUF 无 MTP 张量」之上（无米之炊），
而 dense 27B 每 token 的搬运量比 MoE 大得多、单步更慢（23.7 ms/token），
于是投机解码的框架层开销（当时测得每轮 ~96 ms，主导了 MoE 上的失败）被摊薄，接受率带来的收益就能显出来。

**但这个"有效"只在上面的短请求条件下成立。** 把它用到 GEBAI 的真实负载（15K prompt）时性能反向崩塌：
MTP 的 draft context 额外吃 **657 MiB** 显存（16 006 → 15 349 MiB 即其代价），
本档余量本就只有 1.7 GB，被压到 **370 MiB** 后触发显存换页——
同一请求的 prefill **从 1870 t/s 掉到 190 t/s（慢 10 倍）**，端到端 8.4 s → 80~146 s。

| 同一 GEBAI 请求（15.3K prompt） | 关 MTP | 开 MTP |
|---|---|---|
| 显存占用 | 15 349 MiB | 16 006 MiB |
| prefill | **1860 ~ 1874 t/s** | 190 t/s |
| 端到端 3 连测 | **8.4 / 8.4 / 8.5 s** | 80.9 / 145.3 / 145.9 s |

**结论（修正 §6.12 的⑤）**：投机是否值得，不只看"GGUF 有没有 MTP 张量"，还要看
**它吃掉的显存会不会把余量压到换页阈值之下**。GEBAI 这类「prompt 15K、output 几百 token」的负载里，
**prefill 才是主成本，必须优先保 prefill**——所以本档关闭 MTP。

### 6.16 用本模型驱动 GEBAI 实例（:3100）与一次 prefill 异常的排查（2026-09-26）

本档可直接作为 GEBAI 的模型算力：`infer/scripts/run-gebai-local.ps1 -Port 3100 -InferPort 8080
-InferModel qwen3.8-27b`，得到一个与主实例完全隔离的 GEBAI（独立数据根 `.gebai-local`、
不跑调度、不订阅飞书），主实例（:3000，deepseek 驱动）不受影响。

| GEBAI 端到端（3100 实例，含 15.3K token 系统提示+工具 schema） | 实测 |
|---|---|
| 冷启动后第一个请求（server + GEBAI 双双重启） | **8.4 s** |
| 连续对话请求 ×3 | **8.4 / 8.4 / 8.5 s** |
| server 侧 prefill（15 340 token） | **1870 t/s** |
| 上下文预算 | 24 576（服务端 n_ctx 32768 − 输出预留 8192，脚本自动收敛） |

**⚠ 一次 prefill 崩塌的排查记录（结论：MTP 吃显存所致，已关闭）**

首次接入时观察到 **prefill 仅 23.4 t/s**（88 s 处理 2048 token，15.3K prompt 需约 11 分钟，
GEBAI 侧 300 s 超时）。排查过程中一度"恢复"到 15~18 s、又反复劣化到 80~146 s，
最终定位为 **MTP draft context 占用 657 MiB 把显存余量压到 370 MiB → 触发换页**（见 §6.15）。

| 排查项（按当时怀疑顺序） | 实验 | 结论 |
|---|---|---|
| 模型/架构本身慢（GDN 线性注意力等） | llama-bench 干净基线（`-fitt/-fitc`） | ❌ pp2048 **1986 t/s**，绝不慢 |
| 工具 schema 的 GBNF grammar 拖慢 prefill | 1 / 30 / 真实 20 工具三组对照 | ❌ 761~1077 t/s，正常量级 |
| 冷启动（CUDA graph/kernel 首次编译） | 重启 server 后**立即**发 4.5K / 6K 长 prompt | ❌ 4.3 s / 5.6 s，正常 |
| 请求体本身有异常参数 | 抓包代理录下真实请求体（stream/tools/23.8KB schema）原样重放 | ❌ 正常 |
| GPU 降频 | 请求全程采样 `nvidia-smi` | ⚠ 确实出现过 P8/210 MHz，但**全速 P2/2745 MHz 时同样 80~146 s** → 非主因 |
| **MTP draft context 挤占显存** | **同配置仅切换 MTP，连测 3 次** | ✅ **关 MTP：prefill 1870 t/s、8.4 s；开 MTP：190 t/s、80~146 s** |

**顺带确认的两条有用事实**：① llama-server 的 **prompt caching 有效**——固定前缀（系统提示+工具 schema）
二次请求直接命中，重放同一请求 1.9 s 返回（注意：GEBAI 每次新建会话时前缀含会话路径，会全量 prefill，
同一会话续聊才命中）；② llama-bench 干净基线与 server 实测在关 MTP 后一致（~1950 vs ~1870 t/s）。

> 两条教训：
> ① **别把单次观测当结论**——我一度先判"偶发、未能复现"，实际是可稳定复现的显存问题，
>    只是它随"显存余量"在好坏之间摇摆，单次采样很容易采到错的一侧。**稳定复现要控变量连续测**。
> ② **bench 正常 ≠ 问题在 server 配置**——这次 bench 一直正常（1950 t/s），
>    但真凶正是 server 侧多出来的那个参数（MTP）。bench 的作用是排除"模型/引擎慢"，
>    剩下的怀疑要往"server 特有参数"上收。

### 6.17 128K 上下文档 `qwen38-27b-128k`：降档换窗口 + MTP 的真实权衡（2026-09-26）

目标：把 Qwen3.8-27B 的窗口从 32K 提到 **128K**，并保留 MTP。结论先行：
**两个目标都达成了，但 MTP 是一笔需要按输出长度结算的账，不是白拿。**

#### 为什么必须降档（静态估算 → 被实测修正）

IQ4_XS 装不下 128K，故降到 **UD-IQ3_XXS（10.18 GiB，sha256 `c0b7c303…` 已逐位校验）**：

| 方案 | 模型 | KV@128K | 合计 | 判断 |
|---|---|---|---|---|
| IQ4_XS + q8_0 KV | 13 588 | ~4342 | ~18 530 MiB | ❌ 超 2 GB |
| IQ4_XS + q4_0 KV | 13 588 | ~2304 | ~16 490 MiB | ❌ 超 |
| IQ3_S + MTP | 11 479 | ~2304 | ~15 040 MiB | ⚠ 贴安全线 |
| **IQ3_XXS + MTP** | **10 424** | **~2304** | **~13 985 MiB** | ✅ 选定 |

**估算被实测修正**：实际显存 15 628~15 809 MiB（MTP 开）/ 14 102 MiB（MTP 关），
高于估算的 13 985——反推 **q4_0 KV 实际约 30 KiB/token，而非按层数推算的 18 KiB**。
122 043 token 的极限 prompt 显存峰值 15 809 MiB 未超上限。
教训：**本模型的 KV 成本别用层数推算，一定以实测量为准**。

#### 实测数据

| 项 | 值 |
|---|---|
| 模型 | `Qwen3.8-27B-UD-IQ3_XXS.gguf`（10.18 GiB，sha256 逐位校验通过） |
| 上下文 | **n_ctx 131072 实测生效**（GEBAI 侧预算自动收敛为 122 880） |
| 加载 | **8~11 s** |
| 显存 | 15 628~15 809 MiB（MTP 开）/ 14 102 MiB（MTP 关） |
| prefill | 1600~1626 t/s（MTP 开）/ **1716~1746 t/s**（MTP 关），15.3K prompt |
| decode | **52.8~53.5 t/s**（MTP 开）/ 43.3~43.5 t/s（MTP 关） |
| 长上下文 | **45 461 tok → 33.3 s｜91 413 tok → 45.0 s｜122 043 tok → 111.1 s**（全通） |
| 质量 | 中文问答 / 思维链 / 代码 bug 分析 / 概率题（复测 3/3）均正常，**未见降档退化** |

#### MTP 的真实权衡（与 §6.15 的"短请求 +42%"合并看）

| 指标 | MTP 开 | MTP 关 | 差异 |
|---|---|---|---|
| 显存 | 15 628~15 809 | 14 102 | +1 526 MiB |
| prefill | 1600~1626 t/s | 1716~1746 t/s | **−7%** |
| decode | 52.8~53.5 t/s | 43.3~43.5 t/s | **+22%** |
| 短问答端到端 | 10.2~10.9 s | **9.0~9.7 s** | MTP 更慢 |
| 600 token 回答端到端 | **17.4~19.3 s** | 22.2~23.0 s | MTP 更快 |

**盈亏平衡点在约 148 个输出 token**：MTP 用 −7% 的 prefill 换 +22% 的 decode，
输出短于 148 token 时整体反而变慢。GEBAI 的 tool-call 轮次输出常偏短、最终答复往往数百 token，
故本档**默认开启**并由用户按负载选择（去掉 `extra_args` 里 `--spec-type`/`--spec-draft-n-max` 即关）。

> 与 §6.15 对照：那次 MTP 在 32K + IQ4_XS 下把 prefill 从 1870 打到 190 t/s（−90%，不可用）；
> 本次在 128K + IQ3_XXS 下只降 7%（可用）。**同一参数在不同显存预算下后果差一个数量级**——
> 再次印证"显存余量"才是这个模型性能的主变量。

### 6.18 稠密 vs 混合专家：批量推理性能实测对比（2026-09-26）

两组模型同量化（IQ3_XXS）、同卡全 GPU 驻留、同 KV（q8_0）、同 batch 参数，
用 `llama-batched-bench -npp 512,2048 -ntg 128 -npl 1,2,4,8` 测（表取 npp=2048）：

| | 稠密 Qwen3.8-27B | MoE Qwen3.6-35B-A3B |
|---|---|---|
| 参数 | **27B 全激活** | 35B 总参 / **3B 激活** |
| 权重体积 | 10.18 GiB | 13.10 GiB |

| 并行 B | 稠密 TG | MoE TG | MoE/稠密 | 稠密 prefill | MoE prefill |
|---|---|---|---|---|---|
| 1 | 49.54 | **143.76** | 2.90× | 1892.74 | **4541.14** |
| 2 | 86.78 | **257.07** | 2.96× | 1893.57 | **4662.80** |
| 4 | 135.30 | **367.89** | 2.72× | 1901.93 | **4615.86** |
| 8 | 191.59 | **494.93** | 2.58× | 1888.56 | **4631.26** |

**四条实测结论：**

**① prefill 与批量无关，两者各自恒定。** 稠密稳定在 ~1900 t/s，MoE 稳定在 ~4630 t/s
（4 档并行度波动 <1.5%）——预填充是纯算力密集，加批量不改变单位算力的处理量。

**② decode 随批量提升，但都远非线性。** 8 倍批量只换来：稠密 **3.87×**、MoE **3.44×**
（效率 48% / 43%）。单流速度随批量同步下降（稠密 49.5→24.0 t/s、MoE 143.8→61.9 t/s），
即**加并发是用单请求延迟换聚合吞吐**，且 MoE 的单流衰减更快。

**③ MoE 的优势随批量增大而收窄**：2.90×（B=1）→ 2.58×（B=8）。
机理：稠密模型在 B=1 时是**算力受限**（每 token 要过全部 27B 权重），加批量把权重读取
摊薄到多条序列上，收益大；MoE 在 B=1 时已接近**显存带宽受限**（每 token 只读约 3B 激活权重，
且不同序列路由到**不同专家**、权重无法共享），加批量几乎没有可摊薄的空间。
→ **批量推理场景下稠密的"批量红利"更大；MoE 的优势主要来自单流基线就高。**

**④ MoE 的实际加速比远小于参数量比（27B/3B = 9×），只有 2.4~2.9×。**
因为**非专家部分是共享开销**：稠密的 FFN 只占 6.216/10.17 GiB（61%），
其余 39%（attention、embedding、output projection）两个模型都要全量计算，MoE 省不掉。
路由与专家 gather 也另有开销。**"激活 3B"不等于"快 9 倍"。**

**对本项目的选型含义**：GEBAI 的典型轮次是「15K prompt + 数百 token 输出」，
按上表推算稠密约 18 s、MoE 约 6.7 s（**2.7×**）——与 §6.16/§6.17 的端到端实测吻合。
故：**交互式会话选 MoE**（prefill 2.4× + decode 2.9× 双重优势）；
稠密 27B 的价值在原生视觉与长程能力，**不是吞吐**。批量任务（triage、批量抽取）两者都吃批量红利，
但 MoE 仍领先约 2.5×。

> 方法学注记：`llama-batched-bench` 的**首行（B=1）有预热伪影**——MoE 首次测 PP 得 1575 t/s，
> 复测得 2186 t/s，而 B≥2 两次吻合（4600/4666 vs 4676/4694）。
> 引用首行数据前必须复测，或用 `--no-warmup` 明确取舍。

---

## 七、引擎改造的启动条件

自建 fork 是本项目「完全可控」的最终形态。**前置条件已达成**（可重复构建 CUDA 引擎，
与官方二进制性能持平），且剖析已把开销结构量化到具体数字（每步 23 次同步 + 10 次拷贝，
见 6.11–6.12，并已排除图切分这一候选）。

### 已就绪的部分

| 项 | 状态 |
|---|---|
| 源码 | `engine/llama.cpp-b11100`（已解压，含 `src/models/qwen35*.cpp`、`delta-net-base.cpp`） |
| 构建脚本 | `scripts/build-engine.ps1`（cpu/cuda/vulkan 三后端，含 **nvcc 宿主编译器版本门禁**，自动探测 vcvars 与 ninja） |
| 工具链约束 | 已实测并记录（见 `engine/README.md` 的兼容性表） |
| 补丁目标 | 上游 qwen3_5 未闭合条目已列出（见下节）；参数层已确认无空间（见 6.6） |
| 精度基线 | `smoke-test.ps1`（全链路）+ `verify.ps1`（固定种子质量回归） |

### 编译环境（已解决）

| 路径 | 结果 |
|---|---|
| **VS Installer 并存旧工具集**：同一 VS 实例内装 VS2022 era 的 MSVC 14.44，用 `vcvars64.bat -vcvars_ver=14.44` 选中 | ✅ **本机构建方案**（`engine/build-cuda-1444.bat`，实测 460/460 构建成功，性能与官方持平） |
| LLVM 官方包解压 `clang-cl`，`nvcc -ccbin clang-cl` | ✅ 备选（本机已下载至 `vendor/llvm/`） |
| VS2026（14.51）+ CUDA 12.9 | ❌ nvcc 硬拒，逃生开关下 `cudafe++` 崩溃 |

### 靶心与风险

**根因已定位**（见 6.10–6.12）：每 token **1 425 个微小内核**（均 3.87 µs）串行执行，
GPU 时间利用率 88% 但 SM 占用率仅 18–38%——硬件填不满，不是带宽问题、也不是等主机。
已排除：图切分、参数旋钮、平台差异、嵌入表落位、采样往返（实测仅值 ~5%）。
**主攻方向是算子融合 / 增大内核粒度**——把上千个微内核合并成少数填得满 SM 的内核。
（此为**方向判断**：SM 占用率 18–38% 说明填不满，但融合的具体收益未实测——
可能是 1.5×，也可能受限于图调度架构。）

**修复方向明确但属引擎核心改造**：合并两段图（嵌入/logits 设备化）、
循环状态缓冲常驻设备、用后端采样消除 logits 回传。
验收标准：同步次数/步 → O → 单流 tok/s（可量化、可回归）。

**风险**：llama.cpp 的调度器与循环状态耦合较深（`llama-memory-recurrent`），
且上游正活跃演进该部分，局部 fork 需持续 rebase；改动必须逐项过 `verify.ps1` 质量回归。
工作量按天计，为本项目风险最高的一项。

### 启动门槛

- ✅ 可重复构建 CUDA 引擎（已达成）
- ✅ 根因量化到具体机制与可验收指标（已达成）
- ⏳ 待决：是否投入引擎级改造（减少每步同步/拷贝；天级工作量、需正确性回归守护）

> 现状：**140 t/s 是 llama.cpp 现有实现的稳态上限，参数层空间已完全用尽**。
> 进一步提速只能改引擎代码；不改也能用——多会话并发已能拿到 2.67× 聚合。

---

## 八、已知风险与上游现状（2026-09 核实）

llama.cpp 的 qwen3_5 支持仍有未闭合条目。**当前使用方式下均未触发**（现成 GGUF + 纯文本路径），
因此不作为行动项，仅作库存记录——一旦需要自转换或出现对应症状，这些就是补丁的起点
（启动条件见上节）：

| 条目 | 症状 | 我们的应对 |
|---|---|---|
| #27019 / PR #27132 | `convert_hf_to_gguf` 对混合线性注意力张量（ssm_conv1d 核维度、in_proj_a/b 扩展）处理有问题 | 只用现成 GGUF 可规避；自转换时打补丁 |
| #26916 | 混合模型加载报 `tensor 'blk.32.attn_norm.weight' not found` | 加载期校验，锁定 stride/层映射 |
| #28166 | 混合递归模型的 mrope 位置警告 | 纯文本场景影响小，持续跟踪 |
| #28879 | GDN 架构上 perplexity 非精度单调（F16 反而不如 Q4_K_M） | **关键**：不盲信"更高精度更好"，用实测质量标尺选档 |
| （本次新发现）**分片命名探测过窄** | `conversion/base.py:232-236` 先按 `model*.safetensors` 前缀探测分片；若仓库用 `layers-N.safetensors` / `outside.safetensors` 这类命名（Qwen3.6 官方 FP8 仓即如此），会退化为找 `pytorch_model.bin.index.json`，最终 **0 张量**——“转换成功”但只导出词表空壳（10.9MB），不报错 | 用 `scripts/prepare-convert-mirror.py` 造硬链接镜像绕开（零拷贝）；值得提上游 issue |
| （本次新发现）**i-quant 的 imatrix 覆盖缺口** | `IQ3_XXS` 对每张量强制要求 imatrix（`llama-quant.cpp:1264`）；混用兄弟模型 imatrix 时，对方没有的层（如 Qwen3.6 的 MTP 层 `blk.40.*`，共 40 张量）会跑到末尾才 bail out | `requires_imatrix` 按**覆盖后目标类型**判定（`llama-quant.cpp:1095`）→ `--tensor-type 'blk\.40\..*=q4_K'` 可绕过（代价 0.30→0.44 GiB）；治本是在本模型上自算 imatrix |

**构建工具链注意**：CUDA 12.9 的 nvcc 只支持 MSVC ≤2022（宿主编译器硬校验），
本机 VS2026（MSVC 14.51）会触发 `unsupported Microsoft Visual Studio version`，
加 `-allow-unsupported-compiler` 后 `cudafe++` 直接 ACCESS_VIOLATION（已实测）。
→ 自建 fork 需要 VS2022 生成工具（或免安装的 clang-cl）作为 nvcc 的宿主编译器；
**预编译二进制路径不受影响**。

---

## 九、安全与可控

- 全链路离线：不依赖任何云 API；模型与引擎均落本地磁盘，来源与校验和可追溯。
- 可审计：每次启动的完整命令行、显存规划、引擎版本记录进 `bench/reports/`。
- 可回退：档位化配置，任一优化不达标即回滚到上一档；`git` 管理 `config/` 与 `engine/patches/`。

---

## 十、在 GEBAI 中使用

把本地引擎接成 GEBAI 的模型算力，起一份独立实例体验（`scripts/run-gebai-local.ps1`）：

```powershell
# 先拉起推理服务
pwsh -File infer/scripts/run-server.ps1 -Profile fast -Background

# 再起一个独立的 GEBAI 实例（默认 :3100 → 本地模型）
pwsh -File infer/scripts/run-gebai-local.ps1

# 停止
pwsh -File infer/scripts/run-gebai-local.ps1 -Stop
```

**与既有实例共处**（脚本已内置，无需手工配）：独立数据根 `<仓库根>/.gebai-local`（会话/用户数据隔离，
`resources/` 以目录联接指向仓库资源，CV 等能力照常可用）；`GEBAI_SCHEDULER=off`（不跑定时/闲时调度）；
GC 关闭；飞书机器人关闭（两实例同时订阅事件会重复响应）。

### 关键：默认关闭思考模式

该模型是推理型，**思维链可占输出 90% 以上**。实测同一工具调用任务：

| 模式 | 工具调用结果 | token 用量 | 端到端耗时 |
|---|---|---|---|
| 开思考（默认） | `ls({"path":"C:\\Windows"})` | 315 | 13.2 s |
| **关思考** | 同上（**完全一致**） | **27（↓91%）** | **5.0 s** |

关闭方式（脚本默认已带）：

```
GEBAI_LLM_EXTRA_PARAMS={"chat_template_kwargs":{"enable_thinking":false}}
```

复杂推理任务如需思维链，用 `-EnableThinking` 重启，或在前端环境变量面板临时覆盖该变量为空。

### 上下文窗口由推理服务的档位决定

GEBAI 侧的上下文预算**自适应服务端窗口**（脚本启动时探测 `/props` 的 `n_ctx`，取
`min(-MaxContext 期望值, n_ctx − 输出预留)`）——所以**要调大上下文，改的是推理服务的档位**，不是实例参数：

| 推理档位 | 服务端窗口 | 实例预算 | 显存 | 适用 |
|---|---|---|---|---|
| `fast` | 32K | 24K | 15.0 GB | 日常交互（余量充裕） |
| **`long-context`** | **128K** | **120K** | 15.9 GB | 长文档处理（余量仅约 730 MiB） |

```powershell
# 换到 128K：先重起推理服务，再重起实例
pwsh -File infer/scripts/run-server.ps1 -Profile long-context -Background
pwsh -File infer/scripts/run-gebai-local.ps1 -Port 3100 -Stop
pwsh -File infer/scripts/run-gebai-local.ps1 -Port 3100
```

> **128K 是本机实测硬上限**，且瓶颈不是显存（160K 以上显存占用与 128K 几乎相同，但解码崩到 1/4）。
> 完整边界数据见 6.9 节——不要试图用 KV 量化或其它手段突破它。

### 实测体验基线（经 GEBAI SDK 端到端）

```
思维链: 0 chunk        正文: 9 chunk / 97 字符
首字延迟: 4.6 s       总耗时: 5.0 s
```

**能力边界**（据实说明，避免误用）：工具调用格式正确可靠（已验证 `tool_calls` 与参数 JSON），
适合日常问答、单步/少步的工具操作。但它是 3 bit 量化的 35B MoE 本地模型，**复杂多步推理、
长链路任务规划明显弱于云端大模型**；且 llama-server 为单 slot（同时只处理一个请求），
多会话并发会排队。重活请交给云端模型，本地模型的定位是**离线、零配额、数据不出本机**。

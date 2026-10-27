# 裁云片子制作 · 接手须知（新人从这里开始）

> 这是一份**按"你现在要做什么"索引**的文档，不是 changelog。
> 每条都带实测代价——它们不是理论上的坑，是踩过的。

## 要做视觉风格时

examples/caiyun/assets/ 下有两个文件是**长期资产**：

| 文件 | 用途 |
|---|---|
| _kit.js | 复用内核：弹簧/缓动/样条/错峰 + 等轴测与弱透视实体 + 常用图元。**粘到 layout 最前面**即可用 |
| STYLES.md | 13 种视觉风格的配方（材质、配色、运动手法、各自的坑） |
| slot-robot-example.js + .json | **唯一保留的完整示例**：机器人启动（浅底实体 + 时间线点亮 + 关节姿态） |

其余场景脚本一律不入库——需要时照 STYLES.md 的配方现写。

## 我要做的第一件事

| 你要干嘛 | 去看 |
|---|---|
| 做一条新片 | `docs/script_design.md`（剧本模板，**开工必读**） |
| 写 `layout` 脚本 | `docs/caiyun_layout_script.md`（尤其 §6 排查） |
| 判断片子"够不够动" | `docs/caiyun_motion_acceptance.md` |
| 查某个坑 | `docs/PITFALLS.md`（按场景索引） |
| 知道有哪些字段 | `get director schema_json`（自省）或 `json_schema`（标准 JSON Schema） |

## 脚本写错会在启动时就失败（`--strict` 默认开）

字段名拼错、类型不符 —— 这些**曾经是静默忽略的**：照常出片、日志零提示，
而“球没自转”与“写对了但转速是 0”在画面上完全一样。现在默认会拦住：

```bash
caiyun --headless --script x.json --render out.mp4     # 写错 → 退出码 1
caiyun --headless --no-strict --script x.json ...       # 逐字段试效果时关掉
```

报错会直接给路径与建议字段名（`scenes[0].layers[0]` / `model_rot` → `model_spin`）。
**看到退出码 1 就去改那一行**，不要 `--no-strict` 绕过去——那行本来就没生效。

### 给约束解码用：标准 JSON Schema

`get director json_schema` 返回 draft-2020-12 标准 schema（`additionalProperties:false`
+ 嵌套 `items` 引用）。把它喂给 structured outputs / GBNF / Outlines，模型在**生成时**
就写不出错误字段名——比事后校验更早一层。

交叉验证（用**标准**校验器，不是只看自己的单测）：

```bash
python tools/caiyun_schema_crosscheck.py <导出的 schema.json>
```

> 为何需要这个脚本：`schema_to_json_schema` 的 C++ 单测曾**六条全绿**，
> 而标准校验器一跑就发现 `layers` 只写了 `type: array` 却没给 `items`——
> 校验器不会进入元素，图层里的拼写错**永远抓不到**。详见 `PITFALLS.md`〔schema〕。

## 成本纪律：怎么不把一轮创作变成三十轮

> 实测代价：一条 10 秒三维短片花了约 30 轮，其中 **18 轮（60%）是"用渲染去试画面"**——
> 一次 300 帧 1080p 全片 100~140 秒，加一次读图，每猜错一个参数就是一轮。
> 下面的四条把其中大部分可省掉。

### 1. 参数语义不确定时，读源码而不试渲

一个字段"到底怎么算"，`grep` 它的使用处约 30 秒；而"改一下渲一版看看"约 20~140 秒。
**判据：如果你准备为了搞清语义而渲一次，先停下来 grep 那个字段名。**
特别值钱的一类：着色公式（`shade_surface` / `shade_terms_at`）——
它能一举解释"为什么某个参数调上去反而更黑/更平"。

### 2. 先查"已经好看"的官方样例，再动参数

`examples/caiyun/assets/` 下的 `scene-script-demo.json`、`sample-cinematic.json`
是**已验证过观感**的参照物。自己写的东西发灰/发平时，**先 diff 光照与相机参数**，
而不是从零调——实测多次证明：参数看似相近、结果差一个数量级，往往就是某一项偏离了样例。

### 3. 造型试错用极小样本，构图定型后才渲全片

```
造型/材质/光照试错 → 单镜、3~10 帧（约 8~20 秒/次）
构图定型           → 单镜落定帧，抽帧看（不重渲）
动效验证           → 只渲该幕（其他场景 duration 调到 0.1，或单独建小脚本）
最终成片           → 全片仅渲 1~2 次
```
⚠ 小样本时必须让**脚本里的 `size` 与实际渲染尺寸一致**，否则画面会错乱。

### 4. 验收用「量化 + 抽帧」替代「重渲」

```bash
python shuangtian/examples/caiyun/styles/measure_motion.py <frames>   # 3 秒，占静止/变化密度
ffmpeg -ss 3.0 -i out/film.mp4 -frames:v 1 -y check/f3.png            # 抽帧，不重渲
```
数字管"有没有在动"，人眼管"好不好看"——但**两者都不需要重渲一次全片**。

### 反模式

- **为一个动效对不到，渲整片**（应只渲那一幕）
- **在没有量化口径的情况下连调三个参数**（失败时无法归因，已排除项也没记下来）
- **用渲染结果反推参数语义**（应先读源码；渲染只用于"确认观感"）


## 三条最容易撞的（都在 `PITFALLS.md`〔编排〕有完整取证）

### 1. `layout(t, …)` 的 `t` 是**场景内局部时间**

不是全片时间。给"第 7 秒"加东西写成 `if (t > 6.9 && t < 7.1)`
**永远不成立**——第 7 秒在场景 2 里，`t` 只有约 1.4。

**判据**：算局部时间（全片时刻 − 场景起点），或按 `duration` 分派。

### 2. `shapes` 收**屏幕像素**，且**后 push 的在上面**

- `paint_script_shapes` 只乘 `画布宽/1920`（1920 宽时即 1），**相机不参与**。
  写 `120` 就是画面 120px；写 `120/Z` 得到亚像素，看起来像没画。
- shapes 按数组顺序叠。纹路写在底块之前会被整片盖掉，**且不报错**。

### 3. 匀速平移 / 连续旋转**测不到**「在动」

验收口径是 32×18 网格（每格 60px）逐格均值差 <1.2 算静止。
同样多的亮点只挪几像素 ⇒ 格均值几乎不变。

**能测到的**：明暗在跳（亮↔暗切换）、存在感在变（线从 8% 长到 92%）、
位移跨整格且图元稀疏。

## 改 layout 脚本时必须做的两件事

```powershell
# ① 先确认代码真的执行了（洋红探针，或看诊断输出）
$env:CAIYUN_SHAPES_DEBUG="1"
caiyun.exe --headless --script x.json --render /tmp/diag.mp4
# 输出：count / bbox / in_view / kind 分布 / 前 3 个图元
# in_view=0 时会直接打 "⚠ 没有任何图元落在画布内"

# ② 改完必须删旧产物再渲（帧缓存会让你以为没生效）
Remove-Item film.mp4, frames -Recurse -Force
```

**顺序很重要**：先确认"代码有没有跑"，再调数值。
反过来做的话，每一轮都是"改了、渲了、数字没变、不知道为什么"。

## 最有效的手法：整屏亮度脉动（dL 与网格差 1:1）

如果一条片子"动得不够"，**先试这个**，再考虑加图元：

```js
const pulse = 0.5 + 0.5 * Math.sin((t / 0.36) * Math.PI * 2);
shapes.push({ kind: 1, x: 0, y: 0, w: w, h: h,
              color: '#FFFFFF', alpha: 0.006 + 0.026 * pulse });
```

一层覆盖全屏的半透明色，alpha 随时间起伏。深底（luma ~13）上
alpha 0.03 约产生 dL≈7。周期 0.3~0.4 秒是"轻微呼吸"，不是频闪。

**为何它有效（定量依据）**：验收量的是 32×18 网格的逐格均值差，
而**整屏亮度变化 `dL` 与网格差是 1:1** —— 压暗 3 级就是 3.0 的变化。
这是所有手法里最省的：不用堆图元、不改构图、不碰布局。
另两条数值规律（位移=周期整数倍时差为 0、低 alpha 在深底上约等于 `bg×a`）
见 `docs/caiyun_layout_script.md` §6.1.8。

**实测**：歌白片静止占比 88.1% → **62.8%**，一次到位。
而在此之前调了 5 轮淡纹理点阵（alpha/尺寸/速度），只从 88.3% 到 87.0%。

## 尺寸 × alpha 的定量门槛

底色与点色差约 147 luma 时：`Δ ≈ (点面积/3600) × 147 × alpha`

| 点直径 | alpha | 实测 Δ |
|---|---|---|
| 6 | 0.10 | **0.12** ← 差一个数量级 |
| 13 | 0.25 | 1.35 ✓ |
| 20 | 0.10 | 1.34 ✓ |
| 34 | 0.10 | 3.87 ✓ |

跑 `tools/layout_alpha_probe` 可复算。**先按表定参数，再调观感**，不要反过来。

## 调功时要先跑**单段探针**，别全片重渲

```powershell
py scripts/probe_scene.py --script x.json --scene 2
```

它把**除目标场景外的 duration 全置 0**，只渲那一段（几秒），
用与 `ot_bench` **完全相同**的口径量（直接复用同一套网格算法，不另写一份）。

**为什么这条最重要**：本轮两个子会话各花了约 2000 秒，其中绝大部分
只是在等「全片渲染（70s）」这个**根本不必要**的过程——
而"这一改让画面变化了多少"只需要几秒的画面就能回答。

实测对比：全片 955 帧 ≈ 70s；单场景 160 帧 ≈ **5s**。

## 一次完整操作（改 → 验执行 → 量变化 → 看观感）

这是**最小闭环**，四条命令。改 layout 时按这个顺序，每步都有失败时的判据。

```powershell
cd shuangtian
examples\caiyun\assets\x.json       # 要改的片子（下面的 x.json 换成它）

# ── ① 改完先确认**代码真的执行了**（不看过数字先看这一条）
$env:CAIYUN_SHAPES_DEBUG="1"
.\build\dev\bin\caiyun.exe --headless --script x.json --render %TEMP%\diag.mp4
# 看输出：loaded=true ? count 够不够 ? in_view 占多少 ?
#   loaded=false        → 脚本没装上（看 st 日志「装载失败，已降级为纯 JSON」）
#   in_view 远小于 count → 坐标不在画布内（忘了用屏幕像素）
#   打“⚠ 没有任何图元落在画布内” → 同上
#   屏幕上却看不见 → 被后面的图元盖住了（后 push 的在上）

# ── ② 量这一段变化了多少（**5 秒**，不是全片 70 秒）
$env:PYTHONIOENCODING="utf-8"
py scripts\probe_scene.py --script x.json --scene 2
# 看：静止占比（要 <=69）、p95（要 >=5.51）、逐秒 mean（找最静那段）

# ── ③ 数字过了再看观感（抽帧，不要只看数字）
ffmpeg -y -ss 3 -i %TEMP%\diag.mp4 -frames:v 1 %TEMP%\f.png
# 肉眼看：主体挡没挡字、颜色脏不脏、边缘硬不硬

# ── ④ 都过了才渲全片
.\build\dev\bin\caiyun.exe --headless --script x.json --render out\films\x.mp4
```

**为何是这个顺序**：反过来（先渲全片）的话，一次试错就是 70 秒；
而 ①② 合计不到 10 秒。本轮两个子会话共花约 2000 秒，主要就差在这里。

## 出片与验片

```powershell
# 渲染（16 路并行最快；GPU 腿反而更慢，见 render.hpp 的实测表）
$env:CAIYUN_WORKERS="16"
caiyun.exe --headless --script x.json --render out.mp4

# 验片：先用可复算的指标，再抽帧看构图
py scripts/ot_bench.py --script x.json --out out/otbench/x
```

`ot_bench` 的口径：32×18 网格、逐帧差 <1.2 算静止、`>=5.51` 是 p95 门槛。
**注意过亮判据已改为相对参考片比较**（绝对阈值会把亮调片全部判红）。

## 可参考的东西

| 要看什么 | 去哪 |
|---|---|
| **十三个对标案例的索引**（按“我要做什么效果”查） | **`out/otbench/INDEX.md`** |
| 一个完整的、能跑的示例 | `examples/caiyun/assets/slot-robot-example.js` + `.json` |
| 13 种视觉风格的做法 | `examples/caiyun/assets/STYLES.md` |
| 弹簧/样条/实体绘制 | `examples/caiyun/assets/_kit.js` |
| 具体某个效果的制作过程与坑 | `out/otbench/<case>/NOTES.md`（轮次演进 + 实测数字） |

> **片子产物本身不入库**。`out/` 下只有 `otbench/*/NOTES.md` 这类过程记录值得看，
> mp4 与逐帧 PNG 都是可重生成的中间物。
>
> `INDEX.md` 里标了哪些 case 有完整记录（七个）、哪些只有风格配方（六个）。
> 做类似效果前先查它 —— 里面有几条“指标全绿但画面错了”的实测教训，
> 只靠自己调是发现不了的。

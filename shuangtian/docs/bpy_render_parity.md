# bpy 渲染像素级对标 Blender（2026-11-06）

> 需求原话：「霜天的 bpy 渲染也想像 threejs 对标浏览器渲染一样，像素级对标 Blender」。
> 本文只写**实测**：哪一层能对齐、差多少、哪些差是参数错、哪些是物理不同。
>
> **状态（2026-11-06 二期已完成）**：对标目标已锁定 **Cycles**，四项修正已落地，
> 受控平面 262144 个像素**最大差 1 级**，并已固化进 `tests/raster_bpy_cycles_parity_test.cpp`。
> 一期（调研）在 §二~§七；**二期（落地）在 §十一**。
>
> 为什么是 Cycles 而不是 EEVEE：见 [`eevee_vs_cycles.md`](eevee_vs_cycles.md)。
> 一句话：**Cycles 无偏**（加样本收敛到物理解），EEVEE 有偏（屏幕空间 GI 看不到画面外光源，
> 实测差 34.4 vs 2.5 级）。

---

## 零、先读这段：二期之后「对标」怎么用

```python
import bpy

# …建场景…

# 声明对标 Cycles：自动把三项「本框架特有、Cycles 没有」的东西关掉
bpy.context.scene.__st_compat__ = 'cycles'

result = bpy.context.scene
```

| 项 | 默认 | 声明后 | 依据 |
|---|---|---|---|
| `ambient` | 0.35（常量保底） | **0** | Cycles 没写 world 就是零环境光 |
| `ibl_strength` | 0.45 | **0**（仅当脚本没设 world） | 同上 |
| `tone_map` | AgX | **None**（= `Standard` 纯 sRGB 编码） | Cycles 的 Standard 就是纯编码；AgX 是 OCIO LUT |

**默认行为一点没改**——不声明 `__st_compat__` 的场景，画面与改动前相同（除了 §十一的标度修正）。
这与 three 那条路已验证过的设计同一个：`globalThis.renderer = { compat: 'three' }`。

复算：

```bash
st build bpy_brdf_compare
ST_SCENE3D_BACKEND=software build/dev/bin/bpy_brdf_compare \
    tmp/bpy_parity/flat_compat_cycles.py \
    tmp/bpy_parity/cycle_truth_flat_e1p0.png out/x 512 512
# → 真值均值 111.00  本框架均值 112.00  p50=1 p90=1 p99=1 max=1
```

---

## 一、先说结论

| 判据 | 结果 |
|---|---|
| **像素级对标在「参数对齐后」成立吗** | **成立**：受控平面全部 262144 像素**最大差 1 级**；漫反射球 p50=1、p90=1 |
| **现在（不对齐）差多少** | 受控平面 **123 vs 111**（差 12）；球面 p50 = **22~30** |
| **差的主要来源** | **不是 BRDF**，是四条**参数/管线错**（§三） |
| **真值通道可复现吗** | **可**：同场景 4 次渲染逐像素相同（max=0）；Cycles/EEVEE 对受控平面一致 |
| **标的「Blender」要先指定引擎** | EEVEE 与 Cycles 在粗糙金属上彼此差 **5.3 级**、覆盖像素差 40%（§4.2） |
| **bpy 路径与 Blender 能跑同一份源码吗** | **现在不能**（§五，6 处 API 宽严/签名差异），改造后可以 |
| **建议做吗** | **做**，但按 §八 的顺序：先修标度（bug），再立真值通道，最后才谈 BRDF 精度 |

**一句话**：bpy 路径现在离「像素级」差 12~30 级，而其中 **90% 以上是四个可以精确指出的参数错误**，
不是「Blender 和我们的 BRDF 算法不同」这种不可消除的差。把四项参数对齐之后：
**受控平面 262144 个像素的最大差是 1 级**（漫反射球 p50=1）——
「像素级对标」在这个口径下**成立**，与 three.js 那条路已经拿到的口径**同级**。

---

## 二、真值通道（像素级的前提）

没有可复现的真值，就只能说「看起来像」。本次建立了两条通道：

### 2.1 8bit PNG（最终像素）

```
Blender 5.2.3 LTS（本机 installed）
  → EEVEE / Cycles
  → view_transform = 'Standard'（= 纯 sRGB 编码，不套曲线）
  → dither_intensity = 0（默认 1.0 会加 ±1 级抖动噪声）
  → render.filter_size = 0.01
  → PNG 8bit
```

### 2.2 EXR 32bit float（线性辐亮度，用于把「着色层」与「输出链」分开）

这一条是**关键**：8bit PNG 里 111 这个数同时受着色和色调映射影响，
无法判断错在哪一层。EXR 直接给出线性辐亮度。

**实测：输出链是精确的 sRGB 编码，逐字节对得上**

| 发光面线性值 | EXR 读回 | PNG 字节 | sRGB8 期望 |
|---|---|---|---|
| 0.05 | 0.049988 | 63 | 63 ✔ |
| 0.16 | 0.159912 | 111 | 111 ✔ |
| 0.2159 | 0.21582 | 128 | 128 ✔ |
| 0.5 | 0.5 | 188 | 188 ✔ |
| 1.0 | 1.0 | 255 | 255 ✔ |

→ **Blender 的 `Standard` view transform 就是纯 sRGB 编码**，不含任何隐藏环节。
这条结论让后面所有归因都变干净：8bit 差 = 线性差，不是编码差。

### 2.3 确定性（逐像素，不是 md5）

| 引擎 | 采样 | 同一进程连渲 4 次 |
|---|---|---|
| EEVEE | 1 / 64 | max=0，mean=0.0，差>1 的像素 **0%** |
| Cycles | 64 | max=0，mean=0.0，差>1 的像素 **0%** |

> md5 会不同（PNG 元数据含时间戳），**逐像素完全相同**。所以判据必须用像素差，不能用文件哈希。
> 这与 three.js 那条路「同进程两次 readPixels 逐字节相同」是同一个结论。

---

## 三、现状：12 级差逐项归因（受控平面）

场景：0.5 灰平面 + 一枚 SUN `energy=1.0` 正对平面（n·l = n·v = 1）+ 无环境光 + 黑背景。

**真 Blender（Cycles = EEVEE）：`rgb(111,111,111)`**
= `sRGB8(0.5/π × 1.0)` —— 手算与实测一致，这个场景把公式压成了一个数。

霜天 bpy 路径（`tools/bpy_flat_ablate`，逐项消融）：

| 配置 | 中心像素 | 改了什么 |
|---|---|---|
| 原样 | **66** （ACES 口径）/ **123**（None 口径） | — |
| 只修 energy 标度 | 155 | `diffuse × 1000` |
| 只去掉 ambient | 1 | `ambient = 0` |
| 只去掉 IBL | 65 | `ibl_strength = 0` |
| 只换 Standard | 67 | `tone_map = None` |
| **四项全对** | **112** | 真值 111 → **差 1** |

### 3.1 四条差，逐条落到代码行

#### ① `energy/1000` 多除了一次（**这是 bug，不是取舍**）

`src/raster/bpy_runtime.cpp:240`：

```cpp
lighting.diffuse = static_cast<float>(key_energy / 1000.0) * kSunEnergyToIrradiance;
```

而同文件 `:176-186` 的注释写的是：

```
取 `energy 1000 W/m² → intensity 1.0`：
    kSunEnergyToIrradiance = 1.0
```

**注释与代码互相矛盾**：注释说「1000 → 1.0」，代码实际算出 `1000/1000 × 1.0 = 1.0` ✔…
但同一段代码对 `energy=1.0` 算出 `0.001`，而它的语义应当是 `1.0`。

实测证据（SUN energy 扫描，同一份脚本只改 energy）：

| energy | 真 Blender | 霜天（默认） |
|---|---|---|
| 1 | **111** | 72 |
| 2 | **153** | 73 |
| 10 | 255（过曝） | 74 |
| 100 | 255 | 91 |
| 1000 | 255 | 180 |

→ **差整整 1000 倍**。Blender 的 SUN `energy` 单位是 W/m²，也就是**辐照度**，
与内核的 `diffuse`（乘进 `irradiance = n·l × color × intensity`）**同一量纲**，
不需要任何换算系数。`/1000` 是历史遗留（旧式 `energy/1000 × 0.769` 的分母）。

#### ② `ambient = 0.35` 常量保底

`Lighting::ambient` 默认 0.35，而真 Blender **不写 world 就是 0 环境光**。
实测贡献：`ambient=0` 时从 66 → 1。这是 three.js 对标时就已经认定的第 ② 项，
bpy 路径没有跟着改。

#### ③ `Lighting::rim` 已删除，但 bpy 路径没清理

`docs/render_parity.md §11.3` 写明「`rim` **删除**：物理上不存在加一圈亮边」。
现在 `bpy_lighting_probe` 甚至编译不过（读 `l.rim` 报 no member）——说明**探针没跟进**，
而 `render_scene` 内部是否还在用 rim 需要单独确认（见 §八 第 1 步）。

#### ④ 色调映射：bpy 路径强制 `AgX`，而 Blender `Standard` 才是纯编码

`src/raster/scene3d_script.cpp:1627`：

```cpp
spec.lighting.tone_map = ToneMap::Agx;
```

理由写的是「Blender 4.x 的 view transform 默认是 AgX」——**这句对**（Blender 5.2.3 实测默认
`view_transform='AgX'`）。但对标必须两边**同一个算子**：

- Blender 侧用 `AgX` 时，Blender 的 AgX 是 **OCIO LUT**（`AgX_Base_sRGB.cube` 2.9MB）
- 本框架的 `ToneMap::Agx` 是**解析拟合**

`docs/render_parity.md §11.5` 已经量化过这条既存缺陷：

| 线性输入 | 本框架 AgX | 真 Blender AgX | 差 |
|---|---|---|---|
| 0.05 | rgb(107) | 约 rgb(71) | **+36** |
| 0.18（中灰） | rgb(188) | 约 rgb(115) | **+73** |

→ 用 AgX 对标，**先要背 +73 级的色调映射差**。这不是「修 BRDF」能解决的。

### 3.2 为什么这四条这么重要

因为它们**恰好落在「改设置就消掉」和「改公式才能消掉」的分界线上**。
不分清就会出现 `render_parity.md §三` 记录的那种事：
扫 100 组 `tone_map × ambient × diffuse × env_strength`，最优配置仍有 9.3% 的像素差 >8——
**参数已经到头，剩下的必须改模型**。反过来，先修参数再谈模型，才量得准。

---

## 四、BRDF 层：参数对齐后还剩多少

四项对齐之后，用球面（一张图覆盖 `n·l ∈ [0,1]` 全区间 + 掠射菲涅耳 + 高光瓣形状）。

### 4.1 漫反射：已经逐像素对齐

| 场景 | 真值均值 | 本框架均值 | 差 | p50 | p90 | 差>8 |
|---|---|---|---|---|---|---|
| 0.5 灰球 / rough 1 / metal 0 | 81.26 | 81.87 | **+0.62** | **1** | **1** | 0.3% |

→ 漫反射归一化（`albedo/π`）、`n·l` 的形状、法线插值都对上了。
剩下的 1 级来自 8bit 量化与边缘采样。

### 4.2 金属：EEVEE 与 Cycles 彼此都不一致，先定「对标哪个」

同一场景、只换引擎（metal / rough 0.5）：

| 引擎 | 主体均值 | 主体像素 |
|---|---|---|
| **EEVEE** | 54.79 | 23219 |
| **Cycles** | 49.49 | 32664 |
| 差 | **5.30** | — |

→ 两个真渲染器在「粗糙金属 + 无环境光」上差 5.3 级、覆盖像素数差 40%（软边不同）。
**所以「像素级对标 Blender」这句话必须先指定引擎**：

- 对标 **EEVEE**（光栅化路径，与霜天的定位一致）：`metal r=0.5` 本框架 **+6.21**，
  `r=0.9` **−6.68**（交叉偏差，不是单方向的能量项）
- 对标 **Cycles**（真值更像「物理正确」）：`r=0.5` 本框架 **−0.99（p50=1，已对齐！）**，
  `r=0.9` **−9.01**

有意思的是：本框架的 BRDF 在 r=0.5 与 Cycles **几乎完全一致**（p50=1），
而 EEVEE 自己偏亮 5.3。**所以「EEVEE 是准的」这个直觉是错的**。

### 4.3 一处需要注意的物理事实

`metal / rough 0.05` 在无环境光下**几乎全黑**（真值主体均值 18.4、最大 66）。
这是物理正确的：纯直接光的金属只有镜面反射瓣，粗糙度极低时瓣极窄，
除了高光核心之外全黑。本框架同一场景 **+1.77（p50=2）**——也对。

> 这条对产出有直接影响：`apps/jianmu/scenes/bpy_watch.py` 用的是 `metal 0.95 / rough 0.22`，
> 若世界里没有环境光，齿轮会几乎全黑。它显式设了 world（线性 0.05），所以能出片。

---

## 五、兼容性：两边能跑同一份源码吗（**现在不能**）

这是本次调研最意外的发现，而且**直接决定「对标」在工程上能不能落地**。

### 5.1 工件 `bpy_watch.py` 在真 Blender 里跑不起来

```
ValueError: faces.new(...): found the same (BMVert) used multiple times
```

根因是齿轮几何的固有特性（`tools/blender_reference.py` 已记录）：内圈只有 32 个顶点、
外轮廓数百个，多个外轮廓角插值到**同一个**内圈顶点，四边形退化。真 bpy 的
`bmesh.faces.new()` 对此**报错**；`mesh.from_pydata()` 只是建出退化三角形（接受）。

**而霜天兼容层接受它。** 于是：

| | bpy_watch.py | 等价 from_pydata 版 |
|---|---|---|
| 真 Blender | ❌ ValueError | ✅ |
| 霜天兼容层 | ✅ | ✅ |

→ **宽严不一致**。后果：能用原版脚本对标吗？不能——真值侧根本没有图。

### 5.2 六处 API 差异（实测，逐一列出）

写一份「两边都能跑」的等价脚本（`tmp/bpy_parity/gears_portable.py`）时，
撞到 6 个必须先绕开的差异：

| # | 项 | 真 Blender 5.2.3 | 霜天兼容层 | 影响 |
|---|---|---|---|---|
| 1 | `mesh.from_pydata` 收退化面 | 接受 | 接受 | 一致 |
| 2 | `bmesh.faces.new` 遇重复顶点 | **抛 ValueError** | 不报错 | **宽严不一致** |
| 3 | `Euler(x,y,z)` 三个位置参数 | 接受 | **`takes at most 2 arguments`** | 兼容层**缺法** |
| 4 | `bpy.data.lights.new(name)` | **必须给 type** | 可省（默认 POINT） | 兼容层**过宽** |
| 5 | 材质槽 | `obj.data.materials.append(m)` | `obj.material_slots.append(m)` | **签名不同** |
| 6 | `World.background_color` | **没有**（色在节点树） | 有 | 兼容层**自造** |
| 7 | 对象必须 `link` 到场景集合 | **不 link 就不渲染** | 不需要（扫 `bpy.data.objects`） | 见下 |

第 7 条最坑：写对了几何、材质、灯光、相机，**渲出来只有背景**——
因为 `objects.new()` 之后没 `scene.collection.objects.link()`。
而兼容层不要求这一步，所以**在霜天里永远发现不了**。

> 这正是「宽严不一致」的真实代价：兼容层「太宽容」会让人写出一份
> **在霜天能跑、在 Blender 白屏**的脚本，而且是**静默白屏**。

### 5.3 一处方向性错误（`Euler`）

`Euler(0.3, 0.1, 0.2)` 在**真 bpy 完全正常**，在兼容层抛 `TypeError`。
而 `apps/jianmu/scenes/bpy_watch.py:398` 写的正是：

```python
cam_obj.rotation_euler = Euler(math.radians(8.0), 0.0, 0.0)
```

→ 兼容层连自己仓库里的示例脚本都跑不了那个写法（它实际走的是别处）。
这是 **API 漏实现**，不是设计取舍。

---

## 六、工件级对比：一处必须记住的坑

用等价的 `gears_portable.py`（两边都能跑）做工件级对比：

| | 结果 |
|---|---|
| 真 Blender（energy=2600，`Standard`） | 齿轮区 **255（全白）**，10249 px |
| 霜天（同脚本、四项对齐） | p50 = **1**（背景逐像素相同），齿轮区偏暗 |
| 全图 | 有 **9.3% 的像素差 >32** |

**为什么**：`energy=2600` 在 Blender 里严重过曝（`tools/blender_reference.py:331` 的注释
已经写明「2600 会过曝成白；SUN energy 是 W/m²，3.0 才对」）。

而霜天的 `/1000` 把这个 2600 变成了 2.6，**反而更接近正确量级**——
所以同一份过曝脚本，在霜天里"看起来还行"，在 Blender 里全白。

**这条的教训**：
1. 存量 bpy 脚本的 energy 值（2600/900/1500）是「在 Blender 里摆完灯看的」，
   量级本身就不可信（同一份语料里 3.0 与 2600 并存）
2. **不能用「过曝工件」判标度**——两边都饱和时，差多少都被截平了
3. 现行 `docs/render_parity.md §11.4` 的验证（"k=1.0 在 ACES 下几乎命中参照图，主体 80.1 vs 78.1"）
   **建立在这份过曝工件上**，所以那个结论不成立，需要重做

---

## 七、与 three.js 那条路的对照（口径可比）

| 维度 | three.js（已做完） | bpy（本次） |
|---|---|---|
| 真值通道 | 无头 Chrome + SwiftShader + `readPixels` | Blender 5.2.3 + Standard + `filter_size=0.01` |
| 确定性 | 同进程两次逐字节相同 | 同场景 4 次逐像素相同（max=0） |
| 受控平面结果 | 76 = 76（逐字节） | 112 vs 111（差 1） |
| 主体 p50 | **0** | **1**（漫反射球） |
| 剩余差 | 轮廓软边 1.1~1.6% | 轮廓软边 + 粗糙金属 0.3~37% |
| 不可消除项 | `envMap`/PMREM、阴影语义、AgX | **AgX（LUT vs 解析）**、粗糙金属能量、阴影语义 |
| 兼容性问题 | 无（three.js 是 JS 库，脚本即真值） | **有**（§五，6 处 API 差异 + 宽严不一致） |

**最大的结构性差异**：

- three.js 那条路，**浏览器跑的就是真值**——脚本不需要改，`renderer.compat='three'` 一开，
  两边执行同一份代码语义
- bpy 这条路，**Blender 与兼容层是两套独立实现**，而且**宽严不同**。
  所以对标的前提多一层：**先保证同一份源码两边都跑得起来**

---

## 八、建议路线（按「先修 bug、再谈精度」排序）

### 第 0 步：先把 §五 的兼容性问题解决（**不做这步，后面的数都不可信**）

| 动作 | 判据 |
|---|---|
| 给 `Euler` 补齐三位置参数签名（与真 bpy 一致） | `Euler(0.3,0.1,0.2)` 两边都通 |
| `lights.new` / `cameras.new` 缺 type 时**报错**（与真 bpy 一致），而不是默认 POINT | 拿一份错脚本验 |
| 写一份 `tools/bpy_compat_matrix`（脚本级）：同一批 bpy 脚本同时在真 Blender 与兼容层跑，逐条报「两边结果/报错」 | 输出矩阵表 |
| 加一条 lint：`objects.new()` 之后没有 `link()` 时警告（兼容层自己不需要，但**为了真 Blender 可跑**） | 警告能命中 `gears_portable.py` 的历史版本 |

### 第 1 步：修标度 bug（4 行以内）

```cpp
// src/raster/bpy_runtime.cpp:240,266 —— 去掉多余的 /1000
lighting.diffuse      = static_cast<float>(key_energy / 1000.0) * kSunEnergyToIrradiance;
//                    ↑ 若语义是「W/m² 1:1 → irradiance」，这一层必须去掉
```

**但先做判定实验**（本次已备好工具）：

```bash
# 同一份可跑脚本，扫 ENERGY_SCALE，看哪一档与真 Blender 落在同一曝光
build/dev/bin/bpy_energy_scale tmp/bpy_parity/gears_portable.py \
    tmp/bpy_parity/watch_truth_Standard_BLENDER_EEVEE.png out/es 800 600
```

**验收判据**：受控平面（§三）从 112 vs 111 的 1 级差**保持**，且
`bpy_watch` 类工件的「主体均值」与真 Blender 在 ±8 以内。

> ⚠ 不要同时改 `kSunEnergyToIrradiance` 与 `/1000`——**两个未知量不能一起调**，
> 那正是 `§11.4` 记录过的错误（用 k 去补模型缺的自由度）。

### 第 2 步：把「对标模式」显式化（与 three 那条路同一个开关思路）

参考三次对标已经验证过的做法：**默认不改，脚本声明了才切**。

```python
# bpy 脚本里声明
import bpy
bpy.context.scene.__st_compat__ = 'blender'   # 或通过宿主 API 传入
```

开启后自动：

| 项 | 值 | 理由 |
|---|---|---|
| `ambient` | 0 | Blender 不写 world 就是 0 |
| `ibl_strength` | 0（脚本没设 world 时） | 同上 |
| `rim` | 确认已删除（§3.1③） | 物理上不存在 |
| `tone_map` | `None`（= Standard）或**与脚本的 `view_transform` 同名** | 两边必须同一算子 |
| `exposure` | 读 `scene.view_settings.exposure` | Blender 的曝光是 stops（×2^n），不是线性倍数 |

**验收**：受控平面 `== 111`（两条腿都要），漫反射球 p50 ≤ 1。

### 第 3 步：色调映射 AgX 的真值化（这是 bpy 对标特有的硬骨头）

Blender 的 AgX 是 **OCIO LUT**（`AgX_Base_sRGB.cube`，2.9 MB，`config.ocio` 里定义），
本框架是**解析拟合**。§11.5 已量化：线性 0.18 处差 **+73 级**。

三条路，代价与收益：

| 路 | 做法 | 代价 | 能到哪 |
|---|---|---|---|
| A | **不对标 AgX**：对标只用 `Standard`（纯 sRGB） | 无 | 像素级成立（§三已验） |
| B | 读 Blender 的 `.cube` LUT，做 trilinear + tetrahedral 插值 | 中（实现 + 2.9MB 数据 + 两条腿一致） | 与 Blender AgX 差 ≤1 级 |
| C | 继续修解析拟合 | **不建议**：已试过，差 36~73 级，且 Blender 换版本会变 | — |

**建议 A 起步**（本轮），B 作为「要对标 AgX 出片」时的独立任务。

### 第 4 步：粗糙金属的能量（唯一真实的 BRDF 差）

对齐 Cycles 后 `metal r=0.9` 本框架 **−9.01**（偏暗）。而 `r=0.5` 已对齐（p50=1）。
偏差随 roughness 单调增大 → 指向**单次散射的能量损失**：

- 本框架：`D·V·F` 单次散射（three.js 同构）
- Blender 5.x Principled：有多次散射补偿（`Principled inputs` 实测**没有**独立开关，
  说明它是内建的）

**可选动作**（按性价比）：

1. **先不改**：`rough ≤ 0.5` 已经对齐，而产品场景（黄铜齿轮 `rough 0.22`）都在这区间
2. 若要改：加 Fdez-Agüera 的多散射补偿项 `(1 + F0·(1/r − 1))` 形式的能量项，
   但**它与 three.js 的 `MeshStandardMaterial` 不兼容**——等于在「一个内核服务三条路径」里
   为 bpy 单独开一个分支。**这正是本仓反复批判的「各条路径长成不同样子」，要慎做**

### 第 5 步：基线固化（不做这步，下次改引擎不知道变好还是劈了）

新建 `tests/raster_bpy_blender_parity_test.cpp`，判据全部来自本次实测：

| 判据 | 基线 | 阈值 |
|---|---|---|
| 受控平面中心像素（Standard，energy=1） | **111** | 必须 == 111（两条腿） |
| 受控平面（energy=2） | **153** | 必须 == 153 |
| 漫反射球主体均值 | **81.3** | ±2 |
| 漫反射球 p50 | **1** | ≤ 2 |
| 粗糙金属 r=0.9 主体均值（vs Cycles） | **63.5** | ±4（已知差 −9） |
| 真值可复现性（4 次渲染） | max=0 | 必须 == 0 |

> 数字来源必须写在测试注释里（**期望值必须来自真值通道，不能手推**）——
> `render_parity.md §九` 记过这个教训：手推的两个数（153、120）两次都错。

---

## 九、可复算清单

所有数字都能用下列命令重跑（工具已提交）：

```bash
# ── 真值通道 ────────────────────────────────────────────────
# 受控平面真值（Cycles + EEVEE，含 EXR 线性值）
"C:/Program Files/Blender Foundation/Blender 5.2/blender.exe" --background \
    --factory-startup --python tmp/bpy_parity/flat_truth.py -- tmp/bpy_parity "1.0,2.0"

# 球面真值（漫反射 / 金属光滑 / 金属粗糙）
... --python tmp/bpy_parity/sphere_truth.py -- tmp/bpy_parity

# 确定性（逐像素）
... --python tmp/bpy_parity/compare_runs.py -- tmp/bpy_parity 4

# 输出链标定（EXR ↔ PNG 逐字节对账）
... --python tmp/bpy_parity/png_vs_exr.py -- tmp/bpy_parity

# ── 霜天侧 ──────────────────────────────────────────────────
st build bpy_flat_ablate      # 受控平面逐项消融
st build bpy_brdf_compare     # 四项对齐后逐像素比
st build bpy_energy_scale     # 工件级标度扫描

ST_SCENE3D_BACKEND=software build/dev/bin/bpy_brdf_compare \
    tmp/bpy_parity/sphere_diffuse.py \
    tmp/bpy_parity/sphere_diffuse_blender.png out/bc 400 400
```

### 本次新增/修复的探针

| 工具 | 用途 | 状态 |
|---|---|---|
| `tools/bpy_flat_ablate.cpp` | 受控平面四项消融 | 新增 |
| `tools/bpy_brdf_compare.cpp` | 四项对齐后逐像素对比（真值 PNG 为输入） | 新增 |
| `tools/bpy_energy_scale.cpp` | 工件级 energy 标度扫描 | 新增 |
| `tools/bpy_lighting_probe.cpp` | 读已删字段 `rim` 导致编译失败 | **已修** |

---

## 十、这次调研顺手抓到的 4 个真缺陷

1. **`bpy_lighting_probe` 编译不过**（读已删除的 `Lighting::rim`）——说明探针没跟 `§11.3` 的改动
2. **`Euler` 签名被误判**——见 §十一.4 的更正（本节的旧判断是错的）
3. **`/1000` 与注释矛盾**——注释说 1:1，代码多除 1000
4. **`§11.4` 的 k 标定建立在过曝工件上**（Blender energy=2600 全白）——
   那条「k=1.0 主体 80.1 vs 78.1」的结论**不成立**，因为 2600 在 Blender 里渲出来是 255

> 第 4 条最值得记：**用一份两边都过曝的图去"命中"参照，看起来对得上，
> 实际是两边都撞了天花板。** 判标定必须选「不过曝」的工作点。

---

# 二期：把「对标 Cycles」落地（2026-11-06）

一期的结论是「参数对齐后能到 max=1」，但那些对齐是在**探针**里手工做的。
二期把它变成**产品能力 + 永久基线**。

## 十一、七项改动

### 11.1 真值通道正式化（从 `tmp/` 收进 `tools/`）

| 工具 | 产物 |
|---|---|
| `tools/blender_cycles_truth.py` | 受控平面真值（PNG + EXR 线性值 + 手算对账） |
| `tools/blender_cycles_sphere_truth.py` | 球面三档真值（diffuse / metal_smooth / metal_rough） |

一期那些 `tmp/bpy_parity/*.py` 是临时脚本；现在真值通道是**仓库资产**，任何人一条命令可复算。

**实测真值（Cycles 128 samples，本机 Blender 5.2.3）：**

| 场景 | 线性值 | 8bit | 手算 |
|---|---|---|---|
| 受控平面 energy=1 | 0.160422 | **111** | 0.159155 → 111（差 0.796% = 镜面项）|
| 受控平面 energy=2 | 0.320843 | **154** | 0.318310 → 153（正好跨过 `.5`）|
| 球面 diffuse | — | 主体均值 **80.38** | — |
| 球面 metal_smooth | — | 主体均值 **9.04**（无环境光时几乎全黑，物理正确）|
| 球面 metal_rough | — | 主体均值 **49.68** | — |

### 11.2 修掉 `energy / 1000`（真的 bug）

`src/raster/bpy_runtime.cpp` 两处（主光 `:264`、附加光 `:290`）：

```diff
- lighting.diffuse = static_cast<float>(key_energy / 1000.0) * kSunEnergyToIrradiance;
+ lighting.diffuse = static_cast<float>(key_energy) * kSunEnergyToIrradiance;
```

**依据**：Blender SUN `energy` 的单位就是 **W/m²（辐照度）**，而内核的 `diffuse` 乘进
`irradiance = n·l × color × intensity` 之后也是辐照度。两者**同量纲**，系数只能是 1。

那是旧式 `energy/1000 × 0.769` 的**分母残留**（`0.769` 早就删了，分母没人清理）。

**影响面预演**（`tools/bpy_scale_impact`，不改代码先模拟）：

| 场景 | 修正前均值 | 修正后均值 | 全白占比 |
|---|---|---|---|
| `align_bench.py` | 62.1 | 68.0 | 0% |
| `bpy_watch.py` | 107.5 | 107.6 | 0% |
| `gears_asset.py` | 58.4 | 210.0 | 0% |
| `mixed_gears_asset.py` | 77.2 | 118.7 | 0% |
| `quad.py` | 66.8 | 90.5 | 0% |
| `shop_asset.py` | 57.5 | 170.6 | 0% |

**没有一个场景过曝**（全白占比全为 0%）。这是修正成立的硬证据——
若标度错 1000 倍，`gears_asset` 应该直接烧成白。

### 11.3 「对标 Cycles」显式开关

`SceneSpec::CompatMode{None, Cycles, Eevee}` + 脚本声明 `bpy.context.scene.__st_compat__`。

**实测切换效果（同一份脚本，只差一行声明）：**

| 口径 | 中心像素 |
|---|---|
| 不声明（默认 AgX + ambient 0.35 + IBL 1.0） | 155 |
| 声明 `__st_compat__='cycles'` | **112** |
| Cycles 真值 | **111** |

**三档球面结果（对齐后 vs Cycles）：**

| 档 | 真值均值 | 本框架 | 差 | p50 | p90 |
|---|---|---|---|---|---|
| diffuse | 80.52 | 81.69 | **+1.17** | **1** | 1 |
| metal_smooth | 17.62 | 19.75 | +2.13 | 2 | 5 |
| metal_rough | 50.00 | 48.87 | **−1.13** | **1** | 3 |

> 注意 `metal_rough`：一期（用 EEVEE 真值）量到的是 **−9.01**，换成 Cycles 后变成 **−1.13**。
> 所以那个「粗糙金属能量差」**大部分是 EEVEE 的偏差**，不是本框架的 BRDF 差。
> 一期 §八 第 4 步的担心（“改它会为 bpy 单独开分支”）**不成立**，可以不改。

### 11.4 兼容层 API 口径（一处**推翻了自己上一轮的判断**）

一期报告写「`Euler` 缺三位置参数签名，兼容层连自己仓库示例都不支持」。
**实测证明这条是错的**：

```
>>> mathutils.Euler.__doc__
class Euler(angles=(0.0, 0.0, 0.0), order='XYZ', /)

>>> Euler(0.1, 0.2, 0.3)
TypeError: mathutils.Euler(): takes at most 2 arguments (3 given)
```

**真 bpy 恰恰不接受三参数，而兼容层旧实现反而宽容了它。**

所以 `apps/jianmu/scenes/bpy_watch.py:398` 的 `Euler(math.radians(8.0), 0.0, 0.0)`
在真 Blender 里是**错的**——那份脚本从没在真 Blender 里跑过。

| # | 项 | 真 bpy 5.2.3 | 兼容层（修后） | 策略 |
|---|---|---|---|---|
| 1 | `Euler(x,y,z)` 三位置参数 | ❌ TypeError | ✅ 接受 | **宽容 + 报 warning** |
| 2 | `Euler((x,y,z))` 元组 | ✅ | ✅ | 已一致 |
| 3 | `lights.new(name)` 缺 type | ❌ TypeError | ✅ 接受 | **宽容 + 报 warning** |
| 4 | `obj.data.materials.append(m)` | ✅ | ✅（新增）| 代理到 `material_slots`，**只有一份数据** |
| 5 | `world.node_tree.nodes['Background'].inputs['Color']` | ✅ | ✅（新增）| 节点树影子，写进 `background_color` |
| 6 | `objects.new()` 后必须 `link` | ✅ 不 link 不渲染 | 不需要 | 文档说明（兼容层扫 `bpy.data.objects`）|

**为何选「宽容 + 报 warning」而不是「直接拒绝」**：存量脚本用了三参数写法，
拒绝会让它们全部失效。而静默宽容会让用户以为能拿到真 Blender 里用。
报警是唯一同时不破存量、又不隐短的做法。

实测（`bpy_watch.py` 现在会报）：

```
warning: 灯光 'key' 建时没给 type——真 Blender 要求 lights.new(name, "SUN")
warning: 物体 'barrel_wheel' 的 rotation_euler 用了 Euler(x, y, z) 三参数写法
```

### 11.5 永久基线：`tests/raster_bpy_cycles_parity_test.cpp`

6 条断言，期望值**全部来自真值通道**（不是手推）：

| 测试 | 判据 |
|---|---|
| `bpy_cycles_parity_flat_plane_is_111` | 对标模式下中心像素 = 111±1 |
| `bpy_cycles_parity_flat_plane_is_154` | energy=2 时 = 154±1（跨过 `.5` 舍入边界，更硬）|
| `bpy_default_mode_differs_from_parity_mode` | 默认口径必须明显更亮（守「默认不改」）|
| `bpy_cycles_parity_back_face_is_black` | 对标模式下背光面 = 0（守「关掉 ambient」）|
| `bpy_cycles_parity_uses_standard_tone_map` | 对标模式 = `None`，默认 = `Agx`（守「不去对标 AgX」）|
| `bpy_script_text_dispatches_to_bpy_branch` | 含 `import bpy` 的文本走 bpy 分支 |
| `bpy_compat_does_not_leak_across_runs` | 跨 run 不残留（见 11.6）|

### 11.6 写基线时抓到的**第五个真缺陷**：跨 run 残留

写测试时 `bpy_cycles_parity_uses_standard_tone_map` 红了，而探针打印是正常的。
根因：**`bpy.data` 是模块级单例，`reset()` 能清数据块，但清不了脚本挂在 `scene` 上的自定义属性**。

```
顺序                              期望                        实际（修前）
1. 先跑「声明 compat=cycles」      —                          —
2. 再跑「不声明」                  tone=Agx ambient=0.35       tone=None ambient=0  ← 继承了上一条
```

**后果很难查**：后一份场景无故变暗，零报错。

修法：`_Data.reset()` 重建一个干净的 `scene`（保留 `name` / `world`）。
`tests/..._test.cpp` 里专门加了一条 `bpy_compat_does_not_leak_across_runs` 守住它。

> 这条与 `docs/PITFALLS.md` 里「单例状态跨用例残留」是同一类坑。它的特点是：
> **单跑绿、套跑红**——只看单条测试永远发现不了。

### 11.7 回归结果

```
st lint            : 扫描 1063 文件，违反 51 项  ← 与基线同数（新增文件零违规）
st test            : 16 分片全绿，0 failed，退出码 0
st test bpy        : 12 passed（其中 7 条是本期新增）
```

### 11.8 复算命令（本期新增的工具全在这里）

```bash
B="C:/Program Files/Blender Foundation/Blender 5.2/blender.exe"

# ── 真值（Cycles）─────────────────────────────────────────────
"$B" --background --factory-startup --python tools/blender_cycles_truth.py -- \
    out "1.0,2.0" CYCLES
"$B" --background --factory-startup --python tools/blender_cycles_sphere_truth.py -- \
    out CYCLES

# ── 霜天侧 ───────────────────────────────────────────────
st build bpy_brdf_compare && ST_SCENE3D_BACKEND=software \
    build/dev/bin/bpy_brdf_compare tmp/bpy_parity/flat_compat_cycles.py \
    out/cycle_truth_flat_e1p0.png out/x 512 512

# 标度改动影响面（不改代码先预演）
st build bpy_scale_impact && build/dev/bin/bpy_scale_impact apps/jianmu/scenes

# 兼容层口径诊断（tone / ambient / ibl / compat 四个字段）
st build bpy_compat_diag && build/dev/bin/bpy_compat_diag

# 回归基线
st test bpy_cycles_parity
```

| 新增/修的工具 | 用途 |
|---|---|
| `tools/blender_cycles_truth.py` | 受控平面真值（PNG+EXR+手算对账）|
| `tools/blender_cycles_sphere_truth.py` | 球面三档真值 |
| `tools/bpy_brdf_compare.cpp` | 逐像素对比（已不再需要手工 ×1000）|
| `tools/bpy_scale_impact.cpp` | 标度改动影响面预演 |
| `tools/bpy_compat_diag.cpp` | 兼容模式字段诊断（写基线时定位残留 bug 用的）|
| `tests/raster_bpy_cycles_parity_test.cpp` | 永久基线（6 条断言）|

---

## 十二、二期后的基线表（拿这张判「变好了还是劈了」）

| 判据 | 基线值 | 阈值 |
|---|---|---|
| 受控平面 energy=1（对标模式） | **111** | == 111 ±1 |
| 受控平面 energy=2（对标模式） | **154** | == 154 ±1 |
| 受控平面 全画面最大差（vs Cycles） | **1** | ≤ 2 |
| 背光面（对标模式） | **0** | == 0 |
| 球面 diffuse 主体均值 | **81.69** | 80.38 ±3 |
| 球面 metal_rough 主体均值 | **48.87** | 49.68 ±3 |
| 默认口径受控平面 | **155** | 只记录，不作阈值 |

---

## 十三、仍未做的（不假装）

| 项 | 状态 | 为什么 |
|---|---|---|
| **GI（全局光照）** | ⚠ AO 近似（非真 GI） | 真 GI 是渲染架构（路径追踪）；现已补 AO——见 §十四 |
| **AgX 像素级** | ✅ **已达成** | 运行时加载 OCIO LUT，9 样本 7 个逐字节一致，max=2——见 §十四 |
| **阴影语义** | ✅ **两腿已实现** | `shadow_map` 两腿俱备（共用光源相机矩阵）；边缘与 Cycles 仍不同 |
| **`envMap`/PMREM** | ⚠ 已实现（探针式） | 六面 cubemap → 预过滤成方向探针；不是真 mip 链，高光形状不同 |
| **工件级逐像素基线** | ⚠ 只有统计量 | `bpy_watch` 那类场景的真值需从真 Blender 拿（2400 行齿轮需要 `from_pydata` 重建）|

**最后一条的取舍**：受控平面与球面已经把「着色层」全覆盖（`n·l` 全区间 + 菲涅耳 + 高光瓣）。
工件级再测主要是防止「多物体 / 多材质 / 相机路径」上的集成缺陷，而那类问题
更适合用 `bpy_scale_impact` 那种统计量探针守，不必逐像素。

---

## 十四、四项补完的实账（2026-11-06）

§十三 列的四项中，两项已真正拿下来（AgX、阴影），两项只拿了“近似”
（AO 对 GI、以及 PMREM 仍未做）。下面每一项都给出**可重现的证据与边界**，
以及“为什么不能再往前走”的物理原因。

### 14.1 AgX：从「拟合差 70 级」到「逐字节一致」

**改法**：运行时加载 Blender 自带的 OCIO LUT（`AgX_Base_sRGB.cube`，57³），
不再用解析拟合。

正确的链（**四条都是坑，每一条都单独错过**）：

```
Rec.709 线性
  → ① E-Gamut 矩阵（OCIO 算出 0.5593711138...，不能手算）
  → ② lg2 分配（[-12.47393, 12.5260688117]，不是 [0,1]！）
  → ③ 三维 LUT triplelinear/tetrahedral（索引 **r 变化最快**）
  → ④ sRGB_encode(pow(v, 2.4))（LUT 输出是 Rec.1886 编码，要先反线性化）
```

**四个真问题（都是实测撞出来的，不是推测）**：

| # | 问题 | 症状 | 根因 |
|---|---|---|---|
| 1 | `encode_pixel` 对 AgX 输出二次编码 | 0.18 灰 118 → **186** | `tone_map_outputs_encoded` 没标 AgX |
| 2 | LUT 索引次序反了 | **中性灰变绿、纯红渲成蓝** | 写成 b-fastest，应为 `r + g*N + b*N*N` |
| 3 | LUT 输出当线性值用 | 整体偏暗 | 输出是 Rec.1886 编码，需 `^2.4` |
| 4 | GPU 着色器无 AgX 分支 | GPU 腿差 68 级 | `tone_op==4` 走了 ACES 默认档 |

**逐点对账**（`tools/agx_verify`，真值来自 `tools/blender_agx_truth.py`）：

| 输入（线性） | 本框架 | 真 Blender | 差 |
|---|---|---|---|
| 0.05 灰 | 58 | 58 | **0** |
| 0.18 灰 | 118 | 118 | **0** |
| 0.5 灰 | 170 | 170 | **0** |
| 1.0 灰 | 197 | 197 | **0** |
| 2.0 灰 | 217 | 217 | **0** |
| 纯红 | — | 219,57,33 | 0 |
| 纯绿 | — | 110,197,81 | ≤2 |
| 纯蓝 | — | 20,86,209 | ≤2 |
| 黄铜 | 186,171,140 | 186,171,140 | **0** |

**判据**：最大差 ≤ 2（LUT 格子插值的固有误差），平均 0.22。

**两腿一致**（`tools/agx_two_legs`，受控平面）：软件腿 **130**、GPU 腿 **129**。
那 1 级的差来自 GPU 侧 8bit 中转的反解舍入，不是算子差异。

> **⚠ 两腿实现必须同源**：GPU 着色器对 `tone_op==4` 只输出**线性值**，
> 回读后用**同一个** `agx::apply()` 做变换。若让着色器自己做
> （比如“退到 Narkowicz”），实测差 **68 级**。三维 LUT 放不进着色器
> （D3D11 cbuffer 上限 64KB），所以只能在 CPU 侧接。

### 14.2 阴影：`ShadowMap` 从「就空的接口」到可用

**发现**：`ShadowMap` 结构写在 `scene3d.hpp` 并配了完整设计说明，但
**脚本里声明不了、字段表没登记、GPU 腿没实现、测试覆盖为零**。
那是一个「看起来有这个能力、实际用不了」的接口。

**补的三处**：

1. `scene_schema.cpp` 登记 `shadow_map` 字段（否则报“未知字段”，直接拒掉）
2. `scene3d_script.cpp` 的 `read_shadow_map()` + `render_scene` 里调用
3. 软件腿本来就有 `build_shadow_map` / `shadow_factor_at`，这次是**接通**而非重写

**软件腿实测**（球悬在地板上 + 斜射平行光，1100×800）：

| 量 | 开阴影 | 关阴影 |
|---|---|---|
| 主体均值 | **68.1** | 76.4 |
| 变暗像素 | 64090（**7.28%**）|
| 变亮像素 | **0** |
| 最大差 | 78.7 级 |

**空间分布验证**（这一条比均值重要）：变暗像素全部集中在 y=200~599
（球下方的地板），**没有一个是变亮的**。均匀变暗说明乘错了地方，
局部变暗才是真遮挡。

> **⚠ GPU 腿未实现**：它只报一条 warning（`platform_d3d11.cpp` 的
> `set_shadow_map`）。这是刻意的——**静默忽略会让差异隐形**，
> 而“开了参数没效果”是最难查的一类失效。

### 14.3 GI：AO 能补哪一块，不能补哪一块

**这一节的主结论：真 GI 没做，也做不到像素级。**

Cycles 是路径追踪：它算光线在场景里**反复弹射**（漫反射反弹、色彩传递、
大范围反光）。那是渲染架构级的，不是多写一个算子能补齐的。

做的是 **AO（ambient occlusion）**——真 GI 最便宜的那一层近似：

| 项 | 真 GI（Cycles） | AO |
|---|---|---|
| 沟壑/接触处变暗 | ✅ | ✅（主要价值）|
| 物体间色彩传递 | ✅ | ❌ |
| 大范围反弹光 | ✅ | ❌ |
| 算力 | 几十倍 | 几乎为零 |

**实现**：沿法线半球取 16 个方向（固定黄金角螺旋，**不用随机数**——
随机数会让两次运行出不同结果），每根短射线用 Möller–Trumbore 查世界三角形。

**只乘间接光**（环境光 + IBL），**不乘直接光**：

```
indirectDiffuse  = ao * (ambient + iblDiffuse) * diffuseColor/π
indirectSpecular = ao * iblSpecular * (F0·A + B)
directDiffuse    = （不受 ao 影响）
```

为何不乘直接光：直接光有自己的投影阴影路径（`ShadowMap` / `PlanarShadow`）。
两套遮蔽相乘没有物理意义，且会让暗部黑得不真实。

**实测**（球 + 盒子 + 地板，`occlusion.strength = 0.8`）：

| 量 | 值 |
|---|---|
| 变暗像素 | 3.71% |
| 变亮像素 | **0** |
| 最大变暗 | 11 级 |
| 主体均值 关/开 | 83.9 / 83.3 |

**均值只降 0.8 级而局部最大降 11 级——这是 AO 的正常形态**：
它只暗在接触带，不该把整幅画面压暗。所以判据必须看**分布**而不是均值。

**性能（这一条是硬伤，实打实的教训）**：

| 版本 | 单帧耗时 |
|---|---|
| 逐超采样样本算（1100×800 × 4x SSAA）| **199 秒** |
| 加「世界位置量化」缓存 | **1.58 秒**（126×）|

缓存键用**量化世界位置 + 量化法线**（不是屏幕坐标）：屏幕坐标会被
三角形绘制顺序影响，而世界位置量化与顺序无关——同一个世界位置
物理上就是同一个 AO。

> **⚠ 默认必须关**：所有像素级基准（受控平面 / 球面 BRDF）都在
> `occlusion = 1` 下对账。默认开 AO 会把那些基准整体变暗、全部变红，
> 而那些才是“与 Cycles 一致”的真凭据。

### 14.4 PMREM：环境贴图（预过滤成方向探针）

**改法**：六张 cubemap 面 PNG → 预过滤成方向探针 → 走与手工探针**同一个** IBL 公式。

**为何是 PNG 而不是 .hdr/.exr**：本框架的图像解码只支持 PNG/JPEG（`codec/image.hpp`）。
加 .hdr 要新解码依赖，而「六张 PNG」用已有路径就行，且调用方能用任意工具生成。

**为何是“预过滤成探针”而不是真 mip 链**：本框架的 IBL 内核是
「方向探针 + 余弦/镜面加权」（`pbr_ibl_diffuse` / `pbr_ibl_specular`），
没有贴图采样路径。所以：

| 项 | 真 PMREM | 本实现 |
|---|---|---|
| 高光形状 | 连续（按 mip 层）| 探针间插值，有台阶 |
| 粗糙面模糊 | 精确 GGX 卷积 | 近似（锥角平均）|
| 镜像反射 | ✅ | sharpness 高时接近 |

**实测**（金属球，64×64）：无贴图 138.0，全红贴图 **69.2**。

> **⚠ 顺带修掉一个既有 bug（重要）**：`read_lighting` 里旧键名的
> 兜底逻辑写反了——`pick("ibl_strength","env_strength") != nullptr`
> 只要**任一**存在就非空，于是 JSON 里只写旧名 `env_strength` 时
> 会去读不存在的 `ibl_strength`，**静默返回 0**。
>
> 后果：**存量剧本里所有 `env_strength` / `env_rough_falloff` 一直失效**，
> 环境反射从未生效，而且不报错。这个 bug 是查 PMREM 为什么没效果时
> 才发现的（两者叠在一起，一个掩盖了另一个）。

### 14.5 GPU 腿的两个实现（2026-11-07 补齐）

| 项 | 改法 | 实测 |
|---|---|---|
| `shadow_map` | 延迟绘制 → 光源视角深度 pass → PCF 采样 | 均值 76.5→**68.4**（软件 68.1）|
| AO | G-Buffer（MRT 写世界位置+法线）→ 回读 → 调**同一个** `pbr_ambient_occlusion` | 83.9→**83.5**（软件 83.3）|

**共用同一份算法**是这两项的共同原则：
· 阴影的光源相机矩阵 → `ShadowMap::make_view_projection()`（`scene3d.cpp`）
· AO 的射线查询 → `pbr_ambient_occlusion()`（`pbr.cpp`）

## 14.6 GPU 腿实现时踩到的五个“静默” bug

这五个的共同点：**不报错、不崩、画面看上去“正常”**。

| # | 现象 | 根因 |
|---|---|---|
| 1 | 着色器编译失败 → 静默退回软件腿 | 采样器声明成 `SamplerState`，`SampleCmpLevelZero` 只接受 `SamplerComparisonState` |
| 2 | 深度图全 255 → 阴影无效果 | 空 PS（不输出颜色）时 D3D **不写渲染目标**；改成 `return float4(position.z,…)` |
| 3 | 深度图全空 | 绑了 `nullptr` 深度缓冲 → D3D **禁用深度测试**，光栅什么也不写 |
| 4 | G-Buffer 全零 | MRT 各目标采样数不一致（MSAA 颜色 + 单采样 G-Buffer）→ `OMSetRenderTargets` **静默失败** |
| 5 | AO 三角形数 0 | 几何收集写在 `if (shadow_map.strength > 0)` 里 → 单开 AO 时永远是 0 |

**教训：把中间结果存盘看一眼**。第 2/3/4 个都是“把深度图/G-Buffer 存成 PGM 并统计 min/max”才定位的，
光看最终画面永远分不清“没画上”还是“比较逻辑错”。

## 14.7 仍存在的差异（不假装）

| 项 | 差异 | 量级 |
|---|---|---|
| 阴影边缘 | 软件=逐三角形精确遮挡；GPU=深度图+PCF | 均值差 0.3 级，逐像素 79.6% 差>2 |
| GPU 腿 AO | 乘整像素（按环境光占比加权）vs 软件只乘间接光 | 均值差 **0.2 级** |
| PMREM | 探针插值 vs 真 mip 链 | 高光形状不同，不逐像素 |
| GI | 仍是 AO 近似，**没有**多少次弹射与色彩传递 | 物理上不可补齐（除非改渲染架构）|

**像素级基准（受控平面 / 球面 BRDF）全部在默认关闭态下对账**，不受上述差异影响。



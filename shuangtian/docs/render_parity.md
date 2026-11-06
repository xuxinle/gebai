# 与浏览器**像素级对标**：three.js 同构着色

> 需求原话：「霜天渲染 threejs 要和浏览器渲染像素级对标」。
> 本文只写**实测**：哪一层对齐了、差多少、哪一层必然差、怎么复算。
> 日期 2026-11-06。前置：`three3d.md`（three.js 当表达层）、`color_pipeline.hpp`（输出链）。

## 一、先说结论

| 判据 | 改造前 | 现在 |
|---|---|---|
| 受控平面（0.5 灰 + 单平行光 n·l=1） | 176（浏览器 76） | **76 = 76**（逐字节相等） |
| quad 场景主体逐像素平均差 | **60.08**/255 | **0.49**/255 |
| 同上的 p50 / p90 | 45 / 126 | **0 / 0** |
| 差 > 16 的像素 | 88.3% | 1.1%（全在轮廓软边） |
| 差 > 8 的像素 | 92.2% | 1.6% |

**主体内部已经逐字节相同**（p50 = p90 = 0）。剩下的差**全部**在轮廓边缘，
原因是采样方式不同（见 §六）。所以：

> 「像素级对标」在这个口径下**已经成立**：同采样下内部逐像素相等；
> 要连边缘也相等，见 §六的开关。

## 二、真值通道（这是“像素级”的前提）

没有真值就没有像素级，只有"看起来像"。参考环境原来的通道**不可复现**
（`tools/reference_page.js` 依赖的 `three_browser.js` / `reference.html` 已不在仓库），
所以重建了一条自包含的：

```
无头 Chrome（--disable-gpu → SwiftShader CPU 光栅，确定性）
  → 页面内 THREE.WebGLRenderer.render()
  → gl.readPixels() 取原始 RGBA（不经过窗口合成 / 色彩管理 / DPI 缩放）
  → POST 回本地 Bun 服务 → 写 PNG
```

| 项 | 实测 |
|---|---|
| three 版本 | **r160**（页面内 `THREE.REVISION` 回读） |
| GL | WebGL 2.0 (OpenGL ES 3.0 Chromium) |
| 同进程两次 `readPixels` | 逐字节相同（确定性成立） |
| 可切换的 renderer 配置 | `toneMapping` ×6、`toneMappingExposure` |

工具：`tmp/parity-20261106/harness.mjs`（通道）、`pixel.mjs`（取点）、`analyze.mjs`（逐像素分析）。

## 三、改造前：差在哪（逐项消融，不是推的）

受控场景把公式压成一个可手算的数：一个朝向相机的平面、一盏正对相机的平行光
（n·l = n·v = 1）、0.5 灰、`metalness=0`、`roughness=1`、无环境光、黑背景。

真 three.js r160 的代数（`third_party/three/three.cjs` 的 ShaderChunk 逐字）：

```
diffuseColor  = color × (1 − metalness)          = 0.5
irradiance    = saturate(n·l) × lightColor       = 1.0        (scaleFactor = 1，r160 非 legacy)
directDiffuse = irradiance × diffuseColor/π      = 0.15915
directSpecular≈ 0.0032（roughness=1 时 GGX 瓣极宽，但有 F0=0.04）
────────────────────────────────────────────────
outgoing ≈ 0.1624 → (NoToneMapping) → sRGB 8bit = 76          ← 实测 76 ✔
```

改造前的霜天：

```
scale = ambient + n·l×diffuse + rim = 0.35 + 1.0 + 0 = 1.35
lit   = 0.5 × 1.35 = 0.675 → ACES(Narkowicz) → 0.806 → sRGB = 176   ← 实测 176 ✔
```

### 逐项差

| # | 项 | three.js | 改造前 | 效果（0.5 灰面） |
|---|---|---|---|---|
| ① | **漫反射归一化** | `albedo/π` | `albedo × diffuse` | **差 2.5~3.1 倍**，是 76→176 的主因 |
| ② | **常量环境保底** | 没 `AmbientLight` 就是 0 | `ambient = 0.35` 恒在 | 背光面 **0 → 84** |
| ③ | **rim（边缘光）** | 不存在 | `rim = 0.25` | 轮廓一侧 +25% |
| ④ | **自推环境反射** | 纯色 `background` **不产生 IBL** | 从 `background` **推导** `env_sky`、`env_ground = sky×2.6`、强制 `env_strength = 0.45` | 金属凭空多一层反射 |
| ⑤ | **ACES 曲线** | 完整 ACESFilmic | Narkowicz 近似 | 逐点 **6~14 级** |
| ⑥ | G 项 | `V_GGX_SmithCorrelated` | Schlick-GGX | 高光形状/尾巴 |
| ⑦ | F 项 | `exp2((-5.55473c-6.98316)c)` | `(1-c)^5` | 掠射角反射率 |
| ⑧ | `roughness` 下限 | `max(r, 0.0525) + geometryRoughness` | `max(r, 1e-3)`，无几何项 | 光滑面高光大小 |
| ⑨ | 采样 | `antialias:false` → 1 样本 | 软件腿 3× 超采样 + box | 边缘像素不可能逐字节相同 |

消融实测（`align_render` 加光照覆盖参数，逐档跑）：

| 配置 | 中心像素 |
|---|---|
| 全默认（改造前） | **176** |
| ambient=0 / diffuse=0 / rim=0 / env=0 | 3 |
| ambient=0.35，其余 0 | **84** ← 常量保底一项就是 84 |
| +diffuse=1.0 | **176** ← 漫反射一步跳 92 级（缺 1/π） |

ACES 两条曲线的差（同一线性输入）：

| 线性 | Narkowicz | ACESFilmic | 差 |
|---|---|---|---|
| 0.05 | 59 | 50 | +9 |
| 0.159 | 132 | 118 | **+14** |
| 0.30 | 177 | 165 | +12 |
| 0.2159 | 154 | **141** | **+13** |
| 1.00 | 232 | 226 | +6 |

→ 即使着色完全对齐，**色调映射这一层还会系统性偏亮 6~14 级**。

### 调参能到哪（证明这不是调参问题）

扫 `tone_map × ambient × diffuse × env_strength`（100 组）：

| 配置 | 主体平均差 | 差>8 占比 |
|---|---|---|
| 改造前默认（ACES / amb .35 / diff 1.0 / env .45） | 11.21 | 18.9% |
| **扫出的最优**（amb 0 / diff 0.83 / env .45） | **5.39** | **9.3%** |

把 `ambient` 调到 0 才勉强压住（等于承认环境项本来是错的），且 9.3% 的像素仍差 >8。
**参数已经到头，剩下的必须改模型。**

## 四、做法：three.js 同构

新增 `include/st/raster/three_parity.hpp` + `src/raster/three_parity.cpp`，逐式对照
`third_party/three/three.cjs` 的 ShaderChunk：

```
diffuseColor   = color × (1 − metalness)
specularColor  = mix(vec3(0.04), color, metalness)        // = F0，F90 = 1
alpha          = max(roughness, 0.0525)²                   // + geometryRoughness（本实现取 0）
D = a² / (π · ((n·h)²(a²−1)+1)²)
V = 0.5 / max(n·l·√(a²+(1−a²)(n·v)²) + n·v·√(a²+(1−a²)(n·l)²), ε)
F = f0·(1−Fc) + F90·Fc,   Fc = exp2((−5.55473·v·h − 6.98316)·v·h)
irradiance     = saturate(n·l) × lightColor（已含 intensity）
directSpecular += irradiance · D·V·F
directDiffuse  += irradiance · diffuseColor/π
indirectDiffuse+= ambientIrradiance · diffuseColor/π
outgoing        = saturate(前三项) + emissive
```

配套：

| 项 | 改动 |
|---|---|
| 色调映射 | 新增 `ToneMap::AcesThree` = three 的完整 ACESFilmic（input 矩阵 → RRTAndODTFit → output 矩阵，含 `/0.6`） |
| 模型选择 | `Lighting::Model{Legacy, Three}`，**默认 Legacy** |
| 光源颜色 | 新增 `Lighting::light_color` / `ambient_color`（Legacy 不读，零影响） |
| 脚本入口 | `globalThis.renderer = { compat: 'three' }` 或 `scene.compat = 'three'` |
| 清屏色 | 对齐模式下用脚本的 `scene.background`（而不是调用方传的背景） |

**对齐模式关掉的三样**（都是“本框架特有、three 没有”的）：
① `ambient = 0.35` 常量保底 → “没写 `AmbientLight` 就是 0”；
② `rim = 0.25` → 0；
③ 从 `scene.background` 推导的环境反射 → 关闭。

> **为什么必须显式声明**：关掉这三样会让背光面**真的全黑**。那是物理正确的，
> 但会改变存量出片的观感。所以默认保持 Legacy，脚本声明了才切。

## 五、改造后的实测

| 场景 | 主体平均差 | p50 | p90 | p99 | 差>8 | 差>16 |
|---|---|---|---|---|---|---|
| **受控平面**（96²） | **0** | 0 | 0 | 0 | 0% | 0% |
| **quad**（1100×800，10 材质，22.4% 主体） | **0.49** | **0** | **0** | 20 | 1.6% | 1.1% |
| quad（GPU 腿） | 0.64 | 0 | 0 | 21.7 | 1.9% | 1.2% |

绝对值逐字节相等的证据：受控平面霜天 `rgb(76,76,76)` = 浏览器 `rgb(76,76,76)`；
强度 2.0 时两边都是 `rgb(106,106,106)`。

剩下的差集中在**轮廓软边**（8×8 网格里差异最大的几格全是构件边缘）：

| 项 | 现状 | 原因 |
|---|---|---|
| 主体内部 | **p50 = p90 = 0**（逐字节相同） | 已经对齐 |
| 轮廓边缘 | 差 20~120，占 1.1~1.6% 像素 | 采样方式不同（见 §六） |
| 亮部（>198） | −1.2 ~ −1.6 | 边缘像素计入主体 + `geometryRoughness` 取 0 |

## 六、要连边缘也逐字节相同

必须**两边同采样**。浏览器侧 `antialias:false` 是每像素 1 个样本；而：

- 软件腿固定 `kBake = 3`（每个逻辑像素 9 个子样本 + box 降采样）；
- D3D11 腿走 MSAA。

三条可选路线，代价从低到高：

1. **对标时关超采样**：给 `Scene3D` 加一个 `set_supersampling(1)`，只在 `compat:'three'`
   的验证路径开。改动最小，且不碰默认行为。
2. 浏览器侧开 MSAA 再比较（不可控：SwiftShader 的样本位置不是规范化的，跨驱动还会变）。
3. 接受边缘差：像素级只承诺“内部逐像素相等 + 边缘在采样噪声内”。（当前口径）

§五的数字就是第 3 条路线的结果。

## 七、途中抓到的三个真缺陷

### 7.1 D3D11 腿完全忽略 `tone_map`（已修）

`platform_d3d11.cpp` 的像素着色器里写死：

```hlsl
lit = saturate(lit);                                                 // ← 先截断
hdr = saturate((hdr*(2.51*hdr+0.03))/(hdr*(2.43*hdr+0.59)+0.14));    // ← 永远 Narkowicz ACES
```

实测（同场景同参数，只切后端）：

| 后端 | tone=0 (None) | tone=4 |
|---|---|---|
| software | rgb(13,18,25) | rgb(4,6,12) |
| **gpu**（修前） | rgb(4,6,12) | rgb(4,6,12) ← 逐像素相同 |
| **gpu**（修后） | rgb(18,24,32) | rgb(4,7,13) |

两个错：① `tone_map` 是**死设置**；② 先 `saturate` 再映射会把 HDR 高光截平。
修法：常量缓冲加 `g_tune2.x` = `ToneMap` 枚举，分支用与 C++ 侧相同的常数；
`tone_map` 的分支放在 `saturate` 内部（各算子自己做 clamp）。

### 7.2 清屏色与实体用了两个算子（已修）

`begin_frame()` 用**默认** ACES 清屏，而 `set_lighting()` 总在它之后才换算子
（那是 `Scene3D` 的契约）。于是一帧里背景与实体两套算子。

实测（背景 0.5 灰、tone = None，期望 sRGB 188）：实际清出 **206** ——
那正是 ACES 作用于 0.5 的结果。背景占画面 77%，一亮就是全图差十几级，
而单看实体完全正常，极难定位。

修法：`set_lighting` 发现算子变了且**还没画过东西**时重清一次。

### 7.3 `clear_color` 的契约（我踩了一次，已记）

`Surface::clear` / `Scene3D::begin_frame` 的参数是「**0~255 表示的线性值**」——
内部做 `clear_color.r / 255` 当线性分量，再走 `encode_pixel`。
我第一版传了**已编码**的值，等于编码两次：背景从期望 63 跑到 **164**。

## 八、仍然“必然不同”的部分（不假装）

| 项 | 差异原因 | 能不能消掉 |
|---|---|---|
| 轮廓边缘像素 | 采样方式不同（3× 超采样 vs 1 样本） | 能，见 §六 |
| `geometryRoughness` | three 用屏幕空间导数补偿面片法线，本实现取 0 | 部分能（需引入 dFdx/dFdy 等价物） |
| `envMap` / PMREM | 本框架**没有环境贴图管线** | 不能（功能缺失，不是精度问题） |
| 阴影 | three = PCF 采样；本框架 = 平面投影阴影 | 不能（语义不同） |
| 背景的 ±1 级 | `begin_frame` 的 8bit 中转（0.05 落在 12/13 台阶之间） | 能（若改成浮点清屏） |
| AgX | three 的 AgX 是完整 3D LUT + 自定义 look；本框架是解析拟合 | 不能（精度层级不同） |

## 九、怎么复算

```bash
# 1. 真值（无头 Chrome + SwiftShader + readPixels）
cd tmp/parity-20261106
bun harness.mjs --scene scenes/quad-parity.js --out out/quad-browser.png \
    --width 1100 --height 800 --tone NoToneMapping

# 2. 霜天侧（同一份脚本；脚本里已声明 compat:'three'）
ST_SCENE3D_BACKEND=software build/dev/bin/align_render.exe \
    tmp/parity-20261106/scenes/quad-parity.js out/quad-fw.png 1100 800 0

# 3. 逐像素对比（分位 / 分桶归因 / 空间热点）
bun analyze.mjs out/quad-fw.png out/quad-browser.png

# 4. 回归断言（把浏览器实测值钉住）
st test three_parity
```

`tests/raster_three_parity_test.cpp` 里的三条硬数字 **76 / 106 / 141** 全部来自
浏览器实测。教训写在那里：**期望值必须来自真值通道，不能手推**——我第一版手推了
两个数（153、120），两次都错。

## 十、基线（改引擎后用这些数字判断“变好了还是劈了”）

| 判据 | 基线值 | 阈值 |
|---|---|---|
| 受控平面中心像素 | 76 | 必须 == 76 |
| 强度 2.0 的中心像素 | 106 | 必须 == 106 |
| 背光面 | 0 | 必须 == 0 |
| ACESFilmic(线性 0.2159) | 141 | 必须 == 141 |
| quad 主体平均差 | 0.49 | < 2 |
| quad 差>16 占比 | 1.1% | < 3% |
## 十一、内核统一与 bpy 重标（2026-11-06）

上面十节做的是「让 three.js 脚本能对齐浏览器」。这一节记录把它变成**唯一内核**
之后的三件事：改名、bpy 适配、以及一个必须分开看的既存缺陷。

### 11.1 `three_parity` → `pbr`：为什么改名

原文件叫 `three_parity.hpp`，会让人以为「只有 three 路径走它」。实际上它现在是
**三条输入路径共用的唯一内核**：

| 输入 | 谁负责翻译 | 内核 |
|---|---|---|
| three.js 脚本 | `ThreeRuntime` | `pbr_shade` |
| bpy 脚本 | `BpySceneExtractor` | 同一个 `pbr_shade` |
| JSON 剧本 | `scene3d_script` | 同一个 `pbr_shade` |

改名后「内核只有一套公式」在命名上就是显然的——本仓反复出现的错误正是
「各条路径各自长一副样子」（两条腿、三条路径）。

### 11.2 内核补了什么（为了能承担 bpy）

**① 三种灯光类型**。`extract.py` 会导出 `SUN`/`POINT`/`SPOT`，旧 Legacy 模型是
「主光一套公式 + 点光/聚光当成附加漫反射项」，于是点光**没有高光**、颜色口径也不同。
现在三种都折成同一个 `PbrLight`（方向 + 辐照度），BRDF 只有一个入口。

**② 多盏灯**。`bpy_watch.py` 有 key/fill/rim 三盏灯，旧实现只算主光——画面会
「不够立体」但不报错，属于最难定位的那类。现在 `ShadeInput::extra_lights` 走
同一个 `pbr_direct_light`。

> **实测缺陷**：加多灯光时 GPU 腿只写了一份常量缓冲就读，而**光颜色没地方放**
> （旧着色器把 `g_light.xyz` 当方向用了）。当时误写成 `g_light.rgb`，结果 GPU 腿
> 输出 `rgb(0,0,76)`——红绿通道拿到的是光方向的 x/y（≈0），蓝通道拿到 z（≈1）。
> 单看任一条腿都不报错，只有并排比才看得见。

**③ IBL**。旧模型从 `scene.background` **推导**环境反射（`env_ground = sky×2.6`、
强制 `env_strength = 0.45`）——那是「凭空多一层」。现在是方向探针 + split-sum 的
解析近似：

```
漫反射 E(n) = Σ w_i·c_i·max(0,dot(n,dir_i)) / Σ w_i        （余弦加权）
高光   S(R) = Σ w_i·c_i·max(0,dot(R,dir_i))^sharp(r) / Σ w_i
强度因子 = F0·A + B                                        （Lazarov 解析拟合）
sharp(r) = 1 + (1−r)²·63
```

没有 PMREM 那样几十 MB 的贴图与 mip 链，但两条腿能算出**逐像素相同**的数——
这是本框架选解析式而不是 LUT 的硬理由。

> **归一化只能做一次**：软件腿曾在「无显式探针但 `ibl_strength > 0`」时自己合成
> 上/下两个探针，而 GPU 腿没合成 → 同一个剧本两条腿差出一整层环境光。
> 现在由 `Lighting::make_ibl_probes`（`scene3d.cpp`）统一归一，两条腿都读它。

### 11.3 `env_*` → `ibl_*` 与 `rim` 的删除

| 旧 | 新 | 语义变化 |
|---|---|---|
| `env_sky` | `ibl_sky` | 从「反射向量 Y 分量的天空色」变成「朝上的环境辐照度」 |
| `env_ground` | `ibl_ground` | 同上（朝下） |
| `env_strength` | `ibl_strength` | 从「在反射结果上乘的强度」变成「IBL 能量倍数」 |
| `rim` | **删除** | 物理上不存在「加一圈亮边」；真实轮廓亮边是 IBL 在掠射角的菲涅耳 |
| `Lighting::Model` | **删除** | 只剩一套公式，没有可切的东西 |

**JSON 键名两个都收**（新名优先）：`assets/models/` 里 9 个资产写的是旧名，
不能让内部改名就让它们静默失效。9 个资产已迁移到新名。

**裁云的 `light_rim` 没有丢弃，而是重接**：它映射成一盏从背后来、偏冷、低强度的
平行光（摄影里的 rim light），强度上限 0.6——超过就不再是「轮廓」而变成
「第二个主光」（实测会把背光面从黑抬成中灰）。走的是内核已有的直接光路径，
不新增任何着色分支。

### 11.4 bpy 主光强度重标（`kSunEnergyToIrradiance`）

旧式是 `energy/1000 × 0.769`。三个问题叠在一起：

1. `0.769` 是**为补偿旧模型缺 1/π 而反推出来的**（当时只有一个自由度）；
2. 标定用的是**三盏灯**的场景去拟合**只算一盏灯**的模型——用一个未知量去补
   模型缺失的自由度，得到的 k 必然被撑大；
3. 换成物理内核后再套它，三灯合计 irradiance 到 9.5（远超 1.0），直接过曝。

现在按**量纲**定义，不拟合观感：

```
kSunEnergyToIrradiance = 1.0     // energy 1000 W/m² → intensity 1.0
```

锚点是 Blender 点光的默认能量 1000W（有据可依，不是拟合出来的）。

**验证**（`bpy_watch.py`，同一张真 Blender 参照图）：

| | 主体均值 | 整图均值 | 暗部占比 |
|---|---|---|---|
| 真 Blender | 78.1 | 75.7 | 6.7% |
| 本框架（k=1.0，ACES） | **80.1** | 82.6 | 2.3% |
| 本框架（k=1.0，AgX） | 127.9 | 132.7 | 0.0% |
| 旧式（k=0.769π，单灯） | 55.2 | 60.3 | 98.9% |

k=1.0 在 ACES 下几乎命中参照图（主体 80.1 vs 78.1）。**AgX 那一行的偏差不是
本系数的问题**，见下。

### 11.5 一个必须分开看的既存缺陷：`ToneMap::Agx` 偏亮

`tools/tone_map_probe` 量到的对照：

| 线性输入 | 本框架 AgX | 真 Blender AgX | 差 |
|---|---|---|---|
| 0.05 | rgb(107) | 约 rgb(71) | **+36** |
| 0.0706 | rgb(128) | 约 rgb(78) | **+50** |
| 0.18（中灰） | rgb(188) | 约 rgb(115) | **+73** |

它**与本次内核统一无关**（`color_pipeline.cpp` 本轮未改动），已登记进
`docs/BACKLOG.md` 的 P1。

**关键是别让它污染标定**：如果拿 k 去补偿 AgX 的偏亮，等 AgX 修好之后这边又会
偏暗，变成两个错误互相抵消——那比单个错误更难查。所以 k 只按物理量纲定，
AgX 单独修。

### 11.6 基线（含 bpy）

| 判据 | 基线值 | 阈值 |
|---|---|---|
| 受控平面中心像素 | 76 | 必须 == 76（两条腿都是） |
| 强度 2.0 的中心像素 | 106 | 必须 == 106 |
| 背光面 | 0 | 必须 == 0 |
| ACESFilmic(线性 0.2159) | 141 | 必须 == 141 |
| quad 主体平均差 | 0.49 | < 2 |
| quad 差>16 占比 | 1.1% | < 3% |
| bpy `bpy_watch` 主体均值（ACES） | 80.1 | 真 Blender 78.1 ±8 |
| bpy 两条腿主体均值 | 150.1 = 150.1 | 必须相等 |

复算 bpy 那两行：

```
st build align_bench_probe
build/dev/bin/align_bench_probe.exe examples/jianmu/scenes/bpy_watch.py out.png
build/dev/bin/render_compare.exe out.png blender_reference.png
```


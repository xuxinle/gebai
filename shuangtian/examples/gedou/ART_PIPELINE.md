# 角色美术 —— AI 立绘生产流水线

> 这份文档记录**怎样把一个动漫风 SDXL 模型变成可用的游戏角色素材**，
> 以及这条路上试错出来的全部结论。核心答案只有一句：
> **ControlNet 必须与底模同源**，而提示词永远控制不了精确姿势。

## 一、结论先行

| 方案 | 画质 | 姿势 | 是否可用 |
|---|---|---|---|
| 纯提示词（同一 seed） | ★★★ | ✗ | 17 个动作约 7 个能用 |
| 纯提示词（多种子挑选） | ★★★ | ✗ | 约 9 个能用，靠人挑 |
| SDXL + Canny ControlNet | ★★★ | ✗ | 线稿被当成"画面里的黑块" |
| SD1.5 + OpenPose ControlNet | ★☆ | ✓ 准 | 姿势对了，但画质不够、常出畸形 |
| SDXL + Union(pose) | ★★★ | ✗ | 骨架线**直接被画进画面** |
| **SDXL + Illustrious 专用 OpenPose** | ★★★ | ✓ 准 | **✅ 就是它** |

**根因**：通用 ControlNet 是在**写实图**上训练的，动漫模型理解不了那种约束形态。
换成 `Illustrious-XL ControlNet Openpose`（与底模同源）后，第一次就画准了。

## 二、最终采用的方案

### 2.1 依赖清单

| 组件 | 文件 | 位置 |
|---|---|---|
| 底模 | `PetalRain亚洲动漫-Illustrious_V2.safetensors` (6.46 GB) | `C:\Users\Administrator\Downloads\` |
| ControlNet | `openpose_s6000.safetensors` (2386 MB) | 复制到 `build/cn-ill/diffusion_pytorch_model.safetensors` |
| ControlNet 配置 | 从 `build/cn-union/config.json` 借用（同为 SDXL ControlNet，结构一致） | `build/cn-ill/config.json` |
| SDXL 配置源 | 歌白自带 `sdxl-turbo` 目录（**只用目录结构，不读权重**） | `resources/models/imagegen/sdxl-turbo` |

> ControlNet 权重来源：civitai 模型 `Illustrious-XL ControlNet Openpose`
> （modelId 1359846），36,467 次下载。直链通过 civarchive 找到，
> 从 hf-mirror 下载：`windsingai/openpose/openpose_s6000.safetensors`。

### 2.2 流水线

```
make_poses.py          17 个动作的骨架关节坐标（局部空间）→ poses.json + 调试图
      ↓
make_openpose.py       骨架坐标 → COCO-18 彩色骨架图（OpenPose 输入格式）
      ↓
sdxl_ill.py            底模 + Illustrious OpenPose ControlNet → 生成立绘
      ↓
cutout_char.py         泛洪抠图（白底 → 透明）
      ↓
pack_actions.py        统一缩到 640 高 → examples/gedou/assets/actions/
      ↓
（构建时 b::embed 内嵌进二进制）
```

一键跑全套：

```bash
py -3 tools/make_poses.py
py -3 tools/make_openpose.py --poses build/char-gen/poses/poses.json
py -3 tools/sdxl_ill.py --characters donut,husheng --control-scale 1.0 --outdir build/char-gen/final
py -3 tools/cutout_char.py build/char-gen/final/*.png --outdir build/char-gen/final-cut
py -3 tools/pack_actions.py
st build gedou
```

### 2.3 推荐参数

| 参数 | 值 | 为什么 |
|---|---|---|
| `control-scale` | **1.0** | 0.7 姿势偏软、1.4 会把骨架线画进画面 |
| `steps` | 28 | 24 已可用，28 细节更好，再多收益很小 |
| `cfg` | 5.0 | 高 CFG 会"用力过猛"，压低 ControlNet 信号 |
| 尺寸 | 832×1216 | SDXL 原生比例，竖构图适合全身立绘 |
| 单张耗时 | 12~18 s | RTX 4080 SUPER / fp16 / 带 ControlNet |

## 三、OpenPose 骨架图的两个硬要求

这两条错一个，ControlNet 就完全不认。

### 3.1 18 个关键点的**顺序与颜色**必须严格对上

OpenPose 靠**像素颜色**反推"这是左肩还是右肘"。颜色错了，模型会把左手当右手。

```python
COCO_COLORS = [
    (255,0,0), (255,85,0), (255,170,0), (255,255,0), (170,255,0), (85,255,0),
    (0,255,0), (0,255,85), (0,255,170), (0,255,255), (0,170,255), (85,0,255),
    (0,0,255), (85,0,255), (170,0,255), (255,0,255), (255,0,170), (255,0,85),
]
```

顺序（COCO-18）：
```
0 鼻 1 颈 2 右肩 3 右肘 4 右腕 5 左肩 6 左肘 7 左腕
8 右髋 9 右膝 10 右踝 11 左髋 12 左膝 13 左踝 14 右眼 15 左眼 16 右耳 17 左耳
```

### 3.2 必须补一条**躯干连线**

COCO-18 的官方 `limbSeq` 里**没有"颈→髋"这一条**（它靠"颈→两髋"两条斜线隐式表达）。
只画标准 limbSeq 时，骨架图上下半身是断开的——渲染出来像"两根腿飘在空中"。

**修法**：手动补 `颈→左髋`、`颈→右髋` 两条斜线。

### 3.3 游戏关节 → COCO-18 的映射

游戏骨架只有 13 个关节，且**没有 "neck"**（颈的位置就是 `chest`）：

```python
JOINT_MAP = {
    "chest": 1,        # 颈（⚠ 不是 "neck"，游戏里没这个键）
    "head": 0,         # 鼻
    "shoulder_f": 5, "elbow_f": 6, "hand_f": 7,     # 前方臂 = 左侧
    "shoulder_b": 2, "elbow_b": 3, "hand_b": 4,     # 后方臂 = 右侧
    # 左右髋由 hip 中点 + 膝盖方向推出
    "knee_f": 12, "foot_f": 13,
    "knee_b": 9,  "foot_b": 10,
}
```

## 四、动作库的提示词经验

即使有 ControlNet，提示词仍要**几何化**——它负责"画什么风格"，ControlNet 负责"摆什么姿势"。

### 4.1 不要用武术术语

| 写的 | 画出来 | 改成 |
|---|---|---|
| `throwing a straight punch` | **弯腰驼背** | `right arm extended straight forward horizontally` |
| `powerful hook punch` | 站姿，没挥拳 | `arm swung horizontally across the body` |
| `special attack pose` 里的 swing | **荡秋千**（真画了绳子和板） | `arm across chest` |
| `both forearms raised in front of face` | **举拳欢呼** | `both fists held at chin level` |

动漫数据里 `punch` 常和"前倾发力/打沙包"一起出现，模型就照着画了。

### 4.2 每条都带 `standing upright`

模型很爱画侧身前倾，不写就歪。躺地动作（`knockdown`）除外。

### 4.3 动作词放**最前面**

CLIP 只保留 77 token，从头截断。动作写后面会被整段截掉——**姿势完全失控，
而且不报错**。`sdxl_actions.py` 里加了 token 长度自检，超了会警告。

### 4.4 负面提示要压住"多出东西"

SDXL 对"一个人"的约束很弱，`multiple people` 单独压不住。实测补充：

```
small person, chibi, doll, rope, hanging,           # 冒小人/吊绳
extra limbs, extra legs, extra arms, mutated hands,  # 多手多脚
background scenery, gradient background, pattern,    # 背景不纯（影响抠图）
barefoot, sandals, long hair                          # 角色一致性漂移
```

## 五、抠图：不能用"按亮度判透明"

第一版按亮度阈值抠白底，结果**皮肤和白鞋全被抠掉**——脸上出现黑洞、鞋消失。
根因是"白"与"浅色"在数值上分不开。

**正确做法是泛洪法**：从图像四边出发 BFS，只把**与边框连通的白色**当背景。
角色内部的白色（眼白、白鞋、白条纹）被深色描边包围、不连通，因此保得住。

边界那一圈再做**反混合**去白边：

```
α = 1 - min(R,G,B)/255
F = (O - (1-α)·255) / α
```

这样边缘像素既半透明、又不带白毛边，贴在深色背景上不会"发亮"。

## 六、游戏内的呈现

### 6.1 立绘与矢量分工

`fighter_art.*` 是 2D 矢量绘制（逐关节画），`char_portrait.*` 是 AI 立绘。
现在的规则：

| 状态 | 画法 |
|---|---|
| 所有 17 个动作 | **立绘**（每个 `MoveId` 一张） |
| 立绘缺失时 | 回退矢量（保底，不会白屏） |

`ActionSprites::get()` 按 `MoveId` 直接索引数组（不用 map 查表，60Hz×2 角色下省开销）；
某动作缺图时回退 `idle`，连 `idle` 都没有才判定整张不可用。

### 6.2 尺寸必须与骨架对齐

立绘是整体贴图，矢量是逐关节画，**两者必须同高**，否则会跳。

- 立绘高度 = 骨架的 `foot.y - (head.y - head_radius)`
- 矢量头半径按头身比归一：`(span + r) / (2r) = 7` ⇒ `r = span / 13`

（早先按 `head_radius × 6.4` 算立绘高度，只有正确值的 55%，人物像小人偶。）

### 6.3 夜景里要加"舞台光"

立绘是白底渲染的明亮动漫图，直接放在深色舞台上会**陷进背景**，轮廓读不出来。
在脚下画一圈暖色径向光晕（`Paint::with_gradient(Gradient::radial(...))`）。

注意这是**加光晕**，不是给贴图调亮度——后者会让颜色失真。

### 6.4 朝向镜像

`DrawOptions` 没有水平翻转字段，所以在加载时**预存一份镜像副本**
（一张额外贴图约 640KB），绘制路径保持普通。比"每帧逐像素镜像"或"改框架加字段"都便宜。

## 七、踩过的坑总表

| 现象 | 根因 | 修法 |
|---|---|---|
| 角色完全不见 | 蒙皮网格多乘一次 `node.transform`（3D 时期） | 蒙皮网格用单位阵 |
| 骨架线被画进画面 | 通用 ControlNet + 动漫模型 | 换**同底模**的 ControlNet |
| 姿势完全不受控（不报错） | `control_mode` 参数 diffusers 不支持 | 用单一模式专用 ControlNet |
| 全身变米白 | 分区按 `y` 判高度，实际模型是 **Z-up**（`y` 是深度） | 改按 `z` 判 |
| 三个种子都在鞠躬 | `standing upright` + `arms at sides` 在动漫数据里对应鞠躬 | 给具体手部动作（插兜） |
| 举拳欢呼当防守 | `forearms raised in front of face` 被理解成举手 | 用 `fists at chin level` |
| 画成荡秋千 | `swing` 的字面义 | 改 `arm across chest` |
| 脸上黑洞、鞋消失 | 按亮度抠白底 | 改**泛洪法** |
| 人物像小人偶 | 立绘高度按 `head_radius × 6.4` | 量骨架实际跨度 |
| 切画法时人物突然变大 | 矢量头半径按判定盒尺寸（偏小） | 按头身比归一 `r = span/13` |
| 立绘在夜景里发暗 | 白底图对比深背景 | 脚下加径向光晕 |

## 八、工具清单

| 脚本 | 用途 | 何时用 |
|---|---|---|
| `make_poses.py` | 17 个动作的骨架坐标 + 调试图 | 改动作姿势时 |
| `make_openpose.py` | 骨架 → COCO-18 彩色骨架图 | 每次改完 poses |
| `sdxl_ill.py` | **当前最佳路线**：Illustrious + OpenPose | 正常出图 |
| `cutout_char.py` | 泛洪抠图 | 出图后 |
| `pack_actions.py` | 缩放到 640 高入库 | 抠图后 |
| `make_sheet.py` | 拼联络表（同动作多候选并排） | 人工挑图时 |
| `check_actions.py` | 自动质检（内容占比/底部着地） | 批量筛图 |
| `sd15_openpose.py` | SD1.5 + OpenPose（画质不够，**保留作对比**） | 想验证控制是否生效 |
| `sdxl_union.py` | SDXL + Union（姿势不受控，**淘汰**） | — |
| `sdxl_controlnet.py` | SDXL + Canny（姿势不受控，**淘汰**） | — |
| `sdxl_multi.py` | 纯提示词多种子（画质好但姿势不准，**备用**） | 没有 ControlNet 时 |
| `diag_union.py` / `sweep_union.py` | 诊断 ControlNet 是否生效 | 怀疑权重没加载 |

> **淘汰的脚本保留着**是有意的：它们是"这条路为什么走不通"的证据。
> 下次有人想再试 Canny 或 Union，先看 `diag_union.py` 的输出再决定。

## 九、当前素材状态

- `examples/gedou/assets/actions/` —— 34 张（17 动作 × 2 角色），共 5.13 MB
- 全部由 Illustrious + OpenPose 生成，姿势与 `pose.cpp` 的骨架一一对应
- 入库尺寸 640 高（游戏内角色约 260 世界像素，2x 供高 DPI）

## 十、还能改进的地方

1. **每个动作只有一张**，动作之间靠游戏里切图，没有中间帧。要真正的连贯动画，
   得按骨架的每个关键帧各出一张（图量 ×3~5）。
2. **角色一致性靠"同一段角色描述 + 固定 seed"**，不是严格的角色锁定。
   严格要求需要 IP-Adapter 或训练 LoRA。
3. **户圣偶尔多一件外套**（`husheng-idle` 等），是 SDXL 的随机漂移。
   想修就单独重跑那几张、换种子。
4. **`build/cn-ill/config.json` 是从 Union 版借的**。结构验证过（844 张量 / 0 缺失 /
   0 多余），但严格来说应该从原模型作者处取。

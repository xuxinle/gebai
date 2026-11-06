# 建木（jianmu）—— 三维建模与演示工作台（应用）

一个常驻的三维工作窗口：**改脚本 → 画面立刻变**，并且可以旋转、缩放、平移、悬浮、点选。

两种脚本**同权**：

| 扩展名 | 表达层 | 擅长 |
|---|---|---|
| `.js` | three.js | 组装/层级/材质变体/动画/实例化（“在引擎里搭场景”） |
| `.py` | bpy（Blender 建模 API） | 精确建模：bmesh 逐顶点、布尔/倒角/挤出、参数化几何 |

名字取意《山海经》的建木（“众帝所自上下”）——它是**通道**：一边建模、一边演示。
旧名 `threelab` 只说了 three.js 那一半，所以改名。

两种脚本产出**同一个 `raster::ThreeScene`**，所以下游（相机/拾取/高亮/动画/两条渲染腿）
完全同源——分派只在“脚本 → 场景”这一段（`ThreeView::load_script_file_kind`）。

## 跑起来

```bash
st build jianmu
./build/dev/bin/jianmu                  # 有窗口（默认读 apps/jianmu/scenes/）
./build/dev/bin/jianmu --dir=我的场景    # 换目录
./build/dev/bin/jianmu --scene=quad.py   # 直开某个脚本（裸主名也能匹，如 --scene=quad）
```

然后编辑 `apps/jianmu/scenes/*.js` 或 `*.py`，保存即可看到变化（文件监听默认开）。

> ⚠ 目录里的场景名**带扩展名**：同名两种写法（`quad.js` / `quad.py`）是常见情形
>（就是要对比两条路），用裸主名会让清单里出现两个 "quad"，选中哪个没法分辨。

## 这个示例检验什么

四件事缺一条就用不了：

| 能力 | 为什么是硬要求 |
|---|---|
| **热重载** | 否则每改一行都要重启，编辑循环断掉 |
| **交互** | 否则只是个"会旋转的图片"，判断不了模型对不对 |
| **错误可见** | 否则改坏一行就白屏，人会以为程序崩了 |
| **状态自检** | 否则"为什么没反应"只能靠猜 |

前三条是产品能力，第四条是**可验证性**。所以视口把 `objects` / `picked` /
`selected_name` / `hover_events` / `click_events` / `error` 全暴露成属性，
控制通道与测试都能直接读——不必靠截图猜。

## 场景脚本怎么写

### three.js（`.js`）

**就是普通 three.js 代码**，不需要任何本框架的 API：

```js
const scene = new THREE.Scene();

const camera = new THREE.PerspectiveCamera(42, 16/9, 0.05, 100);
camera.position.set(0, 1.15, 4.4);
camera.lookAt(0, 0.35, 0);
scene.add(camera);

scene.add(new THREE.AmbientLight(0x8899bb, 0.55));
const key = new THREE.DirectionalLight(0xffffff, 2.4);
key.position.set(3, 4.5, 3.5);
scene.add(key);

const ball = new THREE.Mesh(
  new THREE.SphereGeometry(0.62, 48, 32),
  new THREE.MeshStandardMaterial({ color: 0xd09050, metalness: 0.9, roughness: 0.18 }));
ball.name = 'metal_ball';       // ← 名字会成为拾取结果与状态栏标签
ball.position.set(-0.78, 0.68, 0);
scene.add(ball);

scene;                          // ← 最后一句就是场景
```

三条契约（与 `docs/three3d.md` 一致）：

| 项 | 规则 |
|---|---|
| 返回值 | 最后一个表达式应是 `Scene` / `Group` / `Mesh`，或定义全局 `scene` |
| 相机 | 放在场景里或不放都行；**用户拖拽的视角优先于脚本里的相机**（见下） |
| 能写什么 | 完整 JS：循环、函数、条件 |

内置三个示例：`01-product.js`（产品展示）、`02-generative.js`（参数化几何）、
`03-materials.js`（PBR 参数对照表）。

### bpy（`.py`）

**就是普通 bpy 脚本**（Blender 的 Python 建模 API），同样不需要本框架的 API：

```python
import bpy
from mathutils import Euler

bpy.ops.mesh.primitive_cube_add(size=1.4, location=(-2.7, -0.9, 0.0))
obj = bpy.context.active_object
obj.name = 'brass'
obj.rotation_euler = Euler(0.42, 0.62, 0.0)

mat = bpy.data.materials.new('brass_mat')
mat.diffuse_color = (0.72, 0.55, 0.24, 1.0)
mat.metallic = 0.95
mat.roughness = 0.22
obj.material_slots.append(mat)

# 相机/灯光同 bpy 习惯：bpy.data.cameras.new / bpy.data.lights.new
```

四条契约：

| 项 | 规则 |
|---|---|
| 返回值 | 最后一个表达式给一个 `str`（场景名）或留空都行；**不返回 `Scene` 不报错** |
| 坐标系 | bpy 是 **Z-up**（three.js 是 Y-up），转换已在抽取层做完 |
| 色调映射 | bpy 来源默认 **AgX**（与 Blender 4.x 默认一致），three 来源默认 None |
| 能写什么 | MicroPython 子集 + `bpy.data` / `bpy.ops` / `bmesh` / `mathutils`（见 `docs/python.md` 的已知边界） |

内置 bpy 示例：`quad.py`（金属度/粗糙度阶梯，与 `quad.js` 逐参数同构，
用来对比两条表达层）、`bpy_watch.py`（机械腕表轮系，bpy 的能力分水岭）。

### 两条表达层怎么选

| 要做的事 | three.js | bpy |
|---|---|---|
| 一个球浮在底座上 | 好写 | 好写 |
| 按模数生成 60 齿齿轮 | 要手算顶点 + `Shape` 挖孔 | `bmesh` 逐顶点，与 Blender 语料同构 |
| 倒角、布尔、挤出 | 没有 | `bpy.ops` / 修改器 |
| 布局、层级、动画、实例化 | 好写 | 要手算 |

典型的**混合工作流**：bpy 产资产（几何）→ three.js 组装（摆位、材质变体、相机）。
资产侧的导出工具是 `tools/bpy_export_asset.cpp`（产出 JSON 资产），
再用 `tools/inline_assets.py` 内联进组装脚本——见 `docs/render_alignment.md`。

## 操作

| 操作 | 效果 |
|---|---|
| 左键拖拽 | 绕目标点旋转（俯仰夹在 ±89°） |
| 滚轮 | 推拉距离（等比缩放） |
| 右键 / 中键拖拽 | 平移目标点 |
| 悬浮 | 高亮该物体 + 左下角显示名字 |
| 单击 | 选中（更明显的高亮 + 标签变成"选中 · 名字"）；点空白处取消 |
| 双击 | 回到初始视角 |

**脚本里的相机只作为初始参考**：一旦用户拖过视角，改脚本不会把视角重置
（否则"改一下颜色结果镜头跳回去了"，很烦）。要让视角重新贴合场景，
用控制通道的 `invoke viewport frame_scene`。

## 高亮怎么做的（以及为什么不是"变亮"）

试过三种写法，前两种都会把物体洗成白块——**金属尤其明显**：

| 写法 | 结果 |
|---|---|
| 基色往主题 accent（偏白）混 35%/70% | 固有色被白吃掉 → 白块 |
| 再加 `emission`（即使只有 0.06） | 自发光叠在受光结果上，金属反射本来就接近 1 → 冲白 |
| **基色往暖色偏一点、不改亮度** | 颜色变了、材质感保留 ✓ |

第三种是现在的做法。差别用**偏移量**表达（悬浮 0.16、选中 0.34），
而不是靠叠加亮度。这条写在 `three_view.cpp` 的注释里。

> 顺带记一个坑：`Material::emission_unlit = true` 的语义是
> **"丢掉全部受光项，只留自发光"**（见 `software_scene.cpp`）。用它做高亮
> 会让物体变成纯色平面——一开始就是这么错的。

## 「画面有东西但不对」的五个缺陷

做机械表场景时踩到的一批量，它们的共同点是**全都不会报错**，而且相互遮蔽：

| # | 缺陷 | 现象 | 根因 |
|---|---|---|---|
| 1 | 远平面没跟着距离扩 | 视口里只有背景色 | `far_z` 默认 100，而大场景的取景距离 122 → **整个场景被裁掉** |
| 2 | 表壳是实心盘 | 一整块白盘，里面什么都看不到 | `LatheGeometry` 的 profile 从 `[0, -0.95]`（圆心）起笔 → 扫出实心底 |
| 3 | 材质没传给后端 | 全白，所有零件长得一样 | A/B 实验时把 `set_material` 删了没恢复 |
| 4 | 切场景不重新取景 | 新场景缩成一小块 | 比较 `source_name_` 时它已经被覆盖成了新值 |

四个都已修并配了回归（`tests/ui_three_view_camera_test.cpp`）。

### 一个被纠正的“缺陷”：`lookAt` 其实是好的

上一版这里写过第 5 条：“`lookAt` 在 QuickJS 里角度减半”。**那条是错的。**

错在哪：读 `camera.target`（一个**点**）时把它当成了方向。
`eye.z=10`、方向 `(0.71,0,-0.71)` → `target.z = 10-0.71 = 9.29`，
而我把 `9.29` 当成了“方向 z 分量偏小”，于是算出“角度减半”。

用哨兵手法重新实测（`tools/lookat_diag_probe.cpp`）——把诊断值拼进一个
Mesh 的**名字**里，C++ 侧从 `ThreeObject::name` 读回：

| 环节 | 实测 | 结论 |
|---|---|---|
| `Matrix4.lookAt` | `m8/m10 = ∓0.7071` | ✓ 正确 |
| `setFromRotationMatrix` | `q=(0,-0.3827,0,0.9239)` 即 −45° | ✓ 正确 |
| `applyQuaternion` | `dir=(0.7071,0,-0.7071)` | ✓ 正确 |
| `camera.lookAt`（真实路径） | 同上 | ✓ 完全可用 |

场景现在用 `camera.lookAt(0, 0.4, 1.4)`，与手写 quaternion 的效果一致。

> **教训**：当一个数值“看起来不对”时，先在纸上把它推一遍。
> 我跳过了这一步，把一个自己算错的期望值当成了库的缺陷，
> 还据此在文档里写了一条“规避方案”。

### 为什么这批缺陷特别难查

单独看每一环都正常：物体数 129、网格 51.8 万顶点、世界矩阵逐帧在变、
`draw_calls` 129 次、`paint_frames` 在增长、后端 `gpu`。而
`three_check`（新建画布、无裁剪、scale=1、用场景自带相机）渲染同一份场景
**完全正常**。

差一点就归因到“框架坏了”——真正的转折点是**把相机参数当判据打印出来**
（`camera_state` 属性）：`eye.z = 118.75` 而 `far_z = 100`，一眼就完了。

> 教训：排查渲染问题时，先打印**相机与目标矩形**，再看别的。
> “画了但看不见”的绝大多数原因在这两样东西上。

## 脚本出错时会怎样

**画面不清空**，保留上一个可用场景，同时：

- 视口角标变红并显示错误（含文件名与行号）；
- 状态栏显示完整错误；
- `viewport` 的 `error` 属性非空。

修好保存后错误自动消失。这条是刻意的：改坏半行是常态，
如果此时白屏，用户的判断会是"程序坏了"而不是"我脚本写错了"。

## 用控制通道驱动（自动化）

```bash
# 启动并拿到端口
./build/dev/bin/jianmu --headless --control-port 0 --control-file /tmp/ctl.json

# 常用方法
get    {"id":"viewport"}                           # 全部属性（含 objects/picked/error）
invoke {"id":"viewport","action":"reset_view"}     # 回初始视角
invoke {"id":"viewport","action":"frame_scene"}    # 相机框住整个场景
invoke {"id":"viewport","action":"select","argument":"metal_ball"}
set    {"id":"viewport","props":{"distance":"6.0"}}
input.mouse {"kind":"drag","x":300,"y":200,"to_x":500,"to_y":200}
input.mouse {"kind":"wheel","x":300,"y":200,"delta":120}
script {"code":"app.lab.status()"}                 # 应用自己的业务状态
capture {"encode":"file","path":"shot.png"}
```

> ⚠ 两个容易踩的点（都是实测）：
> 1. **属性在 `get` 返回的 `props` 里**，不在顶层；
> 2. 桥暴露的名字带 `app.` 前缀：`expose("lab.status")` → 脚本里 `app.lab.status()`。

## 端到端验收

```bash
py tools/jianmu_e2e.py
```

20 条判据，覆盖：渲染出图、状态面可查、**基线噪声**（不改文件连拍两张必须几乎相同，
否则后面的"改后差异"证明不了任何事）、热重载生效、滚轮缩放、拖拽旋转、
悬浮命中具体物体、点击选中、脚本写坏不清空、修回后错误清除，
以及 **bpy 那一半**：`.py` 场景能装载并出图、改 `.py` 也热重载、两个扩展名都在场景清单里。

单元测试：`tests/ui_three_view_test.cpp`（11 用例 / 46 断言）覆盖相机球坐标映射、
俯仰夹取、射线方向、拾取、拖拽 vs 点击的区分、加载失败保留旧场景。

## 文件

| 文件 | 作用 |
|---|---|
| `main.cpp` | 应用装配：窗口 + 视口 + 状态栏 + 文件监听（`.js`/`.py`）+ 脚本桥 |
| `scenes/*.js` / `scenes/*.py` | 示例场景（普通 three.js 代码 / 普通 bpy 脚本） |
| `include/st/ui/components/three_view.hpp` | 视口组件（本示例的核心，框架级可复用；含按扩展名分派） |
| `src/ui/components/three_view.cpp` | 实现：轨道相机 + 拾取 + 高亮 + HUD |
| `tests/ui_three_view_test.cpp` | 组件回归 |
| `tools/jianmu_e2e.py` | 端到端验收 |

底层能力见 [`three3d.md`](three3d.md)（three.js 表达层、语义翻译表、`three_check`）
与 [`python.md`](python.md)（bpy 兼容层的能力边界）。

## 一个别踩的坑：同名场景

`quad.js` 与 `quad.py` 逐参数同构（就是要对比两条表达层）。场景清单里的名字
**带扩展名**，`--scene` 也优先精确文件名：曾经用裸主名做身份，清单里出现两个
"quad"、选中哪个无法分辨，`app.lab.select(idx)` 选到了 js 却以为是 py——
于是"改 .py 画面不变"看起来像热重载坏了（e2e 实测踩过）。

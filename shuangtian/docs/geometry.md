# 三维建模能力（几何层）

> 本文是**几何层**的能力台账：图元库、建模运算、glTF 2.0 交换格式。
> 渲染侧（材质/光照/阴影/两条腿）见 `docs/scene3d_capability_plan.md`；
> 场景脚本的字段与报错口径见 `include/st/raster/scene3d_script.hpp`。

## 0. 一句话

> 霜天现在能**造**任意常见三维形状（22 个参数化图元）、**改**它们（变换/合并/焊接/
> 归一/法线），并**用通用格式交换**（glTF 2.0 `.gltf` + `.glb`、Wavefront `.obj`）——
> 造出来的东西能直接拖进 Blender / Unity / Unreal / Godot / three.js。

## 1. 为什么是 glTF（格式选型的硬理由）

选交换格式的标准只有一个：**下游有没有人会用**。glTF 是 Khronos 的标准，
被称作"三维界的 JPEG"，覆盖面是所有候选里最广的：

| 消费者 | 支持 |
|---|---|
| Blender | 原生双向（`File ▸ Import/Export ▸ glTF 2.0`） |
| Unity / Unreal / Godot / Cocos | 原生导入 |
| three.js / babylon.js / `<model-viewer>` | 原生 |
| Windows 3D 查看器 / macOS 预览 / Office 3D | 原生 |
| Sketchfab / Spline / Vectary | 原生 |

对"大模型生成三维"这件事还有第二层理由：**模型见过的 glTF 样本最多**，
写出来的字段名与结构最可能一次就对。OBJ 作为纯文本备选保留（模型也熟，
而且人眼可读、diff 友好），两者互不替代：

| 格式 | 用在哪 |
|---|---|
| `.glb` / `.gltf` | 与外部工具交换（材质/节点/相机都在里面） |
| `.obj` | 纯几何、要能被人读和手改、进版本库看 diff |

## 2. 图元库（`include/st/raster/primitives.hpp`）

22 个参数化形状，参数名与 Blender 对齐（`radius` / `depth` / `segments` /
`rings` / `subdivisions` / `major_radius` / `minor_radius`）。

| 种类 | 规范名 | 主要参数 | Blender 对应 |
|---|---|---|---|
| 平面 | `plane` | `radius`(半宽), `depth`, `face_camera` | Add ▸ Plane |
| 立方体 | `cube` | `radius`(半边长), `depth`(Y), `height`(Z) | Add ▸ Cube |
| 经纬球 | `uv_sphere` | `radius`, `segments`, `rings` | Add ▸ UV Sphere |
| 二十面体球 | `ico_sphere` | `radius`, `subdivisions` | Add ▸ Ico Sphere |
| 圆柱 | `cylinder` | `radius`, `depth`, `segments` | Add ▸ Cylinder |
| 锥/圆台 | `cone` | `radius`, `top_radius`, `depth` | Add ▸ Cone |
| 胶囊 | `capsule` | `radius`, `depth`(总高), `rings` | Add ▸ 无（引擎常用） |
| 圆环 | `torus` | `major_radius`, `minor_radius`, 两向分段 | Add ▸ Torus |
| 圆盘 | `disc` | `radius`, `segments`, `face_camera` | Add ▸ Circle(fill) |
| 圆环带 | `circle` | `radius`, `tooth_width`(带宽) | —（光环/描边） |
| 网格面 | `grid` | `radius`, `depth`, `columns`, `rows` | Add ▸ Grid |
| 管 | `tube` | `radius`, `tooth_width`(壁厚占比), `depth` | —（管道/法兰） |
| N 棱柱 | `prism` | `radius`, `sides`, `depth`, `twist` | Add ▸ Cylinder(vertices=n) |
| 齿轮 | `gear` | `radius`, `teeth`, `tooth_height`, `tooth_width` | —（机械件） |
| 环面纽结 | `torus_knot` | `knot_p`, `knot_q`, `minor_radius` | Add ▸ Torus Knot |
| 箭头 | `arrow` | `radius`, `depth`, `segments` | —（指示/坐标轴） |
| 楔形 | `wedge` | `radius`, `depth`, `height` | —（斜坡/屋顶） |
| 正四面体 | `tetrahedron` | `radius` | Add ▸ Tetrahedron |
| 正八面体 | `octahedron` | `radius` | Add ▸ Octahedron |
| 正十二面体 | `dodecahedron` | `radius` | Add ▸ Dodecahedron |
| 正二十面体 | `icosahedron` | `radius` | Add ▸ Icosahedron |
| 螺旋 | `helix` | `radius`, `depth`, `turns`, `taper`, `minor_radius` | —（弹簧/螺纹） |

别名（`sphere`/`ball`/`box`/`disk`/`donut`/`knot`/`spring`/`cog`…）都能解析到同一枚举，
但**写出去的一律是规范名**——别名是为兼容而存在，不是新写法。

### 2.1 每个生成器的五条契约（测试逐项断言）

1. **右手系、Y 轴朝上、单位尺度**（`radius` = 到中心的距离）；
2. **闭合体水密**：每条边恰好被两个三角形共享（单面形状 `plane`/`disc`/`circle`/`grid` 除外，
   它们的名字就写着"单面"）；
3. **法线朝外且单位长**；三角形**逆时针**为正面（与 `CullMode::Back` 一致）；
4. **UV 在 0..1**；
5. **确定性**：同一参数永远同一字节序列（无随机、无时间依赖）。

## 3. 建模运算（`include/st/raster/mesh_ops.hpp`）

| 运算 | 对应 Blender | 关键点 |
|---|---|---|
| `transform(mesh, Mat4)` | Object ▸ Transform | 法线用**逆转置**；镜像（行列式 < 0）自动翻转绕序 |
| `transform(mesh, t, r, s)` | 同上（T·R·S） | 顺序固定：缩放 → 旋转 → 平移 |
| `merge({...})` / `merge(a,b)` | Object ▸ Join（Ctrl+J） | 索引与 UV 全部重映射；UV 只有"全有"才保留 |
| `weld(mesh, eps)` | Mesh ▸ Merge by Distance | **位置 + 法线**同判据（只按位置会把硬边焊成软边） |
| `recompute_normals(mesh, Smooth/Flat)` | Shade Smooth / Shade Flat | Flat 会复制顶点（硬边无法共享顶点表达） |
| `resize(mesh, size)` | Object ▸ Dimensions | 某轴尺寸为 0 时不缩放该轴（避免除零 → NaN） |
| `center_at` / `sit_on_ground` | — | "放到地上"是搭场景最常见的构图动作 |
| `bounds(mesh)` / `describe(mesh)` | — | `describe` 返回 JSON 摘要，给模型自检用 |
| `to_obj_text` / `write_obj_file` | File ▸ Export ▸ Wavefront | 索引 1 基、Y-up、`v//vn` 写法（往返可读） |

## 4. glTF 读写（`include/st/raster/gltf.hpp`）

### 4.1 支持范围（诚实声明）

✅ **已支持**

- `.gltf`（JSON + 外部 `.bin` 或内嵌 base64）与 `.glb`（单文件二进制容器，含 chunk 对齐）
- `POSITION` / `NORMAL` / `TEXCOORD_0` 与 `indices`（无索引时按顺序三角形）
- 节点变换（`matrix` 或 `translation`/`rotation`/`scale`）与**嵌套 children**（父链累乘）
- 材质：`baseColorFactor` / `metallicFactor` / `roughnessFactor` / `emissiveFactor` / `alphaMode`
- 相机（`perspective` / `orthographic`）
- 多 mesh / 多 primitive → 合并进一个 `Mesh`（当前渲染器"一网格一材质"）

❌ **不支持**（遇到时**明确报错或跳过该项**，不静默出错）

- 动画、蒙皮、变形目标
- `KHR_draco_mesh_compression`、`KHR_texture_basisu`（需额外解码器）
- 纹理图像解码（字节会读出，但不自动解码成像素——PNG 在 `codec` 层）
- 稀疏 accessor、`TEXCOORD_1+`、点/线图元（只做三角形，其它 `mode` 报 `Unsupported`）

### 4.2 两条硬口径

- **坐标与单位**：glTF 规定 +Y up / 右手系 / 米，与霜天三维**完全一致** → 导入导出**不做轴变换**
  （OBJ ↔ glTF 转换最常见的坑就是 Y-up/Z-up 互换，这里从源头不存在）。
- **颜色空间**：glTF 的 `baseColorFactor` 是**线性**，`Material::base_color` 也是线性
  → 读取时经 sRGB 编码成 `Color`、写回时解码，不做这一步会亮一档（"过曝的塑料"）。

### 4.3 错误要能指路

所有失败都带 **JSON 路径**：`meshes[0].primitives[0].attributes.POSITION`、
`accessors[0]`、`buffers[0].uri`。理由与场景脚本同一口径：**生成代码的模型拿到
"解析失败"没法修正，拿到字段路径能立刻改对**。

## 4.4 组合模型的空间体系（节点树 / 局部坐标 / 枢轴）

一个"组合模型"是**一棵节点树**：每个节点有自己的局部坐标系，子节点相对父节点定位
（父动子动）。这与 Blender 的父子关系、游戏引擎的 Scene Graph 是同一套语义，
数值可以直接互相搬。实现见 `include/st/raster/scene_graph.hpp`。

### 4.4.1 两种等价写法

```json
// 嵌套：层级清楚，适合人读
{ "name": "robot", "group": true, "translate": [0, 0, 0], "children": [
    { "name": "arm", "group": true, "translate": [0.5, 0.3, 0], "rotate": [0, 0.6, 0],
      "children": [
        { "name": "upper", "shape": "capsule", "radius": 0.12, "depth": 0.8,
          "translate": [0, -0.4, 0] },
        { "name": "hand", "shape": "uv_sphere", "radius": 0.1, "translate": [0, -0.85, 0] } ] } ] }

// 扁平：适合增量编辑（改一行不影响其它行），父可以出现在子之后
[ { "name": "robot", "group": true },
  { "name": "arm", "parent": "robot", "group": true, "translate": [0.5, 0.3, 0], "rotate": [0, 0.6, 0] },
  { "name": "upper", "parent": "arm", "shape": "capsule", "translate": [0, -0.4, 0] },
  { "name": "hand", "parent": "arm", "shape": "uv_sphere", "translate": [0, -0.85, 0] } ]
```

两种写法得到**逐位相同**的世界矩阵（`tests/geom_scene_graph_test.cpp` 逐元素比对）。

### 4.4.2 枢轴（pivot）：绕哪里转

没有枢轴时一切旋转绕局部原点。而现实里的组合体几乎都不是这样：门的枢轴在合页、
机械臂的枢轴在肩关节、整台设备的枢轴在底座中心。

```text
local = T(translate) · T(pivot) · R(rotation) · S(scale) · T(-pivot)
```

`pivot = (0,0,0)` 时退化成 `T · R · S`（旧行为，逐像素不变）。

**预设**（按自身几何包围盒算，比手算可靠）：

| `pivot_at` | 枢轴位置 | 典型用途 |
|---|---|---|
| `origin` | 局部原点（默认） | 旧行为 |
| `center` | 包围盒中心 | 自由旋转 |
| `bottom` | 底面中心 | **"站在地上"的物体**（机器人/家具/车辆） |
| `top` | 顶面中心 | 悬挂物（吊灯/钟摆） |

```json
{ "shape": "cube", "radius": 0.5, "depth": 1.0, "height": 1.0,
  "pivot_at": "bottom", "rotate": [1.5707963, 0, 0] }   // 就地倾倒，不插进地里
```

### 4.4.3 约定与校验

| 项 | 约定 |
|---|---|
| 手性 | 右手系，+Y 朝上 |
| 角度 | 弧度；`rotate` 合成顺序 **X → Y → Z**（= Blender `rotation_euler` XYZ） |
| 局部 | 子节点的 `translate/rotate/scale` **相对父节点** |
| 可见性 | **继承**：父不可见 → 整棵子树不可见（藏一台设备不必逐个零件关） |
| 名字 | 同一场景内**必须唯一**（重名即报错，不做"后者覆盖前者"） |
| 深度 | 上限 32 层（防病态输入） |

**错误都带路径**：重名报出是哪几个名字、扁平写法的父不存在报出"谁要挂谁"、
环报出经过哪个节点、`pivot_at` 拼错时列出可用清单。

### 4.4.5 尺寸字段的优先级（一处"静默用错默认值"的修复）

历史上不同形状吃不同的尺寸字段名：`plane` 吃 `width`/`height`、`cylinder` 吃 `height`、
`box` 吃三个都行、新图元统一吃 `radius`/`depth`。于是

```json
{ "shape": "cylinder", "radius": 0.2, "depth": 0.1 }   // 意图：一个 0.1 高的矮圆柱
```

里的 `depth` 会被**静默忽略**，实际用 `height` 的默认值 1.0——写探针时底座本该 0.12 高，
渲染出来 1.0 高，整张图比例全错，而且没有任何报错。

现在的取值优先级是：

```text
显式写的字段  >  该形状的历史字段  >  图元库默认值
```

`SceneObjectSpec::specified` 记下"调用方到底写了哪些"，`build_mesh` 据此选值。
**旧脚本（只写历史字段）逐像素不变；新脚本写通用字段立刻生效。**

### 4.4.4 地面对齐（一处必须知道的历史包袱）

`ground: true` 的节点**只保留绕 Y 的朝向**（自动去掉绕 X/Z 的倾斜）。
原因：旧 `Mesh::plane` 朝 +Z（立面），所以旧脚本普遍写 `rotate: [-π/2,0,0]` 把它扳平；
而新图元 `plane` 本就朝 +Y。不吸收那 -90°，地面会立成墙——实测投影阴影
**一个像素都不出**（开关阴影画面均值 99.063 → 99.063）。

## 4.5 动画（关键帧 / 缓动 / 四元数插值）

有了空间体系之后，"骨架"已经能摆位；动画让它可以**按时间动**。
实现见 `include/st/raster/animation.hpp` 与 `st/math/quaternion.hpp`。

### 4.5.1 为什么旋转必须用四元数

欧拉角有三个致命的插值问题，全都表现为"动画看起来不对"：

| 问题 | 症状 | 四元数如何解决 |
|---|---|---|
| **绕远路** | 350° → 10° 线性插值转过 340°（先反着猛转一圈） | `slerp` 走球面大圆的短弧（20°） |
| **万向锁** | 两轴对齐时丢一个自由度，中间姿态突然跳变 | 四元数无奇点 |
| **分量不独立** | 逐分量插值得到的中间姿态不在最短路径上（"甩"而非"摆"） | 整体插值 |

这也是 glTF 用四元数存旋转的原因。`slerp` 里还有一个**最容易漏**的细节：
`q` 与 `-q` 表示同一个旋转，若点积为负必须翻转 `b`，否则会走 360°-θ 的长弧——
**短动画看着正常，某些关键帧对之间突然反向猛转**。`tests/geom_animation_test.cpp`
有一条专门钉它（350°→10° 的中点必须在 0° 附近，而不是 180°）。

脚本层仍写欧拉角（人更容易算），转换点只有两处：
`Quat::from_euler`（进动画）与 `Quat::to_euler`（出动画）。

### 4.5.2 脚本形态

```json
"animation": {
  "name": "wave", "duration": 2.0,
  "tracks": [
    { "node": "shoulder", "kind": "rotate",
      "keys": [ { "t": 0.0, "value": [0, 0, 0],   "easing": "ease-in-out" },
                { "t": 1.0, "value": [0, 0, 1.0], "easing": "ease-in-out" },
                { "t": 2.0, "value": [0, 0, 0] } ] },
    { "node": "hand", "kind": "visible",
      "keys": [ { "t": 0.0, "value": [1, 0, 0] }, { "t": 1.5, "value": [0, 0, 0] } ] }
  ]
}
```

| 字段 | 含义 | 说明 |
|---|---|---|
| `tracks[].node` | 目标节点名 | 必须存在于场景树（否则渲染时报错并指路） |
| `tracks[].kind` | `translate` / `rotate` / `scale` / `visible` | 别名：`position`/`rotation`/`size`/`show` |
| `keys[].t` | 时间（**秒**） | 也接受 `time`；**不要求有序**（解析时排序） |
| `keys[].value` | 值 | `rotate` 是**欧拉角（弧度）**；`visible` 取 `value.x > 0.5` |
| `keys[].easing` | `linear` / `constant` / `ease-in` / `ease-out` / `ease-in-out` | 作用于"本帧 → 下一帧"这一段 |

**端点保持**：时间在关键帧范围外取端点值（不循环、不外推）——循环是播放器的策略
（`t = fmod(t, duration)`），库只负责"给我 t 时刻的值"。

### 4.5.3 渲染某一时刻

```cpp
// 静态（原行为，逐像素不变）
render_scene(spec, canvas, background);
// 动画：渲染 t 时刻
render_scene_at(spec, time_seconds, canvas, background);
// 复用已有场景实例（批量导出用这条）
render_scene_at_into(spec, time, scene, canvas, background);
```

没有动画时两者**逐像素相同**（`anim_script_without_time_matches_static` 钉住）。

### 4.5.4 glTF 中的动画（含一条必须说清的损失）

| glTF | 本框架 |
|---|---|
| `channels[].target.path: translation / rotation / scale` | `TrackKind::Translation / Rotation / Scale` |
| `rotation` 采样（**四元数**） | 转欧拉角存 → **万向锁附近有损** |
| `interpolation: LINEAR / STEP` | `Easing::Linear / Constant` |
| `interpolation: CUBICSPLINE` | 取"值"那个控制点（曲率丢失，**声明为近似**） |

**反方向的损失更要紧**：glTF 的插值只有 LINEAR / STEP / CUBICSPLINE 三种，
而本框架的 `ease-*` 是**任意曲线**。直接写 LINEAR 会让缓动消失（霜天里对、Blender 里不对）。
解法是导出前**稠密化**：

```cpp
options.animations = { densify(clip, 1.0f / 30.0f) };   // 按 30Hz 重采样
```

曲线形状因此保真（2 秒动画 → 61 个关键帧），代价是关键帧变多。
`densify` 会跳过本来就全是 LINEAR 的轨道（不白白膨胀）。



```cpp
// ① 直接造形状
namespace prim = st::raster::primitives;
prim::PrimitiveParams params{.radius = 0.6f, .depth = 0.3f, .teeth = 12};
st::raster::Mesh gear = prim::make(prim::PrimitiveKind::Gear, params);

// ② 改它
gear = st::raster::transform(gear, st::math::Mat4::translation({0, 1, 0}));
gear = st::raster::sit_on_ground(gear, 0.0f);
auto summary = st::raster::describe(gear);   // JSON：顶点/三角形/包围盒

// ③ 换出去
std::vector<st::raster::GltfObject> objects{{.mesh = gear, .name = "gear"}};
st::raster::gltf_write_file("out/gear.glb", objects, {.binary = true});
```

```json
// ④ 或者只用脚本（不需要写 C++）
{
  "camera": { "eye": [0.8, 0.9, 2.6], "target": [0, 0, 0], "fov": 40 },
  "objects": [
    { "shape": "gear", "radius": 0.6, "teeth": 12, "color": "#d4b07a",
      "metallic": 0.6, "roughness": 0.3, "translate": [0, 0.5, 0] },
    { "shape": "sphere", "gltf": "assets/robot.glb", "rotate": [0, 0.6, 0] }
  ]
}
```

> `shape` 走内置图元；`gltf` 走外部文件（自动读节点变换与材质）。两者互斥，
> 同时写时 `gltf` 优先。

## 4.6 蒙皮（骨骼 / 权重 / LBS）

刚性父子（`scene_graph`）对机械臂够用，但**角色不行**：手臂弯曲时肩部皮肤要连续变形，
不能出现一条裂缝。蒙皮解决这件事，实现见 `include/st/raster/skin.hpp`。

### 4.6.1 原理一句话

```text
顶点 = Σ wᵢ · (骨骼ᵢ的世界矩阵 · 逆绑定矩阵ᵢ) · 顶点
```

- **逆绑定矩阵**：模型空间 → 骨骼的绑定（建模时）空间；
- **世界矩阵**：再把它搬回**当前姿态**的模型空间；
- 两者之积就是"这根骨骼此刻的形变"，多根按权重加权。

绑定姿态下两者相消 → 蒙皮矩阵 = 单位阵 → **形变恒等**（最硬的一条自检）。

### 4.6.2 数据布局：为什么权重是平行边表

`Mesh::kVertexFloats = 9` 是"生成器 ↔ 后端上传"的契约（三处后端按它算 stride）。
往里加 8 个分量要同时改生成器、全部后端、glTF 读写、OBJ 导出——其中绝大多数路径
根本不涉及蒙皮（静态场景、界面小图标）。所以与 UV 一致，存成平行边表：

```cpp
std::vector<float> skins;   // 每顶点 8 个：[j0,w0, j1,w1, j2,w2, j3,w3]
```

空 = 没有蒙皮。好处是零破坏、零开销、语义清楚。
固定 4 个影响是行业事实标准（GPU 顶点着色器不能循环可变长数组，glTF 的
`JOINTS_0` 也是 4 个/组）；超过时取权重最大的四个再归一化。

### 4.6.3 脚本形态

```json
"skeleton": { "joints": ["root", "upper", "forearm"] },
"objects": [
  { "name": "root", "group": true },
  { "name": "upper", "parent": "root", "translate": [0, 0.6, 0] },
  { "name": "body", "parent": "upper", "shape": "capsule", "skin": true }
]
```

- `skeleton.joints` 的关节名**必须与节点名一致**（找不到时报错并指出是哪个关节）；
- `inverse_bind` 可省略（默认单位阵）；真实角色应当在绑定姿态下用
  `inverse_bind_from_world(节点世界矩阵)` 生成；
- `objects[].skin: true` 的对象才参与蒙皮（场景里常常刚体与软体混排）。

**没有权重时会用朴素自动权重**（离哪个关节近给谁）——这不是自动绑定，
真实角色应当在 Blender 里刷权重再用 glTF 导入。

### 4.6.4 一条必须声明的限制

本实现是 **LBS（线性混合蒙皮）**。它在关节**大幅扭转**时会出现"糖纸效应"
（体积塌陷、边缘捏扁）。双四元数（DQS）能解决，但成本翻倍且 glTF 不表达它——
所以这里选 LBS（与所有引擎的实际行为一致），并写进文档而不是假装没有。

### 4.6.5 glTF 互操作

| glTF | 本框架 |
|---|---|
| `skins[].joints`（节点索引） | `Skeleton::joints`（**名字**，读写时按名互转） |
| `skins[].inverseBindMatrices` | `Skeleton::inverse_bind`（列主序 MAT4） |
| `JOINTS_0`（VEC4 无符号整数） | `Mesh::skins` 的 `j*`（写 u8，索引 > 255 时自动升 u16） |
| `WEIGHTS_0`（VEC4 float） | `Mesh::skins` 的 `w*`（写出前强制归一化） |

**写出时会补出缺失的关节节点**：骨架关节常常是"纯骨骼"节点（没有几何），
不在 `objects` 里；若只按 `objects` 建节点表就找不到它们，整副骨架会被跳过——
文件仍然合法，只是读回来没有蒙皮。所以缺失的关节会自动补节点（用逆绑定矩阵的逆
作为绑定姿态）。

## 6. 验收（可复算）

| 产物 | 钉住什么 |
|---|---|
| `tests/geom_primitives_test.cpp` | 22 个图元逐个：绕序/水密/单位法线/UV/确定性/尺寸语义/别名与报错 |
| `tests/geom_gltf_test.cpp` | `.glb` 头与 chunk 长度、往返逐顶点、第三方文件能读、错误带路径、落盘往返 |
| `tests/geom_scene_graph_test.cpp` | 组合模型：父动子动、乘序、枢轴（含预设/符号）、扁平=嵌套、可见性继承、端到端渲染、错误带路径 |
| `tests/geom_animation_test.cpp` | 动画：slerp 最短路径、缓动曲线、端点保持、旋转不走欧拉插值、可见性阶跃、校验、端到端、枢轴×动画组合 |
| `tools/animation_probe.cpp` | 8 帧分镜图 + 带动画的 `.glb`（稠密化后读回验证） |
| `tests/geom_skin_test.cpp` | 蒙皮 10 条：逆绑定往返、权重限制/归一化、刚体与混合、未归一化不缩小、绑定姿态恒等、glTF 权重往返、端到端形变 |
| `tools/skin_probe.cpp` | 蒙皮 vs 刚性分段对照图（关节连续 vs 断裂台阶） |
| `tests/geom_scene_integration_test.cpp` | 22 个图元**真的画出像素**、旧形状语义不变、参数生效、导出→读回、焊接/变换/OBJ 往返 |
| `tools/primitives_gallery_probe.cpp` | 22 格总览 PNG + 每个图元的 `.glb`（可拖进 Blender 对照） |
| `tools/composite_pivot_probe.cpp` | 组合模型四格对照（无旋转 / 绕肩转 / 枢轴错放 / 父隐藏），证明父子与枢轴各自生效 |

```
st test primitives_      # 6 条
st test gltf_            # 6 条
st test scene_script_    # 12 条（含图元与 glTF 集成）
st build primitives_gallery_probe && build/dev/bin/primitives_gallery_probe.exe out/primitives
```

实测（2026-10-30，RTX 4080 SUPER / GPU 腿）：22/22 格出图、22/22 个 `.glb` 导出并被读取器验证
（顶点数一致），单格渲染 220×220 全部 < 1 帧预算。

## 7. 已知边界与后续（诚实清单）

| 项 | 状态 | 说明 |
|---|---|---|
| 曲线 / NURBS / 布尔运算 | 未做 | 布尔需要精确的网格求交（CGAL 量级），当前收益不抵成本 |
| 网格简化（Decimate） | 未做 | 有现成且足够简单的近似（边折叠），但需要"可接受误差"的产品口径 |
| 蒙皮 / 动画 / morph | 未做 | glTF 侧只读静态几何；动画属于时间轴层（裁云）的职责 |
| UV 展开（Unwrap） | 未做 | 现有 UV 是生成器内置的"够贴图"版本，不是可编辑展开 |
| Draco / KTX2 压缩 | 未做 | 需要第三方解码器；接入会打破"零第三方依赖"的现状 |
| 纹理自动解码进 glTF | 未做 | 图片字节已读出，解码交给 `codec`（保持层间不互相依赖） |

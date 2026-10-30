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

## 4.7 碰撞与拾取（`math/collision.hpp` + `raster/pick.hpp`）

### 4.7.1 为什么分宽相 / 窄相

一个 5000 三角形的模型逐三角形求交要算 5000 次（每次含一次除法 + 三个叉积），
而先测它的 AABB 只要 6 次比较。差 2~3 个数量级——所以凡"场景里可能有多个物体"，
先宽相后窄相不是优化，是**能不能用**的区别。

| 阶段 | 判据 | 说明 |
|---|---|---|
| 宽相 | 射线 × **世界空间 AABB**（slab 法） | 永远先做 |
| 窄相 | 射线 × **三角形**（Möller–Trumbore，在模型空间） | 宽相通过后 |

窄相在**模型空间**做（把射线用逆矩阵搬进去）：后者每个三角形要 12 次乘法，
前者每个物体只有一次求逆。

### 4.7.2 三个数值陷阱（都踩过）

1. **平行射线的 0/0**：方向某轴为 0 时除法得 `±inf` 或 `NaN`。
   `if (t < best)` 对 NaN 恒假 → "永远打不中"，看起来像"还没实现拾取"。
   本实现**显式判断**平行轴，不依赖 IEEE 特殊值比较。
2. **退化三角形**：`|det|` 极小时硬算会得到噪声 `t`，症状是掠射角下随机命中。
   本实现直接返回"不相交"。
3. **世界 AABB 只变换两个角**：旋转后严重低估（绕对角线转 45° 的长方体，
   变换两角得到的盒子小一半）→ "明明在盒里却拾取不到"。
   本实现变换**8 个角**再取并。

### 4.7.3 用法

```cpp
// 造拾取物体（自动算世界 AABB）
auto object = make_pick_object("box", mesh, node_world_matrix);

// 拾取最近命中（结果带节点名、命中点、法线、重心坐标）
if (const auto hit = raycast(objects, ray); hit.has_value()) {
  // hit->name / distance / point / normal / triangle / bary_u / bary_v
}

// 穿透看全部（按距离升序）
const auto all = raycast_all(objects, ray);

// 碰撞宽相
if (overlaps(a, b)) { ... }          // 世界 AABB 重叠
const float gap = bounds_gap(a, b);  // 最近距离（重叠为 0）
```

法线统一**朝射线来向**（调用方总想"往自己这边推"）；不统一的话同一物体会时正时反。

### 4.7.4 边界

只做**几何查询**（raycast / overlap），不做刚体动力学（质量、摩擦、冲量）。
"点选物体""判断两个模型是否穿插""视线遮挡"这些三维应用最常要的事，几何查询就够。

## 4.8 材质贴图（纹理 + UV 变换）

### 4.8.1 脚本形态

```json
{ "name": "wall", "shape": "cube", "radius": 1.0, "depth": 2.0, "height": 1.0,
  "color": "#c8c8c8",            // 顶点色 = 染色（与纹理相乘）
  "texture": "brick.png",        // 相对路径按 asset_basis 解析
  "texture_tile": 4.0,           // 便利字段：两个方向同时平铺 4 次
  "texture_scale_u": 4.0,        // 等价于 texture_tile 的单轴版本
  "texture_scale_v": 4.0,
  "texture_offset_u": 0.0,       // 平铺相位
  "texture_offset_v": 0.0,
  "texture_clamp": false }       // false = repeat 平铺（默认），true = 夹边缘
```

采样契约：**纹理 × 顶点色**（与 Blender 的 Base Color 纹理 + 顶点色相乘同语义）。

### 4.8.2 为什么 UV 变换是场景状态而不是烘焙进网格

"同一张图在不同零件上以不同密度平铺"（砖墙、地面、布料）是极常见的需求。
烘焙意味着每个零件一份网格副本（内存 ×N、且不再共享几何），
而 UV 变换只是 `uv' = uv * scale + offset`——采样前两次乘法。

### 4.8.3 repeat 与 clamp

| 模式 | 语义 | 用在哪 |
|---|---|---|
| `repeat`（默认） | 超出 0..1 环绕 | 平铺（砖、地面、布料） |
| `clamp` | 取边缘色 | 图标、单张贴图、UI 面片 |

repeat 用 `x - floor(x)` 而**不用 `fmod`**：`fmod` 对负数返回负值（C++ 语义），
直接拿去索引会越界，而越界会被 clamp 掩盖——表现为"负 UV 区域全取边缘色"。

### 4.8.4 一处必须写下来的坑：立方体 UV

`make_cube` 的 UV **按每个面各自归一化到 0..1**（与所有建模软件一致）。
踩过两次：

1. 六个面硬编码 0..1 → 非立方体时各面长宽比不同，方格被拉成**条纹**；
2. 按世界尺寸归一化（"1 米一格"）→ 方格比例对了，但小面只覆盖 UV 的一部分，
   `texture_tile` 一乘就露馅（图案被切一半再拉伸）。

代价：面上图案的长宽比随面尺寸变化（宽面上方格被拉宽）——这是 UV 映射的固有取舍。

### 4.8.5 双后端一致

`set_texture_transform` 在软件腿（`sample_texture` 里做仿射 + 环绕）与
GPU 腿（顶点着色器里做仿射 + 常量缓冲传参）都实现了。
**仿射放顶点侧**（逐顶点与逐像素等价，省每像素两次乘法），
**repeat/clamp 留像素侧**（那是取模，跨三角形插值后逐像素才正确）。

## 4.9 生态素材兼容（Blender / three.js）

### 4.9.1 起点：实测只有 3/6 项信息存活

用一份 three.js 形态的 glTF（外部 .bin + 外部贴图 + PBR 材质）实测：

```
[OK]   几何、UV
[依赖] 材质 base_color —— 全白（贴图丢了）
[缺失] 材质贴图        —— GltfScene 根本没有 images/textures 字段
```

这就是"吸收生态"的真正障碍：**不是读不了文件，而是读进来信息全丢**。

### 4.9.2 补齐后：5/5 项 100% 保留

| 能力 | 说明 |
|---|---|
| `images` 三种来源 | 外部文件 / `data:` URI / `bufferView`（`.glb` 内嵌） |
| JPEG 解码 | **自研**，基线 + 渐进式 + 灰度/YCbCr + 4:4:4/4:2:2/4:2:0/4:1:1 |
| 按魔数识别格式 | 不信扩展名（素材包里 `.png` 实际是 JPEG 的情况真实存在） |
| 材质贴图引用 | `baseColorTexture` / `normalTexture` / `emissiveTexture` / `metallicRoughnessTexture` |
| `KHR_texture_transform` | `uv' = uv * scale + offset`（与脚本层同一语义） |
| 渲染自动挂贴图 | 脚本**零改动**即可用生态素材 |

### 4.9.3 扩展三档处置（消除静默失败）

| 档 | 含义 | 处置 |
|---|---|---|
| **支持** | 本框架实现了 | 正常读 |
| **忽略** | 只影响细节（clearcoat/sheen/variants…） | 继续渲染，**列进报告** |
| **阻断** | 缺了就读不对（Draco / KTX2） | **报错 + 给解决动作** |

**为什么阻断必须报错**：Draco 压缩的模型在不装解码器时，旧行为是**静默读出空几何**——
文件合法、解析成功、画面空白。那是最糟的失败形态（用户以为素材坏了或框架不行）。
现在会得到：

```
这份 glTF 依赖本框架不支持的必需扩展：KHR_draco_mesh_compression。
其中 KHR_draco_mesh_compression 是几何压缩：请在 Blender 导出时关闭"压缩"，
或让导出方选择未压缩的 glTF（本框架暂不内置 Draco 解码器）
```

### 4.9.4 明确不做：Draco / KTX2 解码器

**不做**，理由是与收益不匹配：Draco 解码器是独立库级工程（熵解码 + 属性预测，
数千行 + 持续跟进格式演进），而它解决的问题（减小文件体积）在"出图"场景里不紧迫。

替代方案更负责：**检测到就明确报错并告诉用户怎么办**。若之后确实需要，
正确路径是引入官方 C++ 库做可选编译依赖，而不是手写一份再维护它。

### 4.9.5 一条重要的经验

`asset_basis` 字段**一直在被读、却从没被解析**——`resolve_shape_path` 用它，
但解析器里没读，于是它恒为空，所有相对路径退回"按工作目录找"。
现有用例能过是因为它们从 C++ 直接赋值，走 JSON 的路径从未被覆盖。

> 教训：字段"被用到"不等于"被解析到"，两头都要有用例。

## 4.10 过程化几何（脚本生成）

### 4.10.1 结论：过程化比"跑 three.js 脚本"更可行

| 脚本类型 | 依赖 | 霜天能否 |
|---|---|---|
| **过程化**（循环算顶点） | 数学 + 数组 | ✅ **能**（已实测） |
| 声明式（`new Mesh` + `scene.add`） | three.js 对象模型 | ❌ 需兼容层 |
| 自定义 shader | GLSL → WebGL | ❌ 渲染器不同 |

**关键洞察**：过程化脚本的信息量几乎全在**公式**里，而不在 API 里。
`Math.sin`、循环、数组 QuickJS 原生就有——不需要模拟任何 three.js 对象。

实测生成的扭转带（半径按 `sin` 起伏、绕 Y 扭转 3π、压扁 1.6 倍的截面）
2184 顶点 / 4320 三角形，全部由脚本公式决定。

### 4.10.2 两条通道与实测开销

| 通道 | 接口 | 实测 |
|---|---|---|
| JSON | `register_function`（`vector<Json>`） | **3.5 µs/顶点** |
| 二进制 | `register_binary_function`（`span<const float>`） | **1.4 µs/顶点** |

```cpp
// 注册
engine.register_binary_function("addGeometry", [](const std::vector<std::span<const float>>& arrays)
    -> Result<Json> { /* arrays[0] = 位置, arrays[1] = 法线 */ });
```

```js
const positions = new Float32Array(count * 3);
// ... 填充 ...
addGeometry(positions, normals);   // 一次调用传多个平行数组
```

### 4.10.3 拆解：瓶颈已转移到 JS 自身

| 项 | 每顶点 |
|---|---|
| 纯 JS 计算（不调宿主函数） | 1.018 µs |
| 二进制通道总耗时 | 1.375 µs |
| 其中通道开销 | **0.356 µs（占 26%）** |

规模放大 10 倍（21624 顶点）后通道占比从 31% 降到 26%——**确认通道不再是瓶颈**。
继续优化通道没有意义，真要进一步提速得优化 JS 循环本身（或改 C++ 侧生成）。

这个拆解本身很重要：不做它，看到"2.4x 提速"会误判成"通道没优化好"，
而真相是通道已经让位。

### 4.10.4 类型校验：不隐式转换

传普通数组给二进制通道会**立即报错**并指出参数序号：

```
TypeError: addData 的第 1 个参数必须是 Float32Array（收到类型码 -1）；
请用 new Float32Array(n) 构造后再传
```

**不做隐式转换**——那会悄悄退回慢路径，于是"我明明用了二进制通道怎么还是慢"
变成一个查不出的问题。

## 4.11 真实素材实测（Khronos glTF 官方样例）

自造素材的局限：**我按自己的理解构造，也按自己的理解验证**，于是双方一致的错误
永远不会被发现。所以用别人按规范导出的真实素材再验一遍。

### 4.11.1 结果：8/8 渲染成功，1/1 正确拒绝

| 素材 | 三角形 | 贴图 | 半径 | 结果 |
|---|---|---|---|---|
| Box | 12 | 0 | 0.5 | ✅ |
| BoxTextured | 12 | 1 | 0.5 | ✅ |
| DamagedHelmet | 15452 | **5** | 1.0 | ✅ 细节/破损/贴纸全在 |
| Avocado | 682 | 1 | **0.031** | ✅ 果皮纹理、果核渐变 |
| Corset | 18324 | 1 | **0.031** | ✅ 天鹅绒、系带、木纹 |
| WaterBottle | 4510 | 1 | 0.13 | ✅ 标签 logo 清晰 |
| AntiqueCamera | 20066 | 多张 | 3.60 | ✅ 木纹三脚架、金属机身 |
| RiggedSimple | 188 | 0 | 4.58 | ✅ 骨架 2 关节 + 动画 |
| **Box (Draco)** | — | — | — | ✅ **正确拒绝并指路** |

### 4.11.2 真素材抓到的两个缺陷（自造素材碰不到）

**① 近裁剪面固定 0.1**

Avocado / Corset 的半径是 **0.031**（3cm 道具）。相机距离约 0.08，
于是**整个模型落在近裁剪面之内 → 渲染出 0 个像素**，而且没有任何报错——
画面只是一片背景色。

这是"生态素材"的普遍问题：不同来源的模型单位差几个数量级
（珠宝 mm、道具 cm、建筑 km）。**实测跨度 115 倍**（0.0314 ↔ 3.604），
任何固定的 near/far 都会在某一端失效。

修法：调用方按模型尺度自适应 `near_z = distance * 0.01`。

**② 相机距离凭常数**

初版用 `radius * 3.4`：对半径 1.0 合适，对 0.5 就把模型顶出画框。
根因是"物体占画面的比例"由 `2*atan(radius/distance)` 决定，而 fov 固定——
正解是由 fov 反解距离：`distance = radius / (fill_ratio * tan(fov/2))`。

### 4.11.3 素材获取与测试

真实素材**不进仓库**（几十 MB + 第三方版权）。用脚本取：

```powershell
powershell -ExecutionPolicy Bypass -File tools/fetch_assets.ps1
# 或指定目录
powershell -ExecutionPolicy Bypass -File tools/fetch_assets.ps1 -OutDir D:\assets
```

来源 [KhronosGroup/glTF-Sample-Assets](https://github.com/KhronosGroup/glTF-Sample-Assets)
（多数 CC0 / CC-BY 4.0，每个模型目录下有 LICENSE.md）。

`tests/real_asset_test.cpp` 在素材不存在时用 **`ST_SKIP`**（报告里可见，
不算通过也不算失败）——而不是静默 `return`（那会让"没测"和"测过"看起来一样）。
也可用 `ST_REAL_ASSETS` 环境变量指向自己的素材目录。

```powershell
# 兼容性总账
build\dev\bin\ecosystem_probe.exe tmp\real\*.glb
# 渲染成图片（人眼验收）
build\dev\bin\real_asset_probe.exe out\real tmp\real\*.glb
# 回归测试
st test real_asset
```

## 4.12 大模型可用性（把"对熟练人类友好"变成"对模型友好"）

### 4.12.1 起点：实测出的四个短板

用"模拟大模型最常犯的错"的方式测（`tools/llm_usability_probe.cpp`），
起点是 **5 类错误被静默接受**：

```
radius 拼成 raduis       → (接受)，用默认值 0.8
translate 拼成 translaet  → (接受)，物体停在原点
顶层 camra                → (接受)，相机用默认
radius 写成字符串 "0.5"   → (接受)，读不到就退回默认
segments: -5              → (接受)，被 clamp
```

**为什么这比崩溃更糟**：模型看到"解析成功"会认为改好了，画面没变 → 随机试错
（换字段名、换数值、换形状），每次都被静默接受 → 长循环。**报错反而能让它一次改对。**

### 4.12.2 ① 未知字段检查：报错 + 候选，但**不自动修**

现在得到：

```
objects[0].raduis：未知字段（objects[] 这一层不认识 "raduis"）；
    你是不是想写 radius？
```

机制：`scene_schema.hpp` 的**字段白名单表**是唯一真源，解析后逐层比对，
差集用编辑距离给候选（返回 2~3 个，因为 `radius` 与 `rings` 只差两字符，
单给一个有时会指错方向）。

**刻意不自动修**：自动替换会让调用方失去校正机会，一旦猜错就变成新的静默错误。
"报错 + 候选"已经能让模型一次改对，比任何自动修复都可靠。

覆盖 9 层：`scene` / `objects[]` / `camera` / `lighting` / `shadow` / `animation` /
`animation.tracks[]` / `animation.tracks[].keys[]` / `skeleton`。

实测结果：**静默接受错误 5 → 0**。

### 4.12.3 ② `st_api`：机读能力清单

```bash
st_api              # 完整清单（JSON）
st_api --compact    # 单行
st_api --example    # 最小可运行脚本
st_api --scopes     # 只列各层（9 层 / 92 个字段）
st_api --check f.json    # 校验 + 自检（写完自查）
st_api --normalize f.json # 读入再写出（看引擎"理解成了什么"）
```

清单含：图元名（22 个）、轨道种类、缓动名、9 层字段（类型/范围/说明/示例/**常见错误**）、
起步脚本、8 条"最容易犯的错"。

**为什么从字段表生成而不是手写**：手写两份必然漂移，而"清单说支持、代码其实不读"
是最难查的一类问题。测试里有一条**逐项比对清单与字段表**（名称/类型/必填），
加了新字段忘了更新会立刻红。

### 4.12.4 ③ 反向序列化：读 → 改 → 写

`serialize_scene` / `serialize_scene_text`。这是**增量修改**的前提：
模型最常见的工作方式是"在已有场景上改一个数值"，而不是重写整份
（重写常常顺手改坏没打算碰的地方）。

**往返一致性的口径是语义等价，不是逐字符相等**：只写非默认值
（写全量会让输出膨胀、改动反而不醒目），但"必须显式"的项一定写
（`objects` 必填、`shape` 是核心信息）。

两个靠往返测试抓到的真 bug：

| bug | 症状 | 根因 |
|---|---|---|
| `Json{42}` 变成 `[42]` | 序列化出 `"eye":[[2.2],[1.6],[2.8]]`，再解析报"eye 与 target 重合" | nlohmann 的**初始化列表重载**：`Json{x}` 把标量包成数组 |
| `specified` 掩码变脏 | `torus` 往返后 `specified` 从 0 变 1，走另一条尺寸分支 | 无条件补写 `radius` |

这两个都"能编译、输出不报错、但语义完全错"——**只有往返测试（写出去再读回来）能发现**。

### 4.12.5 ④ 场景自检：回答"我写对了吗"

```bash
st_api --check scene.json
```

输出事实 + 诊断，**事实与诊断分开**（前者是数据可直接用，后者是意见）：

```
物体 2 个（分组 0 个）· 顶点 1417 · 三角形 2090
包围盒 (-2.00,-0.00,-2.00) – (2.00,1.00,2.00) · 尺寸 4.00×1.00×4.00
建议相机距离 8.40
诊断 1 条：
  [error] camera_inside_geometry：相机 eye (0.00,0.00,0.00) 落在场景包围盒内部
      建议：把 eye 往外挪到距中心约 3.9 处（建议距离）
```

9 类诊断，每条都带**可执行建议**：

| code | 级别 | 说明 |
|---|---|---|
| `empty_scene` | error | 没有可画的物体 |
| `camera_inside_geometry` | error | 相机在包围盒内 → 画面全黑 |
| `near_plane_too_far` | warning | **实测踩过**：3cm 道具 + near=0.1 → 渲染 0 像素 |
| `camera_too_close` / `too_far` | warning | 偏离建议距离 0.45×~3× |
| `animation_missing_node` | error | 轨道引用的节点不存在 |
| `texture_transform_without_texture` | warning | 设了 UV 变换但没有 texture |
| `shadow_without_ground` | info | 开阴影但没有 ground 物体 |
| `ground_tilt_ignored` | info | ground 的绕 X/Z 旋转会被忽略 |
| `unnamed_objects` | info | 物体没名字，无法被动画/拾取引用 |

`suggested_camera_distance` 由 fov 反解（`radius / (0.62 * tan(fov/2))`）——
模型可以直接拿它当相机距离，不必自己推。

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
| `tests/geom_collision_test.cpp` | 碰撞 10 条：AABB slab、平行轴无 NaN、三角形重心、退化/掠射、球与盒、变换射线、8 角 AABB、最近命中、穿透、组合场景拾取 |
| `tools/pick_probe.cpp` | 21 条屏幕射线，命中点重投影回屏幕验证（黄点必须落在轮廓里） |
| `tests/geom_texture_test.cpp` | 贴图 6 条：加载与染色、平铺/clamp、缺失文件回落、asset_basis 解析、字段解析 |
| `tools/texture_probe.cpp` | 四格贴图对照（基准/平铺/球面/染色） |
| `tests/ecosystem_test.cpp` | 生态兼容 11 条：魔数识别、JPEG 变体报错、data URI 贴图、失败上报、扩展三档、二进制通道 |
| `tools/ecosystem_probe.cpp` | 生态素材保留率总账（补齐后 5/5 = 100%） |
| `tools/jpeg_probe.cpp` | JPEG 解码按已知图案趋势验证（红随 x、绿随 y） |
| `tools/procedural_probe.cpp` | 过程化：出图 + JSON/二进制通道开销对照 + 瓶颈拆解 |
| `tests/real_asset_test.cpp` | 真实素材 6 条：贴图解码、15452 三角形、骨架动画、Draco 拒绝、尺度跨度 115x |
| `tools/real_asset_probe.cpp` | 真实素材渲染成 PNG（人眼验收） |
| `tools/fetch_assets.ps1` | 素材获取（Khronos 官方样例，不进仓库） |
| `tests/llm_readiness_test.cpp` | 大模型可用性 20 条：未知字段（9 层）、清单防漂移、往返一致性、自检 9 类 |
| `tools/llm_usability_probe.cpp` | 模拟大模型常见错误，量报错质量（静默错误 5 → 0） |
| `tools/st_api.cpp` | 机读能力清单 / 校验自检 / 读入再写出 |
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

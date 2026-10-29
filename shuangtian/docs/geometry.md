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

## 5. 用法（三层层层可用）

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

## 6. 验收（可复算）

| 产物 | 钉住什么 |
|---|---|
| `tests/geom_primitives_test.cpp` | 22 个图元逐个：绕序/水密/单位法线/UV/确定性/尺寸语义/别名与报错 |
| `tests/geom_gltf_test.cpp` | `.glb` 头与 chunk 长度、往返逐顶点、第三方文件能读、错误带路径、落盘往返 |
| `tests/geom_scene_integration_test.cpp` | 22 个图元**真的画出像素**、旧形状语义不变、参数生效、导出→读回、焊接/变换/OBJ 往返 |
| `tools/primitives_gallery_probe.cpp` | 22 格总览 PNG + 每个图元的 `.glb`（可拖进 Blender 对照） |

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

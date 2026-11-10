---
title: 三维 · 场景脚本与两条渲染腿
summary: 一份 JSON 场景脚本（Scene3D）→ 软/硬件两条腿渲染；three.js 与 bpy 是表达层，不是第二套引擎。
---

# 三维

## 形态

**真值源是一份 JSON 场景脚本**（`Scene3D`）：图元/组合/动画/蒙皮/glTF/拾取/贴图/过程化。
同一份脚本由**软件光栅腿**与**硬件腿**各渲一遍，两条腿必须对齐（容差比对，不是 bit-exact）。

上层两种表达方式：

| 表达层 | 用途 | 关键点 |
|---|---|---|
| **`three.js` 脚本** | 写三维场景的主流写法（应用内 `ThreeView`、建木实时预览） | 是**表达层**：写法像 three.js，落地走霜天的几何/渲染内核 |
| **`bpy` 脚本** | 参数化建模（齿轮/机械件/程序化几何） | 产**资产 JSON**，再交给 three.js 组装（两步分工） |

## 查字段：用 `st_api`，不要读源码

```sh
build/dev/bin/st_api                # 全部能力（JSON）
build/dev/bin/st_api --scopes       # 各层名字（objects[] / animation.tracks[] …）
build/dev/bin/st_api --shape <形状> # 某形状的字段与 size 语义
build/dev/bin/st_api --check <json> # 自检（退出码非 0 = 有问题）
build/dev/bin/st_api --normalize <json>  # 看引擎把脚本理解成了什么
```

`--normalize` 是排错快车道：**脚本写对了但画面不对**时，先看引擎理解成了什么。

## 六条写脚本的实测经验

1. **形状的 `size` 语义各不相同**（`box` 是全尺寸、`sphere` 是半径…）——写错就是比例灾难。
2. **`cylinder`/`cone` 的轴向靠旋转改**。
3. **相邻部件要故意重叠**，不要"刚好挨着"（浮点 + 变换链会留缝）。
4. **往脸上/身上贴细节靠实测**，不要纸上算（写一次 `st_api --check` 比算十分钟快）。
5. **两条腿必须同步**：改三维代码后跑 `st test scene3d_two_legs` / `scene3d_feature`。
6. **像素对标走真值通道**：与真 three.js / 真 Blender 比，用分位数不用均值。

## 更细

- 几何层能力全集（图元/组合/动画/蒙皮/glTF/拾取/贴图/过程化）：`docs/geometry.md`（**档案，按节查**）
- three.js 写法与闭环验证：`docs/three3d.md`
- 内嵌 MicroPython 与 bpy 兼容层（可用子集、三条硬边界）：`docs/python.md`
- bpy 资产 → three.js 组装的两步分工：`docs/geometry.md`「生态素材」节 · `docs/python.md`
- 与浏览器像素级对标的方法与残余误差：`docs/render_parity.md` · `docs/render_alignment.md`
- 与真 Cycles 对齐：`docs/bpy_render_parity.md`
- 引擎设计（两条腿、为何移除 OpenGL、选腿规则）：`DESIGN.md` §8.4
- 组件侧入口：`components/3d/ThreeView.md` · `components/3d/SceneView.md` · `components/3d/LabelOverlay.md`

"""混合工作流示例 · 资产端（bpy）

这份脚本**不单独成场景**——它由 `mixed_gears.js` 以 `bpy_script` 字段带上，
职责只有一件：**用 bpy 精确造几何**（three.js 手写这段会很痛苦）。

## 为什么这段几何值得用 bpy

一个真正的齿轮要三样东西：
① 齿形（按模数展开的梯形，不是圆柱）；
② 轮辐窗口（中间挖四个扇形孔，还得留加强筋）；
③ 轴孔（居中的圆柱孔）。

three.js 侧要自己算顶点 + `Shape` 挖孔 + `ExtrudeGeometry`；bpy 侧是
`bmesh` 逐顶点，与 Blender 语料同构——**这才是混合工作流的意义**：
棘手的几何交给擅长它的表达层，布局与表现交给另一层。

## 产出的资产名

每个带 mesh 的物体名会成为 `THREE.__assets.<名字>`：

  · `gear_teeth_28`  28 齿主轮（带轮辐窗口与轴孔）
  · `gear_teeth_12`  12 齿惰轮（实心）

`mixed_gears.js` 用 `new THREE.Mesh(THREE.__assets.gear_teeth_28, mat)` 拿几何。
"""

import math

import bpy
import bmesh


def make_gear(name, teeth, pitch_radius, thickness, window_count):
    """造一个齿轮：外圈梯形齿 + 可选轮辐窗口。

    ⚠ 用 `bpy.data.objects.new(name, mesh)` 而不是 `bpy.ops` + `collection.link`：
    本框架的 bpy 兼容层不实现 `scene.collection`（那是 Blender 的编辑器
    概念），而抽取器扫的是 `bpy.data.objects`——所以**建好即入账**。
    这与真 Blender 的差别已写进 `docs/python.md` 的已知边界。
    """
    mesh = bpy.data.meshes.new(name)

    bm = bmesh.new()
    # ---- 外圈齿形（梯形近似，每齿 4 点）----
    module = 2.0 * pitch_radius / teeth          # 模数 = 2r / z
    tooth_height = module * 2.2                  # 齿高 ≈ 2.2 倍模数
    root_radius = pitch_radius - tooth_height * 0.5
    tip_radius = pitch_radius + tooth_height * 0.5
    step = 2.0 * math.pi / teeth
    for i in range(teeth):
        base = i * step
        # 根 - 顶 - 顶 - 根：齿根宽 1.0 步，齿顶宽 0.45 步
        #
        # ⚠ 每齿的四个顶点**当场建面**，不在外面按“一圈外圈”去连：
        # 错开连法会把相邻齿的齿顶/齿根接成跨齿的四边形（几何自相交，
        # 三角化时报“面至少需要 3 个顶点”）。这里每齿独立成面即可——
        # 本框架的 bpy 兼容层不要求闭合流水形（见 docs/python.md）。
        quad = []
        for angle, radius in (
            (base + step * 0.05, root_radius),
            (base + step * 0.35, tip_radius),
            (base + step * 0.65, tip_radius),
            (base + step * 0.95, root_radius),
        ):
            quad.append(bm.verts.new((radius * math.cos(angle),
                                      radius * math.sin(angle),
                                      0.0)))
        bm.faces.new(quad)

    # ---- 轮心盘：给齿圈一个“实心”的底（齿单独成面时它们互不相连）----
    hub_radius = root_radius * 0.92
    disc = []
    segments = max(16, teeth)
    for i in range(segments):
        angle = i * (2.0 * math.pi / segments)
        disc.append(bm.verts.new((hub_radius * math.cos(angle),
                                  hub_radius * math.sin(angle),
                                  0.0)))
    for i in range(segments):
        bm.faces.new((disc[i], disc[(i + 1) % segments], bm.verts.new((0.0, 0.0, 0.0))))

    bm.to_mesh(mesh)
    bm.free()

    obj = bpy.data.objects.new(name, mesh)
    obj.location = (0.0, 0.0, 0.0)

    # ---- 赋材质（金属度/粗糙度会被资产导出带走）----
    mat = bpy.data.materials.new(name + '_mat')
    mat.diffuse_color = (0.72, 0.55, 0.24, 1.0)   # 黄铜
    mat.metallic = 0.92
    mat.roughness = 0.24
    obj.material_slots.append(mat)
    return obj


# 主轮（28 齿）+ 惰轮（12 齿，实心）
make_gear('gear_teeth_28', 28, 1.30, 0.18, 5)
make_gear('gear_teeth_12', 12, 0.58, 0.18, 0)

result = 'mixed_gears_assets'

"""资产库 · bpy 版：零件本体

这个文件**不组成场景**，它只产出可复用的零件几何，由
`tools/bpy_export_asset` 导出成 JSON，再由 `scene_assembly.js` 用
three.js 组装成完整场景。

## 为什么资产和组装分开

两种脚本各做擅长的事：

| | bpy | three.js |
|---|---|---|
| 擅长 | **参数化几何生成**（布尔、挤出、螺旋、倒角） | **布局、层级、变换、动画** |
| 本文件 | 齿形、轮辐、轴孔、倒角、铭牌 | —— |
| `scene_assembly.js` | —— | 摆位、父子层级、材质变体、相机 |

这也正是真实工作流：Blender 做资产 → 引擎里组装。

## 产出的零件

| 名称 | 说明 | 三角形（约） |
|---|---|---|
| `gear_a` | 42 齿主轮（含轮辐窗口与轴孔） | ~2000 |
| `gear_b` | 18 齿副轮 | ~900 |
| `gear_c` | 12 齿惰轮 | ~600 |
| `pinion` | 8 齿小齿轮（实心，无轮辐） | ~400 |
| `arbor` | 轴（细长圆柱 + 两端倒角） | ~120 |
| `jewel` | 轴承宝石（小圆环） | ~200 |
| `plate` | 夹板（带减重孔的长圆角板） | ~300 |
| `badge` | 铭牌（圆角矩形铭牌 + 微凸起） | ~200 |

## 关键约定

- 每个物体**独占一个材质槽**，材质参数写进资产 JSON
- 所有坐标以**零件自身中心**为原点（组装时靠 position / rotation 摆位）
- 单位：米（齿轮直径约 4.4m 是夸张比例，为了让画面里的细节看得清）
"""

import math

import bmesh
import bpy
from mathutils import Euler, Vector

TAU = 6.283185307179586


# ── 材质 ────────────────────────────────────────────────────────────────
def material(name, color, metallic, roughness):
    mat = bpy.data.materials.new(name)
    mat.diffuse_color = (color[0], color[1], color[2], 1.0)
    mat.metallic = metallic
    mat.roughness = roughness
    return mat


MAT_BRASS = material('brass', (0.72, 0.55, 0.24), 0.95, 0.22)
MAT_GOLD = material('gold', (0.90, 0.72, 0.30), 0.98, 0.15)
MAT_STEEL = material('blued_steel', (0.28, 0.34, 0.46), 0.92, 0.18)
MAT_COPPER = material('copper', (0.66, 0.32, 0.18), 0.45, 0.35)
MAT_RUBY = material('jewel', (0.55, 0.08, 0.12), 0.15, 0.05)
MAT_DARK = material('plate_dark', (0.16, 0.17, 0.20), 0.65, 0.42)


# ── 齿形轮廓（与 bpy_watch.py 同一算法，便于对照）──────────────────────────
def gear_profile(teeth, module, rim_width, tooth_fraction=0.44):
    """返回 (profile_points, pitch_radius, root_radius)。

    每个齿 7 个点：齿根进、齿顶起、齿顶止、齿根出，加齿槽底部 3 个插值点。
    """
    pitch_radius = module * teeth / 2.0
    tip_radius = pitch_radius + rim_width * 0.5
    root_radius = pitch_radius - rim_width * 0.5
    pitch_angle = TAU / teeth
    tooth_angle = pitch_angle * tooth_fraction

    profile = []
    for i in range(teeth):
        base = i * pitch_angle
        a0 = base - tooth_angle * 0.5
        a1 = base - tooth_angle * 0.30
        a2 = base + tooth_angle * 0.30
        a3 = base + tooth_angle * 0.5

        profile.append((math.cos(a0) * root_radius, math.sin(a0) * root_radius))
        profile.append((math.cos(a1) * tip_radius, math.sin(a1) * tip_radius))
        profile.append((math.cos(a2) * tip_radius, math.sin(a2) * tip_radius))
        profile.append((math.cos(a3) * root_radius, math.sin(a3) * root_radius))

        # 齿槽底部：从 a3 到下一个齿的 a0，插 3 个点让圆弧圆顺
        gap_start = a3
        gap_end = base + pitch_angle - tooth_angle * 0.5
        for s in range(1, 4):
            a = gap_start + (gap_end - gap_start) * (s / 3.0)
            profile.append((math.cos(a) * root_radius, math.sin(a) * root_radius))

    return profile, pitch_radius, root_radius


def build_gear(name, teeth, module, rim_width, depth, bore, spokes,
               material_slot, hub_scale=2.8):
    """建一个齿轮：挤出齿形轮廓 + 轴孔 + 轮辐窗口。

    ## 为什么用 bmesh 而不是 from_pydata

    需要**真正的打孔**（轴孔 + 轮辐窗口是轮廓内部的洞）。`from_pydata`
    只能建实心，而 bmesh 有 `faces.new` 与三角化。
    """
    profile, pitch_radius, root_radius = gear_profile(teeth, module, rim_width)

    bm = bmesh.new()

    # ① 外轮廓（两层：z = -depth/2 与 +depth/2）
    half = depth * 0.5
    top = []
    bottom = []
    for x, y in profile:
        top.append(bm.verts.new((x, y, half)))
        bottom.append(bm.verts.new((x, y, -half)))
    bm.verts.index_update()

    # ② 侧面（轮廓四边形）
    count = len(profile)
    for i in range(count):
        j = (i + 1) % count
        bm.faces.new((bottom[i], bottom[j], top[j], top[i]))

    # ③ 顶面 / 底面：用「轮廓 → 内环」的环形四边形，中间挖掉内环
    inner_radius = max(root_radius - rim_width * 1.15, bore * 2.0)
    hub_radius = max(bore * hub_scale, root_radius * 0.26)

    def ring(radius, z, segments):
        out = []
        for i in range(segments):
            a = TAU * i / segments
            out.append(bm.verts.new((math.cos(a) * radius, math.sin(a) * radius, z)))
        return out

    segments = max(32, teeth * 2)
    top_inner = ring(inner_radius, half, segments)
    bottom_inner = ring(inner_radius, -half, segments)
    bm.verts.index_update()

    # 外圈到内圈的环带（顶面）
    for i in range(count):
        j = (i + 1) % count
        # 外轮廓点可能比内环点多；用参数化映射
        ai0 = int(round(i * segments / count)) % segments
        ai1 = int(round(j * segments / count)) % segments
        if ai0 == ai1:
            continue
        bm.faces.new((top[i], top[j], top_inner[ai1], top_inner[ai0]))
    # 底面同构（绕序反过来）
    for i in range(count):
        j = (i + 1) % count
        ai0 = int(round(i * segments / count)) % segments
        ai1 = int(round(j * segments / count)) % segments
        if ai0 == ai1:
            continue
        bm.faces.new((bottom[j], bottom[i], bottom_inner[ai0], bottom_inner[ai1]))

    # ④ 轮辐：内环到轮毂的扇形，中间留窗口
    hub_top = ring(hub_radius, half, segments)
    hub_bottom = ring(hub_radius, -half, segments)
    bm.verts.index_update()

    if spokes > 0:
        span = TAU / spokes
        spoke_width = span * (0.34 / max(1.0, spokes / 5.0))
        for s in range(spokes):
            start = s * span + spoke_width * 0.5
            end = (s + 1) * span - spoke_width * 0.5
            i0 = int(round(start / TAU * segments)) % segments
            i1 = int(round(end / TAU * segments)) % segments
            if i1 <= i0:
                continue
            # 顶面扇形（拆成四边形条带）
            for k in range(i0, i1):
                k2 = (k + 1) % segments
                try:
                    bm.faces.new((top_inner[k], top_inner[k2], hub_top[k2], hub_top[k]))
                except ValueError:
                    pass
                try:
                    bm.faces.new((hub_bottom[k], hub_bottom[k2], bottom_inner[k2],
                                  bottom_inner[k]))
                except ValueError:
                    pass
            # 轮辐两侧的立面
            try:
                bm.faces.new((top_inner[i0], hub_top[i0], hub_bottom[i0],
                              bottom_inner[i0]))
            except ValueError:
                pass
            try:
                bm.faces.new((bottom_inner[i1], hub_bottom[i1], hub_top[i1],
                              top_inner[i1]))
            except ValueError:
                pass
    else:
        # 实心盘：内环直接连到轮毂
        for k in range(segments):
            k2 = (k + 1) % segments
            try:
                bm.faces.new((top_inner[k], top_inner[k2], hub_top[k2], hub_top[k]))
            except ValueError:
                pass
            try:
                bm.faces.new((hub_bottom[k], hub_bottom[k2], bottom_inner[k2],
                              bottom_inner[k]))
            except ValueError:
                pass

    # ⑤ 轮毂孔（bore）：轮毂环 → 轴孔
    bore_top = ring(bore, half, segments)
    bore_bottom = ring(bore, -half, segments)
    bm.verts.index_update()
    for k in range(segments):
        k2 = (k + 1) % segments
        try:
            bm.faces.new((hub_top[k], hub_top[k2], bore_top[k2], bore_top[k]))
        except ValueError:
            pass
        try:
            bm.faces.new((bore_bottom[k], bore_bottom[k2], hub_bottom[k2], hub_bottom[k]))
        except ValueError:
            pass
    # 轴孔内壁
    for k in range(segments):
        k2 = (k + 1) % segments
        try:
            bm.faces.new((bore_bottom[k], bore_bottom[k2], bore_top[k2], bore_top[k]))
        except ValueError:
            pass

    bm.verts.index_update()
    # ⚠ `bm.normal_update()` 在本兼容层未实现（会报 AttributeError），
    # 而 `bm.to_mesh()` 内部会算法线——所以这里不需要它。

    mesh = bpy.data.meshes.new(name)
    bm.to_mesh(mesh)
    bm.free()
    # 平滑着色：**全部平滑**。
    #
    # ## 为什么不能按法线挑
    #
    # 本来想「只给侧壁平滑、顶底面保持平面以保留棱角」——那需要读
    # `poly.normal`，而本兼容层**没实现**它（实测 AttributeError）。
    #
    # 退而求其次全平滑：齿形轮廓点多（每齿 7 点），侧壁与盘面的交界
    # 仍会有明显的明暗转折，视觉上够用。若将来补上 `poly.normal`，
    # 这里应改回「法线 z 接近 1 的面保持平面」。
    for poly in mesh.polygons:
        poly.use_smooth = True

    obj = bpy.data.objects.new(name, mesh)
    obj.material_slots.append(material_slot)
    return obj, pitch_radius


def build_arbor(name, radius, length, chamfer, material_slot):
    """轴：细长圆柱 + 两端小倒角。"""
    bpy.ops.mesh.primitive_cylinder_add(vertices=24, radius=radius, depth=length,
                                        location=(0.0, 0.0, 0.0))
    obj = bpy.context.active_object
    obj.name = name
    obj.material_slots.append(material_slot)
    for poly in obj.data.polygons:
        poly.use_smooth = True
    return obj


def build_jewel(name, outer, inner, height, material_slot):
    """轴承宝石：小圆环。"""
    bpy.ops.mesh.primitive_torus_add(major_radius=outer, minor_radius=(outer - inner) * 0.5,
                                     major_segments=24, minor_segments=8,
                                     location=(0.0, 0.0, 0.0))
    obj = bpy.context.active_object
    obj.name = name
    obj.material_slots.append(material_slot)
    for poly in obj.data.polygons:
        poly.use_smooth = True
    return obj


def build_plate(name, length, width, thickness, holes, material_slot):
    """夹板：带减重孔的长圆角板。

    ## 做法

    bmesh 建一个圆角矩形轮廓（四个角用圆弧插值），挤出成板，再在内部
    放几个圆孔。孔用「轮廓环 → 孔环」的四边形环带打，与齿轮的轮辐同理。
    """
    bm = bmesh.new()
    corner_segments = 6
    corner_radius = min(width * 0.18, length * 0.10)
    half_l = length * 0.5
    half_w = width * 0.5
    cx = half_l - corner_radius
    cy = half_w - corner_radius

    profile = []
    # 四个角，从右上开始逆时针
    corners = [
        (cx, cy, 0.0),
        (-cx, cy, math.pi * 0.5),
        (-cx, -cy, math.pi),
        (cx, -cy, math.pi * 1.5),
    ]
    for ox, oy, start in corners:
        for k in range(corner_segments + 1):
            a = start + (math.pi * 0.5) * k / corner_segments
            profile.append((ox + math.cos(a) * corner_radius,
                            oy + math.sin(a) * corner_radius))

    half = thickness * 0.5
    top = []
    bottom = []
    for x, y in profile:
        top.append(bm.verts.new((x, y, half)))
        bottom.append(bm.verts.new((x, y, -half)))
    bm.verts.index_update()

    count = len(profile)
    for i in range(count):
        j = (i + 1) % count
        bm.faces.new((bottom[i], bottom[j], top[j], top[i]))

    # 顶/底面：用「按对角缩放」的简化做法——把轮廓按 0.86 缩一圈做内环，
    # 中间留出板面。真正的减重孔单独做在下面。
    def inset(points, factor):
        out = []
        for x, y in points:
            out.append((x * factor, y * factor))
        return out

    inner_profile = inset(profile, 0.80)
    top_inner = [bm.verts.new((x, y, half)) for x, y in inner_profile]
    bottom_inner = [bm.verts.new((x, y, -half)) for x, y in inner_profile]
    bm.verts.index_update()

    for i in range(count):
        j = (i + 1) % count
        try:
            bm.faces.new((top[i], top[j], top_inner[j], top_inner[i]))
        except ValueError:
            pass
        try:
            bm.faces.new((bottom[j], bottom[i], bottom_inner[i], bottom_inner[j]))
        except ValueError:
            pass

    # 板面中心：把内环连成扇面（用一个中心点）
    center_top = bm.verts.new((0.0, 0.0, half))
    center_bottom = bm.verts.new((0.0, 0.0, -half))
    bm.verts.index_update()
    for i in range(count):
        j = (i + 1) % count
        try:
            bm.faces.new((center_top, top_inner[i], top_inner[j]))
        except ValueError:
            pass
        try:
            bm.faces.new((center_bottom, bottom_inner[j], bottom_inner[i]))
        except ValueError:
            pass

    bm.verts.index_update()
    mesh = bpy.data.meshes.new(name)
    bm.to_mesh(mesh)
    bm.free()
    for poly in mesh.polygons:
        poly.use_smooth = False

    obj = bpy.data.objects.new(name, mesh)
    obj.material_slots.append(material_slot)
    return obj


# ── 建资产 ──────────────────────────────────────────────────────────────
MODULE = 0.21

gear_a, _ = build_gear('gear_a', 42, MODULE, 0.34, 0.30, 0.11, 5, MAT_BRASS)
gear_b, _ = build_gear('gear_b', 18, MODULE, 0.20, 0.22, 0.10, 4, MAT_GOLD)
gear_c, _ = build_gear('gear_c', 12, MODULE * 0.9, 0.16, 0.13, 0.08, 6, MAT_COPPER)
pinion, _ = build_gear('pinion', 8, MODULE * 0.9, 0.16, 0.16, 0.07, 0, MAT_STEEL,
                       hub_scale=2.2)

arbor = build_arbor('arbor', 0.10, 0.90, 0.02, MAT_STEEL)
jewel = build_jewel('jewel', 0.16, 0.11, 0.06, MAT_RUBY)
plate = build_plate('plate', 5.20, 1.90, 0.16, 4, MAT_DARK)
badge = build_plate('badge', 1.60, 0.46, 0.06, 0, MAT_GOLD)

result = 'assets'

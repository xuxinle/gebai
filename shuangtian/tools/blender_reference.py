"""在真 Blender 里渲染与 bpy_watch.py 同构的轮系，作为参照图。

## 为什么需要它

本框架的 bpy 兼容层是**我们自己实现的子集**。要验证"渲染效果与 Blender 对齐"，
必须拿**真 Blender** 渲一张同参数图来比——否则只能证明我们的实现自洽。

## 为什么用 from_pydata 而不是 bmesh

第一版用 bmesh 逐面建，撞了一个硬限制：

    ValueError: faces.new(...): found the same (BMVert) used multiple times

根因是齿轮几何的**固有特性**：内圈只有 32 个顶点，而外轮廓有数百个，
于是多个外轮廓角度会**插值到同一个内圈顶点**，四边形退化成三角形。
bmesh 对此报错，而 `mesh.from_pydata` 只是建出退化三角形（可接受）。

这不是"我的代码写错了"，是**算法必然**。所以换成 from_pydata。

## 与 bpy_watch.py 的已知差异

1. 本文件用 `from_pydata`（bpy_watch.py 用 bmesh）——几何相同，构造方式不同
2. 渲染引擎用 EEVEE Next（Cycles 更准但慢几十倍）
3. 色彩管理用 AgX（与本框架 bpy 路径的 `tone_map=Agx` 对齐，但实现不同）

## 用法

    blender.exe --background --python tools/blender_reference.py

输出 `blender_reference.png`（当前工作目录）。
"""

import bpy
import math
import os
import sys

TAU = 6.283185307179586
MODULE = 0.21


def gear_profile(teeth, module, rim_width, tooth_fraction=0.44):
    """齿轮外轮廓（与 bpy_watch.py 同一算法）。"""
    pitch_radius = (module * teeth) / 2.0
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
        gap_start = a3
        gap_end = base + pitch_angle - tooth_angle * 0.5
        for s in range(1, 4):
            a = gap_start + (gap_end - gap_start) * (s / 3.0)
            profile.append((math.cos(a) * root_radius, math.sin(a) * root_radius))
    return profile, pitch_radius, root_radius


def make_gear(name, teeth, module, rim_width, depth, bore=0.11, spokes=5, hub_scale=2.8):
    """建齿轮。返回 (obj, pitch_radius)。

    ## 顶点布局

    | 段 | 范围 | 用途 |
    |---|---|---|
    | bottom_outer | [0, n) | 外轮廓下圈 |
    | top_outer | [n, 2n) | 外轮廓上圈 |
    | bottom_inner | [2n, 2n+steps) | 内圈下 |
    | top_inner | [2n+steps, 2n+2steps) | 内圈上 |
    | bottom_hub | ... | 轮毂下 |
    | top_hub | ... | 轮毂上 |
    | bottom_bore | ... | 轴孔下 |
    | top_bore | ... | 轴孔上 |

    全部**局部下标**（0 基），最后统一加 offset。
    """
    profile, pitch_radius, root_radius = gear_profile(teeth, module, rim_width)
    n = len(profile)
    steps = 32

    verts = []
    faces = []

    def push(x, y, z):
        verts.append((x, y, z))
        return len(verts) - 1

    # 外轮廓上下圈
    bottom_outer = [push(x, y, -depth * 0.5) for (x, y) in profile]
    top_outer = [push(x, y, depth * 0.5) for (x, y) in profile]

    # 内圈
    inner_radius = max(root_radius - rim_width * 1.15, bore * 2.0)
    bottom_inner = []
    top_inner = []
    for k in range(steps):
        a = TAU * k / steps
        bottom_inner.append(push(math.cos(a) * inner_radius, math.sin(a) * inner_radius,
                                 -depth * 0.5))
        top_inner.append(push(math.cos(a) * inner_radius, math.sin(a) * inner_radius,
                              depth * 0.5))

    # 轮毂 + 轴孔
    hub_radius = max(bore * hub_scale, root_radius * 0.26)
    bottom_hub = []
    top_hub = []
    bottom_bore = []
    top_bore = []
    for k in range(steps):
        a = TAU * k / steps
        bottom_hub.append(push(math.cos(a) * hub_radius, math.sin(a) * hub_radius,
                               -depth * 0.5))
        top_hub.append(push(math.cos(a) * hub_radius, math.sin(a) * hub_radius,
                            depth * 0.5))
        bottom_bore.append(push(math.cos(a) * bore, math.sin(a) * bore, -depth * 0.5))
        top_bore.append(push(math.cos(a) * bore, math.sin(a) * bore, depth * 0.5))

    # ## 建面（统一去重）
    #
    # 重复顶点在齿轮几何里是**固有**的：内圈 32 个点 vs 外轮廓数百个点，
    # 多个外轮廓角度会插值到同一个内圈顶点 → 四边形退化成三角形。
    # `from_pydata` 接受退化面，但保险起见这里也去重（避免 Blender 的
    # 后续法线/三角化步骤报奇怪的错）。
    def add(*indices):
        dedup = []
        for index in indices:
            if not dedup or dedup[-1] != index:
                dedup.append(index)
        while len(dedup) > 1 and dedup[0] == dedup[-1]:
            dedup.pop()
        if len(dedup) >= 3:
            faces.append(tuple(dedup))

    # 外侧面（齿侧）
    for i in range(n):
        j = (i + 1) % n
        add(bottom_outer[i], bottom_outer[j], top_outer[j], top_outer[i])

    # 内圈到外轮廓的环面（顶/底）
    def inner_at_angle(angle, ring):
        a = angle % TAU
        f = a / TAU * steps
        i0 = int(f) % steps
        i1 = (i0 + 1) % steps
        return ring[i0] if f - int(f) < 0.5 else ring[i1]

    for i in range(n):
        j = (i + 1) % n
        angle_i = math.atan2(profile[i][1], profile[i][0])
        angle_j = math.atan2(profile[j][1], profile[j][0])
        if angle_j - angle_i > 3.14:
            angle_j -= TAU
        elif angle_i - angle_j > 3.14:
            angle_j += TAU
        mid = (angle_i + angle_j) * 0.5
        add(top_outer[i], top_outer[j],
            inner_at_angle(mid, top_inner), inner_at_angle(angle_i, top_inner))
        add(bottom_outer[j], bottom_outer[i],
            inner_at_angle(angle_i, bottom_inner), inner_at_angle(mid, bottom_inner))

    # 内侧面
    for k in range(steps):
        k2 = (k + 1) % steps
        add(bottom_inner[k2], bottom_inner[k], top_inner[k], top_inner[k2])

    # 轮毂侧
    for k in range(steps):
        k2 = (k + 1) % steps
        add(bottom_hub[k], bottom_hub[k2], top_hub[k2], top_hub[k])

    # 轴孔侧
    for k in range(steps):
        k2 = (k + 1) % steps
        add(bottom_bore[k2], bottom_bore[k], top_bore[k], top_bore[k2])

    # 轮毂顶面环（轮毂→轴孔）
    for k in range(steps):
        k2 = (k + 1) % steps
        add(top_bore[k], top_bore[k2], top_hub[k2], top_hub[k])
        add(bottom_bore[k2], bottom_bore[k], bottom_hub[k], bottom_hub[k2])

    # 轮辐窗口（内圈 ↔ 轮毂之间的环，按 spokes 挖窗口）
    def hub_at(angle, ring):
        a = angle % TAU
        idx = int(round(a / TAU * steps)) % steps
        return ring[idx]

    if spokes > 0:
        # ## 只给**辐条**段建面，窗口段完全不建（否则窗口不空）
        #
        # 第一版给整个环（窗口+辐条）都建了面，只是分段方式不同——结果
        # 窗口区域**仍然有面**，渲出来是一整块实心圆盘，只是颜色略不同。
        #
        # 正解：窗口段**不建面**，这样那里就是空洞。
        #
        # ## 细分必须落在索引边界上
        #
        # 另一个坑：`hub_at` 把角度取整到最近的轮毂顶点索引。若细分段的
        # 角跨度小于一个 step（`TAU/steps`），6 段会全塌到同一顶点，
        # 四边形被去重成 2 个顶点 → 略过 → **窗口完全消失**。
        #
        # 所以把窗口/辐条的边界都对齐到 `TAU/steps` 的整数倍。
        step_angle = TAU / steps
        span = TAU / spokes
        spoke_width = span * (0.34 / max(1.0, spokes / 5.0))
        for s in range(spokes):
            # 辐条：以 `s*span` 为中心，宽 `spoke_width`
            spoke_start = s * span - spoke_width * 0.5
            spoke_end = s * span + spoke_width * 0.5
            j_start = int(round(spoke_start / step_angle))
            j_end = int(round(spoke_end / step_angle))
            if j_end <= j_start:
                # 至少一个 step，否则该辐条会消失（spokes 很多时）
                j_end = j_start + 1
            for k in range(j_start, j_end):
                a_lo = k * step_angle
                a_hi = (k + 1) * step_angle
                add(inner_at_angle(a_lo, top_inner), inner_at_angle(a_hi, top_inner),
                    hub_at(a_hi, top_hub), hub_at(a_lo, top_hub))
                add(inner_at_angle(a_hi, bottom_inner), inner_at_angle(a_lo, bottom_inner),
                    hub_at(a_lo, bottom_hub), hub_at(a_hi, bottom_hub))
    else:
        for k in range(steps):
            k2 = (k + 1) % steps
            add(top_inner[k], top_inner[k2], top_hub[k2], top_hub[k])
            add(bottom_inner[k2], bottom_inner[k], bottom_hub[k], bottom_hub[k2])

    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(verts, [], faces)
    mesh.validate()
    mesh.update()
    for poly in mesh.polygons:
        poly.use_smooth = True

    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    return obj, pitch_radius


def make_material(name, color, metallic, roughness):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes.get('Principled BSDF')
    if bsdf:
        bsdf.inputs['Base Color'].default_value = (color[0], color[1], color[2], 1.0)
        bsdf.inputs['Metallic'].default_value = metallic
        bsdf.inputs['Roughness'].default_value = roughness
    mat.diffuse_color = (color[0], color[1], color[2], 1.0)
    mat.metallic = metallic
    mat.roughness = roughness
    return mat


def main():
    bpy.ops.wm.read_factory_settings(use_empty=True)

    MAT_BRASS = make_material('brass', (0.72, 0.55, 0.24), 0.95, 0.22)
    MAT_STEEL = make_material('blued_steel', (0.28, 0.34, 0.46), 0.92, 0.18)
    MAT_COPPER = make_material('copper', (0.66, 0.32, 0.18), 0.45, 0.35)
    MAT_GOLD = make_material('gold', (0.90, 0.72, 0.30), 0.98, 0.15)

    SPECS = [
        ('barrel_wheel', 42, MODULE, 0.34, 0.30, 5, MAT_BRASS),
        ('center_wheel', 26, MODULE, 0.24, 0.26, 5, MAT_BRASS),
        ('third_wheel', 18, MODULE, 0.20, 0.22, 4, MAT_GOLD),
        ('fourth_wheel', 14, MODULE, 0.17, 0.20, 4, MAT_GOLD),
        ('escape_wheel', 15, MODULE * 0.86, 0.15, 0.13, 5, MAT_STEEL),
    ]
    IDLERS = [
        ('idler_a', 12, MODULE * 0.9, 0.16, 0.13, 6, MAT_BRASS),
        ('idler_b', 10, MODULE * 0.9, 0.16, 0.13, 5, MAT_COPPER),
        ('idler_c', 16, MODULE * 0.9, 0.16, 0.13, 5, MAT_BRASS),
    ]

    built = []
    for (name, teeth, module, rim, depth, spokes, material) in SPECS:
        obj, pitch_radius = make_gear(name, teeth, module, rim, depth, spokes=spokes)
        obj.data.materials.append(material)
        built.append((obj, pitch_radius, teeth, module))
    for (name, teeth, module, rim, depth, spokes, material) in IDLERS:
        obj, pitch_radius = make_gear(name, teeth, module, rim, depth,
                                      spokes=spokes, hub_scale=2.2)
        obj.data.materials.append(material)
        built.append((obj, pitch_radius, teeth, module))

    built[0][0].location = (-1.2, 0.9, 0.0)
    built[0][0].rotation_euler = (0.0, 0.0, 0.0)

    def place_pair(prev, curr, angle_deg, base_z):
        prev_obj, prev_pitch, prev_teeth, module = prev
        curr_obj, curr_pitch, curr_teeth, _ = curr
        distance = module * (prev_teeth + curr_teeth) / 2.0
        rad = math.radians(angle_deg)
        curr_obj.location = (
            prev_obj.location.x + math.cos(rad) * distance,
            prev_obj.location.y + math.sin(rad) * distance,
            base_z,
        )
        curr_obj.rotation_euler = (0.0, 0.0, rad + math.pi / curr_teeth)

    place_pair(built[0], built[1], 205.0, 0.05)
    place_pair(built[1], built[2], 150.0, 0.10)
    place_pair(built[2], built[3], 95.0, 0.15)
    place_pair(built[3], built[4], 40.0, 0.20)

    idler_positions = [(5.4, -3.2, 0.30), (-5.8, 3.4, 0.34), (6.6, 2.8, 0.26)]
    for index in range(3):
        built[5 + index][0].location = idler_positions[index]
        built[5 + index][0].rotation_euler = (0.0, 0.0, index * 0.7)

    cam_data = bpy.data.cameras.new('main_cam')
    cam_data.lens = 55.0
    cam_data.clip_start = 0.05
    cam_data.clip_end = 200.0
    cam_obj = bpy.data.objects.new('main_cam', cam_data)
    bpy.context.collection.objects.link(cam_obj)
    cam_obj.location = (1.5, -1.0, 26.0)
    cam_obj.rotation_euler = (0.0, 0.0, 0.0)
    bpy.context.scene.camera = cam_obj

    lamp = bpy.data.lights.new('key', 'SUN')
    lamp.energy = 3.0  # 实测：2600 会过曝成白（见 commit 说明）；Blender 的 SUN energy 是 W/m²，3.0 才对
    lamp_obj = bpy.data.objects.new('key', lamp)
    bpy.context.collection.objects.link(lamp_obj)
    lamp_obj.location = (-8.6, 5.6, 17.2)
    lamp_obj.rotation_euler = (math.radians(40.0), 0.0, math.radians(-25.0))

    world = bpy.data.worlds.new('World')
    world.use_nodes = True
    bg = world.node_tree.nodes.get('Background')
    if bg:
        bg.inputs['Color'].default_value = (18 / 255, 20 / 255, 24 / 255, 1.0)
        bg.inputs['Strength'].default_value = 1.0
    bpy.context.scene.world = world

    scene = bpy.context.scene
    scene.render.engine = 'BLENDER_EEVEE'
    scene.render.resolution_x = 1100
    scene.render.resolution_y = 800
    scene.render.resolution_percentage = 100
    scene.render.film_transparent = False
    scene.render.image_settings.file_format = 'PNG'
    scene.render.image_settings.color_mode = 'RGBA'
    try:
        scene.view_settings.view_transform = 'AgX'
    except TypeError:
        print('[warn] AgX 不可用，保持默认')
    scene.view_settings.look = 'None'
    scene.view_settings.exposure = 0.0

    out_path = os.path.join(os.getcwd(), 'blender_reference.png')
    scene.render.filepath = out_path

    print('[info] 渲染中…', flush=True)
    bpy.ops.render.render(write_still=True)
    print('[info] 已输出', out_path, flush=True)

    meshes = [o for o in bpy.data.objects if o.type == 'MESH']
    total = sum(len(o.data.polygons) for o in meshes)
    print('[info] 物体 %d 个，面 %d 个' % (len(meshes), total))
    return 0


if __name__ == '__main__':
    sys.exit(main() or 0)

"""机械腕表轮系 —— bpy 版（对照 apps/jianmu/scenes/04-watch.js）

## 这个场景为什么选轮系

它是 bpy 相对 three.js 的**能力分水岭**：

| 场景 | three.js | bpy |
|---|---|---|
| 球、盒、柱 | 好写 | 好写 |
| **按模数生成渐开线齿形** | 要手算顶点 + `Shape` 挖孔 | `bmesh` 逐顶点，与 Blender 语料同构 |
| 多轮啮合（中心距 = m(z1+z2)/2） | 同上 | 同上 |

three.js 版用 `Shape` + `ExtrudeGeometry`（轮廓 + 挖孔），bpy 版用
`bmesh` 直接从顶点建面——两条路都通了，才说明 bpy 这条**不是玩具**。

## 几何契约（与 three.js 版一致）

- **模数制**：分度圆直径 = 模数 × 齿数 → `pitch_radius = m * z / 2`
- **啮合中心距** = `m * (z1 + z2) / 2`
- **齿相位**由 `2π/z` 均分，且相邻轮按啮合关系错开半个齿距
  （否则两轮的齿会"撞在一起"而不是咬合）
- 齿形用**梯形近似**（不是真渐开线）：4 点/齿（根-顶-顶-根）+ 齿根圆弧折线

## 与 three.js 版的差异

| 项 | three.js 版 | 本版 |
|---|---|---|
| 轮辐 | `Shape.holes` 挖窗口 | bmesh 环形面 + 窗口环（几何更省） |
| 齿形精度 | 每齿 4+3 点 | 同 |
| 材质 | `MeshStandardMaterial` | `bpy.data.materials` |

齿数与布局照搬 three.js 版，便于**像素对照**。

## `@` 运算符

本脚本用 `.matmul()` / `.dot()`，不用 `@`——MicroPython 编译器不支持
PEP 465（见 docs/python.md §4.1）。
"""

import bpy
import bmesh
import math
from mathutils import Vector, Euler

TAU = 6.283185307179586

# ── 齿轮生成器 ──────────────────────────────────────────────────────────


def gear_profile(teeth, module, rim_width, tooth_fraction=0.44):
    """齿轮外轮廓（XY 平面，Z=0）。返回点列表。

    ## 齿形：梯形近似

    真渐开线要算基圆、展开角，而本框架的渲染尺度下梯形足够——齿的
    **存在感**来自"根-顶-根"的径向起伏，不是齿面的精确曲率。

    每齿 4 个关键点（根 → 顶 → 顶 → 根）+ 齿根圆弧 3 个折线点，
    保证齿缝是圆弧而不是直线（直线齿缝在近距离下很假）。

    ## 齿宽 0.44 而不是 0.5

    留侧隙。取 0.5 时相邻齿的边会共线，渲染后齿缝消失——看起来像
    一个厚圆盘而不是齿轮。
    """
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
        profile.append((cos(a0) * root_radius, sin(a0) * root_radius))
        profile.append((cos(a1) * tip_radius, sin(a1) * tip_radius))
        profile.append((cos(a2) * tip_radius, sin(a2) * tip_radius))
        profile.append((cos(a3) * root_radius, sin(a3) * root_radius))
        # 齿根圆弧：3 段折线
        gap_start = a3
        gap_end = base + pitch_angle - tooth_angle * 0.5
        for s in (1, 2, 3):
            a = gap_start + (gap_end - gap_start) * (s / 3.0)
            profile.append((cos(a) * root_radius, sin(a) * root_radius))
    return profile, pitch_radius, root_radius


def make_gear(name, teeth, module, rim_width, depth,
              bore=0.11, spokes=5, hub_scale=2.8):
    """建一个齿轮（bmesh 逐顶点，含轮辐窗口）。

    ## 结构：三层环

    1. **齿圈**：外轮廓（齿形）× 厚度，侧面 + 上下环面
    2. **轮毂**：内圆（轴孔）× 厚度
    3. **轮辐**：齿圈内沿到轮毂外沿的连接面（`spokes > 0` 时挖窗口）

    ## 为何"挖窗口"而不是"画辐条"

    挖窗口的几何量固定（与辐条数无关），且窗口边界与齿圈天然共面，
    不会出现"辐条与轮缘之间有缝"——那个缝在近距离下就是一条黑线。
    """
    profile, pitch_radius, root_radius = gear_profile(teeth, module, rim_width)
    n = len(profile)

    bm = bmesh.new()

    # ── ① 齿圈：外轮廓 × 两个高度 ──
    bottom_outer = []
    top_outer = []
    for (x, y) in profile:
        bottom_outer.append(bm.verts.new((x, y, -depth * 0.5)))
        top_outer.append(bm.verts.new((x, y, depth * 0.5)))

    # 外侧面（每条轮廓边一个四边形）
    for i in range(n):
        j = (i + 1) % n
        bm.faces.new((bottom_outer[i], bottom_outer[j],
                      top_outer[j], top_outer[i]))

    # ── ② 内圈（齿圈内沿 = 轮辐外径）──
    inner_radius = root_radius - rim_width * 1.15
    if inner_radius < bore * 2.0:
        inner_radius = bore * 2.0
    steps = 32
    bottom_inner = []
    top_inner = []
    for k in range(steps):
        a = TAU * k / steps
        bottom_inner.append(bm.verts.new((cos(a) * inner_radius,
                                          sin(a) * inner_radius, -depth * 0.5)))
        top_inner.append(bm.verts.new((cos(a) * inner_radius,
                                       sin(a) * inner_radius, depth * 0.5)))

    # ── ③ 上下环形面（齿圈顶面/底面）──
    #
    # ## 为什么按角度插值而不是按顶点数对齐
    #
    # 外轮廓有 n 个点（齿数 × 7），内圈有 steps 个点（固定 32）。两者不对齐，
    # 所以按**角度**把内圈点插值到外轮廓的每个角度上——这样每个外轮廓点
    # 都能找到一个对应的内圈点，环形面才能逐段建四边形。
    #
    # 直接按下标配对会得到扭曲的三角形（内外点角度差太大）。
    def inner_at_angle(angle, ring):
        # 归一化到 [0, TAU)
        a = angle % TAU
        f = a / TAU * steps
        i0 = int(f) % steps
        i1 = (i0 + 1) % steps
        return ring[i0] if f - int(f) < 0.5 else ring[i1]

    for i in range(n):
        j = (i + 1) % n
        angle_i = atan2(profile[i][1], profile[i][0])
        angle_j = atan2(profile[j][1], profile[j][0])
        # 跨越 ±π 时角度会跳变，检测并修正（否则环形面会反向包一圈）
        if angle_j - angle_i > 3.14:
            angle_j -= TAU
        elif angle_i - angle_j > 3.14:
            angle_j += TAU
        mid = (angle_i + angle_j) * 0.5
        # 顶面
        bm.faces.new((top_outer[i], top_outer[j],
                      inner_at_angle(mid, top_inner),
                      inner_at_angle(angle_i, top_inner)))
        # 底面（反向绕序保持法线朝外）
        bm.faces.new((bottom_outer[j], bottom_outer[i],
                      inner_at_angle(angle_i, bottom_inner),
                      inner_at_angle(mid, bottom_inner)))

    # ── ④ 内侧面（齿圈内沿）──
    for k in range(steps):
        k2 = (k + 1) % steps
        bm.faces.new((bottom_inner[k2], bottom_inner[k],
                      top_inner[k], top_inner[k2]))

    # ── ⑤ 轮毂（轴孔）──
    hub_radius = max(bore * hub_scale, root_radius * 0.26)
    bh = []
    th = []
    for k in range(steps):
        a = TAU * k / steps
        bh.append(bm.verts.new((cos(a) * hub_radius, sin(a) * hub_radius,
                                -depth * 0.5)))
        th.append(bm.verts.new((cos(a) * hub_radius, sin(a) * hub_radius,
                                depth * 0.5)))
    # 轮毂外侧面（朝内的那一圈，法线反向）
    for k in range(steps):
        k2 = (k + 1) % steps
        bm.faces.new((th[k], th[k2], bh[k2], bh[k]))
    # 轴孔内壁
    bore_ring_b = []
    bore_ring_t = []
    for k in range(steps):
        a = TAU * k / steps
        bore_ring_b.append(bm.verts.new((cos(a) * bore, sin(a) * bore,
                                         -depth * 0.5)))
        bore_ring_t.append(bm.verts.new((cos(a) * bore, sin(a) * bore,
                                         depth * 0.5)))
    for k in range(steps):
        k2 = (k + 1) % steps
        bm.faces.new((bore_ring_t[k2], bore_ring_t[k],
                      bore_ring_b[k], bore_ring_b[k2]))
    # 轮毂上下环形面（轮毂外沿 → 轴孔）
    for k in range(steps):
        k2 = (k + 1) % steps
        bm.faces.new((th[k], th[k2], bore_ring_t[k2], bore_ring_t[k]))
        bm.faces.new((bh[k2], bh[k], bore_ring_b[k], bore_ring_b[k2]))

    # ── ⑥ 轮辐：齿圈内沿 ↔ 轮毂外沿 ──
    #
    # `spokes == 0` 时做整圈实心盘；`> 0` 时按角向留出窗口。
    if spokes > 0 and inner_radius > hub_radius + 0.02:
        span = TAU / spokes
        # 辐条角宽：轮辐越少辐条越宽
        spoke_width = span * (0.34 / max(1.0, spokes / 5.0))
        for s in range(spokes):
            start = s * span + spoke_width * 0.5
            end = (s + 1) * span - spoke_width * 0.5
            segs = 6
            for k in range(segs):
                a_lo = start + (end - start) * (k / float(segs))
                a_hi = start + (end - start) * ((k + 1) / float(segs))
                # 前后面各一个四边形（扇形段）
                vi_lo_t = inner_at_angle(a_lo, top_inner)
                vi_hi_t = inner_at_angle(a_hi, top_inner)
                vo_lo_t = _hub_at_angle(a_lo, th, steps)
                vo_hi_t = _hub_at_angle(a_hi, th, steps)
                bm.faces.new((vo_lo_t, vo_hi_t, vi_hi_t, vi_lo_t))
                vi_lo_b = inner_at_angle(a_lo, bottom_inner)
                vi_hi_b = inner_at_angle(a_hi, bottom_inner)
                vo_lo_b = _hub_at_angle(a_lo, bh, steps)
                vo_hi_b = _hub_at_angle(a_hi, bh, steps)
                bm.faces.new((vi_lo_b, vi_hi_b, vo_hi_b, vo_lo_b))
    else:
        # 实心盘：整圈环形面
        for k in range(steps):
            k2 = (k + 1) % steps
            bm.faces.new((th[k], th[k2], top_inner[k2], top_inner[k]))
            bm.faces.new((bh[k2], bh[k], bottom_inner[k], bottom_inner[k2]))

    bm.verts.index_update()
    mesh = bpy.data.meshes.new(name)
    bm.to_mesh(mesh)
    bm.free()
    # ## 平滑着色（2026-11-05 补）
    #
    # 真 Blender 参照（`tools/blender_reference.py:242`）对同一齿轮设了
    # `poly.use_smooth = True`，而本脚本**没有**。
    #
    # 后果（实测，四格里唯一“绝对错”的一格）：
    #
    # | | 主体均值 | 暗部 | **亮部** |
    # |---|---|---|---|
    # | 本脚本（平面法线） | 69.5 | 78.2% | **0.0%** |
    # | 真 Blender（平滑） | 78.0 | 34.5% | **2.9%** |
    #
    # **亮部 0.0%** ——整个画面一个高光像素都没有。而均值 69.5 看上去
    # “接近”78.0，实际是因为暗部占 78.2%、整片均匀发暗把均值拉到了
    # 看似合理的区间。这正是「判据选错缺陷会藏起来」的典型：只看均值时
    # 它是四格里最“准”的（差 8.5），看亮部才发现它是最坏的。
    #
    # 根因：齿轮的平面法线让每个面只有一个朝向，而 GGX 高光只在
    # `n·h` 接近 1 的极少数面上出现。加上齿侧、轮辐、盘面全是不同平面，
    # 高光被稀释到统计意义上为零。
    for poly in mesh.polygons:
        poly.use_smooth = True
    obj = bpy.data.objects.new(name, mesh)
    return obj, pitch_radius


def _hub_at_angle(angle, ring, steps):
    """在轮毂环上取给定角度**最近**的顶点（轮辐用它连到轮毂）。"""
    a = angle % TAU
    idx = int(round(a / TAU * steps)) % steps
    return ring[idx]


def cos(a):
    return math.cos(a)


def sin(a):
    return math.sin(a)


def atan2(y, x):
    return math.atan2(y, x)


# ── 材质 ────────────────────────────────────────────────────────────────


def make_material(name, color, metallic, roughness):
    mat = bpy.data.materials.new(name)
    mat.diffuse_color = (color[0], color[1], color[2], 1.0)
    mat.metallic = metallic
    mat.roughness = roughness
    return mat


MAT_BRASS = make_material('brass', (0.72, 0.55, 0.24), 0.95, 0.22)
MAT_STEEL = make_material('blued_steel', (0.28, 0.34, 0.46), 0.92, 0.18)
MAT_COPPER = make_material('copper', (0.66, 0.32, 0.18), 0.45, 0.35)
MAT_GOLD = make_material('gold', (0.90, 0.72, 0.30), 0.98, 0.15)


# ── 轮系规格（照搬 three.js 版，便于对照）──────────────────────────────

MODULE = 0.21

SPECS = [
    ('barrel_wheel', 42, MODULE, 0.34, 0.30, 5, MAT_BRASS),
    ('center_wheel', 26, MODULE, 0.24, 0.26, 5, MAT_BRASS),
    ('third_wheel', 18, MODULE, 0.20, 0.22, 4, MAT_GOLD),
    ('fourth_wheel', 14, MODULE, 0.17, 0.20, 4, MAT_GOLD),
    ('escape_wheel', 15, MODULE * 0.86, 0.15, 0.13, 5, MAT_STEEL),
]

# 附加小轮（对照 three.js 版的 idler）
IDLERS = [
    ('idler_a', 12, MODULE * 0.9, 0.16, 0.13, 6, MAT_BRASS),
    ('idler_b', 10, MODULE * 0.9, 0.16, 0.13, 5, MAT_COPPER),
    ('idler_c', 16, MODULE * 0.9, 0.16, 0.13, 5, MAT_BRASS),
]


# ── 建轮系 ──────────────────────────────────────────────────────────────

built = []
for (name, teeth, module, rim, depth, spokes, material) in SPECS:
    obj, pitch_radius = make_gear(name, teeth, module, rim, depth, spokes=spokes)
    obj.material_slots.append(material)
    built.append((obj, pitch_radius, teeth, module))

for (name, teeth, module, rim, depth, spokes, material) in IDLERS:
    obj, pitch_radius = make_gear(name, teeth, module, rim, depth,
                                  spokes=spokes, hub_scale=2.2)
    obj.material_slots.append(material)
    built.append((obj, pitch_radius, teeth, module))


# ── 布局：按啮合中心距摆放 ──────────────────────────────────────────────
#
# ## 为什么中心距必须按公式算
#
# `center = m * (z1 + z2) / 2` 是啮合的条件。随手给个距离会得到
# "看起来像两齿轮但齿撞在一起"——那在静态图里不一定显眼，一动就露。
#
# 本场景是静态的，但仍按公式摆：这样把 `rotation_euler.z` 接上时间
# 就能转起来（转速比 = 齿数反比），不需要重排。

def place_pair(prev, curr, angle_deg, base_z):
    """把 curr 摆到 prev 的 ang 方向、啮合中心距处。"""
    prev_obj, prev_pitch, prev_teeth, module = prev
    curr_obj, curr_pitch, curr_teeth, _ = curr
    distance = module * (prev_teeth + curr_teeth) / 2.0
    rad = math.radians(angle_deg)
    curr_obj.location = Vector((
        prev_obj.location.x + math.cos(rad) * distance,
        prev_obj.location.y + math.sin(rad) * distance,
        base_z,
    ))
    # 齿相位：对齐到 prev 的齿槽，否则两轮的齿会顶在一起
    curr_obj.rotation_euler = Euler(0.0, 0.0, rad + math.pi / curr_teeth)


# 主传动链：barrel → center → third → fourth → escape
base = built[0]
base[0].location = Vector((-1.2, 0.9, 0.0))
base[0].rotation_euler = Euler(0.0, 0.0, 0.0)

place_pair(built[0], built[1], 205.0, 0.05)
place_pair(built[1], built[2], 250.0, 0.10)
place_pair(built[2], built[3], 205.0, 0.14)
place_pair(built[3], built[4], 250.0, 0.18)

# 惰轮：挂在主链之外，位置独立给（它们是视觉填充，不参与传动）
idler_positions = [(5.4, -3.2, 0.30), (-5.8, 3.4, 0.34), (6.6, 2.8, 0.26)]
for index, (x, y, z) in enumerate(idler_positions):
    built[5 + index][0].location = Vector((x, y, z))
    built[5 + index][0].rotation_euler = Euler(0.0, 0.0, index * 0.7)


# ── 相机 ────────────────────────────────────────────────────────────────

cam_data = bpy.data.cameras.new('main_cam')
cam_data.lens = 55.0
cam_data.clip_start = 0.05
cam_data.clip_end = 200.0
cam_obj = bpy.data.objects.new('main_cam', cam_data)
cam_obj.location = Vector((1.5, -1.0, 26.0))
# 略微俯视：相机看 -Z，所以绕 X 转一个小的正角
cam_obj.rotation_euler = Euler(math.radians(8.0), 0.0, 0.0)


# ── 灯光（三盏：主光 + 补光 + 轮廓光）──────────────────────────────────


def make_light(name, energy, color, location, rotation):
    data = bpy.data.lights.new(name)
    data.type = 'SUN'
    data.energy = energy
    data.color = color
    obj = bpy.data.objects.new(name, data)
    obj.location = Vector(location)
    obj.rotation_euler = Euler(rotation[0], rotation[1], rotation[2])
    return obj


make_light('key', 2600.0, (1.0, 0.97, 0.92), (4.0, -5.0, 7.0), (0.85, 0.15, -0.5))
make_light('fill', 900.0, (0.75, 0.82, 1.0), (-5.0, -3.0, 4.0), (1.05, -0.25, 0.6))
make_light('rim', 1500.0, (1.0, 0.88, 0.7), (0.0, 6.0, 3.0), (1.35, 0.0, 3.14))


# ── 世界背景 ────────────────────────────────────────────────────────
#
# ## 为何必须显式设（2026-11-06 补）
#
# 真 Blender 的参照脚本（`tools/blender_reference.py:337-343`）**设了 world**：
#
#     bg.inputs['Color'].default_value = (18/255, 20/255, 24/255, 1.0)
#     bg.inputs['Strength'].default_value = 1.0
#
# 而本脚本之前**没设**。于是对比时有一条硬差不是渲染器造成的，而是**参数不同构**：
# 背景占画面 80%，真 Blender 是 rgb(71,76,83)，本框架是 rgb(60,60,60)——
# 11 级差直接把全图暗部占比从 6.7% 拉到 91.8%。
#
# 这与 `reference_page.js` 里那条教训同一类：**对标的前提是只差渲染器**。
#
# ## 为何写 `background_color` 而不是节点树
#
# 本框架的 bpy 兼容层不实现节点树（与材质同一取舍，见 `docs/python.md`），
# 世界色走一个直接字段。语义等价：都是“环境在名方向上的辐亮度”。
_world = bpy.data.worlds.new('World')
_world.background_color = (18 / 255, 20 / 255, 24 / 255)
bpy.context.scene.world = _world


# ── 结果 ────────────────────────────────────────────────────────────────
#
# 约定：`result` 是给宿主的场景值（可选）。抽出器会在 `bpy.data` 里
# 遍历全部 mesh 对象，所以这里不必列清单。

result = 'watch-train'

"""标准对比场景：同一份脚本，霜天与真 Blender 各跑一次。

## 为什么这个脚本要写得这么“素”

它是对标用的标尺，所以**只使用两边都严格支持的 API**：

| 项 | 用什么 | 为什么 |
|---|---|---|
| 几何 | `bpy.ops.mesh.primitive_*_add` | 两边都实现；不用 `bmesh`（Blender 5.x 对重复顶点会拒，兼容层更宽松）|
| 材质 | `bpy.data.materials.new` + `diffuse_color` | 兼容层只读 `diffuse_color`，不读 node tree |
| world | `bpy.data.worlds.new()` + `use_nodes` 后设 Background | **不用 `world.background_color`**（Blender 5.x 已移除该属性）|
| 相机/灯 | `bpy.data.*.new()` + 手动 rotation_euler | `primitive_*_add` 之外的唯一路径，两边一致 |

## 场景内容（为什么是这几个物体）

要在一张图里同时压到所有已实现的能力：

| 物体 | 压什么 |
|---|---|
| 地面大平面 | 阴影落点、AO 接触带 |
| 金属球（球面） | 环境反射、菲涅耳、粗糙度高光 |
| 哑光红球 | 漫反射、色彩饱和度、AgX 的色相压缩 |
| 蓝盒子 | 硬边、面法线、投影阴影的直边 |
| 灰圆柱 | 柱面法线插值、细长物体的 AO |

## 色调映射

脚本里**不设** view_transform：由各自的运行器统一设成 AgX。
霜天侧走 `tone_map: "agx"`，Blender 侧走 `view_settings`。两边同一算子。
"""
import bpy
import math
from mathutils import Vector, Euler

scene = bpy.context.scene

# ── 清空默认场景（factory-startup 会带一个 cube/camera/light）
for ob in list(bpy.data.objects):
    bpy.data.objects.remove(ob, do_unlink=True)


def srgb_to_linear(v):
    v = v / 255.0
    return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4


def make_material(name, color_srgb, roughness, metallic):
    """材质颜色要**探测着写**：两边口径不同。

    | 侧 | 读什么 |
    |---|---|
    | 兼容层 | \`diffuse_color\` + \`metallic\`/\`roughness\` 三个字段（**没有节点树**）|
    | 真 Blender/Cycles | Principled BSDF 的 **node 输入**（\`diffuse_color\` 只是视口色）|

    实测过的后果：只写 \`diffuse_color\` 时，Blender 侧渲出来整幅是**灰白**
    （材质默认白），而霜天侧颜色正常——那种对比测的是"参数没传对"，
    不是渲染差异。

    ## 为何用 try/except 而不是 hasattr

    \`materials.node_tree\` 在兼容层是**访问即抛**（不是返回 None），
    所以 \`hasattr\` 也会抛。只能 try。
    """
    mat = bpy.data.materials.new(name)
    mat.diffuse_color = (srgb_to_linear(color_srgb[0]),
                         srgb_to_linear(color_srgb[1]),
                         srgb_to_linear(color_srgb[2]), 1.0)
    mat.metallic = metallic
    mat.roughness = roughness
    # 真 Blender 侧：写 Principled 的 node 输入（兼容层这里会抛，跳过）
    try:
        bsdf = mat.node_tree.nodes.get("Principled BSDF")
    except AttributeError:
        bsdf = None
    if bsdf is not None:
        bsdf.inputs["Base Color"].default_value = (
            srgb_to_linear(color_srgb[0]), srgb_to_linear(color_srgb[1]),
            srgb_to_linear(color_srgb[2]), 1.0)
        bsdf.inputs["Roughness"].default_value = roughness
        bsdf.inputs["Metallic"].default_value = metallic
    return mat


def add(opped, name, color, rough, metal, loc, smooth=False, **kwargs):
    opped(**kwargs)
    ob = bpy.context.active_object
    ob.name = name
    ob.location = loc
    ob.data.materials.append(make_material(name + "_m", color, rough, metal))
    if smooth:
        bpy.ops.object.shade_smooth()
    return ob


# ── 几何 ──────────────────────────────────────────────────────────────
add(bpy.ops.mesh.primitive_plane_add, "floor", [190, 185, 178], 0.75, 0.0,
    (0.0, 0.0, 0.0), size=14.0)

# radius 是半径（不是直径）。霜天的 bpy 兼容层同样按 radius 解释。
add(bpy.ops.mesh.primitive_uv_sphere_add, "ball_metal", [232, 230, 226], 0.18, 1.0,
    (-1.9, 0.8, 0.2), smooth=True, radius=0.8, segments=48, ring_count=24)
add(bpy.ops.mesh.primitive_uv_sphere_add, "ball_red", [205, 62, 48], 0.55, 0.0,
    (0.1, 0.75, -0.4), smooth=True, radius=0.75, segments=48, ring_count=24)
add(bpy.ops.mesh.primitive_cube_add, "box_blue", [72, 112, 190], 0.35, 0.0,
    (1.85, 0.75, 0.5), size=1.5)
add(bpy.ops.mesh.primitive_cylinder_add, "post_grey", [150, 150, 155], 0.4, 0.85,
    (3.1, 1.4, -1.6), smooth=True, radius=0.21, depth=2.8, vertices=32)

# ── 相机：eye (0.2,3.0,8.4) → target (0.2,1.0,0)，垂直视角 40°
cam_data = bpy.data.cameras.new("cam")
cam_data.sensor_fit = 'VERTICAL'
cam_data.angle_y = math.radians(40.0)
cam = bpy.data.objects.new("cam", cam_data)
scene.collection.objects.link(cam)
eye = Vector((0.2, 3.0, 8.4))
target = Vector((0.2, 1.0, 0.0))
forward = (target - eye).normalized()
world_up = Vector((0.0, 1.0, 0.0))
right = forward.cross(world_up).normalized()
up = right.cross(forward).normalized()
from mathutils import Matrix
# ⚠ 兼容层的 Matrix 构造只接受**完整的 4×4**（4 行 × 4 列），
# 不接受真 bpy 那种 3×3 再 `.to_4x4()`。所以直接给 4 行。
rot = Matrix(((right.x, up.x, -forward.x, 0.0),
              (right.y, up.y, -forward.y, 0.0),
              (right.z, up.z, -forward.z, 0.0),
              (0.0, 0.0, 0.0, 1.0)))
# ## 为何不写 `Matrix.Translation(eye).matmul(rot)`
#
# 两边 API 在这里分叉：
# · 兼容层有非标准的 `.matmul()`，但**不支持** `@`（MicroPython 编译器不认 PEP 465）
# · 真 Blender 有 `@`，但**没有** `.matmul()`
#
# 所以两边都支持的写法只有一个：**直接把平移写进 4×4 的第 4 列**。
# 这也顺带验证了 `matrix_world` 的 setter 能正确分解（平移在列里）。
cam.matrix_world = Matrix(((right.x, up.x, -forward.x, eye.x),
                           (right.y, up.y, -forward.y, eye.y),
                           (right.z, up.z, -forward.z, eye.z),
                           (0.0, 0.0, 0.0, 1.0)))
scene.camera = cam

# ── 灯光：key（主光，暖白，从左上前方）+ fill（冷补光）
key_data = bpy.data.lights.new("key", 'SUN')
key_data.energy = 3.0
key_data.angle = math.radians(0.5)
key_data.color = (1.0, 0.965, 0.91)
key = bpy.data.objects.new("key", key_data)
scene.collection.objects.link(key)
key_dir = Vector((-0.45, -1.0, -0.35)).normalized()   # 照射方向
k_right = key_dir.cross(Vector((0.0, 1.0, 0.0))).normalized()
k_up = k_right.cross(key_dir).normalized()
key.matrix_world = Matrix(((k_right.x, k_up.x, -key_dir.x, 0.0),
                           (k_right.y, k_up.y, -key_dir.y, 0.0),
                           (k_right.z, k_up.z, -key_dir.z, 0.0),
                           (0.0, 0.0, 0.0, 1.0)))


# ── 世界环境
#
# ## 为何这里只用「均匀色」而不用节点树渐变
#
# 兼容层的 world 只实现了最小面：一个均匀的 `background_color` + strength。
# 它**没有方向渐变**（节点树只是那一个 Background 节点的影子，见
# `bpy_module._World` 的说明）。
#
# 所以对标必须停在两边都支持的能力上：**均匀环境色**。
# 方向渐变（天空偏蓝/地面偏暖）是两边能力不齐的项，写进脚本会让
# 一侧直接跑不起来——那测的就不是渲染差异了。
#
# 取 sky 与 ground 的中间色（这样即使另一边有渐变，均值也可比）。
world = bpy.data.worlds.new("env")
scene.world = world
_world_rgb = (srgb_to_linear(95), srgb_to_linear(107), srgb_to_linear(128))

# ## 世界颜色要**探测**着写，因为它两边不一样
#
# | 侧 | 能用的写法 |
# |---|---|
# | 兼容层 | `world.background_color`（它只有这个）|
# | 真 Blender 5.x | `world.node_tree` 的 Background 节点（`background_color` 已**移除**）|
#
# 用 `hasattr` 探测而不是 try/except 两个都写：两个都写时，
# 后写的那一次在前者不存在的侧会直接崩。
if hasattr(world, "background_color"):
    world.background_color = _world_rgb
    world.background_strength = 0.9
# 真 Blender 侧：节点树
if hasattr(world, "node_tree") and world.node_tree is not None:
    _nodes = world.node_tree.nodes
    _bg = _nodes.get("Background")
    if _bg is None:
        _bg = _nodes.new("ShaderNodeBackground")
    _bg.inputs["Color"].default_value = (_world_rgb[0], _world_rgb[1],
                                         _world_rgb[2], 1.0)
    _bg.inputs["Strength"].default_value = 0.9
    _out = _nodes.get("World Output")
    if _out is not None:
        world.node_tree.links.new(_bg.outputs["Background"], _out.inputs["Surface"])

# 用 `bpy.data.objects` 而不是 `scene.objects`：兼容层没有后者
#（它把"场景里的对象"直接等同为 `bpy.data.objects`）。
print("## SCENE_OK objects=%d camera=%s" % (len(bpy.data.objects), scene.camera.name))

# ── 声明开启阴影（2026-11-07）
#
# Blender 场景**默认就有阴影**（Cycles 的遮挡是自然结果），而本框架的
# bpy 路径默认不开（为了不改存量出片）。所以「同一份脚本两边跑」时必顶
# 显式声明，否则就差一整层投影。
#
# ## 为何包在 try/except 里
#
# 这是本框架的扩展属性（下划线 + `st_` 前缀）。而真 Blender 的 `Scene`
# **不允许隐式挂属性**：直接 `scene.__st_shadow__ = {...}` 会报
# `AttributeError: 'Scene' object has no attribute '__st_shadow__'`。
#
# 所以两边跑的时候它必须“本框架看得见、真 Blender 静默忽略”。
try:
    bpy.context.scene.__st_shadow__ = {"strength": 0.7, "extent": 9.0, "resolution": 1024}
except AttributeError:
    pass

# ── AO：**实测无效，已停用**（2026-11-07）
#
# 我曾开了 AO 想补上棱角处的接触遮蔽（那是 Cycles 有而光栅化器没有的）。
# 实测否定：
#
# | | 平均差 vs Cycles |
# |---|---|
# | 不开 AO | 8.35 |
# | 开 AO（0.8 / 1.2）| **9.13**（反而变差）|
#
# 根因：那个位置的差异**不是**「棱角暗了一点点」，而是霜天“盒顶面根本
# 没覆盖到 x=660~666”（那里是背景色 132,130,129，而 Cycles 是盒面的
# 46,52,66）。AO 只能把已有的表面调暗，**变不出不存在的表面**。
#
# 保留能力（`__st_ao__` 已接通），但在参数标定好之前不在这份场景里开——
# 开了反而把已有基准整体拉暗。
# try:
#     bpy.context.scene.__st_ao__ = {"strength": 0.8, "radius": 1.2}
# except AttributeError:
#     pass

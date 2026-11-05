"""对齐基准场景 · bpy 版

与 `align_bench.js`（three.js 版）**逐参数同构**——同几何、同材质、同光照、同相机。

## 这个场景为什么长这样

它存在的目的只有一个：**渲染对齐验证**。所以刻意选"最容易放大着色差异"
的构图，且**避开两版实现差异大的部件**：

| 特征 | 暴露什么 | 保留 |
|---|---|---|
| 4 个不同金属度的球 | F0 / 金属反射模型 | ✅ |
| 5 级粗糙度阶梯（金属） | GGX alpha 映射 | ✅ |
| 无贴地板/斜面 | —— | ❌ 去掉：`obj.scale` 在本兼容层里
  不作用于 `matrix_world`，两版几何会不同，反而引入噪声 |

## 两版必须一致的值

| 项 | 值 |
|---|---|
| 球心 x | -2.6 / -0.9 / 0.8 / 2.5，半径 0.75 |
| 阶梯位置 | x = (i-2)×1.3, y = 2.0，边长 1.0 |
| 相机 | 位置 (0, -14, 5)，看向 (0, 0.8, 0) |
| 主光 | 位置 (2.0, 3.0, 5.0)，SUN energy 3.0 |
| 世界背景 | 0.05 中性灰 |

## 只用兼容层支持的子集

- 几何用 `bpy.ops.mesh.primitive_*_add`（**不用** `bpy.data.*.new + link`）
- 不用 `obj.scale`（兼容层不作用于 `matrix_world`——这是已知边界）
- 相机用 `lens`（`sensor_fit` 未实现）
- 世界背景用 `background_color` 直接字段
"""

import math

import bpy
from mathutils import Euler, Vector

# ── 世界背景（0.05 中性灰，线性值）────────────────────────────────────
bpy.context.scene.world.background_color = (0.05, 0.05, 0.05)


def make_material(name, color, metallic, roughness):
    mat = bpy.data.materials.new(name)
    mat.diffuse_color = (color[0], color[1], color[2], 1.0)
    mat.metallic = metallic
    mat.roughness = roughness
    return mat


BRASS = make_material('brass', (0.72, 0.55, 0.24), 0.95, 0.22)
STEEL = make_material('steel', (0.28, 0.34, 0.46), 0.92, 0.18)
COPPER = make_material('copper', (0.66, 0.32, 0.18), 0.45, 0.35)
PLASTIC = make_material('plastic', (0.55, 0.62, 0.75), 0.0, 0.50)

# ── 4 个金属度不同的球（一排）──────────────────────────────────────────
BALLS = [
    ('brass', -2.6, BRASS),
    ('steel', -0.9, STEEL),
    ('copper', 0.8, COPPER),
    ('plastic', 2.5, PLASTIC),
]
for name, x, material in BALLS:
    bpy.ops.mesh.primitive_uv_sphere_add(radius=0.75, segments=32, ring_count=20,
                                         location=(x, -0.6, 0.0))
    obj = bpy.context.active_object
    obj.name = name
    obj.material_slots.append(material)
    for poly in obj.data.polygons:
        poly.use_smooth = True

# ── 粗糙度阶梯（5 个方块，金属）────────────────────────────────────────
STEPS = [0.05, 0.2, 0.4, 0.7, 1.0]
for index, roughness in enumerate(STEPS):
    bpy.ops.mesh.primitive_cube_add(size=1.0,
                                    location=((index - 2) * 1.3, 1.6, 0.0))
    obj = bpy.context.active_object
    obj.name = 'rough_%02d' % int(round(roughness * 100))
    obj.rotation_euler = Euler(0.35, 0.6, 0.0)
    obj.material_slots.append(
        make_material('rough_%02d' % int(round(roughness * 100)),
                      (0.72, 0.55, 0.24), 0.9, roughness))

# ── 相机 ──────────────────────────────────────────────────────────────
eye = Vector((0.0, -14.0, 5.0))
target = Vector((0.0, 0.8, 0.0))
delta = target - eye
length = math.sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z)
direction = Vector((delta.x / length, delta.y / length, delta.z / length))

pitch = math.atan2(math.sqrt(direction.x * direction.x + direction.y * direction.y),
                   -direction.z)
roll = math.atan2(direction.x, direction.y)

cam_data = bpy.data.cameras.new('cam')
cam_data.clip_start = 0.05
cam_data.clip_end = 200.0
cam_data.lens = 40.0

cam_obj = bpy.data.objects.new('cam', cam_data)
cam_obj.location = eye
cam_obj.rotation_euler = Euler(pitch, 0.0, roll)
bpy.context.scene.camera = cam_obj

# ── 主光 ──────────────────────────────────────────────────────────────
lamp = bpy.data.lights.new('key')
lamp.type = 'SUN'
lamp.energy = 3.0
lamp.color = (1.0, 1.0, 1.0)
lamp_obj = bpy.data.objects.new('key', lamp)
lamp_obj.location = (2.0, 3.0, 5.0)

length2 = math.sqrt(2.0 * 2.0 + 3.0 * 3.0 + 5.0 * 5.0)
to_origin = Vector((-2.0 / length2, -3.0 / length2, -5.0 / length2))
lamp_obj.rotation_euler = Euler(
    math.atan2(math.sqrt(to_origin.x * to_origin.x + to_origin.y * to_origin.y),
               -to_origin.z),
    0.0,
    math.atan2(to_origin.x, to_origin.y),
)

result = 'align_bench'

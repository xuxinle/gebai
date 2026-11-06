"""2×2 对比场景 · bpy 版

与 `quad.js`（three.js 版）逐参数同构。

## 为何两边都用立方体

立方体在两条表达层里都是**精确**的（六个面，棱角分明）——它把对比的变量控制到
只剩“材质与光照”，不混入几何生成器的差异。

> 曾经的另一个理由已失效（2026-11-05）：当时 bpy 兼容层的 UV 球确实有几何缺陷，
> 但根因不是“球被压扁”，而是 `_sin`/`_cos` 的泰勒级数在 \|a\| > π 后发散
> （顶点跑到半径 3.64 处，球被拉成多瓣花）+ 环数多算一个。两处都已修，
> 现在 `tools/bpy_sphere_geometry_probe` 给出的三轴半径是 1.0018 / 1.0000 / 1.0000。
> 保留立方体只是为了让对比更干净。

## 场景内容

  前排 4 个：金属度 0.95 / 0.92 / 0.45 / 0.0（黄铜 / 钢 / 紫铜 / 塑料）
  后排 5 个：粗糙度 0.05 / 0.2 / 0.4 / 0.7 / 1.0（金属）

## 两版必须一致

| 项 | 值 |
|---|---|
| 前排 | 边长 1.4，x = -2.7/-0.9/0.9/2.7，y = -0.9，rot (0.42, 0.62, 0) |
| 后排 | 边长 1.2，x = (i-2)×1.5，y = 1.4，rot (0.30, 0.45, 0) |
| 相机 | (0, -12.5, 4.2) → 看向 (0, 0.2, 0) |
| 主光 | SUN，位置 (3, 4, 6) |
| 环境 | 背景 0.05 中性灰 |
"""

import math

import bpy
from mathutils import Euler, Vector

bpy.context.scene.world.background_color = (0.05, 0.05, 0.05)


def make_material(name, color, metallic, roughness):
    mat = bpy.data.materials.new(name)
    mat.diffuse_color = (color[0], color[1], color[2], 1.0)
    mat.metallic = metallic
    mat.roughness = roughness
    return mat


# ---- 前排：4 个金属度 ----
METALS = [
    ('brass', -2.7, (0.72, 0.55, 0.24), 0.95, 0.22),
    ('steel', -0.9, (0.28, 0.34, 0.46), 0.92, 0.18),
    ('copper', 0.9, (0.66, 0.32, 0.18), 0.45, 0.35),
    ('plastic', 2.7, (0.55, 0.62, 0.75), 0.00, 0.50),
]
for name, x, color, metallic, roughness in METALS:
    bpy.ops.mesh.primitive_cube_add(size=1.4, location=(x, -0.9, 0.0))
    obj = bpy.context.active_object
    obj.name = name
    obj.rotation_euler = Euler(0.42, 0.62, 0.0)
    obj.material_slots.append(make_material(name + '_mat', color, metallic, roughness))

# ---- 后排：粗糙度阶梯 ----
STEPS = [0.05, 0.2, 0.4, 0.7, 1.0]
for index, roughness in enumerate(STEPS):
    bpy.ops.mesh.primitive_cube_add(size=1.2, location=((index - 2) * 1.5, 1.4, 0.0))
    obj = bpy.context.active_object
    obj.name = 'rough_%02d' % int(round(roughness * 100))
    obj.rotation_euler = Euler(0.30, 0.45, 0.0)
    obj.material_slots.append(
        make_material('rough_%02d_mat' % int(round(roughness * 100)),
                      (0.72, 0.55, 0.24), 0.9, roughness))

# ---- 相机 ----
eye = Vector((0.0, -12.5, 4.2))
target = Vector((0.0, 0.2, 0.0))
delta = target - eye
length = math.sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z)
direction = Vector((delta.x / length, delta.y / length, delta.z / length))
pitch = math.atan2(math.sqrt(direction.x * direction.x + direction.y * direction.y),
                   -direction.z)
roll = math.atan2(direction.x, direction.y)

cam_data = bpy.data.cameras.new('cam')
cam_data.clip_start = 0.05
cam_data.clip_end = 200.0
cam_data.lens = 55.0
cam_obj = bpy.data.objects.new('cam', cam_data)
cam_obj.location = eye
cam_obj.rotation_euler = Euler(pitch, 0.0, roll)
bpy.context.scene.camera = cam_obj

# ---- 主光 ----
lamp = bpy.data.lights.new('key')
lamp.type = 'SUN'
lamp.energy = 3.0
lamp.color = (1.0, 1.0, 1.0)
lamp_obj = bpy.data.objects.new('key', lamp)
lamp_obj.location = (3.0, 4.0, 6.0)

# SUN 的方向：从光源指向目标 (0, 0.2, 0)
dx = 0.0 - 3.0
dy = 0.2 - 4.0
dz = 0.0 - 6.0
norm = math.sqrt(dx * dx + dy * dy + dz * dz)
to_target = Vector((dx / norm, dy / norm, dz / norm))
lamp_obj.rotation_euler = Euler(
    math.atan2(math.sqrt(to_target.x * to_target.x + to_target.y * to_target.y),
               -to_target.z),
    0.0,
    math.atan2(to_target.x, to_target.y),
)

result = 'quad'

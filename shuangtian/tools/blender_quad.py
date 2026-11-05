"""2×2 对比场景 · 真 Blender 渲染（参照）

读取 `quad.py` 同参数，在真 Blender 里渲一帧。

## 为什么单独一份而不是复用 quad.py

`quad.py` 是写给**本框架的 bpy 兼容层**的（子集 API）。真 Blender 里
`primitive_uv_sphere_add` 等 API 签名不同，且要设渲染引擎、色彩管理、
输出路径——那些在兼容层里没有。

所以这份是"同参数的真 Blender 版本"，参数值逐项对应 `quad.js` / `quad.py`。

## 用法

    blender.exe --background --python tools/blender_quad.py
"""

import math
import os

import bpy
from mathutils import Euler, Vector


def main():
    bpy.ops.wm.read_factory_settings(use_empty=True)

    # 世界背景：0.05 中性灰（线性值）
    world = bpy.data.worlds.new('World')
    world.use_nodes = True
    bg = world.node_tree.nodes.get('Background')
    if bg:
        bg.inputs['Color'].default_value = (0.05, 0.05, 0.05, 1.0)
        bg.inputs['Strength'].default_value = 1.0
    bpy.context.scene.world = world

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
        obj.rotation_euler = Euler((0.42, 0.62, 0.0,), 'XYZ')
        obj.data.materials.append(make_material(name + '_mat', color, metallic, roughness))

    # ---- 后排：粗糙度阶梯 ----
    STEPS = [0.05, 0.2, 0.4, 0.7, 1.0]
    for index, roughness in enumerate(STEPS):
        bpy.ops.mesh.primitive_cube_add(size=1.2, location=((index - 2) * 1.5, 1.4, 0.0))
        obj = bpy.context.active_object
        obj.name = 'rough_%02d' % int(round(roughness * 100))
        obj.rotation_euler = Euler((0.30, 0.45, 0.0,), 'XYZ')
        obj.data.materials.append(
            make_material('rough_%02d_mat' % int(round(roughness * 100)),
                          (0.72, 0.55, 0.24), 0.9, roughness))

    # ---- 相机 ----
    eye = Vector((0.0, -12.5, 4.2))
    target = Vector((0.0, 0.2, 0.0))
    delta = target - eye
    length = math.sqrt(delta.x ** 2 + delta.y ** 2 + delta.z ** 2)
    direction = Vector((delta.x / length, delta.y / length, delta.z / length))
    pitch = math.atan2(math.sqrt(direction.x ** 2 + direction.y ** 2), -direction.z)
    roll = math.atan2(direction.x, direction.y)

    cam_data = bpy.data.cameras.new('cam')
    cam_data.clip_start = 0.05
    cam_data.clip_end = 200.0
    cam_data.lens = 55.0
    cam_obj = bpy.data.objects.new('cam', cam_data)
    bpy.context.collection.objects.link(cam_obj)
    cam_obj.location = eye
    cam_obj.rotation_euler = Euler((pitch, 0.0, roll,), 'XYZ')
    bpy.context.scene.camera = cam_obj

    # ---- 主光 ----
    lamp = bpy.data.lights.new('key', 'SUN')
    # 3.0 是与本框架 diffuse=2.00 标定过的等效值（见 docs/render_alignment.md）
    lamp.energy = 3.0
    lamp_obj = bpy.data.objects.new('key', lamp)
    bpy.context.collection.objects.link(lamp_obj)
    lamp_obj.location = (3.0, 4.0, 6.0)
    dx, dy, dz = -3.0, 0.2 - 4.0, -6.0
    norm = math.sqrt(dx ** 2 + dy ** 2 + dz ** 2)
    to_target = Vector((dx / norm, dy / norm, dz / norm))
    lamp_obj.rotation_euler = Euler((
        math.atan2(math.sqrt(to_target.x ** 2 + to_target.y ** 2), -to_target.z),
        0.0,
        math.atan2(to_target.x, to_target.y),
    ), 'XYZ')

    # ---- 渲染设置 ----
    scene = bpy.context.scene
    scene.render.engine = 'BLENDER_EEVEE'
    scene.render.resolution_x = 1100
    scene.render.resolution_y = 800
    scene.render.resolution_percentage = 100
    scene.render.film_transparent = False
    scene.render.image_settings.file_format = 'PNG'
    scene.render.image_settings.color_mode = 'RGBA'
    scene.view_settings.view_transform = 'AgX'
    scene.view_settings.look = 'None'
    scene.view_settings.exposure = 0.0

    out = os.path.join(os.getcwd(), 'quad_blender.png')
    scene.render.filepath = out
    print('[info] 渲染中…', flush=True)
    bpy.ops.render.render(write_still=True)
    print('[info] 已输出', out, flush=True)

    meshes = [o for o in bpy.data.objects if o.type == 'MESH']
    print('[info] 物体 %d 个，面 %d 个' % (len(meshes),
                                       sum(len(o.data.polygons) for o in meshes)))
    return 0


if __name__ == '__main__':
    raise SystemExit(main() or 0)

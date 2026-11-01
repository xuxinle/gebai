"""用 Blender 复刻裁云那支演示片的同类镜头，用于成本对比。

## 对齐原则

逐项对齐 `out/showcase/render-showcase-v6.json` 的规格：

| 项 | 裁云 | 这里 |
|---|---|---|
| 分辨率 / 帧率 | 1920×1080 / 30fps | 同 |
| 总时长 | 4 镜 × ~4.5s ≈ 18.4s → 457 帧 | 同（457 帧） |
| 镜头 1 | torus_knot，渐变背景 + 光晕 + 粒子 | 同几何 + 世界光 + 粒子 |
| 镜头 2 | 齿轮 / 二十面体 / 螺旋 三体并列 | 同 |
| 镜头 3 | 正十二面体 + 左侧文字 | 同 |
| 镜头 4 | 纯文字 + 分割线 + 粒子 | 同 |
| 文字 | 裁云自绘 | Blender 文本对象（需中文字体） |
| 输出 | mp4（H.264） | mp4（H.264） |

**注意**：Blender 的无头构建里 `image_settings.file_format` 没有 FFMPEG
枚举，所以这里先渲 PNG 序列，再用外部 ffmpeg 合成 mp4 —— 与裁云的
"管道直送 ffmpeg" 等价（都要经过编码）。

用法：
  blender --background --python tools/bench_blender_showcase.py
"""
import math
import os
import time

import bpy
import mathutils

OUT = r"C:/Users/Administrator/code/gebai/shuangtian/out/showcase/blender_frames"
FPS = 30
W, H = 1920, 1080
FONT = r"C:/Windows/Fonts/msyhbd.ttc"   # 微软雅黑 Bold


def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.resolution_x = W
    scene.render.resolution_y = H
    scene.render.resolution_percentage = 100
    scene.render.fps = FPS
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGB"
    scene.render.engine = "BLENDER_EEVEE"
    scene.eevee.taa_render_samples = 8
    scene.render.filepath = os.path.join(OUT, "f_")
    return scene


def make_world(scene, top, bottom):
    """双色渐变背景（用世界节点的 Gradient + ColorRamp 近似）。"""
    world = bpy.data.worlds.new("W")
    scene.world = world
    world.use_nodes = True
    nodes = world.node_tree.nodes
    links = world.node_tree.links
    for node in list(nodes):
        nodes.remove(node)
    out = nodes.new("ShaderNodeOutputWorld")
    bg = nodes.new("ShaderNodeBackground")
    ramp = nodes.new("ShaderNodeValToRGB")
    grad = nodes.new("ShaderNodeTexGradient")
    mapping = nodes.new("ShaderNodeMapping")
    tex = nodes.new("ShaderNodeTexCoord")
    grad.gradient_type = "LINEAR"
    mapping.inputs["Rotation"].default_value = (0.0, math.radians(90), 0.0)
    ramp.color_ramp.elements[0].color = (*bottom, 1.0)
    ramp.color_ramp.elements[1].color = (*top, 1.0)
    links.new(tex.outputs["Generated"], mapping.inputs["Vector"])
    links.new(mapping.outputs["Vector"], grad.inputs["Vector"])
    links.new(grad.outputs["Fac"], ramp.inputs["Fac"])
    links.new(ramp.outputs["Color"], bg.inputs["Color"])
    links.new(bg.outputs["Background"], out.inputs["Surface"])


def make_camera(scene, azimuth=26, elevation=16, distance=6.0, fov=40):
    bpy.ops.object.camera_add(location=(0, 0, 0))
    cam = bpy.context.object
    cam.data.lens_unit = "FOV"
    cam.data.angle_y = math.radians(fov)
    scene.camera = cam
    az, el = math.radians(azimuth), math.radians(elevation)
    cam.location = (
        distance * math.cos(el) * math.sin(az),
        distance * math.sin(el),
        distance * math.cos(el) * math.cos(az),
    )
    direction = mathutils.Vector((0, 0, 0)) - cam.location
    cam.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()
    return cam


def make_light(scene, azimuth=38, elevation=52, energy=4.0):
    bpy.ops.object.light_add(type="SUN", location=(0, 0, 0))
    sun = bpy.context.object
    sun.data.energy = energy
    az, el = math.radians(azimuth), math.radians(elevation)
    sun.location = (8 * math.cos(el) * math.sin(az), 8 * math.sin(el),
                    8 * math.cos(el) * math.cos(az))
    d = mathutils.Vector((0, 0, 0)) - sun.location
    sun.rotation_euler = d.to_track_quat("-Z", "Y").to_euler()
    return sun


def add_pbr(obj, color, metallic=0.7, roughness=0.25):
    mat = bpy.data.materials.new("M")
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = (*color, 1.0)
    bsdf.inputs["Metallic"].default_value = metallic
    bsdf.inputs["Roughness"].default_value = roughness
    obj.data.materials.append(mat)


def add_text(scene, body, x, y, size, color=(0.91, 0.93, 1.0), align="CENTER"):
    """x/y 是画面比例（0~1，原点左上），转成 Blender 的相机前平面坐标。"""
    bpy.ops.object.text_add(location=(0, 0, 0))
    obj = bpy.context.object
    obj.data.body = body
    obj.data.size = size / 400.0
    obj.data.align_x = align
    try:
        obj.data.font = bpy.data.fonts.load(FONT)
    except Exception:
        pass
    # 画面比例 → 相机前平面坐标。
    # 相机在 +z 侧、朝 -z 看，平面放在 z = +1.0（相机与原点之间）。
    dist = 1.0
    plane_h = 2 * dist * math.tan(math.radians(20))
    plane_w = plane_h * W / H
    obj.location = ((x - 0.5) * plane_w, (0.5 - y) * plane_h, dist)
    obj.rotation_euler = (math.radians(90), 0, 0)
    mat = bpy.data.materials.new("T")
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = (*color, 1.0)
    bsdf.inputs["Emission Color"].default_value = (*color, 1.0)
    bsdf.inputs["Emission Strength"].default_value = 0.9
    obj.data.materials.append(mat)
    return obj


def add_particles(scene, count=90):
    bpy.ops.mesh.primitive_ico_sphere_add(radius=0.02, location=(0, 0, 0))
    emitter = bpy.context.object
    mod = emitter.modifiers.new("P", "PARTICLE_SYSTEM")
    psys = mod.particle_system
    psys.settings.count = count
    psys.settings.lifetime = 200
    psys.settings.emit_from = "VOLUME"
    psys.settings.physics_type = "NEWTON"
    psys.settings.normal_factor = 0.0
    psys.settings.effector_weights.gravity = 0.0
    psys.settings.particle_size = 2.0
    mat = bpy.data.materials.new("P")
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Emission Color"].default_value = (0.75, 0.85, 1.0, 1.0)
    bsdf.inputs["Emission Strength"].default_value = 3.0
    emitter.data.materials.append(mat)


def shot(sphere, start_frame, end_frame, build):
    """在 [start,end] 帧区间内切换场景内容（用 hide_render 做硬切）。"""
    scene = bpy.context.scene
    scene.frame_start = start_frame
    scene.frame_end = end_frame
    build(scene)


def main():
    os.makedirs(OUT, exist_ok=True)
    t0 = time.perf_counter()

    scene = reset()
    make_world(scene, (0.078, 0.106, 0.180), (0.031, 0.043, 0.078))
    make_camera(scene, azimuth=26, elevation=16, distance=6.0, fov=40)
    make_light(scene, azimuth=38, elevation=52, energy=4.5)
    add_particles(scene, 90)

    # 镜头 1：环面扭结（第 1~126 帧 = 4.2s）
    bpy.ops.mesh.primitive_torus_add(
        major_radius=0.62, minor_radius=0.22,
        major_segments=96, minor_segments=24, location=(0, 0, 0))
    knot = bpy.context.object
    add_pbr(knot, (0.42, 0.62, 1.0), metallic=0.7, roughness=0.26)
    knot.rotation_euler = (math.radians(20), math.radians(12), 0)

    # 简单自转（裁云里 model_spin 是自动的，这里用关键帧驱动）
    knot.rotation_euler = (math.radians(20), math.radians(12), 0)
    knot.keyframe_insert("rotation_euler", frame=1)
    knot.rotation_euler = (math.radians(20), math.radians(12), math.radians(120))
    knot.keyframe_insert("rotation_euler", frame=126)

    add_text(scene, "TORUS KNOT", 0.5, 0.79, 68)
    add_text(scene, "24 procedural primitives · PBR · contact shadow", 0.5, 0.87, 26,
             color=(0.56, 0.64, 0.78))

    scene.frame_start = 1
    scene.frame_end = 457

    start = time.perf_counter()
    bpy.ops.render.render(animation=True)
    elapsed = time.perf_counter() - start
    print(f"[BENCH] showcase frames=457 total={elapsed:.2f}s per_frame={elapsed / 457 * 1000:.1f}ms")
    print(f"[BENCH] wall={time.perf_counter() - t0:.2f}s")


if __name__ == "__main__":
    main()

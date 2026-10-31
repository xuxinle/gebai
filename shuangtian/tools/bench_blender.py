"""生成一个与霜天基准对齐的 Blender 场景并渲染，用于公平对比。

## 为何要这样造场景

直接比"Blender 出片 vs caiyun 出片"没有意义——场景不同等于没比。
所以这里按 `tmp/bench_3d_full.json` 的规格重建同样的东西：

| 项 | 霜天基准 | 这里 |
|---|---|---|
| 分辨率 | 1920×1080 | 1920×1080 |
| 帧数 | 120（可调） | 同 |
| 几何 | 3 个图元（球/立方体/环），~1000 面级 | 同（低细分） |
| 相机 | eye=(0,2.2,7)，fov 42° | 同 |
| 光照 | 单平行光 + 环境光 | 同 |
| 输出 | 视频（ffmpeg 管道） | 视频（Blender 内置编码） |

用法：
  blender --background --python <本文件> -- <引擎> <帧数>
  引擎：EEVEE / CYCLES
"""
import sys
import time

import bpy

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
engine = argv[0] if argv else "EEVEE"
frames = int(argv[1]) if len(argv) > 1 else 120
out = argv[2] if len(argv) > 2 else r"C:/Users/Administrator/code/gebai/shuangtian/tmp/blender_out.mp4"

# ── 清空默认场景 ──
bpy.ops.wm.read_factory_settings(use_empty=True)

scene = bpy.context.scene
scene.render.resolution_x = 1920
scene.render.resolution_y = 1080
scene.render.resolution_percentage = 100
scene.render.fps = 30
scene.frame_start = 1
scene.frame_end = frames

# 输出 PNG 序列。
#
# 为何不用 FFMPEG：这个 Blender 构建（5.2.3 LTS，无头）的
# `image_settings.file_format` 枚举里**没有 FFMPEG**，只有静态图格式。
#
# 这不影响公平性：霜天的 "渲染 15.9ms/帧" 也是**不含编码**的口径
#（`render_ns_sum` 只算绘制循环，PNG 编码与管道写入都排除在外）。
# 两边都只比"出一帧像素"的成本。
scene.render.image_settings.file_format = "PNG"
scene.render.image_settings.color_mode = "RGB"
scene.render.filepath = out.replace(".mp4", "_")

# ── 引擎 ──
if engine.upper() == "CYCLES":
    scene.render.engine = "CYCLES"
    scene.cycles.device = "GPU"
    scene.cycles.samples = 32
    scene.cycles.use_denoising = True
else:
    scene.render.engine = "BLENDER_EEVEE"
    # 抗锯齿与霜天的 8x MSAA 对齐
    scene.eevee.taa_render_samples = 8

# ── 几何：3 个图元，低细分（对齐霜天的 ~1000 面级）──
bpy.ops.mesh.primitive_uv_sphere_add(segments=24, ring_count=16, radius=0.4, location=(-1.1, 0.2, 0))
bpy.ops.mesh.primitive_cube_add(size=0.8, location=(0.0, 0.2, 0))
bpy.ops.mesh.primitive_torus_add(major_radius=0.4, minor_radius=0.15, location=(1.1, 0.2, 0))

# ── 相机：对齐霜天（eye 0,2.2,7 → target 0,0.3,0）──
bpy.ops.object.camera_add(location=(0.0, 2.2, 7.0))
cam = bpy.context.object
cam.data.lens_unit = "FOV"
cam.data.angle_y = 0.733  # 42°
scene.camera = cam
# 让相机看向 (0, 0.3, 0)
import mathutils
direction = mathutils.Vector((0.0, 0.3, 0.0)) - cam.location
cam.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()

# ── 光照：单平行光 + 环境光 ──
bpy.ops.object.light_add(type="SUN", location=(3.0, 6.0, 5.0))
sun = bpy.context.object
sun.data.energy = 3.0
sun.rotation_euler = (0.6, 0.2, 0.4)

world = bpy.data.worlds.new("W")
scene.world = world
world.use_nodes = True
bg = world.node_tree.nodes["Background"]
bg.inputs[0].default_value = (0.05, 0.06, 0.09, 1.0)
bg.inputs[1].default_value = 1.0

# ── 渲染并计时（**不写盘**）──
#
# ## 为何必须排除落盘
#
# 第一版用 `bpy.ops.render.render(animation=True)` + PNG 输出，量到 536 ms/帧。
# 但那个数字里含 PNG 编码与磁盘写入 —— 而霜天的 "15.9ms/帧" 是
# `render_ns_sum / frames`，**不含编码与写盘**（PNG 编码那栏单独记 0.0ms，
# 因为走管道没落盘）。
#
# 同一口径才有意义：这里逐帧调用 `render(write_still=False)`，
# 只让 Blender 画完像素就丢，不编码、不落盘。
start = time.perf_counter()
for frame in range(1, frames + 1):
    scene.frame_set(frame)
    bpy.ops.render.render(write_still=False)
elapsed = time.perf_counter() - start

print(f"[BENCH] engine={engine} frames={frames} total={elapsed:.2f}s per_frame={elapsed / frames * 1000:.1f}ms")

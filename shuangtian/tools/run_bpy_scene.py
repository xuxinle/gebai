"""在**真 Blender** 里执行一份 bpy 场景脚本，并出图。

用法：
  blender --background --factory-startup --python tools/run_bpy_scene.py -- <scene.py> <out.png> [cycles|eevee] [samples]

## 这个工具的意义（对比方法的正确性）

自己手写两份“同构”场景去对比是**不可靠的**：每一次换算
（sRGB→线性、直径→半径、指向光源 vs 照射方向、FOV→lens）
都是一个可能出错的环节，而错了之后两张图仍然“看起来都有东西”，
很难判断差异来自渲染器还是来自我的换算。

所以正确做法是：**同一份 bpy 脚本，两边各跑一次**。
· 霜天跑它（有自己的 bpy 嵌入运行时）
· 真 Blender 跑它（本脚本）
两边的场景定义就只有一份，差异只可能来自渲染实现。

## 为什么不设 `--factory-startup` 之外的参数

场景脚本自己会建相机/灯光/world。本脚本只补三样脚本通常不写的东西：
采样数、分辨率、是否开降噪——那些属于“出图设置”而不是“场景定义”。

## 三个坑（都实测踩过）

1. `--factory-startup` 会重置渲染引擎为 EEVEE。所以必须在**执行场景脚本之后**
   再改成 Cycles，否则脚本里设的 `scene.render.engine` 会被覆盖。
2. 脚本里可能写 `bpy.ops.wm.save_mainfile` 之类，在 background 下要挡住。
3. AgX 的 look 必须显式设 `'None'`：Blender 5.x 的默认 look 不是 None，
   而霜天侧没有 look 概念。
"""
import bpy
import sys
import os
import math

argv = sys.argv
if "--" not in argv:
    print("## ERROR 缺少参数")
    sys.exit(1)
rest = argv[argv.index("--") + 1:]
scene_path = rest[0]
out_path = rest[1]
engine = rest[2] if len(rest) > 2 else "cycles"
samples = int(rest[3]) if len(rest) > 3 else 256

scene_path = os.path.abspath(scene_path)
print("## 执行场景脚本:", scene_path)

# 脚本里常用 `bpy.context.scene`；factory-startup 已有一个空场景
with open(scene_path, "r", encoding="utf-8") as handle:
    source = handle.read()
code = compile(source, scene_path, "exec")
exec(code, {"__name__": "__main__", "__file__": scene_path})

sc = bpy.context.scene

# ── 出图设置（在场景脚本之后，理由见文件头坑 1）
sc.render.engine = 'CYCLES' if engine == "cycles" else 'BLENDER_EEVEE'
if engine == "cycles":
    sc.cycles.device = 'CPU'
    sc.cycles.samples = samples
    sc.cycles.use_denoising = False
else:
    sc.eevee.taa_render_samples = samples

# 分辨率：与霜天侧一致（1100×800，与其他对比图同口径）
sc.render.resolution_x = 1100
sc.render.resolution_y = 800
sc.render.resolution_percentage = 100
sc.render.dither_intensity = 0.0
# `filter_size = 0.01`：几乎关掉像素过滤。霜天那边没有 resolution filter，
# 两边都关掉才可比（默认 1.5 会让边缘多出几级差异）。
sc.render.filter_size = 0.01

# 色调映射：霜天的 AgX 是 **OCIO 的 AgX**（不带 look）。
sc.view_settings.view_transform = 'AgX'
sc.view_settings.look = 'None'
sc.view_settings.exposure = 0.0
sc.view_settings.gamma = 1.0

sc.render.image_settings.file_format = 'PNG'
sc.render.image_settings.color_mode = 'RGB'
sc.render.image_settings.color_depth = '8'
sc.render.filepath = os.path.abspath(out_path)[:-4]
bpy.ops.render.render(write_still=True)
print("## DONE", sc.render.filepath + ".png")
print("## 相机:", [round(v, 3) for v in sc.camera.location] if sc.camera else "无")
print("## 物体数:", len(sc.objects))

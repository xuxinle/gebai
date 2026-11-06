"""把 AgX 变换链的**每一环**用 Blender 自己算出来，供本框架硬编码对账。

## 为什么要这样做（而不是手抄 config.ocio 的矩阵）

`config.ocio` 里的定义是**间接**的：`ACES2065-1 → Linear FilmLight E-Gamut`
要经过 `ACES2065-1 → XYZ D65` 再乘一个矩阵，而 `ACES2065-1` 又是 ACES AP0
原色。手抄这条链有三次乘法与一次白点适配的机会出错，而错一位就会差几级。

**让 Blender/OCIO 自己算**是唯一能确认「我复现的链就是 Blender 用的链」的方法：
用 `OCIO_ACTIVE_DISPLAYS` + 纯色输入渲一张图，看输出值。

## 产出什么

对一批**单色线性输入**（中性灰 + 若干纯色），同时给出：

1. 8bit PNG 的输出值（= Blender 的 AgX 最终结果）——这是**对账的唯一真值**
2. `Standard` 的输出值（= 纯 sRGB 编码）——用来验证「同一份几何/光照」没变

## 用法

    blender --background --factory-startup --python tools/blender_agx_truth.py -- <outdir>
"""
import bpy
import sys
import os
import json
import math
import traceback

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUTDIR = os.path.abspath(argv[0]) if argv else os.path.abspath(".")

# 要量的线性输入（覆盖暗部 / 中灰 / 亮部 / 高饱和纯色）
# 用 EEVEE 的自发光平面给"精确线性值"，避开光照模型的干扰
CASES = [
    ('gray_0p05', (0.05, 0.05, 0.05)),
    ('gray_0p18', (0.18, 0.18, 0.18)),
    ('gray_0p5', (0.5, 0.5, 0.5)),
    ('gray_1p0', (1.0, 1.0, 1.0)),
    ('gray_2p0', (2.0, 2.0, 2.0)),      # HDR：AgX 的用处在这里
    ('red_1p0', (1.0, 0.0, 0.0)),
    ('green_1p0', (0.0, 1.0, 0.0)),
    ('blue_1p0', (0.0, 0.0, 1.0)),
    ('brass', (0.72, 0.55, 0.24)),
]

VIEWS = ['Standard', 'AgX']


def fresh():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    return bpy.context.scene


def build(view):
    sc = fresh()
    sc.render.engine = 'BLENDER_EEVEE'
    sc.render.resolution_x = 64
    sc.render.resolution_y = 64
    sc.render.resolution_percentage = 100
    sc.render.dither_intensity = 0.0
    sc.render.film_transparent = False
    sc.render.filter_size = 0.01
    sc.view_settings.view_transform = view
    sc.view_settings.look = 'None'
    sc.view_settings.exposure = 0.0
    sc.view_settings.gamma = 1.0
    sc.eevee.taa_render_samples = 1

    cd = bpy.data.cameras.new("C")
    cd.type = 'ORTHO'
    cd.ortho_scale = 1.0
    cam = bpy.data.objects.new("C", cd)
    sc.collection.objects.link(cam)
    cam.location = (0, 0, 5)
    sc.camera = cam
    return sc


def make_emitter(rgb):
    """自发光平面：给出**精确的线性值**，不经过任何光照模型。"""
    bpy.ops.mesh.primitive_plane_add(size=20.0, location=(0, 0, 0))
    pl = bpy.context.active_object
    m = bpy.data.materials.new("EM")
    m.use_nodes = True
    nt = m.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    em = nt.nodes.new("ShaderNodeEmission")
    em.inputs[0].default_value = (rgb[0], rgb[1], rgb[2], 1.0)
    em.inputs[1].default_value = 1.0      # strength 1 → 输出就是 rgb 本身
    nt.links.new(em.outputs[0], out.inputs[0])
    pl.data.materials.append(m)
    return pl


def measure_png(path):
    img = bpy.data.images.load(path)
    img.colorspace_settings.name = 'Non-Color'
    px = img.pixels[:]
    w, h = img.size
    ch = img.channels
    i = ((h // 2) * w + (w // 2)) * ch
    v = tuple(round(px[i + k] * 255) for k in range(3))
    # EXR 线性值（AgX 的输入应当是原始 rgb）
    bpy.data.images.remove(img)
    return v


def run():
    out = {}
    for view in VIEWS:
        for name, rgb in CASES:
            sc = build(view)
            make_emitter(rgb)
            sc.render.image_settings.file_format = 'PNG'
            sc.render.image_settings.color_mode = 'RGB'
            sc.render.image_settings.color_depth = '8'
            p = os.path.join(OUTDIR, "agx_%s_%s.png" % (view, name))
            sc.render.filepath = p[:-4]
            bpy.ops.render.render(write_still=True)
            v = measure_png(p)
            key = "%s|%s" % (view, name)
            out[key] = {'input': list(rgb), 'png': list(v)}
            print("## %-24s in=(%s) -> rgb%s" % (
                key, ", ".join("%.4g" % c for c in rgb), v))
    print("JSON " + json.dumps(out))
    print("AGX_TRUTH_DONE")


try:
    run()
except Exception:
    traceback.print_exc()
    print("AGX_TRUTH_FAIL")

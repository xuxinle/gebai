"""Cycles 真值通道 —— 受控平面。

## 这是「对标 Cycles」的唯一真值源

`docs/bpy_render_parity.md` 的结论全部来自这里。要复算任何一个数字，
只需跑这一个脚本。

## 为什么选 Cycles 而不是 EEVEE

见 `docs/eevee_vs_cycles.md`。一句话：**Cycles 无偏**（加样本收敛到物理解），
**EEVEE 有偏**（屏幕空间 GI，看不到画面外的光源）。实测同一场景里，
一面画面外的高亮墙：Cycles 均值 +34.4，EEVEE 只 +2.5。

而本框架的 BRDF 在 `metal r=0.5` 上与 Cycles 差 p50=1，与 EEVEE 差 +6.21
—— **本框架本来就离 Cycles 更近**，没有理由去对标一个更远的近似。

## 场景：把着色公式压成一个可手算的数

    0.5 灰平面 + 一盏 SUN energy=1.0 正对平面（n·l = n·v = 1）+ 无环境光 + 黑背景

    outgoing = albedo/π × irradiance = 0.5/π × 1.0 = 0.159155
    Standard（= 纯 sRGB 编码，无色调映射） → 8bit = 111

## 五个「少一个画面就不是纯色」的设置

| 设置 | 值 | 不设的后果 |
|---|---|---|
| `dither_intensity` | 0 | 默认 1.0 会加 ±1 级抖动噪声，像素不纯 |
| `film_transparent` | False | 背景变透明而不是黑 |
| world 强度 | 0 | world 会当环境光，平面被额外照亮 |
| `view_transform` | `Standard` | `AgX`（默认）会套 S 曲线，差 36~73 级 |
| `read_factory_settings(use_empty=True)` | — | factory 场景自带 Cube + Light |

## 为什么用 EXR 同时落一份

8bit PNG 里的 111 同时受着色与色调映射影响，错在哪一层看不出来。
EXR 的 32bit float 直接给线性辐亮度，能把两层分开量。

实测过账（`docs/bpy_render_parity.md §2.2`）：Blender 的 `Standard` 就是纯 sRGB 编码，
线性 0.16 → PNG 111，逐字节对得上。

## 确定性

同场景连渲 4 次**逐像素相同**（max=0）。判据必须用像素差，不能用 md5
（PNG 元数据含时间戳，md5 必然不同而画面相同）。

## 用法

    blender --background --factory-startup --python tools/blender_cycles_truth.py -- \
        <outdir> [energies] [engine]

    # 默认：energy 1.0,2.0，Cycles
    # engines: CYCLES / BLENDER_EEVEE（后者仅用于「对比两个引擎」，不是真值）
"""
import bpy
import sys
import os
import json
import math
import traceback

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUTDIR = os.path.abspath(argv[0]) if argv else os.path.abspath(".")
ENERGIES = [float(x) for x in argv[1].split(",")] if len(argv) > 1 else [1.0, 2.0]
ENGINE = argv[2] if len(argv) > 2 else 'CYCLES'
RES = 512

# Cycles 采样数：实测 s128 已与 s512 逐像素相同（无偏的判据），这里取 128 省时间
CYCLES_SAMPLES = 128


def fresh():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    return bpy.context.scene


def setup(sc):
    sc.render.engine = ENGINE
    sc.render.resolution_x = RES
    sc.render.resolution_y = RES
    sc.render.resolution_percentage = 100
    sc.render.dither_intensity = 0.0        # 关键：默认 1.0 会加 ±1 级噪声
    sc.render.film_transparent = False
    sc.render.filter_size = 0.01            # 与「1 样本/像素」口径接近
    sc.view_settings.view_transform = 'Standard'   # 关键：AgX 会套 S 曲线
    sc.view_settings.look = 'None'
    sc.view_settings.exposure = 0.0
    sc.view_settings.gamma = 1.0
    if ENGINE == 'CYCLES':
        sc.cycles.device = 'CPU'
        sc.cycles.samples = CYCLES_SAMPLES
        sc.cycles.use_denoising = False     # 降噪会改变像素值，破坏「与公式对账」
        sc.cycles.seed = 0
        sc.cycles.max_bounces = 12
    else:
        sc.eevee.taa_render_samples = 64

    # world：纯黑零强度（否则会当环境光）
    world = bpy.data.worlds.new("black")
    sc.world = world
    bg = world.node_tree.nodes["Background"]
    bg.inputs[0].default_value = (0.0, 0.0, 0.0, 1.0)
    bg.inputs[1].default_value = 0.0

    # 相机：透视 lens 50 / sensor 36（正方形画幅 → 水平竖直 fov 相同）
    #
    # 为何不用正交：霜天 bpy 兼容层**没有** `camera.ortho_scale`（实测 AttributeError），
    # 而三路要共用同一套相机参数，所以只能用透视。
    cd = bpy.data.cameras.new("probe_camera")
    cd.lens = 50.0
    cd.sensor_width = 36.0
    cam = bpy.data.objects.new("probe_camera", cd)
    sc.collection.objects.link(cam)
    cam.location = (0.0, 0.0, 20.0)
    cam.rotation_euler = (0.0, 0.0, 0.0)
    sc.camera = cam

    # 平面：40×40（半宽 20·tan(19.8°)=7.2，足够填满画面）
    bpy.ops.mesh.primitive_plane_add(size=40.0, location=(0.0, 0.0, 0.0))
    plane = bpy.context.active_object
    plane.name = 'probe_plane'
    mat = bpy.data.materials.new("probe_material")
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = (0.5, 0.5, 0.5, 1.0)
    bsdf.inputs["Metallic"].default_value = 0.0
    bsdf.inputs["Roughness"].default_value = 1.0
    plane.data.materials.append(mat)

    # 平行光：正对平面（n·l = 1）。energy 单位是 W/m² = 辐照度
    sd = bpy.data.lights.new("probe_sun", 'SUN')
    sd.angle = 0.0
    sun = bpy.data.objects.new("probe_sun", sd)
    sc.collection.objects.link(sun)
    sun.rotation_euler = (0.0, 0.0, 0.0)
    return sc, sd


def measure(png_path, exr_path):
    """同时读 8bit PNG 与 EXR 线性值。

    PNG 用 `Non-Color` 读 = 拿到文件里的原始字节 / 255（不二次解码）。
    """
    out = {}
    img = bpy.data.images.load(png_path)
    img.colorspace_settings.name = 'Non-Color'
    px = img.pixels[:]
    w, h = img.size
    ch = img.channels
    i = ((h // 2) * w + (w // 2)) * ch
    out['png'] = tuple(round(px[i + k] * 255) for k in range(3))
    # 全图唯一值数：受控平面应当是纯色（1 或极少）
    uniq = set()
    for y in range(0, h, 4):
        for x in range(0, w, 4):
            j = (y * w + x) * ch
            uniq.add(tuple(round(px[j + k] * 255) for k in range(3)))
    out['png_distinct'] = len(uniq)
    bpy.data.images.remove(img)

    if exr_path and os.path.exists(exr_path):
        img = bpy.data.images.load(exr_path)
        px = img.pixels[:]
        w, h = img.size
        ch = img.channels
        i = ((h // 2) * w + (w // 2)) * ch
        out['linear'] = round(px[i], 6)
        bpy.data.images.remove(img)
    return out


def run():
    results = []
    for energy in ENERGIES:
        sc, sd = setup(fresh())
        sd.energy = energy
        base = os.path.join(OUTDIR, "cycle_truth_flat_e%s" % str(energy).replace('.', 'p'))

        sc.render.image_settings.file_format = 'PNG'
        sc.render.image_settings.color_mode = 'RGB'
        sc.render.image_settings.color_depth = '8'
        sc.render.filepath = base
        bpy.ops.render.render(write_still=True)

        sc.render.image_settings.file_format = 'OPEN_EXR'
        sc.render.image_settings.color_depth = '32'
        sc.render.image_settings.exr_codec = 'ZIP'
        sc.render.filepath = base + "_linear.exr"
        bpy.ops.render.render(write_still=True)

        m = measure(base + '.png', base + '_linear.exr')
        # 手算期望值，一起打出来（判据必须可对账，不能只信实测）
        expected_linear = 0.5 / math.pi * energy
        expected_srgb = round(255.0 * (
            12.92 * expected_linear if expected_linear <= 0.0031308
            else 1.055 * (expected_linear ** (1 / 2.4)) - 0.055)) if expected_linear < 1.0 else 255
        rec = {'engine': ENGINE, 'energy': energy, 'expected_linear': round(expected_linear, 6),
               'expected_srgb8': min(255, expected_srgb), **m}
        results.append(rec)
        print("## %-13s energy=%-6.2f linear=%-10s expected=%-10s png=%-16s expect_srgb8=%d distinct=%d" % (
            ENGINE, energy, m.get('linear'), round(expected_linear, 6),
            str(m.get('png')), rec['expected_srgb8'], m.get('png_distinct')))
    print("JSON " + json.dumps(results))
    print("CYCLES_TRUTH_DONE")


try:
    run()
except Exception:
    traceback.print_exc()
    print("CYCLES_TRUTH_FAIL")

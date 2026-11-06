"""Cycles 真值通道 —— 球面 BRDF 三档。

## 为什么受控平面不够

受控平面只覆盖「漫反射 + n·l=1」这一个点。而球面在一张图里覆盖：

| 覆盖到的物理量 | 怎么覆盖 |
|---|---|
| `n·l ∈ [0,1]` 全区间 | 球面各处法线朝向不同 |
| 掠射角菲涅耳 | 球边缘的入射/视线夹角接近 90° |
| 高光瓣形状 | 高光在球面上展开成可见的椭圆 |
| `D` / `V` / `F` 三个项 | 三档材质分别放大各自的敏感度 |

## 三档（对应 `docs/bpy_render_parity.md §4`）

| 档 | metal | rough | 用作判据的东西 |
|---|---|---|---|
| `diffuse` | 0.0 | 1.0 | 漫反射归一化（`albedo/π`）、`n·l` 形状 → 本框架**已对齐**（p50=1） |
| `metal_smooth` | 1.0 | 0.05 | 高光瓣宽度。无环境光时**几乎全黑**（物理正确：纯直接光的金属只有窄镜面瓣） |
| `metal_rough` | 1.0 | 0.5 | 粗糙高光的能量。本框架与 Cycles 差 p50=1 |

## 一个必须记住的物理事实

`metal_smooth` 在无环境光下主体均值只有 18.4、最大 66——**不是渲染坏了**。
纯直接光的金属只有镜面反射瓣，粗糙度极低时瓣极窄，除高光核心外全黑。
把「金属应该亮」当成本能预期会误判这一档。

## 场景设置与受控平面一致

单盏 SUN energy=1.0、**不给 world**（无环境光）、`Standard`（纯 sRGB 编码）、
`dither=0`、`filter_size=0.01`。这样三档之间只差材质。

## 用法

    blender --background --factory-startup --python tools/blender_cycles_sphere_truth.py -- \
        <outdir> [engine]

产物：`sphere_<档>_<engine>.png`，供 `tools/bpy_brdf_compare` 逐像素比。
"""
import bpy
import sys
import os
import json
import traceback

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUTDIR = os.path.abspath(argv[0]) if argv else os.path.abspath(".")
ENGINE = argv[1] if len(argv) > 1 else 'CYCLES'
RES = 400
CYCLES_SAMPLES = 128

# (档名, metallic, roughness)
CASES = [
    ('diffuse', 0.0, 1.0),
    ('metal_smooth', 1.0, 0.05),
    ('metal_rough', 1.0, 0.5),
]


def fresh():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    return bpy.context.scene


def build(metal, rough):
    sc = fresh()
    sc.render.engine = ENGINE
    sc.render.resolution_x = RES
    sc.render.resolution_y = RES
    sc.render.resolution_percentage = 100
    sc.render.dither_intensity = 0.0
    sc.render.film_transparent = False
    sc.render.filter_size = 0.01
    sc.view_settings.view_transform = 'Standard'
    sc.view_settings.look = 'None'
    sc.view_settings.exposure = 0.0
    sc.view_settings.gamma = 1.0
    if ENGINE == 'CYCLES':
        sc.cycles.device = 'CPU'
        sc.cycles.samples = CYCLES_SAMPLES
        sc.cycles.use_denoising = False
        sc.cycles.seed = 0
        sc.cycles.max_bounces = 12
    else:
        sc.eevee.taa_render_samples = 64

    # 不给 world：与「bpy 脚本没写 world」同义，即无环境光
    cd = bpy.data.cameras.new("C")
    cd.lens = 50.0
    cd.sensor_width = 36.0
    cam = bpy.data.objects.new("C", cd)
    sc.collection.objects.link(cam)
    cam.location = (0.0, 0.0, 6.0)
    cam.rotation_euler = (0.0, 0.0, 0.0)
    sc.camera = cam

    # 球：64×32 段（够光滑，边缘无可见棱）
    bpy.ops.mesh.primitive_uv_sphere_add(segments=64, ring_count=32, radius=1.5,
                                         location=(0.0, 0.0, 0.0))
    sp = bpy.context.active_object
    m = bpy.data.materials.new("M")
    m.use_nodes = True
    b = m.node_tree.nodes["Principled BSDF"]
    b.inputs["Base Color"].default_value = (0.5, 0.5, 0.5, 1.0)
    b.inputs["Metallic"].default_value = metal
    b.inputs["Roughness"].default_value = rough
    sp.data.materials.append(m)

    # 平行光：斜射（绕 Y 转 0.9 rad），让球面同时有受光面/背光面/掠射区
    sd = bpy.data.lights.new("S", 'SUN')
    sd.energy = 1.0
    sd.angle = 0.0
    s = bpy.data.objects.new("S", sd)
    sc.collection.objects.link(s)
    s.rotation_euler = (0.0, 0.9, 0.0)

    sc.render.image_settings.file_format = 'PNG'
    sc.render.image_settings.color_mode = 'RGB'
    sc.render.image_settings.color_depth = '8'
    return sc


def stats(path):
    img = bpy.data.images.load(path)
    img.colorspace_settings.name = 'Non-Color'
    px = img.pixels[:]
    w, h = img.size
    ch = img.channels
    vals = []
    for y in range(h):
        for x in range(w):
            i = (y * w + x) * ch
            vals.append((px[i] + px[i + 1] + px[i + 2]) / 3.0 * 255.0)
    vals.sort()
    n = len(vals)
    nz = [v for v in vals if v > 1.0]
    bpy.data.images.remove(img)
    return {
        'mean_all': round(sum(vals) / n, 2),
        # 主体均值：亮度 > 1 的像素（排除黑背景）。判据看这个，不看全图
        # （全图均值被背景占空比主导，而背景在三种档里完全相同）
        'subject_mean': round(sum(nz) / len(nz), 2) if nz else 0.0,
        'subject_px': len(nz),
        'p90': round(vals[int(n * 0.9)], 1),
        'max': round(vals[-1], 1),
    }


def run():
    out = {}
    for name, metal, rough in CASES:
        sc = build(metal, rough)
        p = os.path.join(OUTDIR, "sphere_%s_%s.png" % (name, ENGINE))
        sc.render.filepath = p[:-4]
        bpy.ops.render.render(write_still=True)
        s = stats(p)
        out[name] = s
        print("## %-13s %s" % (name, json.dumps(s)))
    print("JSON " + json.dumps(out))
    print("SPHERE_TRUTH_DONE")


try:
    run()
except Exception:
    traceback.print_exc()
    print("SPHERE_TRUTH_FAIL")

#!/usr/bin/env python3
"""验证裁云三维的**组合能力**：`model_script` 提供几何，`model_*` 叠加姿态与相机。

## 要证的三件事

1. **纯场景**：只给 `model_script`，画面正常（几何来自场景）
2. **姿态叠加**：加 `model_spin`/`model_tilt`/`model_bob`，画面**必须变**
   （若不变，说明叠加没接上——而那正是这个功能存在的意义）
3. **相机叠加**：打开 `model_use_layer_camera` 并改 `cam_azimuth`，画面**必须变**
   （且与只改姿态的变化不同）

判据一律用**像素差异**：这三件事都不报错，只能靠"画面是否真的不同"来区分。
"""
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CAIYUN = os.path.join(ROOT, "build", "dev", "bin", "caiyun.exe")
OUT = os.path.join(ROOT, "build", "e2e-caiyun-model")
SCENE = os.path.join(ROOT, "examples", "jianmu", "scenes", "04-watch.js")


def decode_png(path):
    import struct
    import zlib

    with open(path, "rb") as handle:
        data = handle.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise RuntimeError("不是 PNG")
    pos = 8
    width = height = None
    bit_depth = color_type = None
    idat = bytearray()
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        payload = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, bit_depth, color_type = struct.unpack(">IIBB", payload[:10])
        elif kind == b"IDAT":
            idat += payload
        elif kind == b"IEND":
            break
    channels = 4 if color_type == 6 else 3
    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    out = bytearray(width * height * channels)
    prev = bytearray(stride)
    offset = 0
    for row in range(height):
        filter_type = raw[offset]
        offset += 1
        line = bytearray(raw[offset:offset + stride])
        offset += stride
        if filter_type == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif filter_type == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif filter_type == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif filter_type == 4:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        out[row * stride:(row + 1) * stride] = line
        prev = line
    return width, height, channels, bytes(out)


def diff_ratio(path_a, path_b):
    wa, ha, ca, pa = decode_png(path_a)
    wb, hb, cb, pb = decode_png(path_b)
    if (wa, ha) != (wb, hb) or ca != cb:
        return 1.0
    total = wa * ha
    different = 0
    for i in range(total):
        base = i * ca
        if (pa[base] != pb[base] or pa[base + 1] != pb[base + 1]
                or pa[base + 2] != pb[base + 2]):
            different += 1
    return different / total


def painted_ratio(path):
    width, height, channels, pixels = decode_png(path)
    bg = pixels[0:3]
    total = width * height
    painted = 0
    for i in range(total):
        base = i * channels
        dist = (abs(pixels[base] - bg[0]) + abs(pixels[base + 1] - bg[1])
                + abs(pixels[base + 2] - bg[2]))
        if dist > 12:
            painted += 1
    return painted / total


def run_layer(label, layer):
    """渲染一个三维图层到 PNG（取时间轴中间那一帧）。

    ## 为什么必须走完整导演脚本

    要验的是**叠加层**（model_spin / cam_* 这些**图层级**字段），
    而 --preview-model 吃的是场景 spec 本身、根本看不到图层字段——
    用它验叠加层会得到"永远不变"的假绿。
    """
    spec_path = os.path.join(OUT, "spec-%s.json" % label)
    frames_dir = os.path.join(OUT, "frames-" + label)
    os.makedirs(frames_dir, exist_ok=True)
    for stale in os.listdir(frames_dir):
        os.remove(os.path.join(frames_dir, stale))

    # kind=custom 是必需的：缺了它 schema 会报
    # "scenes[0].kind 不认识：「」" 然后**回退到内置歌白宣传片**，
    # 于是探针拿到完全无关的画面（实测踩过，排查了很久）。
    spec = {
        "fps": 10,
        "scenes": [{
            "kind": "custom",
            "duration": 1.0,
            "layers": [dict({
                "kind": "model3d",
                "name": "watch",
                "x": 0, "y": 0, "w": 720, "h": 720,
                "model_script": open(SCENE, encoding="utf-8").read(),
            }, **layer)],
        }],
    }
    with open(spec_path, "w", encoding="utf-8") as handle:
        json.dump(spec, handle, ensure_ascii=False)

    result = subprocess.run(
        [CAIYUN, "--headless", "--strict", "--script", spec_path,
         "--export-dir", frames_dir, "--no-stats"],
        capture_output=True, text=True, timeout=300,
        encoding="utf-8", errors="replace")

    # ## 脚本被拒时必须直接失败（不能继续拿内置片子去比像素）
    #
    # 实测踩过：spec 少一个字段 → 裁云打印一行提示后**回退到内置歌白宣传片**
    # 继续导出 300 帧。探针于是拿到"三联卡版式"，跟被验的图层毫无关系，
    # 而所有像素判据都在这个错误前提上运行——差异恒为 0，看起来像"叠加没生效"。
    combined = (result.stdout or "") + (result.stderr or "")
    if "改用内置歌白宣传片" in combined or "脚本解析失败" in combined:
        raise RuntimeError("脚本被拒并回退到内置片子（探针前提失效）：\n" + combined[-800:])

    produced = sorted(f for f in os.listdir(frames_dir) if f.endswith(".png"))
    # ## 没出帧必须直接失败
    #
    # 实测踩过：schema 拒了新字段 → 脚本没启动 → 帧目录是**空的** →
    # 探针拿到空路径 → 差异恒为 0.0% → 看起来像“叠加没生效”，
    # 实际是“根本没渲染”。空集合当成“没差异”是这类探针最隐蔽的假阴性。
    if not produced:
        raise RuntimeError("没有导出任何帧（渲染未发生）：\n" + combined[-600:])
    # 取**中间**那一帧：首帧可能落在入场动画起点（全透明）
    png_path = os.path.join(frames_dir, produced[len(produced) // 2])
    return png_path, result


def main():
    os.makedirs(OUT, exist_ok=True)
    if not os.path.exists(CAIYUN):
        print("找不到 caiyun.exe：%s" % CAIYUN)
        return 2
    if not os.path.exists(SCENE):
        print("找不到场景：%s" % SCENE)
        return 2

    failures = []

    def check(name, ok, detail=""):
        print("  [%s] %-44s %s" % ("PASS" if ok else "FAIL", name, detail))
        if not ok:
            failures.append(name)

    # ① 纯场景
    plain, r1 = run_layer("plain", {})
    if not os.path.exists(plain):
        print("渲染失败：%s" % (r1.stderr or r1.stdout)[-800:])
        return 1
    check("纯场景能出图", painted_ratio(plain) > 0.05,
          "%.1f%%" % (painted_ratio(plain) * 100))

    # ② 姿态叠加
    posed, r2 = run_layer("posed", {"model_spin": 120.0, "model_tilt": 25.0})
    pose_diff = diff_ratio(plain, posed) if os.path.exists(posed) else 0.0
    check("model_spin/tilt 叠加生效", pose_diff > 0.02, "%.1f%%" % (pose_diff * 100))

    # ③ 相机叠加
    cam, r3 = run_layer("camera", {"model_use_layer_camera": True,
                                   "cam_azimuth": 120.0, "cam_elevation": 40.0})
    cam_diff = diff_ratio(plain, cam) if os.path.exists(cam) else 0.0
    check("model_use_layer_camera 叠加生效", cam_diff > 0.02, "%.1f%%" % (cam_diff * 100))
    if os.path.exists(cam) and os.path.exists(posed):
        check("相机与姿态是两种不同效果", diff_ratio(posed, cam) > 0.02,
              "%.1f%%" % (diff_ratio(posed, cam) * 100))

    print()
    if failures:
        print("失败 %d 项：%s" % (len(failures), ", ".join(failures)))
        return 1
    print("裁云三维组合能力全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())

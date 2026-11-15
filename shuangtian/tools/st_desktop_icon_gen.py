"""按 `src/app/splash.cpp` 的**权威几何**画出霜天六角雪花图标（256×256 PNG）。

## 为什么是"照抄几何"而不是另画一个雪花

霜天的品牌标记不是一张位图文件，而是启动卡里**用代码画出来的**（`splash.cpp`
的 `draw mark` 段）：三条贯穿主干（0°/60°/120°）+ 六个枝尖各一对 ±52° 短分叉
+ 中心小圆点。几何一旦改动，启动卡上的标就与任务栏、桌面上的标不一致。

所以本脚本把那几行参数逐条搬过来（半径、线宽、分叉角度与长度都取同一比例），
只把"画布"从启动卡换成 256×256 的图标卡。

## 画布规格取自同目录的其它应用图标（实测，不是猜）

`apps/*/assets/*-icon.png` 全部是 256×256 RGBA：
- 圆角半径 ≈ 23px（占边长 **9%**）
- 四角**完全透明**（alpha=0），不是不透明白底
- 底色为单一实色，图形用白色系

## 配色

霜天没有既定品牌色（`st.pkg` 里 `st_desktop` 的 `icon` 是空的，说明它此前
只有 PE 资源、没有位图标）。这里取**冷钢蓝 `#4C7ED6`**：与"霜天曉角"的清冽、
与其它七件套的饱和实色风格都相容，且不与任一既有底色重复。

## 输出两份（同源，必须一起更新）

| 文件 | 用途 |
|---|---|
| `apps/st_desktop/assets/st_desktop-icon.png` | 嵌进 exe，桌面/Dock 上的图标（256²） |
| `apps/st_desktop/assets/st_desktop.ico` | exe 的 PE 资源图标（标题栏/任务栏/资源管理器） |

`.ico` 不是另画一套，而是把同一份像素按 Windows 惯例降采样成 16/24/32/48/64/128/256
七档（256 那档按惯例内嵌 PNG，其余为 32bpp DIB）——与 `gebai.ico` 的档位完全一致。

    python tools/st_desktop_icon_gen.py
"""

import math
import os
import struct
import zlib

SIZE = 256
RADIUS_RATIO = 23.0 / 256.0        # 实测自同目录其它应用图标
BG = (0x4C, 0x7E, 0xD6)            # 冷钢蓝
INK = (255, 255, 255)              # 与其它图标一致的白色图形

# —— 以下比例全部照抄 `src/app/splash.cpp` ——
MARK_R_RATIO = 0.92         # mark_radius * 0.92
SPINE_W_RATIO = 0.155       # spine_w = mark_r * 0.155
BARB_ANGLE = 0.91           # ±52° = 0.91 rad
BARB_LEN = 0.30             # 分叉长 = r * 0.30
BARB_W_RATIO = 0.80         # 分叉线宽 = spine_w * 0.80
CENTER_DOT_RATIO = 1.15     # 中心点半径 = spine_w * 1.15

SUPERSAMPLE = 4             # 4×4 超采样做抗锯齿（图标必须边缘干净）


def rounded_square_distance(x, y, size, radius):
    """点 (x,y) 到圆角方块边界的**有符号距离**（负 = 在内）。"""
    half = size / 2.0
    px, py = abs(x - half), abs(y - half)
    dx, dy = px - (half - radius), py - (half - radius)
    if dx > 0 and dy > 0:
        return math.hypot(dx, dy) - radius
    return max(dx, dy) - radius


def segment_distance(px, py, ax, ay, bx, by):
    """点到线段的距离（雪花所有笔画都是线段，用胶囊模型求覆盖）。"""
    vx, vy = bx - ax, by - ay
    wx, wy = px - ax, py - ay
    length_sq = vx * vx + vy * vy
    t = 0.0 if length_sq == 0 else max(0.0, min(1.0, (wx * vx + wy * vy) / length_sq))
    return math.hypot(px - (ax + t * vx), py - (ay + t * vy))


def snowflake_segments(cx, cy, r):
    """返回 [(x1,y1,x2,y2,半宽)]——完全对应 splash.cpp 的三组笔画。"""
    segments = []
    spine_w = max(1.1, r * SPINE_W_RATIO)

    # ① 三条贯穿主干：0° / 60° / 120°
    for axis in range(3):
        angle = math.pi * axis / 3.0
        dx, dy = math.cos(angle), math.sin(angle)
        segments.append((cx - dx * r, cy - dy * r, cx + dx * r, cy + dy * r, spine_w / 2.0))

    # ② 六个枝尖的短分叉（主干方向 ±0.91rad，长度 30%）
    for tip in range(6):
        angle = math.pi * tip / 3.0
        dx, dy = math.cos(angle), math.sin(angle)
        ex, ey = cx + dx * r, cy + dy * r
        for side in (-1, 1):
            off = BARB_ANGLE * side
            bx, by = math.cos(angle + off), math.sin(angle + off)
            segments.append((ex, ey, ex + bx * r * BARB_LEN, ey + by * r * BARB_LEN,
                             spine_w * BARB_W_RATIO / 2.0))
    return segments, spine_w


def build_pixels():
    size = SIZE
    radius = size * RADIUS_RATIO
    center = size / 2.0
    # 留白比例：其它七件套的图形都落在方块内约 70% 的范围里（实测 gallery 的九宫格
    # 距边缘约 36px / 256 = 14%）。雪花是放射状，视觉上比块状图形更"撑"，
    # 所以再收一点——0.64 时枝尖离边约 15%，与九宫格观感一致。
    mark_r = size * 0.5 * MARK_R_RATIO * 0.64
    segments, spine_w = snowflake_segments(center, center, mark_r)
    dot_r = spine_w * CENTER_DOT_RATIO

    pixels = bytearray(size * size * 4)
    step = 1.0 / SUPERSAMPLE
    for y in range(size):
        for x in range(size):
            bg_cover = 0
            ink_cover = 0
            for sy in range(SUPERSAMPLE):
                for sx in range(SUPERSAMPLE):
                    px = x + (sx + 0.5) * step
                    py = y + (sy + 0.5) * step
                    if rounded_square_distance(px, py, size, radius) > 0:
                        continue
                    bg_cover += 1
                    inside = False
                    for (ax, ay, bx, by, half_w) in segments:
                        if segment_distance(px, py, ax, ay, bx, by) <= half_w:
                            inside = True
                            break
                    if not inside and math.hypot(px - center, py - center) <= dot_r:
                        inside = True
                    if inside:
                        ink_cover += 1
            total = SUPERSAMPLE * SUPERSAMPLE
            if bg_cover == 0:
                continue                       # 圆角外：保持全透明
            o = (y * size + x) * 4
            alpha = int(round(255 * bg_cover / total))
            # 墨迹按"占方块内面积"的比例混色，避免边缘发灰
            mix = ink_cover / bg_cover
            pixels[o] = int(round(BG[0] * (1 - mix) + INK[0] * mix))
            pixels[o + 1] = int(round(BG[1] * (1 - mix) + INK[1] * mix))
            pixels[o + 2] = int(round(BG[2] * (1 - mix) + INK[2] * mix))
            pixels[o + 3] = alpha
    return bytes(pixels)


def write_png(path, pixels, size):
    raw = bytearray()
    stride = size * 4
    for y in range(size):
        raw.append(0)                          # filter type 0
        raw += pixels[y * stride:(y + 1) * stride]

    def chunk(kind, data):
        body = kind + data
        return (struct.pack(">I", len(data)) + body
                + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))

    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    blob = (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", header)
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
            + chunk(b"IEND", b""))
    with open(path, "wb") as handle:
        handle.write(blob)
    return len(blob)




def _downscale(pixels, size, target):
    """面积平均降采样（**在预乘 alpha 空间**做，否则透明边缘会发黑）。

    直接对非预乘像素求平均，透明区的 RGB（可能是 0）会混进边缘，
    得到一圈灰黑描边——这是图标缩小最常见的丑陋缺陷。
    """
    factor = size // target
    out = bytearray(target * target * 4)
    for y in range(target):
        for x in range(target):
            r = g = b = a = 0
            for sy in range(factor):
                for sx in range(factor):
                    o = ((y * factor + sy) * size + (x * factor + sx)) * 4
                    pa = pixels[o + 3]
                    r += pixels[o] * pa
                    g += pixels[o + 1] * pa
                    b += pixels[o + 2] * pa
                    a += pa
            n = factor * factor
            avg_a = a // n
            dst = (y * target + x) * 4
            if a == 0:
                out[dst:dst + 4] = b"\x00\x00\x00\x00"
            else:
                out[dst] = min(255, r // a)
                out[dst + 1] = min(255, g // a)
                out[dst + 2] = min(255, b // a)
                out[dst + 3] = avg_a
    return bytes(out)


def _dib(pixels, size):
    """32bpp BGRA 自下而上 + AND 掩码（全 0；Windows 用 alpha 通道）。"""
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0,
                         size * size * 4, 0, 0, 0, 0)
    body = bytearray()
    for y in range(size - 1, -1, -1):
        for x in range(size):
            o = (y * size + x) * 4
            body += bytes((pixels[o + 2], pixels[o + 1], pixels[o],
                           pixels[o + 3]))
    # AND 掩码：每行按 4 字节对齐，1bpp，全透明位应为 1；这里全 0（交由 alpha 决定）
    row_bytes = ((size + 31) // 32) * 4
    return header + bytes(body) + bytes(row_bytes * size)


def write_ico(path, pixels, size):
    sizes = [16, 24, 32, 48, 64, 128, 256]
    blobs = []
    for target in sizes:
        if target == 256:
            # 惯例：256 档内嵌 PNG（体积小得多，Vista+ 全支持）
            tmp = path + ".256.png"
            write_png(tmp, pixels, size)
            blobs.append(open(tmp, "rb").read())
            os.remove(tmp)
        else:
            blobs.append(_dib(_downscale(pixels, size, target), target))

    out = bytearray(struct.pack("<HHH", 0, 1, len(sizes)))
    offset = 6 + 16 * len(sizes)
    for target, blob in zip(sizes, blobs):
        dim = 0 if target >= 256 else target
        out += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
    for blob in blobs:
        out += blob
    with open(path, "wb") as handle:
        handle.write(bytes(out))
    return len(out)

def main():
    out = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                       "apps", "st_desktop", "assets", "st_desktop-icon.png")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    pixels = build_pixels()
    written = write_png(out, pixels, SIZE)
    print(f"写出 {out}（{written} 字节）")

    ico = os.path.join(os.path.dirname(out), "st_desktop.ico")
    ico_size = write_ico(ico, pixels, SIZE)
    print(f"写出 {ico}（{ico_size} 字节，7 档：16/24/32/48/64/128/256）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

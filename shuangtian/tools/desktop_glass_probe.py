"""玻璃「透亮度」量具：对**已有截图**算两个正交量，跨档位比。

## 为什么必须两个正交量

用户说“不够透亮”，而这两个字包含两件**互相独立**的事：

| 量 | 定义 | 只看它会被怎么骗 |
|---|---|---|
| **亮度差** = Dock − 紧邻壁纸 | “亮不亮” | 把底色改成浅灰就能任意抬高，板子仍是一块死色 |
| **纹理透过率** = Dock 内部标准差 / 背景标准差 | “透不透” | 只看它会以为调 tint 有用；而真因是 blur 把细纹抹平了 |

两个都高才叫“透亮”；只升一个必然被用户驳回（本仓实测过这类反复）。

## 第三个数：可读性

玻璃再透也不能让 Dock 上的文字读不出来。`Dock 极差`（内部亮度极值差）
过大意味着背后纹理太抢——它逼近壁纸极差时，文字就开始难读。

## 实测结论（2026-11-15，本工具量的）

| 档位 | 亮度差 | 纹理透过 | Dock 极差 |
|---|---|---|---|
| blur 22 + 深底 `0x14..@0xB4` | **−13.3**（压暗） | 39% | 17.8 |
| blur 22 + 浅底 tint .34 | +12.9 | 42% | 22.3 |
| **blur 6 + 浅底 tint .30（采用）** | **+11.4** | **54%** | 31.5 |
| blur 4 + tint .18 | +4.8 | 62% | 36.8 |

`blur` 是“透不透”的主因（2~3px 的石纹被 22px 半径全抹平），`tint` 只影响“亮不亮”。
4px 那一档纹理更透、但玻璃观感开始退化（像半透明而不是毛玻璃）。

## 用法

先用霜天子Agent 工具截几档图（或应用自带 `--shots`），再：

    python tools/desktop_glass_probe.py shots/glass-blur6.png shots/glass-final.png

截图必须是**整窗**、且 Dock 在画面底部（本工具按窗口高度推 Dock 的 y 区间）。
"""

import os
import struct
import sys
import zlib


def read_png(path):
    data = open(path, "rb").read()
    pos = 8
    idat = b""
    w = h = chn = 0
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            w, h, _bd, ct = struct.unpack(">IIBB", chunk[:10])
            chn = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ct]
        elif kind == b"IDAT":
            idat += chunk
        elif kind == b"IEND":
            break
    raw = zlib.decompress(idat)
    stride = w * chn
    out = bytearray(h * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        if f == 1:
            for i in range(chn, stride):
                line[i] = (line[i] + line[i - chn]) & 255
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride):
                a = line[i - chn] if i >= chn else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i - chn] if i >= chn else 0
                b = prev[i]
                c = prev[i - chn] if i >= chn else 0
                pp = a + b - c
                pa, pb, pc = abs(pp - a), abs(pp - b), abs(pp - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, chn, bytes(out)


def measure(path):
    w, h, chn, px = read_png(path)

    def at(x, y):
        o = (y * w + x) * chn
        return px[o], px[o + 1], px[o + 2]

    def lum(c):
        return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]

    def stats(x0, x1, y0, y1):
        vals = [lum(at(x, y)) for y in range(y0, y1) for x in range(x0, x1)]
        m = sum(vals) / len(vals)
        var = sum((v - m) ** 2 for v in vals) / len(vals)
        return m, var ** 0.5, (max(vals) - min(vals))

    # Dock 几何：高度 72、底边距 12 → 顶缘在 h-84，下端在 h-12。
    # 取样区避开中央图标，取左右两段（图标只占中央约 400px）。
    dock_top = h - 84
    dock_y0, dock_y1 = dock_top + 6, h - 18
    wall_y0, wall_y1 = dock_top - 76, dock_top - 16
    left = (int(w * 0.13), int(w * 0.32))
    right = (int(w * 0.70), int(w * 0.93))

    dl = stats(left[0], left[1], dock_y0, dock_y1)
    dr = stats(right[0], right[1], dock_y0, dock_y1)
    wl = stats(left[0], left[1], wall_y0, wall_y1)
    wr = stats(right[0], right[1], wall_y0, wall_y1)

    dock_mean = (dl[0] + dr[0]) / 2
    dock_sd = (dl[1] + dr[1]) / 2
    dock_span = (dl[2] + dr[2]) / 2
    wall_mean = (wl[0] + wr[0]) / 2
    wall_sd = (wl[1] + wr[1]) / 2
    wall_span = (wl[2] + wr[2]) / 2
    return {
        "size": (w, h),
        "dock_mean": dock_mean,
        "wall_mean": wall_mean,
        "delta": dock_mean - wall_mean,
        "texture_pass": (dock_sd / wall_sd * 100.0) if wall_sd > 0 else 0.0,
        "dock_span": dock_span,
        "wall_span": wall_span,
    }


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    print(f"{'截图':34s} {'Dock':>6s} {'壁纸':>6s} {'亮度差':>7s} {'纹理透过':>8s} "
          f"{'Dock极差':>8s} {'壁纸极差':>8s}")
    print("-" * 86)
    for path in argv:
        if not os.path.exists(path):
            print(f"{path:34s}  （文件不存在）")
            continue
        m = measure(path)
        print(f"{os.path.basename(path):34s} {m['dock_mean']:6.1f} {m['wall_mean']:6.1f} "
              f"{m['delta']:+7.1f} {m['texture_pass']:7.0f}% "
              f"{m['dock_span']:8.1f} {m['wall_span']:8.1f}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

"""屏幕像素量尺：行带 / 文字墨迹 / 光标 三者的纵向关系。

这是**唯一的屏上量尺**。此前每换一个问法就新写一个脚本（`row_bands` / `row_center_check`
/ `row_center_final` / `row_parts_histogram` / `user_shot_check` 五个），互相只差几行——
正是「重复编写」，本次合并成一个（子命令区分口径）。

## 为什么需要「屏上」量而不是只信几何
组件自报的 `line_height` / `text_offset` 是**几何声明**；屏幕上真正画成什么样
要另量。两者不一致时（实测踩过：几何说已居中，屏上偏 6.6px）只有屏上量能发现。

## 判据（这几轮踩出来的，改这个文件前先看）
1. **底纹探针列必须永远空白**（编辑器最右侧 x=1860）——放在文字上会把墨迹色当成底色。
2. **光标是半透明叠绘，边缘不是饱和蓝**（实测 `(212,196,224)` 这种淡紫），
   按"蓝色判据"排不干净、窄窗也排不到（边缘在光标列外 10px）——要整段按 x 窗口排除。
3. **判据要自适应就地底色**：行号是灰、代码是彩色，固定阈值会把某一边整片误判。
4. **行号槽含分隔线**（实测 x 481..482），取样窗要避开，否则"墨迹高"会等于整带高。

## 用法
```
python tools/row_ink_probe.py bands  <shot.png> [探针x]           # 找当前行带
python tools/row_ink_probe.py center <shot.png> [带顶] [带底] [步长]  # 行号/代码 居中核对
python tools/row_ink_probe.py parts  <shot.png> <带顶> <带底> [x分割] # 一行内 左段/右段 同底核对
python tools/row_ink_probe.py scan   <shot.png>                   # 未知图：先看纵向着色与墨迹带
```

前两个子命令需要"带顶/带底"，用 `bands` 先求；`center` 省略时会在图里自动找当前行带。
"""

import sys

from PIL import Image

PAGE = (232, 232, 238)
HIGHLIGHT = (226, 226, 237)
GUTTER = (488, 556)          # 行号槽（避开 481..482 的分隔线）
CODE = (566, 1850)           # 代码区
PROBE_X = 1860               # 底纹探针列：编辑器最右，永远空白


def dist(a, b):
    return sum(abs(a[i] - b[i]) for i in range(3))


def find_bands(pixels, probe_x, height, limit=3):
    """找当前行带（连续为 HIGHLIGHT 的 y 段）。"""
    ys = [y for y in range(60, height - 60) if dist(pixels[probe_x, y], HIGHLIGHT) <= 4]
    runs = []
    start = prev = None
    for y in ys:
        if start is None:
            start = prev = y
        elif y == prev + 1:
            prev = y
        else:
            runs.append((start, prev))
            start = prev = y
    if start is not None:
        runs.append((start, prev))
    return runs[:limit]


def caret_span(pixels, top, bottom, x0, x1, width):
    """光标列及其半透明边缘（返回 (列, 排除窗)）。"""
    caret, best = None, 0
    for x in range(x0, min(x1, width - 1)):
        n = sum(1 for y in range(top, bottom + 1)
                if pixels[x, y][2] - pixels[x, y][0] > 70 and pixels[x, y][2] > 140)
        if n > best:
            caret, best = x, n
    if caret is None or best < 6:
        return None, None
    lo = hi = caret
    while lo > 1 and any(pixels[lo - 1, y][2] - pixels[lo - 1, y][0] > 20
                         for y in range(top, bottom + 1)):
        lo -= 1
    while hi < width - 2 and any(pixels[hi + 1, y][2] - pixels[hi + 1, y][0] > 20
                                 for y in range(top, bottom + 1)):
        hi += 1
    return caret, (lo - 12, hi + 12)


def ink_rows(pixels, x0, x1, y0, y1, skip=(), need=1):
    rows = []
    for y in range(y0, y1 + 1):
        n = 0
        for x in range(x0, x1):
            if any(lo <= x <= hi for lo, hi in skip):
                continue
            c = pixels[x, y]
            if dist(c, PAGE) > 12 and dist(c, HIGHLIGHT) > 12:
                n += 1
        if n >= need:
            rows.append(y)
    return rows


def cmd_bands(path, probe_x):
    image = Image.open(path).convert("RGB")
    pixels = image.load()
    bands = find_bands(pixels, probe_x, image.size[1])
    if not bands:
        print(f"x={probe_x} 处找不到底纹带（该列可能被文字占了，换一列）")
        return 2
    for t, b in bands:
        print(f"行带 {t}..{b}  高 {b - t + 1}px = {(b - t + 1) / 1.5:.2f} 逻辑  中心 {(t + b) / 2:.1f}")
    return 0


def cmd_center(path, top, bottom, step):
    image = Image.open(path).convert("RGB")
    pixels = image.load()
    if top is None:
        bands = find_bands(pixels, PROBE_X, image.size[1])
        if not bands:
            print("找不到行带，请显式传 带顶 带底")
            return 2
        top, bottom = bands[0]
    caret, skip = caret_span(pixels, top, bottom, CODE[0], CODE[1], image.size[0])
    skip = [skip] if skip else []
    print(f"行带 {top}..{bottom} 高 {bottom - top + 1}px 中心 {(top + bottom) / 2:.1f}"
          + (f"   （已排除光标 x={caret}）" if caret else ""))
    worst = 0.0
    for index in range(6):
        t = int(top + index * step)
        b = int(t + step - 1)
        band_centre = (t + b) / 2
        for name, (x0, x1) in (("行号", GUTTER), ("代码", CODE)):
            rows = ink_rows(pixels, x0, x1, t, b, skip)
            if not rows:
                continue
            centre = (min(rows) + max(rows)) / 2
            off = centre - band_centre
            worst = max(worst, abs(off))
            flag = "居中 ✓" if abs(off) <= 1.5 else f"偏 {off:+.1f}px ✗"
            print(f"  +{index} {name}: 墨迹 {min(rows)}..{max(rows)}（高 {max(rows) - min(rows) + 1}px）"
                  f" 中心 {centre:.1f} vs 带中心 {band_centre:.1f} → {flag}")
    print(f"\n最大偏差 {worst:.1f} 物理px = {worst / 1.5:.2f} 逻辑px")
    return 0


def cmd_parts(path, top, bottom, split):
    image = Image.open(path).convert("RGB")
    pixels = image.load()
    caret, skip = caret_span(pixels, top, bottom, CODE[0], CODE[1], image.size[0])
    skip = [skip] if skip else []
    print(f"行带 {top}..{bottom}  中心 {(top + bottom) / 2:.1f}")
    print(f"{'y':>5} {'左段(拉丁)' :>10} {'右段(中文)':>12}   直方图")
    left, right = [], []
    for y in range(top - 2, bottom + 3):
        a = len(ink_rows_x(pixels, 566, split, y, skip))
        b = len(ink_rows_x(pixels, split, 1850, y, skip))
        if a:
            left.append(y)
        if b:
            right.append(y)
        band = "★" if dist(pixels[PROBE_X, y], HIGHLIGHT) <= 4 else " "
        print(f"{y:>5} {a:>10} {b:>12} {band}  {'#' * min(28, a)}{'|' * min(28, b)}")
    print()
    for name, rows in (("左段", left), ("右段", right)):
        if rows:
            print(f"{name}: {min(rows)}..{max(rows)}  中心 {(min(rows) + max(rows)) / 2:.1f}")
    if left and right:
        # 同一基线：两段的**下沿**应在同一条线上（拉丁底=基线，中文底=基线+CJK 降部）
        print(f"\n下沿差 {max(right) - max(left):+d}px（拉丁底=基线；中文底=基线+CJK 降部）")
    return 0


def ink_rows_x(pixels, x0, x1, y, skip):
    out = []
    for x in range(x0, x1):
        if any(lo <= x <= hi for lo, hi in skip):
            continue
        c = pixels[x, y]
        if dist(c, PAGE) > 12 and dist(c, HIGHLIGHT) > 12:
            out.append(x)
    return out


def cmd_scan(path):
    image = Image.open(path).convert("RGB")
    pixels = image.load()
    width, height = image.size
    print(f"图 {width}x{height}")
    print("\n左端着色变化（定位带边界；x=8）:")
    prev = None
    for y in range(height):
        c = pixels[8, y]
        if prev is None or dist(c, prev) > 6:
            print(f"  y={y:>4}  左={c}  中={pixels[width // 2, y]}")
            prev = c
    print("\n逐行墨迹带（非底色像素 > 2 的连续段）:")
    start = None
    for y in range(height):
        n = sum(1 for x in range(width) if dist(pixels[x, y], PAGE) > 30)
        if n > 2 and start is None:
            start = y
        elif n <= 2 and start is not None:
            if y - 1 - start >= 2:
                print(f"  y {start}..{y - 1}  高 {y - start}px  中心 {(start + y - 1) / 2:.1f}")
            start = None
    return 0


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    mode, path = sys.argv[1], sys.argv[2]
    rest = sys.argv[3:]
    if mode == "bands":
        return cmd_bands(path, int(rest[0]) if rest else PROBE_X)
    if mode == "center":
        top = int(rest[0]) if len(rest) > 0 else None
        bottom = int(rest[1]) if len(rest) > 1 else None
        step = float(rest[2]) if len(rest) > 2 else 1.5 * 20.79
        return cmd_center(path, top, bottom, step)
    if mode == "parts":
        return cmd_parts(path, int(rest[0]), int(rest[1]), int(rest[2]) if len(rest) > 2 else 640)
    if mode == "scan":
        return cmd_scan(path)
    print(f"未知子命令 {mode}")
    print(__doc__)
    return 1


if __name__ == "__main__":
    sys.exit(main())

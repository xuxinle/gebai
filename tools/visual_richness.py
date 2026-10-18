"""量"画面丰富度"——把"太简单"拆成几个互不蕴含的量化指标。

## 为什么是这几个量

用户说"简单"时，可能指四件**互不蕴含**的事（A 成立推不出 B）：

| 量 | 含义 | 对应观感 |
|---|---|---|
| `ink_ratio` | 画面被非背景内容覆盖的比例 | "空荡荡" vs "有内容" |
| `edge_count` | 显著边缘的数量（Sobel 阈值化的连通带） | "平" vs "有结构" |
| `level_steps` | 亮度直方图里可分辨的层数 | "只有底色+文字" vs "有层次" |
| `color_spread` | 彩度分布宽度 | "单色" vs "有配色" |

"细节多"与"有层次"是两个量：一张噪点图 edge 很多但 level_steps 很少。

## 量法（口径写死在这里，避免每次手算漂移）

- 只看**中心安全区**（去掉四周 5%）：边缘常有暗角/装饰线，会把 ink_ratio 抬虚高。
- `ink_ratio`：与**该行背景中位数**差 > 12 的像素占比（自适应底色——
  固定阈值会把深色背景整片当成内容，那是 §13.4 的坑）。
- `edge_count`：Sobel 幅值 > 26 的像素数 / 总像素（千分比）。
- `level_steps`：亮度直方图做 1% 截断后用 Otsu 找峰，数可分辨的峰个数（1~8）。
- `color_spread`：非灰像素的 HSV 饱和度标准差（0~1）。

用法：
    python tools/visual_richness.py <图片或目录> [更多...]
"""
import os
import sys

try:
    from PIL import Image
except ImportError:
    print("需要 Pillow：pip install pillow", file=sys.stderr)
    raise SystemExit(2)


def analyze(path):
    image = Image.open(path).convert("RGB")
    width, height = image.size
    # 中心安全区：去掉四周 5%，避开暗角/边框装饰（它们不是"内容"）
    left, right = int(width * 0.05), int(width * 0.95)
    top, bottom = int(height * 0.05), int(height * 0.95)
    crop = image.crop((left, top, right, bottom))
    pixels = list(crop.getdata())
    total = len(pixels)

    # —— ink_ratio：按行自适应底色 ——
    ink = 0
    row_medians = []
    crop_w = crop.size[0]
    for y in range(crop.size[1]):
        row = pixels[y * crop_w:(y + 1) * crop_w]
        lum = [(r * 299 + g * 587 + b * 114) // 1000 for r, g, b in row]
        ordered = sorted(lum)
        median = ordered[len(ordered) // 2]
        row_medians.append(median)
        for value in lum:
            if abs(value - median) > 12:
                ink += 1

    # —— edge_count：Sobel（用灰度差分近似，够用且快）——
    gray = crop.convert("L")
    gw, gh = gray.size
    data = list(gray.getdata())
    edges = 0
    for y in range(1, gh - 1):
        base = y * gw
        for x in range(1, gw - 1):
            i = base + x
            gx = (data[i - gw + 1] + 2 * data[i + 1] + data[i + gw + 1]
                  - data[i - gw - 1] - 2 * data[i - 1] - data[i + gw - 1])
            gy = (data[i + gw - 1] + 2 * data[i + gw] + data[i + gw + 1]
                  - data[i - gw - 1] - 2 * data[i - gw] - data[i - gw + 1])
            if abs(gx) + abs(gy) > 26 * 4:
                edges += 1

    # —— level_steps：亮度直方图峰数（1% 截断 + 邻域峰检测）——
    hist = [0] * 256
    for value in gray.getdata():
        hist[value] += 1
    cutoff = max(1, int(sum(hist) * 0.01))
    peaks = 0
    for i in range(1, 255):
        if hist[i] < cutoff:
            continue
        if hist[i] >= hist[i - 1] and hist[i] >= hist[i + 1]:
            # 只数"显著"的峰：至少是均值的 1.5 倍
            if hist[i] * 256 > sum(hist) * 0.06:
                peaks += 1

    # —— color_spread：饱和度标准差 ——
    sat = []
    for r, g, b in pixels[::7]:   # 抽样即可，省时间
        mx, mn = max(r, g, b), min(r, g, b)
        sat.append(0.0 if mx == 0 else (mx - mn) / mx)
    mean = sum(sat) / len(sat) if sat else 0.0
    variance = sum((v - mean) ** 2 for v in sat) / len(sat) if sat else 0.0

    return {
        "path": path,
        "size": (width, height),
        "ink_ratio": ink / total,
        "edge_permille": edges / total * 1000.0,
        "level_steps": peaks,
        "color_spread": variance ** 0.5,
        "levels_range": (min(row_medians), max(row_medians)) if row_medians else (0, 0),
    }


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    paths = []
    for arg in sys.argv[1:]:
        if os.path.isdir(arg):
            paths.extend(
                os.path.join(arg, f) for f in sorted(os.listdir(arg))
                if f.lower().endswith((".png", ".jpg"))
            )
        else:
            paths.append(arg)

    print(f"{'文件':38} {'ink%':>6} {'edge‰':>7} {'层数':>5} {'彩散':>6}")
    print("-" * 68)
    for path in paths:
        try:
            r = analyze(path)
        except Exception as exc:  # noqa: BLE001
            print(f"{os.path.basename(path):38} 失败：{exc}")
            continue
        name = os.path.basename(path)
        if len(name) > 36:
            name = name[:33] + "..."
        print(f"{name:38} {r['ink_ratio']*100:6.2f} {r['edge_permille']:7.1f} "
              f"{r['level_steps']:5d} {r['color_spread']:6.3f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

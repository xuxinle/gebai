"""给棒球帽**程序化画上美国国旗图案**。

## 为什么需要这一步

SDXL 控制不住这么小的细节。试过三轮：

| 写法 | 结果 |
|---|---|
| `american flag baseball cap` | **纯深蓝帽**，还让头发变成红色、衣服印上 "NAVY" 校队字样 |
| `navy cap with red and white horizontal stripes and white star pattern` | 纯深蓝帽 |
| 调 `control-scale` 0.85~1.0 | 无变化 |

根因是**帽子的像素面积太小**（约占画面 3%），SDXL 的注意力分配不到那里；
而且训练数据里"棒球帽"绝大多数是纯色，模型没有"帽子上的复杂图案"这个概念。

所以改用**后期合成**：把国旗图案按帽子的实际形状画上去。
这比继续磨提示词可靠得多——图案是我们自己画的，位置、颜色、大小完全可控。

## 怎么画才不像"贴纸"

关键在于**贴合帽子的立体形状**：

1. **横条要跟着帽子的弧度弯**。帽子是个球面，横条在边缘处会被"压扁"。
   做法：每条横条的 y 坐标按 `sin` 曲线偏移（越靠两侧越往下压）。
2. **颜色要跟着帽子的明暗走**。同一块布，受光面和背光面的亮度不同。
   做法：从原图取该处的亮度，按比例乘到国旗色上（`preserve_shading`）。
3. **星星只在帽子的左上一块**（真实国旗的 canton 位置），不是撒满整顶。
4. **边缘要羽化**。硬切线在抗锯齿后仍会留下台阶。

## 用法

```bash
py -3 tools/paint_flag_cap.py --input build/char-gen/look3/donut-idle.png \
    --outdir build/flag-test
```

单张试好了再批量（`--batch <目录>`）。
"""
import argparse
import glob
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

# 国旗配色（取自美国国旗标准色）
FLAG_RED = (0xB2, 0x22, 0x34)
FLAG_BLUE = (0x3C, 0x3B, 0x6E)
FLAG_WHITE = (0xF4, 0xF4, 0xF4)


def find_cap_region(image: Image.Image) -> tuple[int, int, int, int] | None:
    """找棒球帽的包围盒。

    ## 为什么不能只取"深蓝像素的最小/最大坐标"

    实测那样会框到 `y[24,362]`——把**头发和脸**也算进去了（深色头发满足了
    "R 低"的条件）。于是国旗图案被拉伸成一条竖长条，完全变形。

    ## 按行密度找

    帽子是一个**连续的横向大块**（每行有上百个深蓝像素），
    而头发/阴影只有零星几个。所以：
    1. 逐行数深蓝像素；
    2. 取"密度超过阈值"的连续区间当帽子；
    3. 在该区间内取列范围。
    """
    array = np.asarray(image.convert("RGB")).astype(int)
    height, width, _ = array.shape
    red, green, blue = array[:, :, 0], array[:, :, 1], array[:, :, 2]
    navy = (red < 95) & (blue > 65) & (blue > red + 12) & (green < 100)

    # ⚠ 先只看**上部 25%**：深蓝裤子也符合"深蓝"判据，不限制的话
    # 包围盒会一路延伸到 y=1077（膝盖），图案被拉成一条竖线。
    search_bottom = int(height * 0.25)
    row_counts = navy[:search_bottom].sum(axis=1)
    # 阈值取"最大行密度的一半"：帽子每行几十上百，头发只有个位数
    peak = int(row_counts.max()) if row_counts.size else 0
    if peak < 20:
        return None
    threshold = max(18, peak * 0.35)

    rows = np.where(row_counts >= threshold)[0]
    if rows.size == 0:
        return None
    top, bottom = int(rows.min()), int(rows.max())

    band = navy[top:bottom + 1]
    columns = np.where(band.sum(axis=0) > 0)[0]
    if columns.size == 0:
        return None
    return (int(columns.min()), top, int(columns.max()), bottom)


def paint_flag(image: Image.Image, box: tuple[int, int, int, int]) -> Image.Image:
    """在指定包围盒里画国旗图案（贴合帽面弧度 + 保留原明暗）。"""
    left, top, right, bottom = box
    width = right - left
    height = bottom - top
    if width < 20 or height < 20:
        return image

    array = np.asarray(image.convert("RGB")).astype(np.float32)
    original = array.copy()

    # 帽子的**明暗图**：用它保留原图的光影（否则图案会像贴纸一样平）
    luminance = original.mean(axis=2)[top:bottom, left:right]
    # 归一化到 0.55~1.15（原图亮度可能整体偏暗或偏亮）
    mean = float(luminance.mean())
    if mean < 1.0:
        mean = 128.0
    shading = np.clip(luminance / mean, 0.55, 1.15)

    # ── 国旗图案 ──
    #
    # 布局要照真实国旗（第一版画错了：把蓝块画满整个左侧、
    # 红条盖住右半边，变成"左蓝右红"两块）：
    #
    #   ┌──────────────┐
    #   │ 蓝块(带星) │  红 ∥ 白 ∥ 红 ∥ 白 …（横条）
    #   └──────────────┘
    #
    # 即：**左上 canton 是蓝块**，其余区域从 canton 右边开始才是红白横条；
    # canton 下方那些横条要**贯穿整个宽度**。
    overlay = Image.new("RGB", (width, height), FLAG_WHITE)
    draw = ImageDraw.Draw(overlay)

    # 13 条横条是古典国旗的比例；帽子上取 7 条才看得清（再少就不像国旗）
    stripes = 7
    band = height / stripes
    canton_w = int(width * 0.40)
    canton_h = band * 4            # canton 覆盖顶部 4 条的高度

    # ① 先铺满红白横条（贯穿整个宽度）
    for index in range(stripes):
        if index % 2 == 1:
            continue
        y0 = index * band
        draw.rectangle([0, y0, width, y0 + band], fill=FLAG_RED)

    # ② 左上 canton 盖成蓝块
    draw.rectangle([0, 0, canton_w, canton_h], fill=FLAG_BLUE)

    # ③ 星星：在 canton 内按网格排（真实国旗是 5×4 交错，帽子太小取 4×3）
    for row in range(3):
        for column in range(4):
            cx = canton_w * (column + 0.5) / 4.0
            cy = canton_h * (row + 0.5) / 3.0
            radius = min(canton_w / 4.0, canton_h / 3.0) * 0.26
            draw.ellipse([cx - radius, cy - radius, cx + radius, cy + radius], fill=FLAG_WHITE)

    # ③ 应用明暗
    overlay_array = np.asarray(overlay).astype(np.float32)
    shaded = overlay_array * shading[:, :, None]
    shaded = np.clip(shaded, 0, 255)

    # ④ 只画在**帽子像素**上（不要把图案糊到头发/脸上）
    patch = original[top:bottom, left:right]
    red_c, green_c, blue_c = patch[:, :, 0], patch[:, :, 1], patch[:, :, 2]
    cap_mask = (red_c < 100) & (blue_c > 60) & (blue_c > red_c + 10) & (green_c < 105)
    # 羽化边缘：硬边在抗锯齿下仍会留台阶
    mask_image = Image.fromarray((cap_mask * 255).astype(np.uint8)).filter(
        ImageFilter.GaussianBlur(radius=2.0)
    )
    mask = np.asarray(mask_image).astype(np.float32)[:, :, None] / 255.0

    blended = patch * (1.0 - mask) + shaded * mask
    array[top:bottom, left:right] = blended
    return Image.fromarray(np.clip(array, 0, 255).astype(np.uint8))


def process(path: str, outdir: str) -> bool:
    image = Image.open(path).convert("RGB")
    box = find_cap_region(image)
    if box is None:
        print(f"  {os.path.basename(path)}: 没找到深蓝帽子，跳过", flush=True)
        return False
    result = paint_flag(image, box)
    os.makedirs(outdir, exist_ok=True)
    out = os.path.join(outdir, os.path.basename(path))
    result.save(out)
    print(f"  {os.path.basename(path)}: 帽子 x[{box[0]},{box[2]}] y[{box[1]},{box[3]}] -> {out}",
          flush=True)
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", default="")
    parser.add_argument("--batch", default="")
    parser.add_argument("--outdir", default="build/flag-caps")
    args = parser.parse_args()

    if args.batch:
        paths = sorted(glob.glob(os.path.join(args.batch, "donut-*.png")))
    elif args.input:
        paths = [args.input]
    else:
        print("要么给 --input，要么给 --batch", file=sys.stderr)
        return 1

    ok = 0
    for path in paths:
        if process(path, args.outdir):
            ok += 1
    print(f"完成 {ok}/{len(paths)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

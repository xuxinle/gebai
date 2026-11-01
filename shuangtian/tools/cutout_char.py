"""把白底角色立绘抠成透明 PNG（泛洪法）。

## 为什么不是"按亮度判透明"

第一版按亮度阈值判：亮度越高越透明。结果**皮肤和白鞋全被抠掉了**
（皮肤是浅色、鞋是白色，都被当成背景）——脸上出现黑洞、鞋消失。

问题在于"白"与"浅色"在数值上分不开。正确做法是利用**连通性**：
背景是**与图像边框连通**的那片白色；角色内部的白色（眼白、白鞋、白条纹）
被深色描边包围，与该片区域不连通，因此应当保留。

## 算法

1. 从四条边出发做 BFS，只走"接近白"的像素 → 得到背景掩码；
2. 背景掩码内 alpha = 0；
3. 掩码**边界**（与角色相邻的那一圈）做反混合去白边：
   `α = 1 - min(R,G,B)/255`，`F = (O - (1-α)·255) / α`。
   只在那一圈上做，避免像第一版那样把整件浅色衣服变透明。
"""
import argparse
import os
import sys
from collections import deque

from PIL import Image


def matte_flood(path: str, out: str, threshold: float = 0.93) -> None:
    image = Image.open(path).convert("RGBA")
    width, height = image.size
    pixels = image.load()

    def is_background(x: int, y: int) -> bool:
        r, g, b, _ = pixels[x, y]
        # "接近白"：三通道都很高，且彼此接近（排除浅粉/浅蓝这类有色浅色）。
        minimum = min(r, g, b) / 255.0
        maximum = max(r, g, b) / 255.0
        return minimum >= threshold and (maximum - minimum) <= 0.055

    # ① 从四边泛洪，得到"与边界连通的背景"
    background = bytearray(width * height)
    queue = deque()
    for x in range(width):
        for y in (0, height - 1):
            if is_background(x, y) and not background[y * width + x]:
                background[y * width + x] = 1
                queue.append((x, y))
    for y in range(height):
        for x in (0, width - 1):
            if is_background(x, y) and not background[y * width + x]:
                background[y * width + x] = 1
                queue.append((x, y))

    while queue:
        x, y = queue.popleft()
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nx, ny = x + dx, y + dy
            if 0 <= nx < width and 0 <= ny < height and not background[ny * width + nx]:
                if is_background(nx, ny):
                    background[ny * width + nx] = 1
                    queue.append((nx, ny))

    background_count = sum(background)

    # ② 涂透明 + 在边界环上去白边
    for y in range(height):
        for x in range(width):
            index = y * width + x
            r, g, b, _ = pixels[x, y]
            if background[index]:
                pixels[x, y] = (r, g, b, 0)
                continue
            # 判断是否是"边界环"：四邻里存在背景像素
            on_edge = False
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nx, ny = x + dx, y + dy
                if 0 <= nx < width and 0 <= ny < height and background[ny * width + nx]:
                    on_edge = True
                    break
            if not on_edge:
                pixels[x, y] = (r, g, b, 255)
                continue
            # 反混合：这个像素是"角色色 + 白背景"的混合
            minimum = min(r, g, b) / 255.0
            alpha = max(0.0, min(1.0, 1.0 - minimum))
            if alpha <= 0.02:
                pixels[x, y] = (r, g, b, 0)
                continue
            inv = (1.0 - alpha) * 255.0
            fr = max(0.0, min(255.0, (r - inv) / alpha))
            fg = max(0.0, min(255.0, (g - inv) / alpha))
            fb = max(0.0, min(255.0, (b - inv) / alpha))
            pixels[x, y] = (int(fr), int(fg), int(fb), int(alpha * 255))

    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    image.save(out)
    ratio = background_count / (width * height) * 100.0
    print(f"  {os.path.basename(path)} → {out}  (背景占 {ratio:.1f}%)", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("inputs", nargs="+")
    parser.add_argument("--outdir", default="build/char-gen/cutout")
    parser.add_argument("--threshold", type=float, default=0.93)
    args = parser.parse_args()

    for path in args.inputs:
        if not os.path.exists(path):
            print(f"不存在：{path}", file=sys.stderr)
            continue
        name = os.path.splitext(os.path.basename(path))[0]
        matte_flood(path, os.path.join(args.outdir, f"{name}.png"), args.threshold)
    return 0


if __name__ == "__main__":
    sys.exit(main())

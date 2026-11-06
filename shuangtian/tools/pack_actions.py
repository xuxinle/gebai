"""把抠好的动作立绘统一缩放并放进 assets。

游戏里角色高约 260 世界像素；2x 供高 DPI = 520。取 640 留余量
（不同动作的构图高矮不一，统一缩到 640 高再由运行时按骨架缩放，
比"逐张算目标尺寸"简单且不会出现某张特别小）。
"""
import glob
import os
import sys

from PIL import Image

OUTDIR = "apps/gedou/assets/actions"
TARGET_HEIGHT = 640


def main() -> int:
    os.makedirs(OUTDIR, exist_ok=True)
    paths = sorted(glob.glob("build/char-gen/actions-cut/*.png"))
    if not paths:
        print("没有找到抠好的图", file=sys.stderr)
        return 1
    total = 0
    for path in paths:
        image = Image.open(path).convert("RGBA")
        width = int(image.width * TARGET_HEIGHT / image.height)
        destination = os.path.join(OUTDIR, os.path.basename(path))
        image.resize((width, TARGET_HEIGHT), Image.LANCZOS).save(destination, optimize=True)
        total += os.path.getsize(destination)
    count = len(glob.glob(os.path.join(OUTDIR, "*.png")))
    print(f"{count} 张 -> {OUTDIR}，共 {total / 1024 / 1024:.2f} MB")
    return 0


if __name__ == "__main__":
    sys.exit(main())

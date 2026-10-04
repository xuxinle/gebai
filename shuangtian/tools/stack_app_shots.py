"""把同一应用、不同 gamma 的截图拼成一张对照图（每张取若干区域，纵向堆叠）。

用法：python tools/stack_app_shots.py <输出png> <区域1> <区域2> ... -- <图1> <图2> ...
区域形如 x0,y0,x1,y1；各图按同样区域裁剪后依次堆叠，图间画分隔线。
"""

import sys

from PIL import Image, ImageDraw

SEP = (255, 120, 120)


def parse_rect(s):
    return tuple(int(v) for v in s.split(","))


def main() -> int:
    out_path = sys.argv[1]
    rest = sys.argv[2:]
    cut = rest.index("--")
    regions = [parse_rect(s) for s in rest[:cut]]
    images = rest[cut + 1:]

    tiles = []
    for path in images:
        im = Image.open(path).convert("RGB")
        for r in regions:
            tiles.append(im.crop(r))

    width = max(t.width for t in tiles)
    height = sum(t.height for t in tiles) + 6 * len(tiles)
    out = Image.new("RGB", (width, height), (255, 255, 255))
    draw = ImageDraw.Draw(out)
    y = 0
    for t in tiles:
        out.paste(t, (0, y))
        y += t.height
        draw.rectangle([0, y, width, y + 5], fill=SEP)
        y += 6
    out.save(out_path)
    print(f"{out_path}  {out.size}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

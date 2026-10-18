"""把素材目录拼成一张缩略图总览（一次看完，不必逐张读）。"""
import os
import sys

from PIL import Image, ImageDraw

def main():
    src = sys.argv[1]
    out = sys.argv[2]
    names = sorted(
        f[:-4] for f in os.listdir(src) if f.lower().endswith(".png")
    )
    if not names:
        print(f"目录里没有 PNG：{src}", file=sys.stderr)
        return 2
    cols = 4
    cw, ch, caption = 400, 225, 22
    rows = (len(names) + cols - 1) // cols
    sheet = Image.new("RGB", (cw * cols, (ch + caption) * rows), (10, 12, 20))
    draw = ImageDraw.Draw(sheet)
    for index, name in enumerate(names):
        path = os.path.join(src, name + ".png")
        try:
            image = Image.open(path).convert("RGB").resize((cw, ch), Image.LANCZOS)
        except Exception as exc:  # noqa: BLE001
            print(f"跳过 {name}：{exc}", file=sys.stderr)
            continue
        x = (index % cols) * cw
        y = (index // cols) * (ch + caption)
        sheet.paste(image, (x, y))
        draw.text((x + 8, y + ch + 5), name, fill=(200, 210, 230))
    sheet.save(out)
    print(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())

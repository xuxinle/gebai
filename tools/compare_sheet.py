"""把多张图并排拼成对照表（A/B 对比用）。

用法：
    python tools/compare_sheet.py <输出.png> <左图> <右图> [更多...] [--labels A,B]

为什么要它：观感类改动的验收在用户身上，最有效的交付方式是**并排对照**——
让用户直接看差异，而不是读一堆数字（数字可能指向错误方向，
本仓库为这个偏差付过四次返工）。
"""
import os
import sys

try:
    from PIL import Image, ImageDraw
except ImportError:
    print("需要 Pillow", file=sys.stderr)
    raise SystemExit(2)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    labels = None
    for arg in sys.argv[1:]:
        if arg.startswith("--labels="):
            labels = arg.split("=", 1)[1].split(",")
    if len(args) < 3:
        print(__doc__)
        return 2
    out, inputs = args[0], args[1:]

    images = []
    for path in inputs:
        images.append(Image.open(path).convert("RGB"))
    # 统一到同一高度（保持各自长宽比），便于并排看不出"被拉伸"
    target_h = max(img.size[1] for img in images)
    scaled = []
    for img in images:
        ratio = target_h / img.size[1]
        scaled.append(img.resize((int(img.size[0] * ratio), target_h), Image.LANCZOS))

    gap, caption = 24, 44
    total_w = sum(img.size[0] for img in scaled) + gap * (len(scaled) - 1)
    sheet = Image.new("RGB", (total_w, target_h + caption), (16, 18, 26))
    draw = ImageDraw.Draw(sheet)
    x = 0
    for index, img in enumerate(scaled):
        sheet.paste(img, (x, caption))
        label = labels[index] if labels and index < len(labels) else os.path.basename(inputs[index])
        draw.text((x + 10, 14), label, fill=(190, 205, 230))
        x += img.size[0] + gap
    sheet.save(out)
    print(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())

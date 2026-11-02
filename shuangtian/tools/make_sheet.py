"""把多候选动作图拼成**联络表**，方便人工挑选。

## 为什么按动作分组而不是按角色

挑选时的任务是"这个动作哪张最像"——同一动作的 3 张放一起，
差异一眼可见。按角色拼会把 17 个动作混在 51 格里，没法比。

## 编号规则

每张图左上角标 `动作名 + 候选号`，选中后直接说"light 的 c1"就能定位到文件。
"""
import argparse
import json
import os
import sys

from PIL import Image, ImageDraw


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--indir", default="build/char-gen/multi")
    parser.add_argument("--outdir", default="build/sheets")
    parser.add_argument("--per-row", type=int, default=3)
    parser.add_argument("--cell", type=int, default=240)
    args = parser.parse_args()

    manifest_path = os.path.join(args.indir, "manifest.json")
    with open(manifest_path, encoding="utf-8") as handle:
        manifest = json.load(handle)

    # 按动作分组（保持 manifest 里的出现顺序）
    groups: dict[str, list[dict]] = {}
    for entry in manifest:
        groups.setdefault(entry["action"], []).append(entry)

    os.makedirs(args.outdir, exist_ok=True)
    cell = args.cell
    label_h = 22

    for action, entries in groups.items():
        columns = len(entries)
        width = cell * columns
        height = int(cell * 1.46) + label_h
        sheet = Image.new("RGB", (width, height), (245, 245, 245))
        draw = ImageDraw.Draw(sheet)

        for index, entry in enumerate(entries):
            path = entry["path"]
            if not os.path.exists(path):
                continue
            image = Image.open(path).convert("RGB")
            image.thumbnail((cell - 8, height - label_h - 8), Image.LANCZOS)
            x = index * cell + (cell - image.width) // 2
            sheet.paste(image, (x, label_h + 2))
            label = f"{action}  c{entry['candidate']}  seed={entry['seed']}"
            draw.text((index * cell + 6, 5), label, fill=(20, 20, 20))
            draw.line([(index * cell, 0), (index * cell, height)], fill=(200, 200, 200))

        out = os.path.join(args.outdir, f"{action}.png")
        sheet.save(out)
        print(f"  {out}  ({columns} 个候选)", flush=True)

    print(f"\n{len(groups)} 张联络表 -> {args.outdir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

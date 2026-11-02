"""核对"立绘里人物的实际脚底位置"与"骨架里的脚点"是否一致。

## 为什么要查这个

`game_view.cpp` 里立绘的缩放是：

```cpp
const float top    = pose.head.y - pose.head_radius;   // 骨架头顶
const float height = std::max(40.0f, lowest - top);    // lowest = 骨架最低点
sprite.draw(canvas, fighter.x, lowest, height, ...);   // 贴到 lowest 处
```

也就是说：**假设"立绘图片的最下沿就是骨架最低点"**。

但这个假设在素材层面不一定成立：
- 生成时人物没有踩到画布底部（画布下方留了白）⇒ 图片下沿比脚低
- 或者脚尖伸出画布被裁掉 ⇒ 图片下沿比脚高

任一种都会让**缩放比例失真**：图片留白多 ⇒ 同样的 `height` 装下更多人 ⇒ 人物显得小。

## 判据

对每张抠好的立绘，量"不透明像素的最低点距图片底边还有多少像素"。
如果这个留白在各张之间差异大，就是缩放不一致的来源。

修复思路：**先按内容裁掉多余留白**（`pack_actions.py` 里做），
再用骨架跨度缩放，两边就对上了。
"""
import glob
import os
import sys

import numpy as np
from PIL import Image


def main() -> int:
    parser = __import__("argparse").ArgumentParser()
    parser.add_argument("--dir", default="build/char-gen/final2-cut")
    parser.add_argument("--pattern", default="donut-*.png")
    args = parser.parse_args()

    paths = sorted(glob.glob(os.path.join(args.dir, args.pattern)))
    print(f"{'动作':14s} {'画布高':>7s} {'内容底':>7s} {'底部留白':>9s} {'顶部留白':>9s} {'内容高':>7s}")
    blanks = []
    for path in paths:
        image = Image.open(path).convert("RGBA")
        array = np.array(image)
        height = array.shape[0]
        mask = array[:, :, 3] > 40
        rows = np.where(mask.any(axis=1))[0]
        if rows.size == 0:
            continue
        top, bottom = int(rows.min()), int(rows.max())
        bottom_blank = height - 1 - bottom
        name = os.path.basename(path).split("-", 1)[1][:-4]
        content = bottom - top
        print(f"{name:14s} {height:7d} {bottom:7d} {bottom_blank:9d} {top:9d} {content:7d}")
        blanks.append((name, bottom_blank, top, content))

    if not blanks:
        print("没有可用文件", file=sys.stderr)
        return 1

    bottom_blanks = [b for _, b, _, _ in blanks]
    tops = [t for _, _, t, _ in blanks]
    contents = [c for _, _, _, c in blanks]
    print(f"\n底部留白：{min(bottom_blanks)}~{max(bottom_blanks)}"
          f"（差 {max(bottom_blanks) - min(bottom_blanks)}px）")
    print(f"顶部留白：{min(tops)}~{max(tops)}（差 {max(tops) - min(tops)}px）")
    print(f"内容高：  {min(contents)}~{max(contents)}"
          f"（比值 {max(contents) / max(1, min(contents)):.3f}）")

    span = max(contents) - min(contents)
    if span > 20:
        print(f"\n⚠ 内容高相差 {span}px —— 内容高不一致会让立绘缩放后大小不同。")
    else:
        print("\n内容高一致（差 <20px）")
    return 0


if __name__ == "__main__":
    sys.exit(main())

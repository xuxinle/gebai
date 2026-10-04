#!/usr/bin/env python3
"""「字号 → gamma」测量的**干净量尺**：每页只放一行字，比整幅总墨量。

## 为什么必须这样

前两版量尺都不可靠（都实测踩过）：
  · 按**逻辑 top** 切行带——两侧基线算法不同，带子会偏到相邻行
  · 按**墨迹包围盒**裁剪——两侧盒高不同（霜天 82% 于浏览器），各自裁剪会截掉较高那侧的墨
  · 按**某一侧的元素框**切带——两侧文字位置不同，另一侧的墨有 14% 落在带外

**整幅总墨量**是唯一不依赖裁剪与对齐的量（墨迹落在画布哪里都不影响积分）。
但它要求每幅画布**只有一行字**——否则不同字号混在一页，无法归因。

所以：一页一行，两侧同尺寸画布，比整幅总墨量。

用法：python tools/gen_size_pages.py     输出 build/probe/sizes1/<size>.html 与 .json
"""

import json
import pathlib

SIZES = [10, 11, 12, 13, 14, 15, 16, 18, 20, 24, 28, 32]
SAMPLE = "组件画廊 Overview 24"
COLOR = "#0F172A"
FAMILY = "'Segoe UI','Microsoft YaHei'"
WIDTH = 640
HEIGHT = 96          # 单行：够高即可，两侧墨迹都在画布内


def main() -> int:
    root = pathlib.Path(__file__).resolve().parent.parent
    out = root / "build" / "probe" / "sizes1"
    out.mkdir(parents=True, exist_ok=True)
    meta = []
    for size in SIZES:
        top = 8
        (out / f"{size}.html").write_text(
            "<!doctype html>\n"
            "<!-- 由 tools/gen_size_pages.py 生成：一页一行，测整幅总墨量。 -->\n"
            '<meta charset="utf-8">\n'
            "<style>\n"
            f"  html,body{{margin:0;padding:0;background:#FFFFFF;width:{WIDTH}px;height:{HEIGHT}px;overflow:hidden}}\n"
            f"  .t{{position:absolute;left:4px;top:{top}px;color:{COLOR};white-space:nowrap;"
            f"line-height:1;font-size:{size}px;font-family:{FAMILY}}}\n"
            "</style>\n"
            f'<div class="t" id="line">{SAMPLE}</div>\n',
            encoding="utf-8", newline="\n")
        meta.append({"size": size, "width": WIDTH, "height": HEIGHT, "top": top})
    (out / "meta.json").write_text(json.dumps(meta, indent=1), encoding="utf-8")
    print(f"已生成 {len(SIZES)} 个单行页 → {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

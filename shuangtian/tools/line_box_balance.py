"""从 line_box_probe 的输出里算第二个正交量：墨迹中心相对行盒中心的偏差。

为什么需要它：修复前/后的**单一量**（上留白）本身不能宣布"更好"（手册 §2）——
必须有两个正交量同向。这里：

  正交量 ①「下−上」= 行盒内上下留白之差      → 应**恒定**（行距不该改变字的位置）
  正交量 ②「墨迹中心 − 行盒中心」= 是否居中   → 应**不随行距漂移**

修复前的典型读数（实测）：① 从 −1.33 一路涨到 +8.57（间距全堆下方）；
② 墨迹中心随行距持续下沉。修复后 ① 恒 −1.33、② 恒定。

用法：build/dev/bin/line_box_probe > out.txt && python tools/line_box_balance.py out.txt
"""

import re
import sys


PATTERN = re.compile(
    r"spacing ([\d.]+) (\S+)\s+行高 ([\d.]+) \| "
    r"上留白\s+([-\d.]+)\s+下留白\s+([-\d.]+)"
)


def main():
    text = open(sys.argv[1], encoding="utf-8", errors="replace").read() if len(sys.argv) > 1 else sys.stdin.read()
    rows = []
    for line in text.splitlines():
        m = PATTERN.search(line)
        if m:
            rows.append((float(m.group(1)), m.group(2), float(m.group(3)),
                         float(m.group(4)), float(m.group(5))))
    if not rows:
        print("没解析到数据——确认输入是 line_box_probe 的 stdout")
        return 2

    print(f"{'spacing':>8} {'字':>4} {'行高':>6} {'上留白':>7} {'下留白':>7} "
          f"{'下-上':>7} {'墨迹中心-行盒中心':>17}")
    by_kind = {}
    for spacing, kind, height, up, down in rows:
        ink_height = height - up - down
        centre_delta = (up + ink_height / 2.0) - height / 2.0
        by_kind.setdefault(kind, []).append((spacing, down - up, centre_delta))
        print(f"{spacing:>8.2f} {kind:>4} {height:>6.2f} {up:>7.2f} {down:>7.2f} "
              f"{down - up:>+7.2f} {centre_delta:>+17.3f}")

    print("\n按字体汇总（判据见脚本头）:")
    for kind, values in by_kind.items():
        spreads = [v[1] for v in values]
        centres = [v[2] for v in values]
        print(f"  {kind}: ① 下-上 极差 {max(spreads) - min(spreads):.3f}px   "
              f"② 墨迹中心 极差 {max(centres) - min(centres):.3f}px")
    print("\n判据：两个极差都接近 0 => 行距只加在**行间**，字形在自己的行盒里没挪位。")
    return 0


if __name__ == "__main__":
    sys.exit(main())

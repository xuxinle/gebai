"""把 line_box_probe 的修前/修后输出并排成对照表，供用户验收观感改动。

用法：python tools/line_box_compare.py tmp_before.txt tmp_after.txt
"""

import re
import sys

PATTERN = re.compile(
    r"spacing ([\d.]+) (\S+)\s+行高 ([\d.]+) \| "
    r"上留白\s+([-\d.]+)\s+下留白\s+([-\d.]+)"
)


def parse(path):
    rows = {}
    for line in open(path, encoding="utf-8", errors="replace").read().splitlines():
        m = PATTERN.search(line)
        if m:
            rows[(float(m.group(1)), m.group(2))] = (
                float(m.group(3)), float(m.group(4)), float(m.group(5)))
    return rows


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    before, after = parse(sys.argv[1]), parse(sys.argv[2])
    keys = sorted(set(before) & set(after))
    print("行盒内上下留白（单位 px，字号 15 / Monospace / 浅色主题）")
    print()
    print(f"{'spacing':>8} {'字':>4} | {'修前 上':>8} {'修前 下':>8} {'修前 下-上':>10} "
          f"| {'修后 上':>8} {'修后 下':>8} {'修后 下-上':>10}")
    print("-" * 78)
    for key in keys:
        _, up_b, down_b = before[key]
        _, up_a, down_a = after[key]
        print(f"{key[0]:>8.2f} {key[1]:>4} | {up_b:>8.2f} {down_b:>8.2f} {down_b - up_b:>+10.2f} "
              f"| {up_a:>8.2f} {down_a:>8.2f} {down_a - up_a:>+10.2f}")
    print()
    print("判据：『下-上』这一列——修前随行距**单调增大**（间距全堆下方），")
    print("      修后**恒定**（行距只改变行间空隙，字形在自己行盒里没挪位）。")

    def spread(rows_, kind):
        values = [d - u for (sp, k), (h, u, d) in rows_.items() if k == kind]
        return max(values) - min(values) if values else 0.0

    for kind in {k[1] for k in keys}:
        print(f"  {kind}: 修前『下-上』极差 {spread(before, kind):.2f}px  →  "
              f"修后 {spread(after, kind):.2f}px")
    return 0


if __name__ == "__main__":
    sys.exit(main())

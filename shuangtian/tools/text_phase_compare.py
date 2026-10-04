"""对比两次相位扫描输出（仅验证用）：按 device_scale 段汇总「相位抖动」。"""
import re
import sys

def parse(path):
    sections = {}
    scale = None
    for line in open(path, encoding="utf-8"):
        m = re.match(r"=== device_scale=([\d.]+)", line)
        if m:
            scale = m.group(1)
            sections[scale] = {}
            continue
        m = re.match(r"\s+U\+([0-9A-F]+)\s+相位抖动=([\d.]+)", line)
        if m and scale:
            sections[scale][m.group(1)] = float(m.group(2))
    return sections

old, new = parse(sys.argv[1]), parse(sys.argv[2])
print(f"  {'scale':6s} {'glyph':6s} {'before':>9s} {'after':>9s}  {'变化':>8s}")
for scale in sorted(set(old) | set(new)):
    for glyph in sorted(set(old.get(scale, {})) | set(new.get(scale, {}))):
        a = old.get(scale, {}).get(glyph)
        b = new.get(scale, {}).get(glyph)
        if a is None or b is None:
            continue
        mark = "" if abs(b - a) < 1e-9 else ("改善" if b < a else "变差")
        print(f"  {scale:6s} {glyph:6s} {a:9.4f} {b:9.4f}  {b - a:+8.4f} {mark}")

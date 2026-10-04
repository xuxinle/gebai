"""检查系统字体是否覆盖指定码点（只看 cmap 的 format 4 / 12）。

用途：定位「霜天缺字」的修复来源——缺哪个字符、哪个回退字体有它。
不改字体栈，只做实测取证。
"""

import os
import struct
import sys

FONTS_DIR = "C:/Windows/Fonts"


def cmap_offset(d):
    num_tables = struct.unpack(">H", d[4:6])[0]
    for i in range(num_tables):
        o = 12 + i * 16
        if d[o:o + 4] == b"cmap":
            return struct.unpack(">I", d[o + 8:o + 12])[0]
    return None


def covers(path, cp):
    try:
        d = open(path, "rb").read()
    except OSError:
        return None
    base = cmap_offset(d)
    if base is None:
        return None
    n = struct.unpack(">H", d[base + 2:base + 4])[0]
    for i in range(n):
        o = base + 4 + i * 8
        sub = base + struct.unpack(">I", d[o + 4:o + 8])[0]
        fmt = struct.unpack(">H", d[sub:sub + 2])[0]
        if fmt == 4:
            seg_x2 = struct.unpack(">H", d[sub + 6:sub + 8])[0]
            seg = seg_x2 // 2
            endo = sub + 14
            starto = endo + seg_x2 + 2
            for j in range(seg):
                e = struct.unpack(">H", d[endo + j * 2:endo + j * 2 + 2])[0]
                s = struct.unpack(">H", d[starto + j * 2:starto + j * 2 + 2])[0]
                if s <= cp <= e:
                    return True
        elif fmt == 12:
            ng = struct.unpack(">I", d[sub + 12:sub + 16])[0]
            for j in range(min(ng, 20000)):
                o2 = sub + 16 + j * 12
                s, e, _g = struct.unpack(">III", d[o2:o2 + 12])
                if s <= cp <= e:
                    return True
            # 有 format 12 但没命中，也不能就此否掉其它子表
    return False


def main() -> int:
    cps = [int(a, 16) for a in sys.argv[1:]] or [0x2713, 0x2717]
    names = ["segoeui.ttf", "segoeuib.ttf", "seguisym.ttf", "arial.ttf", "arialbd.ttf",
             "msyh.ttc", "msyhbd.ttc", "simhei.ttf", "simsun.ttc", "consola.ttf",
             "CascadiaMono.ttf", "seguiemj.ttf"]
    header = "字体".ljust(18) + "".join(f"U+{c:04X}".rjust(10) for c in cps)
    print(header)
    print("-" * len(header))
    for name in names:
        path = os.path.join(FONTS_DIR, name)
        if not os.path.exists(path):
            continue
        row = name.ljust(18)
        for cp in cps:
            got = covers(path, cp)
            row += ("有" if got else "—").rjust(8) + "  "
        print(row)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

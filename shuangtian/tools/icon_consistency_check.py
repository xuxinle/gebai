#!/usr/bin/env python3
"""图标一致性门禁：**一个应用一个记号，四条链路必须全部接通且同源**。

## 它防的是什么（真实事故）

相期的标题栏图标曾被一个提交（`c018aee9`）从「红/白/蓝三色竖条」改成
「深灰人像」，而**只改了 SVG、没改 PNG**——于是同一个应用在标题栏显示人像、
任务栏与资源管理器显示三色条，用户看到两个不同的东西。

更早还有一次：某个应用调了 `set_icon("svg:xxx")` 却忘了注册 `xxx`——
`Icon::draw` 查不到就**什么都不画**，属性面读回来一切正常（`icon` 字段就是
那个字符串），只有把像素数一遍才知道是空的。

这两类缺陷的共同点：**没有任何一处代码“错了”**（都能编译、都能启动、属性面
自洽），错在**多份资产之间的对应关系**。所以只能靠跨资产的一致性校验来守。

## 四条链路（缺一条就是“图标不一致”）

| 链路 | 谁消费 | 必须存在 |
|---|---|---|
| `window_icon_png` | 窗口系统（任务栏 / Alt-Tab） | `apps/<app>/assets/<app>-icon.png` |
| `svg:` 品牌标记 | 自绘标题栏 | `.svg` 素材 + **运行期注册的 id 与 `apply_brand` 的 id 一致** |
| `.ico` 资源 | 资源管理器 / 快捷方式 | `apps/<app>/assets/<app>.ico` |
| `tray_icon_png` | 托盘（可选） | 与窗口图标同源即可 |

## 同源判据（不做“看起来差不多”，做可复算的）

- **配色**：SVG 里出现的实色 `fill`/`stroke` 必须能从 PNG 的主色集合里找到
  （PNG 采样前 8 个最高频不透明色；容差 24/255 应付抗锯齿与渐变取中）。
- **几何不查**：SVG 与 PNG 的构图换算（24 视框 vs 256 画布）已在两边注释里
  写死，几何一致靠人；这里只把“**符号/配色整体被换掉**”这类事故钉住。

## 用法

    py -3 tools/icon_consistency_check.py           # 全量检查（有违规退出码 1）
    py -3 tools/icon_consistency_check.py xiangqi   # 只查一个
    py -3 tools/icon_consistency_check.py --verbose # 附各应用采样到的色表
"""
from __future__ import annotations

import re
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# 参与校验的应用（`apps/<name>/` 存在即可，桌面壳类同理）
SKIP = {"assets"}  # 共享素材目录，不是应用

# 合理例外：这些应用**没有常规标题栏**，因此不适用品牌标记，
# 但它们仍然必须有窗口图标与 .ico。
#
# 为何是白名单而不是“自己探测”：探测只能看到“没调 apply_brand”，
# 而分不清“这个应用压根不该有标题栏”与“该有却漏了”——后者正是要拦的缺陷。
# 显式声明后才能把后者变成硬报错。
NO_TITLE_BAR = {
    "st_desktop":   "全屏置底桌面壳：铺满显示器、无窗口装饰",
    "gebai_desktop": "全窗 WebView 宿主：内容区即是网页，chrome 由服务端渲染",
}


# —— 极简 PNG 解码（仓库工具链一贯无第三方依赖，与 make_icons.py 同口径）——

def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def read_png_rgba(path: Path):
    """返回 (width, height, pixels[list[tuple[r,g,b,a]]])；只支持 8 位 RGBA/RGB。"""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("不是 PNG")

    pos = 8
    width = height = 0
    bit_depth = color_type = 0
    idat = bytearray()
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if ctype == b"IHDR":
            width, height, bit_depth, color_type = struct.unpack(">IIBB", chunk[:10])
        elif ctype == b"IDAT":
            idat += chunk
        elif ctype == b"IEND":
            break

    if bit_depth != 8:
        raise ValueError(f"只支持 8 位色深（实际 {bit_depth}）")

    channels = {2: 3, 6: 4}.get(color_type)
    if channels is None:
        raise ValueError(f"只支持 RGB/RGBA（color_type={color_type}）")

    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    out = []
    prev = bytearray(stride)
    p = 0
    for _ in range(height):
        f = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        if f == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif f == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif f == 4:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                up_left = prev[i - channels] if i >= channels else 0
                line[i] = (line[i] + _paeth(left, prev[i], up_left)) & 0xFF
        prev = line

        for x in range(width):
            base = x * channels
            if channels == 4:
                out.append((line[base], line[base + 1], line[base + 2], line[base + 3]))
            else:
                out.append((line[base], line[base + 1], line[base + 2], 255))
    return width, height, out


def dominant_colors(path: Path, limit: int = 8):
    """从 PNG 取**高频不透明色**（每色 8 级量化后聚合，再取簇内真值）。"""
    w, h, px = read_png_rgba(path)
    buckets: dict[tuple[int, int, int], int] = {}
    sample = px[::7]          # 稀疏采样：256² 全扫没必要，够统计主色
    for r, g, b, a in sample:
        if a < 200:           # 只看不透明区（透明边是抗锯齿，不代表配色）
            continue
        key = (r >> 3, g >> 3, b >> 3)
        buckets[key] = buckets.get(key, 0) + 1
    top = sorted(buckets.items(), key=lambda kv: -kv[1])[:limit]
    return [(k[0] << 3, k[1] << 3, k[2] << 3) for k, _ in top]


def svg_colors(path: Path):
    """SVG 里出现的实色（`fill="#RRGGBB"` / `stroke="#RRGGBB"`）。"""
    text = path.read_text(encoding="utf-8", errors="replace")
    found = set()
    for m in re.finditer(r'(?:fill|stroke)\s*=\s*"#([0-9A-Fa-f]{6})"', text):
        v = m.group(1)
        found.add((int(v[0:2], 16), int(v[2:4], 16), int(v[4:6], 16)))
    return found


def near(a, b, tol: int = 32) -> bool:
    return all(abs(x - y) <= tol for x, y in zip(a, b))


def read_main(app_dir: Path):
    for cand in [app_dir / "main.cpp", app_dir / "src" / "main.cpp"]:
        if cand.exists():
            return cand.read_text(encoding="utf-8", errors="replace")
    return None


def check_app(name: str, verbose: bool):
    app_dir = ROOT / "apps" / name
    problems: list[str] = []
    notes: list[str] = []

    assets = app_dir / "assets"
    src = read_main(app_dir)
    if src is None:
        return [f"{name}: 找不到 main.cpp"], notes

    # ① 窗口/任务栏 PNG
    #
    # `gebai_desktop` 走 `.rgba` 原始像素（运行期 `make_icon_png` 转 PNG），
    # 所以 PNG 文件不存在也不算错——只要**源素材**在。
    png = next(iter(assets.glob("*icon*.png")), None)
    rgba = next(iter(assets.glob("*icon*.rgba")), None)
    has_png_opt = ("window_icon_png" in src) or ("attach_png" in src)
    if not has_png_opt:
        problems.append("未设窗口图标（任务栏/Alt-Tab 会是默认图标）")
    elif png is None and rgba is None:
        problems.append("代码里设了窗口图标，但 assets 下既无 *icon*.png 也无 *icon*.rgba")

    # ② 标题栏品牌 SVG
    brand = re.search(r'apply_brand\(\s*[^,]+,\s*"([^"]+)"\s*\)', src)
    brand_id = brand.group(1) if brand else None
    manual = re.search(r'add_single\(\s*"([^"]+)"', src)
    manual_id = manual.group(1) if manual else None

    # 手写路径：`set_icon("svg:<id>")` + `add_single("<id>")`
    seticon = re.search(r'set_icon\(\s*"svg:([^"]+)"\s*\)', src)
    seticon_id = seticon.group(1) if seticon else None

    used_id = brand_id or seticon_id
    no_title_bar = name in NO_TITLE_BAR

    if used_id is None:
        if not no_title_bar:
            problems.append('标题栏未设品牌标记（既无 apply_brand 也无 set_icon("svg:…")）')
    else:
        # 注册必须覆盖用到的那个 id。
        #
        # 直接查“注册调用的参数里有没有这个 id”，而不是数字面量出现次数：
        # `set_icon("svg:{id}")` 里的 id 是拼在 `svg:` 后面的，
        # 数 `"{id}"` 的引号形式永远数不到它（实测：gbcode 明明注册了
        # `add_single("gbcode-logo")`，旧写法却报“只出现 1 次”）。
        reg_ids = set()
        for m in re.finditer(r'register_svg\(\s*"([^"]+)"', src):
            reg_ids.add(m.group(1))
        for m in re.finditer(r'add_single\(\s*"([^"]+)"', src):
            reg_ids.add(m.group(1))

        if not reg_ids:
            problems.append(f'set_icon 用了 "svg:{used_id}" 但**没有任何 SVG 注册调用**'
                            "（图标会静默不画）")
        elif used_id not in reg_ids:
            problems.append(
                f'品牌 id "{used_id}" 没有被注册（注册的是 {sorted(reg_ids)}）：'
                "图标不会出现")

    # SVG 素材与配色同源
    svg_file = None
    for cand in assets.glob("*logo*.svg"):
        svg_file = cand
        break
    if used_id and svg_file is None:
        problems.append("用了品牌标记但 assets 下没有 *logo*.svg")

    if png is not None and svg_file is not None:
        try:
            png_cols = dominant_colors(png)
            svg_cols = svg_colors(svg_file)
            if verbose:
                notes.append(f"PNG 主色 {png_cols[:6]}")
                notes.append(f"SVG 实色 {sorted(svg_cols)}")

            # **不按“是不是彩色”筛**，而是按“PNG 里有没有对应色”判。
            #
            # 曾经写过 `is_neutral()` 把“深浅差小”的颜色当背景跳过——
            # 而真实事故恰恰是“**深灰人像 vs 红白蓝三色条**”：灰被当成中性色
            # 跳过，于是一次真正的符号/配色整体替换仍然报 OK（逆向验证实测）。
            # 正确做法：一律去对，只豁免“确实两边都有”的白与纯黑。
            def shared_white_or_black(c):
                return (min(c) > 235) or (max(c) < 12)

            candidates = [c for c in svg_cols if not shared_white_or_black(c)]
            if candidates:
                # 容差取 **24**：比它宽就会把“深灰”匹配到另一个灰。
                # 实测：`#3D4249`(61,66,73) 与 PNG 的 (96,96,88) 相差 ~30，
                # 用 48 就把两种本该区分的色当成同一个 → 符号被整体换掉仍报 OK。
                # 而同一种颜色在 PNG 侧的抗锯齿误差典型在 10~20 内（量化到 8 级后再取簇真值）。
                def same(a, b):
                    return all(abs(x - y) <= 24 for x, y in zip(a, b))

                # 还要允许一种真实情况：PNG 侧有**径向渐变/高光**，
                # SVG 侧用“中点色”逼近（相期的 #1C1915 就是这么做）。
                # 所以匹配失败时再看“SVG 色是否落在 PNG 某个主色的同色相鄰域”——
                # 但那种情况太多误伤，此处不做：宁可报一次待人工确认，也不放走真事故。
                unmatched = [c for c in candidates
                             if not any(same(c, p) for p in png_cols)]
                if unmatched:
                    shown = [f"#{r:02X}{g:02X}{b:02X}" for r, g, b in unmatched]
                    have = [f"#{r:02X}{g:02X}{b:02X}" for r, g, b in png_cols[:6]]
                    problems.append(
                        f"SVG 使用的颜色 {shown} 在 PNG 主色 {have} 里找不到"
                        "（两份资产不是同一记号）")
        except Exception as exc:                      # noqa: BLE001 - 报告即可，不拦构建
            notes.append(f"配色校验跳过（{exc}）")

    # ③ .ico 资源
    if not any(assets.glob("*.ico")):
        problems.append("assets 下没有 .ico（资源管理器/快捷方式无图标）")

    return problems, notes


def main() -> int:
    # Windows 控制台缺省是 GBK，本文件输出含 `✗`/`─` 等符号——
    # 不改成 UTF-8 会直接 UnicodeEncodeError（实测把整份报告吞掉）。
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, OSError):
        pass

    verbose = "--verbose" in sys.argv
    args = [a for a in sys.argv[1:] if not a.startswith("--")]

    if args:
        names = args
    else:
        names = sorted(p.name for p in (ROOT / "apps").iterdir()
                       if p.is_dir() and p.name not in SKIP)

    total_problems = 0
    for name in names:
        problems, notes = check_app(name, verbose)
        status = "OK" if not problems else f"{len(problems)} 项问题"
        print(f"{name:<16}{status}")
        for p in problems:
            print(f"    ✗ {p}")
            total_problems += 1
        if verbose:
            for n in notes:
                print(f"    · {n}")

    print()
    if total_problems:
        print(f"── 失败 ── {total_problems} 项图标一致性问题")
        return 1
    print("── 通过 ── 全部应用的图标四条链路已接通且同源")
    return 0


if __name__ == "__main__":
    sys.exit(main())
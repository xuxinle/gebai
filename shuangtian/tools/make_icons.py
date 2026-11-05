#!/usr/bin/env python3
"""霜天各桌面应用的图标源图（PNG，256×256，无第三方依赖）。

## 为什么用代码画而不是找素材

图标要能**重新生成**：配色/圆角/线宽一旦要统一调整，位图没法批量改，
而代码画的可以。这里不引 Pillow（仓库工具链一贯无第三方依赖，
`make_ico.py` 也是自己解 PNG），只用一个极简的 RGBA 画布 + 抗锯齿。

## 风格（与既有 gbcode / xiangqi 图标一致）

- 圆角方形底（同 22% 圆角半径，接近 Windows 应用图标的观感）
- 单一前景符号，居中，不写字（16px 下文字必糊）
- 每个应用一个主题色，语义上对应它的用途

产物落在各应用的 `assets/`，再由 `tools/make_ico.py` 转多尺寸 `.ico`。

用法：
    py -3 tools/make_icons.py            # 全部重画
    py -3 tools/make_icons.py gedou      # 只画一个
"""
import math
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SIZE = 256          # 源图边长（.ico 的最大档就是 256，够用）
SS = 3              # 超采样倍数：3×3 子采样做抗锯齿，边缘足够干净且快


class Canvas:
    """RGBA 画布，浮点累积后一次性量化（避免多形状叠加时反复取整）。"""

    def __init__(self, size):
        self.size = size
        self.buf = [[0.0, 0.0, 0.0, 0.0] for _ in range(size * size)]

    def blend(self, x, y, color, alpha):
        if alpha <= 0.0:
            return
        if not (0 <= x < self.size and 0 <= y < self.size):
            return
        dst = self.buf[y * self.size + x]
        a = min(1.0, alpha)
        # 源覆盖（source-over），颜色各通道线性混合
        out_a = a + dst[3] * (1.0 - a)
        if out_a <= 0.0:
            return
        for c in range(3):
            dst[c] = (color[c] * a + dst[c] * dst[3] * (1.0 - a)) / out_a
        dst[3] = out_a

    def to_rgba(self):
        out = bytearray(self.size * self.size * 4)
        for i, (r, g, b, a) in enumerate(self.buf):
            out[i * 4] = max(0, min(255, int(r * 255 + 0.5)))
            out[i * 4 + 1] = max(0, min(255, int(g * 255 + 0.5)))
            out[i * 4 + 2] = max(0, min(255, int(b * 255 + 0.5)))
            out[i * 4 + 3] = max(0, min(255, int(a * 255 + 0.5)))
        return bytes(out)


def rounded_rect(canvas, x0, y0, x1, y1, radius, color, alpha=1.0):
    """圆角矩形（超采样抗锯齿）。radius=0 时就是普通矩形。"""
    s = canvas.size
    for py in range(s):
        for px in range(s):
            hits = 0
            for sy in range(SS):
                for sx in range(SS):
                    fx = px + (sx + 0.5) / SS
                    fy = py + (sy + 0.5) / SS
                    if not (x0 <= fx <= x1 and y0 <= fy <= y1):
                        continue
                    # 四个角上判断到圆心的距离
                    cx = min(max(fx, x0 + radius), x1 - radius)
                    cy = min(max(fy, y0 + radius), y1 - radius)
                    if math.hypot(fx - cx, fy - cy) <= radius:
                        hits += 1
            if hits:
                canvas.blend(px, py, color, alpha * hits / (SS * SS))


def disc(canvas, cx, cy, r, color, alpha=1.0):
    s = canvas.size
    for py in range(max(0, int(cy - r - 1)), min(s, int(cy + r + 2))):
        for px in range(max(0, int(cx - r - 1)), min(s, int(cx + r + 2))):
            hits = 0
            for sy in range(SS):
                for sx in range(SS):
                    fx = px + (sx + 0.5) / SS
                    fy = py + (sy + 0.5) / SS
                    if math.hypot(fx - cx, fy - cy) <= r:
                        hits += 1
            if hits:
                canvas.blend(px, py, color, alpha * hits / (SS * SS))


def thick_line(canvas, x0, y0, x1, y1, width, color, alpha=1.0):
    """带宽度的线段（两端圆头）。"""
    s = canvas.size
    r = width / 2.0
    minx = max(0, int(min(x0, x1) - r - 1))
    maxx = min(s, int(max(x0, x1) + r + 2))
    miny = max(0, int(min(y0, y1) - r - 1))
    maxy = min(s, int(max(y0, y1) + r + 2))
    dx, dy = x1 - x0, y1 - y0
    length2 = dx * dx + dy * dy
    for py in range(miny, maxy):
        for px in range(minx, maxx):
            hits = 0
            for sy in range(SS):
                for sx in range(SS):
                    fx = px + (sx + 0.5) / SS
                    fy = py + (sy + 0.5) / SS
                    if length2 > 0:
                        t = ((fx - x0) * dx + (fy - y0) * dy) / length2
                        t = max(0.0, min(1.0, t))
                    else:
                        t = 0.0
                    nx, ny = x0 + t * dx, y0 + t * dy
                    if math.hypot(fx - nx, fy - ny) <= r:
                        hits += 1
            if hits:
                canvas.blend(px, py, color, alpha * hits / (SS * SS))


def point_in_convex(fx, fy, pts):
    """点是否在凸多边形内 —— **绕向无关**。

    早先这里直接要求叉积同号，而顶点顺序是顺时针时全部反号，
    于是多边形永远判"不在内部"、形状根本不画出来。实测代价：
    裁云的播放三角一直是隐形的（图标看着像一块空胶片框）。
    """
    n = len(pts)
    sign = 0
    for i in range(n):
        ax, ay = pts[i]
        bx, by = pts[(i + 1) % n]
        cross = (bx - ax) * (fy - ay) - (by - ay) * (fx - ax)
        if abs(cross) < 1e-12:
            continue
        this_sign = 1 if cross > 0 else -1
        if sign == 0:
            sign = this_sign
        elif sign != this_sign:
            return False
    return True


def quad(canvas, pts, color, alpha=1.0):
    """凸四边形填充（用于"笔画"形状，如箭头/卡片）。"""
    s = canvas.size
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    for py in range(max(0, int(min(ys)) - 1), min(s, int(max(ys)) + 2)):
        for px in range(max(0, int(min(xs)) - 1), min(s, int(max(xs)) + 2)):
            hits = 0
            for sy in range(SS):
                for sx in range(SS):
                    fx = px + (sx + 0.5) / SS
                    fy = py + (sy + 0.5) / SS
                    if point_in_convex(fx, fy, pts):
                        hits += 1
            if hits:
                canvas.blend(px, py, color, alpha * hits / (SS * SS))


def write_png(path, size, rgba):
    raw = bytearray()
    for y in range(size):
        raw.append(0)
        raw += rgba[y * size * 4:(y + 1) * size * 4]

    def chunk(tag, payload):
        body = tag + payload
        return (struct.pack(">I", len(payload)) + body +
                struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)


# —— 各应用图标 ——

def icon_gallery(c):
    """gallery（控件陈列）：九宫格 —— "一排控件"最直白的表达。"""
    fg = (1.0, 1.0, 1.0)
    n, gap, pad = 3, 14, 46
    cell = (SIZE - 2 * pad - (n - 1) * gap) / n
    for row in range(n):
        for col in range(n):
            x0 = pad + col * (cell + gap)
            y0 = pad + row * (cell + gap)
            rounded_rect(c, x0, y0, x0 + cell, y0 + cell, cell * 0.26, fg,
                         alpha=1.0 if (row + col) % 2 == 0 else 0.45)


def icon_louyue(c):
    """镂月（海报/排版）：一张"版式"——标题横条 + 两个正文块 + 图像方块。"""
    fg = (1.0, 1.0, 1.0)
    rounded_rect(c, 52, 44, 204, 212, 12, fg, alpha=0.22)     # 纸面
    rounded_rect(c, 70, 64, 186, 96, 8, fg)                    # 标题
    rounded_rect(c, 70, 116, 140, 128, 6, fg, alpha=0.85)      # 正文行
    rounded_rect(c, 70, 140, 160, 152, 6, fg, alpha=0.85)
    rounded_rect(c, 150, 116, 186, 152, 8, fg, alpha=0.55)     # 图像块


def icon_caiyun(c):
    """裁云（视频）：胶片 + 播放三角。

    三角要**深色挖空**而不是同色调：早先用白色画在白色胶片框上，
    实测出来是一片空白（影片框内根本看不见三角）。
    """
    fg = (1.0, 1.0, 1.0)
    hole = (0.10, 0.35, 0.34)     # 胶片框内的“压暗”色 → 三角可辨
    rounded_rect(c, 44, 66, 212, 190, 14, fg, alpha=0.24)      # 胶片框
    for i in range(5):
        x = 58 + i * 32
        rounded_rect(c, x, 52, x + 18, 66, 4, fg, alpha=0.85)
        rounded_rect(c, x, 190, x + 18, 204, 4, fg, alpha=0.85)
    rounded_rect(c, 44, 66, 212, 190, 14, hole, alpha=0.55)    # 把框内压暗
    quad(c, [(108, 100), (108, 156), (156, 128)], fg)          # 现在白三角清楚


def icon_gedou(c):
    """格斗：两拳相击。

    早先画成“两个圆 + 中间一个黄球”，实测看着像**哑铃**——
    原因是圆臂与中间的冲击星混在一起、没有“迎面而來”的方向感。
    现在改用一只**举起的拳套**（拳 + 护腕），配合三道冲击弧，
    小尺寸下也能读成“格斗”而不是器械。
    """
    fg = (1.0, 1.0, 1.0)
    # 拳套主体（略向右上倾斜）+ 护腕
    disc(c, 116, 116, 46, fg)
    rounded_rect(c, 62, 150, 128, 196, 18, fg, alpha=0.88)
    # 指节凹槽（用背景色刻三道）
    for i in range(3):
        y = 88 + i * 22
        thick_line(c, 96, y, 140, y - 10, 7, (0.78, 0.26, 0.32), alpha=0.9)
    # 冲击弧（向右）
    for i, r in enumerate((66, 84, 102)):
        for step in range(24):
            a0 = math.radians(-42 + step * 3.5)
            a1 = math.radians(-42 + (step + 1) * 3.5)
            thick_line(c, 116 + math.cos(a0) * r, 116 + math.sin(a0) * r,
                       116 + math.cos(a1) * r, 116 + math.sin(a1) * r,
                       9, (1.0, 0.86, 0.45), alpha=0.95 - i * 0.18)


def icon_cefu(c):
    """册府（结构化知识库）：一摞书 + 检索环。"""
    fg = (1.0, 1.0, 1.0)
    rounded_rect(c, 52, 74, 168, 106, 8, fg, alpha=0.9)
    rounded_rect(c, 60, 116, 176, 148, 8, fg, alpha=0.7)
    rounded_rect(c, 68, 158, 184, 190, 8, fg, alpha=0.5)
    # 检索放大镜（压在右上）
    disc(c, 176, 96, 34, (0.15, 0.18, 0.24), alpha=1.0)
    for py in range(SIZE):
        pass
    # 环：用两段粗圆弧近似
    for i in range(72):
        a0 = math.radians(i * 5)
        a1 = math.radians((i + 1) * 5)
        thick_line(c, 176 + math.cos(a0) * 34, 96 + math.sin(a0) * 34,
                   176 + math.cos(a1) * 34, 96 + math.sin(a1) * 34, 10, fg)
    disc(c, 176, 96, 20, (0.15, 0.18, 0.24), alpha=1.0)
    thick_line(c, 200, 120, 222, 142, 12, fg)


def icon_xiangqi(c):
    """相期：红/白/蓝三段竖条（与 `make_xiangqi_icon_png` 小尺寸档同源）。

    ⚠ 这份图标必须与 `examples/xiangqi/main.cpp` 的 `make_xiangqi_icon_png()`
    小尺寸档（≤24px 那一档）画**同一套图形**：那份是任务栏/窗口图标，
    这份经 `make_ico.py` 转成 exe 资源图标（资源管理器/快捷方式看到的）。
    之前两套各画各的（这里是卡片小人，那边是三竖条），
    同一个应用在两处显示两个图标（用户实测报过）。

    同源基准（与 main.cpp 常量一致）：
    - 条色：红 #F26455 / 白 / 蓝 #5AA4E8，色相差 201°，16px 下不读成同色
    - 位置：cx = 0.265 / 0.500 / 0.735，半宽 0.058 / 0.064 / 0.058（×256）
    - 纵向：0.230..0.770（×256 = 59..197）
    """
    red = (0xF2 / 255.0, 0x64 / 255.0, 0x55 / 255.0)
    white = (1.0, 1.0, 1.0)
    blue = (0x5A / 255.0, 0xA4 / 255.0, 0xE8 / 255.0)
    bars = (
        (0.265 * SIZE, 0.058 * SIZE, red),
        (0.500 * SIZE, 0.064 * SIZE, white),
        (0.735 * SIZE, 0.058 * SIZE, blue),
    )
    top, bottom = 0.230 * SIZE, 0.770 * SIZE
    for cx, half_w, color in bars:
        rounded_rect(c, cx - half_w, top, cx + half_w, bottom, 6, color)


def icon_gbcode(c):
    """gbcode：尖括号 + 中心点。

    用**粗线段**拼而不是四边形填：四边形版实测出来是菱形
    （凹多边形被当成凸的来填，两个角退化成三角），看着不像尖括号。
    """
    fg = (1.0, 1.0, 1.0)
    w = 26
    thick_line(c, 106, 66, 62, 128, w, fg)     # <
    thick_line(c, 62, 128, 106, 190, w, fg)
    thick_line(c, 150, 66, 194, 128, w, fg)    # >
    thick_line(c, 194, 128, 150, 190, w, fg)
    disc(c, 128, 128, 15, fg)


APPS = {
    # name: (源图输出路径, 底色, 画法)
    "gallery": ("examples/gallery/assets/gallery-icon.png", (0.36, 0.42, 0.86), icon_gallery),
    "louyue":  ("examples/louyue/assets/louyue-icon.png",   (0.85, 0.42, 0.36), icon_louyue),
    "caiyun":  ("examples/caiyun/assets/caiyun-icon.png",   (0.20, 0.62, 0.60), icon_caiyun),
    "gedou":   ("examples/gedou/assets/gedou-icon.png",     (0.78, 0.26, 0.32), icon_gedou),
    "cefu":    ("examples/cefu/assets/cefu-icon.png",       (0.42, 0.36, 0.68), icon_cefu),
    "xiangqi": ("examples/xiangqi/assets/xiangqi-icon.png", (0x14 / 255.0, 0x11 / 255.0, 0x0E / 255.0), icon_xiangqi),
    "gbcode":  ("examples/gbcode/assets/gbcode-icon.png",   (0.33, 0.42, 0.95), icon_gbcode),
}


def main():
    only = sys.argv[1:] or list(APPS)
    for name in only:
        if name not in APPS:
            print(f"未知应用：{name}（可选：{', '.join(APPS)}）")
            return 2
        rel, base, draw = APPS[name]
        canvas = Canvas(SIZE)
        radius = SIZE * 0.22
        rounded_rect(canvas, 0, 0, SIZE - 1, SIZE - 1, radius, base)
        draw(canvas)
        out = ROOT / rel
        write_png(out, SIZE, canvas.to_rgba())
        print(f"{name:10s} → {rel}  ({out.stat().st_size} 字节)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

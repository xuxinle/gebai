"""质感层核验：判「画上去没有」与「方向对不对」。

## 口径教训（本脚本第一版判错过）

第一版拿**开启与关闭的差值图**去判暗角方向——结果四角差值 1~2、中心差值 23，
于是报「暗角画反了」。而看实际画面完全正确。

错在哪：差值图回答的是"这一层改了多少像素"，**不回答"改完以后哪里亮哪里暗"**。
四角本来就是 `#0A1020` 压到近黑，暗角在那种底色上的绝对变化很小（饱和），
所以差值天然比中心小。**方向要用「开启后」的绝对亮度判，不是用差值。**

这与仓库里 perceptual_changes 的那条是同一类错误：拿"变化量"当"观感"。
"""
import subprocess
import sys

sys.stdout.reconfigure(encoding="utf-8")


def gray(path):
    return subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-f", "rawvideo",
                           "-pix_fmt", "gray", "-"], capture_output=True).stdout


def avg(img, w, h, x0, y0, x1, y1, step=6):
    s = 0
    c = 0
    for y in range(y0, y1, step):
        for x in range(x0, x1, step):
            s += img[y * w + x]
            c += 1
    return s / max(1, c)


on_path, off_path = sys.argv[1], sys.argv[2]
w, h = 1920, 1080
on = gray(on_path)
off = gray(off_path)
m = 80

regions = {
    "左上角": (0, 0, m, m),
    "右上角": (w - m, 0, w, m),
    "左下角": (0, h - m, m, h),
    "右下角": (w - m, h - m, w, h),
    "画面中心": (w // 2 - m // 2, h // 2 - m // 2, w // 2 + m // 2, h // 2 + m // 2),
}

print("区域        关闭时  开启后   (差值)")
vals = {}
for name, box in regions.items():
    o1 = avg(off, w, h, *box)
    o2 = avg(on, w, h, *box)
    vals[name] = (o1, o2)
    print(f"  {name:8s} {o1:6.1f}  {o2:6.1f}  {o2 - o1:+6.1f}")

# 生效判据：整体必须有可观变化
diff = sum(abs(a - b) for a, b in zip(on, off)) / len(on)
print(f"\n整体平均差: {diff:.2f}")
print("质感生效: " + ("是 ✓" if diff > 0.5 else "**否**（未画上）"))

# 方向判据：用**开启后**的绝对亮度（不是差值）
corner_on = sum(vals[k][1] for k in ("左上角", "右上角", "左下角", "右下角")) / 4.0
center_on = vals["画面中心"][1]
print(f"开启后：四角均值 {corner_on:.1f}  中心 {center_on:.1f}")
print("暗角方向（四角应暗于中心，除非光晕落在中心）: "
      + ("四角更暗 ✓" if corner_on < center_on else "四角不暗 ✗"))

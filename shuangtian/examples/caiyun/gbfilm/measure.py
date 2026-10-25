"""量成片/单幕的**变化**（两个正交口径同时报）。

## 为何必须报两个口径（这最贵的一课）

早先只用 32×18 网格（每格 60px 的格均值差）一个口径，结果：
幕 6 有 30 个节点沿双环流动，网格口径报 **95% 静止**——
而逐像素实测量是 **7.8% 像素变化 >8 级、全分辨率平均差 2.79**，画面上明显在动。

原因：网格把每格 3600 个像素平均成 1 个值。**又小又稀疏的运动元素**
（细环线、液体粒子、流动节点）在格均值里几乎不变——
它们占的像素面积太小，而“有没有在动”靠的是“变了多少像素”。

因此：
- **网格口径**：与验收器/渲染日志可对账，适合“这一段是不是完全卡住了”的粗筛；
- **像素口径**（本脚本新报）：`pixel%` = 变化 >8 级的像素占比、`mad` = 全分辨率平均差，
  回答“观众眼里画面到底活不活”。

**网格值高不等于设计差，也不等于要加东西。** 两者一起看：
网格 95% + 像素 8% = 小而密的运动（正常）；
网格 100% + 像素 0.2% = 真的死了（该改）；
网格 30% + 像素 30% = 整屏在翻（小心抢镜头）。

用法：
  py measure.py film.mp4
  py measure.py film.mp4 --scenes 4.6,5.2,5.8,5.4,6.0,5.4,6.0 --quiet
"""
import argparse
import subprocess
import sys

COLS, ROWS = 32, 18


def frames_grid(path: str):
    """32×18 网格的格均值（与 caiyun_verify / 渲染日志同口径）。"""
    cmd = ["ffmpeg", "-v", "error", "-i", path, "-vf", f"scale={COLS}:{ROWS}",
           "-f", "rawvideo", "-pix_fmt", "rgb24", "-"]
    raw = subprocess.run(cmd, capture_output=True).stdout
    size = COLS * ROWS * 3
    n = len(raw) // size
    out = []
    for k in range(n):
        o = k * size
        out.append([0.299 * raw[o + i * 3] + 0.587 * raw[o + i * 3 + 1] + 0.114 * raw[o + i * 3 + 2]
                    for i in range(COLS * ROWS)])
    return out


def frames_pixel(path: str):
    """全分辨率灰度帧（降采样到 480×270 以控内存：够看“变了多少像素”）。"""
    cmd = ["ffmpeg", "-v", "error", "-i", path, "-vf", "scale=480:270",
           "-f", "rawvideo", "-pix_fmt", "gray", "-"]
    raw = subprocess.run(cmd, capture_output=True).stdout
    n = 480 * 270
    return [raw[k * n:(k + 1) * n] for k in range(len(raw) // n)]


def diffs_of(frames):
    d = []
    for i in range(1, len(frames)):
        a, b = frames[i - 1], frames[i]
        d.append(sum(abs(x - y) for x, y in zip(a, b)) / len(a))
    return d


def diffs_pixel(frames):
    """返回 [(变化>8级的像素占比, 平均绝对差)]，与 frames 对齐（长 1）。"""
    out = []
    for i in range(1, len(frames)):
        a, b = frames[i - 1], frames[i]
        changed = 0
        total = 0
        s = 0
        for x, y in zip(a, b):
            d = abs(x - y)
            s += d
            if d > 8:
                changed += 1
            total += 1
        out.append((100.0 * changed / max(1, total), s / max(1, total)))
    return out


def stats(vals):
    if not vals:
        return None
    s = sorted(vals)
    m = len(s)
    return {
        "pairs": m,
        "still%": round(100.0 * sum(1 for v in s if v < 1.2) / m, 1),
        "median": round(s[m // 2], 2),
        "p95": round(s[min(m - 1, int(m * 0.95))], 2),
        "mean": round(sum(s) / m, 2),
        "peak": round(s[-1], 2),
    }


def pixel_stats(pairs):
    if not pairs:
        return None
    pct = sorted(p[0] for p in pairs)
    mad = sorted(p[1] for p in pairs)
    m = len(pct)
    return {
        "pixel%_median": round(pct[m // 2], 2),
        "pixel%_p95": round(pct[min(m - 1, int(m * 0.95))], 2),
        "mad_median": round(mad[m // 2], 2),
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("video")
    ap.add_argument("--fps", type=float, default=30.0)
    ap.add_argument("--scenes", default="", help="幕时长逗号分隔（秒），用于逐幕汇总")
    ap.add_argument("--quiet", action="store_true", help="只打逐幕汇总")
    ap.add_argument("--no-pixel", action="store_true", help="不算像素口径（快，但少了“观众眼里活不活”）")
    args = ap.parse_args()

    gframes = frames_grid(args.video)
    if len(gframes) < 3:
        print("帧数不足")
        return 1
    gd = diffs_of(gframes)
    fps = args.fps

    pd = None
    if not args.no_pixel:
        pframes = frames_pixel(args.video)
        if len(pframes) >= 3:
            pd = diffs_pixel(pframes)

    print(f"{args.video}: {len(gframes)} 帧 / {len(gframes) / fps:.2f}s")
    print("网格口径（32x18，差<1.2 记静止；与验收器/渲染日志同口径）:", stats(gd))
    if pd:
        print("像素口径（480x270，回答'观众眼里活不活'）:", pixel_stats(pd))
        print("  读法：网格 95% + 像素 8% = 小而密的运动（正常）；"
              "网格 100% + 像素 <0.5% = 真的死了；网格 30% + 像素 30% = 整屏在翻（小心抢镜头）")

    if not args.quiet:
        print("\n逐秒：")
        per = int(round(fps))
        for s in range(0, len(gd) // per + 1):
            seg = gd[s * per:(s + 1) * per]
            if not seg:
                continue
            still = 100.0 * sum(1 for v in seg if v < 1.2) / len(seg)
            extra = ""
            if pd:
                pseg = pd[s * per:(s + 1) * per]
                if pseg:
                    extra = f"  pixel%={sum(p[0] for p in pseg) / len(pseg):5.2f}"
            print(f"  {s:3d}s  grid={sum(seg) / len(seg):6.2f}  still={still:5.1f}%{extra}")

    if args.scenes:
        lens = [float(x) for x in args.scenes.split(",") if x.strip()]
        print("\n逐幕：")
        start = 0.0
        for i, dur in enumerate(lens):
            a = int(round(start * fps))
            b = int(round((start + dur) * fps))
            seg = gd[max(0, a - 1):max(0, b - 1)]
            st = stats(seg)
            line = f"  幕{i + 1} {start:5.2f}-{start + dur:5.2f}s  {st}"
            if pd:
                pseg = pd[max(0, a - 1):max(0, b - 1)]
                ps = pixel_stats(pseg)
                if ps:
                    line += f"  pixel%={ps['pixel%_median']:5.2f} mad={ps['mad_median']:5.2f}"
            print(line)
            start += dur
    return 0


if __name__ == "__main__":
    sys.exit(main())

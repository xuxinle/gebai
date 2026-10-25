"""量成片/单幕的**逐秒变化**（与 caiyun_verify 同口径）。

口径必须与验收器一致，否则"改好了"只是换了个尺子：
32×18 网格（每格 60px）、逐格灰度均值、相邻帧绝对差均值 < 1.2 记作"静止"。

用法：
  py measure.py film.mp4                          # 全片逐秒
  py measure.py film.mp4 --scenes 4.6,5.2,5.8,...  # 按幕输出汇总
"""
import argparse
import subprocess
import sys

COLS, ROWS = 32, 18


def frames_luma(path: str):
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


def diffs_of(frames):
    d = []
    for i in range(1, len(frames)):
        a, b = frames[i - 1], frames[i]
        d.append(sum(abs(x - y) for x, y in zip(a, b)) / len(a))
    return d


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


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("video")
    ap.add_argument("--fps", type=float, default=30.0)
    ap.add_argument("--scenes", default="", help="幕时长逗号分隔（秒），用于逐幕汇总")
    ap.add_argument("--quiet", action="store_true", help="只打逐幕汇总")
    args = ap.parse_args()

    frames = frames_luma(args.video)
    if len(frames) < 3:
        print("帧数不足")
        return 1
    d = diffs_of(frames)
    fps = args.fps

    print(f"{args.video}: {len(frames)} 帧 / {len(frames) / fps:.2f}s  口径 32x18 网格，差<1.2 记静止")
    overall = stats(d)
    print("全片:", overall)

    if not args.quiet:
        print("\n逐秒（mean / 该秒静止帧占比）：")
        per = int(round(fps))
        for s in range(0, len(d) // per + 1):
            seg = d[s * per:(s + 1) * per]
            if not seg:
                continue
            still = 100.0 * sum(1 for v in seg if v < 1.2) / len(seg)
            bar = "#" * int(sum(seg) / len(seg) * 4)
            print(f"  {s:3d}s  mean={sum(seg) / len(seg):6.2f}  still={still:5.1f}%  {bar}")

    if args.scenes:
        lens = [float(x) for x in args.scenes.split(",") if x.strip()]
        print("\n逐幕：")
        start = 0.0
        for i, dur in enumerate(lens):
            a = int(round(start * fps))
            b = int(round((start + dur) * fps))
            seg = d[max(0, a - 1):max(0, b - 1)]
            st = stats(seg)
            flag = "  ← 太静" if st and st["still%"] > 70 else ""
            print(f"  幕{i + 1} {start:5.2f}-{start + dur:5.2f}s  {st}{flag}")
            start += dur
    return 0


if __name__ == "__main__":
    sys.exit(main())

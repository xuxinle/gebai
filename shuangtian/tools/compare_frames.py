"""并排对比两帧（改前 / 改后），带标签与分隔线。

为什么要写这个：精致度是**看**出来的，不是量出来的。但来回翻两张图
很难判断"到底哪张好"——并排放在同一张图里才有可比性。

用法：
    py tools/compare_frames.py --before a.png --after b.png --out cmp.png
        [--label-before 改前] [--label-after 改后] [--scale 0.5]
"""
import argparse
import os
import subprocess
import sys


def probe_size(path):
    r = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0",
         "-show_entries", "stream=width,height", "-of", "csv=p=0", path],
        capture_output=True, text=True)
    txt = r.stdout.strip()
    if not txt:
        return None
    w, h = txt.split(",")[:2]
    return int(w), int(h)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--before", required=True)
    ap.add_argument("--after", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--scale", type=float, default=0.5, help="每张的缩放比")
    ap.add_argument("--crop", default="", help="裁剪区域 x:y:w:h（原图坐标），两图共用")
    args = ap.parse_args()

    for path in (args.before, args.after):
        if not os.path.isfile(path):
            print(f"缺文件：{path}", file=sys.stderr)
            return 1

    size = probe_size(args.before)
    if size is None:
        print(f"读不出尺寸：{args.before}", file=sys.stderr)
        return 1
    w, h = size
    sw, sh = int(w * args.scale), int(h * args.scale)

    crop = f"crop={args.crop}," if args.crop else ""
    # 左：改前；右：改后；中间 4px 白色分隔线。
    #
    # 不画文字标签：`drawtext` 依赖 fontconfig，而本机没有默认字体配置
    # （实测报 `Cannot load default config file`）。标签改由调用方在文件名里体现
    # ——文件名在聊天里就是可见的，比图内的字更可靠。
    filt = (
        f"[0:v]{crop}scale={sw}:{sh}[a];"
        f"[1:v]{crop}scale={sw}:{sh}[b];"
        f"[a][b]hstack=inputs=2[ab];"
        f"[ab]drawbox=x={sw - 2}:y=0:w=4:h={sh}:color=white@0.85:t=fill"
    )
    cmd = ["ffmpeg", "-y", "-v", "error", "-i", args.before, "-i", args.after,
           "-filter_complex", filt, "-frames:v", "1", args.out]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print("ffmpeg 失败：", r.stderr[:600], file=sys.stderr)
        return 1
    print(f"已生成 {args.out}（{sw * 2 + 4}x{sh}）")
    return 0


if __name__ == "__main__":
    sys.exit(main())

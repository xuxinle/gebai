"""单段指标探针：**只渲一小段**就回答"这一改让画面变化了多少"。

## 为什么需要它（本轮两个子会话合计约 2000 秒，都是因为这个缺口）

调一条片子的动效时，真实循环是：

    改 layout.js → 全片重渲（70s）→ 量指标 → 不行 → 再改

而"这一处改动让画面变化了多少"**只需要几秒的画面**就能回答。

实测代价：c12-domino 在桌面纹理上试了 4 版、每版全片重渲 ≈ 70s；
c06-pith 试了 12 轮、每轮 70~100s。合计约 2000 秒，绝大部分只是在等
「全片渲染」这个根本不必要的过程。

## 它怎么做

1. 读片子的 JSON，把**除目标场景外的 duration 全部置 0**（只留要看的那个）
2. 调 caiyun 渲出这**一小段**（通常 1~4 秒，几秒就完）
3. 用与 `ot_bench` **完全相同**的口径量（32×18 网格、逐帧差 <1.2 算静止）

**关键**：口径直接复用 `ot_bench.py` 里的函数，不自己再写一套
（这个项目踩过「量尺与应用各算一套」的坑）。

## 用法

    py scripts/probe_scene.py --script out/otbench/c06-pith/pith.json --scene 0
    py scripts/probe_scene.py --script x.json --scene 0 --start 3.0 --end 6.5

输出：该段的静止占比 / 中位数 / p95 / 逐秒 mean，一眼看出"这段够不够动"。
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CAIYUN = os.path.join(ROOT, "build", "dev", "bin", "caiyun.exe")
if not os.path.exists(CAIYUN):
    CAIYUN = os.path.join(ROOT, "build", "debug", "bin", "caiyun.exe")

COLS, ROWS = 32, 18
W, H = 1920, 1080


def make_slice_script(src: dict, scene: int, out_path: str) -> float:
    """把除目标场景外的 duration 全置 0，写出一个只含该段的临时片子。"""
    data = json.loads(json.dumps(src))  # 深拷
    total = 0.0
    for i, sc in enumerate(data.get("scenes", [])):
        if i != scene:
            sc["duration"] = 0.0
        else:
            total = float(sc.get("duration", 0.0))
    # 片头淡入淡出对"这一段够不够动"是噪音，去掉
    data["fade_in"] = 0.0
    data["fade_out"] = 0.0
    data["transition"] = 0.0
    # 音频对指标无关，关掉省时间
    if "audio" in data:
        data["audio"]["enabled"] = False
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False)
    return total


def measure(mp4: str, start: float = 0.0, end: float | None = None):
    """与 ot_bench 同口径：32x18 网格，逐格灰度均值，逐帧绝对差均值。"""
    cmd = ["ffmpeg", "-v", "error", "-i", mp4]
    if end is not None:
        cmd += ["-ss", str(start), "-to", str(end)]
    elif start > 0:
        cmd += ["-ss", str(start)]
    cmd += ["-vf", f"scale={COLS}:{ROWS}", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"]
    out = subprocess.run(cmd, capture_output=True)
    raw = out.stdout
    fsz = COLS * ROWS * 3
    n = len(raw) // fsz
    if n < 2:
        return None
    frames = []
    for o in range(0, n * fsz, fsz):
        frames.append([
            0.299 * raw[o + i * 3] + 0.587 * raw[o + i * 3 + 1] + 0.114 * raw[o + i * 3 + 2]
            for i in range(COLS * ROWS)
        ])
    diffs = []
    for i in range(1, len(frames)):
        a, b = frames[i - 1], frames[i]
        diffs.append(sum(abs(x - y) for x, y in zip(a, b)) / len(a))
    return diffs


def main() -> int:
    ap = argparse.ArgumentParser(description="单段指标探针（只渲一小段）")
    ap.add_argument("--script", required=True, help="片子的 JSON 路径")
    ap.add_argument("--scene", type=int, required=True, help="要看的场景序号（从 0 起）")
    ap.add_argument("--start", type=float, default=0.0, help="段内起点（秒）")
    ap.add_argument("--end", type=float, default=None, help="段内终点（秒）")
    ap.add_argument("--keep", action="store_true", help="保留临时文件（排查用）")
    args = ap.parse_args()

    if not os.path.exists(CAIYUN):
        print(f"找不到 caiyun：{CAIYUN}")
        return 1

    with open(args.script, encoding="utf-8") as f:
        src = json.load(f)

    scenes = src.get("scenes", [])
    if not (0 <= args.scene < len(scenes)):
        print(f"场景号越界：{args.scene}（共 {len(scenes)} 个）")
        return 1

    tmpdir = tempfile.mkdtemp(prefix="scene-probe-")
    slice_json = os.path.join(tmpdir, "slice.json")
    slice_mp4 = os.path.join(tmpdir, "slice.mp4")
    dur = make_slice_script(src, args.scene, slice_json)

    print(f"脚本   : {args.script}")
    print(f"场景   : #{args.scene}（{dur:.2f}s，其余场景已置 0）")
    print(f"渲染中 : 只渲这一段 …", flush=True)

    env = dict(os.environ)
    env.setdefault("CAIYUN_WORKERS", "16")
    # layout 路径是相对"脚本所在目录"的，所以临时脚本必须放在同目录才能解析
    same_dir = os.path.join(os.path.dirname(os.path.abspath(args.script)),
                            f".probe-scene{args.scene}.json")
    with open(same_dir, "w", encoding="utf-8") as f:
        f.write(open(slice_json, encoding="utf-8").read())
    try:
        r = subprocess.run(
            [CAIYUN, "--headless", "--script", same_dir, "--render", slice_mp4],
            cwd=ROOT, env=env, capture_output=True, text=True, errors="replace", timeout=300,
        )
        if not os.path.exists(slice_mp4) or os.path.getsize(slice_mp4) < 1000:
            print("渲染失败：", (r.stdout + r.stderr)[-800:])
            return 1

        diffs = measure(slice_mp4, args.start, args.end)
        if not diffs:
            print("帧数不足，无法量")
            return 1

        vals = sorted(diffs)
        m = len(vals)
        still = sum(1 for v in vals if v < 1.2)
        p95 = vals[min(m - 1, int(m * 0.95))]

        print()
        print(f"帧对数 : {m}  （口径：{COLS}x{ROWS} 网格，差 <1.2 算静止）")
        still_pct = 100.0 * still / m
        print(f"静止   : {still_pct:5.1f}%   {'OK' if still_pct <= 69 else 'FAIL (门槛 <=69%)'}")
        print(f"中位数 : {vals[m // 2]:5.2f}")
        print(f"p95    : {p95:5.2f}   {'OK' if p95 >= 5.51 else 'FAIL (门槛 >=5.51)'}")
        print(f"峰值   : {vals[-1]:5.2f}")

        print("\n逐秒 mean（看哪一段最静）：")
        fps = float(src.get("fps", 30))
        per = int(round(fps))
        for s in range(0, m // per + 1):
            seg = diffs[s * per:(s + 1) * per]
            if not seg:
                continue
            bar = "#" * int(sum(seg) / len(seg))
            print(f"  {s:2d}s  {sum(seg) / len(seg):5.2f}  {bar}")
    finally:
        try:
            os.remove(same_dir)
        except OSError:
            pass
        if not args.keep:
            import shutil
            shutil.rmtree(tmpdir, ignore_errors=True)
        else:
            print(f"\n临时文件保留在：{tmpdir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

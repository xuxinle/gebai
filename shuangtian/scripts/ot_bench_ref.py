"""为 onetake 的 13 条参考片建**同口径基准** → out/otbench/ref_baseline.json

## 为什么从「帧目录」建，而不是从 mp4 建

原版遍历 `onetake/cases/*/*.mp4` 抽帧。实测的两次代价：

1. **源被移走后静默退化**：`onetake/cases` 里已经没有 mp4 了，脚本 `continue` 掉每一条，
   最后只写回 1 条就覆盖了整份基线——而输出「基准确认：1 条」看起来像正常完成。
2. 抽帧本身是重复劳动：帧已经落盘（`_ref/<case>/frames/`），没有任何理由每轮重抽。

所以：**帧目录才是真值源**。mp4 只在帧缺失时才去找；找不到就**明确报错并拒绝写回**
（宁可让调用方看见失败，也不能用半份数据覆盖一份好基线）。

## 口径

所有数都用 `ot_bench.pixel_still()` / `motion_qa` 量——与被测片**同一份代码**。
这是全套对比成立的前提：参照片的数不能用它自己 README 里的尺子。

用法：
    py scripts/ot_bench_ref.py              # 用已有帧重建（不重抽）
    py scripts/ot_bench_ref.py --refetch    # 强制重新抽帧（需要 mp4 还在）
"""

import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ONETAKE = r"C:\Users\Administrator\code\onetake\cases"
OUT = os.path.join(ROOT, "out", "otbench", "_ref")
MOTION_QA = os.path.join(ROOT, "build", "dev", "bin", "motion_qa.exe")
BASELINE = os.path.join(ROOT, "out", "otbench", "ref_baseline.json")

# 与 ot_bench 用**同一份**量尺（静止/过亮/帧差口径必须完全一致，否则两边的数不可比）
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ot_bench import pixel_still  # noqa: E402

# 帧缺失时必须凑齐这些参考片，否则拒绝写回（防「半份基线覆盖好基线」）
EXPECTED = (
    "unbroken-15s", "one-dot-15s", "knockon-15s", "motion-web-15s", "clearing-15s",
    "pith-21s", "onetake-launch-30s", "overlap-15s", "skill-demo-15s", "ebb-15s",
    "infra-25s", "domino-25s", "clawd-desktop-19s",
)


def run(cmd, cwd=None):
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                          encoding="utf-8", errors="replace")


def probe(mp4):
    r = run(["ffprobe", "-v", "error", "-select_streams", "v:0",
             "-show_entries", "stream=width,height,r_frame_rate,nb_frames",
             "-show_entries", "format=duration", "-of", "json", mp4])
    try:
        j = json.loads(r.stdout)
        st = (j.get("streams") or [{}])[0]
        num, den = (st.get("r_frame_rate") or "30/1").split("/")
        return {"width": st.get("width"), "height": st.get("height"),
                "fps": round(float(num) / max(float(den), 1e-9), 3),
                "duration": round(float(j.get("format", {}).get("duration") or 0), 3)}
    except Exception:
        return {}


def parse_motion(text):
    out = {}
    for key, pattern in (
        ("carry_ratio_mean", r"沿用率\s*均值\s*([\d.]+)"),
        ("change_mean", r"变化强度\s*均值\s*([\d.]+)"),
        ("change_median", r"中位数\s*([\d.]+)"),
        ("change_p95", r"p95\s*([\d.]+)"),
        ("change_peak", r"峰值\s*([\d.]+)"),
    ):
        m = re.search(pattern, text)
        if m:
            out[key] = float(m.group(1))
    m = re.search(r"有效变化\s*(\d+)\s*/\s*(\d+)\s*=\s*([\d.]+)%", text)
    if m:
        out.update({"moving_pairs": int(m.group(1)), "pairs": int(m.group(2)),
                    "moving_pct": float(m.group(3))})
    m = re.search(r"采样间隔\s*([\d.]+)\s*ms", text)
    if m:
        out["interval_ms"] = float(m.group(1))
    return out


def fetch_frames(name, frames):
    """帧缺失时从 onetake 的 mp4 补抽。返回 True 表示成功。"""
    case = os.path.join(ONETAKE, name)
    if not os.path.isdir(case):
        return False
    mp4s = [f for f in os.listdir(case) if f.endswith(".mp4")]
    if not mp4s:
        return False
    os.makedirs(frames, exist_ok=True)
    run(["ffmpeg", "-y", "-v", "error", "-i", os.path.join(case, mp4s[0]),
         "-vf", "scale=320:180", os.path.join(frames, "f_%05d.png")])
    return bool(os.listdir(frames))


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    refetch = "--refetch" in sys.argv
    os.makedirs(OUT, exist_ok=True)

    # 以「帧目录」为准枚举：这是真值源，mp4 只在缺失时才是必需品
    names = sorted(d for d in os.listdir(OUT) if os.path.isdir(os.path.join(OUT, d)))
    if not names:
        print("✗ out/otbench/_ref 下没有任何帧目录——先跑一次带 --refetch 的建库")
        return 1

    table, missing = {}, []
    for name in names:
        frames = os.path.join(OUT, name, "frames")
        if refetch or not (os.path.isdir(frames) and os.listdir(frames)):
            if not fetch_frames(name, frames):
                missing.append(name)
                continue
        if not (os.path.isdir(frames) and os.listdir(frames)):
            missing.append(name)
            continue

        # 元信息：能探到 mp4 就用真值，探不到就从帧数 + 帧率推断（两者都要写进基线，
        # 否则 fps 缺失会让 motion_qa 的采样间隔算错、进而把强度比成假数）。
        info = {}
        case = os.path.join(ONETAKE, name)
        mp4s = [f for f in os.listdir(case) if f.endswith(".mp4")] if os.path.isdir(case) else []
        if mp4s:
            info = probe(os.path.join(case, mp4s[0]))
        n = len(os.listdir(frames))
        info.setdefault("fps", 30.0)
        info.setdefault("width", 320 * 6)
        info.setdefault("height", 180 * 6)
        info.setdefault("duration", round(n / max(info["fps"], 1e-9), 3))

        text = run([MOTION_QA, "--dir", frames, "--fps", str(info["fps"])]).stdout
        entry = dict(info, **parse_motion(text))
        # 与 ot_bench.pixel_still **同一口径**量参考片（静止/过亮/帧差统计）。
        # 不写这一段，「过亮」判据就拿不到参考值，于是静默不判——
        # 实测踩过：此字段从未进过基线，而 13 条片子的报告看上去全是 PASS。
        entry.update({k: v for k, v in pixel_still(frames).items() if k != "frames"})
        table[name] = entry
        print(f"{name:20s} {info['fps']:>5} fps {info['duration']:>6.2f}s  "
              f"静止 {entry.get('still_ratio','?'):>5}% 过亮 {entry.get('bright_ratio','?'):>5}%  "
              f"变化均值 {entry.get('change_mean','?')} 中位 {entry.get('change_median','?')} "
              f"p95 {entry.get('change_p95','?')}")

    # 覆盖率校验：宁可失败，也不能用半份数据覆盖一份好基线
    if missing:
        print(f"\n✗ 缺 {len(missing)} 条的帧：{missing}")
        print("  这些片子若当年没抽过帧、onetake 源 mp4 也已不在，就无法补——"
              "拒绝写回基线（否则对应判据会静默不判）。")
        return 1

    with open(BASELINE, "w", encoding="utf-8") as fh:
        json.dump(table, fh, ensure_ascii=False, indent=2)
    print(f"\n基准确认：{len(table)} 条 → {os.path.relpath(BASELINE, ROOT)}")
    lack = [n for n in EXPECTED if n not in table]
    if lack:
        print(f"⚠ 期望清单里还差：{lack}（判据会对这些片子跳过）")
    return 0


if __name__ == "__main__":
    sys.exit(main())

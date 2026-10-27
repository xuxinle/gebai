"""把 examples/caiyun/assets 下的示例脚本逐个渲一遍并量画面质量，产出资产清单的原始数据。

## 为什么要这一步

"库里有示例"与"库里有**验证过的**示例"是两件事。没有量过就进库的样例，
模型会把它当成正面参照——而它可能静止 90%、可能过亮、可能是旧版字段。
所以入库前的门槛是：**能渲 + 有量化结论 + 能渲出抽帧**，三者齐了才标 verified。

用法：
    python tools/caiyun_audit_assets.py --out <会话目录>/audit
输出：<out>/assets-audit.json（每项含 exit / 静止占比 / 变化密度 / 亮度 / 抽帧数）
"""

import argparse
import io
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "dev", "bin",
                   "caiyun.exe" if os.name == "nt" else "caiyun")
ASSETS = os.path.join(ROOT, "examples", "caiyun", "assets")
STYLES = os.path.join(ROOT, "examples", "caiyun", "styles")


def run_one(script: str, outdir: str, frames: int = 0) -> dict:
    """渲一个脚本并返回量化结果。"""
    os.makedirs(outdir, exist_ok=True)
    mp4 = os.path.join(outdir, "film.mp4")
    cmd = [EXE, "--headless", "--strict", "--script", script,
           "--render", mp4, "--keep-frames", "--render-frames", str(frames)]
    proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                          errors="replace", timeout=900)
    text = (proc.stdout or "") + (proc.stderr or "")

    result = {
        "script": os.path.relpath(script, ROOT).replace("\\", "/"),
        "exit": proc.returncode,
        "ok": proc.returncode == 0 and os.path.exists(mp4),
    }

    # 质检摘要行（frame_stats.cpp 的 out.summary）
    def pick(pattern: str) -> float | None:
        m = re.search(pattern, text)
        return float(m.group(1)) if m else None

    result["still_ratio"] = pick(r"静止\s*([\d.]+)%")
    result["change_density"] = pick(r"变化\s*([\d.]+)/s")
    result["bright_ratio"] = pick(r"过亮\s*([\d.]+)%")
    result["mean_luma"] = pick(r"平均亮度\s*([\d.]+)")

    check = os.path.join(outdir, "check")
    result["shots"] = len([f for f in os.listdir(check)]) if os.path.isdir(check) else 0
    # 帧序列（渲染器固定落在 <成片目录>/frames）
    fdir = os.path.join(outdir, "frames")
    result["frames"] = len(os.listdir(fdir)) if os.path.isdir(fdir) else 0

    if proc.returncode != 0:
        # 严格模式的诊断优先（比整段输出清楚）
        at = text.find("[{")
        result["error"] = text[at:at + 600] if at >= 0 else text[-600:]
    return result


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, help="产物目录（放会话目录，不进仓）")
    ap.add_argument("--only", default="", help="只测名字含该子串的")
    ap.add_argument("--frames", type=int, default=0, help="只渲前 N 帧（0=全片）")
    args = ap.parse_args()

    if not os.path.exists(EXE):
        print(f"找不到裁云：{EXE}\n先 shuangtian_run(action=build, target=caiyun)")
        return 2

    targets: list[tuple[str, str]] = []
    if os.path.isdir(ASSETS):
        for f in sorted(os.listdir(ASSETS)):
            if f.endswith(".json"):
                targets.append((f[:-5], os.path.join(ASSETS, f)))
    if os.path.isdir(STYLES):
        for d in sorted(os.listdir(STYLES)):
            p = os.path.join(STYLES, d)
            if os.path.isdir(p):
                for f in sorted(os.listdir(p)):
                    if f.endswith(".json"):
                        targets.append((f"{d}/{f[:-5]}", os.path.join(p, f)))

    if args.only:
        targets = [t for t in targets if args.only in t[0]]

    print(f"待测 {len(targets)} 个\n")
    rows = []
    for name, path in targets:
        safe = name.replace("/", "__")
        row = run_one(path, os.path.join(args.out, safe), args.frames)
        rows.append(row)
        mark = "OK  " if row["ok"] else "FAIL"
        still = f"{row['still_ratio']:.1f}%" if row["still_ratio"] is not None else "  -  "
        print(f"[{mark}] {name:42s} 静止 {still}  帧 {row['frames']}")

    os.makedirs(args.out, exist_ok=True)
    out_path = os.path.join(args.out, "assets-audit.json")
    with io.open(out_path, "w", encoding="utf-8") as fh:
        json.dump(rows, fh, ensure_ascii=False, indent=2)
    ok = sum(1 for r in rows if r["ok"])
    print(f"\n{ok}/{len(rows)} 渲染通过 → {out_path}")
    return 0 if ok == len(rows) else 1


if __name__ == "__main__":
    sys.exit(main())

"""度量七份多镜示例的承接分与在动帧比，产出登记用的实测数据。

## 为什么要单独一份（不复用 caiyun_audit_assets.py）

多镜示例的验收**不能只看像素**：`still_ratio` 会被面积加权稀释（实测一条占全屏
0.09% 的光带横穿，全屏平均差只有 0.18，远低于判据 1.2），于是"元素每帧都在动"
的镜头会被报成 96% 静止。所以本脚本以**元素轨迹**为主口径，像素为辅：

| 量 | 来源 | 用途 |
|---|---|---|
| `carry` | `carry_qa.py --strict` | 承接分，唯一判"像不像 PPT"的指标 |
| `moving` | 渲染输出的「在动帧对」 | 像素口径，只作粗筛 |
| `traj_still` | 自己算的轨迹逐镜静止比 | 真值：元素到底有没有在动 |

## 为何要自己算 traj_still

`carry_qa.py` 只判边界，不判幕内。而"幕内有没有卡住"正是这次踩到的坑：
像素口径报 96% 静止，轨迹口径报 0%。自己算一遍才能把这两者摆在一起。

用法：`python tools/caiyun_audit_multishot.py --out <会话目录>/multishot`
"""

import argparse
import collections
import csv
import io
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "dev", "bin",
                   "caiyun.exe" if os.name == "nt" else "caiyun")
STYLES = os.path.join(ROOT, "examples", "caiyun", "styles")


def trajectory_still_ratio(path: str) -> float | None:
    """用元素轨迹真值算「逐帧对里有多少帧什么都没变」。

    判据：任一元素的位置/尺寸变化 > 0.5px，或 alpha 变化 > 0.005，即算"在动"。
    这比像素判据严格得多——它不受元素面积影响。
    """
    if not os.path.exists(path):
        return None
    by_frame: dict[int, dict[str, tuple]] = collections.defaultdict(dict)
    with io.open(path, encoding="utf-8") as fh:
        for row in csv.DictReader(fh, delimiter="\t"):
            by_frame[int(row["frame"])][row["name"]] = (
                float(row["x"]), float(row["y"]), float(row["w"]),
                float(row["h"]), float(row["alpha"]),
            )
    frames = sorted(by_frame)
    still = moved = 0
    for i in range(1, len(frames)):
        if frames[i] != frames[i - 1] + 1:
            continue
        a, b = by_frame[frames[i - 1]], by_frame[frames[i]]
        changed = False
        for name, pb in b.items():
            pa = a.get(name)
            if pa is None:
                changed = True          # 新元素出现也算在动
                break
            if (abs(pa[0] - pb[0]) > 0.5 or abs(pa[1] - pb[1]) > 0.5
                    or abs(pa[2] - pb[2]) > 0.5 or abs(pa[3] - pb[3]) > 0.5
                    or abs(pa[4] - pb[4]) > 0.005):
                changed = True
                break
        if changed:
            moved += 1
        else:
            still += 1
    total = still + moved
    return (100.0 * still / total) if total else None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--only", default="")
    args = ap.parse_args()

    if not os.path.exists(EXE):
        print(f"找不到裁云：{EXE}")
        return 2

    rows = []
    for name in sorted(os.listdir(STYLES)):
        script = os.path.join(STYLES, name, "film-multishot.json")
        if not os.path.exists(script):
            continue
        if args.only and args.only not in name:
            continue
        outdir = os.path.join(args.out, name)
        os.makedirs(outdir, exist_ok=True)

        doc = json.load(io.open(script, encoding="utf-8"))
        durations = [s["duration"] for s in doc["scenes"]]
        scenes_arg = ",".join(f"{d:g}" for d in durations)

        proc = subprocess.run(
            [EXE, "--headless", "--strict", "--script", script,
             "--render", os.path.join(outdir, "f.mp4"), "--track-elements"],
            capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=900,
        )
        if proc.returncode != 0:
            print(f"[FAIL] {name}：退出码 {proc.returncode}")
            rows.append({"style": name, "ok": False})
            continue

        text = proc.stdout + proc.stderr
        m_moving = re.search(r"在动帧对\s*([\d.]+)%", text)
        m_still = re.search(r"静止\s*([\d.]+)%", text)

        traj = os.path.join(outdir, "trajectory.tsv")
        carry = None
        try:
            qa = subprocess.run(
                [sys.executable, os.path.join(ROOT, "scripts", "carry_qa.py"), traj,
                 "--scenes", scenes_arg, "--strict"],
                capture_output=True, text=True, encoding="utf-8", errors="replace",
                timeout=300,
            )
            mc = re.search(r"carry score\s*([\d.]+)", qa.stdout)
            carry = float(mc.group(1)) if mc else None
            passed = "PASS" in qa.stdout
        except Exception:
            passed = False

        row = {
            "style": name,
            "ok": True,
            "carry": carry,
            "pass": passed,
            "moving_pixel_pct": float(m_moving.group(1)) if m_moving else None,
            "still_pixel_pct": float(m_still.group(1)) if m_still else None,
            "still_traj_pct": trajectory_still_ratio(traj),
        }
        rows.append(row)
        print(f"[OK] {name:22s} carry={carry}  在动(像素)={row['moving_pixel_pct']}%  "
              f"静止(像素)={row['still_pixel_pct']}%  静止(轨迹)={row['still_traj_pct']:.1f}%")

    os.makedirs(args.out, exist_ok=True)
    with io.open(os.path.join(args.out, "multishot-audit.json"), "w",
                 encoding="utf-8", newline="\n") as fh:
        json.dump(rows, fh, ensure_ascii=False, indent=2)
        fh.write("\n")
    ok = sum(1 for r in rows if r.get("pass"))
    print(f"\n{ok}/{len(rows)} 承接分 PASS")
    return 0 if ok == len(rows) else 1


if __name__ == "__main__":
    sys.exit(main())

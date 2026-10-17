#!/usr/bin/env python3
"""X11 上屏路径的**干净**对照测量：单段+每帧 XSync（基线） vs 双段+完成事件（本树）。

为什么单独一个脚本（而不是手工敲三条命令）：
  - 每个数据点都要**前后各记一次 uptime** 并记录"测量期间有没有并行构建/测试"——
    负载不可比时数字不算数（`CONVENTIONS` §9.0③：并行构建会伪造读数）；
  - 两组数字必须**同机同参**：同 DISPLAY、同 `--scale 1`、同 `--bench 200`、
    同 backend=x11；两组交替跑（A/B/A/B）以抵消机器随时间漂移。

产出：JSON + 一张 markdown 表（贴进 docs/BACKLOG.md 用）。

用法：
    python3 tools/x11_present_bench.py --baseline /home/x/code/gebai-baseline-x11/shuangtian \
        --current . [--rounds 2] [--frames 200]
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import subprocess
import sys
import time


def uptime() -> str:
    try:
        return pathlib.Path("/proc/loadavg").read_text(encoding="utf-8").strip()
    except OSError:
        return "n/a"


def busy_processes() -> list[str]:
    """测量期间会伪造读数的进程：编译器与并行构建/测试。

    判据必须**精确**（踩过的坑：第一版用 `"ld " in line` 子串判匹配，把内核工作线程
    `kworker/R-mld` 当成了链接器——于是本来干净的一组数字被标成“作废”。反向的错
    （漏掉真构建）更危险，所以两侧都收紧）：① 进程名**精确**匹配编译器/链接器，
    ② 命令行里 `st build` / `st test` 按**完整词**匹配，不猜子串。
    """
    try:
        out = subprocess.run(["ps", "-eo", "comm=,args="], capture_output=True, text=True,
                             timeout=20).stdout
    except (OSError, subprocess.SubprocessError):
        return ["ps 不可用"]
    compilers = {"cc1plus", "cc1", "collect2", "ld", "ld.bfd", "ld.gold", "lld",
                 "clang", "clang++", "gcc", "g++"}
    hits = []
    for line in out.splitlines():
        parts = line.strip().split(None, 1)
        if not parts:
            continue
        comm = parts[0].rsplit("/", 1)[-1]
        args = parts[1] if len(parts) > 1 else ""
        # 按空白分词后逐个比**词干**（`/usr/bin/g++` → `g++`），避免子串误命中
        words = {word.rsplit("/", 1)[-1] for word in args.split()}
        if comm in compilers or (words & compilers):
            hits.append(line.strip()[:120])
        elif re.search(r"(?:^|\s)st\s+(?:build|test)(?:\s|$)", args) or "st_tests" in args:
            hits.append(line.strip()[:120])
    return hits


def run_bench(exe: pathlib.Path, width: int, height: int, frames: int) -> dict:
    """跑一次 `--bench`，把四行分位耗时解析成数。"""
    env = dict(os.environ)
    env["DISPLAY"] = env.get("DISPLAY", ":0")
    proc = subprocess.run([str(exe), "--backend", "x11", "--scale", "1", "--width", str(width),
                           "--height", str(height), "--bench", str(frames)],
                          capture_output=True, text=True, timeout=1800, env=env,
                          cwd=str(exe.parent))
    text = proc.stdout + proc.stderr
    if proc.returncode != 0:
        raise RuntimeError(f"{exe} 运行失败（{proc.returncode}）：{text[-1200:]}")
    rows: dict[str, dict[str, float]] = {}
    # 形如：  总帧     p50  11.75 ms · p95  12.10 ms · 均  11.84 ms · 最大   15.59 ms
    pattern = re.compile(r"^\s*(总帧|排版|绘制|送显)\s+p50\s+([\d.]+) ms · p95\s+([\d.]+) ms · "
                         r"均\s+([\d.]+) ms · 最大\s+([\d.]+) ms", re.M)
    for match in pattern.finditer(text):
        rows[match.group(1)] = {"p50": float(match.group(2)), "p95": float(match.group(3)),
                                "mean": float(match.group(4)), "max": float(match.group(5))}
    missing = {"总帧", "排版", "绘制", "送显"} - set(rows)
    if missing:
        raise RuntimeError(f"没解析到这些行：{missing}（{text[-800:]}）")
    shm = re.search(r"x11 上屏走 MIT-SHM（[^）]*）", text)
    rows["_present_path"] = shm.group(0) if shm else "未走 MIT-SHM"
    return rows


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", required=True, help="基线树（单段 + 每帧 XSync）的工程根")
    parser.add_argument("--current", default=".", help="本树（双段 + 完成事件）的工程根")
    parser.add_argument("--rounds", type=int, default=2, help="每档跑几轮（取中位）")
    parser.add_argument("--frames", type=int, default=200)
    parser.add_argument("--warmup", type=int, default=60,
                        help="热身帧数：测量前先跑一次并**丢弃**（首跑要付字体/页缓存/驱动\n"
                             "首次触碰的冷代价——实测同一二进制首次跑 总帧 p95 会到 55 ms，\n"
                             "第二跑就回到 12 ms。不热身就会把冷启动当成稳态。")
    parser.add_argument("--out", default="build/x11-present-bench.json")
    args = parser.parse_args()

    baseline_exe = pathlib.Path(args.baseline).resolve() / "build/dev/bin/gallery"
    current_exe = pathlib.Path(args.current).resolve() / "build/dev/bin/gallery"
    for name, exe in (("基线", baseline_exe), ("本树", current_exe)):
        if not exe.exists():
            print(f"缺 {name} 可执行文件：{exe}（先在那棵树 st build gallery）")
            return 2

    sizes = [(1280, 720), (1920, 1080)]
    result: dict = {"frames": args.frames, "sizes": {}, "load": [], "busy": []}

    print(f"=== 测量前 uptime（loadavg）: {uptime()}")
    # 热身：两棵树各先跑一次小帧数并丢弃（字体/页缓存/首次触碰的冷代价不计入稳态）
    if args.warmup > 0:
        for label, exe in (("baseline", baseline_exe), ("current", current_exe)):
            print(f"  热身 {label}（{args.warmup} 帧，丢弃）…")
            run_bench(exe, 1280, 720, args.warmup)
            run_bench(exe, 1920, 1080, args.warmup)
    for width, height in sizes:
        key = f"{width}x{height}"
        bucket: dict[str, list] = {"baseline": [], "current": []}
        for round_index in range(args.rounds):
            # A/B 交替：机器随时间的漂移对两组同等施加
            for label, exe in (("baseline", baseline_exe), ("current", current_exe)):
                before = uptime()
                busy_before = busy_processes()
                rows = run_bench(exe, width, height, args.frames)
                after = uptime()
                busy_after = busy_processes()
                bucket[label].append(rows)
                result["load"].append({"size": key, "round": round_index, "variant": label,
                                       "before": before, "after": after})
                if busy_before or busy_after:
                    result["busy"].append({"size": key, "round": round_index, "variant": label,
                                           "before": busy_before, "after": busy_after})
                print(f"  [{key} r{round_index + 1}] {label:8} 送显 p50 {rows['送显']['p50']:6.2f}"
                      f" · 总帧 p50 {rows['总帧']['p50']:6.2f} | load {before.split()[0]} → "
                      f"{after.split()[0]}")
        result["sizes"][key] = {
            label: {
                metric: {stat: sorted(row[metric][stat] for row in bucket[label])[
                    len(bucket[label]) // 2] for stat in ("p50", "p95", "mean")}
                for metric in ("总帧", "排版", "绘制", "送显")
            } for label in ("baseline", "current")
        }
    print(f"=== 测量后 uptime（loadavg）: {uptime()}")

    out = pathlib.Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")

    print("\n| 场景 | 总帧 p50 / p95 | 绘制 p50 / p95 | 送显 p50 / p95（单段+每帧 XSync）"
          " | 送显 p50 / p95（双段+完成事件） | 送显降幅 |")
    print("|---|---|---|---|---|---|")
    for key, rows in result["sizes"].items():
        base, curr = rows["baseline"], rows["current"]
        ratio = base["送显"]["p50"] / curr["送显"]["p50"] if curr["送显"]["p50"] > 0 else 0.0
        print(f"| x11 {key} | {curr['总帧']['p50']:.2f} / {curr['总帧']['p95']:.2f} ms | "
              f"{curr['绘制']['p50']:.2f} / {curr['绘制']['p95']:.2f} ms | "
              f"{base['送显']['p50']:.2f} / {base['送显']['p95']:.2f} ms | "
              f"**{curr['送显']['p50']:.2f} / {curr['送显']['p95']:.2f} ms** | {ratio:.1f}× |")
    print(f"\n明细 JSON: {out}")
    if result["busy"]:
        print(f"\n⚠ 测量期间检测到 {len(result['busy'])} 次并行构建/测试——这组数字**作废**：")
        for item in result["busy"][:5]:
            print(f"  {item}")
        return 1
    print("✓ 测量期间（前后各一次采样）没有并行构建/测试在跑：数字可用")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""测 GPU 真实占用率：边渲染边采样 Windows 性能计数器。

## 为什么必须实测占用率

"耗时短"不等于"跑满 GPU"。GPU 可能在两帧之间闲着——CPU 在算布局、
上传纹理、等回读。要判断还有多少性能余量，必须看 `3D` 引擎的利用率。

## 数据来源

Windows 的 `GPU Engine` 计数器（按进程 / 引擎类型细分）。
`engtype_3D` 是 3D 渲染引擎，`engtype_Copy` 是 DMA 拷贝，
`engtype_Compute` 是计算着色器。三维光栅化应该压倒性地吃 `3D`。

用法：py tools/gpu_utilization.py <脚本> [--size 1920x1080] [--frames N]
"""
import argparse
import os
import re
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CAIYUN = os.path.join(ROOT, "build", "dev", "bin", "caiyun.exe")


def sample_gpu(pid, stop, samples):
    """持续采样指定 pid 的 GPU 引擎利用率（百分比）。

    ## 为什么要轮询等计数器出现

    `GPU Engine` 的实例是按需注册的：进程还没提交过任何 GPU 工作时，
    计数器**根本不存在**。第一版直接查一次就返回，于是永远拿不到数据
    （表现为"计数器无数据"，看起来像进程没用 GPU——实际是查早了）。
    """
    counters = []
    for _ in range(100):   # 最多等 10 秒
        if stop.is_set():
            return
        try:
            out = subprocess.run(
                ["typeperf", "-qx", "GPU Engine"],
                capture_output=True,
            ).stdout.decode("utf-8", "replace")
        except Exception:
            return
        pattern = re.compile(r"\\(GPU Engine\(pid_%d_[^)]*\)\\Utilization Percentage)" % pid)
        counters = pattern.findall(out)
        if counters:
            break
        time.sleep(0.1)
    if not counters:
        return

    args = ["typeperf"] + ["\\" + c for c in counters] + ["-si", "1", "-sc", "60"]
    try:
        proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    except Exception:
        return

    engines = [re.search(r"engtype_(\w+)", c).group(1) for c in counters]
    for raw in proc.stdout:
        if stop.is_set():
            proc.kill()
            break
        line = raw.decode("utf-8", "replace").strip()
        if not line or line.startswith("(") or line.startswith("Exiting"):
            continue
        parts = line.split(",")
        if len(parts) < 2:
            continue
        values = {}
        for name, cell in zip(engines, parts[1:]):
            try:
                v = float(cell.strip('"'))
            except ValueError:
                continue
            values[name] = values.get(name, 0.0) + v
        if values:
            samples.append(values)
    try:
        proc.wait(timeout=5)
    except Exception:
        proc.kill()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("script")
    ap.add_argument("--size", default="1920x1080")
    ap.add_argument("--frames", default="60")
    ap.add_argument("--gpu", default="1", help="CAIYUN_GPU（1=强制开，0=关）")
    args = ap.parse_args()

    env = dict(os.environ)
    env["CAIYUN_GPU"] = args.gpu
    env["CAIYUN_WORKERS"] = env.get("CAIYUN_WORKERS", "1")

    proc = subprocess.Popen(
        [CAIYUN, "--headless", "--script", args.script,
         "--render", os.path.join(ROOT, "tmp", "gpu_probe.mp4"),
         "--render-size", args.size, "--render-frames", args.frames],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env,
    )

    stop = threading.Event()
    samples = []
    sampler = threading.Thread(target=sample_gpu, args=(proc.pid, stop, samples), daemon=True)
    sampler.start()

    out, _ = proc.communicate()
    stop.set()
    sampler.join(timeout=5)

    text = (out or b"").decode("utf-8", "replace")
    m = re.search(r"渲染结果：\w+（(\d+) 帧 / ([\d.]+)s）", text)
    total = f"{m.group(1)} 帧 / {m.group(2)}s" if m else "?"
    per_frame = (float(m.group(2)) / float(m.group(1)) * 1000) if m else 0.0

    print(f"渲染：{total}  ({per_frame:.1f} ms/帧)")

    if not samples:
        print("GPU 计数器无数据（进程可能未用 GPU，或采样太晚）")
        return

    # 汇总各引擎的平均/峰值
    agg = {}
    for s in samples:
        for k, v in s.items():
            a = agg.setdefault(k, [0.0, 0.0])
            a[0] += v
            a[1] = max(a[1], v)
    print(f"\nGPU 引擎利用率（{len(samples)} 个采样点）：")
    for k in sorted(agg, key=lambda x: -agg[x][0]):
        avg = agg[k][0] / len(samples)
        peak = agg[k][1]
        if avg < 0.1 and peak < 0.1:
            continue
        print(f"  {k:<20} 平均 {avg:6.2f}%   峰值 {peak:6.2f}%")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""进程内存/线程采样（Linux /proc；Windows 走 psutil 或 PowerShell 对照）。

用途：霜天框架层内存指标的**可复跑基线**。评估报告记录过最小应用 176MB、
其中字体整文件驻留 ~86MB——本脚本给出可一条命令复核的数字。

    python3 tools/mem_sample.py --exe build/dev/bin/gallery -- \
        --headless --control-port 0 --frames 100000 --renderer software

口径与报告一致：**RSS/Private 取峰值**（字体加载后驻留），线程数取峰值。
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time


def read_status(pid: int) -> dict[str, int] | None:
    try:
        with open(f"/proc/{pid}/status", encoding="utf-8") as handle:
            text = handle.read()
    except OSError:
        return None
    out: dict[str, int] = {}
    for line in text.splitlines():
        key, _, rest = line.partition(":")
        if key in ("VmRSS", "VmHWM", "VmSize", "VmData"):
            digits = rest.strip().split()[0]
            out[key] = int(digits)          # kB
        elif key == "Threads":
            out[key] = int(rest.strip())
    return out


def sample(cmd: list[str], hold_seconds: float, interval: float) -> int:
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    peak: dict[str, int] = {}
    try:
        deadline = time.monotonic() + hold_seconds
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                break
            status = read_status(proc.pid)
            if status is not None:
                for key, value in status.items():
                    peak[key] = max(peak.get(key, 0), value)
            time.sleep(interval)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    if not peak:
        print("采样失败：进程未启动或立即退出", file=sys.stderr)
        return 1
    mib = lambda kb: kb / 1024.0        # noqa: E731
    print(f"命令: {' '.join(cmd)}")
    print(f"  RSS 峰值      : {mib(peak.get('VmRSS', 0)):8.1f} MiB")
    print(f"  RSS 高水位    : {mib(peak.get('VmHWM', 0)):8.1f} MiB")
    print(f"  虚拟内存峰值  : {mib(peak.get('VmSize', 0)):8.1f} MiB")
    print(f"  线程数峰值    : {peak.get('Threads', 0)}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True)
    parser.add_argument("--hold", type=float, default=3.0, help="采样时长（秒）")
    parser.add_argument("--interval", type=float, default=0.05)
    parser.add_argument("args", nargs=argparse.REMAINDER)
    parsed = parser.parse_args()
    extra = [item for item in parsed.args if item != "--"]
    exe = os.path.abspath(parsed.exe)
    return sample([exe, *extra], parsed.hold, parsed.interval)


if __name__ == "__main__":
    raise SystemExit(main())

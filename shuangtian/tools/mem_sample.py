#!/usr/bin/env python3
"""进程内存/线程采样（Linux /proc；Windows 走 Win32 API——ctypes，不起子进程）。

用途：霜天框架层内存指标的**可复跑基线**。评估报告记录过最小应用 176MB、
其中字体整文件驻留 ~86MB——本脚本给出可一条命令复核的数字。

    python3 tools/mem_sample.py --exe build/dev/bin/gallery -- \
        --headless --control-port 0 --frames 100000 --renderer software

口径与报告一致：**RSS/Private 取峰值**（字体加载后驻留），线程数取峰值。
两平台键映射：Linux `VmRSS`/`VmHWM`/`VmSize`/`Threads`；
Windows `WorkingSetSize`/`PeakWorkingSetSize`/`PrivateUsage`（EX 结构）/Toolhelp32 线程计数。
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time

IS_WINDOWS = os.name == "nt"


def read_status(pid: int) -> dict[str, int] | None:
    if IS_WINDOWS:
        return read_status_windows(pid)
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


# ————————————————————————————————————————————————————————————————————————————
# Windows 侧：ctypes 直调 Win32（不用 PowerShell 子进程——每 50ms 起一次
# powershell.exe 要 200ms+ 启动，采样密度不够；也不用 psutil——避免第三方依赖）。
# ————————————————————————————————————————————————————————————————————————————

if IS_WINDOWS:
    import ctypes
    from ctypes import wintypes

    class _MemoryCountersEx(ctypes.Structure):
        """PROCESS_MEMORY_COUNTERS_EX：EX 尾部多 PrivateUsage（私有内存，报告口径之一）。"""
        _fields_ = [
            ("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
            ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
            ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
            ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t),
            ("PrivateUsage", ctypes.c_size_t),
        ]

    class _ThreadEntry32(ctypes.Structure):
        _fields_ = [
            ("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD),
            ("th32ThreadID", wintypes.DWORD), ("th32OwnerProcessID", wintypes.DWORD),
            ("tpBasePri", ctypes.c_long), ("tpDeltaPri", ctypes.c_long), ("dwFlags", wintypes.DWORD),
        ]

    _PROCESS_QUERY_INFORMATION = 0x0400
    _PROCESS_VM_READ = 0x0010
    _TH32CS_SNAPTHREAD = 0x00000004

    def _thread_count(kernel32: ctypes.WinDLL, pid: int) -> int:
        """Toolhelp32 线程快照里属于本进程的线程数（与 /proc Threads 同口径）。"""
        snapshot = kernel32.CreateToolhelp32Snapshot(_TH32CS_SNAPTHREAD, 0)
        if not snapshot:
            return 0
        try:
            entry = _ThreadEntry32()
            entry.dwSize = ctypes.sizeof(entry)
            count = 0
            if kernel32.Thread32First(snapshot, ctypes.byref(entry)):
                while True:
                    if entry.th32OwnerProcessID == pid:
                        count += 1
                    if not kernel32.Thread32Next(snapshot, ctypes.byref(entry)):
                        break
            return count
        finally:
            kernel32.CloseHandle(snapshot)

    def read_status_windows(pid: int) -> dict[str, int] | None:
        kernel32 = ctypes.windll.kernel32
        psapi = ctypes.windll.psapi
        handle = kernel32.OpenProcess(_PROCESS_QUERY_INFORMATION | _PROCESS_VM_READ, False, pid)
        if not handle:
            return None
        try:
            counters = _MemoryCountersEx()
            counters.cb = ctypes.sizeof(counters)
            if not psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
                return None
            return {
                "VmRSS": counters.WorkingSetSize // 1024,       # kB（与 /proc 同单位）
                "VmHWM": counters.PeakWorkingSetSize // 1024,
                "Private": counters.PrivateUsage // 1024,
                "Threads": _thread_count(kernel32, pid),
            }
        finally:
            kernel32.CloseHandle(handle)


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
    # 键按平台可得性打印（Windows 无 VmSize 对应物、Linux 无 Private——诚实缺省，不造数）。
    rows = [("RSS 峰值", "VmRSS"), ("RSS 高水位", "VmHWM"), ("私有内存峰值", "Private"),
            ("虚拟内存峰值", "VmSize"), ("线程数峰值", "Threads")]
    for label, key in rows:
        if key in peak:
            suffix = "" if key == "Threads" else " MiB"
            print(f"  {label:<12s}: {mib(peak[key]):8.1f}{suffix}" if key != "Threads"
                  else f"  {label:<12s}: {peak[key]}")
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

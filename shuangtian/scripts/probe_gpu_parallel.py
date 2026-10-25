"""GPU 渲染稳定性 / 并行曲线测量（含崩溃检测）。

为什么必须逐档测：
  16 路一次实测崩溃（退出码 0xC0000374 = 堆损坏），而 14 路是好的。
  崩溃点不是"大概在哪"，而是"哪一档会炸"——所以每档都要独立进程 + 退出码检查。
"""
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build/dev/bin/caiyun.exe")
SCRIPT = os.path.join(ROOT, "out/otbench/c05-clearing/clearing.json")

results = []
for w in [4, 6, 8, 10, 12, 14, 16]:
    env = dict(os.environ)
    env["CAIYUN_WORKERS"] = str(w)
    out = os.path.join(ROOT, f"out/audit/stab{w}.mp4")
    t0 = time.time()
    status = "OK"
    try:
        r = subprocess.run([EXE, "--headless", "--script", SCRIPT, "--render", out,
                            "--motion-blur", "4"],
                           env=env, capture_output=True, timeout=240)
        if r.returncode != 0:
            status = f"CRASH(0x{r.returncode & 0xFFFFFFFF:08X})"
    except subprocess.TimeoutExpired:
        status = "TIMEOUT"
    dt = time.time() - t0
    results.append((w, dt, status))
    print(f"  {w:2d} 路: {dt:6.2f}s  {status}", flush=True)

print("\n结论：")
ok = [r for r in results if r[2] == "OK"]
if ok:
    best = min(ok, key=lambda r: r[1])
    print(f"  最快可用档：{best[0]} 路 / {best[1]:.2f}s")
bad = [r for r in results if r[2] != "OK"]
if bad:
    print(f"  不可用档：{[(r[0], r[2]) for r in bad]}")

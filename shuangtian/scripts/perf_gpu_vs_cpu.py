"""GPU / 软件两条腿的导出耗时对比（含崩溃检测）。

为什么每档独立进程 + 退出码检查：多路 GPU 曾经崩溃（0xC0000005 / 0xC0000374），
而"成片出来了"不等于"进程正常结束"——收尾阶段崩也会留下完整 mp4。
"""
import os
import subprocess
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build/dev/bin/caiyun.exe")
SCRIPT = os.path.join(ROOT, "out/otbench/c05-clearing/clearing.json")


def run(workers, blur, tag, gpu):
    env = dict(os.environ)
    env["CAIYUN_WORKERS"] = str(workers)
    if gpu:
        env["CAIYUN_GPU"] = "1"
    else:
        env.pop("CAIYUN_GPU", None)
    out = os.path.join(ROOT, f"out/audit/perf_{tag}.mp4")
    t0 = time.time()
    status = "OK"
    try:
        r = subprocess.run([EXE, "--headless", "--script", SCRIPT, "--render", out,
                            "--motion-blur", str(blur)],
                           env=env, capture_output=True, timeout=600)
        if r.returncode != 0:
            status = "CRASH(0x%08X)" % (r.returncode & 0xFFFFFFFF)
    except subprocess.TimeoutExpired:
        status = "TIMEOUT"
    return time.time() - t0, status


print(f"{'配置':>26s} {'耗时':>9s}  状态")
print("-" * 50)
for gpu in (False, True):
    for workers in (4, 8, 16):
        tag = ("gpu" if gpu else "cpu") + f"_w{workers}"
        dt, status = run(workers, 4, tag, gpu)
        leg = "GPU" if gpu else "软件"
        print(f"{leg + ' / ' + str(workers) + ' 路 (blur=4)':>26s} {dt:8.2f}s  {status}",
              flush=True)

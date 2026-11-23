#!/usr/bin/env python3
"""量 `paint_ms`（改前/改后各跑一次），用于提交说明的性能对照。

为什么单独一个量尺：本次重构的价值**不在**性能（瓶颈在光栅化，不在主题重算），
但提交说明必须如实给出数据——“没变慢”要靠测量而不是直觉。

用法：
    python tools/measure_paint_ms.py <二进制路径> [--label=改前]
"""
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import st_client_lib  # noqa: E402

BIN = os.path.abspath(sys.argv[1])
LABEL = next((a.split("=", 1)[1] for a in sys.argv[2:] if a.startswith("--label=")), "")
TMP = os.environ.get("ST_VTMP", os.path.join(os.path.dirname(BIN), "..", "..", "perf-measure"))
os.makedirs(TMP, exist_ok=True)
CTL = os.path.join(TMP, "control.json")
LOG = os.path.join(TMP, "app.log")
SAMPLES = int(os.environ.get("ST_SAMPLES", "60"))


def main():
    import subprocess

    if os.path.exists(CTL):
        os.remove(CTL)
    log = open(LOG, "wb")
    process = subprocess.Popen(
        [BIN, "--headless", "--control-port", "0", "--control-file", CTL],
        stdout=log, stderr=log, stdin=subprocess.DEVNULL, start_new_session=True)
    port = token = None
    try:
        for _ in range(120):
            if os.path.exists(CTL):
                try:
                    info = json.load(open(CTL))
                    if info.get("port"):
                        port, token = info["port"], info.get("token", "")
                        break
                except Exception:
                    pass
            time.sleep(0.25)
        if port is None:
            print("控制通道没起来")
            return 1

        def call(method, params=None):
            return st_client_lib.call_with_token(port, method, params or {}, token)

        # 先跑几帧进入稳态（首帧永远是整帧，不代表持续开销）。
        for _ in range(5):
            call("metrics")
        values = []
        for _ in range(SAMPLES):
            payload = call("metrics").get("result") or {}
            if "paint_ms" in payload:
                values.append(float(payload["paint_ms"]))
        if not values:
            print("metrics 没有 paint_ms；keys =",
                  sorted((call("metrics").get("result") or {}).keys())[:20])
            return 1
        values.sort()
        median = values[len(values) // 2]
        p90 = values[min(len(values) - 1, int(len(values) * 0.9))]
        print(f"{LABEL or BIN}\n  paint_ms  n={len(values)}  median={median:.3f}  p90={p90:.3f}")
        return 0
    finally:
        try:
            process.kill()
            process.wait(timeout=10)
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""测量 gbcode 键盘回显延迟（端到端）。

## 为什么不用截图/像素判断

"回显慢"是**时延**问题，截图只能回答"对不对"，回答不了"多久"。这里用两个量：

1. **帧耗时**（`metrics.frame_p50_ms` / `p95`）：绘制一帧本身有多贵；
2. **按键到帧号增长**：敲一个键后，多久帧号才前进（= 用户感知的回显延迟）。

## 为什么用帧号而不是时间戳

帧号由应用自己递增，不受脚本侧轮询间隔影响——轮询快慢只影响分辨率，
不会把"没动"测成"动了"。

用法：
  python tools/gbcode_echo_latency.py --bin build/dev/bin/gbcode.exe --keystrokes 30
"""

from __future__ import annotations

import argparse
import json
import statistics
import sys
import time

sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parent))
from st_client_lib import call_with_token  # noqa: E402


def wait_port(control_file, timeout=30.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with open(control_file, encoding="utf-8") as f:
                info = json.load(f)
            port = info.get("port")
            token = info.get("token", "")
            if port and call_with_token(port, "ping", {}, token=token):
                return port, token
        except Exception:
            pass
        time.sleep(0.1)
    raise TimeoutError(f"控制通道未就绪：{control_file}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True, help="gbcode 可执行文件")
    ap.add_argument("--control-file", required=True)
    ap.add_argument("--keystrokes", type=int, default=30)
    ap.add_argument("--tag", default="?")
    args = ap.parse_args()

    port, token = wait_port(args.control_file)

    def call(method, params=None):
        return call_with_token(port, method, params or {}, token=token)

    # 固定尺寸（避免不同档位/不同 DPI 让两次测量不可比）
    call("invoke", {"id": "root", "action": "set", "argument": ""})
    time.sleep(1.0)

    metrics = call("metrics", {})
    print(
        f"[{args.tag}] backend={metrics.get('backend')} renderer={metrics.get('renderer')} "
        f"scale={metrics.get('device_scale')}",
        flush=True,
    )
    print(
        f"[{args.tag}] 基线 frame p50={metrics.get('frame_p50_ms')}ms "
        f"p95={metrics.get('frame_p95_ms')}ms frames={metrics.get('frames')}",
        flush=True,
    )

    # 聚焦编辑器：优先点编辑区（键盘输入要落到它上面）
    focused = None
    for selector in ("#editor", "CodeEditor", "#terminal"):
        found = call("find", {"selector": selector})
        matches = found.get("matches") or []
        if matches:
            focused = matches[0]
            break
    if focused is None:
        raise RuntimeError("找不到可聚焦的编辑控件")
    call("invoke", {"id": focused["id"], "action": "focus"})
    time.sleep(0.3)

    # 逐键测量：敲一个字符，看帧号多久前进
    latencies = []
    frame_before = call("metrics", {}).get("frames", 0)
    for i in range(args.keystrokes):
        before = call("metrics", {}).get("frames", 0)
        t0 = time.perf_counter()
        call("input", {"kind": "text", "text": "a"})
        # 轮询到帧号前进（分辨率 ~2ms）
        deadline = t0 + 2.0
        while time.perf_counter() < deadline:
            after = call("metrics", {}).get("frames", 0)
            if after > before:
                latencies.append((time.perf_counter() - t0) * 1000.0)
                break
            time.sleep(0.002)
        else:
            latencies.append(2000.0)  # 超时
        time.sleep(0.05)  # 让界面静下来，避免上一个键的余帧混进下一个

    frame_after = call("metrics", {}).get("frames", 0)
    metrics2 = call("metrics", {})
    latencies_sorted = sorted(latencies)
    n = len(latencies_sorted)
    print(
        f"[{args.tag}] 回显延迟 p50={statistics.median(latencies_sorted):.1f}ms "
        f"p95={latencies_sorted[int(n * 0.95)]:.1f}ms max={latencies_sorted[-1]:.1f}ms "
        f"(n={n})",
        flush=True,
    )
    print(
        f"[{args.tag}] 输入后 frame p50={metrics2.get('frame_p50_ms')}ms "
        f"p95={metrics2.get('frame_p95_ms')}ms 总帧数增量={frame_after - frame_before}",
        flush=True,
    )

    call("shutdown", {})


if __name__ == "__main__":
    main()

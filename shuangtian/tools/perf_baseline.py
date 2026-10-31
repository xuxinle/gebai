#!/usr/bin/env python3
"""性能基线：同一场景多次渲染，取中位数，避免噪声误导。

## 为什么要中位数而不是平均值

单帧耗时会被系统调度、GC、后台任务拉高。平均值被离群值拉偏，
而这些任务要比较的是"改动前后的真实差异"，一点噪声就能淹没收益。
中位数对离群值不敏感，是这里更可靠的指标。

用法：py tools/perf_baseline.py [标签]
"""
import json
import os
import re
import statistics
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CAIYUN = os.path.join(ROOT, "build", "dev", "bin", "caiyun.exe")
TMP = os.path.join(ROOT, "tmp")
os.makedirs(TMP, exist_ok=True)


def run_render(script, size, frames=None, repeat=3, env_extra=None):
    """渲染若干次，返回每帧毫秒的中位数。"""
    samples = []
    for _ in range(repeat):
        cmd = [CAIYUN, "--headless", "--script", script, "--render",
               os.path.join(tempfile.gettempdir(), "perf.mp4"), "--render-size", size]
        if frames:
            cmd += ["--render-frames", str(frames)]
        env = dict(os.environ)
        if env_extra:
            env.update(env_extra)
        # ## 必须按字节读再显式解码
        #
        # `text=True` 会用系统默认编码（GBK）解码，而 caiyun 输出 UTF-8，
        # 于是 `subprocess` 静默失败、stdout 变空——表现为"所有场景都失败"，
        # 排查起来很容易误以为是渲染本身挂了。
        result = subprocess.run(cmd, capture_output=True, env=env)
        raw = (result.stdout or b"") + (result.stderr or b"")
        text = raw.decode("utf-8", errors="replace")
        match = re.search(r"渲染 ([\d.]+)ms/帧", text)
        if match:
            samples.append(float(match.group(1)))
    if not samples:
        return None
    return statistics.median(samples)


def make_3d_script(path, objects, size_px):
    """造一个三维场景脚本。"""
    doc = {
        "name": "perf", "fps": 30, "size": size_px,
        "fade_in": 0, "fade_out": 0,
        "scenes": [{
            "kind": "custom", "duration": 1.0,
            "background": ["#101020", "#202040"],
            "layers": [{
                "kind": "model3d", "name": "perf",
                "x": "0%", "y": "0%", "w": "100%", "h": "100%",
                "model_script": {
                    "camera": {"eye": [0, 2.2, 7], "target": [0, 0.3, 0], "fov": 42},
                    "lighting": {"ambient": 0.25, "diffuse": 0.7,
                                 "light_from": [0.4, 0.8, 0.6]},
                    "objects": objects,
                },
            }],
        }],
    }
    json.dump(doc, open(path, "w", encoding="utf-8"), ensure_ascii=False)
    return path


def main():
    label = sys.argv[1] if len(sys.argv) > 1 else "current"
    results = []

    # ── 场景 1：3 简单物体（轻载） ──
    light = [{"name": f"o{i}", "shape": s, "size": 0.8,
              "translate": [i * 1.1 - 1.1, 0.2, 0]}
             for i, s in enumerate(["uv_sphere", "cube", "torus"])]
    path = make_3d_script(os.path.join(TMP, "perf_light.json"), light, [800, 450])
    for size in ("800x450", "1920x1080"):
        ms = run_render(path, size, frames=10)
        results.append(("3 物体", size, ms))

    # ── 场景 2：15 高细分球（重载） ──
    heavy = [{"name": f"s{i}", "shape": "uv_sphere", "size": 0.7,
              "segments": 96, "rings": 64,
              "translate": [(i % 5) * 0.9 - 1.8, 0.4, (i // 5) * 0.9 - 0.9],
              "metallic": 0.4, "roughness": 0.3}
             for i in range(15)]
    path = make_3d_script(os.path.join(TMP, "perf_heavy.json"), heavy, [1920, 1080])
    ms = run_render(path, "1920x1080", frames=6)
    results.append(("15 高细分球", "1920x1080", ms))

    # ── 场景 3：真实片子（2D 密集）──
    #
    # 默认只测 sample-basic（快）；`--full` 才测 scene-script-demo（每帧 4 秒，
    # 一次基线要 12 分钟，日常对比不必跑）。
    scripts = [os.path.join(ROOT, "examples/caiyun/assets/sample-basic.json")]
    if "--full" in sys.argv:
        scripts.append(os.path.join(ROOT, "examples/caiyun/assets/scene-script-demo.json"))
    for script in scripts:
        if os.path.exists(script):
            ms = run_render(script, "1920x1080", frames=20)
            results.append((os.path.basename(script), "1920x1080", ms))

    print(f"\n=== 性能基线 [{label}] ===")
    for name, size, ms in results:
        if ms is None:
            print(f"  {name:<26} {size:<10} 失败")
        else:
            fps = 1000.0 / ms
            print(f"  {name:<26} {size:<10} {ms:>9.1f} ms/帧  ({fps:>6.2f} fps)")
    print()


if __name__ == "__main__":
    main()

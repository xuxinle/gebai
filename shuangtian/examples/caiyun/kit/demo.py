#!/usr/bin/env python3
"""一键把 kit 模板跑成一条可看的片子（用来确认环境与观感）。

## 为什么需要它

模板是"起点"，但一开始没人知道它长什么样。这个脚本把
`templates/tpl-*.js` → 拼 bundle → 检查 → 出片 → 抽 3 帧，
让你在 30 秒内看到成品，再决定改哪里。

## 用法

    py examples/caiyun/kit/demo.py                    # 两个模板都跑
    py examples/caiyun/kit/demo.py --only single      # 只跑单体特写
    py examples/caiyun/kit/demo.py --no-render        # 只拼+检查（不渲）

产物在 `examples/caiyun/kit/_build/`：
    <name>.bundle.js   拼接产物（json 的 layout 指向它）
    <name>.mp4         成片
    _still-<name>.png  抽的一帧（供肉眼确认）
"""

import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
BUILD = os.path.join(HERE, "_build")

TEMPLATES = {
    "single": ("tpl-single-subject", 12.0),
    "multi": ("tpl-multi-element", 14.0),
}


def sh(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True,
                       encoding="utf-8", errors="replace", **kw)
    return r.returncode, (r.stdout or "") + (r.stderr or "")


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", choices=list(TEMPLATES), default="")
    ap.add_argument("--no-render", action="store_true")
    args = ap.parse_args()

    caiyun = os.path.join(ROOT, "build", "dev", "bin", "caiyun.exe")
    if not os.path.isfile(caiyun):
        sys.exit(f"找不到 {caiyun}：先 `st build caiyun --profile dev`")

    os.makedirs(BUILD, exist_ok=True)
    picks = [args.only] if args.only else list(TEMPLATES)
    failed = 0

    for key in picks:
        name, dur = TEMPLATES[key]
        print(f"\n=== {name}（{dur}s）")

        # ① 拼
        bundle = os.path.join(BUILD, f"{name}.bundle.js")
        code, out = sh(["py", os.path.join(HERE, "build.py"),
                        "--script", os.path.join(HERE, "templates", f"{name}.js"),
                        "--out", bundle], cwd=ROOT)
        print("  拼接:", "OK" if code == 0 else "失败")
        if code != 0:
            print(out.strip()[:400])
            failed += 1
            continue

        # ② 检查（渲染前必过，否则画面会静默降级）
        code, out = sh(["py", os.path.join(ROOT, "scripts", "check_layout.py"),
                        bundle, "--duration", str(dur)], cwd=ROOT)
        print("  检查:", out.strip().splitlines()[-1][:110] if out.strip() else "?")
        if code != 0:
            failed += 1
            continue

        if args.no_render:
            continue

        # ③ 写场景 json（layers 必须有：没有它 layout 的 states 无处落）
        scene_json = os.path.join(BUILD, f"{name}.json")
        layers = ([
            {"kind": "text", "name": "名字", "text": "ONE THING", "x": "460", "y": "470",
             "w": "1000", "h": "110", "size": 96, "color": "#E8E4DA", "align": 0,
             "tracking": 3, "bold": True, "enter": 0},
            {"kind": "text", "name": "副标", "text": "a single subject, one take",
             "x": "462", "y": "580", "w": "1000", "h": "44", "size": 30,
             "color": "#8B8375", "align": 0, "tracking": 2, "enter": 0},
        ] if key == "single" else [
            {"kind": "text", "name": "标题", "text": "THE TOOLKIT", "x": "560", "y": "120",
             "w": "900", "h": "86", "size": 72, "color": "#EDEAE3", "align": 1,
             "tracking": 3, "bold": True, "enter": 0},
            {"kind": "text", "name": "副标", "text": "eight things it does", "x": "562",
             "y": "930", "w": "900", "h": "40", "size": 28, "color": "#7E8577",
             "align": 1, "tracking": 2, "enter": 0},
        ])
        data = {
            "name": name, "fps": 30, "size": [1920, 1080],
            "transition": 0.3, "fade_in": 0.3, "fade_out": 0.4,
            "brand_wordmark": "", "brand_latin": "", "footer_note": "",
            "audio": {"enabled": False},
            "scenes": [{
                "kind": "custom", "duration": dur,
                "layout": f"_build/{name}.bundle.js",
                "layers": layers,
                "accent": "#D2553C",
                "background": ["#0B0B0D", "#0B0B0D"],
            }],
        }
        json.dump(data, open(scene_json, "w", encoding="utf-8"),
                  ensure_ascii=False, indent=2)

        # ④ 渲
        mp4 = os.path.join(BUILD, f"{name}.mp4")
        code, out = sh([caiyun, "--headless", "--script", scene_json, "--render", mp4],
                       cwd=ROOT, timeout=1800)
        q = next((l.strip() for l in out.splitlines() if "画面质量" in l), "")
        print("  渲染:", q[:120] if q else ("OK" if code == 0 else "失败"))
        if code != 0:
            failed += 1
            continue

        # ⑤ 抽一帧（指标过 ≠ 画面对，必须肉眼看）
        still = os.path.join(BUILD, f"_still-{name}.png")
        sh(["ffmpeg", "-y", "-v", "error", "-ss", str(dur * 0.45), "-i", mp4,
            "-frames:v", "1", still], cwd=ROOT)
        if os.path.isfile(still):
            print("  抽帧:", still)

    print("\n完成" if failed == 0 else f"\n有 {failed} 项失败")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""在渲染**之前**把 layout 脚本逐帧跑一遍，抓出运行期错误。

## 为什么必须有这一步（本轮最贵的一条）

裁云的 layout 按设计会**静默降级**：脚本抛错时渲染器不报错、继续出片，
画面只剩背景。于是：

- 出片"成功"、报告有数字、指标能过 —— 但**内容是错的/空的**
- 我为此白改了 3 轮相机关键帧：`c06-pith` 的 `keys()` 调了从未定义的
  `ease()`，**每一帧都抛 ReferenceError**，所有改动一个都没生效，
  而逐秒数据一直是 `mean=0.00`（降级的特征），没有任何报错提示。

所以：**先跑 layout，再渲染**。这一步是 10 秒的事，省的是几十分钟的渲染 + 误判。

## 用法

    py scripts/check_layout.py out/otbench/c06-pith/pith.js --duration 21.5
    py scripts/check_layout.py --scan out/otbench        # 扫一个目录下所有 case

## 判据

- 任一帧抛错 → 退出码 1，打印**首个出错时刻与错误原文**
- 全程无错但 shapes 恒为 0 → 也算失败（"能跑但不产出"）
"""

import argparse
import glob
import json
import os
import re
import subprocess
import sys
import tempfile

# 在 node 侧跑：把脚本包成函数，逐帧调用，捕获异常。
# 用临时文件传脚本路径（`node -e` 下 process.argv 索引会偏，实测踩过）。
NODE_RUNNER = r"""
const fs = require('fs');
const src = fs.readFileSync(process.env.CHECK_JS, 'utf8');
const duration = parseFloat(process.env.CHECK_DUR);
const frameLayers = JSON.parse(process.env.CHECK_LAYERS);
const step = parseFloat(process.env.CHECK_STEP || '0.25');

// ## 宿主函数桩（必须与裁云的白名单一致）
//
// 这些名字在裁云的 layout 里是**宿主注入的**（`layout.cpp` 的 register_function），
// node 里没有。不注入就会得到 13 个假警报——实测第一版正是如此：
// 全部 case 报 "ease is not defined"，而它们在生产里跑得好好的。
//
// 桩的行为要与宿主一致（ease 是**同名曲线的查表**，不是恒等），
// 否则"改动是否生效"的判断会失真。
const EASE = {
  linear: u => u, in: u => u * u, out: u => 1 - (1 - u) * (1 - u),
  in_out: u => (u < 0.5 ? 2 * u * u : 1 - 2 * (1 - u) * (1 - u)),
  expo_in: u => (u <= 0 ? 0 : Math.pow(2, 10 * (u - 1))),
  expo_out: u => (u >= 1 ? 1 : 1 - Math.pow(2, -10 * u)),
  expo_in_out: u => (u <= 0 ? 0 : (u >= 1 ? 1 : (u < 0.5
      ? Math.pow(2, 20 * u - 10) / 2 : (2 - Math.pow(2, -20 * u + 10)) / 2))),
  back: u => { const c = 1.70158; return 1 + (c + 1) * Math.pow(u - 1, 3) + c * Math.pow(u - 1, 2); },
};
const ease = (name, u) => { const f = EASE[name]; return f ? f(u) : u; };
const spring = (t, zeta, omega) => {
  const z = zeta === undefined ? 0.8 : zeta, w = omega === undefined ? 10 : omega;
  if (t < 0) return 0;
  return 1 - Math.exp(-z * w * t) * Math.cos(w * Math.sqrt(1 - z * z) * t);
};
const spring_vel = (t, zeta, omega) => {
  const z = zeta === undefined ? 0.8 : zeta, w = omega === undefined ? 10 : omega;
  if (t < 0) return 0;
  return Math.exp(-z * w * t) * (z * w * Math.cos(w * t) + w * Math.sin(w * t));
};
const settle = (t, dur) => { const d = dur === undefined ? 1 : dur; const u = Math.min(1, t / d); return u * u * (3 - 2 * u); };
const lerpHost = (a, b, k) => a + (b - a) * k;
const clampHost = (v, a, b) => Math.min(b === undefined ? 1 : b, Math.max(a === undefined ? 0 : a, v));
const snap = (v, step) => { const s = step === undefined ? 1 : step; return Math.round(v / s) * s; };
const rand = (n) => { const x = Math.sin(n * 12.9898) * 43758.5453; return x - Math.floor(x); };

let fn;
try {
  // 宿主桩以参数形式注入，与裁云的调用环境同形
  fn = new Function('t', 'duration', 'canvas', 'layers',
                    'const ease = arguments[4], spring = arguments[5], spring_vel = arguments[6],'
                    + ' settle = arguments[7], lerp = arguments[8], clamp = arguments[9],'
                    + ' snap = arguments[10], rand = arguments[11];'
                    + src + '\n; return layout(t, duration, canvas, layers);');
} catch (e) {
  console.log(JSON.stringify({ loadError: String(e && e.message || e) }));
  process.exit(0);
}

let bad = 0, first = null, maxShapes = 0, zeroFrames = 0, total = 0;
let offscreen = 0, offscreenFirst = null;
const W = 1920, H = 1080;
for (let t = 0; t <= duration + 1e-9; t += step) {
  total++;
  try {
    const r = fn(t, duration, { width: W, height: H }, frameLayers,
                  ease, spring, spring_vel, settle, lerpHost, clampHost, snap, rand);
    const shapes = (r && r.shapes) ? r.shapes : [];
    const n = shapes.length;
    if (n > maxShapes) maxShapes = n;
    if (n === 0) zeroFrames++;

    // ## 检查图元是否**整体跑出画布**
    //
    // 实测代价：pith 的工具清单用了页面坐标 1480，而页面宽只有 1240，
    // 在 zoom=9 时右对齐的数值整片跑到屏幕外 —— 画面里只剩左半截文字
    // （"grep" 显示成 "rep"）。渲染器不报错，指标也照算。
    //
    // 判据：图元中心落在 [-W, 2W] × [-H, 2H] 之外算跑飞（留一屏容差，
    // 因为设计上允许"从画外滑入"这类入场）。
    for (let i = 0; i < n; i++) {
      const s = shapes[i];
      const cx = (s.x === undefined) ? 0 : s.x;
      const cy = (s.y === undefined) ? 0 : s.y;
      if (cx < -2 * W || cx > 3 * W || cy < -2 * H || cy > 3 * H) {
        offscreen++;
        if (!offscreenFirst) offscreenFirst = { t: Math.round(t * 100) / 100, x: Math.round(cx), y: Math.round(cy) };
      }
    }
  } catch (e) {
    if (!first) first = { t: Math.round(t * 100) / 100, msg: String(e && e.message || e) };
    bad++;
  }
}
console.log(JSON.stringify({ bad: bad, first: first, maxShapes: maxShapes,
                             zeroFrames: zeroFrames, total: total,
                             offscreen: offscreen, offscreenFirst: offscreenFirst }));
"""


def run_one(js_path: str, duration: float, layers: int = 8, step: float = 0.25) -> dict:
    env = dict(os.environ)
    env["CHECK_JS"] = js_path
    env["CHECK_DUR"] = str(duration)
    env["CHECK_LAYERS"] = json.dumps([{} for _ in range(layers)])
    env["CHECK_STEP"] = str(step)
    r = subprocess.run(["node", "-e", NODE_RUNNER], env=env,
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    out = (r.stdout or "").strip()
    if not out:
        return {"error": (r.stderr or "node 无输出").strip()[:200]}
    try:
        return json.loads(out.splitlines()[-1])
    except Exception:
        return {"error": out[:200]}


def report(name: str, res: dict) -> bool:
    """打印一条结果，返回是否通过。"""
    if "error" in res:
        print(f"  [X] {name:22s} {res['error']}")
        return False
    if "loadError" in res:
        print(f"  [X] {name:22s} 加载失败：{res['loadError']}")
        return False
    if res["bad"] > 0:
        f = res["first"]
        print(f"  [X] {name:22s} {res['bad']}/{res['total']} 帧抛错；"
              f"首个 @{f['t']}s：{f['msg']}")
        return False
    if res["maxShapes"] == 0:
        print(f"  [X] {name:22s} 全程 shapes 为 0（能跑但不产出）")
        return False
    note = ""
    if res["zeroFrames"] > 0:
        note = f"（{res['zeroFrames']} 帧 shapes=0，可能是空白段）"
    ok = True
    if res.get("offscreen", 0) > 0:
        f = res.get("offscreenFirst") or {}
        print(f"  [!] {name:22s} {res['offscreen']} 个图元跑出画布；"
              f"首个 @{f.get('t')}s (x={f.get('x')}, y={f.get('y')})")
        note += "（有图元跑出画布，见上行）"
    print(f"  [OK] {name:22s} {res['total']} 帧无错，shapes 峰值 {res['maxShapes']}{note}")
    return ok


def scan(out_dir: str) -> int:
    """扫一个目录下的所有 case：找 json → 取 layout → 跑。"""
    cases = sorted(d for d in os.listdir(out_dir)
                   if os.path.isdir(os.path.join(out_dir, d)))
    print(f"扫描 {out_dir}（{len(cases)} 个目录）\n")
    ok = bad = skipped = 0
    for name in cases:
        d = os.path.join(out_dir, name)
        cand = [p for p in glob.glob(os.path.join(d, "*.json"))
                if os.path.basename(p) not in ("report.json", "ref_baseline.json")]
        if not cand:
            continue
        try:
            data = json.load(open(cand[0], encoding="utf-8"))
        except Exception as e:
            print(f"  [?] {name:22s} json 读不出：{e}")
            skipped += 1
            continue
        scenes = data.get("scenes", [])
        if not scenes:
            skipped += 1
            continue
        layout = scenes[0].get("layout")
        if not layout or not isinstance(layout, str):
            print(f"  [-] {name:22s} 无 layout（纯 JSON 场景）")
            skipped += 1
            continue
        js = layout if os.path.isabs(layout) else os.path.join(d, layout)
        if not os.path.isfile(js):
            print(f"  [X] {name:22s} layout 文件缺失：{layout}")
            bad += 1
            continue
        dur = float(scenes[0].get("duration", 15))
        if report(name, run_one(js, dur)):
            ok += 1
        else:
            bad += 1
    print(f"\n通过 {ok} · 失败 {bad} · 跳过 {skipped}")
    return 0 if bad == 0 else 1


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass
    ap = argparse.ArgumentParser()
    ap.add_argument("script", nargs="?", help="单个 layout 脚本路径")
    ap.add_argument("--duration", type=float, default=15.0)
    ap.add_argument("--layers", type=int, default=8, help="给 layout 的图层数（默认 8）")
    ap.add_argument("--step", type=float, default=0.25, help="采样步长（秒）")
    ap.add_argument("--scan", default="", help="扫一个目录下的所有 case")
    args = ap.parse_args()

    if args.scan:
        return scan(args.scan)
    if not args.script:
        ap.error("需要 script 或 --scan")
    ok = report(os.path.basename(args.script),
                run_one(args.script, args.duration, args.layers, args.step))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

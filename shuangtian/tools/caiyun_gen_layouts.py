"""生成裁云多镜示例的 `layout-msN.js`：把公共前言与各幕的 body 拼成独立文件。

## 为什么要生成而不是手写

`layout` 脚本有个致命特性：**写错就整份静默降级为纯 JSON，且零报错**
（画面停在初始帧，看起来只是"没做动效"）。而每一份都要带同一段前言
（`seg`/`expoOut`/`mk` 等宿主不提供的辅助函数）+ 一份 20 条注释。

七模板 × 三镜 = 21 份。手抄必然漂移，而漂移的表现是某个模板"莫名其妙不动"。
生成器把前言收敛到一处，body 单独写——**改动前言只需要改这一个文件**。

用法：`python tools/caiyun_gen_layouts.py <styles 子目录...>`
每份 body 从 `<dir>/bodies/ms<N>.js` 读取（只含 `function shotN(...)` 与注释）。
"""

import os
import sys

PREAMBLE = """// 裁云可编排脚本（layout）—— 宿主只提供这些名字，写错就整份**静默降级**为纯 JSON。
//
// 可用：ease(name,t) spring(tau[,zeta,omega]) spring_vel settle
//       lerp(a,b,k) clamp(v,lo,hi) snap(t,fps) noise(seed)
// 不可用：seg / E.* / D() —— 用了就 ReferenceError → 降级 → 画面停在初始帧且零报错。
//
// LayerDelta 字段：dx dy dw dh scale rotate alpha tint
//   —— **改不了 fill**。要"点亮"必须底下垫一层亮色块，改它的 alpha（实测坑）。
//
// 返回必须是 { states, shapes }，不是裸数组。
function seg(t, t0, t1) {
  if (t1 === t0) return (t >= t1) ? 1 : 0;
  const k = (t - t0) / (t1 - t0);
  return k < 0 ? 0 : (k > 1 ? 1 : k);
}
function expoOut(u) { return (u >= 1) ? 1 : 1 - Math.pow(2, -9 * u); }
function expoInOut(u) {
  if (u <= 0) return 0;
  if (u >= 1) return 1;
  return (u < 0.5) ? (u * 2) * (u * 2) * (u * 2) / 2 : 1 - Math.pow(2, -9 * (1 - u)) / 2;
}
function mk(o) {
  const a = (o.alpha === undefined) ? 1 : o.alpha;
  return {
    dx: o.dx || 0, dy: o.dy || 0,
    dw: o.dw || 0, dh: o.dh || 0,
    scale: (o.scale === undefined) ? 1 : o.scale,
    rotate: o.rotate || 0,
    alpha: a < 0 ? 0 : (a > 1 ? 1 : a),
  };
}

"""

TRAILER = """
function layout(t, duration, canvas, layers) {
  return shot(t, duration, canvas, layers);
}
"""


def build(directory: str) -> int:
    bodies = os.path.join(directory, "bodies")
    if not os.path.isdir(bodies):
        print(f"  跳过（无 bodies/）：{directory}")
        return 0
    made = 0
    for name in sorted(os.listdir(bodies)):
        if not name.endswith(".js"):
            continue
        idx = name[:-3]                      # 如 ms0
        body = open(os.path.join(bodies, name), encoding="utf-8").read().rstrip()
        out = os.path.join(directory, f"layout-{idx}.js")
        with open(out, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(PREAMBLE + body + TRAILER)
        print(f"  {os.path.basename(directory)}/layout-{idx}.js")
        made += 1
    return made


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    total = 0
    for d in sys.argv[1:]:
        total += build(d)
    print(f"\n共同前言已写入 {total} 份 layout 文件")
    return 0


if __name__ == "__main__":
    sys.exit(main())

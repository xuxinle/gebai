"""标定第二轮：找"既够动、又不像故障"的可持续方案。

第一轮结论（同口径实测）：
  A 基线（落定版面）      still 100.0%
  B 竖向数据流            still 100.0%
  C 全屏亮度脉冲 0.26s    still  29.8%
  D 全屏光带横扫          still 100.0%
  E 扩散环                still 100.0%
  F 大块快速穿过          still 100.0%
  G = B+C                 still  34.0%
  H = E+F                 still   0.0%

两个发现：
1. **单层几乎都过不了判据**——阈值 1.2 是硬的，只有把多层叠起来才稳定越线；
2. 我的片子用 `grain` 800~1000 却测得 82% 静止 ⇒ grain 要么不动、要么量级不够。

这一轮补测四件事：
· grain 到底带不带变化（它若带，是最省的一层）
· 亮度脉冲在**可接受频率与幅度**下能到多少（避免 3.8Hz 频闪）
· 光斑漂移（大半径、低压差）能带多少
· 三层组合（脉冲 + 扩散环 + 快扫）的稳态

用法：py calib2.py
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CAIYUN = os.path.abspath(os.path.join(HERE, "..", "..", "..", "build", "dev", "bin", "caiyun.exe"))
OUT = os.path.join(HERE, "qa", "calib2")
W, H = 1920, 1080
COLS, ROWS = 32, 18

COMMON = r"""
function baseGrid(sh, t) {
  for (let i = 0; i < 12; i++) {
    const x = 140 + (i % 4) * 420, y = 260 + Math.floor(i / 4) * 200;
    sh.push({ kind: 1, x: x, y: y, w: 360, h: 150, color: '#101827', alpha: 0.95, radius: 12 });
    sh.push({ kind: 1, x: x, y: y, w: 3, h: 150, color: '#3B82F6', alpha: 0.6 });
    sh.push({ kind: 3, text: 'AGENT ' + (i + 1), x: x + 20, y: y + 30, h: 30, color: '#EAF0FF', alpha: 0.9 });
    sh.push({ kind: 3, text: 'tool surface', x: x + 20, y: y + 80, h: 20, color: '#7C93B4', alpha: 0.8 });
  }
}
"""

RINGS = r"""
function rings(sh, t, W, H, gain) {
  for (let k = 0; k < 3; k++) {
    const ph = ((t / 1.55) + k / 3) % 1;
    const r = 40 + ph * 940;
    sh.push({ kind: 0, x: W * 0.5 - r, y: H * 0.5 - r * 0.62, w: r * 2, h: r * 1.24,
              color: '#4D7DFF', alpha: 0.22 * (1 - ph) * gain });
  }
}
"""

VARIANTS = {
    # 只测 grain（场景级特效，不是 shapes）：dyn 为空
    "G1_grain_only": (r"function dyn(sh,t,W,H){}", {"grain": 1100, "grain_alpha": 0.12}),
    "G2_grain_heavy": (r"function dyn(sh,t,W,H){}", {"grain": 2400, "grain_alpha": 0.22}),
    # 可接受频率的整屏呼吸：1.5Hz、幅度 0.045（≈11 级 luma 摆幅，柔和）
    "P1_pulse_1p5hz": (r"""
function dyn(sh, t, W, H) {
  const p = 0.5 + 0.5 * Math.sin(t * (Math.PI * 2 / 0.66) - Math.PI / 2);
  sh.push({ kind: 1, x: -10, y: -10, w: W + 20, h: H + 20, color: '#FFFFFF', alpha: 0.001 + 0.045 * p });
}
""", {}),
    # 慢呼吸：0.9Hz、幅度 0.06
    "P2_pulse_0p9hz": (r"""
function dyn(sh, t, W, H) {
  const p = 0.5 + 0.5 * Math.sin(t * (Math.PI * 2 / 1.10) - Math.PI / 2);
  sh.push({ kind: 1, x: -10, y: -10, w: W + 20, h: H + 20, color: '#FFFFFF', alpha: 0.001 + 0.060 * p });
}
""", {}),
    # 光斑漂移：两个大半径低压差圆，速度 ~260px/s
    "L1_glow_drift": (r"""
function dyn(sh, t, W, H) {
  for (let i = 0; i < 2; i++) {
    const cx = W * (0.3 + 0.4 * i) + Math.sin(t * 0.9 + i * 2.1) * 260;
    const cy = H * 0.45 + Math.cos(t * 0.7 + i * 1.3) * 150;
    for (let k = 5; k >= 1; k--) {
      const r = 150 + k * 110;
      sh.push({ kind: 0, x: cx - r, y: cy - r, w: r * 2, h: r * 2,
                color: i === 0 ? '#3B82F6' : '#7FB2FF', alpha: 0.020 * (6 - k) / 5 });
    }
  }
}
""", {}),
    # 组合 A：呼吸 + 扩散环（两层，速度都慢）
    "C1_pulse_rings": (r"""
function dyn(sh, t, W, H) {
  const p = 0.5 + 0.5 * Math.sin(t * (Math.PI * 2 / 0.66) - Math.PI / 2);
  sh.push({ kind: 1, x: -10, y: -10, w: W + 20, h: H + 20, color: '#FFFFFF', alpha: 0.001 + 0.038 * p });
  rings(sh, t, W, H, 1.0);
}
""", {}),
    # 组合 B：呼吸 + 扩散环 + 快扫（三层）
    "C2_pulse_rings_sweep": (r"""
function dyn(sh, t, W, H) {
  const p = 0.5 + 0.5 * Math.sin(t * (Math.PI * 2 / 0.66) - Math.PI / 2);
  sh.push({ kind: 1, x: -10, y: -10, w: W + 20, h: H + 20, color: '#FFFFFF', alpha: 0.001 + 0.034 * p });
  rings(sh, t, W, H, 0.9);
  const bx = ((t * 1700) % (W + 700)) - 350;
  for (let g = 0; g < 4; g++) {
    sh.push({ kind: 1, x: bx + g * 80, y: 0, w: 80, h: H, color: '#7FB2FF', alpha: 0.07 - g * 0.014 });
  }
}
""", {}),
    # 组合 C：呼吸 + 环 + 大块（最"热闹"，用于峰值幕）
    "C3_pulse_rings_blocks": (r"""
function dyn(sh, t, W, H) {
  const p = 0.5 + 0.5 * Math.sin(t * (Math.PI * 2 / 0.66) - Math.PI / 2);
  sh.push({ kind: 1, x: -10, y: -10, w: W + 20, h: H + 20, color: '#FFFFFF', alpha: 0.001 + 0.034 * p });
  rings(sh, t, W, H, 0.85);
  for (let i = 0; i < 12; i++) {
    const y = 70 + i * 84;
    const x = ((t * (1500 + i * 80) + i * 420) % (W + 560)) - 280;
    sh.push({ kind: 1, x: x, y: y, w: 240, h: 42, color: '#7FB2FF', alpha: 0.14, radius: 8 });
  }
}
""", {}),
}


def measure(path: str):
    cmd = ["ffmpeg", "-v", "error", "-i", path, "-vf", f"scale={COLS}:{ROWS}",
           "-f", "rawvideo", "-pix_fmt", "rgb24", "-"]
    raw = subprocess.run(cmd, capture_output=True).stdout
    size = COLS * ROWS * 3
    n = len(raw) // size
    if n < 3:
        return None
    fr = []
    for k in range(n):
        o = k * size
        fr.append([0.299 * raw[o + i * 3] + 0.587 * raw[o + i * 3 + 1] + 0.114 * raw[o + i * 3 + 2]
                   for i in range(COLS * ROWS)])
    d = []
    for i in range(1, len(fr)):
        a, b = fr[i - 1], fr[i]
        d.append(sum(abs(x - y) for x, y in zip(a, b)) / len(a))
    s = sorted(d)
    m = len(s)
    return {"still%": round(100.0 * sum(1 for v in s if v < 1.2) / m, 1),
            "median": round(s[m // 2], 2), "mean": round(sum(d) / m, 2),
            "min": round(s[0], 2), "peak": round(s[-1], 2)}


def main() -> int:
    os.makedirs(OUT, exist_ok=True)
    results = {}
    for name, (dyn, extra) in VARIANTS.items():
        js = COMMON + RINGS + dyn + f"""
function layout(t, duration, canvas, layers) {{
  const sh = [];
  baseGrid(sh, t);
  dyn(sh, t, {W}, {H});
  return {{ states: layers.map(() => ({{}})), shapes: sh }};
}}
"""
        with open(os.path.join(OUT, f"{name}.js"), "w", encoding="utf-8") as f:
            f.write(js)
        scene = {
            "kind": "custom", "duration": 2.2, "layout": f"{name}.js",
            "accent": "#3B82F6", "background": ["#08090D", "#101724"], "wrap": False,
            "layers": [{"kind": "rect", "x": "0", "y": "0", "w": "2", "h": "2",
                        "fill": "#000000", "opacity": 0.01, "enter": 0}],
        }
        scene.update(extra)
        script = {
            "name": f"calib2 {name}", "fps": 30, "size": [W, H],
            "transition": 0.0, "fade_in": 0.0, "fade_out": 0.0,
            "audio": {"enabled": False, "style": "calm", "bpm": 96, "gain": 0.3, "voice": []},
            "scenes": [scene],
        }
        jspath = os.path.join(OUT, f"{name}.json")
        with open(jspath, "w", encoding="utf-8") as f:
            json.dump(script, f, ensure_ascii=False)
        mp4 = os.path.join(OUT, f"{name}.mp4")
        if os.path.exists(mp4):
            os.remove(mp4)
        r = subprocess.run([CAIYUN, "--headless", "--script", jspath, "--render", mp4],
                           capture_output=True, text=True, timeout=300)
        if not os.path.exists(mp4):
            print(f"{name}: 渲染失败 {(r.stdout + r.stderr)[-300:]}")
            continue
        st = measure(mp4)
        results[name] = st
        print(f"{name:24s} {st}")

    print("\n排序（still% 低者优先）：")
    for name, st in sorted(results.items(), key=lambda kv: kv[1]["still%"]):
        print(f"  {name:24s} still={st['still%']:5.1f}%  median={st['median']:6.2f}  "
              f"min={st['min']:5.2f}  peak={st['peak']:6.2f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

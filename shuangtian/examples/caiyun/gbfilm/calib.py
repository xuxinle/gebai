"""标定：哪种手法真的能让「静止占比」降下来（同口径实测，不靠猜）。

## 为什么必须标定

第一版全片 7 幕静止 82%，而我用的全是"入场动画 + 落定后微起伏"——
入场只占每幕前 1.5 秒，之后 3~4 秒画面几乎不变。
而 `cam_zoom` / `cam_rotate` 对**脚本图元无效**（`paint_script_shapes` 明确：
shapes 收屏幕像素、不过相机），所以我原本指望的"运镜让画面活着"根本不发生。

这个脚本把候选手法各自渲一段 1.2 秒，用与验收器相同的口径量：
32×18 网格、逐格灰度均值、相邻帧差 <1.2 记静止。

用法：py calib.py
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CAIYUN = os.path.abspath(os.path.join(HERE, "..", "..", "..", "build", "dev", "bin", "caiyun.exe"))
OUT = os.path.join(HERE, "qa", "calib")

W, H = 1920, 1080
COLS, ROWS = 32, 18


# ── 每个变体的 shapes 生成器（都画同一套"静止主机身"，只加不同的动态层）──
COMMON = r"""
function baseGrid(sh, t) {
  // 12 张卡片（模拟"落定后的版面"）：本身几乎不动
  for (let i = 0; i < 12; i++) {
    const x = 140 + (i % 4) * 420, y = 260 + Math.floor(i / 4) * 200;
    sh.push({ kind: 1, x: x, y: y, w: 360, h: 150, color: '#101827', alpha: 0.95, radius: 12 });
    sh.push({ kind: 1, x: x, y: y, w: 3, h: 150, color: '#3B82F6', alpha: 0.6 });
    sh.push({ kind: 3, text: 'AGENT ' + (i + 1), x: x + 20, y: y + 30, h: 30, color: '#EAF0FF', alpha: 0.9 });
    sh.push({ kind: 3, text: 'tool surface', x: x + 20, y: y + 80, h: 20, color: '#7C93B4', alpha: 0.8 });
  }
}
"""

VARIANTS = {
    # A 基线：只有落定的版面
    "A_base": r"""
function dyn(sh, t, W, H) {}
""",
    # B 竖向数据流：稀疏亮点以 520px/s 下落（17px/帧）
    "B_stream": r"""
function dyn(sh, t, W, H) {
  for (let i = 0; i < 70; i++) {
    const lane = (i * 137.5) % W;
    const speed = 420 + (i % 5) * 130;
    const y = ((t * speed + i * 173) % (H + 220)) - 110;
    sh.push({ kind: 1, x: lane, y: y, w: 2.5, h: 46, color: '#6FA8FF', alpha: 0.55 });
    sh.push({ kind: 0, x: lane, y: y + 46, w: 7, h: 7, color: '#CFE6FF', alpha: 0.8 });
  }
}
""",
    # C 整屏亮度脉冲：alpha 0.000→0.030，周期 0.26s（dL 与网格差 1:1）
    "C_pulse": r"""
function dyn(sh, t, W, H) {
  const p = 0.5 + 0.5 * Math.sin(t * (Math.PI * 2 / 0.26));
  sh.push({ kind: 1, x: -10, y: -10, w: W + 20, h: H + 20, color: '#FFFFFF', alpha: 0.001 + 0.030 * p });
}
""",
    # D 全屏光带横扫：1900px/s（63px/帧，跨整格）
    "D_sweep": r"""
function dyn(sh, t, W, H) {
  const bx = ((t * 1900) % (W + 600)) - 300;
  for (let g = 0; g < 4; g++) {
    sh.push({ kind: 1, x: bx + g * 70, y: 0, w: 70, h: H, color: '#7FB2FF', alpha: 0.10 - g * 0.02 });
  }
}
""",
    # E 扩散环：每秒 2.2 圈，半径 0→980（存在感在变）
    "E_rings": r"""
function dyn(sh, t, W, H) {
  for (let k = 0; k < 3; k++) {
    const ph = ((t / 1.35) + k / 3) % 1;
    const r = 40 + ph * 940;
    const a = 0.30 * (1 - ph);
    sh.push({ kind: 0, x: W * 0.5 - r, y: H * 0.5 - r * 0.62, w: r * 2, h: r * 1.24,
              color: '#4D7DFF', alpha: a });
    sh.push({ kind: 0, x: W * 0.5 - r * 0.94, y: H * 0.5 - r * 0.62 + 14, w: r * 1.88, h: r * 1.24 - 28,
              color: '#05070C', alpha: a });
  }
}
""",
    # F 大块高对比图元快速穿过（每帧 80px，尺寸 >100px）
    "F_blocks": r"""
function dyn(sh, t, W, H) {
  for (let i = 0; i < 14; i++) {
    const y = 80 + i * 72;
    const x = ((t * (1400 + i * 90) + i * 410) % (W + 500)) - 250;
    sh.push({ kind: 1, x: x, y: y, w: 220, h: 40, color: i % 2 ? '#2E63D8' : '#7FB2FF',
              alpha: 0.22, radius: 8 });
  }
}
""",
    # G B+C 组合
    "G_stream_pulse": r"""
function dyn(sh, t, W, H) {
  const p = 0.5 + 0.5 * Math.sin(t * (Math.PI * 2 / 0.26));
  sh.push({ kind: 1, x: -10, y: -10, w: W + 20, h: H + 20, color: '#FFFFFF', alpha: 0.001 + 0.026 * p });
  for (let i = 0; i < 70; i++) {
    const lane = (i * 137.5) % W;
    const speed = 420 + (i % 5) * 130;
    const y = ((t * speed + i * 173) % (H + 220)) - 110;
    sh.push({ kind: 1, x: lane, y: y, w: 2.5, h: 46, color: '#6FA8FF', alpha: 0.5 });
  }
}
""",
    # H E+F 组合
    "H_rings_blocks": r"""
function dyn(sh, t, W, H) {
  for (let k = 0; k < 3; k++) {
    const ph = ((t / 1.35) + k / 3) % 1;
    const r = 40 + ph * 940;
    const a = 0.26 * (1 - ph);
    sh.push({ kind: 0, x: W * 0.5 - r, y: H * 0.5 - r * 0.62, w: r * 2, h: r * 1.24,
              color: '#4D7DFF', alpha: a });
  }
  for (let i = 0; i < 14; i++) {
    const y = 80 + i * 72;
    const x = ((t * (1400 + i * 90) + i * 410) % (W + 500)) - 250;
    sh.push({ kind: 1, x: x, y: y, w: 220, h: 40, color: '#7FB2FF', alpha: 0.18, radius: 8 });
  }
}
""",
}


def make_scene(name: str, dyn: str) -> dict:
    js = COMMON + dyn + f"""
function layout(t, duration, canvas, layers) {{
  const sh = [];
  baseGrid(sh, t);
  dyn(sh, t, {W}, {H});
  return {{ states: layers.map(() => ({{}})), shapes: sh }};
}}
"""
    path = os.path.join(OUT, f"{name}.js")
    with open(path, "w", encoding="utf-8") as f:
        f.write(js)
    return {
        "kind": "custom",
        "duration": 1.6,
        "layout": f"{name}.js",
        "accent": "#3B82F6",
        "background": ["#08090D", "#101724"],
        "wrap": False,
        "grain": 0,
        "layers": [{"kind": "rect", "x": "0", "y": "0", "w": "2", "h": "2",
                    "fill": "#000000", "opacity": 0.01, "enter": 0}],
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
    return {
        "still%": round(100.0 * sum(1 for v in s if v < 1.2) / m, 1),
        "median": round(s[m // 2], 2),
        "mean": round(sum(s) / m, 2),
    }


def main() -> int:
    os.makedirs(OUT, exist_ok=True)
    results = {}
    for name, dyn in VARIANTS.items():
        script = {
            "name": f"calib {name}", "fps": 30, "size": [W, H],
            "transition": 0.0, "fade_in": 0.0, "fade_out": 0.0,
            "audio": {"enabled": False, "style": "calm", "bpm": 96, "gain": 0.3, "voice": []},
            "scenes": [make_scene(name, dyn)],
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
        print(f"{name:18s} {st}")

    print("\n按静止占比排序（低 = 画面更活）：")
    for name, st in sorted(results.items(), key=lambda kv: kv[1]["still%"]):
        print(f"  {name:18s} still={st['still%']:5.1f}%  median={st['median']:6.2f}  mean={st['mean']:6.2f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

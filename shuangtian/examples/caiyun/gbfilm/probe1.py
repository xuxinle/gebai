"""单幕快速迭代：只渲一幕 + 同口径量（避免每改一版都重渲 38 秒全片）。

用法：py probe1.py 7            # 只渲幕 7 并量
      py probe1.py 2 4          # 多个幕
"""
import json
import os
import subprocess
import sys

sys.stdout.reconfigure(encoding="utf-8")

HERE = os.path.dirname(os.path.abspath(__file__))
CAIYUN = os.path.abspath(os.path.join(HERE, "..", "..", "..", "build", "dev", "bin", "caiyun.exe"))
COLS, ROWS = 32, 18


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
            "median": round(s[m // 2], 2), "mean": round(sum(d) / m, 2)}


def main() -> int:
    wants = [int(x) for x in sys.argv[1:]] or [1]
    with open(os.path.join(HERE, "gbfilm.json"), encoding="utf-8") as f:
        full = json.load(f)
    out_dir = os.path.join(HERE, "qa", "probe1")
    os.makedirs(out_dir, exist_ok=True)

    for idx in wants:
        scene = full["scenes"][idx - 1]
        script = dict(full)
        script["scenes"] = [scene]
        script["fade_in"] = 0.0
        script["fade_out"] = 0.0
        script["transition"] = 0.0
        if "audio" in script:
            script["audio"] = dict(script["audio"], enabled=False)
        js = os.path.join(out_dir, f"s{idx}.json")
        with open(js, "w", encoding="utf-8") as f:
            json.dump(script, f, ensure_ascii=False)
        mp4 = os.path.join(out_dir, f"s{idx}.mp4")
        if os.path.exists(mp4):
            os.remove(mp4)
        r = subprocess.run([CAIYUN, "--headless", "--script", js, "--render", mp4],
                           capture_output=True, text=True, timeout=600)
        if not os.path.exists(mp4):
            print(f"幕{idx}: 渲染失败 {(r.stdout + r.stderr)[-400:]}")
            continue
        st = measure(mp4)
        flag = "  [TOO STILL]" if st and st["still%"] > 70 else ""
        print(f"幕{idx}  {st}{flag}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

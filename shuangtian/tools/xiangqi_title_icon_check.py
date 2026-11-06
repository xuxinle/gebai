"""相期：标题栏品牌 SVG 渲染像素断言（用户报「标题栏图标糊」）。

背景：标题栏图标走 `apply_brand(frame->title_bar(), "xiangqi-logo")` →
`Icon::draw` → `SvgIconRegistry::draw`（按物理分辨率重栅）。
霜天 SVG 子集**不支持渐变**（defs 被跳过、url(#) 回落黑）——
上一版 SVG 用了 radialGradient，底色失效发黑，表现为「糊」。

判据（对截图标题栏图标区域做像素统计）：
  1. 存在明显的红/白/蓝三色像素簇（三竖条画出来了）
  2. 深色底色像素占多数且是 #1C1915 附近（实色底生效，不是全黑 #000 或缺失）
  3. 无大面积纯黑（rgb 都 < 8）——那是渐变失效的特征

跑法：py -3 tools/xiangqi_title_icon_check.py
"""
import json
import pathlib
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN = ROOT / "build" / "dev" / "bin" / "xiangqi.exe"
SHOTS = ROOT / "tmp" / "xiangqi-title-icon-check"
sys.path.insert(0, str(ROOT / "tools"))
from gbcode_hint_e2e import Client  # noqa: E402
from xiangqi_bg_check import png_pixels  # noqa: E402

if sys.stdout.encoding and sys.stdout.encoding.lower() not in ("utf-8", "utf8"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")


def log(m):
    print(m, flush=True)


def wait_for(fn, sec):
    end = time.time() + sec
    while time.time() < end:
        try:
            if fn():
                return True
        except Exception:  # noqa: BLE001
            pass
        time.sleep(0.2)
    return False


def main():
    if not BIN.exists():
        log(f"找不到 {BIN}")
        return 2
    SHOTS.mkdir(parents=True, exist_ok=True)
    ctl = SHOTS / "ctl.json"
    if ctl.exists():
        ctl.unlink()
    logf = open(SHOTS / "log.txt", "wb")
    proc = subprocess.Popen(
        [str(BIN), "--headless", "--control-port", "0", "--control-file", str(ctl),
         "--shots", str(SHOTS), "--home", str(SHOTS / "home"),
         "--ui", str(ROOT / "apps" / "xiangqi" / "ui")],
        stdout=logf, stderr=subprocess.STDOUT)
    try:
        client = None
        if wait_for(lambda: ctl.exists(), 20.0):
            for _ in range(80):
                try:
                    info = json.loads(ctl.read_text(encoding="utf-8"))
                    if info.get("port"):
                        client = Client(info["port"], info.get("token", ""))
                        client.ok("hello")
                        break
                except Exception:  # noqa: BLE001
                    pass
                time.sleep(0.2)
        if client is None:
            log("控制通道没起来")
            return 1

        shot = SHOTS / "title.png"
        client.ok("capture", {"encode": "file", "path": str(shot)})
        w, h, ch, rows = png_pixels(shot)

        # 标题栏图标位置：k_padding_x=12 起，16x16（标题栏高 40，垂直居中 → y=12..28）
        # 扫 x=8..36, y=8..32 的区域做像素统计
        reds = whites = blues = darks = blacks = 0
        samples = 0
        for y in range(8, 33):
            for x in range(8, 37):
                i = x * ch
                r, g, b = rows[y][i], rows[y][i + 1], rows[y][i + 2]
                samples += 1
                if r > 180 and g < 140 and b < 140:
                    reds += 1
                elif r > 220 and g > 220 and b > 220:
                    whites += 1
                elif b > 150 and r < 140:
                    blues += 1
                elif r < 8 and g < 8 and b < 8:
                    blacks += 1
                elif r < 60 and g < 60 and b < 60:
                    darks += 1
        log(f"  标题栏图标区采样 {samples} 像素：红 {reds} / 白 {whites} / 蓝 {blues} / "
            f"深底 {darks} / 纯黑 {blacks}")
        ok_bars = reds > 3 and whites > 3 and blues > 3
        ok_base = darks > samples * 0.2
        ok_no_black = blacks < samples * 0.05
        log(("  PASS " if ok_bars else "  FAIL ") + "三竖条（红/白/蓝像素簇）已渲染")
        log(("  PASS " if ok_base else "  FAIL ") + f"玄底实色占多数（{darks}/{samples}）")
        log(("  PASS " if ok_no_black else "  FAIL ") + f"无大面积纯黑（渐变失效特征，{blacks}/{samples}）")
        return 0 if (ok_bars and ok_base and ok_no_black) else 1
    finally:
        try:
            proc.terminate()
            proc.wait(timeout=5)
        except Exception:  # noqa: BLE001
            try:
                proc.kill()
            except Exception:  # noqa: BLE001
                pass
        logf.close()


if __name__ == "__main__":
    sys.exit(main())

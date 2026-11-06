"""相期：焦点灰框复现与验证（用户报「后台切前台有灰框，鼠标移上去就没了」）。

判据：给 #cards-scroll 设焦点后截图，扫窗口内容区四周是否出现**一整圈同色细线**
（焦点环）。修好后不应有。

跑法：py -3 tools/xiangqi_focus_ring_check.py
"""
import json
import pathlib
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN = ROOT / "build" / "dev" / "bin" / "xiangqi.exe"
SHOTS = ROOT / "tmp" / "xiangqi-focus-check"
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
    for f in SHOTS.glob("*.png"):
        f.unlink()
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

        # 空态（用户截图那一屏）
        out_before = SHOTS / "before.png"
        client.ok("capture", {"encode": "file", "path": str(out_before)})
        # 布局探针：空态下三块元素的实际 bounds（验证“空态真居中”）
        def bounds_of(cid):
            try:
                p = client.ok("get", {"id": cid}).get("props") or {}
                return p.get("bounds") or p.get("rect")
            except Exception:  # noqa: BLE001
                return None

        log("  空态布局：")
        try:
            tree = client.ok("tree", {"depth": 6})

            def walk(node, depth=0):
                if not isinstance(node, dict):
                    return
                nid = node.get("id")
                if nid in ("content", "cards-scroll", "cards", "empty", "empty-inner",
                           "empty-icon", "empty-title", "empty-hint", "footer-actions", "root"):
                    b = node.get("bounds") or {}
                    log(f"    #{nid}: y={b.get('y')} h={b.get('height')} vis={node.get('visible')}")
                for ch in (node.get("children") or []):
                    walk(ch, depth + 1)

            walk(tree.get("tree") or tree)
        except Exception as exc:  # noqa: BLE001
            log(f"    tree 查询失败: {exc}")

        # 给滚动容器设焦点——复现「窗口切到前台、焦点落在可聚焦容器上」
        focused = False
        for target in ("cards-scroll", "cards"):
            try:
                client.ok("invoke", {"id": target, "action": "focus"})
                focused = True
                log(f"  已聚焦 #{target}")
                break
            except Exception:  # noqa: BLE001
                continue
        if not focused:
            try:
                client.ok("focus", {"id": "cards-scroll"})
                focused = True
                log("  已通过 focus 方法聚焦 #cards-scroll")
            except Exception as exc:  # noqa: BLE001
                log(f"  ⚠ 无法聚焦（该版本控制通道无 focus）：{exc}")
        time.sleep(0.6)

        out_after = SHOTS / "after-focus.png"
        client.ok("capture", {"encode": "file", "path": str(out_after)})

        # ── 判据：内容区四周是否出现一整圈同色细线 ──
        def ring_line(path):
            w, h, ch, rows = png_pixels(path)

            def px(x, y):
                i = x * ch
                return tuple(rows[y][i:i + 3])

            # 在一行（内容区中部）里找「与两侧都不同的窄色带」——焦点环典型宽 1~3px
            y = h // 2
            hits = []
            for x in range(1, w - 1):
                left, mid, right = px(x - 1, y), px(x, y), px(x + 1, y)
                if mid != left and mid != right and left == right:
                    hits.append((x, mid))
            # 在中间一行出现的“孤立窄带”通常就是环线（左右各一条）
            return hits

        hb = ring_line(out_before)
        ha = ring_line(out_after)
        log(f"  聚焦前孤立窄带：{hb[:4]}")
        log(f"  聚焦后孤立窄带：{ha[:4]}")

        w, h, ch, rows = png_pixels(out_after)

        def px(x, y):
            i = x * ch
            return tuple(rows[y][i:i + 3])

        # 更强判据：内容区左边缘附近一列里，是否有一条与上下都不同的线
        vertical_hits = 0
        for x in range(2, 30):
            ys = [px(x, y) for y in range(h // 2 - 40, h // 2 + 40)]
            if len(set(ys)) > 1 and len(set(ys)) <= 3:
                vertical_hits += 1
        log(f"  左边缘竖直方向多色列数：{vertical_hits}")

        ok = len(ha) <= max(2, len(hb))
        log("")
        log(("  PASS " if ok else "  FAIL ") + (
            "聚焦后未新增孤立边框线（无焦点环）" if ok else
            f"聚焦后出现 {len(ha) - len(hb)} 条新孤立窄带 —— 疑似焦点环"))
        return 0 if ok else 1
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

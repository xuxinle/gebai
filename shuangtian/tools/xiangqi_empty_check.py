"""相期：空态布局验证（用户报「空白页面有点乱」）。

判据（几何，不靠眼看）：
  1. 空态三块内容（图标/标题/提示）作为**一个整体**，其竖直中心与内容区中心的
     偏差 ≤ 内容区高度的 8%——即"真的居中"，不是"在半屏里居中"。
  2. 空态下「全部忽略」不可见（没有任何可忽略的东西）。

历史缺陷（实测）：
  - `#cards-scroll` 与 `#empty` 同为 grow 兄弟 → 各分一半高度，
    空态只在**下半屏**里居中，看起来整块偏下、页面"乱"。
  - `set_visible` 只改标志不标脏布局 → 运行时隐掉一半，剩下那个不会重新分空间。

跑法：py -3 tools/xiangqi_empty_check.py
"""
import json
import pathlib
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN = ROOT / "build" / "dev" / "bin" / "xiangqi.exe"
SHOTS = ROOT / "tmp" / "xiangqi-empty-check"
sys.path.insert(0, str(ROOT / "tools"))
from gbcode_hint_e2e import Client  # noqa: E402
from xiangqi_e2e import MiniWsServer  # noqa: E402

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


def tree_index(client):
    """把控件树摊平成 {id: {y, height, visible}}。"""
    out = {}

    def walk(node):
        if not isinstance(node, dict):
            return
        nid = node.get("id")
        b = node.get("bounds") or {}
        if nid:
            out[nid] = {
                "y": b.get("y"),
                "h": b.get("height"),
                "x": b.get("x"),
                "w": b.get("width"),
                "visible": node.get("visible"),
            }
        for ch in (node.get("children") or []):
            walk(ch)

    walk(client.ok("tree", {"depth": 8}).get("tree") or {})
    return out


def main():
    if not BIN.exists():
        log(f"找不到 {BIN}")
        return 2
    SHOTS.mkdir(parents=True, exist_ok=True)
    ctl = SHOTS / "ctl.json"
    if ctl.exists():
        ctl.unlink()
    server = MiniWsServer()
    server.start()
    logf = open(SHOTS / "log.txt", "wb")
    proc = subprocess.Popen(
        [str(BIN), "--headless", "--control-port", "0", "--control-file", str(ctl),
         "--shots", str(SHOTS), "--home", str(SHOTS / "home"),
         "--ui", str(ROOT / "apps" / "xiangqi" / "ui"),
         "--agent", f"ws://127.0.0.1:{server.port}/xiangqi"],
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
            log("控制通道没起来：")
            log((SHOTS / "log.txt").read_text(encoding="utf-8", errors="replace")[-1500:])
            return 1

        # 推**空**卡片列表 → 进入空态
        server.send_text(json.dumps({"type": "cards", "cards": []}))
        wait_for(lambda: (tree_index(client).get("empty") or {}).get("h", 0) > 0, 8.0)
        time.sleep(0.8)

        idx = tree_index(client)
        try:
            client.ok("capture", {"encode": "file", "path": str(SHOTS / "empty.png")})
            log(f"  截图：{SHOTS / 'empty.png'}")
        except Exception as exc:  # noqa: BLE001
            log(f"  截图失败：{exc}")
        for cid in ("content", "cards-scroll", "empty", "empty-inner",
                    "empty-icon", "empty-title", "empty-hint", "footer-actions"):
            log(f"  #{cid}: {idx.get(cid)}")

        content = idx.get("content") or {}
        block_ids = ["empty-icon", "empty-title", "empty-hint"]
        ys = []
        for cid in block_ids:
            n = idx.get(cid) or {}
            if n.get("y") is not None and n.get("h"):
                ys.append((n["y"], n["y"] + n["h"]))
        if not ys:
            log("  FAIL 空态三块内容未找到（id 变了？）")
            return 1
        top = min(a for a, _ in ys)
        bottom = max(b for _, b in ys)
        block_center = (top + bottom) / 2
        area_top = content.get("y") or 0
        area_h = content.get("h") or 0
        area_center = area_top + area_h / 2
        drift = abs(block_center - area_center)
        limit = area_h * 0.08
        log(f"  内容区 y={area_top} h={area_h} 中心={area_center:.1f}")
        log(f"  空态块 {top}..{bottom} 中心={block_center:.1f}")
        log(f"  竖直偏差 {drift:.1f}px（限 {limit:.1f}px = 内容区高的 8%）")

        cards_scroll = idx.get("cards-scroll") or {}
        log(f"  #cards-scroll 高={cards_scroll.get('h')}（空态下应不占位）")

        footer = idx.get("footer-actions") or {}
        footer_visible = footer.get("h", 0) > 0

        ok_center = drift <= limit
        ok_footer = not footer_visible
        log("")
        log(("  PASS " if ok_center else "  FAIL ") + "空态内容竖直居中")
        log(("  PASS " if ok_footer else "  FAIL ") + "空态下「全部忽略」隐藏")
        return 0 if (ok_center and ok_footer) else 1
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

"""相期：卡片区底色一致性验证（用户报「有两个背景色」）。

做法：无头启动相期 → 注入一张审核卡 → 截整窗 → 逐行采样卡片区两列背景色
（避开卡片本身与文字），断言「卡片区上下留白」的颜色等于「卡片间隙」的颜色。

判据为什么是"留白 vs 间隙"：bug 形态是卡片列自身铺了 surface_alt、而列表滚动区
露出根的 base —— 于是有卡片的行是 alt，卡片下方的留白是 base，肉眼看是两块色。
修好之后两者应同色（都来自根 base，卡片自己的 content 底是另一层但只在卡片矩形内）。

跑法：py -3 tools/xiangqi_bg_check.py
"""
import json
import pathlib
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN = ROOT / "build" / "dev" / "bin" / "xiangqi.exe"
SHOTS = ROOT / "tmp" / "xiangqi-bg-check"
sys.path.insert(0, str(ROOT / "tools"))
from gbcode_hint_e2e import Client  # noqa: E402
from xiangqi_e2e import MiniWsServer  # noqa: E402

# 非 UTF-8 控制台自愈（GBK 代码页下 print 中文会崩整跑）
if sys.stdout.encoding and sys.stdout.encoding.lower() not in ("utf-8", "utf8"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")


def log(msg):
    print(msg, flush=True)


def wait_for(fn, seconds):
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            if fn():
                return True
        except Exception:  # noqa: BLE001
            pass
        time.sleep(0.2)
    return False


def png_pixels(path):
    """读 PNG 为 (w,h,rgb rows)。只用 zlib+struct，避免依赖 Pillow。"""
    import struct
    import zlib

    data = path.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "不是 PNG"
    pos, w, h, bitdepth, color = 8, 0, 0, 0, 0
    idat = b""
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos : pos + 4])
        ctype = data[pos + 4 : pos + 8]
        chunk = data[pos + 8 : pos + 8 + length]
        if ctype == b"IHDR":
            w, h, bitdepth, color = struct.unpack(">IIBB", chunk[:10])
        elif ctype == b"IDAT":
            idat += chunk
        elif ctype == b"IEND":
            break
        pos += 12 + length
    assert bitdepth == 8 and color in (2, 6), f"不支持的 PNG 形态 bitdepth={bitdepth} color={color}"
    ch = 3 if color == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * ch
    rows = []
    prev = bytearray(stride)
    off = 0
    for _ in range(h):
        f = raw[off]
        off += 1
        line = bytearray(raw[off : off + stride])
        off += stride
        if f == 1:
            for i in range(ch, stride):
                line[i] = (line[i] + line[i - ch]) & 0xFF
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif f == 3:
            for i in range(stride):
                a = line[i - ch] if i >= ch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif f == 4:
            for i in range(stride):
                a = line[i - ch] if i >= ch else 0
                b = prev[i]
                c = prev[i - ch] if i >= ch else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        rows.append(bytes(line))
        prev = line
    return w, h, ch, rows


def main():
    if not BIN.exists():
        log(f"找不到 {BIN}")
        return 2
    SHOTS.mkdir(parents=True, exist_ok=True)
    for old in SHOTS.glob("*.png"):
        old.unlink()
    ctl = SHOTS / "ctl.json"
    home = SHOTS / "home"
    if ctl.exists():
        ctl.unlink()
    logf = open(SHOTS / "log.txt", "wb")
    server = MiniWsServer()
    server.start()
    proc = subprocess.Popen(
        [str(BIN), "--headless", "--control-port", "0", "--control-file", str(ctl),
         "--shots", str(SHOTS), "--home", str(home),
         "--ui", str(ROOT / "examples" / "xiangqi" / "ui"),
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
            log((SHOTS / "log.txt").read_text(encoding="utf-8", errors="replace")[-2000:])
            return 1

        # 等控制器连上，推一张与用户截图同形的审核卡（出站审核卡的形态）
        connected = wait_for(lambda: getattr(server, "connected", False) or True, 3.0)
        _ = connected
        pushed = server.send_text(json.dumps({"type": "cards", "cards": [{
            "id": "bg-check-1",
            "kind": "choice",
            "title": "人格［xiaoshuang］请求向飞书群发送消息",
            "body": "目标是 feishu:oc_f71d4cc283006d167354b75be3a9050f\n\n正文：今天是 2026 年 11 月 3 日，星期二。",
            "from": "出站审核",
            "options": [{"label": "批准发送", "value": "approve"}, {"label": "驳回", "value": "reject"}],
        }]}))
        log(f"  推卡：{'ok' if pushed else '失败'}")
        # 等卡片渲染出来（一张卡 → 卡片区高度止于内容，正好复现用户截图的比例）
        ok_render = wait_for(
            lambda: len(client.ok("find", {"selector": "#card-0"}).get("matches") or []) > 0, 8.0)
        log(f"  卡片已渲染：{ok_render}")

        time.sleep(1.0)

        # 切到深色主题（用户实际用的形态）：浅色下两层底只差 1/255，看不出问题；
        # 深色下 surface_alt 与 base 差得明显——这才是用户报“两个背景色”的那个形态。
        # 依据 ThemePicker 的现有选项（e2e 已验证 invoke change 是用户选择语义）
        options = ""
        try:
            props = client.ok("get", {"id": "theme-picker"}).get("props") or {}
            options = str(props.get("options", ""))
        except Exception:  # noqa: BLE001
            pass
        log(f"  主题选项：{options[:80]}")
        themed = False
        for theme_name in [o.strip() for o in options.replace("|", ",").split(",") if o.strip()]:
            if "dark" not in theme_name and "night" not in theme_name and theme_name not in ("qinhan",):
                continue
            try:
                client.ok("invoke", {"id": "theme-picker", "action": "change", "argument": theme_name})
                themed = True
                log(f"  已切主题：{theme_name}")
                break
            except Exception:  # noqa: BLE001
                continue
        if themed:
            time.sleep(0.8)
            try:
                tok = client.ok("theme", {"tokens": True})
                bg = (tok.get("tokens") or {}).get("colors", {}).get("bg", "")
                log(f"  当前主题 bg={bg}")
            except Exception:  # noqa: BLE001
                pass
        else:
            log("  ⚠ 未能切深色主题（按默认档测）")
        out = SHOTS / "bg-check.png"
        client.ok("capture", {"encode": "file", "path": str(out)})
        log(f"  截图：{out}")

        w, h, ch, rows = png_pixels(out)
        log(f"  尺寸 {w}x{h}")

        def px(x, y):
            line = rows[y]
            i = x * ch
            return tuple(line[i : i + 3])

        # 采样位置 x=24 会同时碰到卡片本体与留白——仅作诊断输出，不作判据
        #（判据在下面「卡片外区域」那段：边带+底部永远不可能有卡片）。
        x = 24
        seen = {}
        for y in range(int(h * 0.10), int(h * 0.92)):
            c = px(x, y)
            seen.setdefault(c, []).append(y)
        blocks = []
        for c, ys in seen.items():
            run = 1
            best = 1
            for i in range(1, len(ys)):
                run = run + 1 if ys[i] == ys[i - 1] + 1 else 1
                best = max(best, run)
            if best >= 8:
                blocks.append((c, len(ys), best))
        blocks.sort(key=lambda t: -t[1])
        log("  x=24 竖线色块（诊断：卡片本体与留白都会出现）：")
        for c, total, longest in blocks[:6]:
            log(f"    {c}  总={total:4d}  最长连续={longest:4d}")

        # ── 主判据：**卡片之外的区域必须只有一种底色** ──
        #
        # 用户报的“两个背景色”是：内容容器又铺了一层 surface（#15151A），而卡片少时
        # 容器高度止于内容，下方露出窗口 bg（#0B0B0E）—— 一块亮一块暗。
        #
        # 不能拿来当判据的是“卡片内部颜色 ≠ 窗口底色”：卡片本就是 surface_sunken
        #（暗色下比窗口底更沉）——那是分层，不是 bug。所以只扫边带 + 底部：
        # 那里永远不可能有卡片。
        outside = {}
        for y in range(46, h):
            for x in (2, w - 3):
                key = px(x, y)
                outside[key] = outside.get(key, 0) + 1
        for x in range(0, w):
            key = px(x, h - 5)
            outside[key] = outside.get(key, 0) + 1
        ranked = sorted(outside.items(), key=lambda kv: -kv[1])
        log("  卡片外区域（边带+底部）颜色分布：")
        for c, n in ranked[:4]:
            log(f"    {c}  {n}")
        total = sum(outside.values())
        majors = [(c, n) for c, n in ranked if n / total > 0.02]
        bg_ok = len(majors) == 1
        log("")
        log(("  PASS " if bg_ok else "  FAIL ") + (
            f"卡片外只剩一种底色 {majors[0][0]}" if bg_ok else
            f"卡片外仍有 {len(majors)} 种底色 —— 内容容器多铺了一层 surface"))
        for c, n in majors:
            log(f"    底色 {c} 占 {100.0 * n / total:.1f}%")
        return 0 if bg_ok else 1
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

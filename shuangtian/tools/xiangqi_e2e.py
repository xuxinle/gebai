"""相期（agent 控制的桌面常驻面板）端到端回归。

验证的是**四条接缝**，任一处坏掉应用就不成立：

① XML → 界面树（三种卡片区、徽标、连接状态都在场）
② WebSocket → C++ → JS（agent 下发的卡片真的渲染出来了）
   —— 这是本源最关键的一条：WS 客户端握手/掩码/分帧全自研，mock 掉就测不到
③ JS → C++ → WebSocket（点选项后答复真的回到了 agent 控制器）
④ 断线如实显示（agent 不在时不假装正常）

跑法：py -3 tools/xiangqi_e2e.py
"""

import base64
import json
import pathlib
import shutil
import socket
import subprocess
import sys
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN = ROOT / "build" / "quick" / "bin" / "xiangqi.exe"
UI_DIR = ROOT / "examples" / "xiangqi" / "ui"

sys.path.insert(0, str(ROOT / "tools"))
from gbcode_hint_e2e import Client  # noqa: E402

# 非 UTF-8 控制台自愈：GBK 代码页下 print 中文会 UnicodeEncodeError 崩整跑。
if sys.stdout.encoding and sys.stdout.encoding.lower() not in ("utf-8", "utf8"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")


def log(message):
    print(message, flush=True)


class MiniWsServer:
    """最小 WebSocket 服务端（只够当 agent 控制器）。

    为什么要自己写而不是用第三方库：这个仓库的工具链只依赖标准库
    （`tools/*_e2e.py` 全是 stdlib），引入 websockets 包会破坏"克隆即可跑"。

    实现范围：握手（算 Sec-WebSocket-Accept）+ 收客户端帧（**必须解掩码**）
    + 发未掩码帧。这正是相期客户端需要被验证的对端行为。
    """

    GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

    def __init__(self, port=0):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", port))
        self.sock.listen(1)
        self.port = self.sock.getsockname()[1]
        self.conn = None
        self.received = []
        self._stop = False
        self._thread = None
        self._lock = threading.Lock()

    def start(self):
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()

    def _serve(self):
        try:
            conn, _ = self.sock.accept()
        except OSError:
            return
        self.conn = conn
        try:
            # 握手
            data = b""
            while b"\r\n\r\n" not in data:
                chunk = conn.recv(4096)
                if not chunk:
                    return
                data += chunk
            key = ""
            for line in data.decode("latin-1").split("\r\n"):
                if line.lower().startswith("sec-websocket-key:"):
                    key = line.split(":", 1)[1].strip()
            accept = base64.b64encode(
                __import__("hashlib").sha1((key + self.GUID).encode()).digest()
            ).decode()
            conn.sendall(
                (
                    "HTTP/1.1 101 Switching Protocols\r\n"
                    "Upgrade: websocket\r\n"
                    "Connection: Upgrade\r\n"
                    f"Sec-WebSocket-Accept: {accept}\r\n\r\n"
                ).encode()
            )
            # 收帧循环
            while not self._stop:
                frame = self._read_frame(conn)
                if frame is None:
                    break
                opcode, payload = frame
                if opcode == 0x8:  # close
                    break
                if opcode == 0x1:  # text
                    try:
                        self.received.append(json.loads(payload.decode("utf-8")))
                    except Exception:  # noqa: BLE001
                        self.received.append({"raw": payload.decode("utf-8", "replace")})
        except OSError:
            pass
        finally:
            try:
                conn.close()
            except OSError:
                pass

    def _read_frame(self, conn):
        header = self._recv_exact(conn, 2)
        if header is None:
            return None
        opcode = header[0] & 0x0F
        masked = bool(header[1] & 0x80)
        length = header[1] & 0x7F
        if length == 126:
            ext = self._recv_exact(conn, 2)
            if ext is None:
                return None
            length = int.from_bytes(ext, "big")
        elif length == 127:
            ext = self._recv_exact(conn, 8)
            if ext is None:
                return None
            length = int.from_bytes(ext, "big")
        mask = b""
        if masked:
            mask = self._recv_exact(conn, 4)
            if mask is None:
                return None
        payload = self._recv_exact(conn, length) if length else b""
        if payload is None:
            return None
        if masked and payload:
            # 客户端→服务端必掩码（RFC 6455 §5.1）——不解掩码就会拿到乱码
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        return opcode, payload

    @staticmethod
    def _recv_exact(conn, n):
        buf = b""
        while len(buf) < n:
            try:
                chunk = conn.recv(n - len(buf))
            except OSError:
                return None
            if not chunk:
                return None
            buf += chunk
        return buf

    def send_text(self, text):
        with self._lock:
            if self.conn is None:
                self.send_note = "conn 尚未就绪"
                return False
            data = text.encode("utf-8")
            frame = bytearray([0x81])  # FIN + text
            length = len(data)
            if length < 126:
                frame.append(length)
            elif length < 65536:
                frame.append(126)
                frame += length.to_bytes(2, "big")
            else:
                frame.append(127)
                frame += length.to_bytes(8, "big")
            # 服务端→客户端**不掩码**（RFC §5.1）
            frame += data
            try:
                self.conn.sendall(bytes(frame))
                return True
            except OSError as exc:
                self.send_note = f"sendall 失败：{exc}"
                return False

    def stop(self):
        self._stop = True
        if self.conn is not None:
            try:
                self.conn.close()
            except OSError:
                pass
        try:
            self.sock.close()
        except OSError:
            pass


def wait_for(fn, timeout=6.0, interval=0.15):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if fn():
            return True
        time.sleep(interval)
    return False


def main():
    if not BIN.exists():
        log(f"找不到 {BIN}（先 st build xiangqi --profile quick）")
        return 2

    failures = []

    def check(ok, label):
        log(("  PASS " if ok else "  FAIL ") + label)
        if not ok:
            failures.append(label)

    # ── 场景 A：有 agent 控制器（正常路径）─────────────────────────────────
    log("场景 A：agent 控制器在线")
    server = MiniWsServer()
    server.start()
    log(f"  控制器监听 ws://127.0.0.1:{server.port}/xiangqi")

    shots = ROOT / "tmp" / "xiangqi-e2e"
    shutil.rmtree(shots, ignore_errors=True)
    shots.mkdir(parents=True, exist_ok=True)
    ctl = shots / "ctl.json"
    home = shots / "home"
    handle = open(shots / "log.txt", "wb")
    proc = subprocess.Popen(
        [str(BIN), "--headless", "--control-port", "0", "--control-file", str(ctl),
         "--shots", str(shots), "--home", str(home),
         "--agent", f"ws://127.0.0.1:{server.port}/xiangqi"],
        stdout=handle, stderr=subprocess.STDOUT)

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
            log((shots / "log.txt").read_text(encoding="utf-8", errors="replace")[-2000:])
            return 1

        def find(selector):
            return client.ok("find", {"selector": selector}).get("matches") or []

        def text_of(selector):
            matches = find(selector)
            return matches[0].get("text") if matches else None

        # ① XML → 界面树
        # 注：标题栏元素来自 `WindowFrame`（C++ 装配）而不是 XML；
        # 以前 XML 里还多一条重复的 header 与底部 footer，已合并进标题栏。
        for selector in ("#titlebar", "#badge", "#cards", "#empty", "#link-text", "#dismiss-all"):
            check(len(find(selector)) == 1, f"元素在场：{selector}")
        # 标题由 `WindowFrame("相期")` 给出，不是 XML
        tb = client.ok("get", {"id": "titlebar"}).get("props") or {}
        check(tb.get("title") == "相期", f"标题栏标题正确（{tb.get('title')!r}）")
        check(text_of("#badge") == "暂无待办", f"初始徽标为空态（{text_of('#badge')!r}）")

        # ② WebSocket → 渲染：投三张卡（三种类型）
        cards = [
            {"id": "n1", "kind": "notice", "title": "转码队列已清空",
             "body": "夜间批处理跑完了，共 128 个文件。", "from": "小霜"},
            {"id": "c1", "kind": "choice", "title": "这份报告发到哪个群？",
             "body": "内容已生成，等你选一个去处。", "from": "小霜",
             "options": [{"label": "支持群", "value": "oc_support"},
                         {"label": "内部群", "value": "oc_internal"}]},
            {"id": "t1", "kind": "todo", "title": "把 TSDB 那条慢查询加到监控",
             "body": "阈值建议 3s。", "from": "小霜", "due": "今天"},
        ]
        # 等连接就绪再发（服务端 accept 是异步线程，太早发会 conn=None）
        wait_for(lambda: server.conn is not None, 10.0)
        ok = server.send_text(json.dumps({"type": "cards", "cards": cards}))
        check(ok, "控制器能把卡片推给相期（WS 发送成功）")
        if not ok:
            log(f"  发送失败原因：{getattr(server, 'send_note', '(未知)')}")

        rendered = wait_for(lambda: len(find("#card-0")) > 0 and len(find("#card-2")) > 0, 8.0)
        check(rendered, "三张卡片都渲染出来了（WS → C++ → JS 通路）")
        check(text_of("#badge") == "3 件等你处理",
              f"徽标跟着卡片数更新（{text_of('#badge')!r}）")
        # 连接状态只用图标（省横向空间）：绿色 link = 通。
        # 具体文字挂在 icon 的 hint 里（悬停可见），不再占一行。
        link_icon = (client.ok("get", {"id": "link-icon"}).get("props") or {})
        check(link_icon.get("icon") == "link",
              f"连接状态图标为 link（{link_icon.get('icon')!r}）")

        # 三类卡片的交互区各按类型生成
        # 槽位复用模型下，三个动作按钮一次建齐、按类型显隐；
        # 判据因此看**可见性**而不是存在性（元素在场不等于对用户可见）。
        # `find` 的 match 不带 props，所以用 `get` 单独取属性。
        def visible(element_id):
            """元素是否**存在且可见**。

            重建模型下"不存在"是常态（没生成的按钮就是没有），
            所以这里把 not_found 归为不可见，而不是让整条用例抛异常。
            """
            try:
                reply = client.ok("get", {"id": element_id})
            except AssertionError:
                return False
            props = (reply.get("state") or {}).get("props") or reply.get("props") or {}
            value = props.get("visible", True)
            return str(value).lower() not in ("false", "0")

        check(visible("card-0-act-ok"), "提示卡显示「知道了」按钮")
        check(visible("card-1-act-opt-0") and visible("card-1-act-opt-1"), "选择卡显示两个选项按钮")
        check(visible("card-2-act-done") and visible("card-2-act-later"), "待办卡显示「完成了 / 先放着」")
        # 选择卡**不该**出现「知道了」——那类的价值就是必须定夺
        check(not visible("card-1-act-ok"), "选择卡不显示「知道了」（必须给出选择）")

        # ③ JS → C++ → WebSocket：点第一个选项，答复该回到控制器
        client.ok("invoke", {"id": "card-1-act-opt-0", "action": "click"})
        got = wait_for(lambda: any(m.get("type") == "answer" for m in server.received), 6.0)
        check(got, "点了选项后答复回到 agent 控制器（JS → C++ → WS）")
        if got:
            answer = next(m for m in server.received if m.get("type") == "answer")
            check(answer.get("id") == "c1", f"答复带对了卡片 id（{answer.get('id')!r}）")
            check((answer.get("answer") or {}).get("choice") == "oc_support",
                  f"答复内容正确（{answer.get('answer')!r}）")
        # 答复后卡片应从界面移除。
        # 判据用「重建后的编号重排」而不是"id 消失"：render 是整片重建，
        # 原 c1 移出后，原来的 n1/t1 会重新编号 → #card-1 仍然存在（是 t1）。
        # 所以真正的判据是：卡片总数减一（用 #card-2 不存在来判）。
        # clear + 重建（natural rendering）：答复掉的卡片真的不在了，
        # 原第 3 张会前移成 #card-1，所以判据是"第 3 张不存在"。
        check(wait_for(lambda: len(find("#card-2")) == 0, 4.0), "答复后卡片数减一（#card-2 不存在）")
        check(text_of("#badge") == "2 件等你处理",
              f"徽标跟着减少（{text_of('#badge')!r}）")

        # 截图留证（人工复核用）
        try:
            client.ok("screenshot", {"path": str(shots / "xiangqi-cards.png")})
            log(f"  截图：{shots / 'xiangqi-cards.png'}")
        except Exception as exc:  # noqa: BLE001
            log(f"  截图失败（不影响结论）：{exc}")

        # ③b 轻提示形态：只有 notice 时不拉出主界面
        #
        # 这是"不必每次弹出主界面"的验收：agent 只是通知一声，
        # 就不该把整个工作面板摊到用户面前。
        server.send_text(json.dumps({"type": "cards", "cards": [
            {"id": "n2", "kind": "notice", "title": "夜间批处理完成",
             "body": "128 个文件全部成功。", "from": "小霜"},
        ]}))
        entered = wait_for(lambda: visible("compact"), 6.0)
        check(entered, "只剩 notice 时进入轻提示形态")
        if entered:
            # 标题栏**不隐藏**——徽标/连接状态/全部忽略都住在那里。
            # 以前隐的是 XML 自带的两条冗余带，现在它们已并入标题栏。
            check(visible("badge"), "轻提示形态下徽标仍可见（常驻标题栏）")
            check(visible("dismiss-all"), "轻提示形态下「全部忽略」仍可见")
            title = text_of("#compact-title")
            check(title == "夜间批处理完成", f"轻提示显示最新一条（{title!r}）")

        # ③c 出现 choice → 自动展开（那类必须给足界面做决策）
        server.send_text(json.dumps({"type": "cards", "cards": [
            {"id": "n2", "kind": "notice", "title": "夜间批处理完成",
             "body": "128 个文件全部成功。", "from": "小霜"},
            {"id": "c2", "kind": "choice", "title": "继续跑下一批？",
             "body": "等你定。", "from": "小霜",
             "options": [{"label": "继续", "value": "go"}, {"label": "停下", "value": "stop"}]},
        ]}))
        expanded = wait_for(lambda: not visible("compact"), 6.0)
        check(expanded, "出现选择卡后自动展开主界面")
        if expanded:
            check(visible("card-0") or visible("compact") is False,
                  "展开后回到主界面（compact 已收起）")

        # ③d 轻提示形态下**也必须**能看见连接状态
        #
        # 这一形态把主界面底部栏整体隐了（`#footer`），若轻提示区里没有一份，
        # 断线时整个窗口里没有任何"已断开"的痕迹——用户以为 agent 还在盯着，
        # 而提醒根本没送到。那是比"不弹窗"更糟的状态。
        #
        # 先回到"只有一条 notice"的轻提示形态
        server.send_text(json.dumps({"type": "cards", "cards": [
            {"id": "n3", "kind": "notice", "title": "测试连接状态", "from": "小霜"},
        ]}))
        if wait_for(lambda: visible("compact"), 6.0):
            ci = (client.ok("get", {"id": "compact-link-icon"}).get("props") or {})
            check(ci.get("icon") in ("link", "warning"),
                  f"轻提示形态下显示连接状态图标（{ci.get('icon')!r}）")

        # ④ 断线如实显示：关掉控制器后界面应转成未连接
        server.stop()
        # 等两个轮询周期（500ms × 2）+ 余量，让 tick 感知到断开并重绘
        time.sleep(2.0)
        # 当前形态决定"哪个连接状态可见"：
        # 轻提示形态下 `#footer` 被隐藏，可见的是 `#compact-link`；
        # 主界面形态下才是 `#link-text`。读错那一个会拿到上一次的旧文本。
        icon_id = "compact-link-icon" if visible("compact") else "link-icon"
        icon_props = (client.ok("get", {"id": icon_id}).get("props") or {})
        # 判据：图标从 link 变成 warning（"不再假装连着"这个不变式）
        check(icon_props.get("icon") == "warning",
              f"控制器断开后图标变为 warning（{icon_props.get('icon')!r}）")

    finally:
        try:
            proc.terminate()
            proc.wait(timeout=5)
        except Exception:  # noqa: BLE001
            proc.kill()
        handle.close()
        server.stop()

    # ── 场景 B：没有 agent 控制器（降级路径）───────────────────────────────
    log("场景 B：agent 控制器不在（降级）")
    shots_b = ROOT / "tmp" / "xiangqi-e2e-nolink"
    shutil.rmtree(shots_b, ignore_errors=True)
    shots_b.mkdir(parents=True, exist_ok=True)
    ctl_b = shots_b / "ctl.json"
    handle_b = open(shots_b / "log.txt", "wb")
    # 指向一个确定没人听的端口
    proc_b = subprocess.Popen(
        [str(BIN), "--headless", "--control-port", "0", "--control-file", str(ctl_b),
         "--shots", str(shots_b), "--home", str(shots_b / "home"),
         "--agent", "ws://127.0.0.1:59999/xiangqi"],
        stdout=handle_b, stderr=subprocess.STDOUT)
    try:
        client_b = None
        if wait_for(lambda: ctl_b.exists(), 20.0):
            for _ in range(80):
                try:
                    info = json.loads(ctl_b.read_text(encoding="utf-8"))
                    if info.get("port"):
                        client_b = Client(info["port"], info.get("token", ""))
                        client_b.ok("hello")
                        break
                except Exception:  # noqa: BLE001
                    pass
                time.sleep(0.2)
        # 连不上也必须起得来（常驻应用启动时 agent 常常还没跑）
        check(client_b is not None, "agent 不在时应用仍能启动（不因连不上而崩）")
        if client_b is not None:
            # 等一次 tick 让 render 跑完
            time.sleep(1.5)
            # 连接状态现在是图标：未连接时应该是 warning（而不是 link）。
            props = client_b.ok("get", {"id": "link-icon"}).get("props") or {}
            icon = props.get("icon")
            check(icon == "warning", f"未连接状态如实显示（图标 {icon!r}）")
    finally:
        try:
            proc_b.terminate()
            proc_b.wait(timeout=5)
        except Exception:  # noqa: BLE001
            proc_b.kill()
        handle_b.close()

    # ── 场景 C：页面切主题（ThemePicker）───────────────────────────────
    log("场景 C：页面切主题（ThemePicker + 持久化）")
    shots_c = ROOT / "tmp" / "xiangqi-e2e-theme"
    shutil.rmtree(shots_c, ignore_errors=True)
    shots_c.mkdir(parents=True, exist_ok=True)
    ctl_c = shots_c / "ctl.json"
    handle_c = open(shots_c / "log.txt", "wb")
    proc_c = subprocess.Popen(
        [str(BIN), "--headless", "--control-port", "0", "--control-file", str(ctl_c),
         "--shots", str(shots_c), "--home", str(shots_c / "home")],
        stdout=handle_c, stderr=subprocess.STDOUT)
    try:
        client_c = None
        if wait_for(lambda: ctl_c.exists(), 20.0):
            for _ in range(80):
                try:
                    info = json.loads(ctl_c.read_text(encoding="utf-8"))
                    if info.get("port"):
                        client_c = Client(info["port"], info.get("token", ""))
                        client_c.ok("hello")
                        break
                except Exception:  # noqa: BLE001
                    pass
                time.sleep(0.2)
        check(client_c is not None, "主题场景：应用启动")
        if client_c is not None:
            time.sleep(0.5)
            # 选项来自内置主题库
            props = client_c.ok("get", {"id": "theme-picker"}).get("props") or {}
            options = props.get("options", "")
            check("qinhan" in options and "light" in options,
                  f"ThemePicker 选项含内置主题库（{options[:40]}…）")
            # 切换（invoke change = 用户选择语义，会触发回调与持久化）
            before = client_c.ok("theme", {"tokens": True})
            client_c.ok("invoke", {"id": "theme-picker", "action": "change",
                                   "argument": "qinhan"})
            time.sleep(0.5)
            after = client_c.ok("theme", {"tokens": True})
            check(after.get("name") == "qinhan", f"切后主题名 qinhan（{after.get('name')!r}）")
            bg_before = before.get("tokens", {}).get("colors", {}).get("bg", "")
            bg_after = after.get("tokens", {}).get("colors", {}).get("bg", "")
            check(bg_before != bg_after and bg_after != "",
                  f"调色板真换了（bg {bg_before} -> {bg_after}）")
            # 持久化：theme.txt
            saved = (shots_c / "home" / "theme.txt")
            check(saved.exists() and saved.read_text(encoding="utf-8").strip() == "qinhan",
                  "主题持久化 theme.txt=qinhan")
            # picker 显示同步
            props = client_c.ok("get", {"id": "theme-picker"}).get("props") or {}
            check(props.get("value") == "qinhan", f"picker 选中同步（{props.get('value')!r}）")
    finally:
        try:
            proc_c.terminate()
            proc_c.wait(timeout=5)
        except Exception:  # noqa: BLE001
            proc_c.kill()
        handle_c.close()

    log("")
    if failures:
        log(f"失败 {len(failures)} 项：")
        for item in failures:
            log(f"  - {item}")
        return 1
    log("全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())

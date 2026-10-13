"""终端滚动 E2E（用户报「滚动用不了」的回归护栏）。

跑法：py -3 tools/gbcode_terminal_scroll_e2e.py

覆盖两件容易同时坏的事：
① **尺寸跟随**：面板高度变了，PTY/屏幕行列就要跟着变（否则渲染行数与滚动几何不一致，
   表现为“滚动不准/用不了”）。判据不看内部字段，而是看**屏幕行数与滚动条视口是否同步**：
   把面板压矮 → 屏幕行数必须变少。
② **滚动本身**：滚轮回看（偏移变大）、滚动条 thumb 随之上移、拖滚动条能到底、
   「回到底部」归零；收起/展开之后这些**仍然可用**（保活的另一半）。

定稿过一条教训：判断面板是否可见要看 `#bottom-split` 的 `second_hidden`，
**不能假设“点活动栏 = 打开”**——面板默认就是开的，点一下是**收起**
（早期探针在这里栽过：点击把面板收起，滚轮落在编辑器上，得出“滚动坏了”的假象）。
"""

import base64
import json
import pathlib
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN = ROOT / "build" / "dev" / "bin" / ("gbcode.exe" if __import__("os").name == "nt" else "gbcode")


def log(msg):
    print(msg, flush=True)


class Client:
    def __init__(self, port, token):
        self.port = port
        self.token = token
        self.rid = 0
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=30.0)
        self.sock.settimeout(30.0)

    def _read(self, size):
        buf = b""
        while len(buf) < size:
            chunk = self.sock.recv(size - len(buf))
            if not chunk:
                raise RuntimeError("控制通道提前关闭")
            buf += chunk
        return buf

    def _call(self, method, params=None):
        self.rid += 1
        if method == "hello":
            params = dict(params or {}, token=self.token)
        body = json.dumps({"id": self.rid, "method": method, "params": params or {}}).encode()
        self.sock.sendall(struct.pack(">I", len(body)) + body)
        return json.loads(self._read(struct.unpack(">I", self._read(4))[0]))

    def call(self, method, params=None):
        try:
            return self._call(method, params)
        except (ConnectionAbortedError, ConnectionResetError):
            self.sock.close()
            self.sock = socket.create_connection(("127.0.0.1", self.port), timeout=30.0)
            self.sock.settimeout(30.0)
            self.rid = 0
            self._call("hello")
            return self._call(method, params)

    def ok(self, method, params=None):
        reply = self.call(method, params)
        if not reply.get("ok"):
            raise AssertionError(f"{method} 失败: {reply.get('error')}")
        return reply.get("result") or {}

    def bounds(self, selector):
        matches = self.ok("find", {"selector": selector}).get("matches") or []
        return matches[0]["bounds"] if matches else None

    def props(self, element_id):
        return self.ok("get", {"id": element_id})["props"]

    def click(self, selector):
        box = self.bounds(selector)
        if box is None:
            raise AssertionError(f"找不到元素 {selector}")
        self.ok("input.mouse", {"kind": "click", "x": box["x"] + box["width"] / 2.0,
                                "y": box["y"] + box["height"] / 2.0, "button": 1})
        time.sleep(0.5)

    def wheel(self, x, y, delta, times=1):
        for _ in range(times):
            self.ok("input.mouse", {"kind": "wheel", "x": x, "y": y, "delta": delta})
            time.sleep(0.1)

    def screenshot(self, path):
        pathlib.Path(path).write_bytes(base64.b64decode(self.ok("capture", {})["base64"]))


def wait_until(fn, timeout=25.0, what="条件"):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        try:
            last = fn()
            if last:
                return last
        except Exception as exc:  # noqa: BLE001
            last = exc
        time.sleep(0.25)
    raise AssertionError(f"等待超时：{what}（最后：{last!r}）")


def start(workspace):
    shots = ROOT / "tmp" / "gbcode-terminal-shots"
    shots.mkdir(parents=True, exist_ok=True)
    ctl = shots / "gbcode-scroll-ctl.json"
    ctl.unlink(missing_ok=True)
    handle = open(shots / "gbcode-scroll.log", "wb")
    proc = subprocess.Popen(
        [str(BIN), "--headless", "--control-port", "0", "--control-file", str(ctl),
         "--shots", str(shots), "--tool-root", str(ROOT), "--workspace", str(workspace)],
        stdout=handle, stderr=subprocess.STDOUT)
    for _ in range(160):
        if ctl.exists():
            try:
                info = json.loads(ctl.read_text(encoding="utf-8"))
                if info.get("port"):
                    client = Client(info["port"], info.get("token", ""))
                    client.ok("hello")
                    return proc, client, shots
            except Exception:  # noqa: BLE001
                pass
        time.sleep(0.25)
    proc.kill()
    raise AssertionError("控制通道没起来")


def parse_scroll(props):
    raw = props.get("scroll", "0:0")
    offset, _, total = (raw.partition(":"))
    return int(offset or 0), int(total or 0)


def parse_bar(props):
    raw = props.get("scrollbar", "")
    if not raw:
        return None
    content, view, offset = (float(v) for v in raw.split(":"))
    return content, view, offset


def screen_rows(props):
    screen = props.get("screen", "")
    return screen.count("\n") + 1 if screen else 0


def panel_open(client):
    """面板是否**在显示态**——看分栏的 second_hidden，不猜“点一下就是打开”。"""
    return client.props("bottom-split").get("second_hidden") == "false"


def main():
    if not BIN.exists():
        log(f"找不到 {BIN}")
        return 2

    work = pathlib.Path(tempfile.mkdtemp(prefix="gbcode_term_scroll_"))
    repo = work / "repo"
    repo.mkdir()
    subprocess.run(["git", "init", "-q", "-b", "main"], cwd=repo, capture_output=True)
    (repo / "a.txt").write_text("x\n", encoding="utf-8")

    failures = []

    def check(ok, label):
        log(("  PASS " if ok else "  FAIL ") + label)
        if not ok:
            failures.append(label)

    proc = None
    try:
        proc, client, shots = start(repo)
        check(panel_open(client), "前置：终端面板处于显示态")
        client.ok("invoke", {"id": "terminal", "action": "focus"})
        # 长输出命令按平台选（Windows PowerShell / POSIX shell 语法不同）：
        import platform as _platform
        flood = ("1..120 | % { \"Scroll_$_\" }" if _platform.system() == "Windows"
                 else "for i in $(seq 1 120); do echo \"Scroll_$i\"; done")
        client.ok("invoke", {"id": "terminal", "action": "send_line", "argument": flood})
        time.sleep(2.0)

        props = client.props("terminal")
        offset, total = parse_scroll(props)
        rows_full = screen_rows(props)
        check(total > 40, f"长输出产生了回看历史（total={total}）")

        box = client.bounds("#terminal")
        cx = box["x"] + box["width"] / 2.0
        cy = box["y"] + box["height"] / 2.0

        # ① 滚轮回看
        client.wheel(cx, cy, 1.0, times=5)
        time.sleep(0.4)
        props = client.props("terminal")
        offset_now, _ = parse_scroll(props)
        bar = parse_bar(props)
        check(offset_now > 0, f"滚轮进入回看（offset={offset_now}）")
        check(bar is not None and bar[0] > bar[1], f"回看时滚动条几何自洽（{bar}）")

        # ② 拖滚动条到顶
        bar_x = box["x"] + box["width"] - 5.0
        client.ok("input.mouse", {"kind": "down", "x": bar_x,
                                  "y": box["y"] + box["height"] * 0.6, "button": 1})
        time.sleep(0.3)
        client.ok("input.mouse", {"kind": "move", "x": bar_x, "y": box["y"] + 30.0, "button": 1})
        time.sleep(0.3)
        client.ok("input.mouse", {"kind": "up", "x": bar_x, "y": box["y"] + 30.0, "button": 1})
        time.sleep(0.5)
        dragged, _ = parse_scroll(client.props("terminal"))
        check(dragged > offset_now, f"拖滚动条能继续翻（{offset_now} → {dragged}）")

        # ③ 回到底部
        client.ok("invoke", {"id": "terminal", "action": "scroll_to_end"})
        time.sleep(0.4)
        check(parse_scroll(client.props("terminal"))[0] == 0, "「回到底部」把偏移归零")

        # ④ 收起 → 展开：滚动仍然可用
        client.click("#activity-terminal")
        wait_until(lambda: not panel_open(client), what="面板收起")
        client.click("#activity-terminal")
        wait_until(lambda: panel_open(client), what="面板展开")
        box = client.bounds("#terminal")
        cx = box["x"] + box["width"] / 2.0
        cy = box["y"] + box["height"] / 2.0
        client.wheel(cx, cy, 1.0, times=5)
        time.sleep(0.4)
        after_reopen, _ = parse_scroll(client.props("terminal"))
        check(after_reopen > 0, f"收起再展开后滚动仍可用（offset={after_reopen}）")
        client.ok("invoke", {"id": "terminal", "action": "scroll_to_end"})
        time.sleep(0.3)

        # ⑤ **尺寸跟随**：把面板压矮 → 屏幕行数必须变少（否则渲染与滚动几何不一致）
        split = client.bounds("#bottom-split")
        handle_x = split["x"] + split["width"] / 2.0
        handle_y = split["y"] + split["height"] * 0.55
        client.ok("input.mouse", {"kind": "down", "x": handle_x, "y": handle_y, "button": 1})
        client.ok("input.mouse", {"kind": "move", "x": handle_x, "y": handle_y + 150, "button": 1})
        time.sleep(0.4)
        client.ok("input.mouse", {"kind": "up", "x": handle_x, "y": handle_y + 150, "button": 1})
        time.sleep(1.6)
        rows_short = screen_rows(client.props("terminal"))
        check(rows_short < rows_full,
              f"面板变矮后屏幕行数跟着变（{rows_full} → {rows_short}）")
        # 压矮后仍能滚（几何与所见一致才可能滚得对）
        box = client.bounds("#terminal")
        client.wheel(box["x"] + box["width"] / 2.0, box["y"] + box["height"] / 2.0, 1.0, times=4)
        time.sleep(0.4)
        check(parse_scroll(client.props("terminal"))[0] > 0, "压矮后仍能滚回看")

        shot = shots / "terminal-scroll.png"
        client.screenshot(shot)
        check(shot.exists() and shot.stat().st_size > 2000, f"截图落盘 {shot}")
    finally:
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        shutil.rmtree(work, ignore_errors=True)

    log("")
    if failures:
        log(f"失败 {len(failures)} 项：")
        for item in failures:
            log("  - " + item)
        return 1
    log("全部通过。")
    return 0


if __name__ == "__main__":
    sys.exit(main())

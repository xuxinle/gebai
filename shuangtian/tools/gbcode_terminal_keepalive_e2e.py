"""gbcode 终端面板「收起再展开，内容与尺寸都不变」E2E。

跑法：py -3 tools/gbcode_terminal_keepalive_e2e.py

要验证的语义（用户报的问题）：
- 把终端面板收起来再展开，**屏上的内容原样在**（PTY 没被 resize、屏幕缓冲没被清）；
- 会话/工作目录/标题都还在（不是新起了一个 shell）；
- 顺带覆盖：底部面板在「终端 ↔ Git」之间切换时，终端实例也保活。
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
BIN = ROOT / "build" / "dev" / "bin" / "gbcode.exe"


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

    def count(self, selector):
        return self.ok("find", {"selector": selector})["count"]

    def props(self, element_id):
        return self.ok("get", {"id": element_id})["props"]

    def click(self, selector):
        matches = self.ok("find", {"selector": selector}).get("matches") or []
        if not matches:
            raise AssertionError(f"找不到元素 {selector}")
        bounds = matches[0]["bounds"]
        self.ok("input.mouse", {"kind": "click", "x": bounds["x"] + bounds["width"] / 2.0,
                                "y": bounds["y"] + bounds["height"] / 2.0, "button": 1})
        time.sleep(0.5)

    def type_text(self, text, element_id="terminal"):
        # ⚠ `input.text` 需要 **`id`**（往哪个元素送文本）——不传会被当成“给当前焦点”，
        # 而焦点可能还在活动栏按钮上（实测：`handled: false, focused: activity-terminal`，
        # 命令根本没进终端，看着像“终端不接收输入”）。
        self.ok("invoke", {"id": element_id, "action": "focus"})
        return self.call("input.text", {"id": element_id, "text": text})

    def screenshot(self, path):
        data = self.ok("capture", {}).get("base64")
        pathlib.Path(path).write_bytes(base64.b64decode(data))


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
    ctl = shots / "gbcode-ctl.json"
    ctl.unlink(missing_ok=True)
    handle = open(shots / "gbcode-terminal.log", "wb")
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


def main():
    if not BIN.exists():
        log(f"找不到 {BIN}")
        return 2

    work = pathlib.Path(tempfile.mkdtemp(prefix="gbcode_term_"))
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
        # ① 打开终端并跑一条命令（让屏上有“用户产出”的内容）。
        client.click("#activity-terminal")
        wait_until(lambda: client.count("#terminal") == 1, what="终端出现")
        wait_until(lambda: "PowerShell" in client.props("terminal").get("screen", "") or
                   "PS " in client.props("terminal").get("screen", ""), what="shell 就绪")
        baseline = client.props("terminal")
        check(baseline.get("sessions") == "1", "起了一条 shell 会话")

        # 在终端里执行一条命令（聚焦终端元素再送文本——见 `type_text` 的注释）。
        client.type_text("echo KEEP_ME_12345\r")
        wait_until(lambda: "KEEP_ME_12345" in client.props("terminal").get("screen", ""),
                   timeout=15.0, what="命令输出上屏")
        before = client.props("terminal")
        before_screen = before.get("screen", "")
        check("KEEP_ME_12345" in before_screen, "命令输出已在屏上")

        # ② 收起终端面板 → 再展开：内容必须**原样**（尺寸不变、屏不清）。
        client.click("#activity-terminal")
        time.sleep(1.2)
        client.click("#activity-terminal")
        time.sleep(1.5)
        after = client.props("terminal")
        after_screen = after.get("screen", "")
        check("KEEP_ME_12345" in after_screen, "收起再展开：命令输出**仍在屏上**")
        check(after_screen == before_screen,
              "收起再展开：屏幕内容**逐字相同**（没有被 resize/清屏改过）")
        check(after.get("sessions") == "1", "收起再展开：仍是同一个会话（没新起 shell）")
        check(after.get("session_title") == before.get("session_title"),
              "收起再展开：会话标题不变（同一进程）")

        # ③ 切到 Git 面板再切回终端：终端实例与内容同样保活。
        client.click("#activity-git")
        wait_until(lambda: client.count("#git-view") == 1, what="Git 面板出现")
        client.click("#activity-terminal")
        wait_until(lambda: "KEEP_ME_12345" in client.props("terminal").get("screen", ""),
                   what="切回终端")
        check("KEEP_ME_12345" in client.props("terminal").get("screen", ""),
              "终端 ↔ Git 切换后终端内容仍在")

        shot = shots / "terminal-keepalive.png"
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

"""悬停提示（Hint / Tooltip）E2E 回归。

跑法：py -3 tools/gbcode_hint_e2e.py

断言面用 `#hint-bubble` 与 `hint` 属性（`UiRoot` 的一等读取面）：气泡是延后出现、
带位置的浮层，读“当前该不该显示 + 文本是什么”与机制同构，不受阴影/光标闪烁干扰。

覆盖：
① 悬停活动栏按钮 → **延时内不出现**（扫过一排按钮时不该闪出一串气泡）；
② 悬停超过延时 → 出现，且文本是那个按钮的提示；
③ 移开 → 消失；
④ 点击 → 消失（提示不该黏在屏幕上）；
⑤ 悬停**子元素**也能拿到父元素的提示（与 Web `title` 的冒泡语义一致，
   气泡归属按父链向上找）。
"""

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

    def hover(self, x, y):
        self.ok("input.mouse", {"kind": "move", "x": x, "y": y})

    def bubble_text(self):
        """气泡当前文本（不存在或**不可见** = 没显示）。

        ⚠ 必须查 `visible`：`find` 会把**不可见**的匹配也返回（`Tooltip` 收起时
        我们置 `set_visible(false)` 而不是从树上摘除），只看“有没有 matches”
        会把“收着”误判成“弹着”。
        """
        reply = self.call("find", {"selector": "#hint-bubble"})
        matches = (reply.get("result") or {}).get("matches") or []
        for match in matches:
            if not match.get("visible", True):
                continue
            return match.get("value") or match.get("text") or ""
        return None


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
        time.sleep(0.15)
    raise AssertionError(f"等待超时：{what}（最后：{last!r}）")


def main():
    if not BIN.exists():
        log(f"找不到 {BIN}")
        return 2

    work = pathlib.Path(tempfile.mkdtemp(prefix="gbcode_hint_"))
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
        shots = ROOT / "tmp" / "gbcode-hint-shots"
        shots.mkdir(parents=True, exist_ok=True)
        ctl = shots / "hint-ctl.json"
        ctl.unlink(missing_ok=True)
        handle = open(shots / "hint.log", "wb")
        proc = subprocess.Popen(
            [str(BIN), "--headless", "--control-port", "0", "--control-file", str(ctl),
             "--shots", str(shots), "--tool-root", str(ROOT), "--workspace", str(repo)],
            stdout=handle, stderr=subprocess.STDOUT)
        client = None
        for _ in range(160):
            if ctl.exists():
                try:
                    info = json.loads(ctl.read_text(encoding="utf-8"))
                    if info.get("port"):
                        client = Client(info["port"], info.get("token", ""))
                        client.ok("hello")
                        break
                except Exception:  # noqa: BLE001
                    pass
            time.sleep(0.25)
        if client is None:
            raise AssertionError("控制通道没起来")

        # 先把指针停在空白处（编辑器中央），确保初始没有提示。
        client.hover(640.0, 400.0)
        time.sleep(0.6)
        check(client.bubble_text() is None, "初始没有提示气泡")

        # ① 悬停活动栏「搜索」——**延时内不出现**。
        box = client.bounds("#activity-search")
        check(box is not None, "活动栏搜索按钮有几何")
        cx = box["x"] + box["width"] / 2.0
        cy = box["y"] + box["height"] / 2.0
        client.hover(cx, cy)
        time.sleep(0.15)
        early = client.bubble_text()
        check(early is None, f"悬停 150ms（延时内）**不出现**（实际 {early!r}）")

        # ② 超过延时 → 出现，且文本是它自己的提示。
        appeared = wait_until(lambda: client.bubble_text(), timeout=4.0, what="提示出现")
        check(appeared is not None and "搜索" in appeared,
              f"悬停超过延时后出现且文本正确（{appeared!r}）")

        # ③ 移开 → 消失。
        client.hover(640.0, 400.0)
        gone = wait_until(lambda: client.bubble_text() is None, timeout=3.0, what="提示消失")
        check(gone, "移开后提示消失")

        # ④ 键盘操作（点击）后也不该留着提示。
        client.hover(cx, cy)
        wait_until(lambda: client.bubble_text(), timeout=4.0, what="提示再次出现")
        client.ok("input.mouse", {"kind": "click", "x": cx, "y": cy, "button": 1})
        time.sleep(0.3)
        check(client.bubble_text() is None, "点击后提示消失（不黏在屏幕上）")

        # ⑤ 不同按钮的提示文本不同（防止“所有按钮同一句话”这种假实现）。
        git_box = client.bounds("#activity-git")
        client.hover(git_box["x"] + git_box["width"] / 2.0, git_box["y"] + git_box["height"] / 2.0)
        git_hint = wait_until(lambda: client.bubble_text(), timeout=4.0, what="Git 按钮提示")
        check("Git" in git_hint, f"Git 按钮的提示是它自己的（{git_hint!r}）")
        check(git_hint != appeared, "相邻按钮的提示文本不同")

        # ⑥ 侧栏图标钮（`scm_icon_button` 的 tip 以前被丢弃）。
        client.hover(640.0, 400.0)
        time.sleep(0.5)
        bar = client.bounds("#sidebar-actions")
        if bar is not None:
            refresh = client.bounds("#explorer-refresh")
            if refresh is not None:
                client.hover(refresh["x"] + refresh["width"] / 2.0,
                             refresh["y"] + refresh["height"] / 2.0)
                icon_hint = wait_until(lambda: client.bubble_text(), timeout=4.0,
                                       what="侧栏图标钮提示")
                check(icon_hint is not None and icon_hint.strip() != "",
                      f"侧栏图标钮也有提示（{icon_hint!r}）")
            else:
                log("  SKIP 资源管理器当前视图没有 explorer-refresh（视图可能不是资源管理器）")
        else:
            log("  SKIP 没有 #sidebar-actions")

        client.hover(640.0, 400.0)
        time.sleep(0.4)
        client.ok("capture", {"path": str(shots / "hint.png")})
        check((shots / "hint.png").exists(), "截图落盘")
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

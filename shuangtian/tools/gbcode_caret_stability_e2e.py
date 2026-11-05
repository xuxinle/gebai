"""光标稳定性验证：在差异视图里连续打字，光标必须留在原地（不跳回行首）。

为何要单独一个脚本：这个缺陷**改完看起来一切正常**——文本对、差异对、摘要对，
只有“正在打字的人”会发现光标跳了。它是 `set_text`（换文档语义：光标归 0、
滚动回顶、撤销栈清空）被用在“重新合成同一份文本”上的后果。

跑法：py -3 tools/gbcode_caret_stability_e2e.py
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

if sys.stdout.encoding and sys.stdout.encoding.lower() not in ("utf-8", "utf8"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN = ROOT / "build" / "dev" / "bin" / ("gbcode.exe" if __import__("os").name == "nt" else "gbcode")
SHOTS = ROOT / "tmp" / "gbcode-caret-shots"


class Client:
    def __init__(self, port, token):
        self.port = port
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=30.0)
        self.sock.settimeout(30.0)
        self.rid = 0
        self.token = token

    def _call(self, method, params=None):
        self.rid += 1
        if method == "hello":
            params = dict(params or {}, token=self.token)
        body = json.dumps({"id": self.rid, "method": method, "params": params or {}}).encode()
        self.sock.sendall(struct.pack(">I", len(body)) + body)
        payload = self._read(struct.unpack(">I", self._read(4))[0])
        return json.loads(payload)

    def _read(self, size):
        buf = b""
        while len(buf) < size:
            chunk = self.sock.recv(size - len(buf))
            if not chunk:
                raise RuntimeError("控制通道提前关闭")
            buf += chunk
        return buf

    def ok(self, method, params=None):
        reply = self._call(method, params)
        if not reply.get("ok"):
            raise AssertionError(f"{method} 失败: {reply.get('error')}")
        return reply.get("result") or {}

    def count(self, selector):
        return self.ok("find", {"selector": selector})["count"]

    def props(self, element_id):
        return self.ok("get", {"id": element_id})["props"]

    def set_prop(self, element_id, name, value):
        return self.ok("set", {"id": element_id, "props": {name: value}})

    def key(self, key, **mods):
        return self.ok("input.key", {"key": key, **mods})


def git(cwd, *args):
    subprocess.run(["git", *args], cwd=cwd, capture_output=True)


def start(workspace):
    SHOTS.mkdir(parents=True, exist_ok=True)
    ctl = SHOTS / "ctl.json"
    ctl.unlink(missing_ok=True)
    log_file = open(SHOTS / "caret.log", "wb")
    proc = subprocess.Popen(
        [str(BIN), "--headless", "--control-port", "0", "--control-file", str(ctl),
         "--shots", str(SHOTS), "--tool-root", str(ROOT), "--workspace", str(workspace)],
        stdout=log_file, stderr=subprocess.STDOUT)
    for _ in range(160):
        if ctl.exists():
            try:
                info = json.loads(ctl.read_text(encoding="utf-8"))
                if info.get("port"):
                    client = Client(info["port"], info.get("token", ""))
                    client.ok("hello")
                    return proc, client
            except Exception:  # noqa: BLE001
                pass
        time.sleep(0.25)
    proc.kill()
    raise AssertionError("控制通道没起来（看 tmp/gbcode-caret-shots/caret.log）")


def main():
    if not BIN.exists():
        print(f"找不到 {BIN}；先 `st build gbcode --profile dev`")
        return 2

    work = pathlib.Path(tempfile.mkdtemp(prefix="gbcode_caret_"))
    repo = work / "repo"
    repo.mkdir()
    git(repo, "init", "-q", "-b", "main")
    git(repo, "config", "user.email", "e2e@local")
    git(repo, "config", "user.name", "E2E")
    lines = [f"line {i}" for i in range(1, 41)]
    (repo / "alpha.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    git(repo, "add", ".")
    git(repo, "commit", "-qm", "first")
    lines[1] = "line 2 CHANGED"
    (repo / "alpha.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")

    failures = []
    proc = None

    def check(ok, label):
        print(("  PASS " if ok else "  FAIL ") + label)
        if not ok:
            failures.append(label)

    try:
        proc, client = start(repo)
        # 打开变更面板 → 点该文件 → 差异视图
        client.ok("invoke", {"id": "activity-scm", "action": "click"})
        time.sleep(1.2)
        if client.count("[id=git-file-alpha.txt]"):
            client.ok("invoke", {"id": "git-file-alpha.txt", "action": "click"})
        time.sleep(1.4)
        check(client.count("#diff-view") == 1, "差异视图已打开")
        check(client.count("#diff-right") == 1, "右栏存在（可编辑那一侧）")

        # 光标放到**行中**（不是行首也不是行尾）：用户报告的现象是
        # “跑到同一行的倒数第二个字符之后”。只在行首打字测不出行内定位对不对。
        right_text = client.props("diff-right").get("text") or ""
        lines_now = right_text.split("\n")
        target_line = 5
        line_start = sum(len(item) + 1 for item in lines_now[:target_line - 1])
        # 放在第 5 行第 3 列（“line 5” 中间，前面两个字符）
        target_offset = line_start + 2
        client.set_prop("diff-right", "cursor", str(target_offset))
        time.sleep(0.3)
        # ⚠ 必须**在设完位置后重新聚焦**：输入事件只看焦点元素，
        # 而控制通道的 `set` 不保证焦点还在该编辑器上（实测：不聚焦时 `input.text`
        # 静默落地无收——文本一个字也没进，光标自然也不动）。
        client.ok("invoke", {"id": "diff-right", "action": "focus"})
        time.sleep(0.3)
        before_line = client.props("diff-right").get("line")
        before_col = int(client.props("diff-right").get("column") or 0)
        print(f"  初始位置 行{before_line} 列{before_col}（偏移 {target_offset}）")
        check(int(before_line or 0) == target_line, f"光标定位到第 {target_line} 行（实际 {before_line}）")
        check(before_col == 3, f"光标定位到第 3 列（实际 {before_col}）")

        # 连续打三个字符（每次输入都会触发“重算 + 重排文本”）
        #
        # 断言用**行列**而不是字节偏移：填充行会让同一位置在两版文本里偏移不同，
        # 而行列就是用户看到的光标位置（缺陷现象也正是“光标跑到行首”）。
        for index in range(3):
            client.ok("input.text", {"text": chr(ord("X") + index)})
            # 等到“重算完成”：右栏文本出现刚打的字（防抖 + 重组需要一个帧循环）。
            want = chr(ord("X") + index)
            for _ in range(40):
                if want in (client.props("diff-right").get("text") or ""):
                    break
                time.sleep(0.1)
            time.sleep(0.25)   # 再等一拍让光标位置稳定
            now_line = client.props("diff-right").get("line")
            now_col = int(client.props("diff-right").get("column") or 0)
            started = int(before_col or 0)
            check(now_line == before_line,
                  f"第 {index + 1} 次输入后光标未跳行（行 {now_line}，期望 {before_line}）")
            check(now_col == started + index + 1,
                  f"第 {index + 1} 次输入后光标紧跟刚打的字（列 {now_col}，"
                  f"期望 {started + index + 1}）")
        # 先把剩余检查写完再断言文本
        # （文本内容本身由差异视图自己的 e2e 衡，这里只盯光标）

        # —— 真正会触发“重排”的动作：在光标处**回车** ——
        #
        # 为何单靠打字不够（逆向验证发现）：纯打字时右栏文本没变结构，
        # `push_to_editors` 的文本相等早退，`set_text` 那种“光标归 0”根本不会发生。
        # 回车则改变行数 ⇒ 差异结构变 ⇒ 两栏文本真的重新合成 ⇒ 才会走到那条路。
        before_line_num = int(client.props("diff-right").get("line") or 1)
        before_col_num = int(client.props("diff-right").get("column") or 1)
        client.ok("input.key", {"key": "Enter"})
        time.sleep(0.9)
        after_line_num = int(client.props("diff-right").get("line") or 1)
        after_col_num = int(client.props("diff-right").get("column") or 1)
        check(after_line_num > before_line_num,
              f"回车后光标下到新行（行 {before_line_num} → {after_line_num}）")
        check(after_col_num <= 2,
              f"回车后光标在新行行首附近（列 {after_col_num}，而不是跑到别处）")

        # 撤销栈不能被清空：重算若走了 `set_text`（换文档语义）会把历史清掉，
        # 用户打字时撤销键就静默失灵了。
        client.ok("input.key", {"key": "z", "ctrl": True})
        time.sleep(0.5)
        now_col = int(client.props("diff-right").get("column") or 0)
        check(now_col >= int(before_col or 0),
              f"撤销之后光标未跑到行首（列 {now_col}，初始 {before_col}）")
    finally:
        if proc is not None:
            try:
                proc.terminate()
                proc.wait(timeout=10)
            except Exception:  # noqa: BLE001
                pass
        shutil.rmtree(work, ignore_errors=True)

    print(f"\n失败 {len(failures)} 项")
    for item in failures:
        print(" -", item)
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())

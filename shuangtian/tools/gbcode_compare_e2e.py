"""gbcode 对比能力 E2E（无头 + 控制通道）：比较视图 → 文件清单 → 并列差异 → 导航。

跑法：py -3 tools/gbcode_compare_e2e.py
自带一个真 git 仓库（临时目录，用完即删），启动 gbcode --headless 后走控制通道驱动界面。

与 `gbcode_e2e.py` 的关系：那份是全量走查（29 组，含终端/菜单/主题），本脚本只覆盖
**对比能力这一条链路**——它能独立跑、失败时定位清楚，也避免改一处就要跑整套。
"""

import base64
import json
import os
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


def git(cwd, *args):
    return subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True)


class Client:
    """控制通道客户端（长度前缀 JSON，与 `gbcode_e2e.py` 同一套）。"""

    def __init__(self, port, token):
        self.port = port
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=30.0)
        self.sock.settimeout(30.0)
        self.rid = 0
        self.token = token

    def call(self, method, params=None):
        try:
            return self._call(method, params)
        except ConnectionAbortedError:
            # 过渡帧里查询可能被当场拒绝（应用正在重组）：**重连再试一次**。
            # 不这么做的话，一个偶发断连会把整条 e2e 当成“功能坏了”。
            self.sock.close()
            self.sock = socket.create_connection(("127.0.0.1", self.port), timeout=30.0)
            self.sock.settimeout(30.0)
            self.rid = 0
            self._call("hello")
            return self._call(method, params)

    def _call(self, method, params=None):
        self.rid += 1
        if method == "hello":
            params = dict(params or {}, token=self.token)
        body = json.dumps({"id": self.rid, "method": method, "params": params or {}}).encode()
        self.sock.sendall(struct.pack(">I", len(body)) + body)
        header = self._read(4)
        payload = self._read(struct.unpack(">I", header)[0])
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
        reply = self.call(method, params)
        if not reply.get("ok"):
            raise AssertionError(f"{method} 失败: {reply.get('error')}")
        return reply.get("result") or {}

    def count(self, selector):
        return self.ok("find", {"selector": selector})["count"]

    def props(self, element_id):
        return self.ok("get", {"id": element_id})["props"]

    def set_prop(self, element_id, name, value):
        # 控制通道的 `set` 收的是 **props 对象**（`{"props": {名: 值}}`），
        # 不是单个 `property`/`value`（实测：后者报 “props 必须是对象”）。
        return self.ok("set", {"id": element_id, "props": {name: value}})

    def invoke(self, element_id, action, argument=""):
        return self.ok("invoke", {"id": element_id, "action": action, "argument": argument})

    def click(self, selector):
        found = self.ok("find", {"selector": selector})
        matches = found.get("matches") or []
        if not matches:
            raise AssertionError(f"找不到元素 {selector}")
        bounds = matches[0]["bounds"]
        x = bounds["x"] + bounds["width"] / 2.0
        y = bounds["y"] + bounds["height"] / 2.0
        self.ok("input.mouse", {"kind": "click", "x": x, "y": y, "button": 1})
        time.sleep(0.45)

    def screenshot(self, path):
        result = self.ok("capture", {})
        # 控制通道的 `capture` 返回 `base64`（另有 `bytes`/`format`/`region` 元信息）。
        data = (result.get("base64") or result.get("png_base64") or result.get("data")
                or result.get("png"))
        if not data:
            raise AssertionError(f"截图返回里没有图像数据：{sorted(result.keys())}")
        pathlib.Path(path).write_bytes(base64.b64decode(data))


def wait_until(fn, timeout=25.0, interval=0.25, what="条件"):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        try:
            last = fn()
            if last:
                return last
        except Exception as exc:  # noqa: BLE001
            last = exc
        time.sleep(interval)
    raise AssertionError(f"等待超时：{what}（最后：{last!r}）")


def start(workspace):
    shots = ROOT / "tmp" / "gbcode-compare-shots"
    shots.mkdir(parents=True, exist_ok=True)
    ctl = shots / "gbcode-ctl.json"
    ctl.unlink(missing_ok=True)
    log_file = open(shots / "gbcode-compare.log", "wb")
    proc = subprocess.Popen(
        [str(BIN), "--headless", "--control-port", "0", "--control-file", str(ctl),
         "--shots", str(shots), "--tool-root", str(ROOT), "--workspace", str(workspace)],
        stdout=log_file, stderr=subprocess.STDOUT)
    for _ in range(160):
        if ctl.exists():
            try:
                info = json.loads(ctl.read_text(encoding="utf-8"))
                if info.get("port"):
                    client = Client(info["port"], info.get("token", ""))
                    # 控制通道要求**先握手**：不 hello 的调用一律被拒
                    #（实测报 “必须先 hello（携带 token）”，而它看起来像“选择器不对”）。
                    client.ok("hello")
                    return proc, client, info, shots
            except Exception:  # noqa: BLE001
                pass
        time.sleep(0.25)
    proc.kill()
    raise AssertionError("gbcode 控制通道没起来（看 tmp/gbcode-compare-shots/gbcode-compare.log）")


def main():
    if not BIN.exists():
        log(f"找不到 {BIN}；先 `st build gbcode --profile dev`")
        return 2

    work = pathlib.Path(tempfile.mkdtemp(prefix="gbcode_cmp_"))
    repo = work / "repo"
    repo.mkdir()
    git(repo, "init", "-q", "-b", "main")
    git(repo, "config", "user.email", "e2e@local")
    git(repo, "config", "user.name", "E2E")
    (repo / "alpha.txt").write_text("one\ntwo\nthree\nfour\nfive\n", encoding="utf-8")
    (repo / "beta.txt").write_text("bee\n", encoding="utf-8")
    git(repo, "add", ".")
    git(repo, "commit", "-qm", "first")
    (repo / "alpha.txt").write_text("one\nTWO\nthree\nfour\nfive\nsix\n", encoding="utf-8")
    git(repo, "add", ".")
    git(repo, "commit", "-qm", "second")
    # 工作区再改一处（未暂存）——比较视图的默认两端能看到它。
    (repo / "alpha.txt").write_text("one\nTWO\nthree\nfour!\nfive\nsix\n", encoding="utf-8")

    failures = []

    def check(ok, label):
        log(("  PASS " if ok else "  FAIL ") + label)
        if not ok:
            failures.append(label)

    proc = None
    try:
        proc, client, info, shots = start(repo)
        wait_until(lambda: client.count("#activity-compare") == 1, what="活动栏出现「比较」")

        # ① 比较视图
        client.click("#activity-compare")
        wait_until(lambda: client.count("#compare-view") == 1, what="比较视图出现")
        check(client.count("#compare-view") == 1, "点活动栏「比较」→ 比较视图出现")

        # ② 两端用侧栏的「设 A / 设 B」按钮设好 → 文件清单出现
        #（端点按钮的 id 形如 `compare-from-HEAD~1`，但侧栏那套按钮在**比较侧栏**里，
        #  活动栏切过去才会出现——所以先点活动栏再点它们）。
        client.click("#compare-from-HEAD~1")
        client.click("#compare-to-HEAD")
        wait_until(lambda: client.count("#compare-file-0") == 1, what="文件清单出现")
        check(True, "A=HEAD~1 / B=HEAD → 清单出现")
        label = client.props("compare-file-0").get("text", "")
        check("alpha.txt" in label, f"清单第一条是 alpha.txt（实际：{label}）")

        # ③ 点文件 → 并列差异
        client.click("#compare-file-0")
        wait_until(lambda: client.count("#diff-view") == 1, what="差异视图出现")
        props = client.props("diff-view")
        hunks = props.get("hunks", "0")
        check(hunks not in ("", "0"), f"差异块数 = {hunks}")
        old_lines = props.get("old_lines", "")
        new_lines = props.get("new_lines", "")
        check(old_lines == new_lines and old_lines not in ("", "0"),
              f"两栏显示行数相等（{old_lines} vs {new_lines}）")
        left = props.get("left_text", "")
        right = props.get("right_text", "")
        check("one" in left and "one" in right, "两栏都拿到内容")
        check(left != right, "两栏内容不同（确实在对照）")
        check("TWO" in right or "two" in left, "改动行在两侧各自的栏里")

        # ④ 差异块导航（F7 的等价动作）
        before = props.get("current_hunk", "")
        before_line = props.get("current_hunk_line", "")
        client.invoke("diff-view", "next_hunk")
        after = client.props("diff-view")
        check(before != "" and after.get("current_hunk") != "",
              f"差异块导航可用（{before} → {after.get('current_hunk')}，行 {before_line} → "
              f"{after.get('current_hunk_line')}）")

        # ⑤ 内联 ↔ 并列（占位行随模式重建，两栏必须仍对齐）
        client.set_prop("diff-view", "layout", "unified")
        unified = client.props("diff-view").get("old_lines", "")
        check(unified not in ("", "0"), f"内联模式有内容（{unified} 行）")
        client.set_prop("diff-view", "layout", "side_by_side")
        back = client.props("diff-view")
        check(back.get("old_lines") == back.get("new_lines"),
              f"切回并列后两栏仍对齐（{back.get('old_lines')} vs {back.get('new_lines')}）")

        # ⑥ 截图（人可复核）
        shot = shots / "compare-side-by-side.png"
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

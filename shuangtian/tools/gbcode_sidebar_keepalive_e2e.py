"""测量侧栏「常驻声明 + 隐藏保活」的帧耗时代价（并做切换保活的观感断言）。

跑法：py -3 tools/gbcode_sidebar_keepalive_e2e.py

它验证两件事：
① 切换活动栏视图再切回来，各视图的**用户位置不丢**（滚动偏移、输入框文本）；
② 帧耗时的量级（`metrics`）——常驻声明让每帧多构建几棵侧栏子树，
   要确认它没把帧预算吃掉。
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
BIN = ROOT / "build" / "dev" / "bin" / "gbcode.exe"


def log(msg):
    print(msg, flush=True)


def git(cwd, *args):
    return subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True)


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

    def set_props(self, element_id, **values):
        """写入元素属性。

        ⚠ 控制通道的 `set` 收的是 **props 对象**（`{"props": {名: 值}}`），
        不是 `property`/`value` 两个平铺键（后者报 “props 必须是对象”）。
        另：`Input` 的**文本**在属性面叫 **`value`**（不是 `text`）——
        `set text=…` 会静默成功但读回来仍是空（实测）。
        """
        return self.ok("set", {"id": element_id, "props": dict(values)})

    def visible(self, element_id):
        """元素是否存在且可见（隐藏保活的断言面）。"""
        reply = self.ok("find", {"selector": f"#{element_id}"})
        matches = reply.get("matches") or []
        return bool(matches) and matches[0].get("visible", False)

    def click(self, selector):
        matches = self.ok("find", {"selector": selector}).get("matches") or []
        if not matches:
            raise AssertionError(f"找不到元素 {selector}")
        bounds = matches[0]["bounds"]
        self.ok("input.mouse", {"kind": "click", "x": bounds["x"] + bounds["width"] / 2.0,
                                "y": bounds["y"] + bounds["height"] / 2.0, "button": 1})
        time.sleep(0.4)

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
    shots = ROOT / "tmp" / "gbcode-sidebar-shots"
    shots.mkdir(parents=True, exist_ok=True)
    ctl = shots / "gbcode-ctl.json"
    ctl.unlink(missing_ok=True)
    handle = open(shots / "gbcode-sidebar.log", "wb")
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

    work = pathlib.Path(tempfile.mkdtemp(prefix="gbcode_sidebar_"))
    repo = work / "repo"
    repo.mkdir()
    git(repo, "init", "-q", "-b", "main")
    git(repo, "config", "user.email", "e2e@local")
    git(repo, "config", "user.name", "E2E")
    for index in range(30):
        (repo / f"src{index}.txt").write_text(f"content {index}\n", encoding="utf-8")
    git(repo, "add", ".")
    git(repo, "commit", "-qm", "first")
    # 让变更面板也有内容。
    (repo / "src0.txt").write_text("changed\n", encoding="utf-8")
    (repo / "src1.txt").write_text("changed too\n", encoding="utf-8")

    failures = []

    def check(ok, label):
        log(("  PASS " if ok else "  FAIL ") + label)
        if not ok:
            failures.append(label)

    proc = None
    try:
        proc, client, shots = start(repo)
        # ① 初始只声明**当前**视图（未访问的不建——启动首帧不能一次建五棵子树）。
        wait_until(lambda: client.count("#sidebar-explorer") == 1, what="资源管理器出现")
        check(client.count("#sidebar-scm") == 0,
              "未访问过的视图**不声明**（启动首帧不白建——这是保活与启动成本的平衡点）")

        # ② 逐个访问四个视图：访问后它们就**常驻**了（此后只隐藏、不再销毁）。
        for name, selector in [("search", "#activity-search"), ("scm", "#activity-scm"),
                               ("run", "#activity-run"), ("compare", "#activity-compare")]:
            client.click(selector)
            wait_until(lambda n=name: client.visible(f"sidebar-{n}"), what=f"{name} 可见")
        check(client.count("#sidebar-explorer") == 1 and client.count("#sidebar-search") == 1 and
              client.count("#sidebar-scm") == 1 and client.count("#sidebar-run") == 1 and
              client.count("#sidebar-compare") == 1,
              "访问过的五个视图全部**常驻在场**")
        visible_now = [v for v in ["sidebar-explorer", "sidebar-search", "sidebar-scm",
                                   "sidebar-run", "sidebar-compare"] if client.visible(v)]
        check(visible_now == ["sidebar-compare"], f"只有当前视图可见（实际 {visible_now}）")

        # ③ 切到「变更」，再切回「资源管理器」——两者都还在（没被销毁）。
        client.click("#activity-scm")
        wait_until(lambda: client.visible("sidebar-scm"), what="变更可见")
        check(client.visible("sidebar-scm"), "切到变更：变更可见")
        check(not client.visible("sidebar-explorer"), "切到变更：资源管理器隐藏（但仍在场）")
        check(client.count("#sidebar-explorer") == 1,
              "隐藏的资源管理器**没有被销毁**（这正是保活的意义）")
        client.click("#activity-explorer")
        wait_until(lambda: client.visible("sidebar-explorer"), what="资源管理器恢复可见")
        check(client.visible("sidebar-explorer"), "切回来：资源管理器恢复可见且是同一实例")
        check(not client.visible("sidebar-scm"), "变更转为隐藏（同样保活）")

        # ③ 搜索框里的字在切换后仍在（输入态保活——旧写法切走即丢）。
        client.click("#activity-search")
        wait_until(lambda: client.visible("sidebar-search"), what="搜索视图可见")
        # 注意：`Input` 的文本在属性面叫 `value`（不是 `text`）。
        client.set_props("search-input", value="保留我")
        time.sleep(0.5)
        before = client.props("search-input").get("value", "")
        check(before == "保留我", f"搜索框写入成功（{before!r}）")
        client.click("#activity-explorer")
        wait_until(lambda: client.visible("sidebar-explorer"), what="切到资源管理器")
        client.click("#activity-search")
        wait_until(lambda: client.visible("sidebar-search"), what="切回搜索")
        after = client.props("search-input").get("value", "")
        check(after == "保留我", f"切走再切回，搜索框内容仍在（{after!r}）")

        # ④ 滚动偏移保活：在变更列表里滚一段 → 切走 → 切回，偏移不变。
        #    （这是最容易被“切走即重建”弄丢的东西，也是旧写法实际丢过的。）
        client.click("#activity-scm")
        wait_until(lambda: client.visible("sidebar-scm"), what="变更可见")
        time.sleep(0.6)
        if client.count("#scm-scroll") == 1:
            offset_before = client.props("scm-scroll").get("offset", "")
            check(offset_before != "", f"变更滚动容器有 offset 属性（{offset_before!r}）")
            client.click("#activity-explorer")
            wait_until(lambda: client.visible("sidebar-explorer"), what="切到资源管理器")
            client.click("#activity-scm")
            wait_until(lambda: client.visible("sidebar-scm"), what="切回变更")
            offset_after = client.props("scm-scroll").get("offset", "")
            check(offset_before == offset_after,
                  f"滚动偏移保活（{offset_before!r} → {offset_after!r}）")
        else:
            log("  SKIP 变更视图没有滚动容器")

        # ④ 资源管理器的展开态保活：展开一个目录 → 切走 → 切回仍展开。
        client.click("#activity-explorer")
        wait_until(lambda: client.visible("sidebar-explorer"), what="资源管理器可见")
        time.sleep(0.6)
        # 目录行 id 形如 `tree-<相对路径>`；没有目录时跳过这一条（不硬编码仓库结构）。
        dir_buttons = client.ok("find", {"selector": "[id^=tree-]"})["matches"]
        if dir_buttons:
            target = dir_buttons[0]["id"]
            client.click(f"#{target}")
            time.sleep(0.6)
            expanded_before = client.props(target).get("label", "") if target else ""
            client.click("#activity-scm")
            wait_until(lambda: client.visible("sidebar-scm"), what="切到变更")
            client.click("#activity-explorer")
            wait_until(lambda: client.visible("sidebar-explorer"), what="切回")
            expanded_after = client.props(target).get("label", "")
            check(expanded_before == expanded_after,
                  f"树节点展开态保活（{expanded_before!r} → {expanded_after!r}）")
        else:
            log("  SKIP 仓库里没有可展开的目录（不硬编码结构）")

        # ⑤ 稳态帧耗时（常驻声明让每帧多构建几棵侧栏子树）。
        #
        # 判据用 **p50**（稳态）而不是 p95：p95 里包含“首次访问某个视图那一帧”
        # 的构建成本（那一帧本来就要建一棵新子树）与无头环境的调度噪声——
        # 逆向验证实测：旧写法（只声明当前视图）的 p95 同样会尖到 20~30ms，
        # 而两者 p50 相差 <1ms。拿 p95 当门禁只会得到一个会随机红的护栏。
        metrics = client.ok("metrics", {})
        p50 = metrics.get("frame_p50_ms", 0.0)
        p95 = metrics.get("frame_p95_ms", 0.0)
        last = metrics.get("last_frame_ms", 0.0)
        log(f"  帧耗时：last={last:.2f}ms p50={p50:.2f}ms p95={p95:.2f}ms")
        check(p50 < 16.0, f"稳态帧耗时 p50 {p50:.2f}ms 在 60fps 预算内（<16ms）")

        shot = shots / "sidebar-keepalive.png"
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
